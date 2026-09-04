#pragma once

#include "WaveTypes.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace wavefifo {

enum class OccupancyKind {
    NumReadable,
    NumAvail,
    Count
};

enum class ResourceKind {
    Fifo,
    Queue
};

struct ResourceDescriptor {
    QString path;
    ResourceKind resourceKind = ResourceKind::Fifo;
    OccupancyKind occupancyKind = OccupancyKind::NumReadable;
    int occupancySignalId = -1;
    int capacitySignalId = -1;
    bool representativeOnly = false;
};

struct RejectedCandidate {
    QString path;
    QString reason;
};

struct DiscoveryResult {
    QVector<ResourceDescriptor> resources;
    QVector<RejectedCandidate> rejected;
    QStringList warnings;
    quint64 signalsScanned = 0;
    quint64 fullPathsBuilt = 0;
};

struct FullWindow {
    qint64 fullTicks = 0;
    qint64 knownTicks = 0;
    qint64 expectedTicks = 0;
    long double occupancyWeightedTicks = 0.0L;
    long double capacityWeightedTicks = 0.0L;
};

QString occupancyKindKey(OccupancyKind kind);
QString occupancyKindLabel(OccupancyKind kind);
QString resourceKindKey(ResourceKind kind);
QString resourceKindLabel(ResourceKind kind);
using DiscoveryProgressCallback =
    std::function<void(quint64 completedSignals, quint64 totalSignals)>;

DiscoveryResult discoverResources(
    const WaveFile& directory,
    const DiscoveryProgressCallback& progress = DiscoveryProgressCallback());
QVector<int> requiredSignalIds(const QVector<ResourceDescriptor>& resources);
FullWindow analyzeFullWindow(const WaveSignal* occupancy,
                             const WaveSignal* capacity,
                             qint64 start,
                             qint64 end);

}  // namespace wavefifo
