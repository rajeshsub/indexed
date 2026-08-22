Status: Accepted

## Context

`indexed`'s core product claim is search with "no perceptible delay even across millions
of files" (README). Nothing currently measures whether that stays true as the code
changes: no timing capture on the search/scan hot paths, no historical trend. An
engineering-standards audit flagged this against the project's execution-timing rule for
latency-sensitive code paths.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|-----------------|-----------|
| a. Google Benchmark (FetchContent), new `benchmarks/` dir mirroring `tests/` | Idiomatic, established C++ micro/macro-benchmark tool; project already FetchContents GoogleTest from the same organization | One new FetchContent block, one new CMake target | Add more benchmark files as more hot paths are identified | Adds a build-time dependency (mitigated: same FetchContent pattern as every other dep, ADR-0004) |
| b. Hand-rolled timer (`std::chrono` wrapper + manual stats) | Wanting zero new dependencies | No new FetchContent block | Would need hand-rolled statistics (mean/stddev/outlier handling) as needs grow | Reinvents what Google Benchmark already gives correctly; more code to maintain for a worse result |

## Decision

Use **option a**. Add Google Benchmark via `FetchContent` (same pattern as
`googletest`/`abseil`/`re2`/`utf8proc`, ADR-0004), pinned to a specific `GIT_TAG`. New
`benchmarks/` directory, structured like `tests/`, covering the two hot paths named in the
audit: search query execution (`SearchEngine`/`TokenMatcher`) and directory-walk scanning
(`WalkScanner`). Benchmarks run with `--benchmark_format=json` into
`benchmark-results/`; CI parses `ctest`'s own per-test timings into the same directory so
both are covered by one trend step.

## Consequences

- `CMakeLists.txt` gains a `benchmark` FetchContent block and a `BUILD_BENCHMARKS` option
  (default `OFF` locally, `ON` in the dedicated CI job) so a normal debug/test build
  doesn't pay the extra configure/compile cost.
- `benchmarks/` is a new top-level directory (`benchmarks/CMakeLists.txt`,
  `bench_SearchEngine.cpp`, `bench_WalkScanner.cpp`).
- CI gets a new job (or step) that builds and runs the benchmarks, writes JSON results
  under `benchmark-results/`, and renders a trend into the job summary (or an uploaded
  artifact) so regressions surface without manual digging.
- Adding a further hot-path benchmark is a new `.cpp` file in `benchmarks/` plus one
  `CMakeLists.txt` line -- no new pattern required.
