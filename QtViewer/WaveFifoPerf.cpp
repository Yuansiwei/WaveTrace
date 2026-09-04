#include "WaveFifoPerfOutput.h"
#include "WaveFifoPressure.h"
#include "WaveParser4.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

struct Options {
    QString filePath;
    QString outputDirectory;
    qint64 startCycle = -1;
    qint64 endCycle = -1;
    qint64 ticksPerCycle = 10;
    quint64 maxSamples = 100000000;
    bool showProgress = true;
};

class TerminalProgress {
public:
    TerminalProgress(bool enabled, const char* label)
        : enabled_(enabled), label_(label) {}

    void update(quint64 completed, quint64 total) {
        if (!enabled_) return;
        const int percent = total == 0
                                ? 100
                                : int(qMin<quint64>(100, completed * 100 / total));
        if (percent == lastPercent_) return;
        lastPercent_ = percent;
        constexpr int width = 24;
        const int filled = percent * width / 100;
        QByteArray bar(width, '-');
        for (int i = 0; i < filled; ++i) bar[i] = '#';
        std::fprintf(stderr, "\r%s [%s] %3d%%", label_.constData(),
                     bar.constData(), percent);
        if (percent == 100) std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    void complete() {
        if (lastPercent_ != 100) update(1, 1);
    }

private:
    bool enabled_ = false;
    QByteArray label_;
    int lastPercent_ = -1;
};

bool checkedMultiply(qint64 left, qint64 right, qint64& result) {
    if (left < 0 || right <= 0 ||
        left > std::numeric_limits<qint64>::max() / right)
        return false;
    result = left * right;
    return true;
}

WaveSample sample(qint64 time, quint64 value) {
    WaveSample result;
    result.time = time;
    result.rawBits = value;
    result.rawFieldsReady = true;
    return result;
}

WaveSignal syntheticSignal(int id,
                           const QString& path,
                           std::initializer_list<WaveSample> samples) {
    WaveSignal signal;
    signal.signalId = id;
    signal.storageId = id;
    signal.name = path;
    signal.kind = SignalKind::Bus;
    signal.width = 32;
    signal.samples = QVector<WaveSample>(samples);
    return signal;
}

int runSelfTest() {
    WaveFile wave;
    wave.meta.start = 0;
    wave.meta.end = 1000;
    wave.signalList.push_back(syntheticSignal(
        1, QStringLiteral("alien.a.m_num_readable"),
        {sample(0, 4), sample(300, 1), sample(1000, 2)}));
    wave.signalList.push_back(syntheticSignal(
        2, QStringLiteral("alien.a.m_size"), {sample(0, 4)}));
    wave.signalList.push_back(syntheticSignal(
        3, QStringLiteral("other.b.m_numAvail"),
        {sample(0, 2), sample(200, 8), sample(700, 1), sample(1000, 0)}));
    wave.signalList.push_back(syntheticSignal(
        4, QStringLiteral("other.b.m_size"), {sample(0, 8)}));
    wave.signalList.push_back(syntheticSignal(
        5, QStringLiteral("third.c.m_count"),
        {sample(0, 0), sample(400, 2), sample(1000, 1)}));
    wave.signalList.push_back(syntheticSignal(
        6, QStringLiteral("third.c.m_size"), {sample(0, 2)}));
    wave.signalList.push_back(syntheticSignal(
        7, QStringLiteral("third.c.m_ri"), {sample(0, 0)}));
    wave.signalList.push_back(syntheticSignal(
        8, QStringLiteral("broken.d.m_num_readable"), {sample(0, 1)}));
    for (int i = 0; i < 4096; ++i) {
        wave.signalList.push_back(syntheticSignal(
            1000 + i, QStringLiteral("noise.deep.branch.payload_%1").arg(i),
            {sample(0, quint64(i))}));
    }

    quint64 finalProgress = 0;
    const wavefifo::DiscoveryResult discovery = wavefifo::discoverResources(
        wave, [&](quint64 completed, quint64) { finalProgress = completed; });
    if (discovery.resources.size() != 3 || discovery.rejected.size() != 1)
        return 10;
    if (discovery.signalsScanned != wave.signalList.size() ||
        discovery.fullPathsBuilt != 7 ||
        finalProgress != wave.signalList.size())
        return 22;
    const double expectedRates[] = {30.0, 50.0, 60.0};
    QHash<int, const WaveSignal*> byId;
    for (const WaveSignal& signal : wave.signalList)
        byId.insert(signal.signalId, &signal);
    for (int i = 0; i < discovery.resources.size(); ++i) {
        const wavefifo::ResourceDescriptor& resource =
            discovery.resources.at(i);
        const wavefifo::FullWindow full = wavefifo::analyzeFullWindow(
            byId.value(resource.occupancySignalId),
            byId.value(resource.capacitySignalId), 0, 1000);
        const double rate = full.knownTicks > 0
                                ? 100.0 * double(full.fullTicks) /
                                      double(full.knownTicks)
                                : -1.0;
        if (qAbs(rate - expectedRates[i]) > 0.0001) return 11 + i;
    }
    if (discovery.resources.at(0).resourceKind != wavefifo::ResourceKind::Fifo ||
        discovery.resources.at(1).resourceKind != wavefifo::ResourceKind::Fifo ||
        discovery.resources.at(2).resourceKind != wavefifo::ResourceKind::Queue)
        return 20;
    if (wavefifo::requiredSignalIds(discovery.resources).size() != 6) return 21;
    const WaveSignal changingOccupancy = syntheticSignal(
        9000, QStringLiteral("stream.occupancy"),
        {sample(0, 3), sample(30, 5)});
    const WaveSignal changingCapacity = syntheticSignal(
        9001, QStringLiteral("stream.capacity"),
        {sample(0, 4), sample(20, 2), sample(40, 6)});
    const wavefifo::FullWindow changingWindow = wavefifo::analyzeFullWindow(
        &changingOccupancy, &changingCapacity, 0, 50);
    if (changingWindow.fullTicks != 20 ||
        changingWindow.knownTicks != 40 ||
        changingWindow.expectedTicks != 40 ||
        changingWindow.occupancyWeightedTicks != 140.0L ||
        changingWindow.capacityWeightedTicks != 120.0L)
        return 23;
    const WaveSignal staticOccupancy = syntheticSignal(
        9002, QStringLiteral("static.occupancy"), {sample(10, 4)});
    const WaveSignal staticCapacity = syntheticSignal(
        9003, QStringLiteral("static.capacity"), {sample(10, 4)});
    const wavefifo::FullWindow staticWindow = wavefifo::analyzeFullWindow(
        &staticOccupancy, &staticCapacity, 0, 50);
    if (staticWindow.expectedTicks != 0 || staticWindow.knownTicks != 0 ||
        staticWindow.fullTicks != 0)
        return 24;
    QTextStream(stdout) << "self_test_ok\n";
    return 0;
}

bool parseOptions(QCoreApplication& app, Options& options, bool& selfTest) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("通用 WVZ4 FIFO 满率分析器"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("wave"),
                                 QStringLiteral("输入 WVZ4 文件"));
    parser.addOption(QCommandLineOption(
        {QStringLiteral("o"), QStringLiteral("out")},
        QStringLiteral("输出目录；默认 <波形名>.fifo.perf"),
        QStringLiteral("directory")));
    parser.addOption(QCommandLineOption(QStringLiteral("start-cycle"),
                                        QStringLiteral("起始业务周期"),
                                        QStringLiteral("cycle")));
    parser.addOption(QCommandLineOption(QStringLiteral("end-cycle"),
                                        QStringLiteral("结束业务周期（不包含）"),
                                        QStringLiteral("cycle")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("ticks-per-cycle"),
        QStringLiteral("每业务周期的 WVZ4 tick 数"), QStringLiteral("count"),
        QStringLiteral("10")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("max-samples"), QStringLiteral("最大解码采样数，0 为不限"),
        QStringLiteral("count"), QStringLiteral("100000000")));
    parser.addOption(QCommandLineOption(QStringLiteral("no-progress"),
                                        QStringLiteral("关闭进度输出")));
    parser.addOption(QCommandLineOption(QStringLiteral("self-test"),
                                        QStringLiteral("运行内置自测")));
    parser.process(app);
    selfTest = parser.isSet(QStringLiteral("self-test"));
    const QStringList positional = parser.positionalArguments();
    if (!selfTest && positional.isEmpty()) parser.showHelp(2);
    if (!positional.isEmpty()) options.filePath = positional.first();
    options.outputDirectory = parser.value(QStringLiteral("out"));
    options.showProgress = !parser.isSet(QStringLiteral("no-progress"));
    bool ok = false;
    options.ticksPerCycle =
        parser.value(QStringLiteral("ticks-per-cycle")).toLongLong(&ok);
    if (!ok || options.ticksPerCycle <= 0) return false;
    options.maxSamples =
        parser.value(QStringLiteral("max-samples")).toULongLong(&ok);
    if (!ok) return false;
    if (parser.isSet(QStringLiteral("start-cycle"))) {
        options.startCycle =
            parser.value(QStringLiteral("start-cycle")).toLongLong(&ok);
        if (!ok || options.startCycle < 0) return false;
    }
    if (parser.isSet(QStringLiteral("end-cycle"))) {
        options.endCycle =
            parser.value(QStringLiteral("end-cycle")).toLongLong(&ok);
        if (!ok || options.endCycle < 0) return false;
    }
    return true;
}

QJsonObject makeModel(const Options& options,
                      const WaveFile& directory,
                      const wavefifo::DiscoveryResult& discovery,
                      const QHash<int, const WaveSignal*>& loadedById,
                      qint64 startTick,
                      qint64 endTick) {
    QJsonObject model;
    model.insert(QStringLiteral("schema_version"), 4);
    model.insert(QStringLiteral("tool"), QStringLiteral("WaveFifoPerf"));
    model.insert(QStringLiteral("input_file"),
                 QFileInfo(options.filePath).absoluteFilePath());
    QJsonObject analysis;
    analysis.insert(QStringLiteral("start_tick"), double(startTick));
    analysis.insert(QStringLiteral("end_tick"), double(endTick));
    analysis.insert(QStringLiteral("start_cycle"),
                    double(startTick) / options.ticksPerCycle);
    analysis.insert(QStringLiteral("end_cycle"),
                    double(endTick) / options.ticksPerCycle);
    analysis.insert(QStringLiteral("ticks_per_cycle"),
                    double(options.ticksPerCycle));
    analysis.insert(QStringLiteral("wave_start_tick"),
                    double(directory.meta.start));
    analysis.insert(QStringLiteral("wave_end_tick"), double(directory.meta.end));
    model.insert(QStringLiteral("analysis"), analysis);

    QVector<QJsonObject> resourceItems;
    qint64 aggregateFull = 0;
    qint64 aggregateKnown = 0;
    int completeCount = 0;
    int partialCount = 0;
    for (const wavefifo::ResourceDescriptor& descriptor :
         discovery.resources) {
        const wavefifo::FullWindow full = wavefifo::analyzeFullWindow(
            loadedById.value(descriptor.occupancySignalId, nullptr),
            loadedById.value(descriptor.capacitySignalId, nullptr), startTick,
            endTick);
        QJsonObject item;
        item.insert(QStringLiteral("path"), descriptor.path);
        item.insert(QStringLiteral("resource_kind"),
                    wavefifo::resourceKindKey(descriptor.resourceKind));
        item.insert(QStringLiteral("resource_kind_label"),
                    wavefifo::resourceKindLabel(descriptor.resourceKind));
        item.insert(QStringLiteral("occupancy_kind"),
                    wavefifo::occupancyKindKey(descriptor.occupancyKind));
        item.insert(QStringLiteral("occupancy_kind_label"),
                    wavefifo::occupancyKindLabel(descriptor.occupancyKind));
        item.insert(QStringLiteral("representative_only"),
                    descriptor.representativeOnly);
        item.insert(QStringLiteral("full_ticks"), double(full.fullTicks));
        item.insert(QStringLiteral("known_ticks"), double(full.knownTicks));
        item.insert(QStringLiteral("expected_ticks"), double(full.expectedTicks));
        const double coverage = full.expectedTicks > 0
                                    ? 100.0 * double(full.knownTicks) /
                                          double(full.expectedTicks)
                                    : 0.0;
        item.insert(QStringLiteral("coverage_percent"), coverage);
        if (full.knownTicks > 0) {
            const double rate =
                100.0 * double(full.fullTicks) / double(full.knownTicks);
            item.insert(QStringLiteral("full_rate_percent"), rate);
            const double averageOccupancy =
                double(full.occupancyWeightedTicks / full.knownTicks);
            const double averageCapacity =
                double(full.capacityWeightedTicks / full.knownTicks);
            item.insert(QStringLiteral("average_occupancy"),
                        std::round(averageOccupancy * 100.0) / 100.0);
            item.insert(QStringLiteral("average_capacity"),
                        std::round(averageCapacity * 100.0) / 100.0);
            if (full.capacityWeightedTicks > 0.0L) {
                item.insert(
                    QStringLiteral("occupancy_rate_percent"),
                    100.0 * double(full.occupancyWeightedTicks /
                                   full.capacityWeightedTicks));
            }
            aggregateFull += full.fullTicks;
            aggregateKnown += full.knownTicks;
        }
        if (full.expectedTicks > 0 && full.knownTicks == full.expectedTicks) {
            item.insert(QStringLiteral("status"), QStringLiteral("完整"));
            ++completeCount;
        } else if (full.knownTicks > 0) {
            item.insert(QStringLiteral("status"), QStringLiteral("部分覆盖"));
            ++partialCount;
        } else {
            item.insert(QStringLiteral("status"), QStringLiteral("不可用"));
        }
        resourceItems.push_back(item);
    }
    std::sort(resourceItems.begin(), resourceItems.end(),
              [](const QJsonObject& left, const QJsonObject& right) {
                  const bool leftMeasured =
                      left.contains(QStringLiteral("full_rate_percent"));
                  const bool rightMeasured =
                      right.contains(QStringLiteral("full_rate_percent"));
                  if (leftMeasured != rightMeasured) return leftMeasured;
                  const double leftRate =
                      left.value(QStringLiteral("full_rate_percent")).toDouble();
                  const double rightRate =
                      right.value(QStringLiteral("full_rate_percent")).toDouble();
                  if (leftRate != rightRate) return leftRate > rightRate;
                  return left.value(QStringLiteral("path")).toString() <
                         right.value(QStringLiteral("path")).toString();
              });

    QJsonArray resources;
    QJsonArray fifos;
    QJsonArray queues;
    int fifoCount = 0;
    int queueCount = 0;
    for (const QJsonObject& item : resourceItems) {
        resources.push_back(item);
        if (item.value(QStringLiteral("resource_kind")).toString() ==
            QStringLiteral("queue")) {
            queues.push_back(item);
            ++queueCount;
        } else {
            fifos.push_back(item);
            ++fifoCount;
        }
    }
    model.insert(QStringLiteral("resources"), resources);
    model.insert(QStringLiteral("fifos"), fifos);
    model.insert(QStringLiteral("queues"), queues);

    QJsonArray rejected;
    for (const wavefifo::RejectedCandidate& candidate : discovery.rejected) {
        QJsonObject item;
        item.insert(QStringLiteral("path"), candidate.path);
        item.insert(QStringLiteral("reason"), candidate.reason);
        rejected.push_back(item);
    }
    model.insert(QStringLiteral("rejected_candidates"), rejected);
    QJsonArray warnings;
    for (const QString& warning : discovery.warnings) warnings.push_back(warning);
    model.insert(QStringLiteral("warnings"), warnings);
    QJsonObject summary;
    summary.insert(QStringLiteral("confirmed_fifo_count"),
                   fifoCount);
    summary.insert(QStringLiteral("confirmed_queue_count"), queueCount);
    summary.insert(QStringLiteral("confirmed_resource_count"),
                   discovery.resources.size());
    summary.insert(QStringLiteral("rejected_candidate_count"),
                   discovery.rejected.size());
    summary.insert(QStringLiteral("complete_count"), completeCount);
    summary.insert(QStringLiteral("partial_count"), partialCount);
    summary.insert(QStringLiteral("decoded_signal_count"), loadedById.size());
    summary.insert(QStringLiteral("directory_signals_scanned"),
                   double(discovery.signalsScanned));
    summary.insert(QStringLiteral("full_paths_built"),
                   double(discovery.fullPathsBuilt));
    if (aggregateKnown > 0)
        summary.insert(QStringLiteral("aggregate_full_rate_percent"),
                       100.0 * double(aggregateFull) / double(aggregateKnown));
    model.insert(QStringLiteral("summary"), summary);
    return model;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WaveFifoPerf"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));
    Options options;
    bool selfTest = false;
    if (!parseOptions(app, options, selfTest)) {
        QTextStream(stderr) << "error: invalid options\n";
        return 2;
    }
    if (selfTest) return runSelfTest();

    QString error;
    WaveParser4Reader reader;
    if (options.showProgress) {
        std::fprintf(stderr, "[1/4] Loading WVZ4 directory...\n");
        std::fflush(stderr);
    }
    if (!reader.open(options.filePath, error)) {
        QTextStream(stderr) << "error: " << error << '\n';
        return 3;
    }
    const WaveFile& directory = reader.directoryWave();
    qint64 startTick = directory.meta.start;
    qint64 endTick = directory.meta.end;
    if (options.startCycle >= 0 &&
        !checkedMultiply(options.startCycle, options.ticksPerCycle, startTick))
        return 4;
    if (options.endCycle >= 0 &&
        !checkedMultiply(options.endCycle, options.ticksPerCycle, endTick))
        return 4;
    startTick = qMax(startTick, directory.meta.start);
    endTick = qMin(endTick, directory.meta.end);
    if (endTick <= startTick) {
        QTextStream(stderr) << "error: empty analysis range\n";
        return 4;
    }

    TerminalProgress discoveryProgress(
        options.showProgress, "[2/4] Discovering FIFO/Queue");
    wavefifo::DiscoveryProgressCallback discoveryCallback;
    if (options.showProgress) {
        discoveryCallback = [&](quint64 completed, quint64 total) {
            discoveryProgress.update(completed, total);
        };
    }
    const wavefifo::DiscoveryResult discovery =
        wavefifo::discoverResources(directory, discoveryCallback);
    discoveryProgress.complete();
    const QVector<int> signalIds =
        wavefifo::requiredSignalIds(discovery.resources);
    WaveFile loaded;
    TerminalProgress decodeProgress(options.showProgress,
                                    "[3/4] Decoding selected signals");
    WaveParser4Reader::LoadProgressCallback decodeCallback;
    if (options.showProgress) {
        decodeCallback = [&](quint64 completed, quint64 total) {
            decodeProgress.update(completed, total);
        };
    }
    if (!signalIds.isEmpty() &&
        !reader.loadSignals(signalIds, loaded, error, options.maxSamples,
                            startTick, endTick, decodeCallback)) {
        QTextStream(stderr) << "error: " << error << '\n';
        return 5;
    }
    decodeProgress.complete();
    QHash<int, const WaveSignal*> loadedById;
    for (const WaveSignal& signal : loaded.signalList)
        loadedById.insert(signal.signalId, &signal);

    if (options.outputDirectory.isEmpty()) {
        const QFileInfo input(options.filePath);
        options.outputDirectory = input.dir().filePath(
            input.completeBaseName() + QStringLiteral(".fifo.perf"));
    }
    const QJsonObject model = makeModel(options, directory, discovery, loadedById,
                                        startTick, endTick);
    if (options.showProgress) {
        std::fprintf(stderr, "[4/4] Writing FIFO/Queue web report...\n");
        std::fflush(stderr);
    }
    if (!wavefifo::writeFifoPerformanceBundle(options.outputDirectory, model,
                                               error)) {
        QTextStream(stderr) << "error: " << error << '\n';
        return 6;
    }
    int fifoCount = 0;
    int queueCount = 0;
    for (const wavefifo::ResourceDescriptor& resource :
         discovery.resources) {
        if (resource.resourceKind == wavefifo::ResourceKind::Queue)
            ++queueCount;
        else
            ++fifoCount;
    }
    QTextStream(stdout) << "resource_count=" << discovery.resources.size()
                        << " fifo_count=" << fifoCount
                        << " queue_count=" << queueCount
                        << " decoded_signals=" << signalIds.size()
                        << " report="
                        << QFileInfo(options.outputDirectory +
                                     QStringLiteral("/index.html"))
                               .absoluteFilePath()
                        << '\n';
    return 0;
}
