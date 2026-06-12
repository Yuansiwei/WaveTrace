#pragma once

#include "WaveTypes.h"

#include <QList>
#include <QMainWindow>
#include <QString>
#include <QSet>
#include <QVector>
#include <memory>

class QLabel;
class QDialog;
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
class ActiveSignalListWidget;
struct SignalLogicTree;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool openWaveFilePath(const QString& path, bool showError = true);

private:
    void openWaveFile();
    void compareWaveFiles();
    void exportCompressedWaveFile();
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
    };

    WaveFile m_wave;
    QString m_currentWaveFilePath;
    bool m_currentWaveSupportsOnDemand = false;
    QVector<int> m_signalIndexBySignalId;

    QWidget* m_central = nullptr;
    QLabel* m_metaLabel = nullptr;
    QLabel* m_cursorLabel = nullptr;
    QLabel* m_hoverLabel = nullptr;
    QLabel* m_windowLabel = nullptr;
    QLineEdit* m_treeSearchEdit = nullptr;
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

    void buildUi();
    void applyTheme();
    void setupToolbarButton(QPushButton* button, const QIcon& icon, const QString& objectName, const QString& tip);
    void loadDemoWave();
    void applyWave(WaveFile&& wave);
    void updateMetaLabel();

    void rebuildTree();
    void resetTreeViewModel();
    void collectSignalIndexesFromLogicNode(int nodeId, QSet<int>& seen, QList<int>& output) const;
    QList<int> selectedActiveSignalIndexesForJump() const;
    QList<int> selectedActiveSignalIndexesForFind() const;
    void rebuildValueFindResults();
    void updateValueFindNavigationState();
    void jumpToValueFindHit(int hitIndex);
    void jumpToAdjacentValueFindHit(bool forward);
    void showTreeSearchResults(const QString& query);
    void rebuildActiveListRows();
    void rebuildVisibleSignals();
    void syncActiveScrollToCanvas();
    void clampWindowToAvailableScreen();
    void refreshActiveValueLabels();
    void scheduleRefreshActiveValueLabels(int delayMs = 35);

    void insertSignalIntoTree(const QString& fullName, int signalIndex);
    bool canDeferSamplesWithLod(const WaveSignal& sig) const;
    bool ensureSignalSamplesLoaded(const QList<int>& signalIndexes, bool allowLodDefer = true);
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
    WaveFile materializeWaveSignalNames(const WaveFile& src) const;
};
