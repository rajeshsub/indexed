#pragma once

#include "indexer/IChangeMonitor.h"
#include "indexer/IFileSystemScanner.h"
#include "storage/IIndexStore.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace indexed {

// Indexer orchestration states (indexed-plan.md §7.7). Error is reserved for
// I/O or scan failures surfaced by future milestones (M4/M5 wire real
// scanner/monitor backends); nothing in M3's mock-driven paths reaches it.
enum class IndexerState { Idle, Scanning, LoadingIndex, WatchingForChanges, Error };

struct IndexerStatus {
    IndexerState state = IndexerState::Idle;
    std::string message;
    uint64_t filesIndexed = 0;
    std::vector<std::string> skippedPaths;
    std::vector<std::string> locations;
    uint64_t indexAgeSeconds = 0;
};

using StatusCallback = std::function<void(const IndexerStatus&)>;

// Produces an IChangeMonitor for a given root. A real factory (M5) will
// choose FanotifyMonitor vs InotifyWatcher per indexed-plan.md §7.2's
// selection logic; Indexer itself stays agnostic and only depends on the
// IChangeMonitor interface.
using ChangeMonitorFactory =
    std::function<std::unique_ptr<IChangeMonitor>(const std::string& root)>;

// Orchestrates scanning, storage, and (M5) live monitoring, ported from
// winindex's Indexer (indexed-plan.md §7.7). Dependency-injected per §6:
// depends only on the IFileSystemScanner/IIndexStore/IChangeMonitor
// interfaces, never on a concrete scanner/monitor implementation, so it is
// fully unit-testable against mocks (tests/test_Indexer.cpp) before
// WalkScanner / FanotifyMonitor / InotifyWatcher exist.
class Indexer {
public:
    Indexer(IFileSystemScanner& scanner, IIndexStore& store, ChangeMonitorFactory monitorFactory,
            StatusCallback statusCallback = nullptr);

    // If !force and a valid, non-stale (age <= staleThresholdSeconds) index
    // exists at idxFilePath, loads it into the store (LoadingIndex -> Idle).
    // Otherwise scans options.rootPaths via the injected scanner, streams
    // discovered entries into the store (BeginWrite/AddEntry/EndWrite), sets
    // the store's build timestamp to nowNs, and saves the result to
    // idxFilePath (Scanning -> Idle). staleThresholdSeconds stands in for
    // Settings' ReindexIntervalHours (a sibling track not yet available to
    // M3, indexed-plan.md §7.7) as an explicit parameter instead.
    void StartIndexing(bool force, const ScanOptions& options, const std::string& idxFilePath,
                       uint64_t nowNs, uint64_t staleThresholdSeconds);

    // Incremental add: scans just `paths` and adds each discovered entry via
    // IIndexStore::ApplyAdd (not a full rebuild).
    void IndexPaths(const std::vector<std::string>& paths);

    // Incremental remove: marks every entry under each path deleted via
    // IIndexStore::RemoveEntriesUnderPath.
    void RemovePaths(const std::vector<std::string>& paths);

    // Obtains one IChangeMonitor per root via the injected factory and starts
    // each on its own background thread; blocks (joining those threads)
    // until every monitor's StartMonitoring returns, i.e. until stopToken is
    // set and each backend's blocking loop exits. Each monitor's onChange
    // callback is wired to ApplyChangeEvent.
    void StartLiveMonitoring(const std::vector<std::string>& roots,
                             const std::atomic<bool>& stopToken);

    // The "event -> store mutation" logic factored out so it is directly
    // unit-testable with hand-built FileChangeEvents against a mock
    // store/scanner, without needing a real IChangeMonitor or real
    // filesystem access: Added/Modified re-scan just event.path via the
    // injected IFileSystemScanner to obtain a full FileEntry, exactly like a
    // real backend would need to `stat` the changed path.
    void ApplyChangeEvent(const FileChangeEvent& event);

private:
    void ReportStatus(IndexerState state, std::string message, uint64_t filesIndexed,
                      std::vector<std::string> locations, uint64_t indexAgeSeconds);

    IFileSystemScanner& scanner_;
    IIndexStore& store_;
    ChangeMonitorFactory monitorFactory_;
    StatusCallback statusCallback_;
};

}  // namespace indexed
