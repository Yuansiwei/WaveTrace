#include "WaveBlockCacheLoader.h"

#include <QElapsedTimer>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <limits>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr quint64 kDecodeReserveBytes = 2ull * 1024ull * 1024ull * 1024ull;

quint64 blockKey(const WaveParser4Reader::DataBlockDescriptor& block) {
    const quint64 kind = block.kind == WaveParser4Reader::DataBlockDescriptor::Kind::Lod
        ? (1ull << 63) : 0ull;
    return kind | quint64(quint32(block.index));
}

bool overlaps(qint64 aStart, qint64 aEnd, qint64 bStart, qint64 bEnd) {
    return aEnd > bStart && aStart < bEnd;
}

void compactRanges(QVector<WaveLodValidRange>& ranges) {
    if (ranges.isEmpty()) return;
    std::sort(ranges.begin(), ranges.end(), [](const WaveLodValidRange& a,
                                                const WaveLodValidRange& b) {
        return a.start < b.start || (a.start == b.start && a.end < b.end);
    });
    int write = 0;
    for (const WaveLodValidRange& range : ranges) {
        if (range.end <= range.start) continue;
        if (write > 0 && range.start <= ranges.at(write - 1).end) {
            if (range.end > ranges[write - 1].end) ranges[write - 1].end = range.end;
        } else {
            ranges[write++] = range;
        }
    }
    ranges.resize(write);
}

void compactSamples(QVector<WaveSample>& samples) {
    if (samples.size() <= 1) return;
    std::stable_sort(samples.begin(), samples.end(), [](const WaveSample& a,
                                                         const WaveSample& b) {
        return a.time < b.time;
    });
    int write = 1;
    for (int read = 1; read < samples.size(); ++read) {
        if (samples.at(read).time == samples.at(write - 1).time) {
            samples[write - 1] = std::move(samples[read]);
        } else {
            if (write != read) samples[write] = std::move(samples[read]);
            ++write;
        }
    }
    samples.resize(write);
}

void trimSamples(QVector<WaveSample>& samples, qint64 start, qint64 end) {
    if (samples.isEmpty()) return;
    auto first = std::lower_bound(samples.begin(), samples.end(), start,
        [](const WaveSample& sample, qint64 time) { return sample.time < time; });
    if (first != samples.begin()) --first;
    auto last = std::upper_bound(first, samples.end(), end,
        [](qint64 time, const WaveSample& sample) { return time < sample.time; });
    QVector<WaveSample> kept;
    kept.reserve(int(last - first));
    for (auto it = first; it != last; ++it) kept.push_back(std::move(*it));
    samples = std::move(kept);
}

void mergeLodLevel(WaveLodLevel& target, const WaveLodLevel& source) {
    if (target.bucketCycles <= 0) target.bucketCycles = source.bucketCycles;
    target.samples.reserve(target.samples.size() + source.samples.size());
    for (const WaveSample& sample : source.samples) target.samples.push_back(sample);
    target.buckets.reserve(target.buckets.size() + source.buckets.size());
    for (const WaveLodBucket& bucket : source.buckets) target.buckets.push_back(bucket);
    target.validRanges += source.validRanges;
    target.loadedRanges += source.loadedRanges;
}

quint64 waveBytes(const WaveFile& wave) {
    quint64 bytes = sizeof(WaveFile);
    bytes += quint64(wave.signalList.capacity()) * sizeof(WaveSignal);
    for (const WaveSignal& signal : wave.signalList) {
        bytes += quint64(signal.samples.capacity()) * sizeof(WaveSample);
        for (const WaveSample& sample : signal.samples) {
            bytes += quint64(sample.value.capacity()) * sizeof(QChar);
        }
        bytes += quint64(signal.changeTimes.capacity()) * sizeof(qint64);
        bytes += quint64(signal.rawLoadedRanges.capacity()) * sizeof(WaveLodValidRange);
        bytes += quint64(signal.lodLevels.capacity()) * sizeof(WaveLodLevel);
        for (const WaveLodLevel& level : signal.lodLevels) {
            bytes += quint64(level.samples.capacity()) * sizeof(WaveSample);
            for (const WaveSample& sample : level.samples) {
                bytes += quint64(sample.value.capacity()) * sizeof(QChar);
            }
            bytes += quint64(level.buckets.capacity()) * sizeof(WaveLodBucket);
            bytes += quint64(level.validRanges.capacity()) * sizeof(WaveLodValidRange);
            bytes += quint64(level.loadedRanges.capacity()) * sizeof(WaveLodValidRange);
        }
    }
    // Account for allocator nodes, QVector headers outside payload estimates,
    // and shared_ptr/cache bookkeeping without pretending they are free.
    return bytes + bytes / 4ull + 4096ull;
}

} // namespace

struct WaveBlockCacheLoader::Impl {
    using Descriptor = WaveParser4Reader::DataBlockDescriptor;

    struct Request {
        QVector<int> signalIds;
        qint64 viewStart = 0;
        qint64 viewEnd = 0;
        qint64 prefetchStart = 0;
        qint64 prefetchEnd = 0;
        qint64 targetBucketCycles = 1;
        quint64 serial = 0;
        Completion completion;
        std::atomic_bool cancelled{false};
    };

    struct CacheEntry {
        std::shared_ptr<const WaveFile> wave;
        quint64 bytes = 0;
        bool hot = false;
        std::list<quint64>::iterator lru;
    };

    std::shared_ptr<WaveParser4Reader> reader;
    std::shared_ptr<std::mutex> readerMutex;
    WaveMeta meta;
    QHash<int, WaveSignal> definitions;
    QVector<Descriptor> blocks;
    QVector<int> backgroundOrder;
    int backgroundCursor = 0;

    mutable std::mutex mutex;
    mutable std::mutex cacheMutex;
    std::condition_variable cv;
    std::thread worker;
    std::thread backgroundWorker;
    bool stopping = false;
    std::deque<std::shared_ptr<Request>> foreground;
    std::deque<WaveFile> garbage;
    std::shared_ptr<Request> active;

    std::unordered_map<quint64, CacheEntry> cache;
    std::list<quint64> lru;
    quint64 cacheBytes = 0;
    quint64 cacheLimit = 0;
    bool backgroundPaused = false;
    std::chrono::steady_clock::time_point backgroundNotBefore =
        std::chrono::steady_clock::time_point::min();

    bool signalRangeIntersects(const Descriptor& block, const QSet<int>& storageIds) const {
        if (block.storageCount <= 0) return false;
        const qint64 last = qint64(block.firstStorageId) + block.storageCount - 1ll;
        for (int storageId : storageIds) {
            if (storageId >= block.firstStorageId && qint64(storageId) <= last) return true;
        }
        return false;
    }

    QVector<int> selectBlocks(const Request& request, bool& lodLoad, qint64& bucket) const {
        QSet<int> storageIds;
        for (int signalId : request.signalIds) {
            const auto found = definitions.constFind(signalId);
            if (found == definitions.constEnd()) continue;
            storageIds.insert(found->storageId > 0 ? found->storageId : found->signalId);
        }

        bucket = 1;
        if (request.targetBucketCycles >= 10) {
            for (const Descriptor& block : blocks) {
                if (block.kind != Descriptor::Kind::Lod ||
                    block.bucketCycles > request.targetBucketCycles ||
                    !overlaps(block.start, block.end, request.prefetchStart, request.prefetchEnd) ||
                    !signalRangeIntersects(block, storageIds)) {
                    continue;
                }
                bucket = qMax(bucket, block.bucketCycles);
            }
        }
        lodLoad = bucket > 1;

        QVector<int> selected;
        for (int index = 0; index < blocks.size(); ++index) {
            const Descriptor& block = blocks.at(index);
            if (lodLoad) {
                if (block.kind != Descriptor::Kind::Lod || block.bucketCycles != bucket) continue;
            } else if (block.kind != Descriptor::Kind::Raw) {
                continue;
            }
            if (!overlaps(block.start, block.end, request.prefetchStart, request.prefetchEnd)) continue;
            if (!signalRangeIntersects(block, storageIds)) continue;
            selected.push_back(index);
        }
        const qint64 center = request.viewStart + (request.viewEnd - request.viewStart) / 2;
        std::sort(selected.begin(), selected.end(), [&](int a, int b) {
            const Descriptor& lhs = blocks.at(a);
            const Descriptor& rhs = blocks.at(b);
            const qint64 lhsCenter = lhs.start + (lhs.end - lhs.start) / 2;
            const qint64 rhsCenter = rhs.start + (rhs.end - rhs.start) / 2;
            const quint64 lhsDistance = lhsCenter >= center
                ? quint64(lhsCenter - center) : quint64(center - lhsCenter);
            const quint64 rhsDistance = rhsCenter >= center
                ? quint64(rhsCenter - center) : quint64(center - rhsCenter);
            return lhsDistance < rhsDistance;
        });
        return selected;
    }

    std::shared_ptr<const WaveFile> cached(const Descriptor& block, bool promote) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const quint64 key = blockKey(block);
        auto found = cache.find(key);
        if (found == cache.end()) return {};
        lru.erase(found->second.lru);
        lru.push_front(key);
        found->second.lru = lru.begin();
        if (promote) found->second.hot = true;
        return found->second.wave;
    }

    bool evictOne(bool allowHot) {
        for (auto it = lru.end(); it != lru.begin();) {
            --it;
            auto found = cache.find(*it);
            if (found == cache.end()) {
                it = lru.erase(it);
                continue;
            }
            if (!allowHot && found->second.hot) continue;
            cacheBytes -= qMin(cacheBytes, found->second.bytes);
            cache.erase(found);
            lru.erase(it);
            return true;
        }
        return false;
    }

    bool insert(const Descriptor& block,
                const std::shared_ptr<const WaveFile>& wave,
                quint64 bytes,
                bool foregroundLoad) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const quint64 key = blockKey(block);
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            if (foregroundLoad) existing->second.hot = true;
            return true;
        }
        if (bytes > cacheLimit) return false;
        if (!foregroundLoad && cacheBytes + bytes > cacheLimit) return false;
        while (cacheBytes + bytes > cacheLimit) {
            if (!evictOne(false) && !evictOne(true)) return false;
        }
        lru.push_front(key);
        CacheEntry entry;
        entry.wave = wave;
        entry.bytes = bytes;
        entry.hot = foregroundLoad;
        entry.lru = lru.begin();
        cache.emplace(key, std::move(entry));
        cacheBytes += bytes;
        return true;
    }

    bool canAdmitBackground(const Descriptor& block) const {
        std::lock_guard<std::mutex> lock(cacheMutex);
        return block.estimatedDecodedBytes <= cacheLimit &&
               cacheBytes + block.estimatedDecodedBytes <= cacheLimit;
    }

    std::shared_ptr<const WaveFile> load(const Descriptor& block,
                                         bool foregroundLoad,
                                         QString& error) {
        if (std::shared_ptr<const WaveFile> hit = cached(block, foregroundLoad)) return hit;
        std::shared_ptr<WaveFile> decoded(new WaveFile);
        if (foregroundLoad) {
            std::lock_guard<std::mutex> readerLock(*readerMutex);
            if (!reader->loadDataBlock(block, *decoded, error,
                                       20ull * 1000ull * 1000ull)) {
                return {};
            }
        } else {
            // The parser's indexed reads are const and open an independent
            // QFile for each operation.  Background warming must not take the
            // foreground reader mutex: doing so lets a large speculative block
            // stall an interactive viewport request even on another thread.
            if (!reader->loadDataBlock(block, *decoded, error, 0)) return {};
        }
        const quint64 bytes = qMax(waveBytes(*decoded), block.estimatedDecodedBytes);
        insert(block, decoded, bytes, foregroundLoad);
        return decoded;
    }

    WaveFile assemble(const Request& request,
                      bool lodLoad,
                      qint64 bucket,
                      const QVector<std::shared_ptr<const WaveFile>>& parts) const {
        WaveFile output;
        output.meta = meta;
        QHash<int, int> outputIndex;
        for (int signalId : request.signalIds) {
            const auto found = definitions.constFind(signalId);
            if (found == definitions.constEnd() || outputIndex.contains(signalId)) continue;
            WaveSignal signal = found.value();
            signal.samples.clear();
            signal.changeTimes.clear();
            signal.changeTimesReady = false;
            signal.rawLoadedRanges.clear();
            signal.lodLevels.clear();
            signal.samplesLoaded = false;
            outputIndex.insert(signalId, output.signalList.size());
            output.signalList.push_back(std::move(signal));
        }

        for (const std::shared_ptr<const WaveFile>& part : parts) {
            if (!part) continue;
            for (const WaveSignal& source : part->signalList) {
                const auto targetIt = outputIndex.constFind(source.signalId);
                if (targetIt == outputIndex.constEnd()) continue;
                WaveSignal& target = output.signalList[targetIt.value()];
                if (!lodLoad) {
                    target.samples.reserve(target.samples.size() + source.samples.size());
                    for (const WaveSample& sample : source.samples) target.samples.push_back(sample);
                    target.supportsZState = target.supportsZState || source.supportsZState;
                    continue;
                }
                for (const WaveLodLevel& sourceLevel : source.lodLevels) {
                    if (sourceLevel.bucketCycles != bucket) continue;
                    int levelIndex = -1;
                    for (int i = 0; i < target.lodLevels.size(); ++i) {
                        if (target.lodLevels.at(i).bucketCycles == bucket) {
                            levelIndex = i;
                            break;
                        }
                    }
                    if (levelIndex < 0) {
                        levelIndex = target.lodLevels.size();
                        target.lodLevels.push_back(WaveLodLevel());
                        target.lodLevels[levelIndex].bucketCycles = bucket;
                    }
                    mergeLodLevel(target.lodLevels[levelIndex], sourceLevel);
                }
            }
        }

        for (WaveSignal& signal : output.signalList) {
            if (!lodLoad) {
                compactSamples(signal.samples);
                trimSamples(signal.samples, request.prefetchStart, request.prefetchEnd);
                WaveLodValidRange range;
                range.start = request.prefetchStart;
                range.end = request.prefetchEnd;
                signal.rawLoadedRanges.push_back(range);
                rebuildWaveSignalDerivedCaches(signal);
                continue;
            }
            for (WaveLodLevel& level : signal.lodLevels) {
                compactSamples(level.samples);
                trimSamples(level.samples, request.prefetchStart, request.prefetchEnd);
                compactRanges(level.validRanges);
                level.loadedRanges.clear();
                WaveLodValidRange range;
                range.start = request.prefetchStart;
                range.end = request.prefetchEnd;
                level.loadedRanges.push_back(range);
            }
        }
        return output;
    }

    void processRequest(const std::shared_ptr<Request>& request) {
        QElapsedTimer timer;
        timer.start();
        Result result;
        result.serial = request->serial;
        result.start = request->viewStart;
        result.end = request->viewEnd;
        result.signalIds = request->signalIds;

        bool lodLoad = false;
        qint64 bucket = 1;
        const QVector<int> selected = selectBlocks(*request, lodLoad, bucket);
        result.lodLoad = lodLoad;
        result.bucketCycles = bucket;
        QVector<std::shared_ptr<const WaveFile>> parts;
        parts.resize(selected.size());
        QVector<int> missingPositions;
        missingPositions.reserve(selected.size());
        for (int position = 0; position < selected.size(); ++position) {
            const int blockIndex = selected.at(position);
            parts[position] = cached(blocks.at(blockIndex), true);
            if (!parts.at(position)) missingPositions.push_back(position);
        }

        // Calling loadSignals once per physical RAW block is deliberately kept
        // for small misses so those blocks become reusable cache entries.  For a
        // highly fragmented file, however, a viewport may overlap hundreds of
        // tiny blocks. Reopening/dispatching the indexed reader for every block
        // makes the request effectively serial and leaves the canvas blank until
        // the entire batch completes. The indexed range reader already decodes
        // all overlapping blocks in parallel, so use one bounded foreground read
        // when the miss fan-out is large. Background warming still populates the
        // physical block cache independently.
        constexpr int kMaxIndividualForegroundRawMisses = 8;
        constexpr int kMaxIndividualForegroundLodMisses = 8;
        if (lodLoad && missingPositions.size() > kMaxIndividualForegroundLodMisses) {
            QString error;
            {
                std::lock_guard<std::mutex> readerLock(*readerMutex);
                if (!reader->loadSignalLod(request->signalIds, result.wave, error,
                                           request->prefetchStart, request->prefetchEnd, bucket)) {
                    result.error = error;
                    result.elapsedMs = timer.elapsed();
                    if (!request->cancelled.load(std::memory_order_acquire) && request->completion) {
                        request->completion(std::move(result));
                    }
                    return;
                }
            }
            if (request->cancelled.load(std::memory_order_acquire)) return;

            QVector<int> fallbackSignalIds;
            for (WaveSignal& signal : result.wave.signalList) {
                signal.samplesLoaded = false;
                bool haveLod = false;
                for (WaveLodLevel& level : signal.lodLevels) {
                    if (level.bucketCycles != bucket) continue;
                    level.loadedRanges.clear();
                    WaveLodValidRange range;
                    range.start = request->prefetchStart;
                    range.end = request->prefetchEnd;
                    level.loadedRanges.push_back(range);
                    if (!level.samples.isEmpty() || !level.buckets.isEmpty()) haveLod = true;
                }
                if (!haveLod) fallbackSignalIds.push_back(signal.signalId);
            }
            if (!fallbackSignalIds.isEmpty()) {
                WaveFile fallback;
                QString fallbackError;
                {
                    std::lock_guard<std::mutex> readerLock(*readerMutex);
                    if (!reader->loadSignals(fallbackSignalIds, fallback, fallbackError,
                                             20ull * 1000ull * 1000ull,
                                             request->prefetchStart, request->prefetchEnd)) {
                        result.error = fallbackError;
                        result.elapsedMs = timer.elapsed();
                        if (!request->cancelled.load(std::memory_order_acquire) &&
                            request->completion) {
                            request->completion(std::move(result));
                        }
                        return;
                    }
                }
                QHash<int, int> outputIndex;
                for (int i = 0; i < result.wave.signalList.size(); ++i) {
                    outputIndex.insert(result.wave.signalList.at(i).signalId, i);
                }
                for (WaveSignal& source : fallback.signalList) {
                    const auto found = outputIndex.constFind(source.signalId);
                    if (found == outputIndex.constEnd()) continue;
                    WaveSignal& target = result.wave.signalList[found.value()];
                    target.samples = std::move(source.samples);
                    target.supportsZState = source.supportsZState;
                    target.rawLoadedRanges.clear();
                    WaveLodValidRange range;
                    range.start = request->prefetchStart;
                    range.end = request->prefetchEnd;
                    target.rawLoadedRanges.push_back(range);
                    rebuildWaveSignalDerivedCaches(target);
                }
            }
            result.ok = true;
            result.elapsedMs = timer.elapsed();
            if (request->completion) request->completion(std::move(result));
            return;
        }
        if (!lodLoad && missingPositions.size() > kMaxIndividualForegroundRawMisses) {
            QString error;
            {
                std::lock_guard<std::mutex> readerLock(*readerMutex);
                if (!reader->loadSignals(request->signalIds, result.wave, error,
                                         20ull * 1000ull * 1000ull,
                                         request->prefetchStart, request->prefetchEnd)) {
                    result.error = error;
                    result.elapsedMs = timer.elapsed();
                    if (!request->cancelled.load(std::memory_order_acquire) && request->completion) {
                        request->completion(std::move(result));
                    }
                    return;
                }
            }
            if (request->cancelled.load(std::memory_order_acquire)) return;
            for (WaveSignal& signal : result.wave.signalList) {
                signal.samplesLoaded = false;
                signal.rawLoadedRanges.clear();
                WaveLodValidRange range;
                range.start = request->prefetchStart;
                range.end = request->prefetchEnd;
                signal.rawLoadedRanges.push_back(range);
                rebuildWaveSignalDerivedCaches(signal);
            }
            result.ok = true;
            result.elapsedMs = timer.elapsed();
            if (request->completion) request->completion(std::move(result));
            return;
        }

        for (int position : missingPositions) {
            if (request->cancelled.load(std::memory_order_acquire)) return;
            const int blockIndex = selected.at(position);
            QString error;
            std::shared_ptr<const WaveFile> part = load(blocks.at(blockIndex), true, error);
            if (!part) {
                result.error = error.isEmpty()
                    ? QStringLiteral("Unable to load waveform cache block") : error;
                result.elapsedMs = timer.elapsed();
                if (!request->cancelled.load(std::memory_order_acquire) && request->completion) {
                    request->completion(std::move(result));
                }
                return;
            }
            parts[position] = std::move(part);
        }
        if (request->cancelled.load(std::memory_order_acquire)) return;
        result.wave = assemble(*request, lodLoad, bucket, parts);

        QVector<int> fallbackSignalIds;
        for (const WaveSignal& signal : result.wave.signalList) {
            bool haveLod = false;
            for (const WaveLodLevel& level : signal.lodLevels) {
                if (level.bucketCycles == bucket &&
                    (!level.samples.isEmpty() || !level.buckets.isEmpty())) {
                    haveLod = true;
                    break;
                }
            }
            if (signal.samples.isEmpty() && (!lodLoad || !haveLod)) {
                fallbackSignalIds.push_back(signal.signalId);
            }
        }
        if (!fallbackSignalIds.isEmpty()) {
            WaveFile fallback;
            QString fallbackError;
            {
                std::lock_guard<std::mutex> readerLock(*readerMutex);
                if (!reader->loadSignals(fallbackSignalIds, fallback, fallbackError,
                                         20ull * 1000ull * 1000ull,
                                         request->prefetchStart, request->prefetchEnd)) {
                    result.error = fallbackError;
                    result.elapsedMs = timer.elapsed();
                    if (!request->cancelled.load(std::memory_order_acquire) && request->completion) {
                        request->completion(std::move(result));
                    }
                    return;
                }
            }
            QHash<int, int> outputIndex;
            for (int i = 0; i < result.wave.signalList.size(); ++i) {
                outputIndex.insert(result.wave.signalList.at(i).signalId, i);
            }
            for (WaveSignal& source : fallback.signalList) {
                const auto found = outputIndex.constFind(source.signalId);
                if (found == outputIndex.constEnd()) continue;
                WaveSignal& target = result.wave.signalList[found.value()];
                target.samples = std::move(source.samples);
                target.supportsZState = source.supportsZState;
                WaveLodValidRange range;
                range.start = request->prefetchStart;
                range.end = request->prefetchEnd;
                target.rawLoadedRanges.clear();
                target.rawLoadedRanges.push_back(range);
                rebuildWaveSignalDerivedCaches(target);
            }
        }
        result.ok = true;
        result.elapsedMs = timer.elapsed();
        if (request->completion) request->completion(std::move(result));
    }

    bool processOneBackground() {
        if (backgroundCursor >= backgroundOrder.size()) return false;
        const Descriptor& block = blocks.at(backgroundOrder.at(backgroundCursor));
        if (cached(block, false)) {
            ++backgroundCursor;
            return true;
        }
        if (!canAdmitBackground(block)) {
            std::lock_guard<std::mutex> lock(mutex);
            backgroundPaused = true;
            return false;
        }
        QString error;
        std::shared_ptr<const WaveFile> decoded = load(block, false, error);
        if (!decoded) {
            // Corrupt background blocks are skipped; a foreground request will
            // surface the concrete error if that block is actually needed.
            ++backgroundCursor;
            return true;
        }
        ++backgroundCursor;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }

    void run() {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
#endif
        for (;;) {
            std::shared_ptr<Request> request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() {
                    return stopping || !foreground.empty() || !garbage.empty();
                });
                if (stopping) return;
                if (!garbage.empty()) {
                    WaveFile released = std::move(garbage.front());
                    garbage.pop_front();
                    lock.unlock();
                    released = WaveFile();
                    continue;
                }
                if (!foreground.empty()) {
                    request = foreground.back();
                    foreground.clear();
                    active = request;
                }
            }
            if (request) {
                processRequest(request);
                std::lock_guard<std::mutex> lock(mutex);
                if (active == request) active.reset();
                backgroundPaused = false;
                cv.notify_all();
            }
        }
    }

    void runBackground() {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() {
                    return stopping ||
                           (!backgroundPaused && backgroundCursor < backgroundOrder.size());
                });
                if (stopping) return;
                while (!stopping && !backgroundPaused &&
                       std::chrono::steady_clock::now() < backgroundNotBefore) {
                    // A foreground request can push the deadline farther out;
                    // always re-read it after wake-up instead of waiting on a
                    // stale snapshot.
                    cv.wait_until(lock, backgroundNotBefore);
                }
                if (stopping) return;
                if (backgroundPaused) continue;
            }
            if (!processOneBackground()) {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() { return stopping || !backgroundPaused; });
                if (stopping) return;
            }
        }
    }
};

WaveBlockCacheLoader::WaveBlockCacheLoader()
    : d(new Impl) {}

WaveBlockCacheLoader::~WaveBlockCacheLoader() {
    stop();
}

void WaveBlockCacheLoader::start(const std::shared_ptr<WaveParser4Reader>& reader,
                                 const std::shared_ptr<std::mutex>& readerMutex,
                                 const WaveFile& directory,
                                 quint64 totalBudgetBytes) {
    stop();
    d.reset(new Impl);
    d->reader = reader;
    d->readerMutex = readerMutex;
    d->meta = directory.meta;
    for (const WaveSignal& signal : directory.signalList) {
        if (signal.signalId > 0) d->definitions.insert(signal.signalId, signal);
    }
    d->blocks = reader ? reader->dataBlocks() : QVector<WaveParser4Reader::DataBlockDescriptor>();
    d->cacheLimit = totalBudgetBytes > kDecodeReserveBytes
        ? totalBudgetBytes - kDecodeReserveBytes
        : totalBudgetBytes * 3ull / 4ull;
    const bool disableBackground = qEnvironmentVariableIsSet("WV_VIEWER_DISABLE_BACKGROUND_CACHE") &&
        qgetenv("WV_VIEWER_DISABLE_BACKGROUND_CACHE") != QByteArray("0");
    d->backgroundOrder.resize(disableBackground ? 0 : d->blocks.size());
    for (int i = 0; i < d->backgroundOrder.size(); ++i) d->backgroundOrder[i] = i;
    std::sort(d->backgroundOrder.begin(), d->backgroundOrder.end(), [&](int a, int b) {
        const auto& lhs = d->blocks.at(a);
        const auto& rhs = d->blocks.at(b);
        if (lhs.kind != rhs.kind) return lhs.kind == WaveParser4Reader::DataBlockDescriptor::Kind::Lod;
        if (lhs.kind == WaveParser4Reader::DataBlockDescriptor::Kind::Lod &&
            lhs.bucketCycles != rhs.bucketCycles) {
            return lhs.bucketCycles > rhs.bucketCycles;
        }
        if (lhs.start != rhs.start) return lhs.start < rhs.start;
        return lhs.signalChunkId < rhs.signalChunkId;
    });
    d->worker = std::thread([impl = d.get()]() { impl->run(); });
    d->backgroundWorker = std::thread([impl = d.get()]() { impl->runBackground(); });
    d->cv.notify_all();
}

void WaveBlockCacheLoader::stop() {
    if (!d) return;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->stopping = true;
        if (d->active) d->active->cancelled.store(true, std::memory_order_release);
        for (const auto& request : d->foreground) {
            request->cancelled.store(true, std::memory_order_release);
        }
        d->foreground.clear();
    }
    d->cv.notify_all();
    if (d->worker.joinable()) d->worker.join();
    if (d->backgroundWorker.joinable()) d->backgroundWorker.join();
}

void WaveBlockCacheLoader::requestViewport(const QVector<int>& signalIds,
                                           qint64 viewStart,
                                           qint64 viewEnd,
                                           qint64 prefetchStart,
                                           qint64 prefetchEnd,
                                           qint64 targetBucketCycles,
                                           quint64 serial,
                                           Completion completion) {
    if (!d || !d->reader || signalIds.isEmpty() || viewEnd <= viewStart ||
        prefetchEnd <= prefetchStart) return;
    std::shared_ptr<Impl::Request> request(new Impl::Request);
    request->signalIds = signalIds;
    request->viewStart = viewStart;
    request->viewEnd = viewEnd;
    request->prefetchStart = prefetchStart;
    request->prefetchEnd = prefetchEnd;
    request->targetBucketCycles = qMax<qint64>(1, targetBucketCycles);
    request->serial = serial;
    request->completion = std::move(completion);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->active) d->active->cancelled.store(true, std::memory_order_release);
        for (const auto& old : d->foreground) {
            old->cancelled.store(true, std::memory_order_release);
        }
        d->foreground.clear();
        d->foreground.push_back(request);
        d->backgroundPaused = false;
        d->backgroundNotBefore = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(500);
    }
    d->cv.notify_all();
}

void WaveBlockCacheLoader::releaseWaveLater(WaveFile&& wave) {
    if (!d) return;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->stopping) return;
        d->garbage.push_back(std::move(wave));
    }
    d->cv.notify_all();
}

quint64 WaveBlockCacheLoader::cachedBytes() const {
    if (!d) return 0;
    std::lock_guard<std::mutex> lock(d->cacheMutex);
    return d->cacheBytes;
}

quint64 WaveBlockCacheLoader::cacheLimitBytes() const {
    if (!d) return 0;
    std::lock_guard<std::mutex> lock(d->cacheMutex);
    return d->cacheLimit;
}

int WaveBlockCacheLoader::cachedBlockCount() const {
    if (!d) return 0;
    std::lock_guard<std::mutex> lock(d->cacheMutex);
    return int(d->cache.size());
}

qint64 WaveBlockCacheLoader::preferredBucketCycles(qint64 targetBucketCycles) const {
    if (!d || targetBucketCycles < 1) return 1;
    qint64 best = 1;
    for (const Impl::Descriptor& block : d->blocks) {
        if (block.kind != Impl::Descriptor::Kind::Lod) continue;
        if (block.bucketCycles > best && block.bucketCycles <= targetBucketCycles) {
            best = block.bucketCycles;
        }
    }
    return best;
}
