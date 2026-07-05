Status: Accepted

## Context

Users want to drag files out of the search result view and drop them onto a file manager
(Nautilus, Thunar, Dolphin, the desktop), and to "cut" a file via keyboard so a paste in a
file manager moves it. winindex uses Windows OLE drag-and-drop (`IDataObject`/
`IDropSource`, `CF_HDROP`) for this; Linux has no OLE, and the desktop-integration
conventions differ by toolkit/DE.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. Qt `QDrag` + `QMimeData` (`text/uri-list`) | Standard Linux desktop drag-and-drop, works with GTK/Qt file managers alike | Low — Qt's built-in DnD API, no COM-equivalent boilerplate | N/A — this is the idiomatic mechanism | Relies on the target file manager implementing the freedesktop `text/uri-list` convention (near-universal) |
| b. Clipboard-only (Ctrl+C / Ctrl+X already implemented) | Keyboard-centric users only | Zero | None | No mouse drag; not discoverable, same limitation winindex explicitly avoided |
| c. Raw X11/Wayland protocol-level DnD (bypass Qt) | Need behavior Qt's DnD API can't express | Very high — reimplementing what Qt already does correctly across both display protocols | N/A | No known requirement justifies this; Qt6's DnD already works on both X11 and Wayland |

## Decision

Use **Qt's `QDrag` + `QMimeData::setUrls()`** (option a), populating `text/uri-list` (the
freedesktop.org standard MIME type every Linux file manager's drop target understands) —
this replaces winindex's OLE `CF_HDROP`. For "cut" semantics (mark for move, not copy),
additionally set the GNOME/Nautilus-originated but widely-supported
`x-special/gnome-copied-files` MIME type with payload `"cut\n<uri>"`, understood by
Nautilus, Thunar, and Dolphin, alongside the plain `text/uri-list`.

Clipboard copy/cut for `Ctrl+C`/`Ctrl+X` uses `QClipboard::setText()` for path/name text
and the same MIME construction for file-object cut/paste into a file manager.

## Consequences

- Files can be dragged from the result list to any freedesktop-compliant file manager or
  desktop, on both X11 and Wayland (Qt6 handles both display protocols transparently).
- "Cut" (move-on-paste) works in the three major Linux file managers (Nautilus, Thunar,
  Dolphin) via the `x-special/gnome-copied-files` convention; a file manager that
  recognizes neither MIME type falls back to a plain copy semantics on paste, which is a
  graceful degradation, not a crash.
- No COM/OLE-equivalent boilerplate is needed — this is meaningfully less code than
  winindex's `DropSource`/`DropDataObject` classes, since Qt's DnD API is a complete,
  idiomatic replacement.
