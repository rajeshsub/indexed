Status: Accepted

## Context

Indexing requires enumerating every file under the selected roots. winindex's fast path
(ADR-0002 in the winindex repo) reads the NTFS Master File Table directly
(`FSCTL_ENUM_USN_DATA`) when elevated, falling back to `FindFirstFile` BFS otherwise.
Linux has no portable equivalent: raw on-disk metadata structures differ per filesystem
(ext4 inode tables, btrfs B-trees, xfs AG structures, ...) and reading them directly while
mounted is neither safe nor portable across the filesystems `indexed` must support
(ext4, btrfs, xfs, f2fs, vfat/exFAT, ntfs3).

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. `readdir()` BFS/DFS walk | Simplicity over raw speed | Zero — stdlib only | None | More syscalls and allocations per entry than a batched approach |
| b. Raw `getdents64` parallel walk + `statx` | Want the fastest *portable* option across all supported filesystems | Medium — direct syscall, manual buffer management, thread pool | Per-filesystem raw scanners could be added later (e.g. ext4 inode read) as an opt-in accelerator | Still an O(n) directory walk, not an O(log n) index-structure read — no MFT-style shortcut exists |
| c. Filesystem-specific raw reads (ext4 inode table, btrfs B-tree walk, etc.) | Want MFT-equivalent speed on mounted filesystems | Very high — one implementation per filesystem, none of them safe to do concurrently with a mounted, writable filesystem | N/A for v0.1.0 | Out of scope: no safe, portable way to read live filesystem metadata structures directly on Linux while mounted; explicitly deferred (§2 non-goals) |

## Decision

Use a **parallel `getdents64` + `statx` walk** (option b), implemented behind the same
`IFileSystemScanner` interface winindex used for its dual-scanner design — even though
`indexed` only has one scanner implementation, the interface is retained for testability
(mocking in `Indexer` tests) and as a seam for a future filesystem-specific accelerator.

`getdents64` via raw `syscall()` is faster than `readdir()` (fewer allocations, batched
reads). `statx` with a minimal mask (`size|mtime|type|mode`) fetches only the metadata the
index needs. A work-stealing thread pool of directory jobs parallelizes the walk across
cores. Mount boundaries are detected via `stx_mnt_id`/`st_dev` so the walker never crosses
into an unselected mount or a pseudo/network filesystem. Symlinks are not followed (mirrors
winindex skipping reparse points, avoids traversal loops).

Raw filesystem-specific scanning (option c) is explicitly out of scope for v0.1.0 (§2).

## Consequences

- No MFT-style shortcut exists on Linux: a full walk of a large tree is bound by I/O and
  dentry-cache warmth, not by an index-structure read. This must be stated honestly in the
  README/performance docs — do not promise NTFS-MFT-class cold-scan speed.
- `IFileSystemScanner::FastScanAvailable()` always returns `false` in v0.1.0; kept as an
  interface seam, not dead code, for a possible future per-filesystem scanner.
- The walker's correctness (mount-boundary detection, symlink skipping, exclusion pruning)
  is unit-tested against a temp directory tree in `test_WalkScanner`, independent of any
  real filesystem's raw layout.
