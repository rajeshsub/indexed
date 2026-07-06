#include <gtest/gtest.h>

#include "search/SimdSearch.h"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using indexed::FindSubstring;
using indexed::FindSubstringScalar;
#if defined(__x86_64__) || defined(__i386__)
using indexed::FindSubstringAvx2;
using indexed::FindSubstringSse42;
#endif

namespace {

// A function under test, named for gtest failure messages.
struct Tier {
    std::string name;
    std::function<size_t(std::string_view, std::string_view)> fn;
};

std::vector<Tier> AllTiers() {
    std::vector<Tier> tiers = {
        {"Dispatch", FindSubstring},
        {"Scalar", FindSubstringScalar},
    };
#if defined(__x86_64__) || defined(__i386__)
    tiers.push_back({"Sse42", FindSubstringSse42});
    tiers.push_back({"Avx2", FindSubstringAvx2});
#endif
    return tiers;
}

// Runs `caseFn` against every tier, tagging failures with the tier name.
void ForEachTier(const std::function<void(const Tier&)>& caseFn) {
    for (const Tier& tier : AllTiers()) {
        SCOPED_TRACE("tier=" + tier.name);
        caseFn(tier);
    }
}

}  // namespace

// ---------- Basic cases ----------

TEST(SimdSearch, NeedleAtStart) {
    ForEachTier([](const Tier& tier) { EXPECT_EQ(tier.fn("hello world", "hello"), 0u); });
}

TEST(SimdSearch, NeedleInMiddle) {
    ForEachTier([](const Tier& tier) { EXPECT_EQ(tier.fn("hello world wide web", "world"), 6u); });
}

TEST(SimdSearch, NeedleAtEnd) {
    ForEachTier([](const Tier& tier) { EXPECT_EQ(tier.fn("hello world", "world"), 6u); });
}

TEST(SimdSearch, NeedleEqualsHaystack) {
    ForEachTier([](const Tier& tier) { EXPECT_EQ(tier.fn("exact", "exact"), 0u); });
}

TEST(SimdSearch, NeedleNotPresent) {
    ForEachTier(
        [](const Tier& tier) { EXPECT_EQ(tier.fn("hello world", "xyz"), std::string_view::npos); });
}

TEST(SimdSearch, EmptyNeedleMatchesAtZero) {
    ForEachTier([](const Tier& tier) { EXPECT_EQ(tier.fn("hello world", ""), 0u); });
}

TEST(SimdSearch, EmptyHaystackWithNonEmptyNeedle) {
    ForEachTier([](const Tier& tier) { EXPECT_EQ(tier.fn("", "a"), std::string_view::npos); });
}

TEST(SimdSearch, EmptyHaystackAndNeedle) {
    ForEachTier([](const Tier& tier) { EXPECT_EQ(tier.fn("", ""), 0u); });
}

TEST(SimdSearch, NeedleLongerThanHaystack) {
    ForEachTier([](const Tier& tier) {
        EXPECT_EQ(tier.fn("short", "much longer needle"), std::string_view::npos);
    });
}

// ---------- Boundary-stress cases ----------
// Haystack lengths deliberately straddle the 16-byte SSE and 32-byte AVX2
// chunk boundaries: 15, 16, 17, 31, 32, 33, 63, 64, 65.

namespace {

// Builds a haystack of exactly `length` bytes, filled with filler, with
// `needle` written at `offset`.
std::string MakeHaystack(size_t length, size_t offset, std::string_view needle, char filler) {
    std::string haystack(length, filler);
    for (size_t i = 0; i < needle.size() && offset + i < length; ++i) {
        haystack[offset + i] = needle[i];
    }
    return haystack;
}

}  // namespace

TEST(SimdSearch, BoundaryLengthsNeedleAtStart) {
    const std::vector<size_t> lengths = {15, 16, 17, 31, 32, 33, 63, 64, 65};
    const std::string_view needle = "NEEDLE";
    for (size_t length : lengths) {
        if (length < needle.size()) {
            continue;
        }
        SCOPED_TRACE("length=" + std::to_string(length));
        std::string haystack = MakeHaystack(length, 0, needle, 'x');
        ForEachTier([&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, needle), 0u); });
    }
}

TEST(SimdSearch, BoundaryLengthsNeedleAtEnd) {
    const std::vector<size_t> lengths = {15, 16, 17, 31, 32, 33, 63, 64, 65};
    const std::string_view needle = "NEEDLE";
    for (size_t length : lengths) {
        if (length < needle.size()) {
            continue;
        }
        SCOPED_TRACE("length=" + std::to_string(length));
        size_t offset = length - needle.size();
        std::string haystack = MakeHaystack(length, offset, needle, 'x');
        ForEachTier([&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, needle), offset); });
    }
}

TEST(SimdSearch, BoundaryLengthsNeedleStraddlingChunkBoundary) {
    // Straddle the 16-byte (SSE) boundary by placing the needle so it spans
    // bytes [13, 19) for a haystack long enough to contain that range, and
    // also cover the 32-byte (AVX2) boundary similarly where length allows.
    const std::vector<size_t> lengths = {15, 16, 17, 31, 32, 33, 63, 64, 65};
    const std::string_view needle = "NEEDLE";
    for (size_t length : lengths) {
        if (length < needle.size()) {
            continue;
        }
        // Place the needle straddling offset 13 (crosses the 16-byte SSE
        // boundary at index 16) when the haystack is long enough; otherwise
        // fall back to the middle of the haystack.
        size_t offset = (length >= 13 + needle.size()) ? 13 : (length - needle.size()) / 2;
        SCOPED_TRACE("length=" + std::to_string(length) + " offset=" + std::to_string(offset));
        std::string haystack = MakeHaystack(length, offset, needle, 'x');
        ForEachTier([&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, needle), offset); });
    }
}

// ---------- Adversarial repeated-byte cases ----------

TEST(SimdSearch, RepeatedByteWithNonMatchingNeedleRejectsFalseCandidates) {
    // 200 'a' characters; needle "aab" never matches because there is no 'b'
    // anywhere in the haystack, so every first-byte candidate must be
    // rejected by the scalar verification step.
    std::string haystack(200, 'a');
    ForEachTier(
        [&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, "aab"), std::string_view::npos); });
}

TEST(SimdSearch, RepeatedByteWithMatchAtVeryEnd) {
    // A run of 'a's with a single needle match "aaaa...ab" placed such that
    // it ends exactly at the last byte of the haystack.
    std::string needle(10, 'a');
    needle.push_back('b');
    std::string haystack(200, 'a');
    size_t offset = haystack.size() - needle.size();
    for (size_t i = 0; i < needle.size(); ++i) {
        haystack[offset + i] = needle[i];
    }
    ForEachTier([&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, needle), offset); });
}

// ---------- Non-ASCII bytes ----------

TEST(SimdSearch, NonAsciiBytesMatchAtByteLevel) {
    // Bytes >= 0x80 (UTF-8 continuation-byte range) present in the haystack;
    // this is a byte-level search so multi-byte encoding is irrelevant to
    // correctness.
    std::string haystack = "caf\xC3\xA9 na\xC3\xAFve r\xC3\xA9sum\xC3\xA9";
    std::string_view needle = "\xC3\xA9 na\xC3\xAF";
    size_t expected = haystack.find(needle);
    ASSERT_NE(expected, std::string_view::npos);
    ForEachTier([&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, needle), expected); });
}

TEST(SimdSearch, NonAsciiNeedleNotPresent) {
    std::string haystack = "plain ascii only text here";
    std::string_view needle = "\xC3\xA9\xC3\xAF";
    ForEachTier(
        [&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, needle), std::string_view::npos); });
}

// ---------- Randomized differential test ----------
// Uses std::string_view::find as ground truth. Fixed seed for determinism.

TEST(SimdSearch, RandomizedDifferentialAgainstStdFind) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> lengthDist(0, 300);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<int> copyChance(0, 3);  // 1-in-4 chance of copying a substring

    constexpr int kCases = 1000;
    for (int caseIndex = 0; caseIndex < kCases; ++caseIndex) {
        size_t haystackLen = static_cast<size_t>(lengthDist(rng));
        std::string haystack(haystackLen, '\0');
        std::generate(haystack.begin(), haystack.end(),
                      [&]() { return static_cast<char>(byteDist(rng)); });

        size_t needleLen = static_cast<size_t>(lengthDist(rng) % 40);
        size_t copyLen = std::min(needleLen, haystackLen);
        std::string needle;
        if (copyLen > 0 && copyChance(rng) == 0) {
            // Deliberately copy a real substring from the haystack so matches
            // actually occur sometimes, not just random-miss cases.
            std::uniform_int_distribution<size_t> startDist(0, haystackLen - copyLen);
            size_t start = startDist(rng);
            needle = haystack.substr(start, copyLen);
        } else {
            needle.resize(needleLen);
            std::generate(needle.begin(), needle.end(),
                          [&]() { return static_cast<char>(byteDist(rng)); });
        }

        size_t expected = std::string_view(haystack).find(std::string_view(needle));

        SCOPED_TRACE("case=" + std::to_string(caseIndex) + " haystackLen=" +
                     std::to_string(haystackLen) + " needleLen=" + std::to_string(needle.size()));
        ForEachTier([&](const Tier& tier) { EXPECT_EQ(tier.fn(haystack, needle), expected); });
    }
}
