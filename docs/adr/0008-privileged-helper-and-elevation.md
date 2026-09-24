Status: Accepted

## Context

`indexed`'s privileged component (`indexed-helper`) needs `CAP_SYS_ADMIN` (fanotify
whole-mount monitoring, `open_by_handle_at`) and `CAP_DAC_READ_SEARCH` (read every file
regardless of ownership) -- the Linux analog of winindex's "run as administrator" model
for MFT/USN access. Two questions must be resolved: how the helper obtains these
privileges, and how it can safely write output into an *unprivileged* user's directories
once it has them.

## Options -- privilege delivery

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. pkexec-on-demand | AppImage is the only v0.1.0 packaging target; want one code path that works everywhere immediately | Low -- one privilege path to build, test, document | Add setcap (b) as an opt-in fast path later, if/when distro packaging exists | Helper runs as root; repeated-prompt risk mitigated by session-lifetime (see below), not eliminated |
| b. setcap-installed | Distro packages (deb/rpm/AUR) are the primary distribution channel | Medium -- post-install scripting, capability testing | Fall back to pkexec automatically when `getcap` shows capabilities missing | Cannot apply inside a read-only AppImage squashfs at all -- still needs (a) as a fallback regardless |
| c. systemd always-on service | Want zero-latency monitoring from boot, treat this as a system service | High -- unit file, install/uninstall lifecycle | N/A -- heaviest option | Biggest install footprint; over-provisioned for a desktop search tool with only one packaging target |

## Decision -- privilege delivery

**pkexec-on-demand (option a) is the sole v0.1.0 delivery path.** AppImage is the only
packaging target for v0.1.0 (§14.2 of the implementation plan); setcap has no packaged
install to attach to yet, and a systemd service is unwarranted for a tool whose comparable
prior art (Recoll, Tracker) doesn't require one either. Setcap (b) is deferred as a
follow-on ADR if/when deb/rpm/AUR packaging is pursued.

**Helper lifecycle is tied to the GUI session**, not launched per-action and not
persistent across GUI restarts: `pkexec indexed-helper` is invoked once, at the first
privileged action in a session; the resulting process performs the initial scan-if-stale
and then blocks holding the fanotify monitor for the remainder of the session; the GUI
sends `SIGTERM` on exit. This yields **one polkit prompt per GUI session**, not one per
action, while avoiding the lifecycle-management complexity of a detached/systemd process
(no pidfile discovery, no "is a helper already running" singleton logic, no explicit
"stop monitoring" UI needed for v0.1.0).

## Options -- root-write safety

Because the AppImage helper runs as **root** but must write the index/status/log files
into an *unprivileged* user's XDG directories, it must resolve "which user" and validate
"which path" without trusting attacker-controllable input -- the classic local
root-helper privilege-escalation shape (e.g. a malicious local process pre-creating
`~/.cache/indexed/indexed.idx` as a symlink to `/etc/shadow` before the root helper opens
it for writing).

| Option | Fits when | Cost now | Trade-off |
|--------|-----------|----------|-----------|
| a. `PKEXEC_UID` (set by polkit itself) + `getpwuid()`; `O_NOFOLLOW` + ownership checks on every write | This is exactly the threat model | Low-medium -- standard libc/syscall hardening primitives, no new dependency | Requires careful, tested implementation of the validation path |
| b. Trust a GUI-supplied path passed as a `pkexec` argument | Never -- argv is easier to manipulate than `PKEXEC_UID`, and doesn't defend against a symlink swap regardless | Lowest | Reopens the exact vulnerability -- not acceptable |
| c. Root writes to a root-owned temp file; separate unprivileged step moves/chowns it | Want defense-in-depth beyond (a) | Medium-high -- extra IPC round-trip, extra failure mode | Marginal benefit over (a) done correctly; more moving parts for a two-process app |

## Decision -- root-write safety

**Option a.** The helper resolves the target user via `PKEXEC_UID` (set by polkit, not by
the invoking environment) and `getpwuid()` -- never `$HOME`/`$XDG_CONFIG_HOME`/etc., which
could be spoofed. Every output path (index, status file, log) is opened with
`O_NOFOLLOW`, and the helper refuses to write if any path component up to the target XDG
directory is not owned by the resolved target uid. This is the standard hardening pattern
for `pkexec`-launched helpers and directly closes the escalation path without adding IPC
complexity (option c).

## Consequences

- A local attacker who can write to `~/.cache/indexed/` before the helper runs cannot use
  a symlink to redirect a root write elsewhere; the ownership check plus `O_NOFOLLOW`
  rejects it.
- `test_Elevation` (mocked filesystem) is required and must prove the symlink-rejection
  and ownership-check behavior with explicit red/green assertions -- this is
  security-critical logic, not a routine unit test.
- Users see one polkit prompt per GUI session (acceptable, matches the "require root for
  full functionality" product decision) rather than one per privileged action.
- If distro packaging is added later, setcap becomes a genuine option and should be
  revisited as its own ADR rather than retrofitted into this one.

## Follow-up -- atomic index replacement and fd-relative root I/O (2026-09-24)

Crash-safe index saves (temp file, fsync, rename; `IndexSerializer::Save`) broke the
helper: it passed `/proc/self/fd/N` as the index path so every write landed on an
already-validated inode, and `/proc/self/fd/N.tmp` cannot be created, so every elevated
save failed silently. A rename-based save cannot go through a held fd anyway, because the
fd keeps pointing at the replaced inode. Review of the fix then found the original
`OpenForRootWrite` itself racy: it checked each directory with `lstat` and then opened by
path, and `O_NOFOLLOW` only covers the last component, so a user swapping
`~/.cache/indexed` for a symlink between check and open could have root create or truncate
`indexed.status` in any directory. Root also read `indexed.conf` and parsed `indexed.idx`,
both user-controlled, without those checks.

**Decision:** all root file access into the user's directories is fd-relative, through
three Elevation primitives:

- **Directory walk (shared by all three):** `baseDir` and every directory below it down to
  the file's parent is opened with `openat(O_DIRECTORY|O_NOFOLLOW)` relative to the
  previous directory's fd, never re-resolved by path, and must be a real directory owned by
  the target uid (`fstat` on the opened fd). Swapping any component for a symlink at any
  moment is refused rather than followed.
- **`OpenForRootWrite` (log):** opens the file relative to its parent's fd with
  `O_NOFOLLOW|O_NONBLOCK`; it must be a regular file with one link, owned by the target uid
  unless this call just created it with `O_EXCL` (then it is chowned to the target uid).
  `O_TRUNC` is applied afterwards with `ftruncate`, so a FIFO, device, hard link or other
  unexpected file is refused before a byte of it changes.
- **`OpenRegularFileForRootRead` (index and `indexed.conf`):** the same checks, read-only;
  the contents are then read through `/proc/self/fd/N` of that fd. Reading by plain path as
  root is not acceptable: a FIFO planted there would hang the helper, a symlink to a device
  would have root open it (with side effects for some devices), and a symlink to another
  user's index would leak its entry count and age into the user-readable status file. A
  refused or missing config means defaults at startup and the previous settings on a
  reload.
- **`ReplaceFileForRootWrite` (index and status):** creates `<name>.root.tmp` relative to
  the parent's fd (a stale one is unlinked first; unlink never follows a symlink) with
  `O_CREAT|O_EXCL|O_NOFOLLOW`, writes it, `fchown`s it to the target uid and primary gid with
  mode 0600 so the unprivileged GUI can read it, and `renameat`s it over the target. For the
  index it fsyncs the temp file and the directory; the status file, rewritten per scanned
  directory and worthless after a crash, skips both, and scan-progress updates to it are
  written at most every 100 ms (state changes always). The temp name differs from the GUI's
  `<name>.tmp`, so neither process disturbs the other's in-flight save. Status replacements
  are serialized by a mutex in the helper, since scan progress arrives from every scanner
  thread at once.

`Indexer` takes an `IndexFileIo` so the helper can supply the hardened index load/save
while the GUI keeps `IndexSerializer::Load`/`Save`.

**Consequences:**

- Because root parses `indexed.idx` on behalf of a user who controls it,
  `IndexSerializer::Load` validates every field against the bytes actually present (entry
  count, each record's path bounds, `nameStart`), not just the CRC, which the attacker can
  compute.
- Live-monitoring changes in the helper are saved too, with an idle gap after each save of
  at least 2 s and at least 5x that save's duration (`SaveThrottle`, docs/adr/0014), keeping the build timestamp so the index age still triggers the
  periodic rebuild (docs/adr/0007). A failed live save is retried; pending changes are
  saved on exit. Serializing holds the store's shared lock, but writing and fsyncing happen
  after it is released, so live mutations aren't stalled on disk I/O.
- Versions up to 0.3.1 created `indexed.log` as root. The helper now requires it to be the
  target user's own file, so when it finds the old root-owned one it removes it
  (`RemoveFileForRootWrite`, fd-relative) and recreates it; that old log's contents are
  lost.
- The GUI watches the index with `IndexFileWatcher`, which re-adds the path after every
  change (and picks it up once it appears), because `QFileSystemWatcher` stops tracking a
  file once it is replaced by rename.
- A helper save failure is logged at ERROR, and a refused index load or settings read at
  WARNING, instead of being dropped silently.
