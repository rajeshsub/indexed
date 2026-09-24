Status: Accepted

## Context

The GUI thread did index work directly. With a 5M-entry index (measured on a 16-core
machine):

- reloading the elevated helper's saved index took 1.7 s on the UI thread, and with live
  saves from the helper that happens every few seconds during file activity;
- removing a root in Settings stopped monitoring (a thread join), marked entries deleted
  under the store's write lock and saved the index (about 1 s), all on the UI thread;
- Rebuild or a Settings change during a running scan joined the scan thread (seconds to
  minutes), and closing the window did the same, since a scan could not be cancelled;
- stopping monitoring waited for `InotifyWatcher` to finish walking the whole tree;
- Move to Trash / Delete did a full-index path lookup under the write lock per file (37 ms
  each) and waited behind any running search; moving to Trash across drives copies the file;
- every keystroke joined the previous search thread, which could itself be waiting for the
  lock behind a writer;
- Reveal in File Manager made a synchronous D-Bus call (Qt's default timeout is 25 s), and
  Open stat'ed the file on the UI thread.

A frozen UI is not acceptable for this product, and an audit is coming up that will ask for
evidence, not claims.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. Fix each blocking call in place | Few call sites, no need to prove a global property | Small per site | Each new feature has to remember the rule | Nothing structural stops new UI-thread blocking; hard to demonstrate to an auditor |
| b. One index worker owns all index work; the UI only posts commands and receives results | The property "the UI thread never blocks on index work" must hold everywhere and be provable | Medium: `MainWindow`'s index handling moves into one class | New index operations become new commands on the same worker | One more component; commands run in order (the correct order anyway) |
| c. Immutable snapshots (RCU-style) so nobody waits on locks | Extreme change rates where even short lock waits matter | Large: copy-on-write or overlay structures for multi-million-entry pools | n/a | Most memory and complexity; unnecessary once (b) and a path index exist |

## Decision

**Option b**, decided in a `/grill-me` pass with the developer on 2026-09-24:

- **`IndexService` in `src/core`, Qt-free**, with its own worker thread and command queue,
  reporting through `std::function` callbacks; unit-tested with gtest like `Indexer`.
  `MainWindow` gets a thin adapter that forwards callbacks to the UI thread with queued
  invocations. The UI thread never loads or saves the index file, never takes the store's
  write lock, and never joins a thread.
- **Commands:** index (load-if-fresh or scan), apply a Settings change (the old/new root
  diff moves here from `MainWindow`), reload the index file written by the elevated helper
  (coalesced: requests arriving during a reload cause at most one more), and remove paths
  from the index. Monitoring is started and stopped by the worker.
- **A new rebuild or Settings change cancels a running scan** and starts over, so the result
  always reflects the latest request. `Indexer::StartIndexing` takes the caller's cancel
  token; a cancelled scan neither replaces the live index nor saves. `InotifyWatcher`'s
  initial watch walk checks its stop flag per directory.
- **Search box:** disabled only during a full rebuild or a Settings change (as in
  indexed-plan.md section 19). Reloads of the helper's saves and live changes swap in
  without touching it.
- **Move to Trash / Delete run on a separate file-operations thread**, so a slow cross-drive
  trash never waits behind a scan and a scan never waits behind it. Rows being processed are
  shown as pending, then removed; failures are reported together afterwards.
- **Closing the window** cancels a running scan and closes immediately (the index on disk
  stays the previous one). An in-progress file operation is allowed to finish, so no file is
  left half-moved; the process exits when it has.
- **Searches no longer join on each keystroke:** the previous search is cancelled and left to
  finish on its own thread; its results are discarded.
- **Reveal** uses an asynchronous D-Bus call; **Open** checks the file on a detached thread
  (a stat hung on a dead mount can't hold up closing the app either). **Elevate** reacts to
  QProcess signals instead of waiting, and a helper that exits (including a declined
  password prompt, since pkexec starts before it) returns the window to local indexing.
- **Dialogs:** confirmation and failure dialogs are non-modal. Settings and About remain
  modal dialogs, which keep the event loop running.
- **Elevated helper save pacing:** after each save, an idle gap of at least 2 s and at least
  5x that save's duration (about every 6 s at 5M entries).

**Evidence for the audit:**

1. A CI test drives the real `MainWindow` with a 1M-entry index and fails if any UI
   event-loop gap exceeds 100 ms during reload, Settings changes, trash/delete, rebuild,
   typing and close.
2. A stall detector in the app: a background thread pings the UI thread and logs a WARNING
   with the duration whenever the UI takes more than 250 ms to respond. Always on.
3. A documented manual check at 5M entries, with its results recorded in this ADR.

## Results (2026-09-24, 16-core x86-64, 15 GiB RAM, Fedora 44, debug build)

Longest UI event-loop pause per action, measured by `tests/test_MainWindow.cpp`
(`uiNeverBlocksForMoreThan100Ms`: a 5 ms timer records the longest gap between ticks from
the moment the action is triggered until it has fully finished; limit 100 ms). "Before" is
the same test against the previous `MainWindow`, at 1M entries.

| Action | Before (1M) | After (1M, CI) | After (5M, manual) | What the row proves |
|--------|-------------|----------------|--------------------|---------------------|
| Startup load | 16 ms | 28 ms | 26 ms | Loading runs off the UI thread |
| Typing ten queries | 5 ms | 5 ms | 5 ms | Regression guard only; the removed per-keystroke join is proven by `test_SearchCoordinator` (a search that ignores cancel for 500 ms: 449 ms pause before, under 50 ms after) |
| Rebuild | 5 ms | 5 ms | 6 ms | Measured until the rebuild finishes |
| Rebuild again mid-rebuild | 4083 ms | 6 ms | 6 ms | Also checks the cancelled scan was never swapped in |
| Settings: add a root | 4035 ms | 7 ms | 7 ms | Measured until applied |
| Settings: remove a root | 1725 ms | 5 ms | 5 ms | Measured until applied |
| Move to Trash (300 ms operation) | 308 ms | 6 ms | 6 ms | File operations run off the UI thread |
| Delete permanently (300 ms operation) | 306 ms | 6 ms | 5 ms | Same, after a non-modal confirmation |
| Reveal, file manager never replies | (not measured) | 6 ms | 6 ms | Private D-Bus daemon; the stub answers on its own connection and never replies. A blocking call measured 4764 ms and failed the test |
| Open a file that no longer exists | 5 ms | 11 ms | 12 ms | Regression guard; a hung stat can't be simulated, the detached check is covered by review |
| Reload the elevated helper's save | 1103 ms | 6 ms | 23 ms | Reload runs off the UI thread |
| Close during a rebuild | 0 ms | 0 ms | 0 ms | The window hides at once |
| Teardown after close (not a UI pause; bound 1 s) | 2681 ms | 201 ms | 122 ms | The running scan is cancelled rather than waited for |

The UI thread still does small, local work that isn't index work: saving `indexed.conf`
after the Settings dialog, reading the few-hundred-byte helper status file, the file
watchers' existence checks, and the first `QDBusConnection::sessionBus()` connect. None of
these showed up in the measurements. The timing test is skipped in the AddressSanitizer
job, whose timings aren't representative; the debug and release jobs run it.

**Manual 5M check (procedure):** build the debug preset, then run
`INDEXED_LATENCY_ENTRIES=5000000 QT_QPA_PLATFORM=offscreen ./build/debug/tests/test_MainWindow uiNeverBlocksForMoreThan100Ms`
and record the per-action lines it prints. It needs about 1.5 GB free in the temp directory
(removed when the test finishes) and a few minutes. Repeat after any change to
`MainWindow`, `IndexService`, `SearchCoordinator` or the index format.

## Consequences

- `MainWindow` moves into the `indexed_ui_widgets` library so tests can construct it.
- All index mutations from the UI (trash, delete, settings) become asynchronous: the UI shows
  the result when the worker reports it, not on return from the call.
- A command that is slow for other reasons (a full scan) no longer delays anything the user
  does; a rebuild request cancels it rather than waiting.
- Blocking work added to the UI thread in future fails the latency test in CI.
