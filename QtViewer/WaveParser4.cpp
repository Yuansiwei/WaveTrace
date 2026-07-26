#include "WaveParser4.h"

#include <QByteArray>
#include <QBitArray>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <zstd.h>

namespace {

using u8 = quint8;
using u32 = quint32;
using u64 = quint64;
using i64 = qint64;

thread_local u64 gDecodedSampleLimit = 0;
thread_local u64 gDecodedSampleCount = 0;
thread_local const QVector<quint64>* gDecodedMatchTargets = nullptr;

class DecodedSampleBudgetScope {
public:
    explicit DecodedSampleBudgetScope(u64 limit)
        : oldLimit_(gDecodedSampleLimit),
          oldCount_(gDecodedSampleCount) {
        gDecodedSampleLimit = limit;
        gDecodedSampleCount = 0;
    }

    ~DecodedSampleBudgetScope() {
        gDecodedSampleLimit = oldLimit_;
        gDecodedSampleCount = oldCount_;
    }

private:
    u64 oldLimit_ = 0;
    u64 oldCount_ = 0;
};

class DecodedMatchTargetScope {
public:
    explicit DecodedMatchTargetScope(const QVector<quint64>* targets)
        : oldTargets_(gDecodedMatchTargets) {
        gDecodedMatchTargets = targets;
    }

    ~DecodedMatchTargetScope() {
        gDecodedMatchTargets = oldTargets_;
    }

private:
    const QVector<quint64>* oldTargets_ = nullptr;
};

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

static const u32 kSupportedFormatVersion = 15;

bool isSupportedFormatVersion(u32 version) {
    return version == kSupportedFormatVersion;
}
static const u8 kSignalFlagStorageOnly = 1u << 0;

static const u64 kHeaderFeatureLodTables = 1ull << 6;
static const u64 kHeaderFeatureResidualLodTables = 1ull << 7;

// Raw WDAT payload flags. These match wvz4_writer_typed.h.
static const u64 kWdatDeltaTimes      = 1ull << 0;
static const u64 kWdatFixedValueWidth = 1ull << 1;
static const u64 kWdatSharedTimeTable = 1ull << 2;
static const u64 kWdatSignalChunkTile = 1ull << 3;
static const u64 kWdatSparseSignalRecords = 1ull << 4;
static const u64 kWdatPerRecordValueCodec = 1ull << 5;
static const u64 kKnownWdatFlags =
    kWdatDeltaTimes | kWdatFixedValueWidth | kWdatSharedTimeTable |
    kWdatSignalChunkTile | kWdatSparseSignalRecords | kWdatPerRecordValueCodec;

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
    u32 nameToken = 0;
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
    u32 bitOffset = 0;
    Radix radix = Radix::Auto;
    bool storageOnly = false;
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
    // WDAT tiles are indexed by signal chunk.
    u64 signalChunkId = 0;
    u64 firstSignalId = 1;
    u64 signalCount = 0;
    u64 fileOffset = 0;
    u64 fileSize = 0;
    u64 rawSize = 0;
    Compression compression = Compression::None;
};

struct LodChunkIndexRec {
    u64 chunkId = 0;
    u64 levelIndex = 0;
    qint64 bucketCycles = 0;
    u64 signalChunkId = 0;
    i64 start = 0;
    i64 end = 0;
    u64 fileOffset = 0;
    u64 fileSize = 0;
    u64 rawSize = 0;
    Compression compression = Compression::None;
    u64 storageCount = 0;
    u64 recordCount = 0;
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

struct StorageOutputIndexLookup {
    const QVector<QVector<int>>* direct = nullptr;
    const QHash<int, QVector<int>>* sparse = nullptr;

    const QVector<int>* value(int storageId) const {
        if (sparse) {
            const auto it = sparse->constFind(storageId);
            return it == sparse->constEnd() ? nullptr : &it.value();
        }
        return direct ? directIntListMapValue(*direct, storageId) : nullptr;
    }
};

StorageOutputIndexLookup storageOutputIndexLookup(
        const QVector<QVector<int>>& direct) {
    StorageOutputIndexLookup lookup;
    lookup.direct = &direct;
    return lookup;
}

StorageOutputIndexLookup storageOutputIndexLookup(
        const QHash<int, QVector<int>>& sparse) {
    StorageOutputIndexLookup lookup;
    lookup.sparse = &sparse;
    return lookup;
}

bool applyProceduralClockDefinitions(const QVector<ClockRec>& clocks,
                                     const QVector<int>& outputIndexBySignalId,
                                     QVector<WaveSignal>& outputSignals,
                                     bool requireAllClockSignals,
                                     QString& error) {
    for (const ClockRec& clock : clocks) {
        const int outputIndex =
            directIntMapValue(outputIndexBySignalId, int(clock.signalId), -1);
        if (outputIndex < 0 || outputIndex >= outputSignals.size()) {
            if (requireAllClockSignals) {
                error = QStringLiteral("WVZ4 CLKD references missing visible signal_id %1")
                            .arg(int(clock.signalId));
                return false;
            }
            continue;
        }

        WaveSignal& signal = outputSignals[outputIndex];
        if (signal.proceduralClock) {
            error = QStringLiteral("WVZ4 CLKD contains duplicate signal_id %1")
                        .arg(int(clock.signalId));
            return false;
        }
        if (signal.kind != SignalKind::Bit || signal.width != 1 ||
            clock.periodTicks == 0) {
            error = QStringLiteral("WVZ4 CLKD signal_id %1 is not a valid 1-bit periodic clock")
                        .arg(int(clock.signalId));
            return false;
        }

        signal.proceduralClock = true;
        signal.clockInitialValue = clock.initialValue;
        signal.clockTogglePeriodTicks = clock.periodTicks;
        signal.samplesLoaded = true;
        signal.samples.clear();
        signal.rawLoadedRanges.clear();
        signal.changeTimes.clear();
        signal.changeTimesReady = false;
        signal.lodLevels.clear();
    }
    return true;
}

bool applyProceduralClockDefinitions(const QVector<ClockRec>& clocks,
                                     const QHash<int, int>& outputIndexBySignalId,
                                     QVector<WaveSignal>& outputSignals,
                                     bool requireAllClockSignals,
                                     QString& error) {
    // Clock tables are normally tiny. Avoid rebuilding a global-size direct
    // array for sparse high signal IDs; validate/apply clocks directly.
    for (const ClockRec& clock : clocks) {
        const auto found = outputIndexBySignalId.constFind(int(clock.signalId));
        const int outputIndex =
            found == outputIndexBySignalId.constEnd() ? -1 : found.value();
        if (outputIndex < 0 || outputIndex >= outputSignals.size()) {
            if (requireAllClockSignals) {
                error = QStringLiteral("WVZ4 CLKD references missing visible signal_id %1")
                            .arg(int(clock.signalId));
                return false;
            }
            continue;
        }
        WaveSignal& signal = outputSignals[outputIndex];
        if (signal.proceduralClock) {
            error = QStringLiteral("WVZ4 CLKD contains duplicate signal_id %1")
                        .arg(int(clock.signalId));
            return false;
        }
        if (signal.kind != SignalKind::Bit || signal.width != 1 ||
            clock.periodTicks == 0) {
            error = QStringLiteral("WVZ4 CLKD signal_id %1 is not a valid 1-bit periodic clock")
                        .arg(int(clock.signalId));
            return false;
        }
        signal.proceduralClock = true;
        signal.clockInitialValue = clock.initialValue;
        signal.clockTogglePeriodTicks = clock.periodTicks;
        signal.samplesLoaded = true;
        signal.samples.clear();
        signal.rawLoadedRanges.clear();
        signal.changeTimes.clear();
        signal.changeTimesReady = false;
        signal.lodLevels.clear();
    }
    return true;
}

QSet<int> proceduralClockSignalIds(const QVector<ClockRec>& clocks) {
    QSet<int> ids;
    ids.reserve(clocks.size() * 2 + 1);
    for (const ClockRec& clock : clocks) ids.insert(int(clock.signalId));
    return ids;
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

    bool readVarInt(i64& out) {
        u64 encoded = 0;
        if (!readVarUInt(encoded)) return false;
        out = static_cast<i64>(encoded >> 1u) ^ -static_cast<i64>(encoded & 1u);
        return true;
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
                      QVector<QByteArray>& namesById,
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
        namesById[id] = QByteArray(p, int(len64));
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

bool parseNodeReferenceSection(const QByteArray& payload,
                               QVector<NodeRec>& nodesById,
                               QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count) || count > 100000000ull) {
        error = QStringLiteral("WVZ4 NREF: invalid node count");
        return false;
    }
    i64 previousArrayIndex = 0;
    for (u64 i = 0; i < count; ++i) {
        u64 id64 = 0, parent64 = 0, nameRef = 0, first64 = 0, next64 = 0;
        u8 kind = 0;
        if (!r.readVarUInt(id64) || id64 == 0 || id64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(parent64) || parent64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(nameRef) ||
            !r.readU8(kind) ||
            !r.readVarUInt(first64) || first64 > u64(std::numeric_limits<int>::max()) ||
            !r.readVarUInt(next64) || next64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 NREF: malformed node record");
            return false;
        }
        const bool isArrayIndex = (nameRef & 1u) != 0;
        const u64 nameValue = nameRef >> 1u;
        if ((!isArrayIndex && (nameValue == 0 || nameValue > u64(std::numeric_limits<int>::max()))) ||
            (isArrayIndex && nameValue > (u64(std::numeric_limits<u32>::max()) * 2u + 1u))) {
            error = QStringLiteral("WVZ4 NREF: invalid node name reference");
            return false;
        }
        const int id = int(id64);
        if (nodesById.size() <= id) nodesById.resize(id + 1);
        if (nodesById[id].valid) {
            error = QStringLiteral("WVZ4 NREF: duplicate node_id %1").arg(id);
            return false;
        }
        NodeRec n;
        n.id = u32(id64);
        n.parent = u32(parent64);
        if (isArrayIndex) {
            const i64 delta = i64((nameValue >> 1u) ^ (u64(0) - (nameValue & 1u)));
            const i64 indexValue = previousArrayIndex + delta;
            if (indexValue < 0 || u64(indexValue) > u64(kWaveNameTokenValueMask)) {
                error = QStringLiteral("WVZ4 NREF: array index delta out of range");
                return false;
            }
            n.nameToken = waveArrayIndexToken(u32(indexValue));
            previousArrayIndex = indexValue;
        } else {
            n.nameToken = waveNameIdToken(u32(nameValue));
        }
        n.kind = kind;
        n.firstChild = u32(first64);
        n.nextSibling = u32(next64);
        n.valid = true;
        nodesById[id] = n;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 NREF: trailing bytes after node table");
        return false;
    }
    return true;
}

bool parseCompactNodeReferenceSection(const QByteArray& payload,
                                      QVector<NodeRec>& nodesById,
                                      QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count) || count > 100000000ull ||
        count >= u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 NODI: invalid node count");
        return false;
    }
    nodesById.clear();
    nodesById.resize(int(count) + 1);
    i64 previousNameId = 0;
    i64 previousArrayIndex = 0;
    for (u64 index = 0; index < count; ++index) {
        const u64 id64 = index + 1u;
        u64 parentBack = 0, nameRef = 0, firstForward = 0, nextForward = 0;
        u8 kind = 0;
        if (!r.readVarUInt(parentBack) || !r.readVarUInt(nameRef) || !r.readU8(kind) ||
            !r.readVarUInt(firstForward) || !r.readVarUInt(nextForward)) {
            error = QStringLiteral("WVZ4 NODI: malformed compact node record");
            return false;
        }
        const bool isArrayIndex = (nameRef & 1u) != 0;
        const u64 encodedValue = nameRef >> 1u;
        i64 nameId = 0;
        u32 arrayIndex = 0;
        if (isArrayIndex) {
            if (encodedValue > (u64(std::numeric_limits<u32>::max()) * 2u + 1u)) {
                error = QStringLiteral("WVZ4 NODI: array index out of range for node_id %1").arg(id64);
                return false;
            }
            const i64 delta = i64((encodedValue >> 1u) ^ (u64(0) - (encodedValue & 1u)));
            const i64 indexValue = previousArrayIndex + delta;
            if (indexValue < 0 || u64(indexValue) > u64(kWaveNameTokenValueMask)) {
                error = QStringLiteral("WVZ4 NODI: array index delta out of range for node_id %1").arg(id64);
                return false;
            }
            arrayIndex = u32(indexValue);
            previousArrayIndex = indexValue;
        } else {
            if (encodedValue > (u64(std::numeric_limits<u32>::max()) * 2u + 1u)) {
                error = QStringLiteral("WVZ4 NODI: name delta out of range for node_id %1").arg(id64);
                return false;
            }
            const i64 delta = i64((encodedValue >> 1u) ^ (u64(0) - (encodedValue & 1u)));
            nameId = previousNameId + delta;
            if (nameId <= 0 || nameId > std::numeric_limits<int>::max()) {
                error = QStringLiteral("WVZ4 NODI: name_id out of range for node_id %1").arg(id64);
                return false;
            }
            previousNameId = nameId;
        }
        if (parentBack >= id64 ||
            (firstForward != 0 && firstForward > count - id64) ||
            (nextForward != 0 && nextForward > count - id64)) {
            error = QStringLiteral("WVZ4 NODI: topology delta out of range for node_id %1").arg(id64);
            return false;
        }
        NodeRec n;
        n.id = u32(id64);
        n.parent = parentBack == 0 ? 0u : u32(id64 - parentBack);
        if (isArrayIndex) {
            n.nameToken = waveArrayIndexToken(arrayIndex);
        } else {
            n.nameToken = waveNameIdToken(u32(nameId));
        }
        n.kind = kind;
        n.firstChild = firstForward == 0 ? 0u : u32(id64 + firstForward);
        n.nextSibling = nextForward == 0 ? 0u : u32(id64 + nextForward);
        n.valid = true;
        nodesById[int(id64)] = n;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 NODI: trailing bytes after compact node table");
        return false;
    }
    return true;
}

bool parseCompactNodeTreeSection(const QByteArray& payload,
                                 WaveTreeInfo& tree,
                                 QString& error) {
    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count) || count > 100000000ull ||
        count >= u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 NODI: invalid node count");
        return false;
    }
    if (tree.namesById.isEmpty()) {
        error = QStringLiteral("WVZ4 NODI: NAME must precede the node table");
        return false;
    }

    tree.valid = false;
    tree.nodesById.clear();
    tree.nodesById.resize(int(count) + 1);
    tree.rootNodeIds.clear();
    tree.rootNodeIds.reserve(4);
    tree.signalIndexToNodeId.clear();
    tree.signalIndexBySignalId.clear();

    i64 previousNameId = 0;
    i64 previousArrayIndex = 0;
    for (u64 index = 0; index < count; ++index) {
        const u64 id64 = index + 1u;
        u64 parentBack = 0, nameRef = 0, firstForward = 0, nextForward = 0;
        u8 kind = 0;
        if (!r.readVarUInt(parentBack) || !r.readVarUInt(nameRef) || !r.readU8(kind) ||
            !r.readVarUInt(firstForward) || !r.readVarUInt(nextForward)) {
            error = QStringLiteral("WVZ4 NODI: malformed compact node record");
            return false;
        }
        if (!isValidNodeKind(kind)) {
            error = QStringLiteral("WVZ4 NODI: invalid NodeKind for node_id %1").arg(id64);
            return false;
        }
        if (parentBack >= id64 ||
            (firstForward != 0 && firstForward > count - id64) ||
            (nextForward != 0 && nextForward > count - id64)) {
            error = QStringLiteral("WVZ4 NODI: topology delta out of range for node_id %1").arg(id64);
            return false;
        }

        const bool isArrayIndex = (nameRef & 1u) != 0;
        const u64 encodedValue = nameRef >> 1u;
        quint32 nameToken = 0;
        if (isArrayIndex) {
            if (encodedValue > (u64(std::numeric_limits<u32>::max()) * 2u + 1u)) {
                error = QStringLiteral("WVZ4 NODI: array index out of range for node_id %1").arg(id64);
                return false;
            }
            const i64 delta = i64((encodedValue >> 1u) ^ (u64(0) - (encodedValue & 1u)));
            const i64 indexValue = previousArrayIndex + delta;
            if (indexValue < 0 || u64(indexValue) > u64(kWaveNameTokenValueMask)) {
                error = QStringLiteral("WVZ4 NODI: array index delta out of range for node_id %1").arg(id64);
                return false;
            }
            nameToken = waveArrayIndexToken(u32(indexValue));
            previousArrayIndex = indexValue;
        } else {
            if (encodedValue > (u64(std::numeric_limits<u32>::max()) * 2u + 1u)) {
                error = QStringLiteral("WVZ4 NODI: name delta out of range for node_id %1").arg(id64);
                return false;
            }
            const i64 delta = i64((encodedValue >> 1u) ^ (u64(0) - (encodedValue & 1u)));
            const i64 nameId = previousNameId + delta;
            if (nameId <= 0 || nameId >= tree.namesById.size() || tree.namesById.at(int(nameId)).isEmpty()) {
                error = QStringLiteral("WVZ4 NODI: invalid name_id for node_id %1").arg(id64);
                return false;
            }
            nameToken = waveNameIdToken(u32(nameId));
            previousNameId = nameId;
        }

        const int id = int(id64);
        const int parent = parentBack == 0 ? 0 : int(id64 - parentBack);
        const int firstChild = firstForward == 0 ? 0 : int(id64 + firstForward);
        const int nextSibling = nextForward == 0 ? 0 : int(id64 + nextForward);
        if (parent != 0 && !tree.nodesById.at(parent).valid) {
            error = QStringLiteral("WVZ4 NODI: parent is not a prior node for node_id %1").arg(id);
            return false;
        }

        // signalIndex is unused until SIGD is decoded. Reuse it here as the
        // expected parent carried by first_child/next_sibling forward links.
        WaveTreeNode& dst = tree.nodesById[id];
        const int expectedParent = dst.signalIndex;
        const int expectedRow = dst.signalId;
        if ((parent != 0 && expectedParent != parent) ||
            (parent == 0 && expectedParent > 0)) {
            error = QStringLiteral("WVZ4 NODI: child chain does not reach node_id %1").arg(id);
            return false;
        }

        dst.parentId = parent;
        dst.firstChild = firstChild;
        dst.nextSibling = nextSibling;
        dst.rowInParent = parent == 0 ? tree.rootNodeIds.size() : expectedRow;
        dst.nameToken = nameToken;
        dst.kind = kind;
        dst.signalIndex = -1;
        dst.signalId = -1;
        dst.valid = true;
        if (parent == 0) tree.rootNodeIds.push_back(id);

        auto setExpectedParent = [&](int target, int targetParent, int targetRow) -> bool {
            if (target == 0) return true;
            WaveTreeNode& future = tree.nodesById[target];
            if (future.valid || future.signalIndex != -1 || future.signalId != -1) {
                error = QStringLiteral("WVZ4 NODI: duplicate child-chain link to node_id %1").arg(target);
                return false;
            }
            future.signalIndex = targetParent;
            future.signalId = targetRow;
            return true;
        };
        if (!setExpectedParent(firstChild, id, 0) ||
            !setExpectedParent(nextSibling, parent, dst.rowInParent + 1)) return false;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 NODI: trailing bytes after compact node table");
        return false;
    }
    if (tree.rootNodeIds.isEmpty()) {
        error = QStringLiteral("WVZ4 NODI: no root-level node found");
        return false;
    }
    tree.valid = true;
    return true;
}

bool parseSignalSection(const QByteArray& payload,
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
        u64 sid64 = 0, storage64 = 0, node64 = 0, width64 = 0, bitOffset64 = 0;
        u8 type = 0, radix = 0, flags = 0;
        if (!r.readVarUInt(sid64) || sid64 == 0 || sid64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 SIGT: malformed signal record");
            return false;
        }
        if (!r.readVarUInt(storage64) || storage64 == 0 || storage64 > u64(std::numeric_limits<int>::max())) {
            error = QStringLiteral("WVZ4 SIGT: malformed storage_id for signal_id %1").arg(int(sid64));
            return false;
        }
        if (!r.readVarUInt(node64) || node64 > u64(std::numeric_limits<int>::max()) ||
            !r.readU8(type) ||
            !r.readVarUInt(width64) || width64 == 0 || width64 > 64 ||
            !r.readU8(radix)) {
            error = QStringLiteral("WVZ4 SIGT: malformed signal record");
            return false;
        }
        if (!r.readVarUInt(bitOffset64) || bitOffset64 > 63 ||
            !r.readU8(flags) ||
            (flags & ~kSignalFlagStorageOnly) != 0) {
            error = QStringLiteral("WVZ4 SIGT: malformed range flags for signal_id %1").arg(int(sid64));
            return false;
        }
        const bool storageOnly = (flags & kSignalFlagStorageOnly) != 0;
        if (storageOnly) {
            if (node64 != 0 || storage64 != sid64 || bitOffset64 != 0) {
                error = QStringLiteral("WVZ4 SIGT: malformed storage-only signal_id %1").arg(int(sid64));
                return false;
            }
        } else if (node64 == 0) {
            error = QStringLiteral("WVZ4 SIGT: signal_id %1 has missing node_id").arg(int(sid64));
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
        if (bitOffset64 + width64 > 64) {
            error = QStringLiteral("WVZ4 SIGT: bit range exceeds 64 bits for signal_id %1").arg(int(sid64));
            return false;
        }
        SigRec s;
        s.signalId = u32(sid64);
        s.storageId = u32(storage64);
        s.nodeId = u32(node64);
        s.type = ValueType(type);
        s.bitWidth = u32(width64);
        s.bitOffset = u32(bitOffset64);
        s.radix = Radix(radix);
        s.storageOnly = storageOnly;
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

bool parseCompactSignalSection(const QByteArray& payload,
                               QVector<SigRec>& sigs,
                               QString& error) {
    static const u8 kStorageBackPresent = 1u << 4;
    static const u8 kBitOffsetPresent = 1u << 5;
    static const u8 kNonNaturalWidth = 1u << 6;
    static const u8 kExplicitRadix = 1u << 7;

    SpanReader r(payload);
    u64 count = 0;
    if (!r.readVarUInt(count)) {
        error = QStringLiteral("WVZ4 SIGD: missing count");
        return false;
    }
    if (count > 100000000ull || count >= u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 SIGD: unreasonable signal count");
        return false;
    }

    sigs.clear();
    sigs.reserve(int(count));
    i64 previousNodeId = 0;
    for (u64 index = 0; index < count; ++index) {
        const u64 signalId = index + 1u;
        u8 meta = 0;
        i64 nodeDelta = 0;
        if (!r.readU8(meta) || !r.readVarInt(nodeDelta)) {
            error = QStringLiteral("WVZ4 SIGD: malformed compact signal record");
            return false;
        }

        const ValueType type = ValueType(meta & 0x0fu);
        int typeBytes = 0;
        if (!valueTypeByteWidth(type, typeBytes)) {
            error = QStringLiteral("WVZ4 SIGD: invalid ValueType for signal_id %1").arg(signalId);
            return false;
        }
        const i64 nodeId = previousNodeId + nodeDelta;
        if (nodeId < 0 || nodeId > std::numeric_limits<int>::max()) {
            error = QStringLiteral("WVZ4 SIGD: node delta out of range for signal_id %1").arg(signalId);
            return false;
        }

        u64 storageBack = 0;
        u64 width = type == ValueType::Bool ? 1u : u64(typeBytes * 8);
        u64 bitOffset = 0;
        u8 radixByte = static_cast<u8>(Radix::Auto);
        if ((meta & kStorageBackPresent) != 0) {
            if (!r.readVarUInt(storageBack) || storageBack == 0 || storageBack >= signalId) {
                error = QStringLiteral("WVZ4 SIGD: invalid storage delta for signal_id %1").arg(signalId);
                return false;
            }
        }
        if ((meta & kNonNaturalWidth) != 0 &&
            (!r.readVarUInt(width) || width == 0 || width > 64)) {
            error = QStringLiteral("WVZ4 SIGD: invalid width for signal_id %1").arg(signalId);
            return false;
        }
        if ((meta & kExplicitRadix) != 0 && !r.readU8(radixByte)) {
            error = QStringLiteral("WVZ4 SIGD: missing radix for signal_id %1").arg(signalId);
            return false;
        }
        if ((meta & kBitOffsetPresent) != 0 &&
            (!r.readVarUInt(bitOffset) || bitOffset > 63)) {
            error = QStringLiteral("WVZ4 SIGD: invalid bit offset for signal_id %1").arg(signalId);
            return false;
        }
        if (!isValidRadix(Radix(radixByte)) || width > u64(typeBytes * 8) || bitOffset + width > 64) {
            error = QStringLiteral("WVZ4 SIGD: invalid type/range metadata for signal_id %1").arg(signalId);
            return false;
        }

        const u64 storageId = storageBack == 0 ? signalId : signalId - storageBack;
        const bool storageOnly = nodeId == 0;
        if (storageOnly && (storageId != signalId || bitOffset != 0)) {
            error = QStringLiteral("WVZ4 SIGD: malformed storage-only signal_id %1").arg(signalId);
            return false;
        }

        SigRec s;
        s.signalId = u32(signalId);
        s.storageId = u32(storageId);
        s.nodeId = u32(nodeId);
        s.type = type;
        s.bitWidth = u32(width);
        s.bitOffset = u32(bitOffset);
        s.radix = Radix(radixByte);
        s.storageOnly = storageOnly;
        sigs.push_back(s);
        previousNodeId = nodeId;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 SIGD: trailing bytes after compact signal table");
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

struct RawLeftAnchorState;

struct RawDecodeObserver {
    using Callback = bool (*)(qint64 sampleTime, void* context);
    Callback callback = nullptr;
    void* context = nullptr;
};

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
                        bool compactSamples,
                        RawLeftAnchorState* leftAnchors,
                        QString& error,
                        const RawDecodeObserver* observer = nullptr);

bool decodeLodTransitionStreamPayload(const char* payload,
                                      int payloadSize,
                                      int byteWidth,
                                      QVector<WaveSample>& samples,
                                      QString& error) {
    if (byteWidth <= 0 || byteWidth > 8 || payloadSize < 0) {
        error = QStringLiteral("WVZ4 LOD transition stream header is invalid");
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
                            0, std::numeric_limits<qint64>::max(), false, nullptr, error)) {
        if (error.isEmpty()) error = QStringLiteral("WVZ4 failed to decode LOD transition stream");
        return false;
    }
    if (!rr.eof()) {
        error = QStringLiteral("WVZ4 LOD transition stream has trailing bytes");
        return false;
    }

    samples = decoded[0];
    return true;
}

bool parseFooterSection(const QByteArray& payload,
                        const QVector<int>& byteWidthByStorageId,
                        const QVector<int>& boolStorageByStorageId,
                        QVector<BlockIndexRec>& blocks,
                        QVector<QVector<int>>& blockIndexesByChunk,
                        QVector<QVector<WaveLodLevel>>& lodLevelsByStorageId,
                        QVector<LodChunkIndexRec>& lodChunkIndex,
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
    blockIndexesByChunk.clear();
    lodLevelsByStorageId.clear();
    lodChunkIndex.clear();
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
        if (!r.readVarUInt(signalChunkId) ||
            !r.readVarUInt(firstSignalId) ||
            !r.readVarUInt(signalCount)) {
            error = QStringLiteral("WVZ4 FOOT: malformed signal chunk fields");
            return false;
        }
        if (firstSignalId == 0 || signalCount == 0 ||
            firstSignalId > u64(std::numeric_limits<int>::max()) ||
            signalCount > u64(std::numeric_limits<int>::max()) ||
            firstSignalId > u64(std::numeric_limits<int>::max()) - signalCount + 1ull) {
            error = QStringLiteral("WVZ4 FOOT: invalid signal chunk range for block %1").arg(blockId);
            return false;
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
    {
        u64 chunkCount = 0;
        if (!r.readVarUInt(chunkCount)) {
            error = QStringLiteral("WVZ4 FOOT: missing chunk index count");
            return false;
        }
        if (chunkCount > 10000000ull) {
            error = QStringLiteral("WVZ4 FOOT: unreasonable chunk index count");
            return false;
        }
        for (u64 ci = 0; ci < chunkCount; ++ci) {
            u64 chunkId = 0;
            u64 entryCount = 0;
            if (!r.readVarUInt(chunkId) || !r.readVarUInt(entryCount)) {
                error = QStringLiteral("WVZ4 FOOT: malformed chunk index header");
                return false;
            }
            if (chunkId > u64(std::numeric_limits<int>::max()) ||
                entryCount > u64(std::numeric_limits<int>::max())) {
                error = QStringLiteral("WVZ4 FOOT: chunk index too large");
                return false;
            }
            if (blockIndexesByChunk.size() <= int(chunkId)) {
                blockIndexesByChunk.resize(int(chunkId) + 1);
            }
            QVector<int>& indexes = blockIndexesByChunk[int(chunkId)];
            if (!indexes.isEmpty()) {
                error = QStringLiteral("WVZ4 FOOT: duplicate chunk index %1").arg(chunkId);
                return false;
            }
            indexes.reserve(int(entryCount));
            u64 prev = 0;
            for (u64 ei = 0; ei < entryCount; ++ei) {
                u64 delta = 0;
                if (!r.readVarUInt(delta)) {
                    error = QStringLiteral("WVZ4 FOOT: malformed chunk index body");
                    return false;
                }
                if (delta > u64(std::numeric_limits<int>::max()) ||
                    prev > u64(std::numeric_limits<int>::max()) - delta) {
                    error = QStringLiteral("WVZ4 FOOT: chunk index overflow");
                    return false;
                }
                const u64 index = prev + delta;
                if (index >= u64(blocks.size())) {
                    error = QStringLiteral("WVZ4 FOOT: chunk index references missing block");
                    return false;
                }
                if (!indexes.isEmpty() && int(index) <= indexes.last()) {
                    error = QStringLiteral("WVZ4 FOOT: chunk index is not strictly increasing");
                    return false;
                }
                const BlockIndexRec& b = blocks.at(int(index));
                if (b.signalChunkId != chunkId) {
                    error = QStringLiteral("WVZ4 FOOT: chunk index references wrong chunk");
                    return false;
                }
                indexes.push_back(int(index));
                prev = index;
            }
        }
    }
    u64 lodLevelCount = 0;
        if (!r.readVarUInt(lodLevelCount)) {
            error = QStringLiteral("WVZ4 FOOT: missing LOD level count");
            return false;
        }
        if (lodLevelCount == 0 || lodLevelCount > 64ull) {
            error = QStringLiteral("WVZ4 FOOT: unreasonable LOD level count");
            return false;
        }
        QVector<qint64> bucketCyclesByLevel;
        bucketCyclesByLevel.reserve(int(lodLevelCount));
        qint64 prevBucketCycles = 0;
        for (u64 i = 0; i < lodLevelCount; ++i) {
            u64 bucketCycles = 0;
            if (!r.readVarUInt(bucketCycles) || bucketCycles == 0 ||
                bucketCycles > u64(std::numeric_limits<qint64>::max())) {
                error = QStringLiteral("WVZ4 FOOT: invalid LOD bucket width");
                return false;
            }
            if (prevBucketCycles != 0 && qint64(bucketCycles) <= prevBucketCycles) {
                error = QStringLiteral("WVZ4 FOOT: LOD bucket widths are not increasing");
                return false;
            }
            bucketCyclesByLevel.push_back(qint64(bucketCycles));
            prevBucketCycles = qint64(bucketCycles);
        }

        {
            u64 lodChunkCount = 0;
            if (!r.readVarUInt(lodChunkCount)) {
                error = QStringLiteral("WVZ4 FOOT: missing LODZ chunk count");
                return false;
            }
            if (lodChunkCount > 100000000ull) {
                error = QStringLiteral("WVZ4 FOOT: unreasonable LODZ chunk count");
                return false;
            }
            lodChunkIndex.reserve(int(qMin<u64>(lodChunkCount, u64(std::numeric_limits<int>::max()))));
            for (u64 ci = 0; ci < lodChunkCount; ++ci) {
                u64 chunkId = 0;
                u64 levelIndex = 0;
                u64 signalChunkId = 0;
                i64 startCycle = 0;
                i64 endCycle = 0;
                u64 fileOffset = 0;
                u64 fileSize = 0;
                u64 rawSize = 0;
                u8 comp = 0;
                u64 chunkStorageCount = 0;
                u64 chunkRecordCount = 0;
                if (!r.readVarUInt(chunkId) ||
                    !r.readVarUInt(levelIndex) || levelIndex >= lodLevelCount ||
                    !r.readVarUInt(signalChunkId) ||
                    !r.readI64(startCycle) ||
                    !r.readI64(endCycle) ||
                    !r.readVarUInt(fileOffset) ||
                    !r.readVarUInt(fileSize) ||
                    !r.readVarUInt(rawSize) ||
                    !r.readU8(comp) ||
                    !r.readVarUInt(chunkStorageCount) ||
                    !r.readVarUInt(chunkRecordCount)) {
                    error = QStringLiteral("WVZ4 FOOT: malformed LODZ chunk index record");
                    return false;
                }
                if (endCycle <= startCycle || !isValidCompression(Compression(comp))) {
                    error = QStringLiteral("WVZ4 FOOT: invalid LODZ chunk index record");
                    return false;
                }
                LodChunkIndexRec rec;
                rec.chunkId = chunkId;
                rec.levelIndex = levelIndex;
                rec.bucketCycles = bucketCyclesByLevel.at(int(levelIndex));
                rec.signalChunkId = signalChunkId;
                rec.start = startCycle;
                rec.end = endCycle;
                rec.fileOffset = fileOffset;
                rec.fileSize = fileSize;
                rec.rawSize = rawSize;
                rec.compression = Compression(comp);
                rec.storageCount = chunkStorageCount;
                rec.recordCount = chunkRecordCount;
                lodChunkIndex.push_back(rec);
            }
            if (!r.eof()) {
                error = QStringLiteral("WVZ4 FOOT: trailing bytes after LODZ chunk index");
                return false;
            }
            return true;
        }
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

bool validateNodeAndSignalLayout(const QVector<NodeRec>& nodesById,
                                 const QVector<QByteArray>& namesById,
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

        if (!waveNameTokenIsArrayIndex(n.nameToken)) {
            const u32 nameId = waveNameTokenValue(n.nameToken);
            if (nameId == 0 || nameId >= u32(namesById.size()) || namesById.at(int(nameId)).isEmpty()) {
                error = QStringLiteral("WVZ4 NODE node_id %1 references missing name_id %2")
                            .arg(nodeId).arg(int(nameId));
                return false;
            }
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
        if (s.storageOnly) continue;
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
                       const QVector<QByteArray>& namesById,
                       const QVector<SigRec>& sigs,
                       const QVector<int>& outputIndexBySignalId,
                       int outputSignalCount,
                       WaveTreeInfo& outTree,
                       QString& error) {
    outTree = WaveTreeInfo();
    if (nodesById.isEmpty()) return true;

    outTree.valid = true;
    outTree.namesById = namesById;
    outTree.nodesById.resize(nodesById.size());

    for (int nodeId = 1; nodeId < nodesById.size(); ++nodeId) {
        if (!nodesById.at(nodeId).valid) continue;

        const NodeRec& src = nodesById.at(nodeId);
        WaveTreeNode dst;
        dst.parentId = int(src.parent);
        dst.firstChild = int(src.firstChild);
        dst.nextSibling = int(src.nextSibling);
        dst.nameToken = src.nameToken;
        dst.kind = src.kind;
        dst.valid = true;
        outTree.nodesById[nodeId] = dst;

        if (src.parent == 0) {
            dst.rowInParent = outTree.rootNodeIds.size();
            outTree.rootNodeIds.push_back(nodeId);
        }
    }

    for (int parentId = 1; parentId < nodesById.size(); ++parentId) {
        int row = 0;
        for (int childId = int(nodesById.at(parentId).firstChild);
             childId != 0;
             childId = int(nodesById.at(childId).nextSibling)) {
            if (childId <= 0 || childId >= outTree.nodesById.size()) break;
            outTree.nodesById[childId].rowInParent = row++;
        }
    }

    if (outputSignalCount > 0) {
        outTree.signalIndexToNodeId.resize(outputSignalCount);
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

    return true;
}

bool isVisibleSignalRec(const SigRec& s) {
    return !s.storageOnly;
}

void initializeWaveSignalFromRec(const SigRec& s,
                                 bool selected,
                                 WaveSignal& sig) {
    sig.signalId = int(s.signalId);
    sig.storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
    sig.bitOffset = int(s.bitOffset);
    sig.name.clear();
    sig.kind = convertKind(s.type, int(s.bitWidth));
    sig.width = qMax(1, int(s.bitWidth));
    sig.defaultRadix = convertRadix(s.radix, s.type, sig.width);
    sig.currentRadix = sig.defaultRadix;
    sig.supportsZState = false;
    sig.samplesLoaded = selected;
}

WaveSignal makeWaveSignalFromRec(const SigRec& s,
                                 bool selected) {
    WaveSignal sig;
    initializeWaveSignalFromRec(s, selected, sig);
    return sig;
}

bool finalizeCompactDirectorySignals(const QVector<SigRec>& sigs,
                                     WaveTreeInfo& tree,
                                     QVector<WaveSignal>& outputSignals,
                                     QVector<int>& byteWidthByStorageId,
                                     QVector<int>& boolStorageByStorageId,
                                     QString& error) {
    if (!tree.valid || tree.nodesById.isEmpty()) {
        error = QStringLiteral("WVZ4 SIGD: NODI must precede the signal table");
        return false;
    }

    const int signalCount = sigs.size();
    byteWidthByStorageId.resize(signalCount + 1);
    boolStorageByStorageId.resize(signalCount + 1);
    std::fill(byteWidthByStorageId.begin(), byteWidthByStorageId.end(), -1);
    std::fill(boolStorageByStorageId.begin(), boolStorageByStorageId.end(), -1);

    outputSignals.clear();
    outputSignals.reserve(signalCount);
    tree.signalIndexToNodeId.clear();
    tree.signalIndexToNodeId.reserve(signalCount);
    tree.signalIndexBySignalId.resize(signalCount + 1);
    std::fill(tree.signalIndexBySignalId.begin(), tree.signalIndexBySignalId.end(), 0);

    for (int i = 0; i < signalCount; ++i) {
        const SigRec& s = sigs.at(i);
        const int signalId = i + 1;
        if (s.signalId != u32(signalId)) {
            error = QStringLiteral("WVZ4 SIGD: signal_id table is not dense at %1").arg(signalId);
            return false;
        }

        int bytes = 0;
        if (!valueTypeByteWidth(s.type, bytes)) {
            error = QStringLiteral("WVZ4 SIGD: invalid ValueType for signal_id %1").arg(signalId);
            return false;
        }
        const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
        if (storageId <= 0 || storageId > signalId) {
            error = QStringLiteral("WVZ4 SIGD: invalid storage_id %1 for signal_id %2")
                        .arg(storageId).arg(signalId);
            return false;
        }
        if (storageId == signalId) {
            byteWidthByStorageId[storageId] = bytes;
            boolStorageByStorageId[storageId] =
                (s.type == ValueType::Bool && s.bitWidth == 1 && s.bitOffset == 0) ? 1 : 0;
        } else {
            const int storageBytes = byteWidthByStorageId.at(storageId);
            if (storageBytes <= 0) {
                error = QStringLiteral("WVZ4 SIGD: signal_id %1 references missing storage_id %2")
                            .arg(signalId).arg(storageId);
                return false;
            }
            if (s.bitOffset + s.bitWidth > u32(storageBytes * 8)) {
                error = QStringLiteral("WVZ4 SIGD: signal_id %1 bit range exceeds storage_id %2 capacity")
                            .arg(signalId).arg(storageId);
                return false;
            }
        }

        if (s.storageOnly) continue;
        if (s.nodeId == 0 || s.nodeId >= u32(tree.nodesById.size())) {
            error = QStringLiteral("WVZ4 SIGD: signal_id %1 references missing node_id %2")
                        .arg(signalId).arg(int(s.nodeId));
            return false;
        }
        WaveTreeNode& leaf = tree.nodesById[int(s.nodeId)];
        if (!leaf.valid || leaf.kind != kNodeKindSignalLeaf || leaf.firstChild != 0) {
            error = QStringLiteral("WVZ4 SIGD: node_id %1 is not a signal leaf").arg(int(s.nodeId));
            return false;
        }
        if (leaf.signalIndex != -1) {
            error = QStringLiteral("WVZ4 SIGD: multiple signals reference node_id %1").arg(int(s.nodeId));
            return false;
        }

        const int outputIndex = outputSignals.size();
        outputSignals.push_back(makeWaveSignalFromRec(s, false));
        tree.signalIndexToNodeId.push_back(int(s.nodeId));
        tree.signalIndexBySignalId[signalId] = outputIndex + 1;
        leaf.signalIndex = outputIndex;
        leaf.signalId = signalId;
    }
    return true;
}

bool buildStorageMapsForSignals(const QVector<SigRec>& sigs,
                                QVector<int>& byteWidthByStorageId,
                                QVector<int>& boolStorageByStorageId,
                                QVector<int>& storageIdBySignalId,
                                QString& error) {
    int maxSignalId = 0;
    int maxStorageId = 0;
    for (const SigRec& s : sigs) {
        maxSignalId = qMax(maxSignalId, int(s.signalId));
        maxStorageId = qMax(maxStorageId, int(s.storageId != 0 ? s.storageId : s.signalId));
    }

    byteWidthByStorageId.resize(maxStorageId + 1);
    boolStorageByStorageId.resize(maxStorageId + 1);
    storageIdBySignalId.resize(maxSignalId + 1);
    std::fill(byteWidthByStorageId.begin(), byteWidthByStorageId.end(), -1);
    std::fill(boolStorageByStorageId.begin(), boolStorageByStorageId.end(), -1);
    std::fill(storageIdBySignalId.begin(), storageIdBySignalId.end(), -1);

    for (int i = 0; i < sigs.size(); ++i) {
        const SigRec& s = sigs.at(i);
        int bytes = 0;
        if (!valueTypeByteWidth(s.type, bytes)) {
            error = QStringLiteral("WVZ4 SIGT has invalid ValueType for signal_id %1").arg(int(s.signalId));
            return false;
        }
        const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
        storageIdBySignalId[int(s.signalId)] = storageId;
        const int oldBytes = directIntMapValue(byteWidthByStorageId, storageId, -1);
        if ((s.storageOnly || storageId == int(s.signalId)) && oldBytes > 0 && oldBytes != bytes) {
            error = QStringLiteral("WVZ4 SIGT storage_id %1 has incompatible logical aliases").arg(storageId);
            return false;
        }
        if (s.storageOnly || storageId == int(s.signalId) || oldBytes <= 0) {
            byteWidthByStorageId[storageId] = bytes;
            boolStorageByStorageId[storageId] =
                (s.type == ValueType::Bool && s.bitWidth == 1 && s.bitOffset == 0) ? 1 : 0;
        }
    }

    for (int i = 0; i < sigs.size(); ++i) {
        const SigRec& s = sigs.at(i);
        const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
        const int storageBytes = directIntMapValue(byteWidthByStorageId, storageId, -1);
        if (storageBytes <= 0) {
            error = QStringLiteral("WVZ4 SIGT signal_id %1 references missing storage_id %2")
                .arg(int(s.signalId)).arg(storageId);
            return false;
        }
        if (s.bitOffset + s.bitWidth > u32(storageBytes * 8)) {
            error = QStringLiteral("WVZ4 SIGT signal_id %1 bit range exceeds storage_id %2 capacity")
                .arg(int(s.signalId)).arg(storageId);
            return false;
        }
    }

    return true;
}

bool validateFooterBlockFileRanges(const QVector<BlockIndexRec>& footerBlocks,
                                   quint64 fileSize,
                                   QString& error) {
    for (int i = 0; i < footerBlocks.size(); ++i) {
        const BlockIndexRec& b = footerBlocks.at(i);
        if (b.fileOffset > fileSize || b.fileSize > fileSize - b.fileOffset) {
            error = QStringLiteral("WVZ4 FOOT block %1 file range exceeds file size").arg(b.blockId);
            return false;
        }
    }
    return true;
}

void expandTimeRangeFromFooterBlocks(const QVector<BlockIndexRec>& footerBlocks,
                                     qint64& minTime,
                                     qint64& maxTime) {
    for (int i = 0; i < footerBlocks.size(); ++i) {
        const BlockIndexRec& b = footerBlocks.at(i);
        minTime = qMin(minTime, b.start);
        maxTime = qMax(maxTime, b.end);
    }
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

struct RawLeftAnchorState {
    QVector<WaveSample> samples;
    QVector<quint8> valid;
    QVector<quint8> emitted;

    void reset(int count) {
        samples.resize(count);
        valid.fill(0, count);
        emitted.fill(0, count);
    }
};

quint64 sliceRawBitsForSignal(quint64 rawBits, const WaveSignal& sig) {
    const int offset = qBound(0, sig.bitOffset, 63);
    const int width = qMax(1, sig.width);
    return (rawBits >> offset) & waveBitMaskForWidth(width);
}

WaveLodLevel sliceLodLevelForSignal(const WaveLodLevel& source, const WaveSignal& sig) {
    WaveLodLevel out;
    out.bucketCycles = source.bucketCycles;
    out.validRanges = source.validRanges;
    out.loadedRanges = source.loadedRanges;
    out.samples.reserve(source.samples.size());
    for (const WaveSample& src : source.samples) {
        WaveSample sample = src;
        if (sample.rawFieldsReady && !sample.isZ && !sample.isAbsent) {
            sample.rawBits = sliceRawBitsForSignal(sample.rawBits, sig);
        }
        if (!out.samples.isEmpty() && out.samples.last().time == sample.time) {
            out.samples.last() = std::move(sample);
        } else {
            out.samples.push_back(std::move(sample));
        }
    }

    out.buckets.reserve(source.buckets.size());
    for (const WaveLodBucket& src : source.buckets) {
        WaveLodBucket bucket = src;
        const quint64 first = sliceRawBitsForSignal(src.firstRawBits, sig);
        const quint64 last = sliceRawBitsForSignal(src.lastRawBits, sig);
        const quint64 minV = sliceRawBitsForSignal(src.minRawBits, sig);
        const quint64 maxV = sliceRawBitsForSignal(src.maxRawBits, sig);
        bucket.firstRawBits = first;
        bucket.lastRawBits = last;
        bucket.minRawBits = qMin(qMin(first, last), qMin(minV, maxV));
        bucket.maxRawBits = qMax(qMax(first, last), qMax(minV, maxV));
        bucket.stateMask = src.stateMask & (kWaveLodSeenZ | kWaveLodSeenAbsent);
        if (first == 0 || last == 0 || minV == 0 || maxV == 0) bucket.stateMask |= kWaveLodSeenZero;
        if (first != 0 || last != 0 || minV != 0 || maxV != 0) bucket.stateMask |= kWaveLodSeenNonZero;
        out.buckets.push_back(bucket);
    }
    return out;
}

WaveSample makeDecodedSample(int outputIndex,
                             const char* valueBytes,
                             int byteCount,
                             qint64 sampleTime,
                             const QVector<WaveSignal>& outputSignals) {
    WaveSample sample;
    sample.time = sampleTime;
    sample.isAbsent = false;
    sample.isZ = false;
    sample.rawFieldsReady = true;
    const WaveSignal& sig = outputSignals.at(outputIndex);
    sample.rawBits = sliceRawBitsForSignal(readScalarBitsLE(valueBytes, byteCount), sig);
    if (gDecodedMatchTargets &&
        outputIndex >= 0 && outputIndex < gDecodedMatchTargets->size()) {
        const quint64 targetBits =
            gDecodedMatchTargets->at(outputIndex) & waveBitMaskForWidth(sig.width);
        sample.rawBits = sample.rawBits == targetBits ? 1ull : 0ull;
    }
    // WVZ4 stores fixed-width <=64-bit scalar values.  Do not materialize a
    // QString for every decoded sample; display code formats rawBits on demand.
    sample.value.clear();
    return sample;
}

bool appendDecodedSampleObject(int outputIndex,
                               WaveSample&& sample,
                               const QVector<WaveSignal>& outputSignals,
                               QVector<QVector<WaveSample>>& samplesByOutputIndex,
                               bool compactSamples,
                               QString& error,
                               bool countAgainstBudget = true) {
    if (countAgainstBudget && gDecodedSampleLimit != 0 && gDecodedSampleCount >= gDecodedSampleLimit) {
        error = QStringLiteral("WVZ4 decoded sample limit exceeded (%1). Narrow the selected signals/time range or use LOD.")
            .arg(gDecodedSampleLimit);
        return false;
    }
    if (outputIndex < 0 || outputIndex >= outputSignals.size() || outputIndex >= samplesByOutputIndex.size()) {
        error = QStringLiteral("WVZ4 WDAT sample references invalid output signal index");
        return false;
    }

    if (compactSamples) {
        appendCompactedSample(samplesByOutputIndex[outputIndex], std::move(sample));
    } else if (!samplesByOutputIndex[outputIndex].isEmpty() &&
               samplesByOutputIndex[outputIndex].last().time == sample.time) {
        samplesByOutputIndex[outputIndex].last() = std::move(sample);
    } else {
        samplesByOutputIndex[outputIndex].push_back(std::move(sample));
    }
    if (countAgainstBudget) ++gDecodedSampleCount;
    return true;
}

bool appendDecodedSample(int outputIndex,
                         const char* valueBytes,
                         int byteCount,
                         qint64 sampleTime,
                         const QVector<WaveSignal>& outputSignals,
                         QVector<QVector<WaveSample>>& samplesByOutputIndex,
                         bool compactSamples,
                         QString& error) {
    if (outputIndex < 0 || outputIndex >= outputSignals.size() ||
        outputIndex >= samplesByOutputIndex.size()) {
        error = QStringLiteral("WVZ4 WDAT sample references invalid output signal index");
        return false;
    }
    if (gDecodedMatchTargets &&
        outputIndex < gDecodedMatchTargets->size()) {
        if (gDecodedSampleLimit != 0 &&
            gDecodedSampleCount >= gDecodedSampleLimit) {
            error = QStringLiteral("WVZ4 decoded sample limit exceeded (%1). Narrow the selected signals/time range or use LOD.")
                .arg(gDecodedSampleLimit);
            return false;
        }

        const WaveSignal& signal = outputSignals.at(outputIndex);
        const quint64 rawBits =
            sliceRawBitsForSignal(
                readScalarBitsLE(valueBytes, byteCount), signal);
        const quint64 targetBits =
            gDecodedMatchTargets->at(outputIndex) &
            waveBitMaskForWidth(signal.width);
        const quint64 matched = rawBits == targetBits ? 1ull : 0ull;
        QVector<WaveSample>& rows = samplesByOutputIndex[outputIndex];
        if (!rows.isEmpty()) {
            WaveSample& last = rows.last();
            if (last.time == sampleTime) {
                last.value.clear();
                last.rawBits = matched;
                last.isZ = false;
                last.isAbsent = false;
                last.rawFieldsReady = true;
                ++gDecodedSampleCount;
                return true;
            }
            if (compactSamples && !last.isAbsent && !last.isZ &&
                last.rawFieldsReady && last.rawBits == matched) {
                ++gDecodedSampleCount;
                return true;
            }
        }

        WaveSample sample;
        sample.time = sampleTime;
        sample.rawBits = matched;
        sample.rawFieldsReady = true;
        rows.push_back(std::move(sample));
        ++gDecodedSampleCount;
        return true;
    }
    WaveSample sample = makeDecodedSample(outputIndex, valueBytes, byteCount, sampleTime, outputSignals);
    return appendDecodedSampleObject(outputIndex, std::move(sample), outputSignals,
                                     samplesByOutputIndex, compactSamples, error);
}

bool emitLeftAnchorIfNeeded(int outputIndex,
                            const QVector<WaveSignal>& outputSignals,
                            QVector<QVector<WaveSample>>& samplesByOutputIndex,
                            bool compactSamples,
                            RawLeftAnchorState* leftAnchors,
                            QString& error) {
    if (!leftAnchors) return true;
    if (outputIndex < 0 || outputIndex >= leftAnchors->valid.size()) return true;
    if (!leftAnchors->valid.at(outputIndex) || leftAnchors->emitted.at(outputIndex)) return true;
    WaveSample anchor = leftAnchors->samples.at(outputIndex);
    leftAnchors->emitted[outputIndex] = 1;
    return appendDecodedSampleObject(outputIndex, std::move(anchor), outputSignals,
                                     samplesByOutputIndex, compactSamples, error, false);
}

void storeLeftAnchor(int outputIndex,
                     WaveSample&& sample,
                     RawLeftAnchorState* leftAnchors) {
    if (!leftAnchors) return;
    if (outputIndex < 0 || outputIndex >= leftAnchors->samples.size()) return;
    leftAnchors->samples[outputIndex] = std::move(sample);
    leftAnchors->valid[outputIndex] = 1;
    leftAnchors->emitted[outputIndex] = 0;
}

bool appendDecodedSampleToOutputs(const QVector<int>& outputIndexes,
                                  const char* valueBytes,
                                  int byteCount,
                                  qint64 sampleTime,
                                  const QVector<WaveSignal>& outputSignals,
                                  QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                  bool compactSamples,
                                  QString& error) {
    for (int i = 0; i < outputIndexes.size(); ++i) {
        if (!appendDecodedSample(outputIndexes.at(i), valueBytes, byteCount, sampleTime,
                                 outputSignals, samplesByOutputIndex, compactSamples, error)) {
            return false;
        }
    }
    return true;
}

bool appendImplicitZeroSamplesForSelectedSignals(const QSet<int>& selectedIds,
                                                bool allSelected,
                                                qint64 initialTime,
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
        if (sig.proceduralClock) continue;
        if (outputIndex < 0 || outputIndex >= samplesByOutputIndex.size()) continue;

        WaveSample sample;
        sample.time = initialTime;
        sample.isAbsent = false;
        sample.isZ = false;
        sample.rawBits =
            gDecodedMatchTargets && outputIndex < gDecodedMatchTargets->size()
                ? ((gDecodedMatchTargets->at(outputIndex) &
                    waveBitMaskForWidth(sig.width)) == 0ull ? 1ull : 0ull)
                : 0ull;
        sample.rawFieldsReady = true;
        sample.value.clear();
        appendCompactedSample(samplesByOutputIndex[outputIndex], std::move(sample));
        appendedAny = true;
    }

    if (appendedAny) {
        minTime = qMin(minTime, initialTime);
        maxTime = qMax(maxTime, initialTime);
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
                              bool compactSamples,
                              RawLeftAnchorState* leftAnchors,
                              QString& error,
                              const RawDecodeObserver* observer);

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
                              bool compactSamples,
                              RawLeftAnchorState* leftAnchors,
                              QString& error,
                              const RawDecodeObserver* observer) {
    if (observer && observer->callback &&
        !observer->callback(sampleTime, observer->context)) {
        return false;
    }
    if (windowEnd >= windowStart && sampleTime < windowStart) {
        if (leftAnchors) {
            for (int i = 0; i < outputIndexes.size(); ++i) {
                const int outputIndex = outputIndexes.at(i);
                if (outputIndex < 0 || outputIndex >= outputSignals.size()) continue;
                WaveSample sample = makeDecodedSample(outputIndex, value, byteWidth, sampleTime, outputSignals);
                storeLeftAnchor(outputIndex, std::move(sample), leftAnchors);
            }
        }
        return true;
    }
    if (!sampleInWindow(sampleTime, windowStart, windowEnd)) return true;
    for (int i = 0; i < outputIndexes.size(); ++i) {
        const int outputIndex = outputIndexes.at(i);
        if (!emitLeftAnchorIfNeeded(outputIndex, outputSignals, samplesByOutputIndex,
                                    compactSamples, leftAnchors, error)) {
            return false;
        }
    }
    return appendDecodedSampleToOutputs(outputIndexes, value, byteWidth, sampleTime,
                                        outputSignals, samplesByOutputIndex, compactSamples, error);
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
                        bool compactSamples,
                        RawLeftAnchorState* leftAnchors,
                        QString& error,
                        const RawDecodeObserver* observer) {
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
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
        }
        return true;
    }
    case ValueRecordCodec::ByteMask: {
        if (transitionCount == 0) return true;
        qint64 sampleTime = 0;
        if (!readRecordTime(sampleTime)) return false;
        if (!readFullValue(rr, value, byteWidth, error, "WDAT byte-mask record")) return false;
        if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                      outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
        for (u64 ti = 1; ti < transitionCount; ++ti) {
            if (!readRecordTime(sampleTime)) return false;
            u8 mask = 0;
            if (!rr.readU8(mask)) {
                error = QStringLiteral("WVZ4 WDAT byte-mask record mask truncated");
                return false;
            }
            if (!applyChangedByteMask(rr, value, byteWidth, mask, error, "WDAT byte-mask record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
                                      outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
        for (u64 ti = 1; ti < transitionCount; ++ti) {
            u8 mask = 0;
            if (!rr.readU8(mask)) {
                error = QStringLiteral("WVZ4 WDAT byte-mask-stride record mask truncated");
                return false;
            }
            if (!resolveStrideRecordTime(firstRel, stride, ti, blockStart, blockEnd, sampleTime, error, "WDAT byte-mask-stride record")) return false;
            if (!applyChangedByteMask(rr, value, byteWidth, mask, error, "WDAT byte-mask-stride record")) return false;
            if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
        }
        return true;
    }
    case ValueRecordCodec::NibbleMask: {
        if (transitionCount == 0) return true;
        qint64 sampleTime = 0;
        if (!readRecordTime(sampleTime)) return false;
        if (!readFullValue(rr, value, byteWidth, error, "WDAT nibble-mask record")) return false;
        if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime, windowStart, windowEnd,
                                      outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
            if (haveSecond) {
                if (!applyChangedByteMask(rr, value, byteWidth, static_cast<u8>((packedMask >> 4) & 0x0f), error, "WDAT nibble-mask record")) return false;
                if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime1, windowStart, windowEnd,
                                              outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
                                      outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
                                          outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
            if (ti + 1 < transitionCount) {
                qint64 sampleTime1 = 0;
                if (!resolveStrideRecordTime(firstRel, stride, ti + 1, blockStart, blockEnd, sampleTime1, error, "WDAT nibble-mask-stride record")) return false;
                if (!applyChangedByteMask(rr, value, byteWidth, static_cast<u8>((packedMask >> 4) & 0x0f), error, "WDAT nibble-mask-stride record")) return false;
                if (!appendDecodedRecordValue(outputIndexes, value, byteWidth, sampleTime1, windowStart, windowEnd,
                                              outputSignals, samplesByOutputIndex, compactSamples, leftAnchors, error, observer)) return false;
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
bool decodeRawWaveTile(const QByteArray& rawPayload,
                         u64 expectedBlockId,
                         i64 expectedStart,
                         i64 expectedEnd,
                         u64 expectedSignalChunkId,
                         u64 expectedFirstSignalId,
                         u64 expectedSignalCount,
                         const QSet<int>& selectedIds,
                         bool allSelected,
                         const StorageOutputIndexLookup& outputIndexesByStorageId,
                         const QVector<int>& byteWidthBySignalId,
                         const QVector<WaveSignal>& outputSignals,
                         QVector<QVector<WaveSample>>& samplesByOutputIndex,
                         qint64 windowStart,
                         qint64 windowEnd,
                         qint64& minTime,
                         qint64& maxTime,
                         RawLeftAnchorState* leftAnchors,
                         QString& error,
                         const RawDecodeObserver* observer = nullptr) {
    SpanReader r(rawPayload);

    u64 blockId = 0;
    i64 blockStart = 0;
    i64 blockEnd = 0;
    u64 flags = 0;
    if (!r.readVarUInt(blockId) ||
        !r.readI64(blockStart) ||
        !r.readI64(blockEnd) ||
        !r.readVarUInt(flags)) {
        error = QStringLiteral("WVZ4 WDAT tile header malformed");
        return false;
    }
    if (!validateRawBlockHeader(blockId, blockStart, blockEnd, expectedBlockId, expectedStart, expectedEnd, error)) return false;

    if ((flags & kWdatSignalChunkTile) == 0) {
        error = QStringLiteral("WVZ4 v15 WDAT tile is missing the signal-chunk flag");
        return false;
    }

    const u64 knownFlags = kKnownWdatFlags;
    if ((flags & ~knownFlags) != 0) {
        error = QStringLiteral("WVZ4 WDAT tile has unknown flags: 0x%1").arg(QString::number(flags, 16));
        return false;
    }
    if ((flags & kWdatFixedValueWidth) == 0) {
        error = QStringLiteral("WVZ4 WDAT tile without fixed value width is not supported");
        return false;
    }

    const bool useDeltaTimes = (flags & kWdatDeltaTimes) != 0;
    const bool useSharedTimeTable = (flags & kWdatSharedTimeTable) != 0;
    const bool useSparseSignalRecords = (flags & kWdatSparseSignalRecords) != 0;
    const bool hasValueCodec = (flags & kWdatPerRecordValueCodec) != 0;
    if (useDeltaTimes && useSharedTimeTable) {
        error = QStringLiteral("WVZ4 WDAT tile cannot combine delta-time and shared-time encodings");
        return false;
    }

    u64 signalChunkId = 0;
    u64 firstSignalId64 = 0;
    u64 signalCount64 = 0;
    if (!r.readVarUInt(signalChunkId) ||
        !r.readVarUInt(firstSignalId64) || firstSignalId64 == 0 || firstSignalId64 > u64(std::numeric_limits<int>::max()) ||
        !r.readVarUInt(signalCount64) || signalCount64 == 0 || signalCount64 > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 WDAT tile signal chunk header malformed");
        return false;
    }
    if (signalChunkId != expectedSignalChunkId ||
        firstSignalId64 != expectedFirstSignalId ||
        signalCount64 != expectedSignalCount) {
        error = QStringLiteral("WVZ4 WDAT raw tile header does not match outer WDAT chunk header");
        return false;
    }

    if (firstSignalId64 > u64(std::numeric_limits<int>::max()) - signalCount64 + 1ull) {
        error = QStringLiteral("WVZ4 WDAT tile signal range overflows");
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
            error = QStringLiteral("WVZ4 WDAT shared time table count is invalid");
            return false;
        }
        if (sharedCount > u64(r.remaining())) {
            error = QStringLiteral("WVZ4 WDAT shared time table count exceeds payload size");
            return false;
        }
        sharedTimes.reserve(int(sharedCount));
        u64 prev = 0;
        for (u64 i = 0; i < sharedCount; ++i) {
            u64 delta = 0;
            if (!r.readVarUInt(delta) || delta > std::numeric_limits<u64>::max() - prev) {
                error = QStringLiteral("WVZ4 WDAT shared time table is malformed");
                return false;
            }
            const u64 rel = prev + delta;
            qint64 ignoredTime = 0;
            if (!addRelTimeChecked(blockStart, blockEnd, rel, ignoredTime)) {
                error = QStringLiteral("WVZ4 WDAT shared time is outside the block or overflows");
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
            error = QStringLiteral("WVZ4 WDAT tile offset table count is invalid");
            return false;
        }
        // Every delta-coded offset consumes at least one byte.  Reject impossible
        // counts before reserve(), otherwise a corrupt tile can force a huge allocation.
        if (offsetCount64 > u64(r.remaining())) {
            error = QStringLiteral("WVZ4 WDAT tile offset table count exceeds payload size");
            return false;
        }

        offsets.reserve(int(offsetCount64));
        u64 prevOffset = 0;
        for (u64 i = 0; i < offsetCount64; ++i) {
            u64 delta = 0;
            if (!r.readVarUInt(delta) || delta > std::numeric_limits<u64>::max() - prevOffset) {
                error = QStringLiteral("WVZ4 WDAT tile offset table is malformed");
                return false;
            }
            const u64 off = prevOffset + delta;
            if (off > u64(std::numeric_limits<int>::max())) {
                error = QStringLiteral("WVZ4 WDAT tile record offset is too large");
                return false;
            }
            offsets.push_back(off);
            prevOffset = off;
        }
    }

    u64 recordsBlobSize64 = 0;
    if (!r.readVarUInt(recordsBlobSize64) || recordsBlobSize64 > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 WDAT tile records_blob_size is invalid");
        return false;
    }
    if (recordsBlobSize64 > u64(r.remaining())) {
        error = QStringLiteral("WVZ4 WDAT tile records_blob_size exceeds payload remainder");
        return false;
    }
    if (!useSparseSignalRecords && (offsets.isEmpty() || offsets.first() != 0 || offsets.last() != recordsBlobSize64)) {
        error = QStringLiteral("WVZ4 WDAT tile offset table must start at 0 and end at records_blob_size");
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
        error = QStringLiteral("WVZ4 WDAT tile records_blob truncated");
        return false;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 WDAT tile raw payload has trailing bytes");
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
            const QVector<int>* outputIndexes = outputIndexesByStorageId.value(sid);
            if ((!outputIndexes || outputIndexes->isEmpty()) && !observer) continue;
            static const QVector<int> emptyOutputIndexes;

            SpanReader rr(recordsBlob + int(beginOff64), int(endOff64 - beginOff64));
            if (!decodeSignalRecord(rr, hasValueCodec, useDeltaTimes, useSharedTimeTable, sharedTimes,
                                    blockStart, blockEnd, byteWidth,
                                    outputIndexes ? *outputIndexes : emptyOutputIndexes,
                                    outputSignals, samplesByOutputIndex, windowStart, windowEnd,
                                    true, leftAnchors, error, observer)) {
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
            error = QStringLiteral("WVZ4 WDAT tile record offset range is invalid");
            return false;
        }
        if (endOff64 == beginOff64) continue;

        const int byteWidth = directIntMapValue(byteWidthBySignalId, sid, -1);
        if (byteWidth <= 0 || byteWidth > 8) {
            error = QStringLiteral("WVZ4 WDAT tile references unknown signal_id %1").arg(sid);
            return false;
        }
        const QVector<int>* outputIndexes = outputIndexesByStorageId.value(sid);
        if ((!outputIndexes || outputIndexes->isEmpty()) && !observer) continue;
        static const QVector<int> emptyOutputIndexes;

        SpanReader rr(recordsBlob + int(beginOff64), int(endOff64 - beginOff64));
        if (!decodeSignalRecord(rr, hasValueCodec, useDeltaTimes, useSharedTimeTable, sharedTimes,
                                blockStart, blockEnd, byteWidth,
                                outputIndexes ? *outputIndexes : emptyOutputIndexes,
                                outputSignals, samplesByOutputIndex, windowStart, windowEnd,
                                true, leftAnchors, error, observer)) {
            return false;
        }

        if (!rr.eof()) {
            error = QStringLiteral("WVZ4 WDAT signal record has trailing bytes");
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
                                const QSet<int>& selectedIds,
                                bool allSelected,
                                const StorageOutputIndexLookup& outputIndexesByStorageId,
                                const QVector<int>& byteWidthBySignalId,
                                const QVector<WaveSignal>& outputSignals,
                                QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                qint64 windowStart,
                                qint64 windowEnd,
                                qint64& minTime,
                                qint64& maxTime,
                                QString& error,
                                RawLeftAnchorState* leftAnchors = nullptr,
                                const BlockIndexRec* expectedIndex = nullptr,
                                const RawDecodeObserver* observer = nullptr) {
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

    if (!readVarUIntFromSection(file, sectionEnd, signalChunkId, error, "WDAT outer chunk header") ||
        !readVarUIntFromSection(file, sectionEnd, firstSignalId, error, "WDAT outer chunk header") ||
        firstSignalId == 0 || firstSignalId > u64(std::numeric_limits<int>::max()) ||
        !readVarUIntFromSection(file, sectionEnd, signalCount, error, "WDAT outer chunk header") ||
        signalCount == 0 || signalCount > u64(std::numeric_limits<int>::max())) {
        if (error.isEmpty()) error = QStringLiteral("WVZ4 WDAT outer signal chunk header malformed");
        return false;
    }
    if (firstSignalId > u64(std::numeric_limits<int>::max()) - signalCount + 1ull) {
        error = QStringLiteral("WVZ4 WDAT outer signal chunk range overflows");
        return false;
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
        if (signalChunkId != expectedIndex->signalChunkId ||
            firstSignalId != expectedIndex->firstSignalId ||
            signalCount != expectedIndex->signalCount) {
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

    const bool needLeftAnchor = leftAnchors && windowEnd >= windowStart && start < windowStart;
    bool needDecode = blockOverlapsWindow(start, end, windowStart, windowEnd) || needLeftAnchor;
    if (needDecode) {
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

    return decodeRawWaveTile(raw, blockId, start, end,
                             signalChunkId, firstSignalId, signalCount,
                               selectedIds, allSelected, outputIndexesByStorageId,
                               byteWidthBySignalId, outputSignals, samplesByOutputIndex,
                               windowStart, windowEnd, minTime, maxTime,
                               leftAnchors, error, observer);
}


bool decodeWdatSectionsFromFooterIndex(QFile& file,
                                        const QVector<BlockIndexRec>& footerBlocks,
                                        const QVector<QVector<int>>& blockIndexesByChunk,
                                        u64 signalsPerChunk,
                                        const QSet<int>& selectedIds,
                                        bool allSelected,
                                        const StorageOutputIndexLookup& outputIndexesByStorageId,
                                        const QVector<int>& byteWidthBySignalId,
                                        const QVector<WaveSignal>& outputSignals,
                                        QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                        qint64 windowStart,
                                        qint64 windowEnd,
                                        qint64& minTime,
                                        qint64& maxTime,
                                        RawLeftAnchorState* leftAnchors,
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
            error = QStringLiteral("WVZ4 FOOT chunk index points outside block table");
            return false;
        }
        const BlockIndexRec& b = footerBlocks.at(i);
        const bool needLeftAnchor = leftAnchors && windowEnd >= windowStart && b.start < windowStart;
        if (!blockOverlapsWindow(b.start, b.end, windowStart, windowEnd) && !needLeftAnchor) continue;
        if (b.firstSignalId == 0 || b.signalCount == 0 ||
            b.firstSignalId > u64(std::numeric_limits<int>::max()) ||
            b.signalCount > u64(std::numeric_limits<int>::max()) ||
            b.firstSignalId > u64(std::numeric_limits<int>::max()) - b.signalCount + 1ull) {
            error = QStringLiteral("WVZ4 FOOT block %1 has invalid signal chunk range").arg(b.blockId);
            return false;
        }
        if (!signalRangeIntersectsSelection(selectedIds, allSelected, int(b.firstSignalId), int(b.signalCount))) continue;

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

        if (!decodeWdatSectionStreaming(file, sh, selectedIds, allSelected,
                                        outputIndexesByStorageId, byteWidthBySignalId,
                                        outputSignals, samplesByOutputIndex,
                                        windowStart, windowEnd, minTime, maxTime,
                                        error, leftAnchors, &b)) {
            return false;
        }
    }
    return true;
}

struct ParallelWdatWorkerResult {
    QVector<QVector<WaveSample>> samplesByOutputIndex;
    RawLeftAnchorState leftAnchors;
    qint64 minTime = std::numeric_limits<qint64>::max();
    qint64 maxTime = 0;
};

bool collectFooterBlockIndexesForSelection(const QVector<BlockIndexRec>& footerBlocks,
                                           const QVector<QVector<int>>& blockIndexesByChunk,
                                           u64 signalsPerChunk,
                                           const QSet<int>& selectedIds,
                                           bool allSelected,
                                           qint64 windowStart,
                                           qint64 windowEnd,
                                           QVector<int>& selectedBlockIndexes,
                                           QString& error) {
    selectedBlockIndexes.clear();
    if (selectedIds.isEmpty() && !allSelected) return true;

    QVector<int> candidateBlockIndexes;
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
                candidateBlockIndexes.reserve(candidateBlockIndexes.size() + indexes.size());
                for (int idx : indexes) candidateBlockIndexes.push_back(idx);
            }
        }
        std::sort(candidateBlockIndexes.begin(), candidateBlockIndexes.end());
        candidateBlockIndexes.erase(std::unique(candidateBlockIndexes.begin(), candidateBlockIndexes.end()),
                                    candidateBlockIndexes.end());
    }

    const int loopCount = canUseChunkMap ? candidateBlockIndexes.size() : footerBlocks.size();
    selectedBlockIndexes.reserve(loopCount);
    for (int loopIndex = 0; loopIndex < loopCount; ++loopIndex) {
        const int i = canUseChunkMap ? candidateBlockIndexes.at(loopIndex) : loopIndex;
        if (i < 0 || i >= footerBlocks.size()) {
            error = QStringLiteral("WVZ4 FOOT chunk index points outside block table");
            return false;
        }
        const BlockIndexRec& b = footerBlocks.at(i);
        // A range decode must also recover the exact value entering the
        // window.  A signal can remain unchanged for arbitrarily many WDAT
        // blocks, so selecting only overlapping blocks silently falls back to
        // the implicit zero value.  Keep preceding blocks for the selected
        // signal chunks; the streaming decoder reduces them to one latest
        // left-anchor sample per output signal.
        const bool needLeftAnchor = windowEnd >= windowStart && b.start < windowStart;
        if (!blockOverlapsWindow(b.start, b.end, windowStart, windowEnd) && !needLeftAnchor) continue;
        if (b.firstSignalId == 0 || b.signalCount == 0 ||
            b.firstSignalId > u64(std::numeric_limits<int>::max()) ||
            b.signalCount > u64(std::numeric_limits<int>::max()) ||
            b.firstSignalId > u64(std::numeric_limits<int>::max()) - b.signalCount + 1ull) {
            error = QStringLiteral("WVZ4 FOOT block %1 has invalid signal chunk range").arg(b.blockId);
            return false;
        }
        if (!signalRangeIntersectsSelection(selectedIds, allSelected, int(b.firstSignalId), int(b.signalCount))) {
            continue;
        }
        selectedBlockIndexes.push_back(i);
    }
    return true;
}

bool decodeWdatSectionsFromFooterIndexParallel(const QString& filePath,
                                               const QVector<BlockIndexRec>& footerBlocks,
                                               const QVector<QVector<int>>& blockIndexesByChunk,
                                               u64 signalsPerChunk,
                                               const QSet<int>& selectedIds,
                                               bool allSelected,
                                               const StorageOutputIndexLookup& outputIndexesByStorageId,
                                               const QVector<int>& byteWidthBySignalId,
                                               const QVector<WaveSignal>& outputSignals,
                                               QVector<QVector<WaveSample>>& samplesByOutputIndex,
                                               qint64 windowStart,
                                               qint64 windowEnd,
                                               qint64& minTime,
                                               qint64& maxTime,
                                               quint64 maxDecodedSamples,
                                               QString& error,
                                               const WaveParser4Reader::LoadProgressCallback& progress) {
    if (selectedIds.isEmpty() || allSelected) {
        if (progress) progress(0, 1);
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("Cannot open WVZ4 file: %1").arg(filePath);
            return false;
        }
        const bool ok = decodeWdatSectionsFromFooterIndex(
            file, footerBlocks, blockIndexesByChunk, signalsPerChunk,
            selectedIds, allSelected, outputIndexesByStorageId, byteWidthBySignalId,
            outputSignals, samplesByOutputIndex, windowStart, windowEnd, minTime,
            maxTime, nullptr, error);
        if (ok && progress) progress(1, 1);
        return ok;
    }

    QVector<int> selectedBlockIndexes;
    if (!collectFooterBlockIndexesForSelection(footerBlocks, blockIndexesByChunk, signalsPerChunk,
                                               selectedIds, allSelected,
                                               windowStart, windowEnd,
                                               selectedBlockIndexes, error)) {
        return false;
    }
    if (selectedBlockIndexes.isEmpty()) {
        if (progress) progress(1, 1);
        return true;
    }
    const quint64 totalBlocks = quint64(selectedBlockIndexes.size());
    if (progress) progress(0, totalBlocks);

    const unsigned hw = std::thread::hardware_concurrency();
    const int detectedWorkers = int(hw == 0 ? 4 : hw);
    bool configuredWorkerCountOk = false;
    const int configuredWorkerCount =
        qEnvironmentVariable("WV_VIEWER_RAW_DECODE_WORKERS").toInt(&configuredWorkerCountOk);
    const int desiredWorkerCount = configuredWorkerCountOk
        ? qMax(1, configuredWorkerCount)
        : qMin(detectedWorkers, 4);
    const int workerCount =
        qBound(1, qMin(desiredWorkerCount, selectedBlockIndexes.size()), 32);
    if (workerCount <= 1 || selectedBlockIndexes.size() < 4) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("Cannot open WVZ4 file: %1").arg(filePath);
            return false;
        }
        RawLeftAnchorState leftAnchors;
        leftAnchors.reset(outputSignals.size());
        if (!decodeWdatSectionsFromFooterIndex(file, footerBlocks, blockIndexesByChunk,
                                               signalsPerChunk,
                                               selectedIds, allSelected,
                                               outputIndexesByStorageId, byteWidthBySignalId,
                                               outputSignals, samplesByOutputIndex,
                                               windowStart, windowEnd, minTime, maxTime,
                                               &leftAnchors, error)) {
            return false;
        }
        for (int outputIndex = 0; outputIndex < outputSignals.size(); ++outputIndex) {
            if (!emitLeftAnchorIfNeeded(outputIndex, outputSignals, samplesByOutputIndex,
                                        true, &leftAnchors, error)) {
                return false;
            }
        }
        if (progress) progress(totalBlocks, totalBlocks);
        return true;
    }

    std::vector<ParallelWdatWorkerResult> workerResults;
    workerResults.resize(std::size_t(workerCount));
    for (ParallelWdatWorkerResult& result : workerResults) {
        result.samplesByOutputIndex.resize(outputSignals.size());
        result.leftAnchors.reset(outputSignals.size());
    }

    std::atomic<bool> failed(false);
    std::atomic<quint64> completedBlocks(0);
    std::mutex errorMutex;
    std::mutex progressMutex;
    std::condition_variable progressCondition;
    QString firstError;
    auto setError = [&](const QString& message) {
        bool expected = false;
        if (failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(errorMutex);
            firstError = message;
        }
        progressCondition.notify_one();
    };

    std::vector<std::thread> workers;
    workers.reserve(std::size_t(workerCount));
    const QVector<quint64>* const decodedMatchTargets =
        gDecodedMatchTargets;
    for (int workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers.emplace_back([&, workerIndex]() {
            DecodedMatchTargetScope matchTargetScope(decodedMatchTargets);
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly)) {
                setError(QStringLiteral("Cannot open WVZ4 file: %1").arg(filePath));
                return;
            }

            ParallelWdatWorkerResult& local = workerResults[std::size_t(workerIndex)];
            DecodedSampleBudgetScope localBudget(0);
            QString localError;
            const int begin = (selectedBlockIndexes.size() * workerIndex) / workerCount;
            const int end = (selectedBlockIndexes.size() * (workerIndex + 1)) / workerCount;
            for (int j = begin; j < end; ++j) {
                if (failed.load(std::memory_order_acquire)) return;
                const int blockIndex = selectedBlockIndexes.at(j);
                if (blockIndex < 0 || blockIndex >= footerBlocks.size()) {
                    setError(QStringLiteral("WVZ4 selected block index is out of range"));
                    return;
                }
                const BlockIndexRec& b = footerBlocks.at(blockIndex);
                if (b.fileOffset > u64(std::numeric_limits<qint64>::max()) ||
                    !file.seek(qint64(b.fileOffset))) {
                    setError(QStringLiteral("WVZ4 failed to seek to WDAT block %1 from FOOT index").arg(b.blockId));
                    return;
                }

                SectionHeader sh;
                localError.clear();
                if (!readSectionHeader(file, sh, localError)) {
                    if (localError.isEmpty()) localError = QStringLiteral("WVZ4 FOOT block %1 points past end of file").arg(b.blockId);
                    setError(localError);
                    return;
                }
                if (sh.tag != "WDAT") {
                    setError(QStringLiteral("WVZ4 FOOT block %1 does not point to a WDAT section").arg(b.blockId));
                    return;
                }
                const qint64 sectionStart = sh.payloadOffset - 12;
                if (sectionStart < 0 || u64(sectionStart) != b.fileOffset ||
                    sh.size > u64(std::numeric_limits<qint64>::max() - 12) ||
                    b.fileSize != sh.size + 12ull) {
                    setError(QStringLiteral("WVZ4 FOOT block %1 section size mismatch").arg(b.blockId));
                    return;
                }

                if (!decodeWdatSectionStreaming(file, sh,
                                                selectedIds, allSelected,
                                                outputIndexesByStorageId,
                                                byteWidthBySignalId,
                                                outputSignals,
                                                local.samplesByOutputIndex,
                                                windowStart,
                                                windowEnd,
                                                local.minTime,
                                                local.maxTime,
                                                localError,
                                                &local.leftAnchors,
                                                &b)) {
                    setError(localError);
                    return;
                }
                const quint64 completed =
                    completedBlocks.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (completed == totalBlocks) progressCondition.notify_one();
            }
            for (int outputIndex = 0; outputIndex < outputSignals.size(); ++outputIndex) {
                if (!emitLeftAnchorIfNeeded(outputIndex,
                                            outputSignals,
                                            local.samplesByOutputIndex,
                                            true,
                                            &local.leftAnchors,
                                            localError)) {
                    setError(localError);
                    return;
                }
            }
        });
    }

    quint64 lastReportedBlocks = 0;
    if (progress) {
        while (!failed.load(std::memory_order_acquire) &&
               completedBlocks.load(std::memory_order_acquire) < totalBlocks) {
            std::unique_lock<std::mutex> lock(progressMutex);
            progressCondition.wait_for(
                lock, std::chrono::milliseconds(50), [&]() {
                    return failed.load(std::memory_order_acquire) ||
                           completedBlocks.load(std::memory_order_acquire) == totalBlocks;
                });
            lock.unlock();
            const quint64 completed =
                completedBlocks.load(std::memory_order_acquire);
            if (completed != lastReportedBlocks) {
                progress(completed, totalBlocks);
                lastReportedBlocks = completed;
            }
        }
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    if (failed.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = firstError.isEmpty() ? QStringLiteral("WVZ4 parallel WDAT decode failed") : firstError;
        return false;
    }
    if (progress && lastReportedBlocks != totalBlocks) {
        progress(totalBlocks, totalBlocks);
    }

    quint64 decodedSamples = 0;
    for (const ParallelWdatWorkerResult& result : workerResults) {
        if (result.minTime != std::numeric_limits<qint64>::max()) {
            minTime = qMin(minTime, result.minTime);
            maxTime = qMax(maxTime, result.maxTime);
        }
        for (int i = 0; i < result.samplesByOutputIndex.size(); ++i) {
            decodedSamples += quint64(result.samplesByOutputIndex.at(i).size());
        }
    }
    if (maxDecodedSamples != 0 && decodedSamples > maxDecodedSamples) {
        error = QStringLiteral("WVZ4 decoded sample limit exceeded (%1). Narrow the selected signals/time range or use LOD.")
            .arg(maxDecodedSamples);
        return false;
    }

    for (int outputIndex = 0; outputIndex < samplesByOutputIndex.size(); ++outputIndex) {
        int extra = 0;
        for (const ParallelWdatWorkerResult& result : workerResults) {
            if (outputIndex < result.samplesByOutputIndex.size()) {
                extra += result.samplesByOutputIndex.at(outputIndex).size();
            }
        }
        if (extra == 0) continue;

        QVector<WaveSample> compacted;
        compacted.reserve(samplesByOutputIndex.at(outputIndex).size() + extra);
        for (WaveSample& sample : samplesByOutputIndex[outputIndex]) {
            appendCompactedSample(compacted, std::move(sample));
        }
        for (ParallelWdatWorkerResult& result : workerResults) {
            if (outputIndex >= result.samplesByOutputIndex.size()) continue;
            QVector<WaveSample>& localSamples = result.samplesByOutputIndex[outputIndex];
            if (localSamples.isEmpty()) continue;

            // Each worker already emits a time-ordered, compacted sequence for
            // its contiguous block range. Only the seam between two workers
            // can contain an equal-time replacement or an unchanged value.
            // Rechecking every decoded sample here used to duplicate the
            // hottest comparison from the decode loop.
            int first = 0;
            if (!compacted.isEmpty()) {
                if (compacted.last().time == localSamples.first().time) {
                    compacted.last() = std::move(localSamples.first());
                    first = 1;
                } else if (waveSamplesEquivalent(compacted.last(), localSamples.first())) {
                    first = 1;
                }
            }
            for (int i = first; i < localSamples.size(); ++i) {
                compacted.push_back(std::move(localSamples[i]));
            }
        }
        samplesByOutputIndex[outputIndex] = std::move(compacted);
    }

    return true;
}

void sortAndDedupLodSamples(QVector<QVector<WaveLodLevel>>& lodLevelsByStorageId) {
    for (int storage = 0; storage < lodLevelsByStorageId.size(); ++storage) {
        QVector<WaveLodLevel>& levels = lodLevelsByStorageId[storage];
        for (int levelIndex = 0; levelIndex < levels.size(); ++levelIndex) {
            QVector<WaveSample>& samples = levels[levelIndex].samples;
            if (samples.size() > 1) {
                std::sort(samples.begin(), samples.end(), [](const WaveSample& a, const WaveSample& b) {
                    return a.time < b.time;
                });
                int write = 1;
                for (int read = 1; read < samples.size(); ++read) {
                    if (samples.at(read).time == samples.at(write - 1).time) {
                        samples[write - 1] = samples.at(read);
                    } else {
                        if (write != read) samples[write] = samples.at(read);
                        ++write;
                    }
                }
                samples.resize(write);
            }

            auto sortAndMergeRanges = [](QVector<WaveLodValidRange>& ranges) {
                if (ranges.isEmpty()) return;
                std::sort(ranges.begin(), ranges.end(), [](const WaveLodValidRange& a, const WaveLodValidRange& b) {
                    return a.start < b.start;
                });
                int rangeWrite = 0;
                for (int read = 0; read < ranges.size(); ++read) {
                    WaveLodValidRange range = ranges.at(read);
                    if (range.end <= range.start) continue;
                    if (rangeWrite > 0 && range.start <= ranges.at(rangeWrite - 1).end) {
                        if (range.end > ranges[rangeWrite - 1].end) ranges[rangeWrite - 1].end = range.end;
                    } else {
                        if (rangeWrite != read) ranges[rangeWrite] = range;
                        ++rangeWrite;
                    }
                }
                ranges.resize(rangeWrite);
            };

            sortAndMergeRanges(levels[levelIndex].validRanges);
            sortAndMergeRanges(levels[levelIndex].loadedRanges);
        }
    }
}

void sortAndCompactWaveSamples(QVector<WaveSample>& samples) {
    if (samples.size() <= 1) return;
    std::stable_sort(samples.begin(), samples.end(), [](const WaveSample& a, const WaveSample& b) {
        return a.time < b.time;
    });
    QVector<WaveSample> compacted;
    compacted.reserve(samples.size());
    for (WaveSample& sample : samples) {
        appendCompactedSample(compacted, std::move(sample));
    }
    samples = std::move(compacted);
}

void sortAndDedupWaveSamplesByTime(QVector<WaveSample>& samples) {
    if (samples.size() <= 1) return;
    std::stable_sort(samples.begin(), samples.end(), [](const WaveSample& a, const WaveSample& b) {
        return a.time < b.time;
    });
    int write = 1;
    for (int read = 1; read < samples.size(); ++read) {
        if (samples.at(read).time == samples.at(write - 1).time) {
            samples[write - 1] = samples.at(read);
        } else {
            if (write != read) samples[write] = samples.at(read);
            ++write;
        }
    }
    samples.resize(write);
}

QVector<WaveLodValidRange> normalizedLodRanges(QVector<WaveLodValidRange> ranges) {
    if (ranges.isEmpty()) return ranges;
    std::sort(ranges.begin(), ranges.end(), [](const WaveLodValidRange& a, const WaveLodValidRange& b) {
        return a.start < b.start;
    });
    int write = 0;
    for (int read = 0; read < ranges.size(); ++read) {
        WaveLodValidRange range = ranges.at(read);
        if (range.end <= range.start) continue;
        if (write > 0 && range.start <= ranges.at(write - 1).end) {
            if (range.end > ranges[write - 1].end) ranges[write - 1].end = range.end;
        } else {
            if (write != read) ranges[write] = range;
            ++write;
        }
    }
    ranges.resize(write);
    return ranges;
}

QVector<WaveLodValidRange> intersectLodRangeSets(QVector<WaveLodValidRange> a,
                                                 QVector<WaveLodValidRange> b) {
    a = normalizedLodRanges(std::move(a));
    b = normalizedLodRanges(std::move(b));
    QVector<WaveLodValidRange> out;
    int ai = 0;
    int bi = 0;
    while (ai < a.size() && bi < b.size()) {
        const qint64 start = qMax(a.at(ai).start, b.at(bi).start);
        const qint64 end = qMin(a.at(ai).end, b.at(bi).end);
        if (end > start) {
            WaveLodValidRange range;
            range.start = start;
            range.end = end;
            out.push_back(range);
        }
        if (a.at(ai).end < b.at(bi).end) ++ai;
        else ++bi;
    }
    return out;
}

QVector<WaveLodValidRange> unionLodRangeSets(QVector<WaveLodValidRange> a,
                                             QVector<WaveLodValidRange> b) {
    a = normalizedLodRanges(std::move(a));
    b = normalizedLodRanges(std::move(b));
    a.reserve(a.size() + b.size());
    for (const WaveLodValidRange& range : b) a.push_back(range);
    return normalizedLodRanges(std::move(a));
}

QVector<WaveLodValidRange> combineResidualCoverage(QVector<WaveLodValidRange> current,
                                                   QVector<WaveLodValidRange> coarser) {
    return unionLodRangeSets(std::move(current), std::move(coarser));
}

void makeResidualLodLevelsCumulative(QVector<QVector<WaveLodLevel>>& lodLevelsByStorageId) {
    for (int storage = 0; storage < lodLevelsByStorageId.size(); ++storage) {
        QVector<WaveLodLevel>& levels = lodLevelsByStorageId[storage];
        QVector<WaveSample> coarserSamples;
        QVector<WaveLodValidRange> coarserValidRanges;
        QVector<WaveLodValidRange> coarserLoadedRanges;
        for (int levelIndex = levels.size() - 1; levelIndex >= 0; --levelIndex) {
            WaveLodLevel& level = levels[levelIndex];
            QVector<WaveSample> mergedSamples;
            mergedSamples.reserve(level.samples.size() + coarserSamples.size());
            for (const WaveSample& sample : level.samples) mergedSamples.push_back(sample);
            for (const WaveSample& sample : coarserSamples) mergedSamples.push_back(sample);
            sortAndCompactWaveSamples(mergedSamples);
            level.samples = std::move(mergedSamples);
            level.validRanges = combineResidualCoverage(level.validRanges, coarserValidRanges);
            level.loadedRanges = combineResidualCoverage(level.loadedRanges, coarserLoadedRanges);

            coarserSamples = level.samples;
            coarserValidRanges = level.validRanges;
            coarserLoadedRanges = level.loadedRanges;
        }
    }
}

void appendResidualLodToRawSamples(const QVector<QVector<WaveLodLevel>>& lodLevelsByStorageId,
                                   const QSet<int>& selectedSignalIds,
                                   bool allSelectedSignalIds,
                                   const QVector<WaveSignal>& outputSignals,
                                   QVector<QVector<WaveSample>>& samplesByOutputIndex) {
    for (int outputIndex = 0; outputIndex < outputSignals.size() && outputIndex < samplesByOutputIndex.size(); ++outputIndex) {
        const WaveSignal& sig = outputSignals.at(outputIndex);
        if (!allSelectedSignalIds && !selectedSignalIds.contains(sig.signalId)) continue;
        const int storageId = sig.storageId > 0 ? sig.storageId : sig.signalId;
        if (storageId <= 0 || storageId >= lodLevelsByStorageId.size()) continue;
        QVector<WaveSample>& samples = samplesByOutputIndex[outputIndex];
        const QVector<WaveLodLevel>& storageLevels = lodLevelsByStorageId.at(storageId);
        for (const WaveLodLevel& storageLevel : storageLevels) {
            if (storageLevel.samples.isEmpty()) continue;
            WaveLodLevel sliced = sliceLodLevelForSignal(storageLevel, sig);
            samples.reserve(samples.size() + sliced.samples.size());
            for (WaveSample& sample : sliced.samples) samples.push_back(std::move(sample));
        }
        sortAndDedupWaveSamplesByTime(samples);
    }
}

bool decodeLodzChunksFromFooterIndex(QFile& file,
                                      const QVector<LodChunkIndexRec>& lodChunks,
                                      u64 signalsPerChunk,
                                      const QSet<int>& selectedStorageIds,
                                      bool allSelected,
                                      const QVector<int>& byteWidthByStorageId,
                                      QVector<QVector<WaveLodLevel>>& lodLevelsByStorageId,
                                      qint64 windowStart,
                                      qint64 windowEnd,
                                      qint64 targetBucketCycles,
                                      bool exactBucketOnly,
                                      int onlyChunkIndex,
                                      QString& error) {
    if (lodChunks.isEmpty()) return true;
    if (selectedStorageIds.isEmpty() && !allSelected) return true;
    const bool filterByTime = (windowEnd > windowStart) &&
        (windowStart != 0 || windowEnd != std::numeric_limits<qint64>::max());
    qint64 selectedBucketCycles = 0;
    if (targetBucketCycles > 0 && onlyChunkIndex >= 0) {
        if (onlyChunkIndex >= lodChunks.size() ||
            lodChunks.at(onlyChunkIndex).bucketCycles != targetBucketCycles) {
            error = QStringLiteral("WVZ4 requested LOD cache chunk is invalid");
            return false;
        }
        selectedBucketCycles = targetBucketCycles;
    } else if (targetBucketCycles > 0) {
        for (const LodChunkIndexRec& idx : lodChunks) {
            if (idx.bucketCycles <= 0 ||
                (exactBucketOnly ? idx.bucketCycles != targetBucketCycles
                                 : idx.bucketCycles > targetBucketCycles)) continue;
            if (filterByTime && (idx.end <= windowStart || idx.start >= windowEnd)) continue;
            if (!allSelected && signalsPerChunk > 0 && !selectedStorageIds.isEmpty()) {
                const u64 first64 = idx.signalChunkId * signalsPerChunk + 1ull;
                if (first64 > u64(std::numeric_limits<int>::max())) continue;
                const u64 maxCount = u64(std::numeric_limits<int>::max()) - first64 + 1ull;
                const u64 count64 = qMin<u64>(signalsPerChunk, maxCount);
                if (count64 == 0 ||
                    !signalRangeIntersectsSelection(selectedStorageIds, false, int(first64), int(count64))) {
                    continue;
                }
            }
            selectedBucketCycles = qMax(selectedBucketCycles, idx.bucketCycles);
        }
        if (selectedBucketCycles <= 0) return true;
    }

    const u64 fileSize64 = u64(file.size());
    for (int ci = 0; ci < lodChunks.size(); ++ci) {
        if (onlyChunkIndex >= 0 && ci != onlyChunkIndex) continue;
        const LodChunkIndexRec& idx = lodChunks.at(ci);
        if (filterByTime && (idx.end <= windowStart || idx.start >= windowEnd)) {
            continue;
        }
        if (selectedBucketCycles > 0 &&
            (exactBucketOnly ? idx.bucketCycles != selectedBucketCycles
                             : idx.bucketCycles < selectedBucketCycles)) {
            continue;
        }
        if (!allSelected && signalsPerChunk > 0 && !selectedStorageIds.isEmpty()) {
            const u64 first64 = idx.signalChunkId * signalsPerChunk + 1ull;
            if (first64 > u64(std::numeric_limits<int>::max())) continue;
            const u64 maxCount = u64(std::numeric_limits<int>::max()) - first64 + 1ull;
            const u64 count64 = qMin<u64>(signalsPerChunk, maxCount);
            if (count64 == 0 ||
                !signalRangeIntersectsSelection(selectedStorageIds, false, int(first64), int(count64))) {
                continue;
            }
        }

        if (idx.fileOffset > fileSize64 || idx.fileSize > fileSize64 - idx.fileOffset ||
            idx.fileOffset > u64(std::numeric_limits<qint64>::max())) {
            error = QStringLiteral("WVZ4 FOOT LODZ chunk %1 file range exceeds file size").arg(idx.chunkId);
            return false;
        }
        if (!file.seek(qint64(idx.fileOffset))) {
            error = QStringLiteral("WVZ4 failed to seek to LODZ chunk %1").arg(idx.chunkId);
            return false;
        }

        SectionHeader sh;
        if (!readSectionHeader(file, sh, error)) {
            if (error.isEmpty()) error = QStringLiteral("WVZ4 LODZ chunk %1 points past end of file").arg(idx.chunkId);
            return false;
        }
        if (sh.tag != "LODZ") {
            error = QStringLiteral("WVZ4 FOOT LODZ chunk %1 does not point to a LODZ section").arg(idx.chunkId);
            return false;
        }
        const qint64 sectionStart = sh.payloadOffset - 12;
        if (sectionStart < 0 || u64(sectionStart) != idx.fileOffset ||
            sh.size > u64(std::numeric_limits<qint64>::max() - 12) ||
            idx.fileSize != sh.size + 12ull) {
            error = QStringLiteral("WVZ4 FOOT LODZ chunk %1 section size mismatch").arg(idx.chunkId);
            return false;
        }

        QByteArray payload;
        if (!readSectionPayload(file, sh, payload, error)) return false;
        SpanReader r(payload);
        u64 chunkId = 0;
        u64 levelIndex = 0;
        u64 signalChunkId = 0;
        i64 startCycle = 0;
        i64 endCycle = 0;
        u8 compByte = 0;
        u64 rawSize = 0;
        u64 encodedSize = 0;
        const char* encodedBytes = nullptr;
        if (!r.readVarUInt(chunkId) ||
            !r.readVarUInt(levelIndex) ||
            !r.readVarUInt(signalChunkId) ||
            !r.readI64(startCycle) ||
            !r.readI64(endCycle) ||
            !r.readU8(compByte) ||
            !r.readVarUInt(rawSize) ||
            !r.readVarUInt(encodedSize) ||
            encodedSize > u64(std::numeric_limits<int>::max()) ||
            !r.readBytes(encodedBytes, int(encodedSize)) ||
            !r.eof()) {
            error = QStringLiteral("WVZ4 LODZ chunk %1 header is malformed").arg(idx.chunkId);
            return false;
        }
        if (chunkId != idx.chunkId || levelIndex != idx.levelIndex ||
            signalChunkId != idx.signalChunkId || startCycle != idx.start ||
            endCycle != idx.end || rawSize != idx.rawSize ||
            Compression(compByte) != idx.compression || !isValidCompression(Compression(compByte))) {
            error = QStringLiteral("WVZ4 LODZ chunk %1 header/index mismatch").arg(idx.chunkId);
            return false;
        }

        QByteArray encoded(encodedBytes, int(encodedSize));
        QByteArray raw = decompressBlockPayload(encoded, Compression(compByte), rawSize, error);
        if (!error.isEmpty()) return false;

        SpanReader rr(raw);
        u64 storageRecordCount = 0;
        if (!rr.readVarUInt(storageRecordCount) || storageRecordCount != idx.storageCount) {
            error = QStringLiteral("WVZ4 LODZ chunk %1 storage count mismatch").arg(idx.chunkId);
            return false;
        }
        u64 decodedRecordCount = 0;
        for (u64 si = 0; si < storageRecordCount; ++si) {
            u64 storageId64 = 0;
            u64 byteWidth64 = 0;
            u64 validRangeCount = 0;
            u64 recordCount = 0;
            u64 streamSize64 = 0;
            const char* streamPayload = nullptr;
            if (!rr.readVarUInt(storageId64) || storageId64 == 0 ||
                storageId64 > u64(std::numeric_limits<int>::max()) ||
                !rr.readVarUInt(byteWidth64) || byteWidth64 == 0 || byteWidth64 > 8) {
                error = QStringLiteral("WVZ4 LODZ chunk %1 storage record is malformed").arg(idx.chunkId);
                return false;
            }

            QVector<WaveLodValidRange> validRanges;
            if (!rr.readVarUInt(validRangeCount) ||
                validRangeCount > 1000000ull ||
                validRangeCount > u64(std::numeric_limits<int>::max())) {
                error = QStringLiteral("WVZ4 LODZ chunk %1 valid-range table is malformed").arg(idx.chunkId);
                return false;
            }
            validRanges.reserve(int(validRangeCount));
            for (u64 ri = 0; ri < validRangeCount; ++ri) {
                i64 validStart = 0;
                i64 validEnd = 0;
                if (!rr.readI64(validStart) || !rr.readI64(validEnd) || validEnd <= validStart) {
                    error = QStringLiteral("WVZ4 LODZ chunk %1 has invalid LOD valid range").arg(idx.chunkId);
                    return false;
                }
                WaveLodValidRange range;
                range.start = validStart;
                range.end = validEnd;
                validRanges.push_back(range);
            }

            if (!rr.readVarUInt(recordCount) ||
                recordCount > u64(std::numeric_limits<int>::max()) ||
                !rr.readVarUInt(streamSize64) ||
                streamSize64 > u64(std::numeric_limits<int>::max()) ||
                !rr.readBytes(streamPayload, int(streamSize64))) {
                error = QStringLiteral("WVZ4 LODZ chunk %1 storage record is malformed").arg(idx.chunkId);
                return false;
            }

            const int storageId = int(storageId64);
            const int byteWidth = int(byteWidth64);
            const int expectedByteWidth = directIntMapValue(byteWidthByStorageId, storageId, byteWidth);
            if (expectedByteWidth != byteWidth) {
                error = QStringLiteral("WVZ4 LODZ byte width mismatch for storage_id %1").arg(storageId);
                return false;
            }

            QVector<WaveSample> decodedSamples;
            if (!decodeLodTransitionStreamPayload(streamPayload, int(streamSize64), byteWidth,
                                                  decodedSamples, error)) {
                return false;
            }
            if (decodedSamples.size() > int(recordCount)) {
                error = QStringLiteral("WVZ4 LODZ chunk %1 record count mismatch").arg(idx.chunkId);
                return false;
            }
            decodedRecordCount += recordCount;

            if (lodLevelsByStorageId.size() <= storageId) lodLevelsByStorageId.resize(storageId + 1);
            QVector<WaveLodLevel>& storageLevels = lodLevelsByStorageId[storageId];
            if (storageLevels.size() <= int(idx.levelIndex)) storageLevels.resize(int(idx.levelIndex) + 1);
            WaveLodLevel& level = storageLevels[int(idx.levelIndex)];
            level.bucketCycles = idx.bucketCycles;
            if (idx.end > idx.start) {
                WaveLodValidRange loadedRange;
                loadedRange.start = idx.start;
                loadedRange.end = idx.end;
                level.loadedRanges.push_back(loadedRange);
            }
            if (!validRanges.isEmpty()) {
                level.validRanges.reserve(level.validRanges.size() + validRanges.size());
                for (const WaveLodValidRange& range : validRanges) level.validRanges.push_back(range);
            }
            level.samples.reserve(level.samples.size() + decodedSamples.size());
            for (const WaveSample& sample : decodedSamples) level.samples.push_back(sample);
        }
        if (!rr.eof() || decodedRecordCount != idx.recordCount) {
            error = QStringLiteral("WVZ4 LODZ chunk %1 payload has trailing bytes or record count mismatch").arg(idx.chunkId);
            return false;
        }
    }

    sortAndDedupLodSamples(lodLevelsByStorageId);
    return true;
}

void appendDecodedLodLevels(QVector<QVector<WaveLodLevel>>& target,
                            QVector<QVector<WaveLodLevel>>&& source) {
    if (target.size() < source.size()) target.resize(source.size());
    for (int storageId = 0; storageId < source.size(); ++storageId) {
        QVector<WaveLodLevel>& sourceLevels = source[storageId];
        if (sourceLevels.isEmpty()) continue;
        QVector<WaveLodLevel>& targetLevels = target[storageId];
        if (targetLevels.size() < sourceLevels.size()) targetLevels.resize(sourceLevels.size());
        for (int levelIndex = 0; levelIndex < sourceLevels.size(); ++levelIndex) {
            WaveLodLevel& sourceLevel = sourceLevels[levelIndex];
            if (sourceLevel.bucketCycles <= 0 && sourceLevel.samples.isEmpty() &&
                sourceLevel.buckets.isEmpty() && sourceLevel.validRanges.isEmpty() &&
                sourceLevel.loadedRanges.isEmpty()) {
                continue;
            }
            WaveLodLevel& targetLevel = targetLevels[levelIndex];
            if (targetLevel.bucketCycles <= 0) targetLevel.bucketCycles = sourceLevel.bucketCycles;
            targetLevel.samples.reserve(targetLevel.samples.size() + sourceLevel.samples.size());
            for (WaveSample& sample : sourceLevel.samples) {
                targetLevel.samples.push_back(std::move(sample));
            }
            targetLevel.buckets.reserve(targetLevel.buckets.size() + sourceLevel.buckets.size());
            for (WaveLodBucket& bucket : sourceLevel.buckets) {
                targetLevel.buckets.push_back(std::move(bucket));
            }
            targetLevel.validRanges += sourceLevel.validRanges;
            targetLevel.loadedRanges += sourceLevel.loadedRanges;
        }
    }
}

bool decodeLodzChunksFromFooterIndexParallel(const QString& filePath,
                                              const QVector<LodChunkIndexRec>& lodChunks,
                                              u64 signalsPerChunk,
                                              const QSet<int>& selectedStorageIds,
                                              bool allSelected,
                                              const QVector<int>& byteWidthByStorageId,
                                              QVector<QVector<WaveLodLevel>>& lodLevelsByStorageId,
                                              qint64 windowStart,
                                              qint64 windowEnd,
                                              qint64 targetBucketCycles,
                                              bool exactBucketOnly,
                                              int onlyChunkIndex,
                                              QString& error) {
    // Single-block cache fills and full-layout loads keep the compact serial
    // path. Interactive selected-signal range loads can fan out over many
    // independent compressed LODZ chunks, each backed by its own QFile.
    if (onlyChunkIndex >= 0 || allSelected || lodChunks.size() < 4) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("Cannot open WVZ4 file: %1").arg(filePath);
            return false;
        }
        return decodeLodzChunksFromFooterIndex(file, lodChunks, signalsPerChunk,
                                               selectedStorageIds, allSelected,
                                               byteWidthByStorageId, lodLevelsByStorageId,
                                               windowStart, windowEnd, targetBucketCycles,
                                               exactBucketOnly, onlyChunkIndex, error);
    }

    const bool filterByTime = (windowEnd > windowStart) &&
        (windowStart != 0 || windowEnd != std::numeric_limits<qint64>::max());
    qint64 selectedBucketCycles = 0;
    if (targetBucketCycles > 0) {
        for (const LodChunkIndexRec& idx : lodChunks) {
            if (idx.bucketCycles <= 0 ||
                (exactBucketOnly ? idx.bucketCycles != targetBucketCycles
                                 : idx.bucketCycles > targetBucketCycles)) continue;
            if (filterByTime && (idx.end <= windowStart || idx.start >= windowEnd)) continue;
            if (signalsPerChunk > 0 && !selectedStorageIds.isEmpty()) {
                const u64 first64 = idx.signalChunkId * signalsPerChunk + 1ull;
                if (first64 > u64(std::numeric_limits<int>::max())) continue;
                const u64 maxCount = u64(std::numeric_limits<int>::max()) - first64 + 1ull;
                const u64 count64 = qMin<u64>(signalsPerChunk, maxCount);
                if (count64 == 0 || !signalRangeIntersectsSelection(
                        selectedStorageIds, false, int(first64), int(count64))) continue;
            }
            selectedBucketCycles = qMax(selectedBucketCycles, idx.bucketCycles);
        }
        if (selectedBucketCycles <= 0) return true;
    }

    QVector<int> selectedChunkIndexes;
    selectedChunkIndexes.reserve(lodChunks.size());
    for (int ci = 0; ci < lodChunks.size(); ++ci) {
        const LodChunkIndexRec& idx = lodChunks.at(ci);
        if (filterByTime && (idx.end <= windowStart || idx.start >= windowEnd)) continue;
        if (selectedBucketCycles > 0 &&
            (exactBucketOnly ? idx.bucketCycles != selectedBucketCycles
                             : idx.bucketCycles < selectedBucketCycles)) continue;
        if (signalsPerChunk > 0 && !selectedStorageIds.isEmpty()) {
            const u64 first64 = idx.signalChunkId * signalsPerChunk + 1ull;
            if (first64 > u64(std::numeric_limits<int>::max())) continue;
            const u64 maxCount = u64(std::numeric_limits<int>::max()) - first64 + 1ull;
            const u64 count64 = qMin<u64>(signalsPerChunk, maxCount);
            if (count64 == 0 || !signalRangeIntersectsSelection(
                    selectedStorageIds, false, int(first64), int(count64))) continue;
        }
        selectedChunkIndexes.push_back(ci);
    }
    if (selectedChunkIndexes.size() < 4) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("Cannot open WVZ4 file: %1").arg(filePath);
            return false;
        }
        return decodeLodzChunksFromFooterIndex(file, lodChunks, signalsPerChunk,
                                               selectedStorageIds, false,
                                               byteWidthByStorageId, lodLevelsByStorageId,
                                               windowStart, windowEnd, targetBucketCycles,
                                               exactBucketOnly, -1, error);
    }

    const unsigned hw = std::thread::hardware_concurrency();
    const int detectedWorkers = int(hw == 0 ? 4 : hw);
    const int workerCount = qBound(1, qMin(detectedWorkers, selectedChunkIndexes.size()), 32);
    std::vector<QVector<QVector<WaveLodLevel>>> workerLevels;
    workerLevels.resize(std::size_t(workerCount));
    std::atomic<int> nextChunk(0);
    std::atomic<bool> failed(false);
    std::mutex errorMutex;
    QString firstError;
    auto setError = [&](const QString& message) {
        bool expected = false;
        if (failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(errorMutex);
            firstError = message;
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(std::size_t(workerCount));
    for (int workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers.emplace_back([&, workerIndex]() {
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly)) {
                setError(QStringLiteral("Cannot open WVZ4 file: %1").arg(filePath));
                return;
            }
            for (;;) {
                if (failed.load(std::memory_order_acquire)) return;
                const int position = nextChunk.fetch_add(1, std::memory_order_relaxed);
                if (position >= selectedChunkIndexes.size()) return;
                const int chunkIndex = selectedChunkIndexes.at(position);
                const LodChunkIndexRec& chunk = lodChunks.at(chunkIndex);
                QVector<QVector<WaveLodLevel>> chunkLevels;
                QString localError;
                if (!decodeLodzChunksFromFooterIndex(file, lodChunks, signalsPerChunk,
                                                     selectedStorageIds, false,
                                                     byteWidthByStorageId, chunkLevels,
                                                     windowStart, windowEnd,
                                                     chunk.bucketCycles, true,
                                                     chunkIndex, localError)) {
                    setError(localError);
                    return;
                }
                appendDecodedLodLevels(workerLevels[std::size_t(workerIndex)],
                                       std::move(chunkLevels));
            }
        });
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    if (failed.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = firstError.isEmpty() ? QStringLiteral("WVZ4 parallel LODZ decode failed") : firstError;
        return false;
    }
    for (auto& levels : workerLevels) appendDecodedLodLevels(lodLevelsByStorageId, std::move(levels));
    sortAndDedupLodSamples(lodLevelsByStorageId);
    return true;
}

bool readWdatPayloadByIndex(QFile& file,
                            const BlockIndexRec& block,
                            QByteArray& payload,
                            QString& error) {
    if (block.fileOffset > u64(std::numeric_limits<qint64>::max())) {
        error = QStringLiteral("WVZ4 FOOT block %1 file offset exceeds qint64 range").arg(block.blockId);
        return false;
    }
    if (!file.seek(qint64(block.fileOffset))) {
        error = QStringLiteral("WVZ4 failed to seek to WDAT block %1").arg(block.blockId);
        return false;
    }

    SectionHeader sh;
    if (!readSectionHeader(file, sh, error)) {
        if (error.isEmpty()) error = QStringLiteral("WVZ4 FOOT block %1 points past end of file").arg(block.blockId);
        return false;
    }
    if (sh.tag != "WDAT") {
        error = QStringLiteral("WVZ4 FOOT block %1 does not point to a WDAT section").arg(block.blockId);
        return false;
    }
    const qint64 sectionStart = sh.payloadOffset - 12;
    if (sectionStart < 0 || u64(sectionStart) != block.fileOffset) {
        error = QStringLiteral("WVZ4 FOOT block %1 file offset mismatch").arg(block.blockId);
        return false;
    }
    if (sh.size > u64(std::numeric_limits<qint64>::max() - 12) ||
        block.fileSize != sh.size + 12ull) {
        error = QStringLiteral("WVZ4 FOOT block %1 file size does not match WDAT section size").arg(block.blockId);
        return false;
    }
    return readSectionPayload(file, sh, payload, error);
}

bool parseWdatPayloadHeaderForRawCompare(const QByteArray& payload,
                                         const BlockIndexRec& expected,
                                         const char*& encodedPtr,
                                         int& encodedSize,
                                         Compression& compression,
                                         u64& rawSize,
                                         QString& error) {
    SpanReader r(payload);
    u64 blockId = 0;
    i64 start = 0;
    i64 end = 0;
    u64 signalChunkId = 0;
    u64 firstSignalId = 1;
    u64 signalCount = 0;
    u8 compByte = 0;
    u64 encodedSize64 = 0;

    if (!r.readVarUInt(blockId) ||
        !r.readI64(start) ||
        !r.readI64(end)) {
        error = QStringLiteral("WVZ4 WDAT compare block header malformed");
        return false;
    }
    if (!r.readVarUInt(signalChunkId) ||
        !r.readVarUInt(firstSignalId) ||
        !r.readVarUInt(signalCount)) {
        error = QStringLiteral("WVZ4 WDAT compare chunk header malformed");
        return false;
    }
    if (!r.readU8(compByte) ||
        !r.readVarUInt(rawSize) ||
        !r.readVarUInt(encodedSize64) ||
        encodedSize64 > u64(std::numeric_limits<int>::max())) {
        error = QStringLiteral("WVZ4 WDAT compare compression header malformed");
        return false;
    }

    if (blockId != expected.blockId || start != expected.start || end != expected.end) {
        error = QStringLiteral("WVZ4 WDAT compare block metadata mismatch for block %1").arg(expected.blockId);
        return false;
    }
    if (signalChunkId != expected.signalChunkId ||
        firstSignalId != expected.firstSignalId ||
        signalCount != expected.signalCount) {
        error = QStringLiteral("WVZ4 WDAT compare signal chunk metadata mismatch for block %1").arg(expected.blockId);
        return false;
    }
    compression = Compression(compByte);
    if (!isValidCompression(compression)) {
        error = QStringLiteral("WVZ4 WDAT compare unsupported compression value %1").arg(int(compByte));
        return false;
    }
    if (compression != expected.compression || rawSize != expected.rawSize) {
        error = QStringLiteral("WVZ4 WDAT compare compression metadata mismatch for block %1").arg(expected.blockId);
        return false;
    }

    encodedPtr = nullptr;
    encodedSize = int(encodedSize64);
    if (!r.readBytes(encodedPtr, encodedSize)) {
        error = QStringLiteral("WVZ4 WDAT compare encoded payload truncated");
        return false;
    }
    if (!r.eof()) {
        error = QStringLiteral("WVZ4 WDAT compare payload has trailing bytes");
        return false;
    }
    return true;
}

bool decodeRawWdatPayloadForCompare(const QByteArray& payload,
                                    const BlockIndexRec& expected,
                                    QByteArray& raw,
                                    QString& error) {
    const char* encodedPtr = nullptr;
    int encodedSize = 0;
    Compression compression = Compression::None;
    u64 rawSize = 0;
    if (!parseWdatPayloadHeaderForRawCompare(payload, expected,
                                             encodedPtr, encodedSize, compression, rawSize, error)) {
        return false;
    }

    if (compression == Compression::None) {
        if (rawSize != u64(encodedSize)) {
            error = QStringLiteral("WVZ4 WDAT compare uncompressed raw_size mismatch");
            return false;
        }
        raw = QByteArray(encodedPtr, encodedSize);
        return true;
    }

    const QByteArray encoded = QByteArray::fromRawData(encodedPtr, encodedSize);
    raw = decompressBlockPayload(encoded, compression, rawSize, error);
    return error.isEmpty();
}

} // namespace

struct WaveParser4Reader::Impl {
    QString filePath;
    u32 headerSize = 0;
    u64 footerOffset = 0;
    u64 signalsPerChunk = 0;
    u64 headerFeatureFlags = 0;
    bool residualLodTables = false;

    QVector<SigRec> sigs;
    QVector<ClockRec> clocks;
    QVector<BlockIndexRec> footerBlocks;
    QVector<QVector<int>> footerBlockIndexesByChunk;
    QVector<QVector<WaveLodLevel>> footerLodLevelsByStorageId;
    QVector<LodChunkIndexRec> footerLodChunkIndex;
    QVector<int> byteWidthByStorageId;
    QVector<int> boolStorageByStorageId;

    WaveFile directoryWave;
    bool opened = false;
};

WaveParser4Reader::WaveParser4Reader()
    : d(std::make_unique<Impl>()) {}

WaveParser4Reader::~WaveParser4Reader() = default;
WaveParser4Reader::WaveParser4Reader(WaveParser4Reader&&) noexcept = default;
WaveParser4Reader& WaveParser4Reader::operator=(WaveParser4Reader&&) noexcept = default;

const WaveFile& WaveParser4Reader::directoryWave() const {
    static const WaveFile emptyWave;
    return (d && d->opened) ? d->directoryWave : emptyWave;
}

WaveFile WaveParser4Reader::takeDirectoryWave() {
    if (!d || !d->opened) return WaveFile();
    WaveFile wave = std::move(d->directoryWave);
    d->directoryWave.meta = wave.meta;
    return wave;
}

bool WaveParser4Reader::open(const QString& filePath, QString& error, bool allowUnfinalized) {
    error.clear();
    auto next = std::make_unique<Impl>();
    next->filePath = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot open WVZ4 file: %1").arg(filePath);
        return false;
    }

    const QByteArray header = file.read(64);
    if (header.size() != 64) {
        error = QStringLiteral("WVZ4 header is shorter than 64 bytes");
        return false;
    }
    if (std::memcmp(header.constData(), "WVZ4\r\n\0\0", 8) != 0) {
        error = QStringLiteral("Invalid WVZ4 magic");
        return false;
    }

    const u32 version = readU32LE(header.constData() + 8);
    next->headerSize = readU32LE(header.constData() + 12);
    next->footerOffset = readU64LE(header.constData() + 24);
    next->signalsPerChunk = readU64LE(header.constData() + 32);
    next->headerFeatureFlags = readU64LE(header.constData() + 40);
    next->residualLodTables =
        ((next->headerFeatureFlags & kHeaderFeatureResidualLodTables) != 0) &&
        ((next->headerFeatureFlags & kHeaderFeatureLodTables) != 0);

    if (!isSupportedFormatVersion(version)) {
        error = QStringLiteral("Unsupported WVZ4 version: %1 (this Viewer supports v15 only)").arg(version);
        return false;
    }
    if (next->headerSize < 64 || quint64(next->headerSize) > quint64(file.size())) {
        error = QStringLiteral("Invalid WVZ4 header_size: %1").arg(next->headerSize);
        return false;
    }
    if (next->footerOffset != 0 && next->footerOffset > quint64(file.size())) {
        error = QStringLiteral("WVZ4 footer_offset exceeds file size");
        return false;
    }
    if (!allowUnfinalized && next->footerOffset == 0) {
        error = QStringLiteral("WVZ4 file is not finalized: missing FOOT/footer_offset.");
        return false;
    }
    if (!file.seek(qint64(next->headerSize))) {
        error = QStringLiteral("WVZ4 failed to seek to header_size");
        return false;
    }

    bool haveNodeLayout = false;
    bool haveSignalLayout = false;
    bool haveFooter = false;
    while (!file.atEnd()) {
        SectionHeader sh;
        if (!readSectionHeader(file, sh, error)) {
            if (error.isEmpty()) break;
            return false;
        }

        if (sh.tag == "WDAT") {
            if (next->footerOffset != 0) {
                if (!haveSignalLayout) {
                    error = QStringLiteral("WVZ4 missing SIGD section before WDAT");
                    return false;
                }
                if (next->footerOffset > u64(std::numeric_limits<qint64>::max()) ||
                    !file.seek(qint64(next->footerOffset))) {
                    error = QStringLiteral("WVZ4 failed to seek to FOOT section");
                    return false;
                }
                SectionHeader footerHeader;
                if (!readSectionHeader(file, footerHeader, error)) {
                    if (error.isEmpty()) error = QStringLiteral("WVZ4 footer_offset does not point to a section");
                    return false;
                }
                if (footerHeader.tag != "FOOT") {
                    error = QStringLiteral("WVZ4 footer_offset does not point to FOOT");
                    return false;
                }
                QByteArray footerPayload;
                if (!readSectionPayload(file, footerHeader, footerPayload, error)) return false;
                if (!parseFooterSection(footerPayload,
                                        next->byteWidthByStorageId,
                                        next->boolStorageByStorageId,
                                        next->footerBlocks,
                                        next->footerBlockIndexesByChunk,
                                        next->footerLodLevelsByStorageId,
                                        next->footerLodChunkIndex,
                                        error)) {
                    return false;
                }
                if (!validateFooterBlockFileRanges(next->footerBlocks, quint64(file.size()), error)) return false;
                haveFooter = true;
                break;
            }

            error = QStringLiteral("WVZ4 indexed reader requires a finalized FOOT index");
            return false;
        }

        QByteArray payload;
        if (!readSectionPayload(file, sh, payload, error)) return false;

        if (sh.tag == "NAME") {
            if (!parseNameSection(payload, next->directoryWave.tree.namesById, error)) return false;
        } else if (sh.tag == "NAMZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "NAMZ", error);
            if (!error.isEmpty()) return false;
            if (!parseNameSection(raw, next->directoryWave.tree.namesById, error)) return false;
        } else if (sh.tag == "NODI") {
            if (haveNodeLayout) {
                error = QStringLiteral("WVZ4 contains duplicate NODI layout sections");
                return false;
            }
            if (!parseCompactNodeTreeSection(payload, next->directoryWave.tree, error)) return false;
            haveNodeLayout = true;
        } else if (sh.tag == "NIZ2") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "NIZ2", error);
            if (!error.isEmpty()) return false;
            if (haveNodeLayout) {
                error = QStringLiteral("WVZ4 contains duplicate NODI layout sections");
                return false;
            }
            if (!parseCompactNodeTreeSection(raw, next->directoryWave.tree, error)) return false;
            haveNodeLayout = true;
        } else if (sh.tag == "SIGD") {
            if (haveSignalLayout) {
                error = QStringLiteral("WVZ4 contains duplicate SIGD layout sections");
                return false;
            }
            if (!parseCompactSignalSection(payload, next->sigs, error)) return false;
            if (!finalizeCompactDirectorySignals(next->sigs,
                                                 next->directoryWave.tree,
                                                 next->directoryWave.signalList,
                                                 next->byteWidthByStorageId,
                                                 next->boolStorageByStorageId,
                                                 error)) return false;
            haveSignalLayout = true;
        } else if (sh.tag == "SGZ2") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "SGZ2", error);
            if (!error.isEmpty()) return false;
            if (haveSignalLayout) {
                error = QStringLiteral("WVZ4 contains duplicate SIGD layout sections");
                return false;
            }
            if (!parseCompactSignalSection(raw, next->sigs, error)) return false;
            if (!finalizeCompactDirectorySignals(next->sigs,
                                                 next->directoryWave.tree,
                                                 next->directoryWave.signalList,
                                                 next->byteWidthByStorageId,
                                                 next->boolStorageByStorageId,
                                                 error)) return false;
            haveSignalLayout = true;
        } else if (sh.tag == "NREF" || sh.tag == "NRFZ" ||
                   sh.tag == "SIGT" || sh.tag == "SIGZ") {
            error = QStringLiteral("WVZ4 v15 Viewer requires compact NODI/SIGD layout sections");
            return false;
        } else if (sh.tag == "CLKD") {
            if (!parseClockSection(payload, next->clocks, error)) return false;
        } else if (sh.tag == "CLKZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "CLKZ", error);
            if (!error.isEmpty()) return false;
            if (!parseClockSection(raw, next->clocks, error)) return false;
        } else if (sh.tag == "FOOT") {
            const qint64 footerSectionOffset = sh.payloadOffset - 12;
            if (next->footerOffset != 0 && footerSectionOffset >= 0 &&
                u64(footerSectionOffset) != next->footerOffset) {
                error = QStringLiteral("WVZ4 footer_offset does not point to FOOT");
                return false;
            }
            if (!haveSignalLayout) {
                error = QStringLiteral("WVZ4 missing SIGD section before FOOT");
                return false;
            }
            if (!parseFooterSection(payload,
                                    next->byteWidthByStorageId,
                                    next->boolStorageByStorageId,
                                    next->footerBlocks,
                                    next->footerBlockIndexesByChunk,
                                    next->footerLodLevelsByStorageId,
                                    next->footerLodChunkIndex,
                                    error)) {
                return false;
            }
            if (!validateFooterBlockFileRanges(next->footerBlocks, quint64(file.size()), error)) return false;
            haveFooter = true;
            break;
        } else {
            // Unknown extension section: payload already read and intentionally ignored.
        }
    }

    if (!haveFooter) {
        error = QStringLiteral("WVZ4 indexed reader did not find FOOT");
        return false;
    }
    if (next->directoryWave.tree.namesById.isEmpty()) {
        error = QStringLiteral("WVZ4 missing NAME section");
        return false;
    }
    if (!haveNodeLayout || next->directoryWave.tree.nodesById.isEmpty()) {
        error = QStringLiteral("WVZ4 missing NODI section");
        return false;
    }
    if (!haveSignalLayout || next->sigs.isEmpty()) {
        error = QStringLiteral("WVZ4 missing SIGD section or signal table is empty");
        return false;
    }
    QVector<int> directoryOutputIndexBySignalId =
        next->directoryWave.tree.signalIndexBySignalId;
    for (int& encodedIndex : directoryOutputIndexBySignalId) {
        encodedIndex = encodedIndex > 0 ? encodedIndex - 1 : -1;
    }
    if (!applyProceduralClockDefinitions(next->clocks,
                                         directoryOutputIndexBySignalId,
                                         next->directoryWave.signalList,
                                         true,
                                         error)) {
        return false;
    }

    qint64 minTime = std::numeric_limits<qint64>::max();
    qint64 maxTime = 0;
    expandTimeRangeFromFooterBlocks(next->footerBlocks, minTime, maxTime);
    next->directoryWave.meta.title = QFileInfo(filePath).completeBaseName();
    next->directoryWave.meta.timescale = QStringLiteral("cycle");
    next->directoryWave.meta.start = (minTime == std::numeric_limits<qint64>::max()) ? 0 : minTime;
    next->directoryWave.meta.end = qMax(next->directoryWave.meta.start + 1, maxTime);
    next->opened = true;

    d = std::move(next);
    return true;
}

bool WaveParser4Reader::loadSignals(const QVector<int>& signalIds,
                                    WaveFile& outWave,
                                    QString& error,
                                    quint64 maxDecodedSamples,
                                    qint64 timeStart,
                                    qint64 timeEnd,
                                    const LoadProgressCallback& progress) const {
    error.clear();
    outWave = WaveFile();
    if (!d || !d->opened) {
        error = QStringLiteral("WVZ4 reader is not open");
        return false;
    }

    outWave.meta = d->directoryWave.meta;
    if (signalIds.isEmpty()) return true;

    QSet<int> selectedStorageIds;
    selectedStorageIds.reserve(signalIds.size() * 2 + 1);
    QSet<int> emittedSignalIds;
    emittedSignalIds.reserve(signalIds.size() * 2 + 1);
    const QSet<int> clockSignalIds = proceduralClockSignalIds(d->clocks);
    for (int sid : signalIds) {
        if (sid <= 0 || sid > d->sigs.size()) continue;
        const SigRec& s = d->sigs.at(sid - 1);
        if (s.signalId != u32(sid)) continue;
        if (clockSignalIds.contains(sid)) continue;
        const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
        if (storageId > 0) selectedStorageIds.insert(storageId);
    }

    QVector<WaveSignal> outputSignals;
    QHash<int, int> outputIndexBySignalId;
    QHash<int, QVector<int>> outputIndexesByStorageId;
    outputSignals.reserve(signalIds.size());
    outputIndexBySignalId.reserve(signalIds.size() * 2 + 1);
    outputIndexesByStorageId.reserve(signalIds.size() * 2 + 1);
    for (int sid : signalIds) {
        if (sid <= 0 || sid > d->sigs.size() || emittedSignalIds.contains(sid)) continue;
        const SigRec& s = d->sigs.at(sid - 1);
        if (s.signalId != u32(sid)) continue;
        if (!isVisibleSignalRec(s)) continue;
        emittedSignalIds.insert(sid);
        const int idx = outputSignals.size();
        outputSignals.push_back(makeWaveSignalFromRec(s, true));
        outputIndexBySignalId.insert(int(s.signalId), idx);
        if (!clockSignalIds.contains(sid)) {
            const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
            outputIndexesByStorageId[storageId].push_back(idx);
        }
    }
    if (!applyProceduralClockDefinitions(d->clocks,
                                         outputIndexBySignalId,
                                         outputSignals,
                                         false,
                                         error)) {
        return false;
    }

    QVector<QVector<WaveSample>> samplesByOutputIndex;
    samplesByOutputIndex.resize(outputSignals.size());
    qint64 minTime = std::numeric_limits<qint64>::max();
    qint64 maxTime = 0;

    DecodedSampleBudgetScope decodedSampleBudget(maxDecodedSamples);
    const QSet<int> noFilteredSignalIds;
    if (!appendImplicitZeroSamplesForSelectedSignals(noFilteredSignalIds, true,
                                                     d->directoryWave.meta.start,
                                                     outputSignals, samplesByOutputIndex,
                                                     minTime, maxTime, error)) {
        return false;
    }

    if (!selectedStorageIds.isEmpty()) {
        if (!decodeWdatSectionsFromFooterIndexParallel(d->filePath,
                                                       d->footerBlocks,
                                                       d->footerBlockIndexesByChunk,
                                                       d->signalsPerChunk,
                                                       selectedStorageIds,
                                                       false,
                                                       storageOutputIndexLookup(outputIndexesByStorageId),
                                                       d->byteWidthByStorageId,
                                                       outputSignals,
                                                       samplesByOutputIndex,
                                                       timeStart,
                                                       timeEnd,
                                                       minTime,
                                                       maxTime,
                                                       maxDecodedSamples,
                                                       error,
                                                       progress)) {
            return false;
        }
    }

    if (d->residualLodTables && !selectedStorageIds.isEmpty()) {
        QVector<QVector<WaveLodLevel>> residualLodLevelsByStorageId = d->footerLodLevelsByStorageId;
        if (!decodeLodzChunksFromFooterIndexParallel(
                d->filePath, d->footerLodChunkIndex, d->signalsPerChunk,
                selectedStorageIds, false,
                d->byteWidthByStorageId, residualLodLevelsByStorageId,
                timeStart, timeEnd, 0, false, -1, error)) {
            return false;
        }
        appendResidualLodToRawSamples(residualLodLevelsByStorageId, emittedSignalIds, false,
                                      outputSignals, samplesByOutputIndex);
    }

    expandTimeRangeFromFooterBlocks(d->footerBlocks, minTime, maxTime);

    for (int i = 0; i < outputSignals.size(); ++i) {
        outputSignals[i].samples = std::move(samplesByOutputIndex[i]);
        outputSignals[i].samplesLoaded = true;
    }

    outWave.meta.start = (minTime == std::numeric_limits<qint64>::max()) ? d->directoryWave.meta.start : minTime;
    outWave.meta.end = qMax(outWave.meta.start + 1, maxTime);
    outWave.signalList = std::move(outputSignals);
    return true;
}

bool WaveParser4Reader::loadSignalLod(const QVector<int>& signalIds,
                                      WaveFile& outWave,
                                      QString& error,
                                      qint64 timeStart,
                                      qint64 timeEnd,
                                      qint64 targetBucketCycles) const {
    return loadSignalLodImpl(signalIds, outWave, error, timeStart, timeEnd,
                             targetBucketCycles, false, -1);
}

bool WaveParser4Reader::findSignalValueMatches(
        const QVector<SignalValueMatchRequest>& requests,
        qint64 timeStart,
        qint64 timeEnd,
        QVector<SignalValueMatchSegment>& matches,
        QString& error,
        quint64 maxDecodedSamples) const {
    matches.clear();
    error.clear();
    if (!d || !d->opened) {
        error = QStringLiteral("WVZ4 reader is not open");
        return false;
    }
    if (requests.isEmpty() || timeEnd <= timeStart) return true;

    QVector<int> signalIds;
    QVector<quint64> targetsByOutputIndex;
    QHash<int, quint64> targetBySignalId;
    QSet<int> emittedSignalIds;
    signalIds.reserve(requests.size());
    targetsByOutputIndex.reserve(requests.size());
    targetBySignalId.reserve(requests.size() * 2 + 1);
    emittedSignalIds.reserve(requests.size() * 2 + 1);
    for (const SignalValueMatchRequest& request : requests) {
        const int signalId = request.signalId;
        if (signalId <= 0 || signalId > d->sigs.size() ||
            emittedSignalIds.contains(signalId)) {
            continue;
        }
        const SigRec& rec = d->sigs.at(signalId - 1);
        if (rec.signalId != u32(signalId) || !isVisibleSignalRec(rec)) continue;
        const quint64 targetBits =
            request.targetBits & waveBitMaskForWidth(int(rec.bitWidth));
        emittedSignalIds.insert(signalId);
        signalIds.push_back(signalId);
        targetsByOutputIndex.push_back(targetBits);
        targetBySignalId.insert(signalId, targetBits);
    }
    if (signalIds.isEmpty()) return true;

    WaveFile matchWave;
    bool loaded = false;
    if (d->residualLodTables) {
        // Residual LOD tables can contain exact transitions not present in
        // WDAT. Keep the established full decoder for those older layouts.
        loaded = loadSignals(signalIds, matchWave, error, maxDecodedSamples,
                             timeStart, timeEnd);
    } else {
        // The raw decoder normally compacts equal scalar values. For value
        // search it can compact the much smaller boolean match state instead,
        // preserving every entry/exit boundary without materializing unrelated
        // values.
        DecodedMatchTargetScope matchTargetScope(&targetsByOutputIndex);
        loaded = loadSignals(signalIds, matchWave, error, maxDecodedSamples,
                             timeStart, timeEnd);
    }
    if (!loaded) return false;

    const bool matchEncoded = !d->residualLodTables;
    for (const WaveSignal& signal : matchWave.signalList) {
        const auto targetIt = targetBySignalId.constFind(signal.signalId);
        if (targetIt == targetBySignalId.constEnd()) continue;
        const quint64 targetBits = targetIt.value();
        const quint64 mask = waveBitMaskForWidth(signal.width);

        if (signal.proceduralClock) {
            qint64 segmentStart = timeStart;
            qint64 nextTransition =
                waveProceduralClockNextTransition(signal, timeStart);
            while (segmentStart < timeEnd) {
                const qint64 segmentEnd =
                    nextTransition > segmentStart
                        ? qMin(timeEnd, nextTransition)
                        : timeEnd;
                const quint64 value =
                    waveProceduralClockValueAtTime(signal, segmentStart) ? 1ull : 0ull;
                if ((value & mask) == targetBits && segmentEnd > segmentStart) {
                    SignalValueMatchSegment segment;
                    segment.signalId = signal.signalId;
                    segment.start = segmentStart;
                    segment.end = segmentEnd;
                    matches.push_back(segment);
                }
                if (segmentEnd >= timeEnd) break;
                segmentStart = segmentEnd;
                nextTransition =
                    waveProceduralClockNextTransition(signal, segmentStart);
            }
            continue;
        }

        auto sampleMatches = [&](const WaveSample& sample) {
            if (sample.isAbsent || sample.isZ) return false;
            if (matchEncoded) return sample.rawFieldsReady && sample.rawBits != 0;
            if (sample.rawFieldsReady) return (sample.rawBits & mask) == targetBits;
            WaveSample hydrated = sample;
            hydrateWaveSampleRawFields(signal.kind, signal.width, hydrated);
            return !hydrated.isAbsent && !hydrated.isZ &&
                   ((hydrated.rawBits & mask) == targetBits);
        };

        const auto firstAfterStart = std::upper_bound(
            signal.samples.constBegin(), signal.samples.constEnd(), timeStart,
            [](qint64 time, const WaveSample& sample) {
                return time < sample.time;
            });
        const int firstAfterStartIndex =
            int(firstAfterStart - signal.samples.constBegin());
        const int stateAtStartIndex = firstAfterStartIndex - 1;
        bool previousMatched =
            stateAtStartIndex >= 0 &&
            sampleMatches(signal.samples.at(stateAtStartIndex));
        qint64 activeStart = previousMatched ? timeStart : 0;

        for (int sampleIndex = firstAfterStartIndex;
             sampleIndex < signal.samples.size(); ++sampleIndex) {
            const WaveSample& sample = signal.samples.at(sampleIndex);
            if (sample.time >= timeEnd) break;
            const bool matched = sampleMatches(sample);
            if (matched && !previousMatched) {
                activeStart = sample.time;
            } else if (!matched && previousMatched && sample.time > activeStart) {
                SignalValueMatchSegment segment;
                segment.signalId = signal.signalId;
                segment.start = activeStart;
                segment.end = sample.time;
                matches.push_back(segment);
            }
            previousMatched = matched;
        }
        if (previousMatched && timeEnd > activeStart) {
            SignalValueMatchSegment segment;
            segment.signalId = signal.signalId;
            segment.start = activeStart;
            segment.end = timeEnd;
            matches.push_back(segment);
        }
    }
    return true;
}

bool WaveParser4Reader::findRawSignalEvent(const QVector<int>& signalIds,
                                           qint64 timeStart,
                                           qint64 timeEnd,
                                           bool firstEvent,
                                           int& resultSignalId,
                                           qint64& resultTime,
                                           QString& error,
                                           quint64 maxDecodedSamples) const {
    resultSignalId = -1;
    resultTime = -1;
    error.clear();
    if (!d || !d->opened) {
        error = QStringLiteral("WVZ4 reader is not open");
        return false;
    }
    if (signalIds.isEmpty() || timeEnd <= timeStart) return true;

    // Very large constant-only trees are common in generated hierarchy
    // probes. Prove the no-event case without materializing one WaveSignal and
    // one implicit WaveSample per selected signal. This decoder still parses
    // and validates every selected raw record; it succeeds only when every
    // transition in the entire file collapses at the trace start.
    bool thresholdOk = false;
    const int configuredThreshold =
        qEnvironmentVariable("WV_VIEWER_TREE_EVENT_REDUCE_THRESHOLD").toInt(&thresholdOk);
    const int reductionThreshold = thresholdOk ? qMax(1, configuredThreshold) : 65536;
    if (signalIds.size() > reductionThreshold && !d->residualLodTables) {
        QBitArray selectedSignalIds(d->sigs.size() + 1, false);
        int selectedVisibleCount = 0;
        for (int signalId : signalIds) {
            if (signalId <= 0 || signalId > d->sigs.size() ||
                selectedSignalIds.testBit(signalId)) {
                continue;
            }
            const SigRec& signal = d->sigs.at(signalId - 1);
            if (signal.signalId != u32(signalId) || !isVisibleSignalRec(signal)) continue;
            selectedSignalIds.setBit(signalId);
            ++selectedVisibleCount;
        }

        bool selectedClock = false;
        for (const ClockRec& clock : d->clocks) {
            const int signalId = int(clock.signalId);
            if (signalId > 0 && signalId < selectedSignalIds.size() &&
                selectedSignalIds.testBit(signalId)) {
                selectedClock = true;
                break;
            }
        }

        if (selectedVisibleCount > 0 && !selectedClock) {
            const bool allSelectedStorage =
                selectedVisibleCount == d->directoryWave.signalList.size();
            QSet<int> selectedStorageIds;
            if (!allSelectedStorage) {
                selectedStorageIds.reserve(selectedVisibleCount * 2 + 1);
                for (int signalId = 1; signalId < selectedSignalIds.size(); ++signalId) {
                    if (!selectedSignalIds.testBit(signalId)) continue;
                    const SigRec& signal = d->sigs.at(signalId - 1);
                    const int storageId = int(signal.storageId != 0
                        ? signal.storageId : signal.signalId);
                    if (storageId > 0) selectedStorageIds.insert(storageId);
                }
            }

            struct StartOnlyObserverState {
                qint64 traceStart = 0;
                bool sawOtherTime = false;
            } observerState;
            observerState.traceStart = d->directoryWave.meta.start;
            RawDecodeObserver observer;
            observer.context = &observerState;
            observer.callback = [](qint64 sampleTime, void* context) {
                StartOnlyObserverState* state =
                    static_cast<StartOnlyObserverState*>(context);
                if (sampleTime == state->traceStart) return true;
                state->sawOtherTime = true;
                return false;
            };

            QFile file(d->filePath);
            if (!file.open(QIODevice::ReadOnly)) {
                error = QStringLiteral("Cannot open WVZ4 file: %1").arg(d->filePath);
                return false;
            }
            const QVector<QVector<int>> noOutputIndexes;
            const QVector<WaveSignal> noOutputSignals;
            QVector<QVector<WaveSample>> noOutputSamples;
            qint64 ignoredMinTime = std::numeric_limits<qint64>::max();
            qint64 ignoredMaxTime = 0;
            bool proofComplete = true;
            for (const BlockIndexRec& block : d->footerBlocks) {
                if (!signalRangeIntersectsSelection(
                        selectedStorageIds, allSelectedStorage,
                        int(block.firstSignalId), int(block.signalCount))) {
                    continue;
                }
                if (block.fileOffset > u64(std::numeric_limits<qint64>::max()) ||
                    !file.seek(qint64(block.fileOffset))) {
                    error = QStringLiteral("WVZ4 failed to seek to WDAT block %1 from FOOT index")
                        .arg(block.blockId);
                    return false;
                }
                SectionHeader section;
                if (!readSectionHeader(file, section, error)) {
                    if (error.isEmpty()) {
                        error = QStringLiteral("WVZ4 FOOT block %1 points past end of file")
                            .arg(block.blockId);
                    }
                    return false;
                }
                if (section.tag != "WDAT") {
                    error = QStringLiteral("WVZ4 FOOT block %1 does not point to a WDAT section")
                        .arg(block.blockId);
                    return false;
                }
                if (!decodeWdatSectionStreaming(
                        file, section, selectedStorageIds, allSelectedStorage,
                        storageOutputIndexLookup(noOutputIndexes),
                        d->byteWidthByStorageId,
                        noOutputSignals, noOutputSamples,
                        std::numeric_limits<qint64>::min(),
                        std::numeric_limits<qint64>::max(),
                        ignoredMinTime, ignoredMaxTime, error,
                        nullptr, &block, &observer)) {
                    if (!error.isEmpty()) return false;
                    proofComplete = false;
                    break;
                }
            }
            if (proofComplete && !observerState.sawOtherTime) return true;
        }
    }

    // Keep raw decoding in the parser so the jump path can reduce the result
    // to one event without installing every selected signal's samples and
    // derived caches into the viewer. loadSignals remains the single source
    // of truth for implicit values, clock records, aliases/slices, residual
    // LOD records, range anchors, and sample compaction.
    WaveFile loadedWave;
    if (!loadSignals(signalIds, loadedWave, error, maxDecodedSamples,
                     timeStart, timeEnd)) {
        return false;
    }

    QVector<int> ordinalBySignalId(d->sigs.size() + 1, -1);
    for (int ordinal = 0; ordinal < signalIds.size(); ++ordinal) {
        const int signalId = signalIds.at(ordinal);
        if (signalId > 0 && signalId < ordinalBySignalId.size() &&
            ordinalBySignalId.at(signalId) < 0) {
            ordinalBySignalId[signalId] = ordinal;
        }
    }

    int resultOrdinal = (std::numeric_limits<int>::max)();
    auto lowerSample = [](const QVector<WaveSample>& samples, qint64 time) {
        int lo = 0;
        int hi = samples.size();
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (samples.at(mid).time < time) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    };

    for (const WaveSignal& signal : loadedWave.signalList) {
        qint64 eventTime = -1;
        if (signal.proceduralClock) {
            eventTime = firstEvent
                ? waveProceduralClockTransitionAtOrAfter(signal, timeStart)
                : waveProceduralClockPreviousTransition(signal, timeEnd);
            if (eventTime < timeStart || eventTime >= timeEnd) eventTime = -1;
        } else if (signal.samples.size() < 2) {
            continue;
        } else if (firstEvent) {
            const int begin = qMax(1, lowerSample(signal.samples, timeStart));
            for (int i = begin; i < signal.samples.size(); ++i) {
                const qint64 time = signal.samples.at(i).time;
                if (time >= timeEnd) break;
                if (!waveSamplesEquivalent(signal.samples.at(i), signal.samples.at(i - 1))) {
                    eventTime = time;
                    break;
                }
            }
        } else {
            int i = qMin(lowerSample(signal.samples, timeEnd) - 1,
                         signal.samples.size() - 1);
            for (; i >= 1; --i) {
                const qint64 time = signal.samples.at(i).time;
                if (time < timeStart) break;
                if (!waveSamplesEquivalent(signal.samples.at(i), signal.samples.at(i - 1))) {
                    eventTime = time;
                    break;
                }
            }
        }
        if (eventTime < 0) continue;

        const int ordinal =
            signal.signalId > 0 && signal.signalId < ordinalBySignalId.size()
                ? ordinalBySignalId.at(signal.signalId)
                : (std::numeric_limits<int>::max)();
        if (resultTime < 0 ||
            (firstEvent ? eventTime < resultTime : eventTime > resultTime) ||
            (eventTime == resultTime && ordinal < resultOrdinal)) {
            resultSignalId = signal.signalId;
            resultTime = eventTime;
            resultOrdinal = ordinal;
        }
    }
    return true;
}

bool WaveParser4Reader::loadSignalLodImpl(const QVector<int>& signalIds,
                                          WaveFile& outWave,
                                          QString& error,
                                          qint64 timeStart,
                                          qint64 timeEnd,
                                          qint64 targetBucketCycles,
                                          bool exactBucketOnly,
                                          int onlyChunkIndex) const {
    error.clear();
    outWave = WaveFile();
    if (!d || !d->opened) {
        error = QStringLiteral("WVZ4 reader is not open");
        return false;
    }

    outWave.meta = d->directoryWave.meta;
    if (signalIds.isEmpty()) return true;

    QSet<int> selectedStorageIds;
    QSet<int> emittedSignalIds;
    selectedStorageIds.reserve(signalIds.size() * 2 + 1);
    emittedSignalIds.reserve(signalIds.size() * 2 + 1);
    const QSet<int> clockSignalIds = proceduralClockSignalIds(d->clocks);
    QVector<WaveSignal> outputSignals;
    QVector<int> outputIndexBySignalId;
    outputSignals.reserve(signalIds.size());
    outputIndexBySignalId.reserve(signalIds.size());
    for (int sid : signalIds) {
        if (sid <= 0 || sid > d->sigs.size() || emittedSignalIds.contains(sid)) continue;
        const SigRec& s = d->sigs.at(sid - 1);
        if (s.signalId != u32(sid) || !isVisibleSignalRec(s)) continue;
        emittedSignalIds.insert(sid);
        if (!clockSignalIds.contains(sid)) {
            const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
            if (storageId > 0) selectedStorageIds.insert(storageId);
        }
        directIntMapSet(outputIndexBySignalId, sid, outputSignals.size());
        outputSignals.push_back(makeWaveSignalFromRec(s, false));
    }
    if (!applyProceduralClockDefinitions(d->clocks,
                                         outputIndexBySignalId,
                                         outputSignals,
                                         false,
                                         error)) {
        return false;
    }
    if (outputSignals.isEmpty() || selectedStorageIds.isEmpty()) {
        outWave.signalList = std::move(outputSignals);
        return true;
    }

    QVector<QVector<WaveLodLevel>> levelsByStorageId = d->footerLodLevelsByStorageId;
    if (!decodeLodzChunksFromFooterIndexParallel(
            d->filePath, d->footerLodChunkIndex, d->signalsPerChunk,
            selectedStorageIds, false,
            d->byteWidthByStorageId, levelsByStorageId,
            timeStart, timeEnd, qMax<qint64>(1, targetBucketCycles),
            exactBucketOnly, onlyChunkIndex, error)) {
        return false;
    }
    if (d->residualLodTables) makeResidualLodLevelsCumulative(levelsByStorageId);

    for (WaveSignal& signal : outputSignals) {
        const int storageId = signal.storageId > 0 ? signal.storageId : signal.signalId;
        if (storageId <= 0 || storageId >= levelsByStorageId.size()) continue;
        const QVector<WaveLodLevel>& storageLevels = levelsByStorageId.at(storageId);
        signal.lodLevels.reserve(storageLevels.size());
        for (const WaveLodLevel& level : storageLevels) {
            signal.lodLevels.push_back(sliceLodLevelForSignal(level, signal));
        }
    }
    outWave.signalList = std::move(outputSignals);
    return true;
}

QVector<WaveParser4Reader::DataBlockDescriptor> WaveParser4Reader::dataBlocks() const {
    QVector<DataBlockDescriptor> blocks;
    if (!d || !d->opened) return blocks;
    blocks.reserve(d->footerBlocks.size() + d->footerLodChunkIndex.size());

    for (int index = 0; index < d->footerBlocks.size(); ++index) {
        const BlockIndexRec& source = d->footerBlocks.at(index);
        DataBlockDescriptor block;
        block.kind = DataBlockDescriptor::Kind::Raw;
        block.index = index;
        block.blockId = source.blockId;
        block.bucketCycles = 1;
        block.signalChunkId = source.signalChunkId;
        block.firstStorageId = source.firstSignalId > u64(std::numeric_limits<int>::max())
            ? 1 : int(source.firstSignalId);
        block.storageCount = source.signalCount > u64(std::numeric_limits<int>::max())
            ? 0 : int(source.signalCount);
        block.start = source.start;
        block.end = source.end;
        block.fileBytes = source.fileSize;
        block.estimatedDecodedBytes = qMax<quint64>(source.rawSize * 4ull, source.fileSize);
        blocks.push_back(block);
    }

    const u64 storageLimit = d->byteWidthByStorageId.isEmpty()
        ? 0ull : u64(d->byteWidthByStorageId.size() - 1);
    for (int index = 0; index < d->footerLodChunkIndex.size(); ++index) {
        const LodChunkIndexRec& source = d->footerLodChunkIndex.at(index);
        DataBlockDescriptor block;
        block.kind = DataBlockDescriptor::Kind::Lod;
        block.index = index;
        block.blockId = source.chunkId;
        block.bucketCycles = source.bucketCycles;
        block.signalChunkId = source.signalChunkId;
        const u64 first64 = d->signalsPerChunk == 0
            ? 1ull : source.signalChunkId * d->signalsPerChunk + 1ull;
        const u64 available = first64 <= storageLimit ? storageLimit - first64 + 1ull : 0ull;
        const u64 count64 = d->signalsPerChunk == 0
            ? available : qMin(d->signalsPerChunk, available);
        block.firstStorageId = first64 > u64(std::numeric_limits<int>::max()) ? 1 : int(first64);
        block.storageCount = count64 > u64(std::numeric_limits<int>::max()) ? 0 : int(count64);
        block.start = source.start;
        block.end = source.end;
        block.fileBytes = source.fileSize;
        const quint64 sampleEstimate = source.recordCount >
                (std::numeric_limits<quint64>::max)() / quint64(sizeof(WaveSample))
            ? (std::numeric_limits<quint64>::max)()
            : source.recordCount * quint64(sizeof(WaveSample));
        block.estimatedDecodedBytes = qMax<quint64>(source.rawSize, sampleEstimate);
        blocks.push_back(block);
    }
    return blocks;
}

bool WaveParser4Reader::loadDataBlock(const DataBlockDescriptor& block,
                                      WaveFile& outWave,
                                      QString& error,
                                      quint64 maxDecodedSamples) const {
    error.clear();
    outWave = WaveFile();
    if (!d || !d->opened) {
        error = QStringLiteral("WVZ4 reader is not open");
        return false;
    }

    const int firstStorage = qMax(1, block.firstStorageId);
    const qint64 lastStorage64 = qint64(firstStorage) + qMax(0, block.storageCount) - 1ll;
    QVector<int> signalIds;
    signalIds.reserve(qMax(0, block.storageCount));
    for (const SigRec& signal : d->sigs) {
        if (!isVisibleSignalRec(signal)) continue;
        const int storageId = int(signal.storageId != 0 ? signal.storageId : signal.signalId);
        if (storageId < firstStorage || qint64(storageId) > lastStorage64) continue;
        signalIds.push_back(int(signal.signalId));
    }
    if (signalIds.isEmpty()) {
        outWave.meta = d->directoryWave.meta;
        return true;
    }

    if (block.kind == DataBlockDescriptor::Kind::Raw) {
        if (block.index < 0 || block.index >= d->footerBlocks.size() ||
            d->footerBlocks.at(block.index).blockId != block.blockId) {
            error = QStringLiteral("WVZ4 raw cache block descriptor is stale");
            return false;
        }
        return loadSignals(signalIds, outWave, error, maxDecodedSamples,
                           block.start, qMax(block.start + 1, block.end));
    }

    if (block.index < 0 || block.index >= d->footerLodChunkIndex.size()) {
        error = QStringLiteral("WVZ4 LOD cache block descriptor is stale");
        return false;
    }
    const LodChunkIndexRec& source = d->footerLodChunkIndex.at(block.index);
    if (source.chunkId != block.blockId || source.bucketCycles != block.bucketCycles) {
        error = QStringLiteral("WVZ4 LOD cache block descriptor is stale");
        return false;
    }
    return loadSignalLodImpl(signalIds, outWave, error,
                             block.start, qMax(block.start + 1, block.end),
                             block.bucketCycles, true, block.index);
}

WaveParser4Reader::RawBlockCompareResult
WaveParser4Reader::compareRawBlocksWith(const WaveParser4Reader& other,
                                        QString& error) const {
    error.clear();
    if (!d || !d->opened || !other.d || !other.d->opened) {
        error = QStringLiteral("WVZ4 raw compare reader is not open");
        return RawBlockCompareResult::Error;
    }
    if (d->footerBlocks.size() != other.d->footerBlocks.size()) {
        return RawBlockCompareResult::Unsupported;
    }

    const int blockCount = d->footerBlocks.size();
    for (int i = 0; i < blockCount; ++i) {
        const BlockIndexRec& left = d->footerBlocks.at(i);
        const BlockIndexRec& right = other.d->footerBlocks.at(i);
        if (left.blockId != right.blockId ||
            left.start != right.start ||
            left.end != right.end ||
            left.signalChunkId != right.signalChunkId ||
            left.firstSignalId != right.firstSignalId ||
            left.signalCount != right.signalCount ||
            left.rawSize != right.rawSize) {
            return RawBlockCompareResult::Unsupported;
        }
    }
    if (blockCount == 0) {
        return RawBlockCompareResult::Equal;
    }

    std::atomic<int> nextBlock(0);
    std::atomic<bool> different(false);
    std::atomic<bool> failed(false);
    std::mutex errorMutex;
    QString firstError;

    auto setError = [&](const QString& message) {
        bool expected = false;
        if (failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(errorMutex);
            firstError = message;
        }
    };

    const unsigned hw = std::thread::hardware_concurrency();
    const int detectedWorkers = int(hw == 0 ? 4 : hw);
    const int workerCount = qBound(1, qMin(detectedWorkers, blockCount), 32);
    constexpr int kBlocksPerClaim = 8;

    std::vector<std::thread> workers;
    workers.reserve(std::size_t(workerCount));
    for (int workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        Q_UNUSED(workerIndex);
        workers.emplace_back([&]() {
            QFile leftFile(d->filePath);
            QFile rightFile(other.d->filePath);
            if (!leftFile.open(QIODevice::ReadOnly)) {
                setError(QStringLiteral("Cannot open WVZ4 file: %1").arg(d->filePath));
                return;
            }
            if (!rightFile.open(QIODevice::ReadOnly)) {
                setError(QStringLiteral("Cannot open WVZ4 file: %1").arg(other.d->filePath));
                return;
            }

            for (;;) {
                if (different.load(std::memory_order_acquire) ||
                    failed.load(std::memory_order_acquire)) {
                    return;
                }
                const int begin = nextBlock.fetch_add(kBlocksPerClaim, std::memory_order_relaxed);
                if (begin >= blockCount) return;
                const int end = qMin(blockCount, begin + kBlocksPerClaim);
                for (int i = begin; i < end; ++i) {
                    if (different.load(std::memory_order_acquire) ||
                        failed.load(std::memory_order_acquire)) {
                        return;
                    }

                    const BlockIndexRec& leftBlock = d->footerBlocks.at(i);
                    const BlockIndexRec& rightBlock = other.d->footerBlocks.at(i);
                    QByteArray leftPayload;
                    QByteArray rightPayload;
                    QString localError;
                    if (!readWdatPayloadByIndex(leftFile, leftBlock, leftPayload, localError)) {
                        setError(localError);
                        return;
                    }
                    if (!readWdatPayloadByIndex(rightFile, rightBlock, rightPayload, localError)) {
                        setError(localError);
                        return;
                    }

                    if (leftPayload == rightPayload) {
                        const char* encodedPtr = nullptr;
                        int encodedSize = 0;
                        Compression compression = Compression::None;
                        u64 rawSize = 0;
                        if (!parseWdatPayloadHeaderForRawCompare(leftPayload, leftBlock,
                                                                 encodedPtr, encodedSize,
                                                                 compression, rawSize, localError)) {
                            setError(localError);
                            return;
                        }
                        continue;
                    }

                    QByteArray leftRaw;
                    QByteArray rightRaw;
                    if (!decodeRawWdatPayloadForCompare(leftPayload, leftBlock, leftRaw, localError)) {
                        setError(localError);
                        return;
                    }
                    if (!decodeRawWdatPayloadForCompare(rightPayload, rightBlock, rightRaw, localError)) {
                        setError(localError);
                        return;
                    }
                    if (leftRaw != rightRaw) {
                        different.store(true, std::memory_order_release);
                        return;
                    }
                }
            }
        });
    }

    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }

    if (failed.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = firstError.isEmpty()
            ? QStringLiteral("WVZ4 raw block compare failed")
            : firstError;
        return RawBlockCompareResult::Error;
    }
    if (different.load(std::memory_order_acquire)) {
        return RawBlockCompareResult::Different;
    }
    return RawBlockCompareResult::Equal;
}

bool WaveParser4::loadFromFile(const QString& filePath,
                               WaveFile& outWave,
                               QString& error,
                               const LoadOptions& options) {
    error.clear();
    outWave = WaveFile();
    DecodedSampleBudgetScope decodedSampleBudget(options.maxDecodedSamples);

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
    const bool residualLodTables =
        ((headerFeatureFlags & kHeaderFeatureResidualLodTables) != 0) &&
        ((headerFeatureFlags & kHeaderFeatureLodTables) != 0);
    Q_UNUSED(blockSpan);

    // On-demand WVZ4 sample load only needs SIGT plus WDAT. NAME/NAMZ and the
    // v15 NREF/NODI node layouts are skipped instead of reparsed on every click,
    // which made selecting a signal appear stuck on large metadata-heavy files.
    const bool sampleOnlyLoad = !options.includeAllSignalDefinitions && !options.signalIds.isEmpty();

    if (!isSupportedFormatVersion(version)) {
        error = QStringLiteral("不支持的 WVZ4 版本：%1（此 Viewer 仅支持 v15）").arg(version);
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
    if (!options.allowUnfinalized && footerOffset == 0) {
        error = QStringLiteral("WVZ4 file is not finalized: missing FOOT/footer_offset. "
                               "Wait for the writer helper to finalize the file; if direct writing was killed, rerun the capture.");
        return false;
    }
    if (!file.seek(qint64(headerSize))) {
        error = QStringLiteral("WVZ4 seek 到 header_size 失败");
        return false;
    }

    QVector<QByteArray> namesById;
    QVector<NodeRec> nodesById;
    QVector<SigRec> sigs;
    QVector<ClockRec> clocks;
    QVector<BlockIndexRec> footerBlocks;
    QVector<QVector<int>> footerBlockIndexesByChunk;
    QVector<QVector<WaveLodLevel>> footerLodLevelsByStorageId;
    QVector<LodChunkIndexRec> footerLodChunkIndex;

    QSet<int> selectedIds;
    QSet<int> selectedStorageIds;
    QSet<int> lodSelectedStorageIds;
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
    bool allLodSelectedStorageIds = false;
    bool implicitZeroSamplesNeedFooterRebase = false;

    qint64 minTime = std::numeric_limits<qint64>::max();
    qint64 maxTime = 0;
    RawLeftAnchorState rawLeftAnchors;
    bool rawLeftAnchorsInitialized = false;

    auto windowedRawLoad = [&]() -> bool {
        return options.loadRawSamples &&
               options.timeEnd >= options.timeStart &&
               !(options.timeStart == 0 &&
                 options.timeEnd == std::numeric_limits<qint64>::max());
    };

    auto rawLeftAnchorPtr = [&]() -> RawLeftAnchorState* {
        if (!windowedRawLoad()) return nullptr;
        if (!rawLeftAnchorsInitialized) {
            rawLeftAnchors.reset(outputSignals.size());
            rawLeftAnchorsInitialized = true;
        }
        return &rawLeftAnchors;
    };

    auto flushRawLeftAnchors = [&]() -> bool {
        if (!rawLeftAnchorsInitialized) return true;
        for (int i = 0; i < rawLeftAnchors.valid.size(); ++i) {
            if (!rawLeftAnchors.valid.at(i) || rawLeftAnchors.emitted.at(i)) continue;
            if (!emitLeftAnchorIfNeeded(i, outputSignals, samplesByOutputIndex,
                                        true, &rawLeftAnchors, error)) {
                return false;
            }
        }
        return true;
    };

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
        allLodSelectedStorageIds = false;
        selectedIds.clear();
        selectedStorageIds.clear();
        lodSelectedStorageIds.clear();
        const QSet<int> clockSignalIds = proceduralClockSignalIds(clocks);
        auto isVisibleSignal = [](const SigRec& s) -> bool {
            return !s.storageOnly;
        };
        if (!options.signalIds.isEmpty()) {
            for (int sid : options.signalIds) {
                if (sid > 0) selectedIds.insert(sid);
            }
        } else if (options.autoLoadFirstSignalCount >= 0) {
            int loaded = 0;
            for (int i = 0; i < sigs.size() && loaded < options.autoLoadFirstSignalCount; ++i) {
                if (!isVisibleSignal(sigs.at(i))) continue;
                selectedIds.insert(int(sigs.at(i).signalId));
                ++loaded;
            }
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
            if ((s.storageOnly || storageId == int(s.signalId)) && oldBytes > 0 && oldBytes != bytes) {
                error = QStringLiteral("WVZ4 SIGT storage_id %1 has incompatible logical aliases").arg(storageId);
                return false;
            }
            if (s.storageOnly || storageId == int(s.signalId) || oldBytes <= 0) {
                directIntMapSet(byteWidthByStorageId, storageId, bytes);
                directIntMapSet(boolStorageByStorageId, storageId,
                                (s.type == ValueType::Bool && s.bitWidth == 1 && s.bitOffset == 0) ? 1 : 0);
            }
        }
        for (int i = 0; i < sigs.size(); ++i) {
            const SigRec& s = sigs.at(i);
            const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
            const int storageBytes = directIntMapValue(byteWidthByStorageId, storageId, -1);
            if (storageBytes <= 0) {
                error = QStringLiteral("WVZ4 SIGT signal_id %1 references missing storage_id %2")
                    .arg(int(s.signalId)).arg(storageId);
                return false;
            }
            if (s.bitOffset + s.bitWidth > u32(storageBytes * 8)) {
                error = QStringLiteral("WVZ4 SIGT signal_id %1 bit range exceeds storage_id %2 capacity")
                    .arg(int(s.signalId)).arg(storageId);
                return false;
            }
        }
        if (!allSelectedStorageIds) {
            for (QSet<int>::const_iterator it = selectedIds.constBegin(); it != selectedIds.constEnd(); ++it) {
                if (clockSignalIds.contains(*it)) continue;
                const int storageId = directIntMapValue(storageIdBySignalId, *it, -1);
                if (storageId > 0) selectedStorageIds.insert(storageId);
            }
        }
        allLodSelectedStorageIds = allSelectedStorageIds;
        lodSelectedStorageIds = selectedStorageIds;
        if (!allLodSelectedStorageIds && lodSelectedStorageIds.isEmpty() &&
            options.autoLoadFirstSignalLodCount >= 0) {
            int loaded = 0;
            for (int i = 0; i < sigs.size() && loaded < options.autoLoadFirstSignalLodCount; ++i) {
                if (!isVisibleSignal(sigs.at(i))) continue;
                if (clockSignalIds.contains(int(sigs.at(i).signalId))) {
                    ++loaded;
                    continue;
                }
                const int storageId = directIntMapValue(storageIdBySignalId, int(sigs.at(i).signalId), -1);
                if (storageId > 0) lodSelectedStorageIds.insert(storageId);
                ++loaded;
            }
        }

        auto makeWaveSignal = [&](const SigRec& s, bool selected) -> WaveSignal {
            WaveSignal sig;
            sig.signalId = int(s.signalId);
            sig.storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
            sig.bitOffset = int(s.bitOffset);
            // Keep only the leaf segment here. The complete path is reconstructed
            // from WaveTreeInfo only when the signal is added to the active list or exported.
            // On sample-only reloads MainWindow only consumes signal_id + samples,
            // so NAME/NODE can be skipped completely.
            sig.name.clear();
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
                if (!isVisibleSignal(s)) continue;
                if (s.nodeId >= u32(nodesById.size()) || !nodesById[int(s.nodeId)].valid) {
                    error = QStringLiteral("WVZ4 SIGT references missing node_id %1").arg(int(s.nodeId));
                    return false;
                }
                const int idx = outputSignals.size();
                const bool rawSelected = options.loadRawSamples &&
                    (allSelectedSignalIds || selectedIds.contains(int(s.signalId)));
                outputSignals.push_back(makeWaveSignal(s, rawSelected));
                directIntMapSet(outputIndexBySignalId, int(s.signalId), idx);
                if (outputSignals.at(idx).samplesLoaded &&
                    !clockSignalIds.contains(int(s.signalId))) {
                    const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
                    directIntListMapAppend(outputIndexesByStorageId, storageId, idx);
                }
            }
        } else {
            outputSignals.reserve(allSelectedSignalIds ? sigs.size() : selectedIds.size());
            outputIndexBySignalId.reserve(allSelectedSignalIds ? sigs.size() : selectedIds.size());
            for (int i = 0; i < sigs.size(); ++i) {
                const SigRec& s = sigs.at(i);
                if (!isVisibleSignal(s)) continue;
                if (!allSelectedSignalIds && !selectedIds.contains(int(s.signalId))) continue;
                const int idx = outputSignals.size();
                outputSignals.push_back(makeWaveSignal(s, options.loadRawSamples));
                directIntMapSet(outputIndexBySignalId, int(s.signalId), idx);
                if (options.loadRawSamples &&
                    !clockSignalIds.contains(int(s.signalId))) {
                    const int storageId = int(s.storageId != 0 ? s.storageId : s.signalId);
                    directIntListMapAppend(outputIndexesByStorageId, storageId, idx);
                }
            }
        }

        if (!applyProceduralClockDefinitions(clocks,
                                             outputIndexBySignalId,
                                             outputSignals,
                                             options.includeAllSignalDefinitions,
                                             error)) {
            return false;
        }

        samplesByOutputIndex.resize(outputSignals.size());

        if (options.loadRawSamples) {
            qint64 implicitZeroTime = 0;
            if (!footerBlocks.isEmpty()) {
                implicitZeroTime = footerBlocks.first().start;
                for (int i = 1; i < footerBlocks.size(); ++i) {
                    implicitZeroTime = qMin(implicitZeroTime, footerBlocks.at(i).start);
                }
            } else {
                // Finalized files discover FOOT after the first WDAT header.
                // Rebase the temporary implicit-zero sample once FOOT is read.
                implicitZeroSamplesNeedFooterRebase = true;
            }
            if (!appendImplicitZeroSamplesForSelectedSignals(selectedIds, allSelectedSignalIds,
                                                             implicitZeroTime,
                                                             outputSignals, samplesByOutputIndex,
                                                             minTime, maxTime, error)) {
                return false;
            }
        }

        if (options.includeAllSignalDefinitions) {
            if (!buildWaveTreeInfo(nodesById, namesById, sigs, outputIndexBySignalId,
                                   outputSignals.size(), outWave.tree, error)) {
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

            // FOOT is a real random-access tile index. Use it instead
            // of linearly walking every WDAT tile.  This avoids O(number of all
            // tiles) outer-header scans when only a few signal chunks are loaded.
            if (footerOffset != 0) {
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
                if (!parseFooterSection(footerPayload, byteWidthByStorageId, boolStorageByStorageId,
                                        footerBlocks, footerBlockIndexesByChunk,
                                        footerLodLevelsByStorageId, footerLodChunkIndex, error)) return false;
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

            if (!options.loadRawSamples || (selectedStorageIds.isEmpty() && !allSelectedStorageIds)) {
                if (!skipSectionPayload(file, sh, error)) return false;
                continue;
            }

            if (!decodeWdatSectionStreaming(file, sh, selectedStorageIds, allSelectedStorageIds,
                                            storageOutputIndexLookup(outputIndexesByStorageId),
                                            byteWidthByStorageId,
                                            outputSignals, samplesByOutputIndex,
                                            options.timeStart, options.timeEnd,
                                            minTime, maxTime, error, rawLeftAnchorPtr())) {
                return false;
            }
            continue;
        }

        if (sampleOnlyLoad &&
            (sh.tag == "NAME" || sh.tag == "NAMZ" || sh.tag == "NREF" || sh.tag == "NRFZ" ||
             sh.tag == "NODI" || sh.tag == "NIZ2")) {
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
        } else if (sh.tag == "NREF") {
            if (!parseNodeReferenceSection(payload, nodesById, error)) return false;
        } else if (sh.tag == "NRFZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "NRFZ", error);
            if (!error.isEmpty()) return false;
            if (!parseNodeReferenceSection(raw, nodesById, error)) return false;
        } else if (sh.tag == "NODI") {
            if (!parseCompactNodeReferenceSection(payload, nodesById, error)) return false;
        } else if (sh.tag == "NIZ2") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "NIZ2", error);
            if (!error.isEmpty()) return false;
            if (!parseCompactNodeReferenceSection(raw, nodesById, error)) return false;
        } else if (sh.tag == "SIGT") {
            if (!parseSignalSection(payload, sigs, error)) return false;
        } else if (sh.tag == "SIGZ") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "SIGZ", error);
            if (!error.isEmpty()) return false;
            if (!parseSignalSection(raw, sigs, error)) return false;
        } else if (sh.tag == "SIGD") {
            if (!parseCompactSignalSection(payload, sigs, error)) return false;
        } else if (sh.tag == "SGZ2") {
            const QByteArray raw = decodeCompressedLayoutPayload(payload, "SGZ2", error);
            if (!error.isEmpty()) return false;
            if (!parseCompactSignalSection(raw, sigs, error)) return false;
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
            if (!parseFooterSection(payload, byteWidthByStorageId, boolStorageByStorageId,
                                    footerBlocks, footerBlockIndexesByChunk,
                                    footerLodLevelsByStorageId, footerLodChunkIndex, error)) return false;
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

    if (options.loadRawSamples && useFooterIndexedWdat &&
        (allSelectedStorageIds || !selectedStorageIds.isEmpty())) {
        if (!decodeWdatSectionsFromFooterIndex(file, footerBlocks, footerBlockIndexesByChunk,
                                               headerSignalsPerChunk,
                                               selectedStorageIds, allSelectedStorageIds,
                                               storageOutputIndexLookup(outputIndexesByStorageId),
                                               byteWidthByStorageId,
                                               outputSignals, samplesByOutputIndex,
                                               options.timeStart, options.timeEnd,
                                               minTime, maxTime, rawLeftAnchorPtr(), error)) {
            return false;
        }
    }

    if (options.loadRawSamples && !flushRawLeftAnchors()) {
        return false;
    }

    const qint64 lodDecodeTargetBucketCycles = (residualLodTables && options.loadRawSamples)
        ? 0
        : options.lodTargetBucketCycles;
    if (!decodeLodzChunksFromFooterIndexParallel(
            filePath, footerLodChunkIndex, headerSignalsPerChunk,
            lodSelectedStorageIds, allLodSelectedStorageIds,
            byteWidthByStorageId, footerLodLevelsByStorageId,
            options.timeStart, options.timeEnd,
            lodDecodeTargetBucketCycles, false, -1, error)) {
        return false;
    }

    if (residualLodTables && options.loadRawSamples) {
        appendResidualLodToRawSamples(footerLodLevelsByStorageId, selectedIds, allSelectedSignalIds,
                                      outputSignals, samplesByOutputIndex);
    }

    if (residualLodTables) {
        makeResidualLodLevelsCumulative(footerLodLevelsByStorageId);
    }

    for (int i = 0; i < footerBlocks.size(); ++i) {
        const BlockIndexRec& b = footerBlocks.at(i);
        minTime = qMin(minTime, b.start);
        maxTime = qMax(maxTime, b.end);
    }

    if (implicitZeroSamplesNeedFooterRebase && !footerBlocks.isEmpty()) {
        qint64 footerStart = footerBlocks.first().start;
        for (int i = 1; i < footerBlocks.size(); ++i) {
            footerStart = qMin(footerStart, footerBlocks.at(i).start);
        }
        if (footerStart > 0) {
            for (int i = 0; i < samplesByOutputIndex.size(); ++i) {
                QVector<WaveSample>& rows = samplesByOutputIndex[i];
                if (rows.isEmpty() || rows.first().time != 0) continue;
                if (rows.size() > 1 && rows.at(1).time == footerStart) {
                    rows.removeFirst(); // The real sample at the same time wins.
                } else {
                    rows[0].time = footerStart;
                }
            }
            minTime = footerStart;
        }
    }

    for (int i = 0; i < outputSignals.size(); ++i) {
        const int storageId = outputSignals.at(i).storageId > 0
            ? outputSignals.at(i).storageId
            : directIntMapValue(storageIdBySignalId, outputSignals.at(i).signalId, outputSignals.at(i).signalId);
        if (storageId > 0 && storageId < footerLodLevelsByStorageId.size()) {
            const QVector<WaveLodLevel>& storageLevels = footerLodLevelsByStorageId.at(storageId);
            outputSignals[i].lodLevels.clear();
            outputSignals[i].lodLevels.reserve(storageLevels.size());
            for (const WaveLodLevel& level : storageLevels) {
                outputSignals[i].lodLevels.push_back(sliceLodLevelForSignal(level, outputSignals.at(i)));
            }
        }
        outputSignals[i].samples = std::move(samplesByOutputIndex[i]);
        if (options.loadRawSamples &&
            (allSelectedSignalIds || selectedIds.contains(outputSignals.at(i).signalId))) {
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
