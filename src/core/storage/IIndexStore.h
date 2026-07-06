#pragma once

#include "indexer/IFileSystemScanner.h"
#include "storage/IndexPool.h"
#include <cstdint>
#include <shared_mutex>
#include <string_view>

namespace indexed {

// Pure-virtual interface extracted from IndexStore (indexed-plan.md §6.1/§7.7)
// so Indexer (M3) can be unit-tested against a MockIndexStore instead of
// depending on the concrete storage implementation. Method set mirrors
// IndexStore exactly; see IndexStore.h for behavioral documentation of each
// method.
class IIndexStore {
public:
    virtual ~IIndexStore() = default;

    virtual void BeginWrite() = 0;
    virtual void AddEntry(const FileEntry& entry) = 0;
    virtual void EndWrite() = 0;

    virtual void ApplyAdd(const FileEntry& entry) = 0;
    virtual void ApplyRemove(std::string_view path) = 0;
    virtual void ApplyRename(std::string_view oldPath, std::string_view newPath) = 0;
    virtual void RemoveEntriesUnderPath(std::string_view pathPrefix) = 0;

    // Atomically replaces the live pool with an already-built pool (e.g. from
    // IndexSerializer::Load), setting both timestamps under the same
    // exclusive lock. Distinct from BeginWrite/AddEntry/EndWrite, which stage
    // a pool entry-by-entry for a fresh scan; this installs a pool that
    // already exists in full.
    virtual void LoadPool(IndexPool pool, uint64_t buildTimestampNs,
                          uint64_t lastMonitorStopNs) = 0;

    virtual const IndexPool& GetPool() const = 0;
    virtual std::shared_mutex& GetSearchMutex() = 0;

    virtual void SetBuildTimestamp(uint64_t nsSinceEpoch) = 0;
    virtual uint64_t GetIndexAgeSeconds(uint64_t nowNs) const = 0;

    virtual void SetLastMonitorStop(uint64_t nsSinceEpoch) = 0;
    virtual uint64_t GetLastMonitorStop() const = 0;
};

}  // namespace indexed
