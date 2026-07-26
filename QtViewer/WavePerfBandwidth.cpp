#include "WavePerfBandwidth.h"

#include "WaveTypes.h"

#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace waveperf {
namespace {

struct ValueState {
    bool known = false;
    quint64 value = 0;
};

struct ByteAccumulator {
    long double byteTicks = 0.0L;
    qint64 transferTicks = 0;
    qint64 knownTicks = 0;
    int streams = 0;
};

struct L1LatencyStream {
    QString key;
    QString requestTakenPath;
    QString responseReadyPath;
    const WaveSignal* requestValid = nullptr;
    const WaveSignal* requestTaken = nullptr;
    const WaveSignal* storeMask = nullptr;
    const WaveSignal* responseValid = nullptr;
    const WaveSignal* responseReady = nullptr;
};

bool anyPathContains(
    const QHash<QString, const WaveSignal*>& signalMap,
    const QString& token);

bool selectedBoundary(const QString& path,
                      bool useChannelBoundary,
                      const QString& channelToken,
                      const QString& portToken);

ValueState stateFromSample(const WaveSignal& signal,
                           const WaveSample& input) {
    WaveSample sample = input;
    if (!sample.rawFieldsReady) {
        hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
    }
    if (sample.isZ || sample.isAbsent) return {};
    ValueState result;
    result.known = true;
    result.value = sample.rawBits & waveBitMaskForWidth(signal.width);
    return result;
}

ValueState stateAtOrBefore(const WaveSignal& signal, qint64 time) {
    int lo = 0;
    int hi = signal.samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (signal.samples.at(mid).time <= time) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo <= 0) return {};
    return stateFromSample(signal, signal.samples.at(lo - 1));
}

QVector<qint64> boundaries(
    const QVector<const WaveSignal*>& signalList,
    qint64 start,
    qint64 end) {
    QVector<qint64> result{start, end};
    for (const WaveSignal* signal : signalList) {
        if (!signal) continue;
        for (const WaveSample& sample : signal->samples) {
            if (sample.time > start && sample.time < end) {
                result.push_back(sample.time);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

int population(quint64 value, int width) {
    int result = 0;
    const int boundedWidth = qBound(0, width, 64);
    for (int bit = 0; bit < boundedWidth; ++bit) {
        if ((value >> bit) & 1ull) ++result;
    }
    return result;
}

bool active(const ValueState& state) {
    return state.known && state.value != 0;
}

QString replaceChannel(QString path,
                       const QString& from,
                       const QString& to) {
    path.replace(from, to, Qt::CaseInsensitive);
    return path;
}

QString latencyStreamKey(QString root) {
    root = replaceChannel(root, QStringLiteral("Dls2L1lstxReq"),
                          QStringLiteral("L1LatencyPort"));
    root = replaceChannel(root, QStringLiteral("L1lstx2DlsReq"),
                          QStringLiteral("L1LatencyPort"));
    return root;
}

QVector<qint64> acceptedTransferCycles(
    const WaveSignal* valid,
    const WaveSignal* ready,
    const WaveSignal* storeMask,
    qint64 start,
    qint64 end,
    qint64 ticksPerCycle,
    bool& completeCoverage) {
    QVector<qint64> result;
    completeCoverage = valid && ready && ticksPerCycle > 0;
    if (!completeCoverage) return result;
    const QVector<qint64> points =
        boundaries({valid, ready, storeMask}, start, end);
    for (int i = 0; i + 1 < points.size(); ++i) {
        const qint64 begin = points.at(i);
        const qint64 finish = points.at(i + 1);
        const ValueState validState =
            stateAtOrBefore(*valid, begin);
        const ValueState readyState =
            stateAtOrBefore(*ready, begin);
        bool accepted = false;
        if (!validState.known || !readyState.known) {
            completeCoverage = false;
        } else if (active(validState) && active(readyState) &&
                   storeMask) {
            const ValueState storeState =
                stateAtOrBefore(*storeMask, begin);
            if (!storeState.known) {
                completeCoverage = false;
            } else {
                accepted = !active(storeState);
            }
        } else if (active(validState) && active(readyState)) {
            accepted = true;
        }
        if (!accepted) continue;

        const qint64 offset = begin - start;
        const qint64 remainder = offset % ticksPerCycle;
        qint64 time = begin;
        if (remainder != 0) {
            const qint64 adjustment =
                ticksPerCycle - remainder;
            if (begin >
                std::numeric_limits<qint64>::max() -
                    adjustment) {
                continue;
            }
            time += adjustment;
        }
        while (time < finish) {
            result.push_back(time);
            if (time >
                std::numeric_limits<qint64>::max() -
                    ticksPerCycle) {
                break;
            }
            time += ticksPerCycle;
        }
    }
    return result;
}

double percentile(const QVector<double>& sortedValues,
                  double quantile) {
    if (sortedValues.isEmpty()) return 0.0;
    const double bounded = qBound(0.0, quantile, 1.0);
    const int index =
        qBound(0,
               int(std::ceil(
                       bounded * double(sortedValues.size()))) -
                   1,
               sortedValues.size() - 1);
    return sortedValues.at(index);
}

QJsonObject buildL1LatencyProfile(
    const QHash<QString, const WaveSignal*>& signalMap,
    qint64 start,
    qint64 end,
    qint64 ticksPerCycle,
    bool hasDynamicWave) {
    Q_UNUSED(hasDynamicWave);
    const bool useRequestChannels =
        anyPathContains(signalMap, QStringLiteral("chDls2L1lstxReq"));
    const bool useReturnChannels =
        anyPathContains(signalMap, QStringLiteral("chL1lstx2DlsReq"));
    QHash<QString, L1LatencyStream> streams;

    for (auto it = signalMap.constBegin();
         it != signalMap.constEnd(); ++it) {
        const QString& path = it.key();
        if (!path.endsWith(QStringLiteral(".valid"))) continue;
        if (path.contains(QStringLiteral("Dls2L1lstxReq"),
                          Qt::CaseInsensitive) &&
            !path.contains(QStringLiteral("ReqRdy"),
                           Qt::CaseInsensitive) &&
            selectedBoundary(path, useRequestChannels,
                             QStringLiteral("chDls2L1lstxReq"),
                             QStringLiteral("ptDls2L1lstxReq"))) {
            QString root = path;
            root.chop(QStringLiteral(".valid").size());
            L1LatencyStream& stream =
                streams[latencyStreamKey(root)];
            stream.key = latencyStreamKey(root);
            stream.requestValid = it.value();
            stream.requestTakenPath =
                replaceChannel(
                    root,
                    QStringLiteral("Dls2L1lstxReq"),
                    QStringLiteral("Dls2L1lstxTaken"));
            stream.requestTaken =
                signalMap.value(stream.requestTakenPath, nullptr);
            stream.storeMask = signalMap.value(
                root +
                    QStringLiteral(
                        ".req_packet.store_req.smask"),
                nullptr);
        }
        if (path.contains(QStringLiteral("L1lstx2DlsReq"),
                          Qt::CaseInsensitive) &&
            selectedBoundary(path, useReturnChannels,
                             QStringLiteral("chL1lstx2DlsReq"),
                             QStringLiteral("ptL1lstx2DlsReq"))) {
            QString root = path;
            root.chop(QStringLiteral(".valid").size());
            L1LatencyStream& stream =
                streams[latencyStreamKey(root)];
            stream.key = latencyStreamKey(root);
            stream.responseValid = it.value();
            stream.responseReadyPath =
                replaceChannel(
                    root,
                    QStringLiteral("L1lstx2DlsReq"),
                    QStringLiteral("Dls2L1lstxReqRdy"));
            stream.responseReady =
                signalMap.value(stream.responseReadyPath, nullptr);
        }
    }

    int coveredStreams = 0;
    int completeStreams = 0;
    int requestCount = 0;
    int responseCount = 0;
    int matchedCount = 0;
    int unmatchedRequests = 0;
    int unmatchedResponses = 0;
    bool completeCoverage = true;
    QVector<double> latencies;
    QMap<qint64, int> outstandingDeltas;
    QJsonArray streamCoverage;

    for (auto it = streams.constBegin();
         it != streams.constEnd(); ++it) {
        const L1LatencyStream& stream = it.value();
        if (!stream.requestValid && !stream.responseValid) continue;
        ++coveredStreams;
        QJsonObject streamObject;
        streamObject.insert(QStringLiteral("key"), stream.key);
        streamObject.insert(QStringLiteral("request_valid"),
                            stream.requestValid != nullptr);
        streamObject.insert(QStringLiteral("request_taken"),
                            stream.requestTaken != nullptr);
        streamObject.insert(QStringLiteral("request_taken_path"),
                            stream.requestTakenPath);
        streamObject.insert(QStringLiteral("store_mask"),
                            stream.storeMask != nullptr);
        streamObject.insert(QStringLiteral("response_valid"),
                            stream.responseValid != nullptr);
        streamObject.insert(QStringLiteral("response_ready"),
                            stream.responseReady != nullptr);
        streamObject.insert(QStringLiteral("response_ready_path"),
                            stream.responseReadyPath);
        streamCoverage.push_back(streamObject);
        if (!stream.requestValid || !stream.requestTaken ||
            !stream.storeMask ||
            !stream.responseValid || !stream.responseReady) {
            completeCoverage = false;
            continue;
        }
        ++completeStreams;
        bool requestCovered = true;
        bool responseCovered = true;
        const QVector<qint64> requests =
            acceptedTransferCycles(
                stream.requestValid, stream.requestTaken,
                stream.storeMask, start, end, ticksPerCycle,
                requestCovered);
        const QVector<qint64> responses =
            acceptedTransferCycles(
                stream.responseValid, stream.responseReady,
                nullptr, start, end, ticksPerCycle,
                responseCovered);
        completeCoverage =
            completeCoverage && requestCovered && responseCovered;
        requestCount += requests.size();
        responseCount += responses.size();
        for (qint64 time : requests) {
            outstandingDeltas[time] += 1;
        }
        for (qint64 time : responses) {
            outstandingDeltas[time] -= 1;
        }

        int requestIndex = 0;
        for (qint64 responseTime : responses) {
            if (requestIndex >= requests.size() ||
                requests.at(requestIndex) > responseTime) {
                ++unmatchedResponses;
                continue;
            }
            latencies.push_back(
                double(responseTime -
                       requests.at(requestIndex)) /
                double(ticksPerCycle));
            ++requestIndex;
            ++matchedCount;
        }
        unmatchedRequests += requests.size() - requestIndex;
    }

    qint64 lastTime = start;
    int outstanding = 0;
    int maximumOutstanding = 0;
    long double outstandingTicks = 0.0L;
    for (auto it = outstandingDeltas.constBegin();
         it != outstandingDeltas.constEnd(); ++it) {
        const qint64 time = qBound(start, it.key(), end);
        if (time > lastTime) {
            outstandingTicks +=
                static_cast<long double>(outstanding) *
                static_cast<long double>(time - lastTime);
        }
        outstanding =
            qMax(0, outstanding + it.value());
        maximumOutstanding =
            qMax(maximumOutstanding, outstanding);
        lastTime = time;
    }
    if (lastTime < end) {
        outstandingTicks +=
            static_cast<long double>(outstanding) *
            static_cast<long double>(end - lastTime);
    }

    std::sort(latencies.begin(), latencies.end());
    const bool latencyCoverageComplete =
        coveredStreams > 0 && completeCoverage &&
        coveredStreams == completeStreams;
    QJsonObject result;
    result.insert(QStringLiteral("name"),
                  QStringLiteral("DLS-L1 请求返回延迟"));
    result.insert(QStringLiteral("basis"),
                  QStringLiteral(
                      "同通道请求 Taken 与返回 valid&&ready 按顺序匹配；"
                      "store 请求不进入返回延迟统计"));
    result.insert(QStringLiteral("status"),
                  matchedCount > 0
                      ? QStringLiteral("measured")
                      : QStringLiteral("unavailable"));
    result.insert(QStringLiteral("available"),
                  matchedCount > 0);
    result.insert(QStringLiteral("covered_streams"),
                  coveredStreams);
    result.insert(QStringLiteral("complete_streams"),
                  completeStreams);
    result.insert(QStringLiteral("coverage_complete"),
                  latencyCoverageComplete);
    result.insert(QStringLiteral("streams"), streamCoverage);
    result.insert(QStringLiteral("request_count"),
                  requestCount);
    result.insert(QStringLiteral("response_count"),
                  responseCount);
    result.insert(QStringLiteral("matched_transactions"),
                  matchedCount);
    result.insert(QStringLiteral("unmatched_requests"),
                  unmatchedRequests);
    result.insert(QStringLiteral("unmatched_responses"),
                  unmatchedResponses);
    result.insert(QStringLiteral("maximum_outstanding"),
                  maximumOutstanding);
    result.insert(
        QStringLiteral("average_outstanding"),
        end > start
            ? double(outstandingTicks /
                     static_cast<long double>(end - start))
            : 0.0);
    result.insert(
        QStringLiteral("request_rate_per_cycle"),
        end > start
            ? double(requestCount) * double(ticksPerCycle) /
                  double(end - start)
            : 0.0);
    QString confidence = QStringLiteral("低");
    int confidenceScore = 30;
    if (matchedCount > 0 && unmatchedRequests == 0 &&
        unmatchedResponses == 0 && latencyCoverageComplete) {
        confidenceScore =
            maximumOutstanding <= 1 ? 95 : 55;
        confidence =
            maximumOutstanding <= 1
                ? QStringLiteral("高")
                : QStringLiteral("低");
    }
    result.insert(QStringLiteral("confidence"),
                  confidence);
    result.insert(QStringLiteral("confidence_score"),
                  confidenceScore);
    if (!latencies.isEmpty()) {
        long double sum = 0.0L;
        for (double value : latencies) sum += value;
        result.insert(
            QStringLiteral("average_cycles"),
            double(sum /
                   static_cast<long double>(
                       latencies.size())));
        result.insert(QStringLiteral("p50_cycles"),
                      percentile(latencies, 0.50));
        result.insert(QStringLiteral("p95_cycles"),
                      percentile(latencies, 0.95));
        result.insert(QStringLiteral("maximum_cycles"),
                      latencies.last());
        result.insert(QStringLiteral("minimum_cycles"),
                      latencies.first());
    } else {
        result.insert(
            QStringLiteral("reason"),
            completeStreams == 0
                ? QStringLiteral(
                      "请求 Taken 或返回 ready 握手未完整覆盖")
                : QStringLiteral(
                      "分析区间没有可配对的非 Store 请求与返回"));
    }
    return result;
}

bool anyPathContains(const QHash<QString, const WaveSignal*>& signalMap,
                     const QString& token) {
    for (auto it = signalMap.constBegin(); it != signalMap.constEnd(); ++it) {
        if (it.key().contains(token, Qt::CaseInsensitive)) return true;
    }
    return false;
}

bool selectedBoundary(const QString& path,
                      bool useChannelBoundary,
                      const QString& channelToken,
                      const QString& portToken) {
    return useChannelBoundary
        ? path.contains(channelToken, Qt::CaseInsensitive)
        : path.contains(portToken, Qt::CaseInsensitive);
}

QString stripFinalArrayElement(QString path) {
    static const QRegularExpression finalElement(
        QStringLiteral("(?:\\[size=\\d+\\])?\\.\\[\\d+\\]$"));
    path.remove(finalElement);
    return path;
}

QString dlsPpuKey(QString root) {
    root = stripFinalArrayElement(root);
    root.replace(QStringLiteral("Dls2L1lstxReq"),
                 QStringLiteral("L1lstxPort"),
                 Qt::CaseInsensitive);
    root.replace(QStringLiteral("L1lstx2DlsReq"),
                 QStringLiteral("L1lstxPort"),
                 Qt::CaseInsensitive);
    return root;
}

int declaredArraySize(const QString& path,
                      const QString& fieldName) {
    const QRegularExpression expression(
        QStringLiteral("\\.%1\\[size=(\\d+)\\]\\.\\[\\d+\\]")
            .arg(QRegularExpression::escape(fieldName)));
    const QRegularExpressionMatch match = expression.match(path);
    return match.hasMatch() ? match.captured(1).toInt() : 1;
}

QString arrayRootBeforeField(const QString& path,
                             const QString& fieldName) {
    const QString marker = QLatin1Char('.') + fieldName;
    const int position = path.lastIndexOf(marker);
    return position >= 0 ? path.left(position) : path;
}

QJsonObject directionJson(const ByteAccumulator& data,
                          qint64 durationTicks,
                          qint64 ticksPerCycle,
                          double peakBytesPerCycle,
                          bool hasDynamicWave,
                          bool available,
                          bool utilizationAvailable,
                          const QString& basis) {
    Q_UNUSED(hasDynamicWave);
    QJsonObject result;
    const double durationCycles =
        durationTicks > 0
            ? double(durationTicks) / double(ticksPerCycle)
            : 0.0;
    const double totalBytes =
        double(data.byteTicks / static_cast<long double>(ticksPerCycle));
    result.insert(QStringLiteral("status"),
                  !available
                      ? QStringLiteral("unavailable")
                      : (utilizationAvailable
                             ? QStringLiteral("measured")
                             : QStringLiteral("partial")));
    result.insert(QStringLiteral("available"), available);
    result.insert(QStringLiteral("utilization_available"),
                  utilizationAvailable);
    result.insert(QStringLiteral("coverage_complete"),
                  utilizationAvailable);
    result.insert(QStringLiteral("streams"), data.streams);
    result.insert(QStringLiteral("basis"), basis);
    if (!available) return result;

    result.insert(QStringLiteral("total_bytes"), totalBytes);
    result.insert(QStringLiteral("peak_bytes_per_cycle"),
                  peakBytesPerCycle);
    result.insert(QStringLiteral("transfer_cycles"),
                  double(data.transferTicks) / double(ticksPerCycle));
    result.insert(QStringLiteral("known_cycles"),
                  double(data.knownTicks) / double(ticksPerCycle));
    if (!utilizationAvailable) {
        result.insert(
            QStringLiteral("reason"),
            QStringLiteral("waveform value coverage is incomplete"));
    }
    if (utilizationAvailable) {
        const double bytesPerCycle =
            durationCycles > 0.0 ? totalBytes / durationCycles : 0.0;
        result.insert(QStringLiteral("bytes_per_cycle"), bytesPerCycle);
        if (peakBytesPerCycle > 0.0) {
            result.insert(QStringLiteral("utilization_percent"),
                          100.0 * bytesPerCycle / peakBytesPerCycle);
        }
    }
    return result;
}

QJsonObject unavailableDirection(double peakBytesPerCycle,
                                 const QString& basis,
                                 const QString& reason) {
    QJsonObject result;
    result.insert(QStringLiteral("status"), QStringLiteral("unavailable"));
    result.insert(QStringLiteral("available"), false);
    result.insert(QStringLiteral("utilization_available"), false);
    result.insert(QStringLiteral("peak_bytes_per_cycle"),
                  peakBytesPerCycle);
    result.insert(QStringLiteral("basis"), basis);
    result.insert(QStringLiteral("reason"), reason);
    return result;
}

QJsonObject buildL1Profile(
    const QHash<QString, const WaveSignal*>& signalMap) {
    const bool useRequestChannels =
        anyPathContains(signalMap, QStringLiteral("chDls2L1lstxReq"));
    const bool useReturnChannels =
        anyPathContains(signalMap, QStringLiteral("chL1lstx2DlsReq"));
    QSet<QString> observedPpus;

    for (auto it = signalMap.constBegin(); it != signalMap.constEnd(); ++it) {
        const QString& path = it.key();
        if (path.endsWith(QStringLiteral(".valid")) &&
            path.contains(QStringLiteral("Dls2L1lstxReq"),
                          Qt::CaseInsensitive) &&
            !path.contains(QStringLiteral("ReqRdy"),
                           Qt::CaseInsensitive) &&
            selectedBoundary(path, useRequestChannels,
                             QStringLiteral("chDls2L1lstxReq"),
                             QStringLiteral("ptDls2L1lstxReq"))) {
            QString root = path;
            root.chop(QStringLiteral(".valid").size());
            observedPpus.insert(dlsPpuKey(root));
        }
        if (path.endsWith(QStringLiteral(".valid")) &&
            path.contains(QStringLiteral("L1lstx2DlsReq"),
                          Qt::CaseInsensitive) &&
            selectedBoundary(path, useReturnChannels,
                             QStringLiteral("chL1lstx2DlsReq"),
                             QStringLiteral("ptL1lstx2DlsReq"))) {
            QString root = path;
            root.chop(QStringLiteral(".valid").size());
            observedPpus.insert(dlsPpuKey(root));
        }
    }

    const int ppuCount = observedPpus.size();
    QJsonObject result;
    result.insert(QStringLiteral("name"), QStringLiteral("L1LSTX"));
    result.insert(QStringLiteral("interface"),
                  QStringLiteral("CB Data <-> L1LSTX"));
    result.insert(QStringLiteral("peak_basis"),
                  QStringLiteral("architecture-target"));
    result.insert(QStringLiteral("implementation_peak_verified"), false);
    result.insert(QStringLiteral("observed_ppu_ports"), ppuCount);
    result.insert(
        QStringLiteral("read"),
        unavailableDirection(
            128.0 * ppuCount,
            QStringLiteral("documented L1LSTX -> CB Data read path"),
            QStringLiteral(
                "the waveform does not expose the complete CB/L1 byte-valid path")));
    result.insert(
        QStringLiteral("write"),
        unavailableDirection(
            64.0 * ppuCount,
            QStringLiteral("documented CB Data -> L1LSTX write path"),
            QStringLiteral(
                "the waveform does not expose the complete CB/L1 byte-valid path")));

    QJsonObject dls;
    dls.insert(QStringLiteral("name"),
               QStringLiteral("DLS <-> L1LSTX"));
    dls.insert(QStringLiteral("interface"),
               QStringLiteral("Distributed Local Storage <-> L1LSTX"));
    dls.insert(QStringLiteral("peak_basis"),
               QStringLiteral("not-specified"));
    dls.insert(QStringLiteral("observed_ppu_ports"), ppuCount);
    dls.insert(QStringLiteral("deduplicated_boundary"),
               useRequestChannels || useReturnChannels
                   ? QStringLiteral("cluster-channel")
                   : QStringLiteral("module-port"));
    dls.insert(
        QStringLiteral("read"),
        unavailableDirection(
            0.0,
            QStringLiteral("valid && ready; load_rrb.smask"),
            QStringLiteral(
                "the available source does not define the byte unit of "
                "the 8-bit DLS smask")));
    dls.insert(
        QStringLiteral("write"),
        unavailableDirection(
            0.0,
            QStringLiteral("valid && Taken; store/atomic smask"),
            QStringLiteral(
                "the available source does not define the byte unit of "
                "the 8-bit DLS smask")));
    result.insert(QStringLiteral("dls_path"), dls);
    return result;
}

QJsonObject buildL2Profile(
    const QHash<QString, const WaveSignal*>& signalMap,
    qint64 start,
    qint64 end,
    qint64 ticksPerCycle,
    bool hasDynamicWave) {
    const bool useWriteChannels =
        anyPathContains(signalMap,
                        QStringLiteral("chUscTxArb2L2CacheWrData"));
    const bool useReadChannels =
        anyPathContains(
            signalMap,
            QStringLiteral("chL2Cache2UscTxArbRtnDataIn"));

    ByteAccumulator read;
    ByteAccumulator write;
    QSet<QString> readLaneKeys;
    QSet<QString> writeLaneKeys;
    QSet<QString> readPorts;
    QSet<QString> writePorts;
    QHash<QString, int> readDeclaredByPort;
    QHash<QString, int> writeDeclaredByPort;
    int readMaskWidth = 0;
    int writeMaskWidth = 0;

    for (auto it = signalMap.constBegin(); it != signalMap.constEnd(); ++it) {
        const QString& path = it.key();
        const QString lower = path.toLower();
        const bool l2WriteValid =
            lower.contains(QStringLiteral("2l2cachewrdata")) &&
            lower.contains(QStringLiteral(".vld[size="));
        const bool selectedL2Write =
            selectedBoundary(path, useWriteChannels,
                             QStringLiteral("chUscTxArb2L2CacheWrData"),
                             QStringLiteral("ptUscTxArb2L2CacheWrData"));
        if (l2WriteValid && selectedL2Write) {
            const int field = path.lastIndexOf(QStringLiteral(".vld"));
            if (field < 0) continue;
            const QString base = path.left(field);
            const QString laneSuffix =
                path.mid(field + QStringLiteral(".vld").size());
            const QString maskPath =
                base + QStringLiteral(".wmask") + laneSuffix;
            const WaveSignal* valid = it.value();
            const WaveSignal* mask = signalMap.value(maskPath, nullptr);
            if (!valid || !mask) continue;
            const QString laneKey = base + laneSuffix;
            if (writeLaneKeys.contains(laneKey)) continue;
            writeLaneKeys.insert(laneKey);
            ++write.streams;
            if (writeMaskWidth == 0) {
                writeMaskWidth = mask->width;
            } else if (writeMaskWidth != mask->width) {
                writeMaskWidth = -1;
            }
            const QString port = arrayRootBeforeField(path, QStringLiteral("vld"));
            writePorts.insert(port);
            writeDeclaredByPort[port] =
                qMax(writeDeclaredByPort.value(port),
                     declaredArraySize(path, QStringLiteral("vld")));

            const QVector<qint64> points =
                boundaries({valid, mask}, start, end);
            for (int i = 0; i + 1 < points.size(); ++i) {
                const qint64 ticks = points.at(i + 1) - points.at(i);
                const ValueState validState =
                    stateAtOrBefore(*valid, points.at(i));
                const ValueState maskState =
                    stateAtOrBefore(*mask, points.at(i));
                if (!validState.known || !maskState.known) continue;
                write.knownTicks += ticks;
                if (!active(validState)) continue;
                const int bytes = population(maskState.value, mask->width);
                if (bytes <= 0) continue;
                write.byteTicks +=
                    static_cast<long double>(bytes) *
                    static_cast<long double>(ticks);
                write.transferTicks += ticks;
            }
        }

        const bool l2ReadSectorValid =
            lower.contains(QStringLiteral("l2cache2")) &&
            lower.contains(QStringLiteral("rtndatain")) &&
            lower.contains(QStringLiteral(".sector[size=")) &&
            lower.endsWith(QStringLiteral(".vld"));
        if (!l2ReadSectorValid) continue;
        if (useReadChannels &&
            !path.contains(QStringLiteral("chL2Cache2UscTxArbRtnDataIn"),
                           Qt::CaseInsensitive)) {
            continue;
        }
        if (!useReadChannels &&
            !path.contains(QStringLiteral("ptL2Cache2UscTxArbRtnDataIn"),
                           Qt::CaseInsensitive)) {
            continue;
        }

        QString maskPath = path;
        maskPath.chop(QStringLiteral(".vld").size());
        maskPath += QStringLiteral(".mask");
        const int sectorPosition =
            path.lastIndexOf(QStringLiteral(".sector"));
        if (sectorPosition < 0) continue;
        const QString parentValidPath =
            path.left(sectorPosition) + QStringLiteral(".vld");
        const WaveSignal* parentValid =
            signalMap.value(parentValidPath, nullptr);
        const WaveSignal* sectorValid = it.value();
        const WaveSignal* mask = signalMap.value(maskPath, nullptr);
        if (!parentValid || !sectorValid || !mask) continue;
        QString laneKey = path;
        laneKey.chop(QStringLiteral(".vld").size());
        if (readLaneKeys.contains(laneKey)) continue;
        readLaneKeys.insert(laneKey);
        ++read.streams;
        if (readMaskWidth == 0) {
            readMaskWidth = mask->width;
        } else if (readMaskWidth != mask->width) {
            readMaskWidth = -1;
        }
        const QString port = path.left(sectorPosition);
        readPorts.insert(port);
        readDeclaredByPort[port] =
            qMax(readDeclaredByPort.value(port),
                 declaredArraySize(path, QStringLiteral("sector")));

        const QVector<qint64> points =
            boundaries({parentValid, sectorValid, mask}, start, end);
        for (int i = 0; i + 1 < points.size(); ++i) {
            const qint64 ticks = points.at(i + 1) - points.at(i);
            const ValueState parentState =
                stateAtOrBefore(*parentValid, points.at(i));
            const ValueState sectorState =
                stateAtOrBefore(*sectorValid, points.at(i));
            const ValueState maskState =
                stateAtOrBefore(*mask, points.at(i));
            if (!parentState.known || !sectorState.known ||
                !maskState.known) {
                continue;
            }
            read.knownTicks += ticks;
            if (!active(parentState) || !active(sectorState)) continue;
            if (mask->width <= 0 || 32 % mask->width != 0) continue;
            const int bytes =
                population(maskState.value, mask->width) *
                (32 / mask->width);
            if (bytes <= 0) continue;
            read.byteTicks +=
                static_cast<long double>(bytes) *
                static_cast<long double>(ticks);
            read.transferTicks += ticks;
        }
    }

    int readDeclared = 0;
    for (auto it = readDeclaredByPort.constBegin();
         it != readDeclaredByPort.constEnd(); ++it) {
        readDeclared += it.value();
    }
    int writeDeclared = 0;
    for (auto it = writeDeclaredByPort.constBegin();
         it != writeDeclaredByPort.constEnd(); ++it) {
        writeDeclared += it.value();
    }

    QJsonObject coverage;
    coverage.insert(QStringLiteral("read_sector_lanes_traced"),
                    readLaneKeys.size());
    coverage.insert(QStringLiteral("read_sector_lanes_declared"),
                    readDeclared);
    coverage.insert(QStringLiteral("write_data_lanes_traced"),
                    writeLaneKeys.size());
    coverage.insert(QStringLiteral("write_data_lanes_declared"),
                    writeDeclared);
    coverage.insert(QStringLiteral("read_mask_bits_per_sector"),
                    readMaskWidth);
    coverage.insert(
        QStringLiteral("read_bytes_per_mask_bit"),
        readMaskWidth > 0 && 32 % readMaskWidth == 0
            ? 32 / readMaskWidth
            : 0);
    coverage.insert(QStringLiteral("write_mask_bits_per_lane"),
                    writeMaskWidth);
    coverage.insert(
        QStringLiteral("write_bytes_per_mask_bit"),
        writeMaskWidth == 32 ? 1 : 0);
    const bool readLayoutCoverageComplete =
        readDeclared > 0 && readLaneKeys.size() == readDeclared &&
        readMaskWidth > 0 && 32 % readMaskWidth == 0;
    const bool writeLayoutCoverageComplete =
        writeDeclared > 0 && writeLaneKeys.size() == writeDeclared &&
        writeMaskWidth == 32;
    const qint64 durationTicks = end - start;
    const bool readValueCoverageComplete =
        read.streams > 0 &&
        static_cast<long double>(read.knownTicks) ==
            static_cast<long double>(durationTicks) *
                static_cast<long double>(read.streams);
    const bool writeValueCoverageComplete =
        write.streams > 0 &&
        static_cast<long double>(write.knownTicks) ==
            static_cast<long double>(durationTicks) *
                static_cast<long double>(write.streams);
    const bool readCoverageComplete =
        readLayoutCoverageComplete && readValueCoverageComplete;
    const bool writeCoverageComplete =
        writeLayoutCoverageComplete && writeValueCoverageComplete;
    coverage.insert(QStringLiteral("read_layout_complete"),
                    readLayoutCoverageComplete);
    coverage.insert(QStringLiteral("write_layout_complete"),
                    writeLayoutCoverageComplete);
    coverage.insert(QStringLiteral("read_value_complete"),
                    readValueCoverageComplete);
    coverage.insert(QStringLiteral("write_value_complete"),
                    writeValueCoverageComplete);
    coverage.insert(QStringLiteral("read_complete"),
                    readCoverageComplete);
    coverage.insert(QStringLiteral("write_complete"),
                    writeCoverageComplete);

    QJsonObject result;
    result.insert(QStringLiteral("name"),
                  QStringLiteral("L1LSTX <-> L2"));
    result.insert(QStringLiteral("interface"),
                  QStringLiteral("USC/TX Arb <-> L2Cache"));
    result.insert(QStringLiteral("peak_basis"),
                  QStringLiteral("architecture-target"));
    result.insert(QStringLiteral("implementation_peak_verified"), false);
    result.insert(
        QStringLiteral("peak_note"),
        QStringLiteral(
            "The available QPPU source does not include the L1LSTX/L2 "
            "implementation; 64/32 B/cycle are architecture targets."));
    result.insert(QStringLiteral("coverage"), coverage);
    result.insert(QStringLiteral("deduplicated_boundary"),
                  useReadChannels || useWriteChannels
                      ? QStringLiteral("cluster-channel")
                      : QStringLiteral("module-port"));
    result.insert(
        QStringLiteral("read"),
        directionJson(read, end - start, ticksPerCycle,
                      64.0 * readPorts.size(), hasDynamicWave,
                      read.streams > 0, readCoverageComplete,
                      QStringLiteral(
                          "parent vld && sector.vld; "
                          "popcount(sector.mask) * (32 B / mask width)")));
    result.insert(
        QStringLiteral("write"),
        directionJson(write, end - start, ticksPerCycle,
                      32.0 * writePorts.size(), hasDynamicWave,
                      write.streams > 0, writeCoverageComplete,
                      QStringLiteral("vld; popcount(wmask) B")));
    return result;
}

WaveSignal makeSignal(int id,
                      int width,
                      const QVector<QPair<qint64, quint64>>& samples) {
    WaveSignal signal;
    signal.signalId = id;
    signal.kind = width == 1 ? SignalKind::Bit : SignalKind::Bus;
    signal.width = width;
    for (const auto& pair : samples) {
        WaveSample sample;
        sample.time = pair.first;
        sample.rawBits = pair.second;
        sample.rawFieldsReady = true;
        signal.samples.push_back(sample);
    }
    return signal;
}

}  // namespace

bool isMemoryBandwidthSignal(const QString& path) {
    const QString lower = path.toLower();
    if (lower.contains(QStringLiteral("dls2l1lstxtaken")) ||
        lower.contains(QStringLiteral("dls2l1lstxreqrdy"))) {
        return true;
    }
    if (lower.contains(QStringLiteral("dls2l1lstxreq")) ||
        lower.contains(QStringLiteral("l1lstx2dlsreq"))) {
        return lower.endsWith(QStringLiteral(".valid")) ||
               lower.endsWith(QStringLiteral(".req_packet.store_req.smask")) ||
               lower.endsWith(QStringLiteral(".req_packet.atomic_req.smask")) ||
               lower.endsWith(QStringLiteral(".req_packet.load_rrb.smask"));
    }
    if (lower.contains(
            QStringLiteral("usctxarb2l2cachewrdata"))) {
        return lower.contains(QStringLiteral(".vld[size=")) ||
               lower.contains(QStringLiteral(".wmask[size="));
    }
    if (lower.contains(
            QStringLiteral("l2cache2usctxarbrtndatain"))) {
        return lower.endsWith(QStringLiteral(".vld")) ||
               (lower.contains(QStringLiteral(".sector[size=")) &&
                lower.endsWith(QStringLiteral(".mask")));
    }
    return false;
}

QJsonObject buildMemoryBandwidthProfile(
    const QHash<QString, const WaveSignal*>& signalsByPath,
    qint64 startTick,
    qint64 endTick,
    qint64 ticksPerCycle,
    bool hasDynamicWave,
    QStringList& warnings) {
    QJsonObject result;
    result.insert(
        QStringLiteral("definition"),
        QStringLiteral(
            "有效字节带宽 = 握手或数据有效期间的 mask 有效字节数 / 业务周期"));
    QJsonObject l1 = buildL1Profile(signalsByPath);
    l1.insert(
        QStringLiteral("latency"),
        buildL1LatencyProfile(signalsByPath, startTick, endTick,
                              ticksPerCycle, hasDynamicWave));
    result.insert(QStringLiteral("l1"), l1);
    result.insert(QStringLiteral("l2"),
                  buildL2Profile(signalsByPath, startTick, endTick,
                                 ticksPerCycle, hasDynamicWave));

    const QJsonObject l2Coverage =
        result.value(QStringLiteral("l2")).toObject()
            .value(QStringLiteral("coverage")).toObject();
    if (l2Coverage.value(
            QStringLiteral("read_sector_lanes_traced")).toInt() <
        l2Coverage.value(
            QStringLiteral("read_sector_lanes_declared")).toInt()) {
        warnings.push_back(
            QStringLiteral(
                "L1LSTX-L2 读返回 sector 只覆盖 %1/%2；不计算利用率")
                .arg(l2Coverage.value(
                         QStringLiteral("read_sector_lanes_traced")).toInt())
                .arg(l2Coverage.value(
                         QStringLiteral("read_sector_lanes_declared")).toInt()));
    }
    if (l2Coverage.value(
            QStringLiteral("write_data_lanes_traced")).toInt() <
        l2Coverage.value(
            QStringLiteral("write_data_lanes_declared")).toInt()) {
        warnings.push_back(
            QStringLiteral(
                "L1LSTX-L2 写数据 lane 只覆盖 %1/%2；不计算利用率")
                .arg(l2Coverage.value(
                         QStringLiteral("write_data_lanes_traced")).toInt())
                .arg(l2Coverage.value(
                         QStringLiteral("write_data_lanes_declared")).toInt()));
    }
    if (l2Coverage.value(
            QStringLiteral("read_sector_lanes_traced")).toInt() > 0 &&
        l2Coverage.value(
            QStringLiteral("read_bytes_per_mask_bit")).toInt() == 0) {
        warnings.push_back(
            QStringLiteral(
                "L1LSTX-L2 读 mask 位宽不能映射到 32B sector；不计算利用率"));
    }
    if (l2Coverage.value(
            QStringLiteral("write_data_lanes_traced")).toInt() > 0 &&
        l2Coverage.value(
            QStringLiteral("write_bytes_per_mask_bit")).toInt() == 0) {
        warnings.push_back(
            QStringLiteral(
                "L1LSTX-L2 写 wmask 不是 32-bit/32B lane；不计算利用率"));
    }
    if (l2Coverage.value(
            QStringLiteral("read_layout_complete")).toBool() &&
        !l2Coverage.value(
            QStringLiteral("read_value_complete")).toBool()) {
        warnings.push_back(QStringLiteral(
            "L1LSTX-L2 read valid/mask contains unknown intervals; "
            "bandwidth utilization is unavailable"));
    }
    if (l2Coverage.value(
            QStringLiteral("write_layout_complete")).toBool() &&
        !l2Coverage.value(
            QStringLiteral("write_value_complete")).toBool()) {
        warnings.push_back(QStringLiteral(
            "L1LSTX-L2 write valid/mask contains unknown intervals; "
            "bandwidth utilization is unavailable"));
    }
    return result;
}

bool memoryBandwidthProfilerSelfTest(QString& error) {
    QVector<WaveSignal> storage;
    QStringList paths;
    auto add = [&](const QString& path, int width,
                   const QVector<QPair<qint64, quint64>>& samples) {
        storage.push_back(makeSignal(storage.size() + 1, width, samples));
        paths.push_back(path);
    };

    const QString l1Req =
        QStringLiteral("gpu.chDls2L1lstxReq[size=1].[0][size=1].[0]");
    add(l1Req + QStringLiteral(".valid"), 1,
        {{0, 0}, {10, 1}, {30, 0}});
    add(QStringLiteral("gpu.chDls2L1lstxTaken[size=1].[0][size=1].[0]"),
        1, {{0, 1}});
    add(l1Req + QStringLiteral(".req_packet.store_req.smask"),
        8, {{0, 0}});
    add(l1Req + QStringLiteral(".req_packet.atomic_req.smask"),
        8, {{0, 0}});

    const QString l1Return =
        QStringLiteral("gpu.chL1lstx2DlsReq[size=1].[0][size=1].[0]");
    add(l1Return + QStringLiteral(".valid"), 1,
        {{0, 0}, {30, 1}, {50, 0}});
    add(QStringLiteral("gpu.chDls2L1lstxReqRdy[size=1].[0][size=1].[0]"),
        1, {{0, 1}});
    add(l1Return + QStringLiteral(".req_packet.load_rrb.smask"),
        8, {{0, 15}});

    const QString l2Write =
        QStringLiteral("gpu.chUscTxArb2L2CacheWrData[size=1].[0]");
    add(l2Write + QStringLiteral(".vld[size=1].[0]"), 8,
        {{0, 0}, {10, 0xff}, {30, 0}});
    add(l2Write + QStringLiteral(".wmask[size=1].[0]"), 32,
        {{0, 0xffff}});

    const QString l2Read =
        QStringLiteral("gpu.chL2Cache2UscTxArbRtnDataIn[size=1].[0]");
    add(l2Read + QStringLiteral(".vld"), 1, {{0, 1}});
    add(l2Read + QStringLiteral(".sector[size=1].[0].vld"), 1,
        {{0, 0}, {40, 1}, {70, 0}});
    add(l2Read + QStringLiteral(".sector[size=1].[0].mask"), 8,
        {{0, 0xff}});

    QHash<QString, const WaveSignal*> signalMap;
    for (int i = 0; i < storage.size(); ++i) {
        signalMap.insert(paths.at(i), &storage.at(i));
    }
    QStringList warnings;
    const QJsonObject profile =
        buildMemoryBandwidthProfile(signalMap, 0, 100, 10, true, warnings);
    QStringList noTransitionHintWarnings;
    const QJsonObject noTransitionHintProfile =
        buildMemoryBandwidthProfile(
            signalMap, 0, 100, 10, false,
            noTransitionHintWarnings);
    const QJsonObject l1 = profile.value(QStringLiteral("l1")).toObject();
    const QJsonObject l2 = profile.value(QStringLiteral("l2")).toObject();
    const QJsonObject dls = l1.value(QStringLiteral("dls_path")).toObject();
    const QJsonObject latency =
        l1.value(QStringLiteral("latency")).toObject();
    const QJsonObject l2ReadResult =
        l2.value(QStringLiteral("read")).toObject();
    const QJsonObject l2WriteResult =
        l2.value(QStringLiteral("write")).toObject();
    const QJsonObject noTransitionHintL1 =
        noTransitionHintProfile.value(
            QStringLiteral("l1")).toObject();
    const QJsonObject noTransitionHintL2 =
        noTransitionHintProfile.value(
            QStringLiteral("l2")).toObject();
    if (l1.value(QStringLiteral("read")).toObject()
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("unavailable") ||
        dls.value(QStringLiteral("read")).toObject()
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("unavailable") ||
        dls.value(QStringLiteral("write")).toObject()
            .value(QStringLiteral("status")).toString() !=
            QStringLiteral("unavailable") ||
        latency.value(QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        noTransitionHintL1.value(
            QStringLiteral("latency")).toObject()
                .value(QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        noTransitionHintL2.value(
            QStringLiteral("read")).toObject()
                .value(QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        noTransitionHintL2.value(
            QStringLiteral("write")).toObject()
                .value(QStringLiteral("status")).toString() !=
            QStringLiteral("measured") ||
        latency.value(
            QStringLiteral("matched_transactions")).toInt() != 2 ||
        latency.value(
            QStringLiteral("unmatched_requests")).toInt() != 0 ||
        latency.value(
            QStringLiteral("maximum_outstanding")).toInt() != 2 ||
        latency.value(
            QStringLiteral("confidence_score")).toInt() >= 70 ||
        std::fabs(latency.value(
                      QStringLiteral("average_cycles")).toDouble() -
                  2.0) > 1e-9 ||
        std::fabs(latency.value(
                      QStringLiteral("p95_cycles")).toDouble() -
                  2.0) > 1e-9 ||
        std::fabs(l2ReadResult.value(
                      QStringLiteral("utilization_percent")).toDouble() -
                  15.0) > 1e-9 ||
        std::fabs(l2WriteResult.value(
                      QStringLiteral("utilization_percent")).toDouble() -
                  10.0) > 1e-9) {
        error = QStringLiteral(
            "L1/L2 byte integration mismatch: %1/%2/%3/%4")
                    .arg(l1.value(QStringLiteral("read")).toObject()
                             .value(QStringLiteral("status")).toString())
                    .arg(dls.value(QStringLiteral("read")).toObject()
                             .value(QStringLiteral("status")).toString())
                    .arg(l2ReadResult.value(
                             QStringLiteral("utilization_percent")).toDouble())
                    .arg(l2WriteResult.value(
                             QStringLiteral("utilization_percent")).toDouble());
        return false;
    }

    QHash<QString, const WaveSignal*> missingStoreMaskMap = signalMap;
    missingStoreMaskMap.remove(
        l1Req +
        QStringLiteral(".req_packet.store_req.smask"));
    QStringList missingStoreMaskWarnings;
    const QJsonObject missingStoreMaskProfile =
        buildMemoryBandwidthProfile(
            missingStoreMaskMap, 0, 100, 10, true,
            missingStoreMaskWarnings);
    const QJsonObject missingStoreMaskLatency =
        missingStoreMaskProfile.value(QStringLiteral("l1")).toObject()
            .value(QStringLiteral("latency")).toObject();
    if (missingStoreMaskLatency.value(
            QStringLiteral("coverage_complete")).toBool() ||
        missingStoreMaskLatency.value(
            QStringLiteral("confidence_score")).toInt() >= 70) {
        error = QStringLiteral(
            "missing store mask produced reliable L1 latency");
        return false;
    }

    WaveSignal incompleteRequest =
        makeSignal(1001, 1, {{0, 0}});
    WaveSignal incompleteTaken =
        makeSignal(1002, 1, {{0, 1}});
    QHash<QString, const WaveSignal*> incompleteLatencyMap = signalMap;
    incompleteLatencyMap.insert(
        QStringLiteral(
            "gpu.chDls2L1lstxReq[size=2].[1][size=1].[0].valid"),
        &incompleteRequest);
    incompleteLatencyMap.insert(
        QStringLiteral(
            "gpu.chDls2L1lstxTaken[size=2].[1][size=1].[0]"),
        &incompleteTaken);
    QStringList incompleteLatencyWarnings;
    const QJsonObject incompleteLatencyProfile =
        buildMemoryBandwidthProfile(
            incompleteLatencyMap, 0, 100, 10, true,
            incompleteLatencyWarnings);
    const QJsonObject incompleteLatency =
        incompleteLatencyProfile.value(QStringLiteral("l1")).toObject()
            .value(QStringLiteral("latency")).toObject();
    if (incompleteLatency.value(
            QStringLiteral("coverage_complete")).toBool() ||
        incompleteLatency.value(
            QStringLiteral("confidence_score")).toInt() >= 95) {
        error = QStringLiteral(
            "incomplete L1 stream received high-confidence latency");
        return false;
    }

    QHash<QString, const WaveSignal*> partialMap;
    for (int i = 0; i < storage.size(); ++i) {
        QString path = paths.at(i);
        if (path.contains(
                QStringLiteral("UscTxArb2L2CacheWrData"),
                Qt::CaseInsensitive)) {
            path.replace(QStringLiteral(".vld[size=1]"),
                         QStringLiteral(".vld[size=2]"));
            path.replace(QStringLiteral(".wmask[size=1]"),
                         QStringLiteral(".wmask[size=2]"));
        }
        if (path.contains(
                QStringLiteral("L2Cache2UscTxArbRtnDataIn"),
                Qt::CaseInsensitive)) {
            path.replace(QStringLiteral(".sector[size=1]"),
                         QStringLiteral(".sector[size=4]"));
        }
        partialMap.insert(path, &storage.at(i));
    }
    QStringList partialWarnings;
    const QJsonObject partialProfile =
        buildMemoryBandwidthProfile(
            partialMap, 0, 100, 10, true, partialWarnings);
    const QJsonObject partialL2 =
        partialProfile.value(QStringLiteral("l2")).toObject();
    const QJsonObject partialCoverage =
        partialL2.value(QStringLiteral("coverage")).toObject();
    if (partialL2.value(QStringLiteral("read")).toObject()
            .contains(QStringLiteral("utilization_percent")) ||
        partialL2.value(QStringLiteral("write")).toObject()
            .contains(QStringLiteral("utilization_percent")) ||
        partialCoverage.value(
            QStringLiteral("read_sector_lanes_declared")).toInt() != 4 ||
        partialCoverage.value(
            QStringLiteral("write_data_lanes_declared")).toInt() != 2 ||
        partialWarnings.size() != 2) {
        error = QStringLiteral(
            "partial L1LSTX-L2 lane coverage produced a utilization");
        return false;
    }

    QVector<WaveSignal> unknownStorage = storage;
    QHash<QString, const WaveSignal*> unknownMap;
    for (int i = 0; i < unknownStorage.size(); ++i) {
        if (paths.at(i).contains(
                QStringLiteral("UscTxArb2L2CacheWrData"),
                Qt::CaseInsensitive) &&
            paths.at(i).contains(QStringLiteral(".wmask"))) {
            unknownStorage[i].samples[0].time = 10;
        }
        unknownMap.insert(paths.at(i), &unknownStorage.at(i));
    }
    QStringList unknownWarnings;
    const QJsonObject unknownProfile =
        buildMemoryBandwidthProfile(
            unknownMap, 0, 100, 10, true, unknownWarnings);
    const QJsonObject unknownL2 =
        unknownProfile.value(QStringLiteral("l2")).toObject();
    const QJsonObject unknownWrite =
        unknownL2.value(QStringLiteral("write")).toObject();
    const QJsonObject unknownCoverage =
        unknownL2.value(QStringLiteral("coverage")).toObject();
    if (unknownWrite.contains(QStringLiteral("utilization_percent")) ||
        unknownWrite.contains(QStringLiteral("bytes_per_cycle")) ||
        !unknownCoverage.value(
            QStringLiteral("write_layout_complete")).toBool() ||
        unknownCoverage.value(
            QStringLiteral("write_value_complete")).toBool() ||
        unknownCoverage.value(
            QStringLiteral("write_complete")).toBool()) {
        error = QStringLiteral(
            "unknown L2 values produced a bandwidth utilization");
        return false;
    }
    return true;
}

}  // namespace waveperf
