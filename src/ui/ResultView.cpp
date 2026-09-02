#include "ui/ResultView.h"

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDrag>
#include <QHash>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QList>
#include <QUrl>

namespace indexed {

namespace {

// How a set of files is being placed on the clipboard or drag: a plain
// copy, or a "cut" that a file manager paste turns into a move.
enum class FileTransfer { Copy, Cut, Drag };

// Builds the file-object MIME payload every clipboard/drag path shares
// (docs/adr/0013): text/uri-list for any freedesktop drop target,
// x-special/gnome-copied-files so a Nautilus/Nemo/Caja/Thunar paste does
// the right copy-or-move, and a plain-text path fallback for text editors.
// Drag never carries the gnome cut/copy marker -- a drag is always a copy
// here (ADR 0005) -- but does carry uri-list + text. Caller owns the result.
QMimeData* BuildFileMimeData(const QStringList& paths, FileTransfer transfer) {
    auto* mime = new QMimeData;

    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString& path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    mime->setUrls(urls);
    mime->setText(paths.join('\n'));

    if (transfer != FileTransfer::Drag) {
        QByteArray gnome = transfer == FileTransfer::Cut ? "cut" : "copy";
        for (const QUrl& url : urls) {
            gnome += '\n';
            gnome += url.toString().toUtf8();
        }
        mime->setData("x-special/gnome-copied-files", gnome);
    }
    return mime;
}

}  // namespace

ResultView::ResultView(QWidget* parent) : QTreeView(parent) {
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setAllColumnsShowFocus(true);
    setSortingEnabled(true);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);  // drag out, never accept a drop
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

ResultView::SelectionSnapshot ResultView::SnapshotSelection() const {
    SelectionSnapshot snapshot;
    snapshot.hadFocus = hasFocus();
    if (selectionModel() == nullptr) {
        return snapshot;
    }
    snapshot.selectedPaths = SelectedFullPaths();
    const QModelIndex current = currentIndex();
    if (current.isValid()) {
        snapshot.currentPath = FullPathForRow(current.row());
    }
    return snapshot;
}

void ResultView::RestoreSelection(const SelectionSnapshot& snapshot) {
    if (model_ == nullptr || selectionModel() == nullptr) {
        return;
    }
    if (snapshot.selectedPaths.isEmpty() && snapshot.currentPath.isEmpty()) {
        return;
    }

    // One pass over the fresh snapshot: a result set is at most a few
    // thousand rows and this only runs on a live-monitoring refresh.
    QHash<QString, int> rowForPath;
    const int rows = model_->rowCount();
    rowForPath.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        rowForPath.insert(FullPathForRow(row), row);
    }

    bool first = true;
    for (const QString& path : snapshot.selectedPaths) {
        const auto it = rowForPath.constFind(path);
        if (it == rowForPath.constEnd()) {
            continue;
        }
        const auto flag = first ? QItemSelectionModel::ClearAndSelect : QItemSelectionModel::Select;
        selectionModel()->select(model_->index(*it, 0), flag | QItemSelectionModel::Rows);
        first = false;
    }

    const auto currentIt = rowForPath.constFind(snapshot.currentPath);
    if (currentIt != rowForPath.constEnd()) {
        selectionModel()->setCurrentIndex(model_->index(*currentIt, 0),
                                          QItemSelectionModel::NoUpdate);
    }
    if (snapshot.hadFocus) {
        setFocus(Qt::OtherFocusReason);
    }
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

    QAction* copy = menu->addAction(tr("Copy"));
    copy->setObjectName("copyAction");
    copy->setEnabled(!current.isEmpty());
    connect(copy, &QAction::triggered, this, [this]() { CopySelectionToClipboard(); });

    QAction* cut = menu->addAction(tr("Cut"));
    cut->setObjectName("cutAction");
    cut->setEnabled(!current.isEmpty());
    connect(cut, &QAction::triggered, this, [this]() { CutSelectionToClipboard(); });

    QAction* copyPath = menu->addAction(tr("Copy Full Path"));
    copyPath->setObjectName("copyPathAction");
    connect(copyPath, &QAction::triggered, this, [this]() { CopySelectedFullPathsToClipboard(); });

    QAction* copyName = menu->addAction(tr("Copy Filename"));
    copyName->setObjectName("copyNameAction");
    connect(copyName, &QAction::triggered, this, [this]() { CopySelectedFilenamesToClipboard(); });

    return menu;
}

QMimeData* ResultView::BuildDragMimeData(const QList<int>& rows) const {
    QStringList paths;
    paths.reserve(rows.size());
    for (int row : rows) {
        paths.append(FullPathForRow(row));
    }
    return BuildFileMimeData(paths, FileTransfer::Drag);
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
        CopySelectionToClipboard();
        return;
    }
    if (ctrl && event->key() == Qt::Key_X) {
        CutSelectionToClipboard();
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        const QStringList paths = SelectedFullPathsOrCurrent();
        if (paths.isEmpty()) {
            return;
        }
        // Shift+Delete permanently deletes; plain Delete moves to Trash.
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            emit DeletePermanentlyRequested(paths);
        } else {
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

void ResultView::CopySelectionToClipboard() const {
    const QStringList paths = SelectedFullPathsOrCurrent();
    if (!paths.isEmpty()) {
        QApplication::clipboard()->setMimeData(BuildFileMimeData(paths, FileTransfer::Copy));
    }
}

void ResultView::CutSelectionToClipboard() const {
    const QStringList paths = SelectedFullPathsOrCurrent();
    if (!paths.isEmpty()) {
        QApplication::clipboard()->setMimeData(BuildFileMimeData(paths, FileTransfer::Cut));
    }
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
