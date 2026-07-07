# `indexed` — Implementation Plan (Handoff Document)

> **Status:** Grilled and resolved. All items formerly listed in Open Questions (§18) were
> resolved in a `grill-me` interview; see §18 for the decisions log and `docs/adr/` for the
> structural decisions recorded as ADRs. Ready for M0 implementation.
>
> **Repo:** https://github.com/rajeshsub/indexed · **License:** MIT
> **Reference implementation:** winindex (Windows) — https://github.com/rajeshsub/winindex
> A local checkout of winindex is available at `/home/rajesh/projects/winindex`. Treat it
> as the ground-truth reference for behavior, module boundaries, and UI layout. This plan
> is written to be self-contained, but when in doubt, read the winindex source.
>
> **How to use this doc:** Implement in the **Milestones** order (§16). Every design
> decision below is deliberate and was grilled before being locked in; if new information
> surfaces that invalidates one, raise it rather than silently deviating.

---

## 1. Goal & Product Summary

Build a **desktop file-search application for Linux** that indexes the full filesystem and
returns filename matches **as you type**, with no perceptible delay across millions of files.
It is the Linux port of winindex and must reproduce **all** winindex functionality, adapted
to Linux idioms, and must run across mainstream distributions (developed on Fedora XFCE, but
Ubuntu/Debian, Arch, openSUSE, etc. must all work).

The output utility is named **`indexed`**.

### What the user gets
- A single-window Qt app: a big search box on top, a virtual results list below
  (Name / Path / Size / Date Modified), a status bar at the bottom, and a menu bar.
- Results filter live as they type (150 ms debounce, min 2 chars).
- Search modes: substring, word-token, regex, case-sensitive, whole-word, match-path,
  ignore-diacritics.
- Right-click / keyboard actions: open, open containing folder, copy path, copy filename,
  cut (move), delete (to Trash), drag-and-drop out to file managers.
- A persistent on-disk index that loads instantly on startup and refreshes in the background.

---

## 2. Non-Goals (explicitly out of scope for v0.1.0)

- **File *content* search** (grep-like). This tool indexes **names and paths only**, like
  winindex.
- **Multi-user index sharing / per-user permission filtering.** The privileged indexer sees
  all files and the local user sees all results — same trust model as elevated winindex.
  Designed for a single-user workstation. (Noted as a risk in §17.)
- **Filesystem-specific raw inode reading** (e.g. reading ext4 inode tables directly like
  winindex reads the NTFS MFT). Linux has no portable, safe equivalent on *mounted*
  filesystems. We use a fast parallel directory walk instead (§7.1). Raw-fs scanning is a
  possible future extension, not v0.1.0.
- **macOS / Windows / BSD support.** Linux only.
- **A persistent query daemon / socket server.** The GUI loads and searches the on-disk
  index **in-process** (mirrors winindex). Only the *indexer/monitor* is a separate
  privileged component (§9).
- **A CLI query mode.** Decided during the grill-me pass: `indexed` is **GUI-only**.
  There is no `indexed <query>` command-line entry point, no `src/cli/`, and no
  `indexed --reindex`/`--query` flags. Rebuild and search are GUI-menu-driven only
  (§9, §19). This simplifies the process model (one unprivileged binary, not two) and
  removes an entire interface surface that would otherwise need its own tests and docs.

---

## 3. Target Environment

| Aspect | Target |
|--------|--------|
| OS | Linux, kernel **≥ 5.4** to run at all; **≥ 5.9** for full fanotify whole-mount monitoring (`FAN_REPORT_DFID_NAME`), inotify fallback below that (§8) |
| Primary dev distro | Fedora + XFCE |
| Must also work on | Ubuntu/Debian, Arch, openSUSE, and other mainstream distros |
| Desktop environments | XFCE, GNOME, KDE, others (Qt runs everywhere; file-manager integration is best-effort per DE) |
| Architectures | `x86-64` (primary, with AVX2/SSE4.2 SIMD), `aarch64` (NEON or scalar fallback) |
| Language standard | **C++20** (same as winindex) |
| Filesystems indexed | ext4, btrfs, xfs, f2fs, vfat/exFAT, ntfs3, etc. — anything mounted and readable |
| Filesystems skipped | pseudo/virtual FS (proc, sys, dev, run, tmpfs\*, cgroup, etc.) and network mounts (nfs, cifs, sshfs) by default |

\* `tmpfs` skipped by default except where it hosts real user data — see default exclusions (§12.3).

---

## 4. Technology Stack (with rationale)

| Concern | Choice | Rationale |
|---------|--------|-----------|
| Language | C++20 | Mirrors winindex; performance-critical scanning/search. |
| GUI toolkit | **Qt 6** (Widgets) | Consistent look/behavior across all distros & DEs, rich virtual model/view, built-in DnD, clipboard, `QFile::moveToTrash`, `QDesktopServices`. LGPL-3.0 (dynamic linking keeps us MIT-compatible). |
| Build | CMake **≥ 3.28** + `FetchContent` | Mirrors winindex; no manual dep install for header/small libs. Qt found via system `find_package(Qt6)`. |
| Regex | **RE2** + **abseil** (FetchContent) | Linear-time, DoS-safe; same as winindex (ADR-0001). Cross-platform. |
| Unicode case-fold & diacritics | **utf8proc** (MIT, FetchContent) | Correct UTF-8 lowercasing + NFKD decomposition for `IgnoreDiacritics`. Tiny, MIT — replaces Win32 `towlower`/`CompareString`. |
| Unit tests | **GoogleTest / GoogleMock** (FetchContent) | Mirrors winindex test suite and mocks. |
| Fast scan | Raw **`getdents64`** parallel directory walk + `statx` | No MFT on Linux; this is the portable fast path (§7.1). |
| Live monitoring | **fanotify** (whole-mount, root) + **inotify** fallback | fanotify `FAN_MARK_FILESYSTEM` is the closest analog to the USN journal (§8). |
| Mount enumeration | Parse `/proc/self/mountinfo`; labels via **libblkid** (optional) | Replaces `GetLogicalDrives`/`GetVolumeInformation`. |
| Hotplug detection | Poll `/proc/self/mountinfo` (portable) + optional **libudev** | Replaces `RegisterDeviceNotification`/`WM_DEVICECHANGE`. |
| Trash | `QFile::moveToTrash` (freedesktop Trash spec) | Replaces Recycle Bin. |
| Open / reveal file | `QDesktopServices::openUrl` (xdg-open) + D-Bus `org.freedesktop.FileManager1.ShowItems` | Replaces `ShellExecute` / `SHOpenFolderAndSelectItems`. |
| Elevation | **polkit / `pkexec`**, session-lifetime helper (sole v0.1.0 path — no setcap/systemd) | Replaces the Windows "run as administrator" model (§9). |
| Config storage | Hand-rolled INI in `core` (Qt-free); XDG paths | Keeps `core` free of Qt so tests and the helper don't need Qt. Mirrors winindex INI. |
| Packaging | **AppImage** (+ `.desktop`, icon, AppStream metainfo) | Single portable binary, runs on any modern distro. Closest to winindex's portable ZIP. |
| CI | GitHub Actions on `ubuntu-latest` | Lint (clang-format), build debug/release, ctest, ASAN, coverage, AppImage on tags. |

### Hard architectural rule
**`core/` must not depend on Qt.** Qt appears only in `ui/`. `core/` is plain C++20 + Linux
syscalls + RE2/abseil/utf8proc. This is what lets the privileged helper (`helper/`) and all
unit tests link against `core/` without pulling in Qt.

---

## 5. Windows → Linux Feature Mapping (the heart of the port)

Every winindex capability and its Linux realization. **Nothing here is optional for v0.1.0
unless marked "(future)".**

| # | winindex (Windows) | `indexed` (Linux) |
|---|--------------------|-------------------|
| 1 | UTF-16 `wchar_t`, wide Win32 APIs | **UTF-8 `char`/`std::string`** throughout. Linux paths are byte strings; store UTF-8. Pools become `std::vector<char>`. |
| 2 | MFT direct read (`FSCTL_ENUM_USN_DATA`), NTFS+admin | **Parallel `getdents64` walk** + `statx`. No MFT analog; single fast walker (§7.1). |
| 3 | `FindFirstFile` BFS fallback scanner | Folded into the single `getdents64` walker. `IFileSystemScanner` interface retained for testability & future fs-specific scanners. |
| 4 | USN journal replay + live monitor | **fanotify** `FAN_MARK_FILESYSTEM` + `FAN_REPORT_DFID_NAME` whole-mount live monitor (root). **No historical replay** exists → staleness handled by rescan-if-old (§8). |
| 5 | `ReadDirectoryChangesW` watcher (non-NTFS) | **inotify** recursive watches (fallback when fanotify unavailable / unprivileged). |
| 6 | Recycle Bin (`SHFileOperation` `FOF_ALLOWUNDO`) | `QFile::moveToTrash` (freedesktop Trash spec). |
| 7 | `ShellExecute("open", ...)` | `QDesktopServices::openUrl(QUrl::fromLocalFile(...))` (xdg-open). |
| 8 | `SHOpenFolderAndSelectItems` (reveal + select) | D-Bus `org.freedesktop.FileManager1.ShowItems`; fallback: open parent dir via xdg-open. |
| 9 | OLE drag-drop (`CF_HDROP`) | Qt `QDrag` + `QMimeData::setUrls()` → `text/uri-list`. |
| 10 | Copy path/name to clipboard (`CF_UNICODETEXT`) | `QClipboard::setText()`. |
| 11 | Cut (`CF_HDROP` + `CFSTR_PREFERREDDROPEFFECT`=Move) | Clipboard MIME `x-special/gnome-copied-files` = `"cut\n<uri>"` (understood by Nautilus/Thunar/Dolphin) **plus** `text/uri-list`. |
| 12 | `RegisterDeviceNotification` / `WM_DEVICECHANGE` hotplug | Poll `/proc/self/mountinfo` for mount add/remove (+ optional libudev). Emit "drive connected — add it in Settings". |
| 13 | `GetLogicalDrives` + NTFS/FAT32 detection | Parse `/proc/self/mountinfo`; fstype string; label via libblkid; `removable` via `/sys/block/*/removable`. |
| 14 | `%APPDATA%\winindex\winindex.ini` | XDG paths (§11). Config in `$XDG_CONFIG_HOME/indexed/`, index+log in `$XDG_CACHE_HOME/indexed/`. |
| 15 | Portable mode (`winindex.ini` next to exe) | `indexed.conf` next to the binary → all data stored beside it. |
| 16 | Custom binary index + CRC-32 (ADR-0003/0006) | Same design, UTF-8 byte pools, new magic/version (§10). |
| 17 | Flat pool layout (`EntryMeta` + nameLower + path pools) | Identical structure, byte offsets instead of `wchar_t` offsets (§10). |
| 18 | SIMD substring (AVX2/SSE4.2/scalar) | Same on x86-64 (GCC/Clang intrinsics, runtime dispatch via `__builtin_cpu_supports`); add aarch64 NEON or scalar. |
| 19 | Token-set word matching | Identical, byte-level UTF-8. Separators: space, `_`, `-`, `.`. |
| 20 | RE2 regex (Alt+1) | Identical (RE2 is cross-platform). |
| 21 | `towlower` case-insensitive | **utf8proc** case-folding into `nameLower` pool. |
| 22 | Ignore-diacritics fold | **utf8proc** NFKD decompose + strip combining marks. |
| 23 | Win32 menus / dialogs / ListView | Qt `QMainWindow`, `QMenuBar`, `QLineEdit`, virtual `QAbstractTableModel` + `QTreeView`/`QTableView`, `QStatusBar`, `QDialog`. |
| 24 | "Run as administrator" for MFT/USN | polkit/`pkexec` elevation of the helper, session-lifetime (§9). |
| 25 | NSIS installer + portable ZIP | AppImage + `.desktop` + icon + AppStream metainfo. |

---

## 6. Architecture Overview

Mirror winindex's clean, interface-driven, dependency-injected structure.

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
                                  (src/ui)  <---  (index + status files on disk,
                                                    loaded in-process)
```

**Process/privilege split (§9):**
- **`indexed`** — Qt GUI. Unprivileged. Loads & searches the on-disk index in-process.
  The only binary a user ever launches directly.
- **`indexed-helper`** — the privileged indexer + fanotify monitor. Launched on demand via
  `pkexec` (decided in grill-me: no setcap/systemd path for v0.1.0 — see
  `docs/adr/0008-privileged-helper-and-elevation.md`), runs as **root**, and lives for the
  duration of the GUI session (launched at first privileged action, stopped via `SIGTERM`
  when the GUI exits). It resolves the invoking user via `PKEXEC_UID` + `getpwuid()` (never
  trusts `$HOME`/`$XDG_*` env vars) and writes the index/status files into *that* user's XDG
  dirs with correct ownership, guarding every write with `O_NOFOLLOW` + ownership checks to
  prevent a symlink-based root write-anywhere (§9.2).

### 6.1 Module / directory layout

```
indexed/
  CMakeLists.txt              # top-level; no WIN32 guard; require Linux
  CMakePresets.json           # linux-gcc-debug / linux-gcc-release / linux-asan / linux-clang
  build.sh                    # convenience wrapper: build.sh [debug|release|test|asan]
  LICENSE                     # MIT
  README.md  CHANGELOG.md
  .clang-format  .clang-tidy  .editorconfig  .gitignore  .pre-commit-config.yaml  .codecov.yml
  Doxyfile
  cmake/                      # helper modules (e.g. CompilerWarnings.cmake)
  docs/adr/                   # ADRs, ported + new (§13)
  packaging/
    indexed.desktop
    indexed.metainfo.xml      # AppStream
    icons/                    # SVG + PNG (16..512)
    appimage/                 # linuxdeploy recipe / build-appimage.sh
    systemd/indexed-helper.service           # optional always-on monitor
    polkit/org.indexed.helper.policy
  src/
    core/
      CMakeLists.txt
      indexer/
        IFileSystemScanner.h          WalkScanner.{h,cpp}          # getdents64 walk
        IChangeMonitor.h              FanotifyMonitor.{h,cpp}      InotifyWatcher.{h,cpp}
        Indexer.{h,cpp}
      search/
        ISearchEngine.h  SearchEngine.{h,cpp}
        SimdSearch.{h,cpp}  SimdSearchAvx2.cpp  SimdSearchNeon.cpp
        TokenMatcher.{h,cpp}
      settings/
        Settings.{h,cpp}  PathUtils.{h,cpp}  Logger.{h,cpp}  IniFile.{h,cpp}
      storage/
        IIndexStore.h  IndexStore.{h,cpp}  IndexPool.{h,cpp}  IndexSerializer.{h,cpp}
      platform/
        MountEnumerator.{h,cpp}   # /proc/self/mountinfo + libblkid
        Trash.{h,cpp}             # (thin; GUI may use QFile::moveToTrash directly)
        Reveal.{h,cpp}            # FileManager1 D-Bus (GUI-side wrapper may live in ui/)
        Elevation.{h,cpp}         # pkexec / capability checks
    ui/            # Qt — depends on core
      CMakeLists.txt
      main.cpp
      MainWindow.{h,cpp}
      SearchLineEdit.{h,cpp}      # QLineEdit subclass: Down/Up -> focus list
      ResultModel.{h,cpp}         # QAbstractTableModel over DisplayEntry snapshot
      ResultView.{h,cpp}          # QTreeView: sorting, DnD, context menu, key handling
      FirstRunDialog.{h,cpp}
      SettingsDialog.{h,cpp}
      resources.qrc
    helper/
      CMakeLists.txt  main_helper.cpp   # privileged indexer/monitor entry point
  tests/
    CMakeLists.txt
    mocks/  MockFileSystemScanner.h  MockIndexStore.h  MockChangeMonitor.h
    test_WalkScanner.cpp  test_Indexer.cpp  test_IndexPool.cpp
    test_IndexSerializer.cpp  test_SearchEngine.cpp  test_TokenMatcher.cpp
    test_Settings.cpp  test_MountEnumerator.cpp  test_PathUtils.cpp  test_main.cpp
```

---

## 7. Component Specs

### 7.1 Scanner — `WalkScanner` (implements `IFileSystemScanner`)

**Interface (ported from winindex `IFileSystemScanner.h`, UTF-8):**
```cpp
struct FileEntry {
    std::string name;        // basename only
    std::string nameLower;   // pre-folded lowercase (utf8proc); may be filled by pool
    std::string path;        // full absolute path
    uint64_t    size;
    uint64_t    lastModified;// nanoseconds since epoch (statx stx_mtime) — pick ONE epoch, document it
    uint32_t    attributes;  // bitfield: is-dir, is-symlink, hidden (name starts with '.'), etc.
};
using ScanCallback     = std::function<void(const FileEntry&)>;
using ProgressCallback = std::function<void(uint64_t filesFound, const std::string& currentDir)>;
struct ScanOptions { std::vector<std::string> rootPaths, excludedPaths; };

class IFileSystemScanner {
public:
  virtual ~IFileSystemScanner() = default;
  virtual bool FastScanAvailable(const std::string& root) const = 0;  // analog of IsMftAvailable
  virtual void Scan(const ScanOptions&, ScanCallback, ProgressCallback,
                    const std::atomic<bool>& cancelToken) = 0;
};
```

**Implementation:**
- Iterative walk using the raw `getdents64` syscall (via `syscall(SYS_getdents64, ...)`) over
  `open(dir, O_RDONLY|O_DIRECTORY)`. Faster than `readdir()` (fewer allocations, batched).
- **Do not cross mount boundaries** unless the mount is itself a selected root. Detect via
  `statx` `stx_mnt_id` (kernel ≥5.8) or by comparing `st_dev`. This prevents descending into
  pseudo/network mounts and avoids double-indexing.
- **Skip symlinks** (don't follow) to avoid loops — mirrors winindex skipping reparse points.
- **Exclusions:** prune any directory whose path matches an excluded prefix (normalize
  trailing `/`). Compare on canonical absolute paths.
- **Metadata:** use `statx` with a mask requesting only `size|mtime|type|mode`. Consider
  `AT_STATX_DONT_SYNC` for speed. `d_type` from `getdents64` often gives file type without a
  stat — use it to skip `statx` on entries where only name is needed (but we need size+mtime
  for columns, so `statx` per file is generally required; batch/parallelize).
- **Parallelism:** a work-stealing thread pool of directory jobs (one root seeds N workers;
  subdirectories are pushed back onto the queue). Target: saturate I/O without thrashing.
  Emit progress periodically (files-found count + current dir) like winindex.
- **Cancellation:** check `cancelToken` between directory batches.
- `FastScanAvailable` returns false for all roots in v0.1.0 (no MFT analog); kept for the
  interface and future fs-specific scanners.

**Honest performance note for the plan reviewer:** With a warm dentry cache and SSD, a full
walk of a large home directory is sub-second to a few seconds. Cold cache on spinning disk is
seek-bound and slower. There is **no** MFT-style shortcut on ext4/btrfs/xfs while mounted.
Do not over-promise "seconds for a 500 GB drive" the way the NTFS MFT path could.

### 7.2 Change Monitoring — `FanotifyMonitor` + `InotifyWatcher` (implement `IChangeMonitor`)

**Interface (replaces `IUsnJournalMonitor` + `ChangeWatcher`):**
```cpp
enum class FileChangeType { Added, Removed, Renamed, Modified };
struct FileChangeEvent { FileChangeType type; std::string path, oldPath; };
using ChangeCallback = std::function<void(const FileChangeEvent&)>;

class IChangeMonitor {
public:
  virtual ~IChangeMonitor() = default;
  virtual bool IsAvailable(const std::string& root) const = 0;
  // Blocks until stopToken set; invokes onChange for each live change on the calling thread.
  virtual void StartMonitoring(const std::string& root, ChangeCallback onChange,
                               const std::atomic<bool>& stopToken) = 0;
};
```

**`FanotifyMonitor` (privileged, preferred):**
- `fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME, O_RDONLY)`.
- `fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_CREATE|FAN_DELETE|FAN_MOVED_FROM|FAN_MOVED_TO|FAN_MODIFY|FAN_ONDIR, AT_FDCWD, mountpoint)`.
- Read events; each carries a `struct fanotify_event_info_fid` with a file handle + name.
  Resolve the parent directory via `open_by_handle_at` (needs `CAP_DAC_READ_SEARCH`) and join
  with the reported name to reconstruct the full path.
- Map `FAN_CREATE`→Added, `FAN_DELETE`→Removed, `FAN_MOVED_FROM`+`FAN_MOVED_TO`→Renamed,
  `FAN_MODIFY`→Modified.
- **Requires kernel ≥ 5.9** (`FAN_REPORT_DFID_NAME`) and `CAP_SYS_ADMIN`.
- **Key difference from USN journal:** fanotify has **no historical replay** — it is
  live-only. There is no "saved USN cursor" to replay missed changes after downtime.
  Therefore: on startup, if the on-disk index is older than the reindex interval (or the
  helper wasn't running), **do a fresh scan**; then start live monitoring. Persist a
  `lastMonitorStop` timestamp so the app can tell the user the index may have drifted.

**`InotifyWatcher` (unprivileged fallback):**
- `inotify_init1`; recursively `inotify_add_watch` on each directory under the roots
  (`IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_MODIFY|IN_CLOSE_WRITE`).
- Maintain a `wd → path` map to reconstruct paths and to add/remove watches as directories
  are created/deleted.
- **Limitation to document & surface in UI:** watches are per-directory and bounded by
  `fs.inotify.max_user_watches`. On huge trees this can be exhausted; when `add_watch` fails
  with `ENOSPC`, log it and set index status to "live monitoring incomplete — periodic
  rescan active". Fall back to interval rescans.

**Selection logic (in `Indexer`):** prefer fanotify when the helper has `CAP_SYS_ADMIN` and
kernel supports it; else inotify; else interval-only rescans.

### 7.3 Storage — `IndexPool`, `IndexStore`, `IndexSerializer`

Port winindex's flat-pool design (ADR-0006) verbatim, converting `wchar_t`→`char` (UTF-8).

**`EntryMeta`** (fixed-size record; keep it tightly packed and cache-friendly):
```cpp
struct EntryMeta {
    uint64_t size;
    uint64_t lastModified;     // ns since Unix epoch (CLOCK_REALTIME-derived; see §7.1/§10)
    uint64_t pathOffset;       // byte offset into pathPool
    uint64_t nameLowerOffset;  // byte offset into nameLowerPool
    uint32_t attributes;
    uint32_t pathLen;          // byte length of full path
    uint32_t nameLowerLen;     // byte length of lowercased name
    uint16_t nameStart;        // byte offset of basename within the path (component-bounded)
    uint8_t  deleted;
    uint8_t  _pad;
};
```
> **Resolved in grill-me (see `docs/adr/0006-pool-based-index-layout.md`):** pool offsets
> are **64-bit** (`pathOffset`/`nameLowerOffset`), permanently removing the ~4 GiB
> single-pool ceiling a 32-bit offset would impose — cheap to pay for (+8 bytes/entry,
> ~80 MB at 10M entries) given this is an on-disk format that's expensive to migrate later.
> `nameStart` stays `uint16_t` (a single path *component* never approaches 65535 bytes).
> `pathLen`/`nameLowerLen` stay `uint32_t` (`PATH_MAX` is 4096, so `uint16_t` would already
> suffice, but the wider type costs nothing here). Add `static_assert(sizeof(EntryMeta) == N)`
> plus a round-trip test in `test_IndexPool`.

**`IndexPool`:** `std::vector<EntryMeta> meta; std::vector<char> nameLowerPool; std::vector<char> pathPool;`
Separate name-lower pool (compact, L3-resident, default search target) and full-path pool
(opt-in `matchPath`). `nameLower` is **not** persisted — rebuilt from path + `nameStart` via
utf8proc at load. Same zero-copy `string_view` accessors.

**`IndexStore`:** `BeginWrite/AddEntry/EndWrite` bulk staging swapped in under an exclusive
`std::shared_mutex`; `ApplyAdd/ApplyRemove/ApplyRename/RemoveEntriesUnderPath` for incremental
changes; `GetPool()`/`GetSearchMutex()` for the search thread (shared lock). Port
`GetIndexAgeSeconds` (staleness) and replace the USN-cursor map (`GetSavedUsn/SetSavedUsn`)
with a `lastMonitorStop` timestamp (fanotify has no cursor — §7.2).

**`IndexSerializer` — on-disk format `indexed.idx` v1** (§10). Custom binary + CRC-32.

### 7.4 Search — `SearchEngine` (implements `ISearchEngine`)

Port winindex `SearchEngine` exactly, byte-level UTF-8:
- **Substring mode:** needle + names pre-folded to lowercase; `SimdFindSubstring` dispatches at
  runtime to AVX2 / SSE4.2 (x86-64) or NEON / scalar (aarch64) via `__builtin_cpu_supports`
  (x86) or compile-time arch selection. Byte-level substring is correct for UTF-8.
- **Token-set mode:** when the query contains a separator (space, `_`, `-`, `.`), split both
  query and filename into tokens; match if **every** query token appears in the filename token
  set. Single-word queries skip this (keeps SIMD fast path). Port `TokenMatcher` verbatim.
- **Regex mode:** RE2 over UTF-8 names (or full paths when `matchPath`). RE2 is natively UTF-8
  — simpler than winindex which had to convert UTF-16→UTF-8 first.
- **Options** (`SearchOptions`): `useRegex, caseSensitive, wholeWord, matchPath, ignoreDiacritics`.
- **Diacritics:** when `ignoreDiacritics`, fold both query and (pre-computed) names via utf8proc
  NFKD + strip combining marks. Decide whether to precompute a diacritic-folded pool or fold
  on the fly (winindex folded on the fly with a per-thread buffer) — **recommend on-the-fly**
  to avoid a 4th pool, matching winindex's `pathLower` deferral.
- **Caps:** max 10 000 results; cooperative early-exit + `cancelToken`; results carry
  `entryIndex, matchStart, matchLen` for highlight.
- **Concurrency:** search runs on a background thread holding a shared read lock for the scan
  duration; results posted back to the UI thread (Qt: `QMetaObject::invokeMethod` /
  queued signal, replacing `PostMessage`).

### 7.5 Settings — `Settings`, `IniFile`, `PathUtils`, `Logger`

Port winindex `Settings` with a small hand-rolled `IniFile` (Qt-free). Schema in §12.
- **Data dir resolution** (`PathUtils`): portable mode if `indexed.conf` sits next to the
  executable (resolve via `/proc/self/exe`) → store everything beside it. Else XDG (§11).
- `EnsureDirectory`, `FormatFileCount` (thousands separators), `FormatAge`
  ("just indexed" / "N min old" / "N hrs old" / "N days, N hrs old"),
  `FormatLocationList` (shorten roots, strip trailing `/`). Port all.
- `Logger`: simple timestamped append log to `<datadir>/indexed.log`.

### 7.6 Mount enumeration & hotplug — `MountEnumerator`

- Parse `/proc/self/mountinfo` → `{ mountPoint, device, fsType, label, removable, isNetwork }`.
- Classify: real fs vs pseudo (skip proc/sys/dev/run/cgroup*/tmpfs\*/...) vs network
  (nfs/nfs4/cifs/smb/sshfs/fuse.sshfs → skip by default).
- `label` via **libblkid** (`blkid_get_tag_value`) or reading `/dev/disk/by-label/` symlinks;
  `removable` via `/sys/block/<dev>/removable`.
- **Hotplug:** monitor `/proc/self/mountinfo` for changes by `poll()`-ing its `fd` for
  `POLLPRI` (mountinfo signals changes via priority events) — portable, no extra deps.
  Optional libudev `monitor` on the `block`/`filesystem` subsystem for richer events. On
  mount add → status "Filesystem <x> mounted — add it in Settings to index it." On remove →
  "Filesystem <x> unmounted." (Mirrors winindex `OnDeviceChange`.)

### 7.7 Indexer orchestration — `Indexer`

Port winindex `Indexer` control flow:
- `StartIndexing(force)`: if a valid, non-stale on-disk index exists and `!force`, load it and
  go straight to monitoring; else scan roots, build pool, save `.idx`, then monitor.
- `IndexPaths(paths)` / `RemovePaths(paths)`: incremental add/remove when Settings change (the
  UI diffs old vs new selected roots — port that logic from `MainWindow::OnCommand`
  `ID_INDEX_SETTINGS`).
- `StartLiveMonitoring`: spin one monitor per root (fanotify or inotify) on background threads;
  apply `FileChangeEvent`s into `IndexStore` under exclusive lock, then notify the UI to
  refresh the current query (so new files appear in results instantly — winindex commit
  "live file monitoring so new files appear instantly").
- Status reporting via a `StatusCallback(IndexerStatus)` (state, message, filesIndexed,
  skippedPaths, locations, indexAgeSeconds) — identical struct to winindex. States:
  `Idle, Scanning, LoadingIndex, WatchingForChanges, Error`.
- **Where it runs:** the scanning + monitoring work is performed by the **`indexed-helper`**
  process (§9), launched via `pkexec` for the lifetime of the GUI session. The GUI's
  `Indexer` instance signals the helper (`SIGHUP`/`SIGUSR1`/`SIGTERM`, §9.3) and watches the
  index + status files for updates; it never talks to the helper any other way.

---

## 8. Live-Monitoring Design Detail (fanotify vs USN — read carefully)

The single biggest semantic gap from winindex. Spell it out so grill-me can probe it:

1. **USN journal (winindex):** persistent, replayable log with a monotonic cursor. On startup
   winindex replays everything since the saved cursor, so it never misses changes across runs.
2. **fanotify (indexed):** live event stream, **no history**. If the monitor isn't running,
   changes are missed silently.

**Consequence & mitigation:**
- The privileged helper should ideally run **continuously** (systemd service option) so live
  monitoring is always on. When it is, the index stays current with zero rescans.
- When the helper is *not* always-on (typical AppImage / pkexec-on-launch case), on each
  startup: (a) load the index, (b) if `now - indexBuildTime > reindexInterval` **or** the
  helper detects it wasn't running continuously, trigger a background rescan, (c) then attach
  live monitoring.
- Persist `lastMonitorStop`; surface drift honestly in the status bar / on-demand rescan.
- inotify path has the same "no history" property **plus** the watch-count ceiling.

**This trade-off must be called out to the user in the README and in an ADR (§13).**

---

## 9. Privilege Model & Helper Process

User chose **"require root for full functionality."** Resolved in grill-me (see
`docs/adr/0008-privileged-helper-and-elevation.md`): **pkexec-on-demand is the sole
delivery path for v0.1.0** — no setcap/systemd alternative is built now. If distro
packaging (deb/rpm/AUR) is pursued later, a setcap-installed fast path can be added as a
follow-on ADR; it isn't designed here because AppImage is the only v0.1.0 packaging target
(§14.2) and setcap cannot apply inside a read-only squashfs anyway.

### 9.1 Two binaries
- **`indexed`** — Qt GUI. **Never runs as root.** Loads/searches the on-disk index
  in-process. The only binary a user launches directly.
- **`indexed-helper`** — the indexer + fanotify monitor. Needs `CAP_SYS_ADMIN` (fanotify
  whole-mount, `open_by_handle_at`) and `CAP_DAC_READ_SEARCH` (traverse/read all files).

### 9.2 Privilege delivery: pkexec-on-demand, session-lifetime helper
- Elevated via **`pkexec indexed-helper`** (polkit policy
  `packaging/polkit/org.indexed.helper.policy`). The helper runs as **root**.
- **Lifecycle:** the GUI launches the helper once — at first privileged action (initial
  index build or enabling live monitoring), not at every action — and it runs for the
  **lifetime of the GUI session**: it performs the initial scan-if-stale, then blocks
  holding the fanotify monitor. The GUI sends `SIGTERM` when it exits (or the user
  explicitly stops monitoring). This means **one polkit prompt per GUI session**, not one
  per action. No systemd service, no persistence across GUI restarts — a fresh session
  re-prompts and re-evaluates staleness (§8's rescan-if-stale mitigation already covers the
  resulting drift, so this isn't a new gap).
- **Root-write safety (closes Risk §17.2 — this is the actual security-critical part):**
  because the helper runs as root but must write into an *unprivileged* user's XDG dirs, it
  must never trust attacker-controllable input to decide where it writes:
  - Resolve the target user via **`PKEXEC_UID`** (set by polkit itself, not by the
    environment the helper inherits) + `getpwuid()` — never `$HOME`/`$XDG_CONFIG_HOME`/etc.,
    which could be spoofed by whatever launched the pkexec call.
  - Open every output path (index file, status file, log) with **`O_NOFOLLOW`**, and refuse
    to write if any path component up to the target XDG directory is not owned by the
    resolved target uid. This closes the classic local-root-helper escalation: a malicious
    local process pre-creating `~/.cache/indexed/indexed.idx` as a symlink to `/etc/shadow`
    before the root helper opens it for writing.
  - Unit-test this in `test_Elevation` (mocked filesystem) — proving the symlink-rejection
    and ownership-check behavior is exactly the kind of security-critical logic that most
    needs a runnable check (engineering-standards rule 15).

### 9.3 GUI ↔ helper interaction
Resolved in grill-me: a fixed, small set of Unix signals plus shared files — no socket, no
D-Bus service, no request/response protocol. Three commands and one status readout don't
justify more machinery (see `docs/adr/0007-fanotify-vs-inotify-monitoring.md` for the
comparison against a socket/D-Bus alternative).

- **`SIGHUP`** → helper re-reads the Settings INI (roots/exclusions changed).
- **`SIGUSR1`** → helper triggers an immediate reindex.
- **`SIGTERM`** → helper stops monitoring and exits cleanly.
- Helper periodically rewrites a small **status file** (state enum, files-found count,
  current directory) that the GUI watches via inotify — this is how scan progress (§19
  status bar) reaches the GUI without a control channel.
- Helper writes/updates `<datadir>/indexed.idx` on completion of a (re)scan or incremental
  change; GUI detects updates by watching the `.idx` file (inotify on the single file) and
  reloads the pool under exclusive lock. The `.idx` file itself stays **whole-file reload**,
  not a delta protocol — resolved as the pragmatic v0.1.0 choice; the status file (above)
  carries progress instead of requiring `.idx` itself to support incremental deltas.

---

## 10. On-Disk Index Format (`indexed.idx`, version 1)

Port ADR-0003 layout, UTF-8, new magic. `#pragma pack(1)` header:

```
Header:
  u32 magic       = 0x44584449   ("IDXD", little-endian bytes 'I','D','X','D')
  u16 version     = 1
  u64 timestamp   = index build time, ns since Unix epoch (CLOCK_REALTIME-derived)
  u64 entryCount
  u32 crc32       = CRC-32 of everything after the header

Payload:
  u64 pathPoolSize  (byte count)
  char pathPool[pathPoolSize]          (UTF-8, no NULs)

  Per-entry disk record (entryCount records):
    u64 size
    u64 lastModified    (ns since Unix epoch)
    u32 attributes
    u64 pathOffset      (byte offset into pathPool)
    u32 pathLen         (byte length)
    u16 nameStart       (byte offset of basename within the path)

  Trailer (replaces winindex USN map):
    u64 lastMonitorStop (ns since Unix epoch; 0 if never monitored)
```
- `nameLower` is **not** stored; rebuilt at load via utf8proc case-fold.
- CRC mismatch or version/magic mismatch → discard and rebuild (silent), exactly like winindex.
- Offset widths and timestamp epoch resolved in grill-me: 64-bit pool offsets (§7.3,
  `docs/adr/0006-pool-based-index-layout.md`), nanoseconds since Unix epoch throughout
  (`docs/adr/0003-binary-index-format.md`).

---

## 11. Paths, Config & Portable Mode (XDG)

| Data | Location (normal) | Portable mode |
|------|-------------------|---------------|
| Config (INI) | `$XDG_CONFIG_HOME/indexed/indexed.conf` (default `~/.config/indexed/indexed.conf`) | `indexed.conf` next to the binary |
| Index (`.idx`) | `$XDG_CACHE_HOME/indexed/indexed.idx` (default `~/.cache/indexed/`) — it's a rebuildable cache | beside the binary |
| Log | `$XDG_STATE_HOME/indexed/indexed.log` (default `~/.local/state/indexed/`) or alongside cache | beside the binary |

- **Portable detection:** `indexed.conf` exists next to `/proc/self/exe` → portable; all three
  live in the executable's directory. Mirrors winindex `IsPortableMode`.
- For the **pkexec-as-root** helper, the target user is resolved via `PKEXEC_UID` +
  `getpwuid()` (never trusted env vars), and the helper writes/chowns the `.idx`, status,
  and log files into *that* user's XDG dirs with `O_NOFOLLOW` + ownership checks on every
  write (§9.2).

---

## 12. Settings Schema, Shortcuts, Defaults

### 12.1 Settings (INI) — mirrors winindex keys

| Key | Default | Meaning |
|-----|---------|---------|
| `SelectedRoots` | chosen on first run | newline-separated absolute paths to index (mounts or folders) |
| `ExcludedPaths` | see §12.3 | newline-separated excluded path prefixes |
| `ReindexIntervalHours` | `48` | staleness threshold; `0` = manual only |
| `UseRegex` | `0` | RE2 regex search |
| `CaseSensitive` | `0` | |
| `WholeWord` | `0` | |
| `MatchPath` | `0` | match full path vs basename |
| `IgnoreDiacritics` | `0` | |
| `FirstRunComplete` | `0` | set after first-run dialog |

> **Resolved in grill-me:** newline-delimited (`\n`), not `:`/`;`/`|` — those are all legal
> bytes in a POSIX filename, so naively splitting on them risks silent corruption. A path
> containing a literal `\n` (vanishingly rare, and never producible via the GUI's own folder
> pickers) is rejected with a clear error at Settings-save time rather than silently
> mis-parsed. Newline-delimiting was chosen over percent-encoding/length-prefixing because
> it keeps `indexed.conf` human-readable and hand-editable, which is the point of using INI.

### 12.2 Keyboard shortcuts (mirror winindex, Linux-idiomatic)

| Key | Action |
|-----|--------|
| Alt+1 | Toggle regex |
| Alt+2 | Toggle case-sensitive |
| Alt+3 | Toggle whole-word |
| Alt+4 | Toggle match-path |
| Alt+5 | Toggle ignore-diacritics |
| Down / Up | Move focus from search box into results list |
| Enter | Open selected file |
| Ctrl+Enter | Open containing folder (reveal + select) |
| Ctrl+C | Copy full path(s) |
| Ctrl+X | Cut (mark for move) |
| Delete | Move to Trash |
| Drag | Drag file(s) out to a file manager (`text/uri-list`) |

### 12.3 Default excluded paths (Linux equivalents of winindex's Windows defaults)

Applied as initial defaults on a fresh install; once the user saves their own list, it's used
as-is (don't re-inject) — same policy as winindex.

- Pseudo-filesystems (always skipped by the walker regardless, but list for clarity):
  `/proc`, `/sys`, `/dev`, `/run`, `/tmp`
- `/var/cache`, `/var/tmp`, `/var/lib/docker`, `/var/lib/containers`
- `/snap`, `/var/lib/flatpak`, `~/.cache`, `~/.local/share/Trash`
- `~/.local/share/containers` (rootless Podman storage) — **added in grill-me**
- `~/.var/app` (per-user Flatpak app data, Fedora's default Flatpak layout) — **added in
  grill-me**
- Lost+found: `/lost+found` (and per-mount `lost+found`)
- Network mounts (nfs/cifs/sshfs) — skipped by mount classification, not path list.

> Do **not** exclude `~/.config`, `~/.local/share` broadly, or user project dirs — users
> search those. Keep the default list conservative (system noise only). Confirmed in grill-me.

---

## 13. Documentation & Repo Hygiene (port + adapt)

Create these so the repo mirrors winindex's professionalism:
- **`LICENSE`** — MIT, copyright **Rajesh Subramanian** (no contact info), year 2026.
- **`README.md`** — same structure as winindex's (Features / How it works / Building /
  Settings / Shortcuts / Project structure / Releases / Development setup / License), rewritten
  for Linux, with a screenshot of the Qt app.
- **`CHANGELOG.md`** — Keep-a-Changelog, `0.1.0` initial entry listing ported features.
- **ADRs** in `docs/adr/` — port and adapt:
  - `0001-use-re2-for-regex.md` (unchanged rationale)
  - `0002-directory-walk-scanning-strategy.md` (**new**, replaces MFT ADR: why getdents64
    walk, why no raw-fs read)
  - `0003-binary-index-format.md` (adapt to UTF-8/new magic)
  - `0004-fetchcontent-dependency-management.md` (adapt; note Qt via system `find_package`)
  - `0005-qt-drag-and-drop.md` (replaces OLE ADR)
  - `0006-pool-based-index-layout.md` (adapt to byte offsets)
  - `0007-fanotify-vs-inotify-monitoring.md` (**new**: the no-replay trade-off §8, GUI-session
    helper lifecycle, signals+status-file control channel, min-kernel floor)
  - `0008-privileged-helper-and-elevation.md` (**new**: pkexec-on-demand only for v0.1.0,
    `PKEXEC_UID`-based root-write hardening, §9)
- **`.clang-format`, `.clang-tidy`, `.editorconfig`, `.gitignore`, `.pre-commit-config.yaml`,
  `.codecov.yml`, `Doxyfile`** — port from winindex, adjust for GCC/Clang/Linux.

---

## 14. Build System & Packaging

### 14.1 CMake
- Top-level `CMakeLists.txt`: require Linux (`if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR ...)`), C++20, no extensions.
- FetchContent: googletest, abseil, re2, utf8proc (after which strict warnings are enabled so
  third-party code isn't held to `-Werror`).
- `find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)` — Qt is a **system** dependency, not
  fetched. `find_package(PkgConfig)` → `blkid` (optional), `libudev` (optional).
- Warnings: `-Wall -Wextra -Wpedantic -Werror` for our targets (mirror MSVC `/W4 /WX`).
- `option(ENABLE_ASAN)` → `-fsanitize=address -fno-omit-frame-pointer` (much simpler than the
  MSVC ASAN dance winindex needed).
- SIMD: compile `SimdSearchAvx2.cpp` with `-mavx2` (and SSE4.2 TU with `-msse4.2`) as separate
  translation units; runtime dispatch selects. **aarch64: scalar fallback only in v0.1.0**
  (resolved in grill-me — NEON deferred as a pure drop-in perf addition later; the dispatch
  interface already isolates this with zero design impact).
- Subdirs: `src/core`, `src/ui`, `src/helper`, `tests`. `enable_testing()`.
- `CMakePresets.json`: `linux-gcc-debug`, `linux-gcc-release`, `linux-clang-release`,
  `linux-asan`.
- `build.sh [debug|release|test|asan]` convenience wrapper (mirror `build.bat`).

### 14.2 Packaging — AppImage
- `packaging/appimage/build-appimage.sh` using **linuxdeploy** + `linuxdeploy-plugin-qt` to
  bundle Qt and produce `indexed-x86_64.AppImage`.
- Include `indexed`, `indexed-helper`, `.desktop`, icon, AppStream `metainfo.xml`.
- **Helper elevation inside AppImage:** ship the polkit policy and elevate via `pkexec`
  (§9.2 — this is the only delivery path for v0.1.0). The AppImage's first privileged action
  prompts via polkit once per GUI session. A setcap-installed fast path for distro packages
  is explicitly deferred (no packaging target for it exists yet); revisit as a follow-on ADR
  if/when deb/rpm/AUR packaging is pursued.
- `.desktop` file: `Name=indexed`, `Exec=indexed %f`, `Icon=indexed`, `Categories=Utility;System;FileTools;`,
  `Keywords=search;find;file;index;`.

### 14.3 CI — `.github/workflows/ci.yml` (GitHub Actions, `ubuntu-latest`)
Jobs (mirror winindex's structure):
1. **lint** — install clang-format (matching `.clang-format`), fail on diff over `src/`.
2. **build-debug** — cache FetchContent `_deps`; configure+build; `ctest --output-on-failure`.
3. **build-release** — build; run tests with coverage (`-DCMAKE_CXX_FLAGS="--coverage"` +
   `lcov`/`gcovr`), upload to Codecov.
4. **build-asan** — `-DENABLE_ASAN=ON`, run tests with `ASAN_OPTIONS=halt_on_error=1`.
   Exclude tests that touch real mounts/OS state (mirror winindex excluding DriveEnumerator/Indexer).
5. **release** (on `v*` tags) — build, produce the AppImage, attach to a GitHub Release via
   `softprops/action-gh-release`.
- Install Qt in CI via `install-qt-action` or distro packages (`qt6-base-dev`, `qt6-...`).
- Add libblkid/libudev dev packages.

---

## 15. Testing Strategy (mirror winindex's gtest suite)

- **Mocks** (`tests/mocks/`): `MockFileSystemScanner`, `MockIndexStore`, `MockChangeMonitor`
  (GoogleMock), so `Indexer` is tested without touching the real filesystem.
- **Unit tests**, one per core module:
  - `test_WalkScanner` — walk a temp dir tree (created in the test), assert entries, exclusions,
    symlink skipping, mount-boundary behavior (best-effort).
  - `test_Indexer` — drives mocks; verifies build→save→load, incremental add/remove, status
    callbacks, staleness logic.
  - `test_IndexPool` — add entries, offsets, accessors, name/path split.
  - `test_IndexSerializer` — round-trip, CRC validation, version/magic rejection, corruption
    → rebuild.
  - `test_SearchEngine` — substring, token-set (the `just rosy guitar` case →
    `LedZep_Just-Rosy_June-Bug_guitar.flac`), regex, all option combinations, diacritics,
    result cap, cancellation.
  - `test_TokenMatcher` — separators, tokenization, all-tokens-present.
  - `test_Settings` / `test_IniFile` — round-trip, defaults, portable mode, list-separator
    edge cases (paths containing the separator!).
  - `test_MountEnumerator` — parse fixture `mountinfo` strings; classification.
  - `test_PathUtils` — `FormatFileCount`, `FormatAge`, `FormatLocationList`, XDG resolution.
  - `test_Elevation` — mocked filesystem proving `PKEXEC_UID` resolution, `O_NOFOLLOW`
    symlink rejection, and ownership-check refusal on root-written output paths (§9.2). This
    is security-critical logic and needs explicit red/green coverage of the attack it closes.
- **Coverage target:** meaningful-logic coverage in the core (winindex used Codecov; keep it).
- Tests must not require root; privileged paths (fanotify) are integration-tested manually /
  behind a guard, and unit-tested via the `IChangeMonitor` mock.
- **M4 GUI test tier (decided when M4 started, not originally specified above):** Qt-free
  presentation logic (`DisplayEntry`/`DisplayFormat`, `ResultModel` data mapping, key-routing
  logic, dialog field validation) is split out of Qt widget code and unit-tested with plain
  gtest, no `QApplication` required (see `indexed_ui_core` target in `src/ui/CMakeLists.txt`).
  Actual Qt widgets are tested with QTest (`QSignalSpy`, simulated key/mouse events) run
  headless via `QT_QPA_PLATFORM=offscreen`, wired into the same CI/pre-commit gates as the
  core suite. Pure-layout chrome (menu/toolbar/status-bar wiring in `MainWindow`) is verified
  manually via `/run`, not automated.

---

## 16. Milestones (recommended execution order)

Each milestone should compile, pass tests, and be committed. TDD where practical.

> **Progress tracker** (kept current as milestones land — check here before resuming
> work in a fresh session): **M0 ✅ · M1 ✅ · M2 ✅ · M3 ✅ · M4 ✅ · M5 ✅ · M6 onward: not started.**
> See `CLAUDE.md` for the workflow conventions (TDD discipline, subagent parallelization,
> verification requirements) that apply to every remaining milestone.

1. **M0 — Scaffold. ✅ DONE.** Repo hygiene files, MIT LICENSE, top-level CMake, `core`/`ui`/
   `helper`/`tests` skeletons (no `cli/` — CLI dropped), FetchContent (gtest/re2/absl/utf8proc),
   Qt found, CI green on an empty build + one trivial test. `.desktop`/icon placeholder.
2. **M1 — Core data model & storage. ✅ DONE.** `FileEntry`, `EntryMeta` (64-bit pool offsets),
   `IndexPool`, `IndexStore`, `IndexSerializer` (+ format v1), full unit tests + round-trip.
   No Qt.
3. **M2 — Search engine. ✅ DONE.** `SearchEngine`, `SimdSearch` (scalar first, then AVX2/SSE4.2;
   aarch64 scalar-only for v0.1.0, NEON deferred), `TokenMatcher`, utf8proc
   case-fold/diacritics, `ISearchEngine`. Full unit tests (`test_SearchEngine`) reading a
   hand-built index prove end-to-end search headless — no CLI needed for this.
4. **M3 — Scanner & indexer. ✅ DONE.** `WalkScanner` (getdents64 + statx, parallel, exclusions,
   symlink/mount handling), `Indexer` orchestration (build/load/save/stale/incremental) with
   mocks. `MountEnumerator`. `Settings`/`IniFile`/`PathUtils`/`Logger` (newline-delimited
   path lists).
5. **M4 — Qt GUI. ✅ DONE.** `MainWindow` (search box + debounce + min-2-chars, virtual `ResultModel`,
   `ResultView` with Name/Path/Size/Date columns, sortable, status bar, menu bar, context
   menu), `SearchLineEdit` (Up/Down → list), open/reveal/copy/cut/trash/drag, First-Run &
   Settings dialogs, About. Wire to core. **Match the winindex screenshot layout** (§ below).
6. **M5 — Live monitoring & privileged helper. ✅ DONE.** `InotifyWatcher` (unprivileged) first; then
   `FanotifyMonitor` (privileged) + the `indexed-helper` binary + pkexec/polkit elevation
   (session-lifetime, §9.2) + `PKEXEC_UID`-based root-write hardening (`test_Elevation`).
   Signals/status-file control channel (§9.3). GUI reloads index on `.idx` change. Hotplug
   via mountinfo poll.
7. **M6 — Packaging & polish. ⬅ NEXT.** AppImage build, AppStream metainfo, README + screenshot,
   CHANGELOG, ADRs finalized, release CI job, tag `v0.1.0`.

   > **M6 handoff notes (2026-07-07):** repo hygiene files (LICENSE/README/CHANGELOG/ADRs/
   > `.clang-format`/etc.) and `.github/workflows/ci.yml` already exist from M0 but are stale
   > placeholders — README says "M0 scaffold" and has no Settings/Shortcuts/Releases sections
   > or screenshot; CHANGELOG has no M1-M5 entries; CI has `lint`/`build-debug`/`build-release`/
   > `build-asan` jobs but is **missing the `release` job** (AppImage-on-tag, §14.3 item 5).
   > `packaging/appimage/` and `packaging/icons/` don't exist yet — new for M6.
   > `packaging/polkit/org.indexed.helper.policy` already exists (M5) and is wired into
   > `src/helper/CMakeLists.txt`'s install rule.
   > Also note the `build-asan` CI job has a stale test exclusion
   > (`ctest -E "MountEnumerator|Indexer"`) with a comment "re-enable once M3 lands with proper
   > mocks" — M3-M5 have since landed; revisit whether that exclusion is still needed.
   > Dev-sandbox tooling check (may differ in CI): ImageMagick (`convert`/`magick`) available
   > for icon rasterization; no `rsvg-convert`/`inkscape`/`linuxdeploy` installed, but network
   > access works, so `linuxdeploy` can be fetched at build time the way the real AppImage
   > build (and CI) will — the sandbox just can't fully dry-run the AppImage build itself.
   > Tagging `v0.1.0` is a git action requiring the developer's go-ahead, not done unilaterally.

---

## 17. Risks (resolved in grill-me — see `docs/adr/` for the structural ones)

1. **No fanotify replay** — index drift across downtime; rescan-if-stale is the mitigation,
   accepted as-is. Compounded by the session-lifetime helper (§9.2): a fresh GUI session
   always re-evaluates staleness, so this is the *only* place drift can occur, not an
   additional gap. (§8)
2. **Root-helper output-path safety** — the AppImage helper runs as root and must write into
   an unprivileged user's XDG dirs without being tricked via symlink or spoofed env vars.
   **Resolved:** `PKEXEC_UID` + `getpwuid()` for target-user resolution, `O_NOFOLLOW` +
   ownership checks on every root-written path, covered by `test_Elevation`. (§9.2)
3. **inotify watch-count ceiling** on large trees when unprivileged. Fallback (interval
   rescan + status-bar notice) accepted as-is; this only bites users who decline the pkexec
   prompt, since the primary path (fanotify via the helper) has no per-directory watch limit.
4. **No true MFT-speed cold scan** — honest performance expectations; don't over-promise. (§7.1)
5. **Multi-user / permission filtering** — privileged indexer sees everything; single-user
   trust model. Out of scope, stated explicitly in the README. (§2)
6. **List-separator ambiguity** — **resolved:** newline-delimited, reject paths containing a
   literal `\n` at save time. (§12.1)
7. **`EntryMeta` field widths** — **resolved:** 64-bit pool offsets, `uint32_t` lengths,
   `uint16_t nameStart`; `static_assert` + round-trip test required in `test_IndexPool`. (§7.3)
8. **Qt licensing** — dynamic-link LGPL Qt to stay MIT-clean; AppImage bundles Qt `.so`s
   (LGPL-compliant with relinkability). Confirmed as drafted.
9. **Wayland vs X11** — **resolved:** test against XFCE (X11) + GNOME (Wayland); reveal
   falls back to `xdg-open <parent-dir>` whenever the `FileManager1` D-Bus call fails
   (covers both "no such interface" and "no D-Bus session").
10. **Time epoch consistency** — **resolved:** nanoseconds since the Unix epoch
    (`CLOCK_REALTIME`-derived) everywhere; assert in tests.

---

## 18. Decisions Log (resolved via grill-me pass, 2026-07-04)

All items formerly listed here as Open Questions were resolved in a structured grilling
interview before implementation began. Structural decisions are additionally recorded as
ADRs in `docs/adr/`; the rest are folded directly into the relevant section above.

1. **Helper delivery:** pkexec-on-demand is the *sole* v0.1.0 path (not systemd, not
   setcap) — see `docs/adr/0008-privileged-helper-and-elevation.md`. Setcap deferred until
   distro packaging is actually pursued.
2. **Helper lifecycle:** tied to the GUI session (launched once via pkexec, holds the
   fanotify monitor, `SIGTERM` on GUI exit) — not a systemd-style always-on service.
3. **GUI↔helper channel:** Unix signals (`SIGHUP`/`SIGUSR1`/`SIGTERM`) + a shared status
   file for progress; `.idx` itself stays whole-file-reload, not a delta protocol — see
   `docs/adr/0007-fanotify-vs-inotify-monitoring.md`.
4. **List separator:** newline-delimited for `SelectedRoots`/`ExcludedPaths` (§12.1).
5. **Default excluded paths:** confirmed §12.3 list plus `~/.local/share/containers` and
   `~/.var/app`, added during grill-me.
6. **CLI:** dropped entirely — `indexed` is GUI-only (§2). No `-q/--query`, no `src/cli/`.
7. **LICENSE:** MIT, copyright **Rajesh Subramanian** (no contact info), version `0.1.0`.
8. **Minimum kernel:** 5.4 floor to run at all; 5.9 for full fanotify whole-mount
   monitoring; inotify fallback below that.
9. **aarch64:** scalar-only in v0.1.0; NEON deferred as a drop-in perf addition.
10. **On-disk index format:** 64-bit pool offsets — see
    `docs/adr/0006-pool-based-index-layout.md`.
11. **Root-helper hardening:** `PKEXEC_UID` + `getpwuid()` + `O_NOFOLLOW` + ownership
    checks — see `docs/adr/0008-privileged-helper-and-elevation.md`.
12. **Wayland test target:** GNOME, with `xdg-open` fallback for reveal-in-folder.
13. **Timestamp epoch:** nanoseconds since Unix epoch, everywhere.

---

## 19. UI Reference — reproduce the winindex layout in Qt

Match the reference screenshot (`/home/rajesh/projects/winindex/src/ui/assets/screenshot.png`):

- **Window:** title `indexed`, ~900×600 default. Menu bar: **Search · Index · Help**.
- **Search box:** full-width, large (~20 px font), directly under the menu, `WS_EX_CLIENTEDGE`
  look (a bordered `QLineEdit`). Auto-focused on launch. Placeholder/status: "Enter a search
  term to begin." Typing < 2 chars → "Type at least 2 characters to search…".
- **Results list:** virtual table, columns **Name (250) · Path (350) · Size (90, right-aligned)
  · Date Modified (140)**. Full-row select, alternating/■ double-buffered, header
  drag-reorder, click-to-sort with up/down arrow indicator (Size sorts descending-first). The
  **Path** column shows the *directory* (parent), not the full path (winindex strips the
  basename for that column). Size formatted as `B / KB / MB / GB` (2 decimals ≥ KB). Date as
  `YYYY-MM-DD HH:MM` local time.
- **Status bar:** bottom. Shows `Ready.` initially; during use shows result count
  ("`384 result(s)`", or "`… showing first 10,000. Refine your search…`" at the cap); while
  idle-with-index shows "`<N> files | <locations> | <age>`" (e.g. "1,234,567 files | / , /home |
  2 hrs old"). Indexing progress messages during scan.
- **Menus:**
  - **Search:** Regular Expression (Alt+1), Case Sensitive (Alt+2), Whole Word (Alt+3),
    Match Path (Alt+4), Ignore Diacritics (Alt+5) — checkable, disabled until an index exists.
  - **Index:** Rebuild Index Now · —— · Settings…
  - **Help:** Open Log File · —— · About indexed…
- **Context menu (right-click a result):** Open · Open Containing Folder · —— · Copy Full Path ·
  Copy Filename. (Open disabled for multi-selection.) Cut/Delete available via keyboard;
  optionally add to context menu too.
- **First-Run dialog:** "indexed — First Run Setup". Multi-select list of mounts to index
  (pre-select the mount containing `$HOME` and `/`); **Automatic Reindex** group ("Manual only"
  checkbox + interval spinbox + Hours/Days combo, default 48 Hours); **Excluded folders** list
  with Add…/Remove (folder picker); OK/Cancel.
- **Settings dialog:** "indexed — Settings". **Paths to index** list + Add Location…/Remove;
  same Automatic Reindex group; **Excluded folders** list + Add…/Remove; OK/Cancel. On OK,
  diff old vs new roots → incremental `IndexPaths`/`RemovePaths` or full rebuild (both changed).
- **About:** "indexed v0.1.0 — Blazingly fast Linux file search and indexer." with a clickable
  link to https://github.com/rajeshsub/indexed.
- **Behaviors to preserve:** 150 ms debounce; background search thread posts results to the UI
  thread; auto-focus search box after indexing completes; if a selected file no longer exists
  on open, prompt "The file no longer exists… rebuild the index now?"; disable search box while
  indexing, re-enable when watching.

---

## 20. Definition of Done

`indexed` v0.1.0 is complete when:
- All winindex features in the §5 mapping table are implemented (or explicitly deferred with an
  ADR).
- `indexed` launches on Fedora XFCE **and** at least one other distro (Ubuntu), indexes the
  selected roots, and returns live-filtered results matching the winindex UX.
- Substring, token-set, regex, and all five option toggles work, proven via
  `test_SearchEngine` unit tests loading a hand-built index (no CLI in v0.1.0).
- Live monitoring keeps results current (fanotify when privileged, inotify otherwise); hotplug
  mounts are detected.
- Open / reveal-in-file-manager (GNOME + XFCE, `xdg-open` fallback) / copy-path / copy-name /
  cut / trash / drag-out all work on the test DEs.
- The on-disk index persists, loads instantly, validates via CRC-32, and rebuilds when
  stale/corrupt/version-mismatched.
- `ctest` is green; ASAN clean; CI (lint + build + test + coverage) passes; a tagged build
  produces a working AppImage attached to a GitHub Release.
- README (with screenshot), CHANGELOG, ADRs, and MIT LICENSE are in place.
```
