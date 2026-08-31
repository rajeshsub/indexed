Status: Accepted

## Context

`docs/adr/0005-qt-drag-and-drop.md` established the mechanism for moving files out of the
result view: `QDrag` + `QMimeData::setUrls()` for drag-out, and, for keyboard clipboard
operations, `QClipboard::setText()` for "Copy full path" plus the
`x-special/gnome-copied-files` construction for cut. That ADR shipped with two gaps that
made the feature not work the way a user expects from a file manager:

1. **Ctrl+C copied only path text.** The plan (§12.2) labels Ctrl+C "Copy full path(s)",
   and the implementation did exactly that: `QClipboard::setText(paths.join('\n'))`. Pasting
   that into Nautilus, Nemo, Caja, Thunar, or Dolphin does nothing -- there is no file on
   the clipboard, only a string. Ctrl+X *did* place a real file-object payload (uri-list +
   `x-special/gnome-copied-files` with a `cut` marker), so the two keys were asymmetric:
   cut-then-paste moved a file, copy-then-paste pasted a filename as text.

2. **Drag-out never initiated.** `ResultView` set `setDragEnabled(true)` and overrode
   `startDrag()`, and the unit tests exercised `startDrag()` / `BuildDragMimeData()`
   directly and passed. But `ResultModel` (a `QAbstractTableModel`) never overrode
   `flags()`, so its rows did not carry `Qt::ItemIsDragEnabled`.
   `QAbstractItemView`'s drag machinery only calls `startDrag()` when the pressed index is
   drag-enabled, so in the running application a press-and-drag on a result row just
   rubber-band-selected -- `startDrag()` was dead code.

Separately, users asked for a permanent-delete affordance alongside the existing
move-to-Trash (`Delete` -> `QFile::moveToTrash`, plan §12.2).

## Options

### Ctrl+C payload

| Option | Fits when | Cost now | Trade-off |
|--------|-----------|----------|-----------|
| a. File-object MIME (uri-list + `x-special/gnome-copied-files` `copy` + plain-text path) | Ctrl+C should behave like a file manager's copy | Low -- same construction Ctrl+X already used, factored into one builder | GNOME-family file managers honour the cut/copy marker; Dolphin (KDE) ignores it and treats the paste as a plain copy, which is a graceful degradation (already accepted in ADR 0005) |
| b. Keep text-only, keep the path string as the payload | Keyboard users who only ever paste into text fields | Zero | Does not do what the user asked; keeps the Ctrl+C / Ctrl+X asymmetry |
| c. File-object MIME **and** `application/x-kde-cutselection` for Dolphin cut parity | Full KDE parity wanted | Small extra | No stated KDE requirement; the platform-testing scope is Fedora XFCE (README); adds a second convention to keep in sync |

### Where the shared payload builder lives

| Option | Trade-off |
|--------|-----------|
| a. Free function in `ResultView.cpp` anonymous namespace | `ResultView` already owns `BuildDragMimeData` and the key handlers; copy/cut become self-contained in the widget (matches the header's "handled internally" policy) and are directly testable via `test_ResultView`. Not reusable outside the TU. |
| b. New `ClipboardPayload` class in its own header/cpp | More ceremony (CMake entry, test file); only worth it if reuse outside `ResultView` is expected, which it is not. |
| c. Keep it in `MainWindow` (emit `CopyRequested` like `CutRequested`) | "filesystem-ish ops in MainWindow" is consistent but wrong here -- clipboard touches nothing but `QClipboard` -- and it makes the widget test unable to cover copy. |

### Making drag-out initiate

| Option | Trade-off |
|--------|-----------|
| a. Override `ResultModel::flags()` to add `Qt::ItemIsDragEnabled` | Canonical Qt fix. `startDrag`/`BuildDragMimeData` stay in the view as the payload builder and are now actually reached. Smallest change that fixes the bug. |
| b. `flags()` **and** move the whole DnD (mimeTypes/mimeData) into the model | More idiomatic Qt MVC, larger change; the view's `startDrag` already worked once reachable, so no benefit here. |

### Shift+Delete

| Option | Trade-off |
|--------|-----------|
| a. Permanent `std::filesystem::remove` behind a confirm dialog | Matches Windows Explorer and GNOME/KDE convention. Trash stays the unconfirmed default (reversible); only the irreversible path prompts. |
| b. Permanent delete, no dialog | One mis-selection destroys data with no undo in a UI where mis-selection is easy. Rejected. |

## Decision

1. **Ctrl+C, Ctrl+X, context Copy, context Cut, and the drag payload all build from one
   function** -- `BuildFileMimeData(paths, transfer)` in `ResultView.cpp`'s anonymous
   namespace (option a / a). It sets `text/uri-list`, a plain-text newline-joined path
   (`setText`), and, for clipboard transfers only, `x-special/gnome-copied-files` with a
   leading `copy` or `cut` line. A drag is always a copy (ADR 0005) and carries no
   cut/copy marker. The `application/x-kde-cutselection` convention is **not** added
   (option b rejected for Ctrl+C payload; option c rejected for KDE parity) -- no stated
   requirement, and Dolphin still gets a working copy-on-paste via `text/uri-list`.

2. **Copy and Cut are handled entirely inside `ResultView`.** The `CutRequested` signal and
   `MainWindow::CutToClipboard` are removed; cut joins copy as a widget-internal clipboard
   operation, since neither touches the filesystem, D-Bus, or index state.

3. **`ResultModel::flags()` is overridden** to return
   `Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled` for a valid index and
   `Qt::NoItemFlags` otherwise (option a). This is what makes `QAbstractItemView` call
   `ResultView::startDrag`. The view additionally sets
   `setDragDropMode(QAbstractItemView::DragOnly)` to state that it drags out and never
   accepts a drop.

4. **Shift+Delete permanently deletes** (option a): `ResultView` emits
   `DeletePermanentlyRequested`; `MainWindow::DeletePermanently` shows a
   `QMessageBox::warning` confirm listing the count/paths, then `std::filesystem::remove`s
   each surviving file, calls `store_.ApplyRemove`, and refreshes the visible query. Plain
   `Delete` is unchanged: move to Trash, no confirm.

This supersedes `docs/adr/0005-qt-drag-and-drop.md`, whose `QDrag` + `text/uri-list`
mechanism and `x-special/gnome-copied-files` cut convention remain in force -- this ADR
extends Ctrl+C to the same file-object payload, records the `flags()` requirement that ADR
0005 missed, and adds Shift+Delete.

## Consequences

- Ctrl+C and Ctrl+X are symmetric: both put a real file-object on the clipboard that pastes
  as a file in a GNOME-family file manager (copy or move respectively), with a plain-text
  path fallback for text editors. Verified in the running app against Thunar.
- Dragging a result row out to a file manager now works (it did not before). Verified by
  dragging into Thunar; also covered by a `test_ResultView` case that drives the real mouse
  gesture and asserts `startDrag` is reached, which fails without the `flags()` override.
- Dolphin (KDE) users get copy-on-paste and drag-copy; a Ctrl+X paste in Dolphin copies
  rather than moves (the `x-special/gnome-copied-files` marker is ignored). This is the
  same graceful degradation ADR 0005 already accepted.
- Shift+Delete is irreversible and always confirms. A future change that needs a
  "don't ask again" option should not remove the confirm for the multi-file case.
- `plan §12.2` gains a Shift+Delete row; `README.md` shortcuts section is updated;
  `CHANGELOG.md` records the fix.
