# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
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
