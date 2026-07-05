#pragma once

#include "storage/IndexPool.h"
#include <cstdint>
#include <string>

namespace indexed {

// On-disk index format v1 (indexed.idx) — docs/adr/0003-binary-index-format.md,
// indexed-plan.md §10. Custom binary layout with a CRC-32 integrity check over
// everything after the header. nameLower is never persisted: Load rebuilds it via
// IndexPool::LoadFromPathPool from pathPool + nameStart/pathLen.
class IndexSerializer {
public:
    // Writes pool's Meta()/PathPool() plus the two timestamps to filepath in the v1
    // on-disk format. Returns false on any I/O failure; does not throw.
    static bool Save(const std::string& filepath, const IndexPool& pool, uint64_t buildTimestampNs,
                     uint64_t lastMonitorStopNs);

    struct LoadResult {
        bool success = false;
        IndexPool pool;
        uint64_t buildTimestampNs = 0;
        uint64_t lastMonitorStopNs = 0;
    };

    // Reads filepath and validates magic/version/CRC-32. On any mismatch, truncation,
    // or I/O failure (including a missing file) returns a LoadResult with
    // success = false rather than throwing — the caller (M3's Indexer) is responsible
    // for triggering a rebuild when that happens.
    static LoadResult Load(const std::string& filepath);
};

}  // namespace indexed
