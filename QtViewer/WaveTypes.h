#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <vector>

constexpr quint32 kWaveNameTokenArrayFlag = 0x80000000u;
constexpr quint32 kWaveNameTokenValueMask = 0x7fffffffu;

inline quint32 waveNameIdToken(quint32 nameId) {
    return nameId & kWaveNameTokenValueMask;
}

inline quint32 waveArrayIndexToken(quint32 arrayIndex) {
    return kWaveNameTokenArrayFlag | (arrayIndex & kWaveNameTokenValueMask);
}

inline bool waveNameTokenIsArrayIndex(quint32 token) {
    return (token & kWaveNameTokenArrayFlag) != 0;
}

inline quint32 waveNameTokenValue(quint32 token) {
    return token & kWaveNameTokenValueMask;
}

enum class SignalKind {
    Bit,
    Bus
};

enum class ValueRadix {
    Bin,
    Hex,
    Dec,
    Int,
    UInt,
    Float,
    Int64,
    UInt64,
    Double
};

struct WaveSample {
    qint64 time = 0;
    QString value;          // Optional text cache; display logic should prefer rawBits/isZ/isAbsent.
    quint64 rawBits = 0;
    bool isZ = false;
    bool isAbsent = false;
    bool rawFieldsReady = false;
};

static const quint8 kWaveLodSeenZero = 0x1u;
static const quint8 kWaveLodSeenNonZero = 0x2u;
static const quint8 kWaveLodSeenZ = 0x4u;
static const quint8 kWaveLodSeenAbsent = 0x8u;

struct WaveLodBucket {
    qint64 start = 0;
    qint64 end = 0;
    quint64 firstRawBits = 0;
    quint64 lastRawBits = 0;
    quint64 minRawBits = 0;
    quint64 maxRawBits = 0;
    quint32 transitionCount = 0;
    quint8 stateMask = 0;
};

struct WaveLodValidRange {
    qint64 start = 0;
    qint64 end = 0;
};

struct WaveLodLevel {
    qint64 bucketCycles = 0;
    QVector<WaveLodBucket> buckets;
    QVector<WaveSample> samples;
    QVector<WaveLodValidRange> validRanges;
    QVector<WaveLodValidRange> loadedRanges;
};

struct WaveDiffRegion {
    qint64 start = 0;
    qint64 end = 0;
};

struct WaveSignal {
    // Stable WVZ4 v17 signal id. MainWindow uses it for on-demand loading without
    // assuming signalList index == file id.
    int signalId = -1;
    // Physical WVZ4 storage stream id. Several logical signal ids may share one
    // storage stream through WVZ4 aliases.
    int storageId = -1;
    int bitOffset = 0;
    // Explicit name for synthetic/non-tree signals. Normal WVZ4 file signals
    // leave this empty and resolve their segment through WaveTreeInfo tokens.
    QString name;
    SignalKind kind = SignalKind::Bit;
    int width = 1;
    ValueRadix defaultRadix = ValueRadix::Bin;
    ValueRadix currentRadix = ValueRadix::Bin;
    bool supportsZState = false;
    // Periodic clocks are stored by WVZ4 CLKD as a formula rather than WDAT
    // transitions. Keep that formula all the way through the Viewer instead
    // of expanding an arbitrarily large temporary sample vector.
    bool proceduralClock = false;
    bool clockInitialValue = false;
    quint64 clockTogglePeriodTicks = 0;
    // Viewer-only signal synthesized from a canonical std::bitset word stream.
    // signalId is in the virtual range above the file signal table; storageId
    // and bitOffset select the physical U64 stream stored in WVZ4.
    bool virtualBitsetBit = false;
    // Viewer-only leaf synthesized from an ARRY element schema. The file stores
    // byte patches for the owning array node instead of a WDAT scalar stream.
    bool virtualArrayLeaf = false;
    int arrayNodeId = 0;
    quint64 arrayByteOffset = 0;
    int arrayByteWidth = 0;
    // True means samples already cover the whole file time range for this signal.
    // Directory-only and partial v17 loads leave this false.
    bool samplesLoaded = true;
    QVector<WaveSample> samples;
    // WVZ4 on-demand display path may keep only the current raw viewport plus
    // prefetch. samplesLoaded still means full-file coverage; this range list
    // describes partial raw coverage when samplesLoaded is false.
    QVector<WaveLodValidRange> rawLoadedRanges;
    // Optional compare-mode overlay. Each interval marks a time span where the
    // paired waveform has a different value. Empty means normal waveform mode.
    QVector<WaveDiffRegion> diffRegions;
    // Optional render-only time window. Compare views use this to leave one
    // side blank outside that file's own cycle range without inserting absent samples.
    bool hasVisibleRange = false;
    qint64 visibleStart = 0;
    qint64 visibleEnd = 0;
    // Derived cache for fast navigation. Contains times where samples[i] differs from samples[i-1].
    QVector<qint64> changeTimes;
    bool changeTimesReady = false;
    // WVZ4 v17 file-level overview data loaded from chunked LODZ streams.
    QVector<WaveLodLevel> lodLevels;
};

using WaveSignalList = std::vector<WaveSignal>;

inline int waveSignalCount(const WaveSignalList& signalList) noexcept {
    if (signalList.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        qFatal("Wave signal count exceeds the Qt container index limit");
    }
    return static_cast<int>(signalList.size());
}

inline bool waveProceduralClockValueAtTime(const WaveSignal& sig, qint64 time) {
    if (!sig.proceduralClock || sig.clockTogglePeriodTicks == 0) {
        return sig.clockInitialValue;
    }
    const quint64 nonNegativeTime = time > 0 ? quint64(time) : 0u;
    const bool oddToggle = ((nonNegativeTime / sig.clockTogglePeriodTicks) & 1u) != 0u;
    return oddToggle ? !sig.clockInitialValue : sig.clockInitialValue;
}

inline WaveSample waveProceduralClockSampleAtTime(const WaveSignal& sig, qint64 time) {
    WaveSample sample;
    sample.time = time;
    sample.rawBits = waveProceduralClockValueAtTime(sig, time) ? 1u : 0u;
    sample.rawFieldsReady = true;
    return sample;
}

// Returns the first clock transition at or after time. Transitions occur at
// positive multiples of clockTogglePeriodTicks; -1 means no representable
// transition exists in the qint64 time domain.
inline qint64 waveProceduralClockTransitionAtOrAfter(const WaveSignal& sig, qint64 time) {
    if (!sig.proceduralClock || sig.clockTogglePeriodTicks == 0) return -1;
    const quint64 period = sig.clockTogglePeriodTicks;
    quint64 multiple = 1u;
    if (time > 0) {
        const quint64 t = quint64(time);
        multiple = t / period;
        if ((t % period) != 0u) ++multiple;
        if (multiple == 0u) multiple = 1u;
    }
    const quint64 maxTime = quint64((std::numeric_limits<qint64>::max)());
    if (multiple > maxTime / period) return -1;
    return qint64(multiple * period);
}

inline qint64 waveProceduralClockNextTransition(const WaveSignal& sig, qint64 time) {
    if (!sig.proceduralClock || sig.clockTogglePeriodTicks == 0) return -1;
    const quint64 period = sig.clockTogglePeriodTicks;
    quint64 multiple = 1u;
    if (time >= 0) {
        const quint64 t = quint64(time);
        multiple = t / period + 1u;
    }
    const quint64 maxTime = quint64((std::numeric_limits<qint64>::max)());
    if (multiple == 0u || multiple > maxTime / period) return -1;
    return qint64(multiple * period);
}

inline qint64 waveProceduralClockPreviousTransition(const WaveSignal& sig, qint64 time) {
    if (!sig.proceduralClock || sig.clockTogglePeriodTicks == 0 || time <= 0) return -1;
    const quint64 period = sig.clockTogglePeriodTicks;
    const quint64 multiple = quint64(time - 1) / period;
    return multiple == 0u ? -1 : qint64(multiple * period);
}

inline bool waveSignalRawSamplesCoverRange(const WaveSignal& sig, qint64 start, qint64 end) {
    if (sig.proceduralClock) return true;
    if (sig.samplesLoaded) return true;
    if (end <= start) return true;
    qint64 cursor = start;
    for (const WaveLodValidRange& range : sig.rawLoadedRanges) {
        if (range.end <= cursor) continue;
        if (range.start > cursor) return false;
        cursor = qMax(cursor, range.end);
        if (cursor >= end) return true;
    }
    return false;
}


struct WaveTreeNode {
    int parentId = 0;
    int firstChild = 0;
    int nextSibling = 0;
    int rowInParent = -1;
    int signalIndex = -1;   // -1 means module/container node
    int signalId = -1;
    int referenceTargetId = 0;
    quint32 nameToken = 0;  // NAME id, or kWaveNameTokenArrayFlag | numeric array index
    quint8 kind = 0;
    bool valid = false;
};

struct WaveBitsetInfo {
    int nodeId = 0;
    int firstStorageId = 0;
    int bitCount = 0;
    int wordCount = 0;
    int firstVirtualSignalId = 0;
    int firstMaterializedNodeId = 0;
    bool materialized = false;
};

enum class WaveArraySchemaKind : quint8 {
    Object = 1,
    Field = 2,
    Array = 3,
    Leaf = 4
};

struct WaveArraySchemaNode {
    int schemaNodeId = 0;
    int parentSchemaNodeId = 0;
    quint32 nameToken = 0;
    WaveArraySchemaKind kind = WaveArraySchemaKind::Field;
    quint64 byteOffset = 0;
    quint64 byteSize = 0;
    quint64 elementCount = 0;
    quint64 elementStride = 0;
    quint8 valueType = 0;
    int bitWidth = 0;
    int bitOffset = 0;
};

struct WaveArrayInfo {
    int nodeId = 0;
    quint64 totalBytes = 0;
    quint64 elementCount = 0;
    quint64 elementStride = 0;
    QVector<WaveArraySchemaNode> schema;
    int firstVirtualSignalId = 0;
    quint64 leafCountPerElement = 0;
    int firstMaterializedNodeId = 0;
    quint64 materializedElementCount = 0;
    bool materialized = false;
};

constexpr quint8 kWaveTreeNodeKindReference = 7;

struct WaveTreeInfo {
    bool valid = false;
    // File NAME strings are stored once. Numeric v17 array segments never enter
    // this table and are formatted only when a consumer actually needs text.
    QVector<QByteArray> namesById;
    QVector<WaveTreeNode> nodesById;
    QVector<int> rootNodeIds;
    QVector<int> signalIndexToNodeId;
    // Directory-only v17 loads build this while decoding SIGD. Values store
    // signalIndex + 1 so zero remains the missing-id sentinel used by MainWindow.
    QVector<int> signalIndexBySignalId;
    QVector<WaveBitsetInfo> bitsets;
    // Values store bitset index + 1; zero is the non-bitset sentinel.
    QVector<int> bitsetIndexByNodeId;
    QVector<WaveArrayInfo> arrays;
    // Values store array index + 1; zero is the non-array sentinel.
    QVector<int> arrayIndexByNodeId;
};

// A subtree-reference patch is built off the UI thread. Negative structural
// node ids in appendedNodes are patch-local ids; applying the patch remaps
// them to the destination tree's appended node range.
struct WaveSubtreeReferencePatch {
    int mountNodeId = 0;
    WaveTreeNode mountNode;
    QVector<WaveTreeNode> appendedNodes;
};

struct WaveMeta {
    QString title = "custom_wave";
    QString timescale = "1ns";
    qint64 start = 0;
    qint64 end = 100;
    bool hasCompareSources = false;
    QString compareLeftLabel;
    QString compareLeftPath;
    qint64 compareLeftStart = 0;
    qint64 compareLeftEnd = 0;
    QString compareRightLabel;
    QString compareRightPath;
    qint64 compareRightStart = 0;
    qint64 compareRightEnd = 0;
};

struct WaveFile {
    WaveMeta meta;
    WaveTreeInfo tree;
    WaveSignalList signalList;
};

inline QString waveTreeNameTokenText(const WaveTreeInfo& tree, quint32 token) {
    if (waveNameTokenIsArrayIndex(token)) {
        return QStringLiteral("[%1]").arg(waveNameTokenValue(token));
    }
    const quint32 nameId = waveNameTokenValue(token);
    if (nameId > 0 && nameId < quint32(tree.namesById.size())) {
        return QString::fromUtf8(tree.namesById.at(int(nameId)));
    }
    return QString();
}

inline QString waveTreeNodeSegmentName(const WaveTreeInfo& tree, int nodeId) {
    if (nodeId <= 0 || nodeId >= tree.nodesById.size()) return QString();
    const WaveTreeNode& node = tree.nodesById.at(nodeId);
    if (!node.valid) return QString();
    return waveTreeNameTokenText(tree, node.nameToken);
}

// A subtree reference keeps its own tree identity (name, parent and path), but
// a reference mounted directly on a signal leaf must behave like that leaf in
// the Viewer.  Resolve only the signal identity here; callers must continue to
// use the original node id for presentation and navigation.
inline int waveTreeEffectiveSignalNodeId(const WaveTreeInfo& tree, int nodeId) {
    int current = nodeId;
    int guard = 0;
    while (current > 0 && current < tree.nodesById.size() &&
           guard++ < tree.nodesById.size()) {
        const WaveTreeNode& node = tree.nodesById.at(current);
        if (!node.valid) return 0;
        if (node.signalIndex >= 0) return current;
        if (node.kind != kWaveTreeNodeKindReference ||
            node.referenceTargetId <= 0 ||
            node.referenceTargetId >= tree.nodesById.size()) {
            return 0;
        }
        current = node.referenceTargetId;
    }
    return 0;
}

inline int waveTreeEffectiveSignalIndex(const WaveTreeInfo& tree, int nodeId) {
    const int signalNodeId = waveTreeEffectiveSignalNodeId(tree, nodeId);
    return signalNodeId > 0 ? tree.nodesById.at(signalNodeId).signalIndex : -1;
}

inline int waveTreeEffectiveSignalId(const WaveTreeInfo& tree, int nodeId) {
    const int signalNodeId = waveTreeEffectiveSignalNodeId(tree, nodeId);
    return signalNodeId > 0 ? tree.nodesById.at(signalNodeId).signalId : -1;
}

inline int waveTreeSearchPathEndNodeFrom(
        const WaveTreeInfo& tree, int nodeId, const QStringList& parts,
        int partIndex, Qt::CaseSensitivity caseSensitivity,
        int referenceMountNodeId = 0) {
    if (nodeId <= 0 || nodeId >= tree.nodesById.size() ||
        partIndex < 0 || partIndex >= parts.size()) {
        return 0;
    }
    const WaveTreeNode& node = tree.nodesById.at(nodeId);
    if (!node.valid || QString::compare(
            waveTreeNameTokenText(tree, node.nameToken), parts.at(partIndex),
            caseSensitivity) != 0) {
        return 0;
    }
    if (partIndex == parts.size() - 1) {
        return referenceMountNodeId > 0 ? referenceMountNodeId : nodeId;
    }

    int childSourceId = nodeId;
    int resultNodeId = referenceMountNodeId;
    int referenceGuard = 0;
    while (childSourceId > 0 && childSourceId < tree.nodesById.size() &&
           tree.nodesById.at(childSourceId).valid &&
           tree.nodesById.at(childSourceId).firstChild == 0 &&
           tree.nodesById.at(childSourceId).kind == kWaveTreeNodeKindReference &&
           ++referenceGuard < tree.nodesById.size()) {
        if (resultNodeId <= 0) resultNodeId = childSourceId;
        childSourceId = tree.nodesById.at(childSourceId).referenceTargetId;
    }
    if (childSourceId <= 0 || childSourceId >= tree.nodesById.size() ||
        !tree.nodesById.at(childSourceId).valid) {
        return 0;
    }

    int childGuard = 0;
    for (int childId = tree.nodesById.at(childSourceId).firstChild;
         childId != 0 && childGuard++ < tree.nodesById.size();
         childId = tree.nodesById.at(childId).nextSibling) {
        const int result = waveTreeSearchPathEndNodeFrom(
            tree, childId, parts, partIndex + 1, caseSensitivity,
            resultNodeId);
        if (result > 0) return result;
    }
    return 0;
}

inline QString waveSignalSegmentName(const WaveFile& wave, int signalIndex) {
    if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return QString();
    if (wave.tree.valid && signalIndex < wave.tree.signalIndexToNodeId.size()) {
        const QString segment = waveTreeNodeSegmentName(
            wave.tree, wave.tree.signalIndexToNodeId.at(signalIndex));
        if (!segment.isEmpty()) return segment;
    }
    return wave.signalList.at(signalIndex).name;
}

inline QString waveSignalFullPath(const WaveFile& wave, int signalIndex) {
    if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return QString();
    if (wave.tree.valid && signalIndex < wave.tree.signalIndexToNodeId.size()) {
        int nodeId = wave.tree.signalIndexToNodeId.at(signalIndex);
        QVector<QString> parts;
        int guard = 0;
        while (nodeId > 0 && nodeId < wave.tree.nodesById.size() &&
               guard++ < wave.tree.nodesById.size()) {
            const WaveTreeNode& node = wave.tree.nodesById.at(nodeId);
            if (!node.valid) break;
            const QString segment = waveTreeNameTokenText(wave.tree, node.nameToken);
            if (!segment.isEmpty()) parts.push_back(segment);
            nodeId = node.parentId;
        }
        if (!parts.isEmpty()) {
            std::reverse(parts.begin(), parts.end());
            QString path;
            for (int i = 0; i < parts.size(); ++i) {
                if (i > 0) path += QLatin1Char('.');
                path += parts.at(i);
            }
            return path;
        }
    }
    return wave.signalList.at(signalIndex).name;
}

struct ActiveSignalRef {
    int signalIndex = -1;
    ValueRadix format = ValueRadix::Bin;
};

inline const QString& waveAbsentValue() {
    static const QString kValue = QStringLiteral("__WVZ_ABSENT__");
    return kValue;
}

constexpr qint64 kWaveViewerDisplayTicksPerCycle = 10;

inline QString waveFormatDisplayTime(qint64 internalTime) {
    qint64 decimalBase = kWaveViewerDisplayTicksPerCycle;
    int decimals = 0;
    while (decimalBase > 1 && decimalBase % 10 == 0) {
        decimalBase /= 10;
        ++decimals;
    }

    if (decimalBase != 1) {
        QString text = QString::number(double(internalTime) / double(kWaveViewerDisplayTicksPerCycle), 'f', 6);
        while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
        return text;
    }

    const qint64 whole = internalTime / kWaveViewerDisplayTicksPerCycle;
    qint64 rem = internalTime % kWaveViewerDisplayTicksPerCycle;
    if (rem == 0) return QString::number(whole);
    if (rem < 0) rem = -rem;

    QString frac = QString::number(rem).rightJustified(decimals, QLatin1Char('0'));
    while (frac.endsWith(QLatin1Char('0'))) frac.chop(1);
    const QString wholeText = (internalTime < 0 && whole == 0) ? QStringLiteral("-0") : QString::number(whole);
    return wholeText + QLatin1Char('.') + frac;
}

// WaveMeta::end is an exclusive internal-tick boundary.  File-range labels
// describe the actual business-cycle indexes covered by that half-open range,
// so their right side is the cycle containing end - 1 rather than end itself.
inline QString waveFormatDisplayCycleRangeEnd(qint64 exclusiveEnd) {
    if (exclusiveEnd == (std::numeric_limits<qint64>::min)()) {
        return waveFormatDisplayTime(exclusiveEnd);
    }
    const qint64 lastTick = exclusiveEnd - 1;
    qint64 cycle = lastTick / kWaveViewerDisplayTicksPerCycle;
    if (lastTick < 0 && lastTick % kWaveViewerDisplayTicksPerCycle != 0) --cycle;
    return QString::number(cycle);
}

inline bool waveParseDisplayTime(const QString& text, qint64& internalTime) {
    bool ok = false;
    const double displayTime = text.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(displayTime)) return false;

    const double raw = displayTime * double(kWaveViewerDisplayTicksPerCycle);
    if (raw < double(std::numeric_limits<qint64>::min()) ||
        raw > double(std::numeric_limits<qint64>::max())) {
        return false;
    }

    internalTime = static_cast<qint64>(std::llround(raw));
    return true;
}

inline bool isWaveAbsentValue(const QString& value) {
    return value == waveAbsentValue();
}

inline bool isWaveZValue(const QString& value) {
    const QString trimmed = value.trimmed();
    return trimmed.compare(QStringLiteral("z"), Qt::CaseInsensitive) == 0;
}

inline QString normalizeWaveZValue(const QString& value) {
    return isWaveZValue(value) ? QStringLiteral("Z") : value;
}

inline quint64 waveBitMaskForWidth(const int width) {
    if (width <= 0) return 0ull;
    if (width >= 64) return std::numeric_limits<quint64>::max();
    return (quint64(1) << width) - 1ull;
}

inline bool waveIsDecimalDigitsText(const QString& raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty()) return false;
    for (int i = 0; i < s.size(); ++i) {
        const ushort ch = s.at(i).unicode();
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

inline bool waveIsBinaryDigitsText(const QString& raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty()) return false;
    for (int i = 0; i < s.size(); ++i) {
        const ushort ch = s.at(i).unicode();
        if (ch != '0' && ch != '1') return false;
    }
    return true;
}

inline bool waveIsHexDigitsText(const QString& raw, bool* hasAlpha = nullptr) {
    const QString s = raw.trimmed();
    if (s.isEmpty()) return false;
    bool alpha = false;
    for (int i = 0; i < s.size(); ++i) {
        const ushort ch = s.at(i).unicode();
        const bool dec = (ch >= '0' && ch <= '9');
        const bool upper = (ch >= 'A' && ch <= 'F');
        const bool lower = (ch >= 'a' && ch <= 'f');
        if (!dec && !upper && !lower) return false;
        if (upper || lower) alpha = true;
    }
    if (hasAlpha) *hasAlpha = alpha;
    return true;
}

inline quint64 parseWaveRawBitsText(QString raw) {
    raw = raw.trimmed();
    if (raw.isEmpty() || isWaveZValue(raw) || isWaveAbsentValue(raw)) return 0ull;

    bool ok = false;
    if (raw.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        const QString body = raw.mid(2);
        if (!waveIsHexDigitsText(body)) return 0ull;
        const quint64 v = body.toULongLong(&ok, 16);
        return ok ? v : 0ull;
    }
    if (raw.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) {
        const QString body = raw.mid(2);
        if (!waveIsBinaryDigitsText(body)) return 0ull;
        const quint64 v = body.toULongLong(&ok, 2);
        return ok ? v : 0ull;
    }
    if (waveIsDecimalDigitsText(raw)) {
        const quint64 v = raw.toULongLong(&ok, 10);
        return ok ? v : 0ull;
    }
    if (waveIsHexDigitsText(raw)) {
        const quint64 v = raw.toULongLong(&ok, 16);
        return ok ? v : 0ull;
    }
    return 0ull;
}

inline void hydrateWaveSampleRawFields(const SignalKind kind, const int width, WaveSample& sample) {
    if (sample.rawFieldsReady) return;

    const QString trimmed = sample.value.trimmed();
    sample.isAbsent = isWaveAbsentValue(trimmed);
    sample.isZ = !sample.isAbsent && isWaveZValue(trimmed);
    if (sample.isAbsent || sample.isZ) {
        sample.rawBits = 0ull;
        sample.rawFieldsReady = true;
        return;
    }
    if (kind == SignalKind::Bit) {
        sample.rawBits = (trimmed == QStringLiteral("1")) ? 1ull : 0ull;
        sample.rawFieldsReady = true;
        return;
    }
    sample.rawBits = parseWaveRawBitsText(trimmed) & waveBitMaskForWidth(width);
    sample.rawFieldsReady = true;
}

inline QString waveFormatBinaryValue(const quint64 value, const int width) {
    const quint64 masked = value & waveBitMaskForWidth(width);
    if (width <= 1) return QString::number(masked);
    return QStringLiteral("0b") + QString::number(masked, 2);
}

inline QString waveFormatHexValue(const quint64 value, const int width) {
    const quint64 masked = value & waveBitMaskForWidth(width);
    if (width <= 1) return QString::number(masked);
    return QStringLiteral("0x") + QString::number(masked, 16).toUpper();
}

inline QString waveTrimLeadingZeroDigits(QString digits) {
    int firstNonZero = 0;
    while (firstNonZero + 1 < digits.size() && digits.at(firstNonZero) == QLatin1Char('0')) {
        ++firstNonZero;
    }
    return firstNonZero == 0 ? digits : digits.mid(firstNonZero);
}

inline QString waveSampleRawText(const SignalKind kind, const int width, const ValueRadix radix, const WaveSample& sample) {
    Q_UNUSED(kind);
    // Raw-only WVZ4 samples are formatted from rawBits.  Legacy samples may not
    // have been hydrated yet; in that case preserve their original text instead
    // of accidentally formatting the default rawBits==0 value.
    if (!sample.rawFieldsReady) {
        const QString trimmed = sample.value.trimmed();
        if (sample.isAbsent || isWaveAbsentValue(trimmed)) return waveAbsentValue();
        if (sample.isZ || isWaveZValue(trimmed)) return QStringLiteral("Z");
        return trimmed;
    }

    if (sample.isAbsent) return waveAbsentValue();
    if (sample.isZ) return QStringLiteral("Z");
    const int safeWidth = qMax(1, width);
    const quint64 masked = sample.rawBits & waveBitMaskForWidth(safeWidth);
    switch (radix) {
    case ValueRadix::Bin:
        return waveFormatBinaryValue(masked, safeWidth);
    case ValueRadix::Hex:
        return waveFormatHexValue(masked, safeWidth);
    case ValueRadix::Float: {
        quint32 bits = quint32(masked & 0xffffffffull);
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return QString::number(double(v), 'g', 9);
    }
    case ValueRadix::Double: {
        quint64 bits = sample.rawBits;
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return QString::number(v, 'g', 17);
    }
    default:
        return QString::number(masked);
    }
}

inline QString waveSampleRawText(const WaveSignal& sig, const WaveSample& sample) {
    return waveSampleRawText(sig.kind, sig.width, sig.defaultRadix, sample);
}

inline bool waveSamplesEquivalent(const WaveSample& a, const WaveSample& b) {
    if (a.isAbsent != b.isAbsent || a.isZ != b.isZ) return false;
    if (a.rawFieldsReady && b.rawFieldsReady) return a.rawBits == b.rawBits;

    // Do not compare text-only samples by the default rawBits field.
    // Older parsers may compact rows before derived raw fields are rebuilt; treating
    // two unhydrated samples as rawBits==0 would incorrectly drop real transitions.
    if (!a.rawFieldsReady && !b.rawFieldsReady) return a.value == b.value;

    // Mixed raw/text samples cannot be compared safely without signal kind/width.
    // Keep both samples rather than accidentally compacting a real transition.
    return false;
}

inline void rebuildWaveSignalDerivedCaches(WaveSignal& sig) {
    sig.changeTimes.clear();
    sig.changeTimesReady = true;
    if (sig.samples.isEmpty()) return;

    for (int i = 0; i < sig.samples.size(); ++i) {
        hydrateWaveSampleRawFields(sig.kind, sig.width, sig.samples[i]);
    }
    if (sig.samples.size() < 2) return;

    sig.changeTimes.reserve(sig.samples.size() - 1);
    for (int i = 1; i < sig.samples.size(); ++i) {
        if (!waveSamplesEquivalent(sig.samples.at(i), sig.samples.at(i - 1))) {
            sig.changeTimes.push_back(sig.samples.at(i).time);
        }
    }
}

inline void rebuildWaveFileDerivedCaches(WaveFile& wave) {
    for (int i = 0; i < wave.signalList.size(); ++i) {
        WaveSignal& sig = wave.signalList[i];
        if (sig.samplesLoaded || !sig.samples.isEmpty()) {
            rebuildWaveSignalDerivedCaches(sig);
        } else {
            sig.changeTimes.clear();
            sig.changeTimesReady = false;
        }
    }
}
