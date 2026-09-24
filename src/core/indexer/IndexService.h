#pragma once

#include "indexer/IFileSystemScanner.h"
#include "indexer/Indexer.h"
#include "storage/IIndexStore.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace indexed {

// What changed in the live index, reported after the change is complete.
struct IndexChange {
    enum class Kind { Built, SettingsApplied, Reloaded, PathsRemoved };
    Kind kind = Kind::Built;
    uint64_t entryCount = 0;
    uint64_t indexAgeSeconds = 0;
};

struct FileOperation {
    enum class Type { MoveToTrash, DeletePermanently };
    Type type = Type::MoveToTrash;
    std::string path;
};

struct FileOperationReport {
    std::vector<std::string> succeeded;
    // path, reason
    std::vector<std::pair<std::string, std::string>> failed;
};

// Performs one file operation; returns std::nullopt on success or a
// human-readable reason on failure. Injected because moving to Trash is a
// desktop (Qt) concern and core stays Qt-free.
using FileOperationRunner = std::function<std::optional<std::string>(const FileOperation&)>;

// Every callback runs on one of IndexService's own threads, never the
// caller's. A GUI must hop to its UI thread before touching widgets.
struct IndexServiceCallbacks {
    // Scan progress (throttled to one Scanning update per 100 ms; state
    // changes are always delivered).
    StatusCallback onStatus;
    // True when a full rebuild or a Settings change starts, false when the
    // last queued one has finished.
    std::function<void(bool busy)> onBusyChanged;
    std::function<void(const IndexChange&)> onIndexChanged;
    // A live-monitoring change was applied to the store.
    std::function<void()> onLiveChange;
    std::function<void(const FileOperationReport&)> onFileOperationsFinished;
};

// Owns every piece of index work so a GUI thread never has to block on it
// (docs/adr/0014): scanning, loading and saving the index file, applying
// Settings changes, reloading a file written by the elevated helper, and
// starting/stopping live monitoring all run on one worker thread, in
// request order. File operations (Trash/Delete) run on a second thread so a
// slow one never waits behind a scan or delays one.
//
// Every Request* call only enqueues and returns immediately. A new
// rebuild or Settings change cancels a running or queued scan, so the index
// always ends up reflecting the latest request.
class IndexService {
public:
    IndexService(IFileSystemScanner& scanner, IIndexStore& store,
                 ChangeMonitorFactory monitorFactory, std::string idxFilePath,
                 FileOperationRunner fileOperationRunner, IndexServiceCallbacks callbacks,
                 std::function<uint64_t()> clockNs);
    ~IndexService();

    IndexService(const IndexService&) = delete;
    IndexService& operator=(const IndexService&) = delete;

    // Load-if-fresh (force == false) or full scan, then save and, if
    // monitorAfter, (re)start live monitoring of options.rootPaths.
    void RequestIndexing(bool force, ScanOptions options, uint64_t staleThresholdSeconds,
                         bool monitorAfter);

    // Applies a change of indexed roots: roots only added -> incremental add;
    // only removed -> incremental remove; both -> full rebuild. The index is
    // saved either way, and monitoring restarted on the new roots if
    // monitorAfter.
    void RequestSettingsChange(std::vector<std::string> oldRoots, ScanOptions newOptions,
                               uint64_t staleThresholdSeconds, bool monitorAfter);

    // Loads the index file written by the elevated helper and swaps it in.
    // Requests arriving while one is queued or running cause at most one
    // more load. A missing or invalid file leaves the live index untouched.
    void RequestReload();

    // Runs the operations on the file-operations thread, removes every
    // successful path from the index, then reports once for the batch.
    void RequestFileOperations(std::vector<FileOperation> operations);

    // Cancels any running or queued scan and stops live monitoring (e.g.
    // when the elevated helper takes over indexing).
    void RequestStopLocalIndexing();

    // Blocks until every command and file-operation batch requested before
    // this call has finished. For tests and orderly shutdown only; never
    // call it from a UI thread.
    void Flush();

    // Cancels any scan, stops monitoring, lets queued file operations
    // finish, and joins every thread. Called by the destructor.
    void Shutdown();

private:
    void WorkerLoop();
    void FileOperationsLoop();
    bool Enqueue(std::function<void()> command);
    bool EnqueueFileJob(std::function<void()> job);
    // Re-applies removals made during a scan; true if there were any.
    bool FinishScan();
    void CancelScans();
    void RunIndexing(bool force, const ScanOptions& options, uint64_t staleThresholdSeconds,
                     bool monitorAfter, const std::shared_ptr<std::atomic<bool>>& cancel);
    void RunSettingsChange(const std::vector<std::string>& oldRoots, const ScanOptions& newOptions,
                           uint64_t staleThresholdSeconds, bool monitorAfter,
                           const std::shared_ptr<std::atomic<bool>>& cancel);
    void RunReload();
    void StartMonitoring(const std::vector<std::string>& roots);
    void StopMonitoring();
    void ReportStatus(const IndexerStatus& status);
    void ReportIndexChanged(IndexChange::Kind kind);
    void BeginBusy();
    void EndBusy();

    IFileSystemScanner& scanner_;
    IIndexStore& store_;
    std::string idxFilePath_;
    FileOperationRunner fileOperationRunner_;
    IndexServiceCallbacks callbacks_;
    std::function<uint64_t()> clockNs_;
    Indexer indexer_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> commands_;
    std::vector<std::shared_ptr<std::atomic<bool>>> scanCancels_;
    bool reloadQueued_ = false;
    bool shuttingDown_ = false;
    // Rebuild/Settings commands requested but not yet finished; worker-side
    // flag for whether "busy" has been reported.
    int pendingBusy_ = 0;
    bool busyReported_ = false;
    bool hasMonitorFactory_ = false;
    // Paths removed by file operations while a scan was running: the scan
    // may have seen them before they were removed, so they are removed again
    // once it has been swapped in.
    std::vector<std::string> removedDuringScan_;
    bool scanRunning_ = false;
    // Set when an incremental add was cancelled part-way: the store then
    // holds part of a root, so the next Settings change rebuilds in full.
    bool needsFullRebuild_ = false;

    std::mutex fileOpsMutex_;
    std::condition_variable fileOpsCv_;
    std::deque<std::function<void()>> fileOpJobs_;
    bool fileOpsStopping_ = false;

    std::mutex statusMutex_;
    std::chrono::steady_clock::time_point lastScanStatus_{};

    std::atomic<bool> monitorStop_{false};
    std::thread monitorThread_;

    std::thread worker_;
    std::thread fileOpsWorker_;
};

}  // namespace indexed
