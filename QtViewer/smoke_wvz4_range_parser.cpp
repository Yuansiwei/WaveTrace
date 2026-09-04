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
    int proceduralClockCount = 0;
    for (int signalIndex = 0; signalIndex < wave.signalList.size(); ++signalIndex) {
        const WaveSignal& signal = wave.signalList.at(signalIndex);
        if (signal.proceduralClock) {
            ++proceduralClockCount;
            if (signal.clockTogglePeriodTicks == 0 ||
                !signal.samples.isEmpty() ||
                !signal.lodLevels.isEmpty()) {
                std::cerr << "procedural clock materialized samples or has invalid metadata\n";
                return 8;
            }

            // Exercise the formula beyond the deleted one-million-transition
            // expansion cap. This must remain constant-memory.
            const quint64 probeMultiple = 1000001ull;
            const quint64 maxTime = quint64((std::numeric_limits<qint64>::max)());
            if (signal.clockTogglePeriodTicks <= (maxTime - 1u) / probeMultiple) {
                const qint64 probe =
                    qint64(signal.clockTogglePeriodTicks * probeMultiple);
                if (waveProceduralClockTransitionAtOrAfter(signal, probe) != probe ||
                    waveProceduralClockPreviousTransition(signal, probe + 1) != probe ||
                    waveProceduralClockValueAtTime(signal, probe) ==
                        waveProceduralClockValueAtTime(signal, probe - 1)) {
                    std::cerr << "procedural clock million-transition formula mismatch\n";
                    return 9;
                }
            }

            qint64 finalTransition = -1;
            if (wave.meta.end < (std::numeric_limits<qint64>::max)()) {
                finalTransition =
                    waveProceduralClockPreviousTransition(signal, wave.meta.end + 1);
            } else {
                finalTransition =
                    waveProceduralClockTransitionAtOrAfter(signal, wave.meta.end);
                if (finalTransition != wave.meta.end) {
                    finalTransition =
                        waveProceduralClockPreviousTransition(signal, wave.meta.end);
                }
            }
            if (finalTransition >= wave.meta.start && finalTransition <= wave.meta.end) {
                lastSample = qMax(lastSample, finalTransition);
            }
        }
        for (const WaveSample& sample : signal.samples) {
            firstSample = qMin(firstSample, sample.time);
            lastSample = qMax(lastSample, sample.time);
            if (!signal.proceduralClock) {
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

    WaveParser4Reader reader;
    QString readerError;
    if (!reader.open(QString::fromLocal8Bit(argv[1]), readerError)) {
        std::cerr << "directory reader open failed: "
                  << readerError.toLocal8Bit().constData() << "\n";
        return 10;
    }
    QVector<int> clockIds;
    for (const WaveSignal& signal : reader.directoryWave().signalList) {
        if (signal.proceduralClock) clockIds.push_back(signal.signalId);
    }
    if (clockIds.size() != proceduralClockCount) {
        std::cerr << "directory reader procedural clock count mismatch\n";
        return 11;
    }
    if (!clockIds.isEmpty()) {
        WaveFile rawClockWave;
        if (!reader.loadSignals(clockIds, rawClockWave, readerError, 1,
                                wave.meta.start, wave.meta.end) ||
            rawClockWave.signalList.size() != clockIds.size()) {
            std::cerr << "directory reader clock raw load failed: "
                      << readerError.toLocal8Bit().constData() << "\n";
            return 12;
        }
        WaveFile lodClockWave;
        if (!reader.loadSignalLod(clockIds, lodClockWave, readerError,
                                  wave.meta.start, wave.meta.end, 100) ||
            lodClockWave.signalList.size() != clockIds.size()) {
            std::cerr << "directory reader clock LOD load failed: "
                      << readerError.toLocal8Bit().constData() << "\n";
            return 13;
        }
        for (const WaveSignal& signal : rawClockWave.signalList) {
            if (!signal.proceduralClock || !signal.samples.isEmpty() ||
                !signal.lodLevels.isEmpty()) {
                std::cerr << "directory reader raw clock was materialized\n";
                return 14;
            }
        }
        for (const WaveSignal& signal : lodClockWave.signalList) {
            if (!signal.proceduralClock || !signal.samples.isEmpty() ||
                !signal.lodLevels.isEmpty()) {
                std::cerr << "directory reader LOD clock was materialized\n";
                return 15;
            }
        }
    }

    std::cout << "wavetrace_range_ok start=" << wave.meta.start
              << " end=" << wave.meta.end
              << " first_sample=" << firstSample
              << " last_sample=" << lastSample
              << " last_non_clock=" << lastNonClockSample
              << " procedural_clocks=" << proceduralClockCount << "\n";
    return 0;
}
