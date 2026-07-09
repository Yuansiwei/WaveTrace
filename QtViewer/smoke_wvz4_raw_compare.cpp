#include "WaveParser4.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <iostream>

namespace {

QString signal_key(const WaveFile& wave, int signalIndex) {
    if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return QString();
    if (!wave.tree.valid ||
        signalIndex >= wave.tree.signalIndexToNodeId.size() ||
        wave.tree.signalIndexToNodeId.at(signalIndex) <= 0) {
        return wave.signalList.at(signalIndex).name;
    }

    QStringList parts;
    int nodeId = wave.tree.signalIndexToNodeId.at(signalIndex);
    int guard = 0;
    while (nodeId > 0 &&
           nodeId < wave.tree.nodesById.size() &&
           wave.tree.nodesById.at(nodeId).valid &&
           guard < wave.tree.nodesById.size()) {
        const WaveTreeNode& node = wave.tree.nodesById.at(nodeId);
        parts.prepend(node.name);
        nodeId = node.parentId;
        ++guard;
    }
    return parts.join(QStringLiteral("."));
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

bool load_first_n_raw(const QString& path,
                      int requestedSignals,
                      qint64 start,
                      qint64 end,
                      WaveFile& wave,
                      QString& error) {
    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.loadAllIfWindowEmpty = false;
    options.loadRawSamples = true;
    options.autoLoadFirstSignalCount = std::max(1, requestedSignals);
    options.autoLoadFirstSignalLodCount = 0;
    options.timeStart = start;
    options.timeEnd = end;
    options.maxDecodedSamples = 0;
    return WaveParser4::loadFromFile(path, wave, error, options);
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

const WaveSample* sample_at_time(const WaveSignal& signal, qint64 t) {
    if (signal.samples.isEmpty()) return nullptr;
    int lo = 0;
    int hi = signal.samples.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (signal.samples.at(mid).time <= t) lo = mid + 1;
        else hi = mid;
    }
    const int idx = lo - 1;
    return idx >= 0 ? &signal.samples.at(idx) : nullptr;
}

bool sample_value_equal_at_time(const WaveSignal& left,
                                const WaveSignal& right,
                                qint64 t,
                                QString& reason) {
    const WaveSample* l = sample_at_time(left, t);
    const WaveSample* r = sample_at_time(right, t);
    if (!l || !r) {
        reason = QStringLiteral("missing sample at t=%1 left=%2 right=%3")
            .arg(t).arg(l ? 1 : 0).arg(r ? 1 : 0);
        return false;
    }
    if (l->rawBits != r->rawBits ||
        l->rawFieldsReady != r->rawFieldsReady ||
        l->isZ != r->isZ ||
        l->isAbsent != r->isAbsent) {
        reason = QStringLiteral("value differs at t=%1 left_bits=%2 right_bits=%3")
            .arg(t).arg(l->rawBits).arg(r->rawBits);
        return false;
    }
    return true;
}

int run_window_verify(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: smoke_wvz4_raw_compare --window-verify <file.wvz4> <signals> <start> <end>\n";
        return 2;
    }

    const QString filePath = QString::fromLocal8Bit(argv[2]);
    const int requestedSignals = std::max(1, parse_int_arg(argv[3], 10));
    const qint64 start = parse_i64_arg(argv[4], 0);
    const qint64 end = parse_i64_arg(argv[5], start + 1);
    if (end <= start) {
        std::cerr << "end must be greater than start\n";
        return 2;
    }

    QString error;
    WaveFile full;
    WaveFile windowed;
    if (!load_first_n_raw(filePath, requestedSignals, 0, std::numeric_limits<qint64>::max(), full, error)) {
        std::cerr << "full load failed: " << error.toLocal8Bit().constData() << "\n";
        return 3;
    }
    if (!load_first_n_raw(filePath, requestedSignals, start, end, windowed, error)) {
        std::cerr << "window load failed: " << error.toLocal8Bit().constData() << "\n";
        return 4;
    }

    QHash<int, int> windowBySignalId;
    for (int i = 0; i < windowed.signalList.size(); ++i) {
        windowBySignalId.insert(windowed.signalList.at(i).signalId, i);
    }

    quint64 checkedValues = 0;
    int checkedSignals = 0;
    for (const WaveSignal& fullSignal : full.signalList) {
        if (!fullSignal.samplesLoaded) continue;
        const auto it = windowBySignalId.constFind(fullSignal.signalId);
        if (it == windowBySignalId.constEnd()) continue;
        const WaveSignal& windowSignal = windowed.signalList.at(*it);
        if (!windowSignal.samplesLoaded) continue;
        ++checkedSignals;

        QVector<qint64> probeTimes;
        probeTimes.push_back(start);
        probeTimes.push_back(start + (end - start) / 4);
        probeTimes.push_back(start + (end - start) / 2);
        probeTimes.push_back(end - 1);
        for (const WaveSample& sample : fullSignal.samples) {
            if (sample.time >= start && sample.time <= end) probeTimes.push_back(sample.time);
        }
        for (const WaveSample& sample : windowSignal.samples) {
            if (sample.time >= start && sample.time <= end) probeTimes.push_back(sample.time);
        }
        std::sort(probeTimes.begin(), probeTimes.end());
        probeTimes.erase(std::unique(probeTimes.begin(), probeTimes.end()), probeTimes.end());

        for (qint64 t : probeTimes) {
            QString reason;
            if (!sample_value_equal_at_time(fullSignal, windowSignal, t, reason)) {
                std::cerr << "window verify failed: signal_id=" << fullSignal.signalId
                          << " name=" << fullSignal.name.toLocal8Bit().constData()
                          << " " << reason.toLocal8Bit().constData() << "\n";
                return 5;
            }
            ++checkedValues;
        }
    }

    QTextStream out(stdout);
    out << "window_verify,ok\n";
    out << "file," << QFileInfo(filePath).fileName() << "\n";
    out << "signals," << checkedSignals << "\n";
    out << "checked_values," << checkedValues << "\n";
    out << "time_start," << start << "\n";
    out << "time_end," << end << "\n";
    return 0;
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
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--window-verify")) {
        return run_window_verify(argc, argv);
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
        const QString key = signal_key(right, i);
        if (rightByName.contains(key)) {
            std::cerr << "duplicate right signal key: " << key.toLocal8Bit().constData() << "\n";
            return 5;
        }
        rightByName.insert(key, i);
    }

    if (left.signalList.size() != right.signalList.size()) {
        std::cerr << "signal count differs: left=" << left.signalList.size()
                  << " right=" << right.signalList.size() << "\n";
        return 5;
    }

    quint64 checkedSamples = 0;
    for (int leftIndex = 0; leftIndex < left.signalList.size(); ++leftIndex) {
        const WaveSignal& leftSignal = left.signalList.at(leftIndex);
        const QString key = signal_key(left, leftIndex);
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
