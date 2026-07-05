#pragma once

#include "indexer/IFileSystemScanner.h"
#include "storage/IndexPool.h"
#include <cstdint>
#include <shared_mutex>
#include <string_view>

namespace indexed {

// Owns the live IndexPool and mediates all access to it (docs/adr/0006).
//
// Two write paths:
//  - Bulk build: BeginWrite() resets a staging pool, AddEntry() stages
//    entries into it, EndWrite() atomically swaps the staged pool into
//    place under the exclusive lock. Readers via GetPool() never see a
//    partially-built pool — they keep seeing the previous generation until
//    EndWrite() completes.
//  - Incremental: ApplyAdd/ApplyRemove/ApplyRename/RemoveEntriesUnderPath
//    mutate the live pool in place (each takes the exclusive lock itself),
//    applied by the Indexer (M3) in response to fanotify/inotify events.
//
// Staleness tracking replaces winindex's USN-cursor map: fanotify has no
// cursor to replay (docs/adr/0007-fanotify-vs-inotify-monitoring.md), so
// staleness is tracked via a build timestamp plus a lastMonitorStop
// timestamp instead.
class IndexStore {
public:
    void BeginWrite();
    void AddEntry(const FileEntry& entry);
    void EndWrite();

    void ApplyAdd(const FileEntry& entry);
    void ApplyRemove(std::string_view path);
    void ApplyRename(std::string_view oldPath, std::string_view newPath);
    void RemoveEntriesUnderPath(std::string_view pathPrefix);

    // Search-thread access. Neither of these locks internally — a caller on
    // the search thread must hold a shared lock via GetSearchMutex() for the
    // duration of use. Single-threaded test code may call them without
    // locking (no concurrent writer in a test).
    const IndexPool& GetPool() const;
    std::shared_mutex& GetSearchMutex();

    // buildTimestampNs / nowNs are both nanoseconds since the Unix epoch.
    // "now" is an explicit parameter (rather than reading the system clock
    // inside GetIndexAgeSeconds) to keep this deterministically unit-testable.
    void SetBuildTimestamp(uint64_t nsSinceEpoch);
    uint64_t GetIndexAgeSeconds(uint64_t nowNs) const;

    void SetLastMonitorStop(uint64_t nsSinceEpoch);
    uint64_t GetLastMonitorStop() const;

private:
    IndexPool pool_;
    IndexPool stagingPool_;
    mutable std::shared_mutex mutex_;
    uint64_t buildTimestampNs_ = 0;
    uint64_t lastMonitorStopNs_ = 0;
};

}  // namespace indexed
