// Search-path benchmark (docs/adr/0011): indexed's core product claim is
// "no perceptible delay even across millions of files" -- this measures
// SearchEngine::Search over a synthetic pool of realistic size, tracked
// over time via CI's benchmark trend step.

#include <benchmark/benchmark.h>

#include "search/SearchEngine.h"
#include "storage/IndexPool.h"
#include <atomic>
#include <string>

using indexed::FileEntry;
using indexed::IndexPool;
using indexed::SearchEngine;
using indexed::SearchOptions;

namespace {

std::atomic<bool> kNeverCancel{false};

FileEntry MakeEntry(int i) {
    FileEntry entry;
    entry.name = "file_" + std::to_string(i) + ".txt";
    entry.path = "/mnt/data/dir_" + std::to_string(i % 500) + "/" + entry.name;
    entry.size = static_cast<uint64_t>(i);
    entry.lastModified = 1;
    return entry;
}

IndexPool BuildPool(int64_t entryCount) {
    IndexPool pool;
    for (int64_t i = 0; i < entryCount; ++i) {
        pool.AddEntry(MakeEntry(static_cast<int>(i)));
    }
    // One easily findable needle so every mode below returns real matches
    // rather than scanning-then-finding-nothing.
    FileEntry needle;
    needle.name = "TargetReport.pdf";
    needle.path = "/mnt/data/dir_1/TargetReport.pdf";
    pool.AddEntry(needle);
    return pool;
}

}  // namespace

static void BM_SubstringSearch(benchmark::State& state) {
    IndexPool pool = BuildPool(state.range(0));
    SearchEngine engine;
    SearchOptions options;

    for (auto _ : state) {
        auto results = engine.Search(pool, "targetreport", options, kNeverCancel);
        benchmark::DoNotOptimize(results);
    }
    state.SetItemsProcessed(state.iterations() * pool.Count());
}
BENCHMARK(BM_SubstringSearch)->Arg(1'000)->Arg(100'000)->Arg(1'000'000);

static void BM_TokenSetSearch(benchmark::State& state) {
    IndexPool pool = BuildPool(state.range(0));
    SearchEngine engine;
    SearchOptions options;

    for (auto _ : state) {
        auto results = engine.Search(pool, "target report", options, kNeverCancel);
        benchmark::DoNotOptimize(results);
    }
    state.SetItemsProcessed(state.iterations() * pool.Count());
}
BENCHMARK(BM_TokenSetSearch)->Arg(1'000)->Arg(100'000)->Arg(1'000'000);

static void BM_RegexSearch(benchmark::State& state) {
    IndexPool pool = BuildPool(state.range(0));
    SearchEngine engine;
    SearchOptions options;
    options.useRegex = true;

    for (auto _ : state) {
        auto results = engine.Search(pool, "^Target.*\\.pdf$", options, kNeverCancel);
        benchmark::DoNotOptimize(results);
    }
    state.SetItemsProcessed(state.iterations() * pool.Count());
}
BENCHMARK(BM_RegexSearch)->Arg(1'000)->Arg(100'000)->Arg(1'000'000);

BENCHMARK_MAIN();
