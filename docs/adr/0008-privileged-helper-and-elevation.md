Status: Accepted

## Context

`indexed`'s privileged component (`indexed-helper`) needs `CAP_SYS_ADMIN` (fanotify
whole-mount monitoring, `open_by_handle_at`) and `CAP_DAC_READ_SEARCH` (read every file
regardless of ownership) — the Linux analog of winindex's "run as administrator" model
for MFT/USN access. Two questions must be resolved: how the helper obtains these
privileges, and how it can safely write output into an *unprivileged* user's directories
once it has them.

## Options — privilege delivery

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. pkexec-on-demand | AppImage is the only v0.1.0 packaging target; want one code path that works everywhere immediately | Low — one privilege path to build, test, document | Add setcap (b) as an opt-in fast path later, if/when distro packaging exists | Helper runs as root; repeated-prompt risk mitigated by session-lifetime (see below), not eliminated |
| b. setcap-installed | Distro packages (deb/rpm/AUR) are the primary distribution channel | Medium — post-install scripting, capability testing | Fall back to pkexec automatically when `getcap` shows capabilities missing | Cannot apply inside a read-only AppImage squashfs at all — still needs (a) as a fallback regardless |
| c. systemd always-on service | Want zero-latency monitoring from boot, treat this as a system service | High — unit file, install/uninstall lifecycle | N/A — heaviest option | Biggest install footprint; over-provisioned for a desktop search tool with only one packaging target |

## Decision — privilege delivery

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

## Options — root-write safety

Because the AppImage helper runs as **root** but must write the index/status/log files
into an *unprivileged* user's XDG directories, it must resolve "which user" and validate
"which path" without trusting attacker-controllable input — the classic local
root-helper privilege-escalation shape (e.g. a malicious local process pre-creating
`~/.cache/indexed/indexed.idx` as a symlink to `/etc/shadow` before the root helper opens
it for writing).

| Option | Fits when | Cost now | Trade-off |
|--------|-----------|----------|-----------|
| a. `PKEXEC_UID` (set by polkit itself) + `getpwuid()`; `O_NOFOLLOW` + ownership checks on every write | This is exactly the threat model | Low-medium — standard libc/syscall hardening primitives, no new dependency | Requires careful, tested implementation of the validation path |
| b. Trust a GUI-supplied path passed as a `pkexec` argument | Never — argv is easier to manipulate than `PKEXEC_UID`, and doesn't defend against a symlink swap regardless | Lowest | Reopens the exact vulnerability — not acceptable |
| c. Root writes to a root-owned temp file; separate unprivileged step moves/chowns it | Want defense-in-depth beyond (a) | Medium-high — extra IPC round-trip, extra failure mode | Marginal benefit over (a) done correctly; more moving parts for a two-process app |

## Decision — root-write safety

**Option a.** The helper resolves the target user via `PKEXEC_UID` (set by polkit, not by
the invoking environment) and `getpwuid()` — never `$HOME`/`$XDG_CONFIG_HOME`/etc., which
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
  and ownership-check behavior with explicit red/green assertions — this is
  security-critical logic, not a routine unit test.
- Users see one polkit prompt per GUI session (acceptable, matches the "require root for
  full functionality" product decision) rather than one per privileged action.
- If distro packaging is added later, setcap becomes a genuine option and should be
  revisited as its own ADR rather than retrofitted into this one.
