#include "MainWindow.h"

#include "WaveCanvas.h"
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
#include <QImage>
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
    return QString::number(internalTime);
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
        if (sig.samples.isEmpty() && sig.lodLevels.isEmpty()) continue;
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
        if (sig.samples.isEmpty() && sig.lodLevels.isEmpty()) continue;
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
        if (rawSig.samples.isEmpty() && rawSig.lodLevels.isEmpty()) continue;

        const QString rawName = benchmarkSignalName(rawWave, rawIndex);
        int lodIndex = -1;
        for (int i = 0; i < lodWave.signalList.size(); ++i) {
            const WaveSignal& candidate = lodWave.signalList.at(i);
            if (candidate.samples.isEmpty() && candidate.lodLevels.isEmpty()) continue;
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
        qputenv("WV_VIEWER_STRICT_RENDER_CHECKS", QByteArray("1"));
        return runZoomCaptureSequence(a, args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--value-find-benchmark")) {
        return runValueFindBenchmark(args);
    }

    if (args.size() >= 2 && args.at(1) == QStringLiteral("--render-benchmark")) {
        return runRenderBenchmark(a, args);
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
        const bool ok = w.compareWaveFilePaths(args.at(2), args.at(3),
                                               false, false,
                                               &error, &elapsedMs, &resultSignals);
        const bool noDifference =
            error.startsWith(QStringLiteral("No matching-path signal differs")) ||
            error.startsWith(QStringLiteral("No signal differences"));
        QTextStream out(stdout);
        out << "ok," << ((ok || noDifference) ? 1 : 0) << "\n";
        out << "elapsed_ms," << elapsedMs << "\n";
        out << "result_signals," << resultSignals << "\n";
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
