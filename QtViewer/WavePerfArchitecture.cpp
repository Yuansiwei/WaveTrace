#include "WavePerfArchitecture.h"
#include "WavePerfBandwidth.h"

#include <cmath>

#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>

namespace waveperf {

const QVector<InstructionFeatureSpec>& instructionFeatureSpecs() {
    static const QVector<InstructionFeatureSpec> specs = {
        {QStringLiteral("movrel_src1_is_gr"),
         QStringLiteral("MOVRELSrc1IsGR"),
         QStringLiteral("movrel")},
        {QStringLiteral("all_thread_exit"),
         QStringLiteral("allThreadExit"),
         QStringLiteral("runtime")},
        {QStringLiteral("clause"), QStringLiteral("clause"),
         QStringLiteral("bundle")},
        {QStringLiteral("ebb"), QStringLiteral("ebb"),
         QStringLiteral("bundle")},
        {QStringLiteral("pc_24_bit"), QStringLiteral("is24BitPC"),
         QStringLiteral("addressing")},
        {QStringLiteral("barrier"), QStringLiteral("isBarrier"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("branch"), QStringLiteral("isBranch"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("exit"), QStringLiteral("isExit"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("flow_control"), QStringLiteral("isFlowCtrl"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("fence"), QStringLiteral("isFence"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("group_load"), QStringLiteral("isGLoad"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("global_memory"), QStringLiteral("isGlobalMem"),
         QStringLiteral("memory")},
        {QStringLiteral("ldmb"), QStringLiteral("isLDMB"),
         QStringLiteral("memory")},
        {QStringLiteral("ldg_stl"), QStringLiteral("isLdgStl"),
         QStringLiteral("memory")},
        {QStringLiteral("local_memory"), QStringLiteral("isLocalMem"),
         QStringLiteral("memory")},
        {QStringLiteral("mb_allocate"), QStringLiteral("isMBAllocate"),
         QStringLiteral("memory")},
        {QStringLiteral("mb_deallocate"),
         QStringLiteral("isMBDeallocate"),
         QStringLiteral("memory")},
        {QStringLiteral("memory_barrier"),
         QStringLiteral("isMemBarrier"),
         QStringLiteral("memory")},
        {QStringLiteral("pdt"), QStringLiteral("isPDT"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("stmb"), QStringLiteral("isSTMB"),
         QStringLiteral("memory")},
        {QStringLiteral("tac_umma"), QStringLiteral("isTacUmma"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("wait_dep_cnt"),
         QStringLiteral("isWaitDepCnt"),
         QStringLiteral("dependency")},
        {QStringLiteral("relative_pc"), QStringLiteral("is_relative_pc"),
         QStringLiteral("addressing")},
        {QStringLiteral("movrel"), QStringLiteral("isMOVREL"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("needs_fast_fcu_check"),
         QStringLiteral("needFastFcuCheck"),
         QStringLiteral("dispatch")},
        {QStringLiteral("no_uop"), QStringLiteral("noUop"),
         QStringLiteral("instruction_kind")},
        {QStringLiteral("read_dep_count_valid"),
         QStringLiteral("rdChkDepCntVld"),
         QStringLiteral("dependency")},
        {QStringLiteral("to_fast_fcu"), QStringLiteral("toFastFCU"),
         QStringLiteral("runtime")},
        {QStringLiteral("write_dep_count_valid"),
         QStringLiteral("wrChkDepCntVld"),
         QStringLiteral("dependency")}
    };
    return specs;
}

QString instIssueTypeName(quint64 value) {
    // InstIssueType in the active cmodel:
    // -1 Invalid, 0 NotIssue, 1 Thread, 2 ThreadEbb, 3 Group,
    // 4 GroupEbb, 5 CB, 6 QppuMMA, 7 QppuMMAEbb, 8 Count.
    switch (value) {
    case 0: return QStringLiteral("NotIssue");
    case 1: return QStringLiteral("Thread");
    case 2: return QStringLiteral("Thread EBB");
    case 3: return QStringLiteral("Group");
    case 4: return QStringLiteral("Group EBB");
    case 5: return QStringLiteral("CB");
    case 6: return QStringLiteral("QPPU MMA");
    case 7: return QStringLiteral("QPPU MMA EBB");
    case 8: return QStringLiteral("Count");
    default: return QStringLiteral("Invalid/unknown(%1)").arg(value);
    }
}

QString instIssueClassKey(quint64 value) {
    switch (value) {
    case 1:
    case 2:
        return QStringLiteral("thread");
    case 3:
    case 4:
        return QStringLiteral("group");
    case 5:
        return QStringLiteral("cb");
    case 6:
    case 7:
        return QStringLiteral("mma");
    case 0:
        return QStringLiteral("not_issue");
    default:
        return QStringLiteral("unknown_%1").arg(value);
    }
}

QString cbCtrlInstClientName(quint64 value) {
    // CBCtrlInstClient in vsiQPPUTypes.hpp.
    static const char* const names[] = {
        "IMGLDST", "PSO", "TEXTURE", "FP64", "LDST", "FCU", "TAC",
        "UMMA", "TotalNum", "MBAlloc", "DTF", "BWGBarrier",
        "NonCBInstrEbb"
    };
    return value < sizeof(names) / sizeof(names[0])
               ? QString::fromLatin1(names[value])
               : QStringLiteral("Invalid/unknown(%1)").arg(value);
}

QString cbDataClientName(quint64 value) {
    // CBDataClient in vsiPPUTypes.hpp.
    static const char* const names[] = {
        "FlowControl", "CXR", "MemoryData", "MemoryAddress", "LoadData",
        "Match", "Shuffle", "Vote", "Reduce", "Float64", "Movm",
        "GCCache", "Barrier", "Csbg", "Umma", "Tac", "ClientCount"
    };
    return value < sizeof(names) / sizeof(names[0])
               ? QString::fromLatin1(names[value])
               : QStringLiteral("Invalid/unknown(%1)").arg(value);
}

QString cbDataInstQueueClientName(quint64 value) {
    // CBDataInstQueueClient in vsiPPUTypes.hpp.
    static const char* const names[] = {
        "FlowControl", "MemoryData", "MemoryAddress", "Match",
        "ShuffleMisc", "Reduce", "Float64", "Is1WgLdlstmb",
        "Is2WgLdlstmb", "Is1WgTcInst", "Is2WgTcInst", "IsTac",
        "InstQueueClientCount"
    };
    return value < sizeof(names) / sizeof(names[0])
               ? QString::fromLatin1(names[value])
               : QStringLiteral("Invalid/unknown(%1)").arg(value);
}

namespace {

struct ScopeDescriptor {
    QString classKey;
    QString className;
    QString provenance;
    QString path;
    QString familyKey;
    int arrayIndex = -1;
    int declaredSize = 0;
};

struct EuChannelAccumulator {
    qint64 nonemptyTicks = 0;
    qint64 occupancyKnownTicks = 0;
    qint64 occupancyExpectedTicks = 0;
    qint64 fullTicks = 0;
    qint64 fullKnownTicks = 0;
    qint64 resources = 0;
    quint64 readEvents = 0;
    quint64 writeEvents = 0;
    qint64 readSources = 0;
    qint64 writeSources = 0;
    qint64 readUnknownTicks = 0;
    qint64 writeUnknownTicks = 0;
    QSet<QString> readResources;
    QSet<QString> writeResources;
};

struct ProfileAccumulator {
    qint64 counters = 0;
    qint64 dynamicSignals = 0;
    qint64 transitions = 0;
    qint64 activeSignalTicks = 0;
    qint64 unknownSignalTicks = 0;
    qint64 issueTicks = 0;
    qint64 issueActiveTicks = 0;
    qint64 dualIssueTicks = 0;
    qint64 idleTicks = 0;
    qint64 issueCapacityTicks = 0;
    qint64 issueExpectedTicks = 0;
    qint64 issueContexts = 0;
    QVector<qint64> issueSlotTicks;
    QVector<qint64> issueSlotCapacityTicks;
    QVector<qint64> issueSlotExpectedTicks;
    qint64 stallTicks = 0;
    qint64 pendingTicks = 0;
    qint64 busyTicks = 0;
    qint64 validTicks = 0;
    qint64 fifoNonemptyTicks = 0;
    quint64 fifoReadEvents = 0;
    quint64 fifoWriteEvents = 0;
    qint64 fifoReadSources = 0;
    qint64 fifoWriteSources = 0;
    qint64 fifoReadUnknownTicks = 0;
    qint64 fifoWriteUnknownTicks = 0;
    QSet<QString> fifoReadResources;
    QSet<QString> fifoWriteResources;
    quint64 cacheMissEvents = 0;
    quint64 cacheHitEvents = 0;
    qint64 cacheMissSources = 0;
    qint64 cacheHitSources = 0;
    qint64 cacheMissUnknownTicks = 0;
    qint64 cacheHitUnknownTicks = 0;
    qint64 cacheMissDiscontinuities = 0;
    qint64 cacheHitDiscontinuities = 0;
    QSet<QString> cacheMissResources;
    QSet<QString> cacheHitResources;
    qint64 fifoFullTicks = 0;
    qint64 fifoFullKnownTicks = 0;
    qint64 fifoFullResources = 0;
    qint64 queueFullTicks = 0;
    qint64 queueFullKnownTicks = 0;
    qint64 queueFullResources = 0;
    long double creditValueTicks = 0.0L;
    qint64 creditKnownTicks = 0;
    QMap<QString, qint64> keyCounts;
    QMap<QString, qint64> keyActiveTicks;
    QMap<QString, long double> keyWeightedValueTicks;
    QMap<QString, qint64> keyKnownTicks;
    QMap<QString, EuChannelAccumulator> euChannels;
    qint64 euPhase1PendingTicks = 0;
    qint64 euPhase1KnownTicks = 0;
    qint64 euPhase1ExpectedTicks = 0;
};

struct ProfileNode {
    ScopeDescriptor scope;
    int parent = -1;
    QVector<int> children;
    ProfileAccumulator own;
    ProfileAccumulator aggregate;
    QHash<QString, ProfileAccumulator> hotNodes;
    int observedArrayElements = 1;
    int logicalArraySize = 1;
    quint64 representationFactor = 1;
    quint64 representedInstances = 1;
    bool representativeOnly = false;
};

struct FamilyState {
    int declaredSize = 0;
    QSet<int> indexes;
};

struct ClassState {
    QString className;
    QString provenance;
    int observedInstances = 0;
    quint64 representedInstances = 0;
    ProfileAccumulator observed;
    ProfileAccumulator projected;
};

quint64 saturatingAdd(quint64 left, quint64 right) {
    if (right > std::numeric_limits<quint64>::max() - left) {
        return std::numeric_limits<quint64>::max();
    }
    return left + right;
}

quint64 saturatingMultiply(quint64 left, quint64 right) {
    if (left == 0 || right == 0) return 0;
    if (left > std::numeric_limits<quint64>::max() / right) {
        return std::numeric_limits<quint64>::max();
    }
    return left * right;
}

void addAccumulator(ProfileAccumulator& target,
                    const ProfileAccumulator& source,
                    quint64 multiplier = 1) {
    const qint64 signedMultiplier =
        multiplier > quint64(std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max()
            : qint64(multiplier);
    auto addSigned = [signedMultiplier](qint64& value, qint64 add) {
        const long double result =
            static_cast<long double>(value) +
            static_cast<long double>(add) * static_cast<long double>(signedMultiplier);
        value = result >= static_cast<long double>(std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max()
            : qint64(result);
    };
    addSigned(target.counters, source.counters);
    addSigned(target.dynamicSignals, source.dynamicSignals);
    addSigned(target.transitions, source.transitions);
    addSigned(target.activeSignalTicks, source.activeSignalTicks);
    addSigned(target.unknownSignalTicks, source.unknownSignalTicks);
    addSigned(target.issueTicks, source.issueTicks);
    addSigned(target.issueActiveTicks, source.issueActiveTicks);
    addSigned(target.dualIssueTicks, source.dualIssueTicks);
    addSigned(target.idleTicks, source.idleTicks);
    addSigned(target.issueCapacityTicks, source.issueCapacityTicks);
    addSigned(target.issueExpectedTicks, source.issueExpectedTicks);
    addSigned(target.issueContexts, source.issueContexts);
    if (target.issueSlotTicks.size() < source.issueSlotTicks.size()) {
        target.issueSlotTicks.resize(source.issueSlotTicks.size());
    }
    if (target.issueSlotCapacityTicks.size() <
        source.issueSlotCapacityTicks.size()) {
        target.issueSlotCapacityTicks.resize(
            source.issueSlotCapacityTicks.size());
    }
    if (target.issueSlotExpectedTicks.size() <
        source.issueSlotExpectedTicks.size()) {
        target.issueSlotExpectedTicks.resize(
            source.issueSlotExpectedTicks.size());
    }
    for (int i = 0; i < source.issueSlotTicks.size(); ++i) {
        addSigned(target.issueSlotTicks[i], source.issueSlotTicks.at(i));
    }
    for (int i = 0; i < source.issueSlotCapacityTicks.size(); ++i) {
        addSigned(target.issueSlotCapacityTicks[i],
                  source.issueSlotCapacityTicks.at(i));
    }
    for (int i = 0; i < source.issueSlotExpectedTicks.size(); ++i) {
        addSigned(target.issueSlotExpectedTicks[i],
                  source.issueSlotExpectedTicks.at(i));
    }
    addSigned(target.stallTicks, source.stallTicks);
    addSigned(target.pendingTicks, source.pendingTicks);
    addSigned(target.busyTicks, source.busyTicks);
    addSigned(target.validTicks, source.validTicks);
    addSigned(target.fifoNonemptyTicks, source.fifoNonemptyTicks);
    target.fifoReadEvents =
        saturatingAdd(target.fifoReadEvents,
                      saturatingMultiply(source.fifoReadEvents, multiplier));
    target.fifoWriteEvents =
        saturatingAdd(target.fifoWriteEvents,
                      saturatingMultiply(source.fifoWriteEvents, multiplier));
    addSigned(target.fifoReadSources, source.fifoReadSources);
    addSigned(target.fifoWriteSources, source.fifoWriteSources);
    addSigned(target.fifoReadUnknownTicks,
              source.fifoReadUnknownTicks);
    addSigned(target.fifoWriteUnknownTicks,
              source.fifoWriteUnknownTicks);
    target.fifoReadResources.unite(source.fifoReadResources);
    target.fifoWriteResources.unite(source.fifoWriteResources);
    target.cacheMissEvents =
        saturatingAdd(target.cacheMissEvents,
                      saturatingMultiply(source.cacheMissEvents, multiplier));
    target.cacheHitEvents =
        saturatingAdd(target.cacheHitEvents,
                      saturatingMultiply(source.cacheHitEvents, multiplier));
    addSigned(target.cacheMissSources, source.cacheMissSources);
    addSigned(target.cacheHitSources, source.cacheHitSources);
    addSigned(target.cacheMissUnknownTicks,
              source.cacheMissUnknownTicks);
    addSigned(target.cacheHitUnknownTicks,
              source.cacheHitUnknownTicks);
    addSigned(target.cacheMissDiscontinuities,
              source.cacheMissDiscontinuities);
    addSigned(target.cacheHitDiscontinuities,
              source.cacheHitDiscontinuities);
    target.cacheMissResources.unite(source.cacheMissResources);
    target.cacheHitResources.unite(source.cacheHitResources);
    addSigned(target.fifoFullTicks, source.fifoFullTicks);
    addSigned(target.fifoFullKnownTicks, source.fifoFullKnownTicks);
    addSigned(target.fifoFullResources, source.fifoFullResources);
    addSigned(target.queueFullTicks, source.queueFullTicks);
    addSigned(target.queueFullKnownTicks, source.queueFullKnownTicks);
    addSigned(target.queueFullResources, source.queueFullResources);
    target.creditValueTicks +=
        source.creditValueTicks * static_cast<long double>(multiplier);
    addSigned(target.creditKnownTicks, source.creditKnownTicks);
    for (auto it = source.keyCounts.constBegin(); it != source.keyCounts.constEnd(); ++it) {
        addSigned(target.keyCounts[it.key()], it.value());
    }
    for (auto it = source.keyActiveTicks.constBegin();
         it != source.keyActiveTicks.constEnd(); ++it) {
        addSigned(target.keyActiveTicks[it.key()], it.value());
    }
    for (auto it = source.keyWeightedValueTicks.constBegin();
         it != source.keyWeightedValueTicks.constEnd(); ++it) {
        target.keyWeightedValueTicks[it.key()] +=
            it.value() * static_cast<long double>(multiplier);
    }
    for (auto it = source.keyKnownTicks.constBegin();
         it != source.keyKnownTicks.constEnd(); ++it) {
        addSigned(target.keyKnownTicks[it.key()], it.value());
    }
    for (auto it = source.euChannels.constBegin();
         it != source.euChannels.constEnd(); ++it) {
        EuChannelAccumulator& output = target.euChannels[it.key()];
        addSigned(output.nonemptyTicks, it.value().nonemptyTicks);
        addSigned(output.occupancyKnownTicks, it.value().occupancyKnownTicks);
        addSigned(output.occupancyExpectedTicks,
                  it.value().occupancyExpectedTicks);
        addSigned(output.fullTicks, it.value().fullTicks);
        addSigned(output.fullKnownTicks, it.value().fullKnownTicks);
        addSigned(output.resources, it.value().resources);
        output.readEvents =
            saturatingAdd(output.readEvents,
                          saturatingMultiply(it.value().readEvents, multiplier));
        output.writeEvents =
            saturatingAdd(output.writeEvents,
                          saturatingMultiply(it.value().writeEvents, multiplier));
        addSigned(output.readSources, it.value().readSources);
        addSigned(output.writeSources, it.value().writeSources);
        addSigned(output.readUnknownTicks,
                  it.value().readUnknownTicks);
        addSigned(output.writeUnknownTicks,
                  it.value().writeUnknownTicks);
        output.readResources.unite(it.value().readResources);
        output.writeResources.unite(it.value().writeResources);
    }
    addSigned(target.euPhase1PendingTicks, source.euPhase1PendingTicks);
    addSigned(target.euPhase1KnownTicks, source.euPhase1KnownTicks);
    addSigned(target.euPhase1ExpectedTicks,
              source.euPhase1ExpectedTicks);
}

QString euChannelKey(const QString& path) {
    if (!path.contains(QStringLiteral(".m_QPPUEU."))) return QString();
    if (path.contains(QStringLiteral(".pt_BE_ThdCore_new_inst."))) {
        return QStringLiteral("instruction_input");
    }
    if (path.contains(QStringLiteral(".pt_CBCtrl_ThdCore_uop."))) {
        return QStringLiteral("cb_read_request");
    }
    if (path.contains(QStringLiteral(".ptEU2CBData"))) {
        return QStringLiteral("cb_read_data_output");
    }
    if (path.contains(QStringLiteral(".ptCBData2EUDstP."))) {
        return QStringLiteral("cb_dstp_writeback_input");
    }
    if (path.contains(QStringLiteral(".ptCBData2EU"))) {
        return QStringLiteral("cb_writeback_input");
    }
    return QString();
}

QString counterResourcePath(const QString& path) {
    const int separator = path.lastIndexOf(QLatin1Char('.'));
    return separator >= 0 ? path.left(separator) : path;
}

void addCounter(ProfileAccumulator& target, const CounterView& counter) {
    ++target.counters;
    if (counter.transitions > 0) ++target.dynamicSignals;
    target.transitions += counter.transitions;
    target.activeSignalTicks += counter.activeTicks;
    target.unknownSignalTicks += counter.unknownTicks;
    ++target.keyCounts[counter.key];
    target.keyActiveTicks[counter.key] += counter.activeTicks;
    if (counter.knownTicks > 0 &&
        counter.key != QStringLiteral("fifo_occupancy") &&
        counter.key != QStringLiteral("fifo_capacity") &&
        counter.key != QStringLiteral("queue_occupancy") &&
        counter.key != QStringLiteral("queue_capacity")) {
        target.keyWeightedValueTicks[counter.key] += counter.weightedValueTicks;
        target.keyKnownTicks[counter.key] += counter.knownTicks;
    }

    if (counter.key.contains(QStringLiteral("stall"))) {
        target.stallTicks += counter.activeTicks;
    }
    if (counter.key.contains(QStringLiteral("pending")) ||
        counter.category == QStringLiteral("pending")) {
        target.pendingTicks += counter.activeTicks;
    }
    if (counter.key.contains(QStringLiteral("busy"))) {
        target.busyTicks += counter.activeTicks;
    }
    if (counter.key == QStringLiteral("interface_valid") ||
        counter.key == QStringLiteral("cache_request") ||
        counter.key == QStringLiteral("cache_return")) {
        target.validTicks += counter.activeTicks;
    }
    if (counter.key == QStringLiteral("fifo_occupancy")) {
        target.fifoNonemptyTicks += counter.activeTicks;
        if (counter.fifoFullKnownTicks > 0) {
            target.fifoFullTicks += counter.fifoFullTicks;
            target.fifoFullKnownTicks += counter.fifoFullKnownTicks;
            ++target.fifoFullResources;
        }
    } else if (counter.key == QStringLiteral("queue_occupancy")) {
        if (counter.queueFullKnownTicks > 0) {
            target.queueFullTicks += counter.queueFullTicks;
            target.queueFullKnownTicks += counter.queueFullKnownTicks;
            ++target.queueFullResources;
        }
    } else if (counter.key == QStringLiteral("fifo_reads")) {
        target.fifoReadEvents =
            saturatingAdd(target.fifoReadEvents, counter.eventCount);
        ++target.fifoReadSources;
        target.fifoReadUnknownTicks += counter.unknownTicks;
        target.fifoReadResources.insert(counterResourcePath(counter.path));
    } else if (counter.key == QStringLiteral("fifo_writes")) {
        target.fifoWriteEvents =
            saturatingAdd(target.fifoWriteEvents, counter.eventCount);
        ++target.fifoWriteSources;
        target.fifoWriteUnknownTicks += counter.unknownTicks;
        target.fifoWriteResources.insert(counterResourcePath(counter.path));
    }
    if (counter.category == QStringLiteral("credit")) {
        target.creditValueTicks += counter.weightedValueTicks;
        target.creditKnownTicks += counter.knownTicks;
    }
    if (counter.key.contains(QStringLiteral("cache_miss"))) {
        target.cacheMissEvents =
            saturatingAdd(target.cacheMissEvents, counter.eventCount);
        ++target.cacheMissSources;
        target.cacheMissUnknownTicks += counter.unknownTicks;
        target.cacheMissDiscontinuities +=
            counter.eventDiscontinuities;
        target.cacheMissResources.insert(counterResourcePath(counter.path));
    }
    if (counter.key.contains(QStringLiteral("cache_hit"))) {
        target.cacheHitEvents =
            saturatingAdd(target.cacheHitEvents, counter.eventCount);
        ++target.cacheHitSources;
        target.cacheHitUnknownTicks += counter.unknownTicks;
        target.cacheHitDiscontinuities +=
            counter.eventDiscontinuities;
        target.cacheHitResources.insert(counterResourcePath(counter.path));
    }

    const QString euChannel = euChannelKey(counter.path);
    if (!euChannel.isEmpty()) {
        EuChannelAccumulator& channel = target.euChannels[euChannel];
        if (counter.key == QStringLiteral("fifo_occupancy")) {
            channel.nonemptyTicks += counter.activeTicks;
            channel.occupancyKnownTicks += counter.knownTicks;
            channel.occupancyExpectedTicks +=
                counter.knownTicks + counter.unknownTicks;
            if (counter.fifoFullKnownTicks > 0) {
                channel.fullTicks += counter.fifoFullTicks;
                channel.fullKnownTicks += counter.fifoFullKnownTicks;
                ++channel.resources;
            }
        } else if (counter.key == QStringLiteral("fifo_reads")) {
            channel.readEvents =
                saturatingAdd(channel.readEvents, counter.eventCount);
            ++channel.readSources;
            channel.readUnknownTicks += counter.unknownTicks;
            channel.readResources.insert(counterResourcePath(counter.path));
        } else if (counter.key == QStringLiteral("fifo_writes")) {
            channel.writeEvents =
                saturatingAdd(channel.writeEvents, counter.eventCount);
            ++channel.writeSources;
            channel.writeUnknownTicks += counter.unknownTicks;
            channel.writeResources.insert(counterResourcePath(counter.path));
        }
    }
    if (counter.path.endsWith(QStringLiteral(".m_QPPUEU.phase1_req_.valid"))) {
        target.euPhase1PendingTicks += counter.activeTicks;
        target.euPhase1KnownTicks += counter.knownTicks;
        target.euPhase1ExpectedTicks +=
            counter.knownTicks + counter.unknownTicks;
    }
}

QString removeSizeAnnotation(const QString& segment, int* declaredSize) {
    static const QRegularExpression expression(
        QStringLiteral("^(.*)\\[size=(\\d+)\\]$"));
    const QRegularExpressionMatch match = expression.match(segment);
    if (!match.hasMatch()) {
        if (declaredSize) *declaredSize = 0;
        return segment;
    }
    if (declaredSize) *declaredSize = match.captured(2).toInt();
    return match.captured(1);
}

QStringList performanceFocus(const QString& classKey) {
    if (classKey == QStringLiteral("gpu")) {
        return {QStringLiteral("计算簇负载均衡"),
                QStringLiteral("任务派发与片上互联流量")};
    }
    if (classKey == QStringLiteral("cluster")) {
        return {QStringLiteral("DPPU 负载均衡"),
                QStringLiteral("跨模块 FIFO 压力"),
                QStringLiteral("内存返回流量")};
    }
    if (classKey == QStringLiteral("dppu")) {
        return {QStringLiteral("PPU 负载均衡"),
                QStringLiteral("DPPU MMA 流量"),
                QStringLiteral("调度器回压")};
    }
    if (classKey == QStringLiteral("ppu")) {
        return {QStringLiteral("QPPU 发射吞吐"),
                QStringLiteral("PPUS 与 CB 压力"),
                QStringLiteral("缓存与 TAC 流量")};
    }
    if (classKey == QStringLiteral("qppu")) {
        return {QStringLiteral("前端到执行单元的数据流"),
                QStringLiteral("QPPU 内部 FIFO 压力")};
    }
    if (classKey == QStringLiteral("qppu_ctrl")) {
        return {QStringLiteral("发射与双发射效率"),
                QStringLiteral("SG 指令 Queue 满率"),
                QStringLiteral("停顿与依赖等待"),
                QStringLiteral("操作数和功能单元资源检查"),
                QStringLiteral("L0 指令缓存压力")};
    }
    if (classKey == QStringLiteral("shader_core")) {
        return {QStringLiteral("Shader Group 占用"),
                QStringLiteral("上下文活跃度")};
    }
    if (classKey == QStringLiteral("qppu_eu")) {
        return {QStringLiteral("线程指令接收与执行吞吐"),
                QStringLiteral("指令入口积压与满率"),
                QStringLiteral("CB 双相读数据与回写流量")};
    }
    if (classKey == QStringLiteral("qppu_grp_core")) {
        return {QStringLiteral("组指令发射"),
                QStringLiteral("组寄存器流量"),
                QStringLiteral("组缓存压力")};
    }
    if (classKey == QStringLiteral("qppu_mma")) {
        return {QStringLiteral("MMA 请求 FIFO"),
                QStringLiteral("矩阵缓冲区占用"),
                QStringLiteral("LDMB 与 STMB credit")};
    }
    if (classKey == QStringLiteral("ppus_ctrl")) {
        return {QStringLiteral("共享 Group 调度"),
                QStringLiteral("屏障流量"),
                QStringLiteral("Group 生命周期停顿")};
    }
    if (classKey == QStringLiteral("ppus_data")) {
        return {QStringLiteral("CB 读写带宽"),
                QStringLiteral("pending-clear 返回"),
                QStringLiteral("L1 与组缓存返回")};
    }
    if (classKey == QStringLiteral("cb_ctrl")) {
        return {QStringLiteral("CB 请求队列压力"),
                QStringLiteral("CB 与 L1 credit"),
                QStringLiteral("抢占读取流量"),
                QStringLiteral("读写向量回压")};
    }
    if (classKey == QStringLiteral("csbg")) {
        return {QStringLiteral("合并与批处理流量"),
                QStringLiteral("GAC 与 CBData 回压")};
    }
    if (classKey == QStringLiteral("ppu_tac")) {
        return {QStringLiteral("异步拷贝请求"),
                QStringLiteral("TAC 忙状态与返回流量")};
    }
    return {};
}

QString performanceNodePath(const QString& signalPath, const QString& classPath) {
    QString suffix = signalPath;
    if (suffix.startsWith(classPath + QLatin1Char('.'))) {
        suffix = suffix.mid(classPath.size() + 1);
    }
    const QStringList segments =
        suffix.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (segments.size() <= 1) return signalPath;

    int objectEnd = segments.size() - 2;
    for (int i = 0; i < segments.size(); ++i) {
        const QString base = removeSizeAnnotation(segments.at(i), nullptr);
        if (base == QStringLiteral("m_buf") ||
            base == QStringLiteral("m_FIFO") ||
            base == QStringLiteral("m_QData")) {
            objectEnd = qMax(0, i - 1);
            break;
        }
    }
    const QString objectSuffix =
        segments.mid(0, objectEnd + 1).join(QLatin1Char('.'));
    return classPath.isEmpty()
        ? objectSuffix
        : classPath + QLatin1Char('.') + objectSuffix;
}

int leadingArrayIndex(const QString& segment) {
    static const QRegularExpression expression(QStringLiteral("^\\[(\\d+)\\]"));
    const QRegularExpressionMatch match = expression.match(segment);
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

bool classForSegment(const QString& segment,
                     QString& classKey,
                     QString& className,
                     QString& provenance) {
    if (segment == QStringLiteral("gpu")) {
        classKey = QStringLiteral("gpu");
        className = QStringLiteral("GPU");
        provenance = QStringLiteral("waveform-derived");
    } else if (segment == QStringLiteral("m_clusters")) {
        classKey = QStringLiteral("cluster");
        className = QStringLiteral("Cluster");
        provenance = QStringLiteral("waveform-derived");
    } else if (segment == QStringLiteral("m_dppu")) {
        classKey = QStringLiteral("dppu");
        className = QStringLiteral("DPPU");
        provenance = QStringLiteral("waveform-derived");
    } else if (segment == QStringLiteral("m_ppu")) {
        classKey = QStringLiteral("ppu");
        className = QStringLiteral("PPU");
        provenance = QStringLiteral("architecture-doc");
    } else if (segment == QStringLiteral("m_QPPUTOP")) {
        classKey = QStringLiteral("qppu");
        className = QStringLiteral("QPPUTOP");
        provenance = QStringLiteral("qppu-source");
    } else if (segment == QStringLiteral("m_QPPUCtrl")) {
        classKey = QStringLiteral("qppu_ctrl");
        className = QStringLiteral("QPPUCtrl");
        provenance = QStringLiteral("qppu-source");
    } else if (segment == QStringLiteral("m_QPPUEU")) {
        classKey = QStringLiteral("qppu_eu");
        className = QStringLiteral("QPPU_EU");
        provenance = QStringLiteral("qppu-source");
    } else if (segment == QStringLiteral("m_EU")) {
        classKey = QStringLiteral("shader_core");
        className = QStringLiteral("ShaderCore");
        provenance = QStringLiteral("qppu-source");
    } else if (segment == QStringLiteral("m_QPPUGrpCore")) {
        classKey = QStringLiteral("qppu_grp_core");
        className = QStringLiteral("QPPU_GrpCore");
        provenance = QStringLiteral("qppu-source");
    } else if (segment == QStringLiteral("m_QPPUMMA")) {
        classKey = QStringLiteral("qppu_mma");
        className = QStringLiteral("QPPU_MMA");
        provenance = QStringLiteral("qppu-source");
    } else if (segment == QStringLiteral("m_PPUSControl")) {
        classKey = QStringLiteral("ppus_ctrl");
        className = QStringLiteral("PPUS_CTRL");
        provenance = QStringLiteral("architecture-doc");
    } else if (segment == QStringLiteral("m_PPUSData")) {
        classKey = QStringLiteral("ppus_data");
        className = QStringLiteral("PPUS_Data");
        provenance = QStringLiteral("architecture-doc");
    } else if (segment == QStringLiteral("m_CBCtrl")) {
        classKey = QStringLiteral("cb_ctrl");
        className = QStringLiteral("CB_CTRL");
        provenance = QStringLiteral("architecture-doc+waveform");
    } else if (segment == QStringLiteral("m_CSBG")) {
        classKey = QStringLiteral("csbg");
        className = QStringLiteral("CSBG");
        provenance = QStringLiteral("waveform-derived");
    } else if (segment == QStringLiteral("m_PPUTAC")) {
        classKey = QStringLiteral("ppu_tac");
        className = QStringLiteral("PPU_TAC");
        provenance = QStringLiteral("waveform-derived");
    } else {
        return false;
    }
    return true;
}

QVector<ScopeDescriptor> scopesForPath(const QString& path) {
    const QStringList segments = path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    QVector<ScopeDescriptor> scopes;
    for (int i = 0; i < segments.size(); ++i) {
        int declaredSize = 0;
        const QString baseSegment = removeSizeAnnotation(segments.at(i), &declaredSize);
        QString classKey;
        QString className;
        QString provenance;
        if (!classForSegment(baseSegment, classKey, className, provenance)) continue;

        int arrayIndex = -1;
        int endIndex = i;
        if (i + 1 < segments.size()) {
            arrayIndex = leadingArrayIndex(segments.at(i + 1));
            if (arrayIndex >= 0) endIndex = i + 1;
        }

        ScopeDescriptor scope;
        scope.classKey = classKey;
        scope.className = className;
        scope.provenance = provenance;
        scope.path = segments.mid(0, endIndex + 1).join(QLatin1Char('.'));
        scope.familyKey =
            segments.mid(0, i).join(QLatin1Char('.')) +
            (i == 0 ? QString() : QStringLiteral(".")) + baseSegment;
        scope.arrayIndex = arrayIndex;
        scope.declaredSize = declaredSize;
        scopes.push_back(scope);
    }
    return scopes;
}

QJsonObject euChannelJson(const EuChannelAccumulator& channel,
                          qint64 ticksPerCycle) {
    const double divisor = double(ticksPerCycle);
    const double observedCycles =
        double(channel.occupancyKnownTicks) / divisor;
    const bool occupancyCoverageComplete =
        channel.occupancyExpectedTicks > 0 &&
        channel.occupancyKnownTicks ==
            channel.occupancyExpectedTicks;
    QJsonObject object;
    object.insert(QStringLiteral("resources"), double(channel.resources));
    object.insert(QStringLiteral("observed_cycles"), observedCycles);
    object.insert(QStringLiteral("nonempty_cycles"),
                  double(channel.nonemptyTicks) / divisor);
    object.insert(QStringLiteral("occupancy_coverage_complete"),
                  occupancyCoverageComplete);
    if (occupancyCoverageComplete) {
        object.insert(
            QStringLiteral("nonempty_rate_percent"),
            100.0 * double(channel.nonemptyTicks) /
                double(channel.occupancyKnownTicks));
    }
    object.insert(QStringLiteral("full_cycles"),
                  double(channel.fullTicks) / divisor);
    if (occupancyCoverageComplete &&
        channel.fullKnownTicks == channel.occupancyExpectedTicks) {
        object.insert(
            QStringLiteral("full_rate_percent"),
            100.0 * double(channel.fullTicks) /
                double(channel.fullKnownTicks));
    }
    object.insert(QStringLiteral("read_events"),
                  QString::number(channel.readEvents));
    object.insert(QStringLiteral("write_events"),
                  QString::number(channel.writeEvents));
    object.insert(QStringLiteral("read_sources"),
                  double(channel.readSources));
    object.insert(QStringLiteral("write_sources"),
                  double(channel.writeSources));
    object.insert(QStringLiteral("read_unknown_cycles"),
                  double(channel.readUnknownTicks) / divisor);
    object.insert(QStringLiteral("write_unknown_cycles"),
                  double(channel.writeUnknownTicks) / divisor);
    const bool eventCoverageComplete =
        channel.readSources > 0 && channel.writeSources > 0 &&
        channel.readSources == channel.writeSources &&
        !channel.readResources.isEmpty() &&
        channel.readResources == channel.writeResources &&
        channel.readUnknownTicks == 0 &&
        channel.writeUnknownTicks == 0;
    object.insert(QStringLiteral("paired_event_resources"),
                  channel.readResources == channel.writeResources
                      ? channel.readResources.size()
                      : 0);
    object.insert(QStringLiteral("event_coverage_complete"),
                  eventCoverageComplete);
    if (eventCoverageComplete) {
        object.insert(QStringLiteral("backlog_delta"),
                      double(channel.writeEvents) -
                          double(channel.readEvents));
    }
    if (eventCoverageComplete && occupancyCoverageComplete) {
        object.insert(
            QStringLiteral("service_rate_per_cycle"),
            double(channel.readEvents) / observedCycles);
    }
    return object;
}

QJsonObject accumulatorJson(const ProfileAccumulator& value,
                            qint64 ticksPerCycle) {
    const double divisor = double(ticksPerCycle);
    QJsonObject object;
    object.insert(QStringLiteral("counters"), double(value.counters));
    object.insert(QStringLiteral("dynamic_signals"), double(value.dynamicSignals));
    object.insert(QStringLiteral("transitions"), double(value.transitions));
    object.insert(QStringLiteral("active_signal_cycles"),
                  double(value.activeSignalTicks) / divisor);
    object.insert(QStringLiteral("unknown_signal_cycles"),
                  double(value.unknownSignalTicks) / divisor);
    object.insert(QStringLiteral("issue_slot_cycles"),
                  double(value.issueTicks) / divisor);
    object.insert(QStringLiteral("issue_active_cycles"),
                  double(value.issueActiveTicks) / divisor);
    object.insert(QStringLiteral("dual_issue_cycles"),
                  double(value.dualIssueTicks) / divisor);
    object.insert(QStringLiteral("idle_cycles"),
                  double(value.idleTicks) / divisor);
    const qint64 issueObservedTicks =
        value.issueActiveTicks + value.idleTicks;
    const bool issueCoverageComplete =
        value.issueExpectedTicks > 0 &&
        issueObservedTicks == value.issueExpectedTicks;
    object.insert(QStringLiteral("issue_observed_cycles"),
                  double(issueObservedTicks) / divisor);
    object.insert(QStringLiteral("issue_expected_cycles"),
                  double(value.issueExpectedTicks) / divisor);
    object.insert(QStringLiteral("issue_unknown_cycles"),
                  double(qMax<qint64>(
                      0, value.issueExpectedTicks - issueObservedTicks)) /
                      divisor);
    object.insert(QStringLiteral("issue_coverage_complete"),
                  issueCoverageComplete);
    object.insert(
        QStringLiteral("issue_utilization_percent"),
        issueObservedTicks > 0
            ? 100.0 * double(value.issueTicks) /
                  double(issueObservedTicks)
            : 0.0);
    object.insert(QStringLiteral("issue_capacity_cycles"),
                  double(value.issueCapacityTicks) / divisor);
    object.insert(QStringLiteral("issue_contexts"),
                  double(value.issueContexts));
    QJsonArray issueSlots;
    qint64 issueSlotActiveTicks = 0;
    const int issueSlotCount =
        qMax(qMax(value.issueSlotTicks.size(),
                  value.issueSlotCapacityTicks.size()),
             value.issueSlotExpectedTicks.size());
    bool issueSlotCoverageComplete = issueSlotCount > 0;
    for (int i = 0; i < issueSlotCount; ++i) {
        const qint64 activeTicks =
            i < value.issueSlotTicks.size() ? value.issueSlotTicks.at(i) : 0;
        issueSlotActiveTicks += activeTicks;
        const qint64 capacityTicks =
            i < value.issueSlotCapacityTicks.size()
                ? value.issueSlotCapacityTicks.at(i)
                : 0;
        const qint64 expectedTicks =
            i < value.issueSlotExpectedTicks.size()
                ? value.issueSlotExpectedTicks.at(i)
                : 0;
        const bool slotCovered =
            expectedTicks > 0 && capacityTicks == expectedTicks;
        issueSlotCoverageComplete =
            issueSlotCoverageComplete && slotCovered;
        QJsonObject slot;
        slot.insert(QStringLiteral("index"), i);
        slot.insert(QStringLiteral("issued_cycles"),
                    double(activeTicks) / divisor);
        slot.insert(
            QStringLiteral("utilization_percent"),
            capacityTicks > 0
                ? 100.0 * double(activeTicks) / double(capacityTicks)
                : 0.0);
        slot.insert(QStringLiteral("observed_cycles"),
                    double(capacityTicks) / divisor);
        slot.insert(QStringLiteral("expected_cycles"),
                    double(expectedTicks) / divisor);
        slot.insert(QStringLiteral("unknown_cycles"),
                    double(qMax<qint64>(0, expectedTicks - capacityTicks)) /
                        divisor);
        slot.insert(QStringLiteral("covered"), slotCovered);
        issueSlots.push_back(slot);
    }
    object.insert(QStringLiteral("issue_slots"), issueSlots);
    object.insert(QStringLiteral("issue_slot_coverage_complete"),
                  issueSlotCoverageComplete);
    object.insert(QStringLiteral("issue_slot_utilization_percent"),
                  value.issueCapacityTicks > 0
                      ? 100.0 * double(issueSlotActiveTicks) /
                            double(value.issueCapacityTicks)
                      : 0.0);
    object.insert(QStringLiteral("stall_signal_cycles"),
                  double(value.stallTicks) / divisor);
    object.insert(QStringLiteral("pending_signal_cycles"),
                  double(value.pendingTicks) / divisor);
    object.insert(QStringLiteral("busy_signal_cycles"),
                  double(value.busyTicks) / divisor);
    object.insert(QStringLiteral("interface_valid_signal_cycles"),
                  double(value.validTicks) / divisor);
    object.insert(QStringLiteral("fifo_nonempty_signal_cycles"),
                  double(value.fifoNonemptyTicks) / divisor);
    object.insert(QStringLiteral("fifo_read_events"),
                  QString::number(value.fifoReadEvents));
    object.insert(QStringLiteral("fifo_write_events"),
                  QString::number(value.fifoWriteEvents));
    object.insert(QStringLiteral("fifo_read_sources"),
                  double(value.fifoReadSources));
    object.insert(QStringLiteral("fifo_write_sources"),
                  double(value.fifoWriteSources));
    object.insert(QStringLiteral("fifo_read_unknown_cycles"),
                  double(value.fifoReadUnknownTicks) / divisor);
    object.insert(QStringLiteral("fifo_write_unknown_cycles"),
                  double(value.fifoWriteUnknownTicks) / divisor);
    object.insert(
        QStringLiteral("fifo_event_coverage_complete"),
        value.fifoReadSources > 0 && value.fifoWriteSources > 0 &&
            value.fifoReadSources == value.fifoWriteSources &&
            !value.fifoReadResources.isEmpty() &&
            value.fifoReadResources == value.fifoWriteResources &&
            value.fifoReadUnknownTicks == 0 &&
            value.fifoWriteUnknownTicks == 0);
    object.insert(
        QStringLiteral("fifo_paired_event_resources"),
        value.fifoReadResources == value.fifoWriteResources
            ? value.fifoReadResources.size()
            : 0);
    object.insert(QStringLiteral("cache_miss_events"),
                  QString::number(value.cacheMissEvents));
    object.insert(QStringLiteral("cache_hit_events"),
                  QString::number(value.cacheHitEvents));
    object.insert(QStringLiteral("cache_miss_sources"),
                  double(value.cacheMissSources));
    object.insert(QStringLiteral("cache_hit_sources"),
                  double(value.cacheHitSources));
    object.insert(QStringLiteral("cache_miss_unknown_cycles"),
                  double(value.cacheMissUnknownTicks) / divisor);
    object.insert(QStringLiteral("cache_hit_unknown_cycles"),
                  double(value.cacheHitUnknownTicks) / divisor);
    object.insert(QStringLiteral("cache_miss_discontinuities"),
                  double(value.cacheMissDiscontinuities));
    object.insert(QStringLiteral("cache_hit_discontinuities"),
                  double(value.cacheHitDiscontinuities));
    const bool cacheRateCoverageComplete =
        value.cacheHitSources > 0 &&
        value.cacheHitSources == value.cacheMissSources &&
        !value.cacheHitResources.isEmpty() &&
        value.cacheHitResources == value.cacheMissResources &&
        value.cacheHitUnknownTicks == 0 &&
        value.cacheMissUnknownTicks == 0 &&
        value.cacheHitDiscontinuities == 0 &&
        value.cacheMissDiscontinuities == 0;
    object.insert(QStringLiteral("cache_rate_coverage_complete"),
                  cacheRateCoverageComplete);
    object.insert(
        QStringLiteral("cache_paired_event_resources"),
        value.cacheHitResources == value.cacheMissResources
            ? value.cacheHitResources.size()
            : 0);
    const quint64 cacheAccessEvents =
        saturatingAdd(value.cacheHitEvents, value.cacheMissEvents);
    object.insert(QStringLiteral("cache_hit_rate_percent"),
                  cacheRateCoverageComplete && cacheAccessEvents > 0
                      ? 100.0 * double(value.cacheHitEvents) /
                            double(cacheAccessEvents)
                      : 0.0);
    object.insert(QStringLiteral("fifo_full_cycles"),
                  double(value.fifoFullTicks) / divisor);
    object.insert(QStringLiteral("fifo_full_observed_cycles"),
                  double(value.fifoFullKnownTicks) / divisor);
    object.insert(QStringLiteral("fifo_full_rate_percent"),
                  value.fifoFullKnownTicks > 0
                      ? 100.0 * double(value.fifoFullTicks) /
                            double(value.fifoFullKnownTicks)
                      : 0.0);
    object.insert(QStringLiteral("fifo_full_resources"),
                  double(value.fifoFullResources));
    object.insert(QStringLiteral("queue_full_cycles"),
                  double(value.queueFullTicks) / divisor);
    object.insert(QStringLiteral("queue_full_observed_cycles"),
                  double(value.queueFullKnownTicks) / divisor);
    object.insert(QStringLiteral("queue_full_rate_percent"),
                  value.queueFullKnownTicks > 0
                      ? 100.0 * double(value.queueFullTicks) /
                            double(value.queueFullKnownTicks)
                      : 0.0);
    object.insert(QStringLiteral("queue_full_resources"),
                  double(value.queueFullResources));
    object.insert(QStringLiteral("average_credit"),
                  value.creditKnownTicks > 0
                      ? double(value.creditValueTicks /
                               static_cast<long double>(value.creditKnownTicks))
                      : 0.0);
    QJsonObject keys;
    for (auto it = value.keyCounts.constBegin(); it != value.keyCounts.constEnd(); ++it) {
        keys.insert(it.key(), double(it.value()));
    }
    object.insert(QStringLiteral("counter_key_counts"), keys);
    QJsonObject keyActiveCycles;
    for (auto it = value.keyActiveTicks.constBegin();
         it != value.keyActiveTicks.constEnd(); ++it) {
        keyActiveCycles.insert(it.key(), double(it.value()) / divisor);
    }
    object.insert(QStringLiteral("active_cycles_by_counter_key"),
                  keyActiveCycles);
    QJsonObject keyKnownCycles;
    for (auto it = value.keyKnownTicks.constBegin();
         it != value.keyKnownTicks.constEnd(); ++it) {
        keyKnownCycles.insert(it.key(), double(it.value()) / divisor);
    }
    object.insert(QStringLiteral("known_cycles_by_counter_key"),
                  keyKnownCycles);
    QJsonObject keyAverages;
    for (auto it = value.keyWeightedValueTicks.constBegin();
         it != value.keyWeightedValueTicks.constEnd(); ++it) {
        if (it.key() == QStringLiteral("fifo_occupancy") ||
            it.key() == QStringLiteral("fifo_capacity") ||
            it.key() == QStringLiteral("queue_occupancy") ||
            it.key() == QStringLiteral("queue_capacity")) {
            continue;
        }
        const qint64 knownTicks = value.keyKnownTicks.value(it.key());
        if (knownTicks <= 0) continue;
        keyAverages.insert(
            it.key(),
            double(it.value() / static_cast<long double>(knownTicks)));
    }
    object.insert(QStringLiteral("average_by_counter_key"), keyAverages);
    if (!value.euChannels.isEmpty() || value.euPhase1KnownTicks > 0) {
        QJsonObject eu;
        for (auto it = value.euChannels.constBegin();
             it != value.euChannels.constEnd(); ++it) {
            eu.insert(it.key(), euChannelJson(it.value(), ticksPerCycle));
        }
        QJsonObject phase1;
        phase1.insert(QStringLiteral("pending_cycles"),
                      double(value.euPhase1PendingTicks) / divisor);
        const bool phase1CoverageComplete =
            value.euPhase1ExpectedTicks > 0 &&
            value.euPhase1KnownTicks ==
                value.euPhase1ExpectedTicks;
        phase1.insert(QStringLiteral("coverage_complete"),
                      phase1CoverageComplete);
        if (phase1CoverageComplete) {
            phase1.insert(
                QStringLiteral("pending_rate_percent"),
                100.0 * double(value.euPhase1PendingTicks) /
                    double(value.euPhase1KnownTicks));
        }
        eu.insert(QStringLiteral("phase1_request"), phase1);
        const EuChannelAccumulator instruction =
            value.euChannels.value(QStringLiteral("instruction_input"));
        const double instructionObservedCycles =
            double(instruction.occupancyKnownTicks) / divisor;
        QJsonObject execution;
        const bool instructionEventCoverageComplete =
            instruction.readSources > 0 &&
            instruction.readSources == instruction.writeSources &&
            !instruction.readResources.isEmpty() &&
            instruction.readResources ==
                instruction.writeResources &&
            instruction.readUnknownTicks == 0 &&
            instruction.writeUnknownTicks == 0;
        execution.insert(QStringLiteral("event_coverage_complete"),
                         instructionEventCoverageComplete);
        execution.insert(QStringLiteral("instructions_received"),
                         QString::number(instruction.writeEvents));
        execution.insert(QStringLiteral("instructions_executed"),
                         QString::number(instruction.readEvents));
        if (instructionEventCoverageComplete &&
            instruction.occupancyExpectedTicks > 0 &&
            instruction.occupancyKnownTicks ==
                instruction.occupancyExpectedTicks) {
            execution.insert(
                QStringLiteral("utilization_percent"),
                100.0 * double(instruction.readEvents) /
                    instructionObservedCycles);
        }
        if (instructionEventCoverageComplete) {
            execution.insert(QStringLiteral("input_backlog_delta"),
                             double(instruction.writeEvents) -
                                 double(instruction.readEvents));
        }
        eu.insert(QStringLiteral("execution"), execution);
        object.insert(QStringLiteral("eu"), eu);
    }
    return object;
}

ProfileAccumulator aggregateNode(int nodeIndex, QVector<ProfileNode>& nodes) {
    ProfileNode& node = nodes[nodeIndex];
    node.aggregate = node.own;
    QVector<int> children = node.children;
    std::sort(children.begin(), children.end(),
              [&nodes](int left, int right) {
                  return nodes.at(left).scope.path < nodes.at(right).scope.path;
              });
    node.children = children;
    for (int child : node.children) {
        const ProfileAccumulator childAggregate = aggregateNode(child, nodes);
        addAccumulator(node.aggregate, childAggregate);
    }
    return node.aggregate;
}

QJsonObject nodeJson(int nodeIndex,
                     const QVector<ProfileNode>& nodes,
                     qint64 ticksPerCycle) {
    const ProfileNode& node = nodes.at(nodeIndex);
    QJsonObject object;
    object.insert(QStringLiteral("class_key"), node.scope.classKey);
    object.insert(QStringLiteral("class"), node.scope.className);
    object.insert(QStringLiteral("path"), node.scope.path);
    object.insert(QStringLiteral("provenance"), node.scope.provenance);
    QJsonArray focus;
    for (const QString& item : performanceFocus(node.scope.classKey)) {
        focus.push_back(item);
    }
    object.insert(QStringLiteral("performance_focus"), focus);
    object.insert(QStringLiteral("represented_instances"),
                  QString::number(node.representedInstances));
    object.insert(QStringLiteral("representative_only"), node.representativeOnly);
    if (node.scope.arrayIndex >= 0) {
        QJsonObject array;
        array.insert(QStringLiteral("index"), node.scope.arrayIndex);
        array.insert(QStringLiteral("observed_elements"), node.observedArrayElements);
        array.insert(QStringLiteral("logical_size"), node.logicalArraySize);
        array.insert(QStringLiteral("representation_factor"),
                     QString::number(node.representationFactor));
        array.insert(QStringLiteral("representative_only"), node.representativeOnly);
        object.insert(QStringLiteral("array"), array);
    }
    object.insert(QStringLiteral("own"), accumulatorJson(node.own, ticksPerCycle));
    object.insert(QStringLiteral("aggregate"),
                  accumulatorJson(node.aggregate, ticksPerCycle));
    struct RankedNode {
        QString path;
        ProfileAccumulator metrics;
    };
    QVector<RankedNode> rankedNodes;
    rankedNodes.reserve(node.hotNodes.size());
    for (auto it = node.hotNodes.constBegin(); it != node.hotNodes.constEnd(); ++it) {
        rankedNodes.push_back({it.key(), it.value()});
    }
    std::sort(rankedNodes.begin(), rankedNodes.end(),
              [](const RankedNode& left, const RankedNode& right) {
                  if (left.metrics.activeSignalTicks != right.metrics.activeSignalTicks) {
                      return left.metrics.activeSignalTicks >
                             right.metrics.activeSignalTicks;
                  }
                  if (left.metrics.transitions != right.metrics.transitions) {
                      return left.metrics.transitions > right.metrics.transitions;
                  }
                  if (left.metrics.counters != right.metrics.counters) {
                      return left.metrics.counters > right.metrics.counters;
                  }
                  return left.path < right.path;
              });
    QJsonArray hotNodes;
    const int hotNodeLimit = qMin(32, rankedNodes.size());
    for (int i = 0; i < hotNodeLimit; ++i) {
        QJsonObject hotNode;
        hotNode.insert(QStringLiteral("path"), rankedNodes.at(i).path);
        QString name = rankedNodes.at(i).path.section(QLatin1Char('.'), -1);
        if (leadingArrayIndex(name) >= 0) {
            name = rankedNodes.at(i).path.section(QLatin1Char('.'), -2);
        }
        hotNode.insert(QStringLiteral("name"), name);
        hotNode.insert(QStringLiteral("metrics"),
                       accumulatorJson(rankedNodes.at(i).metrics, ticksPerCycle));
        hotNodes.push_back(hotNode);
    }
    object.insert(QStringLiteral("hot_nodes"), hotNodes);
    QJsonArray children;
    for (int child : node.children) {
        children.push_back(nodeJson(child, nodes, ticksPerCycle));
    }
    object.insert(QStringLiteral("children"), children);
    return object;
}

bool recognizedArchitecturePath(const QString& path) {
    static const QStringList tokens = {
        QStringLiteral(".m_clusters"), QStringLiteral(".m_dppu"),
        QStringLiteral(".m_ppu"), QStringLiteral(".m_QPPUTOP"),
        QStringLiteral(".m_QPPUCtrl"), QStringLiteral(".m_QPPUEU"),
        QStringLiteral(".m_EU"), QStringLiteral(".m_QPPUGrpCore"),
        QStringLiteral(".m_QPPUMMA"), QStringLiteral(".m_PPUSControl"),
        QStringLiteral(".m_PPUSData"), QStringLiteral(".m_CBCtrl"),
        QStringLiteral(".m_CSBG"), QStringLiteral(".m_PPUTAC")
    };
    if (path == QStringLiteral("gpu") || path.startsWith(QStringLiteral("gpu."))) {
        return true;
    }
    for (const QString& token : tokens) {
        if (path.contains(token) || path.startsWith(token.mid(1))) return true;
    }
    return false;
}

}  // namespace

QString canonicalArchitecturePath(const QString& path) {
    QString result;
    result.reserve(path.size() + 16);
    for (int i = 0; i < path.size();) {
        if (path.at(i) != QLatin1Char('[')) {
            result += path.at(i++);
            continue;
        }

        const int close = path.indexOf(QLatin1Char(']'), i + 1);
        if (close < 0) {
            result += path.mid(i);
            break;
        }
        bool numericIndex = close > i + 1;
        for (int j = i + 1; numericIndex && j < close; ++j) {
            numericIndex = path.at(j).isDigit();
        }
        if (numericIndex && !result.isEmpty() &&
            result.back() != QLatin1Char('.')) {
            result += QLatin1Char('.');
        }
        result += path.mid(i, close - i + 1);
        i = close + 1;
    }
    return result;
}

ClassifiedSignal classifyArchitectureSignal(const QString& path) {
    const QString canonicalPath = canonicalArchitecturePath(path);
    ClassifiedSignal result;
    if (!recognizedArchitecturePath(canonicalPath)) return result;

    if (isMemoryBandwidthSignal(canonicalPath)) {
        result.key = QStringLiteral("memory_bandwidth_helper");
        result.category = QStringLiteral("memory_bandwidth");
        result.helper = true;
        return result;
    }

    const QString lowerPath = canonicalPath.toLower();
    const QString leaf = canonicalPath.section(QLatin1Char('.'), -1);
    const QString lowerLeaf = leaf.toLower();
    const bool queueResource =
        lowerPath.contains(QStringLiteral("queue"));

    static const QRegularExpression cacheMissLeaf(
        QStringLiteral("(^|_)(cache_)?miss(es)?($|_)"));
    static const QRegularExpression cacheHitLeaf(
        QStringLiteral("(^|_)(cache_)?hit(s)?($|_)"));
    static const QRegularExpression cacheEventCounterLeaf(
        QStringLiteral(
            "(^|_)(cache_)?(hit(s)?|miss(es)?)_"
            "(count|cnt|counter)$"));
    static const QRegularExpression issueValid(
        QStringLiteral("\\.issue_inst_(?:\\[size=\\d+\\])?\\.\\[\\d+\\]\\.vld$"));
    static const QRegularExpression issueField(
        [] {
            QStringList fields = {
                QStringLiteral("preDecode\\.instType\\.mainType"),
                QStringLiteral("preDecode\\.instIssueType"),
                QStringLiteral("sgId"),
                QStringLiteral("local_sgid"),
                QStringLiteral("PC\\.pc_")
            };
            for (const InstructionFeatureSpec& spec :
                 instructionFeatureSpecs()) {
                fields.push_back(
                    QStringLiteral("preDecode\\.") +
                    QRegularExpression::escape(spec.fieldPath));
            }
            return QStringLiteral(
                       "\\.issue_inst_(?:\\[size=\\d+\\])?\\.\\[\\d+\\]"
                       "\\.(%1)$")
                .arg(fields.join(QLatin1Char('|')));
        }());
    if (issueValid.match(canonicalPath).hasMatch()) {
        result.key = QStringLiteral("issue_valid");
        result.category = QStringLiteral("issue");
        return result;
    }
    if (issueField.match(canonicalPath).hasMatch()) {
        result.key = QStringLiteral("issue_field");
        result.category = QStringLiteral("issue_field");
        result.helper = true;
        return result;
    }

    static const QRegularExpression threadMaskField(
        QStringLiteral("\\.m_EU\\.shader_group_context_.*\\."
                       "thread_(valid|active|execute)_(indicator|mask)"
                       "(?:\\[size=\\d+\\])?(?:\\.\\[\\d+\\])?$"));
    if (threadMaskField.match(canonicalPath).hasMatch() ||
        (lowerPath.contains(QStringLiteral(".m_qppueu.m_eustate.group_info.")) &&
         (lowerLeaf == QStringLiteral("sgid") ||
          lowerLeaf == QStringLiteral("local_sg_id")))) {
        result.key = QStringLiteral("sg_thread_helper");
        result.category = QStringLiteral("thread");
        result.helper = true;
        return result;
    }

    bool queueInstructionFeature = false;
    if (lowerPath.contains(QStringLiteral(".instr_queue_")) &&
        lowerPath.contains(QStringLiteral(".m_qdata")) &&
        lowerPath.contains(QStringLiteral(".predecode."))) {
        for (const InstructionFeatureSpec& feature :
             instructionFeatureSpecs()) {
            if (lowerLeaf == feature.fieldPath.toLower()) {
                queueInstructionFeature = true;
                break;
            }
        }
    }
    if (lowerPath.contains(QStringLiteral(".instr_queue_")) &&
        ((lowerPath.contains(QStringLiteral(".m_qdata")) &&
           (lowerLeaf == QStringLiteral("pc_") ||
            lowerLeaf == QStringLiteral("instissuetype") ||
            lowerLeaf == QStringLiteral("thread") ||
            lowerLeaf == QStringLiteral("exethdunit") ||
            queueInstructionFeature)) ||
         lowerLeaf == QStringLiteral("m_ri") ||
         lowerLeaf == QStringLiteral("m_read_index") ||
         lowerLeaf == QStringLiteral("m_readindex") ||
         lowerLeaf == QStringLiteral("m_head") ||
         lowerLeaf == QStringLiteral("m_front"))) {
        result.key = QStringLiteral("scheduler_queue_head_helper");
        result.category = QStringLiteral("scheduler");
        result.helper = true;
        return result;
    }
    if (lowerPath.endsWith(
            QStringLiteral(".m_qppueu.phase1_req_.valid"))) {
        result.key = QStringLiteral("interface_valid");
        result.category = QStringLiteral("interface");
        return result;
    }
    if (lowerPath.contains(QStringLiteral(".sg_table_")) &&
        lowerLeaf == QStringLiteral("valid")) {
        result.key = QStringLiteral("shader_group_state");
        result.category = QStringLiteral("frontend");
        return result;
    }

    if (lowerPath.contains(QStringLiteral("stall_cnt_vector_"))) {
        result.key = QStringLiteral("stall_count");
        result.category = QStringLiteral("stall");
    } else if (lowerPath.contains(QStringLiteral("check_dep_cnt_vector_"))) {
        result.key = QStringLiteral("dependence_count");
        result.category = QStringLiteral("dependency");
    } else if (lowerPath.contains(QStringLiteral("sleep_cnt_vector_"))) {
        result.key = QStringLiteral("sleep_count");
        result.category = QStringLiteral("control");
    } else if (lowerPath.contains(QStringLiteral("flow_ctrl_pend_wait_vector_"))) {
        result.key = QStringLiteral("flow_control_pending");
        result.category = QStringLiteral("pending");
    } else if (lowerPath.contains(QStringLiteral("barrier_pend_wait_vector_"))) {
        result.key = QStringLiteral("barrier_pending");
        result.category = QStringLiteral("pending");
    } else if (lowerPath.contains(QStringLiteral("set_max_temp_pend_wait_vector_"))) {
        result.key = QStringLiteral("set_max_temp_pending");
        result.category = QStringLiteral("pending");
    } else if (lowerPath.contains(QStringLiteral("function_unit_pend_wait_vector_"))) {
        result.key = QStringLiteral("function_unit_pending");
        result.category = QStringLiteral("pending");
    } else if (lowerPath.contains(QStringLiteral("inflight_mem_cnt_"))) {
        result.key = QStringLiteral("inflight_memory");
        result.category = QStringLiteral("memory");
    } else if (lowerPath.contains(QStringLiteral("mma_ldmb_credit_cnt_"))) {
        result.key = QStringLiteral("mma_ldmb_credit");
        result.category = QStringLiteral("credit");
    } else if (lowerPath.contains(QStringLiteral("mma_stmb_credit_cnt_"))) {
        result.key = QStringLiteral("mma_stmb_credit");
        result.category = QStringLiteral("credit");
    } else if (lowerPath.contains(QStringLiteral("fe_dicache_credit_"))) {
        result.key = QStringLiteral("icache_credit");
        result.category = QStringLiteral("credit");
    } else if (queueResource &&
               (lowerLeaf == QStringLiteral("m_count") ||
                lowerLeaf == QStringLiteral("m_numavail") ||
                lowerLeaf == QStringLiteral("m_num_readable"))) {
        result.key = QStringLiteral("queue_occupancy");
        result.category = QStringLiteral("queue");
    } else if (queueResource && lowerLeaf == QStringLiteral("m_size")) {
        result.key = QStringLiteral("queue_capacity");
        result.category = QStringLiteral("capacity");
    } else if (lowerLeaf == QStringLiteral("m_numavail") ||
        lowerLeaf == QStringLiteral("m_num_readable")) {
        result.key = QStringLiteral("fifo_occupancy");
        result.category = QStringLiteral("fifo");
    } else if (lowerLeaf == QStringLiteral("m_num_read")) {
        result.key = QStringLiteral("fifo_reads");
        result.category = QStringLiteral("fifo");
        result.eventSemantics = EventSemantics::PerCycleValue;
    } else if (lowerLeaf == QStringLiteral("m_num_written")) {
        result.key = QStringLiteral("fifo_writes");
        result.category = QStringLiteral("fifo");
        result.eventSemantics = EventSemantics::PerCycleValue;
    } else if (lowerLeaf == QStringLiteral("m_size")) {
        result.key = QStringLiteral("fifo_capacity");
        result.category = QStringLiteral("capacity");
    } else if (lowerPath.contains(QStringLiteral("stall")) &&
               (lowerLeaf.contains(QStringLiteral("stall")) ||
                lowerLeaf == QStringLiteral("valid") ||
                lowerLeaf == QStringLiteral("vld"))) {
        result.key = QStringLiteral("stall_state");
        result.category = QStringLiteral("stall");
    } else if (lowerPath.contains(QStringLiteral("pending")) &&
               (lowerLeaf.contains(QStringLiteral("pending")) ||
                lowerLeaf == QStringLiteral("valid") ||
                lowerLeaf == QStringLiteral("vld"))) {
        result.key = QStringLiteral("pending_state");
        result.category = QStringLiteral("pending");
    } else if (cacheMissLeaf.match(lowerLeaf).hasMatch()) {
        result.key = QStringLiteral("cache_miss");
        result.category = QStringLiteral("cache");
        result.eventSemantics =
            cacheEventCounterLeaf.match(lowerLeaf).hasMatch()
                ? EventSemantics::CumulativeCounter
                : EventSemantics::PerCycleMask;
    } else if (cacheHitLeaf.match(lowerLeaf).hasMatch()) {
        result.key = QStringLiteral("cache_hit");
        result.category = QStringLiteral("cache");
        result.eventSemantics =
            cacheEventCounterLeaf.match(lowerLeaf).hasMatch()
                ? EventSemantics::CumulativeCounter
                : EventSemantics::PerCycleMask;
    } else if (lowerLeaf.contains(QStringLiteral("busy"))) {
        result.key = QStringLiteral("busy");
        result.category = QStringLiteral("busy");
    } else if (lowerLeaf == QStringLiteral("valid") ||
               lowerLeaf == QStringLiteral("vld")) {
        if (lowerPath.contains(QStringLiteral("cache")) ||
            lowerPath.contains(QStringLiteral("icache"))) {
            result.key =
                lowerPath.contains(QStringLiteral("ret"))
                    ? QStringLiteral("cache_return")
                    : QStringLiteral("cache_request");
            result.category = QStringLiteral("cache");
        }
    }
    return result;
}

ArchitectureProfile buildArchitectureProfile(const QVector<CounterView>& counters,
                                             const QVector<IssueContextView>& issueContexts,
                                             qint64 durationTicks,
                                             qint64 ticksPerCycle) {
    ArchitectureProfile result;
    QVector<ProfileNode> nodes;
    QHash<QString, int> nodeByPath;
    QHash<QString, FamilyState> families;

    for (const CounterView& counter : counters) {
        const QVector<ScopeDescriptor> scopes = scopesForPath(counter.path);
        int parent = -1;
        for (const ScopeDescriptor& scope : scopes) {
            int nodeIndex = nodeByPath.value(scope.path, -1);
            if (nodeIndex < 0) {
                ProfileNode node;
                node.scope = scope;
                node.parent = parent;
                nodeIndex = nodes.size();
                nodes.push_back(node);
                nodeByPath.insert(scope.path, nodeIndex);
                if (parent >= 0) nodes[parent].children.push_back(nodeIndex);
            }
            parent = nodeIndex;
            if (scope.arrayIndex >= 0) {
                FamilyState& family = families[scope.familyKey];
                family.declaredSize = qMax(family.declaredSize, scope.declaredSize);
                family.indexes.insert(scope.arrayIndex);
            }
        }
        if (parent >= 0) {
            addCounter(nodes[parent].own, counter);
            addCounter(
                nodes[parent].hotNodes[
                    performanceNodePath(counter.path, nodes[parent].scope.path)],
                counter);
        }
    }

    for (const IssueContextView& context : issueContexts) {
        const QVector<ScopeDescriptor> scopes = scopesForPath(context.path);
        int target = -1;
        for (const ScopeDescriptor& scope : scopes) {
            const int candidate = nodeByPath.value(scope.path, -1);
            if (candidate >= 0) target = candidate;
        }
        if (target < 0) continue;
        ProfileAccumulator& own = nodes[target].own;
        own.issueTicks += context.issuedTicks;
        own.issueActiveTicks += context.issueActiveTicks;
        own.dualIssueTicks += context.dualIssueTicks;
        own.idleTicks += context.idleTicks;
        own.issueCapacityTicks += context.capacityTicks;
        own.issueExpectedTicks += durationTicks;
        ++own.issueContexts;
        if (own.issueSlotTicks.size() < context.slotActiveTicks.size()) {
            own.issueSlotTicks.resize(context.slotActiveTicks.size());
        }
        if (own.issueSlotCapacityTicks.size() <
            context.slotCapacityTicks.size()) {
            own.issueSlotCapacityTicks.resize(
                context.slotCapacityTicks.size());
        }
        const int contextSlotCount =
            qMax(context.slotActiveTicks.size(),
                 context.slotCapacityTicks.size());
        if (own.issueSlotExpectedTicks.size() < contextSlotCount) {
            own.issueSlotExpectedTicks.resize(contextSlotCount);
        }
        for (int i = 0; i < context.slotActiveTicks.size(); ++i) {
            own.issueSlotTicks[i] += context.slotActiveTicks.at(i);
        }
        for (int i = 0; i < context.slotCapacityTicks.size(); ++i) {
            own.issueSlotCapacityTicks[i] += context.slotCapacityTicks.at(i);
        }
        for (int i = 0; i < contextSlotCount; ++i) {
            own.issueSlotExpectedTicks[i] += durationTicks;
        }
    }

    for (ProfileNode& node : nodes) {
        if (node.scope.arrayIndex < 0) continue;
        const FamilyState family = families.value(node.scope.familyKey);
        node.observedArrayElements = qMax(1, family.indexes.size());
        int inferredSize = 0;
        for (int index : family.indexes) inferredSize = qMax(inferredSize, index + 1);
        node.logicalArraySize =
            qMax(node.observedArrayElements,
                 family.declaredSize > 0 ? family.declaredSize : inferredSize);
        QVector<int> indexes = family.indexes.values().toVector();
        std::sort(indexes.begin(), indexes.end());
        const int rank =
            int(std::lower_bound(indexes.constBegin(), indexes.constEnd(),
                                 node.scope.arrayIndex) -
                indexes.constBegin());
        const quint64 base =
            quint64(node.logicalArraySize) /
            quint64(node.observedArrayElements);
        const quint64 remainder =
            quint64(node.logicalArraySize) %
            quint64(node.observedArrayElements);
        node.representationFactor =
            qMax<quint64>(1, base + (quint64(rank) < remainder ? 1 : 0));
        node.representativeOnly =
            node.logicalArraySize > node.observedArrayElements;
        result.hasRepresentativeArrays =
            result.hasRepresentativeArrays || node.representativeOnly;
    }

    QVector<int> ordered;
    ordered.reserve(nodes.size());
    for (int i = 0; i < nodes.size(); ++i) ordered.push_back(i);
    std::sort(ordered.begin(), ordered.end(),
              [&nodes](int left, int right) {
                  const int leftDepth = nodes.at(left).scope.path.count(QLatin1Char('.'));
                  const int rightDepth = nodes.at(right).scope.path.count(QLatin1Char('.'));
                  if (leftDepth != rightDepth) return leftDepth < rightDepth;
                  return nodes.at(left).scope.path < nodes.at(right).scope.path;
              });
    for (int nodeIndex : ordered) {
        ProfileNode& node = nodes[nodeIndex];
        quint64 represented =
            node.parent >= 0 ? nodes.at(node.parent).representedInstances : 1;
        if (node.scope.arrayIndex >= 0 && node.observedArrayElements > 0) {
            represented =
                saturatingMultiply(represented, node.representationFactor);
        }
        node.representedInstances = represented;
        if (node.parent >= 0 && nodes.at(node.parent).representativeOnly) {
            node.representativeOnly = true;
        }
    }

    QVector<int> roots;
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes.at(i).parent < 0) roots.push_back(i);
    }
    std::sort(roots.begin(), roots.end(),
              [&nodes](int left, int right) {
                  return nodes.at(left).scope.path < nodes.at(right).scope.path;
              });
    for (int root : roots) aggregateNode(root, nodes);

    QMap<QString, ClassState> classes;
    for (const ProfileNode& node : nodes) {
        ClassState& state = classes[node.scope.classKey];
        state.className = node.scope.className;
        state.provenance = node.scope.provenance;
        ++state.observedInstances;
        state.representedInstances =
            saturatingAdd(state.representedInstances, node.representedInstances);
        addAccumulator(state.observed, node.own);
        addAccumulator(state.projected, node.own, node.representedInstances);
    }

    for (int root : roots) {
        result.roots.push_back(nodeJson(root, nodes, ticksPerCycle));
    }
    for (auto it = classes.constBegin(); it != classes.constEnd(); ++it) {
        QJsonObject object;
        object.insert(QStringLiteral("class_key"), it.key());
        object.insert(QStringLiteral("class"), it.value().className);
        object.insert(QStringLiteral("provenance"), it.value().provenance);
        QJsonArray focus;
        for (const QString& item : performanceFocus(it.key())) {
            focus.push_back(item);
        }
        object.insert(QStringLiteral("performance_focus"), focus);
        object.insert(QStringLiteral("observed_instances"),
                      it.value().observedInstances);
        object.insert(QStringLiteral("represented_instances"),
                      QString::number(it.value().representedInstances));
        object.insert(QStringLiteral("observed"),
                      accumulatorJson(it.value().observed, ticksPerCycle));
        object.insert(QStringLiteral("representative_projection"),
                      accumulatorJson(it.value().projected, ticksPerCycle));
        result.classSummaries.push_back(object);
        result.observedInstances += it.value().observedInstances;
        result.representedInstances =
            saturatingAdd(result.representedInstances,
                          it.value().representedInstances);
    }
    if (result.hasRepresentativeArrays) {
        result.warnings.push_back(
            QStringLiteral("检测到 ArrayFirstOnly 拍平层级：代表性投影会重复 [0] 的指标，"
                           "属于估算值，不是其他数组元素的真实观测"));
    }
    if (classes.contains(QStringLiteral("qppu")) &&
        !classes.contains(QStringLiteral("qppu_mma"))) {
        result.warnings.push_back(
            QStringLiteral("当前波形没有形成 QPPU_MMA 性能子树"));
    }
    return result;
}

bool architectureProfilerSelfTest(QString& error) {
    if (instIssueTypeName(5) != QStringLiteral("CB") ||
        instIssueClassKey(1) != QStringLiteral("thread") ||
        instIssueClassKey(2) != QStringLiteral("thread") ||
        instIssueClassKey(3) != QStringLiteral("group") ||
        instIssueClassKey(4) != QStringLiteral("group") ||
        instIssueClassKey(5) != QStringLiteral("cb") ||
        instIssueClassKey(6) != QStringLiteral("mma") ||
        instIssueClassKey(7) != QStringLiteral("mma") ||
        cbCtrlInstClientName(4) != QStringLiteral("LDST") ||
        cbDataClientName(12) != QStringLiteral("Barrier") ||
        cbDataInstQueueClientName(7) != QStringLiteral("Is1WgLdlstmb")) {
        error = QStringLiteral("persisted cmodel enum decoding regressed");
        return false;
    }
    const QString attachedPointerArray =
        QStringLiteral("gpu.m_clusters[0].m_dppu[0].m_ppu[0]."
                       "m_QPPUTOP[3].m_QPPUCtrl.issue_inst_[1].vld");
    const QString canonicalPointerArray =
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_QPPUTOP.[3].m_QPPUCtrl.issue_inst_.[1].vld");
    if (canonicalArchitecturePath(attachedPointerArray) !=
            canonicalPointerArray ||
        classifyArchitectureSignal(attachedPointerArray).key !=
            QStringLiteral("issue_valid")) {
        error = QStringLiteral(
            "attached array-index path was not normalized/classified");
        return false;
    }
    const QString flattenedArray =
        QStringLiteral("gpu.m_QPPUTOP[size=4].[0].m_QPPUCtrl."
                       "sg_table_[size=16].[0].valid");
    if (canonicalArchitecturePath(flattenedArray) != flattenedArray ||
        classifyArchitectureSignal(flattenedArray).key !=
            QStringLiteral("shader_group_state")) {
        error = QStringLiteral(
            "flattened array-index path normalization regressed");
        return false;
    }

    CounterView flat;
    flat.path =
        QStringLiteral("gpu.m_clusters[size=2].[0].m_dppu[size=5].[0]."
                       "m_ppu[size=2].[0].m_QPPUTOP[size=4].[0]."
                       "m_QPPUCtrl.issue_inst_[size=2].[0].vld");
    flat.key = QStringLiteral("issue_valid");
    flat.category = QStringLiteral("issue");
    flat.activeTicks = 20;
    flat.knownTicks = 50;
    IssueContextView flatIssue;
    flatIssue.path =
        QStringLiteral("gpu.m_clusters[size=2].[0].m_dppu[size=5].[0]."
                       "m_ppu[size=2].[0].m_QPPUTOP[size=4].[0].m_QPPUCtrl");
    flatIssue.issuedTicks = 30;
    flatIssue.issueActiveTicks = 20;
    flatIssue.dualIssueTicks = 10;
    flatIssue.idleTicks = 30;
    flatIssue.capacityTicks = 100;
    flatIssue.slotActiveTicks = {20, 10};
    flatIssue.slotCapacityTicks = {50, 50};
    const ArchitectureProfile flatProfile =
        buildArchitectureProfile({flat}, {flatIssue}, 50, 10);
    if (!flatProfile.hasRepresentativeArrays || flatProfile.roots.size() != 1) {
        error = QStringLiteral("flattened hierarchy was not recognized");
        return false;
    }
    bool foundQppuCtrl = false;
    for (const QJsonValue& value : flatProfile.classSummaries) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("class_key")).toString() !=
            QStringLiteral("qppu_ctrl")) {
            continue;
        }
        foundQppuCtrl = true;
        const QJsonObject observed =
            object.value(QStringLiteral("observed")).toObject();
        const QJsonArray issueSlotArray =
            observed.value(QStringLiteral("issue_slots")).toArray();
        if (observed.value(QStringLiteral("issue_contexts")).toInt() != 1 ||
            observed.value(QStringLiteral("issue_observed_cycles")).toDouble() !=
                5.0 ||
            observed.value(QStringLiteral("dual_issue_cycles")).toDouble() != 1.0 ||
            issueSlotArray.size() != 2 ||
            issueSlotArray.at(0).toObject()
                    .value(QStringLiteral("utilization_percent")).toDouble() !=
                40.0 ||
            issueSlotArray.at(1).toObject()
                    .value(QStringLiteral("utilization_percent")).toDouble() !=
                20.0) {
            error = QStringLiteral("per-instance issue metrics were not attached");
            return false;
        }
    }
    if (!foundQppuCtrl) {
        error = QStringLiteral("QPPUCtrl class summary was not generated");
        return false;
    }

    IssueContextView partialIssue = flatIssue;
    partialIssue.issueActiveTicks = 20;
    partialIssue.idleTicks = 10;
    partialIssue.capacityTicks = 70;
    partialIssue.slotCapacityTicks = {50, 20};
    const ArchitectureProfile partialIssueProfile =
        buildArchitectureProfile({flat}, {partialIssue}, 50, 10);
    bool foundPartialIssue = false;
    for (const QJsonValue& value : partialIssueProfile.classSummaries) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("class_key")).toString() !=
            QStringLiteral("qppu_ctrl")) {
            continue;
        }
        foundPartialIssue = true;
        const QJsonObject observed =
            object.value(QStringLiteral("observed")).toObject();
        const QJsonArray issueSlotsJson =
            observed.value(QStringLiteral("issue_slots")).toArray();
        if (observed.value(
                QStringLiteral("issue_coverage_complete")).toBool() ||
            observed.value(
                QStringLiteral("issue_unknown_cycles")).toDouble() != 2.0 ||
            observed.value(
                QStringLiteral("issue_slot_coverage_complete")).toBool() ||
            issueSlotsJson.size() != 2 ||
            issueSlotsJson.at(0).toObject()
                .value(QStringLiteral("covered")).toBool() != true ||
            issueSlotsJson.at(1).toObject()
                .value(QStringLiteral("covered")).toBool() ||
            issueSlotsJson.at(1).toObject()
                .value(QStringLiteral("unknown_cycles")).toDouble() != 3.0) {
            error = QStringLiteral(
                "partial architecture issue coverage was marked complete");
            return false;
        }
    }
    if (!foundPartialIssue) {
        error = QStringLiteral(
            "partial architecture issue context was not generated");
        return false;
    }

    QVector<CounterView> partialArrayCounters;
    for (int index = 0; index < 2; ++index) {
        CounterView counter;
        counter.path =
            QStringLiteral(
                "gpu.m_QPPUTOP[size=5].[%1].m_QPPUCtrl."
                "issue_inst_[size=2].[0].vld")
                .arg(index);
        counter.key = QStringLiteral("issue_valid");
        counter.category = QStringLiteral("issue");
        counter.activeTicks = 10;
        counter.knownTicks = 50;
        partialArrayCounters.push_back(counter);
    }
    const ArchitectureProfile partialArrayProfile =
        buildArchitectureProfile(partialArrayCounters, {}, 50, 10);
    bool foundPartialProjection = false;
    for (const QJsonValue& value : partialArrayProfile.classSummaries) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("class_key")).toString() !=
            QStringLiteral("qppu_ctrl")) {
            continue;
        }
        foundPartialProjection = true;
        const QJsonObject projected =
            object.value(
                QStringLiteral("representative_projection")).toObject();
        if (object.value(
                QStringLiteral("represented_instances")).toString() !=
                QStringLiteral("5") ||
            projected.value(
                QStringLiteral("active_signal_cycles")).toDouble() !=
                5.0) {
            error = QStringLiteral(
                "partial array projection did not preserve logical size");
            return false;
        }
    }
    if (!foundPartialProjection) {
        error = QStringLiteral(
            "partial array projection summary was not generated");
        return false;
    }

    QVector<CounterView> normalCounters;
    QVector<IssueContextView> normalIssues;
    const QVector<QVector<qint64>> normalSlotTicks = {
        {800, 400},
        {500, 0},
        {1000, 1000},
        {0, 0},
    };
    for (int i = 0; i < 4; ++i) {
        for (int slot = 0; slot < 2; ++slot) {
            CounterView counter = flat;
            counter.path =
                QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                               "m_QPPUTOP.[%1].m_QPPUCtrl."
                               "issue_inst_.[%2].vld")
                    .arg(i)
                    .arg(slot);
            counter.activeTicks = normalSlotTicks.at(i).at(slot);
            counter.knownTicks = 1000;
            normalCounters.push_back(counter);
        }

        IssueContextView issue;
        issue.path =
            QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                           "m_QPPUTOP.[%1].m_QPPUCtrl")
                .arg(i);
        issue.issuedTicks =
            normalSlotTicks.at(i).at(0) + normalSlotTicks.at(i).at(1);
        issue.issueActiveTicks = i == 0 ? 800 : (i == 1 ? 500 : (i == 2 ? 1000 : 0));
        issue.dualIssueTicks = i == 0 ? 400 : (i == 2 ? 1000 : 0);
        issue.idleTicks = 1000 - issue.issueActiveTicks;
        issue.capacityTicks = 2000;
        issue.slotActiveTicks = normalSlotTicks.at(i);
        issue.slotCapacityTicks = {1000, 1000};
        normalIssues.push_back(issue);
    }
    const ArchitectureProfile normalProfile =
        buildArchitectureProfile(normalCounters, normalIssues, 1000, 10);
    if (normalProfile.hasRepresentativeArrays || normalProfile.roots.size() != 1) {
        error = QStringLiteral("normal hierarchy was misclassified as representative");
        return false;
    }
    const QJsonObject normalAggregate =
        normalProfile.roots.at(0).toObject()
            .value(QStringLiteral("aggregate")).toObject();
    if (normalAggregate.value(QStringLiteral("issue_contexts")).toInt() != 4 ||
        normalAggregate.value(QStringLiteral("issue_observed_cycles")).toDouble() !=
            400.0 ||
        std::fabs(
            normalAggregate
                    .value(QStringLiteral("issue_utilization_percent"))
                    .toDouble() -
            92.5) > 1e-9) {
        error =
            QStringLiteral("normal hierarchy aggregation mismatch: contexts=%1 "
                           "observed=%2 utilization=%3")
                .arg(normalAggregate.value(QStringLiteral("issue_contexts")).toInt())
                .arg(normalAggregate
                         .value(QStringLiteral("issue_observed_cycles")).toDouble())
                .arg(normalAggregate
                         .value(QStringLiteral("issue_utilization_percent"))
                         .toDouble());
        return false;
    }

    const ClassifiedSignal cb = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters[size=2].[0].m_dppu[size=5].[0]."
                       "m_ppu[size=2].[0].m_CBCtrl.request_fifo.m_num_readable"));
    const ClassifiedSignal fifoRead = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters[size=2].[0].m_dppu[size=5].[0]."
                       "m_ppu[size=2].[0].m_CBCtrl.request_fifo.m_num_read"));
    if (cb.key != QStringLiteral("fifo_occupancy") ||
        fifoRead.key != QStringLiteral("fifo_reads") ||
        fifoRead.eventSemantics != EventSemantics::PerCycleValue) {
        error = QStringLiteral("CB_CTRL FIFO counter classification failed");
        return false;
    }
    const ClassifiedSignal queue = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters[size=2].[0].m_dppu[size=5].[0]."
                       "m_ppu[size=2].[0].m_QPPUTOP[size=4].[0]."
                       "m_QPPUCtrl.instr_queue_[size=16].[0].m_count"));
    const ClassifiedSignal queueCapacity = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters[size=2].[0].m_dppu[size=5].[0]."
                       "m_ppu[size=2].[0].m_QPPUTOP[size=4].[0]."
                       "m_QPPUCtrl.instr_queue_[size=16].[0].m_size"));
    if (queue.key != QStringLiteral("queue_occupancy") ||
        queueCapacity.key != QStringLiteral("queue_capacity")) {
        error = QStringLiteral("instruction Queue counter classification failed");
        return false;
    }
    const ClassifiedSignal cacheMiss = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_L1.cache_miss_valid"));
    const ClassifiedSignal cacheHitCounter = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_L1.cache_hit_count"));
    const ClassifiedSignal cacheHitsCounter = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_L1.cache_hits_count"));
    const ClassifiedSignal cacheMissesCounter = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_L1.cache_misses_count"));
    const ClassifiedSignal outstandingMisses = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_QPPUCtrl.sg_table_.[0].missCount"));
    const ClassifiedSignal creditPulse = classifyArchitectureSignal(
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_L1.return_credit"));
    if (cacheMiss.key != QStringLiteral("cache_miss") ||
        cacheMiss.eventSemantics != EventSemantics::PerCycleMask ||
        cacheHitCounter.eventSemantics !=
            EventSemantics::CumulativeCounter ||
        cacheHitsCounter.key != QStringLiteral("cache_hit") ||
        cacheHitsCounter.eventSemantics !=
            EventSemantics::CumulativeCounter ||
        cacheMissesCounter.key != QStringLiteral("cache_miss") ||
        cacheMissesCounter.eventSemantics !=
            EventSemantics::CumulativeCounter ||
        outstandingMisses.key == QStringLiteral("cache_miss") ||
        creditPulse.category == QStringLiteral("credit")) {
        error = QStringLiteral("cache event/state classification failed");
        return false;
    }

    const QString cbPath =
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_CBCtrl.request_fifo.");
    CounterView occupancy;
    occupancy.path = cbPath + QStringLiteral("m_numAvail");
    occupancy.key = QStringLiteral("fifo_occupancy");
    occupancy.category = QStringLiteral("fifo");
    occupancy.activeTicks = 50;
    occupancy.knownTicks = 50;
    occupancy.weightedValueTicks = 100.0L;
    occupancy.fifoFullTicks = 10;
    occupancy.fifoFullKnownTicks = 50;
    CounterView capacity;
    capacity.path = cbPath + QStringLiteral("m_size");
    capacity.key = QStringLiteral("fifo_capacity");
    capacity.category = QStringLiteral("capacity");
    capacity.activeTicks = 50;
    capacity.knownTicks = 50;
    capacity.weightedValueTicks = 400.0L;
    CounterView hit;
    hit.path = cbPath + QStringLiteral("cache_hit");
    hit.key = QStringLiteral("cache_hit");
    hit.category = QStringLiteral("cache");
    hit.transitions = 18;
    hit.eventCount = 9;
    CounterView miss;
    miss.path = cbPath + QStringLiteral("cache_miss");
    miss.key = QStringLiteral("cache_miss");
    miss.category = QStringLiteral("cache");
    miss.transitions = 2;
    miss.eventCount = 1;
    CounterView queueOccupancy;
    queueOccupancy.path =
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_CBCtrl.request_queue.m_count");
    queueOccupancy.key = QStringLiteral("queue_occupancy");
    queueOccupancy.category = QStringLiteral("queue");
    queueOccupancy.activeTicks = 50;
    queueOccupancy.knownTicks = 50;
    queueOccupancy.queueFullTicks = 5;
    queueOccupancy.queueFullKnownTicks = 50;
    CounterView queueSize;
    queueSize.path =
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_CBCtrl.request_queue.m_size");
    queueSize.key = QStringLiteral("queue_capacity");
    queueSize.category = QStringLiteral("capacity");
    queueSize.activeTicks = 50;
    queueSize.knownTicks = 50;
    const ArchitectureProfile capacityProfile =
        buildArchitectureProfile(
            {occupancy, capacity, hit, miss, queueOccupancy, queueSize},
            {}, 50, 10);
    bool foundCapacityMetrics = false;
    for (const QJsonValue& value : capacityProfile.classSummaries) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("class_key")).toString() !=
            QStringLiteral("cb_ctrl")) {
            continue;
        }
        const QJsonObject observed =
            object.value(QStringLiteral("observed")).toObject();
        if (observed.value(QStringLiteral("fifo_full_rate_percent")).toDouble() !=
                20.0 ||
            observed.value(QStringLiteral("fifo_full_cycles")).toDouble() !=
                1.0 ||
            observed.value(QStringLiteral("queue_full_rate_percent")).toDouble() !=
                10.0 ||
             observed.value(QStringLiteral("queue_full_cycles")).toDouble() !=
                 0.5 ||
             observed.value(QStringLiteral("cache_hit_events")).toString() !=
                 QStringLiteral("9") ||
             observed.value(QStringLiteral("cache_miss_events")).toString() !=
                 QStringLiteral("1") ||
             !observed.value(
                 QStringLiteral("cache_rate_coverage_complete")).toBool() ||
             observed.value(QStringLiteral("cache_hit_rate_percent")).toDouble() !=
                 90.0) {
            error =
                QStringLiteral("FIFO/Queue full rate or cache hit rate failed");
            return false;
        }
        foundCapacityMetrics = true;
    }
    if (!foundCapacityMetrics) {
        error = QStringLiteral("capacity-aware CB_CTRL metrics were not generated");
        return false;
    }
    const ArchitectureProfile oneSidedCacheProfile =
        buildArchitectureProfile({hit}, {}, 50, 10);
    const QJsonObject oneSidedCache =
        oneSidedCacheProfile.roots.first().toObject()
            .value(QStringLiteral("aggregate")).toObject();
    if (oneSidedCache.value(
            QStringLiteral("cache_rate_coverage_complete")).toBool() ||
        oneSidedCache.value(
            QStringLiteral("cache_hit_rate_percent")).toDouble() != 0.0) {
        error = QStringLiteral(
            "one-sided cache coverage produced a hit rate");
        return false;
    }
    CounterView mismatchedMiss = miss;
    mismatchedMiss.path =
        cbPath + QStringLiteral("other_cache.cache_miss");
    const ArchitectureProfile mismatchedCacheProfile =
        buildArchitectureProfile({hit, mismatchedMiss}, {}, 50, 10);
    const QJsonObject mismatchedCache =
        mismatchedCacheProfile.roots.first().toObject()
            .value(QStringLiteral("aggregate")).toObject();
    if (mismatchedCache.value(
            QStringLiteral("cache_rate_coverage_complete")).toBool()) {
        error = QStringLiteral(
            "cache events from different resources were paired");
        return false;
    }
    CounterView discontinuousHit = hit;
    discontinuousHit.eventDiscontinuities = 1;
    const ArchitectureProfile discontinuousCacheProfile =
        buildArchitectureProfile(
            {discontinuousHit, miss}, {}, 50, 10);
    const QJsonObject discontinuousCache =
        discontinuousCacheProfile.roots.first().toObject()
            .value(QStringLiteral("aggregate")).toObject();
    if (discontinuousCache.value(
            QStringLiteral("cache_rate_coverage_complete")).toBool()) {
        error = QStringLiteral(
            "discontinuous cache counter produced a hit rate");
        return false;
    }

    const QString euInputPath =
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_QPPUTOP.[0].m_QPPUEU.pt_BE_ThdCore_new_inst.");
    CounterView euOccupancy;
    euOccupancy.path = euInputPath + QStringLiteral("m_num_readable");
    euOccupancy.key = QStringLiteral("fifo_occupancy");
    euOccupancy.category = QStringLiteral("fifo");
    euOccupancy.activeTicks = 40;
    euOccupancy.knownTicks = 100;
    euOccupancy.fifoFullTicks = 10;
    euOccupancy.fifoFullKnownTicks = 100;
    CounterView euReads;
    euReads.path = euInputPath + QStringLiteral("m_num_read");
    euReads.key = QStringLiteral("fifo_reads");
    euReads.category = QStringLiteral("fifo");
    euReads.eventCount = 8;
    CounterView euWrites;
    euWrites.path = euInputPath + QStringLiteral("m_num_written");
    euWrites.key = QStringLiteral("fifo_writes");
    euWrites.category = QStringLiteral("fifo");
    euWrites.eventCount = 9;
    CounterView euPhase1;
    euPhase1.path =
        QStringLiteral("gpu.m_clusters.[0].m_dppu.[0].m_ppu.[0]."
                       "m_QPPUTOP.[0].m_QPPUEU.phase1_req_.valid");
    euPhase1.key = QStringLiteral("interface_valid");
    euPhase1.category = QStringLiteral("interface");
    euPhase1.activeTicks = 20;
    euPhase1.knownTicks = 100;
    const ArchitectureProfile euProfile =
        buildArchitectureProfile(
            {euOccupancy, euReads, euWrites, euPhase1}, {}, 100, 10);
    bool foundEuMetrics = false;
    for (const QJsonValue& value : euProfile.classSummaries) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("class_key")).toString() !=
            QStringLiteral("qppu_eu")) {
            continue;
        }
        const QJsonObject eu =
            object.value(QStringLiteral("observed")).toObject()
                .value(QStringLiteral("eu")).toObject();
        const QJsonObject execution =
            eu.value(QStringLiteral("execution")).toObject();
        const QJsonObject input =
            eu.value(QStringLiteral("instruction_input")).toObject();
        const QJsonObject phase1 =
            eu.value(QStringLiteral("phase1_request")).toObject();
        if (execution.value(QStringLiteral("instructions_executed")).toString() !=
                QStringLiteral("8") ||
            !execution.value(
                QStringLiteral("event_coverage_complete")).toBool() ||
            execution.value(QStringLiteral("utilization_percent")).toDouble() !=
                80.0 ||
            input.value(QStringLiteral("nonempty_rate_percent")).toDouble() !=
                40.0 ||
            input.value(QStringLiteral("full_rate_percent")).toDouble() != 10.0 ||
            phase1.value(QStringLiteral("pending_rate_percent")).toDouble() !=
                20.0) {
            error = QStringLiteral("QPPU EU channel metrics failed");
            return false;
        }
        foundEuMetrics = true;
    }
    if (!foundEuMetrics) {
        error = QStringLiteral("QPPU EU metrics were not generated");
        return false;
    }

    CounterView mismatchedEuWrites = euWrites;
    mismatchedEuWrites.path.replace(
        QStringLiteral("m_QPPUTOP.[0]"),
        QStringLiteral("m_QPPUTOP.[1]"));
    CounterView partialEuPhase1 = euPhase1;
    partialEuPhase1.knownTicks = 90;
    partialEuPhase1.unknownTicks = 10;
    const ArchitectureProfile invalidEuProfile =
        buildArchitectureProfile(
            {euOccupancy, euReads, mismatchedEuWrites,
             partialEuPhase1},
            {}, 100, 10);
    bool foundInvalidEuMetrics = false;
    for (const QJsonValue& value : invalidEuProfile.classSummaries) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("class_key")).toString() !=
            QStringLiteral("qppu_eu")) {
            continue;
        }
        foundInvalidEuMetrics = true;
        const QJsonObject eu =
            object.value(QStringLiteral("observed")).toObject()
                .value(QStringLiteral("eu")).toObject();
        const QJsonObject execution =
            eu.value(QStringLiteral("execution")).toObject();
        const QJsonObject phase1 =
            eu.value(QStringLiteral("phase1_request")).toObject();
        if (execution.value(
                QStringLiteral("event_coverage_complete")).toBool() ||
            execution.contains(
                QStringLiteral("utilization_percent")) ||
            phase1.value(
                QStringLiteral("coverage_complete")).toBool() ||
            phase1.contains(
                QStringLiteral("pending_rate_percent"))) {
            error = QStringLiteral(
                "partial or mismatched EU signals produced rates");
            return false;
        }
    }
    if (!foundInvalidEuMetrics) {
        error = QStringLiteral(
            "invalid QPPU EU coverage was not reported");
        return false;
    }
    return true;
}

}  // namespace waveperf
