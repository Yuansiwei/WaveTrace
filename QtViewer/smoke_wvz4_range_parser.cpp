#include "WaveParser4.h"

#include <QCoreApplication>
#include <QString>

#include <cstdlib>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 5 && argc != 6) {
        std::cerr << "usage: smoke_wvz4_range_parser <file> <expected-start> <expected-end> <expected-last-sample> [expected-last-non-clock]\n";
        return 2;
    }

    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.loadRawSamples = true;
    options.autoLoadFirstSignalCount = -1;
    options.maxDecodedSamples = 1000000;

    WaveFile wave;
    QString error;
    if (!WaveParser4::loadFromFile(QString::fromLocal8Bit(argv[1]), wave, error, options)) {
        std::cerr << error.toLocal8Bit().constData() << "\n";
        return 3;
    }

    const qint64 expectedStart = QString::fromLocal8Bit(argv[2]).toLongLong();
    const qint64 expectedEnd = QString::fromLocal8Bit(argv[3]).toLongLong();
    const qint64 expectedLastSample = QString::fromLocal8Bit(argv[4]).toLongLong();
    const bool checkLastNonClock = argc == 6;
    const qint64 expectedLastNonClock = checkLastNonClock
        ? QString::fromLocal8Bit(argv[5]).toLongLong()
        : 0;
    if (wave.meta.start != expectedStart || wave.meta.end != expectedEnd) {
        std::cerr << "range mismatch: actual=[" << wave.meta.start << "," << wave.meta.end
                  << "] expected=[" << expectedStart << "," << expectedEnd << "]\n";
        return 4;
    }

    qint64 firstSample = (std::numeric_limits<qint64>::max)();
    qint64 lastSample = (std::numeric_limits<qint64>::min)();
    qint64 lastNonClockSample = (std::numeric_limits<qint64>::min)();
    for (int signalIndex = 0; signalIndex < wave.signalList.size(); ++signalIndex) {
        const WaveSignal& signal = wave.signalList.at(signalIndex);
        for (const WaveSample& sample : signal.samples) {
            firstSample = qMin(firstSample, sample.time);
            lastSample = qMax(lastSample, sample.time);
            if (waveSignalSegmentName(wave, signalIndex) != QStringLiteral("clk")) {
                lastNonClockSample = qMax(lastNonClockSample, sample.time);
            }
        }
    }
    if (firstSample < expectedStart) {
        std::cerr << "sample before configured start: " << firstSample << "\n";
        return 5;
    }
    if (lastSample != expectedLastSample) {
        std::cerr << "last sample mismatch: actual=" << lastSample
                  << " expected=" << expectedLastSample << "\n";
        return 6;
    }
    if (checkLastNonClock && lastNonClockSample != expectedLastNonClock) {
        std::cerr << "last non-clock sample mismatch: actual=" << lastNonClockSample
                  << " expected=" << expectedLastNonClock << "\n";
        return 7;
    }

    std::cout << "wavetrace_range_ok start=" << wave.meta.start
              << " end=" << wave.meta.end
              << " first_sample=" << firstSample
              << " last_sample=" << lastSample
              << " last_non_clock=" << lastNonClockSample << "\n";
    return 0;
}
