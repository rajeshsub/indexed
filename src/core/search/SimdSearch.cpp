#include "search/SimdSearch.h"

namespace indexed {

size_t FindSubstringScalar(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle);
}

#if defined(__x86_64__) || defined(__i386__)

namespace {

// Cached once at first use: which SIMD tier this process should dispatch to.
enum class Tier { kAvx2, kSse42, kScalar };

Tier DetectTier() {
    if (__builtin_cpu_supports("avx2")) {
        return Tier::kAvx2;
    }
    if (__builtin_cpu_supports("sse4.2")) {
        return Tier::kSse42;
    }
    return Tier::kScalar;
}

}  // namespace

size_t FindSubstring(std::string_view haystack, std::string_view needle) {
    static const Tier kTier = DetectTier();
    switch (kTier) {
        case Tier::kAvx2:
            return FindSubstringAvx2(haystack, needle);
        case Tier::kSse42:
            return FindSubstringSse42(haystack, needle);
        case Tier::kScalar:
            return FindSubstringScalar(haystack, needle);
    }
    return FindSubstringScalar(haystack, needle);
}

#else

size_t FindSubstring(std::string_view haystack, std::string_view needle) {
    return FindSubstringScalar(haystack, needle);
}

#endif

}  // namespace indexed
