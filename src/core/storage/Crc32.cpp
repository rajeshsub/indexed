#include "storage/Crc32.h"

#include <array>
#include <numeric>

namespace indexed {

namespace {

std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

}  // namespace

uint32_t Crc32(std::string_view data) {
    static const std::array<uint32_t, 256> kTable = MakeCrcTable();

    uint32_t crc = std::accumulate(
        data.begin(), data.end(), 0xFFFFFFFFu,
        [](uint32_t acc, unsigned char byte) { return kTable[(acc ^ byte) & 0xFFu] ^ (acc >> 8); });
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace indexed
