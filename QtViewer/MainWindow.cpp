#include "MainWindow.h"
#include "AgentRpcServer.h"
#include "WaveBlockCacheLoader.h"
#include "ActiveSignalListWidget.h"
#include "WaveCanvas.h"
#include "WaveParser4.h"
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWindow>
#include <QCoreApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QColor>
#include <QLabel>
#include <QLinearGradient>
#include <QPen>
#include <QPersistentModelIndex>
#include <QLineEdit>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QProgressDialog>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QSpinBox>
#include <QStringList>
#include <QStatusBar>
#include <QTextStream>
#include <QTreeView>
#include <QTreeWidget>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QByteArray>
#include <QBitArray>
#include <QBrush>
#include <QDataStream>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QMimeData>
#include <QMetaObject>
#include <QMap>
#include <QIODevice>
#include <QSet>
#include <QTreeWidgetItem>
#include <QItemSelectionModel>
#include <QUrl>
#include <QVBoxLayout>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLibrary>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <unordered_map>
#include <utility>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct TreeWarmupControl {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<int> priorityReferences;
    // 0 = not requested, 1 = priority queued, 2 = started.
    QVector<quint8> referenceStateByNodeId;
    int pendingUiBatches = 0;
};

#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#define WAVETRACE_COMPARE_HAS_SSE2 1
#else
#define WAVETRACE_COMPARE_HAS_SSE2 0
#endif

#if defined(__AVX2__)
#define WAVETRACE_COMPARE_HAS_AVX2 1
#else
#define WAVETRACE_COMPARE_HAS_AVX2 0
#endif

#if defined(__AVX512F__)
#define WAVETRACE_COMPARE_HAS_AVX512F 1
#else
#define WAVETRACE_COMPARE_HAS_AVX512F 0
#endif

#if WAVETRACE_COMPARE_HAS_SSE2 || WAVETRACE_COMPARE_HAS_AVX2 || WAVETRACE_COMPARE_HAS_AVX512F
#include <immintrin.h>
#endif

namespace {

constexpr const char* kMimeSignalIndexes = "application/x-waveviewer-signal-indexes";
constexpr const char* kMimeActiveRows = "application/x-waveviewer-active-rows";

constexpr int kTreeRoleSignalIndex = Qt::UserRole;
constexpr int kTreeRoleNodeId = Qt::UserRole + 100;
constexpr int kValueFindRoleFirstHit = Qt::UserRole;
constexpr int kValueFindRoleSignalIndex = Qt::UserRole + 1;
constexpr quint64 kViewerOnDemandSampleBudget = 20ull * 1000ull * 1000ull;
constexpr quint64 kViewerCacheBudgetBytes = 32ull * 1024ull * 1024ull * 1024ull;
constexpr int kCompareStreamingDefaultSignalBatchSize = 32768;
constexpr int kCompareStreamingHugeFileSignalBatchSize = 2048;
constexpr qint64 kCompareStreamingHugeFileThresholdBytes = 2ll * 1024ll * 1024ll * 1024ll;
constexpr quint64 kCompareStreamingBatchSampleBudget = 6ull * 1000ull * 1000ull;

int treeEventReductionThreshold() {
    bool ok = false;
    const int configured =
        qEnvironmentVariable("WV_VIEWER_TREE_EVENT_REDUCE_THRESHOLD").toInt(&ok);
    return ok ? qMax(1, configured) : 65536;
}

quint64 viewerCacheBudgetBytes() {
    bool ok = false;
    const quint64 configuredMb = qEnvironmentVariable("WV_VIEWER_CACHE_LIMIT_MB").toULongLong(&ok);
    if (ok && configuredMb != 0) {
        const quint64 boundedMb = qMin<quint64>(configuredMb, 32ull * 1024ull);
        return boundedMb * 1024ull * 1024ull;
    }

    quint64 budget = kViewerCacheBudgetBytes;
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        // 32 GiB is a ceiling, not a target.  Leave at least half of both
        // physical RAM and currently available RAM to the tree, Qt, the OS,
        // and temporary block assembly.  The environment override above is
        // intentionally exact for controlled performance experiments.
        const quint64 physicalShare = memory.ullTotalPhys / 2ull;
        const quint64 availableShare = memory.ullAvailPhys / 2ull;
        budget = qMin(budget, qMin(physicalShare, availableShare));
    }
#endif
    return budget;
}

// Extract a literal that every match must contain, but only for a deliberately
// small regex grammar whose implication is easy to prove. Unsupported syntax
// returns no prefilter and therefore falls back to QRegularExpression alone.
QString conservativeRegexRequiredLiteral(const QString& pattern) {
    QString best;
    QString run;
    run.reserve(pattern.size());
    auto finishRun = [&]() {
        if (run.size() > best.size()) best = run;
        run.clear();
    };
    const QString escapable =
        QStringLiteral(".^$|()[]{}*+?\\");
    const QString unsupported =
        QStringLiteral("^$|()[]{}*+?");
    auto consumeWildcardQuantifier = [&](int& index) {
        const int next = index + 1;
        if (next >= pattern.size()) return;
        const QChar quantifier = pattern.at(next);
        if (quantifier == QLatin1Char('*') ||
            quantifier == QLatin1Char('+') ||
            quantifier == QLatin1Char('?')) {
            index = next;
        } else if (quantifier == QLatin1Char('{')) {
            const int close = pattern.indexOf(QLatin1Char('}'), next + 1);
            if (close < 0) return;
            index = close;
        } else {
            return;
        }
        // Lazy and possessive modifiers do not change which surrounding
        // literals are required.
        if (index + 1 < pattern.size() &&
            (pattern.at(index + 1) == QLatin1Char('?') ||
             pattern.at(index + 1) == QLatin1Char('+'))) {
            ++index;
        }
    };

    for (int i = 0; i < pattern.size(); ++i) {
        const QChar ch = pattern.at(i);
        if (ch == QLatin1Char('^') && i == 0) continue;
        if (ch == QLatin1Char('$') && i == pattern.size() - 1) {
            finishRun();
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            if (++i >= pattern.size()) return QString();
            const QChar escaped = pattern.at(i);
            if (escapable.contains(escaped)) {
                run += escaped;
                continue;
            }
            if (QStringLiteral("dDhHsSvVwWNRX").contains(escaped)) {
                finishRun();
                consumeWildcardQuantifier(i);
                continue;
            }
            if ((escaped == QLatin1Char('p') ||
                 escaped == QLatin1Char('P')) &&
                i + 1 < pattern.size() &&
                pattern.at(i + 1) == QLatin1Char('{')) {
                const int close =
                    pattern.indexOf(QLatin1Char('}'), i + 2);
                if (close < 0) return QString();
                finishRun();
                i = close;
                consumeWildcardQuantifier(i);
                continue;
            }
            return QString();
        }
        if (ch == QLatin1Char('[')) {
            finishRun();
            int contentStart = i + 1;
            if (contentStart < pattern.size() &&
                pattern.at(contentStart) == QLatin1Char('^')) {
                ++contentStart;
            }
            bool escaped = false;
            int close = -1;
            for (int j = contentStart; j < pattern.size(); ++j) {
                const QChar classChar = pattern.at(j);
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (classChar == QLatin1Char('\\')) {
                    escaped = true;
                    continue;
                }
                // A leading ']' inside a character class is a literal.
                if (classChar == QLatin1Char(']') &&
                    j != contentStart) {
                    close = j;
                    break;
                }
            }
            if (close < 0) return QString();
            i = close;
            consumeWildcardQuantifier(i);
            continue;
        }
        if (ch == QLatin1Char('.')) {
            finishRun();
            consumeWildcardQuantifier(i);
            continue;
        }
        if (unsupported.contains(ch)) return QString();
        run += ch;
    }
    finishRun();
    return best;
}

bool isAsciiText(const QString& text) {
    for (const QChar ch : text) {
        if (ch.unicode() > 0x7f) return false;
    }
    return true;
}

bool regexRequiredLiteralMayOccur(
    const QString& subject, const QString& requiredLiteral,
    bool requiredLiteralIsAscii, Qt::CaseSensitivity caseSensitivity) {
    if (requiredLiteral.isEmpty()) return true;
    if (caseSensitivity == Qt::CaseSensitive) {
        return subject.contains(requiredLiteral, Qt::CaseSensitive);
    }
    // Unicode case folding includes non-ASCII characters equivalent to ASCII
    // ones (for example the Kelvin sign). Let PCRE handle those subjects.
    if (!requiredLiteralIsAscii) return true;

    const int literalSize = requiredLiteral.size();
    const int subjectSize = subject.size();
    const ushort* const literalData = requiredLiteral.utf16();
    const ushort* const subjectData = subject.utf16();
    const ushort first = literalData[0] >= 'A' && literalData[0] <= 'Z'
        ? ushort(literalData[0] + ('a' - 'A'))
        : literalData[0];
    bool hasNonAscii = false;
    for (int i = 0; i < subjectSize; ++i) {
        const ushort firstSubject = subjectData[i];
        if (firstSubject > 0x7f) {
            hasNonAscii = true;
            continue;
        }
        const ushort foldedFirst =
            firstSubject >= 'A' && firstSubject <= 'Z'
                ? ushort(firstSubject + ('a' - 'A'))
                : firstSubject;
        if (foldedFirst != first || i + literalSize > subjectSize) {
            continue;
        }
        bool matches = true;
        for (int j = 1; j < literalSize; ++j) {
            const ushort subjectChar = subjectData[i + j];
            if (subjectChar > 0x7f) {
                hasNonAscii = true;
                matches = false;
                break;
            }
            const ushort foldedSubject =
                subjectChar >= 'A' && subjectChar <= 'Z'
                    ? ushort(subjectChar + ('a' - 'A'))
                    : subjectChar;
            const ushort literalChar = literalData[j];
            const ushort foldedLiteral =
                literalChar >= 'A' && literalChar <= 'Z'
                    ? ushort(literalChar + ('a' - 'A'))
                    : literalChar;
            if (foldedSubject != foldedLiteral) {
                matches = false;
                break;
            }
        }
        if (matches) return true;
    }
    return hasNonAscii;
}

QString compareSideSuffix(const QString& text) {
    if ((text.endsWith(QStringLiteral("@A")) || text.endsWith(QStringLiteral("@B"))) && text.size() > 2) {
        return text.right(2);
    }
    return QString();
}

QString stripCompareSideMarker(QString text) {
    const QString suffix = compareSideSuffix(text);
    if (!suffix.isEmpty()) {
        text.chop(suffix.size());
    } else if ((text.startsWith(QStringLiteral("A ")) || text.startsWith(QStringLiteral("B "))) && text.size() > 2) {
        text = text.mid(2);
    }
    return text;
}

QString formatNameWidthBeforeCompareSuffix(QString name, int width) {
    if (width <= 1) return name;
    const QString suffix = compareSideSuffix(name);
    if (!suffix.isEmpty()) name.chop(suffix.size());
    QString out = QStringLiteral("%1[%2:0]").arg(name).arg(qMax(0, width - 1));
    if (!suffix.isEmpty()) out += suffix;
    return out;
}

bool isSupportedWaveFilePath(const QString& path) {
    if (path.isEmpty()) return false;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return false;
    return info.suffix().compare(QStringLiteral("wvz4"), Qt::CaseInsensitive) == 0;
}

bool hasWvz4Suffix(const QString& path) {
    return path.endsWith(QStringLiteral(".wvz4"), Qt::CaseInsensitive);
}

QString unsupportedWaveFormatError(const QString& path) {
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.isEmpty()) {
        return QStringLiteral("Only WVZ4 wave files (*.wvz4) are supported.");
    }
    return QStringLiteral("Unsupported wave format '.%1'. Only WVZ4 wave files (*.wvz4) are supported.")
        .arg(suffix);
}

QString firstSupportedWaveFilePathFromMime(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) return QString();
    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (isSupportedWaveFilePath(path)) return path;
    }
    return QString();
}

bool viewerPerfLogEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("WV_VIEWER_PERF_LOG");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool viewerDisableLodEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("WV_VIEWER_DISABLE_LOD");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void compactLodRanges(QVector<WaveLodValidRange>& ranges) {
    if (ranges.isEmpty()) return;
    std::sort(ranges.begin(), ranges.end(), [](const WaveLodValidRange& a, const WaveLodValidRange& b) {
        return a.start < b.start;
    });
    int write = 0;
    for (int read = 0; read < ranges.size(); ++read) {
        const WaveLodValidRange range = ranges.at(read);
        if (range.end <= range.start) continue;
        if (write > 0 && range.start <= ranges.at(write - 1).end) {
            if (range.end > ranges[write - 1].end) ranges[write - 1].end = range.end;
        } else {
            if (write != read) ranges[write] = range;
            ++write;
        }
    }
    ranges.resize(write);
}

bool lodRangesCoverWindow(const QVector<WaveLodValidRange>& ranges, qint64 start, qint64 end) {
    if (end <= start) return true;
    if (ranges.isEmpty()) return true;
    qint64 cursor = start;
    for (const WaveLodValidRange& range : ranges) {
        if (range.end <= cursor) continue;
        if (range.start > cursor) return false;
        cursor = qMax(cursor, range.end);
        if (cursor >= end) return true;
    }
    return false;
}

bool lodLevelLoadedForWindow(const WaveLodLevel& level, qint64 start, qint64 end) {
    if (level.bucketCycles <= 0 || (level.samples.isEmpty() && level.buckets.isEmpty())) return false;
    return lodRangesCoverWindow(level.loadedRanges, start, end);
}

bool lodRangesIntersectWindowOrEmpty(const QVector<WaveLodValidRange>& ranges, qint64 start, qint64 end) {
    if (end <= start) return true;
    if (ranges.isEmpty()) return true;
    for (const WaveLodValidRange& range : ranges) {
        if (range.end > start && range.start < end) return true;
    }
    return false;
}

bool signalHasLoadedLodForWindow(const WaveSignal& sig,
                                 qint64 start,
                                 qint64 end,
                                 int plotWidth) {
    if (end <= start || sig.lodLevels.isEmpty()) return false;
    const double cyclesPerPixel = double(end - start) / double(qMax(1, plotWidth));
    if (cyclesPerPixel < 10.0) return false;
    const qint64 maxBucketCycles = qMax<qint64>(1, qint64(std::floor(cyclesPerPixel)));
    for (const WaveLodLevel& level : sig.lodLevels) {
        if (level.bucketCycles <= 0) continue;
        if (level.bucketCycles > maxBucketCycles) break;
        if (lodLevelLoadedForWindow(level, start, end)) return true;
    }
    return false;
}

qint64 lodSampleEventTimeInRange(const WaveLodLevel& level, qint64 start, qint64 end, bool firstEvent) {
    if (end <= start || level.samples.isEmpty()) return -1;
    if (!lodRangesIntersectWindowOrEmpty(level.loadedRanges, start, end)) return -1;

    auto lowerSample = [](const QVector<WaveSample>& samples, qint64 t) {
        int lo = 0;
        int hi = samples.size();
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (samples.at(mid).time < t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    };

    if (firstEvent) {
        const int i = lowerSample(level.samples, start);
        return (i < level.samples.size() && level.samples.at(i).time < end)
            ? level.samples.at(i).time
            : -1;
    }

    int i = lowerSample(level.samples, end) - 1;
    if (i < 0 || i >= level.samples.size()) return -1;
    const qint64 t = level.samples.at(i).time;
    return t >= start ? t : -1;
}

qint64 signalLodEventTimeInRange(const WaveSignal& sig,
                                 qint64 start,
                                 qint64 end,
                                 int plotWidth,
                                 bool firstEvent) {
    if (end <= start || sig.lodLevels.isEmpty()) return -1;
    const double cyclesPerPixel = double(end - start) / double(qMax(1, plotWidth));
    if (cyclesPerPixel < 10.0) return -1;
    const qint64 maxBucketCycles = qMax<qint64>(1, qint64(std::floor(cyclesPerPixel)));

    const WaveLodLevel* preferred = nullptr;
    for (const WaveLodLevel& level : sig.lodLevels) {
        if (level.bucketCycles <= 0) continue;
        if (level.bucketCycles > maxBucketCycles) break;
        if (level.samples.isEmpty()) continue;
        if (!lodRangesIntersectWindowOrEmpty(level.loadedRanges, start, end)) continue;
        preferred = &level;
    }

    if (preferred) {
        const qint64 t = lodSampleEventTimeInRange(*preferred, start, end, firstEvent);
        if (t >= 0) return t;
    }

    qint64 best = -1;
    for (const WaveLodLevel& level : sig.lodLevels) {
        if (level.bucketCycles <= 0) continue;
        if (level.bucketCycles > maxBucketCycles) break;
        const qint64 t = lodSampleEventTimeInRange(level, start, end, firstEvent);
        if (t < 0) continue;
        if (best < 0 || (firstEvent ? (t < best) : (t > best))) best = t;
    }
    return best;
}

void compactLodSamples(QVector<WaveSample>& samples) {
    if (samples.size() <= 1) return;
    std::sort(samples.begin(), samples.end(), [](const WaveSample& a, const WaveSample& b) {
        return a.time < b.time;
    });
    int write = 1;
    for (int read = 1; read < samples.size(); ++read) {
        if (samples.at(read).time == samples.at(write - 1).time) {
            samples[write - 1] = samples.at(read);
        } else {
            if (write != read) samples[write] = samples.at(read);
            ++write;
        }
    }
    samples.resize(write);
}

QVector<WaveLodValidRange> missingRangesForWindow(const QVector<WaveLodValidRange>& loadedRanges,
                                                  qint64 start,
                                                  qint64 end) {
    QVector<WaveLodValidRange> loaded = loadedRanges;
    compactLodRanges(loaded);
    QVector<WaveLodValidRange> missing;
    qint64 cursor = start;
    for (const WaveLodValidRange& range : loaded) {
        if (range.end <= cursor) continue;
        if (range.start >= end) break;
        if (range.start > cursor) {
            WaveLodValidRange gap;
            gap.start = cursor;
            gap.end = qMin(end, range.start);
            if (gap.end > gap.start) missing.push_back(gap);
        }
        cursor = qMax(cursor, range.end);
        if (cursor >= end) break;
    }
    if (cursor < end) {
        WaveLodValidRange gap;
        gap.start = cursor;
        gap.end = end;
        missing.push_back(gap);
    }
    return missing;
}

void mergeRawSamples(WaveSignal& target, QVector<WaveSample>&& incoming) {
    compactLodSamples(incoming);
    if (target.samples.isEmpty()) {
        target.samples = std::move(incoming);
        return;
    }
    if (incoming.isEmpty()) return;

    // Both inputs are time ordered. A linear merge avoids sorting the complete
    // rolling cache again at every prefetch-boundary crossing.
    QVector<WaveSample> merged;
    merged.reserve(target.samples.size() + incoming.size());
    int oldIndex = 0;
    int newIndex = 0;
    while (oldIndex < target.samples.size() && newIndex < incoming.size()) {
        const qint64 oldTime = target.samples.at(oldIndex).time;
        const qint64 newTime = incoming.at(newIndex).time;
        if (oldTime < newTime) {
            merged.push_back(std::move(target.samples[oldIndex++]));
        } else if (newTime < oldTime) {
            merged.push_back(std::move(incoming[newIndex++]));
        } else {
            // The newly decoded boundary sample is authoritative.
            merged.push_back(std::move(incoming[newIndex++]));
            ++oldIndex;
        }
    }
    while (oldIndex < target.samples.size()) merged.push_back(std::move(target.samples[oldIndex++]));
    while (newIndex < incoming.size()) merged.push_back(std::move(incoming[newIndex++]));
    target.samples = std::move(merged);
}

void trimRawSamplesToWindow(WaveSignal& signal, qint64 start, qint64 end) {
    if (signal.samples.isEmpty()) return;
    auto first = std::lower_bound(signal.samples.begin(), signal.samples.end(), start,
        [](const WaveSample& sample, qint64 time) { return sample.time < time; });
    // Preserve the state immediately before the retained interval. Without this
    // anchor, a quiet signal would render with an incorrect value at the left edge.
    if (first != signal.samples.begin()) --first;
    auto last = std::upper_bound(first, signal.samples.end(), end,
        [](qint64 time, const WaveSample& sample) { return time < sample.time; });
    if (first == signal.samples.begin() && last == signal.samples.end()) return;
    QVector<WaveSample> kept;
    kept.reserve(int(last - first));
    for (auto it = first; it != last; ++it) kept.push_back(std::move(*it));
    signal.samples = std::move(kept);
}

void mergeLodLevel(WaveLodLevel& dst, WaveLodLevel&& src) {
    if (src.bucketCycles <= 0) return;
    if (dst.bucketCycles <= 0) dst.bucketCycles = src.bucketCycles;
    if (dst.bucketCycles != src.bucketCycles) return;

    dst.samples.reserve(dst.samples.size() + src.samples.size());
    for (WaveSample& sample : src.samples) dst.samples.push_back(std::move(sample));
    compactLodSamples(dst.samples);

    dst.buckets.reserve(dst.buckets.size() + src.buckets.size());
    for (WaveLodBucket& bucket : src.buckets) dst.buckets.push_back(std::move(bucket));
    if (dst.buckets.size() > 1) {
        std::sort(dst.buckets.begin(), dst.buckets.end(), [](const WaveLodBucket& a, const WaveLodBucket& b) {
            if (a.start != b.start) return a.start < b.start;
            return a.end < b.end;
        });
        int write = 1;
        for (int read = 1; read < dst.buckets.size(); ++read) {
            if (dst.buckets.at(read).start == dst.buckets.at(write - 1).start &&
                dst.buckets.at(read).end == dst.buckets.at(write - 1).end) {
                dst.buckets[write - 1] = dst.buckets.at(read);
            } else {
                if (write != read) dst.buckets[write] = dst.buckets.at(read);
                ++write;
            }
        }
        dst.buckets.resize(write);
    }

    dst.validRanges.reserve(dst.validRanges.size() + src.validRanges.size());
    for (const WaveLodValidRange& range : src.validRanges) dst.validRanges.push_back(range);
    compactLodRanges(dst.validRanges);

    dst.loadedRanges.reserve(dst.loadedRanges.size() + src.loadedRanges.size());
    for (const WaveLodValidRange& range : src.loadedRanges) dst.loadedRanges.push_back(range);
    compactLodRanges(dst.loadedRanges);
}

void viewerPerfLog(const char* step, qint64 elapsedMs, int signalCount, int treeNodeCount, int activeRows = -1) {
    if (!viewerPerfLogEnabled()) return;
    std::fprintf(stderr,
                 "[qtviewer-perf] step=%s elapsed_ms=%lld signals=%d tree_nodes=%d active_rows=%d\n",
                 step,
                 static_cast<long long>(elapsedMs),
                 signalCount,
                 treeNodeCount,
                 activeRows);
    std::fflush(stderr);
    const char* filePath = std::getenv("WV_VIEWER_PERF_LOG_FILE");
    if (filePath && filePath[0]) {
        if (FILE* f = std::fopen(filePath, "ab")) {
            std::fprintf(f,
                         "[qtviewer-perf] step=%s elapsed_ms=%lld signals=%d tree_nodes=%d active_rows=%d\n",
                         step,
                         static_cast<long long>(elapsedMs),
                         signalCount,
                         treeNodeCount,
                         activeRows);
            std::fclose(f);
        }
    }
}

void comparePerfLog(const char* step,
                    qint64 elapsedMs,
                    int batchStart,
                    int batchCount,
                    int leftCount,
                    int rightCount,
                    int outputPairs) {
    if (!viewerPerfLogEnabled()) return;
    std::fprintf(stderr,
                 "[qtviewer-perf] step=%s elapsed_ms=%lld batch_start=%d batch_count=%d left=%d right=%d output_pairs=%d\n",
                 step,
                 static_cast<long long>(elapsedMs),
                 batchStart,
                 batchCount,
                 leftCount,
                 rightCount,
                 outputPairs);
    std::fflush(stderr);
    const char* filePath = std::getenv("WV_VIEWER_PERF_LOG_FILE");
    if (filePath && filePath[0]) {
        if (FILE* f = std::fopen(filePath, "ab")) {
            std::fprintf(f,
                         "[qtviewer-perf] step=%s elapsed_ms=%lld batch_start=%d batch_count=%d left=%d right=%d output_pairs=%d\n",
                         step,
                         static_cast<long long>(elapsedMs),
                         batchStart,
                         batchCount,
                         leftCount,
                         rightCount,
                         outputPairs);
            std::fclose(f);
        }
    }
}

bool rawBlockCompareDisabledByEnv() {
    const char* value = std::getenv("WV_VIEWER_DISABLE_RAW_BLOCK_COMPARE");
    return value && value[0] && value[0] != '0';
}

static inline uint32_t fnv1aStep(uint32_t h, unsigned char c) {
    return (h ^ uint32_t(c)) * 16777619u;
}

static inline uint32_t fnv1aHash(const char* data, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; ++i) h = fnv1aStep(h, static_cast<unsigned char>(data[i]));
    return h ? h : 1u;
}

template<class T, int N = 32>
class SmallVec32 {
public:
    SmallVec32() : m_size(0), m_capacity(N), m_data(m_local) {}
    ~SmallVec32() { if (m_data != m_local) delete[] m_data; }

    SmallVec32(const SmallVec32& other) : SmallVec32() {
        reserve(other.m_size);
        for (int i = 0; i < other.m_size; ++i) m_data[i] = other.m_data[i];
        m_size = other.m_size;
    }

    SmallVec32(SmallVec32&& other) noexcept : SmallVec32() {
        if (other.m_data == other.m_local) {
            for (int i = 0; i < other.m_size; ++i) m_local[i] = std::move(other.m_local[i]);
            m_size = other.m_size;
        } else {
            m_data = other.m_data;
            m_capacity = other.m_capacity;
            m_size = other.m_size;
            other.m_data = other.m_local;
            other.m_capacity = N;
            other.m_size = 0;
        }
    }

    SmallVec32& operator=(const SmallVec32& other) {
        if (this == &other) return *this;
        clear();
        reserve(other.m_size);
        for (int i = 0; i < other.m_size; ++i) m_data[i] = other.m_data[i];
        m_size = other.m_size;
        return *this;
    }

    SmallVec32& operator=(SmallVec32&& other) noexcept {
        if (this == &other) return *this;
        if (m_data != m_local) delete[] m_data;
        m_data = m_local;
        m_capacity = N;
        m_size = 0;

        if (other.m_data == other.m_local) {
            for (int i = 0; i < other.m_size; ++i) m_local[i] = std::move(other.m_local[i]);
            m_size = other.m_size;
        } else {
            m_data = other.m_data;
            m_capacity = other.m_capacity;
            m_size = other.m_size;
            other.m_data = other.m_local;
            other.m_capacity = N;
            other.m_size = 0;
        }
        return *this;
    }

    int size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    T* data() { return m_data; }
    const T* data() const { return m_data; }

    T& operator[](int i) { return m_data[i]; }
    const T& operator[](int i) const { return m_data[i]; }

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }

    void clear() { m_size = 0; }

    void reserve(int wanted) {
        if (wanted <= m_capacity) return;
        int newCap = m_capacity;
        while (newCap < wanted) newCap *= 2;
        T* p = new T[newCap];
        for (int i = 0; i < m_size; ++i) p[i] = std::move(m_data[i]);
        if (m_data != m_local) delete[] m_data;
        m_data = p;
        m_capacity = newCap;
    }

    void push_back(const T& v) {
        if (m_size >= m_capacity) reserve(m_size + 1);
        m_data[m_size++] = v;
    }

    void push_back(T&& v) {
        if (m_size >= m_capacity) reserve(m_size + 1);
        m_data[m_size++] = std::move(v);
    }

private:
    int m_size;
    int m_capacity;
    T* m_data;
    T m_local[N];
};

struct SmallStrView {
    const char* data = nullptr;
    int len = 0;
    uint32_t hash = 0;
};

struct SmallStr32 {
    SmallStr32() = default;

    SmallStr32(const char* p, int n, uint32_t h = 0) {
        assign(p, n, h);
    }

    SmallStr32(const SmallStr32& other) {
        assign(other.data(), other.size, other.hash);
    }

    SmallStr32(SmallStr32&& other) noexcept {
        moveFrom(std::move(other));
    }

    ~SmallStr32() {
        if (heap) delete[] heap;
    }

    SmallStr32& operator=(const SmallStr32& other) {
        if (this == &other) return *this;
        assign(other.data(), other.size, other.hash);
        return *this;
    }

    SmallStr32& operator=(SmallStr32&& other) noexcept {
        if (this == &other) return *this;
        if (heap) delete[] heap;
        heap = nullptr;
        size = 0;
        hash = 0;
        moveFrom(std::move(other));
        return *this;
    }

    void assign(const char* p, int n, uint32_t h = 0) {
        if (heap) {
            delete[] heap;
            heap = nullptr;
        }

        if (!p || n <= 0) {
            size = 0;
            hash = 1u;
            local[0] = '\0';
            return;
        }

        if (n > 65535) n = 65535;
        size = static_cast<uint16_t>(n);
        hash = h ? h : fnv1aHash(p, n);

        if (n <= int(sizeof(local))) {
            memcpy(local, p, size_t(n));
            if (n < int(sizeof(local))) local[n] = '\0';
        } else {
            heap = new char[size_t(n) + 1u];
            memcpy(heap, p, size_t(n));
            heap[n] = '\0';
        }
    }

    void moveFrom(SmallStr32&& other) {
        size = other.size;
        hash = other.hash;
        if (other.heap) {
            heap = other.heap;
            other.heap = nullptr;
        } else {
            memcpy(local, other.local, size_t(other.size));
            if (other.size < sizeof(local)) local[other.size] = '\0';
            heap = nullptr;
        }
        other.size = 0;
        other.hash = 1u;
        other.local[0] = '\0';
    }

    const char* data() const { return heap ? heap : local; }

    bool equals(const char* p, int n, uint32_t h) const {
        return int(size) == n && hash == h && memcmp(data(), p, size_t(n)) == 0;
    }

    QString toQString() const {
        return QString::fromUtf8(data(), int(size));
    }

    uint16_t size = 0;
    uint32_t hash = 1u;
    char* heap = nullptr;
    char local[32] = {};
};

struct FlatIntIntMap {
    struct Entry {
        quint32 key = 0;
        int value = -1;
    };

    void clear() {
        table.clear();
        mask = 0;
        used = 0;
    }

    void reserve(int wanted) {
        int cap = 8;
        while (cap < wanted * 2) cap <<= 1;
        if (cap <= table.size()) return;

        QVector<Entry> old = std::move(table);
        table.clear();
        table.resize(cap);
        mask = cap - 1;
        used = 0;

        for (const Entry& e : old) {
            if (e.value >= 0) insert(e.key, e.value);
        }
    }

    bool find(quint32 key, int& outValue) const {
        if (table.isEmpty()) return false;
        uint32_t h = uint32_t(key) * 2654435761u;
        int pos = int(h) & mask;
        for (;;) {
            const Entry& e = table[pos];
            if (e.value < 0) return false;
            if (e.key == key) {
                outValue = e.value;
                return true;
            }
            pos = (pos + 1) & mask;
        }
    }

    void insert(quint32 key, int value) {
        if (table.isEmpty() || used * 2 >= table.size()) reserve(qMax(8, used + 1));
        uint32_t h = uint32_t(key) * 2654435761u;
        int pos = int(h) & mask;
        for (;;) {
            Entry& e = table[pos];
            if (e.value < 0) {
                e.key = key;
                e.value = value;
                ++used;
                return;
            }
            if (e.key == key) {
                e.value = value;
                return;
            }
            pos = (pos + 1) & mask;
        }
    }

    QVector<Entry> table;
    int mask = 0;
    int used = 0;
};

struct SmallStrPool {
    struct Entry {
        uint32_t hash = 0;
        int id = -1;
    };

    void reserve(int n) {
        strings.reserve(n);
        int cap = 8;
        while (cap < n * 2) cap <<= 1;

        if (!strings.isEmpty()) {
            rehash(cap);
            return;
        }

        table.clear();
        table.resize(cap);
        mask = cap - 1;
        used = 0;
    }

    int intern(const SmallStrView& v) {
        if (!v.data || v.len <= 0) return -1;
        if (table.isEmpty()) reserve(1024);
        if (used * 2 >= table.size()) rehash(table.size() * 2);

        int pos = int(v.hash) & mask;
        for (;;) {
            Entry& e = table[pos];
            if (e.id < 0) {
                const int id = strings.size();
                strings.push_back(SmallStr32(v.data, v.len, v.hash));
                e.hash = v.hash;
                e.id = id;
                ++used;
                return id;
            }
            if (e.hash == v.hash && strings[e.id].equals(v.data, v.len, v.hash)) {
                return e.id;
            }
            pos = (pos + 1) & mask;
        }
    }

    const SmallStr32& get(int id) const {
        return strings[id];
    }

    void rehash(int cap) {
        QVector<Entry> old = std::move(table);
        table.clear();
        table.resize(cap);
        mask = cap - 1;
        used = 0;
        for (const Entry& e : old) {
            if (e.id < 0) continue;
            int pos = int(e.hash) & mask;
            for (;;) {
                Entry& dst = table[pos];
                if (dst.id < 0) {
                    dst = e;
                    ++used;
                    break;
                }
                pos = (pos + 1) & mask;
            }
        }
    }

    QVector<SmallStr32> strings;
    QVector<Entry> table;
    int mask = 0;
    int used = 0;
};

using PathSegViewList = SmallVec32<SmallStrView, 32>;
using PathNameIdList = SmallVec32<int, 32>;

PathSegViewList splitPathToSmallVec(const QByteArray& utf8) {
    PathSegViewList out;
    const char* s = utf8.constData();
    const int n = utf8.size();

    int start = 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i <= n; ++i) {
        if (i == n || s[i] == '.') {
            const int len = i - start;
            if (len > 0) {
                SmallStrView v;
                v.data = s + start;
                v.len = len;
                v.hash = h ? h : 1u;
                out.push_back(v);
            }
            start = i + 1;
            h = 2166136261u;
        } else {
            h = fnv1aStep(h, static_cast<unsigned char>(s[i]));
        }
    }
    return out;
}

struct SignalPathEntry {
    int signalIndex = -1;
    int signalId = -1;
    int width = 1;
    PathNameIdList nameIds;
};

struct LogicTreeNode {
    quint32 nameToken = 0;
    int parent = -1;
    int rowInParent = -1;
    int childListId = -1;
    int signalIndex = -1;
    int signalId = -1;
    int width = 1;
};

struct LogicChildList {
    SmallVec32<int, 32> children;
    FlatIntIntMap lookup;
    bool lookupReady = false;
};

} // namespace

struct SignalLogicTree {
    using WaveChildListCache = std::unordered_map<int, std::unique_ptr<LogicChildList>>;

    SmallStrPool names;
    QVector<QByteArray> waveNamesById;
    bool usesWaveNameTokens = false;
    QVector<SignalPathEntry> signalPaths;
    QVector<LogicTreeNode> nodes;
    QVector<LogicChildList> childLists;
    SmallVec32<int, 32> roots;
    FlatIntIntMap rootLookup;
    QVector<int> nodeIdBySignalIndex;
    const QVector<int>* waveNodeIdBySignalIndex = nullptr;
    const WaveTreeInfo* waveTree = nullptr;
    const WaveSignalList* waveSignalDefs = nullptr;
    mutable WaveChildListCache waveChildLists;
    // Expression lookup asks for only a handful of names.  Cache the matching
    // compact tree tokens instead of materializing a QString/hash entry for
    // every signal in a potentially multi-million-node waveform.
    mutable QHash<QString, QVector<quint32>> exactNameTokenCache;
    mutable QHash<QString, int> exactSignalPathCache;

    void clear() {
        names = SmallStrPool();
        waveNamesById.clear();
        usesWaveNameTokens = false;
        signalPaths.clear();
        nodes.clear();
        childLists.clear();
        roots.clear();
        rootLookup.clear();
        nodeIdBySignalIndex.clear();
        waveNodeIdBySignalIndex = nullptr;
        waveTree = nullptr;
        waveSignalDefs = nullptr;
        waveChildLists.clear();
        exactNameTokenCache.clear();
        exactSignalPathCache.clear();
    }

    bool usesDirectWaveTree() const { return waveTree != nullptr; }

    int nodeCount() const {
        return waveTree ? waveTree->nodesById.size() : nodes.size();
    }

    bool isValidNodeId(int nodeId) const {
        if (waveTree) {
            return nodeId > 0 && nodeId < waveTree->nodesById.size() &&
                   waveTree->nodesById.at(nodeId).valid;
        }
        return nodeId >= 0 && nodeId < nodes.size() && nodes.at(nodeId).nameToken != 0;
    }

    quint32 nodeNameToken(int nodeId) const {
        if (!isValidNodeId(nodeId)) return 0;
        return waveTree ? waveTree->nodesById.at(nodeId).nameToken : nodes.at(nodeId).nameToken;
    }

    int nodeParent(int nodeId) const {
        if (!isValidNodeId(nodeId)) return -1;
        if (!waveTree) return nodes.at(nodeId).parent;
        const int parent = waveTree->nodesById.at(nodeId).parentId;
        return parent > 0 ? parent : -1;
    }

    int nodeRowInParent(int nodeId) const {
        if (!isValidNodeId(nodeId)) return -1;
        return waveTree ? waveTree->nodesById.at(nodeId).rowInParent : nodes.at(nodeId).rowInParent;
    }

    int nodeSignalIndex(int nodeId) const {
        if (!isValidNodeId(nodeId)) return -1;
        return waveTree ? waveTreeEffectiveSignalIndex(*waveTree, nodeId)
                        : nodes.at(nodeId).signalIndex;
    }

    int nodeSignalId(int nodeId) const {
        if (!isValidNodeId(nodeId)) return -1;
        return waveTree ? waveTreeEffectiveSignalId(*waveTree, nodeId)
                        : nodes.at(nodeId).signalId;
    }

    int nodeWidth(int nodeId) const {
        const int signalIndex = nodeSignalIndex(nodeId);
        if (waveTree) {
            if (waveSignalDefs && signalIndex >= 0 && signalIndex < waveSignalDefs->size()) {
                return waveSignalDefs->at(signalIndex).width;
            }
            return 1;
        }
        return isValidNodeId(nodeId) ? nodes.at(nodeId).width : 1;
    }

    int nodeIdForSignalIndex(int signalIndex) const {
        const QVector<int>& map = waveNodeIdBySignalIndex
            ? *waveNodeIdBySignalIndex
            : nodeIdBySignalIndex;
        return (signalIndex >= 0 && signalIndex < map.size()) ? map.at(signalIndex) : -1;
    }

    bool hasChildren(int nodeId) const {
        if (waveTree) {
            if (!isValidNodeId(nodeId)) return false;
            const WaveTreeNode& node = waveTree->nodesById.at(nodeId);
            if (node.firstChild != 0) return true;
            if (nodeId < waveTree->bitsetIndexByNodeId.size() &&
                waveTree->bitsetIndexByNodeId.at(nodeId) > 0) {
                return true;
            }
            if (nodeId < waveTree->arrayIndexByNodeId.size() &&
                waveTree->arrayIndexByNodeId.at(nodeId) > 0) {
                return true;
            }
            if (node.kind != kWaveTreeNodeKindReference ||
                node.referenceTargetId <= 0 ||
                node.referenceTargetId >= waveTree->nodesById.size()) {
                return false;
            }
            const WaveTreeNode& target =
                waveTree->nodesById.at(node.referenceTargetId);
            return target.valid && target.firstChild != 0;
        }
        if (nodeId < 0 || nodeId >= nodes.size()) return false;
        const int listId = nodes[nodeId].childListId;
        return listId >= 0 && listId < childLists.size() && !childLists[listId].children.empty();
    }

    const LogicChildList* childListForNode(int nodeId) const {
        if (waveTree) {
            if (!hasChildren(nodeId)) return nullptr;
            const auto found = waveChildLists.find(nodeId);
            if (found != waveChildLists.end()) return found->second.get();

            std::unique_ptr<LogicChildList> list(new LogicChildList());
            int childId = waveTree->nodesById.at(nodeId).firstChild;
            while (childId != 0) {
                if (!isValidNodeId(childId)) return nullptr;
                list->children.push_back(childId);
                childId = waveTree->nodesById.at(childId).nextSibling;
            }
            const LogicChildList* result = list.get();
            waveChildLists.emplace(nodeId, std::move(list));
            return result;
        }
        if (nodeId < 0 || nodeId >= nodes.size()) return nullptr;
        const int listId = nodes[nodeId].childListId;
        if (listId < 0 || listId >= childLists.size()) return nullptr;
        return &childLists[listId];
    }

    bool isPendingReference(int nodeId) const {
        if (!waveTree || !isValidNodeId(nodeId)) return false;
        const WaveTreeNode& node = waveTree->nodesById.at(nodeId);
        return node.kind == kWaveTreeNodeKindReference &&
               node.referenceTargetId > 0 &&
               node.firstChild == 0;
    }

    void invalidateWaveChildList(int nodeId) {
        if (waveTree) waveChildLists.erase(nodeId);
    }

    bool isBitsetContainer(int nodeId) const {
        return waveTree && isValidNodeId(nodeId) &&
               nodeId < waveTree->bitsetIndexByNodeId.size() &&
               waveTree->bitsetIndexByNodeId.at(nodeId) > 0;
    }

    bool isArrayContainer(int nodeId) const {
        return waveTree && isValidNodeId(nodeId) &&
               nodeId < waveTree->arrayIndexByNodeId.size() &&
               waveTree->arrayIndexByNodeId.at(nodeId) > 0;
    }

    bool arrayHasUnmaterializedElements(int nodeId) const {
        if (!isArrayContainer(nodeId)) return false;
        const int encodedIndex = waveTree->arrayIndexByNodeId.at(nodeId);
        return encodedIndex > 0 && encodedIndex <= waveTree->arrays.size() &&
               !waveTree->arrays.at(encodedIndex - 1).materialized;
    }

    LogicChildList& ensureChildList(int nodeId) {
        LogicTreeNode& node = nodes[nodeId];
        if (node.childListId < 0) {
            node.childListId = childLists.size();
            childLists.push_back(LogicChildList());
        }
        return childLists[node.childListId];
    }

    int findChildInList(const LogicChildList& list, quint32 nameToken) const {
        if (list.lookupReady) {
            int nodeId = -1;
            return list.lookup.find(nameToken, nodeId) ? nodeId : -1;
        }
        for (int childNodeId : list.children) {
            if (nodes[childNodeId].nameToken == nameToken) return childNodeId;
        }
        return -1;
    }

    void maybeBuildLookup(LogicChildList& list) {
        if (list.lookupReady || list.children.size() <= 32) return;
        list.lookup.reserve(list.children.size() * 2);
        for (int childNodeId : list.children) {
            list.lookup.insert(nodes[childNodeId].nameToken, childNodeId);
        }
        list.lookupReady = true;
    }

    int findOrCreateChildByNameToken(int parentNodeId,
                                  quint32 nameToken,
                                  bool leaf,
                                  int signalIndex,
                                  int signalId,
                                  int width) {
        if (nameToken == 0) return parentNodeId;

        if (parentNodeId < 0) {
            int existing = -1;
            if (rootLookup.find(nameToken, existing)) {
                if (leaf) {
                    nodes[existing].signalIndex = signalIndex;
                    nodes[existing].signalId = signalId;
                    nodes[existing].width = width;
                }
                return existing;
            }

            LogicTreeNode node;
            node.nameToken = nameToken;
            node.parent = -1;
            node.rowInParent = roots.size();
            node.signalIndex = leaf ? signalIndex : -1;
            node.signalId = leaf ? signalId : -1;
            node.width = width;
            const int nodeId = nodes.size();
            nodes.push_back(node);
            roots.push_back(nodeId);
            rootLookup.insert(nameToken, nodeId);
            return nodeId;
        }

        LogicChildList& list = ensureChildList(parentNodeId);
        int existing = findChildInList(list, nameToken);
        if (existing >= 0) {
            if (leaf) {
                nodes[existing].signalIndex = signalIndex;
                nodes[existing].signalId = signalId;
                nodes[existing].width = width;
            }
            return existing;
        }

        LogicTreeNode node;
        node.nameToken = nameToken;
        node.parent = parentNodeId;
        node.rowInParent = list.children.size();
        node.signalIndex = leaf ? signalIndex : -1;
        node.signalId = leaf ? signalId : -1;
        node.width = width;
        const int nodeId = nodes.size();
        nodes.push_back(node);
        list.children.push_back(nodeId);
        if (list.lookupReady) list.lookup.insert(nameToken, nodeId);
        else maybeBuildLookup(list);
        return nodeId;
    }

    void buildFromSignalDefs(const WaveSignalList& signalDefs) {
        clear();
        nodeIdBySignalIndex.resize(waveSignalCount(signalDefs));
        std::fill(nodeIdBySignalIndex.begin(), nodeIdBySignalIndex.end(), -1);

        const int signalCount = waveSignalCount(signalDefs);
        names.reserve(qMax(1024, signalCount * 2));
        signalPaths.reserve(signalCount);
        nodes.reserve(signalCount * 2);

        for (int signalIndex = 0; signalIndex < signalCount; ++signalIndex) {
            const WaveSignal& sig = signalDefs.at(signalIndex);
            const QByteArray utf8 = sig.name.toUtf8();
            PathSegViewList segViews = splitPathToSmallVec(utf8);
            if (segViews.empty()) continue;

            SignalPathEntry path;
            path.signalIndex = signalIndex;
            path.signalId = sig.signalId;
            path.width = sig.width;

            int parent = -1;
            for (int i = 0; i < segViews.size(); ++i) {
                const bool isLeaf = (i == segViews.size() - 1);
                const int nameId = names.intern(segViews[i]);
                path.nameIds.push_back(nameId);
                parent = findOrCreateChildByNameToken(parent, quint32(nameId) + 1u, isLeaf,
                                                   isLeaf ? signalIndex : -1,
                                                   sig.signalId,
                                                   sig.width);
            }

            if (parent >= 0 && signalIndex < nodeIdBySignalIndex.size()) {
                nodeIdBySignalIndex[signalIndex] = parent;
            }
            signalPaths.push_back(std::move(path));
        }
    }

    void buildFromWaveTree(const WaveTreeInfo& tree, const WaveSignalList& signalDefs) {
        clear();

        if (!tree.valid || tree.nodesById.isEmpty()) {
            buildFromSignalDefs(signalDefs);
            return;
        }
        waveNodeIdBySignalIndex = &tree.signalIndexToNodeId;
        waveTree = &tree;
        waveSignalDefs = &signalDefs;
        usesWaveNameTokens = true;
        waveNamesById = tree.namesById;
        for (int rootId : tree.rootNodeIds) {
            if (!isValidNodeId(rootId)) continue;
            roots.push_back(rootId);
            rootLookup.insert(nodeNameToken(rootId), rootId);
        }
    }

    QString fullPathForNodeId(int nodeId) const {
        if (!isValidNodeId(nodeId)) return QString();

        QVector<QString> parts;
        int cur = nodeId;
        int guard = 0;
        while (isValidNodeId(cur) && guard++ < nodeCount()) {
            const QString segment = nodeNameString(cur);
            if (!segment.isEmpty()) parts.push_back(segment);
            cur = nodeParent(cur);
        }
        std::reverse(parts.begin(), parts.end());

        QString out;
        for (int i = 0; i < parts.size(); ++i) {
            if (i > 0) out += QLatin1Char('.');
            out += parts.at(i);
        }
        return out;
    }

    QString fullPathForSignalIndex(int signalIndex) const {
        return fullPathForNodeId(nodeIdForSignalIndex(signalIndex));
    }

    QVector<int> materializedNodeChainForPath(const QString& path,
                                              bool& complete,
                                              bool& pending) const {
        complete = false;
        pending = false;
        QVector<int> chain;
        const QStringList parts = splitSearchPath(path);
        if (parts.isEmpty()) return chain;

        int current = -1;
        for (int rootNodeId : roots) {
            if (nodeNameEquals(rootNodeId, parts.first(), Qt::CaseSensitive)) {
                current = rootNodeId;
                break;
            }
        }
        if (current < 0) return chain;
        chain.push_back(current);

        for (int partIndex = 1; partIndex < parts.size(); ++partIndex) {
            int nextNode = -1;
            const LogicChildList* list = childListForNode(current);
            if (list) {
                for (int childNodeId : list->children) {
                    if (nodeNameEquals(childNodeId, parts.at(partIndex),
                                       Qt::CaseSensitive)) {
                        nextNode = childNodeId;
                        break;
                    }
                }
            }
            if (nextNode < 0) {
                pending = isPendingReference(current) || isBitsetContainer(current) ||
                          isArrayContainer(current);
                return chain;
            }
            current = nextNode;
            chain.push_back(current);
        }

        complete = true;
        return chain;
    }

    QString nodeNameString(int nodeId) const {
        if (!isValidNodeId(nodeId)) return QString();
        const quint32 nameToken = nodeNameToken(nodeId);
        if (nameToken == 0) return QString();
        if (usesWaveNameTokens) {
            if (waveNameTokenIsArrayIndex(nameToken)) {
                return QStringLiteral("[%1]").arg(waveNameTokenValue(nameToken));
            }
            const quint32 nameId = waveNameTokenValue(nameToken);
            if (nameId >= quint32(waveNamesById.size())) return QString();
            return QString::fromUtf8(waveNamesById.at(int(nameId)));
        }
        const quint32 localNameId = nameToken - 1u;
        if (localNameId >= quint32(names.strings.size())) return QString();
        return names.get(int(localNameId)).toQString();
    }

    static QStringList splitSearchPath(const QString& query) {
        QStringList parts;
        QString current;
        current.reserve(query.size());

        for (int i = 0; i < query.size(); ++i) {
            const QChar ch = query.at(i);
            if (ch == QLatin1Char('.')) {
                const QString trimmed = current.trimmed();
                if (!trimmed.isEmpty()) parts.push_back(trimmed);
                current.clear();
                continue;
            }
            current += ch;
        }

        const QString trimmed = current.trimmed();
        if (!trimmed.isEmpty()) parts.push_back(trimmed);
        return parts;
    }

    QString nodeSearchNameString(int nodeId) const {
        QString text = nodeNameString(nodeId);
        if (nodeSignalIndex(nodeId) >= 0) {
            text = formatNameWidthBeforeCompareSuffix(text, nodeWidth(nodeId));
        }
        return text;
    }

    quint64 nodeSearchPresentationKey(int nodeId) const {
        if (!isValidNodeId(nodeId)) return 0;
        const quint64 token = quint64(nodeNameToken(nodeId)) << 32;
        const quint32 width = nodeSignalIndex(nodeId) >= 0
            ? quint32(qMax(1, nodeWidth(nodeId)))
            : 0u;
        return token | width;
    }

    bool nodeSearchPresentationIsCacheable(int nodeId) const {
        return isValidNodeId(nodeId) &&
               !(usesWaveNameTokens && waveNameTokenIsArrayIndex(nodeNameToken(nodeId)));
    }

    int searchPresentationCacheReserve() const {
        const int nameCount = usesWaveNameTokens ? waveNamesById.size() : names.strings.size();
        return qMax(64, qMin(nameCount, 32768) * 2);
    }

    bool nodeNameContains(int nodeId, const QString& needle, Qt::CaseSensitivity caseSensitivity) const {
        return nodeSearchNameString(nodeId).contains(needle, caseSensitivity);
    }

    bool nodeNameEquals(int nodeId, const QString& needle, Qt::CaseSensitivity caseSensitivity) const {
        const QString displayName = nodeSearchNameString(nodeId);
        const QString bareName = nodeNameString(nodeId);
        if (QString::compare(displayName, needle, caseSensitivity) == 0 ||
            QString::compare(bareName, needle, caseSensitivity) == 0) {
            return true;
        }

        // Compare-mode leaves are named "<signal>@A" / "<signal>@B".
        // Also tolerate the older "A <signal>" / "B <signal>" spelling when
        // searching by the original structural segment.
        const QString strippedBare = stripCompareSideMarker(bareName);
        if (strippedBare != bareName) {
            const QString strippedDisplay = stripCompareSideMarker(displayName);
            return QString::compare(strippedDisplay, needle, caseSensitivity) == 0 ||
                   QString::compare(strippedBare, needle, caseSensitivity) == 0;
        }
        return false;
    }

    bool directPathMatchesFrom(int nodeId, const QStringList& parts, int partIndex, Qt::CaseSensitivity caseSensitivity) const {
        if (partIndex < 0 || partIndex >= parts.size()) return true;
        if (!nodeNameEquals(nodeId, parts.at(partIndex), caseSensitivity)) return false;
        if (partIndex == parts.size() - 1) return true;

        const QString& next = parts.at(partIndex + 1);
        if (waveTree) {
            int childSourceId = nodeId;
            int referenceGuard = 0;
            while (isValidNodeId(childSourceId) &&
                   waveTree->nodesById.at(childSourceId).firstChild == 0 &&
                   waveTree->nodesById.at(childSourceId).kind ==
                       kWaveTreeNodeKindReference &&
                   ++referenceGuard < waveTree->nodesById.size()) {
                childSourceId =
                    waveTree->nodesById.at(childSourceId).referenceTargetId;
            }
            if (!isValidNodeId(childSourceId)) return false;
            for (int childNodeId = waveTree->nodesById.at(childSourceId).firstChild;
                 childNodeId != 0;
                 childNodeId = waveTree->nodesById.at(childNodeId).nextSibling) {
                if (nodeNameEquals(childNodeId, next, caseSensitivity) &&
                    directPathMatchesFrom(childNodeId, parts, partIndex + 1,
                                          caseSensitivity)) {
                    return true;
                }
            }
            return false;
        }

        const LogicChildList* list = childListForNode(nodeId);
        if (!list) return false;
        for (int childNodeId : list->children) {
            if (nodeNameEquals(childNodeId, next, caseSensitivity) &&
                directPathMatchesFrom(childNodeId, parts, partIndex + 1,
                                      caseSensitivity)) return true;
        }
        return false;
    }

    int directPathEndNodeFrom(int nodeId, const QStringList& parts, int partIndex, Qt::CaseSensitivity caseSensitivity) const {
        if (partIndex < 0 || partIndex >= parts.size()) return -1;
        if (!nodeNameEquals(nodeId, parts.at(partIndex), caseSensitivity)) return -1;
        if (partIndex == parts.size() - 1) return nodeId;

        const QString& next = parts.at(partIndex + 1);
        if (waveTree) {
            int childSourceId = nodeId;
            int referenceGuard = 0;
            while (isValidNodeId(childSourceId) &&
                   waveTree->nodesById.at(childSourceId).firstChild == 0 &&
                   waveTree->nodesById.at(childSourceId).kind ==
                       kWaveTreeNodeKindReference &&
                   ++referenceGuard < waveTree->nodesById.size()) {
                childSourceId =
                    waveTree->nodesById.at(childSourceId).referenceTargetId;
            }
            if (!isValidNodeId(childSourceId)) return -1;
            for (int childNodeId = waveTree->nodesById.at(childSourceId).firstChild;
                 childNodeId != 0;
                 childNodeId = waveTree->nodesById.at(childNodeId).nextSibling) {
                if (!nodeNameEquals(childNodeId, next, caseSensitivity)) continue;
                const int endNode = directPathEndNodeFrom(
                    childNodeId, parts, partIndex + 1, caseSensitivity);
                if (endNode >= 0) return endNode;
            }
            return -1;
        }

        const LogicChildList* list = childListForNode(nodeId);
        if (!list) return -1;
        for (int childNodeId : list->children) {
            if (!nodeNameEquals(childNodeId, next, caseSensitivity)) continue;
            const int endNode = directPathEndNodeFrom(
                childNodeId, parts, partIndex + 1, caseSensitivity);
            if (endNode >= 0) return endNode;
        }
        return -1;
    }

    int searchPathEndNodeFrom(int nodeId, const QStringList& parts, int partIndex,
                              Qt::CaseSensitivity caseSensitivity,
                              int referenceMountNodeId = -1) const {
        if (partIndex < 0 || partIndex >= parts.size()) return -1;
        if (!nodeNameEquals(nodeId, parts.at(partIndex), caseSensitivity)) return -1;
        if (partIndex == parts.size() - 1) {
            return referenceMountNodeId >= 0 ? referenceMountNodeId : nodeId;
        }

        if (waveTree) {
            int childSourceId = nodeId;
            int resultNodeId = referenceMountNodeId;
            int referenceGuard = 0;
            while (isValidNodeId(childSourceId) &&
                   waveTree->nodesById.at(childSourceId).firstChild == 0 &&
                   waveTree->nodesById.at(childSourceId).kind ==
                       kWaveTreeNodeKindReference &&
                   ++referenceGuard < waveTree->nodesById.size()) {
                if (resultNodeId < 0) resultNodeId = childSourceId;
                childSourceId =
                    waveTree->nodesById.at(childSourceId).referenceTargetId;
            }
            if (!isValidNodeId(childSourceId)) return -1;
            int childGuard = 0;
            for (int childNodeId =
                     waveTree->nodesById.at(childSourceId).firstChild;
                 childNodeId != 0 &&
                 childGuard++ < waveTree->nodesById.size();
                 childNodeId =
                     waveTree->nodesById.at(childNodeId).nextSibling) {
                const int endNode = searchPathEndNodeFrom(
                    childNodeId, parts, partIndex + 1, caseSensitivity,
                    resultNodeId);
                if (endNode >= 0) return endNode;
            }
            return -1;
        }

        const LogicChildList* list = childListForNode(nodeId);
        if (!list) return -1;
        for (int childNodeId : list->children) {
            const int endNode = searchPathEndNodeFrom(
                childNodeId, parts, partIndex + 1, caseSensitivity,
                referenceMountNodeId);
            if (endNode >= 0) return endNode;
        }
        return -1;
    }

    QVector<quint32> exactNameTokens(const QString& name) const {
        const auto cached = exactNameTokenCache.constFind(name);
        if (cached != exactNameTokenCache.constEnd()) return cached.value();

        QVector<quint32> result;
        if (name.size() >= 3 && name.front() == QLatin1Char('[') &&
            name.back() == QLatin1Char(']')) {
            bool ok = false;
            const qulonglong index = name.mid(1, name.size() - 2).toULongLong(&ok);
            if (ok && index <= kWaveNameTokenValueMask) {
                result.push_back(waveArrayIndexToken(quint32(index)));
            }
        }

        if (result.isEmpty()) {
            const QByteArray utf8 = name.toUtf8();
            if (usesWaveNameTokens) {
                for (int nameId = 1; nameId < waveNamesById.size(); ++nameId) {
                    if (waveNamesById.at(nameId) == utf8) {
                        result.push_back(waveNameIdToken(quint32(nameId)));
                    }
                }
            } else {
                for (int nameId = 0; nameId < names.strings.size(); ++nameId) {
                    if (names.get(nameId).toQString() == name) {
                        result.push_back(quint32(nameId) + 1u);
                    }
                }
            }
        }

        exactNameTokenCache.insert(name, result);
        return result;
    }

    bool resolveExactSignalPath(const QString& path, int& signalIndex,
                                int& width, bool& ambiguous) const {
        signalIndex = -1;
        width = 1;
        ambiguous = false;
        const QStringList parts = splitSearchPath(path);
        if (parts.isEmpty()) return false;

        int matchedNode = -1;
        for (int rootNodeId : roots) {
            if (!nodeNameEquals(rootNodeId, parts.first(), Qt::CaseSensitive)) continue;
            const int endNode = directPathEndNodeFrom(
                rootNodeId, parts, 0, Qt::CaseSensitive);
            if (endNode < 0 || nodeSignalIndex(endNode) < 0) continue;
            if (matchedNode >= 0 && matchedNode != endNode) {
                ambiguous = true;
                return false;
            }
            matchedNode = endNode;
        }
        if (matchedNode >= 0 && nodeSignalIndex(matchedNode) >= 0) {
            signalIndex = nodeSignalIndex(matchedNode);
            width = nodeWidth(matchedNode);
            exactSignalPathCache.insert(path, signalIndex);
            return true;
        }

        const auto cached = exactSignalPathCache.constFind(path);
        if (cached != exactSignalPathCache.constEnd()) {
            signalIndex = cached.value();
            if (signalIndex < 0) return false;
            width = waveSignalDefs && signalIndex < waveSignalDefs->size()
                ? waveSignalDefs->at(signalIndex).width : 1;
            return true;
        }

        // A directory-only WVZ4 keeps repeated subtrees as references.  The
        // display path of an indexed leaf is still available through its
        // parent chain, even when a forward walk reaches an unmaterialized
        // reference mount.  Fall back to a compact token comparison over the
        // signal-to-node table: no QString is built, and the successful path
        // is cached for every subsequent expression.
        if (waveTree && waveNodeIdBySignalIndex) {
            QVector<QVector<quint32>> partTokens;
            partTokens.reserve(parts.size());
            for (const QString& part : parts) {
                QVector<quint32> tokens = exactNameTokens(part);
                if (tokens.isEmpty()) {
                    exactSignalPathCache.insert(path, -1);
                    return false;
                }
                partTokens.push_back(std::move(tokens));
            }

            for (int candidateSignal = 0;
                 candidateSignal < waveNodeIdBySignalIndex->size();
                 ++candidateSignal) {
                int nodeId = waveNodeIdBySignalIndex->at(candidateSignal);
                int partIndex = partTokens.size() - 1;
                while (partIndex >= 0 && isValidNodeId(nodeId)) {
                    const quint32 token = nodeNameToken(nodeId);
                    bool matches = false;
                    for (quint32 expected : partTokens.at(partIndex)) {
                        if (token == expected) {
                            matches = true;
                            break;
                        }
                    }
                    if (!matches) break;
                    --partIndex;
                    nodeId = nodeParent(nodeId);
                }
                if (partIndex >= 0 || nodeId >= 0) continue;
                signalIndex = candidateSignal;
                width = waveSignalDefs && candidateSignal < waveSignalDefs->size()
                    ? waveSignalDefs->at(candidateSignal).width : 1;
                exactSignalPathCache.insert(path, signalIndex);
                return true;
            }
        }

        exactSignalPathCache.insert(path, -1);
        return false;
    }

    bool resolveUniqueLeafSignal(const QString& leaf, int& signalIndex,
                                 int& width, bool& ambiguous) const {
        signalIndex = -1;
        width = 1;
        ambiguous = false;
        const QVector<quint32> tokens = exactNameTokens(leaf);
        if (tokens.isEmpty()) return false;

        const int count = nodeCount();
        for (int nodeId = usesDirectWaveTree() ? 1 : 0; nodeId < count; ++nodeId) {
            if (!isValidNodeId(nodeId) || nodeSignalIndex(nodeId) < 0) continue;
            const quint32 token = nodeNameToken(nodeId);
            bool tokenMatches = false;
            for (quint32 candidate : tokens) {
                if (candidate == token) {
                    tokenMatches = true;
                    break;
                }
            }
            if (!tokenMatches) continue;

            const int foundSignalIndex = nodeSignalIndex(nodeId);
            if (signalIndex >= 0 && signalIndex != foundSignalIndex) {
                ambiguous = true;
                signalIndex = -1;
                return false;
            }
            signalIndex = foundSignalIndex;
            width = nodeWidth(nodeId);
        }
        return signalIndex >= 0;
    }

    QVector<int> searchTreeQuery(const QString& query, int maxResults,
                                 Qt::CaseSensitivity caseSensitivity,
                                 bool regexMode) const {
        QVector<int> result;
        if (maxResults <= 0) return result;

        const QString trimmed = query.trimmed();
        if (trimmed.isEmpty()) return result;

        if (regexMode) {
            QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
            if (caseSensitivity == Qt::CaseInsensitive) {
                options |= QRegularExpression::CaseInsensitiveOption;
            }
            const QRegularExpression re(trimmed, options);
            if (!re.isValid()) return result;

            result.reserve(qMin(maxResults, 1024));
            QHash<quint64, bool> nameMatchByPresentation;
            nameMatchByPresentation.reserve(searchPresentationCacheReserve());
            for (int nodeId = 0; nodeId < nodeCount(); ++nodeId) {
                if (!isValidNodeId(nodeId)) continue;
                const quint64 presentationKey = nodeSearchPresentationKey(nodeId);
                const bool cacheable = nodeSearchPresentationIsCacheable(nodeId);
                auto cached = cacheable
                    ? nameMatchByPresentation.constFind(presentationKey)
                    : nameMatchByPresentation.constEnd();
                bool nameMatches = false;
                if (cached != nameMatchByPresentation.constEnd()) {
                    nameMatches = cached.value();
                } else {
                    const QString displayName = nodeSearchNameString(nodeId);
                    const QString bareName = nodeNameString(nodeId);
                    nameMatches = re.match(displayName).hasMatch() ||
                                  re.match(bareName).hasMatch();
                    if (cacheable) nameMatchByPresentation.insert(presentationKey, nameMatches);
                }

                // Full paths are substantially more expensive to assemble.
                // Avoid walking the parent chain when the node segment already
                // satisfies the expression.
                if (nameMatches || re.match(fullPathForNodeId(nodeId)).hasMatch()) {
                    result.push_back(nodeId);
                    if (result.size() >= maxResults) return result;
                }
            }
            return result;
        }

        const QStringList parts = splitSearchPath(trimmed);
        if (parts.isEmpty()) return result;

        result.reserve(qMin(maxResults, 1024));

        if (parts.size() == 1) {
            const QString& part = parts.first();
            QHash<quint64, bool> matchByPresentation;
            matchByPresentation.reserve(searchPresentationCacheReserve());

            // Single-segment search searches tree node names, not complete leaf paths.
            for (int nodeId = 0; nodeId < nodeCount(); ++nodeId) {
                if (!isValidNodeId(nodeId)) continue;
                const quint64 presentationKey = nodeSearchPresentationKey(nodeId);
                const bool cacheable = nodeSearchPresentationIsCacheable(nodeId);
                auto cached = cacheable
                    ? matchByPresentation.constFind(presentationKey)
                    : matchByPresentation.constEnd();
                bool matches = false;
                if (cached != matchByPresentation.constEnd()) {
                    matches = cached.value();
                } else {
                    matches = nodeNameContains(nodeId, part, caseSensitivity);
                    if (cacheable) matchByPresentation.insert(presentationKey, matches);
                }
                if (matches) {
                    result.push_back(nodeId);
                    if (result.size() >= maxResults) return result;
                }
            }
            return result;
        }

        QVector<QHash<quint64, bool>> exactMatchByPart(parts.size());
        QSet<int> resultNodeIds;
        auto nodeMatchesPart = [&](int nodeId, int partIndex) {
            if (partIndex < 0 || partIndex >= parts.size()) return false;
            QHash<quint64, bool>& cache = exactMatchByPart[partIndex];
            const quint64 presentationKey = nodeSearchPresentationKey(nodeId);
            const bool cacheable = nodeSearchPresentationIsCacheable(nodeId);
            auto cached = cacheable ? cache.constFind(presentationKey) : cache.constEnd();
            if (cached != cache.constEnd()) return cached.value();
            const bool matches = nodeNameEquals(nodeId, parts.at(partIndex), caseSensitivity);
            if (cacheable) cache.insert(presentationKey, matches);
            return matches;
        };
        // Multi-segment search is structural:
        // "a.b" means a node named "a" with a direct child path segment "b".
        // It can match anywhere in the tree, e.g. top.x.a.b.y will match at a.b.
        for (int nodeId = 0; nodeId < nodeCount(); ++nodeId) {
            if (!isValidNodeId(nodeId)) continue;
            if (!nodeMatchesPart(nodeId, 0)) continue;
            const int endNode = searchPathEndNodeFrom(
                nodeId, parts, 0, caseSensitivity);
            if (endNode >= 0 && !resultNodeIds.contains(endNode)) {
                resultNodeIds.insert(endNode);
                result.push_back(endNode);
                if (result.size() >= maxResults) return result;
            }
        }
        return result;
    }
};

namespace {



QByteArray encodeIntList(const QList<int>& values) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_0);
    out << values;
    return bytes;
}

QList<int> decodeIntList(const QMimeData* mimeData, const char* format) {
    QList<int> values;
    if (!mimeData || !mimeData->hasFormat(format)) return values;

    const QByteArray bytes = mimeData->data(format);
    QDataStream in(bytes);
    in.setVersion(QDataStream::Qt_5_0);
    in >> values;
    return values;
}

QString fullSignalPathFromWave(const WaveFile& wave, int signalIndex) {
    return waveSignalFullPath(wave, signalIndex).trimmed();
}

struct ViewportRawLoadBatch {
    qint64 start = 0;
    qint64 end = 0;
    QVector<int> signalIds;
    WaveFile wave;
};

struct ViewportLoadResult {
    bool ok = true;
    bool lodLoad = false;
    QString error;
    qint64 elapsedMs = 0;
    qint64 retainStart = 0;
    qint64 retainEnd = 0;
    QVector<int> requestedSignalIds;
    WaveFile lodWave;
    QVector<ViewportRawLoadBatch> rawBatches;
    QHash<int, WaveSignal> preparedRawSignals;
};

bool waveDirectorySignalPathsIndexAligned(const WaveFile& leftWave,
                                          const WaveFile& rightWave) {
    if (leftWave.signalList.size() != rightWave.signalList.size()) return false;

    const WaveTreeInfo& leftTree = leftWave.tree;
    const WaveTreeInfo& rightTree = rightWave.tree;
    if (leftTree.valid != rightTree.valid) return false;
    if (!leftTree.valid) {
        for (int i = 0; i < leftWave.signalList.size(); ++i) {
            if (leftWave.signalList.at(i).name != rightWave.signalList.at(i).name) return false;
        }
        return true;
    }

    if (leftTree.namesById != rightTree.namesById ||
        leftTree.rootNodeIds != rightTree.rootNodeIds ||
        leftTree.signalIndexToNodeId != rightTree.signalIndexToNodeId ||
        leftTree.nodesById.size() != rightTree.nodesById.size()) {
        return false;
    }
    for (int nodeId = 0; nodeId < leftTree.nodesById.size(); ++nodeId) {
        const WaveTreeNode& left = leftTree.nodesById.at(nodeId);
        const WaveTreeNode& right = rightTree.nodesById.at(nodeId);
        if (left.valid != right.valid) return false;
        if (!left.valid) continue;
        if (left.nameToken != right.nameToken ||
            left.parentId != right.parentId ||
            left.signalIndex != right.signalIndex) {
            return false;
        }
    }
    return true;
}

bool waveDirectoriesEquivalentForRawBlockCompare(const WaveFile& leftWave,
                                                  const WaveFile& rightWave,
                                                  bool signalPathsIndexAligned) {
    if (!signalPathsIndexAligned ||
        leftWave.signalList.size() != rightWave.signalList.size()) {
        return false;
    }
    if (leftWave.meta.timescale != rightWave.meta.timescale ||
        leftWave.meta.start != rightWave.meta.start ||
        leftWave.meta.end != rightWave.meta.end) {
        return false;
    }

    for (int i = 0; i < leftWave.signalList.size(); ++i) {
        const WaveSignal& left = leftWave.signalList.at(i);
        const WaveSignal& right = rightWave.signalList.at(i);
        if (left.signalId != right.signalId ||
            left.storageId != right.storageId ||
            left.bitOffset != right.bitOffset ||
            left.kind != right.kind ||
            left.width != right.width ||
            left.defaultRadix != right.defaultRadix ||
            left.proceduralClock != right.proceduralClock ||
            left.clockInitialValue != right.clockInitialValue ||
            left.clockTogglePeriodTicks != right.clockTogglePeriodTicks) {
            return false;
        }
    }
    return true;
}

struct CompareCycleRange {
    qint64 start = 0;
    qint64 end = 0;
    bool hasRange = false;
};

CompareCycleRange makeMaxCompareCycleRange(const WaveMeta& leftMeta, const WaveMeta& rightMeta) {
    CompareCycleRange range;
    range.start = qMin(leftMeta.start, rightMeta.start);
    range.end = qMax(leftMeta.end, rightMeta.end);
    range.hasRange = range.end > range.start;
    if (!range.hasRange) {
        range.end = range.start;
    }
    return range;
}

CompareCycleRange makeOverlappedCompareCycleRange(const WaveMeta& leftMeta, const WaveMeta& rightMeta) {
    CompareCycleRange range;
    range.start = qMax(leftMeta.start, rightMeta.start);
    range.end = qMin(leftMeta.end, rightMeta.end);
    range.hasRange = range.end > range.start;
    if (!range.hasRange) {
        range.end = range.start;
    }
    return range;
}

bool compareFileRangesDiffer(const WaveMeta& leftMeta, const WaveMeta& rightMeta) {
    return leftMeta.start != rightMeta.start || leftMeta.end != rightMeta.end;
}

QString formatCycleRange(qint64 start, qint64 end) {
    return QStringLiteral("%1~%2").arg(start).arg(end);
}

QString formatCompareRangeNote(const WaveMeta& leftMeta, const WaveMeta& rightMeta) {
    const CompareCycleRange range = makeMaxCompareCycleRange(leftMeta, rightMeta);
    const CompareCycleRange overlap = makeOverlappedCompareCycleRange(leftMeta, rightMeta);
    QString note = QStringLiteral("Compared max cycle range %1. A range=%2, B range=%3.")
        .arg(range.hasRange
             ? formatCycleRange(range.start, range.end)
             : QStringLiteral("<empty>"))
        .arg(formatCycleRange(leftMeta.start, leftMeta.end))
        .arg(formatCycleRange(rightMeta.start, rightMeta.end));
    note += QStringLiteral(" Signal values are diffed only where both files have time coverage: %1.")
        .arg(overlap.hasRange ? formatCycleRange(overlap.start, overlap.end) : QStringLiteral("<empty>"));
    if (compareFileRangesDiffer(leftMeta, rightMeta)) {
        note += QStringLiteral(" File cycle ranges differ; matching signals are compared across the max range without inserting one-sided-only signals.");
    }
    return note;
}

QString formatNoSignalDiffMessage(const WaveMeta& leftMeta, const WaveMeta& rightMeta) {
    QString msg = QStringLiteral("No matching-path signal differs in the overlapped cycle range.");
    if (!makeOverlappedCompareCycleRange(leftMeta, rightMeta).hasRange) {
        msg += QStringLiteral(" No overlapped cycle range to compare.");
    }
    if (compareFileRangesDiffer(leftMeta, rightMeta)) {
        msg += QStringLiteral("\n") + formatCompareRangeNote(leftMeta, rightMeta);
    }
    return msg;
}

void initializeCompareMeta(WaveMeta& meta,
                           const QString& leftPath,
                           const WaveMeta& leftMeta,
                           const QString& rightPath,
                           const WaveMeta& rightMeta) {
    meta.hasCompareSources = true;
    meta.compareLeftPath = leftPath;
    meta.compareLeftLabel = QFileInfo(leftPath).fileName();
    meta.compareLeftStart = leftMeta.start;
    meta.compareLeftEnd = leftMeta.end;
    meta.compareRightPath = rightPath;
    meta.compareRightLabel = QFileInfo(rightPath).fileName();
    meta.compareRightStart = rightMeta.start;
    meta.compareRightEnd = rightMeta.end;
}

WaveSample makeAbsentCompareSample() {
    WaveSample s;
    s.time = 0;
    s.value = waveAbsentValue();
    s.isAbsent = true;
    s.isZ = false;
    s.rawBits = 0;
    s.rawFieldsReady = true;
    return s;
}

bool compareValueEquivalent(const WaveSample& a, const WaveSample& b) {
    return waveSamplesEquivalent(a, b);
}

bool rawSamplePairComparable(const WaveSample& left, const WaveSample& right) {
    return left.rawFieldsReady && right.rawFieldsReady &&
           !left.isAbsent && !right.isAbsent &&
           !left.isZ && !right.isZ;
}

bool rawSampleBlockComparable(const WaveSample* left, const WaveSample* right, int count) {
    for (int i = 0; i < count; ++i) {
        if (!rawSamplePairComparable(left[i], right[i])) return false;
    }
    return true;
}

bool rawSampleStreamsComparable(const WaveSample* left, const WaveSample* right, int count) {
    for (int i = 0; i < count; ++i) {
        if (!rawSamplePairComparable(left[i], right[i])) return false;
    }
    return true;
}

bool compareRawSampleTimeAndBitsScalarBlock(const WaveSample* left, const WaveSample* right, int count) {
    for (int i = 0; i < count; ++i) {
        if (left[i].time != right[i].time || left[i].rawBits != right[i].rawBits) return false;
    }
    return true;
}

#if WAVETRACE_COMPARE_HAS_AVX512F
bool compareRawSampleTimeAndBitsAvx512(const WaveSample* left, const WaveSample* right) {
    if ((sizeof(WaveSample) % sizeof(qint64)) != 0 ||
        sizeof(qint64) != sizeof(long long) ||
        sizeof(quint64) != sizeof(long long)) {
        return compareRawSampleTimeAndBitsScalarBlock(left, right, 8);
    }

    const long long strideWords = static_cast<long long>(sizeof(WaveSample) / sizeof(qint64));
    const __m512i indexes = _mm512_set_epi64(7 * strideWords, 6 * strideWords,
                                            5 * strideWords, 4 * strideWords,
                                            3 * strideWords, 2 * strideWords,
                                            strideWords, 0);
    const __m512i leftTimes = _mm512_i64gather_epi64(indexes, reinterpret_cast<const void*>(&left[0].time), 8);
    const __m512i rightTimes = _mm512_i64gather_epi64(indexes, reinterpret_cast<const void*>(&right[0].time), 8);
    const __m512i leftRaw = _mm512_i64gather_epi64(indexes, reinterpret_cast<const void*>(&left[0].rawBits), 8);
    const __m512i rightRaw = _mm512_i64gather_epi64(indexes, reinterpret_cast<const void*>(&right[0].rawBits), 8);
    const __mmask8 timeMask = _mm512_cmpeq_epi64_mask(leftTimes, rightTimes);
    const __mmask8 rawMask = _mm512_cmpeq_epi64_mask(leftRaw, rightRaw);
    return (timeMask & rawMask) == 0xffu;
}
#endif

#if WAVETRACE_COMPARE_HAS_AVX2
bool compareRawSampleTimeAndBitsAvx2(const WaveSample* left, const WaveSample* right) {
    if ((sizeof(WaveSample) % sizeof(qint64)) != 0 ||
        sizeof(qint64) != sizeof(long long) ||
        sizeof(quint64) != sizeof(long long)) {
        return compareRawSampleTimeAndBitsScalarBlock(left, right, 4);
    }

    const long long strideWords = static_cast<long long>(sizeof(WaveSample) / sizeof(qint64));
    const __m256i indexes = _mm256_set_epi64x(3 * strideWords, 2 * strideWords, strideWords, 0);
    const __m256i leftTimes = _mm256_i64gather_epi64(reinterpret_cast<const long long*>(&left[0].time), indexes, 8);
    const __m256i rightTimes = _mm256_i64gather_epi64(reinterpret_cast<const long long*>(&right[0].time), indexes, 8);
    const __m256i leftRaw = _mm256_i64gather_epi64(reinterpret_cast<const long long*>(&left[0].rawBits), indexes, 8);
    const __m256i rightRaw = _mm256_i64gather_epi64(reinterpret_cast<const long long*>(&right[0].rawBits), indexes, 8);
    const __m256i timeEq = _mm256_cmpeq_epi64(leftTimes, rightTimes);
    const __m256i rawEq = _mm256_cmpeq_epi64(leftRaw, rightRaw);
    const __m256i eq = _mm256_and_si256(timeEq, rawEq);
    return _mm256_movemask_pd(_mm256_castsi256_pd(eq)) == 0x0f;
}
#endif

bool compareRawSampleTimeAndBitsSimd(qint64 leftTime,
                                     quint64 leftRaw,
                                     qint64 rightTime,
                                     quint64 rightRaw) {
#if WAVETRACE_COMPARE_HAS_SSE2
    const __m128i left = _mm_set_epi64x(static_cast<long long>(leftRaw),
                                       static_cast<long long>(leftTime));
    const __m128i right = _mm_set_epi64x(static_cast<long long>(rightRaw),
                                        static_cast<long long>(rightTime));
    const __m128i eq = _mm_cmpeq_epi8(left, right);
    return _mm_movemask_epi8(eq) == 0xffff;
#else
    return leftTime == rightTime && leftRaw == rightRaw;
#endif
}

bool compareRawSampleTimeAndBitsSimdBlock(const WaveSample* left, const WaveSample* right, int count) {
    int i = 0;
#if WAVETRACE_COMPARE_HAS_AVX512F
    for (; i + 8 <= count; i += 8) {
        if (!compareRawSampleTimeAndBitsAvx512(left + i, right + i)) return false;
    }
#endif
#if WAVETRACE_COMPARE_HAS_AVX2
    for (; i + 4 <= count; i += 4) {
        if (!compareRawSampleTimeAndBitsAvx2(left + i, right + i)) return false;
    }
#endif
    for (; i < count; ++i) {
        if (!compareRawSampleTimeAndBitsSimd(left[i].time, left[i].rawBits,
                                            right[i].time, right[i].rawBits)) {
            return false;
        }
    }
    return true;
}

bool compareSamplesExactlyEquivalentFast(const WaveSample& left, const WaveSample& right) {
    if (left.isAbsent != right.isAbsent || left.isZ != right.isZ) return false;
    if (rawSamplePairComparable(left, right)) {
        return compareRawSampleTimeAndBitsSimd(left.time, left.rawBits, right.time, right.rawBits);
    }
    if (left.time != right.time) return false;
    if (left.rawFieldsReady && right.rawFieldsReady) return left.rawBits == right.rawBits;
    if (!left.rawFieldsReady && !right.rawFieldsReady) return left.value == right.value;
    return false;
}

bool compareSampleStreamsExactlyEquivalentFast(const WaveSignal& left,
                                               const WaveSignal& right,
                                               qint64 leftEnd,
                                               qint64 rightEnd) {
    if (left.kind != right.kind || left.width != right.width || leftEnd != rightEnd) return false;
    if (left.proceduralClock || right.proceduralClock) {
        return left.proceduralClock && right.proceduralClock &&
               left.clockInitialValue == right.clockInitialValue &&
               left.clockTogglePeriodTicks == right.clockTogglePeriodTicks;
    }
    const int count = left.samples.size();
    if (count != right.samples.size()) return false;
    const WaveSample* leftSamples = left.samples.constData();
    const WaveSample* rightSamples = right.samples.constData();
    if (rawSampleStreamsComparable(leftSamples, rightSamples, count)) {
        return compareRawSampleTimeAndBitsSimdBlock(leftSamples, rightSamples, count);
    }

    int i = 0;
    while (i < count) {
#if WAVETRACE_COMPARE_HAS_AVX512F
        if (i + 8 <= count && rawSampleBlockComparable(leftSamples + i, rightSamples + i, 8)) {
            if (!compareRawSampleTimeAndBitsAvx512(leftSamples + i, rightSamples + i)) return false;
            i += 8;
            continue;
        }
#endif
#if WAVETRACE_COMPARE_HAS_AVX2
        if (i + 4 <= count && rawSampleBlockComparable(leftSamples + i, rightSamples + i, 4)) {
            if (!compareRawSampleTimeAndBitsAvx2(leftSamples + i, rightSamples + i)) return false;
            i += 4;
            continue;
        }
#endif
        if (!compareSamplesExactlyEquivalentFast(leftSamples[i], rightSamples[i])) return false;
        ++i;
    }
    return true;
}

enum class CompareSignalMode {
    EventTimesAndValues,
    EventTimesOnly
};

void appendCompareDiffRegion(QVector<WaveDiffRegion>& regions, qint64 start, qint64 end, qint64 clipStart, qint64 clipEnd) {
    start = qMax(start, clipStart);
    end = qMin(end, clipEnd);
    if (end <= start) return;

    if (!regions.isEmpty() && regions.last().end >= start) {
        regions.last().end = qMax(regions.last().end, end);
        return;
    }

    WaveDiffRegion r;
    r.start = start;
    r.end = end;
    regions.push_back(r);
}

QVector<WaveDiffRegion> computeSignalDiffRegions(const WaveSignal& left,
                                                  const WaveSignal& right,
                                                  qint64 compareStart,
                                                  qint64 compareEnd,
                                                  qint64 leftEnd,
                                                  qint64 rightEnd) {
    QVector<WaveDiffRegion> regions;
    if (compareEnd <= compareStart) compareEnd = compareStart + 1;

    if (left.kind != right.kind || left.width != right.width) {
        appendCompareDiffRegion(regions, compareStart, compareEnd, compareStart, compareEnd);
        return regions;
    }
    if (left.proceduralClock != right.proceduralClock) {
        // A formula-backed clock and an ordinary sample stream have different
        // representations. Do not let two empty sample vectors hide the change.
        appendCompareDiffRegion(regions, compareStart, compareEnd,
                                compareStart, compareEnd);
        return regions;
    }
    if (left.proceduralClock && right.proceduralClock) {
        if (left.clockInitialValue == right.clockInitialValue &&
            left.clockTogglePeriodTicks == right.clockTogglePeriodTicks) {
            return regions;
        }
        if (left.clockTogglePeriodTicks == right.clockTogglePeriodTicks) {
            appendCompareDiffRegion(regions, compareStart, compareEnd,
                                    compareStart, compareEnd);
            return regions;
        }

        qint64 intervalStart = compareStart;
        qint64 nextLeft =
            waveProceduralClockNextTransition(left, compareStart);
        qint64 nextRight =
            waveProceduralClockNextTransition(right, compareStart);
        while (intervalStart < compareEnd) {
            qint64 intervalEnd = compareEnd;
            if (nextLeft > intervalStart) intervalEnd = qMin(intervalEnd, nextLeft);
            if (nextRight > intervalStart) intervalEnd = qMin(intervalEnd, nextRight);
            if (waveProceduralClockValueAtTime(left, intervalStart) !=
                waveProceduralClockValueAtTime(right, intervalStart)) {
                appendCompareDiffRegion(regions, intervalStart, intervalEnd,
                                        compareStart, compareEnd);
            }
            if (intervalEnd >= compareEnd) break;
            intervalStart = intervalEnd;
            if (nextLeft == intervalEnd) {
                nextLeft =
                    waveProceduralClockNextTransition(left, intervalEnd);
            }
            if (nextRight == intervalEnd) {
                nextRight =
                    waveProceduralClockNextTransition(right, intervalEnd);
            }
        }
        return regions;
    }
    if (compareSampleStreamsExactlyEquivalentFast(left, right, leftEnd, rightEnd)) {
        return regions;
    }

    const WaveSample absent = makeAbsentCompareSample();
    const WaveSample* curLeft = &absent;
    const WaveSample* curRight = &absent;
    int i = 0;
    int j = 0;
    bool leftEnded = (leftEnd <= compareStart);
    bool rightEnded = (rightEnd <= compareStart);

    const WaveSample* leftSamples = left.samples.constData();
    const WaveSample* rightSamples = right.samples.constData();
    const int leftCount = left.samples.size();
    const int rightCount = right.samples.size();

    while (i < leftCount || j < rightCount || !leftEnded || !rightEnded) {
        qint64 t = std::numeric_limits<qint64>::max();
        if (!leftEnded) t = qMin(t, leftEnd);
        if (!rightEnded) t = qMin(t, rightEnd);
        while (i < leftCount && leftEnded && leftSamples[i].time >= leftEnd) ++i;
        while (j < rightCount && rightEnded && rightSamples[j].time >= rightEnd) ++j;
        if (i < leftCount && (!leftEnded || leftSamples[i].time < leftEnd)) {
            t = qMin(t, leftSamples[i].time);
        }
        if (j < rightCount && (!rightEnded || rightSamples[j].time < rightEnd)) {
            t = qMin(t, rightSamples[j].time);
        }
        if (t == std::numeric_limits<qint64>::max()) break;

        if (!leftEnded && t >= leftEnd) {
            curLeft = &absent;
            leftEnded = true;
            while (i < leftCount && leftSamples[i].time <= t) ++i;
        } else {
            while (i < leftCount && leftSamples[i].time == t) {
                curLeft = &leftSamples[i++];
            }
        }

        if (!rightEnded && t >= rightEnd) {
            curRight = &absent;
            rightEnded = true;
            while (j < rightCount && rightSamples[j].time <= t) ++j;
        } else {
            while (j < rightCount && rightSamples[j].time == t) {
                curRight = &rightSamples[j++];
            }
        }

        qint64 nextT = compareEnd;
        if (!leftEnded) nextT = qMin(nextT, leftEnd);
        if (!rightEnded) nextT = qMin(nextT, rightEnd);
        if (i < leftCount && (!leftEnded || leftSamples[i].time < leftEnd)) {
            nextT = qMin(nextT, leftSamples[i].time);
        }
        if (j < rightCount && (!rightEnded || rightSamples[j].time < rightEnd)) {
            nextT = qMin(nextT, rightSamples[j].time);
        }
        if (nextT <= t) nextT = qMin(compareEnd, t + 1);

        if (!compareValueEquivalent(*curLeft, *curRight)) {
            appendCompareDiffRegion(regions, t, nextT, compareStart, compareEnd);
        }

        if (t >= compareEnd) break;
    }

    return regions;
}

QVector<WaveDiffRegion> computeSignalEventTimeDiffRegions(
    const WaveSignal& left,
    const WaveSignal& right,
    qint64 compareStart,
    qint64 compareEnd) {
    QVector<WaveDiffRegion> regions;
    if (compareEnd <= compareStart) return regions;

    struct EventCursor {
        const WaveSignal* signal = nullptr;
        int sampleIndex = 0;
        qint64 time = (std::numeric_limits<qint64>::max)();
        qint64 end = 0;

        void initialize(const WaveSignal& source, qint64 start, qint64 finish) {
            signal = &source;
            end = finish;
            if (source.proceduralClock) {
                time = waveProceduralClockTransitionAtOrAfter(source, start);
                if (time < start || time >= end) {
                    time = (std::numeric_limits<qint64>::max)();
                }
                return;
            }
            while (sampleIndex < source.samples.size() &&
                   source.samples.at(sampleIndex).time < start) {
                ++sampleIndex;
            }
            refreshSampleTime();
        }

        void refreshSampleTime() {
            if (!signal || sampleIndex >= signal->samples.size() ||
                signal->samples.at(sampleIndex).time >= end) {
                time = (std::numeric_limits<qint64>::max)();
                return;
            }
            time = signal->samples.at(sampleIndex).time;
        }

        void advance() {
            if (!signal || time == (std::numeric_limits<qint64>::max)()) return;
            if (signal->proceduralClock) {
                time = waveProceduralClockNextTransition(*signal, time);
                if (time < 0 || time >= end) {
                    time = (std::numeric_limits<qint64>::max)();
                }
                return;
            }
            ++sampleIndex;
            refreshSampleTime();
        }
    };

    EventCursor leftEvents;
    EventCursor rightEvents;
    leftEvents.initialize(left, compareStart, compareEnd);
    rightEvents.initialize(right, compareStart, compareEnd);
    const qint64 noEvent = (std::numeric_limits<qint64>::max)();
    while (leftEvents.time != noEvent || rightEvents.time != noEvent) {
        if (leftEvents.time == rightEvents.time) {
            leftEvents.advance();
            rightEvents.advance();
            continue;
        }

        const qint64 differenceTime = qMin(leftEvents.time, rightEvents.time);
        const qint64 differenceEnd =
            qMin(compareEnd, differenceTime + 1);
        appendCompareDiffRegion(regions, differenceTime, differenceEnd,
                                compareStart, compareEnd);
        if (leftEvents.time < rightEvents.time) {
            leftEvents.advance();
        } else {
            rightEvents.advance();
        }
    }
    return regions;
}

void hydrateSignalSamplesForCompare(WaveSignal& sig) {
    rebuildWaveSignalDerivedCaches(sig);
}

QString comparedSideFullPath(const QString& matchedPath, const QString& sidePrefix) {
    const int dot = matchedPath.lastIndexOf(QLatin1Char('.'));
    if (dot < 0) return matchedPath + QLatin1Char('@') + sidePrefix;
    const QString parent = matchedPath.left(dot);
    const QString leaf = matchedPath.mid(dot + 1);
    return parent + QLatin1Char('.') + leaf + QLatin1Char('@') + sidePrefix;
}

WaveSignal makeComparedSideSignal(const QString& fullName,
                                  const WaveSignal& src,
                                  int signalId,
                                  const QVector<WaveDiffRegion>& diffRegions,
                                  qint64 visibleStart,
                                  qint64 visibleEnd) {
    WaveSignal out = src;
    out.signalId = signalId;
    out.name = fullName;
    out.samplesLoaded = true;
    out.diffRegions = diffRegions;
    out.hasVisibleRange = true;
    out.visibleStart = visibleStart;
    out.visibleEnd = visibleEnd;
    hydrateSignalSamplesForCompare(out);
    return out;
}

QVector<WaveDiffRegion> makeFullCompareDiffRegions(qint64 start, qint64 end) {
    QVector<WaveDiffRegion> regions;
    appendCompareDiffRegion(regions, start, qMax(end, start + 1), start, qMax(end, start + 1));
    return regions;
}

WaveSignal makeAbsentComparedSideSignal(const QString& fullName,
                                         const WaveSignal& reference,
                                         int signalId,
                                         qint64 time,
                                         const QVector<WaveDiffRegion>& diffRegions) {
    WaveSignal out;
    out.signalId = signalId;
    out.storageId = -1;
    out.bitOffset = reference.bitOffset;
    out.name = fullName;
    out.kind = reference.kind;
    out.width = reference.width;
    out.defaultRadix = reference.defaultRadix;
    out.currentRadix = reference.currentRadix;
    out.supportsZState = reference.supportsZState;
    out.samplesLoaded = true;
    out.diffRegions = diffRegions;

    WaveSample absent = makeAbsentCompareSample();
    absent.time = time;
    out.samples.push_back(absent);
    hydrateSignalSamplesForCompare(out);
    return out;
}

struct CompareSignalJob {
    int leftIndex = -1;
    int rightIndex = -1;
};

struct ComparePathDigest {
    quint64 first = 1469598103934665603ull;
    quint64 second = 1099511628211ull;
    quint64 byteCount = 0;

    bool operator==(const ComparePathDigest& other) const {
        return first == other.first &&
               second == other.second &&
               byteCount == other.byteCount;
    }
};

uint qHash(const ComparePathDigest& digest, uint seed = 0) {
    const quint64 mixed =
        digest.first ^
        ((digest.second << 23) | (digest.second >> 41)) ^
        (digest.byteCount * 0x9e3779b97f4a7c15ull);
    return ::qHash(mixed, seed);
}

void appendComparePathDigestByte(ComparePathDigest& digest, quint8 byte) {
    digest.first ^= quint64(byte);
    digest.first *= 1099511628211ull;

    digest.second ^= quint64(byte) + 0x9e3779b97f4a7c15ull +
                     (digest.second << 6) + (digest.second >> 2);
    digest.second *= 0xbf58476d1ce4e5b9ull;
    ++digest.byteCount;
}

void appendComparePathDigestBytes(ComparePathDigest& digest,
                                  const char* data,
                                  int size) {
    for (int i = 0; i < size; ++i) {
        appendComparePathDigestByte(digest, quint8(uchar(data[i])));
    }
}

bool appendCompareTreeSegmentDigest(ComparePathDigest& digest,
                                    const WaveTreeInfo& tree,
                                    quint32 nameToken) {
    if (waveNameTokenIsArrayIndex(nameToken)) {
        appendComparePathDigestByte(digest, quint8('['));
        quint32 value = waveNameTokenValue(nameToken);
        char reversedDigits[10];
        int digitCount = 0;
        do {
            reversedDigits[digitCount++] = char('0' + (value % 10u));
            value /= 10u;
        } while (value != 0 && digitCount < int(sizeof(reversedDigits)));
        for (int i = digitCount - 1; i >= 0; --i) {
            appendComparePathDigestByte(digest, quint8(reversedDigits[i]));
        }
        appendComparePathDigestByte(digest, quint8(']'));
        return true;
    }

    const quint32 nameId = waveNameTokenValue(nameToken);
    if (nameId == 0 || nameId >= quint32(tree.namesById.size())) return false;
    const QByteArray& name = tree.namesById.at(int(nameId));
    if (name.isEmpty()) return false;
    appendComparePathDigestBytes(digest, name.constData(), name.size());
    return true;
}

template <typename Visitor>
void forEachCompareSignalPathDigest(const WaveFile& wave, Visitor&& visitor) {
    const WaveTreeInfo& tree = wave.tree;
    if (!tree.valid) {
        for (int signalIndex = 0; signalIndex < wave.signalList.size(); ++signalIndex) {
            const QByteArray path = wave.signalList.at(signalIndex).name.trimmed().toUtf8();
            if (path.isEmpty()) continue;
            ComparePathDigest digest;
            appendComparePathDigestBytes(digest, path.constData(), path.size());
            visitor(signalIndex, digest);
        }
        return;
    }

    struct PendingDigestNode {
        int nodeId = 0;
        ComparePathDigest parentDigest;
        bool followSibling = false;
    };

    QVector<PendingDigestNode> pending;
    pending.reserve(64);
    for (int rootNodeId : tree.rootNodeIds) {
        pending.clear();
        pending.push_back(PendingDigestNode{rootNodeId, ComparePathDigest(), false});
        while (!pending.isEmpty()) {
            const PendingDigestNode current = pending.takeLast();
            if (current.nodeId <= 0 || current.nodeId >= tree.nodesById.size()) continue;
            const WaveTreeNode& node = tree.nodesById.at(current.nodeId);
            if (!node.valid) continue;

            if (current.followSibling && node.nextSibling > 0) {
                pending.push_back(PendingDigestNode{
                    node.nextSibling, current.parentDigest, true});
            }

            ComparePathDigest nodeDigest = current.parentDigest;
            ComparePathDigest segmentDigest = nodeDigest;
            if (nodeDigest.byteCount > 0) {
                appendComparePathDigestByte(segmentDigest, quint8('.'));
            }
            if (appendCompareTreeSegmentDigest(segmentDigest, tree, node.nameToken)) {
                nodeDigest = segmentDigest;
            }

            if (node.signalIndex >= 0 &&
                node.signalIndex < wave.signalList.size() &&
                nodeDigest.byteCount > 0) {
                visitor(node.signalIndex, nodeDigest);
            }
            if (node.firstChild > 0) {
                pending.push_back(PendingDigestNode{
                    node.firstChild, nodeDigest, true});
            }
        }
    }
}

void sortCompareJobsByLeftIndex(QVector<CompareSignalJob>& jobs) {
    const auto lessByLeftIndex =
        [](const CompareSignalJob& a, const CompareSignalJob& b) {
            return a.leftIndex < b.leftIndex;
        };
    if (!std::is_sorted(jobs.cbegin(), jobs.cend(), lessByLeftIndex)) {
        std::sort(jobs.begin(), jobs.end(), lessByLeftIndex);
    }
}

quint64 compareTreeChildKey(int parentNodeId, quint32 canonicalNameToken) {
    return (quint64(quint32(parentNodeId)) << 32) | quint64(canonicalNameToken);
}

bool canonicalCompareTreeNameToken(const WaveTreeInfo& tree,
                                   quint32 nameToken,
                                   const QHash<QByteArray, quint32>& canonicalNameByText,
                                   quint32& canonicalToken) {
    if (waveNameTokenIsArrayIndex(nameToken)) {
        canonicalToken = nameToken;
        return true;
    }

    const quint32 nameId = waveNameTokenValue(nameToken);
    if (nameId == 0 || nameId >= quint32(tree.namesById.size())) return false;
    const auto it = canonicalNameByText.constFind(tree.namesById.at(int(nameId)));
    if (it == canonicalNameByText.constEnd()) return false;
    canonicalToken = it.value();
    return true;
}

void buildTreeMatchedCompareJobs(const WaveFile& leftWave,
                                 const WaveFile& rightWave,
                                 QVector<CompareSignalJob>& jobs) {
    const WaveTreeInfo& leftTree = leftWave.tree;
    const WaveTreeInfo& rightTree = rightWave.tree;
    if (!leftTree.valid || !rightTree.valid) return;

    const bool indexRightTree =
        rightTree.nodesById.size() <= leftTree.nodesById.size();
    const WaveFile& indexedWave = indexRightTree ? rightWave : leftWave;
    const WaveFile& probeWave = indexRightTree ? leftWave : rightWave;
    const WaveTreeInfo& indexedTree = indexedWave.tree;
    const WaveTreeInfo& probeTree = probeWave.tree;

    // Canonicalize only segment names, not full paths. QByteArray is implicitly
    // shared, so this table does not duplicate the underlying NAME payload.
    QHash<QByteArray, quint32> canonicalNameByText;
    canonicalNameByText.reserve(indexedTree.namesById.size());
    quint32 nextCanonicalName = 1;
    for (int nameId = 1; nameId < indexedTree.namesById.size(); ++nameId) {
        const QByteArray& name = indexedTree.namesById.at(nameId);
        if (name.isEmpty() || canonicalNameByText.contains(name)) continue;
        if (nextCanonicalName >= kWaveNameTokenArrayFlag) break;
        canonicalNameByText.insert(name, nextCanonicalName++);
    }

    // A node is identified exactly by (matched parent path, segment token).
    // This keeps matching O(nodes) even when sibling order or whole branches
    // differ, without retaining one QString for every signal path.
    QHash<quint64, int> rightChildByParentAndToken;
    rightChildByParentAndToken.reserve(qMax(0, indexedTree.nodesById.size() - 1));
    for (int nodeId = 1; nodeId < indexedTree.nodesById.size(); ++nodeId) {
        const WaveTreeNode& node = indexedTree.nodesById.at(nodeId);
        if (!node.valid || node.parentId < 0) continue;
        quint32 canonicalToken = 0;
        if (!canonicalCompareTreeNameToken(
                indexedTree, node.nameToken, canonicalNameByText, canonicalToken)) {
            continue;
        }
        const quint64 key = compareTreeChildKey(node.parentId, canonicalToken);
        if (!rightChildByParentAndToken.contains(key)) {
            rightChildByParentAndToken.insert(key, nodeId);
        }
    }

    struct PendingNode {
        int leftNodeId = 0;
        int rightParentId = 0;
        bool followSibling = false;
    };

    QVector<PendingNode> pending;
    pending.reserve(64);
    jobs.reserve(qMin(leftWave.signalList.size(), rightWave.signalList.size()));
    for (int probeRootId : probeTree.rootNodeIds) {
        pending.clear();
        pending.push_back(PendingNode{probeRootId, 0, false});
        while (!pending.isEmpty()) {
            const PendingNode current = pending.takeLast();
            if (current.leftNodeId <= 0 ||
                current.leftNodeId >= probeTree.nodesById.size()) {
                continue;
            }

            const WaveTreeNode& probeNode = probeTree.nodesById.at(current.leftNodeId);
            if (!probeNode.valid) continue;
            if (current.followSibling && probeNode.nextSibling > 0) {
                pending.push_back(
                    PendingNode{probeNode.nextSibling, current.rightParentId, true});
            }

            quint32 canonicalToken = 0;
            if (!canonicalCompareTreeNameToken(
                    probeTree, probeNode.nameToken,
                    canonicalNameByText, canonicalToken)) {
                continue;
            }
            const quint64 key =
                compareTreeChildKey(current.rightParentId, canonicalToken);
            auto rightIt = rightChildByParentAndToken.find(key);
            if (rightIt == rightChildByParentAndToken.end()) continue;

            const int rightNodeId = rightIt.value();
            if (rightNodeId <= 0 || rightNodeId >= indexedTree.nodesById.size()) continue;
            const WaveTreeNode& indexedNode = indexedTree.nodesById.at(rightNodeId);
            if (!indexedNode.valid) continue;

            if (probeNode.signalIndex >= 0 && indexedNode.signalIndex >= 0) {
                CompareSignalJob job;
                job.leftIndex = indexRightTree
                    ? probeNode.signalIndex
                    : indexedNode.signalIndex;
                job.rightIndex = indexRightTree
                    ? indexedNode.signalIndex
                    : probeNode.signalIndex;
                jobs.push_back(job);
                // Preserve the old first-path-wins behavior for duplicate leaf
                // paths without a second full-size "seen left paths" table.
                rightChildByParentAndToken.erase(rightIt);
            }

            if (probeNode.firstChild > 0) {
                pending.push_back(PendingNode{probeNode.firstChild, rightNodeId, true});
            }
        }
    }

    sortCompareJobsByLeftIndex(jobs);
}

void buildDigestMatchedCompareJobs(const WaveFile& leftWave,
                                   const WaveFile& rightWave,
                                   QVector<CompareSignalJob>& jobs) {
    const bool indexRightWave =
        rightWave.signalList.size() <= leftWave.signalList.size();
    const WaveFile& indexedWave = indexRightWave ? rightWave : leftWave;
    const WaveFile& probeWave = indexRightWave ? leftWave : rightWave;

    QHash<ComparePathDigest, int> indexedSignalByDigest;
    indexedSignalByDigest.reserve(waveSignalCount(indexedWave.signalList));
    forEachCompareSignalPathDigest(
        indexedWave,
        [&](int signalIndex, const ComparePathDigest& digest) {
            if (!indexedSignalByDigest.contains(digest)) {
                indexedSignalByDigest.insert(digest, signalIndex);
            }
        });

    jobs.reserve(qMin(leftWave.signalList.size(), rightWave.signalList.size()));
    forEachCompareSignalPathDigest(
        probeWave,
        [&](int signalIndex, const ComparePathDigest& digest) {
            auto indexedIt = indexedSignalByDigest.find(digest);
            if (indexedIt == indexedSignalByDigest.end()) return;
            CompareSignalJob job;
            job.leftIndex = indexRightWave ? signalIndex : indexedIt.value();
            job.rightIndex = indexRightWave ? indexedIt.value() : signalIndex;
            jobs.push_back(job);
            indexedSignalByDigest.erase(indexedIt);
        });
    sortCompareJobsByLeftIndex(jobs);
}

struct CompareProgressState {
    std::atomic<int> totalJobs{0};
    std::atomic<int> completedJobs{0};
    std::atomic<int> outputSignalPairs{0};
    // 0 = expected-same baseline scan, 1 = formal comparison.
    std::atomic<int> stage{1};
    std::atomic<bool> cancelRequested{false};
};

struct CompareBuildOptions {
    CompareSignalMode signalMode = CompareSignalMode::EventTimesAndValues;
    const QSet<int>* excludedLeftSignalIndexes = nullptr;
    QSet<int>* differingLeftSignalIndexes = nullptr;
    bool emitComparedWave = true;
};

bool isDecodedSampleBudgetError(const QString& error) {
    return error.contains(QStringLiteral("decoded sample limit exceeded"), Qt::CaseInsensitive);
}

bool loadWvz4SignalBatchIntoMapForCompare(const WaveParser4Reader& reader,
                                           const QVector<int>& signalIds,
                                           QHash<int, WaveSignal>& signalsById,
                                           QString& error,
                                           quint64 sampleBudget,
                                           qint64 timeStart,
                                           qint64 timeEnd) {
    if (signalIds.isEmpty()) return true;

    WaveFile loadedWave;
    QString loadError;
    if (reader.loadSignals(signalIds, loadedWave, loadError, sampleBudget,
                           timeStart, timeEnd)) {
        for (WaveSignal& sig : loadedWave.signalList) {
            signalsById.insert(sig.signalId, std::move(sig));
        }
        return true;
    }

    if (sampleBudget > 0 && isDecodedSampleBudgetError(loadError)) {
        if (signalIds.size() > 1) {
            const int mid = signalIds.size() / 2;
            QVector<int> leftIds;
            QVector<int> rightIds;
            leftIds.reserve(mid);
            rightIds.reserve(signalIds.size() - mid);
            for (int i = 0; i < signalIds.size(); ++i) {
                if (i < mid) leftIds.push_back(signalIds.at(i));
                else rightIds.push_back(signalIds.at(i));
            }
            if (!loadWvz4SignalBatchIntoMapForCompare(
                    reader, leftIds, signalsById, error, sampleBudget,
                    timeStart, timeEnd)) {
                return false;
            }
            return loadWvz4SignalBatchIntoMapForCompare(
                reader, rightIds, signalsById, error, sampleBudget,
                timeStart, timeEnd);
        }

        loadedWave = WaveFile();
        loadError.clear();
        if (reader.loadSignals(signalIds, loadedWave, loadError, 0,
                               timeStart, timeEnd)) {
            for (WaveSignal& sig : loadedWave.signalList) {
                signalsById.insert(sig.signalId, std::move(sig));
            }
            return true;
        }
    }

    error = loadError;
    return false;
}

bool buildComparedWaveFileWvz4Streaming(const QString& leftPath,
                                        const QString& rightPath,
                                        WaveFile& outWave,
                                        QString& error,
                                        CompareProgressState* progress,
                                        const CompareBuildOptions& options) {
    error.clear();
    outWave = WaveFile();
    QElapsedTimer totalTimer;
    QElapsedTimer stageTimer;
    if (viewerPerfLogEnabled()) {
        totalTimer.start();
        stageTimer.start();
    }

    WaveParser4Reader leftReader;
    WaveParser4Reader rightReader;
    QString leftError;
    QString rightError;

    auto leftLoad = std::async(std::launch::async, [&]() {
        return leftReader.open(leftPath, leftError);
    });
    auto rightLoad = std::async(std::launch::async, [&]() {
        return rightReader.open(rightPath, rightError);
    });

    if (!leftLoad.get()) {
        error = QStringLiteral("Failed to load first WVZ4 directory:\n%1").arg(leftError);
        return false;
    }
    if (!rightLoad.get()) {
        error = QStringLiteral("Failed to load second WVZ4 directory:\n%1").arg(rightError);
        return false;
    }
    const WaveFile& leftDirectory = leftReader.directoryWave();
    const WaveFile& rightDirectory = rightReader.directoryWave();
    if (viewerPerfLogEnabled()) {
        comparePerfLog(
            "compare.streaming.directory_load", stageTimer.restart(), 0, 0,
            waveSignalCount(leftDirectory.signalList),
            waveSignalCount(rightDirectory.signalList), 0);
    }

    const bool signalPathsIndexAligned =
        waveDirectorySignalPathsIndexAligned(leftDirectory, rightDirectory);
    const int alignedJobCount = signalPathsIndexAligned
        ? waveSignalCount(leftDirectory.signalList)
        : 0;
    if (viewerPerfLogEnabled()) {
        comparePerfLog("compare.streaming.layout_match", stageTimer.restart(),
                       0, alignedJobCount,
                       leftDirectory.tree.nodesById.size(),
                       rightDirectory.tree.nodesById.size(),
                       signalPathsIndexAligned ? 1 : 0);
    }

    // The overwhelmingly common equal-layout case can be decided directly from
    // the raw block streams. Do this before materializing millions of full paths
    // and CompareSignalJob objects.
    if (options.emitComparedWave &&
        options.signalMode == CompareSignalMode::EventTimesAndValues &&
        !options.excludedLeftSignalIndexes &&
        !options.differingLeftSignalIndexes &&
        !rawBlockCompareDisabledByEnv() &&
        waveDirectoriesEquivalentForRawBlockCompare(
            leftDirectory, rightDirectory, signalPathsIndexAligned)) {
        QElapsedTimer rawCompareTimer;
        if (viewerPerfLogEnabled()) rawCompareTimer.start();
        QString rawCompareError;
        const WaveParser4Reader::RawBlockCompareResult rawCompare =
            leftReader.compareRawBlocksWith(rightReader, rawCompareError);
        if (viewerPerfLogEnabled()) {
            comparePerfLog("compare.streaming.raw_block_compare",
                           rawCompareTimer.elapsed(),
                           0,
                           alignedJobCount,
                           waveSignalCount(leftDirectory.signalList),
                           waveSignalCount(rightDirectory.signalList),
                           int(rawCompare));
        }
        if (rawCompare == WaveParser4Reader::RawBlockCompareResult::Equal &&
            !compareFileRangesDiffer(leftDirectory.meta, rightDirectory.meta)) {
            error = QStringLiteral("No matching-path signal differs at any cycle.");
            if (viewerPerfLogEnabled()) {
                comparePerfLog("compare.streaming.total", totalTimer.elapsed(),
                               0, alignedJobCount,
                               waveSignalCount(leftDirectory.signalList),
                               waveSignalCount(rightDirectory.signalList), 0);
            }
            return false;
        }
        // Different/unsupported/error fall back to the materialized signal path so
        // the viewer can still build precise diff regions and report format errors.
        Q_UNUSED(rawCompareError);
    }

    QVector<CompareSignalJob> jobs;
    int leftUniquePathCount = alignedJobCount;
    int rightUniquePathCount = alignedJobCount;
    if (!signalPathsIndexAligned) {
        if (leftDirectory.tree.valid && rightDirectory.tree.valid) {
            buildTreeMatchedCompareJobs(leftDirectory, rightDirectory, jobs);
            leftUniquePathCount = waveSignalCount(leftDirectory.signalList);
            rightUniquePathCount = waveSignalCount(rightDirectory.signalList);
        } else {
            buildDigestMatchedCompareJobs(leftDirectory, rightDirectory, jobs);
            leftUniquePathCount = waveSignalCount(leftDirectory.signalList);
            rightUniquePathCount = waveSignalCount(rightDirectory.signalList);
        }
    }

    const int jobCount = signalPathsIndexAligned ? alignedJobCount : jobs.size();
    if (viewerPerfLogEnabled()) {
        comparePerfLog("compare.streaming.job_build", stageTimer.restart(),
                       0, jobCount, leftUniquePathCount, rightUniquePathCount,
                       signalPathsIndexAligned ? 1 : 0);
    }
    if (options.differingLeftSignalIndexes && !signalPathsIndexAligned) {
        QBitArray matchedLeftIndexes(waveSignalCount(leftDirectory.signalList));
        for (const CompareSignalJob& job : jobs) {
            if (job.leftIndex >= 0 && job.leftIndex < matchedLeftIndexes.size()) {
                matchedLeftIndexes.setBit(job.leftIndex);
            }
        }
        for (int leftIndex = 0; leftIndex < matchedLeftIndexes.size(); ++leftIndex) {
            if (!matchedLeftIndexes.testBit(leftIndex)) {
                options.differingLeftSignalIndexes->insert(leftIndex);
            }
        }
    }
    if (jobCount <= 0) {
        if (!options.emitComparedWave && options.differingLeftSignalIndexes) {
            return true;
        }
        error = QStringLiteral("The two WVZ4 files have no matching-path signals to compare.");
        return false;
    }

    const CompareCycleRange compareRange = makeMaxCompareCycleRange(leftDirectory.meta, rightDirectory.meta);
    const CompareCycleRange diffRange = makeOverlappedCompareCycleRange(leftDirectory.meta, rightDirectory.meta);
    if (options.emitComparedWave) {
        outWave.meta.title = QStringLiteral("Compare_%1_vs_%2")
            .arg(QFileInfo(leftPath).completeBaseName(), QFileInfo(rightPath).completeBaseName());
        outWave.meta.timescale = (leftDirectory.meta.timescale == rightDirectory.meta.timescale)
            ? leftDirectory.meta.timescale
            : QStringLiteral("cycle");
        outWave.meta.start = compareRange.start;
        outWave.meta.end = qMax(compareRange.end, outWave.meta.start + 1);
        initializeCompareMeta(outWave.meta, leftPath, leftDirectory.meta, rightPath, rightDirectory.meta);
    }

    if (!diffRange.hasRange) {
        error = formatNoSignalDiffMessage(leftDirectory.meta, rightDirectory.meta);
        if (viewerPerfLogEnabled()) {
            comparePerfLog("compare.streaming.total", totalTimer.elapsed(),
                           0, jobCount,
                           waveSignalCount(leftDirectory.signalList),
                           waveSignalCount(rightDirectory.signalList), 0);
        }
        return false;
    }

    if (progress) {
        progress->totalJobs.store(jobCount, std::memory_order_release);
        progress->completedJobs.store(0, std::memory_order_release);
        progress->outputSignalPairs.store(0, std::memory_order_release);
    }

    int nextSignalId = 1;
    const qint64 maxInputBytes = qMax(QFileInfo(leftPath).size(), QFileInfo(rightPath).size());
    const int signalBatchSize = (maxInputBytes >= kCompareStreamingHugeFileThresholdBytes)
        ? kCompareStreamingHugeFileSignalBatchSize
        : kCompareStreamingDefaultSignalBatchSize;
    for (int batchStart = 0; batchStart < jobCount; batchStart += signalBatchSize) {
        QElapsedTimer batchTimer;
        if (viewerPerfLogEnabled()) batchTimer.start();
        if (progress && progress->cancelRequested.load(std::memory_order_acquire)) {
            error = QStringLiteral("Compare cancelled.");
            return false;
        }

        const int batchEnd = qMin(jobCount, batchStart + signalBatchSize);
        const int batchCount = batchEnd - batchStart;
        QVector<int> activeJobIndexes;
        QVector<int> leftIds;
        QVector<int> rightIds;
        activeJobIndexes.reserve(batchCount);
        leftIds.reserve(batchCount);
        rightIds.reserve(batchCount);
        for (int i = batchStart; i < batchEnd; ++i) {
            const int leftIndex = signalPathsIndexAligned ? i : jobs.at(i).leftIndex;
            const int rightIndex = signalPathsIndexAligned ? i : jobs.at(i).rightIndex;
            if (options.excludedLeftSignalIndexes &&
                options.excludedLeftSignalIndexes->contains(leftIndex)) {
                if (progress) {
                    progress->completedJobs.fetch_add(1, std::memory_order_release);
                }
                continue;
            }
            activeJobIndexes.push_back(i);
            if (leftIndex >= 0) {
                const int sid = leftDirectory.signalList.at(leftIndex).signalId;
                if (sid > 0) leftIds.push_back(sid);
            }
            if (rightIndex >= 0) {
                const int sid = rightDirectory.signalList.at(rightIndex).signalId;
                if (sid > 0) rightIds.push_back(sid);
            }
        }
        if (viewerPerfLogEnabled()) {
            comparePerfLog("compare.streaming.batch_collect_ids", batchTimer.restart(),
                           batchStart, batchCount, leftIds.size(), rightIds.size(), 0);
        }

        QHash<int, WaveSignal> leftSignalsById;
        QHash<int, WaveSignal> rightSignalsById;
        leftSignalsById.reserve(leftIds.size() * 2 + 1);
        rightSignalsById.reserve(rightIds.size() * 2 + 1);

        QString leftBatchError;
        QString rightBatchError;
        auto leftBatchLoad = std::async(std::launch::async, [&]() {
            return loadWvz4SignalBatchIntoMapForCompare(
                leftReader, leftIds, leftSignalsById, leftBatchError,
                kCompareStreamingBatchSampleBudget, diffRange.start, diffRange.end);
        });
        auto rightBatchLoad = std::async(std::launch::async, [&]() {
            return loadWvz4SignalBatchIntoMapForCompare(
                rightReader, rightIds, rightSignalsById, rightBatchError,
                kCompareStreamingBatchSampleBudget, diffRange.start, diffRange.end);
        });
        if (!leftBatchLoad.get()) {
            error = QStringLiteral("Failed to load first WVZ4 signal batch:\n%1").arg(leftBatchError);
            return false;
        }
        if (!rightBatchLoad.get()) {
            error = QStringLiteral("Failed to load second WVZ4 signal batch:\n%1").arg(rightBatchError);
            return false;
        }
        if (viewerPerfLogEnabled()) {
            comparePerfLog("compare.streaming.batch_load", batchTimer.restart(),
                           batchStart, batchCount, leftSignalsById.size(), rightSignalsById.size(), 0);
        }

        int batchOutputPairs = 0;
        for (int activeJobIndex = 0; activeJobIndex < activeJobIndexes.size();
             ++activeJobIndex) {
            const int i = activeJobIndexes.at(activeJobIndex);
            if (progress && progress->cancelRequested.load(std::memory_order_acquire)) {
                error = QStringLiteral("Compare cancelled.");
                return false;
            }

            const int leftIndex = signalPathsIndexAligned ? i : jobs.at(i).leftIndex;
            const int rightIndex = signalPathsIndexAligned ? i : jobs.at(i).rightIndex;
            QVector<WaveDiffRegion> diffRegions;
            const WaveSignal* leftSig = nullptr;
            const WaveSignal* rightSig = nullptr;

            if (leftIndex >= 0) {
                const int sid = leftDirectory.signalList.at(leftIndex).signalId;
                const auto it = leftSignalsById.constFind(sid);
                if (it == leftSignalsById.constEnd()) {
                    error = QStringLiteral("First WVZ4 batch did not return signal_id %1").arg(sid);
                    return false;
                }
                leftSig = &it.value();
            }
            if (rightIndex >= 0) {
                const int sid = rightDirectory.signalList.at(rightIndex).signalId;
                const auto it = rightSignalsById.constFind(sid);
                if (it == rightSignalsById.constEnd()) {
                    error = QStringLiteral("Second WVZ4 batch did not return signal_id %1").arg(sid);
                    return false;
                }
                rightSig = &it.value();
            }

            if (!diffRange.hasRange) {
                diffRegions.clear();
            } else {
                if (options.signalMode == CompareSignalMode::EventTimesOnly) {
                    diffRegions = computeSignalEventTimeDiffRegions(
                        *leftSig, *rightSig, diffRange.start, diffRange.end);
                } else {
                    diffRegions = computeSignalDiffRegions(*leftSig, *rightSig,
                                                           diffRange.start, diffRange.end,
                                                           diffRange.end, diffRange.end);
                }
            }

            if (!diffRegions.isEmpty()) {
                const QString matchedPath =
                    fullSignalPathFromWave(leftDirectory, leftIndex);
                if (options.differingLeftSignalIndexes) {
                    options.differingLeftSignalIndexes->insert(leftIndex);
                }
                if (options.emitComparedWave) {
                    const QString leftName = comparedSideFullPath(matchedPath, QStringLiteral("A"));
                    const QString rightName = comparedSideFullPath(matchedPath, QStringLiteral("B"));
                    outWave.signalList.push_back(makeComparedSideSignal(leftName, *leftSig, nextSignalId++, diffRegions,
                                                                        leftDirectory.meta.start, leftDirectory.meta.end));
                    outWave.signalList.push_back(makeComparedSideSignal(rightName, *rightSig, nextSignalId++, diffRegions,
                                                                        rightDirectory.meta.start, rightDirectory.meta.end));
                    if (progress) progress->outputSignalPairs.fetch_add(1, std::memory_order_relaxed);
                    ++batchOutputPairs;
                }
            }

            if (progress) progress->completedJobs.fetch_add(1, std::memory_order_release);
        }
        if (viewerPerfLogEnabled()) {
            comparePerfLog("compare.streaming.batch_compare", batchTimer.restart(),
                           batchStart, batchCount, leftSignalsById.size(), rightSignalsById.size(), batchOutputPairs);
        }
    }

    if (!options.emitComparedWave) {
        return true;
    }

    if (outWave.signalList.empty()) {
        error = formatNoSignalDiffMessage(leftDirectory.meta, rightDirectory.meta);
        if (viewerPerfLogEnabled()) {
            comparePerfLog("compare.streaming.total", totalTimer.elapsed(),
                           0, jobCount,
                           waveSignalCount(leftDirectory.signalList),
                           waveSignalCount(rightDirectory.signalList), 0);
        }
        return false;
    }

    if (viewerPerfLogEnabled()) {
        comparePerfLog("compare.streaming.total", totalTimer.elapsed(),
                       0, jobCount,
                       waveSignalCount(leftDirectory.signalList),
                       waveSignalCount(rightDirectory.signalList),
                       waveSignalCount(outWave.signalList) / 2);
    }
    return true;
}

class ReadOnlyTextDelegate : public QStyledItemDelegate {
public:
    explicit ReadOnlyTextDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override {
        QLineEdit* editor = new QLineEdit(parent);
        editor->setReadOnly(true);
        editor->setFrame(false);
        editor->setStyleSheet(QStringLiteral("QLineEdit { background:#2E3540; color:#F7FAFF; selection-background-color:#5E8FD6; selection-color:#FFFFFF; padding:0 2px; border:none; }"));
        return editor;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        QLineEdit* line = qobject_cast<QLineEdit*>(editor);
        if (!line) {
            QStyledItemDelegate::setEditorData(editor, index);
            return;
        }
        const QString text = index.data(Qt::DisplayRole).toString();
        line->setText(text);
        line->setCursorPosition(text.size());
        line->deselect();
    }

    void setModelData(QWidget*, QAbstractItemModel*, const QModelIndex&) const override {
        // Read-only delegate: this editor exists only for text selection/copy and cursor navigation.
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        if (editor) editor->setGeometry(option.rect.adjusted(1, 1, -1, -1));
    }
};

class SignalTreeModel : public QAbstractItemModel {
public:
    explicit SignalTreeModel(QObject* parent = nullptr)
        : QAbstractItemModel(parent) {}

    void setArrayFetchCallback(std::function<void(int)> callback) {
        m_arrayFetchCallback = std::move(callback);
    }

    void setLogicTree(SignalLogicTree* tree) {
        beginResetModel();
        m_tree = tree;
        m_searchMode = false;
        m_searchNodeIds.clear();
        clearSearchStorage();
        rebuildRowCache();
        endResetModel();
    }

    void setSearchRoots(const QVector<int>& nodeIds, bool cropTree = false) {
        m_searchNodeIds = nodeIds;
        if (cropTree) {
            beginResetModel();
            m_searchMode = true;
            clearSearchStorage();
            rebuildSearchStorage();
            endResetModel();
            return;
        }
        if (m_searchMode) {
            beginResetModel();
            m_searchMode = false;
            clearSearchStorage();
            endResetModel();
            return;
        }
    }

    void clearSearch() {
        if (m_searchNodeIds.isEmpty() && !m_searchMode) return;
        m_searchNodeIds.clear();
        if (m_searchMode) {
            beginResetModel();
            m_searchMode = false;
            clearSearchStorage();
            endResetModel();
            return;
        }
    }

    QModelIndex indexForNode(int nodeId, int column = 0) const {
        if (!isValidNode(nodeId) || column < 0 || column >= columnCount()) return QModelIndex();
        if (!isNodeVisibleInCurrentMode(nodeId)) return QModelIndex();

        int row = -1;
        if (m_searchMode) {
            row = searchRowForNode(nodeId);
        } else {
            row = m_tree->nodeRowInParent(nodeId);
        }
        if (row < 0) return QModelIndex();
        return createIndex(row, column, quintptr(nodeId));
    }

    int nodeIdFromIndex(const QModelIndex& index) const {
        if (!index.isValid() || index.model() != this) return -1;
        const int nodeId = int(index.internalId());
        return isValidNode(nodeId) ? nodeId : -1;
    }

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override {
        if (!m_tree || row < 0 || column < 0 || column >= columnCount()) return QModelIndex();

        int nodeId = -1;
        if (!parent.isValid()) {
            const SmallVec32<int, 32>* rootRows = currentRootRows();
            if (!rootRows || row >= rootRows->size()) return QModelIndex();
            nodeId = (*rootRows)[row];
        } else {
            const int parentNodeId = nodeIdFromIndex(parent);
            const SmallVec32<int, 32>* rows = childRowsForNode(parentNodeId);
            if (!rows || row >= rows->size()) return QModelIndex();
            nodeId = (*rows)[row];
        }

        if (!isValidNode(nodeId)) return QModelIndex();
        return createIndex(row, column, quintptr(nodeId));
    }

    QModelIndex parent(const QModelIndex& child) const override {
        if (!m_tree || !child.isValid() || child.model() != this) return QModelIndex();
        const int nodeId = nodeIdFromIndex(child);
        if (!isValidNode(nodeId)) return QModelIndex();

        const int parentNodeId = m_tree->nodeParent(nodeId);
        if (!isValidNode(parentNodeId)) return QModelIndex();
        if (m_searchMode && !isSearchVisibleNode(parentNodeId)) return QModelIndex();

        int parentRow = -1;
        if (m_searchMode) {
            parentRow = searchRowForNode(parentNodeId);
        } else {
            parentRow = m_tree->nodeRowInParent(parentNodeId);
        }
        return parentRow >= 0 ? createIndex(parentRow, 0, quintptr(parentNodeId)) : QModelIndex();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        if (!m_tree || parent.column() > 0) return 0;
        if (!parent.isValid()) {
            const SmallVec32<int, 32>* rootRows = currentRootRows();
            return rootRows ? rootRows->size() : 0;
        }
        const int parentNodeId = nodeIdFromIndex(parent);
        const SmallVec32<int, 32>* rows = childRowsForNode(parentNodeId);
        return rows ? rows->size() : 0;
    }

    int columnCount(const QModelIndex& = QModelIndex()) const override {
        return 1;
    }

    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override {
        if (!parent.isValid()) return rowCount(parent) > 0;
        if (!m_tree || parent.column() > 0) return false;
        const int parentNodeId = nodeIdFromIndex(parent);
        return m_tree->hasChildren(parentNodeId);
    }

    bool canFetchMore(const QModelIndex& parent) const override {
        if (!parent.isValid() || !m_tree || parent.column() != 0) return false;
        const int nodeId = nodeIdFromIndex(parent);
        if (m_searchMode &&
            (!isSearchVisibleNode(nodeId) ||
             !isSearchSubtreeComplete(nodeId))) {
            return false;
        }
        return m_tree->arrayHasUnmaterializedElements(nodeId);
    }

    void fetchMore(const QModelIndex& parent) override {
        if (!canFetchMore(parent) || !m_arrayFetchCallback) return;
        m_arrayFetchCallback(nodeIdFromIndex(parent));
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!m_tree || !index.isValid() || index.column() != 0) return QVariant();
        const int nodeId = nodeIdFromIndex(index);
        if (!isValidNode(nodeId)) return QVariant();

        if (role == kTreeRoleNodeId) return nodeId;
        if (role == kTreeRoleSignalIndex) {
            const int signalIndex = m_tree->nodeSignalIndex(nodeId);
            return signalIndex >= 0 ? QVariant(signalIndex) : QVariant();
        }

        if (role == Qt::DisplayRole) {
            QString text = m_tree->nodeNameString(nodeId);
            if (m_tree->nodeSignalIndex(nodeId) >= 0) {
                text = formatNameWidthBeforeCompareSuffix(text, m_tree->nodeWidth(nodeId));
            }
            return text;
        }
        if (role == Qt::ToolTipRole) {
            return m_tree->fullPathForNodeId(nodeId);
        }
        if (role == Qt::ForegroundRole) {
            if (m_tree->nodeSignalIndex(nodeId) >= 0) return QBrush(QColor("#F2F4F7"));
            return QBrush(QColor("#9CC7FF"));
        }
        if (role == Qt::FontRole && m_tree->nodeSignalIndex(nodeId) < 0) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return QVariant();
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid()) return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsEditable;
    }

    QStringList mimeTypes() const override {
        QStringList types;
        types << QString::fromLatin1(kMimeSignalIndexes);
        return types;
    }

    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        QMimeData* mime = new QMimeData();
        QSet<int> seenNodes;
        QSet<int> seenSignals;
        QList<int> signalIndexes;
        for (const QModelIndex& index : indexes) {
            if (!index.isValid() || index.column() != 0) continue;
            const int nodeId = nodeIdFromIndex(index);
            if (!isValidNode(nodeId) || seenNodes.contains(nodeId)) continue;
            seenNodes.insert(nodeId);
            collectSignalIndexes(nodeId, seenSignals, signalIndexes);
        }
        if (signalIndexes.isEmpty()) {
            delete mime;
            return nullptr;
        }
        mime->setData(kMimeSignalIndexes, encodeIntList(signalIndexes));
        return mime;
    }

    Qt::DropActions supportedDragActions() const override {
        return Qt::CopyAction;
    }

    bool materializeBitsetChildren(WaveTreeInfo& tree,
                                   WaveSignalList& signalDefs,
                                   int nodeId,
                                   QString& error) {
        error.clear();
        if (!m_tree || !m_tree->isBitsetContainer(nodeId) ||
            nodeId <= 0 || nodeId >= tree.nodesById.size() ||
            nodeId >= tree.bitsetIndexByNodeId.size()) {
            return true;
        }
        const int encodedIndex = tree.bitsetIndexByNodeId.at(nodeId);
        if (encodedIndex <= 0 || encodedIndex > tree.bitsets.size()) {
            error = QStringLiteral("Invalid bitset node metadata");
            return false;
        }
        WaveBitsetInfo& existing = tree.bitsets[encodedIndex - 1];
        if (existing.materialized) return true;
        const int bitCount = existing.bitCount;
        if (bitCount <= 0 ||
            tree.nodesById.size() > (std::numeric_limits<int>::max)() - bitCount ||
            signalDefs.size() > std::size_t((std::numeric_limits<int>::max)() - bitCount)) {
            error = QStringLiteral("Bitset is too large to expand in the Viewer");
            return false;
        }

        const int firstNodeId = tree.nodesById.size();
        const int firstSignalIndex = waveSignalCount(signalDefs);
        const int firstSignalId = existing.firstVirtualSignalId;
        const int firstStorageId = existing.firstStorageId;
        if (firstSignalId <= 0 || firstStorageId <= 0 ||
            firstSignalId > (std::numeric_limits<int>::max)() - bitCount + 1) {
            error = QStringLiteral("Invalid bitset virtual signal range");
            return false;
        }

        const bool exposeSearchChildren =
            m_searchMode && isSearchVisibleNode(nodeId) &&
            isSearchSubtreeComplete(nodeId);
        const bool insertVisibleRows = !m_searchMode || exposeSearchChildren;
        const QModelIndex parentIndex = indexForNode(nodeId);
        if (insertVisibleRows) beginInsertRows(parentIndex, 0, bitCount - 1);

        tree.nodesById.reserve(firstNodeId + bitCount);
        tree.signalIndexToNodeId.reserve(firstSignalIndex + bitCount);
        signalDefs.reserve(std::size_t(firstSignalIndex + bitCount));
        const int lastSignalId = firstSignalId + bitCount - 1;
        if (tree.signalIndexBySignalId.size() <= lastSignalId) {
            tree.signalIndexBySignalId.resize(lastSignalId + 1);
        }

        for (int bit = 0; bit < bitCount; ++bit) {
            const int childNodeId = firstNodeId + bit;
            const int signalIndex = firstSignalIndex + bit;
            const int signalId = firstSignalId + bit;

            WaveTreeNode child;
            child.parentId = nodeId;
            child.nextSibling = bit + 1 < bitCount ? childNodeId + 1 : 0;
            child.rowInParent = bit;
            child.signalIndex = signalIndex;
            child.signalId = signalId;
            child.nameToken = waveArrayIndexToken(quint32(bit));
            child.kind = 6; // WVZ4 NodeKind::SignalLeaf
            child.valid = true;
            tree.nodesById.push_back(child);

            WaveSignal signal;
            signal.signalId = signalId;
            signal.storageId = firstStorageId + bit / 64;
            signal.bitOffset = bit % 64;
            signal.kind = SignalKind::Bit;
            signal.width = 1;
            signal.defaultRadix = ValueRadix::Bin;
            signal.currentRadix = ValueRadix::Bin;
            signal.virtualBitsetBit = true;
            signal.samplesLoaded = false;
            signalDefs.push_back(std::move(signal));
            tree.signalIndexToNodeId.push_back(childNodeId);
            tree.signalIndexBySignalId[signalId] = signalIndex + 1;
        }
        tree.nodesById[nodeId].firstChild = firstNodeId;
        WaveBitsetInfo& completed = tree.bitsets[encodedIndex - 1];
        completed.firstMaterializedNodeId = firstNodeId;
        completed.materialized = true;
        m_tree->invalidateWaveChildList(nodeId);

        if (m_searchMode) {
            const int newNodeCount = m_tree->nodeCount();
            m_searchVisible.resize(newNodeCount);
            std::fill(m_searchVisible.begin() + firstNodeId,
                      m_searchVisible.end(), uchar(0));
            m_searchSubtreeComplete.resize(newNodeCount);
            std::fill(m_searchSubtreeComplete.begin() + firstNodeId,
                      m_searchSubtreeComplete.end(), uchar(0));
            m_searchRowByNodeId.resize(newNodeCount);
            std::fill(m_searchRowByNodeId.begin() + firstNodeId,
                      m_searchRowByNodeId.end(), -1);
            if (exposeSearchChildren) {
                m_searchSubtreeComplete[nodeId] = 0;
                markSubtreeVisible(nodeId);
            }
        }
        if (insertVisibleRows) {
            endInsertRows();
            if (parentIndex.isValid()) emit dataChanged(parentIndex, parentIndex);
        }
        return true;
    }

    bool materializeArrayChildren(WaveTreeInfo& tree,
                                  WaveSignalList& signalDefs,
                                  int nodeId,
                                  QString& error) {
        error.clear();
        if (!m_tree || !m_tree->isArrayContainer(nodeId) || nodeId <= 0 ||
            nodeId >= tree.nodesById.size() || nodeId >= tree.arrayIndexByNodeId.size()) {
            return true;
        }
        const int encodedIndex = tree.arrayIndexByNodeId.at(nodeId);
        if (encodedIndex <= 0 || encodedIndex > tree.arrays.size()) {
            error = QStringLiteral("Invalid array node metadata");
            return false;
        }
        WaveArrayInfo& info = tree.arrays[encodedIndex - 1];
        if (info.materialized) return true;
        if (info.elementCount == 0 || info.elementCount > quint64(0x7fffffffu) ||
            info.elementCount > quint64(std::numeric_limits<int>::max()) ||
            info.schema.isEmpty() || info.leafCountPerElement == 0) {
            error = QStringLiteral("Array is too large or has an invalid element schema");
            return false;
        }
        const quint64 totalSignals = info.elementCount * info.leafCountPerElement;
        if (totalSignals > quint64(std::numeric_limits<int>::max()) ||
            info.firstVirtualSignalId <= 0 ||
            quint64(info.firstVirtualSignalId) + totalSignals - 1 >
                quint64(std::numeric_limits<int>::max()) ||
            signalDefs.size() > std::size_t(std::numeric_limits<int>::max()) -
                                    std::size_t(totalSignals)) {
            error = QStringLiteral("Array signal range exceeds Viewer limits");
            return false;
        }

        static const quint64 kElementBatch = 256;
        const quint64 firstOuter = info.materializedElementCount;
        if (firstOuter >= info.elementCount) {
            info.materialized = true;
            return true;
        }
        const quint64 batchCount = qMin(kElementBatch, info.elementCount - firstOuter);

        const bool exposeSearchChildren =
            m_searchMode && isSearchVisibleNode(nodeId) &&
            isSearchSubtreeComplete(nodeId);
        const bool insertVisibleRows = !m_searchMode || exposeSearchChildren;
        const QModelIndex parentIndex = indexForNode(nodeId);
        if (insertVisibleRows) {
            beginInsertRows(parentIndex, int(firstOuter),
                            int(firstOuter + batchCount - 1));
        }

        const int firstNodeId = tree.nodesById.size();
        int nextSignalId = 0;
        QHash<int, int> lastChildByParent;
        auto appendNode = [&](int parent, quint32 nameToken, quint8 kind) -> int {
            if (tree.nodesById.size() == std::numeric_limits<int>::max()) return 0;
            const int id = tree.nodesById.size();
            WaveTreeNode node;
            node.parentId = parent;
            node.nameToken = nameToken;
            node.kind = kind;
            node.valid = true;
            int row = 0;
            int child = tree.nodesById.at(parent).firstChild;
            if (child == 0) {
                tree.nodesById[parent].firstChild = id;
            } else {
                const auto found = lastChildByParent.constFind(parent);
                if (found != lastChildByParent.constEnd()) {
                    child = found.value();
                } else {
                    while (tree.nodesById.at(child).nextSibling != 0) {
                        child = tree.nodesById.at(child).nextSibling;
                    }
                }
                tree.nodesById[child].nextSibling = id;
                row = tree.nodesById.at(child).rowInParent + 1;
            }
            node.rowInParent = row;
            tree.nodesById.push_back(node);
            lastChildByParent.insert(parent, id);
            return id;
        };
        QVector<QVector<int>> schemaChildren(info.schema.size() + 1);
        for (int i = 0; i < info.schema.size(); ++i) {
            const int parentSchemaNodeId = info.schema.at(i).parentSchemaNodeId;
            if (parentSchemaNodeId >= 0 && parentSchemaNodeId < schemaChildren.size()) {
                schemaChildren[parentSchemaNodeId].push_back(i + 1);
            }
        }
        bool ok = true;
        std::function<void(int, int, quint64)> expandSchema;
        expandSchema = [&](int schemaNodeId, int parent, quint64 repeatedOffset) {
            if (!ok || schemaNodeId <= 0 || schemaNodeId > info.schema.size()) { ok = false; return; }
            const WaveArraySchemaNode schema = info.schema.at(schemaNodeId - 1);
            if (schema.kind == WaveArraySchemaKind::Leaf) {
                int leafNode = parent;
                if (schema.nameToken != 0) {
                    leafNode = appendNode(parent, schema.nameToken, 6);
                    if (leafNode == 0) { ok = false; return; }
                } else {
                    tree.nodesById[leafNode].kind = 6;
                }
                if (nextSignalId <= 0 || nextSignalId == std::numeric_limits<int>::max()) { ok = false; return; }
                const int signalIndex = waveSignalCount(signalDefs);
                WaveSignal signal;
                signal.signalId = nextSignalId++;
                signal.storageId = -1;
                signal.bitOffset = schema.bitOffset;
                signal.width = qMax(1, schema.bitWidth);
                signal.kind = signal.width == 1 ? SignalKind::Bit : SignalKind::Bus;
                const int valueType = int(schema.valueType);
                if (valueType == 10) signal.defaultRadix = ValueRadix::Float;
                else if (valueType == 11) signal.defaultRadix = ValueRadix::Double;
                else if (valueType == 2 || valueType == 4 || valueType == 6) signal.defaultRadix = ValueRadix::Int;
                else if (valueType == 8) signal.defaultRadix = ValueRadix::Int64;
                else if (valueType == 9) signal.defaultRadix = ValueRadix::UInt64;
                else if (signal.width == 1) signal.defaultRadix = ValueRadix::Bin;
                else signal.defaultRadix = ValueRadix::UInt;
                signal.currentRadix = signal.defaultRadix;
                signal.virtualArrayLeaf = true;
                signal.arrayNodeId = info.nodeId;
                signal.arrayByteOffset = repeatedOffset + schema.byteOffset;
                signal.arrayByteWidth = int(schema.byteSize);
                signal.samplesLoaded = false;
                signalDefs.push_back(std::move(signal));
                tree.nodesById[leafNode].signalIndex = signalIndex;
                tree.nodesById[leafNode].signalId = nextSignalId - 1;
                tree.signalIndexToNodeId.push_back(leafNode);
                if (tree.signalIndexBySignalId.size() <= nextSignalId - 1) {
                    tree.signalIndexBySignalId.resize(nextSignalId);
                }
                tree.signalIndexBySignalId[nextSignalId - 1] = signalIndex + 1;
                return;
            }

            int container = parent;
            if (schema.nameToken != 0) {
                container = appendNode(parent, schema.nameToken, 3);
                if (container == 0) { ok = false; return; }
            }
            const QVector<int>& children = schemaChildren.at(schemaNodeId);
            if (schema.kind == WaveArraySchemaKind::Array) {
                if (schema.elementCount > quint64(0x7fffffffu)) { ok = false; return; }
                for (quint64 i = 0; i < schema.elementCount; ++i) {
                    const int elementNode = appendNode(container, waveArrayIndexToken(quint32(i)), 4);
                    if (elementNode == 0) { ok = false; return; }
                    for (int childSchema : children) {
                        expandSchema(childSchema, elementNode,
                                     repeatedOffset + i * schema.elementStride);
                    }
                }
            } else {
                for (int childSchema : children) expandSchema(childSchema, container, repeatedOffset);
            }
        };

        for (quint64 outer = firstOuter; outer < firstOuter + batchCount && ok; ++outer) {
            nextSignalId = info.firstVirtualSignalId + int(outer * info.leafCountPerElement);
            const int elementNode = appendNode(nodeId, waveArrayIndexToken(quint32(outer)), 4);
            if (elementNode == 0) { ok = false; break; }
            expandSchema(1, elementNode, outer * info.elementStride);
            const int expectedNext = info.firstVirtualSignalId +
                                     int((outer + 1) * info.leafCountPerElement);
            if (nextSignalId != expectedNext) ok = false;
        }
        if (!ok) {
            if (insertVisibleRows) endInsertRows();
            error = QStringLiteral("Failed to materialize the array element schema");
            return false;
        }
        if (info.firstMaterializedNodeId == 0) info.firstMaterializedNodeId = firstNodeId;
        info.materializedElementCount = firstOuter + batchCount;
        info.materialized = info.materializedElementCount == info.elementCount;
        m_tree->invalidateWaveChildList(nodeId);
        if (m_searchMode) {
            const int newNodeCount = m_tree->nodeCount();
            m_searchVisible.resize(newNodeCount);
            std::fill(m_searchVisible.begin() + firstNodeId,
                      m_searchVisible.end(), uchar(0));
            m_searchSubtreeComplete.resize(newNodeCount);
            std::fill(m_searchSubtreeComplete.begin() + firstNodeId,
                      m_searchSubtreeComplete.end(), uchar(0));
            m_searchRowByNodeId.resize(newNodeCount);
            std::fill(m_searchRowByNodeId.begin() + firstNodeId,
                      m_searchRowByNodeId.end(), -1);
            if (exposeSearchChildren) {
                m_searchSubtreeComplete[nodeId] = 0;
                markSubtreeVisible(nodeId);
            }
        }
        if (insertVisibleRows) {
            endInsertRows();
            if (parentIndex.isValid()) emit dataChanged(parentIndex, parentIndex);
        }
        return true;
    }

    bool installReferencePatch(WaveTreeInfo& tree,
                               const WaveSubtreeReferencePatch& patch,
                               QString& error) {
        if (!m_tree || !m_tree->isValidNodeId(patch.mountNodeId)) {
            error = QStringLiteral("Reference mount is no longer present in the tree");
            return false;
        }
        if (!m_tree->isPendingReference(patch.mountNodeId)) return true;

        int directChildCount = 0;
        int encodedChild = patch.mountNode.firstChild;
        int guard = 0;
        while (encodedChild < 0 &&
               guard++ <= patch.appendedNodes.size()) {
            ++directChildCount;
            const int localIndex = -encodedChild - 1;
            if (localIndex < 0 || localIndex >= patch.appendedNodes.size()) {
                error = QStringLiteral("Reference patch contains an invalid child id");
                return false;
            }
            encodedChild = patch.appendedNodes.at(localIndex).nextSibling;
        }
        if (encodedChild != 0) {
            error = QStringLiteral("Reference patch contains a non-local child id");
            return false;
        }

        if (m_searchMode) {
            // A background subtree-reference patch must not cancel an active
            // search.  The old reset path cleared m_searchMode, so results
            // disappeared shortly after the search completed when tree warmup
            // delivered its next batch.
            //
            // Only a matched container (or a descendant of one) exposes its
            // complete subtree in search mode.  Such a mount gains visible
            // rows and can be updated incrementally.  An ordinary ancestor
            // path keeps filtering out the newly materialized children, so
            // the patch does not change the model's visible row structure.
            const bool exposesPatchedChildren =
                isSearchVisibleNode(patch.mountNodeId) &&
                isSearchSubtreeComplete(patch.mountNodeId);
            const QModelIndex mountIndex =
                exposesPatchedChildren
                    ? indexForNode(patch.mountNodeId)
                    : QModelIndex();
            const bool insertVisibleRows =
                exposesPatchedChildren && mountIndex.isValid() &&
                directChildCount > 0;
            if (insertVisibleRows) {
                beginInsertRows(mountIndex, 0, directChildCount - 1);
            }

            const int oldSearchNodeCount = m_searchVisible.size();
            const bool ok = applyWaveSubtreeReferencePatch(tree, patch, error);
            m_tree->invalidateWaveChildList(patch.mountNodeId);
            if (ok) {
                const int newNodeCount = m_tree->nodeCount();
                if (newNodeCount > oldSearchNodeCount) {
                    m_searchVisible.resize(newNodeCount);
                    std::fill(m_searchVisible.begin() + oldSearchNodeCount,
                              m_searchVisible.end(), uchar(0));
                    m_searchSubtreeComplete.resize(newNodeCount);
                    std::fill(m_searchSubtreeComplete.begin() + oldSearchNodeCount,
                              m_searchSubtreeComplete.end(), uchar(0));
                    m_searchRowByNodeId.resize(newNodeCount);
                    std::fill(m_searchRowByNodeId.begin() + oldSearchNodeCount,
                              m_searchRowByNodeId.end(), -1);
                }
                if (exposesPatchedChildren) {
                    // Revisit this mount even though it was already marked
                    // complete; the patch added descendants after that mark.
                    m_searchSubtreeComplete[patch.mountNodeId] = 0;
                    markSubtreeVisible(patch.mountNodeId);
                }
            }
            if (insertVisibleRows) endInsertRows();
            return ok;
        }

        const QModelIndex mountIndex = indexForNode(patch.mountNodeId);
        if (directChildCount > 0) {
            beginInsertRows(mountIndex, 0, directChildCount - 1);
        }
        const bool ok = applyWaveSubtreeReferencePatch(tree, patch, error);
        m_tree->invalidateWaveChildList(patch.mountNodeId);
        if (directChildCount > 0) endInsertRows();
        if (ok && mountIndex.isValid()) {
            emit dataChanged(mountIndex, mountIndex);
        }
        return ok;
    }

    bool installReferencePatches(
        WaveTreeInfo& tree,
        const QVector<WaveSubtreeReferencePatch>& patches,
        QString& error) {
        for (const WaveSubtreeReferencePatch& patch : patches) {
            if (!installReferencePatch(tree, patch, error)) return false;
        }
        return true;
    }

private:
    SignalLogicTree* m_tree = nullptr;
    std::function<void(int)> m_arrayFetchCallback;
    bool m_searchMode = false;
    QVector<int> m_searchNodeIds;
    SmallVec32<int, 32> m_searchRootRows;
    std::unordered_map<int, std::unique_ptr<SmallVec32<int, 32>>> m_searchChildren;
    QVector<int> m_searchRowByNodeId;
    QVector<uchar> m_searchVisible;
    QVector<uchar> m_searchSubtreeComplete;

    const SmallVec32<int, 32>* currentRootRows() const {
        if (!m_tree) return nullptr;
        return m_searchMode ? &m_searchRootRows : &m_tree->roots;
    }

    const SmallVec32<int, 32>* childRowsForNode(int nodeId) const {
        if (!isValidNode(nodeId)) return nullptr;
        if (m_searchMode) {
            if (isSearchSubtreeComplete(nodeId)) {
                const LogicChildList* list = m_tree->childListForNode(nodeId);
                return list ? &list->children : nullptr;
            }
            const auto found = m_searchChildren.find(nodeId);
            return found != m_searchChildren.end() ? found->second.get() : nullptr;
        }
        const LogicChildList* list = m_tree->childListForNode(nodeId);
        return list ? &list->children : nullptr;
    }

    bool isValidNode(int nodeId) const {
        return m_tree && m_tree->isValidNodeId(nodeId);
    }

    bool isSearchVisibleNode(int nodeId) const {
        return isValidNode(nodeId) && nodeId >= 0 && nodeId < m_searchVisible.size() && m_searchVisible.at(nodeId) != 0;
    }

    bool isNodeVisibleInCurrentMode(int nodeId) const {
        if (!isValidNode(nodeId)) return false;
        if (!m_searchMode) return true;
        return isSearchVisibleNode(nodeId);
    }

    void clearSearchStorage() {
        m_searchRootRows.clear();
        m_searchChildren.clear();
        m_searchRowByNodeId.clear();
        m_searchVisible.clear();
        m_searchSubtreeComplete.clear();
    }

    void rebuildSearchStorage() {
        if (!m_tree) return;

        const int n = m_tree->nodeCount();
        m_searchVisible.resize(n);
        std::fill(m_searchVisible.begin(), m_searchVisible.end(), uchar(0));

        // Search mode still displays the original tree structure. Mark every
        // matched node plus its ancestors, and expose matched module subtrees.
        for (int nodeId : m_searchNodeIds) {
            if (!isValidNode(nodeId)) continue;
            markNodeAndAncestorsVisible(nodeId);
            if (m_tree->nodeSignalIndex(nodeId) < 0) {
                markSubtreeVisible(nodeId);
            }
        }

        m_searchRowByNodeId.resize(n);
        std::fill(m_searchRowByNodeId.begin(), m_searchRowByNodeId.end(), -1);

        for (int row = 0; row < m_tree->roots.size(); ++row) {
            const int rootNodeId = m_tree->roots[row];
            if (!isSearchVisibleNode(rootNodeId)) continue;
            const int visibleRow = m_searchRootRows.size();
            m_searchRootRows.push_back(rootNodeId);
            if (rootNodeId >= 0 && rootNodeId < m_searchRowByNodeId.size()) {
                m_searchRowByNodeId[rootNodeId] = visibleRow;
            }
            buildSearchChildren(rootNodeId);
        }
    }

    void markNodeAndAncestorsVisible(int nodeId) {
        if (!m_tree) return;
        const int n = m_tree->nodeCount();
        int cur = nodeId;
        int guard = 0;
        while (isValidNode(cur) && guard++ < n) {
            if (cur >= 0 && cur < m_searchVisible.size()) m_searchVisible[cur] = 1;
            cur = m_tree->nodeParent(cur);
        }
    }

    void markSubtreeVisible(int nodeId) {
        if (!isValidNode(nodeId) || nodeId < 0 || nodeId >= m_searchVisible.size()) return;
        if (m_searchSubtreeComplete.size() != m_searchVisible.size()) {
            m_searchSubtreeComplete.resize(m_searchVisible.size());
            std::fill(m_searchSubtreeComplete.begin(), m_searchSubtreeComplete.end(), uchar(0));
        }

        QVector<int> pending;
        pending.push_back(nodeId);
        while (!pending.isEmpty()) {
            const int current = pending.takeLast();
            if (!isValidNode(current) || current < 0 || current >= m_searchVisible.size()) continue;
            if (m_searchSubtreeComplete.at(current) != 0) continue;
            m_searchVisible[current] = 1;
            m_searchSubtreeComplete[current] = 1;
            const LogicChildList* list = m_tree->childListForNode(current);
            if (!list) continue;
            for (int childNodeId : list->children) pending.push_back(childNodeId);
        }
    }

    void buildSearchChildren(int nodeId) {
        if (!isSearchVisibleNode(nodeId) || isSearchSubtreeComplete(nodeId)) return;
        const LogicChildList* list = m_tree->childListForNode(nodeId);
        if (!list) return;
        std::unique_ptr<SmallVec32<int, 32>> rows(new SmallVec32<int, 32>());
        for (int childNodeId : list->children) {
            if (!isSearchVisibleNode(childNodeId)) continue;
            const int row = rows->size();
            rows->push_back(childNodeId);
            if (childNodeId >= 0 && childNodeId < m_searchRowByNodeId.size()) {
                m_searchRowByNodeId[childNodeId] = row;
            }
            buildSearchChildren(childNodeId);
        }
        if (!rows->empty()) m_searchChildren.emplace(nodeId, std::move(rows));
    }

    bool isSearchSubtreeComplete(int nodeId) const {
        return nodeId >= 0 && nodeId < m_searchSubtreeComplete.size() &&
               m_searchSubtreeComplete.at(nodeId) != 0;
    }

    int searchRowForNode(int nodeId) const {
        if (!m_tree || nodeId < 0 || nodeId >= m_searchRowByNodeId.size()) return -1;
        const int parentNodeId = m_tree->nodeParent(nodeId);
        if (isSearchSubtreeComplete(parentNodeId)) return m_tree->nodeRowInParent(nodeId);
        return m_searchRowByNodeId.at(nodeId);
    }

    void rebuildRowCache() {
        // rowInParent is assigned once while building the immutable v17 tree.
    }

    void collectSignalIndexes(int nodeId, QSet<int>& seen, QList<int>& output) const {
        if (!isValidNode(nodeId)) return;
        const int signalIndex = m_tree->nodeSignalIndex(nodeId);
        if (signalIndex >= 0) {
            if (!seen.contains(signalIndex)) {
                seen.insert(signalIndex);
                output.push_back(signalIndex);
            }
            return;
        }
        const LogicChildList* list = m_tree->childListForNode(nodeId);
        if (!list) return;
        for (int childNodeId : list->children) collectSignalIndexes(childNodeId, seen, output);
    }
};

static inline SignalTreeModel* signalTreeModelFrom(QAbstractItemModel* model) {
    return static_cast<SignalTreeModel*>(model);
}

class SignalTreeView : public QTreeView {
public:
    using ActiveRowsDroppedCallback = std::function<void(const QList<int>&)>;
    explicit SignalTreeView(QWidget* parent = nullptr)
        : QTreeView(parent),
          m_slowSecondClickTimer(new QTimer(this)) {
        m_slowSecondClickTimer->setSingleShot(true);
        connect(m_slowSecondClickTimer, &QTimer::timeout, this, [this]() {
            const QModelIndex index = m_pendingSlowSecondClickIndex;
            m_pendingSlowSecondClickIndex = QPersistentModelIndex();
            if (!index.isValid() || !selectionModel() || !selectionModel()->isSelected(index)) return;
            edit(index);
        });
    }

    void setActiveRowsDroppedCallback(ActiveRowsDroppedCallback callback) {
        m_activeRowsDropped = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QTreeView::paintEvent(event);
        if (!model()) return;

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect dirty = event ? event->rect() : viewport()->rect();
        drawVisibleDisclosureIndicators(painter, dirty);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton) cancelPendingSlowSecondClick();
        m_pressIndex = QModelIndex();
        m_pressPos = QPoint();
        m_pressOnDisclosure = false;
        m_pressWasSelected = false;
        if (event && event->button() == Qt::LeftButton) {
            const QModelIndex idx = indexAt(event->pos());
            if (idx.isValid()) {
                m_pressIndex = idx;
                m_pressPos = event->pos();
                m_pressWasSelected = selectionModel() && selectionModel()->isSelected(idx);
                m_pressOnDisclosure = isDisclosureArea(idx, event->pos());
                if (m_pressOnDisclosure) {
                    event->accept();
                    return;
                }
            }
        }
        QTreeView::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event && event->button() == Qt::LeftButton && m_pressOnDisclosure) {
            const QModelIndex releaseIndex = indexAt(event->pos());
            const bool sameDisclosure = releaseIndex.isValid() &&
                                        m_pressIndex.isValid() &&
                                        releaseIndex == m_pressIndex &&
                                        (event->pos() - m_pressPos).manhattanLength() <= QApplication::startDragDistance() &&
                                        isDisclosureArea(releaseIndex, event->pos());
            if (sameDisclosure && model() && model()->hasChildren(releaseIndex)) {
                setExpanded(releaseIndex, !isExpanded(releaseIndex));
                viewport()->update();
            }
            m_pressIndex = QModelIndex();
            m_pressPos = QPoint();
            m_pressOnDisclosure = false;
            event->accept();
            return;
        }

        if (event && event->button() == Qt::LeftButton && m_suppressTextEditUntilRelease) {
            m_suppressTextEditUntilRelease = false;
            m_pressIndex = QModelIndex();
            m_pressPos = QPoint();
            m_pressOnDisclosure = false;
            m_pressWasSelected = false;
            QTreeView::mouseReleaseEvent(event);
            return;
        }

        const QModelIndex pressIndex = m_pressIndex;
        const QPoint pressPos = m_pressPos;
        const bool pressWasSelected = m_pressWasSelected;
        QTreeView::mouseReleaseEvent(event);

        if (event && event->button() == Qt::LeftButton &&
            event->modifiers() == Qt::NoModifier &&
            pressIndex.isValid() && indexAt(event->pos()) == pressIndex &&
            (event->pos() - pressPos).manhattanLength() <= QApplication::startDragDistance()) {
            scheduleTextEditorBySlowSecondClick(pressIndex, pressWasSelected);
        }

        m_pressIndex = QModelIndex();
        m_pressPos = QPoint();
        m_pressOnDisclosure = false;
        m_pressWasSelected = false;
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        cancelPendingSlowSecondClick();
        m_suppressTextEditUntilRelease = true;
        QTreeView::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event && event->matches(QKeySequence::Copy)) {
            QStringList lines;
            const QModelIndexList picked = selectionModel() ? selectionModel()->selectedRows(0) : QModelIndexList();
            if (!picked.isEmpty()) {
                for (const QModelIndex& idx : picked) {
                    if (idx.isValid()) lines << idx.data(Qt::DisplayRole).toString();
                }
            } else if (currentIndex().isValid()) {
                lines << currentIndex().data(Qt::DisplayRole).toString();
            }
            if (!lines.isEmpty() && QGuiApplication::clipboard()) {
                QGuiApplication::clipboard()->setText(lines.join(QStringLiteral("\n")));
                event->accept();
                return;
            }
        }
        QTreeView::keyPressEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (!event) return;
        const QMimeData* mime = event->mimeData();
        if (mime && mime->hasFormat(kMimeActiveRows)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        if (mime && mime->hasFormat(kMimeSignalIndexes)) {
            event->ignore();
            return;
        }
        QTreeView::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (!event) return;
        const QMimeData* mime = event->mimeData();
        if (mime && mime->hasFormat(kMimeActiveRows)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        if (mime && mime->hasFormat(kMimeSignalIndexes)) {
            event->ignore();
            return;
        }
        QTreeView::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        if (!event) return;
        const QMimeData* mime = event->mimeData();
        if (mime && mime->hasFormat(kMimeActiveRows)) {
            const QList<int> rows = decodeIntList(mime, kMimeActiveRows);
            if (!rows.isEmpty() && m_activeRowsDropped) {
                m_activeRowsDropped(rows);
                event->setDropAction(Qt::CopyAction);
                event->accept();
                return;
            }
        }
        if (mime && mime->hasFormat(kMimeSignalIndexes)) {
            event->ignore();
            return;
        }
        QTreeView::dropEvent(event);
    }

private:
    void scheduleTextEditorBySlowSecondClick(const QModelIndex& index, bool pressWasSelected) {
        if (!index.isValid() || index.column() != 0) return;
        if (!pressWasSelected || !m_slowSecondClickTimer) return;
        m_pendingSlowSecondClickIndex = QPersistentModelIndex(index);
        m_slowSecondClickTimer->start(QApplication::doubleClickInterval());
    }

    void cancelPendingSlowSecondClick() {
        if (m_slowSecondClickTimer) m_slowSecondClickTimer->stop();
        m_pendingSlowSecondClickIndex = QPersistentModelIndex();
    }

    QRect disclosureRectForIndex(const QModelIndex& index) const {
        if (!index.isValid() || !model() || !model()->hasChildren(index)) return QRect();

        const QRect itemRect = visualRect(index);
        if (!itemRect.isValid()) return QRect();

        QStyleOptionViewItem option;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        initViewItemOption(&option);
#else
        option = viewOptions();
#endif
        option.rect = itemRect;
        option.state |= QStyle::State_Children;
        if (isExpanded(index)) option.state |= QStyle::State_Open;

        QRect rect = style()->subElementRect(QStyle::SE_TreeViewDisclosureItem, &option, this);

        // Some style-sheet combinations make SE_TreeViewDisclosureItem empty or
        // place it over the item body. Fall back to the indentation strip just
        // before the text area; this keeps module rows visibly expandable.
        if (!rect.isValid() || rect.width() < 6 || rect.height() < 6 || rect.left() >= itemRect.left()) {
            const int box = qBound(10, indentation() - 6, 14);
            const int x = qMax(2, itemRect.left() - indentation() + (indentation() - box) / 2);
            const int y = itemRect.top() + (itemRect.height() - box) / 2;
            rect = QRect(x, y, box, box);
        }

        const int box = qBound(10, qMin(rect.width(), rect.height()), 14);
        return QRect(rect.center().x() - box / 2,
                     itemRect.top() + (itemRect.height() - box) / 2,
                     box, box);
    }

    bool isDisclosureArea(const QModelIndex& index, const QPoint& pos) const {
        const QRect rect = disclosureRectForIndex(index);
        return rect.isValid() && rect.adjusted(-3, -3, 3, 3).contains(pos);
    }

    void drawOneDisclosureIndicator(QPainter& painter, const QModelIndex& index) const {
        if (!index.isValid() || !model() || !model()->hasChildren(index)) return;
        const QRect box = disclosureRectForIndex(index);
        if (!box.isValid()) return;

        painter.setPen(QPen(QColor("#E8EDF3"), 1));
        painter.setBrush(QColor("#46515C"));
        painter.drawRoundedRect(box.adjusted(0, 0, -1, -1), 2, 2);

        const int cx = box.center().x();
        const int cy = box.center().y();
        const int half = qMax(3, box.width() / 3);
        painter.setPen(QPen(QColor("#FFFFFF"), 1.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(cx - half, cy, cx + half, cy);
        if (!isExpanded(index)) painter.drawLine(cx, cy - half, cx, cy + half);
    }

    void drawVisibleDisclosureIndicators(QPainter& painter, const QRect& dirty) const {
        if (!model() || !viewport()) return;
        const int step = qMax(1, sizeHintForRow(0));
        int y = qMax(0, dirty.top());
        const int maxY = qMin(viewport()->height() - 1, dirty.bottom());

        while (y <= maxY) {
            QModelIndex index = indexAt(QPoint(qMax(0, viewport()->width() / 2), y));
            if (!index.isValid()) index = indexAt(QPoint(qMax(0, indentation() + 4), y));
            if (!index.isValid()) index = indexAt(QPoint(0, y));

            if (!index.isValid()) {
                y += step;
                continue;
            }

            const QRect itemRect = visualRect(index);
            if (!itemRect.isValid()) {
                y += step;
                continue;
            }

            drawOneDisclosureIndicator(painter, index);
            y = itemRect.bottom() + 1;
        }
    }

private:
    ActiveRowsDroppedCallback m_activeRowsDropped;
    QModelIndex m_pressIndex;
    QPoint m_pressPos;
    bool m_pressOnDisclosure = false;
    bool m_pressWasSelected = false;
    bool m_suppressTextEditUntilRelease = false;
    QTimer* m_slowSecondClickTimer = nullptr;
    QPersistentModelIndex m_pendingSlowSecondClickIndex;
};

    ValueRadix textToFormat(const QString& s) {
        const QString t = s.trimmed().toLower();
        if (t == "hex") return ValueRadix::Hex;
        if (t == "dec") return ValueRadix::Dec;
        if (t == "int") return ValueRadix::Int;
        if (t == "uint" || t == "unsigned") return ValueRadix::UInt;
        if (t == "float") return ValueRadix::Float;
        if (t == "int64") return ValueRadix::Int64;
        if (t == "uint64" || t == "unsigned64") return ValueRadix::UInt64;
        if (t == "double") return ValueRadix::Double;
        return ValueRadix::Bin;
    }

    QString formatToText(ValueRadix r) {
        switch (r) {
        case ValueRadix::Bin: return "bin";
        case ValueRadix::Hex: return "hex";
        case ValueRadix::Dec: return "dec";
        case ValueRadix::Int: return "int";
        case ValueRadix::UInt: return "uint";
        case ValueRadix::Float: return "float";
        case ValueRadix::Int64: return "int64";
        case ValueRadix::UInt64: return "uint64";
        case ValueRadix::Double: return "double";
        }
        return "bin";
    }

    QSet<int> selectedTopLevelIndexes(QTreeWidget* tree) {
        QSet<int> indexes;
        if (!tree || !tree->selectionModel()) return indexes;
        const QModelIndexList picked = tree->selectionModel()->selectedRows(0);
        indexes.reserve(picked.size());
        for (const QModelIndex& index : picked) {
            if (index.isValid() && index.row() >= 0) indexes.insert(index.row());
        }
        return indexes;
    }

    bool allTopLevelRowsSelected(QTreeWidget* tree) {
        if (!tree || !tree->selectionModel()) return false;
        const int total = tree->topLevelItemCount();
        if (total <= 0) return false;

        QVector<QPair<int, int>> ranges;
        const QItemSelection selection = tree->selectionModel()->selection();
        ranges.reserve(selection.size());
        for (const QItemSelectionRange& range : selection) {
            if (!range.isValid() || range.left() > 0 || range.right() < 0) continue;
            const int top = qBound(0, range.top(), total - 1);
            const int bottom = qBound(0, range.bottom(), total - 1);
            if (bottom >= top) ranges.push_back(qMakePair(top, bottom));
        }
        if (ranges.isEmpty()) return false;
        std::sort(ranges.begin(), ranges.end(),
                  [](const QPair<int, int>& a, const QPair<int, int>& b) {
                      return a.first < b.first;
                  });

        int coveredThrough = -1;
        for (const QPair<int, int>& range : ranges) {
            if (range.first > coveredThrough + 1) return false;
            coveredThrough = qMax(coveredThrough, range.second);
            if (coveredThrough >= total - 1) return true;
        }
        return false;
    }

    void syncCanvasSelectionFromActiveList(WaveCanvas* canvas,
                                           QTreeWidget* activeList) {
        if (!canvas) return;
        if (allTopLevelRowsSelected(activeList)) {
            canvas->setAllEntriesSelected();
        } else {
            canvas->setSelectedEntryIndexes(
                selectedTopLevelIndexes(activeList));
        }
    }

    qint64 signalChangeTimeInRange(const WaveSignal& sig, qint64 start, qint64 end, bool firstEvent) {
        if (end <= start) return -1;
        if (sig.proceduralClock) {
            const qint64 time = firstEvent
                ? waveProceduralClockTransitionAtOrAfter(sig, start)
                : waveProceduralClockPreviousTransition(sig, end);
            return time >= start && time < end ? time : -1;
        }
        if (sig.changeTimesReady) {
            const QVector<qint64>& times = sig.changeTimes;
            if (times.isEmpty()) return -1;
            if (firstEvent) {
                const auto it = std::lower_bound(times.constBegin(), times.constEnd(), start);
                return (it != times.constEnd() && *it < end) ? *it : -1;
            }
            const auto it = std::lower_bound(times.constBegin(), times.constEnd(), end);
            if (it == times.constBegin()) return -1;
            const qint64 t = *(it - 1);
            return t >= start ? t : -1;
        }

        if (sig.samples.size() < 2) return -1;
        auto lowerSample = [](const QVector<WaveSample>& samples, qint64 t) {
            int lo = 0;
            int hi = samples.size();
            while (lo < hi) {
                const int mid = lo + (hi - lo) / 2;
                if (samples.at(mid).time < t) lo = mid + 1;
                else hi = mid;
            }
            return lo;
        };

        if (firstEvent) {
            const int begin = qMax(1, lowerSample(sig.samples, start));
            for (int i = begin; i < sig.samples.size(); ++i) {
                const qint64 t = sig.samples.at(i).time;
                if (t >= end) break;
                if (!waveSamplesEquivalent(sig.samples.at(i), sig.samples.at(i - 1))) return t;
            }
            return -1;
        }

        int i = lowerSample(sig.samples, end) - 1;
        i = qMin(i, sig.samples.size() - 1);
        for (; i >= 1; --i) {
            const qint64 t = sig.samples.at(i).time;
            if (t < start) break;
            if (!waveSamplesEquivalent(sig.samples.at(i), sig.samples.at(i - 1))) return t;
        }
        return -1;
    }

    struct ParsedValueFindTarget {
        quint64 bits = 0;
        bool negativeDecimal = false;
    };

    bool parseValueFindTargetText(const QString& text, ParsedValueFindTarget& target) {
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

    bool valueFindTargetForSignal(const ParsedValueFindTarget& target, int width, quint64& maskedBits) {
        if (width <= 0) return false;
        const quint64 mask = waveBitMaskForWidth(width);
        if (!target.negativeDecimal && width < 64 && target.bits > mask) return false;
        maskedBits = target.bits & mask;
        return true;
    }

    QString formatInternalDisplayTime(qint64 internalTime);

    qint64 clampedValueFindSegmentDuration(qint64 segmentStart, qint64 segmentEnd, qint64 waveStart, qint64 waveEnd) {
        if (waveEnd <= waveStart) return 0;
        const qint64 clippedStart = std::max(waveStart, std::min(segmentStart, waveEnd));
        const qint64 clippedEnd = std::max(waveStart, std::min(segmentEnd, waveEnd));
        return clippedEnd > clippedStart ? (clippedEnd - clippedStart) : 0;
    }

    QString formatValueFindTimeShare(qint64 matchedDuration, qint64 totalDuration) {
        if (totalDuration <= 0) return QStringLiteral("-");
        matchedDuration = std::max<qint64>(0, std::min(matchedDuration, totalDuration));
        const double percent = static_cast<double>(matchedDuration) * 100.0 / static_cast<double>(totalDuration);
        const int decimals = (matchedDuration > 0 && percent < 0.01) ? 4 : 2;
        return QStringLiteral("%1% (%2/%3)")
            .arg(QString::number(percent, 'f', decimals))
            .arg(formatInternalDisplayTime(matchedDuration))
            .arg(formatInternalDisplayTime(totalDuration));
    }

    QString stripDisplayRangeSuffix(QString name) {
        name = name.trimmed();
        const QString suffix = compareSideSuffix(name);
        if (!suffix.isEmpty()) name.chop(suffix.size());
        if (!name.endsWith(QLatin1Char(']'))) return name + suffix;
        const int open = name.lastIndexOf(QLatin1Char('['));
        if (open <= 0) return name + suffix;

        const QString body = name.mid(open + 1, name.size() - open - 2).trimmed();
        // Viewer width decoration is always "[msb:0]".  A single numeric
        // suffix such as "[0]" is a real array path segment and must remain
        // part of the signal name.
        if (body.isEmpty() || !body.contains(QLatin1Char(':'))) {
            return name + suffix;
        }
        bool ok = true;
        bool sawDigit = false;
        for (int i = 0; i < body.size(); ++i) {
            const QChar ch = body.at(i);
            if (ch.isDigit()) {
                sawDigit = true;
                continue;
            }
            if (ch == QLatin1Char(':') || ch.isSpace()) continue;
            ok = false;
            break;
        }
        if (!(ok && sawDigit)) return name + suffix;
        return name.left(open).trimmed() + suffix;
    }

    int literalWidthForValue(quint64 value) {
        int width = 1;
        while (width < 64 && (value >> width) != 0) ++width;
        return width;
    }

    enum class DerivedExprOp : quint8 {
        None,
        Positive,
        Negate,
        LogicalNot,
        BitwiseNot,
        Add,
        Subtract,
        Multiply,
        Divide,
        Modulo,
        BitwiseAnd,
        BitwiseOr,
        BitwiseXor,
        ShiftLeft,
        ShiftRight,
        LogicalAnd,
        LogicalOr,
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual
    };

    DerivedExprOp derivedExprOpFromText(const QString& op) {
        if (op == QStringLiteral("+")) return DerivedExprOp::Add;
        if (op == QStringLiteral("-")) return DerivedExprOp::Subtract;
        if (op == QStringLiteral("*")) return DerivedExprOp::Multiply;
        if (op == QStringLiteral("/")) return DerivedExprOp::Divide;
        if (op == QStringLiteral("%")) return DerivedExprOp::Modulo;
        if (op == QStringLiteral("&")) return DerivedExprOp::BitwiseAnd;
        if (op == QStringLiteral("|")) return DerivedExprOp::BitwiseOr;
        if (op == QStringLiteral("^")) return DerivedExprOp::BitwiseXor;
        if (op == QStringLiteral("<<")) return DerivedExprOp::ShiftLeft;
        if (op == QStringLiteral(">>")) return DerivedExprOp::ShiftRight;
        if (op == QStringLiteral("&&")) return DerivedExprOp::LogicalAnd;
        if (op == QStringLiteral("||")) return DerivedExprOp::LogicalOr;
        if (op == QStringLiteral("==")) return DerivedExprOp::Equal;
        if (op == QStringLiteral("!=")) return DerivedExprOp::NotEqual;
        if (op == QStringLiteral("<")) return DerivedExprOp::Less;
        if (op == QStringLiteral("<=")) return DerivedExprOp::LessEqual;
        if (op == QStringLiteral(">")) return DerivedExprOp::Greater;
        if (op == QStringLiteral(">=")) return DerivedExprOp::GreaterEqual;
        return DerivedExprOp::None;
    }

    struct DerivedExprNode {
        enum Kind {
            Literal,
            Signal,
            Unary,
            Binary
        };

        Kind kind = Literal;
        DerivedExprOp op = DerivedExprOp::None;
        quint64 literal = 0;
        int signalIndex = -1;
        int signalSlot = -1;
        int left = -1;
        int right = -1;
        int width = 1;
        quint64 mask = 1;
        quint64 dependencyMask = 0;
        bool hasOverflowDependency = false;
    };

    struct DerivedExpressionProgram {
        QVector<DerivedExprNode> nodes;
        QVector<int> dependencyIndexes;
        QVector<int> evaluationNodeIndexes;
        int root = -1;
        int inferredWidth = 1;
    };

    struct DerivedEvalValue {
        bool known = false;
        quint64 bits = 0;
    };

    class DerivedExpressionParser {
    public:
        using SignalResolver = std::function<bool(const QString&, int&, int&, QString&)>;

        DerivedExpressionParser(const QString& expression, SignalResolver resolver)
            : m_expression(expression), m_resolver(std::move(resolver)) {
            nextToken();
        }

        bool parse(DerivedExpressionProgram& out, QString& error) {
            m_error.clear();
            const int root = parseExpression(1);
            if (root < 0) {
                error = m_error.isEmpty() ? QStringLiteral("Invalid expression.") : m_error;
                return false;
            }
            if (m_token.type != End) {
                error = QStringLiteral("Unexpected token near '%1'.").arg(m_token.text);
                return false;
            }

            out.nodes = std::move(m_nodes);
            out.dependencyIndexes = std::move(m_dependencyIndexes);
            out.root = root;
            out.inferredWidth = qBound(1, out.nodes.at(root).width, 64);
            out.evaluationNodeIndexes.reserve(out.nodes.size());
            for (int nodeIndex = 0; nodeIndex < out.nodes.size(); ++nodeIndex) {
                const DerivedExprNode::Kind kind = out.nodes.at(nodeIndex).kind;
                if (kind == DerivedExprNode::Unary || kind == DerivedExprNode::Binary) {
                    out.evaluationNodeIndexes.push_back(nodeIndex);
                }
            }
            return true;
        }

    private:
        enum TokenType {
            End,
            Number,
            Identifier,
            Operator,
            LParen,
            RParen
        };

        struct Token {
            TokenType type = End;
            QString text;
            quint64 value = 0;
            int width = 1;
        };

        static bool isIdentStart(QChar ch) {
            return ch.isLetter() || ch == QLatin1Char('_') || ch == QLatin1Char('$');
        }

        static bool isIdentChar(QChar ch) {
            return ch.isLetterOrNumber() ||
                   ch == QLatin1Char('_') ||
                   ch == QLatin1Char('$') ||
                   ch == QLatin1Char('.') ||
                   ch == QLatin1Char('[') ||
                   ch == QLatin1Char(']') ||
                   ch == QLatin1Char(':');
        }

        void nextToken() {
            while (m_pos < m_expression.size() && m_expression.at(m_pos).isSpace()) ++m_pos;
            m_token = Token();
            if (m_pos >= m_expression.size()) {
                m_token.type = End;
                return;
            }

            const QChar ch = m_expression.at(m_pos);
            if (ch == QLatin1Char('(')) {
                ++m_pos;
                m_token.type = LParen;
                m_token.text = QStringLiteral("(");
                return;
            }
            if (ch == QLatin1Char(')')) {
                ++m_pos;
                m_token.type = RParen;
                m_token.text = QStringLiteral(")");
                return;
            }
            if (ch == QLatin1Char('`')) {
                const int start = ++m_pos;
                while (m_pos < m_expression.size() && m_expression.at(m_pos) != QLatin1Char('`')) ++m_pos;
                if (m_pos >= m_expression.size()) {
                    m_error = QStringLiteral("Missing closing backtick in signal name.");
                    m_token.type = End;
                    return;
                }
                m_token.type = Identifier;
                m_token.text = m_expression.mid(start, m_pos - start).trimmed();
                ++m_pos;
                return;
            }
            if (ch.isDigit()) {
                const int start = m_pos;
                if (ch == QLatin1Char('0') &&
                    m_pos + 1 < m_expression.size() &&
                    (m_expression.at(m_pos + 1) == QLatin1Char('x') || m_expression.at(m_pos + 1) == QLatin1Char('X'))) {
                    m_pos += 2;
                    const int bodyStart = m_pos;
                    while (m_pos < m_expression.size() && m_expression.at(m_pos).isLetterOrNumber()) ++m_pos;
                    const QString body = m_expression.mid(bodyStart, m_pos - bodyStart);
                    bool ok = false;
                    const quint64 value = body.toULongLong(&ok, 16);
                    if (!ok || !waveIsHexDigitsText(body)) {
                        m_error = QStringLiteral("Invalid hex literal near '%1'.").arg(m_expression.mid(start, m_pos - start));
                    }
                    m_token.type = Number;
                    m_token.text = m_expression.mid(start, m_pos - start);
                    m_token.value = value;
                    m_token.width = qBound(1, body.size() * 4, 64);
                    return;
                }
                if (ch == QLatin1Char('0') &&
                    m_pos + 1 < m_expression.size() &&
                    (m_expression.at(m_pos + 1) == QLatin1Char('b') || m_expression.at(m_pos + 1) == QLatin1Char('B'))) {
                    m_pos += 2;
                    const int bodyStart = m_pos;
                    while (m_pos < m_expression.size() && (m_expression.at(m_pos) == QLatin1Char('0') || m_expression.at(m_pos) == QLatin1Char('1'))) ++m_pos;
                    const QString body = m_expression.mid(bodyStart, m_pos - bodyStart);
                    bool ok = false;
                    const quint64 value = body.toULongLong(&ok, 2);
                    if (!ok || !waveIsBinaryDigitsText(body)) {
                        m_error = QStringLiteral("Invalid binary literal near '%1'.").arg(m_expression.mid(start, m_pos - start));
                    }
                    m_token.type = Number;
                    m_token.text = m_expression.mid(start, m_pos - start);
                    m_token.value = value;
                    m_token.width = qBound(1, body.size(), 64);
                    return;
                }

                while (m_pos < m_expression.size() && m_expression.at(m_pos).isDigit()) ++m_pos;
                const QString raw = m_expression.mid(start, m_pos - start);
                bool ok = false;
                const quint64 value = raw.toULongLong(&ok, 10);
                if (!ok) m_error = QStringLiteral("Invalid decimal literal near '%1'.").arg(raw);
                m_token.type = Number;
                m_token.text = raw;
                m_token.value = value;
                m_token.width = literalWidthForValue(value);
                return;
            }
            if (isIdentStart(ch)) {
                const int start = m_pos++;
                while (m_pos < m_expression.size() && isIdentChar(m_expression.at(m_pos))) ++m_pos;
                m_token.type = Identifier;
                m_token.text = m_expression.mid(start, m_pos - start).trimmed();
                return;
            }

            const QString two = (m_pos + 1 < m_expression.size()) ? m_expression.mid(m_pos, 2) : QString();
            if (two == QStringLiteral("&&") || two == QStringLiteral("||") ||
                two == QStringLiteral("==") || two == QStringLiteral("!=") ||
                two == QStringLiteral("<=") || two == QStringLiteral(">=") ||
                two == QStringLiteral("<<") || two == QStringLiteral(">>")) {
                m_pos += 2;
                m_token.type = Operator;
                m_token.text = two;
                return;
            }
            if (QStringLiteral("+-*/%&|^~!<>").contains(ch)) {
                ++m_pos;
                m_token.type = Operator;
                m_token.text = QString(ch);
                return;
            }

            m_error = QStringLiteral("Unsupported character '%1'.").arg(ch);
            ++m_pos;
            m_token.type = End;
        }

        int precedence(const QString& op) const {
            if (op == QStringLiteral("||")) return 1;
            if (op == QStringLiteral("&&")) return 2;
            if (op == QStringLiteral("|")) return 3;
            if (op == QStringLiteral("^")) return 4;
            if (op == QStringLiteral("&")) return 5;
            if (op == QStringLiteral("==") || op == QStringLiteral("!=")) return 6;
            if (op == QStringLiteral("<") || op == QStringLiteral("<=") ||
                op == QStringLiteral(">") || op == QStringLiteral(">=")) return 7;
            if (op == QStringLiteral("<<") || op == QStringLiteral(">>")) return 8;
            if (op == QStringLiteral("+") || op == QStringLiteral("-")) return 9;
            if (op == QStringLiteral("*") || op == QStringLiteral("/") || op == QStringLiteral("%")) return 10;
            return 0;
        }

        int addNode(DerivedExprNode node) {
            node.width = qBound(1, node.width, 64);
            node.mask = waveBitMaskForWidth(node.width);
            const int index = m_nodes.size();
            m_nodes.push_back(std::move(node));
            return index;
        }

        int parseExpression(int minPrecedence) {
            int lhs = parseUnary();
            if (lhs < 0) return -1;

            while (m_token.type == Operator) {
                const QString op = m_token.text;
                const int prec = precedence(op);
                if (prec < minPrecedence) break;
                nextToken();
                const int rhs = parseExpression(prec + 1);
                if (rhs < 0) return -1;

                DerivedExprNode node;
                node.kind = DerivedExprNode::Binary;
                node.op = derivedExprOpFromText(op);
                node.left = lhs;
                node.right = rhs;
                node.dependencyMask =
                    m_nodes.at(lhs).dependencyMask |
                    m_nodes.at(rhs).dependencyMask;
                node.hasOverflowDependency =
                    m_nodes.at(lhs).hasOverflowDependency ||
                    m_nodes.at(rhs).hasOverflowDependency;
                if (op == QStringLiteral("&&") || op == QStringLiteral("||") ||
                    op == QStringLiteral("==") || op == QStringLiteral("!=") ||
                    op == QStringLiteral("<") || op == QStringLiteral("<=") ||
                    op == QStringLiteral(">") || op == QStringLiteral(">=")) {
                    node.width = 1;
                } else if (op == QStringLiteral("<<") || op == QStringLiteral(">>")) {
                    node.width = m_nodes.at(lhs).width;
                } else {
                    node.width = qMax(m_nodes.at(lhs).width, m_nodes.at(rhs).width);
                }
                lhs = addNode(std::move(node));
            }
            return lhs;
        }

        int parseUnary() {
            if (m_token.type == Operator &&
                (m_token.text == QStringLiteral("+") ||
                 m_token.text == QStringLiteral("-") ||
                 m_token.text == QStringLiteral("!") ||
                 m_token.text == QStringLiteral("~"))) {
                const QString op = m_token.text;
                nextToken();
                const int child = parseUnary();
                if (child < 0) return -1;

                DerivedExprNode node;
                node.kind = DerivedExprNode::Unary;
                if (op == QStringLiteral("+")) node.op = DerivedExprOp::Positive;
                else if (op == QStringLiteral("-")) node.op = DerivedExprOp::Negate;
                else if (op == QStringLiteral("!")) node.op = DerivedExprOp::LogicalNot;
                else node.op = DerivedExprOp::BitwiseNot;
                node.left = child;
                node.dependencyMask = m_nodes.at(child).dependencyMask;
                node.hasOverflowDependency =
                    m_nodes.at(child).hasOverflowDependency;
                node.width = (op == QStringLiteral("!")) ? 1 : m_nodes.at(child).width;
                return addNode(std::move(node));
            }
            return parsePrimary();
        }

        int parsePrimary() {
            if (!m_error.isEmpty()) return -1;

            if (m_token.type == Number) {
                DerivedExprNode node;
                node.kind = DerivedExprNode::Literal;
                node.literal = m_token.value;
                node.width = m_token.width;
                nextToken();
                return addNode(std::move(node));
            }

            if (m_token.type == Identifier) {
                const QString name = m_token.text;
                int signalIndex = -1;
                int width = 1;
                QString resolveError;
                if (!m_resolver(name, signalIndex, width, resolveError)) {
                    m_error = resolveError.isEmpty() ? QStringLiteral("Unknown signal '%1'.").arg(name) : resolveError;
                    return -1;
                }

                int slot = m_depSlotBySignalIndex.value(signalIndex, -1);
                if (slot < 0) {
                    slot = m_dependencyIndexes.size();
                    m_dependencyIndexes.push_back(signalIndex);
                    m_depSlotBySignalIndex.insert(signalIndex, slot);
                }

                DerivedExprNode node;
                node.kind = DerivedExprNode::Signal;
                node.signalIndex = signalIndex;
                node.signalSlot = slot;
                if (slot < 64) node.dependencyMask = quint64(1) << slot;
                else node.hasOverflowDependency = true;
                node.width = width;
                nextToken();
                return addNode(std::move(node));
            }

            if (m_token.type == LParen) {
                nextToken();
                const int node = parseExpression(1);
                if (node < 0) return -1;
                if (m_token.type != RParen) {
                    m_error = QStringLiteral("Missing closing parenthesis.");
                    return -1;
                }
                nextToken();
                return node;
            }

            m_error = QStringLiteral("Expected a number, signal name, or parenthesized expression.");
            return -1;
        }

        QString m_expression;
        SignalResolver m_resolver;
        int m_pos = 0;
        Token m_token;
        QString m_error;
        QVector<DerivedExprNode> m_nodes;
        QVector<int> m_dependencyIndexes;
        QHash<int, int> m_depSlotBySignalIndex;
    };

    DerivedEvalValue evalDerivedExpression(const DerivedExpressionProgram& program,
                                           const QVector<DerivedEvalValue>& currentValues,
                                           QVector<DerivedEvalValue>& workspace,
                                           quint64 changedDependencyMask,
                                           bool forceAllDependencies,
                                           bool initialize) {
        if (program.root < 0 || program.root >= program.nodes.size()) return DerivedEvalValue();
        if (workspace.size() != program.nodes.size()) workspace.resize(program.nodes.size());

        // Literals never change.  Signal leaves are updated only when their
        // dependency changed; operator nodes below carry the union of their
        // input dependency bits and can skip unrelated branches.
        for (int nodeIndex = 0; nodeIndex < program.nodes.size(); ++nodeIndex) {
            const DerivedExprNode& node = program.nodes.at(nodeIndex);
            if (node.kind == DerivedExprNode::Literal) {
                if (!initialize) continue;
                DerivedEvalValue out;
                out.known = true;
                out.bits = node.literal & node.mask;
                workspace[nodeIndex] = out;
                continue;
            }
            if (node.kind == DerivedExprNode::Signal) {
                const bool changed = initialize || forceAllDependencies ||
                    node.hasOverflowDependency ||
                    (node.dependencyMask & changedDependencyMask) != 0;
                if (!changed) continue;
                DerivedEvalValue out;
                if (node.signalSlot >= 0 && node.signalSlot < currentValues.size()) {
                    out = currentValues.at(node.signalSlot);
                    if (out.known) out.bits &= node.mask;
                }
                workspace[nodeIndex] = out;
            }
        }

        for (int nodeIndex : program.evaluationNodeIndexes) {
            if (nodeIndex < 0 || nodeIndex >= program.nodes.size()) continue;
            const DerivedExprNode& node = program.nodes.at(nodeIndex);
            if (!initialize && !forceAllDependencies &&
                !node.hasOverflowDependency &&
                (node.dependencyMask & changedDependencyMask) == 0) {
                continue;
            }

            DerivedEvalValue out;

            if (node.left < 0 || node.left >= nodeIndex) {
                workspace[nodeIndex] = out;
                continue;
            }
            const DerivedEvalValue lhs = workspace.at(node.left);

            if (node.kind == DerivedExprNode::Unary) {
                if (lhs.known) {
                    quint64 bits = lhs.bits;
                    switch (node.op) {
                    case DerivedExprOp::Negate: bits = quint64(0) - bits; break;
                    case DerivedExprOp::BitwiseNot: bits = ~bits; break;
                    case DerivedExprOp::LogicalNot: bits = bits ? 0ull : 1ull; break;
                    default: break;
                    }
                    out.known = true;
                    out.bits = bits & node.mask;
                }
                workspace[nodeIndex] = out;
                continue;
            }

            if (node.right < 0 || node.right >= nodeIndex) {
                workspace[nodeIndex] = out;
                continue;
            }
            const DerivedEvalValue rhs = workspace.at(node.right);
            if (!lhs.known || !rhs.known) {
                workspace[nodeIndex] = out;
                continue;
            }

            quint64 bits = 0;
            bool valid = true;
            switch (node.op) {
            case DerivedExprOp::Add: bits = lhs.bits + rhs.bits; break;
            case DerivedExprOp::Subtract: bits = lhs.bits - rhs.bits; break;
            case DerivedExprOp::Multiply: bits = lhs.bits * rhs.bits; break;
            case DerivedExprOp::Divide:
                if (rhs.bits == 0) valid = false;
                else bits = lhs.bits / rhs.bits;
                break;
            case DerivedExprOp::Modulo:
                if (rhs.bits == 0) valid = false;
                else bits = lhs.bits % rhs.bits;
                break;
            case DerivedExprOp::BitwiseAnd: bits = lhs.bits & rhs.bits; break;
            case DerivedExprOp::BitwiseOr: bits = lhs.bits | rhs.bits; break;
            case DerivedExprOp::BitwiseXor: bits = lhs.bits ^ rhs.bits; break;
            case DerivedExprOp::ShiftLeft: bits = rhs.bits >= 64 ? 0ull : lhs.bits << int(rhs.bits); break;
            case DerivedExprOp::ShiftRight: bits = rhs.bits >= 64 ? 0ull : lhs.bits >> int(rhs.bits); break;
            case DerivedExprOp::LogicalAnd: bits = lhs.bits != 0 && rhs.bits != 0 ? 1ull : 0ull; break;
            case DerivedExprOp::LogicalOr: bits = lhs.bits != 0 || rhs.bits != 0 ? 1ull : 0ull; break;
            case DerivedExprOp::Equal: bits = lhs.bits == rhs.bits ? 1ull : 0ull; break;
            case DerivedExprOp::NotEqual: bits = lhs.bits != rhs.bits ? 1ull : 0ull; break;
            case DerivedExprOp::Less: bits = lhs.bits < rhs.bits ? 1ull : 0ull; break;
            case DerivedExprOp::LessEqual: bits = lhs.bits <= rhs.bits ? 1ull : 0ull; break;
            case DerivedExprOp::Greater: bits = lhs.bits > rhs.bits ? 1ull : 0ull; break;
            case DerivedExprOp::GreaterEqual: bits = lhs.bits >= rhs.bits ? 1ull : 0ull; break;
            default: valid = false; break;
            }
            if (valid) {
                out.known = true;
                out.bits = bits & node.mask;
            }
            workspace[nodeIndex] = out;
        }

        return workspace.at(program.root);
    }

    WaveSample makeDerivedSample(qint64 time, const DerivedEvalValue& value, int width) {
        WaveSample sample;
        sample.time = time;
        sample.rawFieldsReady = true;
        if (!value.known) {
            sample.isZ = true;
            return sample;
        }

        sample.rawBits = value.bits & waveBitMaskForWidth(width);
        return sample;
    }

    qint64 derivedLodWindowStart(qint64 time, qint64 bucketCycles) {
        if (bucketCycles <= 0) return time;
        if (time >= 0) return (time / bucketCycles) * bucketCycles;
        const qint64 quotient = ((time + 1) / bucketCycles) - 1;
        return quotient * bucketCycles;
    }

    QVector<qint64> derivedLodBucketCycles(const WaveFile& wave,
                                           const QVector<int>& dependencyIndexes) {
        QVector<qint64> buckets;
        QSet<qint64> seen;
        auto collect = [&](const WaveSignal& signal) {
            for (const WaveLodLevel& level : signal.lodLevels) {
                if (level.bucketCycles <= 0 || seen.contains(level.bucketCycles)) continue;
                seen.insert(level.bucketCycles);
                buckets.push_back(level.bucketCycles);
            }
        };

        for (int signalIndex : dependencyIndexes) {
            if (signalIndex < 0 || signalIndex >= wave.signalList.size()) continue;
            collect(wave.signalList.at(signalIndex));
        }
        if (buckets.isEmpty()) {
            buckets = QVector<qint64>{10, 100, 1000, 10000};
        }
        std::sort(buckets.begin(), buckets.end());
        return buckets;
    }

    QVector<WaveLodLevel> buildDerivedLodLevels(const QVector<WaveSample>& samples,
                                                const QVector<qint64>& bucketCycles) {
        QVector<WaveLodLevel> levels;
        if (samples.isEmpty()) return levels;

        struct PendingLevel {
            WaveLodLevel level;
            qint64 windowStart = 0;
            WaveSample pending;
            bool hasPending = false;
        };

        QVector<PendingLevel> pendingLevels;
        pendingLevels.resize(bucketCycles.size());
        auto setPendingSample = [](PendingLevel& pendingLevel, const WaveSample& sample) {
            pendingLevel.pending.time = sample.time;
            pendingLevel.pending.value.clear();
            pendingLevel.pending.rawBits = sample.rawBits;
            pendingLevel.pending.isZ = sample.isZ;
            pendingLevel.pending.isAbsent = sample.isAbsent;
            pendingLevel.pending.rawFieldsReady = sample.rawFieldsReady;
        };
        for (int i = 0; i < bucketCycles.size(); ++i) {
            PendingLevel& pendingLevel = pendingLevels[i];
            pendingLevel.level.bucketCycles = bucketCycles.at(i);

            const long double firstWindow = static_cast<long double>(
                derivedLodWindowStart(samples.constFirst().time, bucketCycles.at(i)));
            const long double lastWindow = static_cast<long double>(
                derivedLodWindowStart(samples.constLast().time, bucketCycles.at(i)));
            const long double possibleWindows =
                qMax<long double>(1.0L, ((lastWindow - firstWindow) / bucketCycles.at(i)) + 1.0L);
            const int reserveCount = int(qMin<long double>(
                static_cast<long double>(samples.size()),
                qMin<long double>(possibleWindows, std::numeric_limits<int>::max())));
            pendingLevel.level.samples.reserve(reserveCount);
        }

        for (const WaveSample& sample : samples) {
            for (PendingLevel& pendingLevel : pendingLevels) {
                const qint64 windowStart =
                    derivedLodWindowStart(sample.time, pendingLevel.level.bucketCycles);
                if (!pendingLevel.hasPending) {
                    pendingLevel.windowStart = windowStart;
                    setPendingSample(pendingLevel, sample);
                    pendingLevel.hasPending = true;
                    continue;
                }
                if (windowStart == pendingLevel.windowStart) {
                    setPendingSample(pendingLevel, sample);
                    continue;
                }
                pendingLevel.level.samples.push_back(std::move(pendingLevel.pending));
                pendingLevel.windowStart = windowStart;
                setPendingSample(pendingLevel, sample);
            }
        }

        levels.reserve(pendingLevels.size());
        for (PendingLevel& pendingLevel : pendingLevels) {
            if (pendingLevel.hasPending) {
                pendingLevel.level.samples.push_back(std::move(pendingLevel.pending));
            }
            levels.push_back(std::move(pendingLevel.level));
        }
        return levels;
    }

    QString formatInternalDisplayTime(qint64 internalTime) {
        return waveFormatDisplayTime(internalTime);
    }

    QColor iconColor(const QString& name) {
        if (name == "open") return QColor("#43C59E");
        if (name == "compare") return QColor("#B86BFF");
        if (name == "derive") return QColor("#2EBAC6");
        if (name == "save") return QColor("#5CA8FF");
        if (name == "zoom_in" || name == "zoom_out") return QColor("#F0A43A");
        if (name == "left" || name == "right") return QColor("#55B4FF");
        if (name == "prev_change" || name == "next_change") return QColor("#62C462");
        if (name == "reset") return QColor("#FF7D57");
        return QColor("#7A91AA");
    }

    QIcon makeColorfulToolbarIcon(const QString& kind) {
        QPixmap pm(28, 28);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QColor base = iconColor(kind);
        const QColor light = base.lighter(155);
        const QColor mid = base.lighter(115);
        const QColor dark = base.darker(150);

        auto fillGradient = [&](const QRectF& r) {
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0.0, light);
            g.setColorAt(0.45, mid);
            g.setColorAt(1.0, dark);
            return g;
        };

        auto strokePen = QPen(dark, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

        if (kind == "open") {
            QRectF body(4, 8, 20, 13);
            p.setPen(strokePen);
            p.setBrush(fillGradient(body));
            p.drawRoundedRect(body, 3, 3);
            p.setPen(QPen(QColor(255, 255, 255, 160), 1.2));
            p.drawLine(QPointF(7, 9), QPointF(18, 9));
            p.setPen(QPen(dark, 1.8));
            p.drawLine(7, 8, 10, 5); p.drawLine(10, 5, 16, 5); p.drawLine(16, 5, 19, 8);
        }
        else if (kind == "compare") {
            QRectF leftDoc(4.5, 5.0, 10.5, 16.0);
            QRectF rightDoc(13.0, 7.0, 10.5, 16.0);
            p.setPen(strokePen);
            p.setBrush(fillGradient(leftDoc));
            p.drawRoundedRect(leftDoc, 2.5, 2.5);
            p.setBrush(fillGradient(rightDoc));
            p.drawRoundedRect(rightDoc, 2.5, 2.5);
            p.setPen(QPen(QColor(255, 255, 255, 185), 1.1, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(7, 10, 12, 10);
            p.drawLine(7, 14, 12, 14);
            p.drawLine(16, 12, 21, 12);
            p.drawLine(16, 16, 21, 16);
            p.setPen(QPen(QColor("#FF4D4F"), 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(11, 22, 17, 4);
        }
        else if (kind == "derive") {
            p.setPen(QPen(dark, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(fillGradient(QRectF(5, 5, 18, 18)));
            p.drawRoundedRect(QRectF(5, 5, 18, 18), 3, 3);
            p.setPen(QPen(QColor(255, 255, 255, 190), 1.7, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(9, 14, 13, 10);
            p.drawLine(9, 14, 13, 18);
            p.drawLine(15, 10, 19, 14);
            p.drawLine(15, 18, 19, 14);
            p.setPen(QPen(dark, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(9, 8, 19, 8);
            p.drawLine(9, 20, 19, 20);
        }
        else if (kind == "save") {
            QRectF body(5, 4, 18, 20);
            p.setPen(strokePen);
            p.setBrush(fillGradient(body));
            p.drawRoundedRect(body, 3, 3);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1.2));
            p.drawLine(8, 7, 20, 7);
            p.setPen(QPen(dark, 1.8));
            p.drawLine(9, 8, 18, 8); p.drawLine(9, 12, 18, 12); p.drawLine(11, 17, 17, 17);
        }
        else if (kind == "zoom_in" || kind == "zoom_out") {
            QRectF lens(4, 4, 13, 13);
            p.setPen(QPen(dark, 1.8));
            p.setBrush(fillGradient(lens));
            p.drawEllipse(lens);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1.0));
            p.drawArc(QRectF(5.5, 5.5, 8, 6), 20 * 16, 120 * 16);
            p.setPen(QPen(dark, 2.1, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(16, 16, 23, 23);
            if (kind == "zoom_in") p.drawLine(10.5, 7, 10.5, 13);
            p.drawLine(7.5, 10, 13.5, 10);
        }
        else if (kind == "left" || kind == "right") {
            QPainterPath path;
            if (kind == "left") {
                path.moveTo(9, 14); path.lineTo(17, 7); path.lineTo(17, 11); path.lineTo(24, 11); path.lineTo(24, 17); path.lineTo(17, 17); path.lineTo(17, 21); path.closeSubpath();
            }
            else {
                path.moveTo(19, 14); path.lineTo(11, 7); path.lineTo(11, 11); path.lineTo(4, 11); path.lineTo(4, 17); path.lineTo(11, 17); path.lineTo(11, 21); path.closeSubpath();
            }
            p.setPen(strokePen);
            QRectF br = path.boundingRect();
            p.setBrush(fillGradient(br));
            p.drawPath(path);
            p.setPen(QPen(QColor(255, 255, 255, 130), 1.0));
            p.drawLine(QPointF(br.left() + 2, br.top() + 2), QPointF(br.right() - 2, br.top() + 2));
        }
        else if (kind == "prev_change" || kind == "next_change") {
            p.setPen(QPen(dark, 1.8, Qt::SolidLine, Qt::RoundCap));
            if (kind == "prev_change") {
                p.drawLine(7, 6, 7, 22);
                QPainterPath path; path.moveTo(21, 8); path.lineTo(12, 14); path.lineTo(21, 20); path.closeSubpath();
                QRectF br = path.boundingRect();
                p.setBrush(fillGradient(br));
                p.drawPath(path);
            }
            else {
                p.drawLine(21, 6, 21, 22);
                QPainterPath path; path.moveTo(7, 8); path.lineTo(16, 14); path.lineTo(7, 20); path.closeSubpath();
                QRectF br = path.boundingRect();
                p.setBrush(fillGradient(br));
                p.drawPath(path);
            }
        }
        else if (kind == "reset") {
            p.setPen(QPen(dark, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(5, 5, 18, 18), 35 * 16, 285 * 16);
            QPainterPath path; path.moveTo(18, 5); path.lineTo(24, 6); path.lineTo(22, 12); path.closeSubpath();
            QRectF br = path.boundingRect();
            p.setBrush(fillGradient(br));
            p.drawPath(path);
        }
        return QIcon(pm);
    }

    QIcon makeAppIcon() {
        QPixmap pm(64, 64);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#20354E"));
        p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);
        p.setBrush(QColor("#30C56C"));
        p.drawRoundedRect(QRectF(10, 36, 18, 10), 3, 3);
        p.drawRoundedRect(QRectF(28, 20, 24, 10), 3, 3);
        p.setPen(QPen(QColor("#FFFFFF"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(10, 41, 18, 41);
        p.drawLine(28, 25, 52, 25);
        p.drawLine(28, 25, 28, 41);
        p.drawLine(28, 41, 42, 41);
        p.drawLine(42, 41, 42, 14);
        p.drawLine(42, 14, 52, 14);
        return QIcon(pm);
    }

    QIcon loadImageIconOrFallback(const QString& baseName, const QString& fallbackKind) {
        const QStringList candidates{
            QCoreApplication::applicationDirPath() + "/icons/" + baseName + ".png",
            QFileInfo(QCoreApplication::applicationDirPath() + "/../icons/" + baseName + ".png").absoluteFilePath()
        };
        for (const QString& path : candidates) {
            if (QFileInfo::exists(path)) {
                QPixmap pm(path);
                if (!pm.isNull()) return QIcon(pm);
            }
        }
        return makeColorfulToolbarIcon(fallbackKind);
    }

}

bool agentRpcRuntimeAvailable() {
#if defined(Q_OS_WIN)
#if defined(_DEBUG)
    const QString dllName = QStringLiteral("Qt5Networkd.dll");
#else
    const QString dllName = QStringLiteral("Qt5Network.dll");
#endif
    const QString dllPath = QCoreApplication::applicationDirPath() + QLatin1Char('/') + dllName;
    if (!QFileInfo::exists(dllPath)) return false;
    static QLibrary networkRuntime(dllPath);
    return networkRuntime.isLoaded() || networkRuntime.load();
#else
    return true;
#endif
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowIcon(loadImageIconOrFallback("app", "app"));
    if (windowIcon().isNull()) setWindowIcon(makeAppIcon());
    buildUi();
    setAcceptDrops(true);
    const QList<QWidget*> dropTargets = { m_central, m_splitter, m_tree, m_activeList, m_canvas };
    for (QWidget* widget : dropTargets) {
        if (!widget) continue;
        widget->setAcceptDrops(true);
        widget->installEventFilter(this);
    }
    applyTheme();
    loadDemoWave();
    if (qEnvironmentVariableIntValue("WAVEVIEWER_AGENT_DISABLE") == 0 && agentRpcRuntimeAvailable()) {
        m_agentRpcServer.reset(new AgentRpcServer(this, this));
        QString rpcError;
        if (!m_agentRpcServer->start(qEnvironmentVariable("WAVEVIEWER_AGENT_NAME"), &rpcError)) {
            statusBar()->showMessage(QStringLiteral("Agent API unavailable: %1").arg(rpcError), 5000);
        }
    } else if (qEnvironmentVariableIntValue("WAVEVIEWER_AGENT_DISABLE") == 0) {
        statusBar()->showMessage(QStringLiteral("Agent API disabled: Qt5Network runtime is not installed."), 5000);
    }
}

MainWindow::~MainWindow() {
    stopSignalConditionSearch();
    ++m_viewportLoadSerial;
    if (m_viewportLoadTimer) m_viewportLoadTimer->stop();
    if (m_viewportLoadThread.joinable()) m_viewportLoadThread.join();
    if (m_blockCacheLoader) m_blockCacheLoader->stop();
    stopTreeWarmup();
}

static QString viewerSessionSettingsApplicationName() {
    const QString testNamespace =
        qEnvironmentVariable("WV_VIEWER_SESSION_NAMESPACE").trimmed();
    return testNamespace.isEmpty()
        ? QStringLiteral("WaveViewer")
        : QStringLiteral("WaveViewer_%1").arg(testNamespace);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveViewerSessionState();
    QMainWindow::closeEvent(event);
}

void MainWindow::saveViewerSessionState() const {
    if (m_currentWaveFilePath.isEmpty() || !m_activeList || !m_canvas) return;

    QSettings settings(QStringLiteral("WaveTrace"),
                       viewerSessionSettingsApplicationName());
    settings.beginGroup(QStringLiteral("viewerSessionV1"));
    settings.remove(QString());
    settings.setValue(QStringLiteral("valid"), true);
    settings.setValue(QStringLiteral("sourceFile"),
                      QFileInfo(m_currentWaveFilePath).absoluteFilePath());

    QStringList expandedPaths = m_userExpandedNodePaths.values();
    std::sort(expandedPaths.begin(), expandedPaths.end());
    settings.setValue(QStringLiteral("expandedNodes"), expandedPaths);

    const QSet<int> selectedRows = selectedTopLevelIndexes(m_activeList);
    const int currentRow = m_activeList->currentIndex().isValid()
        ? m_activeList->currentIndex().row()
        : -1;
    settings.beginWriteArray(QStringLiteral("activeSignals"));
    for (int row = 0; row < m_activeList->topLevelItemCount(); ++row) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(row);
        const int signalIndex = signalIndexFromActiveItem(item);
        const QString path = signalDisplayName(signalIndex);
        if (path.isEmpty()) continue;
        settings.setArrayIndex(row);
        settings.setValue(QStringLiteral("path"), path);
        settings.setValue(QStringLiteral("format"),
                          item->data(0, RoleCurrentFormat).toString());
        settings.setValue(QStringLiteral("selected"), selectedRows.contains(row));
        settings.setValue(QStringLiteral("current"), row == currentRow);
    }
    settings.endArray();

    settings.setValue(QStringLiteral("viewStart"), m_canvas->viewStart());
    settings.setValue(QStringLiteral("viewEnd"), m_canvas->viewEnd());
    settings.setValue(QStringLiteral("hasCursor"), m_canvas->cursorTime() >= 0);
    settings.setValue(QStringLiteral("cursor"), m_canvas->cursorTime());
    settings.endGroup();
    settings.sync();
}

bool MainWindow::loadViewerSessionState() {
    m_userExpandedNodePaths.clear();
    m_pendingSessionSignals.clear();
    m_pendingSessionSignalRestore = false;

    QSettings settings(QStringLiteral("WaveTrace"),
                       viewerSessionSettingsApplicationName());
    settings.beginGroup(QStringLiteral("viewerSessionV1"));
    const bool valid = settings.value(QStringLiteral("valid"), false).toBool();
    if (!valid) {
        settings.endGroup();
        return false;
    }

    const QStringList expandedPaths =
        settings.value(QStringLiteral("expandedNodes")).toStringList();
    for (const QString& path : expandedPaths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty()) m_userExpandedNodePaths.insert(trimmed);
    }

    const int activeCount = settings.beginReadArray(QStringLiteral("activeSignals"));
    m_pendingSessionSignals.reserve(activeCount);
    for (int row = 0; row < activeCount; ++row) {
        settings.setArrayIndex(row);
        ViewerSessionSignal signal;
        signal.path = settings.value(QStringLiteral("path")).toString().trimmed();
        signal.format = settings.value(QStringLiteral("format"),
                                       QStringLiteral("bin")).toString();
        signal.selected = settings.value(QStringLiteral("selected"), false).toBool();
        signal.current = settings.value(QStringLiteral("current"), false).toBool();
        if (!signal.path.isEmpty()) m_pendingSessionSignals.push_back(signal);
    }
    settings.endArray();

    const qint64 savedViewStart =
        settings.value(QStringLiteral("viewStart"), m_wave.meta.start).toLongLong();
    const qint64 savedViewEnd =
        settings.value(QStringLiteral("viewEnd"), m_wave.meta.end).toLongLong();
    const bool hasCursor = settings.value(QStringLiteral("hasCursor"), false).toBool();
    const qint64 savedCursor = settings.value(QStringLiteral("cursor"), -1).toLongLong();
    settings.endGroup();

    restoreUserTreeExpansionState();
    m_pendingSessionSignalRestore = !m_pendingSessionSignals.isEmpty();
    retryPendingViewerSessionRestore();

    if (m_canvas && savedViewEnd > savedViewStart &&
        savedViewStart >= m_wave.meta.start && savedViewEnd <= m_wave.meta.end) {
        m_canvas->commitViewportRange(savedViewStart, savedViewEnd);
    }
    if (hasCursor && m_canvas &&
        savedCursor >= m_wave.meta.start && savedCursor <= m_wave.meta.end) {
        m_canvas->setCursorTime(savedCursor);
    }
    return true;
}

bool MainWindow::runViewerSessionStateRegressionForBenchmark(QString* error) {
    if (error) error->clear();
    if (m_currentWaveFilePath.isEmpty() || !m_tree || !m_treeModel ||
        !m_signalTreeModel || !m_activeList || !m_canvas ||
        m_wave.signalList.size() < 2) {
        if (error) *error = QStringLiteral("Viewer state test prerequisites are missing");
        return false;
    }

    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    const QModelIndex root = model ? model->index(0, 0) : QModelIndex();
    if (!root.isValid() || !model->hasChildren(root)) {
        if (error) *error = QStringLiteral("Viewer state test needs a container root");
        return false;
    }
    m_tree->expand(root);

    m_activeList->clear();
    addSignalIndexesToActive(QList<int>() << 0 << 1);
    if (m_activeList->topLevelItemCount() != 2) {
        if (error) *error = QStringLiteral("Unable to prepare active signals");
        return false;
    }
    m_activeList->topLevelItem(0)->setData(0, RoleCurrentFormat,
                                          QStringLiteral("hex"));
    m_activeList->topLevelItem(1)->setData(0, RoleCurrentFormat,
                                          QStringLiteral("uint"));
    if (QItemSelectionModel* selection = m_activeList->selectionModel()) {
        selection->clearSelection();
        selection->select(m_activeList->model()->index(1, 0),
                          QItemSelectionModel::Select |
                          QItemSelectionModel::Rows);
        selection->setCurrentIndex(m_activeList->model()->index(1, 0),
                                   QItemSelectionModel::NoUpdate);
    }
    rebuildVisibleSignals();

    const qint64 fullStart = m_canvas->fullStartTime();
    const qint64 fullEnd = m_canvas->fullEndTime();
    const qint64 fullSpan = fullEnd - fullStart;
    if (fullSpan < 4) {
        if (error) *error = QStringLiteral("Viewer state test needs a wider time range");
        return false;
    }
    const qint64 savedStart = fullStart + fullSpan / 4;
    const qint64 savedEnd = fullStart + (fullSpan * 3) / 4;
    const qint64 savedCursor = savedStart + (savedEnd - savedStart) / 2;
    m_canvas->commitViewportRange(savedStart, savedEnd);
    m_canvas->setCursorTime(savedCursor);
    saveViewerSessionState();

    m_applyingTreeExpansionState = true;
    m_tree->collapseAll();
    m_applyingTreeExpansionState = false;
    m_userExpandedNodePaths.clear();
    m_activeList->clear();
    m_canvas->resetView();
    if (!loadViewerSessionState()) {
        if (error) *error = QStringLiteral("Saved Viewer state was not found");
        return false;
    }

    const QSet<int> selectedRows = selectedTopLevelIndexes(m_activeList);
    const bool ok = m_tree->isExpanded(root) &&
        m_activeList->topLevelItemCount() == 2 &&
        signalIndexFromActiveItem(m_activeList->topLevelItem(0)) == 0 &&
        signalIndexFromActiveItem(m_activeList->topLevelItem(1)) == 1 &&
        m_activeList->topLevelItem(0)->data(0, RoleCurrentFormat).toString() ==
            QStringLiteral("hex") &&
        m_activeList->topLevelItem(1)->data(0, RoleCurrentFormat).toString() ==
            QStringLiteral("uint") &&
        selectedRows == QSet<int>() << 1 &&
        m_canvas->viewStart() == savedStart &&
        m_canvas->viewEnd() == savedEnd &&
        m_canvas->cursorTime() == savedCursor;
    if (!ok && error) {
        *error = QStringLiteral("Expanded nodes, active selection, formats, viewport, or cursor did not round-trip");
    }
    return ok;
}

bool MainWindow::runCompareActivationOrderRegressionForBenchmark(QString* error) {
    if (error) error->clear();

    auto makeEventModeSignal = [](const QVector<qint64>& times,
                                  const QVector<quint64>& values) {
        WaveSignal signal;
        signal.kind = SignalKind::Bit;
        signal.width = 1;
        signal.samplesLoaded = true;
        for (int i = 0; i < times.size(); ++i) {
            WaveSample sample;
            sample.time = times.at(i);
            sample.rawBits = values.at(i);
            sample.rawFieldsReady = true;
            signal.samples.push_back(sample);
        }
        return signal;
    };
    const WaveSignal sameTimesLeft = makeEventModeSignal(
        QVector<qint64>{0, 10, 20}, QVector<quint64>{0, 1, 0});
    const WaveSignal sameTimesDifferentValues = makeEventModeSignal(
        QVector<qint64>{0, 10, 20}, QVector<quint64>{1, 0, 1});
    const WaveSignal shiftedEvent = makeEventModeSignal(
        QVector<qint64>{0, 11, 20}, QVector<quint64>{0, 1, 0});
    if (!computeSignalEventTimeDiffRegions(
             sameTimesLeft, sameTimesDifferentValues, 0, 100).isEmpty() ||
        computeSignalDiffRegions(
             sameTimesLeft, sameTimesDifferentValues, 0, 100, 100, 100).isEmpty() ||
        computeSignalEventTimeDiffRegions(
             sameTimesLeft, shiftedEvent, 0, 100).isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "Event-time-only comparison did not ignore values or detect shifted events");
        }
        return false;
    }

    WaveFile wave;
    wave.meta.title = QStringLiteral("compare_activation_order_regression");
    wave.meta.timescale = QStringLiteral("cycle");
    wave.meta.start = 0;
    wave.meta.end = 100;
    wave.meta.compareLeftPath = QStringLiteral("left.wvz4");
    wave.meta.compareRightPath = QStringLiteral("right.wvz4");

    auto appendSide = [&wave](const QString& name, qint64 firstDifference,
                              int signalId) {
        WaveSignal signal;
        signal.signalId = signalId;
        signal.name = name;
        signal.kind = SignalKind::Bit;
        signal.width = 1;
        signal.defaultRadix = ValueRadix::Bin;
        signal.currentRadix = ValueRadix::Bin;
        signal.samplesLoaded = true;
        WaveSample sample;
        sample.time = 0;
        sample.value = QStringLiteral("0");
        hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
        signal.samples.push_back(sample);
        WaveDiffRegion region;
        region.start = firstDifference;
        region.end = firstDifference + 1;
        signal.diffRegions.push_back(region);
        wave.signalList.push_back(std::move(signal));
    };
    auto appendPair = [&appendSide](const QString& path, qint64 firstDifference,
                                    int firstSignalId) {
        appendSide(path + QStringLiteral("@A"), firstDifference, firstSignalId);
        appendSide(path + QStringLiteral("@B"), firstDifference, firstSignalId + 1);
    };

    // Deliberately emit path order that differs from first-difference order.
    appendPair(QStringLiteral("top.late"), 70, 1);
    appendPair(QStringLiteral("top.early"), 10, 3);
    appendPair(QStringLiteral("top.middle"), 40, 5);

    m_currentWaveFilePath.clear();
    m_currentWaveSupportsOnDemand = false;
    applyWave(std::move(wave));
    if (!m_activeList || m_activeList->topLevelItemCount() != 0) {
        if (error) *error = QStringLiteral("Comparison result was activated automatically");
        return false;
    }
    QList<int> indexes;
    for (int signalIndex = 0; signalIndex < m_wave.signalList.size(); ++signalIndex) {
        indexes.push_back(signalIndex);
    }
    addSignalIndexesToActive(indexes);
    if (findChild<QPushButton*>(
            QStringLiteral("sortActiveByFirstDifferenceButton"))) {
        if (error) *error = QStringLiteral("Obsolete first-difference sort button still exists");
        return false;
    }

    const QStringList expectedNames = {
        QStringLiteral("top.early@A"), QStringLiteral("top.early@B"),
        QStringLiteral("top.middle@A"), QStringLiteral("top.middle@B"),
        QStringLiteral("top.late@A"), QStringLiteral("top.late@B")
    };
    if (!m_activeList || m_activeList->topLevelItemCount() != expectedNames.size()) {
        if (error) *error = QStringLiteral("Not every differing A/B signal was activated");
        return false;
    }

    qint64 previousFirstDifference = (std::numeric_limits<qint64>::min)();
    for (int row = 0; row < expectedNames.size(); ++row) {
        const int signalIndex = signalIndexFromActiveItem(m_activeList->topLevelItem(row));
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) {
            if (error) *error = QStringLiteral("Active comparison row has an invalid signal index");
            return false;
        }
        const WaveSignal& signal = m_wave.signalList.at(signalIndex);
        const qint64 firstDifference = signal.diffRegions.isEmpty()
            ? (std::numeric_limits<qint64>::max)()
            : signal.diffRegions.first().start;
        if (signal.name != expectedNames.at(row) ||
            firstDifference < previousFirstDifference) {
            if (error) {
                *error = QStringLiteral("Comparison rows are not stably sorted by first difference");
            }
            return false;
        }
        previousFirstDifference = firstDifference;
    }

    if (selectedTopLevelIndexes(m_activeList).size() != expectedNames.size()) {
        if (error) *error = QStringLiteral("Activated comparison rows were not all selected");
        return false;
    }

    // Stress the real failure mode: comparison activation selects every row.
    // Restoring those rows one-by-one used to make selection maintenance
    // quadratic and freeze the UI for large comparisons.
    constexpr int kStressSignalCount = 20000;
    WaveFile stressWave;
    stressWave.meta.title = QStringLiteral("compare_first_difference_sort_stress");
    stressWave.meta.timescale = QStringLiteral("cycle");
    stressWave.meta.start = 0;
    stressWave.meta.end = kStressSignalCount + 1;
    stressWave.meta.compareLeftPath = QStringLiteral("stress_left.wvz4");
    stressWave.meta.compareRightPath = QStringLiteral("stress_right.wvz4");
    stressWave.signalList.reserve(kStressSignalCount);
    for (int i = 0; i < kStressSignalCount; ++i) {
        WaveSignal signal;
        signal.signalId = i;
        signal.name = QStringLiteral("stress.%1").arg(i, 5, 10, QLatin1Char('0'));
        signal.kind = SignalKind::Bit;
        signal.width = 1;
        signal.defaultRadix = ValueRadix::Bin;
        signal.currentRadix = ValueRadix::Bin;
        signal.samplesLoaded = true;
        WaveSample sample;
        sample.time = 0;
        sample.value = QStringLiteral("0");
        hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
        signal.samples.push_back(sample);
        WaveDiffRegion region;
        region.start = kStressSignalCount - i;
        region.end = region.start + 1;
        signal.diffRegions.push_back(region);
        stressWave.signalList.push_back(std::move(signal));
    }
    m_currentWaveFilePath.clear();
    m_currentWaveSupportsOnDemand = false;
    applyWave(std::move(stressWave));
    QList<int> stressIndexes;
    stressIndexes.reserve(kStressSignalCount);
    for (int i = 0; i < kStressSignalCount; ++i) stressIndexes.push_back(i);

    QElapsedTimer sortTimer;
    sortTimer.start();
    addSignalIndexesToActive(stressIndexes);
    const qint64 sortElapsedMs = sortTimer.elapsed();
    const int firstStressIndex = signalIndexFromActiveItem(
        m_activeList->topLevelItem(0));
    const int lastStressIndex = signalIndexFromActiveItem(
        m_activeList->topLevelItem(kStressSignalCount - 1));
    if (firstStressIndex != kStressSignalCount - 1 || lastStressIndex != 0 ||
        !allTopLevelRowsSelected(m_activeList) || sortElapsedMs > 10000) {
        if (error) {
            *error = QStringLiteral(
                "Large first-difference sort failed or remained quadratic: "
                "first=%1 last=%2 all_selected=%3 elapsed_ms=%4")
                .arg(firstStressIndex)
                .arg(lastStressIndex)
                .arg(allTopLevelRowsSelected(m_activeList) ? 1 : 0)
                .arg(sortElapsedMs);
        }
        return false;
    }
    return true;
}

bool MainWindow::runTreeSearchStateRegressionForBenchmark(QString* error) {
    SignalTreeModel* model = m_treeModel
        ? signalTreeModelFrom(m_treeModel)
        : nullptr;
    if (!model || !m_tree || !m_treeSearchRegexButton ||
        !m_treeSearchRestoreButton || model->rowCount() <= 0) {
        if (error) *error = QStringLiteral("Tree search UI is unavailable");
        return false;
    }

    const QModelIndex savedIndex = model->index(0, 0);
    if (!savedIndex.isValid()) {
        if (error) *error = QStringLiteral("Demo tree has no selectable root");
        return false;
    }
    m_tree->expand(savedIndex);
    if (QItemSelectionModel* selection = m_tree->selectionModel()) {
        selection->select(savedIndex, QItemSelectionModel::ClearAndSelect |
                                      QItemSelectionModel::Rows);
        selection->setCurrentIndex(savedIndex, QItemSelectionModel::NoUpdate);
    }
    const QSet<QString> savedExpandedPaths = m_userExpandedNodePaths;
    const int savedNodeId = model->nodeIdFromIndex(savedIndex);
    const int savedVerticalScroll = m_tree->verticalScrollBar()->value();
    const int savedHorizontalScroll = m_tree->horizontalScrollBar()->value();
    auto baselineRestored = [&]() {
        const QList<QModelIndex> rows = m_tree->selectionModel()
            ? m_tree->selectionModel()->selectedRows()
            : QList<QModelIndex>();
        return !m_treeSearchActive && !m_treeSearchCropMode &&
            !m_treeSearchSnapshotValid &&
            !m_treeSearchRestoreButton->isEnabled() &&
            m_userExpandedNodePaths == savedExpandedPaths &&
            rows.size() == 1 &&
            model->nodeIdFromIndex(rows.front()) == savedNodeId &&
            model->nodeIdFromIndex(m_tree->currentIndex()) == savedNodeId &&
            m_tree->verticalScrollBar()->value() == savedVerticalScroll &&
            m_tree->horizontalScrollBar()->value() == savedHorizontalScroll;
    };
    QStringList failures;

    {
        const QSignalBlocker blocker(m_treeSearchRegexButton);
        m_treeSearchRegexButton->setChecked(true);
    }
    showTreeSearchResults(QStringLiteral(".*"));
    const int searchSelectionCount = m_tree->selectionModel()
        ? m_tree->selectionModel()->selectedRows().size()
        : 0;
    const bool selectedSearchResults =
        searchSelectionCount == qMin(64, m_treeSearchMatchedNodeIds.size()) &&
        searchSelectionCount > 1;

    m_treeSearchRestoreButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const bool restored = baselineRestored();
    if (!selectedSearchResults || !restored) {
        failures.push_back(QStringLiteral("ordinary selection/restore"));
    }

    {
        const QSignalBlocker blocker(m_treeSearchRegexButton);
        m_treeSearchRegexButton->setChecked(false);
    }
    showTreeSearchResults(QStringLiteral("__missing_tree_node_7f18__"));
    const bool emptyResultClearedSelection = m_treeSearchActive &&
        m_treeSearchMatchedNodeIds.isEmpty() &&
        m_tree->selectionModel() &&
        m_tree->selectionModel()->selectedRows().isEmpty() &&
        !m_tree->currentIndex().isValid();
    m_treeSearchRestoreButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!emptyResultClearedSelection || !baselineRestored()) {
        failures.push_back(QStringLiteral("empty-result selection/restore"));
    }

    showTreeSearchResults(QStringLiteral("top"));
    const QVector<int> validSearchMatches = m_treeSearchMatchedNodeIds;
    const int validSearchCurrent = m_treeSearchCurrentMatch;
    const int validSearchSelectionCount = m_tree->selectionModel()
        ? m_tree->selectionModel()->selectedRows().size() : -1;
    {
        const QSignalBlocker blocker(m_treeSearchRegexButton);
        m_treeSearchRegexButton->setChecked(true);
    }
    showTreeSearchResults(QStringLiteral("("));
    const bool invalidRegexPreservedSearch = m_treeSearchActive &&
        !m_treeSearchCropMode &&
        m_treeSearchMatchedNodeIds == validSearchMatches &&
        m_treeSearchCurrentMatch == validSearchCurrent &&
        m_tree->selectionModel() &&
        m_tree->selectionModel()->selectedRows().size() ==
            validSearchSelectionCount &&
        m_treeSearchRestoreButton->isEnabled();
    {
        const QSignalBlocker blocker(m_treeSearchRegexButton);
        m_treeSearchRegexButton->setChecked(false);
    }
    m_treeSearchRestoreButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!invalidRegexPreservedSearch || !baselineRestored()) {
        failures.push_back(QStringLiteral("invalid-regex preservation"));
    }

    showTreeSearchResults(QStringLiteral("cpu"));
    showTreeSearchResults(QStringLiteral("uart"));
    m_treeSearchRestoreButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!baselineRestored()) {
        failures.push_back(QStringLiteral("consecutive-search baseline"));
    }

    openSignalConditionSearchDialog();
    if (m_signalConditionSearchDialog) m_signalConditionSearchDialog->hide();
    if (m_signalConditionCropTreeCheck) {
        const QSignalBlocker blocker(m_signalConditionCropTreeCheck);
        m_signalConditionCropTreeCheck->setChecked(true);
    }
    const QModelIndex restoredRootIndex = model->indexForNode(savedNodeId);
    const QModelIndex childIndex = model->index(0, 0, restoredRootIndex);
    QVector<int> advancedMatches;
    advancedMatches.push_back(savedNodeId);
    if (childIndex.isValid()) {
        advancedMatches.push_back(model->nodeIdFromIndex(childIndex));
    }
    applySignalConditionSearchResults(advancedMatches, advancedMatches.size(),
                                      advancedMatches.size(), 0, false,
                                      QString());
    const bool advancedSelected = m_treeSearchCropMode &&
        m_tree->selectionModel() &&
        m_tree->selectionModel()->selectedRows().size() ==
            advancedMatches.size();
    const QVector<int> advancedMatchesBeforeOptionToggle =
        m_treeSearchMatchedNodeIds;
    if (m_treeSearchCaseButton) m_treeSearchCaseButton->click();
    const bool emptyOptionTogglePreservedAdvanced =
        m_treeSearchActive && m_treeSearchCropMode &&
        m_treeSearchMatchedNodeIds == advancedMatchesBeforeOptionToggle;
    {
        const QSignalBlocker blocker(m_treeSearchRegexButton);
        m_treeSearchRegexButton->setChecked(true);
    }
    showTreeSearchResults(QStringLiteral("("));
    const bool invalidRegexPreservedAdvanced =
        m_treeSearchActive && m_treeSearchCropMode &&
        m_treeSearchMatchedNodeIds == advancedMatchesBeforeOptionToggle;
    {
        const QSignalBlocker blocker(m_treeSearchRegexButton);
        m_treeSearchRegexButton->setChecked(false);
    }
    m_treeSearchRestoreButton->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!advancedSelected || !emptyOptionTogglePreservedAdvanced ||
        !invalidRegexPreservedAdvanced || !baselineRestored()) {
        failures.push_back(QStringLiteral("advanced crop selection/restore"));
    }

    int arrayNodeId = -1;
    for (int nodeId = 0; nodeId < m_signalTreeModel->nodeCount(); ++nodeId) {
        if (m_signalTreeModel->isArrayContainer(nodeId)) {
            arrayNodeId = nodeId;
            break;
        }
    }
    if (arrayNodeId >= 0) {
        applySignalConditionSearchResults(
            QVector<int>() << arrayNodeId, 1, 1, 0, false, QString());
        QModelIndex arrayIndex = model->indexForNode(arrayNodeId);
        materializeArrayNode(arrayNodeId);
        arrayIndex = model->indexForNode(arrayNodeId);
        const int firstBatchRows = model->rowCount(arrayIndex);
        const bool hasMore =
            m_signalTreeModel->arrayHasUnmaterializedElements(arrayNodeId);
        bool arrayPagingWorks = arrayIndex.isValid() && firstBatchRows > 0;
        if (hasMore) {
            arrayPagingWorks = arrayPagingWorks &&
                model->canFetchMore(arrayIndex);
            model->fetchMore(arrayIndex);
            arrayIndex = model->indexForNode(arrayNodeId);
            arrayPagingWorks = arrayPagingWorks &&
                model->rowCount(arrayIndex) > firstBatchRows;
        }
        m_treeSearchRestoreButton->click();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!arrayPagingWorks || !baselineRestored()) {
            failures.push_back(QStringLiteral("crop-mode array paging"));
        }

        {
            const QSignalBlocker blocker(m_treeSearchRegexButton);
            m_treeSearchRegexButton->setChecked(true);
        }
        showTreeSearchResults(QStringLiteral(".*"));
        const int matchedCount = m_treeSearchMatchedNodeIds.size();
        const int selectedCount = m_tree->selectionModel()
            ? m_tree->selectionModel()->selectedRows().size() : 0;
        const bool selectionCapWorks =
            matchedCount > 0 &&
            selectedCount == qMin(64, matchedCount);
        m_treeSearchRestoreButton->click();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!selectionCapWorks || !baselineRestored()) {
            failures.push_back(QStringLiteral(
                "64-result selection cap(matches=%1 selected=%2)")
                .arg(matchedCount)
                .arg(selectedCount));
        }
    }

    int pendingReferenceNodeId = -1;
    for (int nodeId = 0; nodeId < m_signalTreeModel->nodeCount(); ++nodeId) {
        if (m_signalTreeModel->isPendingReference(nodeId) &&
            m_signalTreeModel->hasChildren(nodeId)) {
            pendingReferenceNodeId = nodeId;
            break;
        }
    }
    if (pendingReferenceNodeId >= 0) {
        applySignalConditionSearchResults(
            QVector<int>() << pendingReferenceNodeId,
            1, 1, 0, false, QString());
        QModelIndex referenceIndex =
            model->indexForNode(pendingReferenceNodeId);
        const bool referenceVisible = referenceIndex.isValid();
        if (referenceVisible) m_tree->expand(referenceIndex);
        if (m_tree->selectionModel()) {
            m_tree->selectionModel()->setCurrentIndex(
                model->indexForNode(savedNodeId),
                QItemSelectionModel::NoUpdate);
        }
        QElapsedTimer waitTimer;
        waitTimer.start();
        while (m_signalTreeModel->isPendingReference(
                   pendingReferenceNodeId) &&
               waitTimer.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        referenceIndex = model->indexForNode(pendingReferenceNodeId);
        const bool referencePatchStable = referenceVisible &&
            !m_signalTreeModel->isPendingReference(pendingReferenceNodeId) &&
            referenceIndex.isValid() && model->rowCount(referenceIndex) > 0 &&
            model->nodeIdFromIndex(m_tree->currentIndex()) == savedNodeId;
        m_treeSearchRestoreButton->click();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!referencePatchStable || !baselineRestored()) {
            failures.push_back(QStringLiteral("crop-mode async reference patch"));
        }
    }

    {
        const QSignalBlocker blocker(m_treeSearchRegexButton);
        m_treeSearchRegexButton->setChecked(true);
    }
    showTreeSearchResults(QStringLiteral(".*"));
    const bool navigationWasEnabled = m_treeSearchMatchedNodeIds.size() <= 1 ||
        (m_treeSearchPrevButton && m_treeSearchPrevButton->isEnabled() &&
         m_treeSearchNextButton && m_treeSearchNextButton->isEnabled());
    loadDemoWave();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const bool waveChangeClearedSearch = navigationWasEnabled &&
        !m_treeSearchActive && !m_treeSearchCropMode &&
        !m_treeSearchSnapshotValid &&
        m_treeSearchMatchedNodeIds.isEmpty() &&
        m_treeSearchEdit && m_treeSearchEdit->text().isEmpty() &&
        m_treeSearchRestoreButton && !m_treeSearchRestoreButton->isEnabled() &&
        m_treeSearchPrevButton && !m_treeSearchPrevButton->isEnabled() &&
        m_treeSearchNextButton && !m_treeSearchNextButton->isEnabled() &&
        (!m_signalConditionPrevButton ||
         !m_signalConditionPrevButton->isEnabled()) &&
        (!m_signalConditionNextButton ||
         !m_signalConditionNextButton->isEnabled());
    if (!waveChangeClearedSearch) {
        failures.push_back(QStringLiteral("wave-change search cleanup"));
    }

    if (!failures.isEmpty() && error) {
        *error = failures.join(QStringLiteral("; "));
    }
    return failures.isEmpty();
}

void MainWindow::restoreUserTreeExpansionState() {
    if (!m_tree || !m_treeModel || !m_signalTreeModel || m_treeSearchActive) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;

    QStringList paths = m_userExpandedNodePaths.values();
    std::sort(paths.begin(), paths.end(), [](const QString& left, const QString& right) {
        const int leftDepth = left.count(QLatin1Char('.'));
        const int rightDepth = right.count(QLatin1Char('.'));
        return leftDepth == rightDepth ? left < right : leftDepth < rightDepth;
    });

    QSet<QString> stillValid;
    m_applyingTreeExpansionState = true;
    for (const QString& path : paths) {
        bool complete = false;
        bool pending = false;
        const QVector<int> chain =
            m_signalTreeModel->materializedNodeChainForPath(path, complete, pending);
        if (chain.isEmpty()) continue;
        if (complete) {
            const QModelIndex index = model->indexForNode(chain.last());
            if (index.isValid()) m_tree->expand(index);
        }
        const int deepestNode = chain.last();
        if (m_signalTreeModel->isBitsetContainer(deepestNode)) {
            materializeBitsetNode(deepestNode);
        }
        if (m_signalTreeModel->isArrayContainer(deepestNode)) {
            materializeArrayNode(deepestNode);
        }
        prioritizeTreeReference(deepestNode);
        if (complete || pending) stillValid.insert(path);
    }
    m_applyingTreeExpansionState = false;
    m_userExpandedNodePaths = stillValid;
}

void MainWindow::captureTreeSearchState() {
    if (m_treeSearchSnapshotValid || !m_tree || !m_treeModel) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;

    m_treeSearchSavedExpandedNodePaths = m_userExpandedNodePaths;
    m_treeSearchSavedSelectedNodeIds.clear();
    m_treeSearchSavedCurrentNodeId = -1;
    if (QItemSelectionModel* selection = m_tree->selectionModel()) {
        QSet<int> seen;
        for (const QModelIndex& index : selection->selectedRows()) {
            const int nodeId = model->nodeIdFromIndex(index);
            if (nodeId >= 0 && !seen.contains(nodeId)) {
                seen.insert(nodeId);
                m_treeSearchSavedSelectedNodeIds.push_back(nodeId);
            }
        }
        m_treeSearchSavedCurrentNodeId =
            model->nodeIdFromIndex(selection->currentIndex());
    }
    m_treeSearchSavedVerticalScroll = m_tree->verticalScrollBar()->value();
    m_treeSearchSavedHorizontalScroll = m_tree->horizontalScrollBar()->value();
    const QModelIndex topIndex = m_tree->indexAt(QPoint(0, 0));
    m_treeSearchSavedTopNodeId = model->nodeIdFromIndex(topIndex);
    m_treeSearchSavedTopNodeOffset = topIndex.isValid()
        ? m_tree->visualRect(topIndex).top() : 0;
    m_treeSearchSnapshotValid = true;
    if (m_treeSearchRestoreButton) m_treeSearchRestoreButton->setEnabled(true);
}

void MainWindow::restoreTreeSearchState() {
    if (!m_treeSearchSnapshotValid || !m_tree || !m_treeModel) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;

    const QVector<int> selectedNodeIds = m_treeSearchSavedSelectedNodeIds;
    const int currentNodeId = m_treeSearchSavedCurrentNodeId;
    const int verticalScroll = m_treeSearchSavedVerticalScroll;
    const int horizontalScroll = m_treeSearchSavedHorizontalScroll;
    const int topNodeId = m_treeSearchSavedTopNodeId;
    const int topNodeOffset = m_treeSearchSavedTopNodeOffset;
    const quint64 restoreGeneration = ++m_treeSearchRestoreGeneration;
    const quint64 waveGeneration = m_waveFileGeneration;

    m_userExpandedNodePaths = m_treeSearchSavedExpandedNodePaths;
    m_applyingTreeExpansionState = true;
    m_tree->collapseAll();
    m_applyingTreeExpansionState = false;
    restoreUserTreeExpansionState();

    if (QItemSelectionModel* selection = m_tree->selectionModel()) {
        const QSignalBlocker blocker(selection);
        selection->clearSelection();
        for (int nodeId : selectedNodeIds) {
            const QModelIndex index = model->indexForNode(nodeId);
            if (index.isValid()) {
                selection->select(index, QItemSelectionModel::Select |
                                         QItemSelectionModel::Rows);
            }
        }
        selection->setCurrentIndex(model->indexForNode(currentNodeId),
                                   QItemSelectionModel::NoUpdate);
    }

    m_treeSearchSnapshotValid = false;
    m_treeSearchSavedExpandedNodePaths.clear();
    m_treeSearchSavedSelectedNodeIds.clear();
    m_treeSearchSavedCurrentNodeId = -1;
    m_treeSearchSavedTopNodeId = -1;
    m_treeSearchSavedTopNodeOffset = 0;
    if (m_treeSearchRestoreButton) m_treeSearchRestoreButton->setEnabled(false);
    QTimer::singleShot(0, this, [this, verticalScroll, horizontalScroll,
                                 topNodeId, topNodeOffset,
                                 restoreGeneration, waveGeneration]() {
        if (!m_tree ||
            restoreGeneration != m_treeSearchRestoreGeneration ||
            waveGeneration != m_waveFileGeneration) {
            return;
        }
        SignalTreeModel* currentModel = m_treeModel
            ? signalTreeModelFrom(m_treeModel) : nullptr;
        const QModelIndex topIndex = currentModel
            ? currentModel->indexForNode(topNodeId) : QModelIndex();
        if (topIndex.isValid()) {
            m_tree->scrollTo(topIndex, QAbstractItemView::PositionAtTop);
            if (m_tree->verticalScrollMode() ==
                QAbstractItemView::ScrollPerPixel) {
                m_tree->verticalScrollBar()->setValue(
                    m_tree->verticalScrollBar()->value() - topNodeOffset);
            }
        } else {
            m_tree->verticalScrollBar()->setValue(verticalScroll);
        }
        m_tree->horizontalScrollBar()->setValue(horizontalScroll);
    });
}

void MainWindow::retryPendingViewerSessionRestore() {
    restoreUserTreeExpansionState();
    if (!m_pendingSessionSignalRestore || !m_signalTreeModel ||
        !m_activeList || !m_canvas) {
        return;
    }

    QList<int> resolvedIndexes;
    QVector<ViewerSessionSignal> resolvedSignals;
    bool waitingForSubtree = false;
    bool retrySoon = false;
    for (const ViewerSessionSignal& saved : m_pendingSessionSignals) {
        bool complete = false;
        bool pending = false;
        const QVector<int> chain =
            m_signalTreeModel->materializedNodeChainForPath(saved.path,
                                                            complete, pending);
        if (complete && !chain.isEmpty()) {
            const int signalIndex = m_signalTreeModel->nodeSignalIndex(chain.last());
            if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) {
                resolvedIndexes.push_back(signalIndex);
                resolvedSignals.push_back(saved);
            }
            continue;
        }
        if (!pending || chain.isEmpty()) continue;

        const int deepestNode = chain.last();
        if (m_signalTreeModel->isBitsetContainer(deepestNode)) {
            materializeBitsetNode(deepestNode);
            retrySoon = true;
        } else if (m_signalTreeModel->isArrayContainer(deepestNode)) {
            materializeArrayNode(deepestNode);
            retrySoon = true;
        } else {
            prioritizeTreeReference(deepestNode);
            waitingForSubtree = true;
        }
    }

    if (retrySoon) {
        QTimer::singleShot(0, this, [this]() { retryPendingViewerSessionRestore(); });
    }
    if (waitingForSubtree || retrySoon) return;

    m_pendingSessionSignalRestore = false;
    m_activeList->clear();
    addSignalIndexesToActive(resolvedIndexes);

    int currentRow = -1;
    QSet<int> selectedRows;
    const int restoredCount = qMin(resolvedSignals.size(),
                                   m_activeList->topLevelItemCount());
    for (int row = 0; row < restoredCount; ++row) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(row);
        item->setData(0, RoleCurrentFormat, resolvedSignals.at(row).format);
        if (resolvedSignals.at(row).selected) selectedRows.insert(row);
        if (resolvedSignals.at(row).current) currentRow = row;
    }

    if (QItemSelectionModel* selection = m_activeList->selectionModel()) {
        const QSignalBlocker blocker(selection);
        selection->clearSelection();
        for (int row : selectedRows) {
            const QModelIndex index = m_activeList->model()->index(row, 0);
            selection->select(index, QItemSelectionModel::Select |
                                     QItemSelectionModel::Rows);
        }
        if (currentRow >= 0 && currentRow < restoredCount) {
            selection->setCurrentIndex(m_activeList->model()->index(currentRow, 0),
                                       QItemSelectionModel::NoUpdate);
        }
    }
    rebuildVisibleSignals();
    refreshActiveValueLabels();
    syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
    m_pendingSessionSignals.clear();
}

void MainWindow::applyTreeSearchExpansion() {
    if (!m_treeSearchActive || !m_tree || !m_treeModel || !m_signalTreeModel) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;

    m_applyingTreeExpansionState = true;
    for (int i = m_treeSearchAutoExpandedNodeIds.size() - 1; i >= 0; --i) {
        const int nodeId = m_treeSearchAutoExpandedNodeIds.at(i);
        const QString path = m_signalTreeModel->fullPathForNodeId(nodeId);
        if (m_userExpandedNodePaths.contains(path)) continue;
        const QModelIndex index = model->indexForNode(nodeId);
        if (index.isValid()) m_tree->collapse(index);
    }
    m_treeSearchAutoExpandedNodeIds.clear();
    QItemSelectionModel* selection = m_tree->selectionModel();
    if (selection) {
        const QSignalBlocker blocker(selection);
        selection->clearSelection();
        selection->setCurrentIndex(QModelIndex(),
                                   QItemSelectionModel::NoUpdate);
    }
    if (m_treeSearchCurrentMatch >= 0 &&
        m_treeSearchCurrentMatch < m_treeSearchMatchedNodeIds.size()) {
        const int targetNodeId =
            m_treeSearchMatchedNodeIds.at(m_treeSearchCurrentMatch);
        QVector<int> visibleTargets;
        visibleTargets.reserve(qMin(64, m_treeSearchMatchedNodeIds.size()) + 1);
        for (int i = 0; i < qMin(64, m_treeSearchMatchedNodeIds.size()); ++i) {
            visibleTargets.push_back(m_treeSearchMatchedNodeIds.at(i));
        }
        if (!visibleTargets.contains(targetNodeId)) visibleTargets.push_back(targetNodeId);

        QSet<int> expandedAncestors;
        for (int visibleTarget : visibleTargets) {
            QVector<int> chain;
            int current = visibleTarget;
            int guard = 0;
            while (m_signalTreeModel->isValidNodeId(current) &&
                   guard++ < m_signalTreeModel->nodeCount()) {
                chain.push_back(current);
                current = m_signalTreeModel->nodeParent(current);
            }
            // Only reveal the target itself. Its children remain untouched.
            for (int i = chain.size() - 1; i >= 1; --i) {
                const int ancestorNodeId = chain.at(i);
                if (expandedAncestors.contains(ancestorNodeId)) continue;
                expandedAncestors.insert(ancestorNodeId);
                const QModelIndex index = model->indexForNode(ancestorNodeId);
                if (index.isValid() && !m_tree->isExpanded(index)) {
                    m_tree->expand(index);
                    m_treeSearchAutoExpandedNodeIds.push_back(ancestorNodeId);
                }
            }
        }

        const QModelIndex target = model->indexForNode(targetNodeId);
        if (selection) {
            const QSignalBlocker blocker(selection);
            const int selectedCount = qMin(64, m_treeSearchMatchedNodeIds.size());
            for (int i = 0; i < selectedCount; ++i) {
                const QModelIndex index =
                    model->indexForNode(m_treeSearchMatchedNodeIds.at(i));
                if (index.isValid()) {
                    selection->select(index, QItemSelectionModel::Select |
                                             QItemSelectionModel::Rows);
                }
            }
            selection->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
        }
        if (target.isValid()) {
            m_tree->scrollTo(target, QAbstractItemView::PositionAtCenter);
        }
    }
    m_applyingTreeExpansionState = false;
    if (m_tree->viewport()) m_tree->viewport()->update();
}

void MainWindow::navigateTreeSearchMatch(int delta) {
    const int count = m_treeSearchMatchedNodeIds.size();
    if (!m_treeSearchActive || count <= 0) return;
    int next = m_treeSearchCurrentMatch;
    if (next < 0 || next >= count) next = 0;
    else next = (next + delta + count) % count;
    m_treeSearchCurrentMatch = next;
    applyTreeSearchExpansion();
    if (m_treeSearchEdit) {
        m_treeSearchEdit->setToolTip(QStringLiteral("Match %1 of %2")
                                     .arg(next + 1).arg(count));
    }
}

bool MainWindow::handleWaveFileDropEvent(QEvent* event) {
    if (!event) return false;

    if (event->type() == QEvent::DragEnter) {
        auto* drag = static_cast<QDragEnterEvent*>(event);
        if (!firstSupportedWaveFilePathFromMime(drag->mimeData()).isEmpty()) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        return false;
    }

    if (event->type() == QEvent::DragMove) {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        if (!firstSupportedWaveFilePathFromMime(drag->mimeData()).isEmpty()) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        return false;
    }

    if (event->type() == QEvent::Drop) {
        auto* drop = static_cast<QDropEvent*>(event);
        const QString path = firstSupportedWaveFilePathFromMime(drop->mimeData());
        if (!path.isEmpty()) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            openWaveFilePath(path);
            return true;
        }
    }

    return false;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (handleWaveFileDropEvent(event)) return true;
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (handleWaveFileDropEvent(event)) return;
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
    if (handleWaveFileDropEvent(event)) return;
    QMainWindow::dragMoveEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (handleWaveFileDropEvent(event)) return;
    QMainWindow::dropEvent(event);
}

void MainWindow::setupToolbarButton(QPushButton* button, const QIcon& icon, const QString& objectName, const QString& tip) {
    button->setObjectName(objectName);
    button->setIcon(icon.isNull() ? makeColorfulToolbarIcon("open") : icon);
    button->setIconSize(QSize(18, 18));
    button->setText(QString());
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setAccessibleDescription(tip);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setFixedSize(28, 28);
}

void MainWindow::buildUi() {
    setWindowTitle("Wave Viewer");
    setMinimumSize(1120, 700);
    resize(1860, 980);

    m_central = new QWidget(this);
    setCentralWidget(m_central);

    auto* root = new QVBoxLayout(m_central);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    m_splitter = new QSplitter(this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(4);
    m_splitter->setOpaqueResize(false);
    root->addWidget(m_splitter, 1);

    auto* leftPane = new QFrame();
    leftPane->setObjectName("darkPanel");
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->setSpacing(6);
    auto* leftTitle = new QLabel(QString::fromUtf8("可选信号"), leftPane);
    leftTitle->setObjectName("panelTitle");

    m_treeSearchEdit = new QLineEdit(leftPane);
    m_treeSearchEdit->setObjectName(QStringLiteral("signalTreeSearch"));
    m_treeSearchEdit->setPlaceholderText(QString::fromUtf8("搜索层级名 / 信号名"));
    m_treeSearchEdit->setMinimumWidth(0);
    m_treeSearchEdit->setClearButtonEnabled(true);
    auto* treeSearchRow = new QHBoxLayout();
    treeSearchRow->setSpacing(4);
    treeSearchRow->setContentsMargins(0, 0, 0, 0);
    m_treeSearchCaseButton = new QPushButton(QStringLiteral("Aa"), leftPane);
    m_treeSearchRegexButton = new QPushButton(QStringLiteral(".*"), leftPane);
    m_treeSearchPrevButton = new QPushButton(QStringLiteral("↑"), leftPane);
    m_treeSearchNextButton = new QPushButton(QStringLiteral("↓"), leftPane);
    m_treeSearchRestoreButton = new QPushButton(QString::fromUtf8("还原"), leftPane);
    m_treeSearchPrevButton->setObjectName(QStringLiteral("signalTreeSearchPrevious"));
    m_treeSearchNextButton->setObjectName(QStringLiteral("signalTreeSearchNext"));
    m_treeSearchRestoreButton->setObjectName(QStringLiteral("signalTreeSearchRestore"));
    auto setupSearchOptionButton = [](QPushButton* button, const QString& tip) {
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("searchOptionButton"));
        button->setFixedSize(34, 30);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setFocusPolicy(Qt::NoFocus);
        QFont f = button->font();
        f.setPointSizeF(9.2);
        f.setBold(true);
        button->setFont(f);
    };
    setupSearchOptionButton(m_treeSearchCaseButton, QStringLiteral("Match case"));
    setupSearchOptionButton(m_treeSearchRegexButton, QStringLiteral("Use regular expression"));
    auto* leftHeaderRow = new QHBoxLayout();
    leftHeaderRow->setContentsMargins(0, 0, 0, 0);
    leftHeaderRow->setSpacing(4);
    leftHeaderRow->addWidget(leftTitle);
    leftHeaderRow->addStretch(1);
    leftHeaderRow->addWidget(m_treeSearchCaseButton);
    leftHeaderRow->addWidget(m_treeSearchRegexButton);
    leftLayout->addLayout(leftHeaderRow);
    for (QPushButton* button : {m_treeSearchPrevButton, m_treeSearchNextButton}) {
        button->setProperty("searchNavigationButton", true);
        button->setFixedSize(30, 30);
        button->setFocusPolicy(Qt::NoFocus);
        button->setEnabled(false);
    }
    m_treeSearchPrevButton->setToolTip(QStringLiteral("Previous search match"));
    m_treeSearchNextButton->setToolTip(QStringLiteral("Next search match"));
    m_treeSearchRestoreButton->setFixedSize(46, 30);
    m_treeSearchRestoreButton->setFocusPolicy(Qt::NoFocus);
    m_treeSearchRestoreButton->setEnabled(false);
    m_treeSearchRestoreButton->setToolTip(QString::fromUtf8("返回查找前的展开、选择和滚动位置"));
    treeSearchRow->addWidget(m_treeSearchEdit, 1);
    treeSearchRow->addWidget(m_treeSearchPrevButton);
    treeSearchRow->addWidget(m_treeSearchNextButton);
    treeSearchRow->addWidget(m_treeSearchRestoreButton);
    leftLayout->addLayout(treeSearchRow);

    auto* signalTree = new SignalTreeView(leftPane);
    signalTree->setObjectName(QStringLiteral("signalTree"));
    signalTree->setActiveRowsDroppedCallback([this](const QList<int>& rows) {
        removeActiveRows(rows);
    });
    m_tree = signalTree;
    m_treeModel = new SignalTreeModel(m_tree);
    signalTreeModelFrom(m_treeModel)->setArrayFetchCallback(
        [this](int nodeId) { materializeArrayNode(nodeId); });
    m_tree->setModel(m_treeModel);
    m_tree->setItemDelegate(new ReadOnlyTextDelegate(m_tree));
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(18);
    m_tree->setAnimated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSortingEnabled(false);
    m_tree->setAutoExpandDelay(-1);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    leftLayout->addWidget(m_tree, 1);


    auto* middlePane = new QFrame();
    middlePane->setObjectName("darkPanel");
    auto* middleLayout = new QVBoxLayout(middlePane);
    middleLayout->setContentsMargins(8, 8, 8, 8);
    middleLayout->setSpacing(6);

    auto* middleTitle = new QLabel(QString::fromUtf8("选中信号"), middlePane);
    middleTitle->setObjectName("panelTitle");
    middleLayout->addWidget(middleTitle);

    m_activeSearchEdit = new QLineEdit(middlePane);
    m_activeSearchEdit->setPlaceholderText(QString::fromUtf8("搜索选中信号"));
    middleLayout->addWidget(m_activeSearchEdit);

    m_activeList = new ActiveSignalListWidget(middlePane);
    m_activeList->setObjectName(QStringLiteral("activeSignalList"));
    middleLayout->addWidget(m_activeList, 1);
    auto* importSignalPathsButton = new QPushButton(
        QStringLiteral("Import signal paths..."), middlePane);
    importSignalPathsButton->setToolTip(
        QStringLiteral("Read one full signal path per line from a UTF-8 text file"));
    middleLayout->addWidget(importSignalPathsButton);

    auto* rightPane = new QFrame();
    rightPane->setObjectName("wavePanel");
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 6);
    rightLayout->setSpacing(4);

    auto* topBarCard = new QFrame(rightPane);
    topBarCard->setObjectName("toolbarCard");
    topBarCard->setFixedHeight(41);
    topBarCard->setMinimumWidth(0);
    topBarCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Keep the toolbar's natural width out of the splitter's minimum-size
    // calculation. The outer card clips the fixed-width contents when the
    // waveform pane is narrowed, so controls on the far right can disappear
    // behind its edge instead of forcing the pane wider.
    auto* topBarContents = new QWidget(topBarCard);
    topBarContents->setObjectName("toolbarContents");
    auto* topBarLayout = new QHBoxLayout(topBarContents);
    topBarLayout->setContentsMargins(6, 5, 6, 5);
    topBarLayout->setSpacing(4);

    auto* btnOpen = new QPushButton(topBarContents);
    auto* btnCompare = new QPushButton(topBarContents);
    auto* btnDerivedSignal = new QPushButton(topBarContents);
    auto* btnZoomIn = new QPushButton(topBarContents);
    auto* btnZoomOut = new QPushButton(topBarContents);
    auto* btnPanLeft = new QPushButton(topBarContents);
    auto* btnPanRight = new QPushButton(topBarContents);
    auto* btnPrevChange = new QPushButton(topBarContents);
    auto* btnNextChange = new QPushButton(topBarContents);
    auto* btnReset = new QPushButton(topBarContents);

    setupToolbarButton(btnOpen, loadImageIconOrFallback("open", "open"), QStringLiteral("toolbarOpenButton"), QString::fromUtf8("打开波形文件"));
    setupToolbarButton(btnCompare, loadImageIconOrFallback("compare", "compare"), QStringLiteral("toolbarCompareButton"), QString::fromUtf8("比较两个波形文件"));
    setupToolbarButton(btnDerivedSignal, loadImageIconOrFallback("derive", "derive"), QStringLiteral("toolbarDerivedSignalButton"), QStringLiteral("Create temporary signal"));
    setupToolbarButton(btnZoomIn, loadImageIconOrFallback("zoom_in", "zoom_in"), QStringLiteral("toolbarZoomInButton"), QString::fromUtf8("放大"));
    setupToolbarButton(btnZoomOut, loadImageIconOrFallback("zoom_out", "zoom_out"), QStringLiteral("toolbarZoomOutButton"), QString::fromUtf8("缩小"));
    setupToolbarButton(btnPanLeft, loadImageIconOrFallback("left", "left"), QStringLiteral("toolbarPanLeftButton"), QString::fromUtf8("左移"));
    setupToolbarButton(btnPanRight, loadImageIconOrFallback("right", "right"), QStringLiteral("toolbarPanRightButton"), QString::fromUtf8("右移"));
    setupToolbarButton(btnPrevChange, loadImageIconOrFallback("prev_change", "prev_change"), QStringLiteral("toolbarPrevChangeButton"), QString::fromUtf8("跳到选中信号上一次变化"));
    setupToolbarButton(btnNextChange, loadImageIconOrFallback("next_change", "next_change"), QStringLiteral("toolbarNextChangeButton"), QString::fromUtf8("跳到选中信号下一次变化"));
    setupToolbarButton(btnReset, loadImageIconOrFallback("reset", "reset"), QStringLiteral("toolbarResetButton"), QString::fromUtf8("重置视图"));

    m_jumpTimeEdit = new QLineEdit(topBarContents);
    m_jumpTimeEdit->setObjectName("jumpTimeEdit");
    m_jumpTimeEdit->setFixedWidth(118);
    m_jumpTimeEdit->setPlaceholderText(QString::fromUtf8("跳转时间"));
    m_jumpTimeEdit->setToolTip(QString::fromUtf8("输入时间轴显示值后按 Enter 跳转"));

    m_windowRangeStartEdit = new QLineEdit(topBarContents);
    m_windowRangeStartEdit->setObjectName("windowRangeEdit");
    m_windowRangeStartEdit->setFixedWidth(92);
    m_windowRangeStartEdit->setPlaceholderText(QString::fromUtf8("起始时间"));
    m_windowRangeStartEdit->setToolTip(QString::fromUtf8("输入窗口起始时间，按 Enter 应用范围"));
    auto* windowRangeSeparator = new QLabel(QStringLiteral("~"), topBarContents);
    windowRangeSeparator->setObjectName("windowRangeSeparator");
    m_windowRangeEndEdit = new QLineEdit(topBarContents);
    m_windowRangeEndEdit->setObjectName("windowRangeEdit");
    m_windowRangeEndEdit->setFixedWidth(92);
    m_windowRangeEndEdit->setPlaceholderText(QString::fromUtf8("结束时间"));
    m_windowRangeEndEdit->setToolTip(QString::fromUtf8("输入窗口结束时间，按 Enter 应用范围"));

    topBarLayout->addWidget(btnOpen);
    topBarLayout->addWidget(btnCompare);
    topBarLayout->addWidget(btnDerivedSignal);
    topBarLayout->addSpacing(4);
    topBarLayout->addWidget(btnZoomIn);
    topBarLayout->addWidget(btnZoomOut);
    topBarLayout->addWidget(btnPanLeft);
    topBarLayout->addWidget(btnPanRight);
    topBarLayout->addWidget(btnPrevChange);
    topBarLayout->addWidget(btnNextChange);
    topBarLayout->addWidget(btnReset);
    topBarLayout->addSpacing(8);
    topBarLayout->addWidget(m_jumpTimeEdit);
    topBarLayout->addSpacing(8);
    topBarLayout->addWidget(m_windowRangeStartEdit);
    topBarLayout->addWidget(windowRangeSeparator);
    topBarLayout->addWidget(m_windowRangeEndEdit);
    topBarLayout->addStretch(1);
    topBarLayout->activate();
    topBarContents->setFixedSize(topBarLayout->sizeHint().width(), topBarCard->height());
    topBarContents->move(0, 0);
    QTimer::singleShot(0, topBarContents, [this, topBarContents, topBarLayout, topBarCard]() {
        // Re-evaluate after the application style sheet and DPI-dependent font
        // metrics have been applied; the outer card remains free to be narrower.
        topBarLayout->activate();
        topBarContents->setFixedSize(topBarLayout->sizeHint().width(), topBarCard->height());

        // Give the waveform pane enough initial width to expose every toolbar
        // control. Narrower user-driven splitter sizes are still allowed later.
        if (m_splitter) {
            const int availableWidth = qMax(0,
                m_splitter->width() - m_splitter->handleWidth() * (m_splitter->count() - 1));
            const int toolbarWidth = topBarContents->width();
            const int desiredWaveWidth = qMax(toolbarWidth, availableWidth - 750);
            const int waveWidth = qMin(desiredWaveWidth, qMax(0, availableWidth - 360));
            const int sideWidth = qMax(0, availableWidth - waveWidth);
            const int leftWidth = sideWidth * 330 / 750;
            m_splitter->setSizes(QList<int>() << leftWidth << sideWidth - leftWidth << waveWidth);
        }
    });
    rightLayout->addWidget(topBarCard);

    m_canvas = new WaveCanvas(rightPane);
    rightLayout->addWidget(m_canvas, 1);

    m_splitter->addWidget(leftPane);
    m_splitter->addWidget(middlePane);
    m_splitter->addWidget(rightPane);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setSizes(QList<int>() << 330 << 420 << 1110);

    connect(btnOpen, &QPushButton::clicked, this, &MainWindow::openWaveFile);
    connect(importSignalPathsButton, &QPushButton::clicked,
            this, &MainWindow::importSignalPathsFromTextFile);
    connect(btnCompare, &QPushButton::clicked, this, &MainWindow::compareWaveFiles);
    connect(btnDerivedSignal, &QPushButton::clicked, this, &MainWindow::openDerivedSignalDialog);
    connect(btnZoomIn, &QPushButton::clicked, this, &MainWindow::zoomIn);
    connect(btnZoomOut, &QPushButton::clicked, this, &MainWindow::zoomOut);
    connect(btnPanLeft, &QPushButton::clicked, this, &MainWindow::panLeft);
    connect(btnPanRight, &QPushButton::clicked, this, &MainWindow::panRight);
    connect(btnPrevChange, &QPushButton::clicked, this, &MainWindow::jumpToPrevChange);
    connect(btnNextChange, &QPushButton::clicked, this, &MainWindow::jumpToNextChange);
    connect(btnReset, &QPushButton::clicked, this, &MainWindow::resetView);
    connect(m_jumpTimeEdit, &QLineEdit::returnPressed, this, &MainWindow::jumpToTime);
    connect(m_windowRangeStartEdit, &QLineEdit::returnPressed, this, &MainWindow::applyWindowRangeInput);
    connect(m_windowRangeEndEdit, &QLineEdit::returnPressed, this, &MainWindow::applyWindowRangeInput);

    auto* findValueShortcut = new QShortcut(QKeySequence::Find, this);
    findValueShortcut->setContext(Qt::WindowShortcut);
    connect(findValueShortcut, &QShortcut::activated, this, [this]() {
        QWidget* focus = QApplication::focusWidget();
        const bool treeHasFocus =
            m_tree && focus && (focus == m_tree || m_tree->isAncestorOf(focus));
        if (treeHasFocus) openSignalConditionSearchDialog();
        else openValueFindDialog();
    });

    auto* derivedSignalShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+E")), this);
    derivedSignalShortcut->setContext(Qt::WindowShortcut);
    connect(derivedSignalShortcut, &QShortcut::activated, this, &MainWindow::openDerivedSignalDialog);

    auto* jumpFirstSignalEventShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+[")), this);
    jumpFirstSignalEventShortcut->setContext(Qt::WindowShortcut);
    connect(jumpFirstSignalEventShortcut, &QShortcut::activated, this, [this]() {
        jumpSelectedTreeSignalToViewportEvent(true);
    });

    auto* jumpLastSignalEventShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+]")), this);
    jumpLastSignalEventShortcut->setContext(Qt::WindowShortcut);
    connect(jumpLastSignalEventShortcut, &QShortcut::activated, this, [this]() {
        jumpSelectedTreeSignalToViewportEvent(false);
    });

    connect(m_tree, &QTreeView::doubleClicked, this, &MainWindow::onTreeIndexDoubleClicked);
    connect(m_tree, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
        if (!model) return;
        const int nodeId = model->nodeIdFromIndex(index);
        if (!m_applyingTreeExpansionState &&
            !(m_treeSearchActive && m_treeSearchCropMode) &&
            m_signalTreeModel) {
            const QString path = m_signalTreeModel->fullPathForNodeId(nodeId);
            if (!path.isEmpty()) m_userExpandedNodePaths.insert(path);
        }
        materializeBitsetNode(nodeId);
        materializeArrayNode(nodeId);
        prioritizeTreeReference(nodeId);
    });
    connect(m_tree, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        if (m_applyingTreeExpansionState ||
            (m_treeSearchActive && m_treeSearchCropMode) ||
            !m_signalTreeModel) {
            return;
        }
        SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
        if (!model) return;
        const int nodeId = model->nodeIdFromIndex(index);
        const QString path = m_signalTreeModel->fullPathForNodeId(nodeId);
        if (!path.isEmpty()) m_userExpandedNodePaths.remove(path);
    });
    connect(m_treeSearchEdit, &QLineEdit::returnPressed, this, [this]() {
        onTreeSearchTextChanged(m_treeSearchEdit ? m_treeSearchEdit->text() : QString());
    });
    connect(m_treeSearchCaseButton, &QPushButton::toggled, this, [this](bool) {
        if (m_treeSearchEdit && !m_treeSearchEdit->text().trimmed().isEmpty()) {
            onTreeSearchTextChanged(m_treeSearchEdit->text());
        }
    });
    connect(m_treeSearchRegexButton, &QPushButton::toggled, this, [this](bool) {
        if (m_treeSearchEdit && !m_treeSearchEdit->text().trimmed().isEmpty()) {
            onTreeSearchTextChanged(m_treeSearchEdit->text());
        }
    });
    connect(m_treeSearchPrevButton, &QPushButton::clicked, this, [this]() {
        navigateTreeSearchMatch(-1);
    });
    connect(m_treeSearchNextButton, &QPushButton::clicked, this, [this]() {
        navigateTreeSearchMatch(1);
    });
    connect(m_treeSearchRestoreButton, &QPushButton::clicked, this, [this]() {
        if (m_treeSearchEdit) {
            const QSignalBlocker blocker(m_treeSearchEdit);
            m_treeSearchEdit->clear();
        }
        showTreeSearchResults(QString());
    });
    connect(m_treeSearchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (text.trimmed().isEmpty() && m_treeSearchActive) {
            onTreeSearchTextChanged(QString());
        }
    });

    connect(m_activeSearchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!m_activeList) return;
        const QString q = text.trimmed();
        QTreeWidgetItem* firstMatch = nullptr;
        QItemSelection matchedRows;
        int openMatchStart = -1;
        const int itemCount = m_activeList->topLevelItemCount();
        QAbstractItemModel* activeModel = m_activeList->model();
        QItemSelectionModel* activeSelection = m_activeList->selectionModel();
        const bool updatesWereEnabled = m_activeList->updatesEnabled();
        m_activeList->setUpdatesEnabled(false);
        {
            QSignalBlocker listBlocker(m_activeList);
            std::unique_ptr<QSignalBlocker> selectionBlocker;
            if (activeSelection) selectionBlocker.reset(new QSignalBlocker(activeSelection));

            auto closeMatchRange = [&](int endRow) {
                if (openMatchStart < 0 || !activeModel || endRow < openMatchStart) return;
                matchedRows.select(activeModel->index(openMatchStart, 0),
                                   activeModel->index(endRow, 0));
                openMatchStart = -1;
            };

            for (int i = 0; i < itemCount; ++i) {
                QTreeWidgetItem* item = m_activeList->topLevelItem(i);
                if (!item) {
                    closeMatchRange(i - 1);
                    continue;
                }
                const bool match = !q.isEmpty() &&
                    item->text(0).contains(q, Qt::CaseInsensitive);
                if (item->isHidden()) item->setHidden(false);
                if (match) {
                    if (openMatchStart < 0) openMatchStart = i;
                    if (!firstMatch) firstMatch = item;
                } else {
                    closeMatchRange(i - 1);
                }
            }
            closeMatchRange(itemCount - 1);

            if (activeSelection) {
                activeSelection->select(matchedRows,
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
            if (firstMatch) {
                m_activeList->setCurrentItem(firstMatch, 0, QItemSelectionModel::NoUpdate);
            }
        }
        m_activeList->setUpdatesEnabled(updatesWereEnabled);
        if (updatesWereEnabled && m_activeList->viewport()) m_activeList->viewport()->update();
        if (firstMatch) {
            m_activeList->scrollToItem(firstMatch);
        }
        syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
        syncActiveScrollToCanvas();
        });
    connect(m_canvas, &WaveCanvas::cursorMoved, this, &MainWindow::onCursorMoved);
    connect(m_canvas, &WaveCanvas::hoverMoved, this, &MainWindow::onHoverMoved);
    connect(m_canvas, &WaveCanvas::viewportChanged, this, &MainWindow::onViewportChanged);
    connect(m_canvas, &WaveCanvas::viewportTargetRequested,
            this, &MainWindow::scheduleAnimationTargetDataLoad);
    connect(m_canvas, &WaveCanvas::viewportRangeSelected,
            this, &MainWindow::onViewportRangeSelected);

    connect(m_canvas, &WaveCanvas::entryClicked, this, [this](int row, bool ctrlHeld) {
        if (row < 0 || row >= m_activeList->topLevelItemCount()) return;
        if (QTreeWidgetItem* item = m_activeList->topLevelItem(row)) {
            if (ctrlHeld) {
                const bool nextSelected = !item->isSelected();
                item->setSelected(nextSelected);
                m_activeList->setCurrentItem(item);
            }
            else {
                m_activeList->clearSelection();
                item->setSelected(true);
                m_activeList->setCurrentItem(item);
            }
            m_activeList->scrollToItem(item);
            syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
        }
        });

    connect(m_activeList->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        rebuildVisibleSignals();
        refreshActiveValueLabels();
        onActiveCurrentItemChanged(m_activeList->currentItem(), nullptr);
        });
    connect(m_activeList, &QTreeWidget::currentItemChanged, this, &MainWindow::onActiveCurrentItemChanged);
    connect(m_activeList, &QTreeWidget::itemSelectionChanged, this, [this]() {
        syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
        });
    connect(m_activeList->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        syncActiveScrollToCanvas();
        scheduleRefreshActiveValueLabels(0);
    });
    connect(m_activeList, &ActiveSignalListWidget::deleteRequested, this, &MainWindow::removeActiveItem);
    connect(m_activeList, &ActiveSignalListWidget::formatRequested, this, &MainWindow::setActiveItemFormat);
    connect(m_activeList, &ActiveSignalListWidget::signalIndexesDropped, this, &MainWindow::addSignalIndexesToActive);
    connect(m_activeList, &ActiveSignalListWidget::activeRowsReordered, this, [this]() {
        rebuildVisibleSignals();
        refreshActiveValueLabels();
        onActiveCurrentItemChanged(m_activeList->currentItem(), nullptr);
    });

    m_activeValueRefreshTimer = new QTimer(this);
    m_activeValueRefreshTimer->setSingleShot(true);
    connect(m_activeValueRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshActiveValueLabels();
    });

    m_viewportLoadTimer = new QTimer(this);
    m_viewportLoadTimer->setSingleShot(true);
    connect(m_viewportLoadTimer, &QTimer::timeout, this, &MainWindow::startPendingViewportDataLoad);

    clampWindowToAvailableScreen();
}

void MainWindow::applyTheme() {
    setStyleSheet(R"(
        QWidget {
            background: #64707C;
            color: #F4F7FA;
            font-family: "Segoe UI", "Microsoft YaHei";
            font-size: 10.2pt;
        }
        QMainWindow {
            background: #5B6773;
        }
        #toolbarCard {
            background: #D7DADF;
            border: none;
            border-top-left-radius: 10px;
            border-top-right-radius: 10px;
            border-bottom-left-radius: 0px;
            border-bottom-right-radius: 0px;
        }
        #toolbarContents {
            background: transparent;
            border: none;
        }
        #darkPanel {
            background: #76818C;
            border: 2px solid #FFFFFF;
            border-radius: 10px;
        }
        #wavePanel {
            background: #0A0A0A;
            border: 1px solid #FFFFFF;
            border-radius: 10px;
        }
        QLineEdit#windowRangeEdit {
            min-height: 14px;
            padding: 5px 8px;
        }
        #windowRangeSeparator {
            color: #3F4650;
            font-weight: 700;
            background: transparent;
        }
        #panelTitle {
            font-size: 10.8pt;
            font-weight: 700;
            color: #FFFFFF;
            background: transparent;
        }
        #statChip {
            padding: 4px 10px;
            border-radius: 8px;
            background: #111827;
            border: 1px solid #B8C7D8;
            color: #F8FAFC;
        }
        QPushButton {
            background: #ECEEF1;
            border: 1px solid #B5BAC1;
            border-radius: 8px;
            padding: 5px 10px;
            color: #2E3945;
            font-weight: 600;
        }
        QPushButton:hover {
            border-color: #D7DBE1;
            background: #F4F5F7;
        }
        QPushButton:pressed {
            background: #DCDDDF;
        }
        QPushButton:checked {
            background: #6D8FB2;
            border-color: #DDEBFA;
            color: #FFFFFF;
        }
        QPushButton#searchOptionButton {
            padding: 0px;
            min-width: 34px;
            max-width: 34px;
            min-height: 30px;
            max-height: 30px;
            border-radius: 10px;
        }
        QLineEdit {
            background: #57616B;
            border: 1px solid #FFFFFF;
            border-radius: 8px;
            padding: 7px 10px;
            color: #F8FAFC;
            selection-background-color: #6D8FB2;
        }
        QTreeWidget, QTreeView {
            background: #5C6670;
            border: 1px solid #FFFFFF;
            border-radius: 8px;
            outline: none;
            padding: 4px;
            alternate-background-color: #66717B;
            color: #F2F4F7;
        }
        QTreeWidget::item, QTreeView::item {
            background: transparent;
            padding: 4px 6px;
            border-radius: 0px;
            margin: 0px;
            border-bottom: 1px solid #8C98A4;
        }
        QTreeWidget::item:selected, QTreeView::item:selected {
            background: #688BB4;
            color: #FFFFFF;
        }
        QHeaderView::section {
            background: #6A7580;
            color: #FFFFFF;
            border: none;
            border-bottom: 1px solid #FFFFFF;
            border-right: 1px solid #7A8694;
            padding: 6px 8px;
            font-weight: 600;
        }
        QSplitter::handle {
            background: #C8D0D8;
            margin: 1px 0;
        }
        QScrollBar:vertical, QScrollBar:horizontal {
            background: #111111;
            border: none;
            margin: 0px;
        }
        QScrollBar:vertical { width: 12px; }
        QScrollBar:horizontal { height: 12px; }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: #FFFFFF;
            border-radius: 5px;
            min-height: 28px;
            min-width: 28px;
        }
        QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
            border: none;
        }
        QTreeWidget::branch, QTreeView::branch {
            background: transparent;
        }
    )");
}

QString MainWindow::signalDisplayName(int signalIndex) const {
    if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return QString();

    if (m_signalTreeModel) {
        const QString full = m_signalTreeModel->fullPathForSignalIndex(signalIndex);
        if (!full.isEmpty()) return full;
    }

    return waveSignalFullPath(m_wave, signalIndex);
}

QString MainWindow::formatNameWithRange(int signalIndex) const {
    if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return QString();

    const WaveSignal& sig = m_wave.signalList.at(signalIndex);
    const QString name = signalDisplayName(signalIndex);
    return formatNameWidthBeforeCompareSuffix(name, sig.width);
}

QString MainWindow::formatNameWithRange(const WaveSignal& sig) const {
    return formatNameWidthBeforeCompareSuffix(sig.name, sig.width);
}

namespace {
QString agentKindName(SignalKind kind) {
    return kind == SignalKind::Bit ? QStringLiteral("bit") : QStringLiteral("bus");
}

QString agentRadixName(ValueRadix radix) {
    switch (radix) {
    case ValueRadix::Bin: return QStringLiteral("bin");
    case ValueRadix::Hex: return QStringLiteral("hex");
    case ValueRadix::Dec: return QStringLiteral("dec");
    case ValueRadix::Int: return QStringLiteral("int");
    case ValueRadix::UInt: return QStringLiteral("uint");
    case ValueRadix::Float: return QStringLiteral("float");
    case ValueRadix::Int64: return QStringLiteral("int64");
    case ValueRadix::UInt64: return QStringLiteral("uint64");
    case ValueRadix::Double: return QStringLiteral("double");
    }
    return QStringLiteral("bin");
}

QJsonObject agentTime(qint64 tick) {
    QJsonObject out;
    out.insert(QStringLiteral("tick"), QString::number(tick));
    out.insert(QStringLiteral("cycle"), waveFormatDisplayTime(tick));
    return out;
}

bool parseAgentTick(const QJsonObject& object, const QString& tickKey,
                    const QString& cycleKey, qint64* result) {
    if (!result) return false;
    const QJsonValue tick = object.value(tickKey);
    if (!tick.isUndefined()) {
        bool ok = false;
        qint64 parsed = 0;
        if (tick.isString()) {
            parsed = tick.toString().trimmed().toLongLong(&ok);
        } else if (tick.isDouble()) {
            const double value = tick.toDouble();
            ok = std::isfinite(value) && std::floor(value) == value &&
                 value >= double(std::numeric_limits<qint64>::min()) &&
                 value <= double(std::numeric_limits<qint64>::max());
            if (ok) parsed = qint64(value);
        }
        if (ok) { *result = parsed; return true; }
        return false;
    }
    const QJsonValue cycle = object.value(cycleKey);
    if (cycle.isUndefined()) return false;
    return waveParseDisplayTime(cycle.isString() ? cycle.toString() : QString::number(cycle.toDouble(), 'g', 17), *result);
}

QJsonObject agentSampleObject(const WaveSignal& signal, const WaveSample& input) {
    WaveSample sample = input;
    hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
    QJsonObject out;
    out.insert(QStringLiteral("time"), agentTime(sample.time));
    out.insert(QStringLiteral("value"), waveSampleRawText(signal.kind, signal.width, signal.defaultRadix, sample));
    out.insert(QStringLiteral("raw_hex"), QStringLiteral("0x") + QString::number(sample.rawBits, 16).toUpper());
    out.insert(QStringLiteral("z"), sample.isZ);
    out.insert(QStringLiteral("absent"), sample.isAbsent);
    return out;
}
}

bool MainWindow::ensureAgentSignalRangeLoaded(int signalIndex, qint64 start, qint64 end, QString* error) {
    if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) {
        if (error) *error = QStringLiteral("Signal index is out of range.");
        return false;
    }
    WaveSignal& signal = m_wave.signalList[signalIndex];
    if (signal.samplesLoaded || waveSignalRawSamplesCoverRange(signal, start, end)) return true;
    if (!m_currentWaveSupportsOnDemand || !m_waveReader || signal.signalId < 0) {
        if (error) *error = QStringLiteral("Raw samples are not available for this range.");
        return false;
    }

    WaveFile loaded;
    QString loadError;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(*m_waveReaderMutex);
        ok = m_waveReader->loadSignals(QVector<int>() << signal.signalId,
                                       loaded,
                                       loadError,
                                       kViewerOnDemandSampleBudget,
                                       start,
                                       end);
    }
    if (!ok) {
        if (error) *error = loadError;
        return false;
    }
    for (WaveSignal& loadedSignal : loaded.signalList) {
        if (loadedSignal.signalId != signal.signalId) continue;
        mergeRawSamples(signal, std::move(loadedSignal.samples));
        signal.supportsZState = loadedSignal.supportsZState;
        break;
    }
    WaveLodValidRange range;
    range.start = start;
    range.end = end;
    signal.rawLoadedRanges.push_back(range);
    compactLodRanges(signal.rawLoadedRanges);
    rebuildWaveSignalDerivedCaches(signal);
    if (m_canvas) {
        m_canvas->invalidateSignalSampleCaches(QVector<int>() << signalIndex);
        m_canvas->update();
    }
    return true;
}

QJsonValue MainWindow::handleAgentRpc(const QString& method,
                                      const QJsonObject& params,
                                      int* errorCode,
                                      QString* errorMessage) {
    if (errorCode) *errorCode = 0;
    if (errorMessage) errorMessage->clear();
    const auto fail = [&](int code, const QString& message) -> QJsonValue {
        if (errorCode) *errorCode = code;
        if (errorMessage) *errorMessage = message;
        return QJsonValue();
    };
    const auto resolveSignal = [&](const QJsonObject& object) -> int {
        if (object.value(QStringLiteral("index")).isDouble()) {
            const int index = object.value(QStringLiteral("index")).toInt(-1);
            return index >= 0 && index < m_wave.signalList.size() ? index : -1;
        }
        if (object.value(QStringLiteral("id")).isDouble()) {
            const int id = object.value(QStringLiteral("id")).toInt(-1);
            if (id >= 0 && id < m_signalIndexBySignalId.size()) return m_signalIndexBySignalId.at(id) - 1;
            return -1;
        }
        const QString path = object.value(QStringLiteral("path")).toString();
        if (!path.isEmpty()) {
            for (int i = 0; i < m_wave.signalList.size(); ++i) {
                if (signalDisplayName(i) == path) return i;
            }
        }
        return -1;
    };
    const auto signalDescription = [&](int index) {
        const WaveSignal& signal = m_wave.signalList.at(index);
        QJsonObject out;
        out.insert(QStringLiteral("index"), index);
        out.insert(QStringLiteral("id"), signal.signalId);
        out.insert(QStringLiteral("storage_id"), signal.storageId);
        out.insert(QStringLiteral("path"), signalDisplayName(index));
        out.insert(QStringLiteral("kind"), agentKindName(signal.kind));
        out.insert(QStringLiteral("width"), signal.width);
        out.insert(QStringLiteral("radix"), agentRadixName(signal.defaultRadix));
        out.insert(QStringLiteral("supports_z"), signal.supportsZState);
        out.insert(QStringLiteral("samples_fully_loaded"), signal.samplesLoaded);
        out.insert(QStringLiteral("procedural_clock"), signal.proceduralClock);
        if (signal.proceduralClock) {
            out.insert(QStringLiteral("clock_initial_value"), signal.clockInitialValue);
            out.insert(QStringLiteral("clock_toggle_period_ticks"),
                       QString::number(signal.clockTogglePeriodTicks));
        }
        return out;
    };

    if (method == QStringLiteral("rpc.ping")) {
        QJsonObject out;
        out.insert(QStringLiteral("viewer"), QStringLiteral("WaveViewer"));
        out.insert(QStringLiteral("api_version"), 1);
        out.insert(QStringLiteral("pid"), QString::number(QCoreApplication::applicationPid()));
        return out;
    }
    if (method == QStringLiteral("rpc.methods")) {
        return QJsonArray::fromStringList(QStringList()
            << QStringLiteral("rpc.ping") << QStringLiteral("rpc.methods")
            << QStringLiteral("viewer.state") << QStringLiteral("viewer.open") << QStringLiteral("viewer.raise")
            << QStringLiteral("signals.search") << QStringLiteral("signals.describe")
            << QStringLiteral("signals.value") << QStringLiteral("signals.transitions")
            << QStringLiteral("active.list") << QStringLiteral("active.add")
            << QStringLiteral("active.remove") << QStringLiteral("active.clear")
            << QStringLiteral("view.set") << QStringLiteral("view.reset")
            << QStringLiteral("view.zoom") << QStringLiteral("view.pan")
            << QStringLiteral("cursor.set") << QStringLiteral("navigation.change"));
    }
    if (method == QStringLiteral("viewer.state")) {
        QJsonObject out;
        out.insert(QStringLiteral("file"), m_currentWaveFilePath);
        out.insert(QStringLiteral("title"), m_wave.meta.title);
        out.insert(QStringLiteral("timescale"), m_wave.meta.timescale);
        out.insert(QStringLiteral("signal_count"),
                   QJsonValue(static_cast<double>(m_wave.signalList.size())));
        out.insert(QStringLiteral("range"), QJsonObject{{QStringLiteral("start"), agentTime(m_wave.meta.start)},
                                                         {QStringLiteral("end"), agentTime(m_wave.meta.end)}});
        if (m_canvas) {
            out.insert(QStringLiteral("view"), QJsonObject{{QStringLiteral("start"), agentTime(m_canvas->viewStart())},
                                                            {QStringLiteral("end"), agentTime(m_canvas->viewEnd())}});
            out.insert(QStringLiteral("cursor"), m_canvas->cursorTime() >= 0 ? QJsonValue(agentTime(m_canvas->cursorTime())) : QJsonValue());
        }
        out.insert(QStringLiteral("loading"), m_viewportLoadPending || m_viewportLoadInFlight);
        return out;
    }
    if (method == QStringLiteral("viewer.open")) {
        const QString path = params.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) return fail(-32602, QStringLiteral("'path' is required."));
        if (!openWaveFilePath(path, false)) return fail(-32010, QStringLiteral("Unable to open WVZ4 file."));
        return QJsonObject{{QStringLiteral("opened"), true}, {QStringLiteral("file"), m_currentWaveFilePath}};
    }
    if (method == QStringLiteral("viewer.raise")) {
        showNormal(); raise(); activateWindow();
        return QJsonObject{{QStringLiteral("ok"), true}};
    }
    if (method == QStringLiteral("signals.search")) {
        const QString query = params.value(QStringLiteral("query")).toString();
        const bool caseSensitive = params.value(QStringLiteral("case_sensitive")).toBool(false);
        const int offset = qMax(0, params.value(QStringLiteral("offset")).toInt(0));
        const int limit = qBound(1, params.value(QStringLiteral("limit")).toInt(100), 1000);
        const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
        QJsonArray matches;
        int skipped = 0;
        int scanned = 0;
        bool more = false;
        for (int i = 0; i < m_wave.signalList.size(); ++i) {
            ++scanned;
            const QString path = signalDisplayName(i);
            if (!query.isEmpty() && !path.contains(query, cs)) continue;
            if (skipped++ < offset) continue;
            if (matches.size() >= limit) { more = true; break; }
            matches.append(signalDescription(i));
        }
        return QJsonObject{{QStringLiteral("signals"), matches},
                           {QStringLiteral("offset"), offset},
                           {QStringLiteral("more"), more},
                           {QStringLiteral("scanned"), scanned}};
    }
    if (method == QStringLiteral("signals.describe")) {
        const int index = resolveSignal(params);
        if (index < 0) return fail(-32004, QStringLiteral("Signal was not found."));
        return signalDescription(index);
    }
    if (method == QStringLiteral("signals.value") || method == QStringLiteral("signals.transitions")) {
        const int index = resolveSignal(params);
        if (index < 0) return fail(-32004, QStringLiteral("Signal was not found."));
        qint64 start = m_canvas ? m_canvas->viewStart() : m_wave.meta.start;
        qint64 end = m_canvas ? m_canvas->viewEnd() : m_wave.meta.end;
        if (method == QStringLiteral("signals.value")) {
            if (!parseAgentTick(params, QStringLiteral("tick"), QStringLiteral("cycle"), &start)) {
                start = m_canvas && m_canvas->cursorTime() >= 0 ? m_canvas->cursorTime() : start;
            }
            end = start == std::numeric_limits<qint64>::max() ? start : start + 1;
        } else {
            if (params.contains(QStringLiteral("start_tick")) || params.contains(QStringLiteral("start_cycle"))) {
                if (!parseAgentTick(params, QStringLiteral("start_tick"), QStringLiteral("start_cycle"), &start))
                    return fail(-32602, QStringLiteral("Invalid start time."));
            }
            if (params.contains(QStringLiteral("end_tick")) || params.contains(QStringLiteral("end_cycle"))) {
                if (!parseAgentTick(params, QStringLiteral("end_tick"), QStringLiteral("end_cycle"), &end))
                    return fail(-32602, QStringLiteral("Invalid end time."));
            }
            if (end <= start) return fail(-32602, QStringLiteral("End time must be greater than start time."));
        }
        QString loadError;
        if (!ensureAgentSignalRangeLoaded(index, start, end, &loadError)) return fail(-32011, loadError);
        const WaveSignal& signal = m_wave.signalList.at(index);
        if (method == QStringLiteral("signals.value")) {
            if (signal.proceduralClock) {
                const WaveSample sample =
                    waveProceduralClockSampleAtTime(signal, start);
                QJsonObject out;
                out.insert(QStringLiteral("signal"), signalDescription(index));
                out.insert(QStringLiteral("requested_time"), agentTime(start));
                out.insert(QStringLiteral("known"), true);
                out.insert(QStringLiteral("sample"), agentSampleObject(signal, sample));
                return out;
            }
            const auto it = std::upper_bound(signal.samples.constBegin(), signal.samples.constEnd(), start,
                [](qint64 time, const WaveSample& sample) { return time < sample.time; });
            if (it == signal.samples.constBegin()) {
                return QJsonObject{{QStringLiteral("signal"), signalDescription(index)},
                                   {QStringLiteral("time"), agentTime(start)},
                                   {QStringLiteral("known"), false}};
            }
            QJsonObject out;
            out.insert(QStringLiteral("signal"), signalDescription(index));
            out.insert(QStringLiteral("requested_time"), agentTime(start));
            out.insert(QStringLiteral("known"), true);
            out.insert(QStringLiteral("sample"), agentSampleObject(signal, *(it - 1)));
            return out;
        }
        const int limit = qBound(1, params.value(QStringLiteral("limit")).toInt(1000), 10000);
        QJsonArray transitions;
        if (signal.proceduralClock) {
            const QJsonValue valueAtStart =
                agentSampleObject(signal,
                                  waveProceduralClockSampleAtTime(signal, start));
            qint64 transition =
                waveProceduralClockTransitionAtOrAfter(signal, start);
            while (transition >= start && transition < end &&
                   transitions.size() < limit) {
                transitions.append(
                    agentSampleObject(
                        signal,
                        waveProceduralClockSampleAtTime(signal, transition)));
                transition =
                    waveProceduralClockNextTransition(signal, transition);
            }
            const bool truncated = transition >= start && transition < end;
            return QJsonObject{{QStringLiteral("signal"), signalDescription(index)},
                               {QStringLiteral("start"), agentTime(start)},
                               {QStringLiteral("end"), agentTime(end)},
                               {QStringLiteral("value_at_start"), valueAtStart},
                               {QStringLiteral("transitions"), transitions},
                               {QStringLiteral("truncated"), truncated}};
        }
        auto it = std::lower_bound(signal.samples.constBegin(), signal.samples.constEnd(), start,
            [](const WaveSample& sample, qint64 time) { return sample.time < time; });
        QJsonValue valueAtStart;
        if (it != signal.samples.constBegin()) valueAtStart = agentSampleObject(signal, *(it - 1));
        else if (it != signal.samples.constEnd() && it->time == start) valueAtStart = agentSampleObject(signal, *it);
        bool truncated = false;
        for (; it != signal.samples.constEnd() && it->time < end; ++it) {
            if (transitions.size() >= limit) { truncated = true; break; }
            transitions.append(agentSampleObject(signal, *it));
        }
        return QJsonObject{{QStringLiteral("signal"), signalDescription(index)},
                           {QStringLiteral("start"), agentTime(start)},
                           {QStringLiteral("end"), agentTime(end)},
                           {QStringLiteral("value_at_start"), valueAtStart},
                           {QStringLiteral("transitions"), transitions},
                           {QStringLiteral("truncated"), truncated}};
    }
    if (method == QStringLiteral("active.list")) {
        QJsonArray activeEntries;
        for (int row = 0; m_activeList && row < m_activeList->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = m_activeList->topLevelItem(row);
            const int index = signalIndexFromActiveItem(item);
            if (index < 0 || index >= m_wave.signalList.size()) continue;
            QJsonObject entry = signalDescription(index);
            entry.insert(QStringLiteral("row"), row);
            entry.insert(QStringLiteral("format"), agentRadixName(formatFromActiveItem(item)));
            entry.insert(QStringLiteral("current_value"), item->text(1));
            activeEntries.append(entry);
        }
        return activeEntries;
    }
    if (method == QStringLiteral("active.add")) {
        QList<int> indexes;
        const QJsonArray requested = params.value(QStringLiteral("signals")).toArray();
        if (requested.isEmpty()) {
            const int index = resolveSignal(params);
            if (index >= 0) indexes.push_back(index);
        } else {
            for (const QJsonValue& value : requested) {
                const int index = resolveSignal(value.toObject());
                if (index >= 0) indexes.push_back(index);
            }
        }
        if (indexes.isEmpty()) return fail(-32004, QStringLiteral("No matching signals were found."));
        addSignalIndexesToActive(indexes);
        return QJsonObject{{QStringLiteral("added"), indexes.size()}};
    }
    if (method == QStringLiteral("active.remove")) {
        QList<int> rows;
        if (params.value(QStringLiteral("rows")).isArray()) {
            for (const QJsonValue& value : params.value(QStringLiteral("rows")).toArray()) rows.push_back(value.toInt(-1));
        } else {
            const int index = resolveSignal(params);
            for (int row = 0; m_activeList && row < m_activeList->topLevelItemCount(); ++row) {
                if (signalIndexFromActiveItem(m_activeList->topLevelItem(row)) == index) rows.push_back(row);
            }
        }
        removeActiveRows(rows);
        return QJsonObject{{QStringLiteral("removed"), rows.size()}};
    }
    if (method == QStringLiteral("active.clear")) {
        const int count = m_activeList ? m_activeList->topLevelItemCount() : 0;
        onClearActive();
        return QJsonObject{{QStringLiteral("removed"), count}};
    }
    if (method == QStringLiteral("view.set")) {
        qint64 start = 0, end = 0;
        if (!parseAgentTick(params, QStringLiteral("start_tick"), QStringLiteral("start_cycle"), &start) ||
            !parseAgentTick(params, QStringLiteral("end_tick"), QStringLiteral("end_cycle"), &end) || end <= start)
            return fail(-32602, QStringLiteral("Valid start/end times are required."));
        onViewportRangeSelected(start, end);
        return QJsonObject{{QStringLiteral("accepted"), true}};
    }
    if (method == QStringLiteral("view.reset")) {
        resetView(); return QJsonObject{{QStringLiteral("ok"), true}};
    }
    if (method == QStringLiteral("view.zoom")) {
        const double factor = params.value(QStringLiteral("factor")).toDouble(0.0);
        if (!std::isfinite(factor) || factor <= 0.0) return fail(-32602, QStringLiteral("Positive 'factor' is required."));
        if (params.contains(QStringLiteral("center_tick")) || params.contains(QStringLiteral("center_cycle"))) {
            qint64 center = 0;
            if (!parseAgentTick(params, QStringLiteral("center_tick"), QStringLiteral("center_cycle"), &center))
                return fail(-32602, QStringLiteral("Invalid zoom center."));
            m_canvas->setCursorTime(center);
        }
        m_canvas->zoomByFactor(factor);
        return QJsonObject{{QStringLiteral("accepted"), true}};
    }
    if (method == QStringLiteral("view.pan")) {
        qint64 delta = 0;
        if (!parseAgentTick(params, QStringLiteral("delta_tick"), QStringLiteral("delta_cycle"), &delta))
            return fail(-32602, QStringLiteral("'delta_tick' or 'delta_cycle' is required."));
        m_canvas->panBy(delta);
        return QJsonObject{{QStringLiteral("accepted"), true}};
    }
    if (method == QStringLiteral("cursor.set")) {
        qint64 time = 0;
        if (!parseAgentTick(params, QStringLiteral("tick"), QStringLiteral("cycle"), &time))
            return fail(-32602, QStringLiteral("'tick' or 'cycle' is required."));
        const bool ok = m_canvas && m_canvas->setCursorTime(time);
        return QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("time"), agentTime(time)}};
    }
    if (method == QStringLiteral("navigation.change")) {
        const int index = resolveSignal(params);
        if (index < 0) return fail(-32004, QStringLiteral("Signal was not found."));
        QString loadError;
        if (!ensureAgentSignalRangeLoaded(index, m_wave.meta.start, m_wave.meta.end, &loadError))
            return fail(-32011, loadError);
        const bool forward = params.value(QStringLiteral("direction")).toString(QStringLiteral("next")) != QStringLiteral("previous");
        const bool found = m_canvas && m_canvas->jumpToNearestChange(index, forward);
        return QJsonObject{{QStringLiteral("found"), found},
                           {QStringLiteral("cursor"), m_canvas && m_canvas->cursorTime() >= 0
                                ? QJsonValue(agentTime(m_canvas->cursorTime())) : QJsonValue()}};
    }
    return fail(-32601, QStringLiteral("Method not found: %1").arg(method));
}

void MainWindow::loadDemoWave() {
    WaveFile wave;
    wave.meta.title = "demo_soc";
    wave.meta.timescale = "1ns";
    wave.meta.start = 0;
    wave.meta.end = 120;

    auto bitS = [](qint64 t, const char* v) { WaveSample s; s.time = t; s.value = QString::fromLatin1(v); hydrateWaveSampleRawFields(SignalKind::Bit, 1, s); return s; };
    auto busS = [](qint64 t, const char* v, int width) { WaveSample s; s.time = t; s.value = QString::fromLatin1(v); hydrateWaveSampleRawFields(SignalKind::Bus, width, s); return s; };

    auto addSig = [&](const QString& name, SignalKind kind, int width, ValueRadix radix, const QVector<WaveSample>& samples) {
        WaveSignal s;
        s.name = name;
        s.kind = kind;
        s.width = width;
        s.defaultRadix = radix;
        s.currentRadix = radix;
        s.samples = samples;
        wave.signalList.push_back(s);
    };

    addSig("clk", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"0"),bitS(5,"1"),bitS(10,"0"),bitS(15,"1"),bitS(20,"0"),bitS(25,"1"),
        bitS(30,"0"),bitS(35,"1"),bitS(40,"0"),bitS(45,"1"),bitS(50,"0"),bitS(55,"1"),
        bitS(60,"0"),bitS(65,"1"),bitS(70,"0"),bitS(75,"1"),bitS(80,"0"),bitS(85,"1"),
        bitS(90,"0"),bitS(95,"1"),bitS(100,"0"),bitS(105,"1"),bitS(110,"0"),bitS(115,"1")
        });

    addSig("rst_n", SignalKind::Bit, 1, ValueRadix::Bin, { bitS(0,"0"), bitS(18,"1") });

    addSig("top.cpu.ifu.pc", SignalKind::Bus, 32, ValueRadix::Hex, {
        busS(0,"0x00000000",32),
        busS(20,"0x00000004",32),
        busS(40,"0x00000008",32),
        busS(60,"0x0000000C",32),
        busS(80,"0x00000010",32),
        busS(100,"0x00000014",32)
        });

    addSig("top.cpu.ctrl.state", SignalKind::Bus, 3, ValueRadix::Bin, {
        busS(0,"0b000",3),
        busS(20,"0b001",3),
        busS(35,"0b010",3),
        busS(60,"0b011",3),
        busS(85,"0b100",3)
        });

    addSig("top.cpu.exe.valid", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"0"),bitS(30,"1"),bitS(52,"0"),bitS(76,"1"),bitS(98,"0")
        });

    addSig("top.cpu.exe.ready", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"1"),bitS(46,"0"),bitS(68,"1")
        });

    addSig("top.uart.tx_busy", SignalKind::Bit, 1, ValueRadix::Bin, {
        bitS(0,"0"),bitS(72,"1"),bitS(104,"0")
        });

    addSig("top.uart.tx_data", SignalKind::Bus, 8, ValueRadix::Hex, {
        busS(0,"0x00",32),
        busS(72,"0x55",32),
        busS(88,"0xA3",32),
        busS(104,"0x1C",32)
        });

    applyWave(std::move(wave));
}

void MainWindow::applyWave(WaveFile&& wave) {
    stopSignalConditionSearch();
    stopTreeWarmup();
    m_treeSearchActive = false;
    m_treeSearchCropMode = false;
    m_treeSearchMatchedNodeIds.clear();
    m_treeSearchAutoExpandedNodeIds.clear();
    m_treeSearchCurrentMatch = -1;
    m_treeSearchSnapshotValid = false;
    m_treeSearchSavedExpandedNodePaths.clear();
    m_treeSearchSavedSelectedNodeIds.clear();
    m_treeSearchSavedCurrentNodeId = -1;
    m_treeSearchSavedTopNodeId = -1;
    m_treeSearchSavedTopNodeOffset = 0;
    ++m_treeSearchRestoreGeneration;
    if (m_treeSearchRestoreButton) m_treeSearchRestoreButton->setEnabled(false);
    if (m_treeSearchPrevButton) m_treeSearchPrevButton->setEnabled(false);
    if (m_treeSearchNextButton) m_treeSearchNextButton->setEnabled(false);
    if (m_signalConditionPrevButton) m_signalConditionPrevButton->setEnabled(false);
    if (m_signalConditionNextButton) m_signalConditionNextButton->setEnabled(false);
    m_pendingSessionSignals.clear();
    m_pendingSessionSignalRestore = false;
    if (m_treeSearchEdit) {
        const QSignalBlocker blocker(m_treeSearchEdit);
        m_treeSearchEdit->clear();
    }

    const bool perf = viewerPerfLogEnabled();
    QElapsedTimer totalTimer;
    QElapsedTimer stepTimer;
    if (perf) {
        totalTimer.start();
        stepTimer.start();
    }

    m_wave = std::move(wave);
    m_valueFindHits.clear();
    m_valueFindSignalIndexes.clear();
    m_valueFindCurrentHit = -1;
    m_valueFindSummaryBase = QStringLiteral("No value search has been run for this wave.");
    if (m_valueFindResults) m_valueFindResults->clear();
    updateValueFindNavigationState();
    if (perf) {
        viewerPerfLog("apply.move_assign", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size());
    }

    if (!m_wave.tree.signalIndexBySignalId.isEmpty()) {
        m_signalIndexBySignalId = std::move(m_wave.tree.signalIndexBySignalId);
    } else {
        m_signalIndexBySignalId.clear();
        if (!m_wave.signalList.empty()) {
            const int maxSignalId = m_wave.signalList.back().signalId;
            if (maxSignalId >= 0 && maxSignalId <= 100000000) {
                m_signalIndexBySignalId.resize(maxSignalId + 1);
                std::fill(m_signalIndexBySignalId.begin(), m_signalIndexBySignalId.end(), 0);
            }
        }
        for (int i = 0; i < m_wave.signalList.size(); ++i) {
            if (m_wave.signalList[i].signalId < 0) m_wave.signalList[i].signalId = i;
            const int sid = m_wave.signalList.at(i).signalId;
            if (sid >= 0 && sid <= 100000000) {
                if (m_signalIndexBySignalId.size() <= sid) m_signalIndexBySignalId.resize(sid + 1);
                m_signalIndexBySignalId[sid] = i + 1; // store +1 so zero means missing
            }
            if ((m_wave.signalList.at(i).samplesLoaded || !m_wave.signalList.at(i).samples.isEmpty()) &&
                !m_wave.signalList.at(i).changeTimesReady) {
                rebuildWaveSignalDerivedCaches(m_wave.signalList[i]);
            }
        }
    }
    if (perf) {
        viewerPerfLog("apply.signal_index", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size());
    }

    updateMetaLabel();
    if (perf) {
        viewerPerfLog("apply.meta", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size());
    }

    rebuildTree();
    if (perf) {
        viewerPerfLog("apply.rebuild_tree", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size());
    }

    m_activeList->clear();
    m_canvas->setWave(&m_wave);
    const bool restoredSession = !m_currentWaveFilePath.isEmpty() &&
                                 loadViewerSessionState();
    if (perf) {
        viewerPerfLog("apply.canvas_bind", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size(),
                      m_activeList->topLevelItemCount());
    }

    QList<int> initialSignalIndexes;
    const bool comparisonWave = !m_wave.meta.compareLeftPath.isEmpty() ||
                                !m_wave.meta.compareRightPath.isEmpty();
    if (m_signalTreeModel && !comparisonWave) {
        for (int nodeId : m_signalTreeModel->roots) {
            const int signalIndex = m_signalTreeModel->nodeSignalIndex(nodeId);
            if (signalIndex >= 0 &&
                m_signalTreeModel->nodeNameString(nodeId).compare(
                    QStringLiteral("clk"), Qt::CaseInsensitive) == 0) {
                initialSignalIndexes.push_back(signalIndex);
                break;
            }
        }
    }
    if (!restoredSession) addSignalIndexesToActive(initialSignalIndexes);
    if (perf) {
        viewerPerfLog("apply.first_active", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size(),
                      m_activeList->topLevelItemCount());
    }

    rebuildVisibleSignals();
    refreshActiveValueLabels();
    if (perf) {
        viewerPerfLog("apply.final_refresh", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size(),
                      m_activeList->topLevelItemCount());
        viewerPerfLog("apply.total", totalTimer.elapsed(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size(),
                      m_activeList->topLevelItemCount());
    }
    scheduleTreeWarmup();
    retryPendingViewerSessionRestore();
}

void MainWindow::updateMetaLabel() {
    const QString rangeText = QStringLiteral("%1 ~ %2")
        .arg(formatInternalDisplayTime(m_wave.meta.start),
             waveFormatDisplayCycleRangeEnd(m_wave.meta.end));
    auto displayRangeText = [](qint64 start, qint64 end) {
        return QStringLiteral("%1 ~ %2")
            .arg(formatInternalDisplayTime(start),
                 waveFormatDisplayCycleRangeEnd(end));
    };

    QString title = QStringLiteral("Wave Viewer");
    if (m_wave.meta.hasCompareSources) {
        title += QStringLiteral(" - %1 [%2] vs %3 [%4]")
            .arg(m_wave.meta.compareLeftLabel,
                 displayRangeText(m_wave.meta.compareLeftStart, m_wave.meta.compareLeftEnd),
                 m_wave.meta.compareRightLabel,
                 displayRangeText(m_wave.meta.compareRightStart, m_wave.meta.compareRightEnd));
    } else if (!m_currentWaveFilePath.isEmpty()) {
        title += QStringLiteral(" - %1 [%2]")
            .arg(QFileInfo(m_currentWaveFilePath).fileName(), rangeText);
    } else if (!m_wave.meta.title.isEmpty()) {
        title += QStringLiteral(" - %1 [%2]").arg(m_wave.meta.title, rangeText);
    }
    setWindowTitle(title);

    if (m_jumpTimeEdit) {
        m_jumpTimeEdit->setPlaceholderText(QString::fromUtf8("跳转时间"));
        m_jumpTimeEdit->setToolTip(QString::fromUtf8("输入显示时间后按 Enter 跳转。合理范围：%1").arg(rangeText));
    }
}



bool MainWindow::openWaveFilePath(const QString& path, bool showError) {
    if (path.isEmpty()) return false;
    if (!hasWvz4Suffix(path)) {
        if (showError) {
            QMessageBox::critical(this, QStringLiteral("Open failed"), unsupportedWaveFormatError(path));
        }
        return false;
    }

    const bool perf = viewerPerfLogEnabled();
    QElapsedTimer totalTimer;
    QElapsedTimer stepTimer;
    if (perf) {
        totalTimer.start();
        stepTimer.start();
    }

    WaveFile wave;
    QString error;
    std::shared_ptr<WaveParser4Reader> reader(new WaveParser4Reader);
    const bool ok = reader->open(path, error);
    if (ok) wave = reader->takeDirectoryWave();
    if (perf) {
        viewerPerfLog("open.load", stepTimer.restart(),
                      waveSignalCount(wave.signalList),
                      wave.tree.nodesById.size());
    }
    if (!ok) {
        if (!showError) return false;
        QMessageBox::critical(this, QString::fromUtf8("打开失败"), error);
        return false;
    }

    saveViewerSessionState();
    m_currentWaveFilePath = path;
    m_currentWaveSupportsOnDemand = true;
    ++m_viewportLoadSerial;
    if (m_viewportLoadTimer) m_viewportLoadTimer->stop();
    if (m_viewportLoadThread.joinable()) m_viewportLoadThread.join();
    if (m_blockCacheLoader) m_blockCacheLoader->stop();
    m_waveReader = std::move(reader);
    m_waveReaderMutex = std::make_shared<std::mutex>();
    ++m_waveFileGeneration;
    m_viewportLoadPending = false;
    m_viewportLoadInFlight = false;
    m_animationTargetLoadScheduled = false;
    m_guardedViewportCommitPending = false;
    m_guardedViewportCommitSerial = 0;
    m_deferredViewportApply = std::function<void()>();
    m_deferredViewportApplySerial = 0;
    m_deferredViewportBucketCycles = 1;
    applyWave(std::move(wave));
    m_blockCacheLoader.reset(new WaveBlockCacheLoader);
    m_blockCacheLoader->start(m_waveReader, m_waveReaderMutex, m_wave, viewerCacheBudgetBytes());
    if (perf) {
        viewerPerfLog("open.apply", stepTimer.restart(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size());
        viewerPerfLog("open.total", totalTimer.elapsed(),
                      waveSignalCount(m_wave.signalList),
                      m_wave.tree.nodesById.size());
    }
    return true;
}

void MainWindow::activateFirstSignalsForBenchmark(int count) {
    if (!m_activeList) return;
    m_activeList->clear();
    QList<int> indexes;
    const int limit =
        qMin(qMax(0, count), waveSignalCount(m_wave.signalList));
    indexes.reserve(limit);
    for (int i = 0; i < limit; ++i) indexes.push_back(i);
    addSignalIndexesToActive(indexes);
}

void MainWindow::jumpSelectedTreeSignalToViewportEventForBenchmark(bool firstEvent) {
    jumpSelectedTreeSignalToViewportEvent(firstEvent);
}

bool MainWindow::runValueFindForBenchmark(const QString& targetText,
                                          int activeSignalCount,
                                          int* hitCount,
                                          quint64* resultChecksum,
                                          qint64* elapsedMs,
                                          qint64 rangeStart,
                                          qint64 rangeEnd) {
    activateFirstSignalsForBenchmark(activeSignalCount);
    if (!m_activeList || m_activeList->topLevelItemCount() <= 0) return false;
    m_activeList->selectAll();
    openValueFindDialog();
    if (!m_valueFindEdit) return false;
    m_valueFindEdit->setText(targetText);
    if (rangeEnd > rangeStart && m_canvas && m_valueFindRangeCombo) {
        m_canvas->commitViewportRange(rangeStart, rangeEnd);
        m_valueFindRangeCombo->setCurrentIndex(1);
    }

    QElapsedTimer timer;
    timer.start();
    runValueFind();
    const qint64 measuredMs = timer.elapsed();

    quint64 checksum = 1469598103934665603ull;
    auto mix = [&](quint64 value) {
        checksum ^= value;
        checksum *= 1099511628211ull;
    };
    for (const ValueFindHit& hit : m_valueFindHits) {
        mix(quint64(quint32(hit.signalIndex)));
        mix(quint64(hit.time));
        mix(quint64(hit.duration));
    }
    if (hitCount) *hitCount = m_valueFindHits.size();
    if (resultChecksum) *resultChecksum = checksum;
    if (elapsedMs) *elapsedMs = measuredMs;
    return true;
}

void MainWindow::selectViewportRangeForBenchmark(qint64 start, qint64 end) {
    onViewportRangeSelected(start, end);
}

void MainWindow::resetViewForBenchmark() {
    resetView();
}

bool MainWindow::benchmarkActiveViewportCoverage(int* covered, int* total) const {
    int coveredCount = 0;
    int totalCount = 0;
    if (m_activeList && m_canvas) {
        const qint64 start = m_canvas->viewStart();
        const qint64 end = m_canvas->viewEnd();
        const int plotWidth = qMax(1, m_canvas->width() - 20);
        for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
            const int signalIndex = signalIndexFromActiveItem(m_activeList->topLevelItem(i));
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
            ++totalCount;
            const WaveSignal& signal = m_wave.signalList.at(signalIndex);
            if (signal.samplesLoaded ||
                waveSignalRawSamplesCoverRange(signal, start, end) ||
                signalHasLoadedLodForWindow(signal, start, end, plotWidth)) {
                ++coveredCount;
            }
        }
    }
    if (covered) *covered = coveredCount;
    if (total) *total = totalCount;
    return totalCount > 0 && coveredCount == totalCount;
}

bool MainWindow::benchmarkValidateRawCaches(QString* error) {
    if (error) error->clear();
    if (!m_activeList || !m_waveReader || !m_waveReaderMutex) return false;
    QVector<int> signalIds;
    for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
        const int signalIndex = signalIndexFromActiveItem(m_activeList->topLevelItem(i));
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& signal = m_wave.signalList.at(signalIndex);
        if (!signal.rawLoadedRanges.isEmpty() && signal.signalId > 0) signalIds.push_back(signal.signalId);
    }
    if (signalIds.isEmpty()) return true;

    WaveFile reference;
    QString loadError;
    {
        std::lock_guard<std::mutex> readerLock(*m_waveReaderMutex);
        if (!m_waveReader->loadSignals(signalIds, reference, loadError, 0, 0,
                                       std::numeric_limits<qint64>::max())) {
            if (error) *error = loadError;
            return false;
        }
    }
    QHash<int, const WaveSignal*> referenceById;
    for (const WaveSignal& signal : reference.signalList) referenceById.insert(signal.signalId, &signal);

    auto lastAtOrBefore = [](const QVector<WaveSample>& samples, qint64 time) {
        int lo = 0;
        int hi = samples.size();
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (samples.at(mid).time <= time) lo = mid + 1;
            else hi = mid;
        }
        return lo - 1;
    };
    for (int sid : signalIds) {
        if (sid <= 0 || sid >= m_signalIndexBySignalId.size()) continue;
        const int targetIndex = m_signalIndexBySignalId.at(sid) - 1;
        if (targetIndex < 0 || targetIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& cached = m_wave.signalList.at(targetIndex);
        const WaveSignal* full = referenceById.value(sid, nullptr);
        if (!full) {
            if (error) *error = QStringLiteral("missing reference signal %1").arg(sid);
            return false;
        }
        for (const WaveLodValidRange& range : cached.rawLoadedRanges) {
            int cachedIndex = lastAtOrBefore(cached.samples, range.start);
            int fullIndex = lastAtOrBefore(full->samples, range.start);
            if (cachedIndex < 0 || fullIndex < 0 ||
                !waveSamplesEquivalent(cached.samples.at(cachedIndex), full->samples.at(fullIndex))) {
                if (error) *error = QStringLiteral("left-edge value mismatch for signal %1 at %2")
                    .arg(sid).arg(range.start);
                return false;
            }
            ++cachedIndex;
            ++fullIndex;
            while (cachedIndex < cached.samples.size() && cached.samples.at(cachedIndex).time <= range.end &&
                   fullIndex < full->samples.size() && full->samples.at(fullIndex).time <= range.end) {
                const WaveSample& a = cached.samples.at(cachedIndex);
                const WaveSample& b = full->samples.at(fullIndex);
                if (a.time != b.time || !waveSamplesEquivalent(a, b)) {
                    if (error) *error = QStringLiteral("transition mismatch for signal %1 at cached=%2 reference=%3")
                        .arg(sid).arg(a.time).arg(b.time);
                    return false;
                }
                ++cachedIndex;
                ++fullIndex;
            }
            const bool cachedRemaining = cachedIndex < cached.samples.size() && cached.samples.at(cachedIndex).time <= range.end;
            const bool fullRemaining = fullIndex < full->samples.size() && full->samples.at(fullIndex).time <= range.end;
            if (cachedRemaining != fullRemaining) {
                if (error) *error = QStringLiteral("transition count mismatch for signal %1").arg(sid);
                return false;
            }
        }
    }
    return true;
}

void MainWindow::openWaveFile() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        QString::fromUtf8("打开波形文件"),
        QString(),
        QStringLiteral("WVZ4 Wave (*.wvz4)"));
    if (path.isEmpty()) return;

    openWaveFilePath(path);
}

void MainWindow::importSignalPathsFromTextFile() {
    if (!m_signalTreeModel || !m_activeList) return;
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Import signal paths"),
        QString(),
        QStringLiteral("Text files (*.txt *.list *.lst);;All files (*.*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, QStringLiteral("Import failed"),
                              QStringLiteral("Cannot read %1: %2")
                                  .arg(path, file.errorString()));
        return;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    QList<int> signalIndexes;
    QStringList missingPaths;
    QSet<QString> seenPaths;
    int lineNumber = 0;
    while (!stream.atEnd()) {
        ++lineNumber;
        const QString signalPath = stream.readLine().trimmed();
        if (signalPath.isEmpty() || signalPath.startsWith(QLatin1Char('#'))) continue;
        if (seenPaths.contains(signalPath)) continue;
        seenPaths.insert(signalPath);

        int signalIndex = -1;
        int width = 1;
        bool ambiguous = false;
        if (m_signalTreeModel->resolveExactSignalPath(signalPath, signalIndex,
                                                      width, ambiguous) &&
            signalIndex >= 0 && signalIndex < m_wave.signalList.size()) {
            signalIndexes.push_back(signalIndex);
        } else {
            missingPaths.push_back(QStringLiteral("line %1: %2%3")
                .arg(lineNumber)
                .arg(signalPath)
                .arg(ambiguous ? QStringLiteral(" (ambiguous)") : QString()));
        }
    }

    addSignalIndexesToActive(signalIndexes);
    const QString summary = QStringLiteral("Imported %1 signal(s); %2 path(s) not found.")
        .arg(signalIndexes.size()).arg(missingPaths.size());
    statusBar()->showMessage(summary, 5000);
    if (!missingPaths.isEmpty()) {
        const int shown = qMin(20, missingPaths.size());
        QString details = missingPaths.mid(0, shown).join(QLatin1Char('\n'));
        if (missingPaths.size() > shown) {
            details += QStringLiteral("\n... and %1 more")
                .arg(missingPaths.size() - shown);
        }
        QMessageBox::warning(this, QStringLiteral("Signal import incomplete"),
                             summary + QLatin1Char('\n') + details);
    }
}

bool MainWindow::compareWaveFilePaths(const QString& leftPath,
                                      const QString& rightPath,
                                      bool showProgress,
                                      bool showMessages,
                                      QString* errorMessage,
                                      qint64* elapsedMs,
                                      int* resultSignalCount,
                                      bool eventTimesOnly,
                                      const QString& expectedSamePeerPath) {
    if (errorMessage) errorMessage->clear();
    if (elapsedMs) *elapsedMs = 0;
    if (resultSignalCount) *resultSignalCount = 0;

    if (!hasWvz4Suffix(leftPath) || !hasWvz4Suffix(rightPath) ||
        (!expectedSamePeerPath.isEmpty() &&
         !hasWvz4Suffix(expectedSamePeerPath))) {
        const QString error = QStringLiteral("Compare supports WVZ4 files (*.wvz4) only.");
        if (errorMessage) *errorMessage = error;
        if (showMessages) QMessageBox::critical(this, QStringLiteral("Compare failed"), error);
        return false;
    }

    QElapsedTimer totalTimer;
    totalTimer.start();

    const QFileInfo leftInfo(leftPath);
    const QFileInfo rightInfo(rightPath);
    const QString leftCanonical = leftInfo.exists() ? leftInfo.canonicalFilePath() : QString();
    const QString rightCanonical = rightInfo.exists() ? rightInfo.canonicalFilePath() : QString();
    if (!leftCanonical.isEmpty() && leftCanonical == rightCanonical) {
        const QString error = QStringLiteral("No matching-path signal differs at any cycle.");
        if (errorMessage) *errorMessage = error;
        if (elapsedMs) *elapsedMs = totalTimer.elapsed();
        if (showMessages) {
            QMessageBox::information(this, QStringLiteral("Compare finished"), error);
        }
        return false;
    }

    WaveFile comparedWave;
    QString error;
    bool ok = false;
    CompareProgressState progressState;
    QSet<int> baselineDifferingSignalIndexes;
    CompareBuildOptions formalOptions;
    formalOptions.signalMode = eventTimesOnly
        ? CompareSignalMode::EventTimesOnly
        : CompareSignalMode::EventTimesAndValues;

    auto runComparison = [&](CompareProgressState* progress) {
        if (!expectedSamePeerPath.isEmpty()) {
            CompareBuildOptions baselineOptions;
            // Baseline filtering must remain strict: any value or event-time
            // instability between the expected-same runs removes that path.
            // The event-times-only choice applies only to the formal compare.
            baselineOptions.signalMode = CompareSignalMode::EventTimesAndValues;
            baselineOptions.differingLeftSignalIndexes =
                &baselineDifferingSignalIndexes;
            baselineOptions.emitComparedWave = false;
            if (progress) progress->stage.store(0, std::memory_order_release);
            WaveFile ignoredWave;
            if (!buildComparedWaveFileWvz4Streaming(
                    leftPath, expectedSamePeerPath, ignoredWave, error,
                    progress, baselineOptions)) {
                return false;
            }
            formalOptions.excludedLeftSignalIndexes =
                baselineDifferingSignalIndexes.isEmpty()
                ? nullptr
                : &baselineDifferingSignalIndexes;
        }
        if (progress) progress->stage.store(1, std::memory_order_release);
        return buildComparedWaveFileWvz4Streaming(
            leftPath, rightPath, comparedWave, error, progress, formalOptions);
    };

    if (showProgress) {
        QProgressDialog compareProgress(QStringLiteral("Loading WVZ4 directories..."),
                                        QStringLiteral("Cancel"), 0, 0, this);
        compareProgress.setWindowTitle(QStringLiteral("Compare wave files"));
        compareProgress.setWindowModality(Qt::WindowModal);
        compareProgress.setMinimumDuration(0);
        compareProgress.setAutoClose(false);
        compareProgress.show();

        auto compareFuture = std::async(std::launch::async, [&]() {
            return runComparison(&progressState);
        });

        int lastTotal = 0;
        while (compareFuture.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
            if (compareProgress.wasCanceled()) {
                progressState.cancelRequested.store(true, std::memory_order_release);
                compareProgress.setLabelText(QStringLiteral("Cancelling comparison..."));
            } else {
                const int total = progressState.totalJobs.load(std::memory_order_acquire);
                const int done = progressState.completedJobs.load(std::memory_order_acquire);
                if (total > 0) {
                    if (total != lastTotal) {
                        compareProgress.setRange(0, total);
                        lastTotal = total;
                    }
                    compareProgress.setValue(qBound(0, done, total));
                    const bool baselineStage =
                        progressState.stage.load(std::memory_order_acquire) == 0;
                    compareProgress.setLabelText(
                        baselineStage
                            ? QStringLiteral("Filtering unstable baseline signals %1 / %2...")
                                  .arg(done).arg(total)
                            : QStringLiteral("Streaming formal compare %1 / %2 signals...")
                                  .arg(done).arg(total));
                }
            }
            QCoreApplication::processEvents();
        }
        ok = compareFuture.get();
        const int total = progressState.totalJobs.load(std::memory_order_acquire);
        if (total > 0) {
            compareProgress.setRange(0, total);
            compareProgress.setValue(qMin(total, progressState.completedJobs.load(std::memory_order_acquire)));
        }
        compareProgress.setLabelText(QStringLiteral("Building result tree..."));
        QCoreApplication::processEvents();
        compareProgress.close();
    } else {
        ok = runComparison(nullptr);
    }

    if (!ok) {
        if (!expectedSamePeerPath.isEmpty() &&
            !baselineDifferingSignalIndexes.isEmpty() &&
            (error.startsWith(QStringLiteral("No matching-path signal differs")) ||
             error.startsWith(QStringLiteral("No signal differences")))) {
            error += QString::fromUtf8("\n基线过滤已剔除 %1 个不稳定信号。")
                .arg(baselineDifferingSignalIndexes.size());
        }
        if (errorMessage) *errorMessage = error;
        if (elapsedMs) *elapsedMs = totalTimer.elapsed();
        if (showMessages) {
            QMessageBox::information(this,
                QStringLiteral("Compare finished"),
                error.isEmpty() ? QStringLiteral("No signal differences were found.") : error);
        }
        return false;
    }

    const int comparedSignalCount =
        waveSignalCount(comparedWave.signalList);
    saveViewerSessionState();
    m_currentWaveFilePath.clear();
    m_currentWaveSupportsOnDemand = false;
    if (m_blockCacheLoader) m_blockCacheLoader->stop();
    m_blockCacheLoader.reset();
    m_waveReader.reset();
    m_signalIndexBySignalId.clear();
    applyWave(std::move(comparedWave));

    if (!expectedSamePeerPath.isEmpty()) {
        statusBar()->showMessage(
            QString::fromUtf8("基线过滤剔除 %1 个不稳定信号；正式比较得到 %2 个差异信号。")
                .arg(baselineDifferingSignalIndexes.size())
                .arg(comparedSignalCount / 2),
            8000);
    }

    if (resultSignalCount) *resultSignalCount = comparedSignalCount;
    if (elapsedMs) *elapsedMs = totalTimer.elapsed();
    return true;
}

void MainWindow::compareWaveFiles() {
    const QStringList baselinePaths = QFileDialog::getOpenFileNames(
        this,
        QString::fromUtf8("第一步：选择 1 或 2 个期望相同的基线波形"),
        QString(),
        QStringLiteral("WVZ4 Wave (*.wvz4)"));
    if (baselinePaths.isEmpty()) return;
    if (baselinePaths.size() > 2) {
        QMessageBox::warning(this,
            QString::fromUtf8("比较失败"),
            QString::fromUtf8("第一步只能选择一个或两个基线波形文件。"));
        return;
    }

    const QString targetPath = QFileDialog::getOpenFileName(
        this,
        QString::fromUtf8("第二步：选择要正式比较的波形"),
        QFileInfo(baselinePaths.at(0)).absolutePath(),
        QStringLiteral("WVZ4 Wave (*.wvz4)"));
    if (targetPath.isEmpty()) return;

    QMessageBox modeDialog(QMessageBox::Question,
                           QString::fromUtf8("正式比较方式"),
                           baselinePaths.size() == 2
                               ? QString::fromUtf8(
                                     "将先比较两个基线文件并剔除不稳定信号，"
                                     "再用第一个基线文件与目标文件正式比较。")
                               : QString::fromUtf8(
                                     "将直接比较基线文件与目标文件。"),
                           QMessageBox::Ok | QMessageBox::Cancel,
                           this);
    auto* eventTimesOnlyCheck = new QCheckBox(
        QString::fromUtf8("正式比较只比较事件时刻，不比较数值"),
        &modeDialog);
    modeDialog.setCheckBox(eventTimesOnlyCheck);
    if (modeDialog.exec() != QMessageBox::Ok) return;

    const QString expectedSamePeerPath = baselinePaths.size() == 2
        ? baselinePaths.at(1)
        : QString();
    compareWaveFilePaths(baselinePaths.at(0), targetPath,
                         true, true, nullptr, nullptr, nullptr,
                         eventTimesOnlyCheck->isChecked(),
                         expectedSamePeerPath);
}

void MainWindow::zoomIn() { m_canvas->zoomByFactor(0.70); }
void MainWindow::zoomOut() { m_canvas->zoomByFactor(1.35); }
void MainWindow::panLeft() { const qint64 s = m_canvas->viewEnd() - m_canvas->viewStart(); m_canvas->panBy(static_cast<qint64>(std::llround(-double(s) * 0.18))); }
void MainWindow::panRight() { const qint64 s = m_canvas->viewEnd() - m_canvas->viewStart(); m_canvas->panBy(static_cast<qint64>(std::llround(double(s) * 0.18))); }

QList<int> MainWindow::selectedActiveSignalIndexesForJump() const {
    QList<int> signalIndexes;
    QSet<int> seen;
    if (!m_activeList) return signalIndexes;

    QModelIndexList picked = m_activeList->selectionModel()
        ? m_activeList->selectionModel()->selectedRows(0)
        : QModelIndexList();
    if (picked.isEmpty() && m_activeList->currentIndex().isValid()) {
        picked.push_back(m_activeList->currentIndex());
    }
    std::sort(picked.begin(), picked.end(),
              [](const QModelIndex& a, const QModelIndex& b) {
                  return a.row() < b.row();
              });

    seen.reserve(picked.size());
    for (const QModelIndex& index : picked) {
        const int signalIndex = index.data(RoleSignalIndex).toInt();
        if (signalIndex >= 0 && !seen.contains(signalIndex)) {
            seen.insert(signalIndex);
            signalIndexes.push_back(signalIndex);
        }
    }
    return signalIndexes;
}

QList<int> MainWindow::selectedActiveSignalIndexesForFind() const {
    return selectedActiveSignalIndexesForJump();
}

QList<int> MainWindow::selectedTreeSignalIndexesForViewportJump() const {
    QList<int> signalIndexes;
    if (!m_tree || !m_treeModel || !m_signalTreeModel) return signalIndexes;

    QModelIndexList picked;
    if (m_tree->selectionModel()) {
        picked = m_tree->selectionModel()->selectedRows(0);
    }
    if (picked.isEmpty() && m_tree->currentIndex().isValid()) {
        picked.push_back(m_tree->currentIndex());
    }

    QBitArray seen(waveSignalCount(m_wave.signalList), false);
    QVector<int> pendingNodes;
    pendingNodes.reserve(256);
    auto appendSignal = [&](int signalIndex) {
        if (signalIndex < 0 || signalIndex >= seen.size() || seen.testBit(signalIndex)) return;
        seen.setBit(signalIndex);
        signalIndexes.push_back(signalIndex);
    };

    for (const QModelIndex& index : picked) {
        const QVariant signalValue = index.data(kTreeRoleSignalIndex);
        if (signalValue.isValid()) {
            appendSignal(signalValue.toInt());
            continue;
        }

        const QVariant nodeValue = index.data(kTreeRoleNodeId);
        if (!nodeValue.isValid()) continue;
        pendingNodes.push_back(nodeValue.toInt());
        while (!pendingNodes.isEmpty()) {
            const int nodeId = pendingNodes.takeLast();
            if (!m_signalTreeModel->isValidNodeId(nodeId)) continue;
            const int signalIndex = m_signalTreeModel->nodeSignalIndex(nodeId);
            if (signalIndex >= 0) {
                appendSignal(signalIndex);
                continue;
            }
            const LogicChildList* list = m_signalTreeModel->childListForNode(nodeId);
            if (!list) continue;
            // Reverse-push keeps the recursive implementation's original
            // depth-first order, including its tie-breaking between signals.
            for (int child = list->children.size() - 1; child >= 0; --child) {
                pendingNodes.push_back(list->children[child]);
            }
        }
    }
    return signalIndexes;
}

bool MainWindow::selectActiveSignalByIndex(int signalIndex) {
    if (!m_activeList || signalIndex < 0) return false;
    for (int row = 0; row < m_activeList->topLevelItemCount(); ++row) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(row);
        if (!item || signalIndexFromActiveItem(item) != signalIndex) continue;
        m_activeList->clearSelection();
        item->setSelected(true);
        m_activeList->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        m_activeList->scrollToItem(item);
        syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
        return true;
    }
    return false;
}

void MainWindow::jumpSelectedTreeSignalToViewportEvent(bool firstEvent) {
    if (!m_canvas || m_wave.signalList.empty()) return;
    const QList<int> signalIndexes = selectedTreeSignalIndexesForViewportJump();
    if (signalIndexes.isEmpty()) return;
    if (!ensureSignalLodLoaded(signalIndexes)) return;

    const qint64 start = m_canvas->viewStart();
    const qint64 end = m_canvas->viewEnd();
    const int plotWidth = qMax(1, m_canvas->width() - 20);
    qint64 targetTime = -1;
    int targetSignalIndex = -1;
    QList<int> rawFallbackIndexes;

    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        qint64 t = -1;
        if (waveSignalRawSamplesCoverRange(sig, start, end)) {
            t = signalChangeTimeInRange(sig, start, end, firstEvent);
        }
        if (t < 0) {
            t = signalLodEventTimeInRange(sig, start, end, plotWidth, firstEvent);
        }
        if (t < 0 && !waveSignalRawSamplesCoverRange(sig, start, end) &&
            !canDeferSamplesWithLod(sig)) {
            rawFallbackIndexes.push_back(signalIndex);
        }
        if (t < 0) continue;
        if (targetTime < 0 ||
            (firstEvent ? (t < targetTime) : (t > targetTime))) {
            targetTime = t;
            targetSignalIndex = signalIndex;
        }
    }

    if (targetTime < 0 && !rawFallbackIndexes.isEmpty()) {
        bool reducedByReader = false;
        if (rawFallbackIndexes.size() > treeEventReductionThreshold() &&
            !qEnvironmentVariableIsSet("WV_VIEWER_LEGACY_TREE_EVENT_JUMP") &&
            m_currentWaveSupportsOnDemand && m_waveReader && m_waveReaderMutex &&
            hasWvz4Suffix(m_currentWaveFilePath)) {
            QVector<int> rawSignalIds;
            rawSignalIds.reserve(rawFallbackIndexes.size());
            for (int signalIndex : rawFallbackIndexes) {
                if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
                const int signalId = m_wave.signalList.at(signalIndex).signalId;
                if (signalId > 0) rawSignalIds.push_back(signalId);
            }
            int targetSignalId = -1;
            QString error;
            {
                std::lock_guard<std::mutex> readerLock(*m_waveReaderMutex);
                reducedByReader = m_waveReader->findRawSignalEvent(
                    rawSignalIds, start, end, firstEvent,
                    targetSignalId, targetTime, error,
                    kViewerOnDemandSampleBudget);
            }
            if (reducedByReader && targetSignalId > 0 &&
                targetSignalId < m_signalIndexBySignalId.size()) {
                targetSignalIndex = m_signalIndexBySignalId.at(targetSignalId) - 1;
            }
            if (!reducedByReader && !error.isEmpty()) {
                statusBar()->showMessage(error, 2600);
            }
        }
        if (!reducedByReader) {
            if (!ensureSignalSamplesLoaded(rawFallbackIndexes, false, true, true)) {
                statusBar()->showMessage(QStringLiteral("Raw event scan is too large for this viewport; zoom out to use LOD or narrow the tree selection."), 2600);
                return;
            }
            for (int signalIndex : rawFallbackIndexes) {
                if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
                const WaveSignal& sig = m_wave.signalList.at(signalIndex);
                if (!waveSignalRawSamplesCoverRange(sig, start, end)) continue;
                const qint64 t = signalChangeTimeInRange(sig, start, end, firstEvent);
                if (t < 0) continue;
                if (targetTime < 0 ||
                    (firstEvent ? (t < targetTime) : (t > targetTime))) {
                    targetTime = t;
                    targetSignalIndex = signalIndex;
                }
            }
        }
    }

    if (targetTime < 0 || targetSignalIndex < 0) {
        statusBar()->showMessage(QStringLiteral("No event in current viewport for selected signal."), 1800);
        return;
    }

    if (!selectActiveSignalByIndex(targetSignalIndex)) {
        addSignalToActive(targetSignalIndex);
    }
    m_canvas->setCursorTime(targetTime);
    refreshActiveValueLabels();
}

void MainWindow::addSignalConditionValueRow(const QString& valueText,
                                            const QString& ratioText) {
    if (!m_signalConditionValueRowsLayout || !m_signalConditionSearchDialog) return;

    SignalConditionValueRow row;
    row.widget = new QWidget(m_signalConditionSearchDialog);
    row.widget->setObjectName(QStringLiteral("signalConditionValueRow"));
    auto* layout = new QHBoxLayout(row.widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    row.valueEdit = new QLineEdit(row.widget);
    row.valueEdit->setObjectName(QStringLiteral("signalConditionValue"));
    row.valueEdit->setPlaceholderText(QStringLiteral("值，例如 0、0xA、-1"));
    row.valueEdit->setText(valueText);
    row.ratioEdit = new QLineEdit(row.widget);
    row.ratioEdit->setObjectName(QStringLiteral("signalConditionRatio"));
    row.ratioEdit->setFixedWidth(150);
    row.ratioEdit->setPlaceholderText(QStringLiteral("最小时间占比(%)"));
    row.ratioEdit->setText(ratioText);
    row.ratioEdit->setToolTip(QStringLiteral("留空表示只要求该值存在；填写 0~100 时要求时间占比严格大于该比例。"));
    row.removeButton = new QPushButton(QStringLiteral("删除"), row.widget);
    row.removeButton->setObjectName(QStringLiteral("signalConditionRemoveValue"));
    row.removeButton->setFixedWidth(58);
    layout->addWidget(row.valueEdit, 1);
    layout->addWidget(row.ratioEdit);
    layout->addWidget(row.removeButton);
    m_signalConditionValueRowsLayout->addWidget(row.widget);
    m_signalConditionValueRows.push_back(row);

    connect(row.removeButton, &QPushButton::clicked, this, [this, widget = row.widget]() {
        removeSignalConditionValueRow(widget);
    });
    const auto startOnReturn = [this]() {
        if (!m_signalConditionSearchButton ||
            m_signalConditionSearchButton->isEnabled()) {
            startSignalConditionSearch();
        }
    };
    connect(row.valueEdit, &QLineEdit::returnPressed, this, startOnReturn);
    connect(row.ratioEdit, &QLineEdit::returnPressed, this, startOnReturn);
}

void MainWindow::removeSignalConditionValueRow(QWidget* rowWidget) {
    if (!rowWidget) return;
    for (int i = 0; i < m_signalConditionValueRows.size(); ++i) {
        if (m_signalConditionValueRows.at(i).widget != rowWidget) continue;
        const SignalConditionValueRow row = m_signalConditionValueRows.takeAt(i);
        if (m_signalConditionValueRowsLayout) {
            m_signalConditionValueRowsLayout->removeWidget(row.widget);
        }
        row.widget->deleteLater();
        break;
    }
    if (m_signalConditionValueRows.isEmpty()) addSignalConditionValueRow();
}

void MainWindow::loadSignalConditionSearchSettings() {
    if (!m_signalConditionSearchDialog) return;
    QSettings settings(QStringLiteral("WaveTrace"), QStringLiteral("WaveViewer"));
    settings.beginGroup(QStringLiteral("SignalConditionSearch"));
    if (m_signalConditionScopeCheck) {
        m_signalConditionScopeCheck->setChecked(
            settings.value(QStringLiteral("selectedSubtree"), false).toBool());
    }
    if (m_signalConditionCropTreeCheck) {
        m_signalConditionCropTreeCheck->setChecked(
            settings.value(QStringLiteral("cropTree"), false).toBool());
    }
    if (m_signalConditionNameEdit) {
        m_signalConditionNameEdit->setText(
            settings.value(QStringLiteral("name")).toString());
    }
    if (m_signalConditionRegexCheck) {
        m_signalConditionRegexCheck->setChecked(
            settings.value(QStringLiteral("regex"), true).toBool());
    }
    if (m_signalConditionCaseCheck) {
        m_signalConditionCaseCheck->setChecked(
            settings.value(QStringLiteral("caseSensitive"), false).toBool());
    }
    if (m_signalConditionChangeMinEdit) {
        m_signalConditionChangeMinEdit->setText(
            settings.value(QStringLiteral("changeMin")).toString());
    }
    if (m_signalConditionChangeMaxEdit) {
        m_signalConditionChangeMaxEdit->setText(
            settings.value(QStringLiteral("changeMax")).toString());
    }
    const QStringList values = settings.value(QStringLiteral("values")).toStringList();
    const QStringList ratios = settings.value(QStringLiteral("ratios")).toStringList();
    settings.endGroup();

    while (!m_signalConditionValueRows.isEmpty()) {
        const SignalConditionValueRow row = m_signalConditionValueRows.takeLast();
        if (m_signalConditionValueRowsLayout) {
            m_signalConditionValueRowsLayout->removeWidget(row.widget);
        }
        delete row.widget;
    }
    const int rowCount = qMax(1, qMax(values.size(), ratios.size()));
    for (int i = 0; i < rowCount; ++i) {
        addSignalConditionValueRow(i < values.size() ? values.at(i) : QString(),
                                   i < ratios.size() ? ratios.at(i) : QString());
    }
}

void MainWindow::saveSignalConditionSearchSettings() const {
    if (!m_signalConditionSearchDialog) return;
    if (qEnvironmentVariableIntValue("WV_SIGNAL_SEARCH_BENCHMARK") != 0) return;
    QSettings settings(QStringLiteral("WaveTrace"), QStringLiteral("WaveViewer"));
    settings.beginGroup(QStringLiteral("SignalConditionSearch"));
    settings.setValue(QStringLiteral("selectedSubtree"),
                      m_signalConditionScopeCheck &&
                      m_signalConditionScopeCheck->isChecked());
    settings.setValue(QStringLiteral("cropTree"),
                      m_signalConditionCropTreeCheck &&
                      m_signalConditionCropTreeCheck->isChecked());
    settings.setValue(QStringLiteral("name"),
                      m_signalConditionNameEdit
                          ? m_signalConditionNameEdit->text()
                          : QString());
    settings.setValue(QStringLiteral("regex"),
                      !m_signalConditionRegexCheck ||
                      m_signalConditionRegexCheck->isChecked());
    settings.setValue(QStringLiteral("caseSensitive"),
                      m_signalConditionCaseCheck &&
                      m_signalConditionCaseCheck->isChecked());
    settings.setValue(QStringLiteral("changeMin"),
                      m_signalConditionChangeMinEdit
                          ? m_signalConditionChangeMinEdit->text()
                          : QString());
    settings.setValue(QStringLiteral("changeMax"),
                      m_signalConditionChangeMaxEdit
                          ? m_signalConditionChangeMaxEdit->text()
                          : QString());
    QStringList values;
    QStringList ratios;
    values.reserve(m_signalConditionValueRows.size());
    ratios.reserve(m_signalConditionValueRows.size());
    for (const SignalConditionValueRow& row : m_signalConditionValueRows) {
        values.push_back(row.valueEdit ? row.valueEdit->text() : QString());
        ratios.push_back(row.ratioEdit ? row.ratioEdit->text() : QString());
    }
    settings.setValue(QStringLiteral("values"), values);
    settings.setValue(QStringLiteral("ratios"), ratios);
    settings.endGroup();
    settings.sync();
}

void MainWindow::openSignalConditionSearchDialog() {
    if (!m_signalConditionSearchDialog) {
        m_signalConditionSearchDialog = new QDialog(this);
        m_signalConditionSearchDialog->setObjectName(
            QStringLiteral("signalConditionSearchDialog"));
        m_signalConditionSearchDialog->setWindowTitle(QStringLiteral("条件查找信号"));
        m_signalConditionSearchDialog->resize(760, 560);

        auto* root = new QVBoxLayout(m_signalConditionSearchDialog);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(9);

        m_signalConditionScopeCheck = new QCheckBox(
            QStringLiteral("仅在当前选中节点及其子节点中查找"),
            m_signalConditionSearchDialog);
        m_signalConditionScopeCheck->setObjectName(
            QStringLiteral("signalConditionScope"));
        m_signalConditionScopeCheck->setToolTip(
            QStringLiteral("未勾选时查找整个待选信号树。"));
        root->addWidget(m_signalConditionScopeCheck);

        m_signalConditionCropTreeCheck = new QCheckBox(
            QStringLiteral("Crop tree to matched subtrees"),
            m_signalConditionSearchDialog);
        m_signalConditionCropTreeCheck->setObjectName(
            QStringLiteral("signalConditionCropTree"));
        m_signalConditionCropTreeCheck->setToolTip(
            QStringLiteral("When disabled, keep the full tree and highlight matches."));
        root->addWidget(m_signalConditionCropTreeCheck);

        auto* nameRow = new QHBoxLayout();
        nameRow->addWidget(new QLabel(QStringLiteral("名字"), m_signalConditionSearchDialog));
        m_signalConditionNameEdit = new QLineEdit(m_signalConditionSearchDialog);
        m_signalConditionNameEdit->setObjectName(
            QStringLiteral("signalConditionName"));
        m_signalConditionNameEdit->setPlaceholderText(
            QStringLiteral("留空不限制；正则匹配完整层级路径"));
        m_signalConditionRegexCheck = new QCheckBox(
            QStringLiteral("正则"), m_signalConditionSearchDialog);
        m_signalConditionRegexCheck->setObjectName(
            QStringLiteral("signalConditionRegex"));
        m_signalConditionCaseCheck = new QCheckBox(
            QStringLiteral("区分大小写"), m_signalConditionSearchDialog);
        m_signalConditionCaseCheck->setObjectName(
            QStringLiteral("signalConditionCase"));
        nameRow->addWidget(m_signalConditionNameEdit, 1);
        nameRow->addWidget(m_signalConditionRegexCheck);
        nameRow->addWidget(m_signalConditionCaseCheck);
        root->addLayout(nameRow);

        auto* changeRow = new QHBoxLayout();
        changeRow->addWidget(new QLabel(QStringLiteral("变化次数"), m_signalConditionSearchDialog));
        m_signalConditionChangeMinEdit = new QLineEdit(m_signalConditionSearchDialog);
        m_signalConditionChangeMinEdit->setObjectName(
            QStringLiteral("signalConditionChangeMin"));
        m_signalConditionChangeMinEdit->setPlaceholderText(QStringLiteral("最小值（可空）"));
        m_signalConditionChangeMaxEdit = new QLineEdit(m_signalConditionSearchDialog);
        m_signalConditionChangeMaxEdit->setObjectName(
            QStringLiteral("signalConditionChangeMax"));
        m_signalConditionChangeMaxEdit->setPlaceholderText(QStringLiteral("最大值（可空）"));
        changeRow->addWidget(m_signalConditionChangeMinEdit, 1);
        changeRow->addWidget(new QLabel(QStringLiteral("~"), m_signalConditionSearchDialog));
        changeRow->addWidget(m_signalConditionChangeMaxEdit, 1);
        root->addLayout(changeRow);

        auto* valuesTitleRow = new QHBoxLayout();
        valuesTitleRow->addWidget(new QLabel(
            QStringLiteral("值条件（多条之间为 AND）"),
            m_signalConditionSearchDialog));
        valuesTitleRow->addStretch(1);
        auto* addValueButton = new QPushButton(
            QStringLiteral("增加值条件"), m_signalConditionSearchDialog);
        addValueButton->setObjectName(QStringLiteral("signalConditionAddValue"));
        valuesTitleRow->addWidget(addValueButton);
        root->addLayout(valuesTitleRow);

        auto* valuesScroll = new QScrollArea(m_signalConditionSearchDialog);
        valuesScroll->setWidgetResizable(true);
        valuesScroll->setFrameShape(QFrame::NoFrame);
        valuesScroll->setMinimumHeight(90);
        valuesScroll->setMaximumHeight(230);
        auto* valuesHost = new QWidget(valuesScroll);
        m_signalConditionValueRowsLayout = new QVBoxLayout(valuesHost);
        m_signalConditionValueRowsLayout->setContentsMargins(0, 0, 0, 0);
        m_signalConditionValueRowsLayout->setSpacing(6);
        valuesScroll->setWidget(valuesHost);
        root->addWidget(valuesScroll);

        auto* hint = new QLabel(
            QStringLiteral("所有留空项均表示无要求。时间占比为空时只检查该值是否存在；"
                           "填写比例时使用整个波形时间范围进行精确统计。结果保持父子树结构，最多显示前 5000 个匹配信号。"),
            m_signalConditionSearchDialog);
        hint->setWordWrap(true);
        root->addWidget(hint);

        root->addStretch(1);
        m_signalConditionStatusLabel = new QLabel(
            QStringLiteral("尚未搜索。"), m_signalConditionSearchDialog);
        m_signalConditionStatusLabel->setObjectName(
            QStringLiteral("signalConditionStatus"));
        m_signalConditionStatusLabel->setWordWrap(true);
        root->addWidget(m_signalConditionStatusLabel);

        auto* buttons = new QHBoxLayout();
        m_signalConditionSearchButton = new QPushButton(
            QStringLiteral("查找"), m_signalConditionSearchDialog);
        m_signalConditionSearchButton->setObjectName(
            QStringLiteral("signalConditionSearch"));
        m_signalConditionSearchButton->setDefault(true);
        m_signalConditionSearchButton->setAutoDefault(true);
        m_signalConditionCancelButton = new QPushButton(
            QStringLiteral("取消搜索"), m_signalConditionSearchDialog);
        m_signalConditionCancelButton->setObjectName(
            QStringLiteral("signalConditionCancel"));
        m_signalConditionCancelButton->setEnabled(false);
        auto* clearResultsButton = new QPushButton(
            QStringLiteral("返回查找前"), m_signalConditionSearchDialog);
        m_signalConditionPrevButton = new QPushButton(
            QStringLiteral("上一个"), m_signalConditionSearchDialog);
        m_signalConditionPrevButton->setObjectName(
            QStringLiteral("signalConditionPrevious"));
        m_signalConditionPrevButton->setEnabled(false);
        m_signalConditionNextButton = new QPushButton(
            QStringLiteral("下一个"), m_signalConditionSearchDialog);
        m_signalConditionNextButton->setObjectName(
            QStringLiteral("signalConditionNext"));
        m_signalConditionNextButton->setEnabled(false);
        auto* closeButton = new QPushButton(
            QStringLiteral("关闭"), m_signalConditionSearchDialog);
        buttons->addWidget(m_signalConditionSearchButton);
        buttons->addWidget(m_signalConditionCancelButton);
        buttons->addWidget(clearResultsButton);
        buttons->addWidget(m_signalConditionPrevButton);
        buttons->addWidget(m_signalConditionNextButton);
        buttons->addStretch(1);
        buttons->addWidget(closeButton);
        root->addLayout(buttons);

        connect(addValueButton, &QPushButton::clicked, this, [this]() {
            addSignalConditionValueRow();
        });
        connect(m_signalConditionSearchButton, &QPushButton::clicked,
                this, &MainWindow::startSignalConditionSearch);
        connect(m_signalConditionCancelButton, &QPushButton::clicked,
                this, &MainWindow::cancelSignalConditionSearch);
        connect(m_signalConditionPrevButton, &QPushButton::clicked,
                this, [this]() { navigateTreeSearchMatch(-1); });
        connect(m_signalConditionNextButton, &QPushButton::clicked,
                this, [this]() { navigateTreeSearchMatch(1); });
        const auto startOnReturn = [this]() {
            if (!m_signalConditionSearchButton ||
                m_signalConditionSearchButton->isEnabled()) {
                startSignalConditionSearch();
            }
        };
        connect(m_signalConditionNameEdit, &QLineEdit::returnPressed,
                this, startOnReturn);
        connect(m_signalConditionChangeMinEdit, &QLineEdit::returnPressed,
                this, startOnReturn);
        connect(m_signalConditionChangeMaxEdit, &QLineEdit::returnPressed,
                this, startOnReturn);
        connect(clearResultsButton, &QPushButton::clicked, this, [this]() {
            showTreeSearchResults(QString());
            if (m_treeSearchEdit) {
                const QSignalBlocker blocker(m_treeSearchEdit);
                m_treeSearchEdit->clear();
            }
            if (m_signalConditionStatusLabel) {
                m_signalConditionStatusLabel->setText(
                    QStringLiteral("已恢复显示完整待选信号树。"));
            }
        });
        connect(closeButton, &QPushButton::clicked, this, [this]() {
            saveSignalConditionSearchSettings();
            if (m_signalConditionSearchDialog) m_signalConditionSearchDialog->hide();
        });
        connect(m_signalConditionSearchDialog, &QDialog::finished,
                this, [this](int) { saveSignalConditionSearchSettings(); });

        loadSignalConditionSearchSettings();
    }

    m_signalConditionSearchDialog->show();
    m_signalConditionSearchDialog->raise();
    m_signalConditionSearchDialog->activateWindow();
    if (m_signalConditionNameEdit) {
        m_signalConditionNameEdit->setFocus();
        m_signalConditionNameEdit->selectAll();
    }
}

void MainWindow::openSignalConditionSearchDialogForBenchmark() {
    openSignalConditionSearchDialog();
}

void MainWindow::cancelSignalConditionSearch() {
    if (m_signalConditionSearchCancel) {
        m_signalConditionSearchCancel->store(true, std::memory_order_release);
    }
    if (m_signalConditionStatusLabel) {
        m_signalConditionStatusLabel->setText(
            QStringLiteral("正在取消；当前解码批次结束后停止……"));
    }
}

void MainWindow::stopSignalConditionSearch() {
    ++m_signalConditionSearchGeneration;
    if (m_signalConditionSearchCancel) {
        m_signalConditionSearchCancel->store(true, std::memory_order_release);
    }
    if (m_signalConditionSearchThread.joinable()) {
        m_signalConditionSearchThread.join();
    }
    m_signalConditionSearchCancel.reset();
    if (m_signalConditionSearchButton) m_signalConditionSearchButton->setEnabled(true);
    if (m_signalConditionCancelButton) m_signalConditionCancelButton->setEnabled(false);
}

void MainWindow::supersedeSignalConditionSearch() {
    if (!m_signalConditionSearchCancel) return;
    ++m_signalConditionSearchGeneration;
    m_signalConditionSearchCancel->store(true, std::memory_order_release);
    if (m_signalConditionSearchButton) {
        m_signalConditionSearchButton->setEnabled(true);
    }
    if (m_signalConditionCancelButton) {
        m_signalConditionCancelButton->setEnabled(false);
    }
    if (m_signalConditionStatusLabel) {
        m_signalConditionStatusLabel->setText(
            QStringLiteral("已切换到层级查找。"));
    }
}

void MainWindow::applySignalConditionSearchResults(
        const QVector<int>& matchedNodeIds,
        qint64 examinedSignals,
        qint64 nameCandidates,
        qint64 elapsedMs,
        bool truncated,
        const QString& error) {
    if (m_signalConditionSearchThread.joinable() &&
        m_signalConditionSearchThread.get_id() != std::this_thread::get_id()) {
        m_signalConditionSearchThread.join();
    }
    m_signalConditionSearchCancel.reset();
    if (m_signalConditionSearchButton) m_signalConditionSearchButton->setEnabled(true);
    if (m_signalConditionCancelButton) m_signalConditionCancelButton->setEnabled(false);

    if (!error.isEmpty()) {
        if (m_signalConditionStatusLabel) {
            m_signalConditionStatusLabel->setText(
                QStringLiteral("查找失败：%1").arg(error));
        }
        statusBar()->showMessage(QStringLiteral("信号条件查找失败：%1").arg(error), 5000);
        return;
    }

    if (m_treeSearchActive) showTreeSearchResults(QString(), true);

    SignalTreeModel* model = m_treeModel
        ? signalTreeModelFrom(m_treeModel)
        : nullptr;
    QVector<int> uniqueMatchedNodeIds;
    if (model) {
        captureTreeSearchState();
        uniqueMatchedNodeIds.reserve(matchedNodeIds.size());
        QSet<int> seenNodeIds;
        for (int nodeId : matchedNodeIds) {
            if (!m_signalTreeModel->isValidNodeId(nodeId) ||
                seenNodeIds.contains(nodeId)) {
                continue;
            }
            seenNodeIds.insert(nodeId);
            uniqueMatchedNodeIds.push_back(nodeId);
        }
        if (m_treeSearchEdit) {
            const QSignalBlocker blocker(m_treeSearchEdit);
            m_treeSearchEdit->clear();
        }
        m_treeSearchMatchedNodeIds = uniqueMatchedNodeIds;
        m_treeSearchCurrentMatch = uniqueMatchedNodeIds.isEmpty() ? -1 : 0;
        m_treeSearchActive = true;
        m_treeSearchCropMode = m_signalConditionCropTreeCheck &&
                               m_signalConditionCropTreeCheck->isChecked();
        m_applyingTreeExpansionState = true;
        model->setSearchRoots(uniqueMatchedNodeIds, m_treeSearchCropMode);
        m_applyingTreeExpansionState = false;
        const bool canNavigate = uniqueMatchedNodeIds.size() > 1;
        if (m_treeSearchPrevButton) m_treeSearchPrevButton->setEnabled(canNavigate);
        if (m_treeSearchNextButton) m_treeSearchNextButton->setEnabled(canNavigate);
        if (m_signalConditionPrevButton) m_signalConditionPrevButton->setEnabled(canNavigate);
        if (m_signalConditionNextButton) m_signalConditionNextButton->setEnabled(canNavigate);
        applyTreeSearchExpansion();
    }

    QString summary = QStringLiteral(
        "完成：检查 %1 个信号，名字预筛通过 %2 个，匹配 %3 个，用时 %4 ms。")
        .arg(examinedSignals)
        .arg(nameCandidates)
        .arg(uniqueMatchedNodeIds.size())
        .arg(elapsedMs);
    if (truncated) {
        summary += QStringLiteral(" 已达到 5000 个结果上限，仅显示前 5000 个。");
    }
    if (m_signalConditionStatusLabel) {
        m_signalConditionStatusLabel->setText(summary);
    }
    statusBar()->showMessage(summary, 5000);
}

void MainWindow::startSignalConditionSearch() {
    if (!m_signalConditionSearchDialog || !m_signalConditionNameEdit) return;
    if (m_signalConditionSearchButton &&
        !m_signalConditionSearchButton->isEnabled()) return;

    struct SearchValueCondition {
        ParsedValueFindTarget target;
        bool requireRatio = false;
        long double minimumPercent = 0.0L;
    };

    const bool selectedSubtree =
        m_signalConditionScopeCheck && m_signalConditionScopeCheck->isChecked();
    const QString nameText = m_signalConditionNameEdit->text().trimmed();
    const bool regexMode =
        m_signalConditionRegexCheck && m_signalConditionRegexCheck->isChecked();
    const Qt::CaseSensitivity caseSensitivity =
        (m_signalConditionCaseCheck && m_signalConditionCaseCheck->isChecked())
            ? Qt::CaseSensitive
            : Qt::CaseInsensitive;

    QRegularExpression nameRegex;
    QString regexRequiredLiteral;
    bool regexRequiredLiteralIsAscii = false;
    if (!nameText.isEmpty() && regexMode) {
        QRegularExpression::PatternOptions options =
            QRegularExpression::NoPatternOption;
        if (caseSensitivity == Qt::CaseInsensitive) {
            options |= QRegularExpression::CaseInsensitiveOption;
        }
        nameRegex = QRegularExpression(nameText, options);
        if (!nameRegex.isValid()) {
            m_signalConditionStatusLabel->setText(
                QStringLiteral("名字正则无效：%1").arg(nameRegex.errorString()));
            return;
        }
        regexRequiredLiteral =
            conservativeRegexRequiredLiteral(nameRegex.pattern());
        regexRequiredLiteralIsAscii =
            isAsciiText(regexRequiredLiteral);
    }

    auto parseCountBound = [&](QLineEdit* edit, qint64& value,
                               bool& present, const QString& label) {
        const QString text = edit ? edit->text().trimmed() : QString();
        present = !text.isEmpty();
        if (!present) return true;
        bool ok = false;
        value = text.toLongLong(&ok, 10);
        if (!ok || value < 0) {
            m_signalConditionStatusLabel->setText(
                QStringLiteral("%1必须是非负整数。").arg(label));
            return false;
        }
        return true;
    };

    qint64 changeMin = 0;
    qint64 changeMax = 0;
    bool haveChangeMin = false;
    bool haveChangeMax = false;
    if (!parseCountBound(m_signalConditionChangeMinEdit, changeMin,
                         haveChangeMin, QStringLiteral("变化次数最小值")) ||
        !parseCountBound(m_signalConditionChangeMaxEdit, changeMax,
                         haveChangeMax, QStringLiteral("变化次数最大值"))) {
        return;
    }
    if (haveChangeMin && haveChangeMax && changeMin > changeMax) {
        m_signalConditionStatusLabel->setText(
            QStringLiteral("变化次数最小值不能大于最大值。"));
        return;
    }

    QVector<SearchValueCondition> valueConditions;
    valueConditions.reserve(m_signalConditionValueRows.size());
    for (int rowIndex = 0; rowIndex < m_signalConditionValueRows.size(); ++rowIndex) {
        const SignalConditionValueRow& row =
            m_signalConditionValueRows.at(rowIndex);
        const QString valueText =
            row.valueEdit ? row.valueEdit->text().trimmed() : QString();
        const QString ratioText =
            row.ratioEdit ? row.ratioEdit->text().trimmed() : QString();
        if (valueText.isEmpty() && ratioText.isEmpty()) continue;
        if (valueText.isEmpty()) {
            m_signalConditionStatusLabel->setText(
                QStringLiteral("第 %1 条值条件填写了比例但没有填写值。")
                    .arg(rowIndex + 1));
            return;
        }
        SearchValueCondition condition;
        if (!parseValueFindTargetText(valueText, condition.target)) {
            m_signalConditionStatusLabel->setText(
                QStringLiteral("第 %1 条值条件不是有效数值。")
                    .arg(rowIndex + 1));
            return;
        }
        if (!ratioText.isEmpty()) {
            bool ok = false;
            const double percent = ratioText.toDouble(&ok);
            if (!ok || !std::isfinite(percent) || percent < 0.0 || percent > 100.0) {
                m_signalConditionStatusLabel->setText(
                    QStringLiteral("第 %1 条时间占比必须是 0~100。")
                        .arg(rowIndex + 1));
                return;
            }
            condition.requireRatio = true;
            condition.minimumPercent = static_cast<long double>(percent);
        }
        valueConditions.push_back(condition);
    }

    QVector<int> selectedNodeIds;
    QVector<int> fallbackScopedSignalIndexes;
    if (selectedSubtree) {
        QModelIndexList picked;
        if (m_tree && m_tree->selectionModel()) {
            picked = m_tree->selectionModel()->selectedRows(0);
        }
        if (picked.isEmpty() && m_tree && m_tree->currentIndex().isValid()) {
            picked.push_back(m_tree->currentIndex());
        }
        QSet<int> seenNodes;
        for (const QModelIndex& index : picked) {
            const QVariant nodeValue = index.data(kTreeRoleNodeId);
            if (!nodeValue.isValid()) continue;
            const int nodeId = nodeValue.toInt();
            if (nodeId < 0 || seenNodes.contains(nodeId)) continue;
            seenNodes.insert(nodeId);
            selectedNodeIds.push_back(nodeId);
        }
        if (selectedNodeIds.isEmpty()) {
            m_signalConditionStatusLabel->setText(
                QStringLiteral("请先在待选信号树中选择一个或多个节点。"));
            return;
        }
        if (!m_wave.tree.valid) {
            const QList<int> scoped = selectedTreeSignalIndexesForViewportJump();
            fallbackScopedSignalIndexes = scoped.toVector();
        }
    }

    QVector<int> fallbackNodeIdBySignalIndex;
    if (!m_wave.tree.valid && m_signalTreeModel) {
        fallbackNodeIdBySignalIndex.resize(
            waveSignalCount(m_wave.signalList));
        for (int signalIndex = 0; signalIndex < m_wave.signalList.size();
             ++signalIndex) {
            fallbackNodeIdBySignalIndex[signalIndex] =
                m_signalTreeModel->nodeIdForSignalIndex(signalIndex);
        }
    }

    saveSignalConditionSearchSettings();
    stopSignalConditionSearch();

    const quint64 generation = ++m_signalConditionSearchGeneration;
    const quint64 waveGeneration = m_waveFileGeneration;
    std::shared_ptr<std::atomic_bool> cancel(new std::atomic_bool(false));
    m_signalConditionSearchCancel = cancel;
    if (m_signalConditionSearchButton) m_signalConditionSearchButton->setEnabled(false);
    if (m_signalConditionCancelButton) m_signalConditionCancelButton->setEnabled(true);
    if (m_signalConditionPrevButton) m_signalConditionPrevButton->setEnabled(false);
    if (m_signalConditionNextButton) m_signalConditionNextButton->setEnabled(false);
    if (m_signalConditionStatusLabel) {
        m_signalConditionStatusLabel->setText(
            QStringLiteral("正在后台预筛名字和范围……"));
    }

    const bool needStatistics =
        haveChangeMin || haveChangeMax || !valueConditions.isEmpty();
    const std::shared_ptr<WaveParser4Reader> reader = m_waveReader;
    const std::shared_ptr<std::mutex> readerMutex = m_waveReaderMutex;
    const bool canLoadOnDemand =
        m_currentWaveSupportsOnDemand && bool(reader) && bool(readerMutex);

    m_signalConditionSearchThread = std::thread(
        [this, generation, waveGeneration, cancel, selectedSubtree,
         selectedNodeIds, fallbackScopedSignalIndexes,
         fallbackNodeIdBySignalIndex, nameText, regexMode, nameRegex,
         regexRequiredLiteral, regexRequiredLiteralIsAscii, caseSensitivity,
         haveChangeMin, haveChangeMax, changeMin, changeMax,
         valueConditions, needStatistics, reader, readerMutex,
         canLoadOnDemand]() mutable {
        QElapsedTimer timer;
        timer.start();
        const WaveFile& wave = m_wave;
        const qint64 waveStart = wave.meta.start;
        const qint64 waveEnd = wave.meta.end;
        const qint64 totalDuration = qMax<qint64>(0, waveEnd - waveStart);
        constexpr int kResultLimit = 5000;
        // Amortize footer/index lookup and file-handle setup while staying far
        // below the 20M exact-sample safety budget for ordinary traces.
        constexpr int kDecodeBatchSize = 1024;

        QVector<int> matchedNodeIds;
        matchedNodeIds.reserve(1024);
        struct SignalSearchCandidate {
            int signalIndex = -1;
            int nodeId = -1;
        };
        QVector<SignalSearchCandidate> decodeBatch;
        decodeBatch.reserve(kDecodeBatchSize);
        qint64 examinedSignals = 0;
        qint64 nameCandidates = 0;
        bool truncated = false;
        QString error;

        auto nodeIdForSignal = [&](int signalIndex) {
            if (wave.tree.valid &&
                signalIndex >= 0 &&
                signalIndex < wave.tree.signalIndexToNodeId.size()) {
                return wave.tree.signalIndexToNodeId.at(signalIndex);
            }
            return signalIndex >= 0 &&
                   signalIndex < fallbackNodeIdBySignalIndex.size()
                ? fallbackNodeIdBySignalIndex.at(signalIndex)
                : -1;
        };

        auto nameMatches = [&](int signalIndex,
                               const QString* segmentOverride,
                               const QString* fullPathOverride) {
            if (nameText.isEmpty()) return true;
            const QString segment = segmentOverride
                ? *segmentOverride
                : waveSignalSegmentName(wave, signalIndex);
            if (regexMode) {
                if (regexRequiredLiteralMayOccur(
                        segment, regexRequiredLiteral,
                        regexRequiredLiteralIsAscii, caseSensitivity) &&
                    nameRegex.match(segment).hasMatch()) {
                    return true;
                }
                const QString fullPath = fullPathOverride
                    ? *fullPathOverride
                    : waveSignalFullPath(wave, signalIndex);
                return regexRequiredLiteralMayOccur(
                           fullPath, regexRequiredLiteral,
                           regexRequiredLiteralIsAscii, caseSensitivity) &&
                       nameRegex.match(fullPath).hasMatch();
            }
            if (segment.contains(nameText, caseSensitivity)) return true;
            const QString fullPath = fullPathOverride
                ? *fullPathOverride
                : waveSignalFullPath(wave, signalIndex);
            return fullPath.contains(nameText, caseSensitivity);
        };

        auto proceduralClockChangeCount = [&](const WaveSignal& signal) -> qint64 {
            if (!signal.proceduralClock || signal.clockTogglePeriodTicks == 0 ||
                waveEnd <= waveStart || waveEnd <= 0) {
                return 0;
            }
            const quint64 period = signal.clockTogglePeriodTicks;
            const quint64 firstTime = quint64(qMax<qint64>(1, waveStart));
            quint64 firstMultiple = firstTime / period;
            if ((firstTime % period) != 0) ++firstMultiple;
            if (firstMultiple == 0) firstMultiple = 1;
            const quint64 lastTime = quint64(waveEnd - 1);
            const quint64 lastMultiple = lastTime / period;
            if (lastMultiple < firstMultiple) return 0;
            const quint64 count = lastMultiple - firstMultiple + 1u;
            return count > quint64((std::numeric_limits<qint64>::max)())
                ? (std::numeric_limits<qint64>::max)()
                : qint64(count);
        };

        auto clockTrueDurationFromZero = [](const WaveSignal& signal,
                                            qint64 endTime) -> qint64 {
            if (endTime <= 0 || signal.clockTogglePeriodTicks == 0) return 0;
            const quint64 period = signal.clockTogglePeriodTicks;
            const quint64 end = quint64(endTime);
            const quint64 fullIntervals = end / period;
            const quint64 remainder = end % period;
            const quint64 trueFullIntervals = signal.clockInitialValue
                ? (fullIntervals + 1u) / 2u
                : fullIntervals / 2u;
            quint64 duration = trueFullIntervals * period;
            const bool remainderIsTrue =
                ((fullIntervals & 1u) == 0u)
                    ? signal.clockInitialValue
                    : !signal.clockInitialValue;
            if (remainderIsTrue) duration += remainder;
            return duration > quint64((std::numeric_limits<qint64>::max)())
                ? (std::numeric_limits<qint64>::max)()
                : qint64(duration);
        };

        auto signalPassesStatistics = [&](const WaveSignal& signal) {
            qint64 changeCount = 0;
            if (signal.proceduralClock) {
                changeCount = proceduralClockChangeCount(signal);
            } else {
                for (int i = 1; i < signal.samples.size(); ++i) {
                    const WaveSample& current = signal.samples.at(i);
                    if (current.time < waveStart) continue;
                    if (current.time >= waveEnd) break;
                    if (!waveSamplesEquivalent(current, signal.samples.at(i - 1))) {
                        if (changeCount < (std::numeric_limits<qint64>::max)()) {
                            ++changeCount;
                        }
                    }
                }
            }
            if (haveChangeMin && changeCount < changeMin) return false;
            if (haveChangeMax && changeCount > changeMax) return false;
            if (valueConditions.isEmpty()) return true;
            if (totalDuration <= 0) return false;

            QVector<quint64> targetBits;
            targetBits.reserve(valueConditions.size());
            for (const SearchValueCondition& condition : valueConditions) {
                quint64 bits = 0;
                if (!valueFindTargetForSignal(condition.target,
                                              signal.width, bits)) {
                    return false;
                }
                targetBits.push_back(bits);
            }

            QVector<qint64> matchedDurations(valueConditions.size(), 0);
            QVector<uchar> exists(valueConditions.size(), uchar(0));
            if (signal.proceduralClock) {
                const qint64 trueDuration =
                    clockTrueDurationFromZero(signal, waveEnd) -
                    clockTrueDurationFromZero(signal, qMax<qint64>(0, waveStart));
                const qint64 falseDuration = totalDuration - trueDuration;
                for (int conditionIndex = 0;
                     conditionIndex < valueConditions.size(); ++conditionIndex) {
                    const qint64 duration =
                        (targetBits.at(conditionIndex) & 1u)
                            ? trueDuration : falseDuration;
                    matchedDurations[conditionIndex] = qMax<qint64>(0, duration);
                    exists[conditionIndex] = duration > 0 ? 1 : 0;
                }
            } else {
                const quint64 mask = waveBitMaskForWidth(signal.width);
                for (int sampleIndex = 0;
                     sampleIndex < signal.samples.size(); ++sampleIndex) {
                    WaveSample sample = signal.samples.at(sampleIndex);
                    hydrateWaveSampleRawFields(signal.kind, signal.width, sample);
                    if (sample.isAbsent || sample.isZ) continue;
                    const qint64 segmentStart =
                        qMax(waveStart, sample.time);
                    const qint64 nextTime =
                        sampleIndex + 1 < signal.samples.size()
                            ? signal.samples.at(sampleIndex + 1).time
                            : waveEnd;
                    const qint64 segmentEnd = qMin(waveEnd, nextTime);
                    if (segmentEnd <= segmentStart) continue;
                    const qint64 duration = segmentEnd - segmentStart;
                    const quint64 bits = sample.rawBits & mask;
                    for (int conditionIndex = 0;
                         conditionIndex < valueConditions.size();
                         ++conditionIndex) {
                        if (bits != targetBits.at(conditionIndex)) continue;
                        exists[conditionIndex] = 1;
                        const qint64 old = matchedDurations.at(conditionIndex);
                        matchedDurations[conditionIndex] =
                            duration > (std::numeric_limits<qint64>::max)() - old
                                ? (std::numeric_limits<qint64>::max)()
                                : old + duration;
                    }
                }
            }

            for (int conditionIndex = 0;
                 conditionIndex < valueConditions.size(); ++conditionIndex) {
                const SearchValueCondition& condition =
                    valueConditions.at(conditionIndex);
                if (!exists.at(conditionIndex)) return false;
                if (condition.requireRatio) {
                    const long double lhs =
                        static_cast<long double>(
                            matchedDurations.at(conditionIndex)) * 100.0L;
                    const long double rhs =
                        condition.minimumPercent *
                        static_cast<long double>(totalDuration);
                    if (!(lhs > rhs)) return false;
                }
            }
            return true;
        };

        auto appendMatchedSignal = [&](const SignalSearchCandidate& candidate) {
            const int nodeId = candidate.nodeId >= 0
                ? candidate.nodeId
                : nodeIdForSignal(candidate.signalIndex);
            if (nodeId < 0) return;
            matchedNodeIds.push_back(nodeId);
            if (matchedNodeIds.size() >= kResultLimit) {
                truncated = true;
            }
        };

        auto processDecodeBatch = [&]() -> bool {
            if (decodeBatch.isEmpty()) return true;
            if (cancel->load(std::memory_order_acquire)) return false;

            if (!needStatistics) {
                for (const SignalSearchCandidate& candidate : decodeBatch) {
                    appendMatchedSignal(candidate);
                    if (truncated) break;
                }
                decodeBatch.clear();
                return !truncated;
            }

            if (canLoadOnDemand) {
                std::function<bool(int, int)> processOnDemandRange;
                processOnDemandRange = [&](int begin, int end) {
                    if (begin >= end) return true;
                    if (cancel->load(std::memory_order_acquire) ||
                        truncated) {
                        return false;
                    }

                    QVector<int> signalIds;
                    signalIds.reserve(end - begin);
                    for (int i = begin; i < end; ++i) {
                        const int signalIndex = decodeBatch.at(i).signalIndex;
                        if (signalIndex < 0 ||
                            signalIndex >= wave.signalList.size()) {
                            continue;
                        }
                        const int signalId =
                            wave.signalList.at(signalIndex).signalId;
                        if (signalId > 0) signalIds.push_back(signalId);
                    }

                    WaveFile loadedWave;
                    QString loadError;
                    bool loadedOk = false;
                    {
                        std::lock_guard<std::mutex> lock(*readerMutex);
                        loadedOk = reader->loadSignals(
                            signalIds, loadedWave, loadError,
                            kViewerOnDemandSampleBudget,
                            waveStart, waveEnd);
                    }
                    if (!loadedOk) {
                        const bool budgetExceeded =
                            loadError.startsWith(QStringLiteral(
                                "WVZ4 decoded sample limit exceeded"));
                        if (budgetExceeded && end - begin > 1) {
                            const int middle =
                                begin + (end - begin) / 2;
                            return processOnDemandRange(begin, middle) &&
                                   processOnDemandRange(middle, end);
                        }
                        error = loadError;
                        return false;
                    }

                    QHash<int, const WaveSignal*> loadedBySignalId;
                    loadedBySignalId.reserve(
                        waveSignalCount(loadedWave.signalList) * 2 + 1);
                    for (const WaveSignal& signal : loadedWave.signalList) {
                        loadedBySignalId.insert(signal.signalId, &signal);
                    }
                    for (int i = begin; i < end; ++i) {
                        if (cancel->load(std::memory_order_acquire)) {
                            return false;
                        }
                        const SignalSearchCandidate& candidate = decodeBatch.at(i);
                        const int signalIndex = candidate.signalIndex;
                        if (signalIndex < 0 ||
                            signalIndex >= wave.signalList.size()) {
                            continue;
                        }
                        const WaveSignal& directorySignal =
                            wave.signalList.at(signalIndex);
                        const WaveSignal* signal =
                            loadedBySignalId.value(
                                directorySignal.signalId, nullptr);
                        if (!signal) continue;
                        if (signalPassesStatistics(*signal)) {
                            appendMatchedSignal(candidate);
                            if (truncated) return false;
                        }
                    }
                    return true;
                };

                const bool ok =
                    processOnDemandRange(0, decodeBatch.size());
                decodeBatch.clear();
                return ok && !truncated;
            }

            for (const SignalSearchCandidate& candidate : decodeBatch) {
                if (cancel->load(std::memory_order_acquire)) return false;
                const int signalIndex = candidate.signalIndex;
                if (signalIndex < 0 ||
                    signalIndex >= wave.signalList.size()) {
                    continue;
                }
                const WaveSignal& signal = wave.signalList.at(signalIndex);
                if (signalPassesStatistics(signal)) {
                    appendMatchedSignal(candidate);
                    if (truncated) break;
                }
            }
            decodeBatch.clear();
            return !truncated;
        };

        auto considerSignal = [&](int signalIndex, int nodeId,
                                  const QString* segmentOverride = nullptr,
                                  const QString* fullPathOverride = nullptr) -> bool {
            if (cancel->load(std::memory_order_acquire) || truncated) return false;
            if (signalIndex < 0 || signalIndex >= wave.signalList.size()) return true;
            ++examinedSignals;
            if (!nameMatches(signalIndex, segmentOverride, fullPathOverride)) return true;
            ++nameCandidates;
            decodeBatch.push_back(SignalSearchCandidate{signalIndex, nodeId});
            if (decodeBatch.size() >= kDecodeBatchSize) {
                if (!processDecodeBatch()) return false;
            }
            if ((examinedSignals & 0x3ffff) == 0) {
                const qint64 examinedSnapshot = examinedSignals;
                const qint64 candidateSnapshot = nameCandidates;
                QMetaObject::invokeMethod(
                    this,
                    [this, generation, waveGeneration,
                     examinedSnapshot, candidateSnapshot]() {
                        if (generation != m_signalConditionSearchGeneration ||
                            waveGeneration != m_waveFileGeneration ||
                            !m_signalConditionStatusLabel) {
                            return;
                        }
                        m_signalConditionStatusLabel->setText(
                            QStringLiteral(
                                "后台查找中：已检查 %1 个信号，名字预筛通过 %2 个……")
                                .arg(examinedSnapshot)
                                .arg(candidateSnapshot));
                    },
                    Qt::QueuedConnection);
            }
            return true;
        };

        auto traverseTreeSubtrees = [&](const QVector<int>& rootNodeIds,
                                        bool includeAncestorPath) {
            QBitArray visitedNodes(wave.tree.nodesById.size(), false);
            struct TreeWalkFrame {
                int nodeId = 0;
                int restorePathLength = 0;
                bool exit = false;
            };
            QVector<TreeWalkFrame> pending;
            pending.reserve(1024);
            QVector<int> children;
            QVector<QString> ancestorSegments;
            QString path;
            path.reserve(256);
            const bool buildPaths = !nameText.isEmpty();

            for (int rootNodeId : rootNodeIds) {
                if (truncated || cancel->load(std::memory_order_acquire)) break;
                if (rootNodeId <= 0 ||
                    rootNodeId >= wave.tree.nodesById.size() ||
                    visitedNodes.testBit(rootNodeId)) {
                    continue;
                }

                path.clear();
                if (buildPaths && includeAncestorPath) {
                    ancestorSegments.clear();
                    int parentId =
                        wave.tree.nodesById.at(rootNodeId).parentId;
                    int guard = 0;
                    while (parentId > 0 &&
                           parentId < wave.tree.nodesById.size() &&
                           guard++ < wave.tree.nodesById.size()) {
                        const WaveTreeNode& parent =
                            wave.tree.nodesById.at(parentId);
                        if (!parent.valid) break;
                        const QString segment =
                            waveTreeNameTokenText(wave.tree, parent.nameToken);
                        if (!segment.isEmpty()) {
                            ancestorSegments.push_back(segment);
                        }
                        parentId = parent.parentId;
                    }
                    for (int i = ancestorSegments.size() - 1; i >= 0; --i) {
                        if (!path.isEmpty()) path += QLatin1Char('.');
                        path += ancestorSegments.at(i);
                    }
                }

                pending.clear();
                pending.push_back(TreeWalkFrame{rootNodeId, path.size(), false});
                while (!pending.isEmpty() && !truncated &&
                       !cancel->load(std::memory_order_acquire)) {
                    const TreeWalkFrame frame = pending.takeLast();
                    if (frame.exit) {
                        path.resize(frame.restorePathLength);
                        continue;
                    }

                    const int nodeId = frame.nodeId;
                    if (nodeId <= 0 ||
                        nodeId >= wave.tree.nodesById.size() ||
                        visitedNodes.testBit(nodeId)) {
                        continue;
                    }
                    visitedNodes.setBit(nodeId);
                    const WaveTreeNode& node = wave.tree.nodesById.at(nodeId);
                    if (!node.valid) continue;

                    const int restorePathLength = path.size();
                    QString segment;
                    if (buildPaths) {
                        segment =
                            waveTreeNameTokenText(wave.tree, node.nameToken);
                        if (!segment.isEmpty()) {
                            if (!path.isEmpty()) path += QLatin1Char('.');
                            path += segment;
                        }
                    }
                    const int effectiveSignalIndex =
                        waveTreeEffectiveSignalIndex(wave.tree, nodeId);
                    if (effectiveSignalIndex >= 0) {
                        if (!considerSignal(
                                effectiveSignalIndex, nodeId,
                                buildPaths ? &segment : nullptr,
                                buildPaths ? &path : nullptr)) {
                            break;
                        }
                        path.resize(restorePathLength);
                        continue;
                    }

                    pending.push_back(
                        TreeWalkFrame{0, restorePathLength, true});
                    children.clear();
                    int childId = node.firstChild;
                    int guard = 0;
                    while (childId != 0 &&
                           guard++ < wave.tree.nodesById.size()) {
                        if (childId <= 0 ||
                            childId >= wave.tree.nodesById.size()) {
                            break;
                        }
                        children.push_back(childId);
                        childId =
                            wave.tree.nodesById.at(childId).nextSibling;
                    }
                    for (int i = children.size() - 1; i >= 0; --i) {
                        pending.push_back(
                            TreeWalkFrame{children.at(i), path.size(), false});
                    }
                }
            }
        };

        bool nameDictionaryRejectedAll = false;
        if (!selectedSubtree && regexMode &&
            !regexRequiredLiteral.isEmpty() &&
            !regexRequiredLiteral.contains(QLatin1Char('.')) &&
            wave.tree.valid) {
            bool mayOccurInNamedSegment = false;
            for (int nameId = 1;
                 nameId < wave.tree.namesById.size(); ++nameId) {
                const QString segment =
                    QString::fromUtf8(wave.tree.namesById.at(nameId));
                if (regexRequiredLiteralMayOccur(
                        segment, regexRequiredLiteral,
                        regexRequiredLiteralIsAscii, caseSensitivity)) {
                    mayOccurInNamedSegment = true;
                    break;
                }
            }

            // Array-index tokens render as "[digits]". Be conservative for
            // literals made only of characters that such a token can contain.
            bool mayOccurInArrayIndex = true;
            for (const QChar ch : regexRequiredLiteral) {
                if (!ch.isDigit() && ch != QLatin1Char('[') &&
                    ch != QLatin1Char(']')) {
                    mayOccurInArrayIndex = false;
                    break;
                }
            }
            if (!mayOccurInNamedSegment && !mayOccurInArrayIndex) {
                nameDictionaryRejectedAll = true;
                examinedSignals = wave.signalList.size();
            }
        }

        bool usedParallelTreeNameSearch = false;
        if (!nameDictionaryRejectedAll &&
            !needStatistics && !selectedSubtree && wave.tree.valid &&
            !wave.tree.rootNodeIds.isEmpty() &&
            wave.signalList.size() >= 250000) {
            struct ParallelTreeTask {
                QVector<int> roots;
            };
            QVector<ParallelTreeTask> tasks;
            tasks.push_back(ParallelTreeTask{wave.tree.rootNodeIds});

            const int hardwareThreads =
                int(std::thread::hardware_concurrency());
            const int workerTarget = qBound(2, hardwareThreads, 4);
            const int taskTarget = workerTarget * 4;
            int splitGuard = 0;
            while (tasks.size() < taskTarget &&
                   splitGuard++ < wave.tree.nodesById.size()) {
                bool split = false;
                for (int taskIndex = 0; taskIndex < tasks.size(); ++taskIndex) {
                    QVector<int> roots = tasks.at(taskIndex).roots;
                    if (roots.size() > 1) {
                        const int middle = roots.size() / 2;
                        ParallelTreeTask left;
                        ParallelTreeTask right;
                        left.roots = roots.mid(0, middle);
                        right.roots = roots.mid(middle);
                        tasks[taskIndex] = std::move(left);
                        tasks.insert(taskIndex + 1, std::move(right));
                        split = true;
                        break;
                    }
                    if (roots.isEmpty()) continue;

                    const int rootNodeId = roots.first();
                    if (rootNodeId <= 0 ||
                        rootNodeId >= wave.tree.nodesById.size()) {
                        continue;
                    }
                    const WaveTreeNode& rootNode =
                        wave.tree.nodesById.at(rootNodeId);
                    if (!rootNode.valid ||
                        waveTreeEffectiveSignalIndex(wave.tree, rootNodeId) >= 0 ||
                        rootNode.firstChild == 0) {
                        continue;
                    }

                    QVector<int> children;
                    int childId = rootNode.firstChild;
                    int guard = 0;
                    while (childId != 0 &&
                           guard++ < wave.tree.nodesById.size()) {
                        if (childId <= 0 ||
                            childId >= wave.tree.nodesById.size()) {
                            break;
                        }
                        children.push_back(childId);
                        childId =
                            wave.tree.nodesById.at(childId).nextSibling;
                    }
                    if (children.isEmpty()) continue;
                    if (children.size() == 1) {
                        // Descend through a single-child module without making
                        // another task. The worker reconstructs the omitted
                        // ancestor prefix before matching the full path.
                        tasks[taskIndex].roots = children;
                        split = true;
                        break;
                    }

                    const int middle = children.size() / 2;
                    ParallelTreeTask left;
                    ParallelTreeTask right;
                    left.roots = children.mid(0, middle);
                    right.roots = children.mid(middle);
                    tasks[taskIndex] = std::move(left);
                    tasks.insert(taskIndex + 1, std::move(right));
                    split = true;
                    break;
                }
                if (!split) break;
            }

            if (tasks.size() >= 2) {
                usedParallelTreeNameSearch = true;
                const int workerCount = qMin(workerTarget, tasks.size());
                // Small/medium NAME tables often contain tokens reused by
                // millions of NODE entries. Decode each UTF-8 token once and
                // share the immutable QStrings across search workers.
                constexpr int kPredecodeNameLimit = 128 * 1024;
                QVector<QString> decodedTreeNames;
                if (wave.tree.namesById.size() <= kPredecodeNameLimit) {
                    decodedTreeNames.resize(wave.tree.namesById.size());
                    for (int nameId = 1;
                         nameId < wave.tree.namesById.size(); ++nameId) {
                        decodedTreeNames[nameId] =
                            QString::fromUtf8(wave.tree.namesById.at(nameId));
                    }
                }
                std::atomic_int nextTask(0);
                std::atomic<qint64> parallelExamined(0);
                std::atomic<qint64> parallelCandidates(0);
                std::atomic_bool cutoffReached(false);
                std::mutex resultMutex;
                QVector<QVector<int>> taskMatches(tasks.size());
                QVector<uchar> taskComplete(tasks.size(), uchar(0));

                auto updateCutoffLocked = [&]() {
                    int accumulated = 0;
                    for (int i = 0; i < taskMatches.size(); ++i) {
                        accumulated += taskMatches.at(i).size();
                        if (accumulated >= kResultLimit) {
                            cutoffReached.store(true, std::memory_order_release);
                            return;
                        }
                        if (!taskComplete.at(i)) return;
                    }
                };

                auto worker = [&]() {
                    const QRegularExpression workerRegex(
                        nameRegex.pattern(), nameRegex.patternOptions());
                    struct LocalNameEntry {
                        quint32 token = 0;
                        QString text;
                    };
                    constexpr int kLocalNameCacheSize = 2048;
                    QVector<LocalNameEntry> localNameCache(
                        kLocalNameCacheSize);
                    auto resolveTreeName =
                        [&](quint32 token, QString& temporary)
                            -> const QString& {
                        if (waveNameTokenIsArrayIndex(token)) {
                            temporary = QStringLiteral("[%1]").arg(
                                waveNameTokenValue(token));
                            return temporary;
                        }
                        const quint32 nameId = waveNameTokenValue(token);
                        if (nameId > 0 &&
                            nameId < quint32(decodedTreeNames.size())) {
                            return decodedTreeNames.at(int(nameId));
                        }
                        if (nameId > 0 &&
                            nameId < quint32(wave.tree.namesById.size())) {
                            LocalNameEntry& entry =
                                localNameCache[int(nameId) &
                                                   (kLocalNameCacheSize - 1)];
                            if (entry.token != token) {
                                entry.token = token;
                                entry.text = QString::fromUtf8(
                                    wave.tree.namesById.at(int(nameId)));
                            }
                            return entry.text;
                        }
                        temporary.clear();
                        return temporary;
                    };
                    struct TreeWalkFrame {
                        int nodeId = 0;
                        int restorePathLength = 0;
                        bool exit = false;
                    };
                    std::vector<TreeWalkFrame> pending;
                    pending.reserve(1024);
                    QVector<int> children;
                    QVector<QString> ancestorSegments;
                    QString path;
                    path.reserve(256);
                    qint64 localExamined = 0;
                    qint64 localCandidates = 0;

                    while (!cutoffReached.load(std::memory_order_acquire) &&
                           !cancel->load(std::memory_order_acquire)) {
                        const int taskIndex =
                            nextTask.fetch_add(1, std::memory_order_relaxed);
                        if (taskIndex >= tasks.size()) break;
                        bool enoughMatchesInTask = false;

                        for (int rootNodeId : tasks.at(taskIndex).roots) {
                            if (cutoffReached.load(std::memory_order_acquire) ||
                                cancel->load(std::memory_order_acquire) ||
                                enoughMatchesInTask) {
                                break;
                            }
                            if (rootNodeId <= 0 ||
                                rootNodeId >= wave.tree.nodesById.size()) {
                                continue;
                            }

                            path.clear();
                            ancestorSegments.clear();
                            int parentId =
                                wave.tree.nodesById.at(rootNodeId).parentId;
                            int guard = 0;
                            while (parentId > 0 &&
                                   parentId < wave.tree.nodesById.size() &&
                                   guard++ < wave.tree.nodesById.size()) {
                                const WaveTreeNode& parent =
                                    wave.tree.nodesById.at(parentId);
                                if (!parent.valid) break;
                                QString parentTemporary;
                                const QString& parentSegment =
                                    resolveTreeName(
                                        parent.nameToken, parentTemporary);
                                if (!parentSegment.isEmpty()) {
                                    ancestorSegments.push_back(parentSegment);
                                }
                                parentId = parent.parentId;
                            }
                            for (int i = ancestorSegments.size() - 1;
                                 i >= 0; --i) {
                                if (!path.isEmpty()) path += QLatin1Char('.');
                                path += ancestorSegments.at(i);
                            }

                            pending.clear();
                            pending.push_back(
                                TreeWalkFrame{rootNodeId, path.size(), false});
                            qint64 walkGuard = 0;
                            while (!pending.empty() &&
                                   !enoughMatchesInTask &&
                                   walkGuard < wave.tree.nodesById.size()) {
                                if ((walkGuard & 0xff) == 0 &&
                                    (cutoffReached.load(
                                         std::memory_order_acquire) ||
                                     cancel->load(
                                         std::memory_order_acquire))) {
                                    break;
                                }
                                ++walkGuard;
                                const TreeWalkFrame frame = pending.back();
                                pending.pop_back();
                                if (frame.exit) {
                                    path.resize(frame.restorePathLength);
                                    continue;
                                }
                                const int nodeId = frame.nodeId;
                                if (nodeId <= 0 ||
                                    nodeId >= wave.tree.nodesById.size()) {
                                    continue;
                                }
                                const WaveTreeNode& node =
                                    wave.tree.nodesById.at(nodeId);
                                if (!node.valid) continue;

                                const int restorePathLength = path.size();
                                QString segmentTemporary;
                                const QString& segment =
                                    resolveTreeName(
                                        node.nameToken, segmentTemporary);
                                if (!segment.isEmpty()) {
                                    if (!path.isEmpty()) {
                                        path += QLatin1Char('.');
                                    }
                                    path += segment;
                                }

                                if (waveTreeEffectiveSignalIndex(
                                        wave.tree, nodeId) >= 0) {
                                    ++localExamined;
                                    bool matches = false;
                                    if (regexMode) {
                                        matches =
                                            (regexRequiredLiteralMayOccur(
                                                 segment,
                                                 regexRequiredLiteral,
                                                 regexRequiredLiteralIsAscii,
                                                 caseSensitivity) &&
                                             workerRegex.match(segment)
                                                 .hasMatch()) ||
                                            (regexRequiredLiteralMayOccur(
                                                 path,
                                                 regexRequiredLiteral,
                                                 regexRequiredLiteralIsAscii,
                                                 caseSensitivity) &&
                                             workerRegex.match(path)
                                                 .hasMatch());
                                    } else {
                                        matches =
                                            segment.contains(
                                                nameText, caseSensitivity) ||
                                            path.contains(
                                                nameText, caseSensitivity);
                                    }
                                    if (matches) {
                                        ++localCandidates;
                                        std::lock_guard<std::mutex> lock(
                                            resultMutex);
                                        QVector<int>& matchesForTask =
                                            taskMatches[taskIndex];
                                        if (matchesForTask.size() <
                                            kResultLimit) {
                                            matchesForTask.push_back(nodeId);
                                        }
                                        if (matchesForTask.size() >=
                                            kResultLimit) {
                                            // The first 5000 matches within
                                            // this ordered tree partition are
                                            // sufficient regardless of what
                                            // earlier partitions contribute.
                                            enoughMatchesInTask = true;
                                            taskComplete[taskIndex] = 1;
                                        }
                                        updateCutoffLocked();
                                    }
                                    path.resize(restorePathLength);
                                    continue;
                                }

                                pending.push_back(
                                    TreeWalkFrame{
                                        0, restorePathLength, true});
                                children.clear();
                                int childId = node.firstChild;
                                int childGuard = 0;
                                while (childId != 0 &&
                                       childGuard++ <
                                           wave.tree.nodesById.size()) {
                                    if (childId <= 0 ||
                                        childId >=
                                            wave.tree.nodesById.size()) {
                                        break;
                                    }
                                    children.push_back(childId);
                                    childId = wave.tree.nodesById
                                                  .at(childId)
                                                  .nextSibling;
                                }
                                for (int i = children.size() - 1;
                                     i >= 0; --i) {
                                    pending.push_back(
                                        TreeWalkFrame{
                                            children.at(i),
                                            path.size(),
                                            false});
                                }
                            }
                        }

                        {
                            std::lock_guard<std::mutex> lock(resultMutex);
                            taskComplete[taskIndex] = 1;
                            updateCutoffLocked();
                        }
                    }
                    parallelExamined.fetch_add(
                        localExamined, std::memory_order_relaxed);
                    parallelCandidates.fetch_add(
                        localCandidates, std::memory_order_relaxed);
                };

                std::vector<std::thread> workers;
                workers.reserve(size_t(workerCount));
                for (int i = 0; i < workerCount; ++i) {
                    workers.emplace_back(worker);
                }
                for (std::thread& thread : workers) thread.join();

                examinedSignals =
                    parallelExamined.load(std::memory_order_relaxed);
                nameCandidates =
                    parallelCandidates.load(std::memory_order_relaxed);
                for (int taskIndex = 0;
                     taskIndex < taskMatches.size() &&
                     matchedNodeIds.size() < kResultLimit;
                     ++taskIndex) {
                    const QVector<int>& matches = taskMatches.at(taskIndex);
                    const int remaining =
                        kResultLimit - matchedNodeIds.size();
                    matchedNodeIds += matches.mid(0, remaining);
                }
                truncated = matchedNodeIds.size() >= kResultLimit;
            }
        }

        if (nameDictionaryRejectedAll) {
            // The deduplicated NAME table proved the required literal absent.
        } else if (usedParallelTreeNameSearch) {
            // Results were gathered above in tree partition order.
        } else if (selectedSubtree && wave.tree.valid) {
            traverseTreeSubtrees(selectedNodeIds, true);
        } else if (selectedSubtree) {
            for (int signalIndex : fallbackScopedSignalIndexes) {
                if (!considerSignal(signalIndex, -1)) break;
            }
        } else if (wave.tree.valid && !wave.tree.rootNodeIds.isEmpty()) {
            traverseTreeSubtrees(wave.tree.rootNodeIds, false);
        } else {
            for (int signalIndex = 0;
                 signalIndex < wave.signalList.size(); ++signalIndex) {
                if (!considerSignal(signalIndex, -1)) break;
            }
        }

        if (!truncated && error.isEmpty() &&
            !cancel->load(std::memory_order_acquire)) {
            processDecodeBatch();
        }
        if (cancel->load(std::memory_order_acquire) && error.isEmpty()) {
            error = QStringLiteral("搜索已取消。");
        }

        const qint64 elapsedMs = timer.elapsed();
        QMetaObject::invokeMethod(
            this,
            [this, generation, waveGeneration, matchedNodeIds,
             examinedSignals, nameCandidates, elapsedMs,
             truncated, error]() {
                if (generation != m_signalConditionSearchGeneration ||
                    waveGeneration != m_waveFileGeneration) {
                    return;
                }
                applySignalConditionSearchResults(
                    matchedNodeIds, examinedSignals, nameCandidates,
                    elapsedMs, truncated, error);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::openValueFindDialog() {
    if (!m_valueFindDialog) {
        m_valueFindDialog = new QDialog(this);
        m_valueFindDialog->setWindowTitle(QStringLiteral("Find value"));
        m_valueFindDialog->resize(680, 430);

        auto* root = new QVBoxLayout(m_valueFindDialog);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* inputRow = new QHBoxLayout();
        auto* inputLabel = new QLabel(QStringLiteral("Target"), m_valueFindDialog);
        m_valueFindEdit = new QLineEdit(m_valueFindDialog);
        m_valueFindEdit->setPlaceholderText(QStringLiteral("10, 0xA, 0b1010, -1"));
        auto* findButton = new QPushButton(QStringLiteral("Find"), m_valueFindDialog);
        inputRow->addWidget(inputLabel);
        inputRow->addWidget(m_valueFindEdit, 1);
        inputRow->addWidget(findButton);
        root->addLayout(inputRow);

        auto* rangeRow = new QHBoxLayout();
        auto* rangeLabel = new QLabel(QStringLiteral("Time range"), m_valueFindDialog);
        m_valueFindRangeCombo = new QComboBox(m_valueFindDialog);
        m_valueFindRangeCombo->addItem(QStringLiteral("Global"));
        m_valueFindRangeCombo->addItem(QStringLiteral("Selected time range (current view)"));
        m_valueFindRangeCombo->setCurrentIndex(0);
        m_valueFindRangeCombo->setToolTip(
            QStringLiteral("Selected time range uses the current waveform viewport when Find is pressed."));
        rangeRow->addWidget(rangeLabel);
        rangeRow->addWidget(m_valueFindRangeCombo, 1);
        root->addLayout(rangeRow);

        m_valueFindSummaryLabel = new QLabel(QStringLiteral("Select highlighted active signals and enter a value."), m_valueFindDialog);
        m_valueFindSummaryLabel->setWordWrap(true);
        root->addWidget(m_valueFindSummaryLabel);

        m_valueFindResults = new QTreeWidget(m_valueFindDialog);
        m_valueFindResults->setColumnCount(4);
        m_valueFindResults->setHeaderLabels(QStringList() << QStringLiteral("Signal")
                                                          << QStringLiteral("Count")
                                                          << QStringLiteral("First time")
                                                          << QStringLiteral("Time %"));
        m_valueFindResults->setRootIsDecorated(false);
        m_valueFindResults->setSelectionMode(QAbstractItemView::SingleSelection);
        m_valueFindResults->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_valueFindResults->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_valueFindResults->header()->setStretchLastSection(false);
        m_valueFindResults->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_valueFindResults->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_valueFindResults->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_valueFindResults->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_valueFindResults->headerItem()->setTextAlignment(
            0, int(Qt::AlignRight | Qt::AlignVCenter));
        root->addWidget(m_valueFindResults, 1);

        auto* buttonRow = new QHBoxLayout();
        m_valueFindPrevButton = new QPushButton(QStringLiteral("Previous"), m_valueFindDialog);
        m_valueFindNextButton = new QPushButton(QStringLiteral("Next"), m_valueFindDialog);
        auto* closeButton = new QPushButton(QStringLiteral("Close"), m_valueFindDialog);
        buttonRow->addWidget(m_valueFindPrevButton);
        buttonRow->addWidget(m_valueFindNextButton);
        buttonRow->addStretch(1);
        buttonRow->addWidget(closeButton);
        root->addLayout(buttonRow);

        connect(findButton, &QPushButton::clicked, this, &MainWindow::runValueFind);
        connect(m_valueFindEdit, &QLineEdit::returnPressed, this, &MainWindow::runValueFind);
        connect(m_valueFindPrevButton, &QPushButton::clicked, this, &MainWindow::jumpToPreviousValueFindHit);
        connect(m_valueFindNextButton, &QPushButton::clicked, this, &MainWindow::jumpToNextValueFindHit);
        connect(closeButton, &QPushButton::clicked, m_valueFindDialog, &QDialog::hide);
        connect(m_valueFindResults, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
            if (!item) return;
            const int hitIndex = item->data(0, kValueFindRoleFirstHit).toInt();
            if (hitIndex >= 0) jumpToValueFindHit(hitIndex);
        });
        connect(m_valueFindResults, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
            if (!current) return;
            const int hitIndex = current->data(0, kValueFindRoleFirstHit).toInt();
            if (hitIndex >= 0) {
                m_valueFindCurrentHit = hitIndex;
                updateValueFindNavigationState();
            }
        });
    }

    if (m_valueFindHits.isEmpty()) {
        const int selectedCount = selectedActiveSignalIndexesForFind().size();
        m_valueFindSummaryBase = selectedCount > 0
            ? QStringLiteral("%1 highlighted active signal(s) selected.").arg(selectedCount)
            : QStringLiteral("No highlighted active signal is selected.");
        updateValueFindNavigationState();
    }

    m_valueFindDialog->show();
    m_valueFindDialog->raise();
    m_valueFindDialog->activateWindow();
    if (m_valueFindEdit) {
        m_valueFindEdit->setFocus();
        m_valueFindEdit->selectAll();
    }
}

void MainWindow::runValueFind() {
    if (!m_valueFindEdit) return;

    ParsedValueFindTarget target;
    const QString targetText = m_valueFindEdit->text().trimmed();
    if (!parseValueFindTargetText(targetText, target)) {
        QMessageBox::warning(this,
            QStringLiteral("Find value"),
            QStringLiteral("Enter a numeric target, for example 10, 0xA, 0b1010, or -1."));
        m_valueFindEdit->setFocus();
        m_valueFindEdit->selectAll();
        return;
    }

    const QList<int> signalIndexes = selectedActiveSignalIndexesForFind();
    if (signalIndexes.isEmpty()) {
        QMessageBox::information(this,
            QStringLiteral("Find value"),
            QStringLiteral("Highlight one or more active signals before searching."));
        return;
    }

    const qint64 waveStart = m_wave.meta.start;
    const qint64 waveEnd = m_wave.meta.end;
    const bool selectedRange = m_valueFindRangeCombo && m_valueFindRangeCombo->currentIndex() == 1;
    qint64 searchStart = waveStart;
    qint64 searchEnd = waveEnd;
    if (selectedRange && m_canvas) {
        searchStart = qMax(waveStart, m_canvas->viewStart());
        searchEnd = qMin(waveEnd, m_canvas->viewEnd());
    }
    if (searchEnd <= searchStart) {
        QMessageBox::information(this,
            QStringLiteral("Find value"),
            QStringLiteral("The selected time range is empty."));
        return;
    }

    int widthSkippedSignals = 0;
    bool streamingEligible = true;
    bool allSearchSamplesAvailable = true;
    QHash<int, quint64> targetBitsBySignalIndex;
    QHash<int, int> signalIndexBySignalId;
    QVector<WaveParser4Reader::SignalValueMatchRequest> streamingRequests;
    targetBitsBySignalIndex.reserve(signalIndexes.size() * 2 + 1);
    signalIndexBySignalId.reserve(signalIndexes.size() * 2 + 1);
    streamingRequests.reserve(signalIndexes.size());
    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& signal = m_wave.signalList.at(signalIndex);
        quint64 targetBits = 0;
        if (!valueFindTargetForSignal(target, signal.width, targetBits)) {
            ++widthSkippedSignals;
            continue;
        }
        targetBitsBySignalIndex.insert(signalIndex, targetBits);
        if (signal.signalId <= 0) {
            streamingEligible = false;
        } else {
            WaveParser4Reader::SignalValueMatchRequest request;
            request.signalId = signal.signalId;
            request.targetBits = targetBits;
            streamingRequests.push_back(request);
            signalIndexBySignalId.insert(signal.signalId, signalIndex);
        }
        if (!signal.proceduralClock && !signal.samplesLoaded &&
            !(selectedRange &&
              waveSignalRawSamplesCoverRange(signal, searchStart, searchEnd))) {
            allSearchSamplesAvailable = false;
        }
    }

    const bool streamingDisabled =
        qEnvironmentVariableIsSet("WV_VIEWER_DISABLE_STREAMING_VALUE_FIND") &&
        qgetenv("WV_VIEWER_DISABLE_STREAMING_VALUE_FIND") != QByteArray("0");
    const bool hasSearchableSignals = !targetBitsBySignalIndex.isEmpty();
    const bool useStreamingValueFind =
        hasSearchableSignals &&
        streamingEligible && !streamingDisabled &&
        !allSearchSamplesAvailable &&
        m_currentWaveSupportsOnDemand &&
        hasWvz4Suffix(m_currentWaveFilePath) &&
        bool(m_waveReader);
    QVector<WaveParser4Reader::SignalValueMatchSegment> streamedMatches;

    QElapsedTimer findStageTimer;
    if (viewerPerfLogEnabled()) findStageTimer.start();
    if (useStreamingValueFind) {
        QString error;
        bool found = false;
        if (m_blockCacheLoader) m_blockCacheLoader->pauseBackground();
        {
            std::lock_guard<std::mutex> readerLock(*m_waveReaderMutex);
            found = m_waveReader->findSignalValueMatches(
                streamingRequests, searchStart, searchEnd,
                streamedMatches, error, kViewerOnDemandSampleBudget);
        }
        if (m_blockCacheLoader) m_blockCacheLoader->resumeBackground();
        if (!found) {
            QMessageBox::warning(
                this, QStringLiteral("Find value"),
                error.isEmpty()
                    ? QStringLiteral("Unable to search the selected signal values.")
                    : error);
            return;
        }
    } else if (!hasSearchableSignals) {
        // Width validation already proved that no selected signal can contain
        // this positive target; avoid decoding data that cannot affect the
        // empty result.
    } else if (selectedRange) {
        if (!ensureSignalSamplesLoaded(signalIndexes, false, true, false, searchStart, searchEnd)) return;
    } else if (!ensureSignalSamplesLoaded(signalIndexes, false)) {
        return;
    }
    if (viewerPerfLogEnabled()) {
        viewerPerfLog("find.load", findStageTimer.restart(),
                      signalIndexes.size(), m_wave.tree.nodesById.size());
    }

    QElapsedTimer timer;
    timer.start();

    m_valueFindHits.clear();
    m_valueFindSignalIndexes = signalIndexes;
    m_valueFindRangeStart = searchStart;
    m_valueFindRangeEnd = searchEnd;
    m_valueFindCurrentHit = -1;

    int matchedSignalCount = 0;
    if (useStreamingValueFind) {
        QSet<int> matchedSignalIndexes;
        matchedSignalIndexes.reserve(signalIndexes.size() * 2 + 1);
        for (const WaveParser4Reader::SignalValueMatchSegment& segment :
             streamedMatches) {
            const auto signalIt =
                signalIndexBySignalId.constFind(segment.signalId);
            if (signalIt == signalIndexBySignalId.constEnd() ||
                segment.end <= segment.start) {
                continue;
            }
            ValueFindHit hit;
            hit.signalIndex = signalIt.value();
            hit.sampleIndex = -1;
            hit.time = segment.start;
            hit.duration = clampedValueFindSegmentDuration(
                segment.start, segment.end, searchStart, searchEnd);
            if (hit.duration <= 0) continue;
            m_valueFindHits.push_back(hit);
            matchedSignalIndexes.insert(hit.signalIndex);
        }
        matchedSignalCount = matchedSignalIndexes.size();
    } else {
        for (int signalIndex : signalIndexes) {
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

            const auto targetIt =
                targetBitsBySignalIndex.constFind(signalIndex);
            if (targetIt == targetBitsBySignalIndex.constEnd()) continue;
            const quint64 targetBits = targetIt.value();
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            const int hitCountBeforeSignal = m_valueFindHits.size();

            const quint64 mask = waveBitMaskForWidth(sig.width);
            auto sampleMatches = [&](const WaveSample& sample) {
                if (sample.isAbsent || sample.isZ) return false;
                if (sample.rawFieldsReady) return (sample.rawBits & mask) == targetBits;

                WaveSample hydrated = sample;
                hydrateWaveSampleRawFields(sig.kind, sig.width, hydrated);
                return !hydrated.isAbsent && !hydrated.isZ &&
                       ((hydrated.rawBits & mask) == targetBits);
            };

            if (sig.proceduralClock) {
                qint64 segmentStart = searchStart;
                qint64 nextTransition =
                    waveProceduralClockNextTransition(sig, searchStart);
                while (segmentStart < searchEnd) {
                    const qint64 segmentEnd =
                        nextTransition > segmentStart
                            ? qMin(searchEnd, nextTransition)
                            : searchEnd;
                    const quint64 value =
                        waveProceduralClockValueAtTime(sig, segmentStart) ? 1u : 0u;
                    if ((value & mask) == targetBits) {
                        ValueFindHit hit;
                        hit.signalIndex = signalIndex;
                        hit.sampleIndex = -1;
                        hit.time = segmentStart;
                        hit.duration = clampedValueFindSegmentDuration(
                            segmentStart, segmentEnd, searchStart, searchEnd);
                        m_valueFindHits.push_back(hit);
                    }
                    if (segmentEnd >= searchEnd) break;
                    segmentStart = segmentEnd;
                    nextTransition =
                        waveProceduralClockNextTransition(sig, segmentStart);
                }
                if (m_valueFindHits.size() > hitCountBeforeSignal) {
                    ++matchedSignalCount;
                }
                continue;
            }

            const auto firstAfterStart = std::upper_bound(
                sig.samples.constBegin(), sig.samples.constEnd(), searchStart,
                [](qint64 time, const WaveSample& sample) { return time < sample.time; });
            const int firstAfterStartIndex = int(firstAfterStart - sig.samples.constBegin());
            const int stateAtStartIndex = firstAfterStartIndex - 1;
            bool previousMatched = stateAtStartIndex >= 0 &&
                                   sampleMatches(sig.samples.at(stateAtStartIndex));
            int activeHitIndex = -1;
            if (previousMatched) {
                ValueFindHit hit;
                hit.signalIndex = signalIndex;
                hit.sampleIndex = stateAtStartIndex;
                hit.time = searchStart;
                activeHitIndex = m_valueFindHits.size();
                m_valueFindHits.push_back(hit);
            }

            for (int sampleIndex = firstAfterStartIndex; sampleIndex < sig.samples.size(); ++sampleIndex) {
                const WaveSample& sample = sig.samples.at(sampleIndex);
                if (sample.time >= searchEnd) break;
                const bool matched = sampleMatches(sample);

                if (matched && !previousMatched) {
                    ValueFindHit hit;
                    hit.signalIndex = signalIndex;
                    hit.sampleIndex = sampleIndex;
                    hit.time = sample.time;
                    activeHitIndex = m_valueFindHits.size();
                    m_valueFindHits.push_back(hit);
                } else if (!matched && previousMatched && activeHitIndex >= 0) {
                    m_valueFindHits[activeHitIndex].duration =
                        clampedValueFindSegmentDuration(m_valueFindHits.at(activeHitIndex).time, sample.time,
                                                        searchStart, searchEnd);
                    activeHitIndex = -1;
                }
                previousMatched = matched;
            }

            if (previousMatched && activeHitIndex >= 0) {
                m_valueFindHits[activeHitIndex].duration =
                    clampedValueFindSegmentDuration(m_valueFindHits.at(activeHitIndex).time, searchEnd,
                                                    searchStart, searchEnd);
            }
            if (m_valueFindHits.size() > hitCountBeforeSignal) ++matchedSignalCount;
        }
    }
    if (viewerPerfLogEnabled()) {
        viewerPerfLog("find.scan", findStageTimer.restart(),
                      signalIndexes.size(), m_wave.tree.nodesById.size(),
                      m_valueFindHits.size());
    }

    const auto hitLess = [](const ValueFindHit& a, const ValueFindHit& b) {
        if (a.time != b.time) return a.time < b.time;
        if (a.signalIndex != b.signalIndex) return a.signalIndex < b.signalIndex;
        return a.sampleIndex < b.sampleIndex;
    };
    if (!std::is_sorted(m_valueFindHits.constBegin(), m_valueFindHits.constEnd(), hitLess)) {
        std::stable_sort(m_valueFindHits.begin(), m_valueFindHits.end(), hitLess);
    }
    if (viewerPerfLogEnabled()) {
        viewerPerfLog("find.sort", findStageTimer.restart(),
                      signalIndexes.size(), m_wave.tree.nodesById.size(),
                      m_valueFindHits.size());
    }

    const QString rangeSummary = selectedRange
        ? QStringLiteral("selected range %1 to %2")
              .arg(formatInternalDisplayTime(searchStart), formatInternalDisplayTime(searchEnd))
        : QStringLiteral("global range");
    m_valueFindSummaryBase = QStringLiteral("%1 target segment(s) in %2/%3 highlighted signal(s), %4, scanned in %5 ms.")
        .arg(m_valueFindHits.size())
        .arg(matchedSignalCount)
        .arg(signalIndexes.size())
        .arg(rangeSummary)
        .arg(timer.elapsed());
    if (widthSkippedSignals > 0) {
        m_valueFindSummaryBase += QStringLiteral(" %1 signal(s) skipped because the positive target exceeds their width.")
            .arg(widthSkippedSignals);
    }

    rebuildValueFindResults();
    if (viewerPerfLogEnabled()) {
        viewerPerfLog("find.results", findStageTimer.restart(),
                      signalIndexes.size(), m_wave.tree.nodesById.size(),
                      m_valueFindHits.size());
    }
    // Finding a value should not move the waveform viewport implicitly.
    // Double-clicking a result or using Previous/Next remains an explicit jump.
    updateValueFindNavigationState();
    if (viewerPerfLogEnabled()) {
        viewerPerfLog("find.finalize", findStageTimer.elapsed(),
                      signalIndexes.size(), m_wave.tree.nodesById.size(),
                      m_valueFindHits.size());
    }
}

void MainWindow::rebuildValueFindResults() {
    if (!m_valueFindResults) return;

    struct SignalFindStats {
        int count = 0;
        int firstHit = -1;
        qint64 duration = 0;
    };
    QHash<int, SignalFindStats> statsBySignal;
    statsBySignal.reserve(m_valueFindSignalIndexes.size());
    for (int i = 0; i < m_valueFindHits.size(); ++i) {
        const ValueFindHit& hit = m_valueFindHits.at(i);
        QHash<int, SignalFindStats>::iterator it = statsBySignal.find(hit.signalIndex);
        if (it == statsBySignal.end()) {
            SignalFindStats initial;
            initial.firstHit = i;
            it = statsBySignal.insert(hit.signalIndex, initial);
        }
        ++it->count;
        it->duration += hit.duration;
    }

    const qint64 totalDuration = std::max<qint64>(0, m_valueFindRangeEnd - m_valueFindRangeStart);
    QSignalBlocker resultSignals(m_valueFindResults);
    m_valueFindResults->setUpdatesEnabled(false);
    m_valueFindResults->clear();
    QList<QTreeWidgetItem*> resultItems;
    resultItems.reserve(m_valueFindSignalIndexes.size());
    for (int signalIndex : m_valueFindSignalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

        const SignalFindStats stats = statsBySignal.value(signalIndex);
        const int count = stats.count;
        const int firstHit = stats.firstHit;
        auto* item = new QTreeWidgetItem();
        item->setText(0, signalDisplayName(signalIndex));
        item->setTextAlignment(0, int(Qt::AlignRight | Qt::AlignVCenter));
        item->setText(1, QString::number(count));
        item->setText(2, firstHit >= 0 ? formatInternalDisplayTime(m_valueFindHits.at(firstHit).time) : QStringLiteral("-"));
        item->setText(3, formatValueFindTimeShare(stats.duration, totalDuration));
        item->setData(0, kValueFindRoleFirstHit, firstHit);
        item->setData(0, kValueFindRoleSignalIndex, signalIndex);
        if (count <= 0) {
            item->setForeground(1, QBrush(QColor("#AAB3BC")));
            item->setForeground(2, QBrush(QColor("#AAB3BC")));
            item->setForeground(3, QBrush(QColor("#AAB3BC")));
        }
        resultItems.push_back(item);
    }
    m_valueFindResults->addTopLevelItems(resultItems);
    m_valueFindResults->setUpdatesEnabled(true);
}

void MainWindow::updateValueFindNavigationState() {
    const bool hasHits = !m_valueFindHits.isEmpty();
    if (m_valueFindPrevButton) m_valueFindPrevButton->setEnabled(hasHits);
    if (m_valueFindNextButton) m_valueFindNextButton->setEnabled(hasHits);

    if (!m_valueFindSummaryLabel) return;

    QString summary = m_valueFindSummaryBase;
    if (m_valueFindCurrentHit >= 0 && m_valueFindCurrentHit < m_valueFindHits.size()) {
        const ValueFindHit& hit = m_valueFindHits.at(m_valueFindCurrentHit);
        summary += QStringLiteral(" Current: %1 @ %2 (%3/%4).")
            .arg(signalDisplayName(hit.signalIndex))
            .arg(formatInternalDisplayTime(hit.time))
            .arg(m_valueFindCurrentHit + 1)
            .arg(m_valueFindHits.size());
    }
    m_valueFindSummaryLabel->setText(summary);
}

void MainWindow::jumpToValueFindHit(int hitIndex) {
    if (hitIndex < 0 || hitIndex >= m_valueFindHits.size() || !m_canvas) return;

    const ValueFindHit hit = m_valueFindHits.at(hitIndex);
    if (!m_canvas->jumpToTime(hit.time)) return;

    m_valueFindCurrentHit = hitIndex;

    if (m_activeList) {
        for (int row = 0; row < m_activeList->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = m_activeList->topLevelItem(row);
            if (!item || signalIndexFromActiveItem(item) != hit.signalIndex) continue;
            m_activeList->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            m_activeList->scrollToItem(item);
            break;
        }
        syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
    }

    if (m_valueFindResults) {
        QSignalBlocker blocker(m_valueFindResults);
        for (int row = 0; row < m_valueFindResults->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = m_valueFindResults->topLevelItem(row);
            if (!item || item->data(0, kValueFindRoleSignalIndex).toInt() != hit.signalIndex) continue;
            m_valueFindResults->setCurrentItem(item);
            m_valueFindResults->scrollToItem(item);
            break;
        }
    }

    refreshActiveValueLabels();
    updateValueFindNavigationState();
}

void MainWindow::jumpToAdjacentValueFindHit(bool forward) {
    if (m_valueFindHits.isEmpty()) return;

    int targetIndex = -1;
    if (m_valueFindCurrentHit >= 0 && m_valueFindCurrentHit < m_valueFindHits.size()) {
        targetIndex = m_valueFindCurrentHit + (forward ? 1 : -1);
        if (targetIndex < 0) targetIndex = m_valueFindHits.size() - 1;
        if (targetIndex >= m_valueFindHits.size()) targetIndex = 0;
    } else {
        const qint64 cursor = m_canvas ? m_canvas->cursorTime() : -1;
        if (cursor >= 0) {
            if (forward) {
                for (int i = 0; i < m_valueFindHits.size(); ++i) {
                    if (m_valueFindHits.at(i).time > cursor) {
                        targetIndex = i;
                        break;
                    }
                }
            } else {
                for (int i = m_valueFindHits.size() - 1; i >= 0; --i) {
                    if (m_valueFindHits.at(i).time < cursor) {
                        targetIndex = i;
                        break;
                    }
                }
            }
        }
        if (targetIndex < 0) targetIndex = forward ? 0 : (m_valueFindHits.size() - 1);
    }

    jumpToValueFindHit(targetIndex);
}

void MainWindow::jumpToPreviousValueFindHit() {
    jumpToAdjacentValueFindHit(false);
}

void MainWindow::jumpToNextValueFindHit() {
    jumpToAdjacentValueFindHit(true);
}

void MainWindow::jumpToPrevChange() {
    const QList<int> signalIndexes = selectedActiveSignalIndexesForJump();
    if (signalIndexes.isEmpty()) return;
    const bool diffOnly = (QApplication::keyboardModifiers() & Qt::ControlModifier) != 0;
    if (m_canvas->jumpToNearestChangeForSignals(signalIndexes, false, diffOnly)) {
        refreshActiveValueLabels();
    }
}

void MainWindow::jumpToNextChange() {
    const QList<int> signalIndexes = selectedActiveSignalIndexesForJump();
    if (signalIndexes.isEmpty()) return;
    const bool diffOnly = (QApplication::keyboardModifiers() & Qt::ControlModifier) != 0;
    if (m_canvas->jumpToNearestChangeForSignals(signalIndexes, true, diffOnly)) {
        refreshActiveValueLabels();
    }
}

void MainWindow::jumpToTime() {
    if (!m_canvas) return;

    const qint64 rangeStart = m_canvas->fullStartTime();
    const qint64 rangeEnd = m_canvas->fullEndTime();

    const QString minText = formatInternalDisplayTime(rangeStart);
    const QString maxText = formatInternalDisplayTime(rangeEnd);
    const QString rangeText = QStringLiteral("%1 ~ %2").arg(minText, maxText);

    if (rangeEnd < rangeStart) {
        QMessageBox::warning(this,
            QString::fromUtf8("无法跳转"),
            QString::fromUtf8("当前波形时间范围无效。"));
        return;
    }

    QString input = m_jumpTimeEdit ? m_jumpTimeEdit->text().trimmed() : QString();
    if (input.isEmpty()) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间为空"),
            QString::fromUtf8("请输入时间轴显示值。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->setFocus();
        return;
    }

    bool parsed = false;
    qint64 internalTime = 0;
    parsed = waveParseDisplayTime(input, internalTime);
    if (!parsed) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间格式不正确"),
            QString::fromUtf8("请输入数字时间。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (internalTime < rangeStart || internalTime > rangeEnd) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间超出范围"),
            QString::fromUtf8("输入时间不在合理范围内。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (internalTime < rangeStart || internalTime > rangeEnd) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间超出范围"),
            QString::fromUtf8("输入时间换算为内部整数时间后超出范围。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (!m_canvas->jumpToTime(internalTime)) {
        QMessageBox::warning(this,
            QString::fromUtf8("无法跳转"),
            QString::fromUtf8("无法跳转到该时间。\\n合理范围：%1").arg(rangeText));
        if (m_jumpTimeEdit) m_jumpTimeEdit->selectAll();
        return;
    }

    if (m_jumpTimeEdit) {
        m_jumpTimeEdit->setText(formatInternalDisplayTime(internalTime));
        m_jumpTimeEdit->selectAll();
    }
    refreshActiveValueLabels();
}

void MainWindow::applyWindowRangeInput() {
    if (!m_canvas || !m_windowRangeStartEdit || !m_windowRangeEndEdit) return;

    qint64 start = 0;
    qint64 end = 0;
    const bool parsed = waveParseDisplayTime(m_windowRangeStartEdit->text(), start) &&
                        waveParseDisplayTime(m_windowRangeEndEdit->text(), end);

    const qint64 fullStart = m_canvas->fullStartTime();
    const qint64 fullEnd = m_canvas->fullEndTime();
    const QString validRange = QStringLiteral("%1 ~ %2")
        .arg(formatInternalDisplayTime(fullStart), formatInternalDisplayTime(fullEnd));

    if (!parsed || end <= start) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间范围格式不正确"),
            QString::fromUtf8("请分别输入起始时间和结束时间，且结束时间大于起始时间。\n可用范围：%1")
                .arg(validRange));
        if (!waveParseDisplayTime(m_windowRangeStartEdit->text(), start)) {
            m_windowRangeStartEdit->setFocus();
            m_windowRangeStartEdit->selectAll();
        } else {
            m_windowRangeEndEdit->setFocus();
            m_windowRangeEndEdit->selectAll();
        }
        return;
    }

    if (start < fullStart || end > fullEnd) {
        QMessageBox::warning(this,
            QString::fromUtf8("时间范围超出波形范围"),
            QString::fromUtf8("输入范围必须位于：%1").arg(validRange));
        QLineEdit* invalidEdit = start < fullStart ? m_windowRangeStartEdit : m_windowRangeEndEdit;
        invalidEdit->setFocus();
        invalidEdit->selectAll();
        return;
    }

    m_windowRangeStartEdit->setModified(false);
    m_windowRangeEndEdit->setModified(false);
    onViewportRangeSelected(start, end);
}

void MainWindow::openDerivedSignalDialog() {
    if (m_wave.signalList.empty()) {
        QMessageBox::information(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("Open a waveform before creating a temporary signal."));
        return;
    }

    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);
    dialog->setWindowTitle(QStringLiteral("Create temporary signal"));
    dialog->resize(680, 230);

    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* nameEdit = new QLineEdit(dialog);
    nameEdit->setText(QStringLiteral("tmp_expr"));
    nameEdit->setPlaceholderText(QStringLiteral("tmp_expr"));
    form->addRow(QStringLiteral("Name"), nameEdit);

    auto* exprEdit = new QLineEdit(dialog);
    exprEdit->setPlaceholderText(QStringLiteral("`top.a` & `top.b`, (`valid` && `ready`), `bus` + 4"));

    auto* exprRow = new QHBoxLayout();
    exprRow->setSpacing(6);
    auto* insertSelectedButton = new QPushButton(QStringLiteral("Insert selected"), dialog);
    exprRow->addWidget(exprEdit, 1);
    exprRow->addWidget(insertSelectedButton);
    form->addRow(QStringLiteral("Expression"), exprRow);

    auto* widthSpin = new QSpinBox(dialog);
    widthSpin->setRange(0, 64);
    widthSpin->setValue(0);
    widthSpin->setSpecialValueText(QStringLiteral("auto"));
    form->addRow(QStringLiteral("Width"), widthSpin);
    root->addLayout(form);

    auto* hint = new QLabel(QStringLiteral("This window is non-modal: select/copy signal names from the main viewer while it stays open. Supported operators: + - * / % & | ^ ~ << >> && || ! == != < <= > >=."), dialog);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* buttonRow = new QHBoxLayout();
    auto* createButton = new QPushButton(QStringLiteral("Create"), dialog);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), dialog);
    buttonRow->addStretch(1);
    buttonRow->addWidget(createButton);
    buttonRow->addWidget(closeButton);
    root->addLayout(buttonRow);

    auto selectedSignalIndex = [this]() -> int {
        if (m_activeList) {
            if (QTreeWidgetItem* item = m_activeList->currentItem()) {
                const int signalIndex = signalIndexFromActiveItem(item);
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }
            const QList<QTreeWidgetItem*> picked = m_activeList->selectedItems();
            for (QTreeWidgetItem* item : picked) {
                const int signalIndex = signalIndexFromActiveItem(item);
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }
        }

        if (m_tree && m_tree->selectionModel()) {
            const QModelIndex current = m_tree->currentIndex();
            const QVariant currentSignal = current.data(kTreeRoleSignalIndex);
            if (currentSignal.isValid()) {
                const int signalIndex = currentSignal.toInt();
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }

            const QModelIndexList picked = m_tree->selectionModel()->selectedRows(0);
            for (const QModelIndex& index : picked) {
                const QVariant signal = index.data(kTreeRoleSignalIndex);
                if (!signal.isValid()) continue;
                const int signalIndex = signal.toInt();
                if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) return signalIndex;
            }
        }

        return -1;
    };

    auto quoteSignalName = [](QString name) {
        name.replace(QLatin1Char('`'), QLatin1Char('_'));
        return QStringLiteral("`") + name + QStringLiteral("`");
    };

    connect(insertSelectedButton, &QPushButton::clicked, this, [this, exprEdit, selectedSignalIndex, quoteSignalName]() {
        const int signalIndex = selectedSignalIndex();
        if (signalIndex < 0) {
            QMessageBox::information(this,
                QStringLiteral("Create temporary signal"),
                QStringLiteral("Select a signal in the active list or signal tree first."));
            return;
        }

        const QString text = quoteSignalName(signalDisplayName(signalIndex));
        if (!exprEdit->text().isEmpty()) exprEdit->insert(QStringLiteral(" "));
        exprEdit->insert(text);
        exprEdit->setFocus();
    });

    connect(createButton, &QPushButton::clicked, this, [this, dialog, nameEdit, exprEdit, widthSpin]() {
        if (createDerivedSignal(nameEdit->text(), exprEdit->text(), widthSpin->value())) {
            dialog->close();
        }
    });
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    exprEdit->setFocus();
}

bool MainWindow::createDerivedSignal(const QString& name, const QString& expression, int widthOverride) {
    const bool perf = viewerPerfLogEnabled();
    QElapsedTimer derivedStepTimer;
    QElapsedTimer derivedTotalTimer;
    if (perf) {
        derivedStepTimer.start();
        derivedTotalTimer.start();
    }

    const QString signalName = name.trimmed();
    const QString expr = expression.trimmed();
    if (signalName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create temporary signal"), QStringLiteral("Enter a signal name."));
        return false;
    }
    if (expr.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create temporary signal"), QStringLiteral("Enter an expression."));
        return false;
    }

    auto resolveExplicitNamedSignal = [&](const QString& key, bool leafOnly,
                                          int& signalIndex, int& width,
                                          bool& ambiguous) {
        signalIndex = -1;
        width = 1;
        ambiguous = false;
        // Normal WVZ4 signals are represented by compact TREE tokens and have
        // no QString name here.  This reverse scan is therefore primarily for
        // the small set of temporary signals appended by the Viewer.
        const int firstExplicitSignal =
            m_wave.tree.valid
                ? qMin(int(m_wave.signalList.size()),
                       int(m_wave.tree.signalIndexToNodeId.size()))
                : 0;
        for (int i = int(m_wave.signalList.size()) - 1;
             i >= firstExplicitSignal; --i) {
            const WaveSignal& signal = m_wave.signalList.at(i);
            if (signal.name.isEmpty()) continue;
            QString candidate = stripDisplayRangeSuffix(signal.name);
            if (leafOnly) {
                const int dot = candidate.lastIndexOf(QLatin1Char('.'));
                if (dot >= 0) candidate = candidate.mid(dot + 1);
            }
            if (candidate != key) continue;
            if (signalIndex >= 0 && signalIndex != i) {
                ambiguous = true;
                signalIndex = -1;
                return false;
            }
            signalIndex = i;
            width = signal.width;
        }
        return signalIndex >= 0;
    };

    auto resolveIndexedSignal = [&](const QString& key, int& signalIndex,
                                    int& width, bool& ambiguous) {
        signalIndex = -1;
        width = 1;
        ambiguous = false;
        const bool leafOnly = !key.contains(QLatin1Char('.'));
        if (m_signalTreeModel) {
            const bool found = leafOnly
                ? m_signalTreeModel->resolveUniqueLeafSignal(
                      key, signalIndex, width, ambiguous)
                : m_signalTreeModel->resolveExactSignalPath(
                      key, signalIndex, width, ambiguous);
            if (found || ambiguous) return found;
        }
        return resolveExplicitNamedSignal(
            key, leafOnly, signalIndex, width, ambiguous);
    };

    const QString normalizedSignalName = stripDisplayRangeSuffix(signalName);
    int existingSignalIndex = -1;
    int existingWidth = 1;
    bool existingNameAmbiguous = false;
    const bool signalNameExists = resolveIndexedSignal(
        normalizedSignalName, existingSignalIndex, existingWidth,
        existingNameAmbiguous) || existingNameAmbiguous;
    if (signalNameExists) {
        QMessageBox::warning(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("Signal name already exists. Choose another name."));
        return false;
    }

    auto resolveSignal = [&](const QString& rawName, int& signalIndex, int& width, QString& error) -> bool {
        const QString key = stripDisplayRangeSuffix(rawName);
        if (key.isEmpty()) {
            error = QStringLiteral("Empty signal name in expression.");
            return false;
        }

        bool ambiguous = false;
        if (resolveIndexedSignal(key, signalIndex, width, ambiguous)) {
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return false;
            width = qBound(1, m_wave.signalList.at(signalIndex).width, 64);
            return true;
        }
        if (ambiguous) {
            error = key.contains(QLatin1Char('.'))
                ? QStringLiteral("Signal name '%1' is ambiguous; use a unique full path.").arg(rawName)
                : QStringLiteral("Leaf signal name '%1' is ambiguous; use the full path in backticks.").arg(rawName);
            return false;
        }

        error = QStringLiteral("Unknown signal '%1'.").arg(rawName);
        return false;
    };

    DerivedExpressionProgram program;
    QString parseError;
    DerivedExpressionParser parser(expr, resolveSignal);
    if (!parser.parse(program, parseError)) {
        if (signalName == QStringLiteral("__derived_expression_benchmark")) {
            qWarning().noquote() << "Derived expression parse failed:" << parseError;
            std::fprintf(stderr, "Derived expression parse failed: %s\n",
                         parseError.toLocal8Bit().constData());
            std::fflush(stderr);
        }
        QMessageBox::warning(this, QStringLiteral("Create temporary signal"), parseError);
        return false;
    }
    if (perf) {
        viewerPerfLog("derived.resolve_parse", derivedStepTimer.restart(),
                      program.dependencyIndexes.size(), m_wave.tree.nodesById.size(), program.nodes.size());
    }

    if (!ensureSignalSamplesLoaded(program.dependencyIndexes, false)) {
        return false;
    }
    if (perf) {
        viewerPerfLog("derived.raw_load", derivedStepTimer.restart(),
                      program.dependencyIndexes.size(), m_wave.tree.nodesById.size());
    }

    const int outputWidth = qBound(1, widthOverride > 0 ? widthOverride : program.inferredWidth, 64);
    const SignalKind outputKind = (outputWidth <= 1) ? SignalKind::Bit : SignalKind::Bus;
    const ValueRadix outputRadix = (outputWidth <= 1) ? ValueRadix::Bin : ValueRadix::Hex;

    QVector<int> samplePositions(program.dependencyIndexes.size(), 0);
    QVector<DerivedEvalValue> currentValues(program.dependencyIndexes.size());
    QVector<DerivedEvalValue> evalWorkspace(program.nodes.size());
    QVector<WaveSample> outputSamples;
    QVector<qint64> outputChangeTimes;

    qint64 estimatedInputEvents = 1;
    for (int signalIndex : program.dependencyIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        estimatedInputEvents = qMin<qint64>(
            qint64(kViewerOnDemandSampleBudget),
            estimatedInputEvents + m_wave.signalList.at(signalIndex).samples.size());
    }
    const int initialReserve = int(qMin<qint64>(estimatedInputEvents, 262144));
    outputSamples.reserve(qMax(1, initialReserve));
    outputChangeTimes.reserve(qMax(0, initialReserve - 1));

    auto consumeSlotAtOrBefore = [&](int slot, qint64 t) -> bool {
        if (slot < 0 || slot >= program.dependencyIndexes.size()) return false;
        const int signalIndex = program.dependencyIndexes.at(slot);
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return false;
        WaveSignal& sig = m_wave.signalList[signalIndex];
        int& pos = samplePositions[slot];
        const DerivedEvalValue previous = currentValues.at(slot);
        while (pos < sig.samples.size() && sig.samples.at(pos).time <= t) {
            WaveSample& source = sig.samples[pos++];
            DerivedEvalValue value;
            if (!source.isAbsent && !source.isZ) {
                if (!source.rawFieldsReady) {
                    hydrateWaveSampleRawFields(sig.kind, sig.width, source);
                }
                if (!source.isAbsent && !source.isZ && source.rawFieldsReady) {
                    value.known = true;
                    value.bits = source.rawBits;
                }
            }
            currentValues[slot] = value;
        }
        const DerivedEvalValue& current = currentValues.at(slot);
        return previous.known != current.known ||
            (current.known && previous.bits != current.bits);
    };

    bool haveLastResult = false;
    DerivedEvalValue lastResult;
    auto appendResultAt = [&](qint64 t, quint64 changedMask,
                              bool forceAllDependencies,
                              bool initialize) -> bool {
        DerivedEvalValue value = evalDerivedExpression(
            program, currentValues, evalWorkspace, changedMask,
            forceAllDependencies, initialize);
        if (value.known) value.bits &= waveBitMaskForWidth(outputWidth);
        const bool unchanged = haveLastResult && value.known == lastResult.known &&
            (!value.known || value.bits == lastResult.bits);
        if (unchanged) return true;

        WaveSample sample = makeDerivedSample(t, value, outputWidth);
        if (!outputSamples.isEmpty()) outputChangeTimes.push_back(t);
        outputSamples.push_back(std::move(sample));
        haveLastResult = true;
        lastResult = value;
        if (outputSamples.size() > int(kViewerOnDemandSampleBudget)) return false;
        return true;
    };

    const qint64 startTime = m_wave.meta.start;
    for (int slot = 0; slot < program.dependencyIndexes.size(); ++slot) {
        consumeSlotAtOrBefore(slot, startTime);
    }
    if (!appendResultAt(startTime, ~quint64(0), true, true)) {
        QMessageBox::warning(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("The derived signal generated too many transitions."));
        return false;
    }

    struct PendingDerivedEvent {
        qint64 time = 0;
        int slot = -1;
    };
    struct PendingDerivedEventLater {
        bool operator()(const PendingDerivedEvent& lhs, const PendingDerivedEvent& rhs) const {
            if (lhs.time != rhs.time) return lhs.time > rhs.time;
            return lhs.slot > rhs.slot;
        }
    };
    std::priority_queue<PendingDerivedEvent,
                        std::vector<PendingDerivedEvent>,
                        PendingDerivedEventLater> pendingEvents;

    auto queueNextSlotEvent = [&](int slot) {
        if (slot < 0 || slot >= program.dependencyIndexes.size()) return;
        const int signalIndex = program.dependencyIndexes.at(slot);
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) return;
        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        const int pos = samplePositions.at(slot);
        if (pos < sig.samples.size()) pendingEvents.push(PendingDerivedEvent{sig.samples.at(pos).time, slot});
    };
    auto appendOrReportOverflow = [&](qint64 time, quint64 changedMask,
                                      bool forceAllDependencies) {
        if (appendResultAt(time, changedMask, forceAllDependencies, false)) return true;
        QMessageBox::warning(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("The derived signal generated too many transitions."));
        return false;
    };

    if (program.dependencyIndexes.size() == 1) {
        const int signalIndex = program.dependencyIndexes.first();
        if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) {
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            while (samplePositions.first() < sig.samples.size()) {
                const qint64 nextTime = sig.samples.at(samplePositions.first()).time;
                if (consumeSlotAtOrBefore(0, nextTime) &&
                    !appendOrReportOverflow(nextTime, 1u, false)) {
                    return false;
                }
            }
        }
    } else if (program.dependencyIndexes.size() <= 8) {
        // Most interactive expressions reference only two or three signals.
        // A tiny linear merge beats priority_queue churn and branchy heap
        // maintenance for this common case.
        for (;;) {
            qint64 nextTime = std::numeric_limits<qint64>::max();
            for (int slot = 0; slot < program.dependencyIndexes.size(); ++slot) {
                const int signalIndex = program.dependencyIndexes.at(slot);
                if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
                const WaveSignal& sig = m_wave.signalList.at(signalIndex);
                const int pos = samplePositions.at(slot);
                if (pos < sig.samples.size()) nextTime = qMin(nextTime, sig.samples.at(pos).time);
            }
            if (nextTime == std::numeric_limits<qint64>::max()) break;

            quint64 changedMask = 0;
            bool forceAllDependencies = false;
            for (int slot = 0; slot < program.dependencyIndexes.size(); ++slot) {
                const int signalIndex = program.dependencyIndexes.at(slot);
                if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
                const WaveSignal& sig = m_wave.signalList.at(signalIndex);
                const int pos = samplePositions.at(slot);
                if (pos >= sig.samples.size() || sig.samples.at(pos).time != nextTime) continue;
                if (!consumeSlotAtOrBefore(slot, nextTime)) continue;
                if (slot < 64) changedMask |= quint64(1) << slot;
                else forceAllDependencies = true;
            }
            if ((changedMask != 0 || forceAllDependencies) &&
                !appendOrReportOverflow(nextTime, changedMask, forceAllDependencies)) {
                return false;
            }
        }
    } else {
        for (int slot = 0; slot < program.dependencyIndexes.size(); ++slot) queueNextSlotEvent(slot);
        while (!pendingEvents.empty()) {
            const qint64 nextTime = pendingEvents.top().time;
            quint64 changedMask = 0;
            bool forceAllDependencies = false;
            do {
                const int slot = pendingEvents.top().slot;
                pendingEvents.pop();
                if (consumeSlotAtOrBefore(slot, nextTime)) {
                    if (slot < 64) changedMask |= quint64(1) << slot;
                    else forceAllDependencies = true;
                }
                queueNextSlotEvent(slot);
            } while (!pendingEvents.empty() && pendingEvents.top().time == nextTime);

            if ((changedMask != 0 || forceAllDependencies) &&
                !appendOrReportOverflow(nextTime, changedMask, forceAllDependencies)) {
                return false;
            }
        }
    }

    if (outputSamples.isEmpty()) {
        QMessageBox::warning(this,
            QStringLiteral("Create temporary signal"),
            QStringLiteral("Expression produced no samples."));
        return false;
    }
    if (perf) {
        viewerPerfLog("derived.evaluate", derivedStepTimer.restart(),
                      program.dependencyIndexes.size(), m_wave.tree.nodesById.size(), outputSamples.size());
    }

    int maxSignalId = m_signalIndexBySignalId.size() - 1;
    while (maxSignalId >= 0 && m_signalIndexBySignalId.at(maxSignalId) == 0) {
        --maxSignalId;
    }

    WaveSignal derived;
    derived.signalId = maxSignalId + 1;
    derived.storageId = -1;
    derived.name = signalName;
    derived.kind = outputKind;
    derived.width = outputWidth;
    derived.defaultRadix = outputRadix;
    derived.currentRadix = outputRadix;
    derived.supportsZState = true;
    derived.samplesLoaded = true;
    derived.samples = std::move(outputSamples);
    derived.changeTimes = std::move(outputChangeTimes);
    derived.changeTimesReady = true;
    derived.lodLevels = buildDerivedLodLevels(
        derived.samples, derivedLodBucketCycles(m_wave, program.dependencyIndexes));
    if (perf) {
        qint64 lodSampleCount = 0;
        for (const WaveLodLevel& level : derived.lodLevels) lodSampleCount += level.samples.size();
        viewerPerfLog("derived.lod", derivedStepTimer.restart(),
                      derived.lodLevels.size(), int(qMin<qint64>(lodSampleCount, std::numeric_limits<int>::max())),
                      derived.samples.size());
    }

    const int newSignalIndex = waveSignalCount(m_wave.signalList);
    m_wave.signalList.push_back(std::move(derived));
    if (m_signalIndexBySignalId.size() <= maxSignalId + 1) {
        m_signalIndexBySignalId.resize(maxSignalId + 2);
    }
    m_signalIndexBySignalId[maxSignalId + 1] = newSignalIndex + 1;

    addSignalToActive(newSignalIndex);
    if (m_canvas) m_canvas->update();
    if (perf) {
        viewerPerfLog("derived.finalize", derivedStepTimer.restart(),
                      program.dependencyIndexes.size(), m_wave.tree.nodesById.size(),
                      m_wave.signalList.at(newSignalIndex).samples.size());
        viewerPerfLog("derived.total", derivedTotalTimer.elapsed(),
                      program.dependencyIndexes.size(), m_wave.tree.nodesById.size(),
                      m_wave.signalList.at(newSignalIndex).samples.size());
    }

    QMessageBox::information(this,
        QStringLiteral("Create temporary signal"),
        QStringLiteral("Temporary signal '%1' created with %2 transition sample(s).")
            .arg(signalName)
            .arg(m_wave.signalList.at(newSignalIndex).samples.size()));
    return true;
}

bool MainWindow::runDerivedExpressionForBenchmark(
    const QString& expression, int widthOverride, qint64* elapsedMs,
    qint64* outputSampleCount, quint64* outputChecksum) {
    if (elapsedMs) *elapsedMs = -1;
    if (outputSampleCount) *outputSampleCount = 0;
    if (outputChecksum) *outputChecksum = 0;

    const int previousSignalCount = int(m_wave.signalList.size());
    QString benchmarkDialogText;
    QTimer dialogCloser;
    dialogCloser.setInterval(10);
    connect(&dialogCloser, &QTimer::timeout, this, [&benchmarkDialogText]() {
        if (QWidget* modal = QApplication::activeModalWidget()) {
            if (QMessageBox* messageBox = qobject_cast<QMessageBox*>(modal)) {
                benchmarkDialogText = messageBox->text();
            }
            modal->close();
        }
    });
    dialogCloser.start();
    QElapsedTimer timer;
    timer.start();
    const bool ok = createDerivedSignal(
        QStringLiteral("__derived_expression_benchmark"), expression,
        widthOverride);
    dialogCloser.stop();
    if (elapsedMs) *elapsedMs = timer.elapsed();
    if (!ok || int(m_wave.signalList.size()) != previousSignalCount + 1) {
        if (!benchmarkDialogText.isEmpty()) {
            qWarning().noquote() << "Derived expression benchmark failed:"
                                 << benchmarkDialogText;
        }
        return false;
    }

    const WaveSignal& result = m_wave.signalList.back();
    quint64 checksum = 1469598103934665603ull;
    for (const WaveSample& sample : result.samples) {
        checksum ^= quint64(sample.time);
        checksum *= 1099511628211ull;
        checksum ^= sample.rawBits;
        checksum *= 1099511628211ull;
        checksum ^= sample.isZ ? 1ull : 0ull;
        checksum *= 1099511628211ull;
    }
    if (outputSampleCount) *outputSampleCount = qint64(result.samples.size());
    if (outputChecksum) *outputChecksum = checksum;
    return true;
}

void MainWindow::resetView() {
    if (!m_canvas) return;
    m_canvas->clearCursor();
    onViewportRangeSelected(m_canvas->fullStartTime(), m_canvas->fullEndTime());
    refreshActiveValueLabels();
}

void MainWindow::insertSignalIntoTree(const QString& fullName, int signalIndex) {
    Q_UNUSED(fullName);
    if (!m_tree || !m_treeModel || !m_signalTreeModel) return;
    const int nodeId = m_signalTreeModel->nodeIdForSignalIndex(signalIndex);
    if (nodeId < 0) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;
    const QModelIndex index = model->indexForNode(nodeId);
    if (!index.isValid()) return;
    m_tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
    if (m_tree->selectionModel()) {
        m_tree->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_tree->setCurrentIndex(index);
    }
}

void MainWindow::resetTreeViewModel() {
    if (!m_tree) return;
    if (!m_treeModel) {
        m_treeModel = new SignalTreeModel(m_tree);
        signalTreeModelFrom(m_treeModel)->setArrayFetchCallback(
            [this](int nodeId) { materializeArrayNode(nodeId); });
        m_tree->setModel(m_treeModel);
    }
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;
    model->setLogicTree(m_signalTreeModel.get());
    m_tree->collapseAll();
    m_tree->viewport()->update();
}

void MainWindow::rebuildTree() {
    if (!m_signalTreeModel) {
        m_signalTreeModel.reset(new SignalLogicTree);
    }

    if (m_wave.tree.valid) {
        m_signalTreeModel->buildFromWaveTree(m_wave.tree, m_wave.signalList);
    } else {
        m_signalTreeModel->buildFromSignalDefs(m_wave.signalList);
    }
    resetTreeViewModel();
}

void MainWindow::materializeBitsetNode(int nodeId) {
    if (!m_signalTreeModel || !m_signalTreeModel->isBitsetContainer(nodeId)) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;
    const int oldSignalCount = waveSignalCount(m_wave.signalList);
    QString error;
    if (!model->materializeBitsetChildren(m_wave.tree, m_wave.signalList,
                                          nodeId, error)) {
        QMessageBox::warning(this, QStringLiteral("Expand bitset failed"), error);
        return;
    }
    const int newSignalCount = waveSignalCount(m_wave.signalList);
    for (int signalIndex = oldSignalCount; signalIndex < newSignalCount; ++signalIndex) {
        const int signalId = m_wave.signalList.at(signalIndex).signalId;
        if (signalId <= 0) continue;
        if (m_signalIndexBySignalId.size() <= signalId) {
            m_signalIndexBySignalId.resize(signalId + 1);
        }
        m_signalIndexBySignalId[signalId] = signalIndex + 1;
    }
}

void MainWindow::materializeArrayNode(int nodeId) {
    if (!m_signalTreeModel || !m_signalTreeModel->isArrayContainer(nodeId)) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;
    const int oldSignalCount = waveSignalCount(m_wave.signalList);
    QString error;
    if (!model->materializeArrayChildren(m_wave.tree, m_wave.signalList, nodeId, error)) {
        QMessageBox::warning(this, QStringLiteral("Expand array failed"), error);
        return;
    }
    const int newSignalCount = waveSignalCount(m_wave.signalList);
    for (int signalIndex = oldSignalCount; signalIndex < newSignalCount; ++signalIndex) {
        const int signalId = m_wave.signalList.at(signalIndex).signalId;
        if (signalId <= 0) continue;
        if (m_signalIndexBySignalId.size() <= signalId) {
            m_signalIndexBySignalId.resize(signalId + 1);
        }
        m_signalIndexBySignalId[signalId] = signalIndex + 1;
    }
}

void MainWindow::stopTreeWarmup() {
    if (m_treeWarmupCancel) {
        m_treeWarmupCancel->store(true, std::memory_order_release);
    }
    if (m_treeWarmupControl) {
        m_treeWarmupControl->condition.notify_all();
    }
    if (m_treeWarmupThread.joinable()) {
        m_treeWarmupThread.join();
    }
    m_treeWarmupCancel.reset();
    m_treeWarmupControl.reset();
    ++m_treeWarmupGeneration;
}

void MainWindow::prioritizeTreeReference(int nodeId) {
    if (!m_signalTreeModel ||
        !m_signalTreeModel->isPendingReference(nodeId) ||
        !m_treeWarmupControl ||
        !m_treeWarmupCancel ||
        m_treeWarmupCancel->load(std::memory_order_acquire)) {
        return;
    }

    const std::shared_ptr<TreeWarmupControl> control = m_treeWarmupControl;
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        if (nodeId <= 0 ||
            nodeId >= control->referenceStateByNodeId.size() ||
            control->referenceStateByNodeId.at(nodeId) != 0) {
            return;
        }
        control->referenceStateByNodeId[nodeId] = 1;
        control->priorityReferences.push_back(nodeId);
    }
    control->condition.notify_all();
}

void MainWindow::scheduleTreeWarmup() {
    if (!m_signalTreeModel || !m_signalTreeModel->usesDirectWaveTree() ||
        !m_wave.tree.valid || m_wave.tree.nodesById.isEmpty()) {
        return;
    }

    // QVector copies are implicitly shared.  The worker keeps immutable
    // snapshots alive if another wave replaces m_wave while it is running.
    WaveTreeInfo sourceTree = m_wave.tree;
    bool hasPendingReference = false;
    for (int nodeId = 1; nodeId < sourceTree.nodesById.size(); ++nodeId) {
        const WaveTreeNode& node = sourceTree.nodesById.at(nodeId);
        if (node.valid &&
            node.kind == kWaveTreeNodeKindReference &&
            node.referenceTargetId > 0 &&
            node.firstChild == 0) {
            hasPendingReference = true;
            break;
        }
    }
    if (!hasPendingReference) return;

    const quint64 generation = m_treeWarmupGeneration;
    const std::shared_ptr<std::atomic_bool> cancel =
        std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<TreeWarmupControl> control =
        std::make_shared<TreeWarmupControl>();
    control->referenceStateByNodeId.resize(sourceTree.nodesById.size());
    std::fill(control->referenceStateByNodeId.begin(),
              control->referenceStateByNodeId.end(), quint8(0));
    m_treeWarmupCancel = cancel;
    m_treeWarmupControl = control;

    struct ReferenceBatch {
        QVector<WaveSubtreeReferencePatch> patches;
    };

    m_treeWarmupThread = std::thread(
        [this, generation, cancel, control,
         sourceTree = std::move(sourceTree)]() mutable {
            std::shared_ptr<ReferenceBatch> batch =
                std::make_shared<ReferenceBatch>();
            int batchNodeCount = 0;

            auto releasePendingBatch = [control]() {
                {
                    std::lock_guard<std::mutex> lock(control->mutex);
                    if (control->pendingUiBatches > 0) {
                        --control->pendingUiBatches;
                    }
                }
                control->condition.notify_all();
            };

            auto postBatch = [&](bool force) -> bool {
                if (batch->patches.isEmpty()) return true;
                if (!force && batch->patches.size() < 16 &&
                    batchNodeCount < 65536) {
                    return true;
                }
                {
                    std::unique_lock<std::mutex> lock(control->mutex);
                    control->condition.wait(lock, [&]() {
                        return cancel->load(std::memory_order_acquire) ||
                               control->pendingUiBatches < 2;
                    });
                    if (cancel->load(std::memory_order_acquire)) return false;
                    ++control->pendingUiBatches;
                }

                std::shared_ptr<ReferenceBatch> posted = std::move(batch);
                batch = std::make_shared<ReferenceBatch>();
                batchNodeCount = 0;
                QMetaObject::invokeMethod(
                    this,
                    [this, generation, cancel, control, posted,
                     releasePendingBatch]() mutable {
                        if (!cancel->load(std::memory_order_acquire) &&
                            generation == m_treeWarmupGeneration &&
                            m_signalTreeModel &&
                            m_signalTreeModel->usesDirectWaveTree()) {
                            SignalTreeModel* model =
                                signalTreeModelFrom(m_treeModel);
                            QString patchError;
                            if (!model ||
                                !model->installReferencePatches(
                                    m_wave.tree,
                                    posted->patches,
                                    patchError)) {
                                if (!patchError.isEmpty()) {
                                    statusBar()->showMessage(
                                        QStringLiteral(
                                            "Tree reference load failed: %1")
                                            .arg(patchError),
                                        5000);
                                }
                            }
                            // Materializing an opened reference only changes its
                            // children.  Reapplying search navigation here would
                            // scroll back to the previous match while the user is
                            // browsing another node.
                            if (!m_treeSearchActive) {
                                retryPendingViewerSessionRestore();
                            }
                            if (m_tree) m_tree->viewport()->update();
                        }
                        releasePendingBatch();
                    },
                    Qt::QueuedConnection);
                return true;
            };

            while (!cancel->load(std::memory_order_acquire)) {
                int nodeId = 0;
                {
                    std::unique_lock<std::mutex> lock(control->mutex);
                    control->condition.wait(lock, [&]() {
                        return cancel->load(std::memory_order_acquire) ||
                               !control->priorityReferences.empty();
                    });
                    if (cancel->load(std::memory_order_acquire)) return;
                    while (!control->priorityReferences.empty()) {
                        const int candidate =
                            control->priorityReferences.front();
                        control->priorityReferences.pop_front();
                        if (candidate > 0 &&
                            candidate <
                                control->referenceStateByNodeId.size() &&
                            control->referenceStateByNodeId.at(candidate) == 1) {
                            control->referenceStateByNodeId[candidate] = 2;
                            nodeId = candidate;
                            break;
                        }
                    }
                }
                if (nodeId == 0) continue;

                // References are materialized only when the user reaches one.
                // Posting each requested mount immediately keeps navigation
                // responsive without allowing an idle full-tree expansion to
                // consume gigabytes and later stall the GUI thread.
                if (!postBatch(true)) return;

                WaveSubtreeReferencePatch patch;
                QString patchError;
                if (!buildWaveSubtreeReferencePatch(
                        sourceTree, nodeId, patch, patchError, cancel.get())) {
                    if (cancel->load(std::memory_order_acquire)) return;
                    continue;
                }
                batchNodeCount += patch.appendedNodes.size();
                batch->patches.push_back(std::move(patch));
                if (!postBatch(true)) return;
            }
        });
}

void MainWindow::collectSignalIndexesFromLogicNode(int nodeId, QSet<int>& seen, QList<int>& output) const {
    if (!m_signalTreeModel || !m_signalTreeModel->isValidNodeId(nodeId)) return;

    const int signalIndex = m_signalTreeModel->nodeSignalIndex(nodeId);
    if (signalIndex >= 0) {
        if (!seen.contains(signalIndex)) {
            seen.insert(signalIndex);
            output.push_back(signalIndex);
        }
        return;
    }

    const LogicChildList* list = m_signalTreeModel->childListForNode(nodeId);
    if (!list) return;

    for (int childNodeId : list->children) {
        collectSignalIndexesFromLogicNode(childNodeId, seen, output);
    }
}

void MainWindow::showTreeSearchResults(const QString& query,
                                       bool preserveSnapshot) {
    if (!m_treeModel || !m_signalTreeModel) return;
    SignalTreeModel* model = signalTreeModelFrom(m_treeModel);
    if (!model) return;

    const QString q = query.trimmed();
    if (q.isEmpty()) {
        m_applyingTreeExpansionState = true;
        if (!m_treeSearchCropMode) {
            for (int i = m_treeSearchAutoExpandedNodeIds.size() - 1;
                 i >= 0; --i) {
                const int nodeId = m_treeSearchAutoExpandedNodeIds.at(i);
                const QString path =
                    m_signalTreeModel->fullPathForNodeId(nodeId);
                if (m_userExpandedNodePaths.contains(path)) continue;
                const QModelIndex index = model->indexForNode(nodeId);
                if (index.isValid() && m_tree->isExpanded(index)) {
                    m_tree->collapse(index);
                }
            }
        }
        model->clearSearch();
        if (m_treeSearchCropMode) m_tree->collapseAll();
        m_applyingTreeExpansionState = false;
        m_treeSearchActive = false;
        m_treeSearchCropMode = false;
        m_treeSearchMatchedNodeIds.clear();
        m_treeSearchAutoExpandedNodeIds.clear();
        m_treeSearchCurrentMatch = -1;
        if (m_treeSearchPrevButton) m_treeSearchPrevButton->setEnabled(false);
        if (m_treeSearchNextButton) m_treeSearchNextButton->setEnabled(false);
        if (m_signalConditionPrevButton) m_signalConditionPrevButton->setEnabled(false);
        if (m_signalConditionNextButton) m_signalConditionNextButton->setEnabled(false);
        if (m_treeSearchEdit) m_treeSearchEdit->setToolTip(QString());
        if (!preserveSnapshot) restoreTreeSearchState();
        else if (m_treeSearchRestoreButton) {
            m_treeSearchRestoreButton->setEnabled(m_treeSearchSnapshotValid);
        }
        return;
    }

    const int maxResults = 5000;
    const Qt::CaseSensitivity caseSensitivity =
        (m_treeSearchCaseButton && m_treeSearchCaseButton->isChecked())
        ? Qt::CaseSensitive
        : Qt::CaseInsensitive;
    const bool regexMode = m_treeSearchRegexButton && m_treeSearchRegexButton->isChecked();
    if (regexMode) {
        QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
        if (caseSensitivity == Qt::CaseInsensitive) {
            options |= QRegularExpression::CaseInsensitiveOption;
        }
        const QRegularExpression re(q, options);
        if (!re.isValid()) {
            if (m_treeSearchEdit) m_treeSearchEdit->setToolTip(re.errorString());
            return;
        }
    }
    supersedeSignalConditionSearch();
    if (m_treeSearchActive && m_treeSearchCropMode) {
        showTreeSearchResults(QString(), true);
    }
    if (m_treeSearchEdit) m_treeSearchEdit->setToolTip(QString());

    captureTreeSearchState();

    m_treeSearchMatchedNodeIds = m_signalTreeModel->searchTreeQuery(
        q, maxResults, caseSensitivity, regexMode);
    m_treeSearchActive = true;
    m_treeSearchCropMode = false;
    m_treeSearchCurrentMatch = m_treeSearchMatchedNodeIds.isEmpty() ? -1 : 0;
    model->setSearchRoots(m_treeSearchMatchedNodeIds, false);
    const bool canNavigate = m_treeSearchMatchedNodeIds.size() > 1;
    if (m_treeSearchPrevButton) m_treeSearchPrevButton->setEnabled(canNavigate);
    if (m_treeSearchNextButton) m_treeSearchNextButton->setEnabled(canNavigate);
    if (m_signalConditionPrevButton) m_signalConditionPrevButton->setEnabled(canNavigate);
    if (m_signalConditionNextButton) m_signalConditionNextButton->setEnabled(canNavigate);
    applyTreeSearchExpansion();
    if (m_treeSearchEdit) {
        m_treeSearchEdit->setToolTip(m_treeSearchMatchedNodeIds.isEmpty()
            ? QStringLiteral("No matches")
            : QStringLiteral("Match 1 of %1").arg(m_treeSearchMatchedNodeIds.size()));
    }
}

void MainWindow::onTreeSearchTextChanged(const QString& text) {
    showTreeSearchResults(text);
}

void MainWindow::onTreeIndexDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    const QVariant data = index.data(kTreeRoleSignalIndex);
    if (data.isValid()) {
        addSignalToActive(data.toInt());
        return;
    }
    if (m_tree && m_treeModel && m_treeModel->hasChildren(index)) {
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
        m_tree->viewport()->update();
    }
}


int MainWindow::signalIndexFromActiveItem(QTreeWidgetItem* item) const {
    return item ? item->data(0, RoleSignalIndex).toInt() : -1;
}

ValueRadix MainWindow::formatFromActiveItem(QTreeWidgetItem* item) const {
    if (!item) return ValueRadix::Bin;
    return textToFormat(item->data(0, RoleCurrentFormat).toString());
}

void MainWindow::setActiveItemFormat(QTreeWidgetItem* item, const QString& text) {
    if (!item) return;

    QList<QTreeWidgetItem*> picked = m_activeList->selectedItems();
    if (picked.isEmpty() || !picked.contains(item)) {
        picked.clear();
        picked.push_back(item);
    }

    bool changed = false;
    const ValueRadix fmt = textToFormat(text);
    for (QTreeWidgetItem* one : picked) {
        const int signalIndex = signalIndexFromActiveItem(one);
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        if (sig.width != 32 && (fmt == ValueRadix::Int || fmt == ValueRadix::UInt || fmt == ValueRadix::Float)) {
            continue;
        }
        one->setData(0, RoleCurrentFormat, text.toLower());
        changed = true;
    }

    if (changed) {
        rebuildVisibleSignals();
        refreshActiveValueLabels();
    }
}

bool MainWindow::canDeferSamplesWithLod(const WaveSignal& sig) const {
    if (viewerDisableLodEnabled()) return false;
    if (!m_canvas) return false;
    const int plotWidth = qMax(1, m_canvas->width() - 20);
    return signalHasLoadedLodForWindow(sig, m_canvas->viewStart(), m_canvas->viewEnd(), plotWidth);
}

bool MainWindow::ensureSignalLodLoaded(const QList<int>& signalIndexes) {
    if (viewerDisableLodEnabled()) return true;
    if (!m_currentWaveFilePath.endsWith(".wvz4", Qt::CaseInsensitive)) return true;
    if (signalIndexes.isEmpty()) return true;

    const qint64 viewStart = m_canvas ? m_canvas->viewStart() : m_wave.meta.start;
    const qint64 viewEnd = m_canvas ? m_canvas->viewEnd() : m_wave.meta.end;
    const qint64 viewSpan = qMax<qint64>(1, viewEnd - viewStart);
    const qint64 loadStart = (viewStart > std::numeric_limits<qint64>::min() + viewSpan)
        ? qMax(m_wave.meta.start, viewStart - viewSpan)
        : m_wave.meta.start;
    const qint64 loadEnd = (viewEnd < std::numeric_limits<qint64>::max() - viewSpan)
        ? qMin(m_wave.meta.end, viewEnd + viewSpan)
        : m_wave.meta.end;
    const int plotWidth = m_canvas ? qMax(1, m_canvas->width() - 20) : 1;
    const double cyclesPerPixel = double(viewSpan) / double(plotWidth);
    if (cyclesPerPixel < 10.0) return true;

    QVector<int> signalIdsToLoad;
    QSet<int> seenIds;
    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        // Canonical std::bitset word streams have word-level LOD summaries, which cannot
        // represent an individual bit exactly. Virtual bits therefore use the
        // raw on-demand path only.
        if (sig.virtualBitsetBit) continue;
        if (signalHasLoadedLodForWindow(sig, viewStart, viewEnd, plotWidth)) continue;
        if (sig.samplesLoaded) continue;
        if (sig.signalId <= 0) continue;
        if (seenIds.contains(sig.signalId)) continue;
        seenIds.insert(sig.signalId);
        signalIdsToLoad.push_back(sig.signalId);
    }
    if (signalIdsToLoad.isEmpty()) return true;

    WaveFile loadedWave;
    QString error;
    QElapsedTimer lodTimer;
    if (viewerPerfLogEnabled()) lodTimer.start();
    bool lodOk = false;
    if (m_waveReader && m_waveReaderMutex) {
        std::lock_guard<std::mutex> readerLock(*m_waveReaderMutex);
        lodOk = m_waveReader->loadSignalLod(signalIdsToLoad,
                                            loadedWave,
                                            error,
                                            loadStart,
                                            qMax(loadStart + 1, loadEnd),
                                            qMax<qint64>(1, qint64(std::floor(cyclesPerPixel))));
    }
    if (!lodOk) {
        QMessageBox::warning(this,
            QStringLiteral("Load LOD failed"),
            error.isEmpty() ? QStringLiteral("Unable to load LOD for selected signals.") : error);
        return false;
    }
    if (viewerPerfLogEnabled()) {
        viewerPerfLog("lod.load", lodTimer.elapsed(),
                      waveSignalCount(loadedWave.signalList),
                      loadedWave.tree.nodesById.size(),
                      signalIdsToLoad.size());
    }

    for (WaveSignal& loadedSig : loadedWave.signalList) {
        if (loadedSig.signalId < 0 || loadedSig.lodLevels.isEmpty()) continue;
        int targetIndex = -1;
        const int sid = loadedSig.signalId;
        if (sid >= 0 && sid < m_signalIndexBySignalId.size()) {
            targetIndex = m_signalIndexBySignalId.at(sid) - 1;
        }
        if (targetIndex < 0 || targetIndex >= m_wave.signalList.size()) continue;
        QVector<WaveLodLevel>& targetLevels = m_wave.signalList[targetIndex].lodLevels;
        if (targetLevels.size() < loadedSig.lodLevels.size()) targetLevels.resize(loadedSig.lodLevels.size());
        for (int levelIndex = 0; levelIndex < loadedSig.lodLevels.size(); ++levelIndex) {
            mergeLodLevel(targetLevels[levelIndex], std::move(loadedSig.lodLevels[levelIndex]));
        }
    }

    if (m_canvas) m_canvas->update();
    return true;
}

bool MainWindow::ensureSignalSamplesLoaded(const QList<int>& signalIndexes,
                                           bool allowLodDefer,
                                           bool viewportRaw,
                                           bool quiet,
                                           qint64 requestedRawStart,
                                           qint64 requestedRawEnd) {
    if (!m_currentWaveSupportsOnDemand || m_currentWaveFilePath.isEmpty()) return true;
    if (signalIndexes.isEmpty()) return true;

    if (allowLodDefer && !ensureSignalLodLoaded(signalIndexes)) return false;
    const bool useViewportRawWindow = viewportRaw &&
        m_currentWaveFilePath.endsWith(".wvz4", Qt::CaseInsensitive);

    const bool haveRequestedRawWindow = requestedRawEnd > requestedRawStart;
    const qint64 viewStart = haveRequestedRawWindow
        ? requestedRawStart
        : (m_canvas ? m_canvas->viewStart() : m_wave.meta.start);
    const qint64 viewEnd = haveRequestedRawWindow
        ? requestedRawEnd
        : (m_canvas ? m_canvas->viewEnd() : m_wave.meta.end);
    const qint64 viewSpan = qMax<qint64>(1, viewEnd - viewStart);
    const qint64 rawLoadStart = (viewStart > std::numeric_limits<qint64>::min() + viewSpan)
        ? qMax(m_wave.meta.start, viewStart - viewSpan)
        : m_wave.meta.start;
    const qint64 rawLoadEnd = (viewEnd < std::numeric_limits<qint64>::max() - viewSpan)
        ? qMin(m_wave.meta.end, viewEnd + viewSpan)
        : m_wave.meta.end;

    QVector<int> signalIdsToLoad;
    QSet<int> seenIds;
    QList<int> validIndexes;

    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        if (sig.samplesLoaded) continue;
        if (useViewportRawWindow && waveSignalRawSamplesCoverRange(sig, viewStart, viewEnd)) continue;
        if (allowLodDefer && canDeferSamplesWithLod(sig)) continue;
        if (sig.signalId < 0) continue;
        if (seenIds.contains(sig.signalId)) continue;
        seenIds.insert(sig.signalId);
        signalIdsToLoad.push_back(sig.signalId);
        validIndexes.push_back(signalIndex);
    }

    if (signalIdsToLoad.isEmpty()) return true;

    struct RawLoadBatch {
        qint64 start = 0;
        qint64 end = std::numeric_limits<qint64>::max();
        QVector<int> signalIds;
        WaveFile wave;
    };
    QVector<RawLoadBatch> batches;
    if (useViewportRawWindow) {
        QMap<QPair<qint64, qint64>, QVector<int>> idsByMissingRange;
        for (int signalIndex : validIndexes) {
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            const QVector<WaveLodValidRange> missing =
                missingRangesForWindow(sig.rawLoadedRanges, rawLoadStart, qMax(rawLoadStart + 1, rawLoadEnd));
            for (const WaveLodValidRange& range : missing) {
                idsByMissingRange[qMakePair(range.start, range.end)].push_back(sig.signalId);
            }
        }
        for (auto it = idsByMissingRange.constBegin(); it != idsByMissingRange.constEnd(); ++it) {
            RawLoadBatch batch;
            batch.start = it.key().first;
            batch.end = it.key().second;
            batch.signalIds = it.value();
            batches.push_back(std::move(batch));
        }
    } else {
        RawLoadBatch batch;
        batch.signalIds = signalIdsToLoad;
        batches.push_back(std::move(batch));
    }
    if (batches.isEmpty()) return true;

    QString error;
    bool loadOk = hasWvz4Suffix(m_currentWaveFilePath) && bool(m_waveReader);
    if (!hasWvz4Suffix(m_currentWaveFilePath)) {
        error = QStringLiteral("On-demand loading supports WVZ4 files (*.wvz4) only.");
    } else if (!m_waveReader) {
        error = QStringLiteral("WVZ4 indexed reader is not available.");
    }
    QElapsedTimer rawTimer;
    if (viewerPerfLogEnabled()) rawTimer.start();
    if (loadOk) {
        std::lock_guard<std::mutex> readerLock(*m_waveReaderMutex);
        for (RawLoadBatch& batch : batches) {
            if (!loadOk) break;
            loadOk = m_waveReader->loadSignals(batch.signalIds,
                                               batch.wave,
                                               error,
                                               kViewerOnDemandSampleBudget,
                                               batch.start,
                                               batch.end);
        }
    }
    if (viewerPerfLogEnabled()) {
        viewerPerfLog("raw.load", rawTimer.elapsed(),
                      signalIdsToLoad.size(), m_wave.tree.nodesById.size(), batches.size());
    }

    if (!loadOk && allowLodDefer && isDecodedSampleBudgetError(error)) {
        if (!ensureSignalLodLoaded(signalIndexes)) return false;
        bool allCoveredByLod = true;
        for (int signalIndex : validIndexes) {
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
            if (!canDeferSamplesWithLod(m_wave.signalList.at(signalIndex))) {
                allCoveredByLod = false;
                break;
            }
        }
        if (allCoveredByLod) {
            if (m_canvas) m_canvas->update();
            return true;
        }
    }

    if (!loadOk) {
        if (quiet) {
            statusBar()->showMessage(
                error.isEmpty()
                    ? QStringLiteral("Unable to load selected signal samples.")
                    : error,
                2600);
            return false;
        }
        QMessageBox::warning(this,
            QString::fromUtf8("加载信号失败"),
            error.isEmpty() ? QString::fromUtf8("无法按需加载所选信号。") : error);
        return false;
    }

    QSet<int> returnedIds;
    for (RawLoadBatch& batch : batches) {
        for (WaveSignal& loadedSig : batch.wave.signalList) {
            if (loadedSig.signalId < 0) continue;
            returnedIds.insert(loadedSig.signalId);
            int targetIndex = -1;
            const int sid = loadedSig.signalId;
            if (sid >= 0 && sid < m_signalIndexBySignalId.size()) {
                targetIndex = m_signalIndexBySignalId.at(sid) - 1;
            }
            if (targetIndex < 0 || targetIndex >= m_wave.signalList.size()) continue;

            WaveSignal& target = m_wave.signalList[targetIndex];
            if (useViewportRawWindow) {
                mergeRawSamples(target, std::move(loadedSig.samples));
            } else {
                target.samples = std::move(loadedSig.samples);
                target.rawLoadedRanges.clear();
            }
            target.samplesLoaded = !useViewportRawWindow;
            target.supportsZState = loadedSig.supportsZState;
            target.defaultRadix = loadedSig.defaultRadix;
        }

        if (useViewportRawWindow) {
            for (int sid : batch.signalIds) {
                if (sid < 0 || sid >= m_signalIndexBySignalId.size()) continue;
                const int targetIndex = m_signalIndexBySignalId.at(sid) - 1;
                if (targetIndex < 0 || targetIndex >= m_wave.signalList.size()) continue;
                WaveLodValidRange loadedRange;
                loadedRange.start = batch.start;
                loadedRange.end = batch.end;
                m_wave.signalList[targetIndex].rawLoadedRanges.push_back(loadedRange);
                compactLodRanges(m_wave.signalList[targetIndex].rawLoadedRanges);
            }
        }
    }

    for (int signalIndex : validIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        WaveSignal& sig = m_wave.signalList[signalIndex];
        if (!useViewportRawWindow && seenIds.contains(sig.signalId) && !returnedIds.contains(sig.signalId)) {
            // A constant signal may produce no records. Mark it as fully loaded
            // so selecting it does not scan the file repeatedly.
            sig.samples.clear();
            sig.samplesLoaded = true;
            sig.rawLoadedRanges.clear();
        }
        if (useViewportRawWindow) {
            trimRawSamplesToWindow(sig, rawLoadStart, qMax(rawLoadStart + 1, rawLoadEnd));
            QVector<WaveLodValidRange> retained;
            for (const WaveLodValidRange& range : sig.rawLoadedRanges) {
                WaveLodValidRange clipped;
                clipped.start = qMax(range.start, rawLoadStart);
                clipped.end = qMin(range.end, qMax(rawLoadStart + 1, rawLoadEnd));
                if (clipped.end > clipped.start) retained.push_back(clipped);
            }
            sig.rawLoadedRanges = std::move(retained);
            compactLodRanges(sig.rawLoadedRanges);
        }
        rebuildWaveSignalDerivedCaches(sig);
    }

    if (m_canvas) {
        m_canvas->invalidateSignalSampleCaches(validIndexes.toVector());
        m_canvas->update();
    }
    return true;
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
bool MainWindow::ensureSignalSamplesLoaded(const QVector<int>& signalIndexes,
                                           bool allowLodDefer,
                                           bool viewportRaw,
                                           bool quiet,
                                           qint64 requestedRawStart,
                                           qint64 requestedRawEnd) {
    return ensureSignalSamplesLoaded(signalIndexes.toList(), allowLodDefer, viewportRaw, quiet,
                                     requestedRawStart, requestedRawEnd);
}
#endif

void MainWindow::addSignalToActive(int signalIndex) {
    addSignalIndexesToActive(QList<int>() << signalIndex);
}

void MainWindow::addSignalIndexesToActive(const QList<int>& signalIndexes) {
    if (signalIndexes.isEmpty()) return;
    if (!m_currentWaveSupportsOnDemand) {
        if (!ensureSignalLodLoaded(signalIndexes)) return;
        if (!ensureSignalSamplesLoaded(signalIndexes, true, true)) return;
    }

    QList<QTreeWidgetItem*> addedItems;
    addedItems.reserve(signalIndexes.size());

    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;

        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        auto* item = new QTreeWidgetItem();
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setText(0, formatNameWithRange(signalIndex));
        item->setText(1, "-");
        item->setData(0, RoleSignalIndex, signalIndex);
        item->setData(0, RoleSignalWidth, sig.width);
        item->setData(0, RoleCurrentFormat, formatToText(sig.defaultRadix));
        item->setSizeHint(0, QSize(0, 40));
        item->setSizeHint(1, QSize(0, 40));
        addedItems.push_back(item);
    }

    if (addedItems.isEmpty()) return;

    const int firstAddedRow = m_activeList->topLevelItemCount();
    const bool updatesWereEnabled = m_activeList->updatesEnabled();
    m_activeList->setUpdatesEnabled(false);
    {
        // Inserting and selecting one row at a time emits thousands of model and
        // selection notifications.  The selection callback scans the selection,
        // turning a large add into O(N^2).  Append and select the contiguous block
        // once, then publish the final selection to the canvas below.
        QSignalBlocker blocker(m_activeList);
        m_activeList->addTopLevelItems(addedItems);
        if (QItemSelectionModel* selection = m_activeList->selectionModel()) {
            const QModelIndex first = m_activeList->model()->index(firstAddedRow, 0);
            const QModelIndex last = m_activeList->model()->index(
                firstAddedRow + addedItems.size() - 1, 0);
            QItemSelection rows(first, last);
            selection->select(rows, QItemSelectionModel::ClearAndSelect |
                                     QItemSelectionModel::Rows);
        }
        m_activeList->setCurrentItem(addedItems.last(), 0, QItemSelectionModel::NoUpdate);
    }
    m_activeList->setUpdatesEnabled(updatesWereEnabled);
    if (updatesWereEnabled && m_activeList->viewport()) m_activeList->viewport()->update();
    const bool comparisonWave = m_wave.meta.hasCompareSources ||
                                !m_wave.meta.compareLeftPath.isEmpty() ||
                                !m_wave.meta.compareRightPath.isEmpty();
    if (comparisonWave && m_activeList->topLevelItemCount() > 1) {
        sortActiveSignalsByFirstDifference();
        return;
    }
    m_activeList->scrollToItem(addedItems.last());

    rebuildVisibleSignals();
    refreshActiveValueLabels();
    syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
    if (m_currentWaveSupportsOnDemand && m_canvas) {
        scheduleViewportDataLoad(m_canvas->viewStart(), m_canvas->viewEnd());
    }
}

void MainWindow::sortActiveSignalsByFirstDifference() {
    if (!m_activeList || m_activeList->topLevelItemCount() < 2) return;

    QTreeWidgetItem* const currentItem = m_activeList->currentItem();
    const int topVisibleRow = qBound(
        0, m_activeList->verticalScrollBar()->value(),
        m_activeList->topLevelItemCount() - 1);
    QTreeWidgetItem* const topVisibleItem =
        m_activeList->topLevelItem(topVisibleRow);
    QSet<QTreeWidgetItem*> selectedItems;
    selectedItems.reserve(m_activeList->topLevelItemCount());
    for (int row = 0; row < m_activeList->topLevelItemCount(); ++row) {
        QTreeWidgetItem* const item = m_activeList->topLevelItem(row);
        if (item && item->isSelected()) selectedItems.insert(item);
    }
    QList<QTreeWidgetItem*> items =
        m_activeList->invisibleRootItem()->takeChildren();

    auto firstDifference = [this](QTreeWidgetItem* item) {
        const int signalIndex = signalIndexFromActiveItem(item);
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size() ||
            m_wave.signalList.at(signalIndex).diffRegions.isEmpty()) {
            return (std::numeric_limits<qint64>::max)();
        }
        return m_wave.signalList.at(signalIndex).diffRegions.first().start;
    };
    std::stable_sort(items.begin(), items.end(),
                     [&firstDifference](QTreeWidgetItem* left,
                                        QTreeWidgetItem* right) {
        return firstDifference(left) < firstDifference(right);
    });

    const bool updatesWereEnabled = m_activeList->updatesEnabled();
    m_activeList->setUpdatesEnabled(false);
    {
        QSignalBlocker blocker(m_activeList);
        m_activeList->addTopLevelItems(items);
        m_activeList->clearSelection();
        if (QItemSelectionModel* selectionModel = m_activeList->selectionModel()) {
            // Re-selecting N rows through QTreeWidgetItem::setSelected() grows
            // the selection model one range at a time. When comparison added
            // every differing signal, that turns restoration into O(N^2) and
            // makes the UI appear hung. Build the final contiguous ranges in
            // one linear pass and publish one selection transaction instead.
            QItemSelection selection;
            int rangeBegin = -1;
            const int rowCount = items.size();
            for (int row = 0; row <= rowCount; ++row) {
                const bool selected = row < rowCount &&
                    selectedItems.contains(items.at(row));
                if (selected && rangeBegin < 0) {
                    rangeBegin = row;
                } else if (!selected && rangeBegin >= 0) {
                    const QModelIndex first =
                        m_activeList->model()->index(rangeBegin, 0);
                    const QModelIndex last =
                        m_activeList->model()->index(row - 1, 1);
                    selection.select(first, last);
                    rangeBegin = -1;
                }
            }
            selectionModel->select(
                selection,
                QItemSelectionModel::ClearAndSelect |
                    QItemSelectionModel::Rows);
        }
        if (currentItem) {
            m_activeList->setCurrentItem(currentItem, 0,
                                         QItemSelectionModel::NoUpdate);
        }
    }
    m_activeList->setUpdatesEnabled(updatesWereEnabled);
    if (topVisibleItem) {
        m_activeList->scrollToItem(topVisibleItem,
                                   QAbstractItemView::PositionAtTop);
    }
    if (updatesWereEnabled && m_activeList->viewport()) {
        m_activeList->viewport()->update();
    }

    rebuildVisibleSignals();
    refreshActiveValueLabels();
    syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
}

void MainWindow::removeActiveItem(QTreeWidgetItem* item) {
    if (!item) return;

    QList<int> rows;
    if (item->isSelected() && m_activeList->selectionModel()) {
        const QModelIndexList picked =
            m_activeList->selectionModel()->selectedRows(0);
        rows.reserve(picked.size());
        for (const QModelIndex& index : picked) {
            if (index.isValid() && index.row() >= 0) rows.push_back(index.row());
        }
    } else {
        const int row = m_activeList->indexOfTopLevelItem(item);
        if (row >= 0) rows.push_back(row);
    }
    removeActiveRows(rows);
}

void MainWindow::removeActiveRows(const QList<int>& inputRows) {
    if (inputRows.isEmpty() || !m_activeList) return;

    QList<int> rows = inputRows;
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    const int total = m_activeList->topLevelItemCount();
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [total](int row) {
                                  return row < 0 || row >= total;
                              }),
               rows.end());
    if (rows.isEmpty()) return;

    const bool updatesWereEnabled = m_activeList->updatesEnabled();
    m_activeList->setUpdatesEnabled(false);
    {
        QSignalBlocker blocker(m_activeList);
        if (rows.size() == total) {
            m_activeList->clear();
        } else if (rows.size() <= 32) {
            for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
                delete m_activeList->takeTopLevelItem(*it);
            }
        } else {
            // Repeated takeTopLevelItem() shifts the remaining child array after
            // every removal. Rebuild the pointer list once so Ctrl+A/Delete and
            // other large removals stay O(N).
            QList<QTreeWidgetItem*> allItems =
                m_activeList->invisibleRootItem()->takeChildren();
            QList<QTreeWidgetItem*> keptItems;
            keptItems.reserve(allItems.size() - rows.size());
            int removedCursor = 0;
            for (int row = 0; row < allItems.size(); ++row) {
                if (removedCursor < rows.size() && rows.at(removedCursor) == row) {
                    delete allItems.at(row);
                    ++removedCursor;
                } else {
                    keptItems.push_back(allItems.at(row));
                }
            }
            m_activeList->addTopLevelItems(keptItems);
            if (!keptItems.isEmpty()) {
                const int currentRow = qMin(rows.first(), keptItems.size() - 1);
                QTreeWidgetItem* current = m_activeList->topLevelItem(currentRow);
                if (current) {
                    current->setSelected(true);
                    m_activeList->setCurrentItem(
                        current, 0, QItemSelectionModel::NoUpdate);
                }
            }
        }
    }
    m_activeList->setUpdatesEnabled(updatesWereEnabled);
    if (updatesWereEnabled && m_activeList->viewport()) {
        m_activeList->viewport()->update();
    }

    rebuildVisibleSignals();
    refreshActiveValueLabels();
    if (m_canvas) {
        syncCanvasSelectionFromActiveList(m_canvas, m_activeList);
    }
}

void MainWindow::rebuildActiveListRows() {
    for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(i);
        const int signalIndex = signalIndexFromActiveItem(item);
        if (signalIndex < 0 || signalIndex >= m_wave.signalList.size()) continue;
        const WaveSignal& sig = m_wave.signalList.at(signalIndex);
        item->setText(0, formatNameWithRange(signalIndex));
    }
}

void MainWindow::rebuildVisibleSignals() {
    QVector<ActiveSignalRef> entries;
    for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(i);
        const int signalIndex = signalIndexFromActiveItem(item);
        if (signalIndex >= 0) {
            ActiveSignalRef ref;
            ref.signalIndex = signalIndex;
            ref.format = formatFromActiveItem(item);
            entries.push_back(ref);
        }
    }
    m_canvas->setVisibleEntries(entries);
    syncActiveScrollToCanvas();
    rebuildActiveListRows();
}

void MainWindow::syncActiveScrollToCanvas() {
    if (!m_canvas || !m_activeList) return;

    const int total = m_activeList->topLevelItemCount();
    if (total <= 0) {
        m_canvas->setVisibleEntryWindow(0, 0);
        return;
    }

    QScrollBar* bar = m_activeList->verticalScrollBar();
    int firstVisible = bar ? bar->value() : 0;
    firstVisible = qBound(0, firstVisible, total - 1);

    const int viewportH = m_activeList->viewport() ? m_activeList->viewport()->height() : 0;
    int rowH = m_activeList->sizeHintForRow(firstVisible);
    if (rowH <= 0) rowH = 40;

    // ActiveSignalListWidget uses uniform row heights and ScrollPerItem.  Walking
    // every top-level item and calling visualItemRect() on scroll/selection is O(N)
    // and becomes a visible stall when thousands of signals are active.  The
    // scrollbar value is the first visible row in this mode; draw one extra row for
    // the partially visible bottom edge.
    const int visibleCount = qMin(total - firstVisible, qMax(1, (viewportH + rowH - 1) / rowH + 1));
    m_canvas->setVisibleEntryWindow(firstVisible, visibleCount);
}

void MainWindow::clampWindowToAvailableScreen() {
    QScreen* screen = nullptr;
    if (QWindow* handle = windowHandle()) screen = handle->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;

    const QRect available = screen->availableGeometry();
    const int margin = 24;
    const int maxW = qMax(320, available.width() - margin);
    const int maxH = qMax(240, available.height() - margin);

    // Keep the historical preferred size on normal screens, but avoid an
    // impossible minimum size on smaller displays.
    setMinimumSize(qMin(1120, maxW), qMin(700, maxH));

    const int targetW = qMin(width(), maxW);
    const int targetH = qMin(height(), maxH);
    resize(targetW, targetH);

    const int maxX = available.right() - width() + 1;
    const int maxY = available.bottom() - height() + 1;
    const int centeredX = available.left() + (available.width() - width()) / 2;
    const int centeredY = available.top() + (available.height() - height()) / 2;
    const int x = (maxX < available.left()) ? available.left() : qBound(available.left(), centeredX, maxX);
    const int y = (maxY < available.top()) ? available.top() : qBound(available.top(), centeredY, maxY);
    move(x, y);
}

void MainWindow::scheduleRefreshActiveValueLabels(int delayMs) {
    if (!m_activeValueRefreshTimer) {
        refreshActiveValueLabels();
        return;
    }
    m_activeValueRefreshTimer->start(qMax(0, delayMs));
}

void MainWindow::refreshActiveValueLabels() {
    if (!m_activeList || !m_canvas) return;
    const int total = m_activeList->topLevelItemCount();
    if (total <= 0) return;

    QScrollBar* bar = m_activeList->verticalScrollBar();
    int first = bar ? bar->value() : 0;
    first = qBound(0, first, total - 1);

    const int viewportH = m_activeList->viewport() ? m_activeList->viewport()->height() : 0;
    int rowH = m_activeList->sizeHintForRow(first);
    if (rowH <= 0) rowH = 40;
    const int count = qMin(total - first, qMax(1, (viewportH + rowH - 1) / rowH + 1));
    const int end = qMin(total, first + count);

    // The value column is an exact probe, not an overview.  LOD is suitable
    // for drawing but may deliberately omit transitions, so it must never be
    // used to answer the value at the cursor.  Fetch a small aligned RAW
    // window for the visible rows before formatting their values.  Alignment
    // keeps mouse movement inside the same window from causing repeated I/O.
    const qint64 cursor = m_canvas->cursorTime();
    if (cursor >= 0 && m_currentWaveSupportsOnDemand) {
        QList<int> missingRawIndexes;
        QSet<int> seenIndexes;
        const qint64 pointEnd = cursor < (std::numeric_limits<qint64>::max)()
            ? cursor + 1
            : cursor;
        for (int i = first; i < end; ++i) {
            QTreeWidgetItem* item = m_activeList->topLevelItem(i);
            if (!item) continue;
            const int signalIndex = signalIndexFromActiveItem(item);
            if (signalIndex < 0 || signalIndex >= m_wave.signalList.size() || seenIndexes.contains(signalIndex)) continue;
            seenIndexes.insert(signalIndex);
            if (!waveSignalRawSamplesCoverRange(m_wave.signalList.at(signalIndex), cursor, pointEnd)) {
                missingRawIndexes.push_back(signalIndex);
            }
        }

        if (!missingRawIndexes.isEmpty()) {
            static const qint64 kCursorRawWindowSpan = 64 * 1024;
            qint64 requestedStart = cursor >= 0
                ? (cursor / kCursorRawWindowSpan) * kCursorRawWindowSpan
                : cursor;
            requestedStart = qMax(m_wave.meta.start, requestedStart);
            qint64 requestedEnd = requestedStart <= (std::numeric_limits<qint64>::max)() - kCursorRawWindowSpan
                ? requestedStart + kCursorRawWindowSpan
                : (std::numeric_limits<qint64>::max)();
            requestedEnd = qMin(requestedEnd, qMax(m_wave.meta.end, pointEnd));
            if (requestedEnd <= requestedStart) requestedEnd = pointEnd;
            ensureSignalSamplesLoaded(missingRawIndexes, false, true, true,
                                      requestedStart, requestedEnd);
        }
    }

    for (int i = first; i < end; ++i) {
        QTreeWidgetItem* item = m_activeList->topLevelItem(i);
        if (!item) continue;
        ActiveSignalRef ref;
        ref.signalIndex = signalIndexFromActiveItem(item);
        ref.format = formatFromActiveItem(item);
        item->setText(1, m_canvas->formattedValueAtCursor(ref));
    }
}

void MainWindow::onAddSelectedFromTree() {
    QSet<int> seen;
    QList<int> signalIndexes;
    if (!m_tree || !m_tree->selectionModel()) {
        addSignalIndexesToActive(signalIndexes);
        return;
    }

    const QModelIndexList picked = m_tree->selectionModel()->selectedRows(0);
    for (const QModelIndex& index : picked) {
        const QVariant nodeValue = index.data(kTreeRoleNodeId);
        if (!nodeValue.isValid()) continue;
        collectSignalIndexesFromLogicNode(nodeValue.toInt(), seen, signalIndexes);
    }
    addSignalIndexesToActive(signalIndexes);
}

void MainWindow::onRemoveSelectedActive() {
    QList<int> rows;
    if (m_activeList && m_activeList->selectionModel()) {
        const QModelIndexList picked =
            m_activeList->selectionModel()->selectedRows(0);
        rows.reserve(picked.size());
        for (const QModelIndex& index : picked) {
            if (index.isValid() && index.row() >= 0) rows.push_back(index.row());
        }
    }
    if (rows.isEmpty() && m_activeList && m_activeList->currentIndex().isValid()) {
        rows.push_back(m_activeList->currentIndex().row());
    }
    removeActiveRows(rows);
}

void MainWindow::onClearActive() {
    m_activeList->clear();
    m_canvas->setSelectedEntryIndex(-1);
    rebuildVisibleSignals();
    refreshActiveValueLabels();
}

void MainWindow::onActiveCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem*) {
    const int row = current ? m_activeList->indexOfTopLevelItem(current) : -1;
    if (row >= 0) {
        if (allTopLevelRowsSelected(m_activeList)) {
            m_canvas->setAllEntriesSelected();
        } else {
            QSet<int> indexes = selectedTopLevelIndexes(m_activeList);
            if (indexes.isEmpty()) indexes.insert(row);
            m_canvas->setSelectedEntryIndexes(indexes);
        }
    }
    else {
        m_canvas->setSelectedEntryIndexes(QSet<int>());
    }
}


void MainWindow::onCursorMoved(qint64) {
    scheduleRefreshActiveValueLabels(35);
}

void MainWindow::onHoverMoved(qint64) {
}

void MainWindow::scheduleViewportDataLoad(qint64 start, qint64 end) {
    if (!m_currentWaveSupportsOnDemand || !m_viewportLoadTimer) return;
    m_pendingViewportStart = start;
    m_pendingViewportEnd = end;
    m_viewportLoadPending = true;
    ++m_viewportLoadSerial;
    m_guardedViewportCommitPending = false;
    m_guardedViewportCommitSerial = 0;
    m_deferredViewportApply = std::function<void()>();
    m_deferredViewportApplySerial = 0;
    m_deferredViewportBucketCycles = 1;
    // Restarting this timer coalesces all animation/drag frames into the latest
    // settled viewport. Decoding never runs from the animation callback.
    m_viewportLoadTimer->start(45);
}

void MainWindow::scheduleAnimationTargetDataLoad(qint64 start, qint64 end) {
    if (!m_currentWaveSupportsOnDemand || !m_viewportLoadTimer) return;
    m_animationTargetStart = start;
    m_animationTargetEnd = end;
    m_animationTargetLoadScheduled = true;
    m_pendingViewportStart = start;
    m_pendingViewportEnd = end;
    m_viewportLoadPending = true;
    ++m_viewportLoadSerial;
    m_guardedViewportCommitPending = false;
    m_guardedViewportCommitSerial = 0;
    // A newer zoom target makes an earlier decoded-but-not-yet-presented frame
    // obsolete. Never flash that stale level during the next animation.
    m_deferredViewportApply = std::function<void()>();
    m_deferredViewportApplySerial = 0;
    m_deferredViewportBucketCycles = 1;
    // Start immediately: the animation itself is the debounce interval.  The
    // final LOD should already be present before the canvas reaches its target.
    m_viewportLoadTimer->start(0);
}

void MainWindow::onViewportRangeSelected(qint64 start, qint64 end) {
    if (!m_canvas || end <= start) return;
    if (!m_currentWaveSupportsOnDemand || !m_blockCacheLoader ||
        !m_viewportLoadTimer || !m_activeList || m_activeList->topLevelItemCount() == 0) {
        m_canvas->commitViewportRange(start, end);
        return;
    }

    // A range selection can jump across every LOD level in one frame. Keep the
    // old, valid viewport visible while the final target is decoded; commit the
    // new viewport only after its exact LOD/RAW data has been installed.
    m_animationTargetLoadScheduled = false;
    m_guardedViewportCommitPending = true;
    m_guardedViewportCommitStart = start;
    m_guardedViewportCommitEnd = end;
    m_pendingViewportStart = start;
    m_pendingViewportEnd = end;
    m_viewportLoadPending = true;
    ++m_viewportLoadSerial;
    m_guardedViewportCommitSerial = m_viewportLoadSerial;
    m_deferredViewportApply = std::function<void()>();
    m_deferredViewportApplySerial = 0;
    m_deferredViewportBucketCycles = 1;
    statusBar()->showMessage(QStringLiteral("Loading selected waveform range..."));
    m_viewportLoadTimer->start(0);
}

void MainWindow::completeGuardedViewportCommit(bool success) {
    if (!m_guardedViewportCommitPending) return;
    if (m_guardedViewportCommitSerial != m_viewportLoadSerial) {
        m_guardedViewportCommitPending = false;
        m_guardedViewportCommitSerial = 0;
        return;
    }
    const qint64 start = m_guardedViewportCommitStart;
    const qint64 end = m_guardedViewportCommitEnd;
    m_guardedViewportCommitPending = false;
    m_guardedViewportCommitSerial = 0;
    if (!success || !m_canvas) return;
    statusBar()->clearMessage();
    m_canvas->commitViewportRange(start, end);
}

void MainWindow::startPendingViewportDataLoad() {
    if (!m_viewportLoadPending ||
        (!m_blockCacheLoader && m_viewportLoadInFlight) || !m_waveReader ||
        !m_waveReaderMutex || !m_activeList || !m_canvas) {
        return;
    }
    if (m_canvas->viewportDragActive()) {
        m_viewportLoadTimer->start(45);
        return;
    }

    const qint64 viewStart = m_pendingViewportStart;
    const qint64 viewEnd = m_pendingViewportEnd;
    const qint64 viewSpan = qMax<qint64>(1, viewEnd - viewStart);
    const qint64 loadStart = (viewStart > std::numeric_limits<qint64>::min() + viewSpan)
        ? qMax(m_wave.meta.start, viewStart - viewSpan) : m_wave.meta.start;
    const qint64 loadEnd = (viewEnd < std::numeric_limits<qint64>::max() - viewSpan)
        ? qMin(m_wave.meta.end, viewEnd + viewSpan) : m_wave.meta.end;
    const int plotWidth = qMax(1, m_canvas->width() - 20);
    const double cyclesPerPixel = double(viewSpan) / double(plotWidth);

    QVector<int> activeIndexes;
    activeIndexes.reserve(m_activeList->topLevelItemCount());
    for (int i = 0; i < m_activeList->topLevelItemCount(); ++i) {
        const int signalIndex = signalIndexFromActiveItem(m_activeList->topLevelItem(i));
        if (signalIndex >= 0 && signalIndex < m_wave.signalList.size()) activeIndexes.push_back(signalIndex);
    }

    if (m_blockCacheLoader) {
        QVector<int> signalIds;
        signalIds.reserve(activeIndexes.size());
        QSet<int> seenIds;
        const qint64 targetBucket = viewerDisableLodEnabled()
            ? 1
            : qMax<qint64>(1, qint64(std::floor(cyclesPerPixel)));
        const qint64 desiredBucket = m_blockCacheLoader->preferredBucketCycles(targetBucket);
        for (int signalIndex : activeIndexes) {
            const WaveSignal& signal = m_wave.signalList.at(signalIndex);
            bool covered = false;
            if (desiredBucket == 1) {
                covered = waveSignalRawSamplesCoverRange(signal, viewStart, viewEnd);
            } else {
                for (const WaveLodLevel& level : signal.lodLevels) {
                    if (level.bucketCycles == desiredBucket &&
                        lodLevelLoadedForWindow(level, viewStart, viewEnd)) {
                        covered = true;
                        break;
                    }
                }
            }
            if (covered) continue;
            const int signalId = signal.signalId;
            if (signalId <= 0 || seenIds.contains(signalId)) continue;
            seenIds.insert(signalId);
            signalIds.push_back(signalId);
        }
        m_viewportLoadPending = false;
        if (signalIds.isEmpty()) {
            m_viewportLoadInFlight = false;
            completeGuardedViewportCommit(true);
            return;
        }

        m_viewportLoadInFlight = true;
        const quint64 generation = m_waveFileGeneration;
        const quint64 serial = m_viewportLoadSerial;
        QPointer<MainWindow> guard(this);
        m_blockCacheLoader->requestViewport(
            signalIds,
            viewStart,
            viewEnd,
            loadStart,
            qMax(loadStart + 1, loadEnd),
            targetBucket,
            serial,
            [guard, generation](WaveBlockCacheLoader::Result&& loaded) {
                std::shared_ptr<WaveBlockCacheLoader::Result> result(
                    new WaveBlockCacheLoader::Result(std::move(loaded)));
                QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, generation, result]() {
                    MainWindow* self = guard.data();
                    if (!self) return;
                    const bool current = generation == self->m_waveFileGeneration &&
                                         result->serial == self->m_viewportLoadSerial;
                    if (!current) return;
                    std::function<void()> applyResult = [guard, generation, result]() {
                        MainWindow* self = guard.data();
                        if (!self || generation != self->m_waveFileGeneration ||
                            result->serial != self->m_viewportLoadSerial) return;
                        QElapsedTimer cacheApplyTimer;
                        if (viewerPerfLogEnabled()) cacheApplyTimer.start();
                        self->m_viewportLoadInFlight = false;
                        if (!result->ok) {
                            self->statusBar()->showMessage(
                            result->error.isEmpty()
                                ? QStringLiteral("Unable to load viewport waveform cache blocks.")
                                : result->error,
                            2600);
                        } else if (result->lodLoad) {
                        for (WaveSignal& loadedSignal : result->wave.signalList) {
                            const int sid = loadedSignal.signalId;
                            if (sid <= 0 || sid >= self->m_signalIndexBySignalId.size()) continue;
                            const int targetIndex = self->m_signalIndexBySignalId.at(sid) - 1;
                            if (targetIndex < 0 || targetIndex >= self->m_wave.signalList.size()) continue;
                            // The block cache owns history outside this retained
                            // viewport. Keep only the assembled current level in
                            // the UI model; merging it repeatedly would re-sort an
                            // ever-growing vector on the GUI thread.
                            self->m_wave.signalList[targetIndex].lodLevels.swap(
                                loadedSignal.lodLevels);
                            if (!loadedSignal.samples.isEmpty()) {
                                WaveSignal& target = self->m_wave.signalList[targetIndex];
                                target.samples.swap(loadedSignal.samples);
                                target.rawLoadedRanges.swap(loadedSignal.rawLoadedRanges);
                                target.changeTimes.swap(loadedSignal.changeTimes);
                                target.changeTimesReady = loadedSignal.changeTimesReady;
                                target.samplesLoaded = false;
                                target.supportsZState = loadedSignal.supportsZState;
                            }
                        }
                        viewerPerfLog("viewport.cache.lod", result->elapsedMs,
                                      result->signalIds.size(), self->m_wave.tree.nodesById.size(),
                                      self->m_blockCacheLoader
                                          ? self->m_blockCacheLoader->cachedBlockCount() : 0);
                        } else {
                        QVector<int> changedIndexes;
                        for (WaveSignal& loadedSignal : result->wave.signalList) {
                            const int sid = loadedSignal.signalId;
                            if (sid <= 0 || sid >= self->m_signalIndexBySignalId.size()) continue;
                            const int targetIndex = self->m_signalIndexBySignalId.at(sid) - 1;
                            if (targetIndex < 0 || targetIndex >= self->m_wave.signalList.size()) continue;
                            WaveSignal& target = self->m_wave.signalList[targetIndex];
                            target.samples.swap(loadedSignal.samples);
                            target.rawLoadedRanges.swap(loadedSignal.rawLoadedRanges);
                            target.changeTimes.swap(loadedSignal.changeTimes);
                            target.changeTimesReady = loadedSignal.changeTimesReady;
                            target.samplesLoaded = false;
                            target.supportsZState = loadedSignal.supportsZState;
                            changedIndexes.push_back(targetIndex);
                        }
                        if (self->m_canvas) self->m_canvas->invalidateSignalSampleCaches(changedIndexes);
                        viewerPerfLog("viewport.cache.raw", result->elapsedMs,
                                      result->signalIds.size(), self->m_wave.tree.nodesById.size(),
                                      self->m_blockCacheLoader
                                          ? self->m_blockCacheLoader->cachedBlockCount() : 0);
                        }
                        self->rebuildVisibleSignals();
                        if (self->m_canvas) self->m_canvas->update();
                        if (self->m_blockCacheLoader) {
                            self->m_blockCacheLoader->releaseWaveLater(std::move(result->wave));
                        }
                        if (viewerPerfLogEnabled()) {
                            viewerPerfLog("viewport.cache.apply", cacheApplyTimer.elapsed(),
                                      result->signalIds.size(), self->m_wave.tree.nodesById.size(),
                                      result->lodLoad ? int(result->bucketCycles) : 1);
                        }
                        if (self->m_viewportLoadPending && self->m_viewportLoadTimer) {
                            self->m_viewportLoadTimer->start(0);
                        }
                    };
                    if (self->m_canvas && self->m_canvas->viewportAnimationActive()) {
                        self->m_deferredViewportApplySerial = result->serial;
                        self->m_deferredViewportBucketCycles = result->lodLoad
                            ? qMax<qint64>(1, result->bucketCycles) : 1;
                        self->m_deferredViewportApply = std::move(applyResult);
                        self->applyDeferredViewportResultIfReady(false);
                        return;
                    }
                    const bool loadSucceeded = result->ok;
                    applyResult();
                    self->completeGuardedViewportCommit(loadSucceeded);
                }, Qt::QueuedConnection);
            });
        return;
    }

    std::shared_ptr<ViewportLoadResult> result(new ViewportLoadResult);
    result->retainStart = loadStart;
    result->retainEnd = qMax(loadStart + 1, loadEnd);
    if (!viewerDisableLodEnabled() && cyclesPerPixel >= 10.0) {
        QSet<int> seen;
        for (int signalIndex : activeIndexes) {
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            if (sig.samplesLoaded || sig.signalId <= 0 || seen.contains(sig.signalId)) continue;
            if (signalHasLoadedLodForWindow(sig, viewStart, viewEnd, plotWidth)) continue;
            seen.insert(sig.signalId);
            result->requestedSignalIds.push_back(sig.signalId);
        }
        result->lodLoad = !result->requestedSignalIds.isEmpty();
    } else {
        QMap<QPair<qint64, qint64>, QVector<int>> idsByRange;
        QSet<int> seen;
        for (int signalIndex : activeIndexes) {
            const WaveSignal& sig = m_wave.signalList.at(signalIndex);
            if (sig.samplesLoaded || sig.signalId <= 0 || seen.contains(sig.signalId)) continue;
            seen.insert(sig.signalId);
            if (waveSignalRawSamplesCoverRange(sig, viewStart, viewEnd)) continue;
            result->requestedSignalIds.push_back(sig.signalId);
            const QVector<WaveLodValidRange> missing =
                missingRangesForWindow(sig.rawLoadedRanges, result->retainStart, result->retainEnd);
            for (const WaveLodValidRange& range : missing) {
                idsByRange[qMakePair(range.start, range.end)].push_back(sig.signalId);
            }
        }
        for (auto it = idsByRange.constBegin(); it != idsByRange.constEnd(); ++it) {
            ViewportRawLoadBatch batch;
            batch.start = it.key().first;
            batch.end = it.key().second;
            batch.signalIds = it.value();
            result->rawBatches.push_back(std::move(batch));
        }
        for (int sid : result->requestedSignalIds) {
            if (sid <= 0 || sid >= m_signalIndexBySignalId.size()) continue;
            const int targetIndex = m_signalIndexBySignalId.at(sid) - 1;
            if (targetIndex < 0 || targetIndex >= m_wave.signalList.size()) continue;
            result->preparedRawSignals.insert(sid, m_wave.signalList.at(targetIndex));
        }
    }

    m_viewportLoadPending = false;
    if (!result->lodLoad && result->rawBatches.isEmpty()) return;
    m_viewportLoadInFlight = true;
    const quint64 generation = m_waveFileGeneration;
    const quint64 serial = m_viewportLoadSerial;
    const qint64 lodBucket = qMax<qint64>(1, qint64(std::floor(cyclesPerPixel)));
    const std::shared_ptr<WaveParser4Reader> reader = m_waveReader;
    const std::shared_ptr<std::mutex> readerMutex = m_waveReaderMutex;
    QPointer<MainWindow> guard(this);
    if (m_viewportLoadThread.joinable()) m_viewportLoadThread.join();
    m_viewportLoadThread = std::thread([guard, reader, readerMutex, result, generation, serial, lodBucket]() {
        QElapsedTimer timer;
        timer.start();
        {
            std::lock_guard<std::mutex> readerLock(*readerMutex);
            if (result->lodLoad) {
                result->ok = reader->loadSignalLod(result->requestedSignalIds,
                                                   result->lodWave,
                                                   result->error,
                                                   result->retainStart,
                                                   result->retainEnd,
                                                   lodBucket);
            } else {
                for (ViewportRawLoadBatch& batch : result->rawBatches) {
                    if (!reader->loadSignals(batch.signalIds,
                                             batch.wave,
                                             result->error,
                                             kViewerOnDemandSampleBudget,
                                             batch.start,
                                             batch.end)) {
                        result->ok = false;
                        break;
                    }
                }
                if (result->ok) {
                    for (ViewportRawLoadBatch& batch : result->rawBatches) {
                        for (WaveSignal& loadedSig : batch.wave.signalList) {
                            auto targetIt = result->preparedRawSignals.find(loadedSig.signalId);
                            if (targetIt == result->preparedRawSignals.end()) continue;
                            mergeRawSamples(targetIt.value(), std::move(loadedSig.samples));
                            targetIt.value().supportsZState = loadedSig.supportsZState;
                        }
                        for (int sid : batch.signalIds) {
                            auto targetIt = result->preparedRawSignals.find(sid);
                            if (targetIt == result->preparedRawSignals.end()) continue;
                            WaveLodValidRange range;
                            range.start = batch.start;
                            range.end = batch.end;
                            targetIt.value().rawLoadedRanges.push_back(range);
                            compactLodRanges(targetIt.value().rawLoadedRanges);
                        }
                    }
                    for (auto it = result->preparedRawSignals.begin();
                         it != result->preparedRawSignals.end(); ++it) {
                        WaveSignal& target = it.value();
                        trimRawSamplesToWindow(target, result->retainStart, result->retainEnd);
                        QVector<WaveLodValidRange> retained;
                        for (const WaveLodValidRange& range : target.rawLoadedRanges) {
                            WaveLodValidRange clipped;
                            clipped.start = qMax(range.start, result->retainStart);
                            clipped.end = qMin(range.end, result->retainEnd);
                            if (clipped.end > clipped.start) retained.push_back(clipped);
                        }
                        target.rawLoadedRanges = std::move(retained);
                        compactLodRanges(target.rawLoadedRanges);
                        target.samplesLoaded = false;
                        rebuildWaveSignalDerivedCaches(target);
                    }
                }
            }
        }
        result->elapsedMs = timer.elapsed();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, result, generation, serial]() {
            MainWindow* self = guard.data();
            if (!self) return;
            self->m_viewportLoadInFlight = false;
            const bool current = generation == self->m_waveFileGeneration &&
                                 serial == self->m_viewportLoadSerial;
            if (current && result->ok) {
                if (result->lodLoad) {
                    for (WaveSignal& loadedSig : result->lodWave.signalList) {
                        const int sid = loadedSig.signalId;
                        if (sid <= 0 || sid >= self->m_signalIndexBySignalId.size()) continue;
                        const int targetIndex = self->m_signalIndexBySignalId.at(sid) - 1;
                        if (targetIndex < 0 || targetIndex >= self->m_wave.signalList.size()) continue;
                        QVector<WaveLodLevel>& levels = self->m_wave.signalList[targetIndex].lodLevels;
                        if (levels.size() < loadedSig.lodLevels.size()) levels.resize(loadedSig.lodLevels.size());
                        for (int i = 0; i < loadedSig.lodLevels.size(); ++i) {
                            mergeLodLevel(levels[i], std::move(loadedSig.lodLevels[i]));
                        }
                    }
                    viewerPerfLog("viewport.lod.worker", result->elapsedMs,
                                  result->requestedSignalIds.size(), self->m_wave.tree.nodesById.size());
                } else {
                    QVector<int> changedSignalIndexes;
                    changedSignalIndexes.reserve(result->preparedRawSignals.size());
                    for (auto it = result->preparedRawSignals.begin();
                         it != result->preparedRawSignals.end(); ++it) {
                        const int sid = it.key();
                        if (sid <= 0 || sid >= self->m_signalIndexBySignalId.size()) continue;
                        const int targetIndex = self->m_signalIndexBySignalId.at(sid) - 1;
                        if (targetIndex < 0 || targetIndex >= self->m_wave.signalList.size()) continue;
                        WaveSignal& target = self->m_wave.signalList[targetIndex];
                        WaveSignal& prepared = it.value();
                        target.samples = std::move(prepared.samples);
                        target.rawLoadedRanges = std::move(prepared.rawLoadedRanges);
                        target.changeTimes = std::move(prepared.changeTimes);
                        target.changeTimesReady = prepared.changeTimesReady;
                        target.samplesLoaded = false;
                        target.supportsZState = prepared.supportsZState;
                        changedSignalIndexes.push_back(targetIndex);
                    }
                    if (self->m_canvas) self->m_canvas->invalidateSignalSampleCaches(changedSignalIndexes);
                    viewerPerfLog("viewport.raw.worker", result->elapsedMs,
                                  result->requestedSignalIds.size(), self->m_wave.tree.nodesById.size(),
                                  result->rawBatches.size());
                }
                self->rebuildVisibleSignals();
                if (self->m_canvas) self->m_canvas->update();
            } else if (current && !result->ok) {
                self->statusBar()->showMessage(result->error.isEmpty()
                    ? QStringLiteral("Unable to load viewport waveform data.") : result->error, 2600);
            }
            if (self->m_viewportLoadPending && self->m_viewportLoadTimer) {
                self->m_viewportLoadTimer->start(0);
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onViewportChanged(qint64 start, qint64 end) {
    if (m_windowRangeStartEdit &&
        !(m_windowRangeStartEdit->hasFocus() && m_windowRangeStartEdit->isModified())) {
        m_windowRangeStartEdit->setText(formatInternalDisplayTime(start));
        m_windowRangeStartEdit->setModified(false);
    }
    if (m_windowRangeEndEdit &&
        !(m_windowRangeEndEdit->hasFocus() && m_windowRangeEndEdit->isModified())) {
        m_windowRangeEndEdit->setText(formatInternalDisplayTime(end));
        m_windowRangeEndEdit->setModified(false);
    }
    if (m_currentWaveSupportsOnDemand) {
        applyDeferredViewportResultIfReady(false);
        if (m_canvas && m_canvas->viewportAnimationActive()) {
            // The final target was scheduled by viewportTargetRequested.  Do
            // not replace it with dozens of intermediate animation frames.
        } else if (m_animationTargetLoadScheduled &&
                   start == m_animationTargetStart && end == m_animationTargetEnd) {
            // The settled notification corresponds to the target already in
            // flight (or already applied), so preserve its request serial.
            m_animationTargetLoadScheduled = false;
            applyDeferredViewportResultIfReady(true);
        } else {
            m_animationTargetLoadScheduled = false;
            scheduleViewportDataLoad(start, end);
        }
    }
    scheduleRefreshActiveValueLabels(35);
}

bool MainWindow::applyDeferredViewportResultIfReady(bool force) {
    if (!m_deferredViewportApply ||
        m_deferredViewportApplySerial != m_viewportLoadSerial) return false;
    if (!force) {
        if (!m_canvas || !m_blockCacheLoader) return false;
        const qint64 viewSpan = qMax<qint64>(1, m_canvas->viewEnd() - m_canvas->viewStart());
        const int plotWidth = qMax(1, m_canvas->width() - 20);
        const qint64 targetBucket = viewerDisableLodEnabled()
            ? 1
            : qMax<qint64>(1, qint64(std::floor(double(viewSpan) / double(plotWidth))));
        const qint64 desiredBucket = m_blockCacheLoader->preferredBucketCycles(targetBucket);
        if (desiredBucket != m_deferredViewportBucketCycles) return false;
    }
    std::function<void()> apply = std::move(m_deferredViewportApply);
    m_deferredViewportApply = std::function<void()>();
    m_deferredViewportApplySerial = 0;
    m_deferredViewportBucketCycles = 1;
    apply();
    return true;
}
