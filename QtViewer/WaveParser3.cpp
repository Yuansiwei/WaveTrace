#include "WaveParser3.h"

#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>
#include <vector>
#include <utility>
#include <zstd.h>
#include <queue>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

    struct FlatSampleEvent {
        qint64 time = 0;
        int signalId = -1;
        QString value;
    };

    template <typename T>
    void appendPod(QByteArray& buf, const T& pod) {
        const int old = buf.size();
        buf.resize(old + int(sizeof(T)));
        memcpy(buf.data() + old, &pod, sizeof(T));
    }

    QByteArray joinStrings(const QVector<QString>& texts, QVector<quint32>& offsetsUtf8) {
        QByteArray table;
        offsetsUtf8.reserve(texts.size());
        for (const QString& s : texts) {
            offsetsUtf8.push_back(quint32(table.size()));
            table.append(s.toUtf8());
            table.append('\0');
        }
        return table;
    }

    bool flushDevice(QFile& file) {
        if (!file.isOpen()) return false;
        if (!file.flush()) return false;
        const int fd = file.handle();
        if (fd < 0) return false;
#ifdef Q_OS_WIN
        return _commit(fd) == 0;
#else
        return ::fsync(fd) == 0;
#endif
    }

    QString normalizeNumericText(const QString& raw) {
        QString s = raw.trimmed();
        if (s.isEmpty()) return QStringLiteral("0");
        if (s.startsWith("0x", Qt::CaseInsensitive)) return s.mid(2).toUpper();
        if (s.startsWith("0b", Qt::CaseInsensitive)) return s.mid(2);
        return s;
    }

    const QString& cachedZeroString() {
        static const QString v = QStringLiteral("0");
        return v;
    }

    const QString& cachedOneString() {
        static const QString v = QStringLiteral("1");
        return v;
    }

    const QString& cachedZString() {
        static const QString v = QStringLiteral("Z");
        return v;
    }

    const QString& decodeBitStateCached(const quint8 state) {
        switch (state & 0x3u) {
        case 1: return cachedOneString();
        case 2: return cachedZString();
        default: return cachedZeroString();
        }
    }

    bool tryStringFromTableOffsetFast(const QByteArray& table, const quint32 offset, QString& out) {
        if (offset >= quint32(table.size())) return false;

        const char* raw = table.constData() + offset;
        if (raw[0] == '0' && raw[1] == '\0') { out = cachedZeroString(); return true; }
        if (raw[0] == '1' && raw[1] == '\0') { out = cachedOneString(); return true; }
        if ((raw[0] == 'Z' || raw[0] == 'z') && raw[1] == '\0') { out = cachedZString(); return true; }

        // Do not cache arbitrary string-table offsets here.
        // In real WVZ3 signal directories, most name offsets are unique, so an
        // offset->QString QHash has poor hit rate and becomes slower than direct
        // QString::fromUtf8(), including expensive cache destruction.
        out = QString::fromUtf8(raw);
        return true;
    }

    struct SignalDirSlot {
        bool valid = false;
        wvz3::SignalDirEntry dir{};
        QString name;
    };

    void resizeSignalTableForMaxId(QVector<SignalDirSlot>& signalTable, const quint32 maxSignalId) {
        if (maxSignalId > quint32(std::numeric_limits<int>::max() - 1)) return;
        const int required = int(maxSignalId) + 1;
        if (signalTable.size() < required) signalTable.resize(required);
    }

    void resizeSampleTableForMaxId(QVector<QVector<WaveSample>>& sampleTable, const quint32 maxSignalId) {
        if (maxSignalId > quint32(std::numeric_limits<int>::max() - 1)) return;
        const int required = int(maxSignalId) + 1;
        if (sampleTable.size() < required) sampleTable.resize(required);
    }

    void ensureSignalTableSize(QVector<SignalDirSlot>& signalTable, const quint32 signalId) {
        if (signalId > quint32(std::numeric_limits<int>::max() - 1)) return;
        const int required = int(signalId) + 1;
        if (signalTable.size() < required) signalTable.resize(required);
    }

    void ensureSampleTableSize(QVector<QVector<WaveSample>>& sampleTable, const quint32 signalId) {
        if (signalId > quint32(std::numeric_limits<int>::max() - 1)) return;
        const int required = int(signalId) + 1;
        if (sampleTable.size() < required) sampleTable.resize(required);
    }

    void setSignalDirSlot(QVector<SignalDirSlot>& signalTable,
        const quint32 signalId,
        const wvz3::SignalDirEntry& dir,
        const QString& name) {
        ensureSignalTableSize(signalTable, signalId);
        if (signalId >= quint32(signalTable.size())) return;
        SignalDirSlot& slot = signalTable[int(signalId)];
        slot.valid = true;
        slot.dir = dir;
        slot.name = name;
    }

    bool sampleTableIsEmpty(const QVector<QVector<WaveSample>>& sampleTable) {
        for (const QVector<WaveSample>& rows : sampleTable) {
            if (!rows.isEmpty()) return false;
        }
        return true;
    }

    void reserveAdditionalSamples(QVector<WaveSample>& outSamples, const quint64 additional) {
        if (additional == 0) return;

        const quint64 current = quint64(outSamples.size());
        const quint64 maxInt = quint64(std::numeric_limits<int>::max());
        const quint64 wanted64 = qMin(maxInt, current + additional);
        const int wanted = int(wanted64);

        if (outSamples.capacity() >= wanted) return;

        int newCapacity = outSamples.capacity();
        if (newCapacity < 16) newCapacity = 16;

        while (newCapacity < wanted && newCapacity < std::numeric_limits<int>::max() / 2) {
            newCapacity *= 2;
        }
        if (newCapacity < wanted) newCapacity = wanted;

        outSamples.reserve(newCapacity);
    }

    inline void setDecodedRawFields(WaveSample& sample,
        const bool isZ,
        const quint64 rawBits,
        const int width) {
        sample.isAbsent = false;
        sample.isZ = isZ;
        sample.rawBits = isZ ? 0ull : (rawBits & waveBitMaskForWidth(width));
        sample.rawFieldsReady = true;
    }

    inline quint64 low64FromPackedBytes(const QByteArray& raw, const int width) {
        quint64 v = 0ull;
        const int n = raw.size();
        const int start = qMax(0, n - 8);
        for (int i = start; i < n; ++i) {
            v = (v << 8) | quint64(quint8(raw.at(i)));
        }
        return v & waveBitMaskForWidth(width);
    }

    struct SignalIdFilter {
        bool keepAll = true;
        QVector<quint8> dense;
        QSet<quint32> sparse;

        explicit SignalIdFilter(const QVector<int>& ids, bool emptyMeansAll = true) {
            if (ids.isEmpty()) {
                keepAll = emptyMeansAll;
                return;
            }

            keepAll = false;
            int maxId = -1;
            int validCount = 0;
            for (const int id : ids) {
                if (id < 0) continue;
                maxId = qMax(maxId, id);
                ++validCount;
            }

            if (validCount <= 0 || maxId < 0) return;

            const int denseLimit = qMax(4096, validCount * 8 + 1024);
            if (maxId <= denseLimit) {
                dense.resize(maxId + 1);
                for (const int id : ids) {
                    if (id >= 0) dense[id] = 1;
                }
            }
            else {
                sparse.reserve(validCount * 2);
                for (const int id : ids) {
                    if (id >= 0) sparse.insert(quint32(id));
                }
            }
        }

        inline bool keep(const quint32 signalId) const {
            if (keepAll) return true;
            if (!dense.isEmpty()) {
                return signalId < quint32(dense.size()) && dense[int(signalId)] != 0;
            }
            return sparse.contains(signalId);
        }
    };


    QString defaultRawValueForSignal(const SignalKind kind, const int width) {
        Q_UNUSED(kind);
        Q_UNUSED(width);
        return cachedZeroString();
    }

    wvz3::DefaultRadixOnDisk radixToDisk(ValueRadix radix) {
        switch (radix) {
        case ValueRadix::Hex: return wvz3::Radix_Hex;
        case ValueRadix::Dec: return wvz3::Radix_Dec;
        case ValueRadix::Int: return wvz3::Radix_Int;
        case ValueRadix::UInt: return wvz3::Radix_UInt;
        case ValueRadix::Float: return wvz3::Radix_Float;
        case ValueRadix::Int64: return wvz3::Radix_Int64;
        case ValueRadix::UInt64: return wvz3::Radix_UInt64;
        case ValueRadix::Double: return wvz3::Radix_Double;
        case ValueRadix::Bin:
        default: return wvz3::Radix_Bin;
        }
    }

    ValueRadix radixFromDisk(quint8 radix) {
        switch (radix) {
        case wvz3::Radix_Hex: return ValueRadix::Hex;
        case wvz3::Radix_Dec: return ValueRadix::Dec;
        case wvz3::Radix_Int: return ValueRadix::Int;
        case wvz3::Radix_UInt: return ValueRadix::UInt;
        case wvz3::Radix_Float: return ValueRadix::Float;
        case wvz3::Radix_Int64: return ValueRadix::Int64;
        case wvz3::Radix_UInt64: return ValueRadix::UInt64;
        case wvz3::Radix_Double: return ValueRadix::Double;
        case wvz3::Radix_Bin:
        default: return ValueRadix::Bin;
        }
    }

    quint32 makeBlockFlags(wvz3::CompressionType comp, bool checksum) {
        Q_UNUSED(checksum);
        // Checksum emission is not implemented yet. Never advertise Block_HasChecksum
        // because the stored checksum fields are intentionally zero.
        return (quint32(comp) << 8);
    }

    wvz3::CompressionType blockCompression(quint32 flags) {
        return wvz3::CompressionType((flags >> 8) & 0xFFu);
    }

    qint64 timeToTicks(qint64 t, quint64 timescalePs) {
        Q_UNUSED(timescalePs);
        return t > 0 ? t : 0;
    }

    qint64 ticksToTime(qint64 ticks, quint64 timescalePs) {
        Q_UNUSED(timescalePs);
        return ticks;
    }

    quint8 encodeBitState(const QString& value) {
        const QString t = value.trimmed().toLower();
        if (t == QStringLiteral("1")) return 1;
        if (t == QStringLiteral("z")) return 2;
        return 0;
    }

    QString decodeBitState(quint8 state) {
        return decodeBitStateCached(state);
    }

    quint64 parseValueU64(const QString& raw) {
        if (isWaveZValue(raw)) return 0;
        const QString s = raw.trimmed();
        if (s.isEmpty()) return 0;

        bool ok = false;
        if (s.startsWith("0x", Qt::CaseInsensitive)) {
            const quint64 v = s.mid(2).toULongLong(&ok, 16);
            return ok ? v : 0;
        }
        if (s.startsWith("0b", Qt::CaseInsensitive)) {
            const quint64 v = s.mid(2).toULongLong(&ok, 2);
            return ok ? v : 0;
        }

        // Bare digits are treated as decimal to avoid mis-decoding values like "10" or "42".
        if (QRegularExpression("^[0-9]+$").match(s).hasMatch()) {
            const quint64 v = s.toULongLong(&ok, 10);
            return ok ? v : 0;
        }

        // Bare bit strings can still be expressed explicitly with a 0b prefix.
        if (QRegularExpression("^[0-9A-Fa-f]+$").match(s).hasMatch()) {
            const quint64 v = s.toULongLong(&ok, 16);
            return ok ? v : 0;
        }

        const quint64 v = s.toULongLong(&ok, 10);
        return ok ? v : 0;
    }

    QByteArray u64ToPackedBytes(const quint64 value, const int bitWidth) {
        const int byteCount = qMax(1, (bitWidth + 7) / 8);
        QByteArray out(byteCount, '\0');
        quint64 v = value;
        for (int i = byteCount - 1; i >= 0; --i) {
            out[i] = char(quint8(v & 0xFFu));
            v >>= 8u;
        }
        if (bitWidth > 0 && bitWidth < byteCount * 8) {
            const int extraBits = byteCount * 8 - bitWidth;
            out[0] = char(quint8(out[0]) & quint8(0xFFu >> extraBits));
        }
        return out;
    }

    quint64 packedBytesToU64(QByteArray raw, const int bitWidth) {
        if (raw.isEmpty()) return 0;
        if (bitWidth > 0 && bitWidth < raw.size() * 8) {
            const int extraBits = raw.size() * 8 - bitWidth;
            raw[0] = char(quint8(raw[0]) & quint8(0xFFu >> extraBits));
        }
        quint64 value = 0;
        for (unsigned char ch : raw) {
            value = (value << 8u) | quint64(ch);
        }
        return value;
    }

    bool tryParseHexPacked(const QString& raw, const int bitWidth, QByteArray& out) {
        QString s = raw.trimmed();
        if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) s = s.mid(2);
        if (s.isEmpty()) s = QStringLiteral("0");
        if (!QRegularExpression(QStringLiteral("^[0-9A-Fa-f]+$")).match(s).hasMatch()) return false;

        const int byteCount = qMax(1, (bitWidth + 7) / 8);
        const int targetHexLen = byteCount * 2;
        if (s.size() > targetHexLen) s = s.right(targetHexLen);
        s = s.rightJustified(targetHexLen, QLatin1Char('0'));

        QByteArray bytes(byteCount, '\0');
        for (int i = 0; i < byteCount; ++i) {
            bool ok = false;
            const quint8 b = quint8(s.mid(i * 2, 2).toUInt(&ok, 16));
            if (!ok) return false;
            bytes[i] = char(b);
        }
        if (bitWidth > 0 && bitWidth < byteCount * 8) {
            const int extraBits = byteCount * 8 - bitWidth;
            bytes[0] = char(quint8(bytes[0]) & quint8(0xFFu >> extraBits));
        }
        out = bytes;
        return true;
    }

    bool tryParseBinPacked(const QString& raw, const int bitWidth, QByteArray& out) {
        QString s = raw.trimmed();
        if (s.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) s = s.mid(2);
        if (s.isEmpty()) s = QStringLiteral("0");
        if (!QRegularExpression(QStringLiteral("^[01]+$")).match(s).hasMatch()) return false;

        const int byteCount = qMax(1, (bitWidth + 7) / 8);
        const int targetBitLen = byteCount * 8;
        if (s.size() > targetBitLen) s = s.right(targetBitLen);
        s = s.rightJustified(targetBitLen, QLatin1Char('0'));

        QByteArray bytes(byteCount, '\0');
        for (int i = 0; i < byteCount; ++i) {
            bool ok = false;
            const quint8 b = quint8(s.mid(i * 8, 8).toUInt(&ok, 2));
            if (!ok) return false;
            bytes[i] = char(b);
        }
        if (bitWidth > 0 && bitWidth < byteCount * 8) {
            const int extraBits = byteCount * 8 - bitWidth;
            bytes[0] = char(quint8(bytes[0]) & quint8(0xFFu >> extraBits));
        }
        out = bytes;
        return true;
    }

    bool tryEncodePackedWideValue(const QString& raw, const int width, const ValueRadix radix, QByteArray& out) {
        if (isWaveZValue(raw)) {
            out = QByteArray(qMax(1, (width + 7) / 8), '\0');
            return true;
        }
        if (width <= 64) {
            out = u64ToPackedBytes(parseValueU64(raw), width);
            return true;
        }

        const QString text = raw.trimmed();
        if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) return tryParseHexPacked(text, width, out);
        if (text.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) return tryParseBinPacked(text, width, out);
        if (radix == ValueRadix::Hex) return tryParseHexPacked(normalizeNumericText(text), width, out);
        if (radix == ValueRadix::Bin) return tryParseBinPacked(normalizeNumericText(text), width, out);
        return false;
    }

    QByteArray encodeWideValueLegacy(const QString& raw) {
        return normalizeNumericText(raw).toUtf8();
    }

    QString forceFloatingDecimalPoint(QString text) {
        text = text.trimmed();
        if (text.isEmpty()) return text;

        const QString lower = text.toLower();
        if (lower.contains(QStringLiteral("nan")) || lower.contains(QStringLiteral("inf"))) {
            return text;
        }

        const int ePosLower = text.indexOf(QLatin1Char('e'));
        const int ePosUpper = text.indexOf(QLatin1Char('E'));
        int ePos = -1;
        if (ePosLower >= 0 && ePosUpper >= 0) ePos = qMin(ePosLower, ePosUpper);
        else ePos = qMax(ePosLower, ePosUpper);

        const QString mantissa = (ePos >= 0) ? text.left(ePos) : text;
        if (mantissa.contains(QLatin1Char('.'))) return text;

        if (ePos >= 0) {
            text.insert(ePos, QStringLiteral(".0"));
        } else {
            text += QStringLiteral(".0");
        }
        return text;
    }

    QString formatU64ForRadix(const quint64 value, const int bitWidth, const ValueRadix radix) {
        switch (radix) {
        case ValueRadix::Bin:
            return QStringLiteral("0b") + QString::number(value, 2).rightJustified(qMax(1, bitWidth), QLatin1Char('0'));
        case ValueRadix::Hex:
            return QString::number(value, 16).toUpper().rightJustified(qMax(1, (bitWidth + 3) / 4), QLatin1Char('0'));
        case ValueRadix::Int: {
            if (bitWidth <= 0 || bitWidth >= 64) return QString::number(static_cast<qint64>(value));
            const quint64 mask = (quint64(1) << bitWidth) - 1ull;
            const quint64 clipped = value & mask;
            const quint64 signBit = quint64(1) << (bitWidth - 1);
            if (clipped & signBit) {
                const qint64 signedValue = static_cast<qint64>(clipped | (~mask));
                return QString::number(signedValue);
            }
            return QString::number(static_cast<qint64>(clipped));
        }
        case ValueRadix::Float:
            if (bitWidth == 32) {
                union { quint32 u; float f; } conv{};
                conv.u = static_cast<quint32>(value & 0xFFFFFFFFu);
                return forceFloatingDecimalPoint(QString::number(static_cast<double>(conv.f), 'g', 8));
            }
            return QStringLiteral("N/A");
        case ValueRadix::Double:
            if (bitWidth == 64) {
                union { quint64 u; double d; } conv{};
                conv.u = value;
                return forceFloatingDecimalPoint(QString::number(conv.d, 'g', 16));
            }
            return QStringLiteral("N/A");
        case ValueRadix::UInt:
        case ValueRadix::Dec:
        default:
            return QString::number(value);
        }
    }

    QString formatBytesForRadix(const QByteArray& raw, const int bitWidth, const ValueRadix radix) {
        if (raw.isEmpty()) return QStringLiteral("0");

        if (bitWidth > 0 && bitWidth <= 64) {
            return formatU64ForRadix(packedBytesToU64(raw, bitWidth), bitWidth, radix);
        }

        QByteArray bytes = raw;
        if (bitWidth > 0 && !bytes.isEmpty()) {
            const int totalBits = bytes.size() * 8;
            if (bitWidth < totalBits) {
                const int extraBits = totalBits - bitWidth;
                bytes[0] = char(quint8(bytes[0]) & quint8(0xFFu >> extraBits));
            }
        }

        if (radix == ValueRadix::Bin) {
            QString bits;
            bits.reserve(bytes.size() * 8);
            for (unsigned char ch : bytes) {
                bits += QString::number(ch, 2).rightJustified(8, QLatin1Char('0'));
            }
            const int keep = qMax(1, bitWidth > 0 ? bitWidth : bits.size());
            if (bits.size() > keep) bits = bits.right(keep);
            return QStringLiteral("0b") + bits;
        }

        QString hex;
        hex.reserve(bytes.size() * 2);
        for (unsigned char ch : bytes) {
            hex += QString::number(ch, 16).toUpper().rightJustified(2, QLatin1Char('0'));
        }
        const int keepNibbles = qMax(1, bitWidth > 0 ? (bitWidth + 3) / 4 : hex.size());
        if (hex.size() > keepNibbles) hex = hex.right(keepNibbles);
        return hex;
    }

    QString decodeWideValueLegacy(const QByteArray& raw) {
        return QString::fromUtf8(raw).trimmed();
    }

    QString decodeWideValuePacked(const QByteArray& raw, const int width, const ValueRadix radix) {
        return formatBytesForRadix(raw, width, radix);
    }

    QString rawU64Text(const quint64 value, const int bitWidth) {
        const int nibbleCount = qMax(1, (bitWidth + 3) / 4);
        return QStringLiteral("0x") +
            QString::number(value, 16).toUpper().rightJustified(nibbleCount, QLatin1Char('0'));
    }

    struct U64TextCache {
        QHash<quint64, QString> cache;
    };

    struct ValueTextInterner {
        ValueTextInterner() : u64ByWidth(65) {}

        static bool shouldCacheU64Text(const quint64 value, const int bitWidth) {
            // Wide counters/PC-like values are often almost all unique. Hashing
            // and inserting those strings is slower than formatting directly.
            // Keep caching for narrow buses and common scalar-like wide values.
            if (bitWidth <= 16) return true;
            if (value <= 15ull) return true;
            if (bitWidth > 0 && bitWidth <= 64 && value == waveBitMaskForWidth(bitWidth)) return true;
            return false;
        }

        QString u64Text(const quint64 value, const int bitWidth) {
            if (!shouldCacheU64Text(value, bitWidth)) {
                return rawU64Text(value, bitWidth);
            }

            if (bitWidth >= 0 && bitWidth <= 64) {
                QHash<quint64, QString>& cache = u64ByWidth[bitWidth].cache;
                auto it = cache.constFind(value);
                if (it != cache.constEnd()) return it.value();

                const QString text = rawU64Text(value, bitWidth);
                cache.insert(value, text);
                return text;
            }

            QHash<quint64, QString>& cache = extraU64ByWidth[bitWidth].cache;
            auto it = cache.constFind(value);
            if (it != cache.constEnd()) return it.value();

            const QString text = rawU64Text(value, bitWidth);
            cache.insert(value, text);
            return text;
        }

        QVector<U64TextCache> u64ByWidth;
        QHash<int, U64TextCache> extraU64ByWidth;
    };


    QString rawPackedBytesText(QByteArray raw, const int bitWidth) {
        if (raw.isEmpty()) return QStringLiteral("0x0");
        if (bitWidth > 0 && bitWidth < raw.size() * 8) {
            const int extraBits = raw.size() * 8 - bitWidth;
            raw[0] = char(quint8(raw[0]) & quint8(0xFFu >> extraBits));
        }
        QString hex;
        hex.reserve(raw.size() * 2);
        for (unsigned char ch : raw) {
            hex += QString::number(ch, 16).toUpper().rightJustified(2, QLatin1Char('0'));
        }
        const int keepNibbles = qMax(1, bitWidth > 0 ? (bitWidth + 3) / 4 : hex.size());
        if (hex.size() > keepNibbles) hex = hex.right(keepNibbles);
        return QStringLiteral("0x") + hex;
    }

    QByteArray encodeVarUInt(quint64 value) {
        QByteArray out;
        while (true) {
            quint8 b = quint8(value & 0x7Fu);
            value >>= 7u;
            if (value) out.append(char(b | 0x80u));
            else {
                out.append(char(b));
                break;
            }
        }
        return out;
    }

    bool decodeVarUInt(const QByteArray& data, int& pos, quint64& out) {
        out = 0;
        int shift = 0;
        while (pos < data.size() && shift < 64) {
            const quint8 b = quint8(data.at(pos++));
            out |= (quint64(b & 0x7Fu) << shift);
            if ((b & 0x80u) == 0) return true;
            shift += 7;
        }
        return false;
    }


    struct ByteSpan {
        const char* dataPtr = nullptr;
        int dataSize = 0;

        int size() const { return dataSize; }
        bool isEmpty() const { return dataSize <= 0; }
        const char* constData() const { return dataPtr; }
        char at(const int index) const { return dataPtr[index]; }

        QByteArray mid(const int pos, const int len) const {
            if (!dataPtr || pos < 0 || len < 0 || pos > dataSize || len > dataSize - pos) {
                return QByteArray();
            }
            return QByteArray(dataPtr + pos, len);
        }
    };

    bool decodeVarUInt(const ByteSpan& data, int& pos, quint64& out) {
        out = 0;
        int shift = 0;
        while (pos < data.size() && shift < 64) {
            const quint8 b = quint8(data.at(pos++));
            out |= (quint64(b & 0x7Fu) << shift);
            if ((b & 0x80u) == 0) return true;
            shift += 7;
        }
        return false;
    }


    QByteArray zstdCompressPayload(const QByteArray& raw, int level, QString& error) {
        error.clear();
        const size_t bound = ZSTD_compressBound(size_t(raw.size()));
        QByteArray out;
        out.resize(int(bound));

        const size_t written = ZSTD_compress(out.data(),
            bound,
            raw.constData(),
            size_t(raw.size()),
            level);
        if (ZSTD_isError(written)) {
            error = QStringLiteral("Zstd 压缩失败：%1")
                .arg(QString::fromUtf8(ZSTD_getErrorName(written)));
            return {};
        }

        out.resize(int(written));
        return out;
    }

    QByteArray zstdDecompressPayload(const QByteArray& raw, QString& error) {
        error.clear();

        const unsigned long long sizeHint =
            ZSTD_getFrameContentSize(raw.constData(), size_t(raw.size()));

        if (sizeHint != ZSTD_CONTENTSIZE_ERROR &&
            sizeHint != ZSTD_CONTENTSIZE_UNKNOWN) {
            if (sizeHint >
                unsigned long long(std::numeric_limits<int>::max())) {
                error = QStringLiteral("Zstd 解压失败：解压后数据过大");
                return {};
            }

            QByteArray out;
            out.resize(int(sizeHint));

            const size_t written = ZSTD_decompress(out.data(),
                size_t(out.size()),
                raw.constData(),
                size_t(raw.size()));
            if (ZSTD_isError(written)) {
                error = QStringLiteral("Zstd 解压失败：%1")
                    .arg(QString::fromUtf8(ZSTD_getErrorName(written)));
                return {};
            }

            out.resize(int(written));
            return out;
        }

        ZSTD_DStream* stream = ZSTD_createDStream();
        if (!stream) {
            error = QStringLiteral("Zstd 解压失败：无法创建 DStream");
            return {};
        }

        size_t initRes = ZSTD_initDStream(stream);
        if (ZSTD_isError(initRes)) {
            error = QStringLiteral("Zstd 解压失败：%1")
                .arg(QString::fromUtf8(ZSTD_getErrorName(initRes)));
            ZSTD_freeDStream(stream);
            return {};
        }

        QByteArray out;
        const size_t chunk = ZSTD_DStreamOutSize();
        ZSTD_inBuffer input{ raw.constData(), size_t(raw.size()), 0 };

        while (input.pos < input.size) {
            const int oldSize = out.size();
            if (oldSize > std::numeric_limits<int>::max() - int(chunk)) {
                error = QStringLiteral("Zstd 解压失败：解压后数据过大");
                ZSTD_freeDStream(stream);
                return {};
            }

            out.resize(oldSize + int(chunk));
            ZSTD_outBuffer output{ out.data() + oldSize, chunk, 0 };

            const size_t res = ZSTD_decompressStream(stream, &output, &input);
            if (ZSTD_isError(res)) {
                error = QStringLiteral("Zstd 解压失败：%1")
                    .arg(QString::fromUtf8(ZSTD_getErrorName(res)));
                ZSTD_freeDStream(stream);
                return {};
            }

            out.resize(oldSize + int(output.pos));
        }

        ZSTD_freeDStream(stream);
        return out;
    }

    QByteArray compressPayload(const QByteArray& raw,
        wvz3::CompressionType comp,
        QString& error) {
        error.clear();
        switch (comp) {
        case wvz3::Comp_None:
            return raw;
        case wvz3::Comp_Zlib:
            return qCompress(raw, 9);
        case wvz3::Comp_Zstd:
            return zstdCompressPayload(raw, 15, error);
        default:
            error = QStringLiteral("当前版本仅实现了 none / zlib / zstd 压缩");
            return {};
        }
    }

    QByteArray decompressPayload(const QByteArray& raw,
        wvz3::CompressionType comp,
        QString& error) {
        error.clear();
        switch (comp) {
        case wvz3::Comp_None:
            return raw;
        case wvz3::Comp_Zlib:
            return qUncompress(raw);
        case wvz3::Comp_Zstd:
            return zstdDecompressPayload(raw, error);
        default:
            error = QStringLiteral("当前版本仅实现了 none / zlib / zstd 解压");
            return {};
        }
    }

    struct SignalRuntimeView {
        SignalKind kind = SignalKind::Bit;
        int width = 1;
        ValueRadix defaultRadix = ValueRadix::Bin;
        QString blockInitialValue;
        QVector<Wvz3StreamWriter::PendingChange> pending;
    };

    inline bool containsAsciiCaseInsensitive(const QString& raw, const QChar lowerNeedle) {
        const QChar upperNeedle = lowerNeedle.toUpper();
        for (QChar ch : raw) {
            if (ch == lowerNeedle || ch == upperNeedle) return true;
        }
        return false;
    }

    bool containsZStateText(const QString& raw) {
        return isWaveZValue(raw);
    }

    bool containsXStateText(const QString& raw) {
        return containsAsciiCaseInsensitive(raw, QLatin1Char('x'));
    }

    bool isValidBitValue(const QString& raw, const bool supportsZState) {
        const QString t = raw.trimmed().toLower();
        if (t == QStringLiteral("0") || t == QStringLiteral("1")) return true;
        if (supportsZState && t == QStringLiteral("z")) return true;
        return false;
    }

    QString normalizeBitValueFast(const QString& raw) {
        const QString t = raw.trimmed().toLower();
        if (t == QStringLiteral("1")) return QStringLiteral("1");
        if (t == QStringLiteral("z")) return QStringLiteral("Z");
        if (t == QStringLiteral("x")) return QStringLiteral("x");
        return QStringLiteral("0");
    }


    bool normalizeSignalDefinition(const Wvz3StreamWriter::SignalDefinition& inputDef,
        QHash<int, bool>& seenIds,
        int& autoId,
        Wvz3StreamWriter::SignalDefinition& outDef,
        QString& error) {
        outDef = inputDef;
        while (seenIds.contains(autoId)) ++autoId;
        if (outDef.signalId <= 0) outDef.signalId = autoId;
        if (outDef.name.trimmed().isEmpty()) {
            error = QStringLiteral("wvz3 信号定义失败：存在空信号名");
            return false;
        }
        if (seenIds.contains(outDef.signalId)) {
            error = QStringLiteral("wvz3 信号定义失败：signalId=%1 重复").arg(outDef.signalId);
            return false;
        }
        if (outDef.width <= 0) outDef.width = 1;
        if (outDef.initialValue.trimmed().isEmpty()) outDef.initialValue = defaultRawValueForSignal(outDef.kind, outDef.width);
        if (containsXStateText(outDef.initialValue)) {
            error = QStringLiteral("wvz3 信号定义失败：当前版本不支持 x 态，signal=%1").arg(outDef.name);
            return false;
        }

        if (outDef.kind == SignalKind::Bit) {
            outDef.initialValue = normalizeBitValueFast(outDef.initialValue);
            if (!isValidBitValue(outDef.initialValue, true)) {
                error = QStringLiteral("wvz3 信号定义失败：bit 初值仅支持 0/1/Z，signal=%1 value=%2").arg(outDef.name).arg(outDef.initialValue);
                return false;
            }
        }
        else {
            outDef.initialValue = isWaveZValue(outDef.initialValue) ? QStringLiteral("Z") : normalizeNumericText(outDef.initialValue);
        }
        outDef.supportsZState = true;

        seenIds.insert(outDef.signalId, true);
        autoId = qMax(autoId, outDef.signalId + 1);
        return true;
    }

    wvz3::SignalDirEntry makeSignalDirEntry(const Wvz3StreamWriter::SignalDefinition& def,
        const quint32 nameOffset,
        const bool enableClockDeltaOptimization) {
        wvz3::SignalDirEntry entry{};
        entry.signalId = quint32(def.signalId);
        entry.nameOffset = nameOffset;
        entry.bitWidth = quint16(qMax(1, def.width));
        if (def.kind == SignalKind::Bit) {
            entry.signalKind = wvz3::Sig_Bit;
            entry.logicKind = def.supportsZState ? wvz3::Logic_FourState : wvz3::Logic_TwoState;
            entry.preferredEncoding = enableClockDeltaOptimization ? wvz3::Enc_ClockDeltaPattern : wvz3::Enc_BitTransitions;
        }
        else {
            entry.signalKind = wvz3::Sig_Bus;
            entry.logicKind = def.supportsZState ? wvz3::Logic_FourState : wvz3::Logic_TwoState;
            entry.preferredEncoding = (def.width == 64 && def.defaultRadix != ValueRadix::Float && def.defaultRadix != ValueRadix::Int)
                ? wvz3::Enc_BusTransitionsU64
                : wvz3::Enc_BusTransitionsPackedBytes;
        }
        entry.defaultRadix = radixToDisk(def.defaultRadix);
        entry.signalFlags = def.supportsZState ? wvz3::Signal_SupportsZ : 0;
        return entry;
    }

    struct DecodedDeltaSignalDef {
        Wvz3StreamWriter::SignalDefinition def;
    };

    QByteArray encodeSignalDirDeltaPayload(const QVector<Wvz3StreamWriter::SignalDefinition>& sigDefs,
        const bool enableClockDeltaOptimization) {
        QVector<QString> texts;
        texts.reserve(sigDefs.size() * 2);
        QVector<quint32> nameOffsets;
        QVector<quint32> initOffsets;
        for (const auto& def : sigDefs) {
            nameOffsets.push_back(quint32(texts.size()));
            texts.push_back(def.name);
            initOffsets.push_back(quint32(texts.size()));
            texts.push_back(def.initialValue);
        }
        QVector<quint32> stringOffsets;
        const QByteArray stringTable = joinStrings(texts, stringOffsets);

        QByteArray payload;
        wvz3::SignalDirDeltaPayloadHeader header{};
        header.signalCount = quint32(sigDefs.size());
        appendPod(payload, header);

        for (int i = 0; i < sigDefs.size(); ++i) {
            wvz3::SignalDirDeltaEntry entry{};
            entry.dir = makeSignalDirEntry(sigDefs.at(i), stringOffsets.at(nameOffsets.at(i)), enableClockDeltaOptimization);
            entry.initialValueOffset = stringOffsets.at(initOffsets.at(i));
            appendPod(payload, entry);
        }

        const quint32 stringSize = quint32(stringTable.size());
        appendPod(payload, stringSize);
        payload.append(stringTable);
        return payload;
    }

    bool decodeSignalDirDeltaPayload(const QByteArray& payload,
        QVector<SignalDirSlot>& signalTable,
        QVector<DecodedDeltaSignalDef>* outDefs,
        QString& error) {
        error.clear();
        int pos = 0;
        if (payload.size() < int(sizeof(wvz3::SignalDirDeltaPayloadHeader))) {
            error = QStringLiteral("信号目录增量 payload 太小");
            return false;
        }

        wvz3::SignalDirDeltaPayloadHeader header{};
        memcpy(&header, payload.constData() + pos, sizeof(header));
        pos += int(sizeof(header));

        QVector<wvz3::SignalDirDeltaEntry> entries;
        entries.resize(int(header.signalCount));
        for (quint32 i = 0; i < header.signalCount; ++i) {
            if (pos + int(sizeof(wvz3::SignalDirDeltaEntry)) > payload.size()) {
                error = QStringLiteral("信号目录增量 entry 越界");
                return false;
            }
            memcpy(&entries[int(i)], payload.constData() + pos, sizeof(wvz3::SignalDirDeltaEntry));
            pos += int(sizeof(wvz3::SignalDirDeltaEntry));
        }

        quint32 stringSize = 0;
        if (pos + int(sizeof(quint32)) > payload.size()) {
            error = QStringLiteral("信号目录增量字符串表大小缺失");
            return false;
        }
        memcpy(&stringSize, payload.constData() + pos, sizeof(quint32));
        pos += int(sizeof(quint32));
        if (pos + int(stringSize) > payload.size()) {
            error = QStringLiteral("信号目录增量字符串表越界");
            return false;
        }

        const QByteArray stringTable = QByteArray::fromRawData(payload.constData() + pos, int(stringSize));

        quint32 maxSignalId = 0;
        bool hasSignalId = false;
        for (const auto& entry : std::as_const(entries)) {
            maxSignalId = hasSignalId ? qMax(maxSignalId, entry.dir.signalId) : entry.dir.signalId;
            hasSignalId = true;
        }
        if (hasSignalId) resizeSignalTableForMaxId(signalTable, maxSignalId);

        if (outDefs) {
            outDefs->clear();
            outDefs->reserve(int(header.signalCount));
        }

        for (const auto& entry : std::as_const(entries)) {
            QString name;
            if (!tryStringFromTableOffsetFast(stringTable, entry.dir.nameOffset, name)) {
                name = QStringLiteral("sig_%1").arg(entry.dir.signalId);
            }

            if (entry.dir.signalId < quint32(signalTable.size())) {
                SignalDirSlot& slot = signalTable[int(entry.dir.signalId)];
                slot.valid = true;
                slot.dir = entry.dir;
                slot.name = name;
            }

            if (outDefs) {
                DecodedDeltaSignalDef def;
                def.def.signalId = int(entry.dir.signalId);
                def.def.name = name;
                def.def.kind = (entry.dir.signalKind == wvz3::Sig_Bit) ? SignalKind::Bit : SignalKind::Bus;
                def.def.width = qMax(1, int(entry.dir.bitWidth));
                def.def.defaultRadix = radixFromDisk(entry.dir.defaultRadix);
                def.def.supportsZState = true;
                if (!tryStringFromTableOffsetFast(stringTable, entry.initialValueOffset, def.def.initialValue)) {
                    def.def.initialValue = defaultRawValueForSignal(def.def.kind, def.def.width);
                }
                outDefs->push_back(std::move(def));
            }
        }
        return true;
    }

    

    bool decodeSignalDirDeltaPayloadLegacy(const QByteArray& payload,
        QVector<SignalDirSlot>& signalTable,
        QVector<DecodedDeltaSignalDef>* outDefs,
        QString& error) {
        error.clear();
        int pos = 0;
        if (payload.size() < int(sizeof(wvz3::SignalDirDeltaPayloadHeader))) {
            error = QStringLiteral("信号目录增量 payload 太小");
            return false;
        }
        wvz3::SignalDirDeltaPayloadHeader header{};
        memcpy(&header, payload.constData() + pos, sizeof(header));
        pos += int(sizeof(header));

        QVector<wvz3::SignalDirEntry> entries;
        entries.resize(int(header.signalCount));
        for (quint32 i = 0; i < header.signalCount; ++i) {
            if (pos + int(sizeof(wvz3::SignalDirEntry)) > payload.size()) {
                error = QStringLiteral("信号目录增量 legacy entry 越界");
                return false;
            }
            memcpy(&entries[int(i)], payload.constData() + pos, sizeof(wvz3::SignalDirEntry));
            pos += int(sizeof(wvz3::SignalDirEntry));
        }

        quint32 stringSize = 0;
        if (pos + int(sizeof(quint32)) > payload.size()) {
            error = QStringLiteral("信号目录增量 legacy 字符串表大小缺失");
            return false;
        }
        memcpy(&stringSize, payload.constData() + pos, sizeof(quint32));
        pos += int(sizeof(quint32));
        if (pos + int(stringSize) > payload.size()) {
            error = QStringLiteral("信号目录增量 legacy 字符串表越界");
            return false;
        }

        const QByteArray stringTable = QByteArray::fromRawData(payload.constData() + pos, int(stringSize));

        quint32 maxSignalId = 0;
        bool hasSignalId = false;
        for (const auto& entry : std::as_const(entries)) {
            maxSignalId = hasSignalId ? qMax(maxSignalId, entry.signalId) : entry.signalId;
            hasSignalId = true;
        }
        if (hasSignalId) resizeSignalTableForMaxId(signalTable, maxSignalId);

        if (outDefs) {
            outDefs->clear();
            outDefs->reserve(int(header.signalCount));
        }

        for (const auto& entry : std::as_const(entries)) {
            QString name;
            if (!tryStringFromTableOffsetFast(stringTable, entry.nameOffset, name)) {
                name = QStringLiteral("sig_%1").arg(entry.signalId);
            }

            if (entry.signalId < quint32(signalTable.size())) {
                SignalDirSlot& slot = signalTable[int(entry.signalId)];
                slot.valid = true;
                slot.dir = entry;
                slot.name = name;
            }

            if (outDefs) {
                DecodedDeltaSignalDef def;
                def.def.signalId = int(entry.signalId);
                def.def.name = name;
                def.def.kind = (entry.signalKind == wvz3::Sig_Bit) ? SignalKind::Bit : SignalKind::Bus;
                def.def.width = qMax(1, int(entry.bitWidth));
                def.def.defaultRadix = radixFromDisk(entry.defaultRadix);
                def.def.supportsZState = true;
                def.def.initialValue = defaultRawValueForSignal(def.def.kind, def.def.width);
                outDefs->push_back(std::move(def));
            }
        }
        return true;
    }

    

    QByteArray encodeSignalRemoveDeltaPayload(const QVector<int>& signalIds) {
        QByteArray payload;
        wvz3::SignalRemoveDeltaPayloadHeader header{};
        header.signalCount = quint32(signalIds.size());
        appendPod(payload, header);
        for (int signalId : signalIds) {
            const quint32 id = quint32(signalId);
            appendPod(payload, id);
        }
        return payload;
    }

    bool decodeSignalRemoveDeltaPayload(const QByteArray& payload,
        QVector<quint32>& signalIds,
        QString& error) {
        error.clear();
        int pos = 0;
        if (payload.size() < int(sizeof(wvz3::SignalRemoveDeltaPayloadHeader))) {
            error = QStringLiteral("信号删除增量 payload 太小");
            return false;
        }
        wvz3::SignalRemoveDeltaPayloadHeader header{};
        memcpy(&header, payload.constData() + pos, sizeof(header));
        pos += int(sizeof(header));
        signalIds.clear();
        signalIds.reserve(int(header.signalCount));
        for (quint32 i = 0; i < header.signalCount; ++i) {
            if (pos + int(sizeof(quint32)) > payload.size()) {
                error = QStringLiteral("信号删除增量 id 越界");
                return false;
            }
            quint32 id = 0;
            memcpy(&id, payload.constData() + pos, sizeof(id));
            pos += int(sizeof(id));
            signalIds.push_back(id);
        }
        return true;
    }

    inline void appendCompactedSample(QVector<WaveSample>& rows, WaveSample&& sample) {
        if (!rows.isEmpty()) {
            WaveSample& last = rows.last();
            if (sample.time == last.time) {
                // Same-cycle updates use last-write-wins semantics.
                last = std::move(sample);
                return;
            }
            if (waveSamplesEquivalent(sample, last)) {
                return;
            }
        }
        rows.push_back(std::move(sample));
    }


    void appendSamplePoint(QVector<QVector<WaveSample>>& sampleTable,
        const quint32 signalId,
        const qint64 time,
        const QString& value,
        const SignalKind kind = SignalKind::Bus,
        const int width = 64) {
        ensureSampleTableSize(sampleTable, signalId);
        if (signalId >= quint32(sampleTable.size())) return;

        WaveSample ws;
        ws.time = time;
        ws.value = value;
        if (value == waveAbsentValue()) {
            ws.isAbsent = true;
            ws.isZ = false;
            ws.rawBits = 0ull;
            ws.rawFieldsReady = true;
        }
        else if (value.compare(QStringLiteral("z"), Qt::CaseInsensitive) == 0) {
            setDecodedRawFields(ws, true, 0ull, width);
        }
        else if (value == cachedZeroString()) {
            setDecodedRawFields(ws, false, 0ull, width);
        }
        else if (value == cachedOneString()) {
            setDecodedRawFields(ws, false, 1ull, width);
        }
        else {
            hydrateWaveSampleRawFields(kind, width, ws);
        }

        appendCompactedSample(sampleTable[int(signalId)], std::move(ws));
    }


    void appendAbsentIfNeeded(QVector<QVector<WaveSample>>& sampleTable,
        const quint32 signalId,
        const qint64 startTime) {
        ensureSampleTableSize(sampleTable, signalId);
        if (signalId >= quint32(sampleTable.size())) return;

        QVector<WaveSample>& rows = sampleTable[int(signalId)];
        if (rows.isEmpty()) {
            appendSamplePoint(sampleTable, signalId, startTime, waveAbsentValue());
            return;
        }
        bool hasEarlierOrEqual = false;
        for (const WaveSample& ws : std::as_const(rows)) {
            if (ws.time <= startTime) { hasEarlierOrEqual = true; break; }
        }
        if (!hasEarlierOrEqual) {
            appendSamplePoint(sampleTable, signalId, startTime, waveAbsentValue());
        }
    }

    bool signalNeedsByteEncoding(const SignalRuntimeView& sig) {
        if (sig.kind == SignalKind::Bit) return false;
        return sig.width > 64;
    }

    struct EncodedSignalRecord {
        quint32 signalId = 0;
        quint8 plainEncoding = 0;
        QByteArray plainEncoded;

        bool sharedEligible = false;
        quint8 sharedEncoding = 0;
        QByteArray sharedEncoded;
        QVector<qint64> pendingTimes;
        bool useShared = false;
    };

    QVector<qint64> collectSortedUniqueTimes(const QVector<qint64>& times) {
        QVector<qint64> out = times;
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    QHash<qint64, quint32> buildTimeToIndex(const QVector<qint64>& sortedTimes) {
        QHash<qint64, quint32> out;
        out.reserve(sortedTimes.size() * 2 + 1);
        for (int i = 0; i < sortedTimes.size(); ++i) out.insert(sortedTimes.at(i), quint32(i));
        return out;
    }

    QByteArray encodeSharedTimeTable(const QVector<qint64>& sortedTimes) {
        QByteArray out;
        qint64 prev = 0;
        for (qint64 t : sortedTimes) {
            out.append(encodeVarUInt(quint64(t - prev)));
            prev = t;
        }
        return out;
    }

    bool decodeSharedTimeTable(const QByteArray& payload,
        int& pos,
        quint32 sharedTimeCount,
        QVector<qint64>& outTimes) {
        outTimes.clear();
        outTimes.reserve(int(sharedTimeCount));
        qint64 prev = 0;
        for (quint32 i = 0; i < sharedTimeCount; ++i) {
            quint64 dt = 0;
            if (!decodeVarUInt(payload, pos, dt)) return false;
            prev += qint64(dt);
            outTimes.push_back(prev);
        }
        return true;
    }

    bool buildBitRecord(const SignalRuntimeView& sig,
        const WaveParser3::SaveOptions& options,
        QByteArray& outEncoded) {
        Q_UNUSED(options);
        outEncoded.clear();
        const bool initialIsZ = isWaveZValue(sig.blockInitialValue);
        outEncoded.append(char(initialIsZ ? 0u : encodeBitState(sig.blockInitialValue)));
        outEncoded.append(char(initialIsZ ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        qint64 prev = 0;
        for (const auto& item : sig.pending) {
            const bool isZ = isWaveZValue(item.value);
            outEncoded.append(encodeVarUInt(quint64(item.time - prev)));
            outEncoded.append(char(isZ ? 0u : encodeBitState(item.value)));
            outEncoded.append(char(isZ ? 1u : 0u));
            prev = item.time;
        }
        return true;
    }

    bool buildBitSharedIndexRecord(const SignalRuntimeView& sig,
        const QHash<qint64, quint32>& timeToIndex,
        QByteArray& outEncoded) {
        outEncoded.clear();
        const bool initialIsZ = isWaveZValue(sig.blockInitialValue);
        outEncoded.append(char(initialIsZ ? 0u : encodeBitState(sig.blockInitialValue)));
        outEncoded.append(char(initialIsZ ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        quint64 prevIndex = 0;
        for (const auto& item : sig.pending) {
            if (!timeToIndex.contains(item.time)) return false;
            const quint64 index = timeToIndex.value(item.time);
            const bool isZ = isWaveZValue(item.value);
            outEncoded.append(encodeVarUInt(index - prevIndex));
            outEncoded.append(char(isZ ? 0u : encodeBitState(item.value)));
            outEncoded.append(char(isZ ? 1u : 0u));
            prevIndex = index;
        }
        return true;
    }

    bool buildBusU64Record(const SignalRuntimeView& sig,
        const WaveParser3::SaveOptions& options,
        QByteArray& outEncoded) {
        Q_UNUSED(options);
        outEncoded.clear();
        const quint64 initial = parseValueU64(sig.blockInitialValue);
        appendPod(outEncoded, initial);
        outEncoded.append(char(isWaveZValue(sig.blockInitialValue) ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        qint64 prev = 0;
        for (const auto& item : sig.pending) {
            outEncoded.append(encodeVarUInt(quint64(item.time - prev)));
            appendPod(outEncoded, parseValueU64(item.value));
            outEncoded.append(char(isWaveZValue(item.value) ? 1u : 0u));
            prev = item.time;
        }
        return true;
    }

    bool buildBusU64SharedIndexRecord(const SignalRuntimeView& sig,
        const QHash<qint64, quint32>& timeToIndex,
        QByteArray& outEncoded) {
        outEncoded.clear();
        const quint64 initial = parseValueU64(sig.blockInitialValue);
        appendPod(outEncoded, initial);
        outEncoded.append(char(isWaveZValue(sig.blockInitialValue) ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        quint64 prevIndex = 0;
        for (const auto& item : sig.pending) {
            if (!timeToIndex.contains(item.time)) return false;
            const quint64 index = timeToIndex.value(item.time);
            outEncoded.append(encodeVarUInt(index - prevIndex));
            appendPod(outEncoded, parseValueU64(item.value));
            outEncoded.append(char(isWaveZValue(item.value) ? 1u : 0u));
            prevIndex = index;
        }
        return true;
    }

    bool buildBusBytesRecordLegacy(const SignalRuntimeView& sig,
        const WaveParser3::SaveOptions& options,
        QByteArray& outEncoded) {
        Q_UNUSED(options);
        outEncoded.clear();
        const QByteArray initial = encodeWideValueLegacy(sig.blockInitialValue);
        outEncoded.append(encodeVarUInt(quint64(initial.size())));
        outEncoded.append(initial);
        outEncoded.append(char(isWaveZValue(sig.blockInitialValue) ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        qint64 prev = 0;
        for (const auto& item : sig.pending) {
            const QByteArray raw = encodeWideValueLegacy(item.value);
            outEncoded.append(encodeVarUInt(quint64(item.time - prev)));
            outEncoded.append(encodeVarUInt(quint64(raw.size())));
            outEncoded.append(raw);
            outEncoded.append(char(isWaveZValue(item.value) ? 1u : 0u));
            prev = item.time;
        }
        return true;
    }

    bool buildBusBytesSharedIndexRecordLegacy(const SignalRuntimeView& sig,
        const QHash<qint64, quint32>& timeToIndex,
        QByteArray& outEncoded) {
        outEncoded.clear();
        const QByteArray initial = encodeWideValueLegacy(sig.blockInitialValue);
        outEncoded.append(encodeVarUInt(quint64(initial.size())));
        outEncoded.append(initial);
        outEncoded.append(char(isWaveZValue(sig.blockInitialValue) ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        quint64 prevIndex = 0;
        for (const auto& item : sig.pending) {
            if (!timeToIndex.contains(item.time)) return false;
            const quint64 index = timeToIndex.value(item.time);
            const QByteArray raw = encodeWideValueLegacy(item.value);
            outEncoded.append(encodeVarUInt(index - prevIndex));
            outEncoded.append(encodeVarUInt(quint64(raw.size())));
            outEncoded.append(raw);
            outEncoded.append(char(isWaveZValue(item.value) ? 1u : 0u));
            prevIndex = index;
        }
        return true;
    }

    bool buildBusPackedBytesRecord(const SignalRuntimeView& sig,
        QByteArray& outEncoded) {
        outEncoded.clear();
        QByteArray initial;
        if (!tryEncodePackedWideValue(sig.blockInitialValue, sig.width, sig.defaultRadix, initial)) return false;
        outEncoded.append(encodeVarUInt(quint64(initial.size())));
        outEncoded.append(initial);
        outEncoded.append(char(isWaveZValue(sig.blockInitialValue) ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        qint64 prev = 0;
        for (const auto& item : sig.pending) {
            QByteArray raw;
            if (!tryEncodePackedWideValue(item.value, sig.width, sig.defaultRadix, raw)) return false;
            outEncoded.append(encodeVarUInt(quint64(item.time - prev)));
            outEncoded.append(encodeVarUInt(quint64(raw.size())));
            outEncoded.append(raw);
            outEncoded.append(char(isWaveZValue(item.value) ? 1u : 0u));
            prev = item.time;
        }
        return true;
    }

    bool buildBusPackedBytesSharedIndexRecord(const SignalRuntimeView& sig,
        const QHash<qint64, quint32>& timeToIndex,
        QByteArray& outEncoded) {
        outEncoded.clear();
        QByteArray initial;
        if (!tryEncodePackedWideValue(sig.blockInitialValue, sig.width, sig.defaultRadix, initial)) return false;
        outEncoded.append(encodeVarUInt(quint64(initial.size())));
        outEncoded.append(initial);
        outEncoded.append(char(isWaveZValue(sig.blockInitialValue) ? 1u : 0u));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        quint64 prevIndex = 0;
        for (const auto& item : sig.pending) {
            if (!timeToIndex.contains(item.time)) return false;
            const quint64 index = timeToIndex.value(item.time);
            QByteArray raw;
            if (!tryEncodePackedWideValue(item.value, sig.width, sig.defaultRadix, raw)) return false;
            outEncoded.append(encodeVarUInt(index - prevIndex));
            outEncoded.append(encodeVarUInt(quint64(raw.size())));
            outEncoded.append(raw);
            outEncoded.append(char(isWaveZValue(item.value) ? 1u : 0u));
            prevIndex = index;
        }
        return true;
    }

    bool tryBuildClockRecord(const SignalRuntimeView& sig,
        const WaveParser3::SaveOptions& options,
        QByteArray& outEncoded) {
        if (sig.kind != SignalKind::Bit) return false;
        if (sig.pending.size() < options.minClockToggleCount) return false;

        QVector<qint64> dts;
        dts.reserve(sig.pending.size());
        qint64 prev = 0;
        quint8 prevState = encodeBitState(sig.blockInitialValue);
        if (prevState > 1u) return false; // clock optimization only supports 0/1 two-state signals

        for (const auto& item : sig.pending) {
            const quint8 state = encodeBitState(item.value);
            if (state > 1u) return false;
            if (state == prevState) return false;
            dts.push_back(item.time - prev);
            prev = item.time;
            prevState = state;
        }
        if (dts.size() < 2) return false;

        const qint64 halfPeriod = dts.at(1);
        for (int i = 2; i < dts.size(); ++i) {
            const qint64 a = dts.at(i);
            const qint64 diff = (a > halfPeriod) ? (a - halfPeriod) : (halfPeriod - a);
            if (diff > qint64(options.clockHalfPeriodTolerance)) return false;
        }

        outEncoded.clear();
        outEncoded.append(char(encodeBitState(sig.blockInitialValue)));
        outEncoded.append(encodeVarUInt(quint64(dts.at(0))));
        outEncoded.append(encodeVarUInt(quint64(halfPeriod)));
        outEncoded.append(encodeVarUInt(quint64(sig.pending.size())));
        outEncoded.append(char(encodeBitState(sig.pending.first().value)));
        return true;
    }



    bool decodeBitRecord(const ByteSpan& encoded,
        qint64 blockStart,
        qint64 blockEnd,
        quint64 timescalePs,
        bool supportsZ,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(blockEnd);
        if (encoded.isEmpty()) return false;

        int pos = 0;
        const quint8 initial = quint8(encoded.at(pos++));
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }
        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : decodeBitStateCached(initial);
        setDecodedRawFields(ws0, initialIsZ, quint64(initial & 1u), 1);
        outSamples.push_back(std::move(ws0));

        qint64 t = blockStart;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 dt = 0;
            if (!decodeVarUInt(encoded, pos, dt) || pos >= encoded.size()) return false;
            t += qint64(dt);
            const quint8 state = quint8(encoded.at(pos++));
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }
            else {
                isZ = (state & 0x3u) == 2u;
            }
            WaveSample ws;
            ws.time = ticksToTime(t, timescalePs);
            ws.value = isZ ? cachedZString() : decodeBitStateCached(state);
            setDecodedRawFields(ws, isZ, quint64(state & 1u), 1);
            outSamples.push_back(std::move(ws));
        }
        return true;
    }

    bool decodeBitSharedIndexRecord(const ByteSpan& encoded,
        const QVector<qint64>& sharedTimes,
        qint64 blockStart,
        quint64 timescalePs,
        bool supportsZ,
        QVector<WaveSample>& outSamples) {
        if (encoded.isEmpty()) return false;

        int pos = 0;
        const quint8 initial = quint8(encoded.at(pos++));
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }
        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : decodeBitStateCached(initial);
        setDecodedRawFields(ws0, initialIsZ, quint64(initial & 1u), 1);
        outSamples.push_back(std::move(ws0));

        quint64 currentIndex = 0;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 deltaIndex = 0;
            if (!decodeVarUInt(encoded, pos, deltaIndex) || pos >= encoded.size()) return false;
            currentIndex += deltaIndex;
            if (currentIndex >= quint64(sharedTimes.size())) return false;
            const quint8 state = quint8(encoded.at(pos++));
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }
            else {
                isZ = (state & 0x3u) == 2u;
            }
            WaveSample ws;
            ws.time = ticksToTime(blockStart + sharedTimes.at(int(currentIndex)), timescalePs);
            ws.value = isZ ? cachedZString() : decodeBitStateCached(state);
            setDecodedRawFields(ws, isZ, quint64(state & 1u), 1);
            outSamples.push_back(std::move(ws));
        }
        return true;
    }

    bool decodeBusU64Record(const ByteSpan& encoded,
        qint64 blockStart,
        qint64 blockEnd,
        quint64 timescalePs,
        int bitWidth,
        bool supportsZ,
        ValueRadix defaultRadix,
        ValueTextInterner& textPool,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(defaultRadix);
        Q_UNUSED(blockEnd);
        if (encoded.size() < int(sizeof(quint64))) return false;

        int pos = 0;
        quint64 initial = 0;
        memcpy(&initial, encoded.constData() + pos, sizeof(initial));
        pos += int(sizeof(initial));
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }

        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : textPool.u64Text(initial, bitWidth);
        setDecodedRawFields(ws0, initialIsZ, initial, bitWidth);
        outSamples.push_back(std::move(ws0));

        qint64 t = blockStart;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 dt = 0;
            if (!decodeVarUInt(encoded, pos, dt) || pos + int(sizeof(quint64)) > encoded.size()) return false;
            t += qint64(dt);
            quint64 value = 0;
            memcpy(&value, encoded.constData() + pos, sizeof(value));
            pos += int(sizeof(value));
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }
            WaveSample ws;
            ws.time = ticksToTime(t, timescalePs);
            ws.value = isZ ? cachedZString() : textPool.u64Text(value, bitWidth);
            setDecodedRawFields(ws, isZ, value, bitWidth);
            outSamples.push_back(std::move(ws));
        }
        return true;
    }

    bool decodeBusU64SharedIndexRecord(const ByteSpan& encoded,
        const QVector<qint64>& sharedTimes,
        qint64 blockStart,
        quint64 timescalePs,
        int bitWidth,
        bool supportsZ,
        ValueRadix defaultRadix,
        ValueTextInterner& textPool,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(defaultRadix);
        if (encoded.size() < int(sizeof(quint64))) return false;

        int pos = 0;
        quint64 initial = 0;
        memcpy(&initial, encoded.constData() + pos, sizeof(initial));
        pos += int(sizeof(initial));
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }

        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : textPool.u64Text(initial, bitWidth);
        setDecodedRawFields(ws0, initialIsZ, initial, bitWidth);
        outSamples.push_back(std::move(ws0));

        quint64 currentIndex = 0;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 deltaIndex = 0;
            if (!decodeVarUInt(encoded, pos, deltaIndex) || pos + int(sizeof(quint64)) > encoded.size()) return false;
            currentIndex += deltaIndex;
            if (currentIndex >= quint64(sharedTimes.size())) return false;
            quint64 value = 0;
            memcpy(&value, encoded.constData() + pos, sizeof(value));
            pos += int(sizeof(value));
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }

            WaveSample ws;
            ws.time = ticksToTime(blockStart + sharedTimes.at(int(currentIndex)), timescalePs);
            ws.value = isZ ? cachedZString() : textPool.u64Text(value, bitWidth);
            setDecodedRawFields(ws, isZ, value, bitWidth);
            outSamples.push_back(std::move(ws));
        }
        return true;
    }

    bool decodeBusBytesRecordLegacy(const ByteSpan& encoded,
        qint64 blockStart,
        qint64 blockEnd,
        quint64 timescalePs,
        int bitWidth,
        bool supportsZ,
        ValueRadix defaultRadix,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(defaultRadix);
        Q_UNUSED(blockEnd);
        int pos = 0;
        quint64 initialLen = 0;
        if (!decodeVarUInt(encoded, pos, initialLen)) return false;
        if (pos + int(initialLen) > encoded.size()) return false;
        const QByteArray initial = encoded.mid(pos, int(initialLen));
        pos += int(initialLen);
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }

        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : decodeWideValueLegacy(initial);
        outSamples.push_back(ws0);

        qint64 t = blockStart;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 dt = 0;
            quint64 valueLen = 0;
            if (!decodeVarUInt(encoded, pos, dt)) return false;
            if (!decodeVarUInt(encoded, pos, valueLen)) return false;
            if (pos + int(valueLen) > encoded.size()) return false;
            t += qint64(dt);
            const QByteArray raw = encoded.mid(pos, int(valueLen));
            pos += int(valueLen);
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }

            WaveSample ws;
            ws.time = ticksToTime(t, timescalePs);
            ws.value = isZ ? cachedZString() : decodeWideValueLegacy(raw);
            outSamples.push_back(ws);
        }
        return true;
    }

    bool decodeBusBytesSharedIndexRecordLegacy(const ByteSpan& encoded,
        const QVector<qint64>& sharedTimes,
        qint64 blockStart,
        quint64 timescalePs,
        int bitWidth,
        bool supportsZ,
        ValueRadix defaultRadix,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(defaultRadix);
        int pos = 0;
        quint64 initialLen = 0;
        if (!decodeVarUInt(encoded, pos, initialLen)) return false;
        if (pos + int(initialLen) > encoded.size()) return false;
        const QByteArray initial = encoded.mid(pos, int(initialLen));
        pos += int(initialLen);
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }

        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : decodeWideValueLegacy(initial);
        outSamples.push_back(ws0);

        quint64 currentIndex = 0;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 deltaIndex = 0;
            quint64 valueLen = 0;
            if (!decodeVarUInt(encoded, pos, deltaIndex)) return false;
            if (!decodeVarUInt(encoded, pos, valueLen)) return false;
            if (pos + int(valueLen) > encoded.size()) return false;
            currentIndex += deltaIndex;
            if (currentIndex >= quint64(sharedTimes.size())) return false;

            const QByteArray raw = encoded.mid(pos, int(valueLen));
            pos += int(valueLen);
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }

            WaveSample ws;
            ws.time = ticksToTime(blockStart + sharedTimes.at(int(currentIndex)), timescalePs);
            ws.value = isZ ? cachedZString() : decodeWideValueLegacy(raw);
            outSamples.push_back(ws);
        }
        return true;
    }

    bool decodeBusPackedBytesRecord(const ByteSpan& encoded,
        qint64 blockStart,
        qint64 blockEnd,
        quint64 timescalePs,
        int bitWidth,
        bool supportsZ,
        ValueRadix defaultRadix,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(defaultRadix);
        Q_UNUSED(blockEnd);
        int pos = 0;
        quint64 initialLen = 0;
        if (!decodeVarUInt(encoded, pos, initialLen)) return false;
        if (pos + int(initialLen) > encoded.size()) return false;
        const QByteArray initial = encoded.mid(pos, int(initialLen));
        pos += int(initialLen);
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }

        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : rawPackedBytesText(initial, bitWidth);
        setDecodedRawFields(ws0, initialIsZ, low64FromPackedBytes(initial, bitWidth), bitWidth);
        outSamples.push_back(std::move(ws0));

        qint64 t = blockStart;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 dt = 0;
            quint64 valueLen = 0;
            if (!decodeVarUInt(encoded, pos, dt)) return false;
            if (!decodeVarUInt(encoded, pos, valueLen)) return false;
            if (pos + int(valueLen) > encoded.size()) return false;
            t += qint64(dt);
            const QByteArray raw = encoded.mid(pos, int(valueLen));
            pos += int(valueLen);
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }

            WaveSample ws;
            ws.time = ticksToTime(t, timescalePs);
            ws.value = isZ ? cachedZString() : rawPackedBytesText(raw, bitWidth);
            setDecodedRawFields(ws, isZ, low64FromPackedBytes(raw, bitWidth), bitWidth);
            outSamples.push_back(std::move(ws));
        }
        return true;
    }

    bool decodeBusPackedBytesSharedIndexRecord(const ByteSpan& encoded,
        const QVector<qint64>& sharedTimes,
        qint64 blockStart,
        quint64 timescalePs,
        int bitWidth,
        bool supportsZ,
        ValueRadix defaultRadix,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(defaultRadix);
        int pos = 0;
        quint64 initialLen = 0;
        if (!decodeVarUInt(encoded, pos, initialLen)) return false;
        if (pos + int(initialLen) > encoded.size()) return false;
        const QByteArray initial = encoded.mid(pos, int(initialLen));
        pos += int(initialLen);
        bool initialIsZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            initialIsZ = encoded.at(pos++) != 0;
        }

        quint64 eventCount = 0;
        if (!decodeVarUInt(encoded, pos, eventCount)) return false;
        reserveAdditionalSamples(outSamples, eventCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = initialIsZ ? cachedZString() : rawPackedBytesText(initial, bitWidth);
        setDecodedRawFields(ws0, initialIsZ, low64FromPackedBytes(initial, bitWidth), bitWidth);
        outSamples.push_back(std::move(ws0));

        quint64 currentIndex = 0;
        for (quint64 i = 0; i < eventCount; ++i) {
            quint64 deltaIndex = 0;
            quint64 valueLen = 0;
            if (!decodeVarUInt(encoded, pos, deltaIndex)) return false;
            if (!decodeVarUInt(encoded, pos, valueLen)) return false;
            if (pos + int(valueLen) > encoded.size()) return false;
            currentIndex += deltaIndex;
            if (currentIndex >= quint64(sharedTimes.size())) return false;

            const QByteArray raw = encoded.mid(pos, int(valueLen));
            pos += int(valueLen);
            bool isZ = false;
            if (supportsZ) {
                if (pos >= encoded.size()) return false;
                isZ = encoded.at(pos++) != 0;
            }

            WaveSample ws;
            ws.time = ticksToTime(blockStart + sharedTimes.at(int(currentIndex)), timescalePs);
            ws.value = isZ ? cachedZString() : rawPackedBytesText(raw, bitWidth);
            setDecodedRawFields(ws, isZ, low64FromPackedBytes(raw, bitWidth), bitWidth);
            outSamples.push_back(std::move(ws));
        }
        return true;
    }

    bool decodeClockRecord(const ByteSpan& encoded,
        qint64 blockStart,
        qint64 blockEnd,
        quint64 timescalePs,
        bool supportsZ,
        QVector<WaveSample>& outSamples) {
        Q_UNUSED(supportsZ);
        if (encoded.isEmpty()) return false;

        int pos = 0;
        quint8 current = quint8(encoded.at(pos++));
        quint64 dt0 = 0;
        quint64 halfPeriod = 0;
        quint64 toggleCount = 0;
        if (!decodeVarUInt(encoded, pos, dt0)) return false;
        if (!decodeVarUInt(encoded, pos, halfPeriod)) return false;
        if (!decodeVarUInt(encoded, pos, toggleCount)) return false;
        if (pos >= encoded.size()) return false;
        reserveAdditionalSamples(outSamples, toggleCount + 1);

        WaveSample ws0;
        ws0.time = ticksToTime(blockStart, timescalePs);
        ws0.value = decodeBitStateCached(current);
        setDecodedRawFields(ws0, false, quint64(current & 1u), 1);
        outSamples.push_back(std::move(ws0));

        qint64 t = blockStart + qint64(dt0);
        current = quint8(encoded.at(pos++));
        if (t < blockEnd) {
            WaveSample ws;
            ws.time = ticksToTime(t, timescalePs);
            ws.value = decodeBitStateCached(current);
            setDecodedRawFields(ws, false, quint64(current & 1u), 1);
            outSamples.push_back(std::move(ws));
        }

        for (quint64 i = 1; i < toggleCount; ++i) {
            t += qint64(halfPeriod);
            current = (current == 0) ? 1 : 0;
            if (t >= blockEnd) break;
            WaveSample ws;
            ws.time = ticksToTime(t, timescalePs);
            ws.value = decodeBitStateCached(current);
            setDecodedRawFields(ws, false, quint64(current & 1u), 1);
            outSamples.push_back(std::move(ws));
        }
        return true;
    }


    bool decodeBlockPayload(const QByteArray& payload,
        const QVector<SignalDirSlot>& signalTable,
        qint64 blockStart,
        qint64 blockEnd,
        quint64 timescalePs,
        const SignalIdFilter& signalFilter,
        bool hasSharedTimeHeader,
        QVector<QVector<WaveSample>>& outSamples,
        QString& error) {
        error.clear();
        int pos = 0;
        quint32 signalRecordCount = 0;
        quint32 sharedTimeCount = 0;

        if (hasSharedTimeHeader) {
            if (payload.size() < int(sizeof(wvz3::BlockPayloadHeader))) {
                error = QStringLiteral("payload 太小");
                return false;
            }
            wvz3::BlockPayloadHeader payloadHeader{};
            memcpy(&payloadHeader, payload.constData(), sizeof(payloadHeader));
            signalRecordCount = payloadHeader.signalRecordCount;
            sharedTimeCount = payloadHeader.sharedTimeCount;
            pos += int(sizeof(payloadHeader));
        }
        else {
            if (payload.size() < int(sizeof(quint32))) {
                error = QStringLiteral("legacy payload 太小");
                return false;
            }
            memcpy(&signalRecordCount, payload.constData(), sizeof(signalRecordCount));
            pos += int(sizeof(signalRecordCount));
        }

        QVector<qint64> sharedTimes;
        if (sharedTimeCount > 0) {
            if (!decodeSharedTimeTable(payload, pos, sharedTimeCount, sharedTimes)) {
                error = QStringLiteral("解码共享时间表失败");
                return false;
            }
        }

        ValueTextInterner textPool;

        for (quint32 i = 0; i < signalRecordCount; ++i) {
            if (pos + int(sizeof(wvz3::SignalRecordHeader)) > payload.size()) {
                error = QStringLiteral("signal record header 越界");
                return false;
            }

            wvz3::SignalRecordHeader recordHeader{};
            memcpy(&recordHeader, payload.constData() + pos, sizeof(recordHeader));
            pos += int(sizeof(recordHeader));

            if (pos + int(recordHeader.dataSize) > payload.size()) {
                error = QStringLiteral("signal record data 越界");
                return false;
            }

            const ByteSpan encoded{ payload.constData() + pos, int(recordHeader.dataSize) };
            pos += int(recordHeader.dataSize);

            if (!signalFilter.keep(recordHeader.signalId)) continue;

            if (recordHeader.signalId >= quint32(signalTable.size()) ||
                !signalTable[int(recordHeader.signalId)].valid) {
                error = QStringLiteral("signalId=%1 缺少信号目录定义").arg(recordHeader.signalId);
                return false;
            }

            ensureSampleTableSize(outSamples, recordHeader.signalId);
            if (recordHeader.signalId >= quint32(outSamples.size())) {
                error = QStringLiteral("signalId=%1 超出可索引范围").arg(recordHeader.signalId);
                return false;
            }

            QVector<WaveSample>& dst = outSamples[int(recordHeader.signalId)];
            const wvz3::SignalDirEntry& dir = signalTable[int(recordHeader.signalId)].dir;
            const bool supportsZ = (dir.signalFlags & wvz3::Signal_SupportsZ) != 0;
            const ValueRadix radix = radixFromDisk(dir.defaultRadix);

            bool ok = false;
            switch (recordHeader.encoding) {
            case wvz3::Enc_BitTransitions:
                ok = decodeBitRecord(encoded, blockStart, blockEnd, timescalePs, supportsZ, dst);
                break;
            case wvz3::Enc_BusTransitionsU64:
                ok = decodeBusU64Record(encoded, blockStart, blockEnd, timescalePs, dir.bitWidth, supportsZ, radix, textPool, dst);
                break;
            case wvz3::Enc_BusTransitionsBytes:
                ok = decodeBusBytesRecordLegacy(encoded, blockStart, blockEnd, timescalePs, dir.bitWidth, supportsZ, radix, dst);
                break;
            case wvz3::Enc_ClockDeltaPattern:
                ok = decodeClockRecord(encoded, blockStart, blockEnd, timescalePs, supportsZ, dst);
                break;
            case wvz3::Enc_BitSharedTimeIndex:
                ok = decodeBitSharedIndexRecord(encoded, sharedTimes, blockStart, timescalePs, supportsZ, dst);
                break;
            case wvz3::Enc_BusTransitionsU64Shared:
                ok = decodeBusU64SharedIndexRecord(encoded, sharedTimes, blockStart, timescalePs, dir.bitWidth, supportsZ, radix, textPool, dst);
                break;
            case wvz3::Enc_BusTransitionsBytesShared:
                ok = decodeBusBytesSharedIndexRecordLegacy(encoded, sharedTimes, blockStart, timescalePs, dir.bitWidth, supportsZ, radix, dst);
                break;
            case wvz3::Enc_BusTransitionsPackedBytes:
                ok = decodeBusPackedBytesRecord(encoded, blockStart, blockEnd, timescalePs, dir.bitWidth, supportsZ, radix, dst);
                break;
            case wvz3::Enc_BusTransitionsPackedBytesShared:
                ok = decodeBusPackedBytesSharedIndexRecord(encoded, sharedTimes, blockStart, timescalePs, dir.bitWidth, supportsZ, radix, dst);
                break;
            default:
                error = QStringLiteral("不支持的记录编码：%1").arg(recordHeader.encoding);
                return false;
            }
            if (!ok) {
                error = QStringLiteral("解码 signalId=%1 失败").arg(recordHeader.signalId);
                return false;
            }
        }
        return true;
    }


    void compactSamples(QVector<WaveSample>& rows) {
        if (rows.size() <= 1) return;

        bool sorted = true;
        for (int i = 1; i < rows.size(); ++i) {
            if (rows.at(i - 1).time > rows.at(i).time) {
                sorted = false;
                break;
            }
        }
        if (!sorted) {
            std::stable_sort(rows.begin(), rows.end(), [](const WaveSample& a, const WaveSample& b) {
                return a.time < b.time;
                });
        }

        int write = 0;
        for (int read = 0; read < rows.size(); ++read) {
            if (write == 0) {
                if (write != read) rows[write] = std::move(rows[read]);
                ++write;
                continue;
            }

            WaveSample& last = rows[write - 1];
            WaveSample& row = rows[read];
            if (row.time == last.time) {
                // Same-cycle updates use last-write-wins semantics.
                last = std::move(row);
                continue;
            }
            if (waveSamplesEquivalent(row, last)) continue;

            if (write != read) rows[write] = std::move(row);
            ++write;
        }
        if (write < rows.size()) rows.resize(write);
    }


    void hydrateSamplesIfNeeded(const SignalKind kind, const int width, QVector<WaveSample>& rows) {
        bool needsHydrate = false;
        for (const WaveSample& ws : std::as_const(rows)) {
            if (!ws.rawFieldsReady) {
                needsHydrate = true;
                break;
            }
        }
        if (!needsHydrate) return;

        for (WaveSample& ws : rows) {
            hydrateWaveSampleRawFields(kind, width, ws);
        }
    }


    qint64 alignDown(qint64 value, qint64 span) {
        if (span <= 0) return value;
        if (value >= 0) return (value / span) * span;
        return -(((-value + span - 1) / span) * span);
    }

} // namespace

Wvz3StreamWriter::Wvz3StreamWriter() = default;

Wvz3StreamWriter::~Wvz3StreamWriter() {
    QString ignored;
    close(ignored, true);
}

void Wvz3StreamWriter::resetState() {
    if (m_file.isOpen()) m_file.close();
    m_title.clear();
    m_options = Options{};
    m_startCycle = 0;
    m_currentBlockStart = 0;
    m_lastAppendedCycle = std::numeric_limits<qint64>::min();
    m_lastObservedCycle = std::numeric_limits<qint64>::min();
    m_haveAnySample = false;
    m_finalized = false;
    m_nextBlockId = 0;
    m_metaOffset = 0;
    m_signalDirOffset = 0;
    m_dataRegionOffset = 0;
    m_signalOrder.clear();
    m_stateById.clear();
    m_knownSignalIds.clear();
}

bool Wvz3StreamWriter::open(const QString & filePath,
    const QVector<SignalDefinition>&sigDefs,
    QString & error,
    const QString & title,
    qint64 startCycle,
    const Options & options) {
    resetState();
    error.clear();

    if (sigDefs.isEmpty()) {
        error = QStringLiteral("wvz3 打开失败：没有信号定义");
        return false;
    }
    if (options.targetBlockSpan <= 0) {
        error = QStringLiteral("wvz3 打开失败：targetBlockSpan 必须 > 0");
        return false;
    }

    m_title = title.isEmpty() ? QStringLiteral("stream_wave") : title;
    m_options = options;
    m_startCycle = startCycle;
    m_currentBlockStart = alignDown(startCycle, m_options.targetBlockSpan);

    QHash<int, bool> seenIds;
    int autoId = 1;
    for (const SignalDefinition& inputDef : sigDefs) {
        SignalDefinition def = inputDef;
        while (seenIds.contains(autoId)) ++autoId;
        if (def.signalId <= 0) def.signalId = autoId;
        if (def.name.trimmed().isEmpty()) {
            error = QStringLiteral("wvz3 打开失败：存在空信号名");
            resetState();
            return false;
        }
        if (seenIds.contains(def.signalId)) {
            error = QStringLiteral("wvz3 打开失败：signalId=%1 重复").arg(def.signalId);
            resetState();
            return false;
        }
        seenIds.insert(def.signalId, true);
        if (def.width <= 0) def.width = 1;
        if (def.initialValue.trimmed().isEmpty()) def.initialValue = defaultRawValueForSignal(def.kind, def.width);
        if (containsXStateText(def.initialValue)) {
            error = QStringLiteral("wvz3 打开失败：当前版本不支持 x 态，signal=%1").arg(def.name);
            resetState();
            return false;
        }
        if (def.kind == SignalKind::Bit) {
            def.initialValue = normalizeBitValueFast(def.initialValue);
            if (!isValidBitValue(def.initialValue, true)) {
                error = QStringLiteral("wvz3 打开失败：bit 初值仅支持 0/1/Z，signal=%1 value=%2").arg(def.name).arg(def.initialValue);
                resetState();
                return false;
            }
        }
        else {
            def.initialValue = isWaveZValue(def.initialValue) ? QStringLiteral("Z") : normalizeNumericText(def.initialValue);
        }
        def.supportsZState = true;

        RuntimeSignalState runtime;
        runtime.def = def;
        runtime.blockInitialValue = def.initialValue;
        runtime.currentValue = def.initialValue;
        m_signalOrder.push_back(def.signalId);
        m_stateById.insert(def.signalId, runtime);
        m_knownSignalIds.insert(def.signalId, true);
        autoId = qMax(autoId, def.signalId + 1);
    }

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("无法写入文件：%1").arg(filePath);
        resetState();
        return false;
    }

    if (!writeInitialLayout(error)) {
        resetState();
        return false;
    }
    return true;
}

bool Wvz3StreamWriter::writeInitialLayout(QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 写初始布局失败：文件未打开");
        return false;
    }

    QVector<QString> names;
    names.reserve(m_signalOrder.size());
    for (int signalId : std::as_const(m_signalOrder)) names.push_back(m_stateById.value(signalId).def.name);
    QVector<quint32> nameOffsets;
    const QByteArray stringTable = joinStrings(names, nameOffsets);

    wvz3::FileHeader header{};
    header.magic = wvz3::kMagic;
    header.versionMajor = wvz3::kVersionMajor;
    header.versionMinor = wvz3::kVersionMinor;
    header.globalFlags = wvz3::Global_LittleEndian;
    header.headerSize = sizeof(wvz3::FileHeader);
    header.metaOffset = sizeof(wvz3::FileHeader);
    header.metaSize = sizeof(wvz3::MetaBlock);

    wvz3::MetaBlock meta{};
    meta.startTime = timeToTicks(m_startCycle, m_options.timescalePs);
    meta.endTime = meta.startTime;
    meta.timescalePs = m_options.timescalePs;
    meta.targetBlockSpan = m_options.targetBlockSpan;
    meta.signalCount = quint32(m_knownSignalIds.size());
    meta.committedBlockCount = 0;
    meta.markerCount = 0;

    m_metaOffset = qint64(header.metaOffset);
    m_signalDirOffset = m_metaOffset + qint64(sizeof(meta));
    header.signalDirOffset = quint64(m_signalDirOffset);

    const quint64 sigDirSize = sizeof(quint32)
        + quint64(sizeof(wvz3::SignalDirEntry) * m_signalOrder.size())
        + sizeof(quint32)
        + quint64(stringTable.size());
    header.signalDirSize = sigDirSize;
    m_dataRegionOffset = m_signalDirOffset + qint64(sigDirSize);
    header.dataRegionOffset = quint64(m_dataRegionOffset);
    header.dataRegionSizeHint = 0;
    header.fileSizeHint = quint64(m_dataRegionOffset);

    if (m_file.write(reinterpret_cast<const char*>(&header), sizeof(header)) != sizeof(header)) {
        error = QStringLiteral("写 WVZ3 文件头失败");
        return false;
    }
    if (m_file.write(reinterpret_cast<const char*>(&meta), sizeof(meta)) != sizeof(meta)) {
        error = QStringLiteral("写 WVZ3 MetaBlock 失败");
        return false;
    }

    const quint32 sigCount = quint32(m_signalOrder.size());
    if (m_file.write(reinterpret_cast<const char*>(&sigCount), sizeof(sigCount)) != sizeof(sigCount)) {
        error = QStringLiteral("写 WVZ3 signal count 失败");
        return false;
    }

    for (int i = 0; i < m_signalOrder.size(); ++i) {
        const RuntimeSignalState runtime = m_stateById.value(m_signalOrder.at(i));
        const SignalDefinition& def = runtime.def;
        wvz3::SignalDirEntry entry = makeSignalDirEntry(def, nameOffsets.at(i), m_options.enableClockDeltaOptimization);
        if (m_file.write(reinterpret_cast<const char*>(&entry), sizeof(entry)) != sizeof(entry)) {
            error = QStringLiteral("写 WVZ3 SignalDirEntry 失败");
            return false;
        }
    }

    const quint32 stringSize = quint32(stringTable.size());
    if (m_file.write(reinterpret_cast<const char*>(&stringSize), sizeof(stringSize)) != sizeof(stringSize) ||
        m_file.write(stringTable) != stringTable.size()) {
        error = QStringLiteral("写 WVZ3 字符串表失败");
        return false;
    }

    if (m_file.pos() != m_dataRegionOffset) {
        error = QStringLiteral("WVZ3 初始布局大小计算错误");
        return false;
    }
    if (m_options.forceDurableCommit && !flushDevice(m_file)) {
        error = QStringLiteral("WVZ3 初始布局刷盘失败");
        return false;
    }
    return true;
}

bool Wvz3StreamWriter::appendSignalDirDeltaBlock(const QVector<SignalDefinition>&sigDefs,
    qint64 activationCycle,
    QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 写信号目录增量失败：文件未打开");
        return false;
    }
    if (sigDefs.isEmpty()) return true;

    const QByteArray payload = encodeSignalDirDeltaPayload(sigDefs, m_options.enableClockDeltaOptimization);

    QString compErr;
    const QByteArray compressed = compressPayload(payload, m_options.compression, compErr);
    if (!compErr.isEmpty()) {
        error = compErr;
        return false;
    }

    const quint64 blockOffset = quint64(m_file.pos());
    wvz3::DataBlockHeader blockHeader{};
    blockHeader.blockType = wvz3::BlockType_SignalDirDelta;
    blockHeader.blockFlags = makeBlockFlags(m_options.compression, m_options.enableChecksum);
    blockHeader.blockStartTime = timeToTicks(activationCycle, m_options.timescalePs);
    blockHeader.blockEndTime = timeToTicks(activationCycle, m_options.timescalePs);
    blockHeader.blockId = m_nextBlockId;
    blockHeader.signalRecordCount = quint32(sigDefs.size());
    blockHeader.payloadUncompressedSize = quint32(payload.size());
    blockHeader.payloadCompressedSize = quint32(compressed.size());
    blockHeader.checksum = 0;

    if (m_file.write(reinterpret_cast<const char*>(&blockHeader), sizeof(blockHeader)) != sizeof(blockHeader) ||
        m_file.write(compressed) != compressed.size()) {
        error = QStringLiteral("wvz3 写 signal dir delta block 失败");
        return false;
    }

    wvz3::BlockCommitFooter footer{};
    footer.magic = wvz3::kCommitMagic;
    footer.versionMajor = wvz3::kVersionMajor;
    footer.versionMinor = wvz3::kVersionMinor;
    footer.blockId = m_nextBlockId;
    footer.blockStartTime = blockHeader.blockStartTime;
    footer.blockEndTime = blockHeader.blockEndTime;
    footer.blockFileOffset = blockOffset;
    footer.totalBlockBytes = quint32(sizeof(blockHeader) + compressed.size());
    footer.checksum = 0;

    if (m_file.write(reinterpret_cast<const char*>(&footer), sizeof(footer)) != sizeof(footer)) {
        error = QStringLiteral("wvz3 写 signal dir delta commit footer 失败");
        return false;
    }
    if (m_options.forceDurableCommit && !flushDevice(m_file)) {
        error = QStringLiteral("wvz3 signal dir delta 刷盘失败");
        return false;
    }

    ++m_nextBlockId;
    return true;
}

bool Wvz3StreamWriter::appendSignalRemoveDeltaBlock(const QVector<int>&signalIds,
    qint64 removalCycle,
    QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 写信号删除增量失败：文件未打开");
        return false;
    }
    if (signalIds.isEmpty()) return true;

    const QByteArray payload = encodeSignalRemoveDeltaPayload(signalIds);

    QString compErr;
    const QByteArray compressed = compressPayload(payload, m_options.compression, compErr);
    if (!compErr.isEmpty()) {
        error = compErr;
        return false;
    }

    const quint64 blockOffset = quint64(m_file.pos());
    wvz3::DataBlockHeader blockHeader{};
    blockHeader.blockType = wvz3::BlockType_SignalRemoveDelta;
    blockHeader.blockFlags = makeBlockFlags(m_options.compression, m_options.enableChecksum);
    blockHeader.blockStartTime = timeToTicks(removalCycle, m_options.timescalePs);
    blockHeader.blockEndTime = timeToTicks(removalCycle, m_options.timescalePs);
    blockHeader.blockId = m_nextBlockId;
    blockHeader.signalRecordCount = quint32(signalIds.size());
    blockHeader.payloadUncompressedSize = quint32(payload.size());
    blockHeader.payloadCompressedSize = quint32(compressed.size());
    blockHeader.checksum = 0;

    if (m_file.write(reinterpret_cast<const char*>(&blockHeader), sizeof(blockHeader)) != sizeof(blockHeader) ||
        m_file.write(compressed) != compressed.size()) {
        error = QStringLiteral("wvz3 写 signal remove delta block 失败");
        return false;
    }

    wvz3::BlockCommitFooter footer{};
    footer.magic = wvz3::kCommitMagic;
    footer.versionMajor = wvz3::kVersionMajor;
    footer.versionMinor = wvz3::kVersionMinor;
    footer.blockId = m_nextBlockId;
    footer.blockStartTime = blockHeader.blockStartTime;
    footer.blockEndTime = blockHeader.blockEndTime;
    footer.blockFileOffset = blockOffset;
    footer.totalBlockBytes = quint32(sizeof(blockHeader) + compressed.size());
    footer.checksum = 0;

    if (m_file.write(reinterpret_cast<const char*>(&footer), sizeof(footer)) != sizeof(footer)) {
        error = QStringLiteral("wvz3 写 signal remove delta commit footer 失败");
        return false;
    }
    if (m_options.forceDurableCommit && !flushDevice(m_file)) {
        error = QStringLiteral("wvz3 signal remove delta 刷盘失败");
        return false;
    }

    ++m_nextBlockId;
    return true;
}

bool Wvz3StreamWriter::addSignals(const QVector<SignalDefinition>&sigDefs, qint64 cycle, QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 addSignals 失败：文件未打开");
        return false;
    }
    if (m_finalized) {
        error = QStringLiteral("wvz3 addSignals 失败：writer 已关闭");
        return false;
    }
    if (sigDefs.isEmpty()) return true;
    if (cycle < 0) {
        error = QStringLiteral("wvz3 addSignals 失败：cycle 必须 >= 0");
        return false;
    }
    if (m_lastAppendedCycle != std::numeric_limits<qint64>::min() && cycle < m_lastAppendedCycle) {
        error = QStringLiteral("wvz3 addSignals 失败：要求全局时间非递减，当前=%1，上次=%2").arg(cycle).arg(m_lastAppendedCycle);
        return false;
    }

    if (!m_haveAnySample) {
        m_haveAnySample = true;
        for (int id : std::as_const(m_signalOrder)) {
            RuntimeSignalState& runtime = m_stateById[id];
            runtime.pending.clear();
            runtime.blockInitialValue = runtime.currentValue;
        }
        if (cycle > m_currentBlockStart && !m_signalOrder.isEmpty()) {
            if (!commitBlock(cycle, error)) return false;
        }
    }
    else {
        if (!flushUntil(cycle, error)) return false;
        if (cycle > m_currentBlockStart) {
            bool hasPendingContent = false;
            for (int signalId : std::as_const(m_signalOrder)) {
                if (!m_stateById.value(signalId).pending.isEmpty()) {
                    hasPendingContent = true;
                    break;
                }
            }
            if (hasPendingContent || m_nextBlockId == 0) {
                if (!commitBlock(cycle, error)) return false;
            }
            else {
                m_currentBlockStart = cycle;
                for (int signalId : std::as_const(m_signalOrder)) {
                    RuntimeSignalState& runtime = m_stateById[signalId];
                    runtime.blockInitialValue = runtime.currentValue;
                    runtime.pending.clear();
                }
            }
        }
    }

    QHash<int, bool> seenIds;
    for (int id : std::as_const(m_signalOrder)) seenIds.insert(id, true);
    int autoId = 1;
    QVector<SignalDefinition> normalized;
    normalized.reserve(sigDefs.size());
    for (const SignalDefinition& inputDef : sigDefs) {
        SignalDefinition def;
        if (!normalizeSignalDefinition(inputDef, seenIds, autoId, def, error)) {
            error.prepend(QStringLiteral("wvz3 addSignals 失败："));
            return false;
        }
        normalized.push_back(def);
    }

    if (!appendSignalDirDeltaBlock(normalized, cycle, error)) return false;

    for (const SignalDefinition& def : std::as_const(normalized)) {
        RuntimeSignalState runtime;
        runtime.def = def;
        runtime.blockInitialValue = def.initialValue;
        runtime.currentValue = def.initialValue;
        m_signalOrder.push_back(def.signalId);
        m_stateById.insert(def.signalId, runtime);
        m_knownSignalIds.insert(def.signalId, true);
    }

    m_lastAppendedCycle = cycle;
    m_lastObservedCycle = qMax(m_lastObservedCycle, cycle);
    return true;
}

bool Wvz3StreamWriter::deleteSignals(const QVector<int>&signalIds, qint64 cycle, QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 deleteSignals 失败：文件未打开");
        return false;
    }
    if (m_finalized) {
        error = QStringLiteral("wvz3 deleteSignals 失败：writer 已关闭");
        return false;
    }
    if (signalIds.isEmpty()) return true;

    QHash<int, bool> seen;
    for (int signalId : signalIds) {
        if (seen.contains(signalId)) continue;
        seen.insert(signalId, true);
        if (!appendSample(signalId, cycle, QStringLiteral("Z"), error)) {
            if (error.isEmpty()) {
                error = QStringLiteral("wvz3 deleteSignals(remove-as-Z) 失败：写入 Z 态失败，signalId=%1").arg(signalId);
            }
            return false;
        }
    }
    return true;
}


bool Wvz3StreamWriter::appendSample(int signalId, qint64 cycle, const QString & value, QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 appendSample 失败：文件未打开");
        return false;
    }
    auto it = m_stateById.find(signalId);
    if (it == m_stateById.end()) {
        error = QStringLiteral("wvz3 appendSample 失败：未知 signalId=%1").arg(signalId);
        return false;
    }
    if (cycle < 0) {
        error = QStringLiteral("wvz3 appendSample 失败：cycle 必须 >= 0");
        return false;
    }
    if (m_lastAppendedCycle != std::numeric_limits<qint64>::min() && cycle < m_lastAppendedCycle) {
        error = QStringLiteral("wvz3 appendSample 失败：要求全局时间非递减，当前=%1，上次=%2").arg(cycle).arg(m_lastAppendedCycle);
        return false;
    }

    if (!m_haveAnySample) {
        m_haveAnySample = true;
        m_currentBlockStart = alignDown(cycle, m_options.targetBlockSpan);
        for (int id : std::as_const(m_signalOrder)) {
            RuntimeSignalState& runtime = m_stateById[id];
            runtime.pending.clear();
            runtime.blockInitialValue = runtime.currentValue;
        }
    }

    if (!flushUntil(cycle, error)) return false;

    RuntimeSignalState& runtime = it.value();
    const QString normalizedValue = (runtime.def.kind == SignalKind::Bit)
        ? normalizeBitValueFast(value)
        : (isWaveZValue(value) ? QStringLiteral("Z") : normalizeNumericText(value));

    if (containsXStateText(normalizedValue)) {
        error = QStringLiteral("wvz3 appendSample 失败：当前版本不支持 x 态，signalId=%1").arg(signalId);
        return false;
    }
    if (runtime.def.kind == SignalKind::Bit) {
        if (!isValidBitValue(normalizedValue, true)) {
            error = QStringLiteral("wvz3 appendSample 失败：bit 信号值仅支持 0/1/Z，signalId=%1 value=%2").arg(signalId).arg(normalizedValue);
            return false;
        }
    }
    else {
        Q_UNUSED(normalizedValue);
    }
    if (runtime.currentValue == normalizedValue) {
        m_lastAppendedCycle = cycle;
        m_lastObservedCycle = qMax(m_lastObservedCycle, cycle);
        return true;
    }

    const qint64 rel = cycle - m_currentBlockStart;
    if (rel < 0 || rel >= m_options.targetBlockSpan) {
        error = QStringLiteral("wvz3 appendSample 失败：sample 未落入当前 block，cycle=%1 block=[%2,%3)")
            .arg(cycle)
            .arg(m_currentBlockStart)
            .arg(m_currentBlockStart + m_options.targetBlockSpan);
        return false;
    }

    if (!runtime.pending.isEmpty() && runtime.pending.last().time == rel) {
        runtime.pending.last().value = normalizedValue;
    }
    else {
        PendingChange change;
        change.time = rel;
        change.value = normalizedValue;
        runtime.pending.push_back(change);
    }
    runtime.currentValue = normalizedValue;
    m_lastAppendedCycle = cycle;
    m_lastObservedCycle = qMax(m_lastObservedCycle, cycle);
    return true;
}


bool Wvz3StreamWriter::commitBlock(qint64 blockEndExclusive, QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 commitBlock 失败：文件未打开");
        return false;
    }
    if (blockEndExclusive <= m_currentBlockStart) {
        error = QStringLiteral("wvz3 commitBlock 失败：非法 block 区间");
        return false;
    }

    WaveParser3::SaveOptions buildOptions;
    buildOptions.timescalePs = m_options.timescalePs;
    buildOptions.targetBlockSpan = m_options.targetBlockSpan;
    buildOptions.compression = m_options.compression;
    buildOptions.enableChecksum = m_options.enableChecksum;
    buildOptions.enableClockDeltaOptimization = m_options.enableClockDeltaOptimization;
    buildOptions.clockHalfPeriodTolerance = m_options.clockHalfPeriodTolerance;
    buildOptions.minClockToggleCount = m_options.minClockToggleCount;
    buildOptions.forceDurableCommit = m_options.forceDurableCommit;
    buildOptions.enableSharedTimeTable = m_options.enableSharedTimeTable;

    QVector<EncodedSignalRecord> records;
    records.reserve(m_signalOrder.size());
    QVector<qint64> candidateSharedTimes;

    for (int signalId : std::as_const(m_signalOrder)) {
        const RuntimeSignalState& runtime = m_stateById.value(signalId);
        SignalRuntimeView sig;
        sig.kind = runtime.def.kind;
        sig.width = runtime.def.width;
        sig.defaultRadix = runtime.def.defaultRadix;
        sig.blockInitialValue = runtime.blockInitialValue;
        sig.pending = runtime.pending;

        EncodedSignalRecord rec;
        rec.signalId = quint32(signalId);

        bool built = false;
        const bool forceBytesEncoding = (sig.kind != SignalKind::Bit) && signalNeedsByteEncoding(sig);
        if (m_options.enableClockDeltaOptimization &&
            sig.kind == SignalKind::Bit &&
            tryBuildClockRecord(sig, buildOptions, rec.plainEncoded)) {
            rec.plainEncoding = wvz3::Enc_ClockDeltaPattern;
            built = true;
        }
        else if (sig.kind == SignalKind::Bit) {
            built = buildBitRecord(sig, buildOptions, rec.plainEncoded);
            rec.plainEncoding = wvz3::Enc_BitTransitions;
            rec.sharedEligible = m_options.enableSharedTimeTable && !sig.pending.isEmpty();
            rec.sharedEncoding = wvz3::Enc_BitSharedTimeIndex;
        }
        else if (sig.width == 64 && !forceBytesEncoding) {
            built = buildBusU64Record(sig, buildOptions, rec.plainEncoded);
            rec.plainEncoding = wvz3::Enc_BusTransitionsU64;
            rec.sharedEligible = m_options.enableSharedTimeTable && !sig.pending.isEmpty();
            rec.sharedEncoding = wvz3::Enc_BusTransitionsU64Shared;
        }
        else {
            built = buildBusPackedBytesRecord(sig, rec.plainEncoded);
            rec.plainEncoding = wvz3::Enc_BusTransitionsPackedBytes;
            rec.sharedEligible = m_options.enableSharedTimeTable && !sig.pending.isEmpty();
            rec.sharedEncoding = wvz3::Enc_BusTransitionsPackedBytesShared;
            if (!built) {
                built = buildBusBytesRecordLegacy(sig, buildOptions, rec.plainEncoded);
                rec.plainEncoding = wvz3::Enc_BusTransitionsBytes;
                rec.sharedEncoding = wvz3::Enc_BusTransitionsBytesShared;
            }
        }

        if (!built) {
            error = QStringLiteral("wvz3 构建 signalId=%1 的 block 记录失败").arg(signalId);
            return false;
        }

        if (rec.sharedEligible) {
            rec.pendingTimes.reserve(sig.pending.size());
            for (const auto& item : std::as_const(sig.pending)) {
                rec.pendingTimes.push_back(item.time);
                candidateSharedTimes.push_back(item.time);
            }
        }

        records.push_back(rec);
    }

    QVector<qint64> finalSharedTimes;
    QByteArray sharedTimeTableBytes;
    if (m_options.enableSharedTimeTable && !candidateSharedTimes.isEmpty()) {
        QVector<qint64> draftSharedTimes = collectSortedUniqueTimes(candidateSharedTimes);
        const QHash<qint64, quint32> draftMap = buildTimeToIndex(draftSharedTimes);

        bool anySharedChosen = false;
        for (int i = 0; i < records.size(); ++i) {
            EncodedSignalRecord& rec = records[i];
            if (!rec.sharedEligible) continue;

            const RuntimeSignalState& runtime = m_stateById.value(int(rec.signalId));
            SignalRuntimeView sig;
            sig.kind = runtime.def.kind;
            sig.width = runtime.def.width;
            sig.defaultRadix = runtime.def.defaultRadix;
            sig.blockInitialValue = runtime.blockInitialValue;
            sig.pending = runtime.pending;

            bool ok = false;
            switch (rec.sharedEncoding) {
            case wvz3::Enc_BitSharedTimeIndex:
                ok = buildBitSharedIndexRecord(sig, draftMap, rec.sharedEncoded);
                break;
            case wvz3::Enc_BusTransitionsU64Shared:
                ok = buildBusU64SharedIndexRecord(sig, draftMap, rec.sharedEncoded);
                break;
            case wvz3::Enc_BusTransitionsBytesShared:
                ok = buildBusBytesSharedIndexRecordLegacy(sig, draftMap, rec.sharedEncoded);
                break;
            case wvz3::Enc_BusTransitionsPackedBytesShared:
                ok = buildBusPackedBytesSharedIndexRecord(sig, draftMap, rec.sharedEncoded);
                break;
            default:
                ok = false;
                break;
            }
            if (!ok) {
                error = QStringLiteral("wvz3 构建共享时间表记录失败，signalId=%1").arg(rec.signalId);
                return false;
            }

            if (rec.sharedEncoded.size() < rec.plainEncoded.size()) {
                rec.useShared = true;
                anySharedChosen = true;
            }
        }

        if (anySharedChosen) {
            QVector<qint64> usedTimes;
            for (const EncodedSignalRecord& rec : std::as_const(records)) {
                if (!rec.useShared) continue;
                usedTimes += rec.pendingTimes;
            }
            finalSharedTimes = collectSortedUniqueTimes(usedTimes);
            const QHash<qint64, quint32> finalMap = buildTimeToIndex(finalSharedTimes);

            for (int i = 0; i < records.size(); ++i) {
                EncodedSignalRecord& rec = records[i];
                if (!rec.useShared) continue;

                const RuntimeSignalState& runtime = m_stateById.value(int(rec.signalId));
                SignalRuntimeView sig;
                sig.kind = runtime.def.kind;
                sig.width = runtime.def.width;
                sig.defaultRadix = runtime.def.defaultRadix;
                sig.blockInitialValue = runtime.blockInitialValue;
                sig.pending = runtime.pending;

                rec.sharedEncoded.clear();
                bool ok = false;
                switch (rec.sharedEncoding) {
                case wvz3::Enc_BitSharedTimeIndex:
                    ok = buildBitSharedIndexRecord(sig, finalMap, rec.sharedEncoded);
                    break;
                case wvz3::Enc_BusTransitionsU64Shared:
                    ok = buildBusU64SharedIndexRecord(sig, finalMap, rec.sharedEncoded);
                    break;
                case wvz3::Enc_BusTransitionsBytesShared:
                    ok = buildBusBytesSharedIndexRecordLegacy(sig, finalMap, rec.sharedEncoded);
                    break;
                case wvz3::Enc_BusTransitionsPackedBytesShared:
                    ok = buildBusPackedBytesSharedIndexRecord(sig, finalMap, rec.sharedEncoded);
                    break;
                default:
                    ok = false;
                    break;
                }
                if (!ok) {
                    error = QStringLiteral("wvz3 重建共享时间表记录失败，signalId=%1").arg(rec.signalId);
                    return false;
                }
            }

            sharedTimeTableBytes = encodeSharedTimeTable(finalSharedTimes);

            quint64 plainPayloadBytes = sizeof(wvz3::BlockPayloadHeader);
            quint64 sharedPayloadBytes = sizeof(wvz3::BlockPayloadHeader) + quint64(sharedTimeTableBytes.size());
            for (const EncodedSignalRecord& rec : std::as_const(records)) {
                plainPayloadBytes += sizeof(wvz3::SignalRecordHeader) + quint64(rec.plainEncoded.size());
                sharedPayloadBytes += sizeof(wvz3::SignalRecordHeader)
                    + quint64(rec.useShared ? rec.sharedEncoded.size() : rec.plainEncoded.size());
            }

            if (sharedPayloadBytes >= plainPayloadBytes) {
                finalSharedTimes.clear();
                sharedTimeTableBytes.clear();
                for (int i = 0; i < records.size(); ++i) {
                    records[i].useShared = false;
                    records[i].sharedEncoded.clear();
                }
            }
        }
    }

    QByteArray payload;
    wvz3::BlockPayloadHeader payloadHeader{};
    payloadHeader.signalRecordCount = quint32(m_signalOrder.size());
    payloadHeader.sharedTimeCount = quint32(finalSharedTimes.size());
    appendPod(payload, payloadHeader);
    payload.append(sharedTimeTableBytes);

    for (const EncodedSignalRecord& rec : std::as_const(records)) {
        const bool chooseShared = rec.useShared && !finalSharedTimes.isEmpty();
        const quint8 encoding = chooseShared ? rec.sharedEncoding : rec.plainEncoding;
        const QByteArray& encoded = chooseShared ? rec.sharedEncoded : rec.plainEncoded;

        wvz3::SignalRecordHeader recordHeader{};
        recordHeader.signalId = rec.signalId;
        recordHeader.encoding = encoding;
        recordHeader.dataSize = quint32(encoded.size());
        appendPod(payload, recordHeader);
        payload.append(encoded);
    }

    QString compErr;
    const QByteArray compressed = compressPayload(payload, m_options.compression, compErr);
    if (!compErr.isEmpty()) {
        error = compErr;
        return false;
    }

    const quint64 blockOffset = quint64(m_file.pos());
    wvz3::DataBlockHeader blockHeader{};
    blockHeader.blockType = wvz3::BlockType_WaveData;
    blockHeader.blockFlags = makeBlockFlags(m_options.compression, m_options.enableChecksum);
    blockHeader.blockStartTime = timeToTicks(m_currentBlockStart, m_options.timescalePs);
    blockHeader.blockEndTime = timeToTicks(blockEndExclusive, m_options.timescalePs);
    blockHeader.blockId = m_nextBlockId;
    blockHeader.signalRecordCount = quint32(m_signalOrder.size());
    blockHeader.payloadUncompressedSize = quint32(payload.size());
    blockHeader.payloadCompressedSize = quint32(compressed.size());
    blockHeader.checksum = 0;

    if (m_file.write(reinterpret_cast<const char*>(&blockHeader), sizeof(blockHeader)) != sizeof(blockHeader) ||
        m_file.write(compressed) != compressed.size()) {
        error = QStringLiteral("wvz3 写 data block 失败");
        return false;
    }

    wvz3::BlockCommitFooter footer{};
    footer.magic = wvz3::kCommitMagic;
    footer.versionMajor = wvz3::kVersionMajor;
    footer.versionMinor = wvz3::kVersionMinor;
    footer.blockId = m_nextBlockId;
    footer.blockStartTime = blockHeader.blockStartTime;
    footer.blockEndTime = blockHeader.blockEndTime;
    footer.blockFileOffset = blockOffset;
    footer.totalBlockBytes = quint32(sizeof(blockHeader) + compressed.size());
    footer.checksum = 0;

    if (m_file.write(reinterpret_cast<const char*>(&footer), sizeof(footer)) != sizeof(footer)) {
        error = QStringLiteral("wvz3 写 commit footer 失败");
        return false;
    }
    if (m_options.forceDurableCommit && !flushDevice(m_file)) {
        error = QStringLiteral("wvz3 block commit 刷盘失败");
        return false;
    }

    ++m_nextBlockId;
    advanceToNextBlock(blockEndExclusive);
    return true;
}

void Wvz3StreamWriter::advanceToNextBlock(qint64 nextBlockStart) {
    m_currentBlockStart = nextBlockStart;
    for (int signalId : std::as_const(m_signalOrder)) {
        RuntimeSignalState& runtime = m_stateById[signalId];
        runtime.blockInitialValue = runtime.currentValue;
        runtime.pending.clear();
    }
}

bool Wvz3StreamWriter::flushUntil(qint64 currentCycle, QString & error) {
    error.clear();
    if (!m_file.isOpen()) {
        error = QStringLiteral("wvz3 flushUntil 失败：文件未打开");
        return false;
    }
    if (!m_haveAnySample) return true;

    while (currentCycle >= m_currentBlockStart + m_options.targetBlockSpan) {
        if (!commitBlock(m_currentBlockStart + m_options.targetBlockSpan, error)) return false;
    }
    return true;
}

bool Wvz3StreamWriter::rewriteFinalHeaderAndMeta(QString & error) {
    error.clear();
    if (!m_file.isOpen()) return true;

    const qint64 fileEnd = m_file.size();
    wvz3::FileHeader header{};
    if (!m_file.seek(0) || m_file.read(reinterpret_cast<char*>(&header), sizeof(header)) != sizeof(header)) {
        return true; // 文件主体已可读，这里仅做 best-effort
    }
    wvz3::MetaBlock meta{};
    if (!m_file.seek(m_metaOffset) || m_file.read(reinterpret_cast<char*>(&meta), sizeof(meta)) != sizeof(meta)) {
        return true;
    }

    meta.endTime = timeToTicks((m_lastObservedCycle == std::numeric_limits<qint64>::min()) ? m_startCycle : m_lastObservedCycle,
        m_options.timescalePs);
    meta.committedBlockCount = m_nextBlockId;
    meta.signalCount = quint32(m_knownSignalIds.size());

    header.dataRegionSizeHint = fileEnd - m_dataRegionOffset;
    header.fileSizeHint = fileEnd;

    if (!m_file.seek(m_metaOffset) || m_file.write(reinterpret_cast<const char*>(&meta), sizeof(meta)) != sizeof(meta)) {
        return true;
    }
    if (!m_file.seek(0) || m_file.write(reinterpret_cast<const char*>(&header), sizeof(header)) != sizeof(header)) {
        return true;
    }
    if (m_options.forceDurableCommit) flushDevice(m_file);
    return true;
}

bool Wvz3StreamWriter::close(QString & error, bool flushPartialBlock) {
    error.clear();
    if (!m_file.isOpen()) return true;
    if (m_finalized) {
        m_file.close();
        return true;
    }

    if (flushPartialBlock && m_haveAnySample) {
        const qint64 finalEndExclusive = qMax(m_currentBlockStart + 1, m_lastObservedCycle + 1);
        if (finalEndExclusive > m_currentBlockStart) {
            bool hasPendingContent = false;
            for (int signalId : std::as_const(m_signalOrder)) {
                if (!m_stateById.value(signalId).pending.isEmpty()) {
                    hasPendingContent = true;
                    break;
                }
            }
            if (hasPendingContent || m_nextBlockId == 0) {
                if (!commitBlock(finalEndExclusive, error)) {
                    m_file.close();
                    resetState();
                    return false;
                }
            }
        }
    }

    rewriteFinalHeaderAndMeta(error);
    m_finalized = true;
    m_file.close();
    return true;
}

bool WaveParser3::saveToFile(const QString & filePath,
    const WaveFile & wave,
    QString & error,
    const SaveOptions & options) {
    error.clear();

    struct SignalRun {
        int signalId = 0;
        const QVector<WaveSample>* samples = nullptr;
        int index = 0;
    };
    struct HeapNode {
        qint64 time = 0;
        int signalOrder = 0;
        int signalId = 0;
        int sampleIndex = 0;
    };
    struct HeapCmp {
        bool operator()(const HeapNode& a, const HeapNode& b) const {
            if (a.time != b.time) return a.time > b.time;
            return a.signalId > b.signalId;
        }
    };

    QVector<Wvz3StreamWriter::SignalDefinition> sigDefs;
    sigDefs.reserve(wave.signalList.size());

    QVector<QVector<WaveSample>> sortedStorage;
    sortedStorage.resize(wave.signalList.size());
    QVector<const QVector<WaveSample>*> sampleViews;
    sampleViews.resize(wave.signalList.size());

    for (int i = 0; i < wave.signalList.size(); ++i) {
        const WaveSignal& sig = wave.signalList.at(i);
        Wvz3StreamWriter::SignalDefinition def;
        def.signalId = i + 1;
        def.name = sig.name;
        def.kind = sig.kind;
        def.width = qMax(1, sig.width);
        def.defaultRadix = sig.defaultRadix;
        def.supportsZState = true;
        sigDefs.push_back(def);

        const bool alreadySorted = std::is_sorted(sig.samples.begin(), sig.samples.end(),
            [](const WaveSample& a, const WaveSample& b) { return a.time < b.time; });
        if (alreadySorted) {
            sampleViews[i] = &sig.samples;
        }
        else {
            sortedStorage[i] = sig.samples;
            std::sort(sortedStorage[i].begin(), sortedStorage[i].end(),
                [](const WaveSample& a, const WaveSample& b) { return a.time < b.time; });
            sampleViews[i] = &sortedStorage[i];
        }
    }

    Wvz3StreamWriter::Options writerOptions;
    writerOptions.timescalePs = options.timescalePs;
    writerOptions.targetBlockSpan = options.targetBlockSpan;
    writerOptions.compression = options.compression;
    writerOptions.enableChecksum = options.enableChecksum;
    writerOptions.enableClockDeltaOptimization = options.enableClockDeltaOptimization;
    writerOptions.clockHalfPeriodTolerance = options.clockHalfPeriodTolerance;
    writerOptions.minClockToggleCount = options.minClockToggleCount;
    writerOptions.forceDurableCommit = false; // offline export path: prioritize throughput
    writerOptions.enableSharedTimeTable = options.enableSharedTimeTable;

    Wvz3StreamWriter writer;
    if (!writer.open(filePath,
        sigDefs,
        error,
        wave.meta.title.isEmpty() ? QFileInfo(filePath).completeBaseName() : wave.meta.title,
        wave.meta.start,
        writerOptions)) {
        return false;
    }

    std::priority_queue<HeapNode, std::vector<HeapNode>, HeapCmp> heap;
    for (int i = 0; i < sampleViews.size(); ++i) {
        const QVector<WaveSample>* samples = sampleViews[i];
        if (!samples || samples->isEmpty()) continue;
        HeapNode node;
        node.time = samples->at(0).time;
        node.signalOrder = i;
        node.signalId = i + 1;
        node.sampleIndex = 0;
        heap.push(node);
    }

    while (!heap.empty()) {
        const HeapNode node = heap.top();
        heap.pop();

        const QVector<WaveSample>& samples = *sampleViews[node.signalOrder];
        const WaveSample& sample = samples.at(node.sampleIndex);
        const WaveSignal& sourceSignal = wave.signalList.at(node.signalOrder);
        const QString rawText = waveSampleRawText(sourceSignal, sample);
        if (!writer.appendSample(node.signalId, sample.time, rawText, error)) {
            QString ignored;
            writer.close(ignored, false);
            return false;
        }

        const int nextIndex = node.sampleIndex + 1;
        if (nextIndex < samples.size()) {
            HeapNode nextNode;
            nextNode.time = samples.at(nextIndex).time;
            nextNode.signalOrder = node.signalOrder;
            nextNode.signalId = node.signalId;
            nextNode.sampleIndex = nextIndex;
            heap.push(nextNode);
        }
    }

    return writer.close(error, true);
}

bool WaveParser3::loadFromFile(const QString & filePath,
    WaveFile & outWave,
    QString & error,
    const LoadOptions & options) {
    error.clear();
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法打开文件：%1").arg(filePath);
        return false;
    }

    wvz3::FileHeader header{};
    if (file.read(reinterpret_cast<char*>(&header), sizeof(header)) != sizeof(header)) {
        error = QStringLiteral("读取 WVZ3 文件头失败");
        return false;
    }
    if (header.magic != wvz3::kMagic) {
        error = QStringLiteral("不是 WVZ3 文件");
        return false;
    }
    if (header.versionMajor != wvz3::kVersionMajor) {
        error = QStringLiteral("不支持的 WVZ3 主版本：%1").arg(header.versionMajor);
        return false;
    }

    if (!file.seek(qint64(header.metaOffset))) {
        error = QStringLiteral("定位 WVZ3 MetaBlock 失败");
        return false;
    }
    wvz3::MetaBlock meta{};
    if (file.read(reinterpret_cast<char*>(&meta), sizeof(meta)) != sizeof(meta)) {
        error = QStringLiteral("读取 WVZ3 MetaBlock 失败");
        return false;
    }

    if (!file.seek(qint64(header.signalDirOffset))) {
        error = QStringLiteral("定位 WVZ3 signal dir 失败");
        return false;
    }
    quint32 sigCount = 0;
    if (file.read(reinterpret_cast<char*>(&sigCount), sizeof(sigCount)) != sizeof(sigCount)) {
        error = QStringLiteral("读取 WVZ3 signal count 失败");
        return false;
    }

    QVector<wvz3::SignalDirEntry> dirEntries;
    dirEntries.resize(int(sigCount));
    for (quint32 i = 0; i < sigCount; ++i) {
        if (file.read(reinterpret_cast<char*>(&dirEntries[int(i)]), sizeof(wvz3::SignalDirEntry)) != sizeof(wvz3::SignalDirEntry)) {
            error = QStringLiteral("读取 WVZ3 SignalDirEntry 失败");
            return false;
        }
    }
    quint32 stringSize = 0;
    if (file.read(reinterpret_cast<char*>(&stringSize), sizeof(stringSize)) != sizeof(stringSize)) {
        error = QStringLiteral("读取 WVZ3 字符串表大小失败");
        return false;
    }
    const QByteArray stringTable = file.read(stringSize);
    if (stringTable.size() != int(stringSize)) {
        error = QStringLiteral("读取 WVZ3 字符串表失败");
        return false;
    }

    QVector<SignalDirSlot> signalTable;
    signalTable.reserve(int(sigCount));
    quint32 maxSignalId = 0;
    bool hasSignalId = false;
    for (const auto& entry : std::as_const(dirEntries)) {
        maxSignalId = hasSignalId ? qMax(maxSignalId, entry.signalId) : entry.signalId;
        hasSignalId = true;
    }
    if (hasSignalId) resizeSignalTableForMaxId(signalTable, maxSignalId);

    for (const auto& entry : std::as_const(dirEntries)) {
        QString name;
        if (!tryStringFromTableOffsetFast(stringTable, entry.nameOffset, name)) {
            name = QStringLiteral("sig_%1").arg(entry.signalId);
        }
        if (entry.signalId < quint32(signalTable.size())) {
            SignalDirSlot& slot = signalTable[int(entry.signalId)];
            slot.valid = true;
            slot.dir = entry;
            slot.name = name;
        }
    }

    QVector<int> effectiveSignalIds = options.signalIds;
    if (effectiveSignalIds.isEmpty() && options.autoLoadFirstSignalCount >= 0) {
        const int wantedCount = options.autoLoadFirstSignalCount;
        if (wantedCount > 0) {
            effectiveSignalIds.reserve(wantedCount);
            for (int id = 0; id < signalTable.size() && effectiveSignalIds.size() < wantedCount; ++id) {
                if (signalTable[id].valid) effectiveSignalIds.push_back(id);
            }
        }
    }

    const bool selectiveSignalLoad = !effectiveSignalIds.isEmpty() || options.autoLoadFirstSignalCount >= 0;
    const SignalIdFilter signalFilter(effectiveSignalIds, !selectiveSignalLoad);
    const bool shouldDecodeWaveSamples = !selectiveSignalLoad || !effectiveSignalIds.isEmpty();

    QVector<QPair<qint64, qint64>> committedRanges;
    QVector<QVector<WaveSample>> sampleTable;
    sampleTable.resize(signalTable.size());
    const qint64 actualFileSize = file.size();
    qint64 pos = qint64(header.dataRegionOffset);
    const qint64 reqStart = options.timeStart;
    const qint64 reqEnd = options.timeEnd;

    struct PendingWaveBlock {
        int ordinal = 0;
        wvz3::DataBlockHeader header{};
        wvz3::CompressionType compression = wvz3::Comp_None;
        QByteArray compressedPayload;
    };

    struct DecodedWaveBlockResult {
        int ordinal = 0;
        QVector<QVector<WaveSample>> samples;
        QString error;
        bool ok = false;
    };

    const unsigned hardwareThreads = std::thread::hardware_concurrency();
    const int decodeThreadCount = qBound(1, int(hardwareThreads > 0 ? hardwareThreads : 2) - 1, 6);
    const int maxPendingWaveJobs = qMax(1, decodeThreadCount * 2);

    QVector<PendingWaveBlock> pendingWaveBlocks;
    pendingWaveBlocks.reserve(maxPendingWaveJobs);
    int nextWaveBlockOrdinal = 0;

    auto mergeDecodedWaveBlock = [&](DecodedWaveBlockResult& result) -> bool {
        if (!result.ok) {
            error = result.error.isEmpty()
                ? QStringLiteral("WVZ3 wave block 解码失败")
                : result.error;
            return false;
        }

        if (sampleTable.size() < result.samples.size()) {
            sampleTable.resize(result.samples.size());
        }

        for (int sid = 0; sid < result.samples.size(); ++sid) {
            QVector<WaveSample>& srcRows = result.samples[sid];
            if (srcRows.isEmpty()) continue;

            QVector<WaveSample>& dstRows = sampleTable[sid];
            reserveAdditionalSamples(dstRows, quint64(srcRows.size()));
            for (WaveSample& ws : srcRows) {
                appendCompactedSample(dstRows, std::move(ws));
            }
        }
        return true;
    };

    auto decodeOneWaveBlock = [&](const PendingWaveBlock& job) -> DecodedWaveBlockResult {
        DecodedWaveBlockResult result;
        result.ordinal = job.ordinal;
        result.samples.resize(signalTable.size());

        QString deErr;
        const QByteArray payload = decompressPayload(job.compressedPayload, job.compression, deErr);
        if (!deErr.isEmpty()) {
            result.error = deErr;
            return result;
        }

        QString localError;
        if (!decodeBlockPayload(payload,
            signalTable,
            job.header.blockStartTime,
            job.header.blockEndTime,
            meta.timescalePs,
            signalFilter,
            header.versionMinor >= 1,
            result.samples,
            localError)) {
            result.error = localError.isEmpty()
                ? QStringLiteral("WVZ3 wave block 解码失败")
                : localError;
            return result;
        }

        result.ok = true;
        return result;
    };

    auto flushPendingWaveBlocks = [&]() -> bool {
        if (pendingWaveBlocks.isEmpty()) return true;

        if (decodeThreadCount <= 1 || pendingWaveBlocks.size() <= 1) {
            for (const PendingWaveBlock& job : std::as_const(pendingWaveBlocks)) {
                DecodedWaveBlockResult result = decodeOneWaveBlock(job);
                if (!mergeDecodedWaveBlock(result)) {
                    pendingWaveBlocks.clear();
                    return false;
                }
            }
            pendingWaveBlocks.clear();
            return true;
        }

        std::vector<std::future<DecodedWaveBlockResult>> futures;
        futures.reserve(size_t(pendingWaveBlocks.size()));
        for (const PendingWaveBlock& job : std::as_const(pendingWaveBlocks)) {
            futures.push_back(std::async(std::launch::async, [&, job]() {
                return decodeOneWaveBlock(job);
            }));
        }

        for (std::future<DecodedWaveBlockResult>& future : futures) {
            DecodedWaveBlockResult result;
            try {
                result = future.get();
            }
            catch (const std::exception& ex) {
                error = QStringLiteral("WVZ3 wave block 线程异常：%1").arg(QString::fromLocal8Bit(ex.what()));
                pendingWaveBlocks.clear();
                return false;
            }
            catch (...) {
                error = QStringLiteral("WVZ3 wave block 线程发生未知异常");
                pendingWaveBlocks.clear();
                return false;
            }

            if (!mergeDecodedWaveBlock(result)) {
                pendingWaveBlocks.clear();
                return false;
            }
        }

        pendingWaveBlocks.clear();
        return true;
    };

    while (pos + qint64(sizeof(wvz3::DataBlockHeader)) <= actualFileSize) {
        if (!file.seek(pos)) break;

        wvz3::DataBlockHeader blockHeader{};
        if (file.read(reinterpret_cast<char*>(&blockHeader), sizeof(blockHeader)) != sizeof(blockHeader)) break;
        if (blockHeader.blockType != wvz3::BlockType_WaveData &&
            blockHeader.blockType != wvz3::BlockType_SignalDirDelta &&
            blockHeader.blockType != wvz3::BlockType_SignalRemoveDelta) break;

        const qint64 payloadOffset = pos + qint64(sizeof(blockHeader));
        const qint64 footerOffset = payloadOffset + qint64(blockHeader.payloadCompressedSize);
        if (footerOffset + qint64(sizeof(wvz3::BlockCommitFooter)) > actualFileSize) break;

        if (!file.seek(footerOffset)) break;
        wvz3::BlockCommitFooter footer{};
        if (file.read(reinterpret_cast<char*>(&footer), sizeof(footer)) != sizeof(footer)) break;
        if (footer.magic != wvz3::kCommitMagic) break;
        if (footer.versionMajor != wvz3::kVersionMajor) break;
        if (footer.blockFileOffset != quint64(pos)) break;
        if (footer.totalBlockBytes != quint32(sizeof(blockHeader) + blockHeader.payloadCompressedSize)) break;
        if (footer.blockStartTime != blockHeader.blockStartTime || footer.blockEndTime != blockHeader.blockEndTime) break;

        if (!file.seek(payloadOffset)) {
            error = QStringLiteral("定位 WVZ3 block payload 失败");
            return false;
        }
        const QByteArray comp = file.read(blockHeader.payloadCompressedSize);
        if (comp.size() != int(blockHeader.payloadCompressedSize)) {
            error = QStringLiteral("读取 WVZ3 block payload 失败");
            return false;
        }

        const wvz3::CompressionType compression = blockCompression(blockHeader.blockFlags);

        if (blockHeader.blockType == wvz3::BlockType_WaveData) {
            committedRanges.push_back({ blockHeader.blockStartTime, blockHeader.blockEndTime });

            const bool overlaps = !(blockHeader.blockEndTime <= reqStart || blockHeader.blockStartTime >= reqEnd);
            if (overlaps && shouldDecodeWaveSamples) {
                PendingWaveBlock job;
                job.ordinal = nextWaveBlockOrdinal++;
                job.header = blockHeader;
                job.compression = compression;
                job.compressedPayload = comp;
                pendingWaveBlocks.push_back(std::move(job));

                if (pendingWaveBlocks.size() >= maxPendingWaveJobs) {
                    if (!flushPendingWaveBlocks()) return false;
                }
            }
        }
        else {
            // Signal directory/remove deltas mutate the signal/sample tables.
            // Flush all earlier wave blocks first so workers only see a stable
            // read-only signalTable snapshot and global sample order remains
            // file/block order.
            if (!flushPendingWaveBlocks()) return false;

            QString deErr;
            const QByteArray payload = decompressPayload(comp, compression, deErr);
            if (!deErr.isEmpty()) {
                error = deErr;
                return false;
            }

            if (blockHeader.blockType == wvz3::BlockType_SignalDirDelta) {
                QVector<DecodedDeltaSignalDef> addedDefs;
                const bool okDelta = (header.versionMinor >= 3)
                    ? decodeSignalDirDeltaPayload(payload, signalTable, &addedDefs, error)
                    : decodeSignalDirDeltaPayloadLegacy(payload, signalTable, &addedDefs, error);
                if (!okDelta) {
                    return false;
                }
                for (const auto& added : std::as_const(addedDefs)) {
                    const quint32 sid = quint32(added.def.signalId);
                    if (!signalFilter.keep(sid)) continue;
                    if (blockHeader.blockStartTime > meta.startTime) {
                        appendAbsentIfNeeded(sampleTable, sid, meta.startTime);
                    }
                    appendSamplePoint(sampleTable, sid, blockHeader.blockStartTime, added.def.initialValue, added.def.kind, added.def.width);
                }
            }
            else if (blockHeader.blockType == wvz3::BlockType_SignalRemoveDelta) {
                QVector<quint32> removedIds;
                if (!decodeSignalRemoveDeltaPayload(payload, removedIds, error)) {
                    return false;
                }
                for (quint32 sid : std::as_const(removedIds)) {
                    if (!signalFilter.keep(sid)) continue;
                    appendSamplePoint(sampleTable, sid, blockHeader.blockStartTime, cachedZString());
                }
            }
        }

        pos = footerOffset + qint64(sizeof(wvz3::BlockCommitFooter));
    }

    if (!flushPendingWaveBlocks()) return false;

    if (!selectiveSignalLoad && sampleTableIsEmpty(sampleTable) && options.loadAllIfWindowEmpty && !committedRanges.isEmpty() &&
        !(options.timeStart == 0 && options.timeEnd == std::numeric_limits<qint64>::max())) {
        WaveParser3::LoadOptions retry = options;
        retry.timeStart = 0;
        retry.timeEnd = std::numeric_limits<qint64>::max();
        retry.loadAllIfWindowEmpty = false;
        return loadFromFile(filePath, outWave, error, retry);
    }

    outWave = WaveFile{};
    outWave.meta.title = QFileInfo(filePath).completeBaseName();
    outWave.meta.timescale = QStringLiteral("%1ps").arg(meta.timescalePs);

    qint64 derivedStart = meta.startTime;
    qint64 derivedEnd = meta.endTime;
    if (!committedRanges.isEmpty()) {
        for (const auto& range : std::as_const(committedRanges)) {
            derivedStart = qMin(derivedStart, range.first);
            derivedEnd = qMax(derivedEnd, range.second);
        }
    }
    outWave.meta.start = ticksToTime(derivedStart, meta.timescalePs);
    outWave.meta.end = ticksToTime(derivedEnd, meta.timescalePs);

    outWave.signalList.reserve(options.includeAllSignalDefinitions ? signalTable.size() : sampleTable.size());
    for (int id = 0; id < signalTable.size(); ++id) {
        if (!signalTable[id].valid) continue;

        const bool hasSamples = (id < sampleTable.size() && !sampleTable[id].isEmpty());
        const bool selectedForLoad = signalFilter.keep(quint32(id));

        bool shouldOutput = false;
        if (options.includeAllSignalDefinitions) {
            shouldOutput = true;
        }
        else if (selectiveSignalLoad) {
            // On-demand load: return the selected signal definitions even if a
            // selected signal is constant and has no sample record in the file.
            shouldOutput = selectedForLoad;
        }
        else {
            shouldOutput = hasSamples;
        }
        if (!shouldOutput) continue;

        const SignalDirSlot& slot = signalTable[id];
        const wvz3::SignalDirEntry& dir = slot.dir;

        WaveSignal sig;
        sig.signalId = id;
        sig.name = slot.name.isEmpty()
            ? QStringLiteral("sig_%1").arg(id)
            : slot.name;
        sig.kind = (dir.signalKind == wvz3::Sig_Bit) ? SignalKind::Bit : SignalKind::Bus;
        sig.width = qMax(1, int(dir.bitWidth));
        sig.defaultRadix = radixFromDisk(dir.defaultRadix);
        sig.currentRadix = sig.defaultRadix;
        if (id < sampleTable.size()) sig.samples = std::move(sampleTable[id]);
        sig.supportsZState = (dir.signalFlags & wvz3::Signal_SupportsZ) != 0;
        sig.samplesLoaded = selectedForLoad;
        outWave.signalList.push_back(std::move(sig));
    }

    return true;
}
