# Engineering standards: gap audit findings

First audit: 2026-08-22. All 9 gaps found were selected by the developer for closure in
the same session. None deferred or declined.

| # | Gap | Decision | Date |
|---|-----|----------|------|
| 1 | G2 Coverage: `.codecov.yml` target 70% (below 80% floor), no measured-scope config | Closed | 2026-08-22 |
| 2 | G3: no dependency vulnerability scanning wired | Closed | 2026-08-22 |
| 3 | G3/rule 4: `cppcheck` binary unpinned in CI/local, could drift | Closed | 2026-08-22 |
| 4 | G3/rule 27: GitHub Actions pinned to mutable tags, no `concurrency:` on release | Closed | 2026-08-22 |
| 5 | G5: `Logger` had no severity levels, not externally configurable | Closed | 2026-08-22 |
| 6 | G6: no `architecture.md` | Closed | 2026-08-22 |
| 7 | G6/rule 6: em dashes throughout docs/comments/UI strings | Closed | 2026-08-22 |
| 8 | G6/rule 28: no release checksums, unsigned tags, no verification docs | Closed | 2026-08-22 |
| 9 | G8: no execution-timing instrumentation on hot paths | Closed | 2026-08-22 |

See `applied.md` for the verified gate list and what remains unverified (CI-only steps
not exercised by a push this session).

## Test-integrity findings (2026-08-31)

Not standards-selection gaps, but defects the existing tests failed to catch because they
asserted on code paths the running application never reached (rule 3):

| # | Finding | Decision | Date |
|---|---------|----------|------|
| 10 | `ResultView` drag-out was dead in the app -- `ResultModel` never overrode `flags()`, so rows lacked `Qt::ItemIsDragEnabled` and `QAbstractItemView` never called `startDrag`. The DnD tests passed by calling `startDrag`/`BuildDragMimeData` directly. | Fixed + real-gesture test added (`docs/adr/0013`) | 2026-08-31 |
| 11 | Unprivileged live indexing did not refresh the results view, and `WalkScanner` could not scan a single-file path, so `Indexer::ApplyChangeEvent`'s Added path never indexed a new file. `test_Indexer` passed against a mock scanner that ignored the root type. | Fixed + `WalkScanner` file-root tests + `Indexer` mutation-callback tests added | 2026-08-31 |
