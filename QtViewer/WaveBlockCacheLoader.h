#pragma once

#include "WaveParser4.h"

#include <QVector>
#include <functional>
#include <memory>
#include <mutex>

class WaveBlockCacheLoader {
public:
    struct Result {
        bool ok = false;
        bool lodLoad = false;
        quint64 serial = 0;
        qint64 start = 0;
        qint64 end = 0;
        qint64 bucketCycles = 1;
        qint64 elapsedMs = 0;
        QString error;
        QVector<int> signalIds;
        WaveFile wave;
    };

    using Completion = std::function<void(Result&&)>;

    WaveBlockCacheLoader();
    ~WaveBlockCacheLoader();
    WaveBlockCacheLoader(const WaveBlockCacheLoader&) = delete;
    WaveBlockCacheLoader& operator=(const WaveBlockCacheLoader&) = delete;

    void start(const std::shared_ptr<WaveParser4Reader>& reader,
               const std::shared_ptr<std::mutex>& readerMutex,
               const WaveFile& directory,
               quint64 totalBudgetBytes = 32ull * 1024ull * 1024ull * 1024ull);
    void stop();
    void pauseBackground();
    void resumeBackground();

    void requestViewport(const QVector<int>& signalIds,
                         qint64 viewStart,
                         qint64 viewEnd,
                         qint64 prefetchStart,
                         qint64 prefetchEnd,
                         qint64 targetBucketCycles,
                         quint64 serial,
                         Completion completion);
    void releaseWaveLater(WaveFile&& wave);

    quint64 cachedBytes() const;
    quint64 cacheLimitBytes() const;
    int cachedBlockCount() const;
    qint64 preferredBucketCycles(qint64 targetBucketCycles) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
