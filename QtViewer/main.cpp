#include "MainWindow.h"

#include "WaveCanvas.h"
#include "WaveParser.h"
#include "WaveParser2.h"
#include "WaveParser3.h"
#include "WaveParser4.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QtGlobal>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static QIcon makeAppIconForApplication() {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#DCE3EA"));
    p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);

    p.setBrush(QColor("#30C56C"));
    p.drawRoundedRect(QRectF(10, 36, 18, 10), 3, 3);
    p.drawRoundedRect(QRectF(28, 20, 24, 10), 3, 3);

    p.setPen(QPen(QColor("#20354E"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(10, 41, 18, 41);
    p.drawLine(28, 25, 52, 25);
    p.drawLine(28, 25, 28, 41);
    p.drawLine(28, 41, 42, 41);
    p.drawLine(42, 41, 42, 14);
    p.drawLine(42, 14, 52, 14);

    return QIcon(pm);
}

static bool isUsableIcon(const QIcon& icon) {
    if (icon.isNull()) {
        return false;
    }
    if (!icon.availableSizes().isEmpty()) {
        return true;
    }
    return !icon.pixmap(32, 32).isNull();
}

#ifdef _WIN32
static std::wstring getModulePathNative() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0) {
        return L"";
    }
    buf.resize(n);
    return buf;
}

static std::wstring dirnameNative(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) {
        return L"";
    }
    return path.substr(0, p);
}

static std::wstring joinNative(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) {
        return name;
    }
    const wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') {
        return dir + name;
    }
    return dir + L"\\" + name;
}

static std::wstring getCurrentDirNative() {
    const DWORD need = GetCurrentDirectoryW(0, nullptr);
    if (need == 0) {
        return L"";
    }

    std::wstring buf(static_cast<size_t>(need) + 2, L'\0');
    const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(buf.size()), &buf[0]);
    if (n == 0) {
        return L"";
    }
    buf.resize(n);
    return buf;
}
#endif

static QIcon loadApplicationIcon() {
    QIcon icon(QStringLiteral(":/app.ico"));
    if (isUsableIcon(icon)) {
        return icon;
    }

#ifdef _WIN32
    const std::wstring exeDir = dirnameNative(getModulePathNative());

    const std::wstring exeIcon = joinNative(exeDir, L"app.ico");
    icon = QIcon(QString::fromWCharArray(exeIcon.c_str()));
    if (isUsableIcon(icon)) {
        return icon;
    }

    const std::wstring cwdIcon = joinNative(getCurrentDirNative(), L"app.ico");
    icon = QIcon(QString::fromWCharArray(cwdIcon.c_str()));
    if (isUsableIcon(icon)) {
        return icon;
    }
#else
    icon = QIcon(QStringLiteral("app.ico"));
    if (isUsableIcon(icon)) {
        return icon;
    }
#endif

    return makeAppIconForApplication();
}

static void processEventsFor(QApplication& app, int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

struct BenchmarkFindTarget {
    quint64 bits = 0;
    bool negativeDecimal = false;
};

static bool parseBenchmarkFindTargetText(const QString& text, BenchmarkFindTarget& target) {
    const QString raw = text.trimmed();
    if (raw.isEmpty()) return false;

    bool ok = false;
    if (raw.startsWith(QLatin1Char('-'))) {
        const QString body = raw.mid(1);
        if (!waveIsDecimalDigitsText(body)) return false;
        const qlonglong signedValue = raw.toLongLong(&ok, 10);
        if (!ok) return false;
        target.bits = static_cast<quint64>(signedValue);
        target.negativeDecimal = true;
        return true;
    }

    if (raw.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        const QString body = raw.mid(2);
        if (!waveIsHexDigitsText(body)) return false;
        const quint64 value = body.toULongLong(&ok, 16);
        if (!ok) return false;
        target.bits = value;
        return true;
    }

    if (raw.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) {
        const QString body = raw.mid(2);
        if (!waveIsBinaryDigitsText(body)) return false;
        const quint64 value = body.toULongLong(&ok, 2);
        if (!ok) return false;
        target.bits = value;
        return true;
    }

    if (waveIsDecimalDigitsText(raw)) {
        const quint64 value = raw.toULongLong(&ok, 10);
        if (!ok) return false;
        target.bits = value;
        return true;
    }

    if (waveIsHexDigitsText(raw)) {
        const quint64 value = raw.toULongLong(&ok, 16);
        if (!ok) return false;
        target.bits = value;
        return true;
    }

    return false;
}

static bool benchmarkFindTargetForSignal(const BenchmarkFindTarget& target, int width, quint64& maskedBits) {
    if (width <= 0) return false;
    const quint64 mask = waveBitMaskForWidth(width);
    if (!target.negativeDecimal && width < 64 && target.bits > mask) return false;
    maskedBits = target.bits & mask;
    return true;
}

static QString csvField(QString text) {
    text.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"") + text + QStringLiteral("\"");
}

static QString benchmarkDisplayTime(qint64 internalTime) {
    const double value = double(internalTime) / 10.0;
    QString text = QString::number(value, 'f', 1);
    if (text.endsWith(QStringLiteral(".0"))) text.chop(2);
    return text;
}

static QString benchmarkSignalName(const WaveFile& wave, int signalIndex) {
    if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return QString();
    if (!wave.tree.valid ||
        signalIndex >= wave.tree.signalIndexToNodeId.size()) {
        return wave.signalList.at(signalIndex).name;
    }

    int nodeId = wave.tree.signalIndexToNodeId.at(signalIndex);
    QStringList parts;
    while (nodeId >= 0 && nodeId < wave.tree.nodesById.size()) {
        const WaveTreeNode& node = wave.tree.nodesById.at(nodeId);
        if (!node.valid) break;
        if (!node.name.isEmpty()) parts.prepend(node.name);
        if (node.parentId == nodeId) break;
        nodeId = node.parentId;
    }
    return parts.isEmpty() ? wave.signalList.at(signalIndex).name : parts.join(QLatin1Char('.'));
}

static bool loadWaveForCaptureTool(const QString& wavePath,
                                   WaveFile& wave,
                                   QString& error,
                                   int autoLoadFirstSignalCount,
                                   quint64 maxDecodedSamples) {
    if (wavePath.endsWith(QStringLiteral(".wvz4"), Qt::CaseInsensitive)) {
        WaveParser4::LoadOptions options;
        options.includeAllSignalDefinitions = true;
        options.autoLoadFirstSignalCount = autoLoadFirstSignalCount;
        options.autoLoadFirstSignalLodCount = autoLoadFirstSignalCount;
        options.loadAllIfWindowEmpty = false;
        options.maxDecodedSamples = maxDecodedSamples;
        return WaveParser4::loadFromFile(wavePath, wave, error, options);
    }

    if (wavePath.endsWith(QStringLiteral(".wvz3"), Qt::CaseInsensitive)) {
        WaveParser3::LoadOptions options;
        options.includeAllSignalDefinitions = true;
        options.autoLoadFirstSignalCount = autoLoadFirstSignalCount;
        options.loadAllIfWindowEmpty = false;
        return WaveParser3::loadFromFile(wavePath, wave, error, options);
    }

    if (wavePath.endsWith(QStringLiteral(".wvz2"), Qt::CaseInsensitive)) {
        return WaveParser2::loadFromFile(wavePath, wave, error);
    }

    return WaveParser::loadFromFile(wavePath, wave, error);
}

static int runValueFindBenchmark(const QStringList& args) {
    if (args.size() < 5) {
        return 2;
    }

    const QString wavePath = args.at(2);
    const QString targetText = args.at(3);
    const QString outPath = args.at(4);
    const int requestedSignals = (args.size() >= 6) ? qMax(1, args.at(5).toInt()) : 64;
    const quint64 maxDecodedSamples = (args.size() >= 7)
        ? qMax<quint64>(0ull, args.at(6).toULongLong())
        : 200ull * 1000ull * 1000ull;

    BenchmarkFindTarget target;
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return 3;
    }
    QTextStream stream(&out);

    if (!parseBenchmarkFindTargetText(targetText, target)) {
        stream << "error," << csvField(QStringLiteral("invalid target")) << "\n";
        return 4;
    }

    QString error;
    WaveFile wave;
    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.autoLoadFirstSignalCount = requestedSignals;
    options.autoLoadFirstSignalLodCount = requestedSignals;
    options.loadAllIfWindowEmpty = false;
    options.maxDecodedSamples = maxDecodedSamples;

    QElapsedTimer loadTimer;
    loadTimer.start();
    if (!WaveParser4::loadFromFile(wavePath, wave, error, options)) {
        stream << "error," << csvField(error) << "\n";
        return 5;
    }
    const qint64 loadMs = loadTimer.elapsed();

    struct SignalResult {
        int signalIndex = -1;
        int width = 0;
        int sampleCount = 0;
        qint64 hitCount = 0;
        qint64 firstHitTime = -1;
        bool skippedByWidth = false;
    };

    QVector<SignalResult> results;
    results.reserve(qMin(requestedSignals, wave.signalList.size()));

    qint64 totalSamples = 0;
    qint64 totalHits = 0;
    qint64 matchedSignals = 0;
    qint64 widthSkippedSignals = 0;
    qint64 firstOverallTime = -1;
    int firstOverallSignal = -1;

    QElapsedTimer scanTimer;
    scanTimer.start();
    for (int signalIndex = 0; signalIndex < wave.signalList.size() && results.size() < requestedSignals; ++signalIndex) {
        const WaveSignal& sig = wave.signalList.at(signalIndex);
        SignalResult result;
        result.signalIndex = signalIndex;
        result.width = sig.width;
        result.sampleCount = sig.samples.size();
        totalSamples += result.sampleCount;

        quint64 targetBits = 0;
        if (!benchmarkFindTargetForSignal(target, sig.width, targetBits)) {
            result.skippedByWidth = true;
            ++widthSkippedSignals;
            results.push_back(result);
            continue;
        }

        const quint64 mask = waveBitMaskForWidth(sig.width);
        bool previousMatched = false;
        for (int sampleIndex = 0; sampleIndex < sig.samples.size(); ++sampleIndex) {
            const WaveSample& sample = sig.samples.at(sampleIndex);
            bool matched = false;
            if (!sample.isAbsent && !sample.isZ) {
                if (sample.rawFieldsReady) {
                    matched = ((sample.rawBits & mask) == targetBits);
                } else {
                    WaveSample hydrated = sample;
                    hydrateWaveSampleRawFields(sig.kind, sig.width, hydrated);
                    matched = !hydrated.isAbsent && !hydrated.isZ && ((hydrated.rawBits & mask) == targetBits);
                }
            }

            if (matched && !previousMatched) {
                ++result.hitCount;
                if (result.firstHitTime < 0) result.firstHitTime = sample.time;
                if (firstOverallTime < 0 || sample.time < firstOverallTime) {
                    firstOverallTime = sample.time;
                    firstOverallSignal = signalIndex;
                }
            }
            previousMatched = matched;
        }

        if (result.hitCount > 0) ++matchedSignals;
        totalHits += result.hitCount;
        results.push_back(result);
    }
    const qint64 scanMs = scanTimer.elapsed();

    stream << "metric,value\n";
    stream << "wave_file," << csvField(QFileInfo(wavePath).fileName()) << "\n";
    stream << "target," << csvField(targetText) << "\n";
    stream << "file_bytes," << QFileInfo(wavePath).size() << "\n";
    stream << "total_signals_in_file," << wave.signalList.size() << "\n";
    stream << "searched_signals," << results.size() << "\n";
    stream << "loaded_samples," << totalSamples << "\n";
    stream << "load_ms," << loadMs << "\n";
    stream << "scan_ms," << scanMs << "\n";
    stream << "total_ms," << (loadMs + scanMs) << "\n";
    stream << "total_target_segments," << totalHits << "\n";
    stream << "matched_signals," << matchedSignals << "\n";
    stream << "width_skipped_signals," << widthSkippedSignals << "\n";
    stream << "first_hit_signal," << csvField(firstOverallSignal >= 0 ? benchmarkSignalName(wave, firstOverallSignal) : QStringLiteral("-")) << "\n";
    stream << "first_hit_time," << (firstOverallTime >= 0 ? benchmarkDisplayTime(firstOverallTime) : QStringLiteral("-")) << "\n";
    stream << "\n";
    stream << "signal,width,samples,target_segments,first_time,skipped\n";
    for (const SignalResult& result : results) {
        stream << csvField(benchmarkSignalName(wave, result.signalIndex)) << ","
               << result.width << ","
               << result.sampleCount << ","
               << result.hitCount << ","
               << (result.firstHitTime >= 0 ? benchmarkDisplayTime(result.firstHitTime) : QStringLiteral("-")) << ","
               << (result.skippedByWidth ? QStringLiteral("width") : QString()) << "\n";
    }

    return 0;
}

static int runDumpSignalHead(const QStringList& args) {
    if (args.size() < 3) return 2;

    const QString wavePath = args.at(2);
    const int requestedSignals = (args.size() >= 4) ? qMax(1, args.at(3).toInt()) : 6;
    const int headCount = (args.size() >= 5) ? qMax(1, args.at(4).toInt()) : 12;

    WaveFile wave;
    QString error;
    if (!loadWaveForCaptureTool(wavePath, wave, error, requestedSignals, 20ull * 1000ull * 1000ull)) {
        QTextStream(stderr) << "error: " << error << "\n";
        return 3;
    }

    QTextStream out(stdout);
    out << "signals=" << wave.signalList.size()
        << " start=" << wave.meta.start
        << " end=" << wave.meta.end << "\n";
    for (int i = 0; i < wave.signalList.size() && i < requestedSignals; ++i) {
        const WaveSignal& sig = wave.signalList.at(i);
        out << "signal[" << i << "] name=" << sig.name
            << " kind=" << (sig.kind == SignalKind::Bit ? "bit" : "bus")
            << " width=" << sig.width
            << " samplesLoaded=" << (sig.samplesLoaded ? 1 : 0)
            << " samples=" << sig.samples.size()
            << " lodLevels=" << sig.lodLevels.size() << "\n";
        for (int s = 0; s < sig.samples.size() && s < headCount; ++s) {
            const WaveSample& sample = sig.samples.at(s);
            out << "  sample[" << s << "] t=" << sample.time
                << " raw=0x" << QString::number(sample.rawBits, 16)
                << " z=" << (sample.isZ ? 1 : 0)
                << " absent=" << (sample.isAbsent ? 1 : 0)
                << " text=" << sample.value << "\n";
        }
        for (int l = 0; l < sig.lodLevels.size(); ++l) {
            const WaveLodLevel& level = sig.lodLevels.at(l);
            if (level.samples.isEmpty() && level.buckets.isEmpty()) continue;
            out << "  lod[" << l << "] bucketCycles=" << level.bucketCycles
                << " samples=" << level.samples.size()
                << " buckets=" << level.buckets.size()
                << " validRanges=" << level.validRanges.size() << "\n";
            for (int r = 0; r < level.validRanges.size() && r < 4; ++r) {
                const WaveLodValidRange& range = level.validRanges.at(r);
                out << "    validRange[" << r << "] start=" << range.start
                    << " end=" << range.end << "\n";
            }
            for (int s = 0; s < level.samples.size() && s < qMin(headCount, 6); ++s) {
                const WaveSample& sample = level.samples.at(s);
                out << "    lodSample[" << s << "] t=" << sample.time
                    << " raw=0x" << QString::number(sample.rawBits, 16)
                    << " z=" << (sample.isZ ? 1 : 0)
                    << " absent=" << (sample.isAbsent ? 1 : 0) << "\n";
            }
        }
    }
    return 0;
}

static int runZoomCaptureSequence(QApplication& app, const QStringList& args) {
    if (args.size() < 4) {
        return 2;
    }

    const QString wavePath = args.at(2);
    const QString outDirPath = args.at(3);
    const int steps = (args.size() >= 5) ? qMax(0, args.at(4).toInt()) : 12;

    QDir outDir(outDirPath);
    if (!outDir.exists() && !QDir().mkpath(outDirPath)) {
        return 3;
    }

    WaveFile wave;
    QString error;
    if (!loadWaveForCaptureTool(wavePath, wave, error, 6, 20ull * 1000ull * 1000ull)) {
        QFile errorFile(outDir.filePath(QStringLiteral("zoom_sequence_error.txt")));
        if (errorFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&errorFile);
            stream << error << "\n";
        }
        return 4;
    }

    QVector<ActiveSignalRef> entries;
    for (int i = 0; i < wave.signalList.size() && entries.size() < 6; ++i) {
        const WaveSignal& sig = wave.signalList.at(i);
        if (sig.samples.isEmpty() && sig.lodLevels.isEmpty()) continue;
        ActiveSignalRef ref;
        ref.signalIndex = i;
        ref.format = sig.defaultRadix;
        entries.push_back(ref);
    }
    if (entries.isEmpty()) {
        return 5;
    }

    WaveCanvas canvas;
    canvas.resize(1600, 360);
    canvas.setWave(&wave);
    canvas.setVisibleEntries(entries);
    canvas.setVisibleEntryWindow(0, entries.size());
    canvas.show();
    processEventsFor(app, 80);

    QFile rangesFile(outDir.filePath(QStringLiteral("zoom_sequence_ranges.csv")));
    if (!rangesFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return 6;
    }
    QTextStream ranges(&rangesFile);
    ranges << "step,start,end,span,image\n";

    auto captureStep = [&](int step) {
        processEventsFor(app, 30);
        const QString imageName = QStringLiteral("zoom_step_%1.png").arg(step, 2, 10, QLatin1Char('0'));
        QPixmap pixmap(canvas.size());
        pixmap.fill(Qt::transparent);
        canvas.render(&pixmap);
        pixmap.save(outDir.filePath(imageName));
        ranges << step << "," << canvas.viewStart() << "," << canvas.viewEnd() << ","
               << (canvas.viewEnd() - canvas.viewStart()) << "," << imageName << "\n";
    };

    captureStep(0);
    for (int step = 1; step <= steps; ++step) {
        canvas.zoomByFactor(0.70);
        processEventsFor(app, 180);
        captureStep(step);
    }

    return 0;
}

int main(int argc, char *argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif

    QApplication a(argc, argv);
    const QStringList args = a.arguments();

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--capture-zoom-sequence")) {
        return runZoomCaptureSequence(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--value-find-benchmark")) {
        return runValueFindBenchmark(args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--dump-signal-head")) {
        return runDumpSignalHead(args);
    }

    const QIcon appIcon = loadApplicationIcon();
    a.setWindowIcon(appIcon);

    if (args.size() >= 3 && args.at(1) == QStringLiteral("--open-and-exit")) {
        const int settleMs = (args.size() >= 4) ? qMax(0, args.at(3).toInt()) : 200;
        MainWindow w;
        w.setWindowIcon(appIcon);
        w.show();
        const bool ok = w.openWaveFilePath(args.at(2), false);
        processEventsFor(a, settleMs);
        return ok ? 0 : 4;
    }

    MainWindow w;
    w.setWindowIcon(appIcon);
    w.show();

    if (args.size() >= 2) {
        w.openWaveFilePath(args.at(1));
    }

    return a.exec();
}
