#pragma once

#include <QMenu>
#include <QMimeData>
#include <QTreeView>

#include "ui/ResultModel.h"

namespace indexed {

// QTreeView over ResultModel (indexed-plan.md §19): Name/Path/Size/Date
// columns at 250/350/90/140, full-row extended selection, alternating rows,
// click-to-sort with Size descending-first, header drag-reorder, drag-out as
// text/uri-list, context menu + keyboard actions.
//
// Action policy: everything that is pure clipboard or drag -- Ctrl+C /
// Ctrl+X (file-object copy/cut, docs/adr/0013), context Copy / Cut / Copy
// Full Path / Copy Filename, and the drag payload -- is handled internally;
// it touches nothing but QClipboard/QDrag. Open / reveal / trash / permanent
// delete are emitted as signals for MainWindow, since they touch the
// filesystem, D-Bus, or index state this widget shouldn't own.
class ResultView : public QTreeView {
    Q_OBJECT

public:
    explicit ResultView(QWidget* parent = nullptr);

    // setModel + the §19 column widths (widths only apply once a model
    // provides the columns, hence a wrapper instead of raw setModel).
    void SetResultModel(ResultModel* model);
    ResultModel* Model() const;

    // "<parentDir>/<name>", collapsing the doubled slash when parentDir is
    // the filesystem root.
    QString FullPathForRow(int row) const;
    QStringList SelectedFullPaths() const;

    // Selected rows + the current row, keyed by full path so they survive a
    // ResultModel::SetEntries (a full model reset that otherwise drops
    // selection, current index, and keyboard focus). A live-monitoring
    // refresh re-runs the query while the user may be mid-interaction with a
    // row; RestoreSelection puts them back on the same file afterwards.
    struct SelectionSnapshot {
        QStringList selectedPaths;
        QString currentPath;
        bool hadFocus = false;
    };
    SelectionSnapshot SnapshotSelection() const;
    void RestoreSelection(const SelectionSnapshot& snapshot);

    // Context menu per §19 + docs/adr/0013: Open, Open Containing Folder,
    // separator, Copy, Cut, Copy Full Path, Copy Filename. Open is disabled
    // when more than one row is selected. Caller owns the returned menu
    // (parented to `parent`). Actions carry objectNames (openAction/
    // revealAction/copyAction/cutAction/copyPathAction/copyNameAction) for
    // tests and shortcut wiring.
    QMenu* BuildContextMenu(QWidget* parent);

    // File-object drag payload (text/uri-list + x-special/gnome-copied-files
    // + plain-text path) for the given rows. Caller owns the result.
    QMimeData* BuildDragMimeData(const QList<int>& rows) const;

signals:
    void OpenRequested(const QString& path);                    // Enter / context Open
    void RevealRequested(const QString& path);                  // Ctrl+Enter / context reveal
    void TrashRequested(const QStringList& paths);              // Delete
    void DeletePermanentlyRequested(const QStringList& paths);  // Shift+Delete

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;

private:
    // Selection when present, else the current row -- keyboard actions work
    // on the focused row even before an explicit selection exists.
    QStringList SelectedFullPathsOrCurrent() const;
    // Ctrl+C / context Copy and Ctrl+X / context Cut: put the file-object
    // MIME (copy or cut variant) for the current selection on the clipboard.
    void CopySelectionToClipboard() const;
    void CutSelectionToClipboard() const;
    void CopySelectedFullPathsToClipboard() const;
    void CopySelectedFilenamesToClipboard() const;
    void OnHeaderClicked(int section);

    ResultModel* model_ = nullptr;
    int lastSortedColumn_ = -1;
};

}  // namespace indexed
