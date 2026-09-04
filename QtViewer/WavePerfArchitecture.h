#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace waveperf {

constexpr quint64 kInstIssueTypeNotIssue = 0;

enum class EventSemantics {
    None,
    PerCycleValue,
    PerCycleMask,
    CumulativeCounter
};

struct ClassifiedSignal {
    QString key;
    QString category;
    bool helper = false;
    EventSemantics eventSemantics = EventSemantics::None;
};

// Predecode fields are admitted here only after both conditions are met:
// 1. the model source shows how the field is assigned/consumed; and
// 2. the field exists under the emitted issue/queue instruction object.
// Keep this as the single source of truth for issue and queue-head analysis.
struct InstructionFeatureSpec {
    QString key;
    QString fieldPath;
    QString group;
};

const QVector<InstructionFeatureSpec>& instructionFeatureSpecs();

// Numeric enum values are persisted in WVZ4. Keep their decoding centralized
// and covered by self-tests so a model enum reorder cannot silently corrupt a
// performance diagnosis.
QString instIssueTypeName(quint64 value);
QString instIssueClassKey(quint64 value);
QString cbCtrlInstClientName(quint64 value);
QString cbDataClientName(quint64 value);
QString cbDataInstQueueClientName(quint64 value);

struct CounterView {
    QString path;
    QString key;
    QString category;
    qint64 activeTicks = 0;
    qint64 unknownTicks = 0;
    qint64 knownTicks = 0;
    qint64 transitions = 0;
    quint64 eventCount = 0;
    qint64 eventDiscontinuities = 0;
    long double weightedValueTicks = 0.0L;
    qint64 fifoFullTicks = 0;
    qint64 fifoFullKnownTicks = 0;
    qint64 queueFullTicks = 0;
    qint64 queueFullKnownTicks = 0;
};

struct IssueContextView {
    QString path;
    qint64 issuedTicks = 0;
    qint64 issueActiveTicks = 0;
    qint64 dualIssueTicks = 0;
    qint64 idleTicks = 0;
    qint64 capacityTicks = 0;
    QVector<qint64> slotActiveTicks;
    QVector<qint64> slotCapacityTicks;
};

struct ArchitectureProfile {
    QJsonArray roots;
    QJsonArray classSummaries;
    QStringList warnings;
    int observedInstances = 0;
    quint64 representedInstances = 0;
    bool hasRepresentativeArrays = false;
};

// Runtime-reflected pointer/annotated arrays may encode an index in the
// member segment ("items[0]"), while native arrays use a child segment
// ("items.[0]").  WavePerf uses the latter as its canonical representation.
QString canonicalArchitecturePath(const QString& path);

ClassifiedSignal classifyArchitectureSignal(const QString& path);

ArchitectureProfile buildArchitectureProfile(const QVector<CounterView>& counters,
                                             const QVector<IssueContextView>& issueContexts,
                                             qint64 durationTicks,
                                             qint64 ticksPerCycle);

bool architectureProfilerSelfTest(QString& error);

}  // namespace waveperf
