#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <string_view>

namespace indexed {

// Resolved identity of the unprivileged user who invoked pkexec, per
// indexed-plan.md §9.2 / docs/adr/0008-privileged-helper-and-elevation.md.
// Populated *only* from PKEXEC_UID (set by polkit itself) + getpwuid() --
// never from $HOME/$XDG_*, which are attacker-controllable by whatever
// process invoked `pkexec indexed-helper`.
struct TargetUser {
    uid_t uid = 0;
    gid_t gid = 0;         // pw_gid (primary group).
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
    kWriteFailed,        // Writing, syncing, or renaming the replacement failed.
    kNotRegularFile,     // The opened file is not a regular file owned by targetUid.
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
//   2. `baseDir` itself, then every directory between it and `path`'s
//      parent, is opened with openat(O_DIRECTORY|O_NOFOLLOW) relative to the
//      previous directory's fd -- never re-resolved by path -- and must be a
//      real directory owned by `targetUid` (fstat on the opened fd). Swapping
//      any component for a symlink, at any moment, is refused rather than
//      followed.
//   3. The file is opened relative to its parent's fd with O_NOFOLLOW (a
//      symlink fails with kSymlinkInPath) and O_NONBLOCK (a FIFO can't block
//      the open). It must be a regular file with exactly one link, owned by
//      `targetUid` unless this call just created it (O_CREAT|O_EXCL); anything
//      else is refused with kNotRegularFile. A newly created file is chowned
//      to `targetUid`.
//   4. O_TRUNC, if requested, is applied only after all of the above (via
//      ftruncate), so a refused file is never modified.
//
// `mode` is used only if `flags` includes O_CREAT. On success, `*outFd`
// holds an open, caller-owned, blocking fd and ElevationError::kNone is
// returned. On any failure, `*outFd` is left untouched, nothing is created,
// and the specific ElevationError is returned. Never throws.
ElevationError OpenForRootWrite(const std::string& path, uid_t targetUid,
                                const std::string& baseDir, int flags, mode_t mode, int* outFd);

// OpenForRootWrite with O_RDONLY and no O_CREAT: the same directory and
// file checks (regular file, one link, owned by targetUid) happen before a
// single byte is read, so a FIFO, device, or symlink planted at `path` is
// refused rather than opened.
ElevationError OpenRegularFileForRootRead(const std::string& path, uid_t targetUid,
                                          const std::string& baseDir, int* outFd);

// Removes the directory entry `path` (whatever it is: file, symlink, FIFO,
// hard link), with the same directory checks as OpenForRootWrite; the
// unlink goes through the validated parent directory's fd and never follows
// the entry. Used to clear a file an older version left root-owned so it can
// be recreated as the target user's. A missing entry is success.
ElevationError RemoveFileForRootWrite(const std::string& path, uid_t targetUid,
                                      const std::string& baseDir);

// Whether ReplaceFileForRootWrite fsyncs. kSynced is for recovery state
// (the index); kUnsynced for display-only state that is rewritten often and
// worthless after a crash (the status file).
enum class ReplaceDurability { kSynced, kUnsynced };

// Atomically replaces `path` with `contents` on behalf of the root helper,
// with the same directory checks as OpenForRootWrite (step 2); the temp
// file and the rename then go through the validated parent directory's fd
// (openat/renameat). The temp file is "<name>.root.tmp" (distinct from
// IndexSerializer::Save's "<name>.tmp", so a concurrent unprivileged save is
// never disturbed); a leftover one is unlinked (never followed) before it is
// recreated with O_CREAT|O_EXCL|O_NOFOLLOW, so it is always a fresh inode.
// It is chowned to targetUid:targetGid with mode 0600 so the unprivileged
// GUI can read the result, and renamed over `path` whatever `path`
// currently is. With kSynced the temp file is fsynced before the rename and
// the directory after it, so the replacement survives a crash. If anything
// fails before the rename, `path` is untouched and no temp file remains; if
// only the final directory fsync fails, `path` already holds the new
// contents but kWriteFailed is returned because the replacement may not
// survive a crash. Callers must not run two replacements of the same `path`
// concurrently (they share the temp name). Never throws.
ElevationError ReplaceFileForRootWrite(const std::string& path, uid_t targetUid, gid_t targetGid,
                                       const std::string& baseDir, std::string_view contents,
                                       ReplaceDurability durability = ReplaceDurability::kSynced);

}  // namespace indexed
