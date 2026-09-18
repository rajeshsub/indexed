Status: Accepted

## Context

winindex keeps its index current via the NTFS USN journal: a persistent, replayable,
monotonic-cursor change log. On startup it replays everything since the last saved
cursor, so a period of downtime never causes missed changes. Linux has no equivalent
persistent change log at the filesystem level available to a userspace process.
`indexed` must choose a live-monitoring mechanism, decide how the GUI and the privileged
monitor process communicate, and decide what happens across the "no replay" gap.

## Options -- monitoring mechanism

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. fanotify (`FAN_MARK_FILESYSTEM`, `FAN_REPORT_DFID_NAME`) | Privileged process available, kernel ≥ 5.9 | Medium -- `CAP_SYS_ADMIN` + `CAP_DAC_READ_SEARCH`, `open_by_handle_at` path resolution | None needed -- this is the closest Linux analog to the USN journal's whole-volume coverage | **No history** -- live-only, unlike the USN journal |
| b. inotify (per-directory recursive watches) | Unprivileged fallback | Low -- well-known API | None | Same "no history" property, **plus** a `fs.inotify.max_user_watches` ceiling on huge trees |
| c. Poll/rescan only, no live monitoring | Simplicity above all | Lowest | fanotify/inotify could be added later | Loses "new files appear instantly," a headline winindex feature -- too large a cut for a feature-for-feature port |

## Decision -- mechanism and staleness mitigation

**Prefer fanotify (a) via the privileged helper; fall back to inotify (b) when
unprivileged; fall back further to interval-only rescans if inotify's watch ceiling is
exhausted (c, as a degraded mode only).** Selection logic lives in `Indexer`.

**The no-replay gap is mitigated, not eliminated:** on startup, if the on-disk index is
older than `ReindexIntervalHours` (default 48) or the helper detects it wasn't running
continuously, a fresh background rescan runs before live monitoring attaches. A
`lastMonitorStop` timestamp (nanoseconds since Unix epoch -- see
`docs/adr/0003-binary-index-format.md`) is persisted in the index trailer, replacing
winindex's USN-cursor map, and surfaced honestly in the status bar as index age.

**Minimum kernel floor (resolved in grill-me):** `indexed` requires kernel **≥ 5.4** to
run at all (a conservative floor -- well below Ubuntu 20.04 LTS's kernel and any
2024+-era distro default). Full fanotify whole-mount monitoring additionally requires
**≥ 5.9** for `FAN_REPORT_DFID_NAME`; below that, `indexed` transparently uses inotify. No
new fallback logic beyond a runtime kernel-version check is needed -- `Indexer`'s existing
fanotify→inotify→rescan selection chain already handles this.

## Options -- GUI↔helper control channel

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. Unix signals + shared status file | Small, fixed set of control actions (reload settings, reindex-now, stop); no need for request/response | Low | Add another signal/file if new actions are ever needed | No acknowledgement/error channel |
| b. Local Unix domain socket, small JSON-RPC-style protocol | Anticipate needing structured request/response or many commands | Medium | Trivially extensible | Effectively a small persistent local daemon protocol -- tension with the "no persistent query daemon" non-goal (§2), even though this would be control, not query, traffic |
| c. D-Bus service | Want idiomatic desktop IPC, discoverability | High | N/A | Over-provisioned for a two-process, 1:1 GUI↔helper relationship with three commands and one status readout |

## Decision -- control channel

**Unix signals + a shared status file (option a).** `SIGHUP` tells the running helper to
re-read the Settings INI (roots/exclusions changed); `SIGUSR1` triggers an immediate
reindex; `SIGTERM` stops monitoring and exits the helper cleanly. The helper periodically
rewrites a small status file (state enum, files-found count, current directory) that the
GUI watches via inotify -- this is how scan progress reaches the GUI without a request/
response channel. The `.idx` index file itself stays whole-file-reload on change, not a
delta protocol; the separate status file carries progress instead of requiring `.idx` to
support incremental deltas.

This mirrors how comparable Linux desktop indexers operate (Recoll's `recollindex
--monitor`, Tracker's miner) -- none of them expose a query-serving daemon, and none needed
more than a handful of control primitives between their indexer and their query-serving
side.

## Decision -- tombstone reclamation and staleness guarantee strength (grill-me, 2026-09-18)

Two follow-up questions from an external persistence/monitoring review were resolved via
`/grill-me` rather than left implicit or decided unilaterally by an agent:

**Tombstone reclamation policy: rebuild-only, no standalone compaction pass.**
`IndexPool::MarkDeleted` never reclaims space -- entries stay in `meta_`/the path pools
with their `deleted` flag set, offsets never reused, so `IndexPool::Count()` only ever
grows across `ApplyRemove`/`ApplyRename`/`RemoveEntriesUnderPath` calls. The only thing
that clears accumulated tombstones is a full rebuild: `Indexer::StartIndexing`'s
`BeginWrite`/`EndWrite` path (triggered by a stale on-disk index past
`ReindexIntervalHours`, default 48h, or a forced/manual reindex) replaces the whole pool
wholesale. **Decision:** keep this as the permanent policy -- no tombstone-ratio or
entry-count trigger, no periodic compaction pass, and no user-facing tombstone-count
surfacing. Rationale: growth between rebuilds is already bounded by the existing 48h
staleness window (the same bound that mitigates the no-replay gap above), so unbounded
churn between rebuilds isn't actually unbounded in practice; adding a second reclamation
mechanism would be complexity with no material benefit over what the staleness check
already provides for free. Covered by `IndexStore`'s
`RepeatedCreateRenameDeleteChurnAccumulatesTombstonesNotLiveGrowth` and
`RebuildViaBeginWriteEndWriteClearsAccumulatedTombstones` tests.

**Staleness/consistency guarantee: kept as the permanent target, not a placeholder for a
future stronger guarantee.** The "cache kept current by live monitoring and periodic
rescans, not a transactional exact snapshot" statement (README, and the no-replay-gap
consequence below) is the intended end-state guarantee for this project, not an interim
compromise slated for strengthening. **Decision:** no lightweight validation pass and no
additional "possibly stale" UI surfacing beyond the existing status-bar index-age readout
are planned. If a future reviewer asks "is this guarantee strong enough," the answer
already decided here is that it is, for this project's scope (§2, single-user, no
query-serving daemon) -- re-open only via a fresh `/grill-me` pass, not a unilateral code
change.

## Consequences

- Index drift is possible only across a period when the GUI (and therefore the
  session-lifetime helper, see `docs/adr/0008-privileged-helper-and-elevation.md`) isn't
  running -- every fresh GUI session re-evaluates staleness, so this is a single, bounded,
  documented gap rather than an open-ended one.
- inotify's watch-count ceiling is a real limitation on huge trees for unprivileged users;
  surfaced via status-bar message ("live monitoring incomplete -- periodic rescan active"),
  not silently swallowed.
- inotify reports nothing for a directory between its creation (or move into the tree) and
  `inotify_add_watch` registering a watch on it. `InotifyWatcher` closes that window by
  walking the new directory's current contents and emitting them as `Added` when it starts
  watching, so a `mkdir d && touch d/f` or an `mv` of a populated directory is not lost.
  Re-emitting an entry a later event also reports is idempotent downstream.
- The GUI↔helper coupling stays deliberately thin: no socket, no D-Bus service, no
  daemon-shaped protocol to test and secure -- three signals and one polled file.
