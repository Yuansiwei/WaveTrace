#pragma once

#include "WaveTypes.h"

#include <QList>
#include <QMainWindow>
#include <QString>
#include <QSet>
#include <QVector>
#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QIcon;
class QSplitter;
class QTimer;
class QAbstractItemModel;
class QModelIndex;
class QTreeView;
class QTreeWidgetItem;
class WaveCanvas;
class ActiveSignalListWidget;
struct SignalLogicTree;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool openWaveFilePath(const QString& path);

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
    std::unique_ptr<SignalLogicTree> m_signalTreeModel;

    void buildUi();
    void applyTheme();
    void setupToolbarButton(QPushButton* button, const QIcon& icon, const QString& tip);
    void loadDemoWave();
    void applyWave(const WaveFile& wave);
    void updateMetaLabel();

    void rebuildTree();
    void resetTreeViewModel();
    void collectSignalIndexesFromLogicNode(int nodeId, QSet<int>& seen, QList<int>& output) const;
    QList<int> selectedActiveSignalIndexesForJump() const;
    void showTreeSearchResults(const QString& query);
    void rebuildActiveListRows();
    void rebuildVisibleSignals();
    void syncActiveScrollToCanvas();
    void clampWindowToAvailableScreen();
    void refreshActiveValueLabels();
    void scheduleRefreshActiveValueLabels(int delayMs = 35);

    void insertSignalIntoTree(const QString& fullName, int signalIndex);
    bool ensureSignalSamplesLoaded(const QList<int>& signalIndexes);

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
