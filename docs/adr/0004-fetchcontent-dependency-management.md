Status: Accepted

## Context

`indexed`'s `core/` depends on RE2, abseil-cpp, GoogleTest, and utf8proc (the last is new
versus winindex, needed for UTF-8 case-folding and diacritics decomposition since Linux
has no Win32 `towlower`/`CompareString` equivalent). `ui/` additionally depends on Qt 6. A
dependency management strategy is required for both.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. vcpkg | Large dep graph, shared toolchain | Install vcpkg + manifest | Broad package ecosystem | Requires vcpkg bootstrap; CI must cache vcpkg tree |
| b. Conan | Cross-platform build system integration | Install Conan + conanfile | Package recipes for most libs | Two-step configure |
| c. CMake FetchContent (small libs) + system `find_package` (Qt) | Small header/small-source dep count; Qt is a heavyweight system package already provided by every mainstream distro | Zero setup beyond CMake and a C++ compiler | Add more FetchContent blocks | Slower cold configure for fetched deps; each compiled from source |

## Decision

Use **CMake FetchContent** (option c) for the four small `core/`-side dependencies
(googletest, abseil, re2, utf8proc), pinned to specific `GIT_TAG` values — unchanged
rationale from winindex's ADR-0004. **Qt 6 is found via system `find_package(Qt6 REQUIRED
COMPONENTS Widgets DBus)`**, not fetched: Qt is a large, actively-packaged system
dependency on every mainstream Linux distro (`qt6-base-dev` / `qt6-qtbase-devel` /
`qt6-base`), and building it from source via FetchContent would be a multi-hour, multi-GB
cold configure for no benefit over the distro package. `libblkid`/`libudev` are optional
`find_package(PkgConfig)` lookups (mount labels, hotplug), degrading gracefully if absent.

## Consequences

- `cmake -B build` fetches and builds the four small deps with zero pre-clone setup steps.
- Qt must be installed via the system package manager before configuring `ui/`; this is
  documented as a build prerequisite (README), consistent with how every other Qt desktop
  app on Linux is built.
- Cold configure fetches/compiles the four FetchContent deps (~30-60s first time);
  subsequent configures reuse the cached `_deps` directory (CI caches by `CMakeLists.txt`
  hash).
- Adding a `core/`-side dependency requires a new `FetchContent_Declare` +
  `FetchContent_MakeAvailable` block; adding a system dependency requires a new
  `find_package`/`pkg_check_modules` call plus a README/CI package-list update.
