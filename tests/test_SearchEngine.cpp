#include <gtest/gtest.h>

#include "indexer/IFileSystemScanner.h"
#include "search/ISearchEngine.h"
#include "search/SearchEngine.h"
#include "storage/IndexPool.h"
#include <atomic>

using indexed::FileEntry;
using indexed::IndexPool;
using indexed::SearchEngine;
using indexed::SearchOptions;
using indexed::SearchResult;

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

TEST(SearchEngine, MatchStartAndMatchLenLocateSubstring) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("myreportfile.pdf", "/x/myreportfile.pdf"));

    SearchEngine engine;
    auto results = engine.Search(pool, "report", SearchOptions{}, kNeverCancel);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].matchStart, 2u);
    EXPECT_EQ(results[0].matchLen, 6u);
}
