#include "WaveParser4.h"

#include <QCoreApplication>
#include <QString>

#include <iostream>
#include <limits>

namespace {

int fail(const QString& message) {
    std::cerr << message.toLocal8Bit().constData() << '\n';
    return 1;
}

QVector<WaveSample> expectedLevel(const QVector<WaveSample>& raw, qint64 bucketCycles) {
    QVector<WaveSample> expected;
    qint64 currentBucket = -1;
    for (const WaveSample& sample : raw) {
        const qint64 bucket = sample.time >= 0 ? sample.time / bucketCycles : -1;
        if (bucket < 0) continue;
        if (bucket != currentBucket) {
            expected.push_back(sample);
            currentBucket = bucket;
        } else {
            expected.last() = sample;
        }
    }
    return expected;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || argc > 3) {
        return fail(QStringLiteral("usage: smoke_wvz4_fixed_lod_parser <input.wvz4> [lod10-bucket=10]"));
    }
    bool bucketOk = false;
    const qint64 lod10Bucket = argc == 3
        ? QString::fromLocal8Bit(argv[2]).toLongLong(&bucketOk)
        : 10;
    if (argc == 2) bucketOk = true;
    if (!bucketOk || lod10Bucket <= 0 || lod10Bucket > std::numeric_limits<qint64>::max() / 100) {
        return fail(QStringLiteral("invalid lod10 bucket"));
    }

    WaveParser4::LoadOptions options;
    options.signalIds.push_back(1);
    options.includeAllSignalDefinitions = false;
    options.loadAllIfWindowEmpty = false;
    options.loadRawSamples = true;
    options.lodTargetBucketCycles = 0;
    options.maxDecodedSamples = 10000000;

    WaveFile wave;
    QString error;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), wave, error, options)) {
        return fail(error);
    }
    if (wave.signalList.size() != 1) return fail(QStringLiteral("expected one selected signal"));
    const WaveSignal& signal = wave.signalList.first();
    const qint64 expectedBuckets[] = {lod10Bucket, lod10Bucket * 10, lod10Bucket * 100};
    if (signal.lodLevels.size() != 3) {
        return fail(QStringLiteral("expected RAW plus exactly three stored LOD levels, got %1 stored levels")
            .arg(signal.lodLevels.size()));
    }

    for (int levelIndex = 0; levelIndex < 3; ++levelIndex) {
        const WaveLodLevel& level = signal.lodLevels.at(levelIndex);
        if (level.bucketCycles != expectedBuckets[levelIndex]) {
            return fail(QStringLiteral("LOD %1 bucket mismatch: %2")
                .arg(levelIndex).arg(level.bucketCycles));
        }
        const QVector<WaveSample> expected = expectedLevel(signal.samples, level.bucketCycles);
        if (level.samples.size() != expected.size()) {
            return fail(QStringLiteral("LOD %1 sample count mismatch: actual=%2 expected=%3")
                .arg(levelIndex).arg(level.samples.size()).arg(expected.size()));
        }
        for (int i = 0; i < expected.size(); ++i) {
            if (level.samples.at(i).time != expected.at(i).time ||
                !waveSamplesEquivalent(level.samples.at(i), expected.at(i))) {
                return fail(QStringLiteral("LOD %1 sample mismatch at %2").arg(levelIndex).arg(i));
            }
        }
    }
    std::cout << "fixed_lod_ok lod1_raw=" << signal.samples.size();
    for (int i = 0; i < 3; ++i) {
        std::cout << " lod" << signal.lodLevels.at(i).bucketCycles
                  << '=' << signal.lodLevels.at(i).samples.size();
    }
    std::cout << '\n';
    return 0;
}
