#pragma once

#include <sys/types.h>

#include <optional>
#include <string>

namespace indexed {

// Resolved identity of the unprivileged user who invoked pkexec, per
// indexed-plan.md §9.2 / docs/adr/0008-privileged-helper-and-elevation.md.
// Populated *only* from PKEXEC_UID (set by polkit itself) + getpwuid() --
// never from $HOME/$XDG_*, which are attacker-controllable by whatever
// process invoked `pkexec indexed-helper`.
struct TargetUser {
    uid_t uid = 0;
    std::string homeDir;   // pw_dir, as reported by getpwuid() -- not $HOME.
    std::string username;  // pw_name.
};

// Reads PKEXEC_UID from the environment, parses it as a uid, and resolves
// it via getpwuid(). Returns std::nullopt (never crashes, never falls back
// to any other environment variable) if:
//   - PKEXEC_UID is unset or empty
//   - PKEXEC_UID isn't a valid non-negative integer (garbage, negative,
//     trailing garbage after the digits)
//   - getpwuid() finds no such user
std::optional<TargetUser> ResolveTargetUser();

// Why OpenForRootWrite refused to open/create a path -- surfaced so the
// caller can log the specific reason (indexed-plan.md §9.2: this is
// security-critical, refusals must be diagnosable, not just "false").
enum class ElevationError {
    kNone,               // Success.
    kPathNotUnderBase,   // path isn't lexically inside baseDir.
    kSymlinkInPath,      // A directory component (or the final open()) hit a symlink.
    kOwnershipMismatch,  // A directory component isn't owned by targetUid.
    kStatFailed,         // lstat() on a required, expected-to-exist component failed.
    kOpenFailed,         // The final open() call itself failed.
};

// Hardened replacement for open() for use whenever the root helper writes
// one of its output files (index/status/log) into an unprivileged target
// user's XDG directory (indexed-plan.md §9.2, docs/adr/0008). Closes the
// classic local-root-helper escalation: a malicious local process
// pre-creating e.g. ~/.cache/indexed/indexed.idx as a symlink to
// /etc/shadow before the root helper opens it for writing.
//
// `baseDir` is the target user's XDG base directory the caller resolved
// (e.g. "<homeDir>/.cache/indexed") -- the boundary above which this
// function does not walk or check (that region -- "/", "/home", etc. --
// isn't part of this threat model and is frequently not owned by
// targetUid even legitimately). `path` must be lexically inside `baseDir`.
//
// Checks performed, in order:
//   1. `path` must be lexically under `baseDir` (string-prefix check on the
//      normalized paths -- no canonicalization that would itself follow a
//      symlink).
//   2. `baseDir` itself, and every directory component between `baseDir`
//      and `path`'s parent, must exist, must not be a symlink, and must be
//      owned by `targetUid` (checked via lstat(), so a symlinked directory
//      component is caught too, not just a symlinked final file).
//   3. The final component (the file itself) is opened with `flags |
//      O_NOFOLLOW`: if it's a symlink, the open() fails outright (ELOOP)
//      regardless of O_CREAT.
//
// `mode` is used only if `flags` includes O_CREAT. On success, `*outFd`
// holds an open, caller-owned fd and ElevationError::kNone is returned. On
// any failure, `*outFd` is left untouched, nothing is created, and the
// specific ElevationError is returned. Never throws.
ElevationError OpenForRootWrite(const std::string& path, uid_t targetUid,
                                const std::string& baseDir, int flags, mode_t mode, int* outFd);

}  // namespace indexed
