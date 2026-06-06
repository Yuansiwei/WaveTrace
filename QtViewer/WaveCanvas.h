#pragma once

#include "WaveTypes.h"
#include <QHash>
#include <QList>
#include <QSet>
#include <QWidget>

class QVariantAnimation;

class WaveCanvas : public QWidget {
    Q_OBJECT
public:
    explicit WaveCanvas(QWidget* parent = nullptr);

    void setWave(WaveFile* wave);
    void setVisibleEntries(const QVector<ActiveSignalRef>& entries);
    void setFirstVisibleEntryIndex(int index);
    void setVisibleEntryWindow(int firstEntryIndex, int visibleEntryCount);
    int firstVisibleEntryIndex() const { return m_firstVisibleEntryIndex; }
    void setSelectedEntryIndex(int index);
    void setSelectedEntryIndexes(const QSet<int>& indexes);

    void resetView();
    void zoomByFactor(double factor);
    void panBy(qint64 deltaTime);

    qint64 cursorTime() const { return m_cursorTime; }
    qint64 hoverTime() const { return m_hoverTime; }
    qint64 viewStart() const { return m_viewStart; }
    qint64 viewEnd() const { return m_viewEnd; }
    qint64 fullStartTime() const;
    qint64 fullEndTime() const;

    QString formattedValueAtCursor(const ActiveSignalRef& entry) const;
    bool jumpToNearestChange(int signalIndex, bool forward);
    bool jumpToNearestChangeForSignals(const QList<int>& signalIndexes, bool forward, bool diffOnly);
    bool jumpToTime(qint64 internalTime);

Q_SIGNALS:
    void cursorMoved(qint64 t);
    void hoverMoved(qint64 t);
    void viewportChanged(qint64 start, qint64 end);
    void entryClicked(int index, bool ctrlHeld);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    QSize minimumSizeHint() const override;

private:
    WaveFile* m_wave = nullptr;
    QVector<ActiveSignalRef> m_visibleEntries;
    int m_selectedEntryIndex = -1;
    QSet<int> m_selectedEntryIndexes;
    int m_firstVisibleEntryIndex = 0;
    int m_visibleEntryCountLimit = -1;

    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 100;
    qint64 m_cursorTime = -1;
    qint64 m_hoverTime = -1;

    bool m_dragging = false;
    QPoint m_dragStartPos;
    qint64 m_dragStartViewStart = 0;
    qint64 m_dragStartViewEnd = 100;
    bool m_panDragMoved = false;

    bool m_overviewDragging = false;
    double m_overviewDragOffset = 0.0;

    bool m_rulerSelecting = false;
    QPoint m_rulerDragStartPos;
    qint64 m_rulerSelectAnchorTime = 0;
    qint64 m_rulerSelectCurrentTime = 0;
    bool m_rulerSelectionMoved = false;
    int m_pendingClickRow = -1;
    bool m_pendingClickCtrlHeld = false;

    const int m_timeHeaderHeight = 37;
    const int m_rowHeight = 40;
    const int m_padX = 10;
    const int m_overviewHeight = 24;
    const qint64 m_minWindow = 1;

    QVariantAnimation* m_viewAnim = nullptr;
    qint64 m_animFromStart = 0;
    qint64 m_animFromEnd = 100;
    qint64 m_animToStart = 0;
    qint64 m_animToEnd = 100;

    mutable QHash<int, int> m_lastSampleIndexBySignal;

    struct SampleStrideLevel {
        qint64 stride = 1;
        QVector<int> nextIndex;
    };
    struct BitParityStrideLevel {
        qint64 stride = 1;
        QVector<int> nextOddIndex;
        QVector<int> nextEvenIndex;
    };
    mutable QHash<int, QVector<SampleStrideLevel>> m_strideLevelsBySignal;
    mutable QHash<int, QVector<BitParityStrideLevel>> m_bitParityStrideLevelsBySignal;

    qint64 fullStart() const;
    qint64 fullEnd() const;
    qint64 span() const;
    QRect overviewRect() const;
    QRectF overviewThumbRect() const;
    qint64 xToTime(double x) const;
    double timeToX(qint64 t) const;
    qint64 overviewXToTime(double x) const;
    int visibleRowCapacity() const;
    int effectiveVisibleRowCount() const;
    int lastSampleIndexAtOrBefore(const WaveSignal& sig, int signalIndex, qint64 t) const;
    void clearLookupCache();
    void clearSamplingCache();
    void ensureStrideLevelsForSignal(int signalIndex) const;
    void ensureBitParityStrideLevelsForSignal(int signalIndex) const;
    QVector<int> collectBitAlternatingSampleIndicesForWindow(const WaveSignal& sig, int signalIndex, qint64 start, qint64 end, int targetSamples = 1000) const;
    QVector<int> collectSampleIndicesForWindow(const WaveSignal& sig, int signalIndex, qint64 start, qint64 end, int targetSamples = 1000) const;

    WaveSample rawValueAtTime(int signalIndex, qint64 t) const;
    QString formatValue(const WaveSignal& sig, const WaveSample& sample, ValueRadix format) const;
    quint64 parseRawBits(const QString& raw) const;

    void zoomAt(qint64 centerTime, double factor, bool animated = true);
    void animateViewportTo(qint64 start, qint64 end, int durationMs = 140);
    void setViewportInstant(qint64 start, qint64 end);
    int rowAtY(double y) const;
};
