#include "WavePerfDiagnosis.h"

#include <QHash>
#include <QJsonValue>

#include <algorithm>
#include <limits>

namespace waveperf {
namespace {

struct ModuleInfo {
    QString key;
    QString name;
};

struct PressureEvidence {
    QString kind;
    QString path;
    ModuleInfo module;
    double rate = 0.0;
    double cycles = 0.0;
};

int severityRank(const QString& severity) {
    if (severity == QStringLiteral("critical")) return 3;
    if (severity == QStringLiteral("warning")) return 2;
    return 1;
}

QJsonObject finding(const QString& severity,
                    const QString& key,
                    const QString& title,
                    const QString& conclusion,
                    const QString& evidence,
                    const QString& nextStep,
                    int evidenceScore = 0) {
    QJsonObject object;
    object.insert(QStringLiteral("severity"), severity);
    object.insert(QStringLiteral("key"), key);
    object.insert(QStringLiteral("title"), title);
    object.insert(QStringLiteral("conclusion"), conclusion);
    object.insert(QStringLiteral("evidence"), evidence);
    object.insert(QStringLiteral("next_step"), nextStep);
    object.insert(QStringLiteral("priority_score"),
                  severityRank(severity) * 1000 + evidenceScore);
    return object;
}

QJsonArray sortedFindings(const QJsonArray& findings) {
    QVector<QJsonObject> ordered;
    ordered.reserve(findings.size());
    for (const QJsonValue& value : findings) {
        ordered.push_back(value.toObject());
    }
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const QJsonObject& left, const QJsonObject& right) {
            return left.value(QStringLiteral("priority_score")).toInt() >
                   right.value(QStringLiteral("priority_score")).toInt();
        });
    QJsonArray result;
    for (const QJsonObject& object : ordered) result.push_back(object);
    return result;
}

void inspectPressureTop(const QJsonObject& section,
                        const QString& rateKey,
                        const QString& kind,
                        double& highestRate,
                        QString& highestPath,
                        QString& highestKind) {
    const QJsonArray top = section.value(QStringLiteral("top")).toArray();
    for (const QJsonValue& value : top) {
        const QJsonObject object = value.toObject();
        const double rate = object.value(rateKey).toDouble();
        if (rate <= highestRate) continue;
        highestRate = rate;
        highestPath = object.value(QStringLiteral("path")).toString();
        highestKind = kind;
    }
}

ModuleInfo moduleForPath(const QString& path) {
    const QString lower = path.toLower();
    if (lower.contains(QStringLiteral(".m_cbctrl"))) {
        return {QStringLiteral("cb"), QStringLiteral("CB")};
    }
    if (lower.contains(QStringLiteral(".m_ppuscontrol")) ||
        lower.contains(QStringLiteral(".m_ppusdata"))) {
        return {QStringLiteral("ppus"), QStringLiteral("PPUS")};
    }
    if (lower.contains(QStringLiteral(".m_qppumma"))) {
        return {QStringLiteral("mma"), QStringLiteral("MMA")};
    }
    if (lower.contains(QStringLiteral(".m_qppugrpcore"))) {
        return {QStringLiteral("group_be"), QStringLiteral("Group BE")};
    }
    if (lower.contains(QStringLiteral(".m_qppueu")) ||
        lower.contains(QStringLiteral(".m_eu"))) {
        return {QStringLiteral("qppu_eu"), QStringLiteral("EU/BE")};
    }
    if (lower.contains(QStringLiteral(".m_qppuctrl"))) {
        if (lower.contains(QStringLiteral("instr_queue")) ||
            lower.contains(QStringLiteral("icache")) ||
            lower.contains(QStringLiteral("dicache")) ||
            lower.contains(QStringLiteral("miss_req"))) {
            return {QStringLiteral("fe"), QStringLiteral("FE")};
        }
        return {QStringLiteral("qppu_ctrl"), QStringLiteral("QPPUCtrl")};
    }
    if (lower.contains(QStringLiteral(".m_csbg"))) {
        return {QStringLiteral("csbg"), QStringLiteral("CSBG")};
    }
    if (lower.contains(QStringLiteral(".m_pputac"))) {
        return {QStringLiteral("tac"), QStringLiteral("TAC")};
    }
    return {QStringLiteral("unknown"), QStringLiteral("未定位模块")};
}

bool belongsToScope(const QString& path, const QString& scope) {
    return !scope.isEmpty() &&
           (path == scope || path.startsWith(scope + QLatin1Char('.')));
}

void inspectScopedPressure(const QJsonObject& section,
                           const QString& kind,
                           const QString& rateKey,
                           const QString& cyclesKey,
                           const QString& scope,
                           PressureEvidence& best) {
    const QJsonArray top = section.value(QStringLiteral("top")).toArray();
    for (const QJsonValue& value : top) {
        const QJsonObject object = value.toObject();
        const QString path =
            object.value(QStringLiteral("path")).toString();
        if (!belongsToScope(path, scope)) continue;
        const double rate = object.value(rateKey).toDouble();
        if (rate <= best.rate) continue;
        best.kind = kind;
        best.path = path;
        best.module = moduleForPath(path);
        best.rate = rate;
        best.cycles = object.value(cyclesKey).toDouble();
    }
}

PressureEvidence pressureForScope(const QJsonObject& resourcePressure,
                                  const QString& scope) {
    PressureEvidence result;
    inspectScopedPressure(
        resourcePressure.value(QStringLiteral("fifo")).toObject(),
        QStringLiteral("FIFO 满"), QStringLiteral("full_rate_percent"),
        QStringLiteral("full_cycles"), scope, result);
    inspectScopedPressure(
        resourcePressure.value(QStringLiteral("queue")).toObject(),
        QStringLiteral("Queue 满"), QStringLiteral("full_rate_percent"),
        QStringLiteral("full_cycles"), scope, result);
    inspectScopedPressure(
        resourcePressure.value(QStringLiteral("credit")).toObject(),
        QStringLiteral("Credit 耗尽"),
        QStringLiteral("exhausted_rate_percent"),
        QStringLiteral("exhausted_cycles"), scope, result);
    return result;
}

QJsonObject topBlockReason(const QJsonArray& blocks) {
    QJsonObject best;
    double bestRate = -1.0;
    for (const QJsonValue& value : blocks) {
        const QJsonObject block = value.toObject();
        const double rate =
            block.value(QStringLiteral("queue_ready_percent")).toDouble();
        if (rate > bestRate) {
            best = block;
            bestRate = rate;
        }
    }
    return best;
}

QString confidenceName(int score) {
    if (score >= 80) return QStringLiteral("高");
    if (score >= 55) return QStringLiteral("中");
    return QStringLiteral("低");
}

void inspectBandwidthDirection(const QJsonObject& direction,
                               const QString& label,
                               double& highest,
                               QString& highestLabel) {
    if (!direction.value(QStringLiteral("utilization_available")).toBool()) return;
    const double utilization =
        direction.value(QStringLiteral("utilization_percent")).toDouble();
    if (utilization > highest) {
        highest = utilization;
        highestLabel = label;
    }
}

QString issueDomainName(const QString& key) {
    if (key == QStringLiteral("thread")) return QStringLiteral("普通指令");
    if (key == QStringLiteral("group")) return QStringLiteral("Group 指令");
    if (key == QStringLiteral("cb")) return QStringLiteral("CB 指令");
    if (key == QStringLiteral("mma")) return QStringLiteral("MMA 指令");
    return QStringLiteral("未分类指令");
}

bool dependencyBlock(const QJsonObject& block) {
    const QString text =
        block.value(QStringLiteral("key")).toString() +
        QLatin1Char(' ') +
        block.value(QStringLiteral("name")).toString();
    return text.contains(QStringLiteral("depend"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("依赖"));
}

}  // namespace

QJsonArray buildQppuConclusions(const QJsonObject& model) {
    const QJsonObject analysis =
        model.value(QStringLiteral("analysis")).toObject();
    const double durationCycles =
        analysis.value(QStringLiteral("duration_cycles")).toDouble();
    const QJsonObject scheduler =
        model.value(QStringLiteral("scheduler")).toObject();
    const QString schedulerStatus =
        scheduler.value(QStringLiteral("status")).toString();
    if (schedulerStatus != QStringLiteral("measured") &&
        schedulerStatus != QStringLiteral("partial")) {
        return {};
    }

    const QJsonObject resourcePressure =
        model.value(QStringLiteral("resource_pressure")).toObject();
    QJsonArray result;
    const QJsonArray qppus =
        scheduler.value(QStringLiteral("qppus")).toArray();
    for (const QJsonValue& value : qppus) {
        const QJsonObject qppu = value.toObject();
        const int index = qppu.value(QStringLiteral("index")).toInt();
        const QString path =
            qppu.value(QStringLiteral("path")).toString();
        const double issueUtilization =
            qppu.value(
                QStringLiteral("issue_utilization_percent")).toDouble();
        const double issueObserved =
            qppu.value(QStringLiteral("issue_observed_cycles")).toDouble();
        const bool issueActivityCovered =
            qppu.value(
                QStringLiteral("issue_coverage_complete")).toBool() &&
            qppu.contains(QStringLiteral("issue_active_percent")) &&
            issueObserved > 0.0;
        const double issueActive =
            issueActivityCovered
                ? qppu.value(
                      QStringLiteral("issue_active_percent")).toDouble()
                : qMin(100.0, issueUtilization);
        const double issueIdle =
            issueActivityCovered
                ? qppu.value(
                      QStringLiteral("issue_idle_cycles")).toDouble()
                : qMax(0.0, durationCycles *
                                 (1.0 - issueActive / 100.0));

        const QJsonArray shaderGroups =
            qppu.value(QStringLiteral("shader_groups")).toArray();
        const double sgCapacity =
            durationCycles * double(shaderGroups.size());
        const bool activityCovered =
            sgCapacity > 0.0 &&
            qppu.value(
                QStringLiteral("activity_coverage_complete")).toBool(false);
        const bool eligibilityCovered =
            sgCapacity > 0.0 &&
            qppu.value(
                QStringLiteral("eligibility_coverage_complete")).toBool(false);
        const bool sgCovered = activityCovered;
        const double activePercent =
            sgCovered
                ? 100.0 *
                      qppu.value(
                          QStringLiteral("active_sg_cycles")).toDouble() /
                      sgCapacity
                : 0.0;
        const double activeCycles =
            qppu.value(QStringLiteral("active_sg_cycles")).toDouble();
        const double queueReadyCycles =
            qppu.value(
                QStringLiteral("queue_ready_sg_cycles")).toDouble();
        const double queueReadyPercent =
            activeCycles > 0.0
                ? 100.0 * queueReadyCycles / activeCycles
                : 0.0;
        const double eligiblePercent =
            qppu.value(QStringLiteral("eligible_percent")).toDouble();
        const QJsonObject topBlock =
            topBlockReason(
                qppu.value(QStringLiteral("block_reasons")).toArray());
        const double topBlockRate =
            topBlock.value(
                QStringLiteral("queue_ready_percent")).toDouble();
        const PressureEvidence pressure =
            pressureForScope(resourcePressure, path);
        const double issuedInstructions =
            qppu.contains(QStringLiteral("issued_instructions_estimate"))
                ? qppu.value(
                      QStringLiteral("issued_instructions_estimate")).toDouble()
                : issueObserved * issueUtilization / 100.0;
        const bool participationKnown =
            sgCovered || issuedInstructions > 0.0;
        const bool participating =
            !sgCovered ||
            activeCycles > 0.0 || queueReadyCycles > 0.0 ||
            issuedInstructions > 0.0;

        QString state = QStringLiteral("healthy");
        QString severity = QStringLiteral("info");
        ModuleInfo module{QStringLiteral("none"),
                          QStringLiteral("未发现瓶颈")};
        QString title = QStringLiteral("发射链正常");
        QString reason =
            QStringLiteral("现有指标未形成明确的模块瓶颈证据。");
        QString evidence =
            QStringLiteral("发射活跃 %1%，发射利用率 %2%。")
                .arg(issueActive, 0, 'f', 1)
                .arg(issueUtilization, 0, 'f', 1);
        QString nextStep =
            QStringLiteral("无需优先处理该 QPPU。");
        int confidenceScore = 70;
        double evidenceCycles = 0.0;
        QString evidencePath;

        if (participationKnown && !participating) {
            state = QStringLiteral("inactive");
            module = {QStringLiteral("scope"),
                      QStringLiteral("未参与")};
            title = QStringLiteral("未参与当前工作负载");
            reason =
                QStringLiteral("分析区间内没有 Active SG、Queue Ready 或实际发射，"
                               "该实例不进入负载均衡分母。");
            evidence =
                QStringLiteral("Active SG-cycle 0，Queue Ready SG-cycle 0，"
                               "估算发射指令 0。");
            nextStep =
                QStringLiteral("若预期该 QPPU 应参与，再检查任务映射与采样区间。");
            confidenceScore = 95;
        } else if (!issueActivityCovered && !sgCovered) {
            state = QStringLiteral("uncovered");
            module = {QStringLiteral("unknown"),
                      QStringLiteral("未覆盖")};
            title = QStringLiteral("诊断覆盖不足");
            reason =
                QStringLiteral("缺少逐周期发射和 SG 状态，无法定位内部瓶颈。");
            evidence =
                QStringLiteral("仅能观测发射利用率 %1%。")
                    .arg(issueUtilization, 0, 'f', 1);
            nextStep =
                QStringLiteral("补充 issue valid 和 SG 调度状态。");
            confidenceScore = 20;
        } else if (!sgCovered && issueActive < 70.0) {
            state = QStringLiteral("uncovered");
            severity = QStringLiteral("warning");
            module = {QStringLiteral("unknown"),
                      QStringLiteral("未覆盖")};
            title = QStringLiteral("发射不足但无法下钻");
            reason =
                QStringLiteral("已观测到发射空洞，但缺少逐 SG 调度状态，"
                               "无法区分 FE、QPPUCtrl 和 BE。");
            evidence =
                QStringLiteral("发射活跃 %1%，未发射 %2 周期。")
                    .arg(issueActive, 0, 'f', 1)
                    .arg(issueIdle, 0, 'f', 1);
            nextStep =
                QStringLiteral("补充 SG valid、指令 Queue 和阻塞状态。");
            confidenceScore = 30;
            evidenceCycles =
                issueActivityCovered ? issueIdle : 0.0;
        } else if (sgCovered && activePercent < 50.0) {
            state = QStringLiteral("bottleneck");
            severity = activePercent < 20.0
                           ? QStringLiteral("critical")
                           : QStringLiteral("warning");
            module = {QStringLiteral("dispatch"),
                      QStringLiteral("任务分发/驻留")};
            title = QStringLiteral("SG 驻留不足");
            reason =
                QStringLiteral("该 QPPU 大部分时间没有足够的有效 SG，"
                               "瓶颈位于内部发射链之前。");
            evidence =
                issueActivityCovered
                    ? QStringLiteral(
                          "Active SG 容量占比 %1%，未发射 %2 周期。")
                          .arg(activePercent, 0, 'f', 1)
                          .arg(issueIdle, 0, 'f', 1)
                    : QStringLiteral(
                          "Active SG 容量占比 %1%；发射槽覆盖不完整，"
                          "不发布未发射周期。")
                          .arg(activePercent, 0, 'f', 1);
            nextStep =
                QStringLiteral("检查任务映射、SG 分发和生命周期空洞。");
            confidenceScore = 90;
            evidenceCycles =
                issueActivityCovered ? issueIdle : 0.0;
        } else if (sgCovered && queueReadyPercent < 60.0) {
            state = QStringLiteral("bottleneck");
            severity = queueReadyPercent < 30.0
                           ? QStringLiteral("critical")
                           : QStringLiteral("warning");
            module = {QStringLiteral("fe"), QStringLiteral("FE")};
            title = QStringLiteral("前端指令供给不足");
            reason =
                QStringLiteral("SG 已驻留，但指令 Queue 经常为空。");
            evidence =
                issueActivityCovered
                    ? QStringLiteral(
                          "Queue Ready/Active 为 %1%，未发射 %2 周期。")
                          .arg(queueReadyPercent, 0, 'f', 1)
                          .arg(issueIdle, 0, 'f', 1)
                    : QStringLiteral(
                          "Queue Ready/Active 为 %1%；发射槽覆盖不完整，"
                          "不发布未发射周期。")
                          .arg(queueReadyPercent, 0, 'f', 1);
            nextStep =
                QStringLiteral("检查取指、I-cache 返回和前端 Credit。");
            confidenceScore = 88;
            evidenceCycles = issueIdle;
        } else if (eligibilityCovered && eligiblePercent < 50.0 &&
                   topBlockRate >= 10.0) {
            state = QStringLiteral("bottleneck");
            severity = topBlockRate >= 30.0
                           ? QStringLiteral("critical")
                           : QStringLiteral("warning");
            module = {QStringLiteral("qppu_ctrl"),
                      QStringLiteral("QPPUCtrl")};
            title = QStringLiteral("调度阻塞");
            reason =
                QStringLiteral("队列中已有指令，但 SG 被已观测到的调度条件阻塞。");
            evidenceCycles =
                topBlock.value(QStringLiteral("cycles")).toDouble();
            evidence =
                QStringLiteral("%1 占 Queue Ready %2%，证据 %3 SG-cycle。")
                    .arg(topBlock.value(
                             QStringLiteral("name")).toString())
                    .arg(topBlockRate, 0, 'f', 1)
                    .arg(evidenceCycles, 0, 'f', 1);
            if (issueActivityCovered) {
                evidence +=
                    QStringLiteral(" 未发射 %1 周期。")
                        .arg(issueIdle, 0, 'f', 1);
            } else {
                evidence +=
                    QStringLiteral(
                        " 发射槽覆盖不完整，不发布未发射周期。");
            }
            nextStep =
                QStringLiteral("在 SG 明细和时间线上定位该阻塞条件。");
            confidenceScore = 88;
        } else if (!eligibilityCovered && topBlockRate >= 10.0) {
            state = QStringLiteral("risk");
            severity = QStringLiteral("warning");
            module = {QStringLiteral("qppu_ctrl"),
                      QStringLiteral("QPPUCtrl")};
            title = QStringLiteral("已确认局部调度阻塞");
            reason =
                QStringLiteral("已观测到明确的调度阻塞，但其覆盖范围不足以解释"
                               "全部发射空洞。");
            evidenceCycles =
                topBlock.value(QStringLiteral("cycles")).toDouble();
            evidence =
                QStringLiteral("%1 占 Queue Ready %2%，证据 %3 SG-cycle；"
                               "其余未发射区间仍有未覆盖条件。")
                    .arg(topBlock.value(
                             QStringLiteral("name")).toString())
                    .arg(topBlockRate, 0, 'f', 1)
                    .arg(evidenceCycles, 0, 'f', 1);
            nextStep =
                QStringLiteral("先补齐调度覆盖，再判断该阻塞是否为主因。");
            confidenceScore = 58;
        } else if (!issueActivityCovered) {
            state = QStringLiteral("uncovered");
            severity = QStringLiteral("warning");
            module = {QStringLiteral("unknown"),
                      QStringLiteral("未覆盖")};
            title = QStringLiteral("发射覆盖不足，停止后端归因");
            reason =
                QStringLiteral("SG 驻留和前端供给未形成明确限制，但发射槽存在未知区间，"
                               "不能继续判断 BE、执行链或资源背压。");
            evidence =
                QStringLiteral("已观测发射利用率 %1%，但发射分母不完整。")
                    .arg(issueUtilization, 0, 'f', 1);
            nextStep =
                QStringLiteral("补齐该 QPPU 的全部 issue valid 槽后再下钻。");
            confidenceScore = 95;
        } else if (issueActive < 70.0) {
            state = QStringLiteral("bottleneck");
            severity = issueActive < 30.0
                           ? QStringLiteral("critical")
                           : QStringLiteral("warning");
            evidenceCycles = issueIdle;
            if (pressure.rate >= 30.0) {
                module = pressure.module;
                title = pressure.module.name +
                        QStringLiteral("资源背压");
                reason =
                    QStringLiteral("本 QPPU 发射不足，同时同一作用域内出现持续资源压力。");
                evidence =
                    QStringLiteral("发射活跃 %1%，未发射 %2 周期；%3率 %4%，%5。")
                        .arg(issueActive, 0, 'f', 1)
                        .arg(issueIdle, 0, 'f', 1)
                        .arg(pressure.kind)
                        .arg(pressure.rate, 0, 'f', 1)
                        .arg(pressure.path);
                nextStep =
                    QStringLiteral("在时间线上核对资源压力与未发射区间是否重合。");
                confidenceScore = 68;
                evidencePath = pressure.path;
            } else if (eligibilityCovered && eligiblePercent >= 50.0) {
                module = {QStringLiteral("backend_unknown"),
                          QStringLiteral("BE/仲裁")};
                title = QStringLiteral("可发射但未进入执行链");
                reason =
                    QStringLiteral("SG 局部 Eligible，但实际发射长期为空；"
                                   "已覆盖调度条件不能解释缺口。");
                evidence =
                    QStringLiteral("局部 Eligible %1%，发射活跃 %2%，"
                                   "未发射 %3 周期。")
                        .arg(eligiblePercent, 0, 'f', 1)
                        .arg(issueActive, 0, 'f', 1)
                        .arg(issueIdle, 0, 'f', 1);
                if (topBlockRate > 0.0) {
                    evidence +=
                        QStringLiteral(" 已观测 %1 阻塞仅占 %2%。")
                            .arg(topBlock.value(
                                     QStringLiteral("name")).toString())
                            .arg(topBlockRate, 0, 'f', 1);
                }
                nextStep =
                    QStringLiteral("补查操作数、仲裁、EU 接收、写回和 CB 返回握手。");
                confidenceScore = 48;
            } else if (eligibilityCovered) {
                module = {QStringLiteral("qppu_ctrl"),
                          QStringLiteral("QPPUCtrl")};
                title = QStringLiteral("多因素调度阻塞");
                reason =
                    QStringLiteral("Queue Ready 后的 Eligible 比例偏低，"
                                   "但没有单一阻塞原因达到主导阈值。");
                evidence =
                    QStringLiteral("局部 Eligible %1%，发射活跃 %2%，"
                                   "未发射 %3 周期。")
                        .arg(eligiblePercent, 0, 'f', 1)
                        .arg(issueActive, 0, 'f', 1)
                        .arg(issueIdle, 0, 'f', 1);
                nextStep =
                    QStringLiteral("按 SG 和时间段展开阻塞原因。");
                confidenceScore = 60;
            } else {
                state = QStringLiteral("uncovered");
                module = {QStringLiteral("unknown"),
                          QStringLiteral("未覆盖")};
                title = QStringLiteral("发射不足但阻塞覆盖不完整");
                reason =
                    QStringLiteral("Queue Ready 后仍有发射空洞，但部分 SG 缺少"
                                   "完整的调度阻塞信号，不能继续归因。");
                evidence =
                    QStringLiteral("发射活跃 %1%，未发射 %2 周期；"
                                   "已观测阻塞未形成主导证据。")
                        .arg(issueActive, 0, 'f', 1)
                        .arg(issueIdle, 0, 'f', 1);
                nextStep =
                    QStringLiteral("补齐该实例的 issue 槽和 SG 阻塞向量。");
                confidenceScore = 30;
            }
        } else if (pressure.rate >= 30.0) {
            state = QStringLiteral("risk");
            severity = QStringLiteral("warning");
            module = pressure.module;
            title = pressure.module.name +
                    QStringLiteral(" 高压运行");
            reason =
                QStringLiteral("资源压力较高，但当前发射活跃度尚未明显下降。");
            evidence =
                QStringLiteral("发射活跃 %1%，发射利用率 %2%；%3率 %4%，%5。")
                    .arg(issueActive, 0, 'f', 1)
                    .arg(issueUtilization, 0, 'f', 1)
                    .arg(pressure.kind)
                    .arg(pressure.rate, 0, 'f', 1)
                    .arg(pressure.path);
            nextStep =
                QStringLiteral("作为风险监控，不直接认定为当前吞吐瓶颈。");
            confidenceScore = 78;
            evidenceCycles = pressure.cycles;
            evidencePath = pressure.path;
        }

        QJsonObject object;
        object.insert(QStringLiteral("qppu_index"), index);
        object.insert(QStringLiteral("path"), path);
        object.insert(QStringLiteral("state"), state);
        object.insert(QStringLiteral("participating"), participating);
        object.insert(QStringLiteral("participation_known"),
                      participationKnown);
        object.insert(QStringLiteral("issue_coverage_complete"),
                      issueActivityCovered);
        object.insert(QStringLiteral("activity_coverage_complete"),
                      activityCovered);
        object.insert(QStringLiteral("eligibility_coverage_complete"),
                      eligibilityCovered);
        object.insert(
            QStringLiteral("participation_basis"),
            sgCovered
                ? QStringLiteral("Active SG、Queue Ready 或实际发射")
                : QStringLiteral("SG 范围未覆盖，保守纳入"));
        object.insert(QStringLiteral("severity"), severity);
        object.insert(QStringLiteral("module_key"), module.key);
        object.insert(QStringLiteral("module"), module.name);
        object.insert(QStringLiteral("title"), title);
        object.insert(QStringLiteral("reason"), reason);
        object.insert(QStringLiteral("evidence"), evidence);
        object.insert(QStringLiteral("next_step"), nextStep);
        object.insert(QStringLiteral("confidence"),
                      confidenceName(confidenceScore));
        object.insert(QStringLiteral("confidence_score"),
                      confidenceScore);
        object.insert(QStringLiteral("issue_utilization_percent"),
                      issueUtilization);
        if (issueActivityCovered) {
            object.insert(QStringLiteral("issue_active_percent"),
                          issueActive);
            object.insert(QStringLiteral("issue_idle_cycles"),
                          issueIdle);
        }
        object.insert(QStringLiteral("active_percent"), activePercent);
        object.insert(QStringLiteral("queue_ready_percent"),
                      queueReadyPercent);
        object.insert(QStringLiteral("eligible_percent"),
                      eligiblePercent);
        object.insert(QStringLiteral("evidence_cycles"),
                      evidenceCycles);
        if (!evidencePath.isEmpty()) {
            object.insert(QStringLiteral("evidence_path"),
                          evidencePath);
        }
        result.push_back(object);
    }
    return result;
}

QJsonObject buildWorkloadProfile(
    const QJsonObject& model,
    const QJsonArray& qppuConclusions) {
    const QJsonObject analysis =
        model.value(QStringLiteral("analysis")).toObject();
    const double durationCycles =
        analysis.value(QStringLiteral("duration_cycles")).toDouble();
    const QJsonObject scheduler =
        model.value(QStringLiteral("scheduler")).toObject();
    const QJsonArray qppus =
        scheduler.value(QStringLiteral("qppus")).toArray();
    const QJsonObject summary =
        model.value(QStringLiteral("summary")).toObject();
    const QJsonObject coverage =
        model.value(QStringLiteral("coverage")).toObject();
    const bool selectionCoverageComplete =
        !coverage.contains(
            QStringLiteral("signal_selection_complete")) ||
        coverage.value(
            QStringLiteral("signal_selection_complete")).toBool();

    auto conclusionByPath =
        [&](const QString& path) {
            for (const QJsonValue& value : qppuConclusions) {
                const QJsonObject object = value.toObject();
                if (object.value(QStringLiteral("path")).toString() == path) {
                    return object;
                }
            }
            return QJsonObject();
        };

    int participatingQppus = 0;
    int inactiveQppus = 0;
    QJsonArray participantIndices;
    QJsonArray participantPaths;
    bool activityCoverageComplete = true;
    bool eligibilityCoverageComplete = true;
    bool issueCoverageComplete = true;
    bool observedActivityCoverageComplete = !qppus.isEmpty();
    bool observedIssueCoverageComplete = !qppus.isEmpty();
    double issuedInstructions = 0.0;
    double issueObservedCycles = 0.0;
    double issueActiveCycles = 0.0;
    double sgCapacityCycles = 0.0;
    double activeSgCycles = 0.0;
    double queueReadySgCycles = 0.0;
    double eligibleSgCycles = 0.0;
    QHash<QString, double> blockCycles;
    QHash<QString, QString> blockNames;
    QJsonObject limitingQppu;
    int limitingScore = -1;

    for (const QJsonValue& value : qppus) {
        const QJsonObject qppu = value.toObject();
        const int index =
            qppu.value(QStringLiteral("index")).toInt();
        const QString path =
            qppu.value(QStringLiteral("path")).toString();
        const QJsonObject diagnosis = conclusionByPath(path);
        observedActivityCoverageComplete =
            observedActivityCoverageComplete &&
            qppu.value(
                QStringLiteral(
                    "activity_coverage_complete")).toBool(false);
        observedIssueCoverageComplete =
            observedIssueCoverageComplete &&
            qppu.value(
                QStringLiteral(
                    "issue_coverage_complete")).toBool(false);
        bool participating =
            diagnosis.value(
                QStringLiteral("participating")).toBool(true);
        if (diagnosis.isEmpty()) {
            const bool sgCovered =
                !qppu.value(
                     QStringLiteral("shader_groups")).toArray().isEmpty();
            participating =
                !sgCovered ||
                qppu.value(
                    QStringLiteral("active_sg_cycles")).toDouble() > 0.0 ||
                qppu.value(
                    QStringLiteral("queue_ready_sg_cycles")).toDouble() >
                    0.0 ||
                qppu.value(
                    QStringLiteral("issued_instructions_estimate")).toDouble() >
                    0.0;
        }
        if (!participating) {
            ++inactiveQppus;
            continue;
        }

        ++participatingQppus;
        participantIndices.push_back(index);
        participantPaths.push_back(path);
        activityCoverageComplete =
            activityCoverageComplete &&
            qppu.value(
                QStringLiteral("activity_coverage_complete")).toBool(false);
        eligibilityCoverageComplete =
            eligibilityCoverageComplete &&
            qppu.value(
                QStringLiteral("eligibility_coverage_complete")).toBool(false);
        issueCoverageComplete =
            issueCoverageComplete &&
            qppu.value(
                QStringLiteral("issue_coverage_complete")).toBool(false);
        const double observed =
            qppu.value(
                QStringLiteral("issue_observed_cycles")).toDouble();
        const double utilization =
            qppu.value(
                QStringLiteral("issue_utilization_percent")).toDouble();
        issuedInstructions +=
            qppu.contains(
                QStringLiteral("issued_instructions_estimate"))
                ? qppu.value(
                      QStringLiteral(
                          "issued_instructions_estimate")).toDouble()
                : observed * utilization / 100.0;
        issueObservedCycles += observed;
        issueActiveCycles +=
            qppu.contains(QStringLiteral("issue_active_cycles"))
                ? qppu.value(
                      QStringLiteral("issue_active_cycles")).toDouble()
                : observed *
                      qppu.value(
                          QStringLiteral(
                              "issue_active_percent")).toDouble() /
                      100.0;

        const int shaderGroups =
            qppu.value(
                QStringLiteral("shader_groups")).toArray().size();
        sgCapacityCycles += durationCycles * shaderGroups;
        activeSgCycles +=
            qppu.value(
                QStringLiteral("active_sg_cycles")).toDouble();
        queueReadySgCycles +=
            qppu.value(
                QStringLiteral("queue_ready_sg_cycles")).toDouble();
        eligibleSgCycles +=
            qppu.value(
                QStringLiteral("eligible_sg_cycles")).toDouble();

        for (const QJsonValue& blockValue :
             qppu.value(
                 QStringLiteral("block_reasons")).toArray()) {
            const QJsonObject block = blockValue.toObject();
            const QString key =
                block.value(QStringLiteral("key")).toString(
                    block.value(
                        QStringLiteral("name")).toString());
            blockCycles[key] +=
                block.value(
                    QStringLiteral("cycles")).toDouble();
            blockNames.insert(
                key,
                block.value(QStringLiteral("name")).toString(key));
        }

        if (diagnosis.value(QStringLiteral("state")).toString() ==
            QStringLiteral("bottleneck")) {
            const int score =
                severityRank(
                    diagnosis.value(
                        QStringLiteral("severity")).toString()) *
                    100000 +
                int(diagnosis.value(
                    QStringLiteral("evidence_cycles")).toDouble());
            if (score > limitingScore) {
                limitingScore = score;
                limitingQppu = diagnosis;
            }
        }
    }

    const double issueUtilization =
        issueObservedCycles > 0.0
            ? 100.0 * issuedInstructions / issueObservedCycles
            : 0.0;
    const double issueActivePercent =
        issueObservedCycles > 0.0
            ? 100.0 * issueActiveCycles / issueObservedCycles
            : 0.0;
    const double activePercent =
        sgCapacityCycles > 0.0
            ? 100.0 * activeSgCycles / sgCapacityCycles
            : 0.0;
    const double queueReadyPercent =
        activeSgCycles > 0.0
            ? 100.0 * queueReadySgCycles / activeSgCycles
            : 0.0;
    const double eligiblePercent =
        queueReadySgCycles > 0.0
            ? 100.0 * eligibleSgCycles / queueReadySgCycles
            : 0.0;

    QJsonObject dominantBlock;
    double dominantBlockCycles = 0.0;
    for (auto it = blockCycles.constBegin();
         it != blockCycles.constEnd(); ++it) {
        if (it.value() <= dominantBlockCycles) continue;
        dominantBlockCycles = it.value();
        dominantBlock.insert(QStringLiteral("key"), it.key());
        dominantBlock.insert(QStringLiteral("name"),
                             blockNames.value(it.key(), it.key()));
        dominantBlock.insert(QStringLiteral("cycles"), it.value());
    }
    dominantBlock.insert(
        QStringLiteral("queue_ready_percent"),
        queueReadySgCycles > 0.0
            ? 100.0 * dominantBlockCycles / queueReadySgCycles
            : 0.0);
    const double dominantBlockRate =
        dominantBlock.value(
            QStringLiteral("queue_ready_percent")).toDouble();

    QString dominantIssueClass = QStringLiteral("unknown");
    double dominantIssueShare = 0.0;
    const bool issueTypeCoverageComplete =
        summary.value(
            QStringLiteral("issue_type_signals_covered")).toBool();
    if (issueTypeCoverageComplete) {
        for (const QJsonValue& value :
             summary.value(
                 QStringLiteral("issue_classes")).toArray()) {
            const QJsonObject issueClass = value.toObject();
            const double share =
                issueClass.value(
                    QStringLiteral("instruction_share_percent")).toDouble();
            if (share > dominantIssueShare) {
                dominantIssueShare = share;
                dominantIssueClass =
                    issueClass.value(
                        QStringLiteral("key")).toString();
            }
        }
    }
    const double memoryIssues =
        summary.value(
            QStringLiteral("global_memory_issue_estimate")).toDouble() +
        summary.value(
            QStringLiteral("local_memory_issue_estimate")).toDouble();
    const double memoryIssueShare =
        issuedInstructions > 0.0
            ? qBound(0.0, 100.0 * memoryIssues / issuedInstructions,
                     100.0)
            : 0.0;
    const bool memoryIssueCoverageComplete =
        summary.value(
            QStringLiteral("memory_issue_signals_covered")).toBool();

    const QJsonObject bandwidth =
        model.value(QStringLiteral("memory_bandwidth")).toObject();
    const QJsonObject l1 =
        bandwidth.value(QStringLiteral("l1")).toObject();
    const QJsonObject l2 =
        bandwidth.value(QStringLiteral("l2")).toObject();
    const QJsonObject l1Latency =
        l1.value(QStringLiteral("latency")).toObject();
    double highestBandwidth = 0.0;
    QString highestBandwidthLabel;
    inspectBandwidthDirection(
        l1.value(QStringLiteral("read")).toObject(),
        QStringLiteral("L1 读"), highestBandwidth,
        highestBandwidthLabel);
    inspectBandwidthDirection(
        l1.value(QStringLiteral("write")).toObject(),
        QStringLiteral("L1 写"), highestBandwidth,
        highestBandwidthLabel);
    inspectBandwidthDirection(
        l2.value(QStringLiteral("read")).toObject(),
        QStringLiteral("L2 读"), highestBandwidth,
        highestBandwidthLabel);
    inspectBandwidthDirection(
        l2.value(QStringLiteral("write")).toObject(),
        QStringLiteral("L2 写"), highestBandwidth,
        highestBandwidthLabel);

    const bool latencyAvailable =
        l1Latency.value(QStringLiteral("available")).toBool();
    const bool latencyReliable =
        latencyAvailable &&
        l1Latency.value(
            QStringLiteral("coverage_complete")).toBool() &&
        l1Latency.value(
            QStringLiteral("confidence_score")).toInt() >= 70;
    const bool dependencyObserved =
        eligibilityCoverageComplete &&
        dependencyBlock(dominantBlock) &&
        dominantBlockRate >=
            (participatingQppus == 1 ? 10.0 : 30.0);
    const bool memoryObserved =
        (memoryIssueCoverageComplete && memoryIssueShare >= 50.0) ||
        (latencyReliable && dependencyObserved);
    const bool issueUnderfilled =
        issueActivePercent < 70.0;

    QString regime = QStringLiteral("mixed");
    QString title = QStringLiteral("未形成单一主导模式");
    QString conclusion =
        QStringLiteral("当前区间混合了多种限制，证据不足以归为单一根因。");
    QString evidence =
        QStringLiteral("发射活跃 %1%，Eligible %2%。")
            .arg(issueActivePercent, 0, 'f', 1)
            .arg(eligiblePercent, 0, 'f', 1);
    int confidenceScore = 55;
    const bool hasObservedExecutionActivity =
        issuedInstructions > 0.0 ||
        activeSgCycles > 0.0 ||
        queueReadySgCycles > 0.0;
    const bool noActivityCoverageComplete =
        observedActivityCoverageComplete &&
        observedIssueCoverageComplete;

    if (!selectionCoverageComplete) {
        regime = QStringLiteral("partial_selection");
        title = QStringLiteral("性能信号选择被截断");
        conclusion =
            QStringLiteral(
                "当前报告只覆盖波形中的性能信号前缀，不能据此判断全局吞吐或瓶颈。");
        evidence =
            QStringLiteral(
                "已选 %1 个性能信号，达到 --max-signals 上限。")
                .arg(coverage.value(
                         QStringLiteral(
                             "signal_selection_count")).toInt());
        confidenceScore = 95;
    } else if (!hasObservedExecutionActivity &&
               !noActivityCoverageComplete) {
        regime = QStringLiteral("partial_coverage");
        title = QStringLiteral("执行活动覆盖不完整");
        conclusion =
            QStringLiteral(
                "当前观测值没有显示执行活动，但仍有 QPPU 缺少完整的 SG 活动或发射信号，不能判定为空闲。");
        evidence =
            QStringLiteral(
                "SG 活动覆盖 %1，发射覆盖 %2。")
                .arg(observedActivityCoverageComplete
                         ? QStringLiteral("完整")
                         : QStringLiteral("不完整"))
                .arg(observedIssueCoverageComplete
                         ? QStringLiteral("完整")
                         : QStringLiteral("不完整"));
        confidenceScore = 95;
    } else if (participatingQppus == 0 ||
               !hasObservedExecutionActivity) {
        regime = QStringLiteral("no_activity");
        title = QStringLiteral("没有 QPPU 参与执行");
        conclusion =
            QStringLiteral("当前区间不适合判断执行吞吐瓶颈。");
        evidence =
            QStringLiteral(
                "观测 %1 个 QPPU，均无 Active SG、Queue Ready 或实际发射。")
                .arg(qppus.size());
        confidenceScore = 90;
    } else if (activityCoverageComplete && activePercent < 20.0) {
        regime = QStringLiteral("workload_fill");
        title = QStringLiteral("工作负载驻留不足");
        conclusion =
            QStringLiteral("参与实例内部的大部分 SG 容量没有被工作占用。");
        evidence =
            QStringLiteral("参与 QPPU 的 Active SG 容量占比 %1%。")
                .arg(activePercent, 0, 'f', 1);
        confidenceScore = 90;
    } else if (activityCoverageComplete && queueReadyPercent < 50.0) {
        regime = QStringLiteral("frontend_supply");
        title = QStringLiteral("前端指令供给受限");
        conclusion =
            QStringLiteral("SG 已驻留，但指令 Queue 没有持续准备好。");
        evidence =
            QStringLiteral("参与 QPPU 的 Queue Ready/Active 为 %1%。")
                .arg(queueReadyPercent, 0, 'f', 1);
        confidenceScore = 88;
    } else if (!issueCoverageComplete) {
        regime = QStringLiteral("partial_coverage");
        title = QStringLiteral("发射覆盖不完整");
        conclusion =
            QStringLiteral("部分参与 QPPU 的发射槽存在缺失或未知区间，"
                           "不能据此判断吞吐、执行延迟或调度瓶颈。");
        evidence =
            QStringLiteral("已覆盖区间的发射利用率为 %1%，但分母不是完整执行区间。")
                .arg(issueUtilization, 0, 'f', 1);
        confidenceScore = 95;
    } else if (highestBandwidth >= 80.0) {
        regime = QStringLiteral("memory_bandwidth");
        title =
            QStringLiteral("%1带宽受限").arg(highestBandwidthLabel);
        conclusion =
            QStringLiteral("有效数据带宽已接近已知峰值，吞吐更受带宽上限约束。");
        evidence =
            QStringLiteral("%1利用率 %2%。")
                .arg(highestBandwidthLabel)
                .arg(highestBandwidth, 0, 'f', 1);
        confidenceScore = 90;
    } else if (issueUnderfilled && memoryObserved &&
               (dependencyObserved || latencyReliable)) {
        regime = QStringLiteral("memory_latency");
        title = latencyReliable
                    ? QStringLiteral("L1 请求返回延迟受限")
                    : QStringLiteral("访存依赖延迟受限");
        conclusion =
            QStringLiteral("发射空洞与访存依赖同时存在，而带宽没有接近峰值；"
                           "根因更像单请求延迟或并发不足，不是带宽饱和。");
        evidence =
            latencyReliable
                ? QStringLiteral(
                      "L1 平均/P95 延迟 %1/%2 周期，最大 outstanding %3，"
                      "发射活跃 %4%，最高带宽 %5%。")
                      .arg(l1Latency.value(
                               QStringLiteral(
                                   "average_cycles")).toDouble(),
                           0, 'f', 2)
                      .arg(l1Latency.value(
                               QStringLiteral(
                                   "p95_cycles")).toDouble(),
                           0, 'f', 2)
                      .arg(l1Latency.value(
                               QStringLiteral(
                                   "maximum_outstanding")).toInt())
                      .arg(issueActivePercent, 0, 'f', 1)
                      .arg(highestBandwidth, 0, 'f', 1)
                : QStringLiteral(
                      "访存指令占比 %1%，%2 占 Queue Ready %3%，"
                      "发射活跃 %4%。")
                      .arg(memoryIssueShare, 0, 'f', 1)
                      .arg(dominantBlock.value(
                               QStringLiteral("name")).toString())
                      .arg(dominantBlockRate, 0, 'f', 1)
                      .arg(issueActivePercent, 0, 'f', 1);
        confidenceScore =
            latencyReliable
                ? l1Latency.value(
                      QStringLiteral("confidence_score")).toInt(70)
                : 68;
    } else if (issueUnderfilled && dependencyObserved) {
        regime = QStringLiteral("dependency_latency");
        title = issueDomainName(dominantIssueClass) +
                QStringLiteral("依赖链延迟受限");
        conclusion =
            QStringLiteral("队首指令主要在等待依赖解除，发射槽因此出现空洞。");
        evidence =
            QStringLiteral("%1 占 Queue Ready %2%，发射活跃 %3%。")
                .arg(dominantBlock.value(
                         QStringLiteral("name")).toString())
                .arg(dominantBlockRate, 0, 'f', 1)
                .arg(issueActivePercent, 0, 'f', 1);
        confidenceScore = 82;
    } else if (issueUnderfilled && !limitingQppu.isEmpty() &&
               limitingQppu.value(
                   QStringLiteral("module_key")).toString() !=
                   QStringLiteral("qppu_ctrl") &&
               limitingQppu.contains(
                   QStringLiteral("evidence_path"))) {
        regime = QStringLiteral("resource_backpressure");
        title =
            limitingQppu.value(QStringLiteral("module")).toString() +
            QStringLiteral("限制发射");
        conclusion =
            limitingQppu.value(
                QStringLiteral("reason")).toString();
        evidence =
            limitingQppu.value(
                QStringLiteral("evidence")).toString();
        confidenceScore =
            limitingQppu.value(
                QStringLiteral("confidence_score")).toInt(60);
    } else if (issueUnderfilled && eligibilityCoverageComplete &&
               eligiblePercent >= 50.0) {
        regime = QStringLiteral("execution_latency");
        title = issueDomainName(dominantIssueClass) +
                QStringLiteral("执行延迟或仲裁受限");
        conclusion =
            QStringLiteral("已有可发射 SG，但执行链没有持续接收；"
                           "应继续下钻操作数、仲裁、功能单元和写回。");
        evidence =
            QStringLiteral("Eligible %1%，发射活跃 %2%，最高带宽 %3%。")
                .arg(eligiblePercent, 0, 'f', 1)
                .arg(issueActivePercent, 0, 'f', 1)
                .arg(highestBandwidth, 0, 'f', 1);
        confidenceScore = 58;
    } else if (issueActivePercent >= 85.0 ||
               issueUtilization >= 100.0) {
        regime = QStringLiteral("throughput");
        title = issueDomainName(dominantIssueClass) +
                QStringLiteral("吞吐运行");
        conclusion =
            QStringLiteral("参与 QPPU 的发射链已持续工作，"
                           "当前更适合比较单位周期吞吐而非寻找发射空洞。");
        evidence =
            QStringLiteral("发射活跃 %1%，每 QPPU 发射利用率 %2%，"
                           "主导指令占比 %3%。")
                .arg(issueActivePercent, 0, 'f', 1)
                .arg(issueUtilization, 0, 'f', 1)
                .arg(dominantIssueShare, 0, 'f', 1);
        confidenceScore = 82;
    } else if (eligibilityCoverageComplete &&
               dominantBlockRate >= 10.0) {
        regime = QStringLiteral("scheduler_bound");
        title = QStringLiteral("调度条件限制发射");
        conclusion =
            QStringLiteral("队列已有工作，但主要阻塞条件降低了 Eligible 比例。");
        evidence =
            QStringLiteral("%1 占 Queue Ready %2%。")
                .arg(dominantBlock.value(
                         QStringLiteral("name")).toString())
                .arg(dominantBlockRate, 0, 'f', 1);
        confidenceScore = 78;
    }

    QJsonObject result;
    result.insert(
        QStringLiteral("status"),
        !selectionCoverageComplete ||
                regime == QStringLiteral("partial_coverage")
            ? QStringLiteral("partial")
            : (hasObservedExecutionActivity &&
                       participatingQppus > 0
                   ? (activityCoverageComplete &&
                              eligibilityCoverageComplete &&
                              issueCoverageComplete
                          ? QStringLiteral("measured")
                          : QStringLiteral("partial"))
                   : QStringLiteral("no_activity")));
    result.insert(QStringLiteral("scope_mode"),
                  QStringLiteral("activity_inferred"));
    result.insert(QStringLiteral("observed_qppus"), qppus.size());
    result.insert(QStringLiteral("participating_qppus"),
                  participatingQppus);
    result.insert(QStringLiteral("inactive_qppus"), inactiveQppus);
    result.insert(QStringLiteral("participant_indices"),
                  participantIndices);
    result.insert(QStringLiteral("participant_paths"),
                  participantPaths);
    result.insert(QStringLiteral("activity_coverage_complete"),
                  participatingQppus > 0 && activityCoverageComplete);
    result.insert(QStringLiteral("eligibility_coverage_complete"),
                  participatingQppus > 0 && eligibilityCoverageComplete);
    result.insert(QStringLiteral("issue_coverage_complete"),
                   participatingQppus > 0 && issueCoverageComplete);
    result.insert(QStringLiteral("selection_coverage_complete"),
                  selectionCoverageComplete);
    result.insert(
        QStringLiteral("scope_name"),
        participatingQppus == 1
            ? QStringLiteral("单 QPPU")
            : QStringLiteral("%1 个 QPPU").arg(participatingQppus));
    result.insert(QStringLiteral("issue_utilization_percent"),
                  issueUtilization);
    result.insert(QStringLiteral("issue_active_percent"),
                  issueActivePercent);
    result.insert(QStringLiteral("issue_idle_cycles"),
                  qMax(0.0, issueObservedCycles - issueActiveCycles));
    result.insert(QStringLiteral("active_percent"), activePercent);
    result.insert(QStringLiteral("queue_ready_percent"),
                  queueReadyPercent);
    result.insert(QStringLiteral("eligible_percent"),
                  eligiblePercent);
    result.insert(QStringLiteral("dominant_block"),
                  dominantBlock);
    result.insert(QStringLiteral("dominant_issue_class"),
                  dominantIssueClass);
    result.insert(QStringLiteral("dominant_issue_name"),
                  issueDomainName(dominantIssueClass));
    result.insert(QStringLiteral("dominant_issue_share_percent"),
                  dominantIssueShare);
    result.insert(QStringLiteral("memory_issue_share_percent"),
                  memoryIssueShare);
    result.insert(QStringLiteral("issue_type_coverage_complete"),
                  issueTypeCoverageComplete);
    result.insert(QStringLiteral("memory_issue_coverage_complete"),
                  memoryIssueCoverageComplete);
    result.insert(QStringLiteral("highest_bandwidth_percent"),
                  highestBandwidth);
    result.insert(QStringLiteral("highest_bandwidth_path"),
                  highestBandwidthLabel);
    result.insert(QStringLiteral("l1_latency"), l1Latency);
    result.insert(QStringLiteral("regime"), regime);
    result.insert(QStringLiteral("title"), title);
    result.insert(QStringLiteral("conclusion"), conclusion);
    result.insert(QStringLiteral("evidence"), evidence);
    result.insert(QStringLiteral("confidence"),
                  confidenceName(confidenceScore));
    result.insert(QStringLiteral("confidence_score"),
                  confidenceScore);
    if (!limitingQppu.isEmpty()) {
        result.insert(QStringLiteral("limiting_qppu"),
                      limitingQppu);
    }
    return result;
}

QJsonArray buildPerformanceFindings(const QJsonObject& model) {
    QJsonArray findings;
    const QJsonObject summary =
        model.value(QStringLiteral("summary")).toObject();
    const QJsonObject scheduler =
        model.value(QStringLiteral("scheduler")).toObject();
    const QJsonObject schedulerSummary =
        scheduler.value(QStringLiteral("summary")).toObject();
    const QJsonObject workload =
        model.value(QStringLiteral("workload_profile")).toObject();
    const bool hasWorkloadProfile = !workload.isEmpty();
    const int participatingQppus =
        workload.value(
            QStringLiteral("participating_qppus")).toInt();
    const bool participantScopeAvailable =
        hasWorkloadProfile && participatingQppus > 0;
    const bool selectionCoverageComplete =
        !workload.contains(
            QStringLiteral("selection_coverage_complete")) ||
        workload.value(
            QStringLiteral("selection_coverage_complete")).toBool();

    const double issueUtilization =
        participantScopeAvailable
            ? workload.value(
                  QStringLiteral(
                      "issue_utilization_percent")).toDouble()
            : summary.value(
                  QStringLiteral(
                      "issue_utilization_percent")).toDouble();
    const double activeSg =
        participantScopeAvailable
            ? workload.value(
                  QStringLiteral("active_percent")).toDouble()
            : schedulerSummary.value(
                  QStringLiteral("active_percent")).toDouble();
    const double queueReady =
        participantScopeAvailable
            ? workload.value(
                  QStringLiteral(
                      "queue_ready_percent")).toDouble()
            : schedulerSummary.value(
                  QStringLiteral(
                      "queue_ready_percent")).toDouble();
    const double eligible =
        participantScopeAvailable
            ? workload.value(
                  QStringLiteral("eligible_percent")).toDouble()
             : schedulerSummary.value(
                   QStringLiteral("eligible_percent")).toDouble();
    const bool activityCoverageComplete =
        selectionCoverageComplete &&
        (participantScopeAvailable
            ? workload.value(
                  QStringLiteral(
                      "activity_coverage_complete")).toBool()
            : schedulerSummary.value(
                  QStringLiteral(
                      "activity_coverage_complete")).toBool());
    const bool eligibilityCoverageComplete =
        selectionCoverageComplete &&
        (participantScopeAvailable
            ? workload.value(
                  QStringLiteral(
                      "eligibility_coverage_complete")).toBool()
            : schedulerSummary.value(
                  QStringLiteral(
                      "eligibility_coverage_complete")).toBool());
    const bool issueCoverageComplete =
        selectionCoverageComplete &&
        (participantScopeAvailable
            ? workload.value(
                  QStringLiteral(
                      "issue_coverage_complete")).toBool()
            : summary.value(
                  QStringLiteral(
                      "issue_activity_coverage_complete")).toBool());

    const QString regime =
        workload.value(QStringLiteral("regime")).toString();
    if (regime == QStringLiteral("partial_coverage") ||
        regime == QStringLiteral("partial_selection")) {
        findings.push_back(finding(
            QStringLiteral("warning"),
            QStringLiteral("performance_coverage"),
            workload.value(QStringLiteral("title")).toString(),
            workload.value(QStringLiteral("conclusion")).toString(),
            workload.value(QStringLiteral("evidence")).toString(),
            QStringLiteral(
                "补齐所有参与 QPPU 的发射槽，再进行瓶颈归因。"),
            950));
    } else if (hasWorkloadProfile &&
               (participatingQppus == 0 ||
                regime == QStringLiteral("no_activity"))) {
        findings.push_back(finding(
            QStringLiteral("warning"),
            QStringLiteral("no_participating_qppu"),
            workload.value(QStringLiteral("title")).toString(),
            workload.value(QStringLiteral("conclusion")).toString(),
            workload.value(QStringLiteral("evidence")).toString(),
            QStringLiteral("选择包含 SG 驻留与指令发射的有效执行区间。"),
            900));
    } else if (regime == QStringLiteral("memory_latency")) {
        const QJsonObject latency =
            workload.value(QStringLiteral("l1_latency")).toObject();
        findings.push_back(finding(
            QStringLiteral("critical"),
            QStringLiteral("memory_latency"),
            workload.value(QStringLiteral("title")).toString(),
            workload.value(QStringLiteral("conclusion")).toString(),
            workload.value(QStringLiteral("evidence")).toString(),
            latency.value(QStringLiteral("available")).toBool()
                ? QStringLiteral(
                      "增加独立访存并发，检查 outstanding 上限、L1 命中路径和返回延迟。")
                : QStringLiteral(
                      "补充请求/返回握手，并增加独立访存以区分延迟与并发限制。"),
            900));
    } else if (regime == QStringLiteral("dependency_latency")) {
        findings.push_back(finding(
            QStringLiteral("critical"),
            QStringLiteral("dependency_latency"),
            workload.value(QStringLiteral("title")).toString(),
            workload.value(QStringLiteral("conclusion")).toString(),
            workload.value(QStringLiteral("evidence")).toString(),
            QStringLiteral(
                "检查队首 PC 的生产者-消费者距离，并增加独立指令或 SG 隐藏延迟。"),
            700));
    } else if (regime == QStringLiteral("resource_backpressure")) {
        findings.push_back(finding(
            QStringLiteral("warning"),
            QStringLiteral("resource_backpressure"),
            workload.value(QStringLiteral("title")).toString(),
            workload.value(QStringLiteral("conclusion")).toString(),
            workload.value(QStringLiteral("evidence")).toString(),
            QStringLiteral(
                "核对限制资源的满状态与发射空洞是否在时间线上重合。"),
            650));
    } else if (regime == QStringLiteral("execution_latency")) {
        findings.push_back(finding(
            QStringLiteral("warning"),
            QStringLiteral("execution_latency"),
            workload.value(QStringLiteral("title")).toString(),
            workload.value(QStringLiteral("conclusion")).toString(),
            workload.value(QStringLiteral("evidence")).toString(),
            QStringLiteral(
                "按 PC 与时间段下钻操作数、仲裁、EU 接收和写回返回。"),
            350));
    } else if (regime == QStringLiteral("throughput")) {
        findings.push_back(finding(
            QStringLiteral("info"),
            QStringLiteral("throughput_regime"),
            workload.value(QStringLiteral("title")).toString(),
            workload.value(QStringLiteral("conclusion")).toString(),
            workload.value(QStringLiteral("evidence")).toString(),
            QStringLiteral(
                "用估算指令数/周期、双发比例和目标功能单元吞吐比较不同实现。"),
            500));
    }
    if (participantScopeAvailable &&
        regime != QStringLiteral("partial_coverage") &&
        regime != QStringLiteral("partial_selection") &&
        (!activityCoverageComplete ||
         !eligibilityCoverageComplete ||
         !issueCoverageComplete)) {
        findings.push_back(finding(
            QStringLiteral("warning"),
            QStringLiteral("scheduler_coverage"),
            QStringLiteral("调度诊断覆盖不完整"),
            QStringLiteral(
                "部分逐周期状态缺失，未覆盖指标不参与瓶颈归因。"),
            QStringLiteral("SG 活动覆盖 %1，Eligible 覆盖 %2，"
                           "发射覆盖 %3。")
                .arg(activityCoverageComplete
                         ? QStringLiteral("完整")
                         : QStringLiteral("不完整"))
                .arg(eligibilityCoverageComplete
                         ? QStringLiteral("完整")
                         : QStringLiteral("不完整"))
                .arg(issueCoverageComplete
                         ? QStringLiteral("完整")
                         : QStringLiteral("不完整")),
            QStringLiteral(
                "补齐缺失信号后再判断驻留、调度或执行链瓶颈。"),
            900));
    }

    const QString schedulerStatus =
        scheduler.value(QStringLiteral("status")).toString();
    if ((schedulerStatus == QStringLiteral("measured") ||
         schedulerStatus == QStringLiteral("partial")) &&
        regime != QStringLiteral("partial_coverage") &&
        regime != QStringLiteral("partial_selection") &&
        (!hasWorkloadProfile || participatingQppus > 0)) {
        if (activityCoverageComplete && activeSg < 20.0) {
            findings.push_back(finding(
                QStringLiteral("warning"), QStringLiteral("workload_fill"),
                QStringLiteral("SG 驻留不足"),
                QStringLiteral("前端没有持续向 QPPU 提供足够的活跃 SG。"),
                QStringLiteral("Active SG 容量占比为 %1%。")
                    .arg(activeSg, 0, 'f', 1),
                QStringLiteral("先检查任务规模、分发间隙和 SG 生命周期。")));
        } else if (activityCoverageComplete &&
                   queueReady < 50.0) {
            findings.push_back(finding(
                QStringLiteral("warning"), QStringLiteral("frontend_supply"),
                QStringLiteral("指令供给不足"),
                QStringLiteral("SG 已驻留，但指令队列较多时间为空。"),
                QStringLiteral("Active SG 中 Queue Ready 占比为 %1%。")
                    .arg(queueReady, 0, 'f', 1),
                QStringLiteral("检查取指、DICache 返回和前端流控。")));
        }

        QJsonObject top;
        if (participantScopeAvailable) {
            top =
                workload.value(
                    QStringLiteral("dominant_block")).toObject();
        } else {
            const QJsonArray blocks =
                schedulerSummary.value(
                    QStringLiteral("block_reasons")).toArray();
            if (!blocks.isEmpty()) top = blocks.first().toObject();
        }
        if (eligibilityCoverageComplete && !top.isEmpty()) {
            const double rate =
                top.value(QStringLiteral("queue_ready_percent")).toDouble();
            if (rate >= 10.0) {
                const bool throughputUnderfilled =
                    issueCoverageComplete &&
                    issueUtilization < 100.0;
                findings.push_back(finding(
                    throughputUnderfilled && rate >= 30.0
                        ? QStringLiteral("critical")
                        : QStringLiteral("warning"),
                    QStringLiteral("scheduler_block"),
                    throughputUnderfilled
                        ? QStringLiteral("主要调度阻塞：%1")
                              .arg(top.value(
                                  QStringLiteral("name")).toString())
                        : QStringLiteral("局部调度阻塞风险：%1")
                              .arg(top.value(
                                  QStringLiteral("name")).toString()),
                    throughputUnderfilled
                        ? QStringLiteral(
                              "队列中已有指令，但该原因占用了最多等待时间。")
                        : QStringLiteral(
                              "已观测到局部等待，但当前总体发射吞吐未低于"
                              "单发基线。"),
                    QStringLiteral("%1 个 SG-cycle，占 Queue Ready 时间 %2%。")
                        .arg(top.value(QStringLiteral("cycles")).toDouble(),
                             0, 'f', 1)
                        .arg(rate, 0, 'f', 1),
                    throughputUnderfilled
                        ? QStringLiteral(
                              "在 QPPU/SG 明细和时间线上定位集中区间。")
                        : QStringLiteral(
                              "作为局部风险监控，不直接认定为主瓶颈。"),
                    qMin(999, int(rate))));
            }
        }

        if (schedulerStatus == QStringLiteral("measured") &&
            issueCoverageComplete &&
            eligibilityCoverageComplete &&
            issueUtilization < 70.0 && eligible >= 50.0) {
            findings.push_back(finding(
                QStringLiteral("critical"), QStringLiteral("issue_underfill"),
                QStringLiteral("有可发射 SG，但发射吞吐未填满"),
                QStringLiteral("瓶颈更接近仲裁、功能单元或未观测的操作数/写回资源。"),
                QStringLiteral("局部 Eligible 为 %1%，发射利用率为 %2%。")
                    .arg(eligible, 0, 'f', 1)
                    .arg(issueUtilization, 0, 'f', 1),
                QStringLiteral("结合 PC 热点和功能单元 pending 信号继续下钻。")));
        }
    }

    const QJsonArray qppus =
        scheduler.value(QStringLiteral("qppus")).toArray();
    const QJsonArray qppuConclusions =
        model.value(QStringLiteral("qppu_conclusions")).toArray();
    auto qppuConclusionByPath =
        [&](const QString& path) {
            for (const QJsonValue& value : qppuConclusions) {
                const QJsonObject object = value.toObject();
                if (belongsToScope(
                        path,
                        object.value(
                            QStringLiteral("path")).toString())) {
                    return object;
                }
            }
            return QJsonObject();
        };
    if (selectionCoverageComplete && qppus.size() >= 2) {
        double minimumRate = std::numeric_limits<double>::max();
        double maximumRate = 0.0;
        int minimumIndex = -1;
        int maximumIndex = -1;
        QString minimumPath;
        QString maximumPath;
        int measuredQppus = 0;
        for (const QJsonValue& value : qppus) {
            const QJsonObject qppu = value.toObject();
            const QString path =
                qppu.value(QStringLiteral("path")).toString();
            const QJsonObject qppuDiagnosis =
                qppuConclusionByPath(path);
            if (qppuDiagnosis.contains(
                    QStringLiteral("participating")) &&
                !qppuDiagnosis.value(
                    QStringLiteral("participating")).toBool()) {
                continue;
            }
            if (!qppu.value(
                    QStringLiteral("issue_coverage_complete")).toBool() ||
                !qppuDiagnosis.value(
                    QStringLiteral("issue_coverage_complete")).toBool()) {
                continue;
            }
            if (!qppu.contains(
                    QStringLiteral("issue_utilization_percent"))) {
                continue;
            }
            const double rate =
                qppu.value(
                    QStringLiteral("issue_utilization_percent")).toDouble();
            const int index =
                qppu.value(QStringLiteral("index")).toInt();
            ++measuredQppus;
            if (rate < minimumRate) {
                minimumRate = rate;
                minimumIndex = index;
                minimumPath = path;
            }
            if (rate > maximumRate) {
                maximumRate = rate;
                maximumIndex = index;
                maximumPath = path;
            }
        }
        const double gap = maximumRate - minimumRate;
        if (measuredQppus >= 2 && maximumRate >= 50.0 &&
            gap >= 50.0 && minimumRate <= maximumRate * 0.5) {
            const bool severe =
                minimumRate < 10.0 && maximumRate >= 80.0;
            const QJsonObject lowQppu =
                qppuConclusionByPath(minimumPath);
            const bool lowRootCause =
                lowQppu.value(QStringLiteral("state")).toString() ==
                QStringLiteral("bottleneck");
            const QString moduleKey =
                lowRootCause
                    ? lowQppu.value(
                          QStringLiteral("module_key")).toString()
                    : QString();
            const QString module =
                lowRootCause
                    ? lowQppu.value(
                          QStringLiteral("module")).toString()
                    : QString();
            QString title =
                severe
                    ? QStringLiteral("QPPU 发射负载严重失衡")
                    : QStringLiteral("QPPU 发射负载不均");
            QString conclusion =
                QStringLiteral(
                    "全局平均值掩盖了不同 QPPU 之间的吞吐差异。");
            QString action =
                QStringLiteral(
                    "检查任务分发、SG 驻留和低利用率 QPPU 的 Queue/阻塞明细。");
            if (moduleKey == QStringLiteral("dispatch")) {
                title = QStringLiteral("QPPU 工作分配不均");
                conclusion =
                    QStringLiteral(
                        "低利用率 QPPU 的主要问题发生在任务分发或 SG 驻留阶段。");
                action =
                    QStringLiteral(
                        "优先检查任务映射、SG 分发和生命周期空洞。");
            } else if (moduleKey == QStringLiteral("fe")) {
                title = QStringLiteral("FE 供给不均导致 QPPU 失衡");
                conclusion =
                    QStringLiteral(
                        "低利用率 QPPU 已有驻留任务，但前端没有持续提供指令。");
                action =
                    QStringLiteral(
                        "优先检查该 QPPU 的取指、I-cache 返回和前端 Credit。");
            } else if (!module.isEmpty() &&
                       moduleKey != QStringLiteral("none") &&
                       moduleKey != QStringLiteral("unknown")) {
                title =
                    QStringLiteral("%1限制导致 QPPU 失衡")
                        .arg(module);
                conclusion =
                    QStringLiteral(
                        "低利用率 QPPU 的内部发射链存在更直接的限制，"
                        "不能只归因为任务分配。");
                action =
                    lowQppu.value(
                        QStringLiteral("next_step")).toString();
            }
            QString evidence =
                QStringLiteral(
                    "QPPU %1 为 %2%，QPPU %3 为 %4%，差值 %5 个百分点。"
                    "低值实例：%6；高值实例：%7。")
                    .arg(minimumIndex)
                    .arg(minimumRate, 0, 'f', 1)
                    .arg(maximumIndex)
                    .arg(maximumRate, 0, 'f', 1)
                    .arg(gap, 0, 'f', 1)
                    .arg(minimumPath)
                    .arg(maximumPath);
            const QString lowReason =
                lowQppu.value(QStringLiteral("reason")).toString();
            if (!lowReason.isEmpty()) {
                evidence +=
                    QStringLiteral(" QPPU %1 二级证据：%2")
                        .arg(minimumIndex)
                        .arg(lowReason);
            }
            findings.push_back(finding(
                severe ? QStringLiteral("critical")
                       : QStringLiteral("warning"),
                QStringLiteral("qppu_imbalance"),
                title, conclusion, evidence, action,
                qMin(999, int(gap))));
        }
    }

    const QJsonObject resourcePressure =
        model.value(QStringLiteral("resource_pressure")).toObject();
    double highestPressure = 0.0;
    QString highestPressurePath;
    QString highestPressureKind;
    inspectPressureTop(
        resourcePressure.value(QStringLiteral("fifo")).toObject(),
        QStringLiteral("full_rate_percent"), QStringLiteral("FIFO"),
        highestPressure, highestPressurePath, highestPressureKind);
    inspectPressureTop(
        resourcePressure.value(QStringLiteral("queue")).toObject(),
        QStringLiteral("full_rate_percent"), QStringLiteral("Queue"),
        highestPressure, highestPressurePath, highestPressureKind);
    if (highestPressure >= 10.0) {
        const ModuleInfo module =
            moduleForPath(highestPressurePath);
        const QJsonObject owner =
            qppuConclusionByPath(highestPressurePath);
        const bool ownerUnderfilled =
            selectionCoverageComplete &&
            owner.value(
                QStringLiteral("issue_coverage_complete")).toBool() &&
            owner.value(QStringLiteral("state")).toString() ==
            QStringLiteral("bottleneck");
        const QString modulePrefix =
            module.key == QStringLiteral("unknown")
                ? QString()
                : module.name + QLatin1Char(' ');
        findings.push_back(finding(
            ownerUnderfilled && highestPressure >= 30.0
                ? QStringLiteral("critical")
                : QStringLiteral("warning"),
            QStringLiteral("buffer_pressure"),
            ownerUnderfilled
                ? QStringLiteral("%1%2 满压")
                      .arg(modulePrefix, highestPressureKind)
                : QStringLiteral("%1%2 容量风险")
                      .arg(modulePrefix, highestPressureKind),
            ownerUnderfilled
                ? QStringLiteral(
                      "该压力位于发射不足的 QPPU 内，可能正在向上游施加背压。")
                : QStringLiteral(
                      "资源压力较高，但尚未证明它限制了当前发射吞吐。"),
            QStringLiteral("%1 满率 %2%，%3。")
                .arg(highestPressureKind)
                .arg(highestPressure, 0, 'f', 1)
                .arg(highestPressurePath),
            ownerUnderfilled
                ? QStringLiteral(
                      "核对满状态与该 QPPU 未发射周期是否重合。")
                : QStringLiteral(
                      "作为容量风险监控，不直接认定为主瓶颈。"),
            qMin(999, int(highestPressure))));
    }
    double highestCreditPressure = 0.0;
    QString highestCreditPath;
    QString creditKind;
    inspectPressureTop(
        resourcePressure.value(QStringLiteral("credit")).toObject(),
        QStringLiteral("exhausted_rate_percent"), QStringLiteral("Credit"),
        highestCreditPressure, highestCreditPath, creditKind);
    if (highestCreditPressure >= 10.0) {
        const ModuleInfo module =
            moduleForPath(highestCreditPath);
        const QJsonObject owner =
            qppuConclusionByPath(highestCreditPath);
        const bool ownerUnderfilled =
            selectionCoverageComplete &&
            owner.value(
                QStringLiteral("issue_coverage_complete")).toBool() &&
            owner.value(QStringLiteral("state")).toString() ==
            QStringLiteral("bottleneck");
        const QString title =
            ownerUnderfilled
                ? (module.key == QStringLiteral("unknown")
                       ? QStringLiteral("Credit 耗尽")
                       : module.name + QStringLiteral(" Credit 耗尽"))
                : (module.key == QStringLiteral("unknown")
                       ? QStringLiteral("Credit 容量风险")
                       : module.name + QStringLiteral(" Credit 容量风险"));
        findings.push_back(finding(
            ownerUnderfilled && highestCreditPressure >= 30.0
                ? QStringLiteral("critical")
                : QStringLiteral("warning"),
            QStringLiteral("credit_exhaustion"),
            title,
            ownerUnderfilled
                ? QStringLiteral(
                      "可用 Credit 为 0 与该 QPPU 发射不足同时存在，"
                      "对应返回通路可能限制请求继续发出。")
                : QStringLiteral(
                      "Credit 压力较高，但当前证据不足以认定吞吐已受限。"),
            QStringLiteral("耗尽率 %1%，%2。")
                .arg(highestCreditPressure, 0, 'f', 1)
                .arg(highestCreditPath),
            ownerUnderfilled
                ? QStringLiteral(
                      "检查对应返回通路延迟及其与未发射周期的重合度。")
                : QStringLiteral(
                      "作为容量风险监控，不直接认定为主瓶颈。"),
            qMin(999, int(highestCreditPressure))));
    }

    const QJsonObject bandwidth =
        model.value(QStringLiteral("memory_bandwidth")).toObject();
    double highestBandwidth = 0.0;
    QString highestBandwidthLabel;
    const QJsonObject l1 = bandwidth.value(QStringLiteral("l1")).toObject();
    const QJsonObject l2 = bandwidth.value(QStringLiteral("l2")).toObject();
    inspectBandwidthDirection(l1.value(QStringLiteral("read")).toObject(),
                              QStringLiteral("L1 读"),
                              highestBandwidth, highestBandwidthLabel);
    inspectBandwidthDirection(l1.value(QStringLiteral("write")).toObject(),
                              QStringLiteral("L1 写"),
                              highestBandwidth, highestBandwidthLabel);
    inspectBandwidthDirection(l2.value(QStringLiteral("read")).toObject(),
                              QStringLiteral("L2 读"),
                              highestBandwidth, highestBandwidthLabel);
    inspectBandwidthDirection(l2.value(QStringLiteral("write")).toObject(),
                              QStringLiteral("L2 写"),
                              highestBandwidth, highestBandwidthLabel);
    if (selectionCoverageComplete && highestBandwidth >= 80.0) {
        findings.push_back(finding(
            QStringLiteral("critical"), QStringLiteral("memory_bandwidth"),
            QStringLiteral("%1带宽接近峰值").arg(highestBandwidthLabel),
            QStringLiteral("内存数据通路可能是当前吞吐上限。"),
            QStringLiteral("观测利用率为 %1%。")
                .arg(highestBandwidth, 0, 'f', 1),
            QStringLiteral("检查访存合并、有效字节率和计算/访存重叠。")));
    }

    const QJsonObject thread =
        model.value(QStringLiteral("sg_thread_efficiency")).toObject();
    if (selectionCoverageComplete &&
        thread.contains(
            QStringLiteral("overall_thread_efficiency_percent"))) {
        const double efficiency =
            thread.value(
                QStringLiteral("overall_thread_efficiency_percent")).toDouble();
        if (efficiency < 70.0) {
            findings.push_back(finding(
                QStringLiteral("warning"), QStringLiteral("thread_efficiency"),
                QStringLiteral("Thread 有效率偏低"),
                QStringLiteral("较多 valid 线程没有参与实际执行。"),
                QStringLiteral("总体 Thread 有效率为 %1%。")
                    .arg(efficiency, 0, 'f', 1),
                QStringLiteral("按 SG 排序检查分歧和无效线程来源。")));
        }
    }

    if (findings.isEmpty()) {
        findings.push_back(finding(
            QStringLiteral("info"), QStringLiteral("no_dominant_bottleneck"),
            QStringLiteral("未发现单一主导瓶颈"),
            QStringLiteral("当前观测指标没有达到明确的瓶颈阈值。"),
            QStringLiteral("发射、调度、缓冲和带宽指标均未形成强证据。"),
            QStringLiteral("在时间线中选择吞吐下降区间重新分析。")));
    }
    return sortedFindings(findings);
}

bool performanceDiagnosisSelfTest(QString& error) {
    QJsonObject analysis;
    analysis.insert(QStringLiteral("duration_cycles"), 100.0);
    analysis.insert(QStringLiteral("dynamic_signals"), 1);

    QJsonObject qppu;
    qppu.insert(QStringLiteral("path"),
                QStringLiteral("gpu.m_QPPUTOP.[0]"));
    qppu.insert(QStringLiteral("index"), 0);
    qppu.insert(QStringLiteral("issue_coverage_complete"), false);
    qppu.insert(QStringLiteral("issue_observed_cycles"), 50.0);
    qppu.insert(QStringLiteral("issue_utilization_percent"), 200.0);
    qppu.insert(QStringLiteral("issue_active_percent"), 100.0);
    qppu.insert(QStringLiteral("issue_active_cycles"), 50.0);
    qppu.insert(QStringLiteral("issue_idle_cycles"), 0.0);
    qppu.insert(QStringLiteral("issued_instructions_estimate"), 100.0);
    qppu.insert(QStringLiteral("activity_coverage_complete"), true);
    qppu.insert(QStringLiteral("eligibility_coverage_complete"), true);
    qppu.insert(QStringLiteral("active_sg_cycles"), 100.0);
    qppu.insert(QStringLiteral("queue_ready_sg_cycles"), 100.0);
    qppu.insert(QStringLiteral("eligible_sg_cycles"), 100.0);
    qppu.insert(QStringLiteral("eligible_percent"), 100.0);
    qppu.insert(QStringLiteral("shader_groups"),
                QJsonArray{QJsonObject()});
    qppu.insert(QStringLiteral("block_reasons"), QJsonArray());

    QJsonObject schedulerSummary;
    schedulerSummary.insert(QStringLiteral("active_percent"), 100.0);
    schedulerSummary.insert(QStringLiteral("queue_ready_percent"), 100.0);
    schedulerSummary.insert(QStringLiteral("eligible_percent"), 100.0);
    QJsonObject scheduler;
    scheduler.insert(QStringLiteral("status"),
                     QStringLiteral("partial"));
    scheduler.insert(QStringLiteral("summary"), schedulerSummary);
    scheduler.insert(QStringLiteral("qppus"), QJsonArray{qppu});

    QJsonObject summary;
    summary.insert(QStringLiteral("issue_utilization_percent"), 200.0);
    summary.insert(QStringLiteral("issue_classes"), QJsonArray());

    QJsonObject model;
    model.insert(QStringLiteral("analysis"), analysis);
    model.insert(QStringLiteral("scheduler"), scheduler);
    model.insert(QStringLiteral("summary"), summary);
    model.insert(QStringLiteral("resource_pressure"), QJsonObject());
    model.insert(QStringLiteral("memory_bandwidth"), QJsonObject());

    const QJsonArray conclusions = buildQppuConclusions(model);
    const QJsonObject workload =
        buildWorkloadProfile(model, conclusions);
    if (conclusions.isEmpty() ||
        conclusions.first().toObject()
                .value(QStringLiteral("issue_coverage_complete")).toBool() ||
        conclusions.first().toObject()
                .value(QStringLiteral("state")).toString() !=
            QStringLiteral("uncovered") ||
        conclusions.first().toObject().contains(
            QStringLiteral("issue_idle_cycles")) ||
        workload.value(QStringLiteral("regime")).toString() !=
            QStringLiteral("partial_coverage") ||
        workload.value(
            QStringLiteral("issue_coverage_complete")).toBool()) {
        error = QStringLiteral(
            "partial issue coverage produced a bottleneck diagnosis");
        return false;
    }

    model.insert(QStringLiteral("workload_profile"), workload);
    const QJsonArray findings = buildPerformanceFindings(model);
    bool foundCoverageFinding = false;
    for (const QJsonValue& value : findings) {
        if (value.toObject().value(QStringLiteral("key")).toString() ==
            QStringLiteral("performance_coverage")) {
            foundCoverageFinding = true;
            break;
        }
    }
    if (!foundCoverageFinding) {
        error = QStringLiteral(
            "partial issue coverage did not produce a coverage finding");
        return false;
    }

    QJsonObject unknownMemoryModel = model;
    QJsonObject unknownMemoryQppu = qppu;
    unknownMemoryQppu.insert(
        QStringLiteral("issue_coverage_complete"), true);
    unknownMemoryQppu.insert(
        QStringLiteral("issue_observed_cycles"), 100.0);
    unknownMemoryQppu.insert(
        QStringLiteral("issue_utilization_percent"), 20.0);
    unknownMemoryQppu.insert(
        QStringLiteral("issue_active_percent"), 20.0);
    unknownMemoryQppu.insert(
        QStringLiteral("issue_active_cycles"), 20.0);
    unknownMemoryQppu.insert(
        QStringLiteral("issue_idle_cycles"), 80.0);
    unknownMemoryQppu.insert(
        QStringLiteral("issued_instructions_estimate"), 20.0);
    unknownMemoryQppu.insert(
        QStringLiteral("eligible_sg_cycles"), 20.0);
    unknownMemoryQppu.insert(
        QStringLiteral("eligible_percent"), 20.0);
    QJsonObject dependencyBlockObject;
    dependencyBlockObject.insert(
        QStringLiteral("key"),
        QStringLiteral("operand_not_ready"));
    dependencyBlockObject.insert(
        QStringLiteral("name"),
        QStringLiteral("数据依赖"));
    dependencyBlockObject.insert(
        QStringLiteral("cycles"), 60.0);
    dependencyBlockObject.insert(
        QStringLiteral("queue_ready_percent"), 60.0);
    unknownMemoryQppu.insert(
        QStringLiteral("block_reasons"),
        QJsonArray{dependencyBlockObject});
    QJsonObject unknownMemoryScheduler = scheduler;
    unknownMemoryScheduler.insert(
        QStringLiteral("status"), QStringLiteral("measured"));
    unknownMemoryScheduler.insert(
        QStringLiteral("qppus"), QJsonArray{unknownMemoryQppu});
    unknownMemoryModel.insert(
        QStringLiteral("scheduler"), unknownMemoryScheduler);
    QJsonObject unknownMemorySummary = summary;
    unknownMemorySummary.insert(
        QStringLiteral("issue_utilization_percent"), 20.0);
    unknownMemorySummary.insert(
        QStringLiteral("issued_instructions_estimate"), 20.0);
    unknownMemorySummary.insert(
        QStringLiteral("global_memory_issue_estimate"), 20.0);
    unknownMemorySummary.insert(
        QStringLiteral("local_memory_issue_estimate"), 0.0);
    unknownMemorySummary.insert(
        QStringLiteral("memory_issue_signals_covered"), false);
    unknownMemoryModel.insert(
        QStringLiteral("summary"), unknownMemorySummary);
    unknownMemoryModel.remove(QStringLiteral("workload_profile"));
    const QJsonArray unknownMemoryConclusions =
        buildQppuConclusions(unknownMemoryModel);
    const QJsonObject unknownMemoryWorkload =
        buildWorkloadProfile(
            unknownMemoryModel, unknownMemoryConclusions);
    if (unknownMemoryWorkload
            .value(QStringLiteral("regime")).toString() !=
        QStringLiteral("dependency_latency")) {
        error = QStringLiteral(
            "uncovered memory classification produced a memory bottleneck");
        return false;
    }
    QJsonObject stableActivityModel = unknownMemoryModel;
    QJsonObject stableActivityAnalysis =
        stableActivityModel.value(
            QStringLiteral("analysis")).toObject();
    stableActivityAnalysis.insert(
        QStringLiteral("dynamic_signals"), 0);
    stableActivityModel.insert(
        QStringLiteral("analysis"), stableActivityAnalysis);
    const QJsonArray stableActivityConclusions =
        buildQppuConclusions(stableActivityModel);
    const QJsonObject stableActivityWorkload =
        buildWorkloadProfile(
            stableActivityModel, stableActivityConclusions);
    if (stableActivityConclusions.isEmpty() ||
        stableActivityWorkload
            .value(QStringLiteral("status")).toString() ==
            QStringLiteral("static_snapshot") ||
        stableActivityWorkload
            .value(QStringLiteral("regime")).toString() !=
            QStringLiteral("dependency_latency")) {
        error = QStringLiteral(
            "stable active signals were treated as a static snapshot");
        return false;
    }

    QJsonObject unknownActivityModel = unknownMemoryModel;
    QJsonObject unknownActivityQppu = unknownMemoryQppu;
    unknownActivityQppu.insert(
        QStringLiteral("issue_coverage_complete"), false);
    unknownActivityQppu.insert(
        QStringLiteral("issue_observed_cycles"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("issue_utilization_percent"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("issue_active_cycles"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("issue_active_percent"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("issue_idle_cycles"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("issued_instructions_estimate"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("activity_coverage_complete"), false);
    unknownActivityQppu.insert(
        QStringLiteral("eligibility_coverage_complete"), false);
    unknownActivityQppu.insert(
        QStringLiteral("active_sg_cycles"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("queue_ready_sg_cycles"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("eligible_sg_cycles"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("eligible_percent"), 0.0);
    unknownActivityQppu.insert(
        QStringLiteral("block_reasons"), QJsonArray());
    QJsonObject unknownActivityScheduler = scheduler;
    unknownActivityScheduler.insert(
        QStringLiteral("status"), QStringLiteral("partial"));
    unknownActivityScheduler.insert(
        QStringLiteral("qppus"), QJsonArray{unknownActivityQppu});
    unknownActivityModel.insert(
        QStringLiteral("scheduler"), unknownActivityScheduler);
    QJsonObject unknownActivitySummary = summary;
    unknownActivitySummary.insert(
        QStringLiteral("issue_activity_coverage_complete"), false);
    unknownActivityModel.insert(
        QStringLiteral("summary"), unknownActivitySummary);
    unknownActivityModel.remove(QStringLiteral("workload_profile"));
    const QJsonArray unknownActivityConclusions =
        buildQppuConclusions(unknownActivityModel);
    const QJsonObject unknownActivityWorkload =
        buildWorkloadProfile(
            unknownActivityModel, unknownActivityConclusions);
    if (unknownActivityWorkload
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("partial") ||
        unknownActivityWorkload
            .value(QStringLiteral("regime")).toString() !=
            QStringLiteral("partial_coverage")) {
        error = QStringLiteral(
            "unknown zero activity produced status/regime %1/%2")
                    .arg(unknownActivityWorkload
                             .value(QStringLiteral("status")).toString())
                    .arg(unknownActivityWorkload
                             .value(QStringLiteral("regime")).toString());
        return false;
    }

    QJsonObject completeIdleModel = unknownActivityModel;
    QJsonObject completeIdleQppu = unknownActivityQppu;
    completeIdleQppu.insert(
        QStringLiteral("issue_coverage_complete"), true);
    completeIdleQppu.insert(
        QStringLiteral("issue_observed_cycles"), 100.0);
    completeIdleQppu.insert(
        QStringLiteral("issue_idle_cycles"), 100.0);
    completeIdleQppu.insert(
        QStringLiteral("activity_coverage_complete"), true);
    completeIdleQppu.insert(
        QStringLiteral("eligibility_coverage_complete"), true);
    QJsonObject completeIdleScheduler = unknownActivityScheduler;
    completeIdleScheduler.insert(
        QStringLiteral("status"), QStringLiteral("measured"));
    completeIdleScheduler.insert(
        QStringLiteral("qppus"), QJsonArray{completeIdleQppu});
    completeIdleModel.insert(
        QStringLiteral("scheduler"), completeIdleScheduler);
    QJsonObject completeIdleSummary = unknownActivitySummary;
    completeIdleSummary.insert(
        QStringLiteral("issue_activity_coverage_complete"), true);
    completeIdleModel.insert(
        QStringLiteral("summary"), completeIdleSummary);
    const QJsonArray completeIdleConclusions =
        buildQppuConclusions(completeIdleModel);
    const QJsonObject completeIdleWorkload =
        buildWorkloadProfile(
            completeIdleModel, completeIdleConclusions);
    if (completeIdleWorkload
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("no_activity") ||
        completeIdleWorkload
            .value(QStringLiteral("regime")).toString() !=
            QStringLiteral("no_activity")) {
        error = QStringLiteral(
            "fully covered idle workload was not reported as no activity");
        return false;
    }

    QJsonObject partialSchedulerModel = model;
    QJsonObject partialSchedulerQppu = qppu;
    partialSchedulerQppu.insert(
        QStringLiteral("issue_coverage_complete"), true);
    partialSchedulerQppu.insert(
        QStringLiteral("issue_observed_cycles"), 100.0);
    partialSchedulerQppu.insert(
        QStringLiteral("issue_utilization_percent"), 20.0);
    partialSchedulerQppu.insert(
        QStringLiteral("issue_active_percent"), 20.0);
    partialSchedulerQppu.insert(
        QStringLiteral("issue_active_cycles"), 20.0);
    partialSchedulerQppu.insert(
        QStringLiteral("issue_idle_cycles"), 80.0);
    partialSchedulerQppu.insert(
        QStringLiteral("issued_instructions_estimate"), 20.0);
    partialSchedulerQppu.insert(
        QStringLiteral("activity_coverage_complete"), false);
    partialSchedulerQppu.insert(
        QStringLiteral("eligibility_coverage_complete"), false);
    partialSchedulerQppu.insert(
        QStringLiteral("active_sg_cycles"), 0.0);
    partialSchedulerQppu.insert(
        QStringLiteral("queue_ready_sg_cycles"), 100.0);
    partialSchedulerQppu.insert(
        QStringLiteral("eligible_sg_cycles"), 100.0);
    partialSchedulerQppu.insert(
        QStringLiteral("eligible_percent"), 100.0);
    partialSchedulerQppu.insert(
        QStringLiteral("block_reasons"),
        QJsonArray{dependencyBlockObject});
    QJsonObject partialScheduler = scheduler;
    QJsonObject partialSchedulerSummary = schedulerSummary;
    partialSchedulerSummary.insert(
        QStringLiteral("activity_coverage_complete"), false);
    partialSchedulerSummary.insert(
        QStringLiteral("eligibility_coverage_complete"), false);
    partialSchedulerSummary.insert(
        QStringLiteral("active_percent"), 0.0);
    partialSchedulerSummary.insert(
        QStringLiteral("queue_ready_percent"), 0.0);
    partialSchedulerSummary.insert(
        QStringLiteral("eligible_percent"), 100.0);
    partialScheduler.insert(
        QStringLiteral("status"), QStringLiteral("measured"));
    partialScheduler.insert(
        QStringLiteral("summary"), partialSchedulerSummary);
    partialScheduler.insert(
        QStringLiteral("qppus"),
        QJsonArray{partialSchedulerQppu});
    partialSchedulerModel.insert(
        QStringLiteral("scheduler"), partialScheduler);
    QJsonObject partialSchedulerRootSummary = summary;
    partialSchedulerRootSummary.insert(
        QStringLiteral("issue_activity_coverage_complete"), true);
    partialSchedulerModel.insert(
        QStringLiteral("summary"), partialSchedulerRootSummary);
    partialSchedulerModel.remove(QStringLiteral("workload_profile"));
    const QJsonArray partialSchedulerConclusions =
        buildQppuConclusions(partialSchedulerModel);
    const QJsonObject partialSchedulerWorkload =
        buildWorkloadProfile(
            partialSchedulerModel, partialSchedulerConclusions);
    if (partialSchedulerWorkload
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("partial") ||
        partialSchedulerWorkload
            .value(QStringLiteral("regime")).toString() ==
            QStringLiteral("dependency_latency") ||
        partialSchedulerWorkload
            .value(QStringLiteral("regime")).toString() ==
            QStringLiteral("scheduler_bound")) {
        error = QStringLiteral(
            "partial scheduler coverage produced a workload bottleneck");
        return false;
    }
    partialSchedulerModel.insert(
        QStringLiteral("qppu_conclusions"),
        partialSchedulerConclusions);
    partialSchedulerModel.insert(
        QStringLiteral("workload_profile"),
        partialSchedulerWorkload);
    const QJsonArray partialSchedulerFindings =
        buildPerformanceFindings(partialSchedulerModel);
    bool foundSchedulerCoverage = false;
    for (const QJsonValue& value : partialSchedulerFindings) {
        const QString key =
            value.toObject().value(QStringLiteral("key")).toString();
        if (key == QStringLiteral("scheduler_coverage")) {
            foundSchedulerCoverage = true;
        }
        if (key == QStringLiteral("workload_fill") ||
            key == QStringLiteral("frontend_supply") ||
            key == QStringLiteral("scheduler_block") ||
            key == QStringLiteral("issue_underfill") ||
            key == QStringLiteral("dependency_latency")) {
            error = QStringLiteral(
                "partial scheduler coverage produced a false finding");
            return false;
        }
    }
    if (!foundSchedulerCoverage) {
        error = QStringLiteral(
            "partial scheduler coverage did not produce a coverage finding");
        return false;
    }

    QJsonObject truncatedModel = unknownMemoryModel;
    QJsonObject truncatedCoverage;
    truncatedCoverage.insert(
        QStringLiteral("signal_selection_complete"), false);
    truncatedCoverage.insert(
        QStringLiteral("signal_selection_count"), 40);
    truncatedCoverage.insert(
        QStringLiteral("signal_selection_limit"), 40);
    truncatedModel.insert(
        QStringLiteral("coverage"), truncatedCoverage);
    QJsonObject truncatedFifoEntry;
    truncatedFifoEntry.insert(
        QStringLiteral("path"),
        QStringLiteral("gpu.m_QPPUTOP.[0].m_QPPUEU.test_fifo"));
    truncatedFifoEntry.insert(
        QStringLiteral("full_rate_percent"), 60.0);
    truncatedFifoEntry.insert(
        QStringLiteral("full_cycles"), 60.0);
    QJsonObject truncatedFifo;
    truncatedFifo.insert(
        QStringLiteral("top"),
        QJsonArray{truncatedFifoEntry});
    QJsonObject truncatedPressure;
    truncatedPressure.insert(
        QStringLiteral("fifo"), truncatedFifo);
    truncatedModel.insert(
        QStringLiteral("resource_pressure"), truncatedPressure);
    truncatedModel.remove(QStringLiteral("workload_profile"));
    const QJsonArray truncatedConclusions =
        buildQppuConclusions(truncatedModel);
    const QJsonObject truncatedWorkload =
        buildWorkloadProfile(
            truncatedModel, truncatedConclusions);
    if (truncatedWorkload
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("partial") ||
        truncatedWorkload
            .value(QStringLiteral("regime")).toString() !=
            QStringLiteral("partial_selection") ||
        truncatedWorkload
            .value(
                QStringLiteral("selection_coverage_complete")).toBool()) {
        error = QStringLiteral(
            "truncated signal selection was treated as complete");
        return false;
    }
    truncatedModel.insert(
        QStringLiteral("qppu_conclusions"),
        truncatedConclusions);
    truncatedModel.insert(
        QStringLiteral("workload_profile"),
        truncatedWorkload);
    const QJsonArray truncatedFindings =
        buildPerformanceFindings(truncatedModel);
    bool foundSelectionCoverage = false;
    if (truncatedFindings.isEmpty() ||
        truncatedFindings.first().toObject()
                .value(QStringLiteral("key")).toString() !=
            QStringLiteral("performance_coverage")) {
        error = QStringLiteral(
            "local evidence outranked truncated selection coverage");
        return false;
    }
    for (const QJsonValue& value : truncatedFindings) {
        const QJsonObject object = value.toObject();
        const QString key =
            object.value(QStringLiteral("key")).toString();
        if (key == QStringLiteral("performance_coverage")) {
            foundSelectionCoverage = true;
        }
        if (key == QStringLiteral("dependency_latency") ||
            key == QStringLiteral("memory_latency") ||
            key == QStringLiteral("scheduler_coverage") ||
            key == QStringLiteral("scheduler_block") ||
            key == QStringLiteral("issue_underfill") ||
            key == QStringLiteral("qppu_imbalance")) {
            error = QStringLiteral(
                "truncated signal selection produced a false finding");
            return false;
        }
        if (object.value(
                QStringLiteral("severity")).toString() ==
            QStringLiteral("critical")) {
            error = QStringLiteral(
                "truncated local evidence was promoted to critical");
            return false;
        }
    }
    if (!foundSelectionCoverage) {
        error = QStringLiteral(
            "truncated signal selection did not produce a coverage finding");
        return false;
    }
    return true;
}

QString buildPerformanceConclusion(const QJsonObject& model,
                                   const QJsonArray& findings) {
    Q_UNUSED(model);
    if (findings.isEmpty()) return QStringLiteral("没有可用结论");
    QJsonObject best;
    int bestScore = (std::numeric_limits<int>::min)();
    for (const QJsonValue& value : findings) {
        const QJsonObject candidate = value.toObject();
        const int score =
            candidate.value(QStringLiteral("priority_score")).toInt(
                severityRank(
                    candidate.value(
                        QStringLiteral("severity")).toString()) *
                1000);
        if (score > bestScore) {
            best = candidate;
            bestScore = score;
        }
    }
    return best.value(QStringLiteral("title")).toString() +
           QStringLiteral("。") +
           best.value(QStringLiteral("conclusion")).toString();
}

}  // namespace waveperf
