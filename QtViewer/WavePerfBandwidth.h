#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct WaveSignal;

namespace waveperf {

bool isMemoryBandwidthSignal(const QString& path);

QJsonObject buildMemoryBandwidthProfile(
    const QHash<QString, const WaveSignal*>& signalsByPath,
    qint64 startTick,
    qint64 endTick,
    qint64 ticksPerCycle,
    bool hasDynamicWave,
    QStringList& warnings);

bool memoryBandwidthProfilerSelfTest(QString& error);

}  // namespace waveperf
