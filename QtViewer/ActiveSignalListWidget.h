#pragma once

#include <QList>
#include <QModelIndex>
#include <QPoint>
#include <QTreeWidget>
#include <QtGlobal>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QKeyEvent;
class QMimeData;
class QMouseEvent;
class QPainter;
class QStyleOptionViewItem;

class ActiveSignalListWidget : public QTreeWidget {
    Q_OBJECT
public:
    explicit ActiveSignalListWidget(QWidget* parent = nullptr);

Q_SIGNALS:
    void deleteRequested(QTreeWidgetItem* item);
    void formatRequested(QTreeWidgetItem* item, const QString& formatText);
    void signalIndexesDropped(const QList<int>& signalIndexes);
    void activeRowsReordered();

protected:
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    Qt::DropActions supportedDropActions() const override;

    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private Q_SLOTS:
    void showContextMenu(const QPoint& pos);
    void onSectionResized(int logicalIndex, int oldSize, int newSize);

private:
    bool m_syncingWidths = false;
    double m_nameRatio = 0.68;
    QModelIndex m_leftPressIndex;
    QPoint m_leftPressPos;
    bool m_leftPressWasSelected = false;
    bool m_resizingDivider = false;
    int m_lastTextClickRow = -1;
    int m_lastTextClickColumn = -1;
    qint64 m_lastTextClickMs = 0;

    void syncColumnWidths();
    int dividerX() const;
    bool isNearDivider(const QPoint& pos) const;
    void copySelectedTextToClipboard() const;
};
