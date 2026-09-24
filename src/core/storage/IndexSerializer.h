#pragma once

#include "storage/IndexPool.h"
#include <cstdint>
#include <string>
#include <vector>

namespace indexed {

// On-disk index format v1 (indexed.idx) -- docs/adr/0003-binary-index-format.md,
// indexed-plan.md §10. Custom binary layout with a CRC-32 integrity check over
// everything after the header. nameLower is never persisted: Load rebuilds it via
// IndexPool::LoadFromPathPool from pathPool + nameStart/pathLen.
class IndexSerializer {
public:
    // The complete v1 file image for pool's live (non-deleted) entries: header
    // followed by the CRC-covered payload. Save writes exactly these bytes; the
    // root helper writes them via ReplaceFileForRootWrite instead (docs/adr/0008).
    static std::vector<char> Serialize(const IndexPool& pool, uint64_t buildTimestampNs,
                                       uint64_t lastMonitorStopNs);

    // Writes `bytes` to filepath atomically: temp file, fsync, rename,
    // directory fsync. Returns false on any I/O failure; does not throw.
    static bool WriteFileAtomically(const std::string& filepath, const std::vector<char>& bytes);

    // Writes pool's live (non-deleted) entries plus the two timestamps to filepath in
    // the v1 on-disk format; tombstones are dropped, so a loaded pool never contains
    // any. Atomic: temp file, fsync, rename, directory fsync. Returns false on any
    // I/O failure; does not throw.
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
    // success = false rather than throwing -- the caller (M3's Indexer) is responsible
    // for triggering a rebuild when that happens.
    static LoadResult Load(const std::string& filepath);
};

}  // namespace indexed
