#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace indexed {

// Resolves the directory containing the running executable via
// readlink("/proc/self/exe", ...). Returns "" if the symlink can't be
// resolved (e.g. unsupported platform) -- callers fall back to XDG paths.
std::string ExecutableDir();

// XDG_* overrides + home directory, passed explicitly (rather than read via
// getenv) so ResolveDataDirs is hermetically testable. An empty override
// string means "not set" -> fall back to the XDG-default location under
// homeDir (e.g. xdgConfigHome == "" -> homeDir + "/.config").
struct XdgEnv {
    std::string home;
    std::string xdgConfigHome;
    std::string xdgCacheHome;
    std::string xdgStateHome;
};

struct DataDirs {
    std::string configPath;  // .../indexed.conf
    std::string indexPath;   // .../indexed.idx
    std::string logPath;     // .../indexed.log
};

// True if `indexed.conf` sits directly inside executableDir -- the portable-
// mode trigger (indexed-plan.md §11): when true, config/index/log all live
// beside the binary instead of under XDG dirs.
bool IsPortableMode(const std::string& executableDir);

// Core, hermetic resolution logic (§11 table). Prefer this overload in tests.
DataDirs ResolveDataDirs(const std::string& executableDir, const XdgEnv& xdg);

// Convenience overload for real use: resolves executableDir via
// ExecutableDir() and reads the real HOME/XDG_* environment variables.
DataDirs ResolveDataDirs();

// Creates `path` (and any missing parent directories) if it doesn't already
// exist. Returns true if the directory exists on return (already existed, or
// was just created); false on a genuine failure (e.g. a file already occupies
// that path). Never throws.
bool EnsureDirectory(const std::string& path);

// Resolves `path` to a canonical absolute form via realpath() when possible
// (indexed-plan.md §7.1: "compare on canonical absolute paths"). When the
// path does not resolve (e.g. does not exist yet), falls back to a
// trailing-slash-stripped copy of the input so callers can still compare
// consistently. A bare "/" is returned unchanged.
std::string CanonicalizeBestEffort(const std::string& path);

// Reduces `roots` to the minimal set of ancestors: canonicalizes each entry
// (CanonicalizeBestEffort), drops any entry that is equal to or nested under
// another kept entry (whole-path-component containment, so "/home/user" does
// not absorb "/home/userfoo"), and returns the survivors sorted ascending
// for stable display. Empty in -> empty out. This is the invariant enforced
// on SelectedRoots so "/" plus "/home" can never both be indexed
// (indexed-plan.md §12.1, §18).
std::vector<std::string> CollapseRedundantRoots(const std::vector<std::string>& roots);

// Formats with thousands separators, e.g. 1234567 -> "1,234,567".
std::string FormatFileCount(uint64_t count);

// "just indexed" (< 60s) / "N min old" / "N hrs old" / "N days, N hrs old".
std::string FormatAge(uint64_t ageSeconds);

// Joins roots for display (", "-separated), stripping each entry's trailing
// '/' (a bare "/" is left alone).
std::string FormatLocationList(const std::vector<std::string>& roots);

}  // namespace indexed
