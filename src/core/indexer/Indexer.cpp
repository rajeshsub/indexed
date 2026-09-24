#include "indexer/Indexer.h"

#include "storage/IndexSerializer.h"
#include <atomic>
#include <shared_mutex>
#include <thread>
#include <utility>

namespace indexed {

Indexer::Indexer(IFileSystemScanner& scanner, IIndexStore& store,
                 ChangeMonitorFactory monitorFactory, StatusCallback statusCallback,
                 MutationCallback mutationCallback, IndexFileIo indexFileIo)
    : scanner_(scanner),
      store_(store),
      monitorFactory_(std::move(monitorFactory)),
      statusCallback_(std::move(statusCallback)),
      mutationCallback_(std::move(mutationCallback)),
      indexFileIo_(std::move(indexFileIo)) {}

void Indexer::ReportStatus(IndexerState state, std::string message, uint64_t filesIndexed,
                           std::vector<std::string> locations, uint64_t indexAgeSeconds) {
    if (!statusCallback_) {
        return;
    }
    IndexerStatus status;
    status.state = state;
    status.message = std::move(message);
    status.filesIndexed = filesIndexed;
    status.locations = std::move(locations);
    status.indexAgeSeconds = indexAgeSeconds;
    statusCallback_(status);
}

bool Indexer::StartIndexing(bool force, const ScanOptions& options, const std::string& idxFilePath,
                            uint64_t nowNs, uint64_t staleThresholdSeconds,
                            const std::atomic<bool>* cancelToken) {
    if (!force) {
        IndexSerializer::LoadResult result =
            indexFileIo_.load ? indexFileIo_.load(idxFilePath) : IndexSerializer::Load(idxFilePath);
        if (result.success) {
            const uint64_t ageSeconds = nowNs > result.buildTimestampNs
                                            ? (nowNs - result.buildTimestampNs) / 1'000'000'000ULL
                                            : 0;
            if (ageSeconds <= staleThresholdSeconds) {
                ReportStatus(IndexerState::LoadingIndex, "Loading index from disk", 0,
                             options.rootPaths, ageSeconds);
                const uint64_t entryCount = result.pool.Count();
                store_.LoadPool(std::move(result.pool), result.buildTimestampNs,
                                result.lastMonitorStopNs);
                ReportStatus(IndexerState::Idle, "Index loaded", entryCount, options.rootPaths,
                             ageSeconds);
                return true;
            }
        }
    }

    ReportStatus(IndexerState::Scanning, "Scanning...", 0, options.rootPaths, 0);
    store_.BeginWrite();
    uint64_t filesFound = 0;
    const std::atomic<bool> neverCancelled{false};
    const std::atomic<bool>& cancel = cancelToken != nullptr ? *cancelToken : neverCancelled;
    scanner_.Scan(
        options,
        [this, &filesFound](const FileEntry& entry) {
            store_.AddEntry(entry);
            ++filesFound;
        },
        [this, &options](uint64_t found, const std::string& currentDir) {
            ReportStatus(IndexerState::Scanning, currentDir, found, options.rootPaths, 0);
        },
        cancel);
    if (cancel.load()) {
        // The staged pool is partial: never swap it in or save it.
        store_.AbortWrite();
        return false;
    }
    store_.EndWrite();
    store_.SetBuildTimestamp(nowNs);
    SaveIndex(idxFilePath, nowNs);
    ReportStatus(IndexerState::Idle, "Indexing complete", filesFound, options.rootPaths, 0);
    return true;
}

bool Indexer::IndexPaths(const std::vector<std::string>& paths,
                         const std::vector<std::string>& excludedPaths,
                         const std::atomic<bool>* cancelToken) {
    ScanOptions options;
    options.rootPaths = paths;
    options.excludedPaths = excludedPaths;
    const std::atomic<bool> neverCancelled{false};
    const std::atomic<bool>& cancel = cancelToken != nullptr ? *cancelToken : neverCancelled;
    scanner_.Scan(
        options, [this](const FileEntry& entry) { store_.ApplyAdd(entry); },
        [this, &paths](uint64_t found, const std::string& currentDir) {
            ReportStatus(IndexerState::Scanning, currentDir, found, paths, 0);
        },
        cancel);
    return !cancel.load();
}

void Indexer::RemovePaths(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        store_.RemoveEntriesUnderPath(path);
    }
}

void Indexer::PersistIndex(const std::string& idxFilePath, uint64_t nowNs) {
    store_.SetBuildTimestamp(nowNs);
    SaveIndex(idxFilePath, nowNs);
}

bool Indexer::PersistLiveChanges(const std::string& idxFilePath) {
    return SaveIndex(idxFilePath, std::nullopt);
}

bool Indexer::SaveIndex(const std::string& idxFilePath, std::optional<uint64_t> buildTimestampNs) {
    // GetPool() returns a reference into the live store with no locking of
    // its own (see IIndexStore::GetPool), and a concurrent live-monitoring
    // mutation on another thread can reallocate its backing vectors
    // mid-serialize, so serializing holds the shared lock. Writing (and
    // fsyncing) the result happens after it is released, so those mutations
    // aren't stalled on disk I/O.
    std::vector<char> bytes;
    {
        std::shared_lock lock(store_.GetSearchMutex());
        bytes = IndexSerializer::Serialize(store_.GetPool(),
                                           buildTimestampNs.value_or(store_.GetBuildTimestamp()),
                                           store_.GetLastMonitorStop());
    }
    if (indexFileIo_.save) {
        return indexFileIo_.save(idxFilePath, bytes);
    }
    return IndexSerializer::WriteFileAtomically(idxFilePath, bytes);
}

void Indexer::StartLiveMonitoring(const std::vector<std::string>& roots,
                                  const std::atomic<bool>& stopToken) {
    ReportStatus(IndexerState::WatchingForChanges, "Watching for changes", 0, roots, 0);

    std::vector<std::unique_ptr<IChangeMonitor>> monitors;
    std::vector<std::thread> threads;
    monitors.reserve(roots.size());
    threads.reserve(roots.size());

    for (const std::string& root : roots) {
        std::unique_ptr<IChangeMonitor> monitor = monitorFactory_(root);
        if (!monitor) {
            continue;
        }
        IChangeMonitor* monitorPtr = monitor.get();
        monitors.push_back(std::move(monitor));
        threads.emplace_back([this, monitorPtr, root, &stopToken]() {
            monitorPtr->StartMonitoring(
                root, [this](const FileChangeEvent& event) { ApplyChangeEvent(event); }, stopToken);
        });
    }

    for (std::thread& t : threads) {
        t.join();
    }
}

void Indexer::ApplyChangeEvent(const FileChangeEvent& event) {
    switch (event.type) {
        case FileChangeType::Removed:
            store_.ApplyRemove(event.path);
            NotifyMutation();
            return;
        case FileChangeType::Renamed:
            store_.ApplyRename(event.oldPath, event.path);
            NotifyMutation();
            return;
        case FileChangeType::Modified:
            // Pool entries are append-only (docs/adr/0006) -- there is no
            // "modify in place". Drop the stale record, then fall through to
            // the Added path to re-scan and add the fresh one.
            store_.ApplyRemove(event.path);
            [[fallthrough]];
        case FileChangeType::Added: {
            // fanotify/inotify report a bare path with no metadata attached
            // (indexed-plan.md §7.2); re-scan just this path via the
            // injected scanner to obtain a full FileEntry, mirroring what a
            // real backend would do with `stat`.
            ScanOptions options;
            options.rootPaths = {event.path};
            std::atomic<bool> cancelToken{false};
            bool found = false;
            FileEntry entry;
            scanner_.Scan(
                options,
                [&entry, &found](const FileEntry& scanned) {
                    entry = scanned;
                    found = true;
                },
                [](uint64_t, const std::string&) {}, cancelToken);
            if (found) {
                store_.ApplyAdd(entry);
            }
            // A Modified event always mutated (the ApplyRemove above); an
            // Added event only mutated if the re-scan found the file.
            if (found || event.type == FileChangeType::Modified) {
                NotifyMutation();
            }
            return;
        }
    }
}

void Indexer::NotifyMutation() {
    if (mutationCallback_) {
        mutationCallback_();
    }
}

}  // namespace indexed
