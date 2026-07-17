#include "WaveBlockCacheLoader.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {

bool sameWindowSamples(const QVector<WaveSample>& left,
                       const QVector<WaveSample>& right,
                       qint64 start,
                       qint64 end) {
    auto stateIndex = [](const QVector<WaveSample>& samples, qint64 time) {
        auto it = std::upper_bound(samples.begin(), samples.end(), time,
            [](qint64 value, const WaveSample& sample) { return value < sample.time; });
        return it == samples.begin() ? -1 : int((it - samples.begin()) - 1);
    };
    int li = stateIndex(left, start);
    int ri = stateIndex(right, start);
    if (li < 0 || ri < 0 || !waveSamplesEquivalent(left.at(li), right.at(ri))) return false;
    while (li < left.size() && left.at(li).time < start) ++li;
    while (ri < right.size() && right.at(ri).time < start) ++ri;
    while (li < left.size() && ri < right.size() &&
           left.at(li).time <= end && right.at(ri).time <= end) {
        if (left.at(li).time != right.at(ri).time ||
            !waveSamplesEquivalent(left.at(li), right.at(ri))) return false;
        ++li;
        ++ri;
    }
    const bool leftRemaining = li < left.size() && left.at(li).time <= end;
    const bool rightRemaining = ri < right.size() && right.at(ri).time <= end;
    return leftRemaining == rightRemaining;
}

const WaveSignal* signalById(const WaveFile& wave, int signalId) {
    for (const WaveSignal& signal : wave.signalList) {
        if (signal.signalId == signalId) return &signal;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: smoke_wvz4_block_cache_loader <input.wvz4>\n";
        return 2;
    }

    std::shared_ptr<WaveParser4Reader> reader(new WaveParser4Reader);
    QString error;
    if (!reader->open(QString::fromLocal8Bit(argv[1]), error)) {
        std::cerr << error.toLocal8Bit().constData() << '\n';
        return 3;
    }
    WaveFile directory = reader->takeDirectoryWave();
    QVector<int> signalIds;
    for (const WaveSignal& signal : directory.signalList) {
        if (signal.signalId > 0) signalIds.push_back(signal.signalId);
        if (signalIds.size() == 64) break;
    }
    if (signalIds.isEmpty()) return 4;

    std::shared_ptr<std::mutex> readerMutex(new std::mutex);
    WaveBlockCacheLoader loader;
    loader.start(reader, readerMutex, directory, 64ull * 1024ull * 1024ull);

    auto request = [&](qint64 start, qint64 end, qint64 bucket, quint64 serial,
                       WaveBlockCacheLoader::Result& output) -> bool {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        loader.requestViewport(signalIds, start, end, start, end, bucket, serial,
            [&](WaveBlockCacheLoader::Result&& result) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    output = std::move(result);
                    done = true;
                }
                cv.notify_one();
            });
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(30), [&]() { return done; });
    };

    const qint64 fullStart = directory.meta.start;
    const qint64 fullEnd = directory.meta.end;
    WaveBlockCacheLoader::Result lodResult;
    if (!request(fullStart, fullEnd, 100, 1, lodResult) ||
        !lodResult.ok || !lodResult.lodLoad || lodResult.bucketCycles != 100) {
        std::cerr << "LOD cache request failed\n";
        loader.stop();
        return 5;
    }

    const qint64 center = fullStart + (fullEnd - fullStart) / 2;
    const qint64 rawStart = qMax(fullStart, center - 256);
    const qint64 rawEnd = qMin(fullEnd, center + 256);
    WaveBlockCacheLoader::Result rawResult;
    if (!request(rawStart, rawEnd, 1, 2, rawResult) ||
        !rawResult.ok || rawResult.lodLoad) {
        std::cerr << "RAW cache request failed\n";
        loader.stop();
        return 6;
    }

    WaveFile direct;
    {
        std::lock_guard<std::mutex> lock(*readerMutex);
        if (!reader->loadSignals(signalIds, direct, error, 0, rawStart, rawEnd)) {
            std::cerr << error.toLocal8Bit().constData() << '\n';
            loader.stop();
            return 7;
        }
    }
    for (int signalId : signalIds) {
        const WaveSignal* cached = signalById(rawResult.wave, signalId);
        const WaveSignal* expected = signalById(direct, signalId);
        if (!cached || !expected ||
            !sameWindowSamples(cached->samples, expected->samples, rawStart, rawEnd)) {
            std::cerr << "RAW cache mismatch for signal " << signalId
                      << " cached=" << (cached ? cached->samples.size() : -1)
                      << " expected=" << (expected ? expected->samples.size() : -1);
            if (cached && !cached->samples.isEmpty()) {
                std::cerr << " cached_range=" << cached->samples.first().time
                          << ".." << cached->samples.last().time;
            }
            if (expected && !expected->samples.isEmpty()) {
                std::cerr << " expected_range=" << expected->samples.first().time
                          << ".." << expected->samples.last().time;
            }
            std::cerr << '\n';
            loader.stop();
            return 8;
        }
    }

    if (loader.cachedBytes() > loader.cacheLimitBytes()) {
        std::cerr << "cache exceeded limit\n";
        loader.stop();
        return 9;
    }
    std::cout << "block_cache_ok cached_bytes=" << loader.cachedBytes()
              << " cache_limit_bytes=" << loader.cacheLimitBytes()
              << " cached_blocks=" << loader.cachedBlockCount() << '\n';
    loader.releaseWaveLater(std::move(lodResult.wave));
    loader.releaseWaveLater(std::move(rawResult.wave));
    loader.stop();
    return 0;
}
