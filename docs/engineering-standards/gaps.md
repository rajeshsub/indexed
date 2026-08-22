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
