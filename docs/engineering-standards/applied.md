# Engineering standards: applied to `indexed`

Recorded 2026-08-22. Posture: **greenfield** (own project). Selection and audit run once
per repo; later sessions read this file plus `gaps.md` instead of re-auditing.

## Selection (Step A)

All seven groups selected (greenfield default):

- **G1 Testing discipline** -- tiers: unit, integration, end-to-end, performance
  (no contract tier: `indexed` exposes no consumed/produced interface contract to
  version).
- **G2 Coverage enforcement** -- diff-gated (patch target 80%, project target 70%
  informational-only). See `.codecov.yml`.
- **G3 Toolchain, gates and CI** -- fast checks at pre-commit, full build+ctest at
  pre-push, matching what was already wired.
- **G5 Logging and observability** -- level hierarchy on a local log file (not a
  deployed-service; no full structured-record requirement), runtime-configurable via
  `indexed.conf`. See `docs/adr/0009-log-severity-levels.md`.
- **G6 Docs, ADRs, architecture and README** -- full group, including release/
  distribution (rule 28) since `indexed` ships an AppImage to end users.
- **G7 Interfaces and design** -- full group.
- **G8 Instrumentation and performance** -- test suite plus latency-sensitive paths
  (search, directory-walk scan). See `docs/adr/0011-benchmark-tooling.md`.

## Verified gate list (rule 16)

Verified 2026-08-22 against `.pre-commit-config.yaml` + `.github/workflows/ci.yml` +
`.github/workflows/dependency-scan.yml`:

| Gate | Mechanism | Verified how |
|------|-----------|---------------|
| clang-format | `.pre-commit-config.yaml` (pre-commit stage) | Observed failing on real draft code during this session (unformatted `Logger.cpp`/`Settings.cpp`/etc.), fixed, re-verified green. |
| cppcheck | `.pre-commit-config.yaml` (pre-commit stage) | `pre-commit run --all-files` green on the full changed set; ran standalone against changed files too. |
| Full test suite (ctest) | `scripts/run-tests.sh` (pre-push stage) | Deliberate-failure proof: a scratch test (`EXPECT_EQ(1, 2)`) registered in `tests/CMakeLists.txt`, `pre-commit run --hook-stage pre-push --all-files` reported `Run unit tests (pre-push)....Failed` with the scratch test named in CTest's failure list; scratch file and registration then removed, tree confirmed clean, gate re-verified green (209/209, then 209/209 again post-cleanup). |
| CI `lint` job | `.github/workflows/ci.yml` | Runs the identical `pre-commit run --all-files` command as local; config read, not independently re-executed in CI for this pass (would require a push). |
| CI `dependency-scan` job + `dependency-scan.yml` | new this pass | Not executable locally (GitHub-hosted reusable workflow); config-reviewed only, not run. Flagged as unverified below. |
| CI `benchmarks` job | new this pass | Local equivalent build+run verified (`bench_SearchEngine`/`bench_WalkScanner` built and executed with `--benchmark_format=json`, valid JSON observed); the `benchmark-action/github-action-benchmark` step itself is config-reviewed only, not run. |

Config hashes at verification time (post-review fixes, see below):
- `.pre-commit-config.yaml`: `ee9d283ae44ad903766f64c92e45be04b567121f53202cbd55163be0fa1a415a`
- `.github/workflows/ci.yml`: `395acd5d17b9b2b4dd9345bf4d45d322d19e62dbf3b938dde05a4061bfca298d`
- `.github/workflows/dependency-scan.yml`: `7fc38c9fd9c9f73a7ccbc62b8e6d111e5d06fcbf0229268f5c25bf7f24f25a1b`

Re-verify only when these hashes change, or on request.

## Independent review (rule 23)

Reviewed by a fresh-context agent (general-purpose, no visibility into this session's
reasoning) against the full diff plus untracked new files. Findings and disposition:

1. **BLOCKING (fixed):** `src/helper/main_helper.cpp` was untouched by the original diff
   but its `Logger` construction defaulted to the new `Warning` threshold, silently
   dropping every lifecycle log line (start, stop, settings reload, reindex) for the
   *privileged, root-running* helper process -- the highest-stakes instance of the
   "old Logger always wrote" assumption breaking silently. Fixed: helper now sources its
   threshold from `Settings::LogLevel()` like the GUI, and its start/stop lines are
   explicitly logged at `Warning` so they survive the default threshold regardless of
   configured verbosity (security-relevant audit trail, docs/adr/0008).
2. **Non-blocking (fixed):** ADR-0009 originally claimed existing call sites were
   "source-compatible" with the new default, which is true for compilation but false for
   behavior (informational messages now silently drop at default verbosity). ADR text
   corrected to state this plainly and explain why it's acceptable for the GUI's own two
   startup lines but was not acceptable left unexamined for the helper (see above).
3. **Non-blocking (fixed):** both OSV-Scanner jobs (`ci.yml`'s `dependency-scan`,
   `dependency-scan.yml`'s `scan-scheduled`) granted `security-events: write` but not
   `actions: read`, which the called reusable workflows' SARIF-upload step needs (per
   upstream source, citing `github/codeql-action#2117`). Added to both permission blocks.

Also verified independently (no changes needed): all 7 pinned Action SHAs match their
stated version tags (spot-checked via `git ls-remote --tags` against upstream), the
`Logger::Log` severity comparison direction, `BUILD_BENCHMARKS` CMake wiring, the
`Settings.h`/`Logger.h` include relationship, and that new/changed tests assert real
content rather than just executing code paths.

All fixes re-verified: full rebuild (`indexed-helper` + `indexed_tests` targets), full
test suite (209/209), `clang-format`/`cppcheck` clean, `pre-commit run --all-files` and
`--hook-stage pre-push --all-files` both green.

**Not independently verified this pass** (documented rather than silently claimed --
rule 0): the `dependency-scan` job, and the `benchmark-action/github-action-benchmark`
trend-tracking step, only run on GitHub's runners and were not exercised by pushing a
commit. Both were config-reviewed against upstream documentation (fetched during this
session) and their inputs traced by hand, but "the YAML is correct" and "it ran green in
CI" are different claims -- the first is what's recorded here.
