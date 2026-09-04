#pragma once

#include "WaveTypes.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace waveperf {

QJsonObject buildSchedulerProfile(
    const QHash<QString, const WaveSignal*>& signalsByPath,
    qint64 startTick,
    qint64 endTick,
    qint64 ticksPerCycle,
    int timelineBinCount,
    QStringList& warnings);

bool schedulerProfilerSelfTest(QString& error);

}  // namespace waveperf
