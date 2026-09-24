#include "indexer/IndexService.h"

#include "settings/RootsDiff.h"
#include "storage/IndexSerializer.h"
#include <future>
#include <shared_mutex>
#include <utility>

namespace indexed {

namespace {

constexpr auto kScanStatusInterval = std::chrono::milliseconds(100);

}  // namespace

IndexService::IndexService(IFileSystemScanner& scanner, IIndexStore& store,
                           ChangeMonitorFactory monitorFactory, std::string idxFilePath,
                           FileOperationRunner fileOperationRunner, IndexServiceCallbacks callbacks,
                           std::function<uint64_t()> clockNs)
    : scanner_(scanner),
      store_(store),
      idxFilePath_(std::move(idxFilePath)),
      fileOperationRunner_(std::move(fileOperationRunner)),
      callbacks_(std::move(callbacks)),
      clockNs_(std::move(clockNs)),
      indexer_(
          scanner, store, monitorFactory,
          [this](const IndexerStatus& status) { ReportStatus(status); },
          [this]() {
              if (callbacks_.onLiveChange) {
                  callbacks_.onLiveChange();
              }
          }),
      hasMonitorFactory_(static_cast<bool>(monitorFactory)) {
    worker_ = std::thread([this]() { WorkerLoop(); });
    fileOpsWorker_ = std::thread([this]() { FileOperationsLoop(); });
}

IndexService::~IndexService() {
    Shutdown();
}

void IndexService::RequestIndexing(bool force, ScanOptions options, uint64_t staleThresholdSeconds,
                                   bool monitorAfter) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            return;
        }
        for (const auto& previous : scanCancels_) {
            previous->store(true);
        }
        scanCancels_.assign(1, cancel);
        ++pendingBusy_;
    }
    Enqueue(
        [this, force, options = std::move(options), staleThresholdSeconds, monitorAfter, cancel]() {
            RunIndexing(force, options, staleThresholdSeconds, monitorAfter, cancel);
        });
}

void IndexService::RequestSettingsChange(std::vector<std::string> oldRoots, ScanOptions newOptions,
                                         uint64_t staleThresholdSeconds, bool monitorAfter) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            return;
        }
        for (const auto& previous : scanCancels_) {
            previous->store(true);
        }
        scanCancels_.assign(1, cancel);
        ++pendingBusy_;
    }
    Enqueue([this, oldRoots = std::move(oldRoots), newOptions = std::move(newOptions),
             staleThresholdSeconds, monitorAfter, cancel]() {
        RunSettingsChange(oldRoots, newOptions, staleThresholdSeconds, monitorAfter, cancel);
    });
}

void IndexService::RequestReload() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reloadQueued_ || shuttingDown_) {
            return;
        }
        reloadQueued_ = true;
    }
    Enqueue([this]() { RunReload(); });
}

void IndexService::RequestFileOperations(std::vector<FileOperation> operations) {
    EnqueueFileJob([this, operations = std::move(operations)]() {
        FileOperationReport report;
        for (const FileOperation& operation : operations) {
            const std::optional<std::string> failure = fileOperationRunner_(operation);
            if (failure) {
                report.failed.emplace_back(operation.path, *failure);
                continue;
            }
            // Recorded before the removal, so a scan that is swapped in
            // after this point can never bring the path back (FinishScan).
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (scanRunning_) {
                    removedDuringScan_.push_back(operation.path);
                }
            }
            store_.ApplyRemove(operation.path);
            report.succeeded.push_back(operation.path);
        }
        if (callbacks_.onFileOperationsFinished) {
            callbacks_.onFileOperationsFinished(report);
        }
        if (!report.succeeded.empty()) {
            ReportIndexChanged(IndexChange::Kind::PathsRemoved);
        }
    });
}

void IndexService::RequestStopLocalIndexing() {
    CancelScans();
    Enqueue([this]() { StopMonitoring(); });
}

void IndexService::Flush() {
    // Shared ownership: if Shutdown discards the queued command, destroying
    // the promise releases the waiter instead of leaving it blocked forever.
    auto workerDone = std::make_shared<std::promise<void>>();
    std::future<void> workerFuture = workerDone->get_future();
    if (Enqueue([workerDone]() { workerDone->set_value(); })) {
        workerDone.reset();
        workerFuture.wait();
    }
    auto fileOpsDone = std::make_shared<std::promise<void>>();
    std::future<void> fileOpsFuture = fileOpsDone->get_future();
    if (EnqueueFileJob([fileOpsDone]() { fileOpsDone->set_value(); })) {
        fileOpsDone.reset();
        fileOpsFuture.wait();
    }
}

void IndexService::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
        for (const auto& cancel : scanCancels_) {
            cancel->store(true);
        }
        scanCancels_.clear();
        commands_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    StopMonitoring();

    {
        std::lock_guard<std::mutex> lock(fileOpsMutex_);
        fileOpsStopping_ = true;
    }
    fileOpsCv_.notify_all();
    if (fileOpsWorker_.joinable()) {
        fileOpsWorker_.join();
    }
}

bool IndexService::Enqueue(std::function<void()> command) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            return false;
        }
        commands_.push_back(std::move(command));
    }
    cv_.notify_one();
    return true;
}

bool IndexService::EnqueueFileJob(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(fileOpsMutex_);
        if (fileOpsStopping_) {
            return false;
        }
        fileOpJobs_.push_back(std::move(job));
    }
    fileOpsCv_.notify_one();
    return true;
}

void IndexService::WorkerLoop() {
    while (true) {
        std::function<void()> command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return shuttingDown_ || !commands_.empty(); });
            if (shuttingDown_) {
                return;
            }
            command = std::move(commands_.front());
            commands_.pop_front();
        }
        command();
    }
}

void IndexService::FileOperationsLoop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(fileOpsMutex_);
            fileOpsCv_.wait(lock, [this]() { return fileOpsStopping_ || !fileOpJobs_.empty(); });
            // A file operation the user asked for always runs to completion,
            // shutdown or not, so no file is left half-moved.
            if (fileOpJobs_.empty()) {
                return;
            }
            job = std::move(fileOpJobs_.front());
            fileOpJobs_.pop_front();
        }
        job();
    }
}

void IndexService::CancelScans() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& cancel : scanCancels_) {
        cancel->store(true);
    }
    scanCancels_.clear();
}

void IndexService::RunIndexing(bool force, const ScanOptions& options,
                               uint64_t staleThresholdSeconds, bool monitorAfter,
                               const std::shared_ptr<std::atomic<bool>>& cancel) {
    if (!cancel->load()) {
        BeginBusy();
        StopMonitoring();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scanRunning_ = true;
        }
        const bool completed = indexer_.StartIndexing(force, options, idxFilePath_, clockNs_(),
                                                      staleThresholdSeconds, cancel.get());
        // Removals made during the scan were applied after it was saved.
        if (FinishScan() && completed) {
            indexer_.PersistLiveChanges(idxFilePath_);
        }
        if (completed) {
            needsFullRebuild_ = false;
            ReportIndexChanged(IndexChange::Kind::Built);
            if (monitorAfter) {
                StartMonitoring(options.rootPaths);
            }
        } else {
            needsFullRebuild_ = true;
        }
    } else {
        // Superseded before it ran: the store may not hold what the next
        // Settings change's old/new diff assumes (e.g. a first run's store is
        // still empty).
        needsFullRebuild_ = true;
    }
    EndBusy();
}

void IndexService::RunSettingsChange(const std::vector<std::string>& oldRoots,
                                     const ScanOptions& newOptions, uint64_t staleThresholdSeconds,
                                     bool monitorAfter,
                                     const std::shared_ptr<std::atomic<bool>>& cancel) {
    if (!cancel->load()) {
        BeginBusy();
        StopMonitoring();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scanRunning_ = true;
        }
        const RootsDiff diff = DiffRoots(oldRoots, newOptions.rootPaths);
        bool completed = true;
        if (diff.bothChanged() || needsFullRebuild_) {
            // A cancelled earlier command may have left the live index out of
            // step with oldRoots, so only a full rebuild is trustworthy then.
            completed = indexer_.StartIndexing(/*force=*/true, newOptions, idxFilePath_, clockNs_(),
                                               staleThresholdSeconds, cancel.get());
        } else if (!diff.added.empty()) {
            completed = indexer_.IndexPaths(diff.added, newOptions.excludedPaths, cancel.get());
            if (completed) {
                indexer_.PersistIndex(idxFilePath_, clockNs_());
            }
        } else if (!diff.removed.empty()) {
            indexer_.RemovePaths(diff.removed);
            indexer_.PersistIndex(idxFilePath_, clockNs_());
        }
        if (FinishScan() && completed) {
            indexer_.PersistLiveChanges(idxFilePath_);
        }
        needsFullRebuild_ = !completed;
        if (completed) {
            ReportIndexChanged(IndexChange::Kind::SettingsApplied);
            if (monitorAfter) {
                StartMonitoring(newOptions.rootPaths);
            }
        }
    } else {
        // Superseded before it ran: the live index never got these roots, so
        // the next Settings change can't trust its own old/new diff.
        needsFullRebuild_ = true;
    }
    EndBusy();
}

void IndexService::RunReload() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reloadQueued_ = false;
    }
    IndexSerializer::LoadResult result = IndexSerializer::Load(idxFilePath_);
    if (!result.success) {
        return;  // missing or mid-replacement; the helper's next save triggers another
    }
    store_.LoadPool(std::move(result.pool), result.buildTimestampNs, result.lastMonitorStopNs);
    ReportIndexChanged(IndexChange::Kind::Reloaded);
}

bool IndexService::FinishScan() {
    std::vector<std::string> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scanRunning_ = false;
        removed.swap(removedDuringScan_);
    }
    for (const std::string& path : removed) {
        store_.ApplyRemove(path);
    }
    return !removed.empty();
}

void IndexService::StartMonitoring(const std::vector<std::string>& roots) {
    if (!hasMonitorFactory_) {
        return;
    }
    StopMonitoring();
    monitorStop_.store(false);
    monitorThread_ =
        std::thread([this, roots]() { indexer_.StartLiveMonitoring(roots, monitorStop_); });
}

void IndexService::StopMonitoring() {
    monitorStop_.store(true);
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
}

void IndexService::ReportStatus(const IndexerStatus& status) {
    if (!callbacks_.onStatus) {
        return;
    }
    if (status.state == IndexerState::Scanning) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastScanStatus_ < kScanStatusInterval) {
            return;
        }
        lastScanStatus_ = now;
    }
    callbacks_.onStatus(status);
}

void IndexService::ReportIndexChanged(IndexChange::Kind kind) {
    if (!callbacks_.onIndexChanged) {
        return;
    }
    IndexChange change;
    change.kind = kind;
    {
        // Count and age come from one consistent state: the worker may be
        // swapping in a new pool (and build timestamp) concurrently.
        std::shared_lock lock(store_.GetSearchMutex());
        const IndexPool& pool = store_.GetPool();
        for (size_t i = 0; i < pool.Count(); ++i) {
            if (!pool.IsDeleted(i)) {
                ++change.entryCount;
            }
        }
        change.indexAgeSeconds = store_.GetIndexAgeSeconds(clockNs_());
    }
    callbacks_.onIndexChanged(change);
}

void IndexService::BeginBusy() {
    if (!busyReported_) {
        busyReported_ = true;
        if (callbacks_.onBusyChanged) {
            callbacks_.onBusyChanged(true);
        }
    }
}

void IndexService::EndBusy() {
    int remaining = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remaining = --pendingBusy_;
    }
    if (remaining == 0 && busyReported_) {
        busyReported_ = false;
        if (callbacks_.onBusyChanged) {
            callbacks_.onBusyChanged(false);
        }
    }
}

}  // namespace indexed
