# Architecture

Current system shape. For *why* a decision was made, see `docs/adr/`; for the full
spec/rationale, see `indexed-plan.md`. This document tracks *how the system is now* and
is updated when structure changes (engineering-standards rule 19).

## Components

| Directory | CMake target | Depends on | Responsibility |
|-----------|-------------|------------|----------------|
| `src/core/storage/` | part of `indexed_core` | -- | On-disk/in-memory index: `EntryMeta`, `IndexPool` (flat byte-pool layout, `docs/adr/0006`), `IndexStore`, `IndexSerializer` (binary format, `docs/adr/0003`), CRC-32 validation |
| `src/core/search/` | part of `indexed_core` | `storage/` | Query execution: `ISearchEngine` contract, `TokenMatcher` (word-set matching), `SimdSearch` (AVX2/SSE4.2 substring search with runtime dispatch), RE2-backed regex (`docs/adr/0001`) |
| `src/core/indexer/` | part of `indexed_core` | `storage/`, `search/`, `platform/` | Orchestration: `WalkScanner` (`getdents64`+`statx` directory walk, `docs/adr/0002`), `Indexer` (scan/load/monitor control flow), `FanotifyMonitor`/`InotifyWatcher` (`IChangeMonitor` implementations, `docs/adr/0007`), `StatusFile` (GUI<->helper status handoff) |
| `src/core/platform/` | part of `indexed_core` | -- | `MountEnumerator` (mount/hotplug detection via `/proc/self/mountinfo` + libblkid), `Elevation` (pkexec/PKEXEC_UID handling, `docs/adr/0008`) |
| `src/core/settings/` | part of `indexed_core` | -- | `Settings`+`IniFile` (persisted config, schema in `indexed-plan.md` §12.1), `PathUtils` (XDG/portable-mode data dir resolution), `Logger` (severity-filtered append log, `docs/adr/0009`) |
| `src/ui/` | `indexed_ui_core`, `indexed_ui_widgets`, `indexed` (executable) | `indexed_core`, Qt 6 (Widgets, DBus) | `MainWindow`, search bar, result view/model, Settings/FirstRun dialogs. Only process in the system that is never privileged. |
| `src/helper/` | `indexed-helper` (executable) | `indexed_core` | Privileged process: runs the actual directory scan and live-monitoring, elevated via `pkexec` for the lifetime of the GUI session |

`indexed_core` is Qt-free C++20 (testable without a display); `indexed_ui_*` is the only
Qt-dependent code.

## Boundaries and data flow

```
   WalkScanner (getdents64)
           |
           | scan
           v
        Indexer  ----------->  IndexStore (memory + .idx)
                    index               |
   FanotifyMonitor  -------> (apply     | entries
   InotifyWatcher    change)  changes)  |
                                        v
                               SearchEngine (RE2 / SIMD / token)
                                        |
                                        v
                                 Qt MainWindow
```

- **`indexed` (GUI, unprivileged)** and **`indexed-helper` (privileged)** are two separate
  OS processes. The GUI never runs as root; `indexed-helper` is launched once via
  `pkexec` for the GUI session's lifetime (`docs/adr/0008`).
- **GUI <-> helper channel:** Unix signals (`SIGHUP` rescan, `SIGUSR1`, `SIGTERM` shutdown)
  plus a shared `StatusFile` for progress. The on-disk index (`.idx`) is whole-file-reload,
  not a delta protocol -- the GUI loads/searches it in-process; it never talks to the
  helper's in-memory state directly.
- **Fallback:** without root, `indexed-helper` cannot use `fanotify` and falls back to
  per-root `inotify` watches (`docs/adr/0007`); the GUI remains fully functional either
  way, only live-monitoring coverage differs.

## Key runtime flows

1. **Startup:** GUI resolves data dir (`PathUtils`, portable-mode or XDG) -> loads
   `Settings` -> if a valid non-stale `.idx` exists, loads it and attaches monitoring;
   otherwise triggers a scan via `indexed-helper`.
2. **Search:** query text -> `SearchEngine` dispatches to RE2 (regex mode), SIMD substring
   search, or token-set matching (word-level, e.g. `just rosy guitar`) depending on active
   search modes -> results streamed into `ResultModel`/`ResultView`.
3. **Live update:** filesystem change -> `FanotifyMonitor`/`InotifyWatcher` in
   `indexed-helper` -> applied into `IndexStore` under lock -> GUI notified to refresh the
   current query so new/changed/removed files reflect instantly.
4. **Settings change:** GUI diffs old vs. new `SelectedRoots`/`ExcludedPaths` -> incremental
   `IndexPaths`/`RemovePaths` on `Indexer`, not a full rebuild.

## External dependencies

| Dependency | Brought in via | Purpose |
|------------|----------------|---------|
| Qt 6 (Widgets, DBus, Test) | system `find_package` | GUI toolkit; DBus for `FileManager1` reveal-in-folder |
| RE2 | FetchContent (`docs/adr/0004`) | Regex search backend |
| abseil-cpp | FetchContent | RE2 dependency |
| utf8proc | FetchContent | UTF-8 case-folding/diacritics decomposition |
| GoogleTest/GoogleMock | FetchContent | Test suite |
| libblkid, libudev | system `pkg_check_modules` (optional) | Mount labels, hotplug events |
| linuxdeploy, linuxdeploy-plugin-qt, patchelf | downloaded at AppImage build time (`packaging/appimage/build-appimage.sh`) | AppImage packaging, not a runtime dependency |

## Performance instrumentation

`benchmarks/` (Google Benchmark, `docs/adr/0011`), built only with `-DBUILD_BENCHMARKS=ON`:
`bench_SearchEngine` (substring/token/regex search modes over a synthetic `IndexPool`) and
`bench_WalkScanner` (directory-walk scan over a synthetic on-disk tree). CI's `benchmarks`
job runs both plus test-suite wall-clock duration on every push to `main`, tracked as
trend charts via `benchmark-action/github-action-benchmark` (cached history, no gh-pages
write needed).
