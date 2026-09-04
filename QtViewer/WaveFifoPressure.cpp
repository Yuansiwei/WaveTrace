#include "WaveFifoPressure.h"

#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace wavefifo {
namespace {

struct Candidate {
    QMap<QString, QVector<int>> fields;
};

struct ValueState {
    bool known = false;
    quint64 value = 0;
};

QString parentPath(const QString& path) {
    const int separator = path.lastIndexOf(QLatin1Char('.'));
    return separator >= 0 ? path.left(separator) : path;
}

QString trackedFieldName(const QString& segment) {
    static const char* const fields[] = {
        "m_num_readable", "m_numavail", "m_count", "m_size"};
    const int separator = segment.lastIndexOf(QLatin1Char('.'));
    const int begin = separator >= 0 ? separator + 1 : 0;
    const QStringRef leaf(&segment, begin, segment.size() - begin);
    for (const char* field : fields) {
        if (leaf.compare(QLatin1String(field), Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(field);
    }
    return QString();
}

ValueState stateFromSample(const WaveSignal& signal,
                           const WaveSample& input) {
    WaveSample sample = input;
    if (!sample.rawFieldsReady) {
        hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
    }
    if (sample.isZ || sample.isAbsent) return ValueState();
    ValueState state;
    state.known = true;
    state.value = sample.rawBits & waveBitMaskForWidth(signal.width);
    return state;
}

int firstSampleAfter(const WaveSignal& signal, qint64 time) {
    int lo = 0;
    int hi = signal.samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (signal.samples.at(mid).time <= time)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

}  // namespace

QString occupancyKindKey(OccupancyKind kind) {
    switch (kind) {
    case OccupancyKind::NumReadable:
        return QStringLiteral("m_num_readable");
    case OccupancyKind::NumAvail:
        return QStringLiteral("m_numAvail");
    case OccupancyKind::Count:
        return QStringLiteral("m_count");
    }
    return QStringLiteral("unknown");
}

QString occupancyKindLabel(OccupancyKind kind) {
    switch (kind) {
    case OccupancyKind::NumReadable:
        return QStringLiteral("readable FIFO");
    case OccupancyKind::NumAvail:
        return QStringLiteral("available-count FIFO");
    case OccupancyKind::Count:
        return QStringLiteral("counted queue");
    }
    return QStringLiteral("unknown");
}

QString resourceKindKey(ResourceKind kind) {
    return kind == ResourceKind::Queue ? QStringLiteral("queue")
                                       : QStringLiteral("fifo");
}

QString resourceKindLabel(ResourceKind kind) {
    return kind == ResourceKind::Queue ? QStringLiteral("Queue")
                                       : QStringLiteral("FIFO");
}

DiscoveryResult discoverResources(
    const WaveFile& directory,
    const DiscoveryProgressCallback& progress) {
    DiscoveryResult result;
    QHash<QString, Candidate> candidates;
    const int signalCount = waveSignalCount(directory.signalList);
    for (int index = 0; index < signalCount; ++index) {
        const QString leaf =
            trackedFieldName(waveSignalSegmentName(directory, index));
        if (!leaf.isEmpty()) {
            const QString path = waveSignalFullPath(directory, index);
            ++result.fullPathsBuilt;
            Candidate& candidate = candidates[parentPath(path)];
            candidate.fields[leaf].push_back(
                directory.signalList.at(static_cast<std::size_t>(index))
                    .signalId);
        }
        ++result.signalsScanned;
        if (progress &&
            (((index + 1) & 0xfffu) == 0 || index + 1 == signalCount)) {
            progress(static_cast<quint64>(index + 1),
                     static_cast<quint64>(signalCount));
        }
    }
    if (progress && signalCount == 0) progress(0, 0);

    const QStringList occupancyFields = {
        QStringLiteral("m_num_readable"), QStringLiteral("m_numavail"),
        QStringLiteral("m_count")};
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        const QString& path = it.key();
        const Candidate& candidate = it.value();
        int occupancyKinds = 0;
        QString occupancyField;
        for (const QString& field : occupancyFields) {
            if (candidate.fields.contains(field)) {
                ++occupancyKinds;
                occupancyField = field;
            }
        }
        if (occupancyKinds == 0 && !candidate.fields.contains(
                                       QStringLiteral("m_size"))) {
            continue;
        }
        if (occupancyKinds == 0) {
            result.rejected.push_back(
                {path, QStringLiteral("存在 m_size，但缺少三类占用字段")});
            continue;
        }
        if (!candidate.fields.contains(QStringLiteral("m_size"))) {
            result.rejected.push_back(
                {path, QStringLiteral("存在占用字段，但缺少 m_size")});
            continue;
        }
        if (occupancyKinds != 1) {
            result.rejected.push_back(
                {path, QStringLiteral("同时存在多个占用字段，拒绝猜测")});
            continue;
        }
        if (candidate.fields.value(occupancyField).size() != 1 ||
            candidate.fields.value(QStringLiteral("m_size")).size() != 1) {
            result.rejected.push_back(
                {path, QStringLiteral("占用字段或 m_size 重复，拒绝猜测")});
            continue;
        }

        ResourceDescriptor descriptor;
        descriptor.path = path;
        descriptor.occupancySignalId =
            candidate.fields.value(occupancyField).first();
        descriptor.capacitySignalId =
            candidate.fields.value(QStringLiteral("m_size")).first();
        if (occupancyField == QStringLiteral("m_num_readable"))
            descriptor.occupancyKind = OccupancyKind::NumReadable;
        else if (occupancyField == QStringLiteral("m_numavail"))
            descriptor.occupancyKind = OccupancyKind::NumAvail;
        else
            descriptor.occupancyKind = OccupancyKind::Count;
        descriptor.resourceKind =
            descriptor.occupancyKind == OccupancyKind::Count
                ? ResourceKind::Queue
                : ResourceKind::Fifo;

        descriptor.representativeOnly =
            path.contains(QRegularExpression(
                QStringLiteral("\\[size=\\d+\\]\\.\\[0\\](?:\\.|$)")));
        result.resources.push_back(descriptor);
    }

    std::sort(result.resources.begin(), result.resources.end(),
              [](const ResourceDescriptor& left,
                 const ResourceDescriptor& right) {
                  return left.path < right.path;
              });
    std::sort(result.rejected.begin(), result.rejected.end(),
              [](const RejectedCandidate& left,
                 const RejectedCandidate& right) {
                  return left.path == right.path
                             ? left.reason < right.reason
                             : left.path < right.path;
              });
    result.warnings.sort();
    return result;
}

QVector<int> requiredSignalIds(
    const QVector<ResourceDescriptor>& resources) {
    QSet<int> seen;
    QVector<int> ids;
    ids.reserve(resources.size() * 2);
    for (const ResourceDescriptor& resource : resources) {
        const int pair[] = {resource.occupancySignalId,
                            resource.capacitySignalId};
        for (int id : pair) {
            if (id >= 0 && !seen.contains(id)) {
                seen.insert(id);
                ids.push_back(id);
            }
        }
    }
    return ids;
}

FullWindow analyzeFullWindow(const WaveSignal* occupancy,
                             const WaveSignal* capacity,
                             qint64 start,
                             qint64 end) {
    FullWindow result;
    if (!occupancy || !capacity) return result;
    if (occupancy->samples.isEmpty() || capacity->samples.isEmpty())
        return result;

    const qint64 firstEvent =
        qMin(occupancy->samples.first().time, capacity->samples.first().time);
    const qint64 lastEvent =
        qMax(occupancy->samples.last().time, capacity->samples.last().time);
    start = qMax(start, firstEvent);
    end = qMin(end, lastEvent);
    result.expectedTicks = qMax<qint64>(0, end - start);
    if (end <= start) return result;

    int occupancyIndex = firstSampleAfter(*occupancy, start);
    int capacityIndex = firstSampleAfter(*capacity, start);
    ValueState occupancyState =
        occupancyIndex > 0
            ? stateFromSample(*occupancy,
                              occupancy->samples.at(occupancyIndex - 1))
            : ValueState();
    ValueState capacityState =
        capacityIndex > 0
            ? stateFromSample(*capacity,
                              capacity->samples.at(capacityIndex - 1))
            : ValueState();
    qint64 cursor = start;
    while (cursor < end) {
        while (occupancyIndex < occupancy->samples.size() &&
               occupancy->samples.at(occupancyIndex).time <= cursor) {
            occupancyState = stateFromSample(
                *occupancy, occupancy->samples.at(occupancyIndex++));
        }
        while (capacityIndex < capacity->samples.size() &&
               capacity->samples.at(capacityIndex).time <= cursor) {
            capacityState = stateFromSample(
                *capacity, capacity->samples.at(capacityIndex++));
        }

        qint64 next = end;
        if (occupancyIndex < occupancy->samples.size())
            next = qMin(next,
                        occupancy->samples.at(occupancyIndex).time);
        if (capacityIndex < capacity->samples.size())
            next = qMin(next, capacity->samples.at(capacityIndex).time);
        if (next <= cursor) continue;

        if (!occupancyState.known || !capacityState.known ||
            capacityState.value == 0) {
            cursor = next;
            continue;
        }
        const qint64 ticks = next - cursor;
        result.knownTicks += ticks;
        result.occupancyWeightedTicks +=
            static_cast<long double>(occupancyState.value) * ticks;
        result.capacityWeightedTicks +=
            static_cast<long double>(capacityState.value) * ticks;
        if (occupancyState.value >= capacityState.value)
            result.fullTicks += ticks;
        cursor = next;
    }
    return result;
}

}  // namespace wavefifo
