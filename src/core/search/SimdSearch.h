#pragma once

#include <cstddef>
#include <string_view>

namespace indexed {

// Returns the byte offset of the first occurrence of `needle` in `haystack`,
// or std::string_view::npos if not found. Empty needle matches at offset 0
// (mirrors std::string_view::find's convention). Runtime-dispatches to the
// fastest tier available on the current CPU (AVX2 > SSE4.2 > scalar) on
// x86-64; unconditionally scalar on other architectures (aarch64 NEON is
// deferred post-v0.1.0 per project decision).
size_t FindSubstring(std::string_view haystack, std::string_view needle);

// Tier implementations, exposed so tests can directly exercise each one
// regardless of which tier FindSubstring's dispatcher would pick on this
// particular machine (the dev/CI machine has AVX2, so without this, the
// SSE4.2 and scalar tiers would never actually get test-covered).
size_t FindSubstringScalar(std::string_view haystack, std::string_view needle);
#if defined(__x86_64__) || defined(__i386__)
size_t FindSubstringSse42(std::string_view haystack, std::string_view needle);
size_t FindSubstringAvx2(std::string_view haystack, std::string_view needle);
#endif

}  // namespace indexed
