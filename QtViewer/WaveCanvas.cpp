#include "WaveCanvas.h"
#include "WaveTypes.h"

#include <QEvent>
#include <QEasingCurve>
#include <QMouseEvent>
#include <QBrush>
#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QtMath>
#include <QtGlobal>
#include <limits>
#include <cmath>
#include <algorithm>
#include <vector>

namespace {

    static inline QPoint mouseEventPosCompat(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return event->position().toPoint();
#else
        return event->pos();
#endif
    }

    static inline int mouseEventXCompat(const QMouseEvent* event) {
        return mouseEventPosCompat(event).x();
    }

    static inline int mouseEventYCompat(const QMouseEvent* event) {
        return mouseEventPosCompat(event).y();
    }

    static inline int wheelEventXCompat(const QWheelEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return int(std::lround(event->position().x()));
#else
        return event->pos().x();
#endif
    }

    QRectF busRoundedFrameRect(const QRect& rect) {
        QRectF r(rect);
        r.adjust(0.5, 0.5, -0.5, -0.5);
        if (r.width() < 1.0) r.setWidth(1.0);
        if (r.height() < 1.0) r.setHeight(1.0);
        return r;
    }

    qreal busRoundedFrameRadius(const QRectF& frame) {
        const qreal w = frame.width();
        const qreal h = frame.height();
        if (w <= 3.0 || h <= 3.0) return 0.0;

        return qMin<qreal>(3.0, qMin<qreal>(h * 0.18, w * 0.18));
    }

    struct BusFrame {
        QRect rect;
        bool drawLeftEdge = true;
        bool drawRightEdge = true;
    };

    void drawBusFrame(QPainter& p, const BusFrame& busFrame, const QColor& color,
                      qreal penWidth, const QBrush& brush = Qt::NoBrush) {
        if (busFrame.rect.isEmpty()) return;

        const QRectF frame = busRoundedFrameRect(busFrame.rect);
        const qreal radius = busRoundedFrameRadius(frame);
        const bool drawLeft = busFrame.drawLeftEdge;
        const bool drawRight = busFrame.drawRightEdge;

        p.setPen(QPen(color, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (drawLeft && drawRight) {
            p.setBrush(brush);
            p.drawRoundedRect(frame, radius, radius);
            return;
        }

        if (brush.style() != Qt::NoBrush) {
            p.fillRect(frame, brush);
        }

        p.setBrush(Qt::NoBrush);
        if (radius <= 0.0) {
            p.drawLine(QPointF(frame.left(), frame.top()), QPointF(frame.right(), frame.top()));
            p.drawLine(QPointF(frame.left(), frame.bottom()), QPointF(frame.right(), frame.bottom()));
            if (drawLeft) p.drawLine(QPointF(frame.left(), frame.top()), QPointF(frame.left(), frame.bottom()));
            if (drawRight) p.drawLine(QPointF(frame.right(), frame.top()), QPointF(frame.right(), frame.bottom()));
            return;
        }

        QPainterPath path;
        const qreal left = frame.left();
        const qreal right = frame.right();
        const qreal top = frame.top();
        const qreal bottom = frame.bottom();

        path.moveTo(left + (drawLeft ? radius : 0.0), top);
        path.lineTo(right - (drawRight ? radius : 0.0), top);
        path.moveTo(right - (drawRight ? radius : 0.0), bottom);
        path.lineTo(left + (drawLeft ? radius : 0.0), bottom);

        if (drawRight) {
            path.moveTo(right - radius, top);
            path.quadTo(right, top, right, top + radius);
            path.lineTo(right, bottom - radius);
            path.quadTo(right, bottom, right - radius, bottom);
        }
        if (drawLeft) {
            path.moveTo(left + radius, bottom);
            path.quadTo(left, bottom, left, bottom - radius);
            path.lineTo(left, top + radius);
            path.quadTo(left, top, left + radius, top);
        }
        p.drawPath(path);
    }

    void drawBusRoundedFrames(QPainter& p, const QVector<BusFrame>& frames, const QColor& color,
                              qreal penWidth, const QBrush& brush = Qt::NoBrush) {
        if (frames.isEmpty()) return;

        const bool oldAntialiasing = p.testRenderHint(QPainter::Antialiasing);
        const QPen oldPen = p.pen();
        const QBrush oldBrush = p.brush();
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const BusFrame& frame : frames) {
            drawBusFrame(p, frame, color, penWidth, brush);
        }
        p.setPen(oldPen);
        p.setBrush(oldBrush);
        p.setRenderHint(QPainter::Antialiasing, oldAntialiasing);
    }

    void drawBusRoundedFrame(QPainter& p, const QRect& rect, const QColor& color,
                             qreal penWidth, const QBrush& brush = Qt::NoBrush,
                             bool drawLeftEdge = true, bool drawRightEdge = true) {
        if (rect.isEmpty()) return;

        const bool oldAntialiasing = p.testRenderHint(QPainter::Antialiasing);
        const QPen oldPen = p.pen();
        const QBrush oldBrush = p.brush();
        p.setRenderHint(QPainter::Antialiasing, true);
        drawBusFrame(p, BusFrame{ rect, drawLeftEdge, drawRightEdge }, color, penWidth, brush);
        p.setPen(oldPen);
        p.setBrush(oldBrush);
        p.setRenderHint(QPainter::Antialiasing, oldAntialiasing);
    }

    QString normalizedTextValue(const QString& raw) {
        QString s = raw.trimmed();
        if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) return s.mid(2).toUpper();
        if (s.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) return s.mid(2).toLower();
        return s;
    }

    bool isAbsentStateText(const QString& raw) {
        return isWaveAbsentValue(raw.trimmed());
    }

    bool containsUnknownStateText(const QString& raw) {
        return isWaveZValue(raw);
    }

    bool isExplicitBinaryText(const QString& raw) {
        if (isWaveZValue(raw)) return false;
        const QString s = raw.trimmed();
        if (!s.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) return false;
        return waveIsBinaryDigitsText(s.mid(2));
    }

    bool isDecimalDigitsText(const QString& raw) {
        if (isWaveZValue(raw)) return false;
        return waveIsDecimalDigitsText(raw);
    }

    bool isHexText(const QString& raw) {
        if (isWaveZValue(raw)) return false;
        const QString s = raw.trimmed();
        if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
            return waveIsHexDigitsText(s.mid(2));
        }
        bool hasAlpha = false;
        return waveIsHexDigitsText(s, &hasAlpha) && hasAlpha;
    }

    QString explicitBinaryToHexText(const QString& raw, int width) {
        if (isWaveZValue(raw)) return QStringLiteral("Z");
        QString bits = normalizedTextValue(raw).toLower();
        if (bits.isEmpty()) bits = QStringLiteral("0");
        const int targetBits = qMax(qMax(1, width), bits.size());
        bits = bits.rightJustified(targetBits, QLatin1Char('0'));

        const int nibbleCount = qMax(1, (targetBits + 3) / 4);
        bits = bits.rightJustified(nibbleCount * 4, QLatin1Char('0'));
        QString out;
        out.reserve(nibbleCount);
        for (int i = 0; i < bits.size(); i += 4) {
            const QString nibble = bits.mid(i, 4);
            bool ok = false;
            const int v = nibble.toInt(&ok, 2);
            out.append(ok ? QString::number(v, 16).toUpper() : QStringLiteral("X"));
        }
        return out;
    }

    QString hexToBinaryText(const QString& raw, int width) {
        if (isWaveZValue(raw)) return QStringLiteral("Z");
        const QString text = normalizedTextValue(raw);
        QString out;
        out.reserve(text.size() * 4);
        for (QChar ch : text) {
            const QChar u = ch.toUpper();
            bool ok = false;
            const int v = QString(u).toInt(&ok, 16);
            out += ok ? QString::number(v, 2).rightJustified(4, QLatin1Char('0')) : QStringLiteral("0000");
        }
        if (width > 0 && out.size() > width) out = out.right(width);
        return out;
    }

    QChar classifyBitStateChar(const QString& raw) {
        const QString s = raw.trimmed();
        if (isWaveAbsentValue(s)) return QLatin1Char('a');
        if (isWaveZValue(s)) return QLatin1Char('z');
        const QString l = s.toLower();
        if (l == QStringLiteral("1")) return QLatin1Char('1');
        return QLatin1Char('0');
    }

    quint8 classifyBitStateCode(const QString& raw) {
        int b = 0;
        int e = raw.size();
        while (b < e && raw.at(b).isSpace()) ++b;
        while (e > b && raw.at(e - 1).isSpace()) --e;
        const QString trimmed = raw.mid(b, e - b);
        if (isWaveAbsentValue(trimmed)) return 8u;
        if (isWaveZValue(trimmed)) return 4u;
        if (e - b == 1 && raw.at(b) == QLatin1Char('1')) return 2u;
        return 1u;
    }

    bool sampleIsAbsentState(const WaveSample& sample) {
        if (sample.isAbsent) return true;
        if (sample.rawFieldsReady) return false;
        return isWaveAbsentValue(sample.value.trimmed());
    }

    bool sampleContainsUnknownState(const WaveSample& sample) {
        if (sample.isZ) return true;
        if (sample.rawFieldsReady) return false;
        return containsUnknownStateText(sample.value);
    }

    QChar classifyBitStateChar(const WaveSample& sample) {
        if (sampleIsAbsentState(sample)) return QLatin1Char('a');
        if (sampleContainsUnknownState(sample)) return QLatin1Char('z');
        if (sample.rawFieldsReady) return (sample.rawBits & 1ull) ? QLatin1Char('1') : QLatin1Char('0');
        return classifyBitStateChar(sample.value);
    }

    quint8 classifyBitStateCode(const WaveSample& sample) {
        if (sampleIsAbsentState(sample)) return 8u;
        if (sampleContainsUnknownState(sample)) return 4u;
        if (sample.rawFieldsReady) return (sample.rawBits & 1ull) ? 2u : 1u;
        return classifyBitStateCode(sample.value);
    }

    QColor stateColorForText(const QString& raw, const QColor& knownColor) {
        return isWaveZValue(raw) ? QColor(255, 72, 72) : knownColor;
    }

    QString pseudoDiv10Text(qint64 value) {
        const bool neg = value < 0;
        const qulonglong absValue = neg ? qulonglong(-(value + 1)) + 1ull : qulonglong(value);
        const qulonglong whole = absValue / 10ull;
        const qulonglong frac = absValue % 10ull;

        QString out = QString::number(whole);
        if (frac != 0ull) {
            out += QLatin1Char('.');
            out += QString::number(frac);
        }

        if (neg) out.prepend(QLatin1Char('-'));
        return out;
    }

    QString forceFloatingDecimalPoint(QString text) {
        text = text.trimmed();
        if (text.isEmpty()) return text;

        const QString lower = text.toLower();
        if (lower.contains(QStringLiteral("nan")) || lower.contains(QStringLiteral("inf"))) {
            return text;
        }

        const int ePosLower = text.indexOf(QLatin1Char('e'));
        const int ePosUpper = text.indexOf(QLatin1Char('E'));
        int ePos = -1;
        if (ePosLower >= 0 && ePosUpper >= 0) ePos = qMin(ePosLower, ePosUpper);
        else ePos = qMax(ePosLower, ePosUpper);

        const QString mantissa = (ePos >= 0) ? text.left(ePos) : text;
        if (mantissa.contains(QLatin1Char('.'))) return text;

        if (ePos >= 0) {
            text.insert(ePos, QStringLiteral(".0"));
        } else {
            text += QStringLiteral(".0");
        }
        return text;
    }

    int targetLodSamplesForPlotWidth(int plotWidth) {
        return qMax(64, plotWidth / 2);
    }

    const WaveLodLevel* chooseLodLevelForViewport(const WaveSignal& sig, qint64 spanValue, int plotWidth) {
        const double cyclesPerPixel = double(spanValue) / double(qMax(1, plotWidth));
        if (cyclesPerPixel < 128.0 || sig.lodLevels.isEmpty()) return nullptr;
        const WaveLodLevel* first = nullptr;
        const WaveLodLevel* best = nullptr;

        const int targetSamples = targetLodSamplesForPlotWidth(plotWidth);
        const qint64 maxBucketCycles = qMax<qint64>(1, qint64(std::ceil(double(spanValue) / double(targetSamples))));
        for (int i = 0; i < sig.lodLevels.size(); ++i) {
            const WaveLodLevel& level = sig.lodLevels.at(i);
            if (level.bucketCycles <= 0 || (level.buckets.isEmpty() && level.samples.isEmpty())) continue;
            if (!first) first = &level;
            if (level.bucketCycles <= maxBucketCycles) best = &level;
            else break;
        }
        return best ? best : first;
    }

    int lowerSampleIndexForTime(const QVector<WaveSample>& samples, qint64 t) {
        int lo = 0;
        int hi = samples.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (samples.at(mid).time < t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    QVector<int> collectTimedSampleIndicesForWindow(const QVector<WaveSample>& samples,
                                                    qint64 start,
                                                    qint64 end,
                                                    int targetSamples) {
        QVector<int> out;
        if (samples.isEmpty()) return out;

        const int firstIdx = qMax(0, lowerSampleIndexForTime(samples, start) - 1);
        const int lastSearchExclusive = (end == std::numeric_limits<qint64>::max())
            ? samples.size()
            : lowerSampleIndexForTime(samples, end + 1);
        int lastExclusive = lastSearchExclusive;
        if (lastExclusive <= firstIdx) lastExclusive = qMin(samples.size(), firstIdx + 1);

        const int visibleCount = lastExclusive - firstIdx;
        const int cappedTarget = qMax(1, targetSamples);
        if (visibleCount <= cappedTarget) {
            out.reserve(visibleCount);
            for (int i = firstIdx; i < lastExclusive; ++i) out.push_back(i);
            return out;
        }

        const qint64 desiredStride = qMax<qint64>(1, (end - start) / cappedTarget);
        out.reserve(cappedTarget + 4);
        int idx = firstIdx;
        while (idx < lastExclusive) {
            out.push_back(idx);
            if (idx >= lastExclusive - 1) break;
            const qint64 nextTime = samples.at(idx).time + desiredStride;
            int next = lowerSampleIndexForTime(samples, nextTime);
            if (next <= idx) next = idx + 1;
            if (next >= lastExclusive) break;
            idx = next;
        }
        if (out.isEmpty() || out.constLast() != lastExclusive - 1) out.push_back(lastExclusive - 1);
        return out;
    }

    int lowerLodBucketByEnd(const QVector<WaveLodBucket>& buckets, qint64 t) {
        int lo = 0;
        int hi = buckets.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (buckets.at(mid).end <= t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    WaveSample makeRawLodSample(quint64 rawBits) {
        WaveSample s;
        s.rawBits = rawBits;
        s.rawFieldsReady = true;
        return s;
    }

    quint8 lodStableMaskForRaw(quint64 rawBits) {
        return (rawBits & 1ull) ? kWaveLodSeenNonZero : kWaveLodSeenZero;
    }
}

WaveCanvas::WaveCanvas(QWidget* parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setAutoFillBackground(false);
    setMinimumWidth(420);

    m_viewAnim = new QVariantAnimation(this);
    m_viewAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_viewAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        const double k = value.toDouble();
        m_viewStart = static_cast<qint64>(std::llround(double(m_animFromStart) + double(m_animToStart - m_animFromStart) * k));
        m_viewEnd = static_cast<qint64>(std::llround(double(m_animFromEnd) + double(m_animToEnd - m_animFromEnd) * k));
        Q_EMIT viewportChanged(m_viewStart, m_viewEnd);
        update();
        });
}

QSize WaveCanvas::minimumSizeHint() const {
    // The active-signal list owns vertical scrolling. Do not let a large
    // number of selected signals increase the canvas minimum height,
    // otherwise the top-level window can be forced outside the screen.
    const int rows = 8;
    return QSize(420, m_timeHeaderHeight + rows * m_rowHeight + m_overviewHeight + 26);
}

qint64 WaveCanvas::fullStart() const { return m_wave ? m_wave->meta.start : 0; }
qint64 WaveCanvas::fullEnd() const { return m_wave ? m_wave->meta.end : 100; }
qint64 WaveCanvas::span() const { return qMax<qint64>(1, m_viewEnd - m_viewStart); }

qint64 WaveCanvas::fullStartTime() const { return fullStart(); }
qint64 WaveCanvas::fullEndTime() const { return fullEnd(); }

QRect WaveCanvas::overviewRect() const {
    return QRect(m_padX, height() - m_overviewHeight - 8, qMax(10, width() - 2 * m_padX), m_overviewHeight);
}

QRectF WaveCanvas::overviewThumbRect() const {
    const QRect r = overviewRect();
    const qint64 total = qMax<qint64>(1, fullEnd() - fullStart());
    const double leftRatio = double(m_viewStart - fullStart()) / double(total);
    const double rightRatio = double(m_viewEnd - fullStart()) / double(total);
    const double x1 = r.left() + leftRatio * r.width();
    const double x2 = r.left() + rightRatio * r.width();
    return QRectF(x1, r.top() + 3, qMax(14.0, x2 - x1), r.height() - 6);
}

void WaveCanvas::setWave(WaveFile* wave) {
    m_wave = wave;
    clearLookupCache();
    clearSamplingCache();
    resetView();
}

void WaveCanvas::setVisibleEntries(const QVector<ActiveSignalRef>& entries) {
    m_visibleEntries = entries;
    setVisibleEntryWindow(m_firstVisibleEntryIndex, m_visibleEntryCountLimit);
    // Sampling stride tables can be very large for long traces.  Build them
    // lazily from the paint path for rows that are actually visible instead of
    // precomputing every active signal whenever the active list changes.
    if (m_selectedEntryIndex >= m_visibleEntries.size()) m_selectedEntryIndex = m_visibleEntries.isEmpty() ? -1 : m_visibleEntries.size() - 1;
    QSet<int> filtered;
    for (QSet<int>::const_iterator it = m_selectedEntryIndexes.constBegin(); it != m_selectedEntryIndexes.constEnd(); ++it) {
        const int idx = *it;
        if (idx >= 0 && idx < m_visibleEntries.size()) filtered.insert(idx);
    }
    m_selectedEntryIndexes = filtered;
    updateGeometry();
    update();
}

void WaveCanvas::setFirstVisibleEntryIndex(int index) {
    setVisibleEntryWindow(index, m_visibleEntryCountLimit);
}

void WaveCanvas::setVisibleEntryWindow(int firstEntryIndex, int visibleEntryCount) {
    const int normalizedCount = (visibleEntryCount < 0) ? -1 : qMax(0, visibleEntryCount);
    const int countForClamp = (normalizedCount > 0) ? qMin(normalizedCount, m_visibleEntries.size()) : 1;
    const int maxFirst = m_visibleEntries.isEmpty() ? 0 : qMax(0, m_visibleEntries.size() - countForClamp);
    const int clampedFirst = m_visibleEntries.isEmpty() ? 0 : qBound(0, firstEntryIndex, maxFirst);
    if (m_firstVisibleEntryIndex == clampedFirst && m_visibleEntryCountLimit == normalizedCount) return;
    m_firstVisibleEntryIndex = clampedFirst;
    m_visibleEntryCountLimit = normalizedCount;
    update();
}

void WaveCanvas::setSelectedEntryIndex(int index) {
    const int clamped = (index < 0 || index >= m_visibleEntries.size()) ? -1 : index;
    if (m_selectedEntryIndex == clamped && m_selectedEntryIndexes.size() == (clamped >= 0 ? 1 : 0) && (clamped < 0 || m_selectedEntryIndexes.contains(clamped))) return;
    m_selectedEntryIndex = clamped;
    m_selectedEntryIndexes.clear();
    if (clamped >= 0) m_selectedEntryIndexes.insert(clamped);
    update();
}

void WaveCanvas::setSelectedEntryIndexes(const QSet<int>& indexes) {
    QSet<int> filtered;
    for (int idx : indexes) {
        if (idx >= 0 && idx < m_visibleEntries.size()) filtered.insert(idx);
    }
    m_selectedEntryIndexes = filtered;
    if (m_selectedEntryIndex >= 0 && !m_selectedEntryIndexes.contains(m_selectedEntryIndex)) {
        m_selectedEntryIndex = m_selectedEntryIndexes.isEmpty() ? -1 : *m_selectedEntryIndexes.constBegin();
    }
    else if (m_selectedEntryIndex < 0 && !m_selectedEntryIndexes.isEmpty()) {
        m_selectedEntryIndex = *m_selectedEntryIndexes.constBegin();
    }
    update();
}

void WaveCanvas::setViewportInstant(qint64 start, qint64 end) {
    m_viewAnim->stop();
    m_viewStart = start;
    m_viewEnd = end;
    Q_EMIT viewportChanged(m_viewStart, m_viewEnd);
    update();
}

void WaveCanvas::animateViewportTo(qint64 start, qint64 end, int durationMs) {
    m_viewAnim->stop();
    m_animFromStart = m_viewStart;
    m_animFromEnd = m_viewEnd;
    m_animToStart = start;
    m_animToEnd = end;
    m_viewAnim->setDuration(durationMs);
    m_viewAnim->setStartValue(0.0);
    m_viewAnim->setEndValue(1.0);
    m_viewAnim->start();
}

void WaveCanvas::resetView() {
    if (m_wave) setViewportInstant(m_wave->meta.start, m_wave->meta.end);
    else setViewportInstant(0, 100);
    m_cursorTime = -1;
    m_hoverTime = -1;
}

void WaveCanvas::clearLookupCache() {
    m_lastSampleIndexBySignal.clear();
}
void WaveCanvas::clearSamplingCache() {
    m_strideLevelsBySignal.clear();
    m_bitParityStrideLevelsBySignal.clear();
}

void WaveCanvas::ensureStrideLevelsForSignal(int signalIndex) const {
    if (!m_wave || signalIndex < 0 || signalIndex >= m_wave->signalList.size()) return;
    if (m_strideLevelsBySignal.contains(signalIndex)) return;

    const WaveSignal& sig = m_wave->signalList.at(signalIndex);
    const int n = sig.samples.size();
    QVector<SampleStrideLevel> levels;
    if (n <= 1) {
        m_strideLevelsBySignal.insert(signalIndex, levels);
        return;
    }

    const qint64 totalSpan = qMax<qint64>(1, m_wave->meta.end - m_wave->meta.start);
    const qint64 maxStride = qMax<qint64>(1, totalSpan / 2000);
    for (qint64 stride = 1; stride <= maxStride; ) {
        SampleStrideLevel level;
        level.stride = stride;
        level.nextIndex.resize(n);
        int j = 1;
        for (int i = 0; i < n; ++i) {
            if (j <= i) j = i + 1;
            const qint64 target = sig.samples.at(i).time + stride;
            while (j < n && sig.samples.at(j).time < target) ++j;
            level.nextIndex[i] = j;
        }
        levels.push_back(std::move(level));
        if (stride > maxStride / 10) break;
        stride *= 10;
    }
    m_strideLevelsBySignal.insert(signalIndex, levels);
}


void WaveCanvas::ensureBitParityStrideLevelsForSignal(int signalIndex) const {
    if (!m_wave || signalIndex < 0 || signalIndex >= m_wave->signalList.size()) return;
    if (m_bitParityStrideLevelsBySignal.contains(signalIndex)) return;

    const WaveSignal& sig = m_wave->signalList.at(signalIndex);
    const int n = sig.samples.size();
    QVector<BitParityStrideLevel> levels;
    if (n <= 1) {
        m_bitParityStrideLevelsBySignal.insert(signalIndex, levels);
        return;
    }

    const qint64 totalSpan = qMax<qint64>(1, m_wave->meta.end - m_wave->meta.start);
    const qint64 maxStride = qMax<qint64>(1, totalSpan / 2000);

    for (qint64 stride = 1; stride <= maxStride; ) {
        BitParityStrideLevel level;
        level.stride = stride;
        level.nextOddIndex.resize(n);
        level.nextEvenIndex.resize(n);

        int oddPtr = 1;
        int evenPtr = 0;

        for (int i = 0; i < n; ++i) {
            const qint64 target = sig.samples.at(i).time + stride;

            while (oddPtr < n && sig.samples.at(oddPtr).time < target) oddPtr += 2;
            while (evenPtr < n && sig.samples.at(evenPtr).time < target) evenPtr += 2;

            level.nextOddIndex[i] = oddPtr;
            level.nextEvenIndex[i] = evenPtr;
        }

        levels.push_back(std::move(level));
        if (stride > maxStride / 10) break;
        stride *= 10;
    }

    m_bitParityStrideLevelsBySignal.insert(signalIndex, levels);
}

QVector<int> WaveCanvas::collectBitAlternatingSampleIndicesForWindow(const WaveSignal& sig, int signalIndex, qint64 start, qint64 end, int targetSamples) const {
    QVector<int> out;
    if (sig.samples.isEmpty()) return out;

    auto lowerIndexForTimeLocal = [](const QVector<WaveSample>& samples, qint64 t) -> int {
        int lo = 0, hi = samples.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (samples.at(mid).time < t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    };

    const int firstIdx = qMax(0, lowerIndexForTimeLocal(sig.samples, start) - 1);
    const int lastSearchExclusive = (end == std::numeric_limits<qint64>::max())
        ? sig.samples.size()
        : lowerIndexForTimeLocal(sig.samples, end + 1);
    int lastExclusive = lastSearchExclusive;
    if (lastExclusive <= firstIdx) lastExclusive = qMin(sig.samples.size(), firstIdx + 1);

    const int visibleCount = lastExclusive - firstIdx;
    if (visibleCount <= qMax(1, targetSamples)) {
        out.reserve(visibleCount);
        for (int i = firstIdx; i < lastExclusive; ++i) out.push_back(i);
        return out;
    }

    ensureBitParityStrideLevelsForSignal(signalIndex);
    auto parityLevelIt = m_bitParityStrideLevelsBySignal.constFind(signalIndex);
    if (parityLevelIt == m_bitParityStrideLevelsBySignal.constEnd() || parityLevelIt.value().isEmpty()) {
        out.reserve(visibleCount);
        for (int i = firstIdx; i < lastExclusive; ++i) out.push_back(i);
        return out;
    }
    const QVector<BitParityStrideLevel>& levels = parityLevelIt.value();

    const qint64 desiredStride = qMax<qint64>(1, (end - start) / qMax(1, targetSamples));
    int bestLevel = 0;
    qint64 bestDiff = std::numeric_limits<qint64>::max();
    for (int i = 0; i < levels.size(); ++i) {
        const qint64 diff = qAbs(levels.at(i).stride - desiredStride);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestLevel = i;
        }
    }

    const auto& level = levels.at(bestLevel);

    auto firstIndexWithParity = [&](int parity) -> int {
        int idx = firstIdx;
        if ((idx & 1) != parity) ++idx;
        return idx < lastExclusive ? idx : -1;
    };

    int currEven = firstIndexWithParity(0);
    int currOdd = firstIndexWithParity(1);
    int turnParity = firstIdx & 1;
    int guard = 0;
    const int guardLimit = qMax(visibleCount + 4, targetSamples * 4 + 16);
    out.reserve(targetSamples + 16);

    while ((currEven >= 0 || currOdd >= 0) && guard < guardLimit) {
        int idx = (turnParity == 0) ? currEven : currOdd;
        if (idx >= 0 && idx < lastExclusive) {
            out.push_back(idx);
            if (turnParity == 0) {
                const int next = (idx >= 0 && idx < level.nextEvenIndex.size()) ? level.nextEvenIndex.at(idx) : lastExclusive;
                currEven = (next >= 0 && next < lastExclusive) ? next : -1;
            }
            else {
                const int next = (idx >= 0 && idx < level.nextOddIndex.size()) ? level.nextOddIndex.at(idx) : lastExclusive;
                currOdd = (next >= 0 && next < lastExclusive) ? next : -1;
            }
        }
        turnParity ^= 1;
        ++guard;
    }

    out.push_back(firstIdx);
    out.push_back(lastExclusive - 1);
    return out;
}

QVector<int> WaveCanvas::collectSampleIndicesForWindow(const WaveSignal& sig, int signalIndex, qint64 start, qint64 end, int targetSamples) const {
    if (sig.kind == SignalKind::Bit && sig.width == 1) {
        return collectBitAlternatingSampleIndicesForWindow(sig, signalIndex, start, end, targetSamples);
    }

    QVector<int> out;
    if (sig.samples.isEmpty()) return out;

    auto lowerIndexForTimeLocal = [](const QVector<WaveSample>& samples, qint64 t) -> int {
        int lo = 0, hi = samples.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (samples.at(mid).time < t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    };

    const int firstIdx = qMax(0, lowerIndexForTimeLocal(sig.samples, start) - 1);
    const int lastSearchExclusive = (end == std::numeric_limits<qint64>::max())
        ? sig.samples.size()
        : lowerIndexForTimeLocal(sig.samples, end + 1);
    int lastExclusive = lastSearchExclusive;
    if (lastExclusive <= firstIdx) lastExclusive = qMin(sig.samples.size(), firstIdx + 1);

    const int visibleCount = lastExclusive - firstIdx;
    if (visibleCount <= qMax(1, targetSamples)) {
        out.reserve(visibleCount);
        for (int i = firstIdx; i < lastExclusive; ++i) out.push_back(i);
        return out;
    }

    ensureStrideLevelsForSignal(signalIndex);
    auto levelIt = m_strideLevelsBySignal.constFind(signalIndex);
    if (levelIt == m_strideLevelsBySignal.constEnd() || levelIt.value().isEmpty()) {
        out.reserve(visibleCount);
        for (int i = firstIdx; i < lastExclusive; ++i) out.push_back(i);
        return out;
    }
    const QVector<SampleStrideLevel>& levels = levelIt.value();

    const qint64 desiredStride = qMax<qint64>(1, (end - start) / qMax(1, targetSamples));
    int bestLevel = 0;
    qint64 bestDiff = std::numeric_limits<qint64>::max();
    for (int i = 0; i < levels.size(); ++i) {
        const qint64 diff = qAbs(levels.at(i).stride - desiredStride);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestLevel = i;
        }
    }

    const auto& nextIndex = levels.at(bestLevel).nextIndex;
    out.reserve(targetSamples + 16);
    int idx = firstIdx;
    while (idx < lastExclusive) {
        out.push_back(idx);
        int next = (idx >= 0 && idx < nextIndex.size()) ? nextIndex.at(idx) : (idx + 1);
        if (next <= idx) next = idx + 1;
        if (next >= lastExclusive) break;
        idx = next;
    }
    if (out.isEmpty() || out.constLast() != lastExclusive - 1) out.push_back(lastExclusive - 1);
    return out;
}


void WaveCanvas::zoomByFactor(double factor) {
    const qint64 center = (m_cursorTime >= 0) ? m_cursorTime : ((m_viewStart + m_viewEnd) / 2);
    zoomAt(center, factor, true);
}

void WaveCanvas::panBy(qint64 deltaTime) {
    qint64 ns = m_viewStart + deltaTime;
    qint64 ne = m_viewEnd + deltaTime;
    const qint64 s = ne - ns;
    if (ns < fullStart()) {
        ne += fullStart() - ns;
        ns = fullStart();
    }
    if (ne > fullEnd()) {
        ns -= ne - fullEnd();
        ne = fullEnd();
    }
    if ((ne - ns) != s) ns = ne - s;
    animateViewportTo(ns, ne, 150);
}

QString WaveCanvas::formattedValueAtCursor(const ActiveSignalRef& entry) const {
    if (!m_wave || entry.signalIndex < 0 || entry.signalIndex >= m_wave->signalList.size() || m_cursorTime < 0) return "-";
    const WaveSignal& sig = m_wave->signalList.at(entry.signalIndex);
    return formatValue(sig, rawValueAtTime(entry.signalIndex, m_cursorTime), entry.format);
}

namespace {
    qint64 nearestOrdinaryChangeInSignal(const WaveSignal& sig, qint64 ref, bool forward) {
        if (sig.changeTimesReady) {
            if (sig.changeTimes.isEmpty()) return -1;
            if (forward) {
                const auto it = std::upper_bound(sig.changeTimes.constBegin(), sig.changeTimes.constEnd(), ref);
                return it == sig.changeTimes.constEnd() ? -1 : *it;
            }
            const auto it = std::lower_bound(sig.changeTimes.constBegin(), sig.changeTimes.constEnd(), ref);
            if (it == sig.changeTimes.constBegin()) return -1;
            return *(it - 1);
        }

        if (sig.samples.size() < 2) return -1;
        auto upper = std::upper_bound(sig.samples.begin(), sig.samples.end(), ref,
            [](qint64 value, const WaveSample& sample) { return value < sample.time; });
        const int pivot = int(upper - sig.samples.begin());

        if (forward) {
            for (int i = qMax(1, pivot); i < sig.samples.size(); ++i) {
                if (sig.samples.at(i).rawFieldsReady && sig.samples.at(i - 1).rawFieldsReady) {
                    if (waveSamplesEquivalent(sig.samples.at(i), sig.samples.at(i - 1))) continue;
                } else if (sig.samples.at(i).value == sig.samples.at(i - 1).value) {
                    continue;
                }
                const qint64 t = sig.samples.at(i).time;
                if (t > ref) return t;
            }
        } else {
            const int start = qMin(sig.samples.size() - 1, pivot - 1);
            for (int i = start; i >= 1; --i) {
                if (sig.samples.at(i).rawFieldsReady && sig.samples.at(i - 1).rawFieldsReady) {
                    if (waveSamplesEquivalent(sig.samples.at(i), sig.samples.at(i - 1))) continue;
                } else if (sig.samples.at(i).value == sig.samples.at(i - 1).value) {
                    continue;
                }
                const qint64 t = sig.samples.at(i).time;
                if (t < ref) return t;
            }
        }
        return -1;
    }

    int lowerDiffRegionByEnd(const QVector<WaveDiffRegion>& regions, qint64 t) {
        int lo = 0;
        int hi = regions.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (regions.at(mid).end <= t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    int upperDiffRegionByStart(const QVector<WaveDiffRegion>& regions, qint64 t) {
        int lo = 0;
        int hi = regions.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (regions.at(mid).start <= t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    qint64 nearestDiffRegionStartInSignal(const WaveSignal& sig, qint64 ref, bool forward) {
        const QVector<WaveDiffRegion>& regions = sig.diffRegions;
        if (regions.isEmpty()) return -1;
        if (forward) {
            const int i = upperDiffRegionByStart(regions, ref);
            return (i >= 0 && i < regions.size()) ? regions.at(i).start : -1;
        }
        const int i = upperDiffRegionByStart(regions, ref) - 1;
        return (i >= 0 && i < regions.size()) ? regions.at(i).start : -1;
    }
}

bool WaveCanvas::jumpToNearestChangeForSignals(const QList<int>& signalIndexes, bool forward, bool diffOnly) {
    if (!m_wave || signalIndexes.isEmpty()) return false;
    const qint64 ref = (m_cursorTime >= 0) ? m_cursorTime : ((m_viewStart + m_viewEnd) / 2);
    qint64 target = -1;

    for (int signalIndex : signalIndexes) {
        if (signalIndex < 0 || signalIndex >= m_wave->signalList.size()) continue;
        const WaveSignal& sig = m_wave->signalList.at(signalIndex);
        const qint64 t = diffOnly
            ? nearestDiffRegionStartInSignal(sig, ref, forward)
            : nearestOrdinaryChangeInSignal(sig, ref, forward);
        if (t < 0) continue;
        if (target < 0) target = t;
        else target = forward ? qMin(target, t) : qMax(target, t);
    }

    if (target < 0) return false;

    m_cursorTime = target;
    Q_EMIT cursorMoved(m_cursorTime);

    qint64 ns = target - span() / 2;
    qint64 ne = ns + span();
    if (ns < fullStart()) { ne += fullStart() - ns; ns = fullStart(); }
    if (ne > fullEnd()) { ns -= ne - fullEnd(); ne = fullEnd(); }
    animateViewportTo(ns, ne, 150);
    update();
    return true;
}

bool WaveCanvas::jumpToNearestChange(int signalIndex, bool forward) {
    return jumpToNearestChangeForSignals(QList<int>() << signalIndex, forward, false);
}

bool WaveCanvas::jumpToTime(qint64 internalTime) {
    if (!m_wave) return false;

    const qint64 fs = fullStart();
    const qint64 fe = fullEnd();
    if (fe < fs) return false;
    if (internalTime < fs || internalTime > fe) return false;

    const qint64 total = qMax<qint64>(m_minWindow, fe - fs);
    qint64 window = qBound<qint64>(m_minWindow, span(), total);
    if (window <= 0) window = m_minWindow;

    qint64 ns = internalTime - window / 2;
    qint64 ne = ns + window;

    if (ns < fs) {
        ne += fs - ns;
        ns = fs;
    }
    if (ne > fe) {
        ns -= ne - fe;
        ne = fe;
    }
    if (ns < fs) ns = fs;
    if (ne <= ns) ne = ns + m_minWindow;
    if (ne > fe) ne = fe;

    m_cursorTime = internalTime;
    Q_EMIT cursorMoved(m_cursorTime);
    animateViewportTo(ns, ne, 150);
    update();
    return true;
}

qint64 WaveCanvas::xToTime(double x) const {
    const double usable = width() - 2.0 * m_padX;
    if (usable <= 1.0) return m_viewStart;
    const double clamped = qBound<double>(m_padX, x, width() - m_padX);
    const double ratio = (clamped - m_padX) / usable;
    return m_viewStart + static_cast<qint64>(std::llround(ratio * double(span())));
}

double WaveCanvas::timeToX(qint64 t) const {
    const double usable = width() - 2.0 * m_padX;
    return m_padX + (double(t - m_viewStart) / double(span())) * usable;
}

qint64 WaveCanvas::overviewXToTime(double x) const {
    const QRect r = overviewRect();
    const qint64 total = qMax<qint64>(1, fullEnd() - fullStart());
    const double clamped = qBound<double>(r.left(), x, r.right());
    const double ratio = (clamped - r.left()) / qMax(1.0, double(r.width()));
    return fullStart() + static_cast<qint64>(std::llround(ratio * double(total)));
}

int WaveCanvas::visibleRowCapacity() const {
    const int mainBottom = overviewRect().top() - 8;
    const int available = mainBottom - m_timeHeaderHeight;
    if (available <= 0) return 0;
    // Include the partially visible bottom row. Using floor here makes the
    // canvas miss one row whenever the view height is not an exact multiple of
    // m_rowHeight, while QTreeWidget still shows that partial item.
    return qMax(0, (available + qMax(1, m_rowHeight) - 1) / qMax(1, m_rowHeight));
}

int WaveCanvas::effectiveVisibleRowCount() const {
    int count = visibleRowCapacity();
    if (m_visibleEntryCountLimit >= 0) count = qMin(count, m_visibleEntryCountLimit);
    const int remaining = qMax(0, m_visibleEntries.size() - m_firstVisibleEntryIndex);
    return qMax(0, qMin(count, remaining));
}

int WaveCanvas::rowAtY(double y) const {
    const int mainBottom = overviewRect().top() - 8;
    if (y < m_timeHeaderHeight || y >= mainBottom) return -1;
    const int visibleRow = int((y - m_timeHeaderHeight) / m_rowHeight);
    if (visibleRow < 0 || visibleRow >= effectiveVisibleRowCount()) return -1;
    const int absoluteRow = m_firstVisibleEntryIndex + visibleRow;
    if (absoluteRow < 0 || absoluteRow >= m_visibleEntries.size()) return -1;
    return absoluteRow;
}

int WaveCanvas::lastSampleIndexAtOrBefore(const WaveSignal& sig, int signalIndex, qint64 t) const {
    if (sig.samples.isEmpty()) return -1;

    int idx = m_lastSampleIndexBySignal.value(signalIndex, 0);
    idx = qBound(0, idx, sig.samples.size() - 1);
    const qint64 currentTime = sig.samples.at(idx).time;

    if (t >= currentTime) {
        int steps = 0;
        while (idx + 1 < sig.samples.size() && sig.samples.at(idx + 1).time <= t && steps < 64) {
            ++idx;
            ++steps;
        }
        if (idx + 1 >= sig.samples.size() || sig.samples.at(idx + 1).time > t) {
            m_lastSampleIndexBySignal.insert(signalIndex, idx);
            return idx;
        }
        auto beginIt = sig.samples.begin() + idx + 1;
        auto upper = std::upper_bound(beginIt, sig.samples.end(), t,
            [](qint64 value, const WaveSample& sample) { return value < sample.time; });
        idx = int(upper - sig.samples.begin()) - 1;
        idx = qMax(0, idx);
        m_lastSampleIndexBySignal.insert(signalIndex, idx);
        return idx;
    }

    int steps = 0;
    while (idx > 0 && sig.samples.at(idx).time > t && steps < 64) {
        --idx;
        ++steps;
    }
    if (idx == 0 || sig.samples.at(idx).time <= t) {
        m_lastSampleIndexBySignal.insert(signalIndex, idx);
        return idx;
    }

    auto upper = std::upper_bound(sig.samples.begin(), sig.samples.begin() + idx, t,
        [](qint64 value, const WaveSample& sample) { return value < sample.time; });
    idx = int(upper - sig.samples.begin()) - 1;
    idx = qMax(0, idx);
    m_lastSampleIndexBySignal.insert(signalIndex, idx);
    return idx;
}

WaveSample WaveCanvas::rawValueAtTime(int signalIndex, qint64 t) const {
    if (!m_wave || signalIndex < 0 || signalIndex >= m_wave->signalList.size()) { WaveSample s; s.isAbsent = true; s.value = waveAbsentValue(); return s; }
    const WaveSignal& sig = m_wave->signalList.at(signalIndex);
    if (sig.samples.isEmpty()) { WaveSample s; s.isAbsent = true; s.value = waveAbsentValue(); return s; }
    if (t < sig.samples.first().time) { WaveSample s; s.isAbsent = true; s.value = waveAbsentValue(); return s; }
    const int idx = lastSampleIndexAtOrBefore(sig, signalIndex, t);
    if (idx < 0) { WaveSample s; s.isAbsent = true; s.value = waveAbsentValue(); return s; }
    return sig.samples.at(idx);
}

quint64 WaveCanvas::parseRawBits(const QString& raw) const {
    return parseWaveRawBitsText(raw);
}

QString WaveCanvas::formatValue(const WaveSignal& sig, const WaveSample& sample, ValueRadix format) const {
    if (sample.isAbsent) return QStringLiteral("-");
    if (sample.isZ) return QStringLiteral("Z");
    const bool rawOnly = sample.rawFieldsReady;
    const bool canUseRawBitsForDisplay = sig.width <= 64;
    const quint64 rawBits = sample.rawBits & waveBitMaskForWidth(sig.width);

    if (rawOnly && canUseRawBitsForDisplay) {
        const quint64 bits = rawBits;
        switch (format) {
        case ValueRadix::Bin:
            return QString::number(bits, 2).rightJustified(qMax(1, sig.width), QLatin1Char('0'));
        case ValueRadix::Hex:
            return QString::number(bits, 16).toUpper().rightJustified(qMax(1, (sig.width + 3) / 4), QLatin1Char('0'));
        case ValueRadix::Int:
            if (sig.width == 32) return QString::number(static_cast<qint32>(bits & 0xFFFFFFFFu));
            return QString::number(static_cast<qint64>(bits));
        case ValueRadix::UInt:
        case ValueRadix::Dec:
            return QString::number(bits);
        case ValueRadix::Float:
            if (sig.width == 32) {
                union { quint32 u; float f; } conv;
                conv.u = static_cast<quint32>(bits & 0xFFFFFFFFu);
                return forceFloatingDecimalPoint(QString::number(static_cast<double>(conv.f), 'g', 8));
            }
            return QStringLiteral("N/A");
        case ValueRadix::Int64:
            if (sig.width != 64) return QStringLiteral("N/A");
            return QString::number(static_cast<qint64>(bits));
        case ValueRadix::UInt64:
            if (sig.width != 64) return QStringLiteral("N/A");
            return QString::number(bits);
        case ValueRadix::Double:
            if (sig.width != 64) return QStringLiteral("N/A");
            { union { quint64 u; double d; } conv; conv.u = bits; return forceFloatingDecimalPoint(QString::number(conv.d, 'g', 16)); }
        default:
            return QString::number(bits);
        }
    }

    if (isWaveAbsentValue(sample.value.trimmed())) return QStringLiteral("-");
    if (isWaveZValue(sample.value)) return QStringLiteral("Z");
    const QString trimmed = sample.value.trimmed();
    const QString normalized = normalizedTextValue(trimmed);

    const bool explicitBinary = isExplicitBinaryText(trimmed);
    const bool decimalDigits = isDecimalDigitsText(trimmed);
    const bool hexText = isHexText(trimmed);
    const bool numericText = explicitBinary || decimalDigits || hexText;
    const quint64 bits = numericText ? (parseRawBits(trimmed) & waveBitMaskForWidth(sig.width)) : 0ull;

    switch (format) {
    case ValueRadix::Bin:
        if (numericText && canUseRawBitsForDisplay) {
            return QString::number(bits, 2).rightJustified(qMax(1, sig.width), QLatin1Char('0'));
        }
        if (explicitBinary) {
            return normalized.toLower().rightJustified(qMax(1, sig.width), QLatin1Char('0'));
        }
        if (hexText) {
            return hexToBinaryText(trimmed, sig.width);
        }
        return normalized;
    case ValueRadix::Hex:
        if (numericText && canUseRawBitsForDisplay) {
            return QString::number(bits, 16).toUpper().rightJustified(qMax(1, (sig.width + 3) / 4), QLatin1Char('0'));
        }
        if (explicitBinary) {
            return explicitBinaryToHexText(trimmed, sig.width);
        }
        if (hexText) {
            const QString hexDigits = normalized.toUpper();
            return hexDigits.rightJustified(qMax(1, (sig.width + 3) / 4), QLatin1Char('0'));
        }
        return normalized.toUpper();
    case ValueRadix::Int: {
        if (!explicitBinary && !decimalDigits && !hexText) return normalized;
        if (sig.width == 32) return QString::number(static_cast<qint32>(bits & 0xFFFFFFFFu));
        return QString::number(static_cast<qint64>(bits));
    }
    case ValueRadix::UInt: {
        if (!explicitBinary && !decimalDigits && !hexText) return normalized;
        return QString::number(bits);
    }
    case ValueRadix::Float: {
        if (!explicitBinary && !decimalDigits && !hexText) return normalized;
        if (sig.width == 32) {
            union { quint32 u; float f; } conv;
            conv.u = static_cast<quint32>(bits & 0xFFFFFFFFu);
            return forceFloatingDecimalPoint(QString::number(static_cast<double>(conv.f), 'g', 8));
        }
        return QStringLiteral("N/A");
    }
    case ValueRadix::Int64: {
        if (!explicitBinary && !decimalDigits && !hexText) return normalized;
        if (sig.width != 64) return QStringLiteral("N/A");
        return QString::number(static_cast<qint64>(bits));
    }
    case ValueRadix::UInt64: {
        if (!explicitBinary && !decimalDigits && !hexText) return normalized;
        if (sig.width != 64) return QStringLiteral("N/A");
        return QString::number(bits);
    }
    case ValueRadix::Double: {
        if (!explicitBinary && !decimalDigits && !hexText) return normalized;
        if (sig.width != 64) return QStringLiteral("N/A");
        union { quint64 u; double d; } conv;
        conv.u = bits;
        return forceFloatingDecimalPoint(QString::number(conv.d, 'g', 16));
    }
    case ValueRadix::Dec:
    default:
        if (explicitBinary || hexText || decimalDigits) {
            return QString::number(bits);
        }
        return normalized;
    }
}

void WaveCanvas::zoomAt(qint64 centerTime, double factor, bool animated) {
    const qint64 total = qMax<qint64>(fullEnd() - fullStart(), m_minWindow);
    const qint64 nextSpan = qBound<qint64>(m_minWindow, (factor > 1.0) ?
        static_cast<qint64>(std::ceil(double(span()) * factor))
        : static_cast<qint64>(std::floor(double(span()) * factor))
        , total);
    const double leftRatio = double(centerTime - m_viewStart) / double(span());
    qint64 ns = centerTime - static_cast<qint64>(std::llround(leftRatio * double(nextSpan)));
    qint64 ne = ns + nextSpan;
    if (ns < fullStart()) {
        ne += fullStart() - ns;
        ns = fullStart();
    }
    if (ne > fullEnd()) {
        ns -= ne - fullEnd();
        ne = fullEnd();
    }
    if (animated) animateViewportTo(ns, ne, 130);
    else setViewportInstant(ns, ne);
}

void WaveCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.fillRect(rect(), QColor("#000000"));
    if (!m_wave) return;

    const int mainBottom = overviewRect().top() - 8;
    p.fillRect(QRect(0, 0, width(), m_timeHeaderHeight), QColor("#0B0B0B"));
    p.setPen(QColor("#2A2A2A"));
    p.drawLine(0, m_timeHeaderHeight, width(), m_timeHeaderHeight);

    const double rawStep = double(span()) / 9.0;
    const double mag = qPow(10.0, qFloor(qLn(qMax(rawStep, 1e-9)) / qLn(10.0)));
    double step = mag;
    const QVector<double> candidates{ 1.0, 2.0, 5.0, 10.0 };
    for (double c : candidates) {
        if (mag * c >= rawStep) { step = mag * c; break; }
    }

    const qint64 spanValue = qMax<qint64>(1, span());
    const qint64 usablePx = qMax<qint64>(1, width() - 2 * m_padX);
    auto fastX = [&](qint64 t) -> int {
        if (t <= m_viewStart) return m_padX;
        if (t >= m_viewEnd) return width() - m_padX;
        const qint64 dt = t - m_viewStart;
        return m_padX + int((dt * usablePx + spanValue / 2) / spanValue);
    };
    auto lowerIndexForTime = [](const QVector<WaveSample>& samples, qint64 t) -> int {
        int lo = 0, hi = samples.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (samples.at(mid).time < t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    };

    const double first = qCeil(m_viewStart / step) * step;
    for (double t = first; t <= m_viewEnd + 1e-9; t += step) {
        const int x = fastX(static_cast<qint64>(std::llround(t)));
        p.setPen(QPen(QColor(255, 255, 255, 45), 1, Qt::DashLine));
        p.drawLine(x, 0, x, mainBottom);
        p.setPen(QColor("#D8E6F6"));
        p.drawText(QPoint(x + 4, 26), pseudoDiv10Text(static_cast<qint64>(std::llround(t))));
    }

    const QColor waveGreen("#22C55E");
    const QColor waveGreenText("#E8FFE8");
    const QColor waveBox(0, 0, 0, 255);
    const QColor zColor("#FF4D4F");
    const QColor absentColor("#FF4D4F");
    const QColor selectedKnownColor = waveGreen.lighter(130);

    const int rowCapacity = effectiveVisibleRowCount();
    const int countForClamp = rowCapacity > 0 ? rowCapacity : 1;
    const int maxFirstEntryRow = m_visibleEntries.isEmpty() ? 0 : qMax(0, m_visibleEntries.size() - countForClamp);
    const int firstEntryRow = m_visibleEntries.isEmpty() ? 0 : qBound(0, m_firstVisibleEntryIndex, maxFirstEntryRow);
    const int lastEntryRowExclusive = qMin(m_visibleEntries.size(), firstEntryRow + rowCapacity);
    for (int absoluteRow = firstEntryRow; absoluteRow < lastEntryRowExclusive; ++absoluteRow) {
        const int row = absoluteRow - firstEntryRow;
        const ActiveSignalRef& entry = m_visibleEntries.at(absoluteRow);
        if (entry.signalIndex < 0 || entry.signalIndex >= m_wave->signalList.size()) continue;
        const WaveSignal& sig = m_wave->signalList.at(entry.signalIndex);
        const int yTop = m_timeHeaderHeight + row * m_rowHeight;
        const int yHigh = yTop + 6;
        const int yLow = yTop + m_rowHeight - 6;
        const int yMid = yTop + m_rowHeight / 2;

        p.fillRect(QRect(0, yTop, width(), m_rowHeight), (absoluteRow % 2 == 0) ? QColor("#000000") : QColor("#080808"));
        const bool isSelectedRow = m_selectedEntryIndexes.contains(absoluteRow) || (absoluteRow == m_selectedEntryIndex);
        if (isSelectedRow) {
            QLinearGradient selGrad(0, yTop, 0, yTop + m_rowHeight);
            selGrad.setColorAt(0.0, QColor(115, 160, 220, 88));
            selGrad.setColorAt(0.55, QColor(80, 120, 185, 74));
            selGrad.setColorAt(1.0, QColor(50, 82, 130, 62));
            p.fillRect(QRect(0, yTop, width(), m_rowHeight), selGrad);
            p.fillRect(QRect(0, yTop, 6, m_rowHeight), QColor(195, 223, 255, 170));
        }

        if (!sig.diffRegions.isEmpty()) {
            const QColor diffFill = isSelectedRow ? QColor(255, 64, 64, 82) : QColor(255, 48, 48, 70);
            p.setPen(Qt::NoPen);
            p.setBrush(diffFill);
            const int firstDiff = lowerDiffRegionByEnd(sig.diffRegions, m_viewStart);
            for (int d = firstDiff; d < sig.diffRegions.size(); ++d) {
                const WaveDiffRegion& region = sig.diffRegions.at(d);
                if (region.start >= m_viewEnd) break;
                const qint64 clippedStart = qMax(region.start, m_viewStart);
                const qint64 clippedEnd = qMin(region.end, m_viewEnd);
                if (clippedEnd <= clippedStart) continue;
                const int x1 = fastX(clippedStart);
                const int x2 = qMax(x1 + 1, fastX(clippedEnd));
                p.fillRect(QRect(x1, yTop + 2, qMax(1, x2 - x1), m_rowHeight - 4), diffFill);
            }
            p.setBrush(Qt::NoBrush);
        }

        p.setPen(QColor("#1A1A1A"));
        p.drawLine(0, yTop + m_rowHeight, width(), yTop + m_rowHeight);

        const int plotLeft = qMax(0, m_padX);
        const int plotRight = qMin(width() - 1, width() - m_padX);
        const int plotWidth = qMax(1, plotRight - plotLeft + 1);
        if (const WaveLodLevel* lodLevel = chooseLodLevelForViewport(sig, spanValue, plotWidth)) {
            if (!lodLevel->samples.isEmpty()) {
                const QVector<WaveSample>& lodSamples = lodLevel->samples;
                const QVector<int> drawSampleIndices = collectTimedSampleIndicesForWindow(
                    lodSamples, m_viewStart, m_viewEnd, targetLodSamplesForPlotWidth(plotWidth));

                if (sig.kind == SignalKind::Bit) {
                    QVector<QLine> knownSolid;
                    QVector<QLine> selectedSolid;
                    QVector<QLine> zDashed;
                    QVector<QLine> absentSolid;
                    knownSolid.reserve(drawSampleIndices.size() * 2);
                    selectedSolid.reserve(drawSampleIndices.size() * 2);
                    zDashed.reserve(drawSampleIndices.size() / 2 + 4);
                    absentSolid.reserve(drawSampleIndices.size() / 2 + 4);

                    for (int s = 0; s < drawSampleIndices.size(); ++s) {
                        const int i = drawSampleIndices.at(s);
                        const qint64 t0 = lodSamples.at(i).time;
                        const int nextIndex = (s + 1 < drawSampleIndices.size()) ? drawSampleIndices.at(s + 1) : (i + 1);
                        const qint64 t1 = (nextIndex < lodSamples.size()) ? lodSamples.at(nextIndex).time : fullEnd();
                        const qint64 segStart = qMax(t0, m_viewStart);
                        const qint64 segEnd = qMin(t1, m_viewEnd);
                        if (segEnd <= segStart) continue;

                        const WaveSample& curSample = lodSamples.at(i);
                        const QChar state = classifyBitStateChar(curSample);
                        const int y = (state == QLatin1Char('1')) ? yHigh : ((state == QLatin1Char('0')) ? yLow : yMid);
                        const bool isZ = (state == QLatin1Char('z'));
                        const bool isAbsent = (state == QLatin1Char('a'));
                        QVector<QLine>& target = isAbsent ? absentSolid : (isZ ? zDashed : (isSelectedRow ? selectedSolid : knownSolid));
                        const int x1 = fastX(segStart);
                        const int x2 = qMax(x1 + 1, fastX(segEnd));
                        target.push_back(QLine(x1, y, x2, y));

                        if (s > 0 && t0 >= m_viewStart && t0 <= m_viewEnd &&
                            !waveSamplesEquivalent(lodSamples.at(drawSampleIndices.at(s - 1)), curSample)) {
                            const QChar prevState = classifyBitStateChar(lodSamples.at(drawSampleIndices.at(s - 1)));
                            const int py = (prevState == QLatin1Char('1')) ? yHigh : ((prevState == QLatin1Char('0')) ? yLow : yMid);
                            const int xs = fastX(t0);
                            if (isAbsent || prevState == QLatin1Char('a')) absentSolid.push_back(QLine(xs, py, xs, y));
                            else if (isZ || prevState == QLatin1Char('z')) zDashed.push_back(QLine(xs, py, xs, y));
                            else if (isSelectedRow) selectedSolid.push_back(QLine(xs, py, xs, y));
                            else knownSolid.push_back(QLine(xs, py, xs, y));
                        }
                    }

                    if (!knownSolid.isEmpty()) {
                        p.setPen(QPen(waveGreen, isSelectedRow ? 2.8 : 2.2, Qt::SolidLine, Qt::SquareCap));
                        p.drawLines(knownSolid);
                    }
                    if (!selectedSolid.isEmpty()) {
                        p.setPen(QPen(selectedKnownColor, 2.8, Qt::SolidLine, Qt::SquareCap));
                        p.drawLines(selectedSolid);
                    }
                    if (!zDashed.isEmpty()) {
                        p.setPen(QPen(zColor, isSelectedRow ? 2.8 : 2.2, Qt::DashLine, Qt::SquareCap));
                        p.drawLines(zDashed);
                    }
                    if (!absentSolid.isEmpty()) {
                        p.setPen(QPen(absentColor, isSelectedRow ? 2.8 : 2.2, Qt::SolidLine, Qt::SquareCap));
                        p.drawLines(absentSolid);
                    }
                } else {
                    const int yBusTop = yTop + 8;
                    const int yBusBottom = yTop + m_rowHeight - 8;
                    const qreal penWidth = isSelectedRow ? 1.9 : 1.5;
                    QVector<BusFrame> knownFrames;
                    QVector<BusFrame> zFrames;
                    QVector<BusFrame> absentRects;
                    knownFrames.reserve(drawSampleIndices.size());
                    zFrames.reserve(drawSampleIndices.size() / 2 + 4);
                    absentRects.reserve(drawSampleIndices.size() / 4 + 4);

                    for (int s = 0; s < drawSampleIndices.size(); ++s) {
                        const int i = drawSampleIndices.at(s);
                        const qint64 t0 = lodSamples.at(i).time;
                        const int nextIndex = (s + 1 < drawSampleIndices.size()) ? drawSampleIndices.at(s + 1) : (i + 1);
                        const qint64 t1 = (nextIndex < lodSamples.size()) ? lodSamples.at(nextIndex).time : fullEnd();
                        const qint64 segStart = qMax(t0, m_viewStart);
                        const qint64 segEnd = qMin(qMax(t1, t0 + 1), m_viewEnd);
                        if (segEnd <= segStart) continue;

                        const WaveSample& curSample = lodSamples.at(i);
                        const bool isZ = sampleContainsUnknownState(curSample);
                        const bool isAbsent = sampleIsAbsentState(curSample);
                        const int x1 = fastX(segStart);
                        const int x2 = qMax(x1 + 1, fastX(segEnd));
                        const QRect frameRect(x1, yBusTop, qMax(1, x2 - x1), qMax(1, yBusBottom - yBusTop));
                        const BusFrame frame{ frameRect, t0 > m_viewStart, t1 < m_viewEnd };
                        if (isAbsent) {
                            absentRects.push_back(frame);
                        } else {
                            QVector<BusFrame>& target = isZ ? zFrames : knownFrames;
                            target.push_back(frame);
                            const QRect textRect(x1 + 6, yTop + 3, qMax(1, x2 - x1 - 12), m_rowHeight - 6);
                            if (textRect.width() > 48) {
                                p.setPen(isZ ? zColor.lighter(140) : waveGreenText);
                                p.drawText(textRect, Qt::AlignVCenter | Qt::AlignHCenter,
                                           formatValue(sig, curSample, entry.format));
                            }
                        }
                    }

                    drawBusRoundedFrames(p, knownFrames, isSelectedRow ? waveGreen.lighter(130) : waveGreen, penWidth);
                    drawBusRoundedFrames(p, zFrames, zColor, penWidth);
                    drawBusRoundedFrames(p, absentRects, absentColor, penWidth);
                }
                continue;
            }

            const QVector<WaveLodBucket>& buckets = lodLevel->buckets;
            int lodIndex = lowerLodBucketByEnd(buckets, m_viewStart);
            quint64 currentRaw = (lodIndex > 0) ? buckets.at(lodIndex - 1).lastRawBits : 0ull;
            qint64 cursor = m_viewStart;

            auto drawBitMaskRange = [&](quint8 mask, qint64 start, qint64 end) {
                if (end <= start || mask == 0u) return;
                const int x1 = fastX(start);
                const int x2 = qMax(x1 + 1, fastX(end));
                const int drawW = qMax(1, x2 - x1);
                const quint8 normalMask = quint8(mask & (kWaveLodSeenZero | kWaveLodSeenNonZero | kWaveLodSeenZ));
                if (mask & kWaveLodSeenAbsent) {
                    p.fillRect(QRect(x1, yMid, drawW, 1), absentColor);
                }
                if (normalMask == 0u) return;
                const QColor fill = (normalMask & kWaveLodSeenZ)
                    ? QColor(40, 90, 120, 220)
                    : (isSelectedRow ? waveGreen.lighter(130) : waveGreen);
                if (normalMask == kWaveLodSeenZero) {
                    p.fillRect(QRect(x1, yLow - 1, drawW, 3), fill);
                } else if (normalMask == kWaveLodSeenNonZero) {
                    p.fillRect(QRect(x1, yHigh - 1, drawW, 3), fill);
                } else if (normalMask == kWaveLodSeenZ) {
                    p.fillRect(QRect(x1, yMid - 1, drawW, 3), fill);
                } else {
                    int top = yMid - 1;
                    int bottom = yMid + 1;
                    if (normalMask & kWaveLodSeenNonZero) { top = qMin(top, yHigh - 1); bottom = qMax(bottom, yHigh + 1); }
                    if (normalMask & kWaveLodSeenZero) { top = qMin(top, yLow - 1); bottom = qMax(bottom, yLow + 1); }
                    if (normalMask & kWaveLodSeenZ) { top = qMin(top, yMid - 1); bottom = qMax(bottom, yMid + 1); }
                    p.fillRect(QRect(x1, top, drawW, qMax(3, bottom - top + 1)), fill);
                }
            };

            const int yBusTop = yTop + 8;
            const int yBusBottom = yTop + m_rowHeight - 8;
            auto drawBusStableRange = [&](quint64 rawBits, qint64 start, qint64 end) {
                if (end <= start) return;
                const int x1 = fastX(start);
                const int x2 = qMax(x1 + 1, fastX(end));
                const QColor color = isSelectedRow ? waveGreen.lighter(130) : waveGreen;
                drawBusRoundedFrame(p, QRect(x1, yBusTop, qMax(1, x2 - x1), qMax(1, yBusBottom - yBusTop)),
                                    color, isSelectedRow ? 1.9 : 1.5, Qt::NoBrush,
                                    start > m_viewStart, end < m_viewEnd);
                const QRect textRect(x1 + 6, yTop + 3, qMax(1, x2 - x1 - 12), m_rowHeight - 6);
                if (textRect.width() > 48) {
                    p.setPen(waveGreenText);
                    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignHCenter,
                               formatValue(sig, makeRawLodSample(rawBits), entry.format));
                }
            };
            auto drawBusActivityRange = [&](const WaveLodBucket& bucket, qint64 start, qint64 end) {
                if (end <= start) return;
                const int x1 = fastX(start);
                const int x2 = qMax(x1 + 1, fastX(end));
                const QColor fill = isSelectedRow ? QColor(68, 190, 118, 140) : QColor(34, 197, 94, 120);
                drawBusRoundedFrame(p, QRect(x1, yBusTop, qMax(1, x2 - x1), qMax(1, yBusBottom - yBusTop)),
                                    isSelectedRow ? waveGreen.lighter(130) : waveGreen,
                                    isSelectedRow ? 1.9 : 1.5, QBrush(fill),
                                    start > m_viewStart, end < m_viewEnd);
                const QRect textRect(x1 + 4, yTop + 3, qMax(1, x2 - x1 - 8), m_rowHeight - 6);
                if (textRect.width() > 76) {
                    const QString text = QStringLiteral("%1..%2")
                        .arg(formatValue(sig, makeRawLodSample(bucket.minRawBits), entry.format),
                             formatValue(sig, makeRawLodSample(bucket.maxRawBits), entry.format));
                    p.setPen(waveGreenText);
                    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignHCenter, text);
                }
            };

            auto drawStableRange = [&](quint64 rawBits, qint64 start, qint64 end) {
                if (sig.kind == SignalKind::Bit) drawBitMaskRange(lodStableMaskForRaw(rawBits), start, end);
                else drawBusStableRange(rawBits, start, end);
            };

            while (lodIndex < buckets.size() && cursor < m_viewEnd) {
                const WaveLodBucket& bucket = buckets.at(lodIndex);
                if (bucket.start >= m_viewEnd) break;
                if (bucket.start > cursor) {
                    drawStableRange(currentRaw, cursor, qMin(bucket.start, m_viewEnd));
                    cursor = qMin(bucket.start, m_viewEnd);
                }
                if (bucket.end > cursor) {
                    const qint64 segStart = qMax(cursor, qMax(bucket.start, m_viewStart));
                    const qint64 segEnd = qMin(bucket.end, m_viewEnd);
                    if (segEnd > segStart) {
                        if (sig.kind == SignalKind::Bit) {
                            quint8 mask = bucket.stateMask;
                            if (bucket.transitionCount == 0) mask = lodStableMaskForRaw(bucket.lastRawBits);
                            drawBitMaskRange(mask, segStart, segEnd);
                        } else {
                            const bool active = bucket.transitionCount > 0 ||
                                bucket.firstRawBits != bucket.lastRawBits ||
                                bucket.minRawBits != bucket.maxRawBits;
                            if (active) drawBusActivityRange(bucket, segStart, segEnd);
                            else drawBusStableRange(bucket.lastRawBits, segStart, segEnd);
                        }
                    }
                    cursor = qMax(cursor, qMin(bucket.end, m_viewEnd));
                }
                currentRaw = bucket.lastRawBits;
                ++lodIndex;
            }
            if (cursor < m_viewEnd) {
                drawStableRange(currentRaw, cursor, m_viewEnd);
            }
            continue;
        }

        if (sig.samples.isEmpty()) continue;

        const int firstIdx = qMax(0, lowerIndexForTime(sig.samples, m_viewStart) - 1);
        int lastExclusive = lowerIndexForTime(sig.samples, m_viewEnd + 1);
        if (lastExclusive <= firstIdx) lastExclusive = qMin(sig.samples.size(), firstIdx + 1);

        if (sig.kind == SignalKind::Bit) {
            const int visibleCount = lastExclusive - firstIdx;
            const bool highDensityBit = visibleCount > qMax(160, plotWidth * 2);

            if (highDensityBit) {
                const WaveSample* sampleData = sig.samples.constData();
                std::vector<int> lowDiff(plotWidth + 1, 0);
                std::vector<int> highDiff(plotWidth + 1, 0);
                std::vector<int> zDiff(plotWidth + 1, 0);
                std::vector<int> absentDiff(plotWidth + 1, 0);

                auto addBitRange = [&](quint8 mask, int x1, int x2) {
                    if (mask == 0u) return;
                    int clampedX1 = x1;
                    int clampedX2 = x2;
                    if (clampedX1 < plotLeft) clampedX1 = plotLeft;
                    if (clampedX1 > plotRight) clampedX1 = plotRight;
                    if (clampedX2 < plotLeft + 1) clampedX2 = plotLeft + 1;
                    if (clampedX2 > plotRight + 1) clampedX2 = plotRight + 1;
                    if (clampedX2 <= clampedX1) return;
                    const int a = clampedX1 - plotLeft;
                    const int b = clampedX2 - plotLeft;
                    if (mask & 0x1u) { ++lowDiff[a];  --lowDiff[b]; }
                    if (mask & 0x2u) { ++highDiff[a]; --highDiff[b]; }
                    if (mask & 0x4u) { ++zDiff[a];    --zDiff[b]; }
                    if (mask & 0x8u) { ++absentDiff[a]; --absentDiff[b]; }
                };

                const QVector<int> sampled = collectSampleIndicesForWindow(sig, entry.signalIndex, m_viewStart, m_viewEnd, 2000);
                quint8 prevMask = (firstIdx > 0) ? classifyBitStateCode(sampleData[firstIdx - 1]) : 0u;
                for (int s = 0; s < sampled.size(); ++s) {
                    const int i = sampled.at(s);
                    const WaveSample& ws = sampleData[i];
                    const qint64 t0 = ws.time;
                    qint64 t1 = m_viewEnd;
                    if (s + 1 < sampled.size()) t1 = sampleData[sampled.at(s + 1)].time;
                    else if (i + 1 < sig.samples.size()) t1 = sampleData[i + 1].time;
                    else t1 = fullEnd();
                    const qint64 segStart = qMax(t0, m_viewStart);
                    const qint64 segEnd = qMin(t1, m_viewEnd);
                    const quint8 curMask = classifyBitStateCode(ws);
                    if (segEnd > segStart) {
                        const int x1 = fastX(segStart);
                        const int x2 = qMax(x1 + 1, fastX(segEnd));
                        addBitRange(curMask, x1, x2);
                    }

                    if (s > 0 && t0 >= m_viewStart && t0 <= m_viewEnd) {
                        const int xs = fastX(t0);
                        addBitRange(quint8(prevMask | curMask), xs, xs + 1);
                    }
                    prevMask = curMask;
                }

                const QColor knownFill = isSelectedRow ? waveGreen.lighter(130) : waveGreen;
                const QColor zFill(40, 90, 120, 220);

                int lowAcc = 0;
                int highAcc = 0;
                int zAcc = 0;
                int absentAcc = 0;
                int runStart = -1;
                quint8 runMask = 0u;

                auto flushMaskRun = [&](int xStart, int xEnd, quint8 mask) {
                    if (mask == 0u || xEnd <= xStart) return;
                    const int drawX = plotLeft + xStart;
                    const int drawW = qMax(1, xEnd - xStart);
                    if (mask & 0x8u) {
                        p.fillRect(QRect(drawX, yMid, drawW, 1), absentColor);
                    }
                    const quint8 normalMask = quint8(mask & 0x7u);
                    if (normalMask == 0u) return;
                    const QColor fill = (normalMask & 0x4u) ? zFill : knownFill;
                    if (normalMask == 0x1u) {
                        p.fillRect(QRect(drawX, yLow - 1, drawW, 3), fill);
                    }
                    else if (normalMask == 0x2u) {
                        p.fillRect(QRect(drawX, yHigh - 1, drawW, 3), fill);
                    }
                    else if (normalMask == 0x4u) {
                        p.fillRect(QRect(drawX, yMid - 1, drawW, 3), fill);
                    }
                    else {
                        int top = yMid - 1;
                        int bottom = yMid + 1;
                        if (normalMask & 0x2u) { top = qMin(top, yHigh - 1); bottom = qMax(bottom, yHigh + 1); }
                        if (normalMask & 0x1u) { top = qMin(top, yLow - 1); bottom = qMax(bottom, yLow + 1); }
                        if (normalMask & 0x4u) { top = qMin(top, yMid - 1); bottom = qMax(bottom, yMid + 1); }
                        p.fillRect(QRect(drawX, top, drawW, qMax(3, bottom - top + 1)), fill);
                    }
                };

                for (int x = 0; x < plotWidth; ++x) {
                    lowAcc += lowDiff[x];
                    highAcc += highDiff[x];
                    zAcc += zDiff[x];
                    absentAcc += absentDiff[x];
                    quint8 mask = 0u;
                    if (lowAcc > 0) mask |= 0x1u;
                    if (highAcc > 0) mask |= 0x2u;
                    if (zAcc > 0) mask |= 0x4u;
                    if (absentAcc > 0) mask |= 0x8u;

                    if (x == 0) {
                        runMask = mask;
                        runStart = 0;
                        continue;
                    }
                    if (mask != runMask) {
                        flushMaskRun(runStart, x, runMask);
                        runStart = x;
                        runMask = mask;
                    }
                }
                flushMaskRun(runStart, plotWidth, runMask);
            }
            else {
                QVector<QLine> knownSolid;
                QVector<QLine> selectedSolid;
                QVector<QLine> zDashed;
                QVector<QLine> absentSolid;
                knownSolid.reserve((lastExclusive - firstIdx) * 2);
                selectedSolid.reserve((lastExclusive - firstIdx) * 2);
                zDashed.reserve((lastExclusive - firstIdx) / 2 + 4);
                absentSolid.reserve((lastExclusive - firstIdx) / 2 + 4);

                int lastKnownVerticalX = std::numeric_limits<int>::min();
                int lastSelectedVerticalX = std::numeric_limits<int>::min();
                int lastZVerticalX = std::numeric_limits<int>::min();
                int lastAbsentVerticalX = std::numeric_limits<int>::min();
                int knownVerticalIndex = -1;
                int selectedVerticalIndex = -1;
                int zVerticalIndex = -1;
                int absentVerticalIndex = -1;

                auto appendMergedVertical = [](QVector<QLine>& lines, int& lastX, int& lineIndex, int x, int y1, int y2) {
                    if (lines.isEmpty() || lineIndex < 0 || lastX != x) {
                        lines.push_back(QLine(x, qMin(y1, y2), x, qMax(y1, y2)));
                        lineIndex = lines.size() - 1;
                        lastX = x;
                    }
                    else {
                        QLine& ln = lines[lineIndex];
                        const int ny1 = qMin(qMin(ln.y1(), ln.y2()), qMin(y1, y2));
                        const int ny2 = qMax(qMax(ln.y1(), ln.y2()), qMax(y1, y2));
                        ln.setP1(QPoint(x, ny1));
                        ln.setP2(QPoint(x, ny2));
                    }
                };

                for (int i = firstIdx; i < lastExclusive; ++i) {
                    const qint64 t0 = sig.samples.at(i).time;
                    const qint64 t1 = (i + 1 < sig.samples.size()) ? sig.samples.at(i + 1).time : fullEnd();
                    const qint64 segStart = qMax(t0, m_viewStart);
                    const qint64 segEnd = qMin(t1, m_viewEnd);
                    if (segEnd <= segStart) continue;

                    const QChar state = classifyBitStateChar(sig.samples.at(i));
                    const int y = (state == QLatin1Char('1')) ? yHigh : ((state == QLatin1Char('0')) ? yLow : yMid);
                    const bool isZ = (state == QLatin1Char('z'));
                    const bool isAbsent = (state == QLatin1Char('a'));
                    QVector<QLine>& target = isAbsent ? absentSolid : (isZ ? zDashed : (isSelectedRow ? selectedSolid : knownSolid));

                    const int x1 = fastX(segStart);
                    const int x2 = fastX(segEnd);

                    if (i > 0 && t0 >= m_viewStart && t0 <= m_viewEnd) {
                        const QChar prevState = classifyBitStateChar(sig.samples.at(i - 1));
                        const int py = (prevState == QLatin1Char('1')) ? yHigh : ((prevState == QLatin1Char('0')) ? yLow : yMid);
                        if (py != y || prevState != state) {
                            const int xs = fastX(t0);
                            if (isAbsent || prevState == QLatin1Char('a')) {
                                appendMergedVertical(absentSolid, lastAbsentVerticalX, absentVerticalIndex, xs, py, y);
                            }
                            else if (isZ || prevState == QLatin1Char('z')) {
                                appendMergedVertical(zDashed, lastZVerticalX, zVerticalIndex, xs, py, y);
                            }
                            else if (isSelectedRow) {
                                appendMergedVertical(selectedSolid, lastSelectedVerticalX, selectedVerticalIndex, xs, py, y);
                            }
                            else {
                                appendMergedVertical(knownSolid, lastKnownVerticalX, knownVerticalIndex, xs, py, y);
                            }
                        }
                    }

                    if (x2 > x1) {
                        target.push_back(QLine(x1, y, x2, y));
                    }
                    else if (target.isEmpty() || target.constLast().x1() != x1 || target.constLast().y1() != y || target.constLast().y2() != y) {
                        target.push_back(QLine(x1, y, x1 + 1, y));
                    }

                    if (isZ && (x2 - x1) >= 18) {
                        p.setPen(zColor.lighter(140));
                        p.drawText(QRect(x1 + 2, yTop + 2, qMax(1, x2 - x1 - 4), m_rowHeight - 4), Qt::AlignCenter,
                            QStringLiteral("Z"));
                    }
                }

                if (!knownSolid.isEmpty()) {
                    p.setPen(QPen(waveGreen, isSelectedRow ? 2.8 : 2.2, Qt::SolidLine, Qt::SquareCap));
                    p.drawLines(knownSolid);
                }
                if (!selectedSolid.isEmpty()) {
                    p.setPen(QPen(selectedKnownColor, 2.8, Qt::SolidLine, Qt::SquareCap));
                    p.drawLines(selectedSolid);
                }
                if (!zDashed.isEmpty()) {
                    p.setPen(QPen(zColor, isSelectedRow ? 2.8 : 2.2, Qt::DashLine, Qt::SquareCap));
                    p.drawLines(zDashed);
                }
                if (!absentSolid.isEmpty()) {
                    p.setPen(QPen(absentColor, isSelectedRow ? 2.8 : 2.2, Qt::SolidLine, Qt::SquareCap));
                    p.drawLines(absentSolid);
                }
            }
        }
        else {
            const int visibleCount = lastExclusive - firstIdx;
            const int plotWidth = qMax(1, width() - 2 * m_padX);
            const bool lowDensity = visibleCount <= qMax(40, plotWidth / 3);
            const bool highDensity = visibleCount > qMax(80, plotWidth * 2 / 5);
            const bool mediumDensity = !lowDensity && !highDensity;
            const qreal penWidth = isSelectedRow ? 1.9 : 1.5;
            const int yBusTop = yTop + 8;
            const int yBusBottom = yTop + m_rowHeight - 8;

            if (!highDensity) {
                QVector<BusFrame> knownFrames;
                QVector<BusFrame> zFrames;
                QVector<BusFrame> absentRects;
                knownFrames.reserve(visibleCount);
                zFrames.reserve(visibleCount / 2 + 4);
                absentRects.reserve(visibleCount / 2 + 4);

                for (int i = firstIdx; i < lastExclusive; ++i) {
                    const qint64 t0 = sig.samples.at(i).time;
                    const qint64 t1 = (i + 1 < sig.samples.size()) ? sig.samples.at(i + 1).time : fullEnd();
                    const qint64 segStart = qMax(t0, m_viewStart);
                    const qint64 segEnd = qMin(qMax(t1, t0 + 1), m_viewEnd);
                    if (segEnd <= segStart) continue;

                    const WaveSample& curSample = sig.samples.at(i);
                    const bool isZ = sampleContainsUnknownState(curSample);
                    const bool isAbsent = sampleIsAbsentState(curSample);
                    const int x1 = fastX(segStart);
                    const int x2 = qMax(x1 + 1, fastX(segEnd));
                    const QRect frameRect(x1, yBusTop, qMax(1, x2 - x1), qMax(1, yBusBottom - yBusTop));
                    const BusFrame frame{ frameRect, t0 > m_viewStart, t1 < m_viewEnd };

                    if (isAbsent) {
                        absentRects.push_back(frame);
                    }
                    else {
                        QVector<BusFrame>& target = isZ ? zFrames : knownFrames;
                        target.push_back(frame);
                    }

                    if (lowDensity && !isAbsent) {
                        const QRect textRect(x1 + 6, yTop + 3, qMax(1, x2 - x1 - 12), m_rowHeight - 6);
                        if (textRect.width() > 36) {
                            const QString formatted = formatValue(sig, sig.samples.at(i), entry.format);
                            p.setPen(isZ ? zColor.lighter(140) : waveGreenText);
                            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignHCenter, formatted);
                        }
                    }
                }

                drawBusRoundedFrames(p, knownFrames, isSelectedRow ? waveGreen.lighter(130) : waveGreen, penWidth);
                drawBusRoundedFrames(p, zFrames, zColor, penWidth);
                drawBusRoundedFrames(p, absentRects, absentColor, penWidth);
            }
            else {
                const QVector<int> sampled = collectTimedSampleIndicesForWindow(
                    sig.samples, m_viewStart, m_viewEnd, targetLodSamplesForPlotWidth(plotWidth));
                QVector<BusFrame> knownFrames;
                QVector<BusFrame> zFrames;
                QVector<BusFrame> absentRects;
                knownFrames.reserve(sampled.size());
                zFrames.reserve(sampled.size() / 2 + 4);
                absentRects.reserve(sampled.size() / 4 + 4);

                for (int s = 0; s < sampled.size(); ++s) {
                    const int i = sampled.at(s);
                    const qint64 t0 = sig.samples.at(i).time;
                    qint64 t1 = m_viewEnd;
                    if (s + 1 < sampled.size()) t1 = sig.samples.at(sampled.at(s + 1)).time;
                    else if (i + 1 < sig.samples.size()) t1 = sig.samples.at(i + 1).time;
                    else t1 = fullEnd();
                    const qint64 segStart = qMax(t0, m_viewStart);
                    const qint64 segEnd = qMin(qMax(t1, t0 + 1), m_viewEnd);
                    if (segEnd <= segStart) continue;
                    const int x1 = fastX(segStart);
                    const int x2 = qMax(x1 + 1, fastX(segEnd));
                    const WaveSample& curSample = sig.samples.at(i);
                    const QRect frameRect(x1, yBusTop, qMax(1, x2 - x1), qMax(1, yBusBottom - yBusTop));
                    const BusFrame frame{ frameRect, t0 > m_viewStart, t1 < m_viewEnd };
                    if (sampleIsAbsentState(curSample)) {
                        absentRects.push_back(frame);
                    }
                    else if (sampleContainsUnknownState(curSample)) {
                        zFrames.push_back(frame);
                    }
                    else {
                        knownFrames.push_back(frame);
                    }
                }

                drawBusRoundedFrames(p, knownFrames, isSelectedRow ? waveGreen.lighter(130) : waveGreen, penWidth);
                drawBusRoundedFrames(p, zFrames, QColor(255, 72, 72), penWidth);
                drawBusRoundedFrames(p, absentRects, absentColor, penWidth);
            }
        }
    }

    if (m_rulerSelecting) {
        const int x1 = fastX(m_rulerSelectAnchorTime);
        const int x2 = fastX(m_rulerSelectCurrentTime);
        const int left = qMin(x1, x2);
        const int right = qMax(x1, x2);
        const QRect band(left, 0, qMax(1, right - left), mainBottom);
        p.fillRect(band, QColor(140, 190, 255, 28));
        p.setPen(QPen(QColor(185, 220, 255, 180), 1, Qt::DashLine));
        p.drawLine(left, 0, left, mainBottom);
        p.drawLine(right, 0, right, mainBottom);
        p.setPen(QPen(QColor(185, 220, 255, 210), 1));
        p.drawRect(QRect(left, 2, qMax(1, right - left), m_timeHeaderHeight - 4));
    }



    if (m_cursorTime >= 0) {
        p.setPen(QPen(QColor("#FFFFFF"), 1));
        const int x = fastX(m_cursorTime);
        p.drawLine(x, 0, x, mainBottom);
        QRect box(x - 30, 8, 60, 22);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#FFFFFF"));
        p.drawRoundedRect(box, 7, 7);
        p.setPen(QColor("#000000"));
        p.drawText(box, Qt::AlignCenter, pseudoDiv10Text(m_cursorTime));
    }

    const QRect ovr = overviewRect();
    p.setPen(QPen(QColor("#4A4A4A"), 1));
    p.setBrush(QColor("#111111"));
    p.drawRoundedRect(ovr, 8, 8);

    p.setPen(QColor(255, 255, 255, 40));
    const int tickCount = 12;
    for (int i = 0; i <= tickCount; ++i) {
        const int x = ovr.left() + i * ovr.width() / tickCount;
        p.drawLine(x, ovr.top() + 4, x, ovr.bottom() - 4);
    }

    QRectF thumb = overviewThumbRect();
    p.setPen(QPen(QColor("#DDEAF8"), 1.2));
    p.setBrush(QColor(60, 130, 210, 160));
    p.drawRoundedRect(thumb, 7, 7);
    p.setPen(QColor("#FFFFFF"));
    p.drawLine(QPointF(thumb.center().x() - 8, thumb.center().y()), QPointF(thumb.center().x() + 8, thumb.center().y()));
}

void WaveCanvas::mousePressEvent(QMouseEvent* event) {
    m_viewAnim->stop();

    if (event->button() == Qt::LeftButton) {
        m_pendingClickRow = -1;
        m_pendingClickCtrlHeld = false;
        if (mouseEventYCompat(event) >= 0.0 && mouseEventYCompat(event) < m_timeHeaderHeight) {
            m_rulerSelecting = true;
            m_rulerDragStartPos = mouseEventPosCompat(event);
            m_rulerSelectAnchorTime = xToTime(mouseEventXCompat(event));
            m_rulerSelectCurrentTime = m_rulerSelectAnchorTime;
            m_rulerSelectionMoved = false;
            update();
            event->accept();
            return;
        }

        if (overviewRect().contains(mouseEventPosCompat(event))) {
            const QRectF thumb = overviewThumbRect();
            if (thumb.contains(QPointF(mouseEventPosCompat(event)))) {
                m_overviewDragging = true;
                m_overviewDragOffset = mouseEventXCompat(event) - thumb.left();
            }
            else {
                const qint64 centerTime = overviewXToTime(mouseEventXCompat(event));
                qint64 ns = centerTime - span() / 2;
                qint64 ne = ns + span();
                if (ns < fullStart()) {
                    ne += fullStart() - ns;
                    ns = fullStart();
                }
                if (ne > fullEnd()) {
                    ns -= ne - fullEnd();
                    ne = fullEnd();
                }
                setViewportInstant(ns, ne);
            }
            event->accept();
            return;
        }

        m_rulerSelecting = true;
        m_rulerDragStartPos = mouseEventPosCompat(event);
        m_rulerSelectAnchorTime = xToTime(mouseEventXCompat(event));
        m_rulerSelectCurrentTime = m_rulerSelectAnchorTime;
        m_rulerSelectionMoved = false;
        m_pendingClickRow = rowAtY(mouseEventYCompat(event));
        m_pendingClickCtrlHeld = (event->modifiers() & Qt::ControlModifier) != 0;
        update();
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        if (mouseEventYCompat(event) >= m_timeHeaderHeight && !overviewRect().contains(mouseEventPosCompat(event))) {
            m_dragging = true;
            m_panDragMoved = false;
            m_dragStartPos = mouseEventPosCompat(event);
            m_dragStartViewStart = m_viewStart;
            m_dragStartViewEnd = m_viewEnd;
            m_pendingClickRow = -1;
            m_pendingClickCtrlHeld = false;
            event->accept();
            return;
        }
    }

    QWidget::mousePressEvent(event);
}

void WaveCanvas::mouseMoveEvent(QMouseEvent* event) {
    m_hoverTime = xToTime(mouseEventXCompat(event));
    Q_EMIT hoverMoved(m_hoverTime);

    if (m_rulerSelecting) {
        m_rulerSelectCurrentTime = xToTime(mouseEventXCompat(event));
        m_rulerSelectionMoved = m_rulerSelectionMoved
            || std::abs(mouseEventXCompat(event) - m_rulerDragStartPos.x()) >= 4;
        update();
        event->accept();
        return;
    }

    if (m_overviewDragging) {
        const QRect ovr = overviewRect();
        const qint64 total = qMax<qint64>(1, fullEnd() - fullStart());
        const double trackX = qBound<double>(double(ovr.left()), double(mouseEventXCompat(event)) - m_overviewDragOffset, double(ovr.right()) - overviewThumbRect().width());
        const double leftRatio = (trackX - ovr.left()) / qMax(1.0, double(ovr.width()));
        qint64 ns = fullStart() + static_cast<qint64>(std::llround(leftRatio * double(total)));
        qint64 ne = ns + span();
        if (ne > fullEnd()) {
            ne = fullEnd();
            ns = ne - span();
        }
        if (ns < fullStart()) {
            ns = fullStart();
            ne = ns + span();
        }
        setViewportInstant(ns, ne);
        event->accept();
        return;
    }

    if (m_dragging) {
        const double usable = width() - 2.0 * m_padX;
        const double dx = mouseEventXCompat(event) - m_dragStartPos.x();
        if (!m_panDragMoved && std::abs(dx) < 4) {
            event->accept();
            return;
        }
        m_panDragMoved = true;
        const qint64 dt = static_cast<qint64>(std::llround(-(dx / qMax(usable, 1.0)) * double(m_dragStartViewEnd - m_dragStartViewStart)));
        qint64 ns = m_dragStartViewStart + dt;
        qint64 ne = m_dragStartViewEnd + dt;
        const qint64 s = ne - ns;
        if (ns < fullStart()) { ne += fullStart() - ns; ns = fullStart(); }
        if (ne > fullEnd()) { ns -= ne - fullEnd(); ne = fullEnd(); }
        if ((ne - ns) != s) ns = ne - s;
        m_viewStart = ns;
        m_viewEnd = ne;
        Q_EMIT viewportChanged(m_viewStart, m_viewEnd);
        update();
        event->accept();
        return;
    }
    update();
    QWidget::mouseMoveEvent(event);
}

void WaveCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_rulerSelecting) {
        const qint64 t1 = m_rulerSelectAnchorTime;
        const qint64 t2 = m_rulerSelectCurrentTime;
        m_rulerSelecting = false;

        if (m_rulerSelectionMoved && t1 != t2) {
            qint64 ns = qMin(t1, t2);
            qint64 ne = qMax(t1, t2);
            if (ne - ns < m_minWindow) ne = ns + m_minWindow;
            if (ns < fullStart()) ns = fullStart();
            if (ne > fullEnd()) ne = fullEnd();
            if (ne - ns < m_minWindow) ns = qMax(fullStart(), ne - m_minWindow);
            setViewportInstant(ns, ne);
            m_cursorTime = ns + (ne - ns) / 2;
        }
        else {
            m_cursorTime = t1;
            if (m_pendingClickRow >= 0) {
                m_selectedEntryIndex = m_pendingClickRow;
                Q_EMIT entryClicked(m_pendingClickRow, m_pendingClickCtrlHeld);
            }
        }
        m_pendingClickRow = -1;
        m_pendingClickCtrlHeld = false;
        Q_EMIT cursorMoved(m_cursorTime);
        update();
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton && m_dragging) {
        m_dragging = false;
        m_panDragMoved = false;
        m_pendingClickRow = -1;
        m_pendingClickCtrlHeld = false;
        update();
        event->accept();
        return;
    }

    m_dragging = false;
    m_panDragMoved = false;
    m_overviewDragging = false;
    QWidget::mouseReleaseEvent(event);
}

void WaveCanvas::leaveEvent(QEvent* event) {
    m_hoverTime = -1;
    Q_EMIT hoverMoved(m_hoverTime);
    update();
    QWidget::leaveEvent(event);
}

void WaveCanvas::wheelEvent(QWheelEvent* event) {
    m_cursorTime = xToTime(wheelEventXCompat(event));
    Q_EMIT cursorMoved(m_cursorTime);
    zoomAt(m_cursorTime, event->angleDelta().y() > 0 ? 0.85 : 1.15, true);
    event->accept();
}
