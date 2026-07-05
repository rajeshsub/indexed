#pragma once

#include "indexer/IFileSystemScanner.h"
#include "storage/EntryMeta.h"
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace indexed {

// Flat-pool in-memory index layout (docs/adr/0006-pool-based-index-layout.md):
// name/path strings live in two contiguous byte pools instead of per-entry
// heap allocations, so a sequential search scan stays cache-resident instead
// of chasing scattered pointers. nameLowerPool is never persisted to disk —
// LoadFromPathPool rebuilds it from pathPool via ASCII case-fold.
class IndexPool {
public:
    struct EntryView {
        std::string_view name;
        std::string_view nameLower;
        std::string_view path;
        uint64_t size = 0;
        uint64_t lastModified = 0;
        uint32_t attributes = 0;
    };

    // Appends entry to the pools; entries are never physically removed
    // (offsets must stay stable), see MarkDeleted for removal.
    void AddEntry(const FileEntry& entry);

    size_t Count() const;
    EntryView GetEntry(size_t index) const;

    void MarkDeleted(size_t index);
    bool IsDeleted(size_t index) const;

    // Linear scan; acceptable at the scale of incremental changes applied by
    // IndexStore (M1). Revisit with a path->index map only if profiling shows
    // it's needed.
    std::optional<size_t> FindByPath(std::string_view path) const;

    const std::vector<EntryMeta>& Meta() const;
    const std::vector<char>& PathPool() const;
    const std::vector<char>& NameLowerPool() const;

    // Reconstructs a pool from serialized meta + pathPool (as IndexSerializer::Load
    // does), rebuilding nameLowerPool since it is never persisted on disk.
    static IndexPool LoadFromPathPool(std::vector<EntryMeta> meta, std::vector<char> pathPool);

private:
    std::vector<EntryMeta> meta_;
    std::vector<char> nameLowerPool_;
    std::vector<char> pathPool_;
};

}  // namespace indexed
