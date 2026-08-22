#include <gtest/gtest.h>

#include "indexer/IFileSystemScanner.h"
#include "search/ISearchEngine.h"
#include "search/SearchEngine.h"
#include "search/TokenMatcher.h"
#include "storage/IndexPool.h"
#include <algorithm>
#include <atomic>
#include <random>
#include <set>
#include <string>

using indexed::CaseFoldAscii;
using indexed::FileEntry;
using indexed::IndexPool;
using indexed::MatchesAllTokens;
using indexed::SearchEngine;
using indexed::SearchOptions;
using indexed::SearchResult;
using indexed::Tokenize;

namespace {

FileEntry MakeEntry(std::string name, std::string path, uint64_t size = 1,
                    uint64_t lastModified = 1, uint32_t attributes = 0) {
    FileEntry entry;
    entry.name = std::move(name);
    entry.path = std::move(path);
    entry.size = size;
    entry.lastModified = lastModified;
    entry.attributes = attributes;
    return entry;
}

std::atomic<bool> kNeverCancel{false};

std::string RandomToken(std::mt19937& rng) {
    static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFG0123456789";
    std::uniform_int_distribution<size_t> lenDist(1, 8);
    std::uniform_int_distribution<size_t> charDist(0, sizeof(kAlphabet) - 2);
    std::string token(lenDist(rng), ' ');
    std::generate(token.begin(), token.end(), [&]() { return kAlphabet[charDist(rng)]; });
    return token;
}

std::string RandomName(std::mt19937& rng) {
    static constexpr char kSeparators[] = {' ', '_', '-', '.'};
    std::uniform_int_distribution<int> tokenCountDist(1, 5);
    std::uniform_int_distribution<int> sepDist(0, 3);
    int tokenCount = tokenCountDist(rng);
    std::string name;
    for (int i = 0; i < tokenCount; ++i) {
        if (i > 0) {
            name += kSeparators[sepDist(rng)];
        }
        name += RandomToken(rng);
    }
    return name;
}

// Trusted reference: computes the expected token-mode match set using only
// the unchanged, already-tested Tokenize/MatchesAllTokens/CaseFoldAscii --
// never the token-span cache under test.
std::set<size_t> ReferenceTokenMatches(const IndexPool& pool, std::string_view query,
                                       bool caseSensitive) {
    std::string queryText = caseSensitive ? std::string(query) : CaseFoldAscii(query);
    std::vector<std::string_view> queryTokens = Tokenize(queryText);

    std::set<size_t> expected;
    for (size_t i = 0; i < pool.Count(); ++i) {
        auto entry = pool.GetEntry(i);
        std::string_view subject = caseSensitive ? entry.name : entry.nameLower;
        std::vector<std::string_view> nameTokens = Tokenize(subject);
        if (MatchesAllTokens(queryTokens, nameTokens)) {
            expected.insert(i);
        }
    }
    return expected;
}

std::set<size_t> ActualTokenMatches(SearchEngine& engine, const IndexPool& pool,
                                    std::string_view query, bool caseSensitive) {
    SearchOptions options;
    options.caseSensitive = caseSensitive;
    auto results = engine.Search(pool, query, options, kNeverCancel);
    std::set<size_t> actual;
    for (const auto& result : results) {
        actual.insert(result.entryIndex);
    }
    return actual;
}

}  // namespace

TEST(SearchEngine, SubstringModeIsCaseInsensitiveByDefault) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/home/user/docs/Report.pdf"));
    pool.AddEntry(MakeEntry("other.txt", "/home/user/docs/other.txt"));

    SearchEngine engine;
    auto results = engine.Search(pool, "report", SearchOptions{}, kNeverCancel);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].entryIndex, 0u);
}

TEST(SearchEngine, CaseSensitiveOptionRespectsCase) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/home/user/Report.pdf"));

    SearchEngine engine;
    SearchOptions options;
    options.caseSensitive = true;

    EXPECT_EQ(engine.Search(pool, "report", options, kNeverCancel).size(), 0u);
    EXPECT_EQ(engine.Search(pool, "Report", options, kNeverCancel).size(), 1u);
}

TEST(SearchEngine, TokenSetModeMatchesNonAdjacentOutOfOrderTokens) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("LedZep_Just-Rosy_June-Bug_guitar.flac",
                            "/music/LedZep_Just-Rosy_June-Bug_guitar.flac"));
    pool.AddEntry(MakeEntry("unrelated_song.flac", "/music/unrelated_song.flac"));

    SearchEngine engine;
    auto results = engine.Search(pool, "just rosy guitar", SearchOptions{}, kNeverCancel);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].entryIndex, 0u);
}

TEST(SearchEngine, TokenSetModeMatchesPartiallyTypedLastToken) {
    // As-you-type: a result that matched at "just rosy" must not vanish at
    // "just rosy guit" on the way to "just rosy guitar".
    IndexPool pool;
    pool.AddEntry(MakeEntry("LedZep_Just-Rosy_June-Bug_guitar.flac",
                            "/music/LedZep_Just-Rosy_June-Bug_guitar.flac"));
    pool.AddEntry(MakeEntry("unrelated_song.flac", "/music/unrelated_song.flac"));

    SearchEngine engine;
    auto results = engine.Search(pool, "just rosy guit", SearchOptions{}, kNeverCancel);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].entryIndex, 0u);
}

TEST(SearchEngine, RegexModeMatchesPattern) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/x/Report.pdf"));
    pool.AddEntry(MakeEntry("Report.txt", "/x/Report.txt"));

    SearchEngine engine;
    SearchOptions options;
    options.useRegex = true;

    auto results = engine.Search(pool, R"(\.pdf$)", options, kNeverCancel);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].entryIndex, 0u);
}

TEST(SearchEngine, RegexModeCaseSensitiveOptionAppliesWithoutCorruptingPattern) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/x/Report.pdf"));

    SearchEngine engine;
    SearchOptions options;
    options.useRegex = true;
    options.caseSensitive = true;

    EXPECT_EQ(engine.Search(pool, "^report", options, kNeverCancel).size(), 0u);
    EXPECT_EQ(engine.Search(pool, "^Report", options, kNeverCancel).size(), 1u);
}

TEST(SearchEngine, RegexModeWholeWordMatchesOnlyCompleteToken) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("error.log", "/var/error.log"));
    pool.AddEntry(MakeEntry("logging.txt", "/var/logging.txt"));

    SearchEngine engine;
    SearchOptions options;
    options.useRegex = true;
    options.wholeWord = true;

    auto results = engine.Search(pool, "log", options, kNeverCancel);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].entryIndex, 0u);

    // Without wholeWord, "log" substring-matches both under regex mode too.
    options.wholeWord = false;
    EXPECT_EQ(engine.Search(pool, "log", options, kNeverCancel).size(), 2u);
}

TEST(SearchEngine, RegexModeMatchPathSearchesFullPathNotJustName) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("file.txt", "/home/user/secret/file.txt"));

    SearchEngine engine;
    SearchOptions options;
    options.useRegex = true;

    EXPECT_EQ(engine.Search(pool, "secret", options, kNeverCancel).size(), 0u);

    options.matchPath = true;
    EXPECT_EQ(engine.Search(pool, "secret", options, kNeverCancel).size(), 1u);
}

TEST(SearchEngine, RegexModeEmptyPatternMatchesEveryEntryAtPositionZero) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/x/Report.pdf"));
    pool.AddEntry(MakeEntry("other.txt", "/x/other.txt"));

    SearchEngine engine;
    SearchOptions options;
    options.useRegex = true;

    auto results = engine.Search(pool, "", options, kNeverCancel);

    ASSERT_EQ(results.size(), 2u);
    for (const auto& result : results) {
        EXPECT_EQ(result.matchStart, 0u);
        EXPECT_EQ(result.matchLen, 0u);
    }
}

TEST(SearchEngine, WholeWordMatchesOnlyCompleteToken) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("error.log", "/var/error.log"));
    pool.AddEntry(MakeEntry("logging.txt", "/var/logging.txt"));

    SearchEngine engine;
    SearchOptions options;
    options.wholeWord = true;

    auto results = engine.Search(pool, "log", options, kNeverCancel);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].entryIndex, 0u);

    // Without wholeWord, "log" substring-matches both.
    auto unrestricted = engine.Search(pool, "log", SearchOptions{}, kNeverCancel);
    EXPECT_EQ(unrestricted.size(), 2u);
}

TEST(SearchEngine, MatchPathSearchesFullPathNotJustName) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("file.txt", "/home/user/secret/file.txt"));

    SearchEngine engine;

    EXPECT_EQ(engine.Search(pool, "secret", SearchOptions{}, kNeverCancel).size(), 0u);

    SearchOptions matchPath;
    matchPath.matchPath = true;
    EXPECT_EQ(engine.Search(pool, "secret", matchPath, kNeverCancel).size(), 1u);
}

TEST(SearchEngine, IgnoreDiacriticsFoldsAccentedCharacters) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("caf\xc3\xa9.txt", "/x/caf\xc3\xa9.txt"));  // "café.txt"

    SearchEngine engine;

    EXPECT_EQ(engine.Search(pool, "cafe", SearchOptions{}, kNeverCancel).size(), 0u);

    SearchOptions options;
    options.ignoreDiacritics = true;
    EXPECT_EQ(engine.Search(pool, "cafe", options, kNeverCancel).size(), 1u);
}

TEST(SearchEngine, DeletedEntriesAreSkipped) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/x/Report.pdf"));
    pool.MarkDeleted(0);

    SearchEngine engine;
    EXPECT_EQ(engine.Search(pool, "report", SearchOptions{}, kNeverCancel).size(), 0u);
}

TEST(SearchEngine, ResultsAreCappedAtMaxSearchResults) {
    IndexPool pool;
    for (size_t i = 0; i < indexed::kMaxSearchResults + 50; ++i) {
        pool.AddEntry(MakeEntry("match" + std::to_string(i) + ".txt",
                                "/x/match" + std::to_string(i) + ".txt"));
    }

    SearchEngine engine;
    auto results = engine.Search(pool, "match", SearchOptions{}, kNeverCancel);
    EXPECT_EQ(results.size(), indexed::kMaxSearchResults);
}

TEST(SearchEngine, CancelTokenSetBeforeSearchYieldsNoResults) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/x/Report.pdf"));

    std::atomic<bool> cancelled{true};
    SearchEngine engine;
    auto results = engine.Search(pool, "report", SearchOptions{}, cancelled);
    EXPECT_EQ(results.size(), 0u);
}

TEST(SearchEngine, RegexModeHandlesLargeMostlyNonMatchingPoolWithoutFalsePositivesOrNegatives) {
    // Stresses the boolean-filter-first path (most entries don't match, a
    // few interleaved ones do) so a false negative from the fast filter or
    // a false positive/stale match from the capture step would show up.
    IndexPool pool;
    for (int i = 0; i < 500; ++i) {
        pool.AddEntry(
            MakeEntry("skip" + std::to_string(i) + ".log", "/x/skip" + std::to_string(i) + ".log"));
        if (i == 0 || i == 250 || i == 499) {
            pool.AddEntry(MakeEntry("Report" + std::to_string(i) + ".pdf",
                                    "/x/Report" + std::to_string(i) + ".pdf"));
        }
    }

    SearchEngine engine;
    SearchOptions options;
    options.useRegex = true;

    auto results = engine.Search(pool, R"(^Report\d+\.pdf$)", options, kNeverCancel);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].matchStart, 0u);
    EXPECT_EQ(results[0].matchLen, std::string("Report0.pdf").size());
}

TEST(SearchEngine, MatchStartAndMatchLenLocateSubstring) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("myreportfile.pdf", "/x/myreportfile.pdf"));

    SearchEngine engine;
    auto results = engine.Search(pool, "report", SearchOptions{}, kNeverCancel);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].matchStart, 2u);
    EXPECT_EQ(results[0].matchLen, 6u);
}

TEST(SearchEngine, TokenModeMatchesRandomizedCorpusAgainstLiveTokenizeReference) {
    // docs/adr/0012: the cached-name-token-span path must produce identical
    // results to live Tokenize()+MatchesAllTokens() over a large randomized
    // corpus, not just hand-picked cases -- this is the kind of
    // offset-arithmetic bug class hand-picked cases tend to miss.
    IndexPool pool;

    pool.AddEntry(MakeEntry("guitar", "/x/guitar"));  // no separators
    pool.AddEntry(MakeEntry("___", "/x/___"));        // separators-only
    pool.AddEntry(MakeEntry("_lead_", "/x/_lead_"));  // leading/trailing separator
    pool.AddEntry(MakeEntry("a__b", "/x/a__b"));      // consecutive separators
    pool.AddEntry(MakeEntry("x", "/x/x"));            // single character

    std::mt19937 rng(12345);
    for (int i = 0; i < 300; ++i) {
        std::string name = RandomName(rng) + ".dat";
        pool.AddEntry(MakeEntry(name, "/corpus/" + name));
    }

    SearchEngine engine;
    std::uniform_int_distribution<size_t> entryDist(0, pool.Count() - 1);

    for (int q = 0; q < 100; ++q) {
        size_t sourceIndex = entryDist(rng);
        auto sourceTokens = Tokenize(pool.GetEntry(sourceIndex).nameLower);

        // Always build a two-word query (guarantees a separator, so
        // SearchEngine always routes this into token mode) drawn from a
        // real entry's own tokens where possible, so most queries actually
        // match something -- plus random fallback tokens for negative
        // coverage and for entries with too few tokens to draw two from.
        std::uniform_int_distribution<size_t> pick(
            0, sourceTokens.empty() ? 0 : sourceTokens.size() - 1);
        std::string word1 =
            sourceTokens.empty() ? RandomToken(rng) : std::string(sourceTokens[pick(rng)]);
        std::string word2 =
            sourceTokens.empty() ? RandomToken(rng) : std::string(sourceTokens[pick(rng)]);
        std::string query = word1 + " " + word2;

        for (bool caseSensitive : {false, true}) {
            auto expected = ReferenceTokenMatches(pool, query, caseSensitive);
            auto actual = ActualTokenMatches(engine, pool, query, caseSensitive);
            EXPECT_EQ(actual, expected)
                << "query=\"" << query << "\" caseSensitive=" << caseSensitive;
        }
    }
}

TEST(SearchEngine, TokenModeMatchPathAndIgnoreDiacriticsStillUseLiveFallbackTokens) {
    // Regression guard: matchPath/ignoreDiacritics must never use the
    // name-token-span cache (no path-token cache exists -- docs/adr/0012
    // deliberately keeps ADR-0006's pathLower deferral; diacritics-folding
    // can change byte length/positions so cached name-offsets don't
    // transfer).
    IndexPool pool;
    pool.AddEntry(MakeEntry("guitar.flac", "/music/rock_and_roll/guitar.flac"));

    SearchEngine engine;
    SearchOptions matchPathOptions;
    matchPathOptions.matchPath = true;
    auto pathResults = engine.Search(pool, "rock roll", matchPathOptions, kNeverCancel);
    ASSERT_EQ(pathResults.size(), 1u);
    EXPECT_EQ(pathResults[0].entryIndex, 0u);

    IndexPool diacriticsPool;
    diacriticsPool.AddEntry(
        MakeEntry("caf\xc3\xa9_menu.txt", "/x/caf\xc3\xa9_menu.txt"));  // "café_menu.txt"
    SearchOptions diacriticsOptions;
    diacriticsOptions.ignoreDiacritics = true;
    auto diacriticsResults =
        engine.Search(diacriticsPool, "cafe menu", diacriticsOptions, kNeverCancel);
    ASSERT_EQ(diacriticsResults.size(), 1u);
    EXPECT_EQ(diacriticsResults[0].entryIndex, 0u);
}
