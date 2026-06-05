#include "ActiveSignalListWidget.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QByteArray>
#include <QDataStream>
#include <QMimeData>
#include <QIODevice>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QLineEdit>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QResizeEvent>
#include <QTreeWidgetItem>
#include <QtGlobal>

#include <algorithm>
#include <cstdlib>
#include <functional>

namespace {
    static inline QPoint eventPosCompat(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return event->position().toPoint();
#else
        return event->pos();
#endif
    }

    static inline QPoint eventPosCompat(const QDropEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return event->position().toPoint();
#else
        return event->pos();
#endif
    }

constexpr const char* kMimeSignalIndexes = "application/x-waveviewer-signal-indexes";
constexpr const char* kMimeActiveRows = "application/x-waveviewer-active-rows";

enum ActiveItemRoles {
    RoleSignalIndex = Qt::UserRole,
    RoleSignalWidth,
    RoleCurrentFormat
};

class ReadOnlyItemTextDelegate : public QStyledItemDelegate {
public:
    explicit ReadOnlyItemTextDelegate(QObject* parent = nullptr)
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
        // Read-only delegate: text selection/copy and cursor navigation only.
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        if (editor) editor->setGeometry(option.rect.adjusted(1, 1, -1, -1));
    }
};

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
}

ActiveSignalListWidget::ActiveSignalListWidget(QWidget* parent)
    : QTreeWidget(parent) {
    setColumnCount(2);
    setHeaderLabels(QStringList() << QString::fromUtf8("选中信号") << QStringLiteral("Value"));
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setAllColumnsShowFocus(false);
    setUniformRowHeights(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setDragDropOverwriteMode(false);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setTextElideMode(Qt::ElideLeft);
    setItemDelegate(new ReadOnlyItemTextDelegate(this));
    setEditTriggers(QAbstractItemView::NoEditTriggers);

    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::Interactive);
    header()->setSectionResizeMode(1, QHeaderView::Interactive);
    header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header()->setMinimumSectionSize(60);
    header()->setFixedHeight(40);
    header()->hide();

    connect(this, &QTreeWidget::customContextMenuRequested, this, &ActiveSignalListWidget::showContextMenu);
    connect(header(), &QHeaderView::sectionResized, this, &ActiveSignalListWidget::onSectionResized);

    syncColumnWidths();
}

void ActiveSignalListWidget::syncColumnWidths() {
    if (m_syncingWidths) return;
    m_syncingWidths = true;

    const int total = viewport()->width();
    const int splitterGap = 2;
    const int minFirst = 90;
    const int minSecond = 60;

    if (total > 20) {
        int first = 0;
        int second = 0;

        if (total <= minFirst + minSecond + splitterGap) {
            // 窗口很窄时，不再强行套用大最小宽度，避免 qBound 的 max < min 触发断言
            first = qMax(1, int(total * m_nameRatio));
            first = qBound(1, first, qMax(1, total - 1));
            second = qMax(1, total - first - splitterGap);
            if (second < 1) {
                second = 1;
                first = qMax(1, total - second - splitterGap);
            }
        } else {
            first = int(total * m_nameRatio);
            first = qBound(minFirst, first, total - minSecond - splitterGap);
            second = qMax(minSecond, total - first - splitterGap);
        }

        header()->resizeSection(0, first);
        header()->resizeSection(1, second);
    }

    m_syncingWidths = false;
}

int ActiveSignalListWidget::dividerX() const {
    return header() ? header()->sectionPosition(1) : 0;
}

bool ActiveSignalListWidget::isNearDivider(const QPoint& pos) const {
    return std::abs(pos.x() - dividerX()) <= 5;
}

void ActiveSignalListWidget::onSectionResized(int logicalIndex, int, int newSize) {
    if (m_syncingWidths) return;
    if (logicalIndex == 0) {
        const int total = qMax(1, viewport()->width());
        m_nameRatio = qBound(0.35, double(newSize) / double(total), 0.85);
    }
}

void ActiveSignalListWidget::resizeEvent(QResizeEvent* event) {
    QTreeWidget::resizeEvent(event);
    syncColumnWidths();
}


QStringList ActiveSignalListWidget::mimeTypes() const {
    QStringList types = QTreeWidget::mimeTypes();
    if (!types.contains(QString::fromLatin1(kMimeActiveRows))) {
        types << QString::fromLatin1(kMimeActiveRows);
    }
    return types;
}

QMimeData* ActiveSignalListWidget::mimeData(const QList<QTreeWidgetItem*>& items) const {
    QMimeData* mime = QTreeWidget::mimeData(items);
    if (!mime) return nullptr;

    QList<int> rows;
    rows.reserve(items.size());
    for (QTreeWidgetItem* item : items) {
        const int row = indexOfTopLevelItem(item);
        if (row >= 0 && !rows.contains(row)) rows.push_back(row);
    }
    if (!rows.isEmpty()) {
        std::sort(rows.begin(), rows.end());
        mime->setData(kMimeActiveRows, encodeIntList(rows));
    }
    return mime;
}

Qt::DropActions ActiveSignalListWidget::supportedDropActions() const {
    return Qt::CopyAction | Qt::MoveAction;
}

void ActiveSignalListWidget::keyPressEvent(QKeyEvent* event) {
    if (event && event->matches(QKeySequence::Copy)) {
        copySelectedTextToClipboard();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        if (QTreeWidgetItem* item = currentItem()) {
            Q_EMIT deleteRequested(item);
            event->accept();
            return;
        }
    }
    QTreeWidget::keyPressEvent(event);
}

void ActiveSignalListWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (!event) return;
    const QMimeData* mime = event->mimeData();
    if (mime && mime->hasFormat(kMimeSignalIndexes)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    if (mime && mime->hasFormat(kMimeActiveRows)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    event->ignore();
}

void ActiveSignalListWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (!event) return;
    const QMimeData* mime = event->mimeData();
    if (mime && mime->hasFormat(kMimeSignalIndexes)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    if (mime && mime->hasFormat(kMimeActiveRows)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    event->ignore();
}

void ActiveSignalListWidget::dropEvent(QDropEvent* event) {
    if (!event) return;

    const QMimeData* mime = event->mimeData();

    // Candidate tree -> active list: add signals. The source keeps its items.
    if (mime && mime->hasFormat(kMimeSignalIndexes)) {
        const QList<int> signalIndexes = decodeIntList(mime, kMimeSignalIndexes);
        if (!signalIndexes.isEmpty()) {
            Q_EMIT signalIndexesDropped(signalIndexes);
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }

    // Active list internal reorder. This must be handled manually.
    // Default tree-drop semantics are not used here, because they may reparent
    // a row as a child item and make the signal disappear from the top-level
    // active signal list.
    if (mime && mime->hasFormat(kMimeActiveRows)) {
        QList<int> rows = decodeIntList(mime, kMimeActiveRows);
        if (rows.isEmpty()) {
            event->ignore();
            return;
        }

        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

        QList<int> validRows;
        validRows.reserve(rows.size());
        for (int row : rows) {
            if (row >= 0 && row < topLevelItemCount()) {
                validRows.push_back(row);
            }
        }
        if (validRows.isEmpty()) {
            event->ignore();
            return;
        }

        const int firstMovingRow = validRows.first();
        const int lastMovingRow = validRows.last();

        int insertRow = topLevelItemCount();
        if (QTreeWidgetItem* target = itemAt(eventPosCompat(event))) {
            const int targetRow = indexOfTopLevelItem(target);
            if (targetRow >= 0) {
                const QAbstractItemView::DropIndicatorPosition pos = dropIndicatorPosition();

                if (pos == QAbstractItemView::AboveItem) {
                    insertRow = targetRow;
                } else if (pos == QAbstractItemView::BelowItem) {
                    insertRow = targetRow + 1;
                } else if (pos == QAbstractItemView::OnItem) {
                    if (targetRow < firstMovingRow) {
                        // Drag upward onto another signal: insert above target.
                        insertRow = targetRow;
                    } else if (targetRow > lastMovingRow) {
                        // Drag downward onto another signal: insert below target.
                        insertRow = targetRow + 1;
                    } else {
                        // Dropped onto itself or inside the selected moving block.
                        // Accept without moving so no item disappears.
                        event->setDropAction(Qt::CopyAction);
                        event->accept();
                        return;
                    }
                } else if (pos == QAbstractItemView::OnViewport) {
                    insertRow = topLevelItemCount();
                } else {
                    insertRow = targetRow;
                }
            }
        }

        // Convert insertion row in the original list to insertion row after
        // taking out the moved top-level items.
        for (int row : validRows) {
            if (row < insertRow) --insertRow;
        }
        insertRow = qBound(0, insertRow, topLevelItemCount());

        QList<QTreeWidgetItem*> movingItems;
        movingItems.reserve(validRows.size());

        QList<int> descendingRows = validRows;
        std::sort(descendingRows.begin(), descendingRows.end(), std::greater<int>());
        for (int row : descendingRows) {
            QTreeWidgetItem* item = takeTopLevelItem(row);
            if (item) {
                item->setSelected(false);
                movingItems.prepend(item);
            }
        }

        if (movingItems.isEmpty()) {
            event->ignore();
            return;
        }

        int currentInsertRow = insertRow;
        for (QTreeWidgetItem* item : movingItems) {
            insertTopLevelItem(currentInsertRow, item);
            item->setSelected(true);
            ++currentInsertRow;
        }

        setCurrentItem(movingItems.last(), 0, QItemSelectionModel::NoUpdate);
        scrollToItem(movingItems.last());

        Q_EMIT activeRowsReordered();

        // We already moved items manually. CopyAction prevents Qt's drag source
        // from deleting/cleaning the source rows a second time.
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }

    event->ignore();
}


void ActiveSignalListWidget::mousePressEvent(QMouseEvent* event) {
    if (event && event->button() == Qt::LeftButton && isNearDivider(eventPosCompat(event))) {
        m_resizingDivider = true;
        setCursor(Qt::SplitHCursor);
        m_leftPressIndex = QModelIndex();
        m_leftPressPos = QPoint();
        m_leftPressWasSelected = false;
        event->accept();
        return;
    }

    if (event && event->button() == Qt::LeftButton) {
        m_leftPressIndex = indexAt(eventPosCompat(event));
        m_leftPressPos = eventPosCompat(event);
        m_leftPressWasSelected = m_leftPressIndex.isValid() && selectionModel() && selectionModel()->isSelected(m_leftPressIndex);
    } else {
        m_leftPressIndex = QModelIndex();
        m_leftPressPos = QPoint();
        m_leftPressWasSelected = false;
    }

    if (event && event->button() == Qt::RightButton) {
        if (QTreeWidgetItem* item = itemAt(eventPosCompat(event))) {
            if (item->isSelected()) {
                setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
                event->accept();
                return;
            }
            clearSelection();
            item->setSelected(true);
            setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            event->accept();
            return;
        }
    }
    QTreeWidget::mousePressEvent(event);
}

void ActiveSignalListWidget::mouseMoveEvent(QMouseEvent* event) {
    if (event && m_resizingDivider) {
        const int total = qMax(1, viewport()->width());
        const int minFirst = 60;
        const int minSecond = 60;
        int first = qBound(minFirst, eventPosCompat(event).x(), qMax(minFirst, total - minSecond));
        int second = 1;
        if (total <= minFirst + minSecond + 2) {
            first = qBound(1, eventPosCompat(event).x(), qMax(1, total - 1));
            second = qMax(1, total - first);
        } else {
            second = qMax(1, total - first - 2);
        }
        m_nameRatio = qBound(0.10, double(first) / double(total), 0.95);
        m_syncingWidths = true;
        header()->resizeSection(0, first);
        header()->resizeSection(1, second);
        m_syncingWidths = false;
        viewport()->update();
        event->accept();
        return;
    }

    if (event) {
        setCursor(isNearDivider(eventPosCompat(event)) ? Qt::SplitHCursor : Qt::ArrowCursor);
    }
    QTreeWidget::mouseMoveEvent(event);
}

void ActiveSignalListWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (m_resizingDivider) {
        m_resizingDivider = false;
        unsetCursor();
        if (event) event->accept();
        return;
    }

    const QModelIndex pressIndex = m_leftPressIndex;
    const QPoint pressPos = m_leftPressPos;
    const bool pressWasSelected = m_leftPressWasSelected;
    QTreeWidget::mouseReleaseEvent(event);

    if (event && event->button() == Qt::LeftButton &&
        event->modifiers() == Qt::NoModifier &&
        pressIndex.isValid() && indexAt(eventPosCompat(event)) == pressIndex &&
        (eventPosCompat(event) - pressPos).manhattanLength() <= QApplication::startDragDistance()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool sameCell = (pressIndex.row() == m_lastTextClickRow && pressIndex.column() == m_lastTextClickColumn);
        const qint64 elapsed = sameCell ? (now - m_lastTextClickMs) : 0;
        const int fastDoubleClickMs = QApplication::doubleClickInterval();
        const bool slowSecondClick = pressWasSelected && sameCell && elapsed > fastDoubleClickMs && elapsed < 2000;
        if (slowSecondClick) {
            edit(pressIndex);
            m_lastTextClickRow = -1;
            m_lastTextClickColumn = -1;
            m_lastTextClickMs = 0;
        }
        else {
            m_lastTextClickRow = pressIndex.row();
            m_lastTextClickColumn = pressIndex.column();
            m_lastTextClickMs = now;
        }
    }

    m_leftPressIndex = QModelIndex();
    m_leftPressPos = QPoint();
    m_leftPressWasSelected = false;
}

void ActiveSignalListWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    // Fast double-click should keep its normal view behavior and must not open
    // the read-only text editor.  Text selection is entered only by a slow
    // second click on an already selected cell, matching Windows rename timing.
    m_lastTextClickRow = -1;
    m_lastTextClickColumn = -1;
    m_lastTextClickMs = 0;
    QTreeWidget::mouseDoubleClickEvent(event);
}

void ActiveSignalListWidget::copySelectedTextToClipboard() const {
    QStringList lines;
    QList<QTreeWidgetItem*> picked = selectedItems();
    if (picked.isEmpty() && currentItem()) picked.push_back(currentItem());

    // Keep copied rows in visual order rather than selection order.
    std::sort(picked.begin(), picked.end(), [this](QTreeWidgetItem* a, QTreeWidgetItem* b) {
        return indexOfTopLevelItem(a) < indexOfTopLevelItem(b);
    });
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());

    for (QTreeWidgetItem* item : picked) {
        if (!item) continue;
        lines << (item->text(0) + QStringLiteral("\t") + item->text(1));
    }
    if (!lines.isEmpty() && QApplication::clipboard()) {
        QApplication::clipboard()->setText(lines.join(QStringLiteral("\n")));
    }
}

void ActiveSignalListWidget::drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QTreeWidget::drawRow(painter, option, index);
    if (!painter) return;
    const int splitX = header()->sectionPosition(1);
    painter->save();
    painter->setPen(QColor("#8C98A4"));
    painter->drawLine(splitX - 1, option.rect.top() + 2, splitX - 1, option.rect.bottom() - 2);
    painter->restore();
}

void ActiveSignalListWidget::showContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = itemAt(pos);
    if (!item) return;

    if (!item->isSelected()) {
        clearSelection();
        item->setSelected(true);
    }
    setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);

    const QList<QTreeWidgetItem*> picked = selectedItems();
    const int signalWidth = item->data(0, RoleSignalWidth).toInt();
    const QString currentFormat = item->data(0, RoleCurrentFormat).toString();

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background:#4A5059; color:#F7FAFF; border:1px solid #FFFFFF; }"
        "QMenu::item { padding:6px 18px; }"
        "QMenu::item:selected { background:#5E7FA7; }"
        "QMenu::separator { height:1px; background:#AEB8C2; margin:4px 10px; }"
    );

    QMenu* formatMenu = menu.addMenu(QString::fromUtf8("修改显示格式"));
    const QStringList basicFormats{QStringLiteral("bin"), QStringLiteral("hex"), QStringLiteral("dec")};
    for (const QString& fmt : basicFormats) {
        QAction* action = formatMenu->addAction(fmt);
        action->setCheckable(true);
        action->setChecked(currentFormat == fmt);
        QObject::connect(action, &QAction::triggered, this, [this, picked, fmt]() {
            for (QTreeWidgetItem* one : picked) Q_EMIT formatRequested(one, fmt);
        });
    }

    formatMenu->addSeparator();
    const QStringList advanced32Formats{QStringLiteral("int"), QStringLiteral("uint"), QStringLiteral("float")};
    for (const QString& fmt : advanced32Formats) {
        QAction* action = formatMenu->addAction(fmt);
        action->setCheckable(true);
        action->setChecked(currentFormat == fmt);
        action->setEnabled(signalWidth == 32);
        QObject::connect(action, &QAction::triggered, this, [this, picked, fmt]() {
            for (QTreeWidgetItem* one : picked) Q_EMIT formatRequested(one, fmt);
        });
    }

    const QStringList advanced64Formats{QStringLiteral("int64"), QStringLiteral("uint64"), QStringLiteral("double")};
    for (const QString& fmt : advanced64Formats) {
        QAction* action = formatMenu->addAction(fmt);
        action->setCheckable(true);
        action->setChecked(currentFormat == fmt);
        action->setEnabled(signalWidth == 64);
        QObject::connect(action, &QAction::triggered, this, [this, picked, fmt]() {
            for (QTreeWidgetItem* one : picked) Q_EMIT formatRequested(one, fmt);
        });
    }

    menu.addSeparator();
    QAction* delAction = menu.addAction(QString::fromUtf8("删除选中信号"));
    QObject::connect(delAction, &QAction::triggered, this, [this, picked]() {
        if (!picked.isEmpty()) Q_EMIT deleteRequested(picked.first());
    });

    menu.exec(viewport()->mapToGlobal(pos));
}
