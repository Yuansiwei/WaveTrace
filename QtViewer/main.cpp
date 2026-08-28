#include "MainWindow.h"

#include "WaveCanvas.h"
#include "WaveParser4.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QRectF>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QtGlobal>

#include <memory>

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
    return QString::number(internalTime);
}

static QString benchmarkSignalName(const WaveFile& wave, int signalIndex) {
    return waveSignalFullPath(wave, signalIndex);
}

static bool loadWaveForCaptureTool(const QString& wavePath,
                                   WaveFile& wave,
                                   QString& error,
                                   int autoLoadFirstSignalCount,
                                   quint64 maxDecodedSamples) {
    if (!wavePath.endsWith(QStringLiteral(".wvz4"), Qt::CaseInsensitive)) {
        error = QStringLiteral("Only WVZ4 wave files (*.wvz4) are supported.");
        return false;
    }

    WaveParser4::LoadOptions options;
    options.includeAllSignalDefinitions = true;
    options.autoLoadFirstSignalCount = autoLoadFirstSignalCount;
    options.autoLoadFirstSignalLodCount = autoLoadFirstSignalCount;
    options.loadAllIfWindowEmpty = false;
    options.maxDecodedSamples = maxDecodedSamples;
    return WaveParser4::loadFromFile(wavePath, wave, error, options);
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
    results.reserve(qMin(requestedSignals, static_cast<int>(wave.signalList.size())));

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
        out << "signal[" << i << "] name=" << waveSignalFullPath(wave, i)
            << " kind=" << (sig.kind == SignalKind::Bit ? "bit" : "bus")
            << " width=" << sig.width
            << " samplesLoaded=" << (sig.samplesLoaded ? 1 : 0)
            << " samples=" << sig.samples.size()
            << " lodLevels=" << sig.lodLevels.size()
            << " proceduralClock=" << (sig.proceduralClock ? 1 : 0);
        if (sig.proceduralClock) {
            out << " clockInitial=" << (sig.clockInitialValue ? 1 : 0)
                << " clockTogglePeriod=" << sig.clockTogglePeriodTicks;
        }
        out << "\n";
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
            for (int r = 0; r < level.loadedRanges.size() && r < 4; ++r) {
                const WaveLodValidRange& range = level.loadedRanges.at(r);
                out << "    loadedRange[" << r << "] start=" << range.start
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

static int runRenderBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 3) {
        return 2;
    }

    const QString wavePath = args.at(2);
    const int requestedSignals = (args.size() >= 4) ? qMax(1, args.at(3).toInt()) : 64;
    const int iterations = (args.size() >= 5) ? qMax(1, args.at(4).toInt()) : 20;
    const int width = (args.size() >= 6) ? qMax(320, args.at(5).toInt()) : 1600;
    const int rowHeight = 44;
    const int height = (args.size() >= 7)
        ? qMax(240, args.at(6).toInt())
        : qMax(360, 92 + requestedSignals * rowHeight);
    const quint64 maxDecodedSamples = (args.size() >= 8)
        ? qMax<quint64>(0ull, args.at(7).toULongLong())
        : 200ull * 1000ull * 1000ull;

    WaveFile wave;
    QString error;
    QElapsedTimer loadTimer;
    loadTimer.start();
    if (!loadWaveForCaptureTool(wavePath, wave, error, requestedSignals, maxDecodedSamples)) {
        QTextStream(stderr) << "error: " << error << "\n";
        return 3;
    }
    const qint64 loadMs = loadTimer.elapsed();

    QVector<ActiveSignalRef> entries;
    for (int i = 0; i < wave.signalList.size() && entries.size() < requestedSignals; ++i) {
        const WaveSignal& sig = wave.signalList.at(i);
        if (!sig.proceduralClock &&
            sig.samples.isEmpty() && sig.lodLevels.isEmpty()) continue;
        ActiveSignalRef ref;
        ref.signalIndex = i;
        ref.format = sig.defaultRadix;
        entries.push_back(ref);
    }
    if (entries.isEmpty()) {
        QTextStream(stderr) << "error: no drawable signals\n";
        return 4;
    }

    WaveCanvas canvas;
    canvas.resize(width, height);
    canvas.setWave(&wave);
    canvas.setVisibleEntries(entries);
    canvas.setVisibleEntryWindow(0, entries.size());
    canvas.show();
    processEventsFor(app, 80);

    QPixmap pixmap(canvas.size());
    pixmap.fill(Qt::transparent);
    canvas.render(&pixmap);
    processEventsFor(app, 30);

    qint64 minMs = std::numeric_limits<qint64>::max();
    qint64 maxMs = 0;
    qint64 totalMs = 0;
    for (int i = 0; i < iterations; ++i) {
        pixmap.fill(Qt::transparent);
        QElapsedTimer renderTimer;
        renderTimer.start();
        canvas.render(&pixmap);
        const qint64 renderMs = renderTimer.elapsed();
        totalMs += renderMs;
        minMs = qMin(minMs, renderMs);
        maxMs = qMax(maxMs, renderMs);
    }

    const bool lodDisabled = qEnvironmentVariableIsSet("WV_VIEWER_DISABLE_LOD") &&
        qgetenv("WV_VIEWER_DISABLE_LOD") != QByteArray("0");
    QTextStream out(stdout);
    out << "metric,value\n";
    out << "wave_file," << csvField(QFileInfo(wavePath).fileName()) << "\n";
    out << "lod_disabled," << (lodDisabled ? 1 : 0) << "\n";
    out << "signals_requested," << requestedSignals << "\n";
    out << "visible_signals," << entries.size() << "\n";
    out << "canvas_width," << width << "\n";
    out << "canvas_height," << height << "\n";
    out << "view_start," << canvas.viewStart() << "\n";
    out << "view_end," << canvas.viewEnd() << "\n";
    out << "view_span," << (canvas.viewEnd() - canvas.viewStart()) << "\n";
    out << "iterations," << iterations << "\n";
    out << "load_ms," << loadMs << "\n";
    out << "render_avg_ms," << (double(totalMs) / double(iterations)) << "\n";
    out << "render_min_ms," << minMs << "\n";
    out << "render_max_ms," << maxMs << "\n";
    return 0;
}

static int runProceduralClockRenderRegression(QApplication& app) {
    WaveFile wave;
    wave.meta.start = 0;
    wave.meta.end = 10'000'010;

    WaveSignal clock;
    clock.signalId = 1;
    clock.storageId = 1;
    clock.name = QStringLiteral("clk_over_two_million_edges");
    clock.kind = SignalKind::Bit;
    clock.width = 1;
    clock.defaultRadix = ValueRadix::Bin;
    clock.currentRadix = ValueRadix::Bin;
    clock.samplesLoaded = true;
    clock.proceduralClock = true;
    clock.clockInitialValue = false;
    clock.clockTogglePeriodTicks = 5;
    wave.signalList.push_back(clock);

    const WaveSignal& signal = wave.signalList.front();
    const qint64 millionthEdge =
        qint64(signal.clockTogglePeriodTicks * 1'000'001ull);
    if (waveProceduralClockTransitionAtOrAfter(signal, millionthEdge) !=
            millionthEdge ||
        waveProceduralClockPreviousTransition(signal, millionthEdge + 1) !=
            millionthEdge ||
        waveProceduralClockValueAtTime(signal, millionthEdge - 1) ==
            waveProceduralClockValueAtTime(signal, millionthEdge)) {
        QTextStream(stderr) << "error: procedural clock formula failed after one million edges\n";
        return 5;
    }

    ActiveSignalRef ref;
    ref.signalIndex = 0;
    ref.format = ValueRadix::Bin;
    QVector<ActiveSignalRef> entries;
    entries.push_back(ref);

    WaveCanvas canvas;
    canvas.resize(1600, 240);
    canvas.setWave(&wave);
    canvas.setVisibleEntries(entries);
    canvas.setVisibleEntryWindow(0, 1);
    canvas.show();
    processEventsFor(app, 20);

    QPixmap pixmap(canvas.size());
    pixmap.fill(Qt::transparent);
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 5; ++i) {
        pixmap.fill(Qt::transparent);
        canvas.render(&pixmap);
    }
    const qint64 renderMs = timer.elapsed();

    if (!wave.signalList.front().samples.isEmpty() ||
        !wave.signalList.front().lodLevels.isEmpty()) {
        QTextStream(stderr) << "error: procedural clock rendering materialized samples\n";
        return 6;
    }

    QTextStream out(stdout);
    out << "procedural_clock_render_ok"
        << " transitions=" << (wave.meta.end / qint64(signal.clockTogglePeriodTicks))
        << " samples=" << wave.signalList.front().samples.size()
        << " lod_levels=" << wave.signalList.front().lodLevels.size()
        << " renders=5"
        << " elapsed_ms=" << renderMs << "\n";
    return 0;
}

static bool lodCompareActivePixel(const QColor& c) {
    return c.green() >= 90 && c.green() >= c.red() + 24 && c.green() >= c.blue() + 24;
}

struct LodVisualCompareMetrics {
    int step = 0;
    qint64 viewStart = 0;
    qint64 viewEnd = 0;
    qint64 viewSpan = 0;
    qint64 exactDifferentPixels = 0;
    qint64 totalPixels = 0;
    qint64 activeRawPixels = 0;
    qint64 activeLodPixels = 0;
    qint64 activeBothPixels = 0;
    qint64 activeUnionPixels = 0;
    qint64 activeOnlyRawPixels = 0;
    qint64 activeOnlyLodPixels = 0;
    qint64 totalAbsRgbDelta = 0;
    double exactDiffRatio = 0.0;
    double activeJaccard = 1.0;
    double activeMissRatio = 0.0;
    double activeExtraRatio = 0.0;
    double avgAbsRgbDelta = 0.0;
};

static QImage renderCanvasImage(WaveCanvas& canvas) {
    QPixmap pixmap(canvas.size());
    pixmap.fill(Qt::transparent);
    canvas.render(&pixmap);
    return pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
}

static int runBitStateRenderRegression(QApplication& app, const QStringList& args) {
    if (waveFormatBinaryValue(0, 1) != QStringLiteral("0") ||
        waveFormatBinaryValue(1, 1) != QStringLiteral("1") ||
        waveFormatHexValue(0, 1) != QStringLiteral("0") ||
        waveFormatHexValue(1, 1) != QStringLiteral("1") ||
        waveFormatBinaryValue(2, 2) != QStringLiteral("0b10") ||
        waveFormatHexValue(2, 2) != QStringLiteral("0x2")) {
        QTextStream(stderr) << "single-bit value formatting regression\n";
        return 6;
    }

    auto makeSample = [](qint64 time, quint64 value) {
        WaveSample sample;
        sample.time = time;
        sample.rawBits = value;
        sample.rawFieldsReady = true;
        return sample;
    };
    auto makeWave = [&](bool lodOnly) {
        WaveFile wave;
        wave.meta.start = 0;
        wave.meta.end = 100000;
        WaveSignal signal;
        signal.name = QStringLiteral("initial_zero_then_one");
        signal.kind = SignalKind::Bit;
        signal.width = 1;
        signal.defaultRadix = ValueRadix::Bin;
        signal.currentRadix = ValueRadix::Bin;
        signal.samplesLoaded = !lodOnly;
        if (lodOnly) {
            WaveLodLevel level;
            level.bucketCycles = 1000;
            level.samples.push_back(makeSample(0, 0));
            level.samples.push_back(makeSample(1000, 1));
            level.validRanges.push_back(WaveLodValidRange{ 0, wave.meta.end });
            level.loadedRanges.push_back(WaveLodValidRange{ 0, wave.meta.end });
            signal.lodLevels.push_back(std::move(level));
        } else {
            signal.samples.push_back(makeSample(0, 0));
            signal.samples.push_back(makeSample(1000, 1));
        }
        wave.signalList.push_back(std::move(signal));
        return wave;
    };

    WaveFile rawWave = makeWave(false);
    WaveFile lodWave = makeWave(true);
    WaveFile fragmentedLodWave;
    fragmentedLodWave.meta.start = 0;
    fragmentedLodWave.meta.end = 200000;
    WaveSignal fragmentedSignal;
    fragmentedSignal.name = QStringLiteral("two_high_pulses_fragmented_legacy_valid_ranges");
    fragmentedSignal.kind = SignalKind::Bit;
    fragmentedSignal.width = 1;
    fragmentedSignal.defaultRadix = ValueRadix::Bin;
    fragmentedSignal.currentRadix = ValueRadix::Bin;
    fragmentedSignal.samplesLoaded = false;
    WaveLodLevel fragmentedLevel;
    fragmentedLevel.bucketCycles = 1000;
    fragmentedLevel.samples.push_back(makeSample(30000, 1));
    fragmentedLevel.samples.push_back(makeSample(50000, 0));
    fragmentedLevel.samples.push_back(makeSample(90000, 1));
    fragmentedLevel.samples.push_back(makeSample(110000, 0));
    fragmentedLevel.validRanges.push_back(WaveLodValidRange{ 30000, 91000 });
    fragmentedLevel.validRanges.push_back(WaveLodValidRange{ 110000, fragmentedLodWave.meta.end });
    fragmentedLevel.loadedRanges.push_back(WaveLodValidRange{ 0, fragmentedLodWave.meta.end });
    fragmentedSignal.lodLevels.push_back(std::move(fragmentedLevel));
    fragmentedLodWave.signalList.push_back(std::move(fragmentedSignal));
    WaveFile equalFinalLodWave;
    equalFinalLodWave.meta.start = 0;
    equalFinalLodWave.meta.end = 100000;
    WaveSignal equalFinalSignal;
    equalFinalSignal.name = QStringLiteral("equal_final_value_but_active_lod_windows");
    equalFinalSignal.kind = SignalKind::Bit;
    equalFinalSignal.width = 1;
    equalFinalSignal.defaultRadix = ValueRadix::Bin;
    equalFinalSignal.currentRadix = ValueRadix::Bin;
    equalFinalSignal.samplesLoaded = false;
    WaveLodLevel equalFinalLevel;
    equalFinalLevel.bucketCycles = 10000;
    // Each record is the last transition in an active LOD window.  Equal
    // final values must still produce one visible activity edge per record.
    equalFinalLevel.samples.push_back(makeSample(10000, 1));
    equalFinalLevel.samples.push_back(makeSample(20000, 1));
    equalFinalLevel.samples.push_back(makeSample(30000, 1));
    equalFinalLevel.loadedRanges.push_back(WaveLodValidRange{ 0, equalFinalLodWave.meta.end });
    equalFinalSignal.lodLevels.push_back(std::move(equalFinalLevel));
    equalFinalLodWave.signalList.push_back(std::move(equalFinalSignal));
    WaveFile rawDenseWave;
    rawDenseWave.meta.start = 0;
    rawDenseWave.meta.end = 100000;
    WaveSignal rawDenseSignal;
    rawDenseSignal.name = QStringLiteral("dense_initial_zero_then_one");
    rawDenseSignal.kind = SignalKind::Bit;
    rawDenseSignal.width = 1;
    rawDenseSignal.defaultRadix = ValueRadix::Bin;
    rawDenseSignal.currentRadix = ValueRadix::Bin;
    rawDenseSignal.samplesLoaded = true;
    rawDenseSignal.samples.reserve(10001);
    for (qint64 time = 0; time < 10000; ++time) {
        rawDenseSignal.samples.push_back(makeSample(time, static_cast<quint64>(time & 1)));
    }
    rawDenseSignal.samples.push_back(makeSample(10000, 1));
    rawDenseWave.signalList.push_back(std::move(rawDenseSignal));
    ActiveSignalRef entry;
    entry.signalIndex = 0;
    entry.format = ValueRadix::Bin;
    const QVector<ActiveSignalRef> entries{ entry };

    WaveCanvas rawCanvas;
    WaveCanvas lodCanvas;
    WaveCanvas fragmentedLodCanvas;
    WaveCanvas equalFinalLodCanvas;
    WaveCanvas rawDenseCanvas;
    rawCanvas.resize(1000, 150);
    lodCanvas.resize(1000, 150);
    fragmentedLodCanvas.resize(1000, 150);
    equalFinalLodCanvas.resize(1000, 150);
    rawDenseCanvas.resize(1000, 150);
    rawCanvas.setWave(&rawWave);
    lodCanvas.setWave(&lodWave);
    fragmentedLodCanvas.setWave(&fragmentedLodWave);
    equalFinalLodCanvas.setWave(&equalFinalLodWave);
    rawDenseCanvas.setWave(&rawDenseWave);
    rawCanvas.setVisibleEntries(entries);
    lodCanvas.setVisibleEntries(entries);
    fragmentedLodCanvas.setVisibleEntries(entries);
    equalFinalLodCanvas.setVisibleEntries(entries);
    rawDenseCanvas.setVisibleEntries(entries);
    rawCanvas.setVisibleEntryWindow(0, 1);
    lodCanvas.setVisibleEntryWindow(0, 1);
    fragmentedLodCanvas.setVisibleEntryWindow(0, 1);
    equalFinalLodCanvas.setVisibleEntryWindow(0, 1);
    rawDenseCanvas.setVisibleEntryWindow(0, 1);
    rawCanvas.show();
    lodCanvas.show();
    fragmentedLodCanvas.show();
    equalFinalLodCanvas.show();
    rawDenseCanvas.show();
    processEventsFor(app, 80);

    const QImage raw = renderCanvasImage(rawCanvas);
    const QImage lod = renderCanvasImage(lodCanvas);
    const QImage fragmentedLod = renderCanvasImage(fragmentedLodCanvas);
    const QImage equalFinalLod = renderCanvasImage(equalFinalLodCanvas);
    const QImage rawDense = renderCanvasImage(rawDenseCanvas);
    if (args.size() >= 3) {
        QDir out(args.at(2));
        if (!out.exists() && !QDir().mkpath(out.path())) return 3;
        raw.save(out.filePath(QStringLiteral("bit_raw.png")));
        lod.save(out.filePath(QStringLiteral("bit_lod.png")));
        fragmentedLod.save(out.filePath(QStringLiteral("bit_lod_fragmented_valid.png")));
        equalFinalLod.save(out.filePath(QStringLiteral("bit_lod_equal_final_activity.png")));
        rawDense.save(out.filePath(QStringLiteral("bit_raw_dense.png")));
    }

    auto isGreen = [](QRgb pixel) {
        const QColor color(pixel);
        return color.green() >= 90 && color.green() >= color.red() + 24 &&
            color.green() >= color.blue() + 24;
    };
    const int yHigh = 43;
    const int yLow = 71;
    int initialLow = 0;
    int steadyHigh = 0;
    int steadyLow = 0;
    int steadyInterior = 0;
    int denseSteadyHigh = 0;
    int denseSteadyLow = 0;
    int denseSteadyInterior = 0;
    int fragmentedSecondPulseHigh = 0;
    int fragmentedSecondPulseLow = 0;
    int fragmentedSecondPulseInterior = 0;
    int equalFinalActivityInterior = 0;
    for (int x = 11; x <= 17; ++x) {
        if (isGreen(lod.pixel(x, yLow))) ++initialLow;
    }
    for (int x = 40; x < lod.width() - 11; ++x) {
        if (isGreen(lod.pixel(x, yHigh))) ++steadyHigh;
        if (isGreen(lod.pixel(x, yLow))) ++steadyLow;
        for (int y = yHigh + 3; y <= yLow - 3; ++y) {
            if (isGreen(lod.pixel(x, y))) ++steadyInterior;
        }
    }
    for (int x = 160; x < rawDense.width() - 11; ++x) {
        if (isGreen(rawDense.pixel(x, yHigh))) ++denseSteadyHigh;
        if (isGreen(rawDense.pixel(x, yLow))) ++denseSteadyLow;
        for (int y = yHigh + 3; y <= yLow - 3; ++y) {
            if (isGreen(rawDense.pixel(x, y))) ++denseSteadyInterior;
        }
    }
    for (int x = 465; x <= 535; ++x) {
        if (isGreen(fragmentedLod.pixel(x, yHigh))) ++fragmentedSecondPulseHigh;
        if (isGreen(fragmentedLod.pixel(x, yLow))) ++fragmentedSecondPulseLow;
        for (int y = yHigh + 3; y <= yLow - 3; ++y) {
            if (isGreen(fragmentedLod.pixel(x, y))) ++fragmentedSecondPulseInterior;
        }
    }
    for (int x = 10; x < equalFinalLod.width() - 10; ++x) {
        for (int y = yHigh + 3; y <= yLow - 3; ++y) {
            if (isGreen(equalFinalLod.pixel(x, y))) ++equalFinalActivityInterior;
        }
    }

    QTextStream out(stdout);
    out << "initial_low_green_pixels," << initialLow << "\n";
    out << "steady_high_green_pixels," << steadyHigh << "\n";
    out << "steady_low_green_pixels," << steadyLow << "\n";
    out << "steady_interior_green_pixels," << steadyInterior << "\n";
    out << "dense_steady_high_green_pixels," << denseSteadyHigh << "\n";
    out << "dense_steady_low_green_pixels," << denseSteadyLow << "\n";
    out << "dense_steady_interior_green_pixels," << denseSteadyInterior << "\n";
    out << "fragmented_second_pulse_high_green_pixels," << fragmentedSecondPulseHigh << "\n";
    out << "fragmented_second_pulse_low_green_pixels," << fragmentedSecondPulseLow << "\n";
    out << "fragmented_second_pulse_interior_green_pixels," << fragmentedSecondPulseInterior << "\n";
    out << "equal_final_activity_interior_green_pixels," << equalFinalActivityInterior << "\n";
    const bool ok = initialLow >= 4 && steadyHigh >= 900 &&
        steadyLow == 0 && steadyInterior == 0 && denseSteadyHigh >= 800 &&
        denseSteadyLow == 0 && denseSteadyInterior == 0 &&
        fragmentedSecondPulseHigh >= 60 && fragmentedSecondPulseLow == 0 &&
        fragmentedSecondPulseInterior == 0 && equalFinalActivityInterior >= 55;
    out << "result," << (ok ? "pass" : "fail") << "\n";
    out.flush();
    return ok ? 0 : 6;
}

static int runLodDisappearanceStress(QApplication& app, const QStringList& args) {
    struct StressCase {
        QString name;
        SignalKind kind = SignalKind::Bus;
        int width = 32;
        qint64 fileEnd = 100000;
        qint64 viewStart = 0;
        qint64 viewEnd = 100000;
        qint64 bucketCycles = 1000;
        QVector<WaveSample> lodSamples;
        qint64 requiredVisibleStart = 0;
        qint64 requiredVisibleEnd = 0;
    };

    auto makeSample = [](qint64 time, quint64 value) {
        WaveSample sample;
        sample.time = time;
        sample.rawBits = value;
        sample.rawFieldsReady = true;
        return sample;
    };
    QVector<StressCase> cases;
    const QVector<int> widths{ 8, 16, 32, 64 };
    const QVector<qint64> spans{ 100000, 1000000, 1000000000ll };
    for (int width : widths) {
        for (qint64 span : spans) {
            StressCase beforeFirst;
            beforeFirst.name = QStringLiteral("bus_w%1_span%2_before_first_event").arg(width).arg(span);
            beforeFirst.width = width;
            beforeFirst.fileEnd = span;
            beforeFirst.viewEnd = span / 2;
            beforeFirst.bucketCycles = qMax<qint64>(10, span / 100);
            beforeFirst.lodSamples.push_back(makeSample(span * 3 / 4, 1));
            beforeFirst.requiredVisibleStart = 0;
            beforeFirst.requiredVisibleEnd = beforeFirst.viewEnd;
            cases.push_back(beforeFirst);

            StressCase initialZero;
            initialZero.name = QStringLiteral("bus_w%1_span%2_initial_zero_prefix").arg(width).arg(span);
            initialZero.width = width;
            initialZero.fileEnd = span;
            initialZero.viewEnd = span;
            initialZero.bucketCycles = qMax<qint64>(10, span / 100);
            initialZero.lodSamples.push_back(makeSample(span / 2, quint64(width + 1)));
            initialZero.requiredVisibleStart = 0;
            initialZero.requiredVisibleEnd = span / 2;
            cases.push_back(initialZero);

            StressCase afterEvent;
            afterEvent.name = QStringLiteral("bus_w%1_span%2_after_prior_event").arg(width).arg(span);
            afterEvent.width = width;
            afterEvent.fileEnd = span;
            afterEvent.viewStart = span / 2;
            afterEvent.viewEnd = span;
            afterEvent.bucketCycles = qMax<qint64>(10, span / 100);
            afterEvent.lodSamples.push_back(makeSample(span / 4, quint64(width + 3)));
            afterEvent.requiredVisibleStart = afterEvent.viewStart;
            afterEvent.requiredVisibleEnd = afterEvent.viewEnd;
            cases.push_back(afterEvent);

            StressCase fragmented;
            fragmented.name = QStringLiteral("bus_w%1_span%2_multi_event").arg(width).arg(span);
            fragmented.width = width;
            fragmented.fileEnd = span;
            fragmented.viewEnd = span;
            fragmented.bucketCycles = qMax<qint64>(10, span / 100);
            fragmented.lodSamples.push_back(makeSample(span / 5, 1));
            fragmented.lodSamples.push_back(makeSample(span * 2 / 5, 0));
            fragmented.lodSamples.push_back(makeSample(span * 4 / 5, quint64(width + 7)));
            fragmented.requiredVisibleStart = 0;
            fragmented.requiredVisibleEnd = span / 5;
            cases.push_back(fragmented);
        }
    }

    // Exercise the dense sample-LOD branch independently.  Its anchor logic is
    // intentionally separate from the sparse frame renderer.
    for (int width : widths) {
        StressCase dense;
        dense.name = QStringLiteral("bus_w%1_dense_lod_initial_zero").arg(width);
        dense.width = width;
        dense.fileEnd = 1000000;
        dense.viewEnd = dense.fileEnd;
        dense.bucketCycles = 1000;
        for (int i = 0; i < 420; ++i) {
            const qint64 time = 250000 + qint64(i) * 1500;
            dense.lodSamples.push_back(makeSample(time, quint64(i + 1)));
        }
        dense.requiredVisibleStart = 0;
        dense.requiredVisibleEnd = 240000;
        cases.push_back(std::move(dense));
    }

    // Bit signals are controls: the established implicit-zero path must remain
    // visible while the bus fix is stressed across the same boundary shapes.
    for (qint64 span : spans) {
        StressCase bit;
        bit.name = QStringLiteral("bit_span%1_before_first_event").arg(span);
        bit.kind = SignalKind::Bit;
        bit.width = 1;
        bit.fileEnd = span;
        bit.viewEnd = span / 2;
        bit.bucketCycles = qMax<qint64>(10, span / 100);
        bit.lodSamples.push_back(makeSample(span * 3 / 4, 1));
        bit.requiredVisibleStart = 0;
        bit.requiredVisibleEnd = bit.viewEnd;
        cases.push_back(bit);
    }

    QString outputPath;
    if (args.size() >= 3) {
        outputPath = args.at(2);
        if (!QDir(outputPath).exists() && !QDir().mkpath(outputPath)) return 3;
    }

    const int canvasWidth = 720;
    WaveCanvas canvas;
    canvas.resize(canvasWidth, 130);
    ActiveSignalRef entry;
    entry.signalIndex = 0;
    entry.format = ValueRadix::Hex;
    canvas.setVisibleEntries(QVector<ActiveSignalRef>{ entry });
    canvas.setVisibleEntryWindow(0, 1);
    canvas.show();
    processEventsFor(app, 40);

    auto isGreen = [](QRgb pixel) {
        const QColor color(pixel);
        return color.green() >= 90 && color.green() >= color.red() + 24 &&
            color.green() >= color.blue() + 24;
    };
    QStringList failures;
    qint64 renderedGreenPixels = 0;
    const QDir failureDir(outputPath);

    for (const StressCase& one : cases) {
        WaveFile wave;
        wave.meta.start = 0;
        wave.meta.end = one.fileEnd;
        WaveSignal signal;
        signal.name = one.name;
        signal.kind = one.kind;
        signal.width = one.width;
        signal.defaultRadix = one.kind == SignalKind::Bit ? ValueRadix::Bin : ValueRadix::Hex;
        signal.currentRadix = signal.defaultRadix;
        signal.samplesLoaded = false;
        WaveLodLevel level;
        level.bucketCycles = one.bucketCycles;
        level.samples = one.lodSamples;
        level.loadedRanges.push_back(WaveLodValidRange{ 0, one.fileEnd });
        signal.lodLevels.push_back(std::move(level));
        wave.signalList.push_back(std::move(signal));

        canvas.setWave(&wave);
        canvas.commitViewportRange(one.viewStart, one.viewEnd);
        const QImage image = renderCanvasImage(canvas);

        const int plotLeft = 10;
        const int plotWidth = canvasWidth - 20;
        auto timeToPixel = [&](qint64 time) {
            const long double fraction = static_cast<long double>(time - one.viewStart) /
                static_cast<long double>(qMax<qint64>(1, one.viewEnd - one.viewStart));
            return qBound(plotLeft, plotLeft + int(std::floor(fraction * plotWidth)),
                          plotLeft + plotWidth);
        };
        const int requiredX1 = timeToPixel(one.requiredVisibleStart);
        const int requiredX2 = timeToPixel(one.requiredVisibleEnd);
        int requiredGreen = 0;
        int totalGreen = 0;
        for (int y = 42; y <= 72; ++y) {
            for (int x = plotLeft; x < plotLeft + plotWidth; ++x) {
                if (!isGreen(image.pixel(x, y))) continue;
                ++totalGreen;
                if (x >= requiredX1 && x < requiredX2) ++requiredGreen;
            }
        }
        renderedGreenPixels += totalGreen;
        const int requiredWidth = qMax(1, requiredX2 - requiredX1);
        const int minimumRequiredGreen = one.kind == SignalKind::Bit
            ? qMax(4, requiredWidth / 2)
            : qMax(8, requiredWidth);
        const bool visible = totalGreen >= 8 && requiredGreen >= minimumRequiredGreen;
        if (!visible) {
            failures.push_back(QStringLiteral("%1,total=%2,required=%3,min=%4")
                                   .arg(one.name).arg(totalGreen).arg(requiredGreen)
                                   .arg(minimumRequiredGreen));
            if (!outputPath.isEmpty() && failures.size() <= 12) {
                image.save(failureDir.filePath(one.name + QStringLiteral(".png")));
            }
        }
    }

    QTextStream out(stdout);
    out << "lod_disappearance_stress,cases," << cases.size()
        << ",failures," << failures.size()
        << ",green_pixels," << renderedGreenPixels << "\n";
    for (const QString& failure : failures) out << "failure," << failure << "\n";
    out << "result," << (failures.isEmpty() ? "pass" : "fail") << "\n";
    out.flush();

    if (!outputPath.isEmpty()) {
        QFile report(failureDir.filePath(QStringLiteral("lod_disappearance_stress.csv")));
        if (report.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&report);
            stream << "cases," << cases.size() << "\nfailures," << failures.size() << "\n";
            for (const QString& failure : failures) stream << failure << "\n";
        }
    }
    return failures.isEmpty() ? 0 : 6;
}

static LodVisualCompareMetrics compareRenderedImages(const QImage& raw,
                                                     const QImage& lod,
                                                     int step,
                                                     qint64 viewStart,
                                                     qint64 viewEnd,
                                                     QImage* diffImage) {
    LodVisualCompareMetrics m;
    m.step = step;
    m.viewStart = viewStart;
    m.viewEnd = viewEnd;
    m.viewSpan = viewEnd - viewStart;

    const int w = qMin(raw.width(), lod.width());
    const int h = qMin(raw.height(), lod.height());
    const int plotPadX = 10;
    const bool ignorePlotBoundaryColumns = w > plotPadX * 2;
    if (diffImage) {
        *diffImage = QImage(w, h, QImage::Format_ARGB32);
        diffImage->fill(QColor(0, 0, 0, 255));
    }

    for (int y = 0; y < h; ++y) {
        const QRgb* rawLine = reinterpret_cast<const QRgb*>(raw.constScanLine(y));
        const QRgb* lodLine = reinterpret_cast<const QRgb*>(lod.constScanLine(y));
        QRgb* diffLine = diffImage ? reinterpret_cast<QRgb*>(diffImage->scanLine(y)) : nullptr;
        for (int x = 0; x < w; ++x) {
            const QColor rc(rawLine[x]);
            const QColor lc(lodLine[x]);
            const bool ignoredColumn = ignorePlotBoundaryColumns &&
                (x == plotPadX || x == (w - plotPadX));
            if (ignoredColumn) {
                if (diffLine) diffLine[x] = qRgb(qGray(rawLine[x]) / 6, qGray(rawLine[x]) / 6, qGray(rawLine[x]) / 6);
                continue;
            }

            ++m.totalPixels;
            if (rawLine[x] != lodLine[x]) {
                ++m.exactDifferentPixels;
                m.totalAbsRgbDelta += qAbs(rc.red() - lc.red()) +
                    qAbs(rc.green() - lc.green()) +
                    qAbs(rc.blue() - lc.blue());
            }

            const bool rawActive = lodCompareActivePixel(rc);
            const bool lodActive = lodCompareActivePixel(lc);
            if (rawActive) ++m.activeRawPixels;
            if (lodActive) ++m.activeLodPixels;
            if (rawActive && lodActive) ++m.activeBothPixels;
            if (rawActive || lodActive) ++m.activeUnionPixels;
            if (rawActive && !lodActive) ++m.activeOnlyRawPixels;
            if (!rawActive && lodActive) ++m.activeOnlyLodPixels;

            if (diffLine) {
                if (rawActive && !lodActive) {
                    diffLine[x] = qRgb(60, 140, 255);
                } else if (!rawActive && lodActive) {
                    diffLine[x] = qRgb(255, 80, 60);
                } else if (rawLine[x] != lodLine[x]) {
                    const int delta = qMin(255, qAbs(rc.red() - lc.red()) +
                                           qAbs(rc.green() - lc.green()) +
                                           qAbs(rc.blue() - lc.blue()));
                    diffLine[x] = qRgb(delta, delta, delta);
                } else {
                    const int gray = qGray(rawLine[x]) / 4;
                    diffLine[x] = qRgb(gray, gray, gray);
                }
            }
        }
    }

    if (m.totalPixels > 0) {
        m.exactDiffRatio = double(m.exactDifferentPixels) / double(m.totalPixels);
        m.avgAbsRgbDelta = double(m.totalAbsRgbDelta) / double(m.totalPixels);
    }
    if (m.activeUnionPixels > 0) {
        m.activeJaccard = double(m.activeBothPixels) / double(m.activeUnionPixels);
    }
    if (m.activeRawPixels > 0) {
        m.activeMissRatio = double(m.activeOnlyRawPixels) / double(m.activeRawPixels);
    }
    if (m.activeLodPixels > 0) {
        m.activeExtraRatio = double(m.activeOnlyLodPixels) / double(m.activeLodPixels);
    }
    return m;
}

static int runLodVisualCompare(QApplication& app, const QStringList& args) {
    if (args.size() < 4) {
        return 2;
    }

    const QString wavePath = args.at(2);
    const QString outDirPath = args.at(3);
    const int requestedSignals = (args.size() >= 5) ? qMax(1, args.at(4).toInt()) : 6;
    const int steps = (args.size() >= 6) ? qMax(0, args.at(5).toInt()) : 10;
    const int width = (args.size() >= 7) ? qMax(320, args.at(6).toInt()) : 1600;
    const int rowHeight = 44;
    const int height = (args.size() >= 8)
        ? qMax(240, args.at(7).toInt())
        : qMax(360, 92 + requestedSignals * rowHeight);
    const quint64 maxDecodedSamples = (args.size() >= 9)
        ? qMax<quint64>(0ull, args.at(8).toULongLong())
        : 300ull * 1000ull * 1000ull;

    QDir outDir(outDirPath);
    if (!outDir.exists() && !QDir().mkpath(outDirPath)) {
        return 3;
    }

    WaveFile loaded;
    QString error;
    QElapsedTimer loadTimer;
    loadTimer.start();
    if (!loadWaveForCaptureTool(wavePath, loaded, error, requestedSignals, maxDecodedSamples)) {
        QFile errorFile(outDir.filePath(QStringLiteral("lod_visual_compare_error.txt")));
        if (errorFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&errorFile);
            stream << error << "\n";
        }
        return 4;
    }
    const qint64 loadMs = loadTimer.elapsed();

    QVector<ActiveSignalRef> entries;
    for (int i = 0; i < loaded.signalList.size() && entries.size() < requestedSignals; ++i) {
        const WaveSignal& sig = loaded.signalList.at(i);
        if (!sig.proceduralClock &&
            sig.samples.isEmpty() && sig.lodLevels.isEmpty()) continue;
        ActiveSignalRef ref;
        ref.signalIndex = i;
        ref.format = sig.defaultRadix;
        entries.push_back(ref);
    }
    if (entries.isEmpty()) {
        QFile errorFile(outDir.filePath(QStringLiteral("lod_visual_compare_error.txt")));
        if (errorFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&errorFile);
            stream << "no drawable signals\n";
        }
        return 5;
    }

    WaveFile rawWave = loaded;
    for (WaveSignal& sig : rawWave.signalList) sig.lodLevels.clear();
    WaveFile lodWave = loaded;

    WaveCanvas rawCanvas;
    WaveCanvas lodCanvas;
    rawCanvas.resize(width, height);
    lodCanvas.resize(width, height);
    rawCanvas.setWave(&rawWave);
    lodCanvas.setWave(&lodWave);
    rawCanvas.setVisibleEntries(entries);
    lodCanvas.setVisibleEntries(entries);
    rawCanvas.setVisibleEntryWindow(0, entries.size());
    lodCanvas.setVisibleEntryWindow(0, entries.size());
    rawCanvas.show();
    lodCanvas.show();
    processEventsFor(app, 80);

    QFile csv(outDir.filePath(QStringLiteral("lod_visual_compare.csv")));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return 6;
    }
    QTextStream stream(&csv);
    stream << "metric,value\n";
    stream << "wave_file," << csvField(QFileInfo(wavePath).fileName()) << "\n";
    stream << "file_bytes," << QFileInfo(wavePath).size() << "\n";
    stream << "load_ms," << loadMs << "\n";
    stream << "visible_signals," << entries.size() << "\n";
    stream << "canvas_width," << width << "\n";
    stream << "canvas_height," << height << "\n";
    stream << "steps," << (steps + 1) << "\n\n";
    stream << "step,view_start,view_end,view_span,exact_diff_pixels,total_pixels,exact_diff_ratio,"
           << "active_raw,active_lod,active_both,active_union,active_jaccard,"
           << "active_only_raw,active_miss_ratio,active_only_lod,active_extra_ratio,avg_abs_rgb_delta,"
           << "raw_image,lod_image,diff_image\n";

    LodVisualCompareMetrics worst;
    bool haveWorst = false;
    QElapsedTimer renderTimer;
    renderTimer.start();
    for (int step = 0; step <= steps; ++step) {
        processEventsFor(app, 40);
        QImage rawImage = renderCanvasImage(rawCanvas);
        QImage lodImage = renderCanvasImage(lodCanvas);
        QImage diffImage;
        const qint64 viewStart = rawCanvas.viewStart();
        const qint64 viewEnd = rawCanvas.viewEnd();
        LodVisualCompareMetrics metrics = compareRenderedImages(rawImage, lodImage, step, viewStart, viewEnd, &diffImage);

        const QString rawName = QStringLiteral("raw_%1.png").arg(step, 2, 10, QLatin1Char('0'));
        const QString lodName = QStringLiteral("lod_%1.png").arg(step, 2, 10, QLatin1Char('0'));
        const QString diffName = QStringLiteral("diff_%1.png").arg(step, 2, 10, QLatin1Char('0'));
        rawImage.save(outDir.filePath(rawName));
        lodImage.save(outDir.filePath(lodName));
        diffImage.save(outDir.filePath(diffName));

        stream << metrics.step << ","
               << metrics.viewStart << ","
               << metrics.viewEnd << ","
               << metrics.viewSpan << ","
               << metrics.exactDifferentPixels << ","
               << metrics.totalPixels << ","
               << metrics.exactDiffRatio << ","
               << metrics.activeRawPixels << ","
               << metrics.activeLodPixels << ","
               << metrics.activeBothPixels << ","
               << metrics.activeUnionPixels << ","
               << metrics.activeJaccard << ","
               << metrics.activeOnlyRawPixels << ","
               << metrics.activeMissRatio << ","
               << metrics.activeOnlyLodPixels << ","
               << metrics.activeExtraRatio << ","
               << metrics.avgAbsRgbDelta << ","
               << csvField(rawName) << ","
               << csvField(lodName) << ","
               << csvField(diffName) << "\n";

        if (!haveWorst || metrics.activeJaccard < worst.activeJaccard) {
            worst = metrics;
            haveWorst = true;
        }

        if (step < steps) {
            rawCanvas.zoomByFactor(0.70);
            lodCanvas.zoomByFactor(0.70);
            processEventsFor(app, 120);
        }
    }

    stream << "\n";
    stream << "summary_metric,value\n";
    stream << "render_compare_ms," << renderTimer.elapsed() << "\n";
    if (haveWorst) {
        stream << "worst_step," << worst.step << "\n";
        stream << "worst_active_jaccard," << worst.activeJaccard << "\n";
        stream << "worst_active_miss_ratio," << worst.activeMissRatio << "\n";
        stream << "worst_active_extra_ratio," << worst.activeExtraRatio << "\n";
        stream << "worst_exact_diff_ratio," << worst.exactDiffRatio << "\n";
    }
    return 0;
}

static int runLodVisualCompareFiles(QApplication& app, const QStringList& args) {
    if (args.size() < 5) {
        return 2;
    }

    const QString rawPath = args.at(2);
    const QString lodPath = args.at(3);
    const QString outDirPath = args.at(4);
    const int requestedSignals = (args.size() >= 6) ? qMax(1, args.at(5).toInt()) : 6;
    const int steps = (args.size() >= 7) ? qMax(0, args.at(6).toInt()) : 10;
    const int width = (args.size() >= 8) ? qMax(320, args.at(7).toInt()) : 1600;
    const int rowHeight = 44;
    const int height = (args.size() >= 9)
        ? qMax(240, args.at(8).toInt())
        : qMax(360, 92 + requestedSignals * rowHeight);
    const quint64 maxDecodedSamples = (args.size() >= 10)
        ? qMax<quint64>(0ull, args.at(9).toULongLong())
        : 300ull * 1000ull * 1000ull;

    QDir outDir(outDirPath);
    if (!outDir.exists() && !QDir().mkpath(outDirPath)) {
        return 3;
    }

    WaveFile rawWave;
    WaveFile lodWave;
    QString rawError;
    QString lodError;
    QElapsedTimer rawLoadTimer;
    rawLoadTimer.start();
    if (!loadWaveForCaptureTool(rawPath, rawWave, rawError, requestedSignals, maxDecodedSamples)) {
        QFile errorFile(outDir.filePath(QStringLiteral("lod_visual_compare_files_error.txt")));
        if (errorFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&errorFile);
            stream << "raw load failed: " << rawError << "\n";
        }
        return 4;
    }
    const qint64 rawLoadMs = rawLoadTimer.elapsed();

    QElapsedTimer lodLoadTimer;
    lodLoadTimer.start();
    if (!loadWaveForCaptureTool(lodPath, lodWave, lodError, requestedSignals, maxDecodedSamples)) {
        QFile errorFile(outDir.filePath(QStringLiteral("lod_visual_compare_files_error.txt")));
        if (errorFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&errorFile);
            stream << "lod load failed: " << lodError << "\n";
        }
        return 5;
    }
    const qint64 lodLoadMs = lodLoadTimer.elapsed();

    QVector<ActiveSignalRef> rawEntries;
    QVector<ActiveSignalRef> lodEntries;
    QStringList matchedNames;
    for (int rawIndex = 0; rawIndex < rawWave.signalList.size() && rawEntries.size() < requestedSignals; ++rawIndex) {
        const WaveSignal& rawSig = rawWave.signalList.at(rawIndex);
        if (!rawSig.proceduralClock &&
            rawSig.samples.isEmpty() && rawSig.lodLevels.isEmpty()) continue;

        const QString rawName = benchmarkSignalName(rawWave, rawIndex);
        int lodIndex = -1;
        for (int i = 0; i < lodWave.signalList.size(); ++i) {
            const WaveSignal& candidate = lodWave.signalList.at(i);
            if (!candidate.proceduralClock &&
                candidate.samples.isEmpty() && candidate.lodLevels.isEmpty()) continue;
            if (benchmarkSignalName(lodWave, i) == rawName) {
                lodIndex = i;
                break;
            }
        }
        if (lodIndex < 0) continue;

        ActiveSignalRef rawRef;
        rawRef.signalIndex = rawIndex;
        rawRef.format = rawSig.defaultRadix;
        rawEntries.push_back(rawRef);

        ActiveSignalRef lodRef;
        lodRef.signalIndex = lodIndex;
        lodRef.format = lodWave.signalList.at(lodIndex).defaultRadix;
        lodEntries.push_back(lodRef);
        matchedNames.push_back(rawName);
    }

    if (rawEntries.isEmpty()) {
        QFile errorFile(outDir.filePath(QStringLiteral("lod_visual_compare_files_error.txt")));
        if (errorFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&errorFile);
            stream << "no matched drawable signals\n";
        }
        return 6;
    }

    WaveCanvas rawCanvas;
    WaveCanvas lodCanvas;
    rawCanvas.resize(width, height);
    lodCanvas.resize(width, height);
    rawCanvas.setWave(&rawWave);
    lodCanvas.setWave(&lodWave);
    rawCanvas.setVisibleEntries(rawEntries);
    lodCanvas.setVisibleEntries(lodEntries);
    rawCanvas.setVisibleEntryWindow(0, rawEntries.size());
    lodCanvas.setVisibleEntryWindow(0, lodEntries.size());
    rawCanvas.show();
    lodCanvas.show();
    processEventsFor(app, 80);

    QFile csv(outDir.filePath(QStringLiteral("lod_visual_compare_files.csv")));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return 7;
    }
    QTextStream stream(&csv);
    stream << "metric,value\n";
    stream << "raw_file," << csvField(QFileInfo(rawPath).fileName()) << "\n";
    stream << "lod_file," << csvField(QFileInfo(lodPath).fileName()) << "\n";
    stream << "raw_file_bytes," << QFileInfo(rawPath).size() << "\n";
    stream << "lod_file_bytes," << QFileInfo(lodPath).size() << "\n";
    stream << "raw_load_ms," << rawLoadMs << "\n";
    stream << "lod_load_ms," << lodLoadMs << "\n";
    stream << "matched_visible_signals," << rawEntries.size() << "\n";
    stream << "canvas_width," << width << "\n";
    stream << "canvas_height," << height << "\n";
    stream << "steps," << (steps + 1) << "\n\n";
    stream << "matched_signal,name\n";
    for (int i = 0; i < matchedNames.size(); ++i) {
        stream << i << "," << csvField(matchedNames.at(i)) << "\n";
    }
    stream << "\n";
    stream << "step,view_start,view_end,view_span,exact_diff_pixels,total_pixels,exact_diff_ratio,"
           << "active_raw,active_lod,active_both,active_union,active_jaccard,"
           << "active_only_raw,active_miss_ratio,active_only_lod,active_extra_ratio,avg_abs_rgb_delta,"
           << "raw_image,lod_image,diff_image\n";

    LodVisualCompareMetrics worst;
    bool haveWorst = false;
    QElapsedTimer renderTimer;
    renderTimer.start();
    for (int step = 0; step <= steps; ++step) {
        processEventsFor(app, 40);
        QImage rawImage = renderCanvasImage(rawCanvas);
        QImage lodImage = renderCanvasImage(lodCanvas);
        QImage diffImage;
        const qint64 viewStart = rawCanvas.viewStart();
        const qint64 viewEnd = rawCanvas.viewEnd();
        LodVisualCompareMetrics metrics = compareRenderedImages(rawImage, lodImage, step, viewStart, viewEnd, &diffImage);

        const QString rawName = QStringLiteral("raw_file_%1.png").arg(step, 2, 10, QLatin1Char('0'));
        const QString lodName = QStringLiteral("lod_file_%1.png").arg(step, 2, 10, QLatin1Char('0'));
        const QString diffName = QStringLiteral("diff_file_%1.png").arg(step, 2, 10, QLatin1Char('0'));
        rawImage.save(outDir.filePath(rawName));
        lodImage.save(outDir.filePath(lodName));
        diffImage.save(outDir.filePath(diffName));

        stream << metrics.step << ","
               << metrics.viewStart << ","
               << metrics.viewEnd << ","
               << metrics.viewSpan << ","
               << metrics.exactDifferentPixels << ","
               << metrics.totalPixels << ","
               << metrics.exactDiffRatio << ","
               << metrics.activeRawPixels << ","
               << metrics.activeLodPixels << ","
               << metrics.activeBothPixels << ","
               << metrics.activeUnionPixels << ","
               << metrics.activeJaccard << ","
               << metrics.activeOnlyRawPixels << ","
               << metrics.activeMissRatio << ","
               << metrics.activeOnlyLodPixels << ","
               << metrics.activeExtraRatio << ","
               << metrics.avgAbsRgbDelta << ","
               << csvField(rawName) << ","
               << csvField(lodName) << ","
               << csvField(diffName) << "\n";

        if (!haveWorst || metrics.activeJaccard < worst.activeJaccard) {
            worst = metrics;
            haveWorst = true;
        }

        if (step < steps) {
            rawCanvas.zoomByFactor(0.70);
            lodCanvas.zoomByFactor(0.70);
            processEventsFor(app, 120);
        }
    }

    stream << "\n";
    stream << "summary_metric,value\n";
    stream << "render_compare_ms," << renderTimer.elapsed() << "\n";
    if (haveWorst) {
        stream << "worst_step," << worst.step << "\n";
        stream << "worst_active_jaccard," << worst.activeJaccard << "\n";
        stream << "worst_active_miss_ratio," << worst.activeMissRatio << "\n";
        stream << "worst_active_extra_ratio," << worst.activeExtraRatio << "\n";
        stream << "worst_exact_diff_ratio," << worst.exactDiffRatio << "\n";
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
    const int firstSignal = (args.size() >= 6) ? qMax(0, args.at(5).toInt()) : 0;

    QDir outDir(outDirPath);
    if (!outDir.exists() && !QDir().mkpath(outDirPath)) {
        return 3;
    }

    WaveFile wave;
    QString error;
    if (!loadWaveForCaptureTool(wavePath, wave, error, firstSignal + 6, 20ull * 1000ull * 1000ull)) {
        QFile errorFile(outDir.filePath(QStringLiteral("zoom_sequence_error.txt")));
        if (errorFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&errorFile);
            stream << error << "\n";
        }
        return 4;
    }

    QVector<ActiveSignalRef> entries;
    for (int i = firstSignal; i < wave.signalList.size() && entries.size() < 6; ++i) {
        const WaveSignal& sig = wave.signalList.at(i);
        if (!sig.proceduralClock &&
            sig.samples.isEmpty() && sig.lodLevels.isEmpty()) continue;
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

static int runZoomBoundaryBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 3) return 2;
    const int zoomSteps = (args.size() >= 4) ? qBound(1, args.at(3).toInt(), 64) : 24;
    const int settleMs = (args.size() >= 5) ? qBound(20, args.at(4).toInt(), 2000) : 190;
    const int activeSignals = (args.size() >= 7) ? qBound(1, args.at(6).toInt(), 4096) : 128;

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    WaveCanvas* canvas = window.findChild<WaveCanvas*>();
    if (!canvas) return 4;
    processEventsFor(app, 250);

    const qint64 cursorAnchor =
        canvas->viewStart() +
        static_cast<qint64>(std::llround(
            double(canvas->viewEnd() - canvas->viewStart()) * 0.37));
    canvas->setCursorTime(cursorAnchor);
    const auto cursorPixel = [canvas, cursorAnchor]() {
        const double viewSpan = qMax<qint64>(
            1, canvas->viewEnd() - canvas->viewStart());
        return 10.0 +
               double(cursorAnchor - canvas->viewStart()) / viewSpan *
                   double(qMax(1, canvas->width() - 20));
    };
    const double cursorPixelBeforeZoom = cursorPixel();
    const QPoint wheelPosition(canvas->width() - 40, canvas->height() / 2);
    QWheelEvent wheelEvent(
        QPointF(wheelPosition),
        QPointF(canvas->mapToGlobal(wheelPosition)),
        QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
        Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &wheelEvent);
    processEventsFor(app, 180);
    const double cursorPixelDrift =
        qAbs(cursorPixel() - cursorPixelBeforeZoom);
    const double cursorPixelTolerance =
        1.0 + double(qMax(1, canvas->width() - 20)) /
                  (2.0 * double(qMax<qint64>(
                      1, canvas->viewEnd() - canvas->viewStart())));
    const bool cursorAnchoredZoom =
        cursorPixelDrift <= cursorPixelTolerance;
    canvas->resetView();
    processEventsFor(app, 20);

    auto countWavePixels = [canvas]() -> qint64 {
        QImage image(canvas->size(), QImage::Format_ARGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        canvas->render(&painter);
        painter.end();
        qint64 count = 0;
        for (int y = 38; y < qMin(image.height(), 300); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QColor color(row[x]);
                if (color.green() >= 110 &&
                    color.green() > color.red() + 20 &&
                    color.green() > color.blue() + 10) {
                    ++count;
                }
            }
        }
        return count;
    };

    // Exercise the user sequence too: signals are already active at the wide
    // view and then the viewport zooms into RAW territory.
    window.activateFirstSignalsForBenchmark(activeSignals);
    processEventsFor(app, 1500);

    QElapsedTimer heartbeatClock;
    heartbeatClock.start();
    qint64 lastHeartbeatNs = heartbeatClock.nsecsElapsed();
    qint64 maxEventLoopGapNs = 0;
    QTimer heartbeat;
    heartbeat.setTimerType(Qt::PreciseTimer);
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() {
        const qint64 now = heartbeatClock.nsecsElapsed();
        maxEventLoopGapNs = qMax(maxEventLoopGapNs, now - lastHeartbeatNs);
        lastHeartbeatNs = now;
    });
    heartbeat.start();

    QFile outputFile;
    std::unique_ptr<QTextStream> fileStream;
    QTextStream consoleStream(stdout);
    QTextStream* out = &consoleStream;
    if (args.size() >= 6) {
        outputFile.setFileName(args.at(5));
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) return 5;
        fileStream.reset(new QTextStream(&outputFile));
        out = fileStream.get();
    }
    *out << "phase,step,start,end,span,step_elapsed_ms,max_event_loop_gap_ms,wave_pixels,"
            "anim_min_wave_pixels,anim_max_wave_pixels,anim_max_adjacent_pixel_delta\n";
    int blankWaveSteps = 0;
    auto runStep = [&](const char* phase, int step, double factor) {
        const qint64 gapBefore = maxEventLoopGapNs;
        QElapsedTimer stepTimer;
        stepTimer.start();
        canvas->zoomByFactor(factor);
        qint64 animMinPixels = std::numeric_limits<qint64>::max();
        qint64 animMaxPixels = 0;
        qint64 animMaxAdjacentDelta = 0;
        qint64 previousPixels = -1;
        while (stepTimer.elapsed() < settleMs) {
            processEventsFor(app, qMin(16, qMax(1, settleMs - int(stepTimer.elapsed()))));
            const qint64 pixels = countWavePixels();
            animMinPixels = qMin(animMinPixels, pixels);
            animMaxPixels = qMax(animMaxPixels, pixels);
            if (previousPixels >= 0) {
                animMaxAdjacentDelta = qMax(animMaxAdjacentDelta,
                                             qAbs(pixels - previousPixels));
            }
            previousPixels = pixels;
        }
        qint64 wavePixels = previousPixels >= 0 ? previousPixels : countWavePixels();
        if (wavePixels == 0) {
            processEventsFor(app, 1500);
            wavePixels = countWavePixels();
        }
        if (wavePixels == 0) ++blankWaveSteps;
        const double maxGapMs = double(qMax(gapBefore, maxEventLoopGapNs)) / 1000000.0;
        *out << phase << ',' << step << ',' << canvas->viewStart() << ',' << canvas->viewEnd()
            << ',' << (canvas->viewEnd() - canvas->viewStart()) << ',' << stepTimer.elapsed()
            << ',' << QString::number(maxGapMs, 'f', 3) << ',' << wavePixels
            << ',' << (animMinPixels == std::numeric_limits<qint64>::max() ? wavePixels : animMinPixels)
            << ',' << animMaxPixels << ',' << animMaxAdjacentDelta << '\n';
        out->flush();
    };

    for (int i = 1; i <= zoomSteps; ++i) runStep("in", i, 0.5);
    // Reproduce the expensive real-world sequence: signals are expanded while
    // deeply zoomed in, so their full-range LOD is not already warm when zooming out.
    window.activateFirstSignalsForBenchmark(activeSignals);
    processEventsFor(app, 80);
    maxEventLoopGapNs = 0;
    lastHeartbeatNs = heartbeatClock.nsecsElapsed();
    for (int i = 1; i <= zoomSteps; ++i) runStep("out", i, 2.0);
    processEventsFor(app, 1500);
    heartbeat.stop();
    int coveredSignals = 0;
    int totalSignals = 0;
    const bool coverageOk = window.benchmarkActiveViewportCoverage(&coveredSignals, &totalSignals);
    QString rawValidationError;
    const bool rawCacheOk = window.benchmarkValidateRawCaches(&rawValidationError);
    *out << "summary,zoom_steps," << zoomSteps << ",settle_ms," << settleMs
        << ",zoom_out_max_event_loop_gap_ms,"
        << QString::number(double(maxEventLoopGapNs) / 1000000.0, 'f', 3)
        << ",covered_signals," << coveredSignals << ",total_signals," << totalSignals
        << ",blank_wave_steps," << blankWaveSteps
        << ",raw_cache_correct," << (rawCacheOk ? 1 : 0)
        << ",raw_cache_error," << csvField(rawValidationError)
        << ",cursor_anchor_pixel_drift,"
        << QString::number(cursorPixelDrift, 'f', 3)
        << ",cursor_anchor_pixel_tolerance,"
        << QString::number(cursorPixelTolerance, 'f', 3)
        << ",cursor_anchored_zoom," << (cursorAnchoredZoom ? 1 : 0)
        << '\n';
    return (coverageOk && rawCacheOk && blankWaveSteps == 0 &&
            cursorAnchoredZoom) ? 0 : 6;
}

static int runRangeSelectionBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 5) return 2;
    bool startOk = false;
    bool endOk = false;
    const qint64 targetStart = args.at(3).toLongLong(&startOk);
    const qint64 targetEnd = args.at(4).toLongLong(&endOk);
    if (!startOk || !endOk || targetEnd <= targetStart) return 2;
    const int activeSignals = args.size() >= 6 ? qBound(1, args.at(5).toInt(), 4096) : 6;

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    WaveCanvas* canvas = window.findChild<WaveCanvas*>();
    if (!canvas) return 4;
    window.activateFirstSignalsForBenchmark(activeSignals);
    processEventsFor(app, 1500);

    auto countWavePixels = [canvas]() -> qint64 {
        QImage image(canvas->size(), QImage::Format_ARGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        canvas->render(&painter);
        painter.end();
        qint64 count = 0;
        for (int y = 38; y < qMin(image.height(), 300); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QColor color(row[x]);
                if (color.green() >= 110 && color.green() > color.red() + 20 &&
                    color.green() > color.blue() + 10) ++count;
            }
        }
        return count;
    };

    const qint64 originalStart = canvas->viewStart();
    const qint64 originalEnd = canvas->viewEnd();
    int committedViewportSignals = 0;
    QObject::connect(canvas, &WaveCanvas::viewportChanged,
                     [&](qint64 start, qint64 end) {
        if (start != originalStart || end != originalEnd) ++committedViewportSignals;
    });

    QElapsedTimer timer;
    timer.start();
    window.selectViewportRangeForBenchmark(targetStart, targetEnd);
    bool committed = false;
    qint64 firstCommittedPixels = 0;
    qint64 commitMs = -1;
    while (timer.elapsed() < 5000) {
        processEventsFor(app, 5);
        if (canvas->viewStart() == targetStart && canvas->viewEnd() == targetEnd) {
            committed = true;
            commitMs = timer.elapsed();
            firstCommittedPixels = countWavePixels();
            break;
        }
        if (canvas->viewStart() != originalStart || canvas->viewEnd() != originalEnd) {
            break;
        }
    }
    processEventsFor(app, 150);
    const qint64 settledPixels = countWavePixels();
    int covered = 0;
    int total = 0;
    const bool coverageOk = window.benchmarkActiveViewportCoverage(&covered, &total);
    QTextStream out(stdout);
    out << "range_selection,committed," << (committed ? 1 : 0)
        << ",commit_ms," << commitMs
        << ",viewport_signals," << committedViewportSignals
        << ",first_pixels," << firstCommittedPixels
        << ",settled_pixels," << settledPixels
        << ",covered," << covered << ",total," << total << '\n';
    out.flush();
    return (committed && committedViewportSignals == 1 && firstCommittedPixels > 0 &&
            firstCommittedPixels == settledPixels && coverageOk) ? 0 : 6;
}

static int runGlobalReturnBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 5) return 2;
    bool startOk = false;
    bool endOk = false;
    const qint64 smallStart = args.at(3).toLongLong(&startOk);
    const qint64 smallEnd = args.at(4).toLongLong(&endOk);
    if (!startOk || !endOk || smallEnd <= smallStart) return 2;
    const int activeSignals = args.size() >= 6 ? qBound(1, args.at(5).toInt(), 4096) : 6;

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    WaveCanvas* canvas = window.findChild<WaveCanvas*>();
    if (!canvas) return 4;
    window.activateFirstSignalsForBenchmark(activeSignals);
    processEventsFor(app, 1500);

    auto countWavePixels = [canvas]() -> qint64 {
        QImage image(canvas->size(), QImage::Format_ARGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        canvas->render(&painter);
        painter.end();
        qint64 count = 0;
        for (int y = 38; y < qMin(image.height(), 300); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QColor color(row[x]);
                if (color.green() >= 110 && color.green() > color.red() + 20 &&
                    color.green() > color.blue() + 10) ++count;
            }
        }
        return count;
    };

    window.selectViewportRangeForBenchmark(smallStart, smallEnd);
    QElapsedTimer enterTimer;
    enterTimer.start();
    while (enterTimer.elapsed() < 5000 &&
           (canvas->viewStart() != smallStart || canvas->viewEnd() != smallEnd)) {
        processEventsFor(app, 5);
    }
    if (canvas->viewStart() != smallStart || canvas->viewEnd() != smallEnd) return 5;

    const qint64 fullStart = canvas->fullStartTime();
    const qint64 fullEnd = canvas->fullEndTime();
    int committedViewportSignals = 0;
    QObject::connect(canvas, &WaveCanvas::viewportChanged,
                     [&](qint64 start, qint64 end) {
        if (start != smallStart || end != smallEnd) ++committedViewportSignals;
    });

    QElapsedTimer timer;
    timer.start();
    window.resetViewForBenchmark();
    bool committed = false;
    bool intermediateViewport = false;
    qint64 firstCommittedPixels = 0;
    qint64 commitMs = -1;
    while (timer.elapsed() < 5000) {
        processEventsFor(app, 5);
        const qint64 start = canvas->viewStart();
        const qint64 end = canvas->viewEnd();
        if (start == fullStart && end == fullEnd) {
            committed = true;
            commitMs = timer.elapsed();
            firstCommittedPixels = countWavePixels();
            break;
        }
        if (start != smallStart || end != smallEnd) {
            intermediateViewport = true;
            break;
        }
    }
    processEventsFor(app, 150);
    const qint64 settledPixels = countWavePixels();
    int covered = 0;
    int total = 0;
    const bool coverageOk = window.benchmarkActiveViewportCoverage(&covered, &total);
    QTextStream out(stdout);
    out << "global_return,committed," << (committed ? 1 : 0)
        << ",commit_ms," << commitMs
        << ",viewport_signals," << committedViewportSignals
        << ",intermediate_viewport," << (intermediateViewport ? 1 : 0)
        << ",first_pixels," << firstCommittedPixels
        << ",settled_pixels," << settledPixels
        << ",covered," << covered << ",total," << total << '\n';
    out.flush();
    return (committed && !intermediateViewport && committedViewportSignals == 1 &&
            firstCommittedPixels > 0 && firstCommittedPixels == settledPixels && coverageOk) ? 0 : 6;
}

static int runAddSignalsBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 3) return 2;
    const int activeSignals = args.size() >= 4 ? qBound(1, args.at(3).toInt(), 65536) : 256;
    const int readyTimeoutMs = args.size() >= 5 ? qBound(100, args.at(4).toInt(), 60000) : 15000;

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    processEventsFor(app, 250);

    QElapsedTimer timer;
    timer.start();
    window.activateFirstSignalsForBenchmark(activeSignals);
    const qint64 synchronousMs = timer.elapsed();

    bool ready = false;
    int covered = 0;
    int total = 0;
    while (timer.elapsed() < readyTimeoutMs) {
        processEventsFor(app, 5);
        if (window.benchmarkActiveViewportCoverage(&covered, &total)) {
            ready = true;
            break;
        }
    }
    const qint64 readyMs = timer.elapsed();
    QTextStream out(stdout);
    out << "add_signals,requested," << activeSignals
        << ",active," << total
        << ",sync_ms," << synchronousMs
        << ",ready_ms," << readyMs
        << ",covered," << covered
        << ",ready," << (ready ? 1 : 0) << '\n';
    out.flush();
    return ready ? 0 : 6;
}

static int runActiveShortcutBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 3) return 2;
    const int activeSignals =
        args.size() >= 4 ? qBound(1, args.at(3).toInt(), 65536) : 16384;

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    processEventsFor(app, 100);
    window.activateFirstSignalsForBenchmark(activeSignals);
    processEventsFor(app, 50);

    QTreeWidget* activeList =
        window.findChild<QTreeWidget*>(QStringLiteral("activeSignalList"));
    if (!activeList) return 4;
    activeList->setFocus();

    QElapsedTimer timer;
    timer.start();
    QKeyEvent selectAllEvent(
        QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(activeList, &selectAllEvent);
    const qint64 selectAllMs = timer.restart();

    QKeyEvent copyEvent(
        QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(activeList, &copyEvent);
    const qint64 copyMs = timer.restart();

    const int selectedCount = activeList->selectionModel()
        ? activeList->selectionModel()->selectedRows(0).size()
        : 0;
    QKeyEvent deleteEvent(
        QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(activeList, &deleteEvent);
    const qint64 deleteMs = timer.elapsed();
    const int remaining = activeList->topLevelItemCount();

    QTextStream out(stdout);
    out << "active_shortcuts,active," << activeSignals
        << ",selected," << selectedCount
        << ",select_all_ms," << selectAllMs
        << ",copy_ms," << copyMs
        << ",delete_ms," << deleteMs
        << ",remaining," << remaining << '\n';
    out.flush();
    return selectedCount == activeSignals && remaining == 0 ? 0 : 6;
}

static int runTreeEventJumpBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 3) return 2;
    const bool firstEvent = args.size() < 4 ||
        args.at(3).compare(QStringLiteral("last"), Qt::CaseInsensitive) != 0;
    const bool legacy = args.size() >= 5 &&
        args.at(4).compare(QStringLiteral("legacy"), Qt::CaseInsensitive) == 0;
    if (legacy) qputenv("WV_VIEWER_LEGACY_TREE_EVENT_JUMP", QByteArray("1"));
    else qunsetenv("WV_VIEWER_LEGACY_TREE_EVENT_JUMP");

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    processEventsFor(app, 100);

    QTreeView* tree =
        window.findChild<QTreeView*>(QStringLiteral("signalTree"));
    WaveCanvas* canvas = window.findChild<WaveCanvas*>();
    if (!tree || !tree->model() || !tree->selectionModel() || !canvas) return 4;
    const QModelIndex root = tree->model()->index(0, 0);
    if (!root.isValid()) return 5;
    tree->selectionModel()->select(
        root, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    tree->setCurrentIndex(root);

    QElapsedTimer timer;
    timer.start();
    window.jumpSelectedTreeSignalToViewportEventForBenchmark(firstEvent);
    const qint64 elapsedMs = timer.elapsed();

    QTextStream out(stdout);
    out << "tree_event_jump,direction," << (firstEvent ? "first" : "last")
        << ",mode," << (legacy ? "legacy" : "optimized")
        << ",elapsed_ms," << elapsedMs
        << ",cursor," << canvas->cursorTime() << '\n';
    out.flush();
    return canvas->cursorTime() >= 0 ? 0 : 6;
}

static int runValueFindUiBenchmark(QApplication& app, const QStringList& args) {
    if (args.size() < 4) return 2;
    const int activeSignals =
        args.size() >= 5 ? qMax(1, args.at(4).toInt()) : 64;
    const qint64 rangeStart =
        args.size() >= 7 ? args.at(5).toLongLong() : 0;
    const qint64 rangeEnd =
        args.size() >= 7 ? args.at(6).toLongLong() : 0;

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    processEventsFor(app, 100);

    int hitCount = 0;
    quint64 checksum = 0;
    qint64 elapsedMs = 0;
    if (!window.runValueFindForBenchmark(args.at(3), activeSignals,
                                         &hitCount, &checksum, &elapsedMs,
                                         rangeStart, rangeEnd)) {
        return 4;
    }

    QTextStream out(stdout);
    out << "value_find_ui,active," << activeSignals
        << ",target," << args.at(3)
        << ",elapsed_ms," << elapsedMs
        << ",hits," << hitCount
        << ",checksum," << QString::number(checksum, 16);
    if (rangeEnd > rangeStart) {
        out << ",range_start," << rangeStart
            << ",range_end," << rangeEnd;
    }
    out << '\n';
    out.flush();
    return 0;
}

static int runSignalConditionSearchBenchmark(QApplication& app,
                                             const QStringList& args) {
    // file, name-or-dash, regex(0/1), change-min-or-dash,
    // change-max-or-dash, optional scope-depth=N, then zero or more
    // value/ratio pairs.
    if (args.size() < 7) return 2;
    int valuePairStart = 7;
    int scopeDepth = -1;
    if (args.size() > 7 &&
        args.at(7).startsWith(QStringLiteral("scope-depth="))) {
        bool depthOk = false;
        scopeDepth = args.at(7).mid(12).toInt(&depthOk);
        if (!depthOk || scopeDepth < 0) return 2;
        valuePairStart = 8;
    }
    if (((args.size() - valuePairStart) & 1) != 0) return 2;
    auto optionalText = [](const QString& value) {
        return value == QStringLiteral("-") ? QString() : value;
    };

    qputenv("WV_SIGNAL_SEARCH_BENCHMARK", QByteArray("1"));
    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    processEventsFor(app, 100);

    QTreeView* tree =
        window.findChild<QTreeView*>(QStringLiteral("signalTree"));
    if (!tree) return 4;
    if (scopeDepth >= 0 && tree->model() && tree->selectionModel()) {
        QModelIndex scopedIndex = tree->model()->index(0, 0);
        for (int depth = 0;
             scopedIndex.isValid() && depth < scopeDepth; ++depth) {
            scopedIndex = tree->model()->index(0, 0, scopedIndex);
        }
        if (!scopedIndex.isValid()) return 4;
        tree->selectionModel()->select(
            scopedIndex,
            QItemSelectionModel::ClearAndSelect |
            QItemSelectionModel::Rows);
        tree->setCurrentIndex(scopedIndex);
    }
    window.openSignalConditionSearchDialogForBenchmark();
    processEventsFor(app, 30);

    QDialog* dialog =
        window.findChild<QDialog*>(QStringLiteral("signalConditionSearchDialog"));
    QLineEdit* nameEdit =
        window.findChild<QLineEdit*>(QStringLiteral("signalConditionName"));
    QCheckBox* scopeCheck =
        window.findChild<QCheckBox*>(QStringLiteral("signalConditionScope"));
    QCheckBox* cropTreeCheck =
        window.findChild<QCheckBox*>(QStringLiteral("signalConditionCropTree"));
    QCheckBox* regexCheck =
        window.findChild<QCheckBox*>(QStringLiteral("signalConditionRegex"));
    QCheckBox* caseCheck =
        window.findChild<QCheckBox*>(QStringLiteral("signalConditionCase"));
    QLineEdit* changeMin =
        window.findChild<QLineEdit*>(QStringLiteral("signalConditionChangeMin"));
    QLineEdit* changeMax =
        window.findChild<QLineEdit*>(QStringLiteral("signalConditionChangeMax"));
    QPushButton* addValue =
        window.findChild<QPushButton*>(QStringLiteral("signalConditionAddValue"));
    QPushButton* search =
        window.findChild<QPushButton*>(QStringLiteral("signalConditionSearch"));
    QPushButton* cancel =
        window.findChild<QPushButton*>(QStringLiteral("signalConditionCancel"));
    QPushButton* previous =
        window.findChild<QPushButton*>(QStringLiteral("signalConditionPrevious"));
    QPushButton* next =
        window.findChild<QPushButton*>(QStringLiteral("signalConditionNext"));
    QLabel* status =
        window.findChild<QLabel*>(QStringLiteral("signalConditionStatus"));
    if (!dialog || !nameEdit || !scopeCheck || !cropTreeCheck ||
        !regexCheck || !caseCheck ||
        !changeMin || !changeMax || !addValue || !search || !cancel ||
        !previous || !next || !status) {
        return 5;
    }

    nameEdit->setText(optionalText(args.at(3)));
    scopeCheck->setChecked(scopeDepth >= 0);
    cropTreeCheck->setChecked(true);
    regexCheck->setChecked(args.at(4).toInt() != 0);
    caseCheck->setChecked(false);
    changeMin->setText(optionalText(args.at(5)));
    changeMax->setText(optionalText(args.at(6)));

    const int requestedValueRows =
        (args.size() - valuePairStart) / 2;
    QList<QLineEdit*> valueEdits =
        window.findChildren<QLineEdit*>(QStringLiteral("signalConditionValue"));
    while (valueEdits.size() < qMax(1, requestedValueRows)) {
        addValue->click();
        valueEdits =
            window.findChildren<QLineEdit*>(QStringLiteral("signalConditionValue"));
    }
    QList<QLineEdit*> ratioEdits =
        window.findChildren<QLineEdit*>(QStringLiteral("signalConditionRatio"));
    for (int i = 0; i < valueEdits.size(); ++i) {
        valueEdits.at(i)->clear();
        if (i < ratioEdits.size()) ratioEdits.at(i)->clear();
    }
    for (int i = 0; i < requestedValueRows; ++i) {
        valueEdits.at(i)->setText(
            optionalText(args.at(valuePairStart + i * 2)));
        ratioEdits.at(i)->setText(
            optionalText(args.at(valuePairStart + 1 + i * 2)));
    }

    QElapsedTimer wallTimer;
    wallTimer.start();
    changeMax->setFocus(Qt::OtherFocusReason);
    QMetaObject::invokeMethod(changeMax, "returnPressed", Qt::DirectConnection);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    const bool enterStartedSearch =
        !search->isEnabled() ||
        status->text().startsWith(QStringLiteral("完成："));
    while (!search->isEnabled() && wallTimer.elapsed() < 120000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    processEventsFor(app, 20);
    if (!search->isEnabled()) {
        cancel->click();
        processEventsFor(app, 100);
        return 6;
    }

    const QString statusText = status->text();
    const QRegularExpression matchCountPattern(
        QStringLiteral("匹配\\s+(\\d+)\\s+个"));
    const QRegularExpressionMatch countMatch =
        matchCountPattern.match(statusText);
    const int matchedSignals =
        countMatch.hasMatch() ? countMatch.captured(1).toInt() : -1;
    const QModelIndex firstMatch = tree->currentIndex();
    const int selectedTargets = tree->selectionModel()
        ? tree->selectionModel()->selectedRows().size()
        : 0;
    const bool selectedFirst64 =
        selectedTargets == qMin(64, qMax(0, matchedSignals));
    bool advancedNavigationChangedTarget = matchedSignals <= 1;
    if (matchedSignals > 1 && previous->isEnabled() && next->isEnabled()) {
        next->click();
        processEventsFor(app, 20);
        advancedNavigationChangedTarget =
            tree->currentIndex().isValid() && tree->currentIndex() != firstMatch;
    }

    const QAbstractItemModel* model = tree->model();
    struct PendingIndex {
        QModelIndex parent;
        int depth = 0;
    };
    auto collectVisibleTreeStats = [model](qint64& visibleNodes,
                                           int& maxDepth) {
        QVector<PendingIndex> pending;
        pending.push_back(PendingIndex{QModelIndex(), 0});
        visibleNodes = 0;
        maxDepth = 0;
        while (!pending.isEmpty() && visibleNodes < 2000000) {
            const PendingIndex current = pending.takeLast();
            const int rows = model ? model->rowCount(current.parent) : 0;
            for (int row = 0; row < rows; ++row) {
                const QModelIndex child = model->index(row, 0, current.parent);
                if (!child.isValid()) continue;
                ++visibleNodes;
                maxDepth = qMax(maxDepth, current.depth + 1);
                if (model->hasChildren(child)) {
                    pending.push_back(PendingIndex{child, current.depth + 1});
                }
            }
        }
    };
    qint64 visibleNodes = 0;
    int maxDepth = 0;
    collectVisibleTreeStats(visibleNodes, maxDepth);
    const int topLevelRows = model ? model->rowCount() : 0;
    const qint64 searchWallMs = wallTimer.elapsed();

    const int stabilityWaitMs =
        qMax(0, qEnvironmentVariableIntValue(
                    "WV_SIGNAL_SEARCH_STABILITY_MS"));
    if (stabilityWaitMs > 0) processEventsFor(app, stabilityWaitMs);
    qint64 postWarmupVisibleNodes = 0;
    int postWarmupMaxDepth = 0;
    collectVisibleTreeStats(postWarmupVisibleNodes, postWarmupMaxDepth);
    const int postWarmupTopLevelRows = model ? model->rowCount() : 0;
    const bool emptySearchStayedFiltered =
        matchedSignals != 0 ||
        (topLevelRows == 0 && postWarmupTopLevelRows == 0 &&
         visibleNodes == 0 && postWarmupVisibleNodes == 0);

    QTextStream out(stdout);
    out << "signal_condition_search,wall_ms," << searchWallMs
        << ",matched," << matchedSignals
        << ",top_level_rows," << topLevelRows
        << ",visible_tree_nodes," << visibleNodes
        << ",max_tree_depth," << maxDepth
        << ",stability_wait_ms," << stabilityWaitMs
        << ",post_warmup_top_level_rows," << postWarmupTopLevelRows
        << ",post_warmup_visible_tree_nodes," << postWarmupVisibleNodes
        << ",post_warmup_max_tree_depth," << postWarmupMaxDepth
        << ",empty_search_stayed_filtered,"
        << (emptySearchStayedFiltered ? 1 : 0)
        << ",enter_started_search," << (enterStartedSearch ? 1 : 0)
        << ",selected_targets," << selectedTargets
        << ",selected_first_64," << (selectedFirst64 ? 1 : 0)
        << ",advanced_navigation_changed_target,"
        << (advancedNavigationChangedTarget ? 1 : 0)
        << ",status," << csvField(statusText) << '\n';
    out.flush();
    return (statusText.startsWith(QStringLiteral("完成：")) &&
            emptySearchStayedFiltered && enterStartedSearch &&
            selectedFirst64 && advancedNavigationChangedTarget)
        ? 0
        : 7;
}

static int runDerivedExpressionBenchmark(QApplication& app,
                                         const QStringList& args) {
    if (args.size() < 4) return 2;
    bool widthOk = true;
    const int width = args.size() >= 5 ? args.at(4).toInt(&widthOk) : 0;
    if (!widthOk) return 2;

    MainWindow window;
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    processEventsFor(app, 20);

    QString expression = args.at(3);
    if (expression == QStringLiteral("@first-two") ||
        expression == QStringLiteral("@last-two")) {
        int errorCode = 0;
        QString errorMessage;
        QJsonArray signalResults;
        if (expression == QStringLiteral("@first-two")) {
            const QJsonValue result = window.handleAgentRpc(
                QStringLiteral("signals.search"),
                QJsonObject{{QStringLiteral("query"), QString()},
                            {QStringLiteral("limit"), 2}},
                &errorCode, &errorMessage);
            signalResults = result.toObject()
                                .value(QStringLiteral("signals"))
                                .toArray();
        } else {
            const QJsonObject state = window.handleAgentRpc(
                QStringLiteral("viewer.state"), QJsonObject(),
                &errorCode, &errorMessage).toObject();
            const int signalCount = int(state.value(
                QStringLiteral("signal_count")).toDouble());
            for (int index = qMax(0, signalCount - 2);
                 index < signalCount; ++index) {
                signalResults.append(window.handleAgentRpc(
                    QStringLiteral("signals.describe"),
                    QJsonObject{{QStringLiteral("index"), index}},
                    &errorCode, &errorMessage));
            }
        }
        if (signalResults.size() < 2) return 3;
        const QString left = signalResults.at(0).toObject()
                                 .value(QStringLiteral("path"))
                                 .toString();
        const QString right = signalResults.at(1).toObject()
                                  .value(QStringLiteral("path"))
                                  .toString();
        if (left.isEmpty() || right.isEmpty()) return 3;
        expression = QStringLiteral("`%1` ^ `%2`").arg(left, right);
    }

    qint64 elapsedMs = -1;
    qint64 sampleCount = 0;
    quint64 checksum = 0;
    const bool ok = window.runDerivedExpressionForBenchmark(
        expression, width, &elapsedMs, &sampleCount, &checksum);
    QTextStream out(stdout);
    out << "status," << (ok ? "ok" : "failed")
        << ",elapsed_ms," << elapsedMs
        << ",samples," << sampleCount
        << ",checksum,0x" << QString::number(checksum, 16)
        << ",expression," << expression << '\n';
    out.flush();
    return ok ? 0 : 4;
}

static int runTreeReferenceSearchBenchmark(QApplication& app,
                                           const QStringList& args) {
    if (args.size() < 4) return 2;
    const int settleMs =
        args.size() >= 5 ? qBound(50, args.at(4).toInt(), 60000) : 1500;

    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;

    QTreeView* tree =
        window.findChild<QTreeView*>(QStringLiteral("signalTree"));
    QLineEdit* search =
        window.findChild<QLineEdit*>(QStringLiteral("signalTreeSearch"));
    QPushButton* previous =
        window.findChild<QPushButton*>(QStringLiteral("signalTreeSearchPrevious"));
    QPushButton* next =
        window.findChild<QPushButton*>(QStringLiteral("signalTreeSearchNext"));
    QPushButton* restore =
        window.findChild<QPushButton*>(QStringLiteral("signalTreeSearchRestore"));
    if (!tree || !search || !previous || !next || !restore || !tree->model()) return 4;

    const int fullTopLevelRows = tree->model()->rowCount();
    const QModelIndex preSearchIndex = tree->model()->index(0, 0);
    if (tree->selectionModel() && preSearchIndex.isValid()) {
        tree->selectionModel()->select(preSearchIndex,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        tree->selectionModel()->setCurrentIndex(preSearchIndex,
                                                QItemSelectionModel::NoUpdate);
    }

    search->setText(args.at(3));
    QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
    processEventsFor(app, settleMs);
    const bool queryRetained = search->text() == args.at(3);
    const int searchedTopLevelRows = tree->model()->rowCount();
    const QString firstMatchStatus = search->toolTip();
    const bool searchReportedMatch =
        firstMatchStatus.startsWith(QStringLiteral("Match 1 of "));
    const QModelIndex openedMatch = tree->currentIndex();
    const int selectedTargets = tree->selectionModel()
        ? tree->selectionModel()->selectedRows().size()
        : 0;
    const bool selectedFirstTargets = selectedTargets > 0 && selectedTargets <= 64;
    if (openedMatch.isValid()) tree->expand(openedMatch);
    if (tree->selectionModel() && preSearchIndex.isValid()) {
        tree->selectionModel()->setCurrentIndex(preSearchIndex,
                                                QItemSelectionModel::NoUpdate);
    }
    processEventsFor(app, settleMs);
    const bool openingNodeKeptUserCurrent =
        tree->currentIndex() == preSearchIndex;
    bool navigationChangedTarget = true;
    if (next->isEnabled()) {
        next->click();
        processEventsFor(app, 20);
        navigationChangedTarget = search->toolTip() != firstMatchStatus;
    }

    const QAbstractItemModel* model = tree->model();
    struct PendingIndex {
        QModelIndex parent;
        int depth = 0;
    };
    QVector<PendingIndex> pending;
    pending.push_back(PendingIndex{QModelIndex(), 0});
    qint64 visibleNodes = 0;
    int maxDepth = 0;
    const qint64 traversalLimit = 100000;
    while (!pending.isEmpty() && visibleNodes < traversalLimit) {
        const PendingIndex current = pending.takeLast();
        const int rows = model->rowCount(current.parent);
        for (int row = 0; row < rows && visibleNodes < traversalLimit; ++row) {
            const QModelIndex child = model->index(row, 0, current.parent);
            if (!child.isValid()) continue;
            ++visibleNodes;
            maxDepth = qMax(maxDepth, current.depth + 1);
            if (model->hasChildren(child)) {
                pending.push_back(PendingIndex{child, current.depth + 1});
            }
        }
    }

    restore->click();
    processEventsFor(app, 20);
    const int cancelledTopLevelRows = tree->model()->rowCount();
    const bool restoredPreSearchSelection = tree->selectionModel() &&
        tree->selectionModel()->selectedRows().size() == 1 &&
        tree->selectionModel()->selectedRows().first() == preSearchIndex &&
        tree->selectionModel()->currentIndex() == preSearchIndex;

    QTextStream out(stdout);
    out << "tree_reference_search,query," << csvField(args.at(3))
        << ",settle_ms," << settleMs
        << ",visible_nodes," << visibleNodes
        << ",max_depth," << maxDepth
        << ",query_retained," << (queryRetained ? 1 : 0)
        << ",full_top_level_rows," << fullTopLevelRows
        << ",searched_top_level_rows," << searchedTopLevelRows
        << ",cancelled_top_level_rows," << cancelledTopLevelRows
        << ",navigation_changed_target," << (navigationChangedTarget ? 1 : 0)
        << ",selected_targets," << selectedTargets
        << ",selected_first_targets," << (selectedFirstTargets ? 1 : 0)
        << ",opening_node_kept_user_current,"
        << (openingNodeKeptUserCurrent ? 1 : 0)
        << ",restored_pre_search_selection," << (restoredPreSearchSelection ? 1 : 0)
        << ",search_reported_match," << (searchReportedMatch ? 1 : 0)
        << '\n';
    out.flush();
    return visibleNodes > 0 && visibleNodes < traversalLimit &&
                   queryRetained &&
                   searchedTopLevelRows == fullTopLevelRows &&
                   cancelledTopLevelRows == fullTopLevelRows &&
                   navigationChangedTarget && selectedFirstTargets &&
                   openingNodeKeptUserCurrent &&
                   restoredPreSearchSelection &&
                   searchReportedMatch
        ? 0
        : 5;
}

class RangeCursorRegressionCanvas : public WaveCanvas {
public:
    using WaveCanvas::mouseMoveEvent;
    using WaveCanvas::mousePressEvent;
    using WaveCanvas::mouseReleaseEvent;
};

static int runRangeSelectionCursorRegression(QApplication& app) {
    WaveFile wave;
    wave.meta.start = 0;
    wave.meta.end = 1000;

    RangeCursorRegressionCanvas canvas;
    canvas.resize(1000, 320);
    canvas.setWave(&wave);
    canvas.show();
    app.processEvents();

    const qint64 originalCursor = 250;
    if (!canvas.setCursorTime(originalCursor)) return 3;

    int cursorSignals = 0;
    int rangeSignals = 0;
    QObject::connect(&canvas, &WaveCanvas::cursorMoved,
                     [&](qint64) { ++cursorSignals; });
    QObject::connect(&canvas, &WaveCanvas::viewportRangeSelected,
                     [&](qint64, qint64) { ++rangeSignals; });

    const QPointF dragStart(180.0, 12.0);
    const QPointF dragEnd(760.0, 12.0);
    QMouseEvent press(QEvent::MouseButtonPress, dragStart,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, dragEnd,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, dragEnd,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    canvas.mousePressEvent(&press);
    canvas.mouseMoveEvent(&move);
    canvas.mouseReleaseEvent(&release);
    app.processEvents();

    const bool rangeKeptCursor =
        canvas.cursorTime() == originalCursor &&
        cursorSignals == 0 && rangeSignals == 1;

    const QPointF clickPos(620.0, 12.0);
    QMouseEvent clickPress(QEvent::MouseButtonPress, clickPos,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent clickRelease(QEvent::MouseButtonRelease, clickPos,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    canvas.mousePressEvent(&clickPress);
    canvas.mouseReleaseEvent(&clickRelease);
    app.processEvents();

    const bool clickStillMovesCursor =
        canvas.cursorTime() != originalCursor && cursorSignals == 1;

    QTextStream out(stdout);
    out << "range_cursor_regression,range_kept_cursor,"
        << (rangeKeptCursor ? 1 : 0)
        << ",click_moved_cursor," << (clickStillMovesCursor ? 1 : 0)
        << ",cursor_signals," << cursorSignals
        << ",range_signals," << rangeSignals << '\n';
    out.flush();
    return (rangeKeptCursor && clickStillMovesCursor) ? 0 : 5;
}

static int runViewerSessionStateRegression(QApplication& app,
                                           const QStringList& args) {
    if (args.size() < 3) return 2;
    MainWindow window;
    window.resize(1600, 800);
    window.show();
    if (!window.openWaveFilePath(args.at(2), false)) return 3;
    processEventsFor(app, 50);
    QString error;
    const bool ok = window.runViewerSessionStateRegressionForBenchmark(&error);
    QTextStream out(stdout);
    out << "viewer_session_state,ok," << (ok ? 1 : 0);
    if (!error.isEmpty()) out << ",error," << csvField(error);
    out << '\n';
    out.flush();
    return ok ? 0 : 5;
}

static int runTreeSearchStateRegression(QApplication& app,
                                        const QStringList& args) {
    MainWindow window;
    window.resize(1100, 700);
    window.show();
    if (args.size() >= 3 && !window.openWaveFilePath(args.at(2), false)) {
        return 3;
    }
    processEventsFor(app, 50);
    QString error;
    const bool ok = window.runTreeSearchStateRegressionForBenchmark(&error);
    QTextStream out(stdout);
    out << "tree_search_state_restore," << (ok ? "ok" : "failed")
        << ",error," << csvField(error) << '\n';
    out.flush();
    return ok ? 0 : 5;
}

static int runCompareActivationOrderRegression(QApplication& app) {
    MainWindow window;
    window.resize(1100, 700);
    window.show();
    processEventsFor(app, 20);
    QString error;
    const bool ok = window.runCompareActivationOrderRegressionForBenchmark(&error);
    QTextStream out(stdout);
    out << "compare_activation_order," << (ok ? "ok" : "failed")
        << ",error," << csvField(error) << '\n';
    out.flush();
    return ok ? 0 : 5;
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

    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--derived-expression-benchmark")) {
        return runDerivedExpressionBenchmark(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--capture-zoom-sequence")) {
        qputenv("WV_VIEWER_STRICT_RENDER_CHECKS", QByteArray("1"));
        return runZoomCaptureSequence(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--zoom-boundary-benchmark")) {
        return runZoomBoundaryBenchmark(a, args);
    }
    if (args.size() >= 2 && args.at(1) == QStringLiteral("--range-selection-benchmark")) {
        return runRangeSelectionBenchmark(a, args);
    }
    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--range-selection-cursor-regression")) {
        return runRangeSelectionCursorRegression(a);
    }
    if (args.size() >= 2 && args.at(1) == QStringLiteral("--global-return-benchmark")) {
        return runGlobalReturnBenchmark(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--add-signals-benchmark")) {
        return runAddSignalsBenchmark(a, args);
    }
    if (args.size() >= 2 && args.at(1) == QStringLiteral("--active-shortcut-benchmark")) {
        return runActiveShortcutBenchmark(a, args);
    }
    if (args.size() >= 2 && args.at(1) == QStringLiteral("--tree-event-jump-benchmark")) {
        return runTreeEventJumpBenchmark(a, args);
    }
    if (args.size() >= 2 && args.at(1) == QStringLiteral("--value-find-ui-benchmark")) {
        return runValueFindUiBenchmark(a, args);
    }
    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--signal-condition-search-benchmark")) {
        return runSignalConditionSearchBenchmark(a, args);
    }
    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--tree-reference-search-benchmark")) {
        return runTreeReferenceSearchBenchmark(a, args);
    }
    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--viewer-session-state-regression")) {
        return runViewerSessionStateRegression(a, args);
    }
    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--tree-search-state-regression")) {
        return runTreeSearchStateRegression(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--value-find-benchmark")) {
        return runValueFindBenchmark(args);
    }

    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--compare-activation-order-regression")) {
        return runCompareActivationOrderRegression(a);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--render-benchmark")) {
        return runRenderBenchmark(a, args);
    }

    if (args.size() >= 2 &&
        args.at(1) == QStringLiteral("--procedural-clock-render-regression")) {
        qputenv("WV_VIEWER_STRICT_RENDER_CHECKS", QByteArray("1"));
        return runProceduralClockRenderRegression(a);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--bit-state-render-regression")) {
        qputenv("WV_VIEWER_STRICT_RENDER_CHECKS", QByteArray("1"));
        return runBitStateRenderRegression(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--lod-disappearance-stress")) {
        qputenv("WV_VIEWER_STRICT_RENDER_CHECKS", QByteArray("1"));
        return runLodDisappearanceStress(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--lod-visual-compare")) {
        qputenv("WV_VIEWER_STRICT_RENDER_CHECKS", QByteArray("1"));
        return runLodVisualCompare(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--lod-visual-compare-files")) {
        qputenv("WV_VIEWER_STRICT_RENDER_CHECKS", QByteArray("1"));
        return runLodVisualCompareFiles(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--dump-signal-head")) {
        return runDumpSignalHead(args);
    }

    const QIcon appIcon = loadApplicationIcon();
    a.setWindowIcon(appIcon);

    if (args.size() >= 4 && args.at(1) == QStringLiteral("--compare-and-exit")) {
        MainWindow w;
        w.setWindowIcon(appIcon);
        QString error;
        qint64 elapsedMs = 0;
        int resultSignals = 0;
        const bool eventTimesOnly = args.contains(QStringLiteral("--events-only"));
        QString expectedSamePeerPath;
        const int baselinePeerOption = args.indexOf(QStringLiteral("--baseline-peer"));
        if (baselinePeerOption >= 0 && baselinePeerOption + 1 < args.size()) {
            expectedSamePeerPath = args.at(baselinePeerOption + 1);
        }
        const bool ok = w.compareWaveFilePaths(args.at(2), args.at(3),
                                               false, false,
                                               &error, &elapsedMs, &resultSignals,
                                               eventTimesOnly,
                                               expectedSamePeerPath);
        const QTreeWidget* activeList =
            w.findChild<QTreeWidget*>(QStringLiteral("activeSignalList"));
        const bool noDifference =
            error.startsWith(QStringLiteral("No matching-path signal differs")) ||
            error.startsWith(QStringLiteral("No signal differences"));
        QTextStream out(stdout);
        out << "ok," << ((ok || noDifference) ? 1 : 0) << "\n";
        out << "elapsed_ms," << elapsedMs << "\n";
        out << "result_signals," << resultSignals << "\n";
        out << "active_signals,"
            << (activeList ? activeList->topLevelItemCount() : -1) << "\n";
        if (noDifference) out << "note," << error << "\n";
        else if (!ok) out << "error," << error << "\n";
        return (ok || noDifference) ? 0 : 7;
    }

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
