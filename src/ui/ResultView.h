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
// Action policy: copy (Ctrl+C full paths, context-menu Copy Filename) is
// handled internally via the clipboard -- it is self-contained. Open /
// reveal / cut / trash are emitted as signals for MainWindow to handle,
// since they touch the filesystem, D-Bus, or index state this widget
// shouldn't own.
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

    // Context menu per §19: Open, Open Containing Folder, separator, Copy
    // Full Path, Copy Filename. Open is disabled when more than one row is
    // selected. Caller owns the returned menu (parented to `parent`).
    // Actions carry objectNames (openAction/revealAction/copyPathAction/
    // copyNameAction) for tests and shortcut wiring.
    QMenu* BuildContextMenu(QWidget* parent);

    // text/uri-list drag payload for the given rows. Caller owns the result.
    QMimeData* BuildDragMimeData(const QList<int>& rows) const;

signals:
    void OpenRequested(const QString& path);        // Enter / context Open
    void RevealRequested(const QString& path);      // Ctrl+Enter / context reveal
    void CutRequested(const QStringList& paths);    // Ctrl+X
    void TrashRequested(const QStringList& paths);  // Delete

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;

private:
    // Selection when present, else the current row -- keyboard actions work
    // on the focused row even before an explicit selection exists.
    QStringList SelectedFullPathsOrCurrent() const;
    void CopySelectedFullPathsToClipboard() const;
    void CopySelectedFilenamesToClipboard() const;
    void OnHeaderClicked(int section);

    ResultModel* model_ = nullptr;
    int lastSortedColumn_ = -1;
};

}  // namespace indexed
