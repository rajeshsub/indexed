# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed
- Sorting by the Size column now stays active across a new search. Sorting,
  then typing a different query, silently reverted the results to unsorted
  order while the Size column header still showed the sort as active, so a
  second click on it appeared to do nothing.

### Fixed
- Folders removed in Settings no longer reappear after a restart. The index
  file stored removed entries as if they were still present, so a drive
  indexed under one mount point (for example `/media/veracrypt6`) and later
  re-added under another (`/media/veracrypt10`) showed up under both.
- Elevated mode now saves the index again. Since 0.3.1's crash-safe saves,
  every save by `indexed-helper` failed silently, so rebuilds and settings
  changes made while elevated never reached the search window.
- The search window now picks up every index update from the elevated
  helper, not just the first one.
- Index saves now also sync the containing directory, so a crash right after
  a save can no longer lose it.
- In elevated mode, files created, changed or deleted now show up in search
  within a few seconds. Previously they only appeared after the next full
  rebuild.

### Changed
- The search window no longer freezes during index work. Loading, rebuilding,
  applying Settings changes, reloading the elevated helper's index, and Move
  to Trash / Delete all run in the background; the window stays responsive
  throughout. A rebuild or Settings change requested while another is
  running cancels it and starts over. Files being moved to Trash or deleted
  are greyed out until done, and failures are listed in a single dialog.
- Closing the window during a rebuild is now immediate: the scan is
  cancelled. If a file is being moved to Trash or deleted, the app finishes
  that before it exits.
- Declining the password prompt for full-system access (or the helper
  exiting) now returns to indexing locally instead of leaving the app
  waiting on a helper that isn't running.
- Typing no longer waits for the previous search to wind down.
- Reveal in File Manager no longer blocks when the file manager is slow or
  missing.
- UI freezes longer than 250 ms are now written to the log as WARNINGs.
- `indexed.log` must now belong to you rather than root. If an earlier
  version of `indexed-helper` created it as root, it is replaced on the next
  elevation and its old contents are discarded.

### Security
- `indexed-helper` (root) no longer follows a path the user controls when
  opening its log, status, settings or index files: every directory is
  opened relative to an already-checked parent, so swapping one for a symlink
  mid-operation is refused. A pipe, device or hard link planted at one of
  those names is refused instead of opened.
- A crafted index file can no longer crash `indexed-helper` or make it read
  out of bounds: every record is checked against the file's actual size.

## [0.3.1] - 2026-09-18

### Fixed
- Selected result and keyboard focus no longer drop every few seconds under
  a monitored, actively-changing folder: a live-monitoring refresh reset the
  result list wholesale, losing selection/focus even when the selected file
  was untouched. Selection now follows the row by path across the refresh.
- Index persistence is now crash-safe: the on-disk index is written to a
  temp file, fsynced, and renamed into place instead of being truncated in
  place, so an interruption mid-write can no longer destroy the last good
  index.
- Fixed a heap-use-after-free: saving the index while a live-monitoring
  mutation was landing on another thread could read the index pool's
  backing storage mid-reallocation. Both save paths now hold the same lock
  as every other pool access.
- Benchmark delta table now actually shows a comparison instead of always
  reporting "nothing to compare against" (a CI cache-key collision between
  two unrelated jobs).

### Changed
- The serialized index format now rejects files with trailing bytes past
  the declared payload, tightening the existing CRC-based validation.
- Documented, in the README, two already-deliberate but previously implicit
  policies: symlinks are never indexed, and the persisted index is a
  monitored cache, not a transactional snapshot. Tombstone reclamation is
  confirmed rebuild-only (no separate compaction pass) and recorded as a
  permanent decision in `docs/adr/0007`.

Full test suite green; clang-format and cppcheck clean; new tests cover
persistence-failure paths, sustained mutation churn, and concurrent
index reload against live monitoring (the last of which is what caught
the use-after-free above).

## [0.3.0] - 2026-08-31

### Added
- `Shift+Delete` in the result list permanently deletes the selected file(s)
  after a confirmation dialog; plain `Delete` still moves to Trash without
  asking (`docs/adr/0013`).
- Right-click Copy and Cut entries in the result context menu, matching the
  `Ctrl+C` / `Ctrl+X` behaviour.

### Changed
- `Ctrl+C` now copies the file object, not just its path: pasting into
  Nautilus, Nemo, Caja, or Thunar copies the actual file, while a plain-text
  path is still available for text editors. `Ctrl+X` is its move-on-paste
  counterpart. Supersedes `docs/adr/0005` with `docs/adr/0013`.

### Fixed
- Live monitoring changes now reach the results view. A file created, copied
  in from a USB drive or network share, or deleted under a monitored folder
  did not change the visible search results until a manual "Rebuild Index
  Now": `WalkScanner` could not scan a single-file path (so newly added files
  were never indexed), and even a correct index change did not refresh the
  on-screen query. Both are fixed; bursts of changes are coalesced into one
  refresh.
- Live monitoring no longer loses files that already exist inside a directory
  the moment it appears. `mkdir d && touch d/f`, or moving an already-populated
  folder into a watched location, created a window between the directory
  existing and `inotify` watching it during which the kernel reported nothing;
  `InotifyWatcher` now walks the new directory's current contents and reports
  them, in addition to watching it for future changes.
- Dragging a result row out to a file manager now works. `ResultModel` did
  not mark its rows drag-enabled, so the view never started the drag.
- Crash on first launch: `IndexStore::AddEntry` was called concurrently by
  `WalkScanner`'s worker threads without a lock, corrupting the staging pool's
  heap ("double free or corruption"). It now takes the same exclusive lock as
  every other store mutator.
- Adding a root in Settings now behaves like a real reindex: it shows
  "Indexing…" and disables search while scanning, honors excluded folders,
  persists the updated index to disk (previously a restart within the reindex
  interval silently resurrected the pre-change index), and restarts live
  monitoring so the new root is watched.
- Clearing the search box (or backspacing below 2 characters) now clears the
  result list instead of leaving the previous query's results on screen; a
  search still in flight when the box is cleared can no longer repopulate it.
- Token-set search now matches per-token substrings, not exact tokens:
  `just rosy guit` keeps matching `LedZep_Just-Rosy_June-Bug_guitar.flac`
  while the last word is still being typed (search-as-you-type parity with
  winindex's documented behavior). Query tokens still never match across a
  separator.
- AppImage no longer segfaults at startup on distros built with RELR
  relocations (Fedora 40+, Ubuntu 24.04+): linuxdeploy's bundled patchelf
  0.15.0 silently corrupted every bundled library; the build script now
  fetches patchelf 0.19.1 and points linuxdeploy at it via `$PATCHELF`.
- `packaging/indexed.desktop` no longer lists two main XDG menu categories.

### Changed
- RegexSearch: two-phase RE2 matching -- a cheap boolean-only `PartialMatch` filters every
  entry first (RE2 takes its fast DFA-only path when no output is requested), and only
  entries that pass pay the slower capturing call to extract the match span. ~1.75-1.8x
  throughput at 100k/1M-entry scale.
- TokenSetSearch: name-token spans are now cached in `IndexPool` at index-build/load time
  instead of being re-tokenized on every search (`docs/adr/0012-cached-name-token-spans.md`).
  ~1.75x throughput at 1M-entry scale; `matchPath`/`ignoreDiacritics` searches are unaffected
  (still live-tokenized, as before).

### Added
- CI: benchmark results are now rendered as a markdown table directly in the job summary,
  readable without expanding the "Run benchmarks" step log.
- Packaging: AppStream metainfo, app icon set (`packaging/icons/`), AppImage build
  script (`packaging/appimage/build-appimage.sh`, linuxdeploy + linuxdeploy-plugin-qt),
  CI `release` job that builds and attaches the AppImage to a GitHub Release on `v*`
  tags. README rewritten with Settings/Shortcuts/Releases/privileged-monitoring
  documentation (M6).
- Live monitoring & privileged helper: `InotifyWatcher` (unprivileged, default),
  `FanotifyMonitor` + `indexed-helper` binary for full-system monitoring, pkexec/polkit
  session-lifetime elevation with `PKEXEC_UID`-based root-write hardening, signal/status-file
  control channel, GUI index reload on change, mountinfo-poll hotplug detection (M5).
- Qt GUI: `MainWindow` with debounced search, virtual `ResultModel`/`ResultView`
  (Name/Path/Size/Date, sortable, drag-out), `SearchLineEdit`, open/reveal/copy/cut/trash
  actions, First-Run and Settings dialogs, About dialog (M4).
- Scanner & indexer: `WalkScanner` (parallel `getdents64`/`statx` walk with exclusions
  and symlink/mount handling), `Indexer` orchestration (build/load/save/stale/incremental),
  `MountEnumerator`, `Settings`/`IniFile`/`PathUtils`/`Logger` (M3).
- Search engine: `SearchEngine`, `SimdSearch` (AVX2/SSE4.2 runtime dispatch, scalar on
  aarch64), `TokenMatcher`, utf8proc case-fold/diacritics handling, `ISearchEngine` (M2).
- Core data model & storage: `FileEntry`/`EntryMeta`, `IndexPool`, `IndexStore`,
  `IndexSerializer` with binary format v1 and round-trip tests (M1).
- Project scaffold: top-level CMake + presets, FetchContent dependencies (googletest,
  abseil, re2, utf8proc), `indexed_core`/`indexed`/`indexed-helper` targets, CI, repo
  hygiene files, and ADRs recording the structural decisions made before implementation
  (M0).
