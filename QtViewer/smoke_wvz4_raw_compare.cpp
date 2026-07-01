#include "WaveParser4.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QFileInfo>
#include <QString>
#include <QTextStream>

#include <algorithm>
#include <iostream>

namespace {

QString signal_key(const WaveSignal& signal) {
    return signal.name;
}

bool samples_equal(const WaveSample& left, const WaveSample& right) {
    return left.time == right.time &&
           left.rawBits == right.rawBits &&
           left.rawFieldsReady == right.rawFieldsReady &&
           left.isZ == right.isZ &&
           left.isAbsent == right.isAbsent;
}

bool load_full_raw(const char* path, WaveFile& wave, QString& error) {
    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.autoLoadFirstSignalCount = -1;
    options.loadRawSamples = true;
    options.loadAllIfWindowEmpty = true;
    options.maxDecodedSamples = 0;
    return WaveParser4::loadFromFile(QString::fromLocal8Bit(path), wave, error, options);
}

qint64 parse_i64_arg(const char* text, qint64 fallback) {
    bool ok = false;
    const qint64 value = QString::fromLocal8Bit(text ? text : "").toLongLong(&ok, 10);
    return ok ? value : fallback;
}

int parse_int_arg(const char* text, int fallback) {
    bool ok = false;
    const int value = QString::fromLocal8Bit(text ? text : "").toInt(&ok, 10);
    return ok ? value : fallback;
}

int run_load_benchmark(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: smoke_wvz4_raw_compare --load-benchmark <file.wvz4> <signals> <raw|lod> [start] [end] [bucket]\n";
        return 2;
    }

    const QString filePath = QString::fromLocal8Bit(argv[2]);
    const int requestedSignals = std::max(1, parse_int_arg(argv[3], 10));
    const QString mode = QString::fromLocal8Bit(argv[4]).toLower();
    const qint64 start = (argc >= 6) ? parse_i64_arg(argv[5], 0) : 0;
    const qint64 end = (argc >= 7) ? parse_i64_arg(argv[6], std::numeric_limits<qint64>::max()) :
        std::numeric_limits<qint64>::max();
    const qint64 bucket = (argc >= 8) ? std::max<qint64>(0, parse_i64_arg(argv[7], 0)) : 0;
    const bool rawMode = (mode == QStringLiteral("raw"));
    const bool lodMode = (mode == QStringLiteral("lod"));
    if (!rawMode && !lodMode) {
        std::cerr << "mode must be raw or lod\n";
        return 2;
    }

    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.loadAllIfWindowEmpty = false;
    options.timeStart = start;
    options.timeEnd = end;
    options.lodTargetBucketCycles = bucket;
    options.maxDecodedSamples = 0;
    if (rawMode) {
        options.loadRawSamples = true;
        options.autoLoadFirstSignalCount = requestedSignals;
        options.autoLoadFirstSignalLodCount = 0;
    } else {
        options.loadRawSamples = false;
        options.autoLoadFirstSignalCount = 0;
        options.autoLoadFirstSignalLodCount = requestedSignals;
    }

    WaveFile wave;
    QString error;
    QElapsedTimer timer;
    timer.start();
    if (!WaveParser4::loadFromFile(filePath, wave, error, options)) {
        std::cerr << "load failed: " << error.toLocal8Bit().constData() << "\n";
        return 3;
    }
    const qint64 loadMs = timer.elapsed();

    qint64 rawSamples = 0;
    qint64 lodSamples = 0;
    qint64 lodLevels = 0;
    int lodSignals = 0;
    int rawSignals = 0;
    for (const WaveSignal& signal : wave.signalList) {
        if (signal.samplesLoaded && !signal.samples.isEmpty()) ++rawSignals;
        rawSamples += signal.samples.size();
        bool hasLod = false;
        for (const WaveLodLevel& level : signal.lodLevels) {
            if (level.samples.isEmpty() && level.buckets.isEmpty()) continue;
            hasLod = true;
            ++lodLevels;
            lodSamples += level.samples.size();
        }
        if (hasLod) ++lodSignals;
    }

    QTextStream out(stdout);
    out << "metric,value\n";
    out << "file," << QFileInfo(filePath).fileName() << "\n";
    out << "mode," << mode << "\n";
    out << "requested_signals," << requestedSignals << "\n";
    out << "signals_in_directory," << wave.signalList.size() << "\n";
    out << "raw_signals," << rawSignals << "\n";
    out << "lod_signals," << lodSignals << "\n";
    out << "raw_samples," << rawSamples << "\n";
    out << "lod_levels," << lodLevels << "\n";
    out << "lod_samples," << lodSamples << "\n";
    out << "time_start," << start << "\n";
    out << "time_end," << end << "\n";
    out << "target_bucket," << bucket << "\n";
    out << "load_ms," << loadMs << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--load-benchmark")) {
        return run_load_benchmark(argc, argv);
    }
    if (argc < 3) {
        std::cerr << "usage: smoke_wvz4_raw_compare <left.wvz4> <right.wvz4>\n";
        return 2;
    }

    QString error;
    WaveFile left;
    WaveFile right;
    if (!load_full_raw(argv[1], left, error)) {
        std::cerr << "left load failed: " << error.toLocal8Bit().constData() << "\n";
        return 3;
    }
    if (!load_full_raw(argv[2], right, error)) {
        std::cerr << "right load failed: " << error.toLocal8Bit().constData() << "\n";
        return 4;
    }

    QHash<QString, int> rightByName;
    for (int i = 0; i < right.signalList.size(); ++i) {
        rightByName.insert(signal_key(right.signalList.at(i)), i);
    }

    if (left.signalList.size() != right.signalList.size()) {
        std::cerr << "signal count differs: left=" << left.signalList.size()
                  << " right=" << right.signalList.size() << "\n";
        return 5;
    }

    quint64 checkedSamples = 0;
    for (const WaveSignal& leftSignal : left.signalList) {
        const QString key = signal_key(leftSignal);
        const auto it = rightByName.constFind(key);
        if (it == rightByName.constEnd()) {
            std::cerr << "missing right signal: " << key.toLocal8Bit().constData() << "\n";
            return 6;
        }

        const WaveSignal& rightSignal = right.signalList.at(*it);
        if (leftSignal.width != rightSignal.width ||
            leftSignal.kind != rightSignal.kind ||
            leftSignal.samples.size() != rightSignal.samples.size()) {
            std::cerr << "signal metadata/sample count differs: "
                      << key.toLocal8Bit().constData()
                      << " left_width=" << leftSignal.width
                      << " right_width=" << rightSignal.width
                      << " left_samples=" << leftSignal.samples.size()
                      << " right_samples=" << rightSignal.samples.size() << "\n";
            return 7;
        }

        for (int i = 0; i < leftSignal.samples.size(); ++i) {
            if (!samples_equal(leftSignal.samples.at(i), rightSignal.samples.at(i))) {
                const WaveSample& l = leftSignal.samples.at(i);
                const WaveSample& r = rightSignal.samples.at(i);
                std::cerr << "sample differs: " << key.toLocal8Bit().constData()
                          << " index=" << i
                          << " left_time=" << l.time
                          << " right_time=" << r.time
                          << " left_bits=" << l.rawBits
                          << " right_bits=" << r.rawBits << "\n";
                return 8;
            }
            ++checkedSamples;
        }
    }

    std::cout << "matched_signals," << left.signalList.size() << "\n";
    std::cout << "matched_samples," << checkedSamples << "\n";
    return 0;
}
