#include "WavePerfScheduler.h"

#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace waveperf {
namespace {

struct State {
    bool known = false;
    quint64 value = 0;
};

State stateFromSample(const WaveSignal& signal, const WaveSample& input) {
    WaveSample sample = input;
    if (!sample.rawFieldsReady) {
        hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
    }
    if (sample.isZ || sample.isAbsent) return State();
    State result;
    result.known = true;
    result.value = sample.rawBits & waveBitMaskForWidth(signal.width);
    return result;
}

State stateAt(const WaveSignal* signal, qint64 time) {
    if (!signal) return State();
    int lo = 0;
    int hi = signal->samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (signal->samples.at(mid).time <= time) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo > 0 ? stateFromSample(*signal, signal->samples.at(lo - 1))
                  : State();
}

bool positive(const WaveSignal* signal, qint64 time, bool& known) {
    const State state = stateAt(signal, time);
    known = state.known;
    return state.known && state.value != 0;
}

struct SgSignals {
    int index = -1;
    const WaveSignal* valid = nullptr;
    const WaveSignal* queueCount = nullptr;
    const WaveSignal* stall = nullptr;
    const WaveSignal* sleep = nullptr;
    const WaveSignal* flow = nullptr;
    const WaveSignal* barrier = nullptr;
    const WaveSignal* setMaxTemp = nullptr;
    const WaveSignal* inflightMemory = nullptr;
    QVector<const WaveSignal*> dependencies;
    const WaveSignal* queueHeadIndex = nullptr;
    QMap<int, const WaveSignal*> queueEntryPc;
    QMap<int, const WaveSignal*> queueEntryIssueType;
    QMap<int, const WaveSignal*> queueEntryThreadSubtype;
    QMap<int, const WaveSignal*> queueEntryExeUnit;
};

struct IssueSignals {
    int slot = -1;
    const WaveSignal* valid = nullptr;
    const WaveSignal* sgId = nullptr;
    const WaveSignal* pc = nullptr;
    const WaveSignal* issueType = nullptr;
};

struct QppuSignals {
    QString path;
    int index = -1;
    QMap<int, SgSignals> shaderGroups;
    QVector<IssueSignals> issueSlots;
    int declaredIssueSlots = 0;
    QMap<int, const WaveSignal*> functionUnitPending;
    const WaveSignal* mmaLoadCredit = nullptr;
    const WaveSignal* mmaStoreCredit = nullptr;
    const WaveSignal* icacheCredit = nullptr;
};

struct SgMetrics {
    qint64 activeTicks = 0;
    qint64 queueReadyTicks = 0;
    qint64 eligibleTicks = 0;
    qint64 issuedTicks = 0;
    qint64 unknownTicks = 0;
    QMap<QString, qint64> stateTicks;
    QMap<QString, qint64> blockTicks;
    qint64 inflightMemoryTicks = 0;
    qint64 inflightMemoryKnownTicks = 0;
    long double inflightMemoryWeightedTicks = 0.0L;
    quint64 inflightMemoryMax = 0;
};

struct Bin {
    qint64 startTick = 0;
    qint64 endTick = 0;
    qint64 activeSgTicks = 0;
    qint64 queueReadySgTicks = 0;
    qint64 eligibleSgTicks = 0;
    qint64 issuedTicks = 0;
    QMap<QString, qint64> blockTicks;
};

struct PcMetrics {
    QString qppuPath;
    quint64 pc = 0;
    quint64 issueType = 0;
    bool issueTypeKnown = false;
    qint64 issuedTicks = 0;
    qint64 waitTicks = 0;
    QMap<QString, qint64> waitReasonTicks;
};

bool queueHeadState(const SgSignals& sg,
                    qint64 time,
                    quint64& pc,
                    quint64& issueType,
                    bool& issueTypeKnown) {
    const State index = stateAt(sg.queueHeadIndex, time);
    if (!index.known) return false;
    const WaveSignal* pcSignal =
        sg.queueEntryPc.value(int(index.value), nullptr);
    const State pcState = stateAt(pcSignal, time);
    if (!pcState.known) return false;
    pc = pcState.value;
    const State typeState =
        stateAt(sg.queueEntryIssueType.value(int(index.value), nullptr), time);
    issueTypeKnown = typeState.known;
    issueType = typeState.value;
    return true;
}

bool queueHeadInstructionState(const SgSignals& sg,
                               qint64 time,
                               quint64& issueType,
                               State& threadSubtype,
                               State& exeUnit) {
    const State index = stateAt(sg.queueHeadIndex, time);
    if (!index.known) return false;
    const int entry = int(index.value);
    const State type =
        stateAt(sg.queueEntryIssueType.value(entry, nullptr), time);
    if (!type.known) return false;
    issueType = type.value;
    threadSubtype =
        stateAt(sg.queueEntryThreadSubtype.value(entry, nullptr), time);
    exeUnit = stateAt(sg.queueEntryExeUnit.value(entry, nullptr), time);
    return true;
}

QString qppuRoot(const QString& path) {
    static const QRegularExpression expression(
        QStringLiteral("^((?:.*\\.)?m_QPPUTOP(?:\\[size=\\d+\\])?\\.\\[\\d+\\])"
                       "\\.m_QPPUCtrl\\."));
    const QRegularExpressionMatch match = expression.match(path);
    return match.hasMatch() ? match.captured(1) : QString();
}

int qppuIndex(const QString& root) {
    static const QRegularExpression expression(
        QStringLiteral("(?:^|\\.)m_QPPUTOP(?:\\[size=\\d+\\])?"
                       "\\.\\[(\\d+)\\]$"));
    const QRegularExpressionMatch match = expression.match(root);
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

void appendBoundarySignals(const SgSignals& sg,
                           QVector<const WaveSignal*>& result) {
    result.push_back(sg.valid);
    result.push_back(sg.queueCount);
    result.push_back(sg.stall);
    result.push_back(sg.sleep);
    result.push_back(sg.flow);
    result.push_back(sg.barrier);
    result.push_back(sg.setMaxTemp);
    result.push_back(sg.inflightMemory);
    result.push_back(sg.queueHeadIndex);
    for (const WaveSignal* signal : sg.dependencies) result.push_back(signal);
    for (const WaveSignal* signal : sg.queueEntryPc) result.push_back(signal);
    for (const WaveSignal* signal : sg.queueEntryIssueType) {
        result.push_back(signal);
    }
    for (const WaveSignal* signal : sg.queueEntryThreadSubtype) {
        result.push_back(signal);
    }
    for (const WaveSignal* signal : sg.queueEntryExeUnit) {
        result.push_back(signal);
    }
}

QVector<qint64> boundaries(const QVector<const WaveSignal*>& signalSet,
                           qint64 startTick,
                           qint64 endTick) {
    QVector<qint64> result;
    result.push_back(startTick);
    result.push_back(endTick);
    for (const WaveSignal* signal : signalSet) {
        if (!signal) continue;
        for (const WaveSample& sample : signal->samples) {
            if (sample.time > startTick && sample.time < endTick) {
                result.push_back(sample.time);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

QString localState(const SgSignals& sg,
                   const QVector<IssueSignals>& issueSlots,
                   const QMap<int, const WaveSignal*>& functionUnitPending,
                   bool issueCoverageComplete,
                   qint64 time,
                   bool& active,
                   bool& queueReady,
                   bool& eligible,
                   bool& issued,
                   bool& activityCoverage,
                   bool& eligibilityCoverage) {
    bool validKnown = false;
    const bool valid = positive(sg.valid, time, validKnown);
    bool queueKnown = false;
    const bool queue = positive(sg.queueCount, time, queueKnown);
    active = validKnown ? valid : (queueKnown && queue);
    queueReady = active && queueKnown && queue;
    eligible = false;
    issued = false;
    activityCoverage = validKnown && queueKnown;
    eligibilityCoverage =
        validKnown && queueKnown && stateAt(sg.stall, time).known &&
        stateAt(sg.sleep, time).known && stateAt(sg.flow, time).known &&
        stateAt(sg.barrier, time).known &&
        stateAt(sg.setMaxTemp, time).known &&
        sg.dependencies.size() == 7 &&
        issueCoverageComplete;
    for (const WaveSignal* dependency : sg.dependencies) {
        eligibilityCoverage =
            eligibilityCoverage && stateAt(dependency, time).known;
    }

    bool issueAttributionKnown = issueCoverageComplete;
    for (const IssueSignals& slot : issueSlots) {
        bool issueKnown = false;
        const bool issueValid = positive(slot.valid, time, issueKnown);
        issueAttributionKnown = issueAttributionKnown && issueKnown;
        if (!issueValid) continue;
        const State issueSg = stateAt(slot.sgId, time);
        issueAttributionKnown =
            issueAttributionKnown && issueSg.known;
        if (issueSg.known && int(issueSg.value) == sg.index) {
            issued = true;
            break;
        }
    }

    if (validKnown && !valid) return QStringLiteral("inactive");
    if (!active) return QStringLiteral("unknown");
    if (queueKnown && !queue) return QStringLiteral("frontend_empty");
    if (!queueKnown) return QStringLiteral("unknown");
    if (issued) {
        eligible = true;
        return QStringLiteral("issued");
    }
    if (!issueAttributionKnown) {
        eligibilityCoverage = false;
        return QStringLiteral("unknown");
    }

    struct ReasonSignal {
        const char* key;
        const WaveSignal* signal;
    };
    const ReasonSignal ordered[] = {
        {"barrier", sg.barrier},
        {"flow_control", sg.flow},
        {"sleep", sg.sleep},
        {"set_max_temp", sg.setMaxTemp},
        {"stall_count", sg.stall}
    };
    for (const ReasonSignal& reason : ordered) {
        bool known = false;
        if (positive(reason.signal, time, known)) {
            return QString::fromLatin1(reason.key);
        }
        eligibilityCoverage = eligibilityCoverage && known;
    }

    bool dependenciesKnown = sg.dependencies.size() == 7;
    for (const WaveSignal* dependency : sg.dependencies) {
        bool known = false;
        if (positive(dependency, time, known)) {
            return QStringLiteral("dependency");
        }
        dependenciesKnown = dependenciesKnown && known;
    }
    eligibilityCoverage = eligibilityCoverage && dependenciesKnown;
    if (!eligibilityCoverage) return QStringLiteral("unknown");

    quint64 issueType = 0;
    State threadSubtype;
    State exeUnit;
    if (!queueHeadInstructionState(
            sg, time, issueType, threadSubtype, exeUnit)) {
        eligibilityCoverage = false;
        return QStringLiteral("unknown");
    }
    if (issueType == 2) {
        // FP32MinOrMax (subtype 0) can use either FP32 unit. Other Thread
        // instructions use the unit selected by predecode.
        if (threadSubtype.known && threadSubtype.value == 0) {
            const State fp32Max =
                stateAt(functionUnitPending.value(0, nullptr), time);
            const State fp32Min =
                stateAt(functionUnitPending.value(1, nullptr), time);
            eligibilityCoverage =
                eligibilityCoverage && fp32Max.known && fp32Min.known;
            if (!eligibilityCoverage) return QStringLiteral("unknown");
            if (fp32Max.value != 0 && fp32Min.value != 0) {
                return QStringLiteral("function_unit");
            }
        } else {
            if (!exeUnit.known) {
                eligibilityCoverage = false;
                return QStringLiteral("unknown");
            }
            const State pending =
                stateAt(functionUnitPending.value(int(exeUnit.value), nullptr),
                        time);
            eligibilityCoverage = eligibilityCoverage && pending.known;
            if (!eligibilityCoverage) return QStringLiteral("unknown");
            if (pending.value != 0) {
                return QStringLiteral("function_unit");
            }
        }
    }

    eligible = true;
    return QStringLiteral("eligible");
}

void addToBins(QVector<Bin>& bins,
               qint64 begin,
               qint64 end,
               const QString& state,
               bool active,
               bool queueReady,
               bool eligible,
               bool issued) {
    if (bins.isEmpty() || end <= begin) return;
    for (Bin& bin : bins) {
        const qint64 overlap =
            qMax<qint64>(0, qMin(end, bin.endTick) - qMax(begin, bin.startTick));
        if (overlap <= 0) continue;
        if (active) bin.activeSgTicks += overlap;
        if (queueReady) bin.queueReadySgTicks += overlap;
        if (eligible) bin.eligibleSgTicks += overlap;
        if (issued) bin.issuedTicks += overlap;
        if (state != QStringLiteral("inactive") &&
            state != QStringLiteral("frontend_empty") &&
            state != QStringLiteral("eligible") &&
            state != QStringLiteral("issued") &&
            state != QStringLiteral("unknown")) {
            bin.blockTicks[state] += overlap;
        }
    }
}

double cycles(qint64 ticks, qint64 ticksPerCycle) {
    return ticksPerCycle > 0 ? double(ticks) / double(ticksPerCycle) : 0.0;
}

double percent(qint64 numerator, qint64 denominator) {
    return denominator > 0 ? 100.0 * double(numerator) / double(denominator)
                           : 0.0;
}

QString reasonName(const QString& key) {
    if (key == QStringLiteral("barrier")) return QStringLiteral("Barrier 等待");
    if (key == QStringLiteral("flow_control")) return QStringLiteral("流控等待");
    if (key == QStringLiteral("sleep")) return QStringLiteral("Sleep 等待");
    if (key == QStringLiteral("set_max_temp")) return QStringLiteral("SetMaxTemp 等待");
    if (key == QStringLiteral("stall_count")) return QStringLiteral("指令延迟计数");
    if (key == QStringLiteral("dependency")) return QStringLiteral("依赖计数");
    if (key == QStringLiteral("function_unit")) return QStringLiteral("功能单元忙");
    return key;
}

}  // namespace

QJsonObject buildSchedulerProfile(
    const QHash<QString, const WaveSignal*>& signalsByPath,
    qint64 startTick,
    qint64 endTick,
    qint64 ticksPerCycle,
    int timelineBinCount,
    QStringList& warnings) {
    QMap<QString, QppuSignals> qppus;
    static const QRegularExpression sgField(
        QStringLiteral("\\.(?:sg_table_|instr_queue_|stall_cnt_vector_|"
                       "sleep_cnt_vector_|flow_ctrl_pend_wait_vector_|"
                       "barrier_pend_wait_vector_|set_max_temp_pend_wait_vector_|"
                       "inflight_mem_cnt_)(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]"));
    static const QRegularExpression dependencyField(
        QStringLiteral("\\.check_dep_cnt_vector_(?:\\[size=\\d+\\])?"
                       "\\.\\[(\\d+)\\](?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]$"));
    static const QRegularExpression issueField(
        QStringLiteral("\\.issue_inst_(?:\\[size=(\\d+)\\])?\\.\\[(\\d+)\\]\\.vld$"));
    static const QRegularExpression queueEntryField(
        QStringLiteral("\\.instr_queue_(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]"
                       "\\.m_QData(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]"
                       "\\.Data\\.(PC\\.pc_|preDecode\\.instIssueType|"
                       "preDecode\\.instType\\.subType\\.thread|"
                       "preDecode\\.exeThdUnit)$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression queueHeadField(
        QStringLiteral("\\.instr_queue_(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]"
                       "\\.(m_ri|m_read_index|m_readindex|m_head|m_front)$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression functionUnitField(
        QStringLiteral("\\.function_unit_pend_wait_vector_"
                       "(?:\\[size=\\d+\\])?\\.\\[(\\d+)\\]$"));

    for (auto it = signalsByPath.constBegin(); it != signalsByPath.constEnd(); ++it) {
        const QString root = qppuRoot(it.key());
        if (root.isEmpty()) continue;
        QppuSignals& qppu = qppus[root];
        qppu.path = root;
        qppu.index = qppuIndex(root);

        const QRegularExpressionMatch queueEntryMatch =
            queueEntryField.match(it.key());
        if (queueEntryMatch.hasMatch()) {
            const int sg = queueEntryMatch.captured(1).toInt();
            const int entry = queueEntryMatch.captured(2).toInt();
            SgSignals& sgSignals = qppu.shaderGroups[sg];
            sgSignals.index = sg;
            const QString field = queueEntryMatch.captured(3);
            if (field.compare(
                    QStringLiteral("PC.pc_"),
                    Qt::CaseInsensitive) == 0) {
                sgSignals.queueEntryPc.insert(entry, it.value());
            } else if (field.compare(
                           QStringLiteral("preDecode.instIssueType"),
                           Qt::CaseInsensitive) == 0) {
                sgSignals.queueEntryIssueType.insert(entry, it.value());
            } else if (field.compare(
                           QStringLiteral(
                               "preDecode.instType.subType.thread"),
                           Qt::CaseInsensitive) == 0) {
                sgSignals.queueEntryThreadSubtype.insert(entry, it.value());
            } else {
                sgSignals.queueEntryExeUnit.insert(entry, it.value());
            }
            continue;
        }
        const QRegularExpressionMatch queueHeadMatch =
            queueHeadField.match(it.key());
        if (queueHeadMatch.hasMatch()) {
            const int sg = queueHeadMatch.captured(1).toInt();
            SgSignals& sgSignals = qppu.shaderGroups[sg];
            sgSignals.index = sg;
            sgSignals.queueHeadIndex = it.value();
            continue;
        }

        const QRegularExpressionMatch issueMatch = issueField.match(it.key());
        if (issueMatch.hasMatch()) {
            IssueSignals issue;
            issue.slot = issueMatch.captured(2).toInt();
            const int declared =
                issueMatch.captured(1).isEmpty()
                    ? issue.slot + 1
                    : issueMatch.captured(1).toInt();
            qppu.declaredIssueSlots =
                qMax(qppu.declaredIssueSlots, declared);
            issue.valid = it.value();
            const QString issueRoot = it.key().left(it.key().size() - 4);
            issue.sgId =
                signalsByPath.value(issueRoot + QStringLiteral(".sgId"), nullptr);
            if (!issue.sgId) {
                issue.sgId =
                    signalsByPath.value(
                        issueRoot + QStringLiteral(".local_sgid"), nullptr);
            }
            issue.pc =
                signalsByPath.value(issueRoot + QStringLiteral(".PC.pc_"), nullptr);
            issue.issueType = signalsByPath.value(
                issueRoot + QStringLiteral(".preDecode.instIssueType"), nullptr);
            qppu.issueSlots.push_back(issue);
            continue;
        }

        const QRegularExpressionMatch dependencyMatch =
            dependencyField.match(it.key());
        if (dependencyMatch.hasMatch()) {
            const int sg = dependencyMatch.captured(1).toInt();
            qppu.shaderGroups[sg].index = sg;
            qppu.shaderGroups[sg].dependencies.push_back(it.value());
            continue;
        }

        const QRegularExpressionMatch sgMatch = sgField.match(it.key());
        if (sgMatch.hasMatch()) {
            const int sg = sgMatch.captured(1).toInt();
            SgSignals& sgSignals = qppu.shaderGroups[sg];
            sgSignals.index = sg;
            if (it.key().endsWith(QStringLiteral(".valid")) &&
                it.key().contains(QStringLiteral(".sg_table_"))) {
                sgSignals.valid = it.value();
            } else if (it.key().endsWith(QStringLiteral(".m_count")) ||
                       it.key().endsWith(QStringLiteral(".m_numAvail")) ||
                       it.key().endsWith(QStringLiteral(".m_num_readable"))) {
                if (it.key().contains(QStringLiteral(".instr_queue_"))) {
                    sgSignals.queueCount = it.value();
                }
            } else if (it.key().contains(QStringLiteral(".stall_cnt_vector_"))) {
                sgSignals.stall = it.value();
            } else if (it.key().contains(QStringLiteral(".sleep_cnt_vector_"))) {
                sgSignals.sleep = it.value();
            } else if (it.key().contains(
                           QStringLiteral(".flow_ctrl_pend_wait_vector_"))) {
                sgSignals.flow = it.value();
            } else if (it.key().contains(
                           QStringLiteral(".barrier_pend_wait_vector_"))) {
                sgSignals.barrier = it.value();
            } else if (it.key().contains(
                           QStringLiteral(".set_max_temp_pend_wait_vector_"))) {
                sgSignals.setMaxTemp = it.value();
            } else if (it.key().contains(QStringLiteral(".inflight_mem_cnt_"))) {
                sgSignals.inflightMemory = it.value();
            }
            continue;
        }

        const QRegularExpressionMatch functionUnitMatch =
            functionUnitField.match(it.key());
        if (functionUnitMatch.hasMatch()) {
            qppu.functionUnitPending.insert(
                functionUnitMatch.captured(1).toInt(), it.value());
        } else if (it.key().contains(QStringLiteral(".mma_ldMb_credit_cnt_"))) {
            qppu.mmaLoadCredit = it.value();
        } else if (it.key().contains(QStringLiteral(".mma_stMb_credit_cnt_"))) {
            qppu.mmaStoreCredit = it.value();
        } else if (it.key().endsWith(QStringLiteral(".fe_dicache_credit_"))) {
            qppu.icacheCredit = it.value();
        }
    }

    QJsonObject result;
    result.insert(QStringLiteral("status"),
                  qppus.isEmpty() ? QStringLiteral("unavailable")
                                  : QStringLiteral("measured"));
    result.insert(
        QStringLiteral("eligibility_definition"),
        QStringLiteral(
            "局部可发射候选 = SG 有效、指令队列非空，且 Barrier、流控、"
            "Sleep、SetMaxTemp、延迟计数、依赖计数和普通 Thread 功能单元"
            "均未阻塞。操作数读取、写回冲突和最终仲裁未被波形完整暴露，"
            "因此只有实际发射能证明最终可发射。"));
    result.insert(QStringLiteral("qppu_count"), qppus.size());
    if (qppus.isEmpty()) return result;

    const int boundedBinCount =
        qBound(1, timelineBinCount,
               int(qMin<qint64>(qMax<qint64>(1, endTick - startTick),
                                1000)));
    QVector<Bin> bins(boundedBinCount);
    const qint64 durationTicks = endTick - startTick;
    for (int i = 0; i < bins.size(); ++i) {
        bins[i].startTick =
            startTick + (durationTicks * qint64(i)) / bins.size();
        bins[i].endTick =
            startTick + (durationTicks * qint64(i + 1)) / bins.size();
    }

    QMap<QString, qint64> totalBlockTicks;
    qint64 totalActiveSgTicks = 0;
    qint64 totalQueueReadySgTicks = 0;
    qint64 totalEligibleSgTicks = 0;
    qint64 totalIssuedTicks = 0;
    qint64 totalUnknownTicks = 0;
    qint64 totalSgCapacityTicks = 0;
    int activityCoveredSgs = 0;
    int eligibilityCoveredSgs = 0;
    QMap<QString, PcMetrics> pcMetrics;
    qint64 totalIssueInstructionTicks = 0;
    qint64 totalPcIssuedTicks = 0;
    qint64 totalPcTypeKnownTicks = 0;
    qint64 totalQueueWaitTicks = 0;
    qint64 totalQueueHeadCoveredTicks = 0;
    QJsonArray qppuArray;

    for (auto qppuIt = qppus.begin(); qppuIt != qppus.end(); ++qppuIt) {
        QppuSignals& qppu = qppuIt.value();
        QJsonArray sgArray;
        qint64 qppuActive = 0;
        qint64 qppuQueueReady = 0;
        qint64 qppuEligible = 0;
        qint64 qppuIssued = 0;
        int qppuActivityCoveredSgs = 0;
        int qppuEligibilityCoveredSgs = 0;
        QMap<QString, qint64> qppuBlocks;
        const bool issueLayoutCoverageComplete =
            qppu.declaredIssueSlots > 0 &&
            qppu.issueSlots.size() == qppu.declaredIssueSlots;

        for (auto sgIt = qppu.shaderGroups.begin();
             sgIt != qppu.shaderGroups.end(); ++sgIt) {
            const SgSignals& sg = sgIt.value();
            QVector<const WaveSignal*> signalSet;
            appendBoundarySignals(sg, signalSet);
            for (const IssueSignals& issue : qppu.issueSlots) {
                signalSet.push_back(issue.valid);
                signalSet.push_back(issue.sgId);
            }
            for (const WaveSignal* signal : qppu.functionUnitPending) {
                signalSet.push_back(signal);
            }
            const QVector<qint64> edges =
                boundaries(signalSet, startTick, endTick);
            SgMetrics metrics;
            bool allActivityIntervalsCovered = true;
            bool allEligibilityIntervalsCovered = true;
            for (int i = 0; i + 1 < edges.size(); ++i) {
                const qint64 begin = edges.at(i);
                const qint64 end = edges.at(i + 1);
                const qint64 ticks = end - begin;
                bool active = false;
                bool queueReady = false;
                bool eligible = false;
                bool issued = false;
                bool activityCoverage = false;
                bool eligibilityCoverage = false;
                const QString state =
                    localState(sg, qppu.issueSlots,
                               qppu.functionUnitPending,
                               issueLayoutCoverageComplete, begin, active,
                               queueReady, eligible, issued,
                               activityCoverage, eligibilityCoverage);
                metrics.stateTicks[state] += ticks;
                if (active) metrics.activeTicks += ticks;
                if (queueReady) metrics.queueReadyTicks += ticks;
                if (eligible) metrics.eligibleTicks += ticks;
                if (issued) metrics.issuedTicks += ticks;
                if (state == QStringLiteral("unknown")) metrics.unknownTicks += ticks;
                if (state != QStringLiteral("inactive") &&
                    state != QStringLiteral("frontend_empty") &&
                    state != QStringLiteral("eligible") &&
                    state != QStringLiteral("issued") &&
                    state != QStringLiteral("unknown")) {
                    metrics.blockTicks[state] += ticks;
                }
                if (queueReady && !issued &&
                    state != QStringLiteral("unknown")) {
                    totalQueueWaitTicks += ticks;
                    quint64 headPc = 0;
                    quint64 headIssueType = 0;
                    bool headIssueTypeKnown = false;
                    if (queueHeadState(sg, begin, headPc,
                                       headIssueType,
                                       headIssueTypeKnown)) {
                        totalQueueHeadCoveredTicks += ticks;
                        const QString key =
                            qppu.path + QLatin1Char(':') +
                            QString::number(headPc) + QLatin1Char(':') +
                            (headIssueTypeKnown
                                 ? QString::number(headIssueType)
                                 : QStringLiteral("unknown"));
                        PcMetrics& wait = pcMetrics[key];
                        wait.qppuPath = qppu.path;
                        wait.pc = headPc;
                        wait.issueType = headIssueType;
                        wait.issueTypeKnown = headIssueTypeKnown;
                        wait.waitTicks += ticks;
                        wait.waitReasonTicks[state] += ticks;
                    }
                }
                allActivityIntervalsCovered =
                    allActivityIntervalsCovered && activityCoverage;
                allEligibilityIntervalsCovered =
                    allEligibilityIntervalsCovered && eligibilityCoverage;

                const State inflight = stateAt(sg.inflightMemory, begin);
                if (inflight.known) {
                    metrics.inflightMemoryKnownTicks += ticks;
                    metrics.inflightMemoryWeightedTicks +=
                        static_cast<long double>(inflight.value) *
                        static_cast<long double>(ticks);
                    metrics.inflightMemoryMax =
                        qMax(metrics.inflightMemoryMax, inflight.value);
                    if (inflight.value > 0) metrics.inflightMemoryTicks += ticks;
                }
                addToBins(bins, begin, end, state, active, queueReady,
                          eligible, issued);
            }

            if (allActivityIntervalsCovered) {
                ++activityCoveredSgs;
                ++qppuActivityCoveredSgs;
            }
            if (allEligibilityIntervalsCovered) {
                ++eligibilityCoveredSgs;
                ++qppuEligibilityCoveredSgs;
            }
            totalSgCapacityTicks += durationTicks;
            totalActiveSgTicks += metrics.activeTicks;
            totalQueueReadySgTicks += metrics.queueReadyTicks;
            totalEligibleSgTicks += metrics.eligibleTicks;
            totalIssuedTicks += metrics.issuedTicks;
            totalUnknownTicks += metrics.unknownTicks;
            qppuActive += metrics.activeTicks;
            qppuQueueReady += metrics.queueReadyTicks;
            qppuEligible += metrics.eligibleTicks;
            qppuIssued += metrics.issuedTicks;
            for (auto it = metrics.blockTicks.constBegin();
                 it != metrics.blockTicks.constEnd(); ++it) {
                totalBlockTicks[it.key()] += it.value();
                qppuBlocks[it.key()] += it.value();
            }

            QJsonObject sgObject;
            sgObject.insert(QStringLiteral("index"), sg.index);
            sgObject.insert(QStringLiteral("active_cycles"),
                            cycles(metrics.activeTicks, ticksPerCycle));
            sgObject.insert(QStringLiteral("queue_ready_cycles"),
                            cycles(metrics.queueReadyTicks, ticksPerCycle));
            sgObject.insert(QStringLiteral("eligible_cycles"),
                            cycles(metrics.eligibleTicks, ticksPerCycle));
            sgObject.insert(QStringLiteral("issued_cycles"),
                            cycles(metrics.issuedTicks, ticksPerCycle));
            sgObject.insert(QStringLiteral("unknown_cycles"),
                            cycles(metrics.unknownTicks, ticksPerCycle));
            sgObject.insert(QStringLiteral("queue_ready_percent"),
                            percent(metrics.queueReadyTicks,
                                    metrics.activeTicks));
            sgObject.insert(QStringLiteral("eligible_percent"),
                            percent(metrics.eligibleTicks,
                                    metrics.queueReadyTicks));
            sgObject.insert(QStringLiteral("activity_coverage_complete"),
                            allActivityIntervalsCovered);
            sgObject.insert(QStringLiteral("eligibility_coverage_complete"),
                            allEligibilityIntervalsCovered);
            sgObject.insert(QStringLiteral("coverage_complete"),
                            allEligibilityIntervalsCovered);
            QJsonArray blocks;
            for (auto it = metrics.blockTicks.constBegin();
                 it != metrics.blockTicks.constEnd(); ++it) {
                QJsonObject block;
                block.insert(QStringLiteral("key"), it.key());
                block.insert(QStringLiteral("name"), reasonName(it.key()));
                block.insert(QStringLiteral("cycles"),
                             cycles(it.value(), ticksPerCycle));
                block.insert(QStringLiteral("queue_ready_percent"),
                             percent(it.value(), metrics.queueReadyTicks));
                blocks.push_back(block);
            }
            sgObject.insert(QStringLiteral("block_reasons"), blocks);
            QJsonObject inflight;
            inflight.insert(QStringLiteral("active_cycles"),
                            cycles(metrics.inflightMemoryTicks, ticksPerCycle));
            const bool inflightCoverageComplete =
                durationTicks > 0 &&
                metrics.inflightMemoryKnownTicks == durationTicks;
            inflight.insert(QStringLiteral("coverage_complete"),
                            inflightCoverageComplete);
            inflight.insert(
                QStringLiteral("observed_cycles"),
                cycles(metrics.inflightMemoryKnownTicks, ticksPerCycle));
            if (inflightCoverageComplete) {
                inflight.insert(
                    QStringLiteral("average"),
                    double(metrics.inflightMemoryWeightedTicks /
                           static_cast<long double>(durationTicks)));
            }
            inflight.insert(QStringLiteral("max"),
                            QString::number(metrics.inflightMemoryMax));
            sgObject.insert(QStringLiteral("inflight_memory"), inflight);
            sgArray.push_back(sgObject);
        }

        QVector<const WaveSignal*> issueBoundarySignals;
        for (const IssueSignals& issue : qppu.issueSlots) {
            issueBoundarySignals.push_back(issue.valid);
            issueBoundarySignals.push_back(issue.pc);
            issueBoundarySignals.push_back(issue.issueType);
        }
        const QVector<qint64> issueEdges =
            boundaries(issueBoundarySignals, startTick, endTick);
        qint64 qppuIssueTicks = 0;
        qint64 qppuIssueActiveTicks = 0;
        qint64 qppuIssueObservedTicks = 0;
        qint64 qppuDualIssueTicks = 0;
        for (int i = 0; i + 1 < issueEdges.size(); ++i) {
            const qint64 begin = issueEdges.at(i);
            const qint64 ticks = issueEdges.at(i + 1) - begin;
            int activeSlots = 0;
            bool allSlotsKnown = issueLayoutCoverageComplete;
            QVector<QPair<State, State>> activeInstructions;
            for (const IssueSignals& issue : qppu.issueSlots) {
                bool validKnown = false;
                const bool issueValid =
                    positive(issue.valid, begin, validKnown);
                allSlotsKnown = allSlotsKnown && validKnown;
                if (!issueValid) continue;
                ++activeSlots;
                const State pc = stateAt(issue.pc, begin);
                const State type = stateAt(issue.issueType, begin);
                activeInstructions.push_back({pc, type});
            }
            if (!allSlotsKnown) continue;
            qppuIssueObservedTicks += ticks;
            qppuIssueTicks += qint64(activeSlots) * ticks;
            if (activeSlots > 0) qppuIssueActiveTicks += ticks;
            if (activeSlots > 1) qppuDualIssueTicks += ticks;
            for (const QPair<State, State>& instruction :
                 activeInstructions) {
                const State& pc = instruction.first;
                const State& type = instruction.second;
                if (!pc.known) continue;
                const QString key =
                    qppu.path + QLatin1Char(':') + QString::number(pc.value) +
                    QLatin1Char(':') +
                    (type.known
                         ? QString::number(type.value)
                         : QStringLiteral("unknown"));
                PcMetrics& hotspot = pcMetrics[key];
                hotspot.qppuPath = qppu.path;
                hotspot.pc = pc.value;
                hotspot.issueType = type.value;
                hotspot.issueTypeKnown = type.known;
                hotspot.issuedTicks += ticks;
                totalPcIssuedTicks += ticks;
                if (type.known) totalPcTypeKnownTicks += ticks;
            }
        }
        totalIssueInstructionTicks += qppuIssueTicks;
        const bool issueCoverageComplete =
            issueLayoutCoverageComplete &&
            qppuIssueObservedTicks == durationTicks;

        QJsonObject qppuObject;
        qppuObject.insert(QStringLiteral("path"), qppu.path);
        qppuObject.insert(QStringLiteral("index"), qppu.index);
        qppuObject.insert(QStringLiteral("shader_groups"), sgArray);
        qppuObject.insert(QStringLiteral("issue_slots_declared"),
                          qppu.declaredIssueSlots);
        qppuObject.insert(QStringLiteral("issue_slots_traced"),
                          qppu.issueSlots.size());
        qppuObject.insert(QStringLiteral("issue_coverage_complete"),
                          issueCoverageComplete);
        qppuObject.insert(
            QStringLiteral("activity_coverage_complete"),
            !qppu.shaderGroups.isEmpty() &&
                qppuActivityCoveredSgs == qppu.shaderGroups.size());
        qppuObject.insert(
            QStringLiteral("eligibility_coverage_complete"),
            !qppu.shaderGroups.isEmpty() &&
                qppuEligibilityCoveredSgs == qppu.shaderGroups.size());
        qppuObject.insert(QStringLiteral("active_sg_cycles"),
                          cycles(qppuActive, ticksPerCycle));
        qppuObject.insert(QStringLiteral("queue_ready_sg_cycles"),
                          cycles(qppuQueueReady, ticksPerCycle));
        qppuObject.insert(QStringLiteral("eligible_sg_cycles"),
                          cycles(qppuEligible, ticksPerCycle));
        qppuObject.insert(QStringLiteral("issued_sg_cycles"),
                          cycles(qppuIssued, ticksPerCycle));
        qppuObject.insert(QStringLiteral("issued_instructions_estimate"),
                          cycles(qppuIssueTicks, ticksPerCycle));
        qppuObject.insert(QStringLiteral("issue_utilization_percent"),
                          percent(qppuIssueTicks,
                                  qppuIssueObservedTicks));
        qppuObject.insert(QStringLiteral("issue_active_cycles"),
                          cycles(qppuIssueActiveTicks, ticksPerCycle));
        qppuObject.insert(
            QStringLiteral("issue_idle_cycles"),
            cycles(qMax<qint64>(0, qppuIssueObservedTicks -
                                      qppuIssueActiveTicks),
                   ticksPerCycle));
        qppuObject.insert(QStringLiteral("issue_observed_cycles"),
                          cycles(qppuIssueObservedTicks, ticksPerCycle));
        qppuObject.insert(QStringLiteral("issue_active_percent"),
                          percent(qppuIssueActiveTicks,
                                  qppuIssueObservedTicks));
        qppuObject.insert(QStringLiteral("dual_issue_cycles"),
                          cycles(qppuDualIssueTicks, ticksPerCycle));
        qppuObject.insert(QStringLiteral("eligible_percent"),
                          percent(qppuEligible, qppuQueueReady));
        QJsonArray blockArray;
        for (auto it = qppuBlocks.constBegin(); it != qppuBlocks.constEnd(); ++it) {
            QJsonObject block;
            block.insert(QStringLiteral("key"), it.key());
            block.insert(QStringLiteral("name"), reasonName(it.key()));
            block.insert(QStringLiteral("cycles"),
                         cycles(it.value(), ticksPerCycle));
            block.insert(QStringLiteral("queue_ready_percent"),
                         percent(it.value(), qppuQueueReady));
            blockArray.push_back(block);
        }
        qppuObject.insert(QStringLiteral("block_reasons"), blockArray);
        qppuArray.push_back(qppuObject);
    }

    const int shaderGroupCount =
        int(totalSgCapacityTicks / qMax<qint64>(1, durationTicks));
    QJsonObject summary;
    summary.insert(QStringLiteral("shader_groups"),
                   int(totalSgCapacityTicks / qMax<qint64>(1, durationTicks)));
    summary.insert(QStringLiteral("activity_covered_shader_groups"),
                   activityCoveredSgs);
    summary.insert(QStringLiteral("fully_covered_shader_groups"),
                   eligibilityCoveredSgs);
    summary.insert(QStringLiteral("activity_coverage_complete"),
                   shaderGroupCount > 0 &&
                       activityCoveredSgs == shaderGroupCount);
    summary.insert(QStringLiteral("eligibility_coverage_complete"),
                   shaderGroupCount > 0 &&
                       eligibilityCoveredSgs == shaderGroupCount);
    summary.insert(QStringLiteral("active_sg_cycles"),
                   cycles(totalActiveSgTicks, ticksPerCycle));
    summary.insert(QStringLiteral("queue_ready_sg_cycles"),
                   cycles(totalQueueReadySgTicks, ticksPerCycle));
    summary.insert(QStringLiteral("eligible_sg_cycles"),
                   cycles(totalEligibleSgTicks, ticksPerCycle));
    summary.insert(QStringLiteral("issued_sg_cycles"),
                   cycles(totalIssuedTicks, ticksPerCycle));
    summary.insert(QStringLiteral("unknown_sg_cycles"),
                   cycles(totalUnknownTicks, ticksPerCycle));
    summary.insert(QStringLiteral("active_percent"),
                   percent(totalActiveSgTicks, totalSgCapacityTicks));
    summary.insert(QStringLiteral("queue_ready_percent"),
                   percent(totalQueueReadySgTicks, totalActiveSgTicks));
    summary.insert(QStringLiteral("eligible_percent"),
                   percent(totalEligibleSgTicks, totalQueueReadySgTicks));
    summary.insert(QStringLiteral("pc_wait_cycles"),
                   cycles(totalQueueWaitTicks, ticksPerCycle));
    summary.insert(QStringLiteral("pc_wait_covered_cycles"),
                   cycles(totalQueueHeadCoveredTicks, ticksPerCycle));
    summary.insert(QStringLiteral("pc_wait_coverage_percent"),
                   percent(totalQueueHeadCoveredTicks,
                           totalQueueWaitTicks));
    summary.insert(QStringLiteral("pc_issue_coverage_percent"),
                   percent(totalPcIssuedTicks,
                           totalIssueInstructionTicks));
    summary.insert(QStringLiteral("pc_issue_type_coverage_percent"),
                   percent(totalPcTypeKnownTicks,
                           totalIssueInstructionTicks));
    QJsonArray totalBlocks;
    QVector<QPair<QString, qint64>> sortedBlocks;
    for (auto it = totalBlockTicks.constBegin();
         it != totalBlockTicks.constEnd(); ++it) {
        sortedBlocks.push_back(qMakePair(it.key(), it.value()));
    }
    std::sort(sortedBlocks.begin(), sortedBlocks.end(),
              [](const QPair<QString, qint64>& left,
                 const QPair<QString, qint64>& right) {
                  return left.second != right.second
                             ? left.second > right.second
                             : left.first < right.first;
              });
    for (const auto& item : sortedBlocks) {
        QJsonObject block;
        block.insert(QStringLiteral("key"), item.first);
        block.insert(QStringLiteral("name"), reasonName(item.first));
        block.insert(QStringLiteral("cycles"),
                     cycles(item.second, ticksPerCycle));
        block.insert(QStringLiteral("queue_ready_percent"),
                     percent(item.second, totalQueueReadySgTicks));
        totalBlocks.push_back(block);
    }
    summary.insert(QStringLiteral("block_reasons"), totalBlocks);
    result.insert(QStringLiteral("summary"), summary);
    result.insert(QStringLiteral("qppus"), qppuArray);
    if (totalSgCapacityTicks == 0) {
        result.insert(QStringLiteral("status"), QStringLiteral("issue_only"));
    } else if (eligibilityCoveredSgs != shaderGroupCount) {
        result.insert(QStringLiteral("status"), QStringLiteral("partial"));
    } else {
        result.insert(QStringLiteral("status"), QStringLiteral("measured"));
    }

    QJsonArray timeline;
    for (const Bin& bin : bins) {
        QJsonObject object;
        object.insert(QStringLiteral("start_cycle"),
                      cycles(bin.startTick, ticksPerCycle));
        object.insert(QStringLiteral("end_cycle"),
                      cycles(bin.endTick, ticksPerCycle));
        object.insert(QStringLiteral("active_sg_cycles"),
                      cycles(bin.activeSgTicks, ticksPerCycle));
        object.insert(QStringLiteral("queue_ready_sg_cycles"),
                      cycles(bin.queueReadySgTicks, ticksPerCycle));
        object.insert(QStringLiteral("eligible_sg_cycles"),
                      cycles(bin.eligibleSgTicks, ticksPerCycle));
        object.insert(QStringLiteral("issued_sg_cycles"),
                      cycles(bin.issuedTicks, ticksPerCycle));
        QJsonObject blocks;
        for (auto it = bin.blockTicks.constBegin();
             it != bin.blockTicks.constEnd(); ++it) {
            blocks.insert(it.key(), cycles(it.value(), ticksPerCycle));
        }
        object.insert(QStringLiteral("block_cycles"), blocks);
        timeline.push_back(object);
    }
    result.insert(QStringLiteral("timeline"), timeline);

    QVector<PcMetrics> hotspots;
    for (auto it = pcMetrics.constBegin(); it != pcMetrics.constEnd(); ++it) {
        hotspots.push_back(it.value());
    }
    std::sort(hotspots.begin(), hotspots.end(),
              [](const PcMetrics& left, const PcMetrics& right) {
                  if (left.issuedTicks != right.issuedTicks) {
                      return left.issuedTicks > right.issuedTicks;
                  }
                  if (left.qppuPath != right.qppuPath) {
                      return left.qppuPath < right.qppuPath;
                  }
                  return left.pc < right.pc;
              });
    QJsonArray hotspotArray;
    for (const PcMetrics& hotspot : hotspots) {
        if (hotspot.issuedTicks <= 0 || hotspotArray.size() >= 200) continue;
        QJsonObject object;
        object.insert(QStringLiteral("qppu_path"), hotspot.qppuPath);
        object.insert(QStringLiteral("pc"),
                      QStringLiteral("0x%1").arg(hotspot.pc, 0, 16));
        object.insert(QStringLiteral("issue_type"),
                      hotspot.issueTypeKnown
                          ? QString::number(hotspot.issueType)
                          : QStringLiteral("unknown"));
        object.insert(QStringLiteral("issued_instructions_estimate"),
                      cycles(hotspot.issuedTicks, ticksPerCycle));
        object.insert(QStringLiteral("share_percent"),
                      percent(hotspot.issuedTicks, totalPcIssuedTicks));
        object.insert(QStringLiteral("queue_wait_cycles"),
                      cycles(hotspot.waitTicks, ticksPerCycle));
        hotspotArray.push_back(object);
    }
    result.insert(QStringLiteral("pc_hotspots"), hotspotArray);

    std::sort(hotspots.begin(), hotspots.end(),
              [](const PcMetrics& left, const PcMetrics& right) {
                  if (left.waitTicks != right.waitTicks) {
                      return left.waitTicks > right.waitTicks;
                  }
                  if (left.qppuPath != right.qppuPath) {
                      return left.qppuPath < right.qppuPath;
                  }
                  return left.pc < right.pc;
              });
    QJsonArray waitHotspotArray;
    for (const PcMetrics& hotspot : hotspots) {
        if (hotspot.waitTicks <= 0 || waitHotspotArray.size() >= 200) continue;
        QString dominantReason;
        qint64 dominantTicks = 0;
        for (auto it = hotspot.waitReasonTicks.constBegin();
             it != hotspot.waitReasonTicks.constEnd(); ++it) {
            if (it.value() > dominantTicks) {
                dominantReason = it.key();
                dominantTicks = it.value();
            }
        }
        QJsonObject object;
        object.insert(QStringLiteral("qppu_path"), hotspot.qppuPath);
        object.insert(QStringLiteral("pc"),
                      QStringLiteral("0x%1").arg(hotspot.pc, 0, 16));
        object.insert(QStringLiteral("issue_type"),
                      hotspot.issueTypeKnown
                          ? QString::number(hotspot.issueType)
                          : QStringLiteral("unknown"));
        object.insert(QStringLiteral("wait_cycles"),
                      cycles(hotspot.waitTicks, ticksPerCycle));
        object.insert(QStringLiteral("share_percent"),
                      percent(hotspot.waitTicks,
                              totalQueueHeadCoveredTicks));
        object.insert(QStringLiteral("issued_instructions_estimate"),
                      cycles(hotspot.issuedTicks, ticksPerCycle));
        object.insert(QStringLiteral("dominant_reason_key"),
                      dominantReason);
        object.insert(QStringLiteral("dominant_reason"),
                      reasonName(dominantReason));
        object.insert(QStringLiteral("dominant_reason_cycles"),
                      cycles(dominantTicks, ticksPerCycle));
        waitHotspotArray.push_back(object);
    }
    result.insert(QStringLiteral("pc_wait_hotspots"), waitHotspotArray);

    if (eligibilityCoveredSgs != shaderGroupCount &&
        totalSgCapacityTicks > 0) {
        warnings.push_back(
            QStringLiteral(
                "调度器只有 %1/%2 个 SG 具备完整的已建模阻塞条件覆盖；"
                "Active、Queue Ready 和实际发射仍可用，Eligible 候选只统计"
                "可证明清除已建模阻塞的周期。")
                .arg(eligibilityCoveredSgs)
                .arg(shaderGroupCount));
    }
    return result;
}

bool schedulerProfilerSelfTest(QString& error) {
    QVector<WaveSignal> storage;
    QHash<QString, const WaveSignal*> signalMap;
    auto add = [&](const QString& path,
                   int width,
                   const QVector<QPair<qint64, quint64>>& samples) {
        WaveSignal signal;
        signal.signalId = storage.size();
        signal.width = width;
        signal.kind = width == 1 ? SignalKind::Bit : SignalKind::Bus;
        for (const auto& pair : samples) {
            WaveSample sample;
            sample.time = pair.first;
            sample.rawBits = pair.second;
            sample.rawFieldsReady = true;
            signal.samples.push_back(sample);
        }
        storage.push_back(signal);
        signalMap.insert(path, &storage.last());
    };
    storage.reserve(32);
    const QString root =
        QStringLiteral("gpu.m_QPPUTOP[size=1].[0].m_QPPUCtrl.");
    add(root + QStringLiteral("sg_table_[size=16].[0].valid"),
        1, {{0, 1}});
    add(root + QStringLiteral("instr_queue_[size=16].[0].m_count"),
        4, {{0, 1}, {40, 0}});
    add(root + QStringLiteral("instr_queue_[size=16].[0].m_ri"),
        2, {{0, 0}});
    add(root + QStringLiteral(
                   "instr_queue_[size=16].[0].m_QData[size=4].[0]."
                   "Data.PC.pc_"),
        32, {{0, 0x100}});
    add(root + QStringLiteral(
                   "instr_queue_[size=16].[0].m_QData[size=4].[0]."
                   "Data.preDecode.instIssueType"),
        4, {{0, 2}});
    add(root + QStringLiteral(
                   "instr_queue_[size=16].[0].m_QData[size=4].[0]."
                   "Data.preDecode.instType.subType.thread"),
        4, {{0, 1}});
    add(root + QStringLiteral(
                   "instr_queue_[size=16].[0].m_QData[size=4].[0]."
                   "Data.preDecode.exeThdUnit"),
        4, {{0, 0}});
    add(root + QStringLiteral("stall_cnt_vector_[size=16].[0]"),
        4, {{0, 0}, {20, 1}, {30, 0}});
    add(root + QStringLiteral("sleep_cnt_vector_[size=16].[0]"),
        4, {{0, 0}});
    add(root + QStringLiteral("flow_ctrl_pend_wait_vector_[size=16].[0]"),
        1, {{0, 0}});
    add(root + QStringLiteral("barrier_pend_wait_vector_[size=16].[0]"),
        1, {{0, 0}});
    add(root + QStringLiteral("set_max_temp_pend_wait_vector_[size=16].[0]"),
        1, {{0, 0}});
    for (int dep = 0; dep < 7; ++dep) {
        add(root +
                QStringLiteral(
                    "check_dep_cnt_vector_[size=16].[0][size=7].[%1]")
                    .arg(dep),
            4, {{0, 0}});
    }
    add(root + QStringLiteral("issue_inst_[size=2].[0].vld"),
        1, {{0, 1}, {10, 0}});
    add(root + QStringLiteral("issue_inst_[size=2].[0].sgId"),
        4, {{0, 0}});
    add(root + QStringLiteral("issue_inst_[size=2].[0].PC.pc_"),
        32, {{0, 0x100}});
    add(root +
            QStringLiteral(
                "issue_inst_[size=2].[0].preDecode.instIssueType"),
        4, {{0, 2}});
    add(root + QStringLiteral("issue_inst_[size=2].[1].vld"),
        1, {{0, 0}});
    add(root +
            QStringLiteral(
                "function_unit_pend_wait_vector_[size=3].[0]"),
        4, {{0, 0}, {10, 1}, {20, 0}});

    QStringList warnings;
    const QJsonObject profile =
        buildSchedulerProfile(signalMap, 0, 50, 10, 5, warnings);
    const QJsonObject summary =
        profile.value(QStringLiteral("summary")).toObject();
    const QJsonObject qppu =
        profile.value(QStringLiteral("qppus")).toArray().first().toObject();
    if (profile.value(QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        std::fabs(summary.value(QStringLiteral("active_sg_cycles")).toDouble() -
                  5.0) > 1e-9 ||
        std::fabs(summary.value(QStringLiteral("queue_ready_sg_cycles")).toDouble() -
                  4.0) > 1e-9 ||
        std::fabs(summary.value(QStringLiteral("issued_sg_cycles")).toDouble() -
                  1.0) > 1e-9 ||
        std::fabs(
            summary.value(QStringLiteral("pc_wait_cycles")).toDouble() -
            3.0) > 1e-9 ||
        std::fabs(
            summary.value(
                QStringLiteral("pc_wait_coverage_percent")).toDouble() -
            100.0) > 1e-9 ||
        summary.value(
            QStringLiteral("pc_issue_coverage_percent")).toDouble() !=
            100.0 ||
        summary.value(
            QStringLiteral("pc_issue_type_coverage_percent")).toDouble() !=
            100.0 ||
        !qppu.value(
            QStringLiteral("issue_coverage_complete")).toBool() ||
        qppu.value(
            QStringLiteral("issue_utilization_percent")).toDouble() != 20.0) {
        error = QStringLiteral("scheduler interval accounting mismatch");
        return false;
    }
    const QJsonArray blocks =
        summary.value(QStringLiteral("block_reasons")).toArray();
    QSet<QString> blockKeys;
    for (const QJsonValue& value : blocks) {
        blockKeys.insert(
            value.toObject().value(QStringLiteral("key")).toString());
    }
    if (!blockKeys.contains(QStringLiteral("stall_count")) ||
        !blockKeys.contains(QStringLiteral("function_unit"))) {
        error = QStringLiteral("scheduler block classification mismatch");
        return false;
    }
    const QJsonArray waitHotspots =
        profile.value(QStringLiteral("pc_wait_hotspots")).toArray();
    if (waitHotspots.isEmpty() ||
        waitHotspots.first().toObject()
                .value(QStringLiteral("pc")).toString() !=
            QStringLiteral("0x100") ||
        std::fabs(waitHotspots.first().toObject()
                          .value(QStringLiteral("wait_cycles")).toDouble() -
                  3.0) > 1e-9) {
        error = QStringLiteral("scheduler PC wait attribution mismatch");
        return false;
    }

    QHash<QString, const WaveSignal*> partialMap = signalMap;
    partialMap.remove(root + QStringLiteral("issue_inst_[size=2].[1].vld"));
    QStringList partialWarnings;
    const QJsonObject partial =
        buildSchedulerProfile(partialMap, 0, 50, 10, 5,
                              partialWarnings);
    if (partial.value(QStringLiteral("status")).toString() !=
            QStringLiteral("partial") ||
        partial.value(QStringLiteral("summary")).toObject()
                .value(QStringLiteral("fully_covered_shader_groups"))
                .toInt() != 0 ||
        partial.value(QStringLiteral("qppus")).toArray().first().toObject()
                .value(QStringLiteral("issue_coverage_complete")).toBool()) {
        error = QStringLiteral("partial issue-slot coverage was misclassified");
        return false;
    }

    QVector<WaveSignal> unknownStorage = storage;
    const QString lateSlotPath =
        root + QStringLiteral("issue_inst_[size=2].[1].vld");
    const int lateSlotId = signalMap.value(lateSlotPath)->signalId;
    unknownStorage[lateSlotId].samples[0].time = 10;
    const QString slot0Path =
        root + QStringLiteral("issue_inst_[size=2].[0].vld");
    const int slot0Id = signalMap.value(slot0Path)->signalId;
    unknownStorage[slot0Id].samples[0].rawBits = 0;
    QHash<QString, const WaveSignal*> unknownMap;
    for (auto it = signalMap.constBegin(); it != signalMap.constEnd(); ++it) {
        unknownMap.insert(it.key(), &unknownStorage.at(it.value()->signalId));
    }
    QStringList unknownWarnings;
    const QJsonObject unknown =
        buildSchedulerProfile(unknownMap, 0, 50, 10, 5,
                              unknownWarnings);
    const QJsonObject unknownQppu =
        unknown.value(QStringLiteral("qppus")).toArray().first().toObject();
    const QJsonObject unknownSummary =
        unknown.value(QStringLiteral("summary")).toObject();
    if (unknownQppu.value(
            QStringLiteral("issue_coverage_complete")).toBool() ||
        unknownQppu.value(
            QStringLiteral("issue_observed_cycles")).toDouble() != 4.0 ||
        unknownQppu.value(
            QStringLiteral("issued_instructions_estimate")).toDouble() !=
            0.0 ||
        unknownQppu.value(
            QStringLiteral("issue_idle_cycles")).toDouble() != 4.0) {
        error = QStringLiteral(
            "unknown issue slot was counted as idle or issued");
        return false;
    }
    if (unknownSummary.value(
            QStringLiteral("unknown_sg_cycles")).toDouble() != 1.0 ||
        unknownSummary.value(
            QStringLiteral("pc_wait_cycles")).toDouble() != 3.0) {
        error = QStringLiteral(
            "unknown issue attribution was counted as queue wait");
        return false;
    }

    QHash<QString, const WaveSignal*> partialDependencyMap = signalMap;
    for (int dep = 1; dep < 7; ++dep) {
        partialDependencyMap.remove(
            root +
            QStringLiteral(
                "check_dep_cnt_vector_[size=16].[0][size=7].[%1]")
                .arg(dep));
    }
    QStringList dependencyWarnings;
    const QJsonObject partialDependencies =
        buildSchedulerProfile(partialDependencyMap, 0, 50, 10, 5,
                              dependencyWarnings);
    const QJsonObject partialDependencySg =
        partialDependencies.value(QStringLiteral("qppus")).toArray()
            .first().toObject()
            .value(QStringLiteral("shader_groups")).toArray()
            .first().toObject();
    if (partialDependencySg.value(
            QStringLiteral("eligibility_coverage_complete")).toBool()) {
        error = QStringLiteral(
            "partial dependency-vector coverage was accepted");
        return false;
    }

    QVector<WaveSignal> unknownTypeStorage = storage;
    const QString issueTypePath =
        root +
        QStringLiteral(
            "issue_inst_[size=2].[0].preDecode.instIssueType");
    const int issueTypeId = signalMap.value(issueTypePath)->signalId;
    unknownTypeStorage[issueTypeId].samples[0].time = 10;
    QHash<QString, const WaveSignal*> unknownTypeMap;
    for (auto it = signalMap.constBegin(); it != signalMap.constEnd(); ++it) {
        unknownTypeMap.insert(
            it.key(), &unknownTypeStorage.at(it.value()->signalId));
    }
    QStringList unknownTypeWarnings;
    const QJsonObject unknownType =
        buildSchedulerProfile(unknownTypeMap, 0, 50, 10, 5,
                              unknownTypeWarnings);
    const QJsonObject unknownTypeSummary =
        unknownType.value(QStringLiteral("summary")).toObject();
    const QJsonArray unknownTypeHotspots =
        unknownType.value(QStringLiteral("pc_hotspots")).toArray();
    if (unknownTypeSummary.value(
            QStringLiteral("pc_issue_coverage_percent")).toDouble() !=
            100.0 ||
        unknownTypeSummary.value(
            QStringLiteral("pc_issue_type_coverage_percent")).toDouble() !=
            0.0 ||
        unknownTypeHotspots.isEmpty() ||
        unknownTypeHotspots.first().toObject()
                .value(QStringLiteral("issue_type")).toString() !=
            QStringLiteral("unknown")) {
        error = QStringLiteral(
            "unknown issue type was merged with a numeric type");
        return false;
    }
    return true;
}

}  // namespace waveperf
