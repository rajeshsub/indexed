#include "ui/ResultView.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDrag>
#include <QHeaderView>
#include <QKeyEvent>
#include <QUrl>

namespace indexed {

ResultView::ResultView(QWidget* parent) : QTreeView(parent) {
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setAllColumnsShowFocus(true);
    setSortingEnabled(true);
    setDragEnabled(true);
    header()->setSectionsMovable(true);
    // Size sorts descending-first (§19): intercept header clicks to flip the
    // indicator when the user first lands on the Size column. Qt's default
    // is ascending-first on every column.
    connect(header(), &QHeaderView::sectionClicked, this, &ResultView::OnHeaderClicked);
}

void ResultView::SetResultModel(ResultModel* model) {
    model_ = model;
    // No initial sort: results display in engine order until the user
    // clicks a header. setModel with sorting enabled would immediately
    // re-sort the model by whatever indicator the header carries, so
    // sorting is suspended around the swap and the indicator cleared.
    setSortingEnabled(false);
    setModel(model);
    header()->setSortIndicator(-1, Qt::AscendingOrder);
    setSortingEnabled(true);
    setColumnWidth(ResultModel::kName, 250);
    setColumnWidth(ResultModel::kPath, 350);
    setColumnWidth(ResultModel::kSize, 90);
    setColumnWidth(ResultModel::kDateModified, 140);
}

ResultModel* ResultView::Model() const {
    return model_;
}

QString ResultView::FullPathForRow(int row) const {
    if (model_ == nullptr || row < 0 || row >= model_->rowCount()) {
        return {};
    }
    const DisplayEntry& entry = model_->EntryAt(row);
    QString dir = QString::fromStdString(entry.parentDir);
    if (!dir.endsWith('/')) {
        dir += '/';
    }
    return dir + QString::fromStdString(entry.name);
}

QStringList ResultView::SelectedFullPaths() const {
    QStringList paths;
    if (selectionModel() == nullptr) {
        return paths;
    }
    const QModelIndexList rows = selectionModel()->selectedRows();
    paths.reserve(rows.size());
    for (const QModelIndex& index : rows) {
        paths.append(FullPathForRow(index.row()));
    }
    return paths;
}

QMenu* ResultView::BuildContextMenu(QWidget* parent) {
    auto* menu = new QMenu(parent);
    const QStringList selected = SelectedFullPaths();
    // Fall back to the current row when nothing is multi-selected (a bare
    // right-click moves currentIndex without necessarily selecting).
    const QString current =
        selected.isEmpty() ? FullPathForRow(currentIndex().row()) : selected.first();

    QAction* open = menu->addAction(tr("Open"));
    open->setObjectName("openAction");
    open->setEnabled(selected.size() <= 1 && !current.isEmpty());
    connect(open, &QAction::triggered, this, [this, current]() { emit OpenRequested(current); });

    QAction* reveal = menu->addAction(tr("Open Containing Folder"));
    reveal->setObjectName("revealAction");
    reveal->setEnabled(!current.isEmpty());
    connect(reveal, &QAction::triggered, this,
            [this, current]() { emit RevealRequested(current); });

    menu->addSeparator();

    QAction* copyPath = menu->addAction(tr("Copy Full Path"));
    copyPath->setObjectName("copyPathAction");
    connect(copyPath, &QAction::triggered, this, [this]() { CopySelectedFullPathsToClipboard(); });

    QAction* copyName = menu->addAction(tr("Copy Filename"));
    copyName->setObjectName("copyNameAction");
    connect(copyName, &QAction::triggered, this, [this]() { CopySelectedFilenamesToClipboard(); });

    return menu;
}

QMimeData* ResultView::BuildDragMimeData(const QList<int>& rows) const {
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    urls.reserve(rows.size());
    for (int row : rows) {
        urls.append(QUrl::fromLocalFile(FullPathForRow(row)));
    }
    mime->setUrls(urls);
    return mime;
}

void ResultView::keyPressEvent(QKeyEvent* event) {
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);

    if (enter && ctrl) {
        const QString path = FullPathForRow(currentIndex().row());
        if (!path.isEmpty()) {
            emit RevealRequested(path);
        }
        return;
    }
    if (enter) {
        // Open only fires for a single selection, mirroring the context
        // menu's Open policy (§19).
        const QStringList selected = SelectedFullPaths();
        if (selected.size() == 1) {
            emit OpenRequested(selected.first());
        } else if (selected.isEmpty()) {
            const QString path = FullPathForRow(currentIndex().row());
            if (!path.isEmpty()) {
                emit OpenRequested(path);
            }
        }
        return;
    }
    if (ctrl && event->key() == Qt::Key_C) {
        CopySelectedFullPathsToClipboard();
        return;
    }
    if (ctrl && event->key() == Qt::Key_X) {
        const QStringList paths = SelectedFullPathsOrCurrent();
        if (!paths.isEmpty()) {
            emit CutRequested(paths);
        }
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        const QStringList paths = SelectedFullPathsOrCurrent();
        if (!paths.isEmpty()) {
            emit TrashRequested(paths);
        }
        return;
    }
    QTreeView::keyPressEvent(event);
}

void ResultView::contextMenuEvent(QContextMenuEvent* event) {
    if (model_ == nullptr || model_->rowCount() == 0) {
        return;
    }
    QMenu* menu = BuildContextMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(event->globalPos());
}

void ResultView::startDrag(Qt::DropActions supportedActions) {
    const QModelIndexList rows = selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    QList<int> rowNumbers;
    rowNumbers.reserve(rows.size());
    for (const QModelIndex& index : rows) {
        rowNumbers.append(index.row());
    }
    auto* drag = new QDrag(this);
    drag->setMimeData(BuildDragMimeData(rowNumbers));
    drag->exec(supportedActions, Qt::CopyAction);
}

QStringList ResultView::SelectedFullPathsOrCurrent() const {
    QStringList paths = SelectedFullPaths();
    if (paths.isEmpty()) {
        const QString current = FullPathForRow(currentIndex().row());
        if (!current.isEmpty()) {
            paths.append(current);
        }
    }
    return paths;
}

void ResultView::CopySelectedFullPathsToClipboard() const {
    const QStringList paths = SelectedFullPathsOrCurrent();
    if (!paths.isEmpty()) {
        QApplication::clipboard()->setText(paths.join('\n'));
    }
}

void ResultView::CopySelectedFilenamesToClipboard() const {
    if (model_ == nullptr || selectionModel() == nullptr) {
        return;
    }
    QModelIndexList rows = selectionModel()->selectedRows();
    if (rows.isEmpty() && currentIndex().isValid()) {
        rows.append(currentIndex());
    }
    QStringList names;
    names.reserve(rows.size());
    for (const QModelIndex& index : rows) {
        names.append(QString::fromStdString(model_->EntryAt(index.row()).name));
    }
    if (!names.isEmpty()) {
        QApplication::clipboard()->setText(names.join('\n'));
    }
}

void ResultView::OnHeaderClicked(int section) {
    // First landing on the Size column flips to descending (biggest files
    // first is the useful default); subsequent clicks on it toggle normally.
    if (section == ResultModel::kSize && lastSortedColumn_ != ResultModel::kSize) {
        header()->setSortIndicator(ResultModel::kSize, Qt::DescendingOrder);
    }
    lastSortedColumn_ = section;
}

}  // namespace indexed
