#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace waveperf {

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

ClassifiedSignal classifyArchitectureSignal(const QString& path);

ArchitectureProfile buildArchitectureProfile(const QVector<CounterView>& counters,
                                             const QVector<IssueContextView>& issueContexts,
                                             qint64 durationTicks,
                                             qint64 ticksPerCycle);

bool architectureProfilerSelfTest(QString& error);

}  // namespace waveperf
