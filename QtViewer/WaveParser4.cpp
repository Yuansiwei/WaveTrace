#include "WaveParser4.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QtGlobal>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <zstd.h>

namespace {

using u8 = quint8;
using u32 = quint32;
using u64 = quint64;
using i64 = qint64;

enum class Compression : u8 {
    None = 0,
    Zstd = 1
};

enum class ValueType : u8 {
    Bool = 1,
    I8   = 2,
    U8   = 3,
    I16  = 4,
    U16  = 5,
    I32  = 6,
    U32  = 7,
    I64  = 8,
    U64  = 9,
    F32  = 10,
    F64  = 11
};

enum class Radix : u8 {
    Bin   = 2,
    Dec   = 10,
    Hex   = 16,
    Float = 32,
    Auto  = 255
};

static const u32 kSupportedVersionV1 = 1;
static const u32 kSupportedVersionV2 = 2;
static const u32 kSupportedVersionV3 = 3;
static const u32 kSupportedVersionV4 = 4;
static const u32 kSupportedVersionV5 = 5;
static const u32 kSupportedVersionV6 = 6;
static const u32 kSupportedVersionV7 = 7;
static const u32 kSupportedVersionV8 = 8;
static const u32 kSupportedVersionV9 = 9;

// WVZ4 v2 raw WDAT payload flags. These match wvz4_writer_typed.h.
static const u64 kWdatDeltaTimes      = 1ull << 0;
static const u64 kWdatFixedValueWidth = 1ull << 1;
static const u64 kWdatSharedTimeTable = 1ull << 2;
static const u64 kWdatSignalChunkTile = 1ull << 3;
static const u64 kWdatSparseSignalRecords = 1ull << 4;
static const u64 kWdatPerRecordValueCodec = 1ull << 5;
static const u64 kKnownWdatV2Flags = kWdatDeltaTimes | kWdatFixedValueWidth | kWdatSharedTimeTable;
static const u64 kKnownWdatV3Flags = kKnownWdatV2Flags | kWdatSignalChunkTile;
static const u64 kKnownWdatV4Flags = kKnownWdatV3Flags | kWdatSparseSignalRecords | kWdatPerRecordValueCodec;

enum class ValueRecordCodec : u8 {
    FullValues = 0,
    BoolToggle = 1,
    ByteMask = 2,
    FullValuesStride = 3,
    BoolToggleStride = 4,
    ByteMaskStride = 5,
    NibbleMask = 6,
    NibbleMaskStride = 7
};

struct NameRec {
    u32 id = 0;
    QString text;
};

struct NodeRec {
    u32 id = 0;
    u32 parent = 0;
    u32 nameId = 0;
    u8 kind = 0;
    u32 firstChild = 0;
    u32 nextSibling = 0;
    bool valid = false;
};

struct SigRec {
    u32 signalId = 0;
    u32 storageId = 0;
    u32 nodeId = 0;
    ValueType type = ValueType::U64;
    u32 bitWidth = 64;
    Radix radix = Radix::Auto;
};

struct ClockRec {
    u32 signalId = 0;
    bool initialValue = false;
    u64 periodTicks = 1;
};

struct BlockIndexRec {
    u64 blockId = 0;
    i64 start = 0;
    i64 end = 0;
    // WVZ4 v3 WDAT tiles are additionally indexed by signal chunk.
    u64 signalChunkId = 0;
    u64 firstSignalId = 1;
    u64 signalCount = 0;
    u64 fileOffset = 0;
    u64 fileSize = 0;
    u64 rawSize = 0;
    Compression compression = Compression::None;
};

struct SectionHeader {
    QByteArray tag;
    u64 size = 0;
    qint64 payloadOffset = 0;
};

inline u32 readU32LE(const char* p) {
    const uchar* b = reinterpret_cast<const uchar*>(p);
    return u32(b[0]) | (u32(b[1]) << 8) | (u32(b[2]) << 16) | (u32(b[3]) << 24);
}

inline u64 readU64LE(const char* p) {
    const uchar* b = reinterpret_cast<const uchar*>(p);
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= (u64(b[i]) << (8 * i));
    return v;
}

inline i64 readI64LE(const char* p) {
    return static_cast<i64>(readU64LE(p));
}

bool isValidCompression(Compression c) {
    return c == Compression::None || c == Compression::Zstd;
}

bool isValidValueType(ValueType t) {
    switch (t) {
    case ValueType::Bool:
    case ValueType::I8:
    case ValueType::U8:
    case ValueType::I16:
    case ValueType::U16:
    case ValueType::I32:
    case ValueType::U32:
    case ValueType::I64:
    case ValueType::U64:
    case ValueType::F32:
    case ValueType::F64:
        return true;
    default:
        return false;
    }
}

bool valueTypeByteWidth(ValueType t, int& bytes) {
    switch (t) {
    case ValueType::Bool:
    case ValueType::I8:
    case ValueType::U8:
        bytes = 1; return true;
    case ValueType::I16:
    case ValueType::U16:
        bytes = 2; return true;
    case ValueType::I32:
    case ValueType::U32:
    case ValueType::F32:
        bytes = 4; return true;
    case ValueType::I64:
    case ValueType::U64:
    case ValueType::F64:
        bytes = 8; return true;
    default:
        bytes = 0; return false;
    }
}

bool isValidRadix(Radix r) {
    switch (r) {
    case Radix::Bin:
    case Radix::Dec:
    case Radix::Hex:
    case Radix::Float:
    case Radix::Auto:
        return true;
    default:
        return false;
    }
}

bool isValidNodeKind(u8 kind) {
    return kind >= 1 && kind <= 6;
}

static const u8 kNodeKindSignalLeaf = 6;

bool validBlockTimeRange(i64 start, i64 end) {
    return start >= 0 && end > start;
}

bool addRelTimeChecked(i64 blockStart, i64 blockEnd, u64 rel, qint64& sampleTime) {
    if (!validBlockTimeRange(blockStart, blockEnd)) return false;
    const u64 span = static_cast<u64>(blockEnd - blockStart);
    if (rel >= span) return false;
    if (rel > u64(std::numeric_limits<qint64>::max() - blockStart)) return false;
    sampleTime = blockStart + qint64(rel);
    return true;
}

bool blockOverlapsWindow(i64 start, i64 end, qint64 windowStart, qint64 windowEnd) {
    if (windowEnd < windowStart) return true;
    return end > windowStart && start <= windowEnd;
}

bool sampleInWindow(qint64 t, qint64 windowStart, qint64 windowEnd) {
    if (windowEnd < windowStart) return true;
    return t >= windowStart && t <= windowEnd;
}

inline int directIntMapValue(const QVector<int>& map, int key, int fallback = -1) {
    return (key >= 0 && key < map.size()) ? map.at(key) : fallback;
}

inline void directIntMapSet(QVector<int>& map, int key, int value) {
    if (key < 0) return;
    const int oldSize = map.size();
    if (oldSize <= key) {
        map.resize(key + 1);
        for (int i = oldSize; i < map.size(); ++i) map[i] = -1;
    }
    map[key] = value;
}

inline void directIntListMapAppend(QVector<QVector<int>>& map, int key, int value) {
    if (key < 0 || value < 0) return;
    if (map.size() <= key) map.resize(key + 1);
    map[key].push_back(value);
}

inline const QVector<int>* directIntListMapValue(const QVector<QVector<int>>& map, int key) {
    if (key < 0 || key >= map.size()) return nullptr;
    return &map[key];
}

class SpanReader {
public:
    SpanReader() = default;
    explicit SpanReader(const QByteArray& bytes)
        : m_data(bytes.constData()), m_size(bytes.size()) {}
    SpanReader(const char* data, int size)
        : m_data(data), m_size(size) {}

    int pos() const { return m_pos; }
    int remaining() const { return m_size - m_pos; }
    bool eof() const { return m_pos >= m_size; }

    bool readU8(u8& out) {
        if (remaining() < 1) return false;
        out = static_cast<u8>(m_data[m_pos++]);
        return true;
    }

    bool readI64(i64& out) {
        if (remaining() < 8) return false;
        out = readI64LE(m_data + m_pos);
        m_pos += 8;
        return true;
    }

    bool readBytes(const char*& ptr, int n) {
        if (n < 0 || remaining() < n) return false;
        ptr = m_data + m_pos;
        m_pos += n;
        return true;
    }

    bool readVarUInt(u64& out) {
        out = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            if (remaining() < 1) return false;
            const u8 byte = static_cast<u8>(m_data[m_pos++]);
            if (i == 9) {
                // The 10th byte may only contribute bit 63 and must terminate.
                if ((byte & 0x80u) != 0 || (byte & 0x7eu) != 0) return false;
                out |= (u64(byte & 0x01u) << 63);
                return true;
            }
            out |= (u64(byte & 0x7fu) << shift);
            if ((byte & 0x80u) == 0) return true;
            shift += 7;
        }
        return false;
    }

private:
    const char* m_data = nullptr;
    int m_size = 0;
    int m_pos = 0;
};

bool readSectionHeader(QFile& file, SectionHeader& out, QString& error) {
    QByteArray header = file.read(12);
    if (header.isEmpty() && file.atEnd()) return false;
    if (header.size() != 12) {
        error = QStringLiteral("WVZ4 section header truncated at offset %1").arg(file.pos() - header.size());
        return false;
    }

    out.tag = header.left(4);
    out.size = readU64LE(header.constData() + 4);
    out.payloadOffset = file.pos();

    const qint64 remain = file.size() - file.pos();
    if (out.size > u64(std::numeric_limits<qint64>::max()) || qint64(out.size) > remain) {
        error = QStringLiteral("WVZ4 section '%1' size exceeds file remainder").arg(QString::fromLatin1(out.tag));
        return false;
    }
    return true;
}

bool skipSectionPayload(QFile& file, const SectionHeader& h, QString& error) {
    const qint64 target = h.payloadOffset + qint64(h.size);
    if (target < h.payloadOffset || target > file.size()) {
        error = QStringLiteral("WVZ4 section skip overflow for '%1'").arg(QString::fromLatin1(h.tag));
        return false;
    }
    if (!file.seek(target)) {
        error = QStringLiteral("WVZ4 failed to seek over section '%1'").arg(QString::fromLatin1(h.tag));
        return false;
    }
    return true;
}

bool readSectionPayload(QFile& file, const SectionHeader& h, QByteArray& payload, QString& error) {
    if (h.size > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 section '%1' too large for QByteArray").arg(QString::fromLatin1(h.tag));
        return false;
    }
    payload = file.read(qint64(h.size));
    if (payload.size() != int(h.size)) {
        error = QStringLiteral("WVZ4 section '%1' payload truncated").arg(QString::fromLatin1(h.tag));
        return false;
    }
    return true;
}

bool parseNameSection(const QByteArray& payload,
                      QVector<QString>& namesById,
                      QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count)) {
        error = QStringLiteral("WVZ4 NAME: missing count");
        return false;
    }
    if (count > 100000000ull) {
        error = QStringLiteral("WVZ4 NAME: unreasonable name count");
        return false;
    }

    for (u64 i = 0; i < count; ++i) {
        u64 id64 = 0;
        u64 len64 = 0;
        if (!r.readVarUInt(id64) || id64 == 0 || id64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 NAME: invalid name_id");
            return false;
        }
        if (!r.readVarUInt(len64) || len64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 NAME: invalid string length");
            return false;
        }
        const char* p = nullptr;
        if (!r.readBytes(p, int(len64))) {
            error = QStringLiteral("WVZ4 NAME: string payload truncated");
            return false;
        }

        const int id = int(id64);
        if (namesById.size() <= id) namesById.resize(id + 1);
        if (!namesById[id].isEmpty()) {
            error = QStringLiteral("WVZ4 NAME: duplicate name_id %1").arg(id);
            return false;
        }
        namesById[id] = QString::fromUtf8(p, int(len64));
        if (namesById[id].isEmpty()) {
            error = QStringLiteral("WVZ4 NAME: empty name for name_id %1").arg(id);
            return false;
        }
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 NAME: trailing bytes after name table");
        return false;
    }
    return true;
}

bool parseNodeSection(const QByteArray& payload,
                      QVector<NodeRec>& nodesById,
                      QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count)) {
        error = QStringLiteral("WVZ4 NODE: missing count");
        return false;
    }
    if (count > 100000000ull) {
        error = QStringLiteral("WVZ4 NODE: unreasonable node count");
        return false;
    }

    for (u64 i = 0; i < count; ++i) {
        u64 id64 = 0, parent64 = 0, name64 = 0, first64 = 0, next64 = 0;
        u8 kind = 0;
        if (!r.readVarUInt(id64) || id64 == 0 || id64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(parent64) || parent64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(name64) || name64 == 0 || name64 > u64(std::numeric_limits<int>::max()) ||
            !r.readU8(kind) ||
            !r.readVarUInt(first64) || first64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(next64) || next64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 NODE: malformed node record");
            return false;
        }

        const int id = int(id64);
        if (nodesById.size() <= id) nodesById.resize(id + 1);
        if (nodesById[id].valid) {
            error = QStringLiteral("WVZ4 NODE: duplicate node_id %1").arg(id);
            return false;
        }

        NodeRec n;
        n.id = u32(id64);
        n.parent = u32(parent64);
        n.nameId = u32(name64);
        n.kind = kind;
        n.firstChild = u32(first64);
        n.nextSibling = u32(next64);
        n.valid = true;
        nodesById[id] = n;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 NODE: trailing bytes after node table");
        return false;
    }
    return true;
}

bool parseSignalSection(const QByteArray& payload,
                        u32 formatVersion,
                        QVector<SigRec>& sigs,
                        QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count)) {
        error = QStringLiteral("WVZ4 SIGT: missing count");
        return false;
    }
    if (count > 100000000ull) {
        error = QStringLiteral("WVZ4 SIGT: unreasonable signal count");
        return false;
    }

    sigs.clear();
    sigs.reserve(int(qMin<u64>(count, u64(std::numeric_limits<int>::max()))));

    // signal_id is generated densely by the WVZ4 writer.  Do not use QSet here:
    // this function is executed on every on-demand signal load, and the hash table
    // showed up as a visible hot path for large SignalTable sections.
    QVector<uchar> seenSignalIds;
    QSet<int> sparseSeenSignalIds;
    for (u64 i = 0; i < count; ++i) {
        u64 sid64 = 0, storage64 = 0, node64 = 0, width64 = 0;
        u8 type = 0, radix = 0;
        if (!r.readVarUInt(sid64) || sid64 == 0 || sid64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 SIGT: malformed signal record");
            return false;
        }
        if (formatVersion >= kSupportedVersionV5) {
            if (!r.readVarUInt(storage64) || storage64 == 0 || storage64 > u64(std::numeric_limits<int>::max())) {
                error = QStringLiteral("WVZ4 SIGT: malformed storage_id for signal_id %1").arg(int(sid64));
                return false;
            }
        } else {
            storage64 = sid64;
        }
        if (!r.readVarUInt(node64) || node64 == 0 || node64 > u64(std::numeric_limits<int>::max()) ||
            !r.readU8(type) ||
            !r.readVarUInt(width64) || width64 == 0 || width64 > 64 ||
            !r.readU8(radix)) {
            error = QStringLiteral("WVZ4 SIGT: malformed signal record");
            return false;
        }

        const int sid = int(sid64);
        if (sid <= 100000000) {
            if (seenSignalIds.size() <= sid) seenSignalIds.resize(sid + 1);
            if (seenSignalIds.at(sid)) {
                error = QStringLiteral("WVZ4 SIGT: duplicate signal_id %1").arg(sid);
                return false;
            }
            seenSignalIds[sid] = 1;
        } else {
            // Pathological sparse signal_id values are valid u32 values, but do not
            // allocate a giant direct table for them.
            if (sparseSeenSignalIds.contains(sid)) {
                error = QStringLiteral("WVZ4 SIGT: duplicate signal_id %1").arg(sid);
                return false;
            }
            sparseSeenSignalIds.insert(sid);
        }

        int typeBytes = 0;
        if (!valueTypeByteWidth(ValueType(type), typeBytes)) {
            error = QStringLiteral("WVZ4 SIGT: invalid ValueType for signal_id %1").arg(int(sid64));
            return false;
        }
        if (!isValidRadix(Radix(radix))) {
            error = QStringLiteral("WVZ4 SIGT: invalid Radix for signal_id %1").arg(int(sid64));
            return false;
        }
        if (width64 > u64(typeBytes * 8)) {
            error = QStringLiteral("WVZ4 SIGT: bit_width exceeds ValueType capacity for signal_id %1").arg(int(sid64));
            return false;
        }
        SigRec s;
        s.signalId = u32(sid64);
        s.storageId = u32(storage64);
        s.nodeId = u32(node64);
        s.type = ValueType(type);
        s.bitWidth = u32(width64);
        s.radix = Radix(radix);
        sigs.push_back(s);
    }

    // Writer already serializes SIGT in signal_id order.  Sorting again is not
    // required for decoding and hurts on-demand load latency on very large files.
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 SIGT: trailing bytes after signal table");
        return false;
    }
    return true;
}

bool parseClockSection(const QByteArray& payload,
                       QVector<ClockRec>& clocks,
                       QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count)) {
        error = QStringLiteral("WVZ4 CLKD: missing count");
        return false;
    }
    if (count > 100000000ull) {
        error = QStringLiteral("WVZ4 CLKD: unreasonable clock count");
        return false;
    }

    clocks.clear();
    clocks.reserve(int(qMin<u64>(count, u64(std::numeric_limits<int>::max()))));
    for (u64 i = 0; i < count; ++i) {
        u64 sid64 = 0, period64 = 0;
        u8 initial = 0;
        if (!r.readVarUInt(sid64) || sid64 == 0 || sid64 > u64(std::numeric_limits<int>::max()) ||
            !r.readU8(initial) || initial > 1 ||
            !r.readVarUInt(period64) || period64 == 0) {
            error = QStringLiteral("WVZ4 CLKD: malformed clock record");
            return false;
        }
        ClockRec c;
        c.signalId = u32(sid64);
        c.initialValue = initial != 0;
        c.periodTicks = period64;
        clocks.push_back(c);
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 CLKD: trailing bytes after clock table");
        return false;
    }
    return true;
}

bool readFooterScalarBits(SpanReader& r, int byteWidth, quint64& out) {
    const char* valueBytes = nullptr;
    if (byteWidth <= 0 || byteWidth > 8 || !r.readBytes(valueBytes, byteWidth)) return false;
    out = 0;
    for (int i = 0; i < byteWidth; ++i) {
        out |= (quint64(static_cast<uchar>(valueBytes[i])) << (8 * i));
    }
    return true;
}

bool decodeSignalRecord(SpanReader& rr,
                        bool hasValueCodec,
                        bool useDeltaTimes,
                        bool useSharedTimeTable,
                        const QVector<u64>& sharedTimes,
                        i64 blockStart,
                        i64 blockEnd,
                        int byteWidth,
                        const QVector<int>& outputIndexes,
                        const QVector<WaveSignal>& outputSignals,
                        QVector<QVector<WaveSample>>& samplesByOutputIndex,
                        qint64 windowStart,
                        qint64 windowEnd,
                        QString& error);

bool decodeLodTransitionStreamPayload(const char* payload,
                                      int payloadSize,
                                      int byteWidth,
                                      QVector<WaveSample>& samples,
                                      QString& error) {
    if (byteWidth <= 0 || byteWidth > 8 || payloadSize < 0) {
        error = QStringLiteral("WVZ4 FOOT v9: invalid LOD transition stream header");
        return false;
    }

    SpanReader rr(payload, payloadSize);
    QVector<WaveSignal> fakeSignals;
    fakeSignals.resize(1);
    fakeSignals[0].width = qMin(64, byteWidth * 8);

    QVector<QVector<WaveSample>> decoded;
    decoded.resize(1);
    QVector<int> outputIndexes;
    outputIndexes.push_back(0);
    QVector<u64> sharedTimes;

    if (!decodeSignalRecord(rr, true, true, false, sharedTimes,
                            0, std::numeric_limits<qint64>::max(),
                            byteWidth, outputIndexes, fakeSignals, decoded,
                            0, std::numeric_limits<qint64>::max(), error)) {
        if (error.isEmpty()) error = QStringLiteral("WVZ4 FOOT v9: failed to decode LOD transition stream");
        return false;
    }
    if (!rr.eof()) {
        error = QStringLiteral("WVZ4 FOOT v9: LOD transition stream has trailing bytes");
        return false;
    }

    samples = decoded[0];
    return true;
}

bool parseFooterSection(const QByteArray& payload,
                        u32 formatVersion,
                        const QVector<int>& byteWidthByStorageId,
                        const QVector<int>& boolStorageByStorageId,
                        QVector<BlockIndexRec>& blocks,
                        QVector<QVector<int>>& blockIndexesByChunk,
                        QVector<QVector<WaveLodLevel>>& lodLevelsByStorageId,
                        QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count)) {
        error = QStringLiteral("WVZ4 FOOT: missing block count");
        return false;
    }
    if (count > 100000000ull) {
        error = QStringLiteral("WVZ4 FOOT: unreasonable block count");
        return false;
    }

    blocks.clear();
    blocks.reserve(int(qMin<u64>(count, u64(std::numeric_limits<int>::max()))));
    for (u64 i = 0; i < count; ++i) {
        u64 blockId = 0, fileOffset = 0, fileSize = 0, rawSize = 0;
        u64 signalChunkId = 0, firstSignalId = 1, signalCount = 0;
        i64 start = 0, end = 0;
        u8 comp = 0;
        if (!r.readVarUInt(blockId) ||
            !r.readI64(start) ||
            !r.readI64(end)) {
            error = QStringLiteral("WVZ4 FOOT: malformed block index record");
            return false;
        }
        if (formatVersion >= kSupportedVersionV3) {
            if (!r.readVarUInt(signalChunkId) ||
                !r.readVarUInt(firstSignalId) ||
                !r.readVarUInt(signalCount)) {
                error = QStringLiteral("WVZ4 FOOT: malformed v3 signal chunk fields");
                return false;
            }
            if (firstSignalId == 0 || signalCount == 0 ||
                firstSignalId > u64(std::numeric_limits<int>::max()) ||
                signalCount > u64(std::numeric_limits<int>::max()) ||
                firstSignalId > u64(std::numeric_limits<int>::max()) - signalCount + 1ull) {
                error = QStringLiteral("WVZ4 FOOT: invalid v3 signal chunk range for block %1").arg(blockId);
                return false;
            }
        }
        if (!r.readVarUInt(fileOffset) ||
            !r.readVarUInt(fileSize) ||
            !r.readVarUInt(rawSize) ||
            !r.readU8(comp)) {
            error = QStringLiteral("WVZ4 FOOT: malformed block index record");
            return false;
        }
        BlockIndexRec b;
        b.blockId = blockId;
        b.start = start;
        b.end = end;
        b.signalChunkId = signalChunkId;
        b.firstSignalId = firstSignalId;
        b.signalCount = signalCount;
        b.fileOffset = fileOffset;
        b.fileSize = fileSize;
        b.rawSize = rawSize;
        b.compression = Compression(comp);
        if (!validBlockTimeRange(b.start, b.end)) {
            error = QStringLiteral("WVZ4 FOOT: invalid block time range for block %1").arg(blockId);
            return false;
        }
        if (!isValidCompression(b.compression)) {
            error = QStringLiteral("WVZ4 FOOT: unsupported compression value %1").arg(int(comp));
            return false;
        }
        blocks.push_back(b);
    }
    blockIndexesByChunk.clear();
    lodLevelsByStorageId.clear();
    if (formatVersion >= kSupportedVersionV6) {
        u64 chunkCount = 0;
        if (!r.readVarUInt(chunkCount)) {
            error = QStringLiteral("WVZ4 FOOT v6: missing chunk index count");
            return false;
        }
        if (chunkCount > 10000000ull) {
            error = QStringLiteral("WVZ4 FOOT v6: unreasonable chunk index count");
            return false;
        }
        for (u64 ci = 0; ci < chunkCount; ++ci) {
            u64 chunkId = 0;
            u64 entryCount = 0;
            if (!r.readVarUInt(chunkId) || !r.readVarUInt(entryCount)) {
                error = QStringLiteral("WVZ4 FOOT v6: malformed chunk index header");
                return false;
            }
            if (chunkId > u64(std::numeric_limits<int>::max()) ||
                entryCount > u64(std::numeric_limits<int>::max())) {
                error = QStringLiteral("WVZ4 FOOT v6: chunk index too large");
                return false;
            }
            if (blockIndexesByChunk.size() <= int(chunkId)) {
                blockIndexesByChunk.resize(int(chunkId) + 1);
            }
            QVector<int>& indexes = blockIndexesByChunk[int(chunkId)];
            if (!indexes.isEmpty()) {
                error = QStringLiteral("WVZ4 FOOT v6: duplicate chunk index %1").arg(chunkId);
                return false;
            }
            indexes.reserve(int(entryCount));
            u64 prev = 0;
            for (u64 ei = 0; ei < entryCount; ++ei) {
                u64 delta = 0;
                if (!r.readVarUInt(delta)) {
                    error = QStringLiteral("WVZ4 FOOT v6: malformed chunk index body");
                    return false;
                }
                if (delta > u64(std::numeric_limits<int>::max()) ||
                    prev > u64(std::numeric_limits<int>::max()) - delta) {
                    error = QStringLiteral("WVZ4 FOOT v6: chunk index overflow");
                    return false;
                }
                const u64 index = prev + delta;
                if (index >= u64(blocks.size())) {
                    error = QStringLiteral("WVZ4 FOOT v6: chunk index references missing block");
                    return false;
                }
                if (!indexes.isEmpty() && int(index) <= indexes.last()) {
                    error = QStringLiteral("WVZ4 FOOT v6: chunk index is not strictly increasing");
                    return false;
                }
                const BlockIndexRec& b = blocks.at(int(index));
                if (b.signalChunkId != chunkId) {
                    error = QStringLiteral("WVZ4 FOOT v6: chunk index references wrong chunk");
                    return false;
                }
                indexes.push_back(int(index));
                prev = index;
            }
        }
    }
    if (formatVersion >= kSupportedVersionV7) {
        u64 lodLevelCount = 0;
        if (!r.readVarUInt(lodLevelCount)) {
            error = QStringLiteral("WVZ4 FOOT v7: missing LOD level count");
            return false;
        }
        if (lodLevelCount == 0 || lodLevelCount > 64ull) {
            error = QStringLiteral("WVZ4 FOOT v7: unreasonable LOD level count");
            return false;
        }
        QVector<qint64> bucketCyclesByLevel;
        bucketCyclesByLevel.reserve(int(lodLevelCount));
        qint64 prevBucketCycles = 0;
        for (u64 i = 0; i < lodLevelCount; ++i) {
            u64 bucketCycles = 0;
            if (!r.readVarUInt(bucketCycles) || bucketCycles == 0 ||
                bucketCycles > u64(std::numeric_limits<qint64>::max())) {
                error = QStringLiteral("WVZ4 FOOT v7: invalid LOD bucket width");
                return false;
            }
            if (prevBucketCycles != 0 && qint64(bucketCycles) <= prevBucketCycles) {
                error = QStringLiteral("WVZ4 FOOT v7: LOD bucket widths are not increasing");
                return false;
            }
            bucketCyclesByLevel.push_back(qint64(bucketCycles));
            prevBucketCycles = qint64(bucketCycles);
        }

        u64 storageCount = 0;
        if (!r.readVarUInt(storageCount)) {
            error = QStringLiteral("WVZ4 FOOT v7: missing LOD storage count");
            return false;
        }
        if (storageCount > 100000000ull) {
            error = QStringLiteral("WVZ4 FOOT v7: unreasonable LOD storage count");
            return false;
        }
        for (u64 si = 0; si < storageCount; ++si) {
            u64 storageId = 0;
            u64 storedByteWidth = 0;
            u64 storageFlags = 0;
            u64 storageLevelCount = 0;
            bool okStorageHeader = r.readVarUInt(storageId) && storageId != 0 &&
                storageId <= u64(std::numeric_limits<int>::max()) &&
                r.readVarUInt(storedByteWidth) && storedByteWidth != 0 && storedByteWidth <= 8;
            if (okStorageHeader && formatVersion >= kSupportedVersionV8) {
                okStorageHeader = r.readVarUInt(storageFlags) &&
                    storageFlags <= ((formatVersion >= kSupportedVersionV9) ? 0ull : 1ull);
            }
            okStorageHeader = okStorageHeader &&
                r.readVarUInt(storageLevelCount) && storageLevelCount <= lodLevelCount;
            if (!okStorageHeader) {
                error = QStringLiteral("WVZ4 FOOT v7: malformed LOD storage header");
                return false;
            }
            const int storageIndex = int(storageId);
            const int byteWidth = int(storedByteWidth);
            const int expectedByteWidth = directIntMapValue(byteWidthByStorageId, storageIndex, byteWidth);
            if (expectedByteWidth != byteWidth) {
                error = QStringLiteral("WVZ4 FOOT v7: LOD byte width mismatch for storage_id %1").arg(storageIndex);
                return false;
            }
            const bool storageIsBool = storageIndex >= 0 && storageIndex < boolStorageByStorageId.size() &&
                boolStorageByStorageId.at(storageIndex) != 0;
            const bool compactBool = formatVersion >= kSupportedVersionV8 &&
                formatVersion < kSupportedVersionV9 && ((storageFlags & 1ull) != 0);
            if (compactBool && (!storageIsBool || byteWidth != 1)) {
                error = QStringLiteral("WVZ4 FOOT v8: compact bool LOD flag on non-bool storage_id %1").arg(storageIndex);
                return false;
            }
            if (lodLevelsByStorageId.size() <= storageIndex) {
                lodLevelsByStorageId.resize(storageIndex + 1);
            }
            QVector<WaveLodLevel>& storageLevels = lodLevelsByStorageId[storageIndex];
            if (!storageLevels.isEmpty()) {
                error = QStringLiteral("WVZ4 FOOT v7: duplicate LOD storage_id %1").arg(storageIndex);
                return false;
            }
            storageLevels.resize(int(lodLevelCount));
            QVector<quint8> seenLevel(int(lodLevelCount), 0);
            for (u64 li = 0; li < storageLevelCount; ++li) {
                u64 levelIndex64 = 0;
                u64 recordCount = 0;
                if (!r.readVarUInt(levelIndex64) || levelIndex64 >= lodLevelCount ||
                    !r.readVarUInt(recordCount) ||
                    recordCount > u64(std::numeric_limits<int>::max())) {
                    error = QStringLiteral("WVZ4 FOOT v7: malformed LOD level header");
                    return false;
                }
                const int levelIndex = int(levelIndex64);
                if (seenLevel.at(levelIndex)) {
                    error = QStringLiteral("WVZ4 FOOT v7: duplicate LOD level for storage_id %1").arg(storageIndex);
                    return false;
                }
                seenLevel[levelIndex] = 1;

                WaveLodLevel& level = storageLevels[levelIndex];
                level.bucketCycles = bucketCyclesByLevel.at(levelIndex);
                if (formatVersion >= kSupportedVersionV9) {
                    u64 payloadSize64 = 0;
                    const char* levelPayload = nullptr;
                    if (!r.readVarUInt(payloadSize64) ||
                        payloadSize64 > u64(std::numeric_limits<int>::max()) ||
                        payloadSize64 > u64(r.remaining()) ||
                        !r.readBytes(levelPayload, int(payloadSize64))) {
                        error = QStringLiteral("WVZ4 FOOT v9: malformed LOD transition stream payload");
                        return false;
                    }
                    level.samples.reserve(int(recordCount));
                    if (!decodeLodTransitionStreamPayload(levelPayload, int(payloadSize64), byteWidth,
                                                          level.samples, error)) {
                        return false;
                    }
                    if (level.samples.size() > int(recordCount)) {
                        error = QStringLiteral("WVZ4 FOOT v9: LOD transition stream record count mismatch");
                        return false;
                    }
                    continue;
                }
                level.buckets.reserve(int(recordCount));
                u64 prevBucketStart = 0;
                for (u64 ri = 0; ri < recordCount; ++ri) {
                    u64 delta = 0;
                    u64 runBucketCount = 1;
                    u64 transitionCount = 0;
                    u8 stateMask = 0;
                    bool okBucketHeader = r.readVarUInt(delta);
                    if (okBucketHeader && formatVersion >= kSupportedVersionV8) {
                        okBucketHeader = r.readVarUInt(runBucketCount) && runBucketCount != 0;
                    }
                    okBucketHeader = okBucketHeader &&
                        r.readVarUInt(transitionCount) &&
                        transitionCount <= u64(std::numeric_limits<quint32>::max());
                    if (!okBucketHeader || !r.readU8(stateMask)) {
                        error = QStringLiteral("WVZ4 FOOT v7: malformed LOD bucket header");
                        return false;
                    }
                    if (transitionCount > u64(std::numeric_limits<quint32>::max())) {
                        error = QStringLiteral("WVZ4 FOOT v7: LOD transition count too large");
                        return false;
                    }
                    if (delta > u64(std::numeric_limits<qint64>::max()) ||
                        prevBucketStart > u64(std::numeric_limits<qint64>::max()) - delta) {
                        error = QStringLiteral("WVZ4 FOOT v7: LOD bucket start overflow");
                        return false;
                    }
                    const u64 bucketStart = prevBucketStart + delta;
                    if (!level.buckets.isEmpty() && qint64(bucketStart) <= level.buckets.last().start) {
                        error = QStringLiteral("WVZ4 FOOT v7: LOD bucket starts are not increasing");
                        return false;
                    }

                    WaveLodBucket bucket;
                    bucket.start = qint64(bucketStart);
                    if (runBucketCount > u64(std::numeric_limits<qint64>::max()) ||
                        u64(level.bucketCycles) > u64(std::numeric_limits<qint64>::max()) / runBucketCount) {
                        error = QStringLiteral("WVZ4 FOOT v8: LOD run span overflow");
                        return false;
                    }
                    const qint64 runCycles = qint64(runBucketCount * u64(level.bucketCycles));
                    if (runCycles > std::numeric_limits<qint64>::max() - bucket.start) {
                        error = QStringLiteral("WVZ4 FOOT v7: LOD bucket end overflow");
                        return false;
                    }
                    bucket.end = bucket.start + runCycles;
                    bucket.transitionCount = quint32(transitionCount);
                    if (!level.buckets.isEmpty() && bucket.start < level.buckets.last().end) {
                        error = QStringLiteral("WVZ4 FOOT v8: LOD records overlap");
                        return false;
                    }
                    if (compactBool) {
                        bucket.stateMask = stateMask & 0x0fu;
                        bucket.lastRawBits = (stateMask & 0x10u) ? 1ull : 0ull;
                        bucket.firstRawBits = bucket.lastRawBits;
                        bucket.minRawBits = (bucket.stateMask & kWaveLodSeenZero) ? 0ull : bucket.lastRawBits;
                        bucket.maxRawBits = (bucket.stateMask & kWaveLodSeenNonZero) ? 1ull : bucket.lastRawBits;
                    } else {
                        bucket.stateMask = stateMask;
                        if (!readFooterScalarBits(r, byteWidth, bucket.firstRawBits) ||
                            !readFooterScalarBits(r, byteWidth, bucket.lastRawBits) ||
                            !readFooterScalarBits(r, byteWidth, bucket.minRawBits) ||
                            !readFooterScalarBits(r, byteWidth, bucket.maxRawBits)) {
                            error = QStringLiteral("WVZ4 FOOT v7: truncated LOD bucket values");
                            return false;
                        }
                    }
                    level.buckets.push_back(bucket);
                    prevBucketStart = bucketStart;
                }
            }
            for (int i = storageLevels.size() - 1; i >= 0; --i) {
                if (storageLevels.at(i).bucketCycles != 0 ||
                    !storageLevels.at(i).buckets.isEmpty() ||
                    !storageLevels.at(i).samples.isEmpty()) break;
                storageLevels.removeLast();
            }
        }
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 FOOT: trailing bytes after block index");
        return false;
    }
    return true;
}

QByteArray zstdDecompress(const QByteArray& input, quint64 rawSize, QString& error) {
    error.clear();
    if (rawSize > quint64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 Zstd raw block too large");
        return {};
    }

    QByteArray out;
    out.resize(int(rawSize));
    const size_t written = ZSTD_decompress(out.data(), size_t(out.size()),
                                           input.constData(), size_t(input.size()));
    if (ZSTD_isError(written)) {
        error = QStringLiteral("WVZ4 ZSTD_decompress failed: %1")
            .arg(QString::fromLatin1(ZSTD_getErrorName(written)));
        return {};
    }
    if (written != size_t(out.size())) {
        error = QStringLiteral("WVZ4 Zstd raw size mismatch");
        return {};
    }
    return out;
}

QByteArray decompressBlockPayload(const QByteArray& encoded,
                                  Compression compression,
                                  quint64 rawSize,
                                  QString& error) {
    error.clear();
    switch (compression) {
    case Compression::None:
        if (rawSize != quint64(encoded.size())) {
            error = QStringLiteral("WVZ4 uncompressed block raw_size mismatch");
            return {};
        }
        return encoded;
    case Compression::Zstd:
        return zstdDecompress(encoded, rawSize, error);
    default:
        error = QStringLiteral("WVZ4 unsupported compression value %1").arg(int(compression));
        return {};
    }
}

QByteArray decodeCompressedLayoutPayload(const QByteArray& payload,
                                         const char* sectionTag,
                                         QString& error) {
    error.clear();

    SpanReader r(payload);
    u8 compByte = 0;
    u64 rawSize = 0;
    u64 encodedSize = 0;
    if (!r.readU8(compByte) ||
        !r.readVarUInt(rawSize) || rawSize > u64(std::numeric_limits<int>::max()) ||
        !r.readVarUInt(encodedSize) || encodedSize > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 %1 compressed layout header malformed").arg(QString::fromLatin1(sectionTag));
        return {};
    }

    const Compression compression = Compression(compByte);
    if (!isValidCompression(compression)) {
        error = QStringLiteral("WVZ4 %1 unsupported layout compression value %2")
            .arg(QString::fromLatin1(sectionTag))
            .arg(int(compByte));
        return {};
    }

    const char* encodedPtr = nullptr;
    if (!r.readBytes(encodedPtr, int(encodedSize))) {
        error = QStringLiteral("WVZ4 %1 compressed layout payload truncated").arg(QString::fromLatin1(sectionTag));
        return {};
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 %1 compressed layout has trailing bytes").arg(QString::fromLatin1(sectionTag));
        return {};
    }

    const QByteArray encoded = QByteArray::fromRawData(encodedPtr, int(encodedSize));
    QByteArray raw = decompressBlockPayload(encoded, compression, rawSize, error);
    if (!error.isEmpty()) {
        error = QStringLiteral("WVZ4 %1 compressed layout decode failed: %2")
            .arg(QString::fromLatin1(sectionTag), error);
        return {};
    }
    return raw;
}

ValueRadix convertRadix(Radix r, ValueType t, int width) {
    switch (r) {
    case Radix::Bin:
        return ValueRadix::Bin;
    case Radix::Dec:
        return ValueRadix::Dec;
    case Radix::Hex:
        return ValueRadix::Hex;
    case Radix::Float:
        return (width == 64 || t == ValueType::F64) ? ValueRadix::Double : ValueRadix::Float;
    case Radix::Auto:
    default:
        if (t == ValueType::F32) return ValueRadix::Float;
        if (t == ValueType::F64) return ValueRadix::Double;
        if (t == ValueType::I64) return ValueRadix::Int64;
        if (t == ValueType::U64) return ValueRadix::UInt64;
        if (t == ValueType::I8 || t == ValueType::I16 || t == ValueType::I32) return ValueRadix::Int;
        if (t == ValueType::U8 || t == ValueType::U16 || t == ValueType::U32) return ValueRadix::UInt;
        if (width == 1 || t == ValueType::Bool) return ValueRadix::Bin;
        return ValueRadix::Hex;
    }
}

SignalKind convertKind(ValueType t, int width) {
    return (t == ValueType::Bool || width == 1) ? SignalKind::Bit : SignalKind::Bus;
}

QString fallbackNodeName(quint32 nodeId) {
    return QStringLiteral("node_%1").arg(nodeId);
}

QString nodeSegmentName(u32 nodeId,
                        const QVector<NodeRec>& nodesById,
                        const QVector<QString>& namesById) {
    if (nodeId < u32(nodesById.size()) && nodesById[int(nodeId)].valid) {
        const NodeRec& n = nodesById[int(nodeId)];
        if (n.nameId < u32(namesById.size()) && !namesById[int(n.nameId)].isEmpty()) {
            return namesById[int(n.nameId)];
        }
    }
    return fallbackNodeName(nodeId);
}

bool validateNodeAndSignalLayout(const QVector<NodeRec>& nodesById,
                                 const QVector<QString>& namesById,
                                 const QVector<SigRec>& sigs,
                                 QString& error) {
    if (nodesById.isEmpty()) {
        error = QStringLiteral("WVZ4 NODE: empty node table");
        return false;
    }

    QVector<int> childCountByParent(nodesById.size(), 0);
    int rootCount = 0;

    for (int nodeId = 1; nodeId < nodesById.size(); ++nodeId) {
        if (!nodesById.at(nodeId).valid) continue;
        const NodeRec& n = nodesById.at(nodeId);

        if (n.nameId == 0 || n.nameId >= u32(namesById.size()) || namesById.at(int(n.nameId)).isEmpty()) {
            error = QStringLiteral("WVZ4 NODE node_id %1 references missing name_id %2").arg(nodeId).arg(int(n.nameId));
            return false;
        }
        if (!isValidNodeKind(n.kind)) {
            error = QStringLiteral("WVZ4 NODE node_id %1 has invalid NodeKind %2").arg(nodeId).arg(int(n.kind));
            return false;
        }
        if (n.parent == 0) {
            ++rootCount;
        } else {
            if (n.parent >= u32(nodesById.size()) || !nodesById.at(int(n.parent)).valid || int(n.parent) == nodeId) {
                error = QStringLiteral("WVZ4 NODE node_id %1 references invalid parent_id %2").arg(nodeId).arg(int(n.parent));
                return false;
            }
            ++childCountByParent[int(n.parent)];
        }
        if (n.firstChild != 0 && (n.firstChild >= u32(nodesById.size()) || !nodesById.at(int(n.firstChild)).valid)) {
            error = QStringLiteral("WVZ4 NODE node_id %1 references invalid first_child %2").arg(nodeId).arg(int(n.firstChild));
            return false;
        }
        if (n.nextSibling != 0 && (n.nextSibling >= u32(nodesById.size()) || !nodesById.at(int(n.nextSibling)).valid)) {
            error = QStringLiteral("WVZ4 NODE node_id %1 references invalid next_sibling %2").arg(nodeId).arg(int(n.nextSibling));
            return false;
        }
    }

    if (rootCount == 0) {
        error = QStringLiteral("WVZ4 NODE: no root-level node found");
        return false;
    }

    for (int nodeId = 1; nodeId < nodesById.size(); ++nodeId) {
        if (!nodesById.at(nodeId).valid) continue;
        const NodeRec& n = nodesById.at(nodeId);
        if (n.nextSibling != 0 && nodesById.at(int(n.nextSibling)).parent != n.parent) {
            error = QStringLiteral("WVZ4 NODE next_sibling of node_id %1 has a different parent_id").arg(nodeId);
            return false;
        }
    }

    // The viewer now trusts first_child/next_sibling and no longer performs an
    // O(N^2) parent_id fallback scan. Therefore every non-root node must be
    // reachable from its parent's child chain, and each chain must be acyclic.
    QVector<int> seen(nodesById.size(), 0);
    int stamp = 1;
    for (int parentId = 1; parentId < nodesById.size(); ++parentId) {
        if (!nodesById.at(parentId).valid) continue;

        int count = 0;
        for (int child = int(nodesById.at(parentId).firstChild);
             child != 0;
             child = int(nodesById.at(child).nextSibling)) {
            if (child < 0 || child >= nodesById.size() || !nodesById.at(child).valid) {
                error = QStringLiteral("WVZ4 NODE child chain references missing node_id %1").arg(child);
                return false;
            }
            if (nodesById.at(child).parent != u32(parentId)) {
                error = QStringLiteral("WVZ4 NODE child %1 has wrong parent_id under parent %2").arg(child).arg(parentId);
                return false;
            }
            if (seen[child] == stamp) {
                error = QStringLiteral("WVZ4 NODE child chain cycle under node_id %1").arg(parentId);
                return false;
            }
            seen[child] = stamp;
            ++count;
        }

        if (count != childCountByParent.at(parentId)) {
            error = QStringLiteral("WVZ4 NODE child chain under node_id %1 does not enumerate all parent_id children").arg(parentId);
            return false;
        }

        ++stamp;
        if (stamp == std::numeric_limits<int>::max()) {
            std::fill(seen.begin(), seen.end(), 0);
            stamp = 1;
        }
    }

    QVector<uchar> seenSignalNodes(nodesById.size(), 0);
    for (const SigRec& s : sigs) {
        if (s.nodeId == 0 || s.nodeId >= u32(nodesById.size()) || !nodesById.at(int(s.nodeId)).valid) {
            error = QStringLiteral("WVZ4 SIGT references missing node_id %1").arg(int(s.nodeId));
            return false;
        }
        const int nodeId = int(s.nodeId);
        const NodeRec& leaf = nodesById.at(nodeId);
        if (leaf.kind != kNodeKindSignalLeaf) {
            error = QStringLiteral("WVZ4 SIGT node_id %1 is not a SignalLeaf").arg(nodeId);
            return false;
        }
        if (leaf.firstChild != 0) {
            error = QStringLiteral("WVZ4 SIGT node_id %1 must be a leaf node").arg(nodeId);
            return false;
        }
        if (seenSignalNodes.at(nodeId)) {
            error = QStringLiteral("WVZ4 SIGT has multiple signals bound to node_id %1").arg(nodeId);
            return false;
        }
        seenSignalNodes[nodeId] = 1;
    }

    return true;
}

bool buildWaveTreeInfo(const QVector<NodeRec>& nodesById,
                       const QVector<QString>& namesById,
                       const QVector<SigRec>& sigs,
                       const QVector<int>& outputIndexBySignalId,
                       WaveTreeInfo& outTree,
                       QString& error) {
    outTree = WaveTreeInfo();
    if (nodesById.isEmpty()) return true;

    outTree.valid = true;
    outTree.nodesById.resize(nodesById.size());

    for (int nodeId = 1; nodeId < nodesById.size(); ++nodeId) {
        if (!nodesById.at(nodeId).valid) continue;

        const NodeRec& src = nodesById.at(nodeId);
        WaveTreeNode dst;
        dst.nodeId = nodeId;
        dst.parentId = int(src.parent);
        dst.firstChild = int(src.firstChild);
        dst.nextSibling = int(src.nextSibling);
        dst.name = nodeSegmentName(src.id, nodesById, namesById);
        dst.valid = true;
        outTree.nodesById[nodeId] = dst;

        if (src.parent == 0) {
            outTree.rootNodeIds.push_back(nodeId);
        }
    }

    int maxSignalIndex = -1;
    for (int i = 0; i < outputIndexBySignalId.size(); ++i) {
        if (outputIndexBySignalId.at(i) >= 0) maxSignalIndex = qMax(maxSignalIndex, outputIndexBySignalId.at(i));
    }
    if (maxSignalIndex >= 0) {
        outTree.signalIndexToNodeId.resize(maxSignalIndex + 1);
        std::fill(outTree.signalIndexToNodeId.begin(), outTree.signalIndexToNodeId.end(), -1);
    }

    for (const SigRec& s : sigs) {
        const int signalIndex = directIntMapValue(outputIndexBySignalId, int(s.signalId), -1);
        if (signalIndex < 0) continue;

        if (s.nodeId >= u32(outTree.nodesById.size()) || !outTree.nodesById[int(s.nodeId)].valid) {
            error = QStringLiteral("WVZ4 SIGT references missing node_id %1").arg(int(s.nodeId));
            return false;
        }

        WaveTreeNode& leaf = outTree.nodesById[int(s.nodeId)];
        leaf.signalIndex = signalIndex;
        leaf.signalId = int(s.signalId);
        if (signalIndex >= 0 && signalIndex < outTree.signalIndexToNodeId.size()) {
            outTree.signalIndexToNodeId[signalIndex] = int(s.nodeId);
        }
    }

    // Validate child/sibling chains enough to prevent lazy UI traversal loops.
    QVector<int> seen(nodesById.size(), 0);
    int stamp = 1;
    for (int parentId = 1; parentId < nodesById.size(); ++parentId) {
        if (!nodesById.at(parentId).valid) continue;

        int child = int(nodesById.at(parentId).firstChild);
        while (child != 0) {
            if (child < 0 || child >= nodesById.size() || !nodesById.at(child).valid) {
                error = QStringLiteral("WVZ4 NODE child chain references missing node_id %1").arg(child);
                return false;
            }
            if (nodesById.at(child).parent != u32(parentId)) {
                error = QStringLiteral("WVZ4 NODE child %1 has wrong parent_id").arg(child);
                return false;
            }
            if (seen[child] == stamp) {
                error = QStringLiteral("WVZ4 NODE child chain cycle under node_id %1").arg(parentId);
                return false;
            }
            seen[child] = stamp;
            child = int(nodesById.at(child).nextSibling);
        }

        ++stamp;
        if (stamp == std::numeric_limits<int>::max()) {
            std::fill(seen.begin(), seen.end(), 0);
            stamp = 1;
        }
    }

    return true;
}


quint64 readScalarBitsLE(const char* p, int n) {
    quint64 v = 0;
    const int count = qMin(n, 8);
    for (int i = 0; i < count; ++i) {
        v |= (quint64(static_cast<uchar>(p[i])) << (8 * i));
    }
    return v;
}

void appendCompactedSample(QVector<WaveSample>& rows, WaveSample&& sample) {
    if (!rows.isEmpty()) {
        WaveSample& last = rows.last();
        if (last.time == sample.time) {
            last = std::move(sample);
            return;
        }
        if (waveSamplesEquivalent(last, sample)) {
            return;
        }
    }
    rows.push_back(std::move(sample));
}

bool appendDecodedSample(int outputIndex,
                         const char* valueBytes,
                         int byteCount,
                         qint64 sampleTime,
                         const QVector<WaveSignal>& outputSignals,
                         QVector<QVector<WaveSample>>& samplesByOutputIndex,
                         QString& error) {
    if (outputIndex < 0 || outputIndex >= outputSignals.size() || outputIndex >= samplesByOutputIndex.size()) {
        error = QStringLiteral("WVZ4 WDAT sample references invalid output signal index");
        return false;
    }

    WaveSample sample;
    sample.time = sampleTime;
    sample.isAbsent = false;
    sample.isZ = false;
    sample.rawFieldsReady = true;
    const WaveSignal& sig = outputSignals.at(outputIndex);
    sample.rawBits = readScalarBitsLE(valueBytes, byteCount) & waveBitMaskForWidth(sig.width);
    // WVZ4 stores fixed-width <=64-bit scalar values.  Do not materialize a
    // QString for every decoded sample; display code formats rawBits on demand.
    sample.value.clear();
    appendCompactedSample(samplesByOutputIndex[outputIndex], std::move(sample));
    return true;
}

bool appendDecodedSampleToOutputs(const QVector<int>& outputIndexes,
                                  const char* valueBytes,
                                  int byteCount,
                                  qint64 sampleTime,
                                  const QVector<WaveSignal>& outputSignals,
                                  QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                  QString& error) {
    for (int i = 0; i < outputIndexes.size(); ++i) {
        if (!appendDecodedSample(outputIndexes.at(i), valueBytes, byteCount, sampleTime,
                                 outputSignals, samplesByOutputIndex, error)) {
            return false;
        }
    }
    return true;
}

bool appendImplicitZeroSamplesForSelectedSignals(const QSet<int>& selectedIds,
                                                bool allSelected,
                                                const QVector<WaveSignal>& outputSignals,
                                                QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                                qint64& minTime,
                                                qint64& maxTime,
                                                QString& error) {
    Q_UNUSED(error);
    bool appendedAny = false;

    for (int outputIndex = 0; outputIndex < outputSignals.size(); ++outputIndex) {
        const WaveSignal& sig = outputSignals.at(outputIndex);
        if (!allSelected && !selectedIds.contains(sig.signalId)) continue;
        if (outputIndex < 0 || outputIndex >= samplesByOutputIndex.size()) continue;

        WaveSample sample;
        sample.time = 0;
        sample.isAbsent = false;
        sample.isZ = false;
        sample.rawBits = 0ull;
        sample.rawFieldsReady = true;
        sample.value.clear();
        appendCompactedSample(samplesByOutputIndex[outputIndex], std::move(sample));
        appendedAny = true;
    }

    if (appendedAny) {
        minTime = qMin(minTime, qint64(0));
        maxTime = qMax(maxTime, qint64(0));
    }
    return true;
}

void appendClockSamplesForLoadedSignals(const QVector<ClockRec>& clocks,
                                        const QVector<int>& outputIndexBySignalId,
                                        qint64 windowStart,
                                        qint64 windowEnd,
                                        QVector<WaveSignal>& outputSignals,
                                        QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                        qint64& minTime,
                                        qint64& maxTime) {
    static const qint64 kMaxSynthClockSamplesPerSignal = 1000000;
    if (clocks.isEmpty()) return;

    qint64 endTime = maxTime;
    if (windowEnd >= windowStart) endTime = qMin(endTime, windowEnd);
    if (endTime < 0) endTime = 0;

    const qint64 startWindow = (windowEnd >= windowStart) ? qMax<qint64>(0, windowStart) : 0;
    for (int i = 0; i < clocks.size(); ++i) {
        const ClockRec& c = clocks.at(i);
        const int outputIndex = directIntMapValue(outputIndexBySignalId, int(c.signalId), -1);
        if (outputIndex < 0 || outputIndex >= outputSignals.size() || outputIndex >= samplesByOutputIndex.size()) continue;
        if (!outputSignals.at(outputIndex).samplesLoaded) continue;

        const qint64 period = c.periodTicks > u64(std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max()
            : qint64(c.periodTicks);
        if (period <= 0) continue;

        auto appendClockSample = [&](qint64 t, bool value) {
            if (!sampleInWindow(t, windowStart, windowEnd)) return;
            char byte = value ? 1 : 0;
            QString ignored;
            appendDecodedSample(outputIndex, &byte, 1, t, outputSignals, samplesByOutputIndex, ignored);
            minTime = qMin(minTime, t);
            maxTime = qMax(maxTime, t);
        };

        appendClockSample(0, c.initialValue);

        if (endTime <= 0) continue;
        qint64 firstToggle = period;
        if (startWindow > period) {
            const qint64 steps = startWindow / period;
            firstToggle = steps * period;
            if (firstToggle < startWindow) {
                if (period > std::numeric_limits<qint64>::max() - firstToggle) continue;
                firstToggle += period;
            }
        }
        if (firstToggle <= 0) firstToggle = period;

        qint64 count = 0;
        for (qint64 t = firstToggle; t <= endTime && count < kMaxSynthClockSamplesPerSignal; ) {
            const qint64 toggleIndex = t / period;
            const bool value = (toggleIndex & 1) ? !c.initialValue : c.initialValue;
            appendClockSample(t, value);
            ++count;
            if (period > std::numeric_limits<qint64>::max() - t) break;
            t += period;
        }
    }
}

bool validateRawBlockHeader(u64 blockId,
                            i64 blockStart,
                            i64 blockEnd,
                            u64 expectedBlockId,
                            i64 expectedStart,
                            i64 expectedEnd,
                            QString& error) {
    if (!validBlockTimeRange(blockStart, blockEnd)) {
        error = QStringLiteral("WVZ4 WDAT raw payload has invalid block time range");
        return false;
    }
    if (blockId != expectedBlockId || blockStart != expectedStart || blockEnd != expectedEnd) {
        error = QStringLiteral("WVZ4 WDAT raw/header block metadata mismatch");
        return false;
    }
    return true;
}

bool decodeRawWaveBlockV1(const QByteArray& rawPayload,
                          u64 expectedBlockId,
                          i64 expectedStart,
                          i64 expectedEnd,
                          const QSet<int>& selectedIds,
                          bool allSelected,
                          const QVector<QVector<int>>& outputIndexesByStorageId,
                          const QVector<WaveSignal>& outputSignals,
                          QVector<QVector<WaveSample>>& samplesByOutputIndex,
                          qint64 windowStart,
                          qint64 windowEnd,
                          qint64& minTime,
                          qint64& maxTime,
                          QString& error) {
    SpanReader r(rawPayload);

    u64 blockId = 0;
    i64 blockStart = 0;
    i64 blockEnd = 0;
    u64 recordCount = 0;
    if (!r.readVarUInt(blockId) ||
        !r.readI64(blockStart) ||
        !r.readI64(blockEnd) ||
        !r.readVarUInt(recordCount)) {
        error = QStringLiteral("WVZ4 WDAT raw payload header malformed");
        return false;
    }
    if (!validateRawBlockHeader(blockId, blockStart, blockEnd, expectedBlockId, expectedStart, expectedEnd, error)) return false;

    minTime = qMin(minTime, blockStart);
    maxTime = qMax(maxTime, blockEnd);

    for (u64 ri = 0; ri < recordCount; ++ri) {
        u64 sid64 = 0;
        u64 transitionCount = 0;
        if (!r.readVarUInt(sid64) || sid64 == 0 || sid64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(transitionCount)) {
            error = QStringLiteral("WVZ4 WDAT record header malformed");
            return false;
        }

        const int sid = int(sid64);
        const bool keep = allSelected || selectedIds.contains(sid);
        const QVector<int>* outputIndexes = directIntListMapValue(outputIndexesByStorageId, sid);

        u64 prevRel = 0;
        bool havePrevRel = false;
        for (u64 ti = 0; ti < transitionCount; ++ti) {
            u64 rel64 = 0;
            u8 byteCount = 0;
            if (!r.readVarUInt(rel64) || rel64 > u64(std::numeric_limits<qint64>::max()) ||
                !r.readU8(byteCount) || byteCount == 0 || byteCount > 8) {
                error = QStringLiteral("WVZ4 WDAT transition header malformed");
                return false;
            }
            if (havePrevRel && rel64 < prevRel) {
                error = QStringLiteral("WVZ4 WDAT transition times are not monotonic");
                return false;
            }
            prevRel = rel64;
            havePrevRel = true;

            const char* valueBytes = nullptr;
            if (!r.readBytes(valueBytes, int(byteCount))) {
                error = QStringLiteral("WVZ4 WDAT transition value truncated");
                return false;
            }

            qint64 sampleTime = 0;
            if (!addRelTimeChecked(blockStart, blockEnd, rel64, sampleTime)) {
                error = QStringLiteral("WVZ4 WDAT sample time is outside the block or overflows");
                return false;
            }
            if (!keep || !outputIndexes || outputIndexes->isEmpty() || !sampleInWindow(sampleTime, windowStart, windowEnd)) continue;
            if (!appendDecodedSampleToOutputs(*outputIndexes, valueBytes, int(byteCount), sampleTime,
                                              outputSignals, samplesByOutputIndex, error)) return false;
        }
    }

    if (!r.eof()) {
        error = QStringLiteral("WVZ4 WDAT raw payload has trailing bytes");
        return false;
    }
    return true;
}

bool decodeRawWaveBlockV2(const QByteArray& rawPayload,
                          u64 expectedBlockId,
                          i64 expectedStart,
                          i64 expectedEnd,
                          const QSet<int>& selectedIds,
                          bool allSelected,
                          const QVector<QVector<int>>& outputIndexesByStorageId,
                          const QVector<int>& byteWidthBySignalId,
                          const QVector<WaveSignal>& outputSignals,
                          QVector<QVector<WaveSample>>& samplesByOutputIndex,
                          qint64 windowStart,
                          qint64 windowEnd,
                          qint64& minTime,
                          qint64& maxTime,
                          QString& error) {
    SpanReader r(rawPayload);

    u64 blockId = 0;
    i64 blockStart = 0;
    i64 blockEnd = 0;
    u64 flags = 0;
    if (!r.readVarUInt(blockId) ||
        !r.readI64(blockStart) ||
        !r.readI64(blockEnd) ||
        !r.readVarUInt(flags)) {
        error = QStringLiteral("WVZ4 WDAT v2 raw payload header malformed");
        return false;
    }
    if (!validateRawBlockHeader(blockId, blockStart, blockEnd, expectedBlockId, expectedStart, expectedEnd, error)) return false;

    if ((flags & ~kKnownWdatV2Flags) != 0) {
        error = QStringLiteral("WVZ4 WDAT v2 raw payload has unknown flags: 0x%1").arg(QString::number(flags, 16));
        return false;
    }
    if ((flags & kWdatFixedValueWidth) == 0) {
        error = QStringLiteral("WVZ4 WDAT v2 without fixed value width is not supported");
        return false;
    }

    const bool useDeltaTimes = (flags & kWdatDeltaTimes) != 0;
    const bool useSharedTimeTable = (flags & kWdatSharedTimeTable) != 0;
    if (useDeltaTimes && useSharedTimeTable) {
        error = QStringLiteral("WVZ4 WDAT v2 cannot combine delta-time and shared-time encodings");
        return false;
    }

    minTime = qMin(minTime, blockStart);
    maxTime = qMax(maxTime, blockEnd);

    QVector<u64> sharedTimes;
    if (useSharedTimeTable) {
        u64 sharedCount = 0;
        if (!r.readVarUInt(sharedCount) || sharedCount > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 WDAT v2 shared time table count is invalid");
            return false;
        }
        if (sharedCount > u64(r.remaining())) {
            error = QStringLiteral("WVZ4 WDAT v2 shared time table count exceeds payload size");
            return false;
        }
        sharedTimes.reserve(int(sharedCount));
        u64 prev = 0;
        for (u64 i = 0; i < sharedCount; ++i) {
            u64 delta = 0;
            if (!r.readVarUInt(delta) || delta > std::numeric_limits<u64>::max() - prev) {
                error = QStringLiteral("WVZ4 WDAT v2 shared time table is malformed");
                return false;
            }
            const u64 rel = prev + delta;
            qint64 ignoredTime = 0;
            if (!addRelTimeChecked(blockStart, blockEnd, rel, ignoredTime)) {
                error = QStringLiteral("WVZ4 WDAT v2 shared time is outside the block or overflows");
                return false;
            }
            sharedTimes.push_back(rel);
            prev = rel;
        }
    }

    u64 recordCount = 0;
    if (!r.readVarUInt(recordCount)) {
        error = QStringLiteral("WVZ4 WDAT v2 missing signal record count");
        return false;
    }
    if (recordCount > u64(r.remaining())) {
        error = QStringLiteral("WVZ4 WDAT v2 signal record count exceeds payload size");
        return false;
    }

    for (u64 ri = 0; ri < recordCount; ++ri) {
        u64 sid64 = 0;
        u64 transitionCount = 0;
        if (!r.readVarUInt(sid64) || sid64 == 0 || sid64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(transitionCount)) {
            error = QStringLiteral("WVZ4 WDAT v2 record header malformed");
            return false;
        }

        const int sid = int(sid64);
        const int byteWidth = directIntMapValue(byteWidthBySignalId, sid, -1);
        if (byteWidth <= 0 || byteWidth > 8) {
            error = QStringLiteral("WVZ4 WDAT v2 references unknown signal_id %1").arg(sid);
            return false;
        }

        const bool keep = allSelected || selectedIds.contains(sid);
        const QVector<int>* outputIndexes = directIntListMapValue(outputIndexesByStorageId, sid);
        u64 prevRel = 0;
        bool havePrevRel = false;

        for (u64 ti = 0; ti < transitionCount; ++ti) {
            u64 timeCode = 0;
            if (!r.readVarUInt(timeCode)) {
                error = QStringLiteral("WVZ4 WDAT v2 transition time code malformed");
                return false;
            }

            u64 rel = 0;
            if (useSharedTimeTable) {
                if (timeCode >= u64(sharedTimes.size())) {
                    error = QStringLiteral("WVZ4 WDAT v2 shared time index out of range");
                    return false;
                }
                rel = sharedTimes.at(int(timeCode));
            } else if (useDeltaTimes) {
                if (timeCode > std::numeric_limits<u64>::max() - prevRel) {
                    error = QStringLiteral("WVZ4 WDAT v2 delta time overflows");
                    return false;
                }
                rel = prevRel + timeCode;
            } else {
                rel = timeCode;
            }

            if (havePrevRel && rel < prevRel) {
                error = QStringLiteral("WVZ4 WDAT v2 transition times are not monotonic");
                return false;
            }
            prevRel = rel;
            havePrevRel = true;

            const char* valueBytes = nullptr;
            if (!r.readBytes(valueBytes, byteWidth)) {
                error = QStringLiteral("WVZ4 WDAT v2 transition value truncated");
                return false;
            }

            qint64 sampleTime = 0;
            if (!addRelTimeChecked(blockStart, blockEnd, rel, sampleTime)) {
                error = QStringLiteral("WVZ4 WDAT v2 sample time is outside the block or overflows");
                return false;
            }
            if (!keep || !outputIndexes || outputIndexes->isEmpty() || !sampleInWindow(sampleTime, windowStart, windowEnd)) continue;
            if (!appendDecodedSampleToOutputs(*outputIndexes, valueBytes, byteWidth, sampleTime,
                                              outputSignals, samplesByOutputIndex, error)) return false;
        }
    }

    if (!r.eof()) {
        error = QStringLiteral("WVZ4 WDAT v2 raw payload has trailing bytes");
        return false;
    }
    return true;
}


bool resolveRecordTime(u64 timeCode,
                       bool useDeltaTimes,
                       bool useSharedTimeTable,
                       const QVector<u64>& sharedTimes,
                       u64& prevRel,
                       bool& havePrevRel,
                       i64 blockStart,
                       i64 blockEnd,
                       qint64& sampleTime,
                       QString& error,
                       const char* context) {
    u64 rel = 0;
    if (useSharedTimeTable) {
        if (timeCode >= u64(sharedTimes.size())) {
            error = QStringLiteral("WVZ4 %1 shared time index out of range").arg(QString::fromLatin1(context));
            return false;
        }
        rel = sharedTimes.at(int(timeCode));
    } else if (useDeltaTimes) {
        if (timeCode > std::numeric_limits<u64>::max() - prevRel) {
            error = QStringLiteral("WVZ4 %1 delta time overflows").arg(QString::fromLatin1(context));
            return false;
        }
        rel = prevRel + timeCode;
    } else {
        rel = timeCode;
    }

    if (havePrevRel && rel < prevRel) {
        error = QStringLiteral("WVZ4 %1 transition times are not monotonic").arg(QString::fromLatin1(context));
        return false;
    }
    prevRel = rel;
    havePrevRel = true;

    if (!addRelTimeChecked(blockStart, blockEnd, rel, sampleTime)) {
        error = QStringLiteral("WVZ4 %1 sample time is outside the block or overflows").arg(QString::fromLatin1(context));
        return false;
    }
    return true;
}

bool resolveStrideRecordTime(u64 firstRel,
                             u64 stride,
                             u64 index,
                             i64 blockStart,
                             i64 blockEnd,
                             qint64& sampleTime,
                             QString& error,
                             const char* context) {
    if (index != 0 && stride > (std::numeric_limits<u64>::max() - firstRel) / index) {
        error = QStringLiteral("WVZ4 %1 stride time overflows").arg(QString::fromLatin1(context));
        return false;
    }
    const u64 rel = firstRel + stride * index;
    if (!addRelTimeChecked(blockStart, blockEnd, rel, sampleTime)) {
        error = QStringLiteral("WVZ4 %1 stride sample time is outside the block or overflows").arg(QString::fromLatin1(context));
        return false;
    }
    return true;
}

bool readFullValue(SpanReader& rr,
                   char value[8],
                   int byteWidth,
                   QString& error,
                   const char* context) {
    const char* p = nullptr;
    if (!rr.readBytes(p, byteWidth)) {
        error = QStringLiteral("WVZ4 %1 value truncated").arg(QString::fromLatin1(context));
        return false;
    }
    std::memset(value, 0, 8);
    std::memcpy(value, p, byteWidth);
    return true;
}

bool applyChangedByteMask(SpanReader& rr,
                          char value[8],
                          int byteWidth,
                          u8 mask,
                          QString& error,
                          const char* context) {
    const u8 validMask = byteWidth >= 8 ? 0xffu : static_cast<u8>((1u << byteWidth) - 1u);
    if ((mask & ~validMask) != 0) {
        error = QStringLiteral("WVZ4 %1 changed-byte mask exceeds value width").arg(QString::fromLatin1(context));
        return false;
    }
    for (int b = 0; b < byteWidth; ++b) {
        if ((mask & static_cast<u8>(1u << b)) == 0) continue;
        const char* p = nullptr;
        if (!rr.readBytes(p, 1)) {
            error = QStringLiteral("WVZ4 %1 changed-byte payload truncated").arg(QString::fromLatin1(context));
            return false;
        }
        value[b] = *p;
    }
    return true;
}

bool appendDecodedRecordValue(const QVector<int>& outputIndexes,
                              const char value[8],
                              int byteWidth,
                              qint64 sampleTime,
                              qint64 windowStart,
                              qint64 windowEnd,
                              const QVector<WaveSignal>& outputSignals,
                              QVector<QVector<WaveSample>>& samplesByOutputIndex,
                              QString& error) {
    if (!sampleInWindow(sampleTime, windowStart, windowEnd)) return true;
    return appendDecodedSampleToOutputs(outputIndexes, value, byteWidth, sampleTime,
                                        outputSignals, samplesByOutputIndex, error);
}

bool decodeSignalRecord(SpanReader& rr,
                        bool hasValueCodec,
                        bool useDeltaTimes,
                        bool useSharedTimeTable,
                        const QVector<u64>& sharedTimes,
                        i64 blockStart,
                        i64 blockEnd,
                        int byteWidth,
                        const QVector<int>& outputIndexes,
                        const QVector<WaveSignal>& outputSignals,
                        QVector<QVector<WaveSample>>& samplesByOutputIndex,
                        qint64 windowStart,
                        qint64 windowEnd,
                        QString& error) {
    u8 codecByte = static_cast<u8>(ValueRecordCodec::FullValues);
    if (hasValueCodec && !rr.readU8(codecByte)) {
        error = QStringLiteral("WVZ4 WDAT signal record missing value codec");
        return false;
    }

    u64 transitionCount = 0;
    if (!rr.readVarUInt(transitionCount)) {
        error = QStringLiteral("WVZ4 WDAT signal record missing transition count");
        return false;
    }

    const ValueRecordCodec codec = static_cast<ValueRecordCodec>(codecByte);
    char value[8] = {};
    u64 prevRel = 0;
    bool havePrevRel = false;

    auto readRecordTime = [&](qint64& sampleTime) -> bool {
        u64 timeCode = 0;
        if (!rr.readVarUInt(timeCode)) {
            error = QStringLiteral("WVZ4 WDAT transition time code malformed");
            return false;
        }
        return resolveRecordTime(timeCode, useDeltaTimes, useSharedTimeTable, sharedTimes,
                                 prevRel, havePrevRel, blockStart, blockEnd, sampleTime,
                                 error, "WDAT");
    };

    switch (codec) {
    case ValueRecordCodec::FullValues: {
        for (u64 ti = 0; ti < transitionCount; ++ti) {
            qint64 sampleTime = 0;
            if (!readRecordTime(sampleTime)) return false;
            if (!readFullValue(rr, value, byteWidth, error, "WDAT full-value record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
        }
        return true;
    }
    case ValueRecordCodec::FullValuesStride: {
        if (transitionCount == 0) return true;
        u64 firstRel = 0, stride = 0;
        if (!rr.readVarUInt(firstRel) || !rr.readVarUInt(stride)) {
            error = QStringLiteral("WVZ4 WDAT full-stride record time header malformed");
            return false;
        }
        for (u64 ti = 0; ti < transitionCount; ++ti) {
            qint64 sampleTime = 0;
            if (!resolveStrideRecordTime(firstRel, stride, ti, blockStart, blockEnd, sampleTime, error, "WDAT full-stride record")) return false;
            if (!readFullValue(rr, value, byteWidth, error, "WDAT full-stride record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
        }
        return true;
    }
    case ValueRecordCodec::BoolToggle: {
        u8 initial = 0;
        if (!rr.readU8(initial) || initial > 1) {
            error = QStringLiteral("WVZ4 WDAT bool-toggle record initial value malformed");
            return false;
        }
        value[0] = initial ? 1 : 0;
        for (u64 ti = 0; ti < transitionCount; ++ti) {
            qint64 sampleTime = 0;
            if (!readRecordTime(sampleTime)) return false;
            value[0] = (ti == 0) ? (initial ? 1 : 0) : (value[0] ? 0 : 1);
            if (!appendDecodedRecordValue(outputIndexes, value, 1, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
        }
        return true;
    }
    case ValueRecordCodec::BoolToggleStride: {
        u8 initial = 0;
        if (!rr.readU8(initial) || initial > 1) {
            error = QStringLiteral("WVZ4 WDAT bool-toggle-stride record initial value malformed");
            return false;
        }
        if (transitionCount == 0) return true;
        u64 firstRel = 0, stride = 0;
        if (!rr.readVarUInt(firstRel) || !rr.readVarUInt(stride)) {
            error = QStringLiteral("WVZ4 WDAT bool-toggle-stride record time header malformed");
            return false;
        }
        value[0] = initial ? 1 : 0;
        for (u64 ti = 0; ti < transitionCount; ++ti) {
            qint64 sampleTime = 0;
            if (!resolveStrideRecordTime(firstRel, stride, ti, blockStart, blockEnd, sampleTime, error, "WDAT bool-toggle-stride record")) return false;
            value[0] = (ti == 0) ? (initial ? 1 : 0) : (value[0] ? 0 : 1);
            if (!appendDecodedRecordValue(outputIndexes, value, 1, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
        }
        return true;
    }
    case ValueRecordCodec::ByteMask: {
        if (transitionCount == 0) return true;
        qint64 sampleTime = 0;
        if (!readRecordTime(sampleTime)) return false;
        if (!readFullValue(rr, value, byteWidth, error, "WDAT byte-mask record")) return false;
        if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                      outputSignals, samplesByOutputIndex, error)) return false;
        for (u64 ti = 1; ti < transitionCount; ++ti) {
            if (!readRecordTime(sampleTime)) return false;
            u8 mask = 0;
            if (!rr.readU8(mask)) {
                error = QStringLiteral("WVZ4 WDAT byte-mask record mask truncated");
                return false;
            }
            if (!applyChangedByteMask(rr, value, byteWidth, mask, error, "WDAT byte-mask record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
        }
        return true;
    }
    case ValueRecordCodec::ByteMaskStride: {
        if (transitionCount == 0) return true;
        u64 firstRel = 0, stride = 0;
        if (!rr.readVarUInt(firstRel) || !rr.readVarUInt(stride)) {
            error = QStringLiteral("WVZ4 WDAT byte-mask-stride record time header malformed");
            return false;
        }
        qint64 sampleTime = 0;
        if (!resolveStrideRecordTime(firstRel, stride, 0, blockStart, blockEnd, sampleTime, error, "WDAT byte-mask-stride record")) return false;
        if (!readFullValue(rr, value, byteWidth, error, "WDAT byte-mask-stride record")) return false;
        if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                      outputSignals, samplesByOutputIndex, error)) return false;
        for (u64 ti = 1; ti < transitionCount; ++ti) {
            u8 mask = 0;
            if (!rr.readU8(mask)) {
                error = QStringLiteral("WVZ4 WDAT byte-mask-stride record mask truncated");
                return false;
            }
            if (!resolveStrideRecordTime(firstRel, stride, ti, blockStart, blockEnd, sampleTime, error, "WDAT byte-mask-stride record")) return false;
            if (!applyChangedByteMask(rr, value, byteWidth, mask, error, "WDAT byte-mask-stride record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
        }
        return true;
    }
    case ValueRecordCodec::NibbleMask: {
        if (transitionCount == 0) return true;
        qint64 sampleTime = 0;
        if (!readRecordTime(sampleTime)) return false;
        if (!readFullValue(rr, value, byteWidth, error, "WDAT nibble-mask record")) return false;
        if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                      outputSignals, samplesByOutputIndex, error)) return false;
        for (u64 ti = 1; ti < transitionCount; ti += 2) {
            qint64 sampleTime0 = 0;
            if (!readRecordTime(sampleTime0)) return false;
            qint64 sampleTime1 = 0;
            const bool haveSecond = (ti + 1 < transitionCount);
            if (haveSecond && !readRecordTime(sampleTime1)) return false;
            u8 packedMask = 0;
            if (!rr.readU8(packedMask)) {
                error = QStringLiteral("WVZ4 WDAT nibble-mask record mask truncated");
                return false;
            }
            if (!applyChangedByteMask(rr, value, byteWidth, static_cast<u8>(packedMask & 0x0f), error, "WDAT nibble-mask record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime0, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
            if (haveSecond) {
                if (!applyChangedByteMask(rr, value, byteWidth, static_cast<u8>((packedMask >> 4) & 0x0f), error, "WDAT nibble-mask record")) return false;
                if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime1, windowStart, windowEnd,
                                              outputSignals, samplesByOutputIndex, error)) return false;
            } else if ((packedMask & 0xf0u) != 0) {
                error = QStringLiteral("WVZ4 WDAT nibble-mask record has dangling high mask");
                return false;
            }
        }
        return true;
    }
    case ValueRecordCodec::NibbleMaskStride: {
        if (transitionCount == 0) return true;
        u64 firstRel = 0, stride = 0;
        if (!rr.readVarUInt(firstRel) || !rr.readVarUInt(stride)) {
            error = QStringLiteral("WVZ4 WDAT nibble-mask-stride record time header malformed");
            return false;
        }
        qint64 sampleTime = 0;
        if (!resolveStrideRecordTime(firstRel, stride, 0, blockStart, blockEnd, sampleTime, error, "WDAT nibble-mask-stride record")) return false;
        if (!readFullValue(rr, value, byteWidth, error, "WDAT nibble-mask-stride record")) return false;
        if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                      outputSignals, samplesByOutputIndex, error)) return false;
        for (u64 ti = 1; ti < transitionCount; ti += 2) {
            u8 packedMask = 0;
            if (!rr.readU8(packedMask)) {
                error = QStringLiteral("WVZ4 WDAT nibble-mask-stride record mask truncated");
                return false;
            }
            qint64 sampleTime0 = 0;
            if (!resolveStrideRecordTime(firstRel, stride, ti, blockStart, blockEnd, sampleTime0, error, "WDAT nibble-mask-stride record")) return false;
            if (!applyChangedByteMask(rr, value, byteWidth, static_cast<u8>(packedMask & 0x0f), error, "WDAT nibble-mask-stride record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime0, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, error)) return false;
            if (ti + 1 < transitionCount) {
                qint64 sampleTime1 = 0;
                if (!resolveStrideRecordTime(firstRel, stride, ti + 1, blockStart, blockEnd, sampleTime1, error, "WDAT nibble-mask-stride record")) return false;
                if (!applyChangedByteMask(rr, value, byteWidth, static_cast<u8>((packedMask >> 4) & 0x0f), error, "WDAT nibble-mask-stride record")) return false;
                if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime1, windowStart, windowEnd,
                                              outputSignals, samplesByOutputIndex, error)) return false;
            } else if ((packedMask & 0xf0u) != 0) {
                error = QStringLiteral("WVZ4 WDAT nibble-mask-stride record has dangling high mask");
                return false;
            }
        }
        return true;
    }
    default:
        error = QStringLiteral("WVZ4 WDAT unsupported value record codec %1").arg(int(codecByte));
        return false;
    }
}


bool signalRangeIntersectsSelection(const QSet<int>& selectedIds,
                                    bool allSelected,
                                    int firstSignalId,
                                    int signalCount) {
    if (signalCount <= 0) return false;
    if (allSelected) return true;
    if (selectedIds.isEmpty()) return false;
    const int lastSignalId = firstSignalId + signalCount - 1;
    if (selectedIds.size() <= signalCount) {
        for (QSet<int>::const_iterator it = selectedIds.constBegin(); it != selectedIds.constEnd(); ++it) {
            const int sid = *it;
            if (sid >= firstSignalId && sid <= lastSignalId) return true;
        }
        return false;
    }
    for (int local = 0; local < signalCount; ++local) {
        if (selectedIds.contains(firstSignalId + local)) return true;
    }
    return false;
}

QVector<int> selectedSignalIdsInRange(const QSet<int>& selectedIds,
                                      bool allSelected,
                                      int firstSignalId,
                                      int signalCount) {
    QVector<int> out;
    if (signalCount <= 0) return out;
    const int lastSignalId = firstSignalId + signalCount - 1;
    if (allSelected) {
        out.reserve(signalCount);
        for (int sid = firstSignalId; sid <= lastSignalId; ++sid) out.push_back(sid);
        return out;
    }
    if (selectedIds.isEmpty()) return out;
    out.reserve(qMin(selectedIds.size(), signalCount));
    if (selectedIds.size() <= signalCount) {
        for (QSet<int>::const_iterator it = selectedIds.constBegin(); it != selectedIds.constEnd(); ++it) {
            const int sid = *it;
            if (sid >= firstSignalId && sid <= lastSignalId) out.push_back(sid);
        }
        return out;
    }
    for (int local = 0; local < signalCount; ++local) {
        const int sid = firstSignalId + local;
        if (selectedIds.contains(sid)) out.push_back(sid);
    }
    return out;
}
bool decodeRawWaveTileV3(const QByteArray& rawPayload,
                         u32 formatVersion,
                         u64 expectedBlockId,
                         i64 expectedStart,
                         i64 expectedEnd,
                         u64 expectedSignalChunkId,
                         u64 expectedFirstSignalId,
                         u64 expectedSignalCount,
                         const QSet<int>& selectedIds,
                         bool allSelected,
                         const QVector<QVector<int>>& outputIndexesByStorageId,
                         const QVector<int>& byteWidthBySignalId,
                         const QVector<WaveSignal>& outputSignals,
                         QVector<QVector<WaveSample>>& samplesByOutputIndex,
                         qint64 windowStart,
                         qint64 windowEnd,
                         qint64& minTime,
                         qint64& maxTime,
                         QString& error) {
    SpanReader r(rawPayload);

    u64 blockId = 0;
    i64 blockStart = 0;
    i64 blockEnd = 0;
    u64 flags = 0;
    if (!r.readVarUInt(blockId) ||
        !r.readI64(blockStart) ||
        !r.readI64(blockEnd) ||
        !r.readVarUInt(flags)) {
        error = QStringLiteral("WVZ4 WDAT v3 tile header malformed");
        return false;
    }
    if (!validateRawBlockHeader(blockId, blockStart, blockEnd, expectedBlockId, expectedStart, expectedEnd, error)) return false;

    // Some transitional v3 writers may emit the old one-tile-per-time-block raw layout
    // when signal chunking is disabled.  Keep this fallback so the viewer can open both
    // v2-style and signal-chunked v3 files.
    if ((flags & kWdatSignalChunkTile) == 0) {
        return decodeRawWaveBlockV2(rawPayload, expectedBlockId, expectedStart, expectedEnd,
                                    selectedIds, allSelected, outputIndexesByStorageId, byteWidthBySignalId,
                                    outputSignals, samplesByOutputIndex,
                                    windowStart, windowEnd, minTime, maxTime, error);
    }

    const u64 knownFlags = (formatVersion >= kSupportedVersionV4) ? kKnownWdatV4Flags : kKnownWdatV3Flags;
    if ((flags & ~knownFlags) != 0) {
        error = QStringLiteral("WVZ4 WDAT v3 tile has unknown flags: 0x%1").arg(QString::number(flags, 16));
        return false;
    }
    if ((flags & kWdatFixedValueWidth) == 0) {
        error = QStringLiteral("WVZ4 WDAT v3 tile without fixed value width is not supported");
        return false;
    }

    const bool useDeltaTimes = (flags & kWdatDeltaTimes) != 0;
    const bool useSharedTimeTable = (flags & kWdatSharedTimeTable) != 0;
    const bool useSparseSignalRecords = (flags & kWdatSparseSignalRecords) != 0;
    const bool hasValueCodec = (flags & kWdatPerRecordValueCodec) != 0;
    if (useDeltaTimes && useSharedTimeTable) {
        error = QStringLiteral("WVZ4 WDAT v3 tile cannot combine delta-time and shared-time encodings");
        return false;
    }

    u64 signalChunkId = 0;
    u64 firstSignalId64 = 0;
    u64 signalCount64 = 0;
    if (!r.readVarUInt(signalChunkId) ||
        !r.readVarUInt(firstSignalId64) || firstSignalId64 == 0 || firstSignalId64 > u64(std::numeric_limits<int>::max()) ||
        !r.readVarUInt(signalCount64) || signalCount64 == 0 || signalCount64 > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 WDAT v3 tile signal chunk header malformed");
        return false;
    }
    if (signalChunkId != expectedSignalChunkId ||
        firstSignalId64 != expectedFirstSignalId ||
        signalCount64 != expectedSignalCount) {
        error = QStringLiteral("WVZ4 WDAT v3 raw tile header does not match outer WDAT chunk header");
        return false;
    }

    if (firstSignalId64 > u64(std::numeric_limits<int>::max()) - signalCount64 + 1ull) {
        error = QStringLiteral("WVZ4 WDAT v3 tile signal range overflows");
        return false;
    }
    const int firstSignalId = int(firstSignalId64);
    const int signalCount = int(signalCount64);
    const int lastSignalId = firstSignalId + signalCount - 1;

    minTime = qMin(minTime, blockStart);
    maxTime = qMax(maxTime, blockEnd);

    QVector<u64> sharedTimes;
    if (useSharedTimeTable) {
        u64 sharedCount = 0;
        if (!r.readVarUInt(sharedCount) || sharedCount > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 WDAT v3 shared time table count is invalid");
            return false;
        }
        if (sharedCount > u64(r.remaining())) {
            error = QStringLiteral("WVZ4 WDAT v3 shared time table count exceeds payload size");
            return false;
        }
        sharedTimes.reserve(int(sharedCount));
        u64 prev = 0;
        for (u64 i = 0; i < sharedCount; ++i) {
            u64 delta = 0;
            if (!r.readVarUInt(delta) || delta > std::numeric_limits<u64>::max() - prev) {
                error = QStringLiteral("WVZ4 WDAT v3 shared time table is malformed");
                return false;
            }
            const u64 rel = prev + delta;
            qint64 ignoredTime = 0;
            if (!addRelTimeChecked(blockStart, blockEnd, rel, ignoredTime)) {
                error = QStringLiteral("WVZ4 WDAT v3 shared time is outside the block or overflows");
                return false;
            }
            sharedTimes.push_back(rel);
            prev = rel;
        }
    }

    QVector<u64> offsets;
    QVector<int> sparseSignalIds;
    QVector<u64> sparseRecordSizes;
    if (useSparseSignalRecords) {
        u64 activeCount64 = 0;
        if (!r.readVarUInt(activeCount64) || activeCount64 > signalCount64 ||
            activeCount64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 WDAT sparse tile active record count is invalid");
            return false;
        }
        if (activeCount64 > u64(r.remaining())) {
            error = QStringLiteral("WVZ4 WDAT sparse tile active table count exceeds payload size");
            return false;
        }
        sparseSignalIds.reserve(int(activeCount64));
        sparseRecordSizes.reserve(int(activeCount64));
        u64 prevLocal = 0;
        for (u64 i = 0; i < activeCount64; ++i) {
            u64 localDelta = 0;
            u64 recordSize = 0;
            if (!r.readVarUInt(localDelta) || localDelta > std::numeric_limits<u64>::max() - prevLocal ||
                !r.readVarUInt(recordSize) || recordSize > u64(std::numeric_limits<int>::max())) {
                error = QStringLiteral("WVZ4 WDAT sparse tile active table is malformed");
                return false;
            }
            const u64 local = prevLocal + localDelta;
            if (local >= signalCount64) {
                error = QStringLiteral("WVZ4 WDAT sparse tile local signal id is out of range");
                return false;
            }
            sparseSignalIds.push_back(firstSignalId + int(local));
            sparseRecordSizes.push_back(recordSize);
            prevLocal = local;
        }
    } else {
        u64 offsetCount64 = 0;
        if (!r.readVarUInt(offsetCount64) || offsetCount64 != signalCount64 + 1ull ||
            offsetCount64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 WDAT v3 tile offset table count is invalid");
            return false;
        }
        // Every delta-coded offset consumes at least one byte.  Reject impossible
        // counts before reserve(), otherwise a corrupt tile can force a huge allocation.
        if (offsetCount64 > u64(r.remaining())) {
            error = QStringLiteral("WVZ4 WDAT v3 tile offset table count exceeds payload size");
            return false;
        }

        offsets.reserve(int(offsetCount64));
        u64 prevOffset = 0;
        for (u64 i = 0; i < offsetCount64; ++i) {
            u64 delta = 0;
            if (!r.readVarUInt(delta) || delta > std::numeric_limits<u64>::max() - prevOffset) {
                error = QStringLiteral("WVZ4 WDAT v3 tile offset table is malformed");
                return false;
            }
            const u64 off = prevOffset + delta;
            if (off > u64(std::numeric_limits<int>::max())) {
                error = QStringLiteral("WVZ4 WDAT v3 tile record offset is too large");
                return false;
            }
            offsets.push_back(off);
            prevOffset = off;
        }
    }

    u64 recordsBlobSize64 = 0;
    if (!r.readVarUInt(recordsBlobSize64) || recordsBlobSize64 > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 WDAT v3 tile records_blob_size is invalid");
        return false;
    }
    if (recordsBlobSize64 > u64(r.remaining())) {
        error = QStringLiteral("WVZ4 WDAT v3 tile records_blob_size exceeds payload remainder");
        return false;
    }
    if (!useSparseSignalRecords && (offsets.isEmpty() || offsets.first() != 0 || offsets.last() != recordsBlobSize64)) {
        error = QStringLiteral("WVZ4 WDAT v3 tile offset table must start at 0 and end at records_blob_size");
        return false;
    }
    if (useSparseSignalRecords) {
        u64 totalRecordBytes = 0;
        for (int i = 0; i < sparseRecordSizes.size(); ++i) {
            if (sparseRecordSizes.at(i) > recordsBlobSize64 ||
                totalRecordBytes > recordsBlobSize64 - sparseRecordSizes.at(i)) {
                error = QStringLiteral("WVZ4 WDAT sparse tile active record sizes exceed records_blob_size");
                return false;
            }
            totalRecordBytes += sparseRecordSizes.at(i);
        }
        if (totalRecordBytes != recordsBlobSize64) {
            error = QStringLiteral("WVZ4 WDAT sparse tile active record sizes do not match records_blob_size");
            return false;
        }
    }

    const char* recordsBlob = nullptr;
    if (!r.readBytes(recordsBlob, int(recordsBlobSize64))) {
        error = QStringLiteral("WVZ4 WDAT v3 tile records_blob truncated");
        return false;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 WDAT v3 tile raw payload has trailing bytes");
        return false;
    }

    if (useSparseSignalRecords) {
        u64 offset = 0;
        for (int i = 0; i < sparseSignalIds.size(); ++i) {
            const int sid = sparseSignalIds.at(i);
            const u64 recordSize = sparseRecordSizes.at(i);
            const u64 beginOff64 = offset;
            const u64 endOff64 = offset + recordSize;
            offset = endOff64;
            if (!allSelected && !selectedIds.contains(sid)) continue;

            const int byteWidth = directIntMapValue(byteWidthBySignalId, sid, -1);
            if (byteWidth <= 0 || byteWidth > 8) {
                error = QStringLiteral("WVZ4 WDAT sparse tile references unknown storage_id %1").arg(sid);
                return false;
            }
            const QVector<int>* outputIndexes = directIntListMapValue(outputIndexesByStorageId, sid);
            if (!outputIndexes || outputIndexes->isEmpty()) continue;

            SpanReader rr(recordsBlob + int(beginOff64), int(endOff64 - beginOff64));
            if (!decodeSignalRecord(rr, hasValueCodec, useDeltaTimes, useSharedTimeTable, sharedTimes,
                                    blockStart, blockEnd, byteWidth, *outputIndexes,
                                    outputSignals, samplesByOutputIndex, windowStart, windowEnd, error)) {
                return false;
            }
            if (!rr.eof()) {
                error = QStringLiteral("WVZ4 WDAT sparse signal record has trailing bytes");
                return false;
            }
        }
        return true;
    }

    const QVector<int> selectedSidsInTile = selectedSignalIdsInRange(selectedIds, allSelected, firstSignalId, signalCount);
    if (selectedSidsInTile.isEmpty()) return true;

    for (int _i = 0; _i < selectedSidsInTile.size(); ++_i) {
        const int sid = selectedSidsInTile.at(_i);
        const int local = sid - firstSignalId;
        const u64 beginOff64 = offsets.at(local);
        const u64 endOff64 = offsets.at(local + 1);
        if (endOff64 < beginOff64 || endOff64 > recordsBlobSize64) {
            error = QStringLiteral("WVZ4 WDAT v3 tile record offset range is invalid");
            return false;
        }
        if (endOff64 == beginOff64) continue;

        const int byteWidth = directIntMapValue(byteWidthBySignalId, sid, -1);
        if (byteWidth <= 0 || byteWidth > 8) {
            error = QStringLiteral("WVZ4 WDAT v3 tile references unknown signal_id %1").arg(sid);
            return false;
        }
        const QVector<int>* outputIndexes = directIntListMapValue(outputIndexesByStorageId, sid);
        if (!outputIndexes || outputIndexes->isEmpty()) continue;

        SpanReader rr(recordsBlob + int(beginOff64), int(endOff64 - beginOff64));
        if (!decodeSignalRecord(rr, hasValueCodec, useDeltaTimes, useSharedTimeTable, sharedTimes,
                                blockStart, blockEnd, byteWidth, *outputIndexes,
                                outputSignals, samplesByOutputIndex, windowStart, windowEnd, error)) {
            return false;
        }

        if (!rr.eof()) {
            error = QStringLiteral("WVZ4 WDAT v3 signal record has trailing bytes");
            return false;
        }
    }

    return true;
}



bool readU8FromSection(QFile& file, qint64 sectionEnd, u8& out, QString& error, const char* context) {
    if (file.pos() >= sectionEnd) {
        error = QStringLiteral("WVZ4 %1 truncated while reading u8").arg(QString::fromLatin1(context));
        return false;
    }
    char ch = 0;
    if (!file.getChar(&ch)) {
        error = QStringLiteral("WVZ4 %1 failed to read u8").arg(QString::fromLatin1(context));
        return false;
    }
    out = static_cast<u8>(ch);
    return true;
}

bool readI64FromSection(QFile& file, qint64 sectionEnd, i64& out, QString& error, const char* context) {
    if (sectionEnd - file.pos() < 8) {
        error = QStringLiteral("WVZ4 %1 truncated while reading i64").arg(QString::fromLatin1(context));
        return false;
    }
    const QByteArray bytes = file.read(8);
    if (bytes.size() != 8) {
        error = QStringLiteral("WVZ4 %1 failed to read i64").arg(QString::fromLatin1(context));
        return false;
    }
    out = readI64LE(bytes.constData());
    return true;
}

bool readVarUIntFromSection(QFile& file, qint64 sectionEnd, u64& out, QString& error, const char* context) {
    out = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        u8 byte = 0;
        if (!readU8FromSection(file, sectionEnd, byte, error, context)) return false;
        if (i == 9) {
            if ((byte & 0x80u) != 0 || (byte & 0x7eu) != 0) {
                error = QStringLiteral("WVZ4 %1 has malformed varuint").arg(QString::fromLatin1(context));
                return false;
            }
            out |= (u64(byte & 0x01u) << 63);
            return true;
        }
        out |= (u64(byte & 0x7fu) << shift);
        if ((byte & 0x80u) == 0) return true;
        shift += 7;
    }
    error = QStringLiteral("WVZ4 %1 has malformed varuint").arg(QString::fromLatin1(context));
    return false;
}

bool decodeWdatSectionStreaming(QFile& file,
                                const SectionHeader& section,
                                u32 formatVersion,
                                const QSet<int>& selectedIds,
                                bool allSelected,
                                const QVector<QVector<int>>& outputIndexesByStorageId,
                                const QVector<int>& byteWidthBySignalId,
                                const QVector<WaveSignal>& outputSignals,
                                QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                qint64 windowStart,
                                qint64 windowEnd,
                                qint64& minTime,
                                qint64& maxTime,
                                QString& error,
                                const BlockIndexRec* expectedIndex = nullptr) {
    const qint64 sectionEnd = section.payloadOffset + qint64(section.size);
    if (sectionEnd < section.payloadOffset || sectionEnd > file.size()) {
        error = QStringLiteral("WVZ4 WDAT section range is invalid");
        return false;
    }
    if (file.pos() != section.payloadOffset && !file.seek(section.payloadOffset)) {
        error = QStringLiteral("WVZ4 failed to seek to WDAT payload");
        return false;
    }

    u64 blockId = 0;
    i64 start = 0, end = 0;
    u64 signalChunkId = 0;
    u64 firstSignalId = 1;
    u64 signalCount = 0;
    u8 compByte = 0;
    u64 rawSize = 0, encodedSize = 0;
    if (!readVarUIntFromSection(file, sectionEnd, blockId, error, "WDAT outer header") ||
        !readI64FromSection(file, sectionEnd, start, error, "WDAT outer header") ||
        !readI64FromSection(file, sectionEnd, end, error, "WDAT outer header")) {
        return false;
    }

    if (formatVersion >= kSupportedVersionV3) {
        if (!readVarUIntFromSection(file, sectionEnd, signalChunkId, error, "WDAT v3 outer chunk header") ||
            !readVarUIntFromSection(file, sectionEnd, firstSignalId, error, "WDAT v3 outer chunk header") ||
            firstSignalId == 0 || firstSignalId > u64(std::numeric_limits<int>::max()) ||
            !readVarUIntFromSection(file, sectionEnd, signalCount, error, "WDAT v3 outer chunk header") ||
            signalCount == 0 || signalCount > u64(std::numeric_limits<int>::max())) {
            if (error.isEmpty()) error = QStringLiteral("WVZ4 WDAT v3 outer signal chunk header malformed");
            return false;
        }
        if (firstSignalId > u64(std::numeric_limits<int>::max()) - signalCount + 1ull) {
            error = QStringLiteral("WVZ4 WDAT v3 outer signal chunk range overflows");
            return false;
        }
    }

    if (!readU8FromSection(file, sectionEnd, compByte, error, "WDAT outer header") ||
        !readVarUIntFromSection(file, sectionEnd, rawSize, error, "WDAT outer header") ||
        !readVarUIntFromSection(file, sectionEnd, encodedSize, error, "WDAT outer header")) {
        if (error.isEmpty()) error = QStringLiteral("WVZ4 WDAT section header malformed");
        return false;
    }
    if (!validBlockTimeRange(start, end)) {
        error = QStringLiteral("WVZ4 WDAT section has invalid block time range");
        return false;
    }
    if (!isValidCompression(Compression(compByte))) {
        error = QStringLiteral("WVZ4 WDAT unsupported compression value %1").arg(int(compByte));
        return false;
    }
    if (expectedIndex) {
        if (blockId != expectedIndex->blockId || start != expectedIndex->start || end != expectedIndex->end) {
            error = QStringLiteral("WVZ4 FOOT/WDAT block metadata mismatch for block %1").arg(expectedIndex->blockId);
            return false;
        }
        if (formatVersion >= kSupportedVersionV3 &&
            (signalChunkId != expectedIndex->signalChunkId ||
             firstSignalId != expectedIndex->firstSignalId ||
             signalCount != expectedIndex->signalCount)) {
            error = QStringLiteral("WVZ4 FOOT/WDAT signal chunk metadata mismatch for block %1").arg(expectedIndex->blockId);
            return false;
        }
        if (Compression(compByte) != expectedIndex->compression || rawSize != expectedIndex->rawSize) {
            error = QStringLiteral("WVZ4 FOOT/WDAT compression metadata mismatch for block %1").arg(expectedIndex->blockId);
            return false;
        }
    }

    const qint64 encodedOffset = file.pos();
    const qint64 encodedRemaining = sectionEnd - encodedOffset;
    if (encodedRemaining < 0 || encodedSize > u64(encodedRemaining)) {
        error = QStringLiteral("WVZ4 WDAT encoded payload truncated");
        return false;
    }
    if (encodedSize != u64(encodedRemaining)) {
        error = QStringLiteral("WVZ4 WDAT outer payload has trailing bytes");
        return false;
    }

    minTime = qMin(minTime, start);
    maxTime = qMax(maxTime, end);

    bool needDecode = blockOverlapsWindow(start, end, windowStart, windowEnd);
    if (needDecode && formatVersion >= kSupportedVersionV3) {
        needDecode = signalRangeIntersectsSelection(selectedIds, allSelected, int(firstSignalId), int(signalCount));
    }
    if (!needDecode) {
        if (!file.seek(sectionEnd)) {
            error = QStringLiteral("WVZ4 failed to skip unneeded WDAT tile");
            return false;
        }
        return true;
    }

    if (encodedSize > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 WDAT encoded payload too large to load");
        return false;
    }

    const QByteArray encoded = file.read(qint64(encodedSize));
    if (encoded.size() != int(encodedSize)) {
        error = QStringLiteral("WVZ4 WDAT encoded payload truncated");
        return false;
    }
    if (file.pos() != sectionEnd) {
        error = QStringLiteral("WVZ4 WDAT outer payload has trailing bytes");
        return false;
    }

    const QByteArray raw = decompressBlockPayload(encoded, Compression(compByte), rawSize, error);
    if (!error.isEmpty()) return false;

    if (formatVersion == kSupportedVersionV1) {
        return decodeRawWaveBlockV1(raw, blockId, start, end, selectedIds, allSelected, outputIndexesByStorageId,
                                    outputSignals, samplesByOutputIndex,
                                    windowStart, windowEnd, minTime, maxTime, error);
    }
    if (formatVersion == kSupportedVersionV2) {
        return decodeRawWaveBlockV2(raw, blockId, start, end, selectedIds, allSelected, outputIndexesByStorageId,
                                    byteWidthBySignalId, outputSignals, samplesByOutputIndex,
                                    windowStart, windowEnd, minTime, maxTime, error);
    }
    if (formatVersion >= kSupportedVersionV3 && formatVersion <= kSupportedVersionV9) {
        return decodeRawWaveTileV3(raw, formatVersion, blockId, start, end,
                                   signalChunkId, firstSignalId, signalCount,
                                   selectedIds, allSelected, outputIndexesByStorageId,
                                   byteWidthBySignalId, outputSignals, samplesByOutputIndex,
                                   windowStart, windowEnd, minTime, maxTime, error);
    }

    error = QStringLiteral("不支持的 WVZ4 版本：%1").arg(formatVersion);
    return false;
}


bool decodeWdatSectionsFromFooterIndex(QFile& file,
                                        const QVector<BlockIndexRec>& footerBlocks,
                                        const QVector<QVector<int>>& blockIndexesByChunk,
                                        u32 formatVersion,
                                        u64 signalsPerChunk,
                                        const QSet<int>& selectedIds,
                                        bool allSelected,
                                        const QVector<QVector<int>>& outputIndexesByStorageId,
                                        const QVector<int>& byteWidthBySignalId,
                                        const QVector<WaveSignal>& outputSignals,
                                        QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                        qint64 windowStart,
                                        qint64 windowEnd,
                                        qint64& minTime,
                                        qint64& maxTime,
                                        QString& error) {
    if (selectedIds.isEmpty() && !allSelected) return true;

    QVector<int> selectedBlockIndexes;
    const bool canUseChunkMap = !allSelected && !selectedIds.isEmpty() &&
                                signalsPerChunk > 0 &&
                                !blockIndexesByChunk.isEmpty();
    if (canUseChunkMap) {
        QSet<int> seenChunks;
        for (int sid : selectedIds) {
            if (sid <= 0) continue;
            const u64 chunk64 = (u64(sid) - 1ull) / signalsPerChunk;
            if (chunk64 > u64(std::numeric_limits<int>::max())) continue;
            const int chunk = int(chunk64);
            if (seenChunks.contains(chunk)) continue;
            seenChunks.insert(chunk);
            if (chunk >= 0 && chunk < blockIndexesByChunk.size()) {
                const QVector<int>& indexes = blockIndexesByChunk.at(chunk);
                selectedBlockIndexes.reserve(selectedBlockIndexes.size() + indexes.size());
                for (int idx : indexes) selectedBlockIndexes.push_back(idx);
            }
        }
        std::sort(selectedBlockIndexes.begin(), selectedBlockIndexes.end());
        selectedBlockIndexes.erase(std::unique(selectedBlockIndexes.begin(), selectedBlockIndexes.end()),
                                   selectedBlockIndexes.end());
    }

    const int loopCount = canUseChunkMap ? selectedBlockIndexes.size() : footerBlocks.size();
    for (int loopIndex = 0; loopIndex < loopCount; ++loopIndex) {
        const int i = canUseChunkMap ? selectedBlockIndexes.at(loopIndex) : loopIndex;
        if (i < 0 || i >= footerBlocks.size()) {
            error = QStringLiteral("WVZ4 FOOT v6 chunk index points outside block table");
            return false;
        }
        const BlockIndexRec& b = footerBlocks.at(i);
        if (!blockOverlapsWindow(b.start, b.end, windowStart, windowEnd)) continue;
        if (formatVersion >= kSupportedVersionV3) {
            if (b.firstSignalId == 0 || b.signalCount == 0 ||
                b.firstSignalId > u64(std::numeric_limits<int>::max()) ||
                b.signalCount > u64(std::numeric_limits<int>::max()) ||
                b.firstSignalId > u64(std::numeric_limits<int>::max()) - b.signalCount + 1ull) {
                error = QStringLiteral("WVZ4 FOOT block %1 has invalid signal chunk range").arg(b.blockId);
                return false;
            }
            if (!signalRangeIntersectsSelection(selectedIds, allSelected, int(b.firstSignalId), int(b.signalCount))) continue;
        }

        if (b.fileOffset > u64(std::numeric_limits<qint64>::max())) {
            error = QStringLiteral("WVZ4 FOOT block %1 file offset exceeds qint64 range").arg(b.blockId);
            return false;
        }
        if (!file.seek(qint64(b.fileOffset))) {
            error = QStringLiteral("WVZ4 failed to seek to WDAT block %1 from FOOT index").arg(b.blockId);
            return false;
        }

        SectionHeader sh;
        if (!readSectionHeader(file, sh, error)) {
            if (error.isEmpty()) error = QStringLiteral("WVZ4 FOOT block %1 points past end of file").arg(b.blockId);
            return false;
        }
        if (sh.tag != "WDAT") {
            error = QStringLiteral("WVZ4 FOOT block %1 does not point to a WDAT section").arg(b.blockId);
            return false;
        }
        const qint64 sectionStart = sh.payloadOffset - 12;
        if (sectionStart < 0 || u64(sectionStart) != b.fileOffset) {
            error = QStringLiteral("WVZ4 FOOT block %1 file offset mismatch").arg(b.blockId);
            return false;
        }
        if (sh.size > u64(std::numeric_limits<qint64>::max() - 12) || b.fileSize != sh.size + 12ull) {
            error = QStringLiteral("WVZ4 FOOT block %1 file size does not match WDAT section size").arg(b.blockId);
            return false;
        }

        if (!decodeWdatSectionStreaming(file, sh, formatVersion, selectedIds, allSelected,
                                        outputIndexesByStorageId, byteWidthBySignalId,
                                        outputSignals, samplesByOutputIndex,
                                        windowStart, windowEnd, minTime, maxTime, error, &b)) {
            return false;
        }
    }
    return true;
}

bool decodeWdatSection(const QByteArray& payload,
                       u32 formatVersion,
                       const QSet<int>& selectedIds,
                       bool allSelected,
                       const QVector<QVector<int>>& outputIndexesByStorageId,
                       const QVector<int>& byteWidthBySignalId,
                       const QVector<WaveSignal>& outputSignals,
                       QVector<QVector<WaveSample>>& samplesByOutputIndex,
                       qint64 windowStart,
                       qint64 windowEnd,
                       qint64& minTime,
                       qint64& maxTime,
                       QString& error) {
    SpanReader r(payload);
    u64 blockId = 0;
    i64 start = 0, end = 0;
    u64 signalChunkId = 0;
    u64 firstSignalId = 1;
    u64 signalCount = 0;
    u8 compByte = 0;
    u64 rawSize = 0, encodedSize = 0;
    if (!r.readVarUInt(blockId) ||
        !r.readI64(start) ||
        !r.readI64(end)) {
        error = QStringLiteral("WVZ4 WDAT section header malformed");
        return false;
    }

    if (formatVersion >= kSupportedVersionV3) {
        if (!r.readVarUInt(signalChunkId) ||
            !r.readVarUInt(firstSignalId) || firstSignalId == 0 || firstSignalId > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(signalCount) || signalCount == 0 || signalCount > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 WDAT v3 outer signal chunk header malformed");
            return false;
        }
        if (firstSignalId > u64(std::numeric_limits<int>::max()) - signalCount + 1ull) {
            error = QStringLiteral("WVZ4 WDAT v3 outer signal chunk range overflows");
            return false;
        }
    }

    if (!r.readU8(compByte) ||
        !r.readVarUInt(rawSize) ||
        !r.readVarUInt(encodedSize) ||
        encodedSize > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 WDAT section header malformed");
        return false;
    }
    if (!validBlockTimeRange(start, end)) {
        error = QStringLiteral("WVZ4 WDAT section has invalid block time range");
        return false;
    }
    if (!isValidCompression(Compression(compByte))) {
        error = QStringLiteral("WVZ4 WDAT unsupported compression value %1").arg(int(compByte));
        return false;
    }

    const char* encodedPtr = nullptr;
    if (!r.readBytes(encodedPtr, int(encodedSize))) {
        error = QStringLiteral("WVZ4 WDAT encoded payload truncated");
        return false;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 WDAT outer payload has trailing bytes");
        return false;
    }

    minTime = qMin(minTime, start);
    maxTime = qMax(maxTime, end);

    if (!blockOverlapsWindow(start, end, windowStart, windowEnd)) {
        return true;
    }
    if (formatVersion >= kSupportedVersionV3) {
        const int first = int(firstSignalId);
        const int count = int(signalCount);
        if (!signalRangeIntersectsSelection(selectedIds, allSelected, first, count)) {
            return true;
        }
    }

    const QByteArray encoded = QByteArray::fromRawData(encodedPtr, int(encodedSize));
    const QByteArray raw = decompressBlockPayload(encoded, Compression(compByte), rawSize, error);
    if (!error.isEmpty()) return false;

    if (formatVersion == kSupportedVersionV1) {
        return decodeRawWaveBlockV1(raw, blockId, start, end, selectedIds, allSelected, outputIndexesByStorageId,
                                    outputSignals, samplesByOutputIndex,
                                    windowStart, windowEnd, minTime, maxTime, error);
    }
    if (formatVersion == kSupportedVersionV2) {
        return decodeRawWaveBlockV2(raw, blockId, start, end, selectedIds, allSelected, outputIndexesByStorageId,
                                    byteWidthBySignalId, outputSignals, samplesByOutputIndex,
                                    windowStart, windowEnd, minTime, maxTime, error);
    }
    if (formatVersion >= kSupportedVersionV3 && formatVersion <= kSupportedVersionV9) {
        return decodeRawWaveTileV3(raw, formatVersion, blockId, start, end,
                                   signalChunkId, firstSignalId, signalCount,
                                   selectedIds, allSelected, outputIndexesByStorageId,
                                   byteWidthBySignalId, outputSignals, samplesByOutputIndex,
                                   windowStart, windowEnd, minTime, maxTime, error);
    }

    error = QStringLiteral("不支持的 WVZ4 版本：%1").arg(formatVersion);
    return false;
}


} // namespace

bool WaveParser4::loadFromFile(const QString& filePath,
                               WaveFile& outWave,
                               QString& error,
                               const LoadOptions& options) {
    error.clear();
    outWave = WaveFile();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法打开 WVZ4 文件：%1").arg(filePath);
        return false;
    }

    const QByteArray header = file.read(64);
    if (header.size() != 64) {
        error = QStringLiteral("WVZ4 文件头不足 64 字节");
        return false;
    }
    if (std::memcmp(header.constData(), "WVZ4\r\n\0\0", 8) != 0) {
        error = QStringLiteral("不是有效 WVZ4 文件：magic 不匹配");
        return false;
    }

    const u32 version = readU32LE(header.constData() + 8);
    const u32 headerSize = readU32LE(header.constData() + 12);
    const u64 blockSpan = readU64LE(header.constData() + 16);
    const u64 footerOffset = readU64LE(header.constData() + 24);
    const u64 headerSignalsPerChunk = readU64LE(header.constData() + 32);
    const u64 headerFeatureFlags = readU64LE(header.constData() + 40);
    Q_UNUSED(blockSpan);
    Q_UNUSED(headerFeatureFlags);

    // On-demand WVZ4 sample load only needs SIGT plus WDAT.  NAME/NODE/NAMZ/NODZ
    // and full child-chain validation were previously repeated on every click,
    // which made selecting a signal appear stuck on large metadata-heavy files.
    const bool sampleOnlyLoad = !options.includeAllSignalDefinitions && !options.signalIds.isEmpty();

    if (version != kSupportedVersionV1 && version != kSupportedVersionV2 &&
        version != kSupportedVersionV3 && version != kSupportedVersionV4 &&
        version != kSupportedVersionV5 && version != kSupportedVersionV6 &&
        version != kSupportedVersionV7 && version != kSupportedVersionV8 &&
        version != kSupportedVersionV9) {
        error = QStringLiteral("不支持的 WVZ4 版本：%1").arg(version);
        return false;
    }
    if (headerSize < 64 || headerSize > u32(file.size())) {
        error = QStringLiteral("WVZ4 header_size 无效：%1").arg(headerSize);
        return false;
    }
    if (footerOffset != 0 && footerOffset > u64(file.size())) {
        error = QStringLiteral("WVZ4 footer_offset 超出文件大小");
        return false;
    }
    if (!file.seek(qint64(headerSize))) {
        error = QStringLiteral("WVZ4 seek 到 header_size 失败");
        return false;
    }

    QVector<QString> namesById;
    QVector<NodeRec> nodesById;
    QVector<SigRec> sigs;
    QVector<ClockRec> clocks;
    QVector<BlockIndexRec> footerBlocks;
    QVector<QVector<int>> footerBlockIndexesByChunk;
    QVector<QVector<WaveLodLevel>> footerLodLevelsByStorageId;

    QSet<int> selectedIds;
    QSet<int> selectedStorageIds;
    QVector<WaveSignal> outputSignals;
    QVector<int> outputIndexBySignalId;
    QVector<QVector<int>> outputIndexesByStorageId;
    QVector<int> byteWidthByStorageId;
    QVector<int> boolStorageByStorageId;
    QVector<int> storageIdBySignalId;
    QVector<QVector<WaveSample>> samplesByOutputIndex;
    bool outputInitialized = false;
    bool useFooterIndexedWdat = false;
    bool allSelectedSignalIds = false;
    bool allSelectedStorageIds = false;

    qint64 minTime = std::numeric_limits<qint64>::max();
    qint64 maxTime = 0;

    auto initializeOutput = [&]() -> bool {
        if (outputInitialized) return true;

        if (!sampleOnlyLoad) {
            if (namesById.isEmpty()) {
                error = QStringLiteral("WVZ4 缺少 NAME section");
                return false;
            }
            if (nodesById.isEmpty()) {
                error = QStringLiteral("WVZ4 缺少 NODE section");
                return false;
            }
        }
        if (sigs.isEmpty()) {
            error = QStringLiteral("WVZ4 缺少 SIGT section 或信号为空");
            return false;
        }
        if (!sampleOnlyLoad && !validateNodeAndSignalLayout(nodesById, namesById, sigs, error)) {
            return false;
        }

        allSelectedSignalIds = false;
        allSelectedStorageIds = false;
        selectedIds.clear();
        selectedStorageIds.clear();
        if (!options.signalIds.isEmpty()) {
            for (int sid : options.signalIds) {
                if (sid > 0) selectedIds.insert(sid);
            }
        } else if (options.autoLoadFirstSignalCount >= 0) {
            const int n = qMin(options.autoLoadFirstSignalCount, sigs.size());
            for (int i = 0; i < n; ++i) selectedIds.insert(int(sigs.at(i).signalId));
        } else if (options.loadAllIfWindowEmpty) {
            allSelectedSignalIds = true;
            allSelectedStorageIds = true;
            // All signals are selected logically; do not materialize every id into
            // QSet.  Large compare/open operations otherwise spend substantial time
            // hashing dense signal ids that are already covered by allSelectedSignalIds.
        }

        byteWidthByStorageId.clear();
        byteWidthByStorageId.reserve(sigs.size());
        boolStorageByStorageId.clear();
        boolStorageByStorageId.reserve(sigs.size());
        storageIdBySignalId.clear();
        for (int i = 0; i < sigs.size(); ++i) {
            const SigRec& s = sigs.at(i);
            int bytes = 0;
            if (!valueTypeByteWidth(s.type, bytes)) {
                error = QStringLiteral("WVZ4 SIGT has invalid ValueType for signal_id %1").arg(int(s.signalId));
                return false;
            }
            const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
            directIntMapSet(storageIdBySignalId, int(s.signalId), storageId);
            const int oldBytes = directIntMapValue(byteWidthByStorageId, storageId, -1);
            if (oldBytes > 0 && oldBytes != bytes) {
                error = QStringLiteral("WVZ4 SIGT storage_id %1 has incompatible logical aliases").arg(storageId);
                return false;
            }
            directIntMapSet(byteWidthByStorageId, storageId, bytes);
            directIntMapSet(boolStorageByStorageId, storageId,
                            (s.type == ValueType::Bool && s.bitWidth == 1) ? 1 : 0);
        }
        if (!allSelectedStorageIds) {
            for (QSet<int>::const_iterator it = selectedIds.constBegin(); it != selectedIds.constEnd(); ++it) {
                const int storageId = directIntMapValue(storageIdBySignalId, *it, -1);
                if (storageId > 0) selectedStorageIds.insert(storageId);
            }
        }

        auto makeWaveSignal = [&](const SigRec& s, bool selected) -> WaveSignal {
            WaveSignal sig;
            sig.signalId = int(s.signalId);
            sig.storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
            // Keep only the leaf segment here. The complete path is reconstructed
            // from WaveTreeInfo only when the signal is added to the active list or exported.
            // On sample-only reloads MainWindow only consumes signal_id + samples,
            // so NAME/NODE can be skipped completely.
            sig.name = sampleOnlyLoad ? QStringLiteral("signal_%1").arg(int(s.signalId))
                                      : nodeSegmentName(s.nodeId, nodesById, namesById);
            sig.kind = convertKind(s.type, int(s.bitWidth));
            sig.width = qMax(1, int(s.bitWidth));
            sig.defaultRadix = convertRadix(s.radix, s.type, sig.width);
            sig.currentRadix = sig.defaultRadix;
            sig.supportsZState = false;
            sig.samplesLoaded = selected;
            return sig;
        };

        if (options.includeAllSignalDefinitions) {
            outputSignals.reserve(sigs.size());
            outputIndexBySignalId.reserve(sigs.size());
            for (int i = 0; i < sigs.size(); ++i) {
                const SigRec& s = sigs.at(i);
                if (s.nodeId >= u32(nodesById.size()) || !nodesById[int(s.nodeId)].valid) {
                    error = QStringLiteral("WVZ4 SIGT references missing node_id %1").arg(int(s.nodeId));
                    return false;
                }
                const int idx = outputSignals.size();
                outputSignals.push_back(makeWaveSignal(s, allSelectedSignalIds || selectedIds.contains(int(s.signalId))));
                directIntMapSet(outputIndexBySignalId, int(s.signalId), idx);
                if (outputSignals.at(idx).samplesLoaded) {
                    const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
                    directIntListMapAppend(outputIndexesByStorageId, storageId, idx);
                }
            }
        } else {
            outputSignals.reserve(allSelectedSignalIds ? sigs.size() : selectedIds.size());
            outputIndexBySignalId.reserve(allSelectedSignalIds ? sigs.size() : selectedIds.size());
            for (int i = 0; i < sigs.size(); ++i) {
                const SigRec& s = sigs.at(i);
                if (!allSelectedSignalIds && !selectedIds.contains(int(s.signalId))) continue;
                const int idx = outputSignals.size();
                outputSignals.push_back(makeWaveSignal(s, true));
                directIntMapSet(outputIndexBySignalId, int(s.signalId), idx);
                const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
                directIntListMapAppend(outputIndexesByStorageId, storageId, idx);
            }
        }

        samplesByOutputIndex.resize(outputSignals.size());

        if (version >= kSupportedVersionV2 && version <= kSupportedVersionV9) {
            if (!appendImplicitZeroSamplesForSelectedSignals(selectedIds, allSelectedSignalIds,
                                                             outputSignals, samplesByOutputIndex,
                                                             minTime, maxTime, error)) {
                return false;
            }
        }

        if (options.includeAllSignalDefinitions) {
            if (!buildWaveTreeInfo(nodesById, namesById, sigs, outputIndexBySignalId, outWave.tree, error)) {
                return false;
            }
        }

        outputInitialized = true;
        return true;
    };

    // Parse layout sections first; WDAT is streamed and decoded as soon as SIGT
    // has initialized the selected signal set, so large WVZ4 files are not copied
    // into a pending payload list.
    while (!file.atEnd()) {
        SectionHeader sh;
        if (!readSectionHeader(file, sh, error)) {
            if (error.isEmpty()) break;
            return false;
        }

        if (sh.tag == "WDAT") {
            if (!outputInitialized && !initializeOutput()) return false;

            // WVZ4 v3 FOOT is a real random-access tile index.  Use it instead
            // of linearly walking every WDAT tile.  This avoids O(number of all
            // tiles) outer-header scans when only a few signal chunks are loaded.
            if (version >= kSupportedVersionV3 && footerOffset != 0) {
                useFooterIndexedWdat = true;
                if (footerOffset > u64(std::numeric_limits<qint64>::max()) || !file.seek(qint64(footerOffset))) {
                    error = QStringLiteral("WVZ4 failed to seek to FOOT section");
                    return false;
                }

                SectionHeader footerHeader;
                if (!readSectionHeader(file, footerHeader, error)) {
                    if (error.isEmpty()) error = QStringLiteral("WVZ4 footer_offset does not point to a section");
                    return false;
                }
                if (footerHeader.tag != "FOOT") {
                    error = QStringLiteral("WVZ4 footer_offset does not point to the FOOT section");
                    return false;
                }

                QByteArray footerPayload;
                if (!readSectionPayload(file, footerHeader, footerPayload, error)) return false;
                if (!parseFooterSection(footerPayload, version, byteWidthByStorageId, boolStorageByStorageId,
                                        footerBlocks, footerBlockIndexesByChunk,
                                        footerLodLevelsByStorageId, error)) return false;
                const u64 fileSize64 = u64(file.size());
                for (int i = 0; i < footerBlocks.size(); ++i) {
                    const BlockIndexRec& b = footerBlocks.at(i);
                    if (b.fileOffset > fileSize64 || b.fileSize > fileSize64 - b.fileOffset) {
                        error = QStringLiteral("WVZ4 FOOT block %1 file range exceeds file size").arg(b.blockId);
                        return false;
                    }
                }
                break;
            }

            if (selectedStorageIds.isEmpty() && !allSelectedStorageIds) {
                if (!skipSectionPayload(file, sh, error)) return false;
                continue;
            }

            if (!decodeWdatSectionStreaming(file, sh, version, selectedStorageIds, allSelectedStorageIds,
                                            outputIndexesByStorageId, byteWidthByStorageId,
                                            outputSignals, samplesByOutputIndex,
                                            options.timeStart, options.timeEnd,
                                            minTime, maxTime, error)) {
                return false;
            }
            continue;
        }

        if (sampleOnlyLoad &&
            (sh.tag == "NAME" || sh.tag == "NAMZ" || sh.tag == "NODE" || sh.tag == "NODZ")) {
            if (!skipSectionPayload(file, sh, error)) return false;
            continue;
        }

        QByteArray payload;
        if (!readSectionPayload(file, sh, payload, error)) return false;

        if (sh.tag == "NAME") {
            if (!parseNameSection(payload, namesById, error)) return false;
        } else if (sh.tag == "NAMZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "NAMZ", error);
            if (!error.isEmpty()) return false;
            if (!parseNameSection(raw, namesById, error)) return false;
        } else if (sh.tag == "NODE") {
            if (!parseNodeSection(payload, nodesById, error)) return false;
        } else if (sh.tag == "NODZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "NODZ", error);
            if (!error.isEmpty()) return false;
            if (!parseNodeSection(raw, nodesById, error)) return false;
        } else if (sh.tag == "SIGT") {
            if (!parseSignalSection(payload, version, sigs, error)) return false;
        } else if (sh.tag == "SIGZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "SIGZ", error);
            if (!error.isEmpty()) return false;
            if (!parseSignalSection(raw, version, sigs, error)) return false;
        } else if (sh.tag == "CLKD") {
            if (!parseClockSection(payload, clocks, error)) return false;
        } else if (sh.tag == "CLKZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "CLKZ", error);
            if (!error.isEmpty()) return false;
            if (!parseClockSection(raw, clocks, error)) return false;
        } else if (sh.tag == "FOOT") {
            const qint64 footerSectionOffset = sh.payloadOffset - 12;
            if (footerOffset != 0 && footerSectionOffset >= 0 && u64(footerSectionOffset) != footerOffset) {
                error = QStringLiteral("WVZ4 footer_offset does not point to the FOOT section");
                return false;
            }
            if (!parseFooterSection(payload, version, byteWidthByStorageId, boolStorageByStorageId,
                                    footerBlocks, footerBlockIndexesByChunk,
                                    footerLodLevelsByStorageId, error)) return false;
            const u64 fileSize64 = u64(file.size());
            for (int i = 0; i < footerBlocks.size(); ++i) {
                const BlockIndexRec& b = footerBlocks.at(i);
                if (b.fileOffset > fileSize64 || b.fileSize > fileSize64 - b.fileOffset) {
                    error = QStringLiteral("WVZ4 FOOT block %1 file range exceeds file size").arg(b.blockId);
                    return false;
                }
            }
            break;
        } else {
            // Unknown extension section: already read and ignored.
        }
    }

    if (!initializeOutput()) return false;

    if (useFooterIndexedWdat && (allSelectedStorageIds || !selectedStorageIds.isEmpty())) {
        if (!decodeWdatSectionsFromFooterIndex(file, footerBlocks, footerBlockIndexesByChunk,
                                               version, headerSignalsPerChunk,
                                               selectedStorageIds, allSelectedStorageIds,
                                               outputIndexesByStorageId, byteWidthByStorageId,
                                               outputSignals, samplesByOutputIndex,
                                               options.timeStart, options.timeEnd,
                                               minTime, maxTime, error)) {
            return false;
        }
    }

    for (int i = 0; i < footerBlocks.size(); ++i) {
        const BlockIndexRec& b = footerBlocks.at(i);
        minTime = qMin(minTime, b.start);
        maxTime = qMax(maxTime, b.end);
    }

    appendClockSamplesForLoadedSignals(clocks, outputIndexBySignalId,
                                       options.timeStart, options.timeEnd,
                                       outputSignals, samplesByOutputIndex,
                                       minTime, maxTime);

    for (int i = 0; i < outputSignals.size(); ++i) {
        const int storageId = outputSignals.at(i).storageId > 0
            ? outputSignals.at(i).storageId
            : directIntMapValue(storageIdBySignalId, outputSignals.at(i).signalId, outputSignals.at(i).signalId);
        if (storageId > 0 && storageId < footerLodLevelsByStorageId.size()) {
            outputSignals[i].lodLevels = footerLodLevelsByStorageId.at(storageId);
        }
        outputSignals[i].samples = std::move(samplesByOutputIndex[i]);
        if (allSelectedSignalIds || selectedIds.contains(outputSignals.at(i).signalId)) {
            outputSignals[i].samplesLoaded = true;
        }
    }

    outWave.meta.title = QFileInfo(filePath).completeBaseName();
    outWave.meta.timescale = QStringLiteral("cycle");
    outWave.meta.start = (minTime == std::numeric_limits<qint64>::max()) ? 0 : minTime;
    outWave.meta.end = qMax(outWave.meta.start + 1, maxTime);
    outWave.signalList = std::move(outputSignals);
    return true;
}
