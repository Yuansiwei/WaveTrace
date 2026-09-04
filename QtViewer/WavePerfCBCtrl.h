#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct WaveSignal;

namespace waveperf {

// These two lightweight predicates are used before full path materialization so
// large flattened waveforms only decode CBCtrl payload fields that the profiler
// can consume.
bool isCBCtrlDetailLeafName(const QByteArray& lowerName);
bool isCBCtrlEndpointSegment(const QByteArray& lowerName);
bool isCBCtrlDetailSignalPath(const QString& path);

QJsonObject buildCBCtrlProfile(
    const QHash<QString, const WaveSignal*>& signalsByPath,
    qint64 startTick,
    qint64 endTick,
    qint64 ticksPerCycle,
    bool hasDynamicWave,
    QStringList& warnings);

bool cbCtrlProfilerSelfTest(QString& error);

}  // namespace waveperf
