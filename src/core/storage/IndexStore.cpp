#include "storage/IndexStore.h"

#include <algorithm>
#include <mutex>

namespace indexed {

namespace {

// True when `path` is pathPrefix itself or a true descendant of it
// (pathPrefix + "/" + anything). A bare prefix match ("/home/user2..."
// against "/home/user") must NOT count — that's a sibling directory, not a
// descendant.
bool IsUnderPath(std::string_view path, std::string_view pathPrefix) {
    if (path == pathPrefix) {
        return true;
    }
    return path.size() > pathPrefix.size() && path.substr(0, pathPrefix.size()) == pathPrefix &&
           path[pathPrefix.size()] == '/';
}

}  // namespace

void IndexStore::BeginWrite() {
    stagingPool_ = IndexPool();
}

void IndexStore::AddEntry(const FileEntry& entry) {
    stagingPool_.AddEntry(entry);
}

void IndexStore::EndWrite() {
    std::unique_lock lock(mutex_);
    pool_ = std::move(stagingPool_);
}

void IndexStore::ApplyAdd(const FileEntry& entry) {
    std::unique_lock lock(mutex_);
    pool_.AddEntry(entry);
}

void IndexStore::ApplyRemove(std::string_view path) {
    std::unique_lock lock(mutex_);
    auto found = pool_.FindByPath(path);
    if (found.has_value()) {
        pool_.MarkDeleted(*found);
    }
}

void IndexStore::ApplyRename(std::string_view oldPath, std::string_view newPath) {
    std::unique_lock lock(mutex_);
    auto found = pool_.FindByPath(oldPath);
    if (found.has_value()) {
        pool_.MarkDeleted(*found);
    }

    FileEntry entry;
    entry.path = std::string(newPath);
    auto slashPos = entry.path.find_last_of('/');
    entry.name = slashPos == std::string::npos ? entry.path : entry.path.substr(slashPos + 1);
    pool_.AddEntry(entry);
}

void IndexStore::RemoveEntriesUnderPath(std::string_view pathPrefix) {
    std::unique_lock lock(mutex_);
    for (size_t i = 0; i < pool_.Count(); ++i) {
        if (pool_.IsDeleted(i)) {
            continue;
        }
        if (IsUnderPath(pool_.GetEntry(i).path, pathPrefix)) {
            pool_.MarkDeleted(i);
        }
    }
}

const IndexPool& IndexStore::GetPool() const {
    return pool_;
}

std::shared_mutex& IndexStore::GetSearchMutex() {
    return mutex_;
}

void IndexStore::SetBuildTimestamp(uint64_t nsSinceEpoch) {
    std::unique_lock lock(mutex_);
    buildTimestampNs_ = nsSinceEpoch;
}

uint64_t IndexStore::GetIndexAgeSeconds(uint64_t nowNs) const {
    if (nowNs <= buildTimestampNs_) {
        return 0;
    }
    return (nowNs - buildTimestampNs_) / 1'000'000'000ULL;
}

void IndexStore::SetLastMonitorStop(uint64_t nsSinceEpoch) {
    std::unique_lock lock(mutex_);
    lastMonitorStopNs_ = nsSinceEpoch;
}

uint64_t IndexStore::GetLastMonitorStop() const {
    return lastMonitorStopNs_;
}

}  // namespace indexed
