#include "indexer/Indexer.h"

#include "storage/IndexSerializer.h"
#include <atomic>
#include <thread>
#include <utility>

namespace indexed {

Indexer::Indexer(IFileSystemScanner& scanner, IIndexStore& store,
                 ChangeMonitorFactory monitorFactory, StatusCallback statusCallback)
    : scanner_(scanner),
      store_(store),
      monitorFactory_(std::move(monitorFactory)),
      statusCallback_(std::move(statusCallback)) {}

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

void Indexer::StartIndexing(bool force, const ScanOptions& options, const std::string& idxFilePath,
                            uint64_t nowNs, uint64_t staleThresholdSeconds) {
    if (!force) {
        IndexSerializer::LoadResult result = IndexSerializer::Load(idxFilePath);
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
                return;
            }
        }
    }

    ReportStatus(IndexerState::Scanning, "Scanning...", 0, options.rootPaths, 0);
    store_.BeginWrite();
    uint64_t filesFound = 0;
    std::atomic<bool> cancelToken{false};
    scanner_.Scan(
        options,
        [this, &filesFound](const FileEntry& entry) {
            store_.AddEntry(entry);
            ++filesFound;
        },
        [this, &options](uint64_t found, const std::string& currentDir) {
            ReportStatus(IndexerState::Scanning, currentDir, found, options.rootPaths, 0);
        },
        cancelToken);
    store_.EndWrite();
    store_.SetBuildTimestamp(nowNs);
    IndexSerializer::Save(idxFilePath, store_.GetPool(), nowNs, store_.GetLastMonitorStop());
    ReportStatus(IndexerState::Idle, "Indexing complete", filesFound, options.rootPaths, 0);
}

void Indexer::IndexPaths(const std::vector<std::string>& paths) {
    ScanOptions options;
    options.rootPaths = paths;
    std::atomic<bool> cancelToken{false};
    scanner_.Scan(
        options, [this](const FileEntry& entry) { store_.ApplyAdd(entry); },
        [this, &paths](uint64_t found, const std::string& currentDir) {
            ReportStatus(IndexerState::Scanning, currentDir, found, paths, 0);
        },
        cancelToken);
}

void Indexer::RemovePaths(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        store_.RemoveEntriesUnderPath(path);
    }
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
            return;
        case FileChangeType::Renamed:
            store_.ApplyRename(event.oldPath, event.path);
            return;
        case FileChangeType::Modified:
            // Pool entries are append-only (docs/adr/0006) — there is no
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
            return;
        }
    }
}

}  // namespace indexed
