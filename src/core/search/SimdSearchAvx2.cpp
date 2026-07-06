#include <immintrin.h>

#include "search/SimdSearch.h"
#include <cstring>

namespace indexed {

// First-byte candidate scan + scalar verify (see indexed-plan.md §7.4 /
// design notes for this module). Every SIMD-found candidate is confirmed
// with memcmp, so a false-positive candidate from the byte-scan can never
// produce a wrong answer.
size_t FindSubstringAvx2(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    if (needle.size() > haystack.size()) {
        return std::string_view::npos;
    }

    constexpr size_t kChunkWidth = 32;
    const size_t haystackLen = haystack.size();
    const size_t needleLen = needle.size();
    const __m256i firstByte = _mm256_set1_epi8(needle[0]);

    size_t chunkStart = 0;
    for (; chunkStart + kChunkWidth <= haystackLen; chunkStart += kChunkWidth) {
        __m256i chunk =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(haystack.data() + chunkStart));
        __m256i eq = _mm256_cmpeq_epi8(chunk, firstByte);
        unsigned int mask = static_cast<unsigned int>(_mm256_movemask_epi8(eq));
        while (mask != 0) {
            unsigned int bit = static_cast<unsigned int>(__builtin_ctz(mask));
            size_t candidatePos = chunkStart + bit;
            if (candidatePos + needleLen <= haystackLen &&
                std::memcmp(haystack.data() + candidatePos, needle.data(), needleLen) == 0) {
                return candidatePos;
            }
            mask &= mask - 1;  // clear lowest set bit
        }
    }

    // Scalar tail loop over the remaining bytes that don't fill a full chunk.
    for (size_t pos = chunkStart; pos + needleLen <= haystackLen; ++pos) {
        if (haystack[pos] == needle[0] &&
            std::memcmp(haystack.data() + pos, needle.data(), needleLen) == 0) {
            return pos;
        }
    }

    return std::string_view::npos;
}

}  // namespace indexed
