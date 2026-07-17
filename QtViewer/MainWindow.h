#pragma once

#include "WaveTypes.h"

#include <QList>
#include <QMainWindow>
#include <QString>
#include <QSet>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class QLabel;
class QDialog;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QLineEdit;
class QPushButton;
class QIcon;
class QSplitter;
class QTimer;
class QAbstractItemModel;
class QModelIndex;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class WaveCanvas;
class WaveParser4Reader;
class WaveBlockCacheLoader;
class ActiveSignalListWidget;
struct SignalLogicTree;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool openWaveFilePath(const QString& path, bool showError = true);
    void activateFirstSignalsForBenchmark(int count);
    void selectViewportRangeForBenchmark(qint64 start, qint64 end);
    void resetViewForBenchmark();
    bool benchmarkActiveViewportCoverage(int* covered, int* total) const;
    bool benchmarkValidateRawCaches(QString* error);
    bool compareWaveFilePaths(const QString& leftPath,
                              const QString& rightPath,
                              bool showProgress = true,
                              bool showMessages = true,
                              QString* errorMessage = nullptr,
                              qint64* elapsedMs = nullptr,
                              int* resultSignalCount = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void openWaveFile();
    void compareWaveFiles();
    void zoomIn();
    void zoomOut();
    void panLeft();
    void panRight();
    void jumpToPrevChange();
    void jumpToNextChange();
    void jumpToTime();
    void openDerivedSignalDialog();
    void openValueFindDialog();
    void runValueFind();
    void jumpToPreviousValueFindHit();
    void jumpToNextValueFindHit();
    void resetView();

    void onAddSelectedFromTree();
    void onRemoveSelectedActive();
    void onClearActive();

    void onTreeIndexDoubleClicked(const QModelIndex& index);
    void onTreeSearchTextChanged(const QString& text);

    void onActiveCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);

    void onCursorMoved(qint64 t);
    void onHoverMoved(qint64 t);
    void onViewportChanged(qint64 start, qint64 end);
    void onViewportRangeSelected(qint64 start, qint64 end);

private:
    enum ActiveItemRoles {
        RoleSignalIndex = Qt::UserRole,
        RoleSignalWidth,
        RoleCurrentFormat
    };

    struct ValueFindHit {
        int signalIndex = -1;
        int sampleIndex = -1;
        qint64 time = -1;
        qint64 duration = 0;
    };

    WaveFile m_wave;
    QString m_currentWaveFilePath;
    bool m_currentWaveSupportsOnDemand = false;
    std::shared_ptr<WaveParser4Reader> m_waveReader;
    std::shared_ptr<std::mutex> m_waveReaderMutex = std::make_shared<std::mutex>();
    std::unique_ptr<WaveBlockCacheLoader> m_blockCacheLoader;
    QVector<int> m_signalIndexBySignalId;

    QWidget* m_central = nullptr;
    QLabel* m_metaLabel = nullptr;
    QLabel* m_cursorLabel = nullptr;
    QLabel* m_hoverLabel = nullptr;
    QLabel* m_windowLabel = nullptr;
    QLineEdit* m_treeSearchEdit = nullptr;
    QPushButton* m_treeSearchCaseButton = nullptr;
    QPushButton* m_treeSearchRegexButton = nullptr;
    QLineEdit* m_activeSearchEdit = nullptr;
    QLineEdit* m_jumpTimeEdit = nullptr;
    QTreeView* m_tree = nullptr;
    QAbstractItemModel* m_treeModel = nullptr;
    ActiveSignalListWidget* m_activeList = nullptr;
    WaveCanvas* m_canvas = nullptr;
    QPushButton* m_removeSelectedButton = nullptr;
    QPushButton* m_clearActiveButton = nullptr;
    QSplitter* m_splitter = nullptr;
    QTimer* m_activeValueRefreshTimer = nullptr;
    QTimer* m_viewportLoadTimer = nullptr;
    QDialog* m_valueFindDialog = nullptr;
    QLineEdit* m_valueFindEdit = nullptr;
    QLabel* m_valueFindSummaryLabel = nullptr;
    QTreeWidget* m_valueFindResults = nullptr;
    QPushButton* m_valueFindPrevButton = nullptr;
    QPushButton* m_valueFindNextButton = nullptr;
    QVector<ValueFindHit> m_valueFindHits;
    QList<int> m_valueFindSignalIndexes;
    QString m_valueFindSummaryBase;
    int m_valueFindCurrentHit = -1;
    std::unique_ptr<SignalLogicTree> m_signalTreeModel;
    std::thread m_treeWarmupThread;
    std::thread m_viewportLoadThread;
    std::shared_ptr<std::atomic_bool> m_treeWarmupCancel;
    quint64 m_treeWarmupGeneration = 0;
    quint64 m_waveFileGeneration = 0;
    quint64 m_viewportLoadSerial = 0;
    qint64 m_pendingViewportStart = 0;
    qint64 m_pendingViewportEnd = 0;
    qint64 m_animationTargetStart = 0;
    qint64 m_animationTargetEnd = 0;
    bool m_viewportLoadPending = false;
    bool m_viewportLoadInFlight = false;
    bool m_animationTargetLoadScheduled = false;
    bool m_guardedViewportCommitPending = false;
    qint64 m_guardedViewportCommitStart = 0;
    qint64 m_guardedViewportCommitEnd = 0;
    quint64 m_guardedViewportCommitSerial = 0;
    quint64 m_deferredViewportApplySerial = 0;
    qint64 m_deferredViewportBucketCycles = 1;
    std::function<void()> m_deferredViewportApply;

    void buildUi();
    void applyTheme();
    void setupToolbarButton(QPushButton* button, const QIcon& icon, const QString& objectName, const QString& tip);
    void loadDemoWave();
    void applyWave(WaveFile&& wave);
    void updateMetaLabel();

    void rebuildTree();
    void scheduleTreeWarmup();
    void stopTreeWarmup();
    void resetTreeViewModel();
    void collectSignalIndexesFromLogicNode(int nodeId, QSet<int>& seen, QList<int>& output) const;
    QList<int> selectedActiveSignalIndexesForJump() const;
    QList<int> selectedActiveSignalIndexesForFind() const;
    void rebuildValueFindResults();
    void updateValueFindNavigationState();
    void jumpToValueFindHit(int hitIndex);
    void jumpToAdjacentValueFindHit(bool forward);
    void jumpSelectedTreeSignalToViewportEvent(bool firstEvent);
    void showTreeSearchResults(const QString& query);
    void rebuildActiveListRows();
    void rebuildVisibleSignals();
    void syncActiveScrollToCanvas();
    void clampWindowToAvailableScreen();
    void refreshActiveValueLabels();
    void scheduleRefreshActiveValueLabels(int delayMs = 35);
    void scheduleViewportDataLoad(qint64 start, qint64 end);
    void scheduleAnimationTargetDataLoad(qint64 start, qint64 end);
    bool applyDeferredViewportResultIfReady(bool force);
    void completeGuardedViewportCommit(bool success);
    void startPendingViewportDataLoad();
    bool handleWaveFileDropEvent(QEvent* event);

    void insertSignalIntoTree(const QString& fullName, int signalIndex);
    QList<int> selectedTreeSignalIndexesForViewportJump() const;
    bool selectActiveSignalByIndex(int signalIndex);
    bool canDeferSamplesWithLod(const WaveSignal& sig) const;
    bool ensureSignalLodLoaded(const QList<int>& signalIndexes);
    bool ensureSignalSamplesLoaded(const QList<int>& signalIndexes, bool allowLodDefer = true, bool viewportRaw = false, bool quiet = false);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    bool ensureSignalSamplesLoaded(const QVector<int>& signalIndexes, bool allowLodDefer = true, bool viewportRaw = false, bool quiet = false);
#endif
    bool createDerivedSignal(const QString& name, const QString& expression, int widthOverride);

    void addSignalToActive(int signalIndex);
    void addSignalIndexesToActive(const QList<int>& signalIndexes);
    void removeActiveItem(QTreeWidgetItem* item);
    void removeActiveRows(const QList<int>& rows);
    int signalIndexFromActiveItem(QTreeWidgetItem* item) const;
    ValueRadix formatFromActiveItem(QTreeWidgetItem* item) const;
    void setActiveItemFormat(QTreeWidgetItem* item, const QString& text);
    QString signalDisplayName(int signalIndex) const;
    QString formatNameWithRange(int signalIndex) const;
    QString formatNameWithRange(const WaveSignal& sig) const;
};
