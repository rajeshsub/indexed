# indexed

Blazing fast file search for Linux. `indexed` builds a full index of your local
filesystems and returns results as you type, with no perceptible delay even across
millions of files. It is a feature-for-feature Linux port of
[winindex](https://github.com/rajeshsub/winindex).

![Platform](https://img.shields.io/badge/platform-Linux-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B20-orange)
![License](https://img.shields.io/badge/license-MIT-green)
![CI](https://github.com/rajeshsub/indexed/actions/workflows/ci.yml/badge.svg)

**Status: early development (M0 scaffold).** See [`indexed-plan.md`](indexed-plan.md)
for the full implementation plan, milestones, and `docs/adr/` for the structural
decisions behind the design.

---

## Features (planned for v0.1.0)

- **Instant search** — results appear as you type, debounced at 150 ms
- **Parallel directory-walk scanning** — a `getdents64` + `statx` walker, parallelized
  across a thread pool (no raw on-disk filesystem structure to shortcut through on Linux
  — see `docs/adr/0002-directory-walk-scanning-strategy.md`)
- **Live monitoring** — fanotify whole-mount monitoring via a privileged helper, with an
  inotify fallback when unprivileged
- **Regex support** — powered by [RE2](https://github.com/google/re2); toggle with Alt+1
- **SIMD-accelerated substring search** — AVX2/SSE4.2 on x86-64 with runtime dispatch;
  scalar on aarch64 in v0.1.0
- **Word-level matching** — queries with spaces, underscores, or hyphens match filenames
  by token set, so `just rosy guitar` finds `LedZep_Just-Rosy_June-Bug_guitar.flac`
- **Search modes** — case-sensitive, whole-word, match full path, ignore diacritics
- **Portable mode** — place an `indexed.conf` next to the executable and all data stays
  in that directory
- **Persistent index** — serialized to disk (CRC-32 validated) and loaded on startup;
  only rebuilt when stale, corrupt, or missing
- **Context menu** — open file, open containing folder, copy full path, copy filename,
  cut (move), delete (to Trash), drag-and-drop out to file managers
- **Smart exclusions** — pseudo-filesystems, container storage, and Flatpak data are
  excluded by default; fully user-configurable

`indexed` is **GUI-only** — there is no CLI query mode (a deliberate scope decision; see
the Decisions Log in `indexed-plan.md` §18).

---

## How it works

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

The indexer/monitor runs in a separate privileged process (`indexed-helper`, elevated via
`pkexec` for the lifetime of the GUI session — see
`docs/adr/0008-privileged-helper-and-elevation.md`); the GUI itself never runs as root and
only loads/searches the on-disk index in-process.

---

## Building

### Requirements

| Tool | Minimum version |
|------|----------------|
| Linux kernel | 5.4 to run; 5.9 for full fanotify monitoring |
| CMake | 3.28 |
| GCC or Clang | C++20 support |
| Qt 6 | Widgets + DBus (system package, e.g. `qt6-qtbase-devel` on Fedora) |
| Git | any recent |

Third-party dependencies (**re2**, **abseil**, **utf8proc**, **GoogleTest**) are fetched
automatically by CMake's `FetchContent` — no manual installation needed.

### Quick build

```bash
./build.sh debug
./build.sh release
```

Or with CMake presets directly:

```bash
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
```

### Running tests

```bash
./build.sh test
```

---

## Project structure

```
indexed/
  src/
    core/       indexer/, search/, settings/, storage/, platform/ (Qt-free, C++20)
    ui/         Qt MainWindow, search bar, result view
    helper/     privileged indexer + fanotify monitor entry point
  tests/        GoogleTest/GoogleMock unit tests
  docs/adr/     Architecture Decision Records
  packaging/    .desktop file, AppImage build (M6)
  .github/workflows/  CI: lint, build+test, ASAN, coverage
  CMakeLists.txt
  CMakePresets.json
  build.sh      Convenience wrapper: build.sh [debug|release|asan|test|all|clean]
  indexed-plan.md   Full implementation plan and milestones
```

---

## Development setup

```bash
pip install pre-commit
pre-commit install
pre-commit install --hook-type pre-push
```

`pre-commit install` wires commit-time checks (clang-format, cppcheck).
`pre-commit install --hook-type pre-push` wires the pre-push gate that builds and runs
the full test suite before a push.

To run all checks on the full codebase (one-time cleanup):

```bash
pre-commit run --all-files
```

---

## License

MIT
