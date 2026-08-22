// Directory-walk scan benchmark (docs/adr/0011): measures WalkScanner over a
// synthetic tree on disk, tracked over time via CI's benchmark trend step.
// docs/adr/0002-directory-walk-scanning-strategy.md covers the getdents64 +
// statx design being measured here.

#include <benchmark/benchmark.h>

#include "indexer/WalkScanner.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using indexed::FileEntry;
using indexed::ScanOptions;
using indexed::WalkScanner;

namespace {

namespace fs = std::filesystem;

std::atomic<bool> kNeverCancel{false};

// Builds `dirCount` subdirectories, each with `filesPerDir` empty files,
// under a fresh temp root. Returns the root path.
std::string BuildTree(int64_t dirCount, int64_t filesPerDir) {
    std::string root = (fs::temp_directory_path() / "indexed_bench_walkscanner").string();
    fs::remove_all(root);
    for (int64_t d = 0; d < dirCount; ++d) {
        fs::path dir = fs::path(root) / ("dir_" + std::to_string(d));
        fs::create_directories(dir);
        for (int64_t f = 0; f < filesPerDir; ++f) {
            std::ofstream(dir / ("file_" + std::to_string(f) + ".txt")).put('x');
        }
    }
    return root;
}

}  // namespace

static void BM_WalkScannerScan(benchmark::State& state) {
    const int64_t dirCount = 50;
    const int64_t filesPerDir = state.range(0) / dirCount;
    std::string root = BuildTree(dirCount, filesPerDir);

    ScanOptions options;
    options.rootPaths = {root};

    for (auto _ : state) {
        std::atomic<uint64_t> entriesFound{0};
        WalkScanner scanner;
        scanner.Scan(
            options, [&entriesFound](const FileEntry&) { ++entriesFound; },
            [](uint64_t, const std::string&) {}, kNeverCancel);
        benchmark::DoNotOptimize(entriesFound.load());
    }
    state.SetItemsProcessed(state.iterations() * dirCount * filesPerDir);

    fs::remove_all(root);
}
BENCHMARK(BM_WalkScannerScan)->Arg(5'000)->Arg(50'000)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
