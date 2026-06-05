#include "WaveParser2.h"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <zstd.h>

namespace {

template <typename T>
void appendPod(QByteArray& buf, const T& pod) {
    const int old = buf.size();
    buf.resize(old + int(sizeof(T)));
    memcpy(buf.data() + old, &pod, sizeof(T));
}

bool shouldKeepSignal(quint32 signalId, const WaveParser2::LoadOptions& options) {
    return options.signalIds.isEmpty() || options.signalIds.contains(int(signalId));
}

QByteArray joinStrings(const QVector<QString>& strings, QVector<quint32>& offsetsUtf8) {
    QByteArray table;
    offsetsUtf8.reserve(strings.size());
    for (const QString& s : strings) {
        offsetsUtf8.push_back(quint32(table.size()));
        table.append(s.toUtf8());
        table.append('\0');
    }
    return table;
}


bool signalNeedsByteEncoding(const WaveParser2::IndexedSignal& sig) {
    if (sig.kind == SignalKind::Bit) return false;
    return sig.width > 64 || sig.supportsZState;
}

wvz2::DefaultRadixOnDisk radixToDisk(ValueRadix r) {
    switch (r) {
    case ValueRadix::Hex: return wvz2::Radix_Hex;
    case ValueRadix::Dec: return wvz2::Radix_Dec;
    case ValueRadix::Int: return wvz2::Radix_Int;
    case ValueRadix::UInt: return wvz2::Radix_UInt;
    case ValueRadix::Float: return wvz2::Radix_Float;
    case ValueRadix::Int64: return wvz2::Radix_Int64;
    case ValueRadix::UInt64: return wvz2::Radix_UInt64;
    case ValueRadix::Double: return wvz2::Radix_Double;
    case ValueRadix::Bin:
    default: return wvz2::Radix_Bin;
    }
}

ValueRadix radixFromDisk(quint8 r) {
    switch (r) {
    case wvz2::Radix_Hex: return ValueRadix::Hex;
    case wvz2::Radix_Dec: return ValueRadix::Dec;
    case wvz2::Radix_Int: return ValueRadix::Int;
    case wvz2::Radix_UInt: return ValueRadix::UInt;
    case wvz2::Radix_Float: return ValueRadix::Float;
    case wvz2::Radix_Int64: return ValueRadix::Int64;
    case wvz2::Radix_UInt64: return ValueRadix::UInt64;
    case wvz2::Radix_Double: return ValueRadix::Double;
    case wvz2::Radix_Bin:
    default: return ValueRadix::Bin;
    }
}

quint32 makeBlockFlags(wvz2::CompressionType comp, bool checksum) {
    quint32 flags = (quint32(comp) << 8);
    if (checksum) flags |= wvz2::Block_HasChecksum;
    return flags;
}

wvz2::CompressionType blockCompression(quint32 flags) {
    return wvz2::CompressionType((flags >> 8) & 0xFFu);
}

QString normalizeNumericText(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty()) return "0";
    if (s.startsWith("0x", Qt::CaseInsensitive)) return s.mid(2).toUpper();
    if (s.startsWith("0b", Qt::CaseInsensitive)) return s.mid(2);
    return s;
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

} // namespace

qint64 WaveParser2::timeToTicks(qint64 t, quint64 timescalePs) {
    Q_UNUSED(timescalePs);
    return t > 0 ? quint64(t) : 0;
}

qint64 WaveParser2::ticksToTime(qint64 ticks, quint64 timescalePs) {
    Q_UNUSED(timescalePs);
    return qint64(ticks);
}

quint8 WaveParser2::encodeBitState(const QString& s) {
    const QString t = s.trimmed().toLower();
    if (t == QStringLiteral("1")) return 1u;
    if (t == QStringLiteral("z")) return 2u;
    return 0u;
}

QString WaveParser2::decodeBitState(quint8 state) {
    switch (state & 0x3u) {
    case 1u: return QStringLiteral("1");
    case 2u: return QStringLiteral("Z");
    default: return QStringLiteral("0");
    }
}

quint64 WaveParser2::parseValueU64(const QString& raw) {
    if (isWaveZValue(raw)) return 0;
    const QString s = raw.trimmed();
    bool ok = false;
    if (s.startsWith("0x", Qt::CaseInsensitive)) return s.mid(2).toULongLong(&ok, 16);
    if (s.startsWith("0b", Qt::CaseInsensitive)) return s.mid(2).toULongLong(&ok, 2);
    if (QRegularExpression("^[01]+$").match(s).hasMatch()) return s.toULongLong(&ok, 2);
    if (QRegularExpression("^[0-9A-Fa-f]+$").match(s).hasMatch()) return s.toULongLong(&ok, 16);
    return s.toULongLong(&ok, 10);
}

QByteArray WaveParser2::encodeWideValue(const QString& raw, int width) {
    Q_UNUSED(width);
    return normalizeNumericText(raw).toUtf8();
}

QString WaveParser2::decodeWideValue(const QByteArray& raw, int width) {
    Q_UNUSED(width);
    return QString::fromUtf8(raw).trimmed();
}

QByteArray WaveParser2::encodeVarUInt(quint64 v) {
    QByteArray out;
    while (true) {
        quint8 b = quint8(v & 0x7Fu);
        v >>= 7u;
        if (v) out.append(char(b | 0x80u));
        else { out.append(char(b)); break; }
    }
    return out;
}

bool WaveParser2::decodeVarUInt(const QByteArray& data, int& pos, quint64& out) {
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

QVector<WaveParser2::IndexedSignal> WaveParser2::buildIndexedSignals(const WaveFile& wave) {
    QVector<IndexedSignal> out;
    out.reserve(wave.signalList.size());
    for (int i = 0; i < wave.signalList.size(); ++i) {
        const WaveSignal& s = wave.signalList.at(i);
        IndexedSignal x;
        x.signalIndex = i;
        x.signalId = quint32(i + 1);
        x.name = s.name;
        x.width = qMax(1, s.width);
        x.kind = s.kind;
        x.radix = s.defaultRadix;
        x.supportsZState = s.supportsZState;
        x.samples = s.samples;
        std::sort(x.samples.begin(), x.samples.end(), [](const WaveSample& a, const WaveSample& b) {
            return a.time < b.time;
        });
        out.push_back(x);
    }
    return out;
}

QVector<WaveParser2::BlockBuildResult> WaveParser2::buildBlocks(const QVector<IndexedSignal>& indexedSignals,
                                                                quint64 timescalePs,
                                                                qint64 blockSpanTicks,
                                                                const SaveOptions& options) {
    qint64 minTick = std::numeric_limits<qint64>::max();
    qint64 maxTick = 0;
    for (const IndexedSignal& s : indexedSignals) {
        for (const WaveSample& ws : s.samples) {
            const qint64 t = timeToTicks(ws.time, timescalePs);
            minTick = qMin(minTick, t);
            maxTick = qMax(maxTick, t);
        }
    }

    QVector<BlockBuildResult> blocks;
    if (indexedSignals.isEmpty() || minTick == std::numeric_limits<qint64>::max()) return blocks;
    if (blockSpanTicks == 0) blockSpanTicks = 100000;
    const qint64 alignedStart = (minTick / blockSpanTicks) * blockSpanTicks;

    for (qint64 blockStart = alignedStart; blockStart <= maxTick; blockStart += blockSpanTicks) {
        const qint64 blockEnd = blockStart + blockSpanTicks;
        BlockBuildResult br;
        br.blockStart = blockStart;
        br.blockEnd = blockEnd;

        for (const IndexedSignal& sig : indexedSignals) {
            BlockBuildSignalRecord rec;
            rec.signalId = sig.signalId;
            QByteArray encoded;
            bool built = false;

            if (options.enableClockDeltaOptimization &&
                sig.kind == SignalKind::Bit &&
                tryBuildClockRecord(sig, blockStart, blockEnd, options, encoded)) {
                rec.encoding = wvz2::Enc_ClockDeltaPattern;
                rec.encoded = encoded;
                built = true;
            } else if (sig.kind == SignalKind::Bit) {
                if (buildBitRecord(sig, blockStart, blockEnd, encoded)) {
                    rec.encoding = wvz2::Enc_BitTransitions;
                    rec.encoded = encoded;
                    built = true;
                }
            } else if (sig.width <= 64 && !signalNeedsByteEncoding(sig)) {
                if (buildBusU64Record(sig, blockStart, blockEnd, encoded)) {
                    rec.encoding = wvz2::Enc_BusTransitionsU64;
                    rec.encoded = encoded;
                    built = true;
                }
            } else {
                if (buildBusBytesRecord(sig, blockStart, blockEnd, encoded)) {
                    rec.encoding = wvz2::Enc_BusTransitionsBytes;
                    rec.encoded = encoded;
                    built = true;
                }
            }

            if (built && !rec.encoded.isEmpty()) br.records.push_back(rec);
        }

        if (!br.records.isEmpty()) blocks.push_back(br);
        if (blockEnd < blockStart) break;
    }

    return blocks;
}

bool WaveParser2::buildBitRecord(const IndexedSignal& sig,
                                 qint64 blockStart,
                                 qint64 blockEnd,
                                 QByteArray& outEncoded) {
    struct Event { qint64 time; quint8 state; bool isZ; };
    QVector<Event> ev;
    quint8 current = 0;
    bool currentIsZ = false;
    bool haveState = false;

    for (const WaveSample& ws : sig.samples) {
        const qint64 t = timeToTicks(ws.time, 1);
        const QString rawText = waveSampleRawText(reinterpret_cast<const WaveSignal&>(sig), ws);
        const bool isZ = ws.isZ || isWaveZValue(rawText);
        const quint8 v = encodeBitState(rawText);
        if (t < blockStart) {
            current = (v > 1u) ? 0u : v;
            currentIsZ = isZ;
            haveState = true;
            continue;
        }
        if (t >= blockEnd) break;
        ev.push_back({t, (v > 1u) ? 0u : v, isZ});
        haveState = true;
    }

    if (!haveState) return false;

    outEncoded.clear();
    outEncoded.append(char(current));
    outEncoded.append(char(currentIsZ ? 1 : 0));
    outEncoded.append(encodeVarUInt(quint64(ev.size())));
    qint64 prev = blockStart;
    for (const auto& item : ev) {
        outEncoded.append(encodeVarUInt(item.time - prev));
        outEncoded.append(char(item.state));
        outEncoded.append(char(item.isZ ? 1 : 0));
        prev = item.time;
    }
    return true;
}

bool WaveParser2::buildBusU64Record(const IndexedSignal& sig,
                                    qint64 blockStart,
                                    qint64 blockEnd,
                                    QByteArray& outEncoded) {
    struct Event { qint64 time; quint64 value; bool isZ; };
    QVector<Event> ev;
    quint64 current = 0;
    bool currentIsZ = false;
    bool haveState = false;

    for (const WaveSample& ws : sig.samples) {
        const qint64 t = timeToTicks(ws.time, 1);
        const QString rawText = waveSampleRawText(reinterpret_cast<const WaveSignal&>(sig), ws);
        const quint64 v = ws.rawBits & waveBitMaskForWidth(sig.width);
        const bool isZ = ws.isZ || isWaveZValue(rawText);
        if (t < blockStart) {
            current = v;
            currentIsZ = isZ;
            haveState = true;
            continue;
        }
        if (t >= blockEnd) break;
        ev.push_back({t, v, isZ});
        haveState = true;
    }

    if (!haveState) return false;

    outEncoded.clear();
    appendPod(outEncoded, current);
    outEncoded.append(char(currentIsZ ? 1 : 0));
    outEncoded.append(encodeVarUInt(quint64(ev.size())));
    qint64 prev = blockStart;
    for (const auto& item : ev) {
        outEncoded.append(encodeVarUInt(item.time - prev));
        appendPod(outEncoded, item.value);
        outEncoded.append(char(item.isZ ? 1 : 0));
        prev = item.time;
    }
    return true;
}

bool WaveParser2::buildBusBytesRecord(const IndexedSignal& sig,
                                      qint64 blockStart,
                                      qint64 blockEnd,
                                      QByteArray& outEncoded) {
    struct Event { qint64 time; QByteArray value; bool isZ; };
    QVector<Event> ev;
    QByteArray current;
    bool currentIsZ = false;
    bool haveState = false;

    for (const WaveSample& ws : sig.samples) {
        const qint64 t = timeToTicks(ws.time, 1);
        const QString rawText = waveSampleRawText(reinterpret_cast<const WaveSignal&>(sig), ws);
        const QByteArray v = encodeWideValue(rawText, sig.width);
        const bool isZ = ws.isZ || isWaveZValue(rawText);
        if (t < blockStart) {
            current = v;
            currentIsZ = isZ;
            haveState = true;
            continue;
        }
        if (t >= blockEnd) break;
        ev.push_back({t, v, isZ});
        haveState = true;
    }

    if (!haveState) return false;

    outEncoded.clear();
    outEncoded.append(encodeVarUInt(quint64(current.size())));
    outEncoded.append(current);
    outEncoded.append(char(currentIsZ ? 1 : 0));
    outEncoded.append(encodeVarUInt(quint64(ev.size())));
    qint64 prev = blockStart;
    for (const auto& item : ev) {
        outEncoded.append(encodeVarUInt(item.time - prev));
        outEncoded.append(encodeVarUInt(quint64(item.value.size())));
        outEncoded.append(item.value);
        outEncoded.append(char(item.isZ ? 1 : 0));
        prev = item.time;
    }
    return true;
}

bool WaveParser2::tryBuildClockRecord(const IndexedSignal& sig,
                                      qint64 blockStart,
                                      qint64 blockEnd,
                                      const SaveOptions& options,
                                      QByteArray& outEncoded) {
    QVector<QPair<qint64, quint8>> toggles;
    quint8 current = 0;
    bool haveStateBeforeBlock = false;

    for (const WaveSample& ws : sig.samples) {
        const qint64 t = timeToTicks(ws.time, 1);
        const quint8 v = encodeBitState(ws.value);
        if (t < blockStart) {
            current = v;
            haveStateBeforeBlock = true;
            continue;
        }
        if (t >= blockEnd) break;
        toggles.push_back({t, v});
    }

    if (!haveStateBeforeBlock || toggles.size() < options.minClockToggleCount) return false;
    if (current > 1u) return false;

    QVector<qint64> dts;
    dts.reserve(toggles.size());
    qint64 prev = blockStart;
    for (const auto& tv : toggles) {
        if (tv.second > 1u) return false;
        dts.push_back(tv.first - prev);
        prev = tv.first;
    }
    if (dts.size() < 2) return false;

    const qint64 halfPeriod = dts.at(1);
    const qint64 tol = options.clockHalfPeriodTolerance;

    for (int i = 2; i < dts.size(); ++i) {
        const qint64 a = dts.at(i);
        const qint64 diff = (a > halfPeriod) ? (a - halfPeriod) : (halfPeriod - a);
        if (diff > tol) return false;
    }
    for (int i = 1; i < toggles.size(); ++i) {
        if (toggles.at(i).second == toggles.at(i - 1).second) return false;
    }

    outEncoded.clear();
    outEncoded.append(char(current));
    outEncoded.append(encodeVarUInt(dts.at(0)));
    outEncoded.append(encodeVarUInt(halfPeriod));
    outEncoded.append(encodeVarUInt(quint64(toggles.size())));
    outEncoded.append(char(toggles.first().second));
    return true;
}

QByteArray WaveParser2::compressPayload(const QByteArray& raw,
                                        wvz2::CompressionType comp,
                                        QString& error) {
    error.clear();
    switch (comp) {
    case wvz2::Comp_None:
        return raw;
    case wvz2::Comp_Zlib:
        return qCompress(raw, 1);
    case wvz2::Comp_Zstd:
        return zstdCompressPayload(raw, 3, error);
    default:
        error = "当前版本仅实现了 none / zlib / zstd 压缩";
        return {};
    }
}

QByteArray WaveParser2::decompressPayload(const QByteArray& raw,
                                          wvz2::CompressionType comp,
                                          QString& error) {
    error.clear();
    switch (comp) {
    case wvz2::Comp_None:
        return raw;
    case wvz2::Comp_Zlib:
        return qUncompress(raw);
    case wvz2::Comp_Zstd:
        return zstdDecompressPayload(raw, error);
    default:
        error = "当前版本仅实现了 none / zlib / zstd 解压";
        return {};
    }
}

bool WaveParser2::saveToFile(const QString& filePath,
                             const WaveFile& wave,
                             QString& error,
                             const SaveOptions& options) {
    error.clear();
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        error = QString("无法写入文件：%1").arg(filePath);
        return false;
    }

    const QVector<IndexedSignal> indexed = buildIndexedSignals(wave);
    const QVector<BlockBuildResult> blocks = buildBlocks(indexed,
                                                         options.timescalePs,
                                                         options.targetBlockSpan,
                                                         options);
    return writeHeaderAndBlocks(f, wave, indexed, blocks, options, error);
}

bool WaveParser2::writeHeaderAndBlocks(QIODevice& dev,
                                       const WaveFile& wave,
                                       const QVector<IndexedSignal>& indexed,
                                       const QVector<BlockBuildResult>& blocks,
                                       const SaveOptions& options,
                                       QString& error) {
    error.clear();
    const qint64 fileStart = dev.pos();

    wvz2::FileHeader header{};
    header.magic = wvz2::kMagic;
    header.versionMajor = wvz2::kVersionMajor;
    header.versionMinor = wvz2::kVersionMinor;
    header.globalFlags = wvz2::Global_LittleEndian | (options.enableChecksum ? wvz2::Global_HasChecksum : 0u);
    header.headerSize = sizeof(wvz2::FileHeader);

    QByteArray zeroHeader(sizeof(wvz2::FileHeader), '\0');
    if (dev.write(zeroHeader) != zeroHeader.size()) {
        error = "写文件头占位失败";
        return false;
    }

    header.metaOffset = quint64(dev.pos());
    wvz2::MetaBlock meta{};
    meta.startTime = wave.meta.start > 0 ? quint64(wave.meta.start) : 0;
    meta.endTime = wave.meta.end > 0 ? quint64(wave.meta.end) : 0;
    meta.timescalePs = options.timescalePs;
    meta.signalCount = quint32(indexed.size());
    meta.scopeCount = 0;
    meta.blockCount = quint32(blocks.size());
    meta.markerCount = 0;
    if (dev.write(reinterpret_cast<const char*>(&meta), sizeof(meta)) != sizeof(meta)) {
        error = "写 MetaBlock 失败";
        return false;
    }
    header.metaSize = sizeof(meta);

    header.signalDirOffset = quint64(dev.pos());
    QVector<QString> names;
    names.reserve(indexed.size());
    for (const IndexedSignal& sig : indexed) names.push_back(sig.name);
    QVector<quint32> nameOffsets;
    const QByteArray stringTable = joinStrings(names, nameOffsets);

    const quint32 sigCount = quint32(indexed.size());
    if (dev.write(reinterpret_cast<const char*>(&sigCount), sizeof(sigCount)) != sizeof(sigCount)) {
        error = "写 signal count 失败";
        return false;
    }
    for (int i = 0; i < indexed.size(); ++i) {
        const IndexedSignal& s = indexed.at(i);
        wvz2::SignalDirEntry e{};
        e.signalId = s.signalId;
        e.scopeId = 0;
        e.nameOffset = nameOffsets.at(i);
        e.bitWidth = quint16(qMax(1, s.width));
        if (s.kind == SignalKind::Bit) {
            e.signalKind = wvz2::Sig_Bit;
            e.logicKind = wvz2::Logic_TwoState;
            e.preferredEncoding = options.enableClockDeltaOptimization ? wvz2::Enc_ClockDeltaPattern : wvz2::Enc_BitTransitions;
        } else {
            e.signalKind = wvz2::Sig_Bus;
            e.logicKind = wvz2::Logic_TwoState;
            e.preferredEncoding = (s.width <= 64) ? wvz2::Enc_BusTransitionsU64 : wvz2::Enc_BusTransitionsBytes;
        }
        e.defaultRadix = radixToDisk(s.radix);
        e.signalFlags = s.supportsZState ? wvz2::Signal_SupportsZ : 0;
        if (dev.write(reinterpret_cast<const char*>(&e), sizeof(e)) != sizeof(e)) {
            error = "写 SignalDirEntry 失败";
            return false;
        }
    }
    const quint32 stringSize = quint32(stringTable.size());
    if (dev.write(reinterpret_cast<const char*>(&stringSize), sizeof(stringSize)) != sizeof(stringSize) ||
        dev.write(stringTable) != stringTable.size()) {
        error = "写 signal 字符串表失败";
        return false;
    }
    header.signalDirSize = quint64(dev.pos()) - header.signalDirOffset;

    header.timeIndexOffset = quint64(dev.pos());
    const quint32 blockCount = quint32(blocks.size());
    if (dev.write(reinterpret_cast<const char*>(&blockCount), sizeof(blockCount)) != sizeof(blockCount)) {
        error = "写 time index count 失败";
        return false;
    }
    const qint64 timeIndexEntriesPos = dev.pos();
    QByteArray zeroIndex(int(sizeof(wvz2::TimeIndexEntry) * blocks.size()), '\0');
    if (!zeroIndex.isEmpty() && dev.write(zeroIndex) != zeroIndex.size()) {
        error = "写 time index 占位失败";
        return false;
    }
    header.timeIndexSize = sizeof(quint32) + quint64(sizeof(wvz2::TimeIndexEntry) * blocks.size());

    QVector<wvz2::TimeIndexEntry> timeEntries;
    timeEntries.reserve(blocks.size());

    for (int bi = 0; bi < blocks.size(); ++bi) {
        const BlockBuildResult& block = blocks.at(bi);

        QByteArray payload;
        wvz2::BlockPayloadHeader payloadHeader{};
        payloadHeader.signalRecordCount = quint32(block.records.size());
        appendPod(payload, payloadHeader);

        for (const BlockBuildSignalRecord& rec : block.records) {
            wvz2::SignalRecordHeader rh{};
            rh.signalId = rec.signalId;
            rh.encoding = rec.encoding;
            rh.dataSize = quint32(rec.encoded.size());
            appendPod(payload, rh);
            payload.append(rec.encoded);
        }

        QString compErr;
        const QByteArray compressed = compressPayload(payload, options.compression, compErr);
        if (!compErr.isEmpty()) {
            error = compErr;
            return false;
        }

        const quint64 blockOffset = quint64(dev.pos());
        wvz2::DataBlockHeader bh{};
        bh.blockType = 1;
        bh.blockFlags = makeBlockFlags(options.compression, options.enableChecksum);
        bh.blockStartTime = block.blockStart;
        bh.blockEndTime = block.blockEnd;
        bh.signalRecordCount = quint32(block.records.size());
        bh.payloadUncompressedSize = quint32(payload.size());
        bh.payloadCompressedSize = quint32(compressed.size());
        bh.checksum = 0;

        if (dev.write(reinterpret_cast<const char*>(&bh), sizeof(bh)) != sizeof(bh) ||
            dev.write(compressed) != compressed.size()) {
            error = "写 data block 失败";
            return false;
        }

        wvz2::TimeIndexEntry te{};
        te.startTime = block.blockStart;
        te.endTime = block.blockEnd;
        te.blockFileOffset = blockOffset;
        te.compressedSize = quint32(sizeof(bh) + compressed.size());
        te.uncompressedSize = quint32(payload.size());
        te.blockId = quint32(bi);
        te.checksum = 0;
        timeEntries.push_back(te);
    }

    const qint64 endPos = dev.pos();
    if (!dev.seek(timeIndexEntriesPos)) {
        error = "回填 time index 失败";
        return false;
    }
    for (const auto& te : timeEntries) {
        if (dev.write(reinterpret_cast<const char*>(&te), sizeof(te)) != sizeof(te)) {
            error = "回填 TimeIndexEntry 失败";
            return false;
        }
    }

    header.sessionOffset = 0;
    header.sessionSize = 0;
    header.fileSize = quint64(endPos - fileStart);
    if (!dev.seek(fileStart)) {
        error = "回填 header seek 失败";
        return false;
    }
    if (dev.write(reinterpret_cast<const char*>(&header), sizeof(header)) != sizeof(header)) {
        error = "回填 header 失败";
        return false;
    }
    dev.seek(endPos);
    return true;
}

bool WaveParser2::readHeader(QIODevice& dev,
                             wvz2::FileHeader& header,
                             QString& error) {
    error.clear();
    if (!dev.isOpen() && !dev.open(QIODevice::ReadOnly)) {
        error = "无法打开文件";
        return false;
    }
    if (dev.read(reinterpret_cast<char*>(&header), sizeof(header)) != sizeof(header)) {
        error = "读取 WVZ2 文件头失败";
        return false;
    }
    if (header.magic != wvz2::kMagic) {
        error = "不是 WVZ2 文件";
        return false;
    }
    if (header.versionMajor != wvz2::kVersionMajor) {
        error = QString("不支持的 WVZ2 主版本：%1").arg(header.versionMajor);
        return false;
    }
    return true;
}

bool WaveParser2::loadFromFile(const QString& filePath,
                               WaveFile& outWave,
                               QString& error,
                               const LoadOptions& options) {
    error.clear();
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        error = QString("无法打开文件：%1").arg(filePath);
        return false;
    }

    wvz2::FileHeader header{};
    if (!readHeader(f, header, error)) return false;

    if (!f.seek(qint64(header.metaOffset))) {
        error = "定位 MetaBlock 失败";
        return false;
    }
    wvz2::MetaBlock meta{};
    if (f.read(reinterpret_cast<char*>(&meta), sizeof(meta)) != sizeof(meta)) {
        error = "读取 MetaBlock 失败";
        return false;
    }

    if (!f.seek(qint64(header.signalDirOffset))) {
        error = "定位 signal dir 失败";
        return false;
    }
    quint32 sigCount = 0;
    if (f.read(reinterpret_cast<char*>(&sigCount), sizeof(sigCount)) != sizeof(sigCount)) {
        error = "读取 signal count 失败";
        return false;
    }

    QVector<wvz2::SignalDirEntry> dirEntries;
    dirEntries.resize(int(sigCount));
    for (quint32 i = 0; i < sigCount; ++i) {
        if (f.read(reinterpret_cast<char*>(&dirEntries[int(i)]), sizeof(wvz2::SignalDirEntry)) != sizeof(wvz2::SignalDirEntry)) {
            error = "读取 SignalDirEntry 失败";
            return false;
        }
    }
    quint32 stringSize = 0;
    if (f.read(reinterpret_cast<char*>(&stringSize), sizeof(stringSize)) != sizeof(stringSize)) {
        error = "读取字符串表大小失败";
        return false;
    }
    const QByteArray stringTable = f.read(stringSize);
    if (stringTable.size() != int(stringSize)) {
        error = "读取字符串表失败";
        return false;
    }

    QHash<quint32, wvz2::SignalDirEntry> sigDir;
    QHash<quint32, QString> sigNames;
    for (const auto& e : dirEntries) {
        sigDir.insert(e.signalId, e);
        if (e.nameOffset < quint32(stringTable.size())) {
            sigNames.insert(e.signalId, QString::fromUtf8(stringTable.constData() + e.nameOffset));
        } else {
            sigNames.insert(e.signalId, QString("sig_%1").arg(e.signalId));
        }
    }

    if (!f.seek(qint64(header.timeIndexOffset))) {
        error = "定位 time index 失败";
        return false;
    }
    quint32 blockCount = 0;
    if (f.read(reinterpret_cast<char*>(&blockCount), sizeof(blockCount)) != sizeof(blockCount)) {
        error = "读取 block count 失败";
        return false;
    }

    QVector<wvz2::TimeIndexEntry> blocks;
    blocks.resize(int(blockCount));
    for (quint32 i = 0; i < blockCount; ++i) {
        if (f.read(reinterpret_cast<char*>(&blocks[int(i)]), sizeof(wvz2::TimeIndexEntry)) != sizeof(wvz2::TimeIndexEntry)) {
            error = "读取 TimeIndexEntry 失败";
            return false;
        }
    }

    const quint64 reqStart = options.timeStart;
    const quint64 reqEnd = options.timeEnd;
    QHash<quint32, QVector<WaveSample>> sampleMap;

    for (const auto& bi : blocks) {
        if (bi.endTime <= reqStart || bi.startTime >= reqEnd) continue;
        if (!f.seek(qint64(bi.blockFileOffset))) {
            error = "定位 data block 失败";
            return false;
        }

        wvz2::DataBlockHeader bh{};
        if (f.read(reinterpret_cast<char*>(&bh), sizeof(bh)) != sizeof(bh)) {
            error = "读取 DataBlockHeader 失败";
            return false;
        }
        const QByteArray comp = f.read(bh.payloadCompressedSize);
        if (comp.size() != int(bh.payloadCompressedSize)) {
            error = "读取 block payload 失败";
            return false;
        }

        QString deErr;
        const QByteArray payload = decompressPayload(comp, blockCompression(bh.blockFlags), deErr);
        if (!deErr.isEmpty()) {
            error = deErr;
            return false;
        }

        if (!decodeBlockPayload(payload, sigDir, bh.blockStartTime, bh.blockEndTime, meta.timescalePs, options, sampleMap, error)) {
            return false;
        }
    }

    outWave = WaveFile{};
    outWave.meta.title = QFileInfo(filePath).completeBaseName();
    outWave.meta.timescale = QString("%1ps").arg(meta.timescalePs);
    outWave.meta.start = ticksToTime(meta.startTime, meta.timescalePs);
    outWave.meta.end = ticksToTime(meta.endTime, meta.timescalePs);

    QVector<quint32> ids;
    ids.reserve(dirEntries.size());
    for (const auto& entry : dirEntries) {
        const quint32 id = entry.signalId;
        if (sampleMap.contains(id)) ids.push_back(id);
    }
    for (quint32 id : ids) {
        const auto dir = sigDir.value(id);
        WaveSignal s;
        s.name = sigNames.value(id, QString("sig_%1").arg(id));
        s.kind = (dir.signalKind == wvz2::Sig_Bit) ? SignalKind::Bit : SignalKind::Bus;
        s.width = qMax(1, int(dir.bitWidth));
        s.defaultRadix = radixFromDisk(dir.defaultRadix);
        s.currentRadix = s.defaultRadix;
        s.samples = sampleMap.value(id);
        for (WaveSample& ws : s.samples) hydrateWaveSampleRawFields(s.kind, s.width, ws);
        s.supportsZState = (dir.signalFlags & wvz2::Signal_SupportsZ) != 0;
        std::sort(s.samples.begin(), s.samples.end(), [](const WaveSample& a, const WaveSample& b) {
            return a.time < b.time;
        });
        outWave.signalList.push_back(s);
    }

    return true;
}

bool WaveParser2::decodeBlockPayload(const QByteArray& payload,
                                     const QHash<quint32, wvz2::SignalDirEntry>& sigDir,
                                     qint64 blockStart,
                                     qint64 blockEnd,
                                     quint64 timescalePs,
                                     const LoadOptions& options,
                                     QHash<quint32, QVector<WaveSample>>& outSamples,
                                     QString& error) {
    error.clear();
    int pos = 0;
    if (payload.size() < int(sizeof(wvz2::BlockPayloadHeader))) {
        error = "payload 太小";
        return false;
    }

    wvz2::BlockPayloadHeader ph{};
    memcpy(&ph, payload.constData(), sizeof(ph));
    pos += int(sizeof(ph));

    for (quint32 i = 0; i < ph.signalRecordCount; ++i) {
        if (pos + int(sizeof(wvz2::SignalRecordHeader)) > payload.size()) {
            error = "signal record header 越界";
            return false;
        }

        wvz2::SignalRecordHeader rh{};
        memcpy(&rh, payload.constData() + pos, sizeof(rh));
        pos += int(sizeof(rh));

        if (pos + int(rh.dataSize) > payload.size()) {
            error = "signal record data 越界";
            return false;
        }

        const QByteArray encoded = payload.mid(pos, rh.dataSize);
        pos += int(rh.dataSize);

        if (!shouldKeepSignal(rh.signalId, options)) continue;

        QVector<WaveSample>& dst = outSamples[rh.signalId];
        const auto dir = sigDir.value(rh.signalId);

        bool ok = false;
        switch (rh.encoding) {
        case wvz2::Enc_BitTransitions:
            ok = decodeBitRecord(encoded, blockStart, blockEnd, timescalePs, (dir.signalFlags & wvz2::Signal_SupportsZ) != 0, dst);
            break;
        case wvz2::Enc_BusTransitionsU64:
            ok = decodeBusU64Record(encoded, blockStart, blockEnd, timescalePs, dir.bitWidth, (dir.signalFlags & wvz2::Signal_SupportsZ) != 0, dst);
            break;
        case wvz2::Enc_BusTransitionsBytes:
            ok = decodeBusBytesRecord(encoded, blockStart, blockEnd, timescalePs, dir.bitWidth, (dir.signalFlags & wvz2::Signal_SupportsZ) != 0, dst);
            break;
        case wvz2::Enc_ClockDeltaPattern:
            ok = decodeClockRecord(encoded, blockStart, blockEnd, timescalePs, (dir.signalFlags & wvz2::Signal_SupportsZ) != 0, dst);
            break;
        default:
            error = QString("不支持的记录编码：%1").arg(rh.encoding);
            return false;
        }
        if (!ok) {
            error = QString("解码 signalId=%1 失败").arg(rh.signalId);
            return false;
        }
    }
    return true;
}

bool WaveParser2::decodeBitRecord(const QByteArray& encoded,
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

    WaveSample ws0;
    ws0.time = ticksToTime(blockStart, timescalePs);
    ws0.value = initialIsZ ? QStringLiteral("Z") : decodeBitState(initial);
    outSamples.push_back(ws0);

    qint64 t = blockStart;
    for (quint64 i = 0; i < eventCount; ++i) {
        quint64 dt = 0;
        if (!decodeVarUInt(encoded, pos, dt) || pos >= encoded.size()) return false;
        t += dt;
        const quint8 s = quint8(encoded.at(pos++));
        bool isZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            isZ = encoded.at(pos++) != 0;
        } else {
            isZ = (s & 0x3u) == 2u;
        }
        WaveSample ws;
        ws.time = ticksToTime(t, timescalePs);
        ws.value = isZ ? QStringLiteral("Z") : decodeBitState(s);
        outSamples.push_back(ws);
    }
    return true;
}

bool WaveParser2::decodeBusU64Record(const QByteArray& encoded,
                                  qint64 blockStart,
                                  qint64 blockEnd,
                                   quint64 timescalePs,
                                   int bitWidth,
                                   bool supportsZ,
                                   QVector<WaveSample>& outSamples) {
    Q_UNUSED(blockEnd);
    Q_UNUSED(bitWidth);
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

    WaveSample ws0;
    ws0.time = ticksToTime(blockStart, timescalePs);
    ws0.value = initialIsZ ? QStringLiteral("Z") : QString::number(initial);
    outSamples.push_back(ws0);

    qint64 t = blockStart;
    for (quint64 i = 0; i < eventCount; ++i) {
        quint64 dt = 0;
        if (!decodeVarUInt(encoded, pos, dt) || pos + int(sizeof(quint64)) > encoded.size()) return false;
        t += dt;
        quint64 v = 0;
        memcpy(&v, encoded.constData() + pos, sizeof(v));
        pos += int(sizeof(v));
        bool isZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            isZ = encoded.at(pos++) != 0;
        }
        WaveSample ws;
        ws.time = ticksToTime(t, timescalePs);
        ws.value = isZ ? QStringLiteral("Z") : QString::number(v);
        outSamples.push_back(ws);
    }
    return true;
}

bool WaveParser2::decodeBusBytesRecord(const QByteArray& encoded,
                                  qint64 blockStart,
                                  qint64 blockEnd,
                                       quint64 timescalePs,
                                       int bitWidth,
                                       bool supportsZ,
                                       QVector<WaveSample>& outSamples) {
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

    WaveSample ws0;
    ws0.time = ticksToTime(blockStart, timescalePs);
    ws0.value = initialIsZ ? QStringLiteral("Z") : decodeWideValue(initial, bitWidth);
    outSamples.push_back(ws0);

    qint64 t = blockStart;
    for (quint64 i = 0; i < eventCount; ++i) {
        quint64 dt = 0, valueLen = 0;
        if (!decodeVarUInt(encoded, pos, dt)) return false;
        if (!decodeVarUInt(encoded, pos, valueLen)) return false;
        if (pos + int(valueLen) > encoded.size()) return false;
        t += dt;
        const QByteArray raw = encoded.mid(pos, int(valueLen));
        pos += int(valueLen);
        bool isZ = false;
        if (supportsZ) {
            if (pos >= encoded.size()) return false;
            isZ = encoded.at(pos++) != 0;
        }

        WaveSample ws;
        ws.time = ticksToTime(t, timescalePs);
        ws.value = isZ ? QStringLiteral("Z") : decodeWideValue(raw, bitWidth);
        outSamples.push_back(ws);
    }
    return true;
}

bool WaveParser2::decodeClockRecord(const QByteArray& encoded,
                                  qint64 blockStart,
                                  qint64 blockEnd,
                                    quint64 timescalePs,
                                    bool supportsZ,
                                    QVector<WaveSample>& outSamples) {
    Q_UNUSED(supportsZ);
    if (encoded.isEmpty()) return false;

    int pos = 0;
    quint8 current = quint8(encoded.at(pos++));
    quint64 dt0 = 0, halfPeriod = 0, toggleCount = 0;
    if (!decodeVarUInt(encoded, pos, dt0)) return false;
    if (!decodeVarUInt(encoded, pos, halfPeriod)) return false;
    if (!decodeVarUInt(encoded, pos, toggleCount)) return false;
    if (pos >= encoded.size()) return false;

    WaveSample ws0;
    ws0.time = ticksToTime(blockStart, timescalePs);
    ws0.value = decodeBitState(current);
    outSamples.push_back(ws0);

    qint64 t = blockStart + dt0;
    current = quint8(encoded.at(pos++));
    if (t < blockEnd) {
        WaveSample ws;
        ws.time = ticksToTime(t, timescalePs);
        ws.value = decodeBitState(current);
        outSamples.push_back(ws);
    }

    for (quint64 i = 1; i < toggleCount; ++i) {
        t += halfPeriod;
        current = (current == 0) ? 1 : 0;
        if (t >= blockEnd) break;
        WaveSample ws;
        ws.time = ticksToTime(t, timescalePs);
        ws.value = decodeBitState(current);
        outSamples.push_back(ws);
    }
    return true;
}
