#pragma once

#include <cstdint>

namespace indexed {

// Fixed-size record in IndexPool::meta_. Pool offsets are 64-bit (not 32-bit)
// per docs/adr/0006-pool-based-index-layout.md: a 32-bit offset caps a single
// pool at ~4 GiB, which a 32-bit-offset design would risk on a large Linux
// tree; widening costs 8 bytes/entry and removes the ceiling permanently.
// Timestamps are nanoseconds since the Unix epoch (docs/adr/0003).
struct EntryMeta {
    uint64_t size = 0;
    uint64_t lastModified = 0;
    uint64_t pathOffset = 0;       // byte offset into IndexPool's pathPool
    uint64_t nameLowerOffset = 0;  // byte offset into IndexPool's nameLowerPool
    uint32_t attributes = 0;
    uint32_t pathLen = 0;       // byte length of full path
    uint32_t nameLowerLen = 0;  // byte length of lowercased name
    uint16_t nameStart = 0;     // byte offset of basename within the path
    uint8_t deleted = 0;        // soft-delete flag; pools are append-only
    uint8_t _pad = 0;
};

static_assert(sizeof(EntryMeta) == 48,
              "EntryMeta layout changed — bump indexed.idx format version");

}  // namespace indexed
