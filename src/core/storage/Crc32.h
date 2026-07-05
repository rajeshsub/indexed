#pragma once

#include <cstdint>
#include <string_view>

namespace indexed {

// Standard reflected IEEE 802.3 / zlib-compatible CRC-32 (polynomial 0xEDB88320,
// reflected input/output). Used by IndexSerializer to checksum the on-disk
// indexed.idx payload (docs/adr/0003-binary-index-format.md). Check value:
// Crc32("123456789") == 0xCBF43926.
uint32_t Crc32(std::string_view data);

}  // namespace indexed
