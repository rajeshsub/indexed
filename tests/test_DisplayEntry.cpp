#include <gtest/gtest.h>

#include "ui/DisplayEntry.h"
#include "ui/DisplayFormat.h"

using indexed::BuildDisplayEntries;
using indexed::DisplayEntry;
using indexed::FileEntry;
using indexed::FormatDateTime;
using indexed::FormatFileSize;
using indexed::IndexPool;
using indexed::SearchResult;

TEST(BuildDisplayEntries, JoinsResultsAgainstPoolInOrder) {
    IndexPool pool;
    FileEntry a;
    a.name = "doc.txt";
    a.path = "/home/user/doc.txt";
    a.size = 2048;
    a.lastModified = 1'700'000'000ULL * 1'000'000'000ULL;
    pool.AddEntry(a);

    FileEntry b;
    b.name = "readme.md";
    b.path = "/readme.md";
    b.size = 10;
    b.lastModified = 1'600'000'000ULL * 1'000'000'000ULL;
    pool.AddEntry(b);

    std::vector<SearchResult> results;
    results.push_back({/*entryIndex=*/1, /*matchStart=*/0, /*matchLen=*/6});
    results.push_back({/*entryIndex=*/0, /*matchStart=*/0, /*matchLen=*/3});

    std::vector<DisplayEntry> rows = BuildDisplayEntries(pool, results);

    ASSERT_EQ(rows.size(), 2u);

    EXPECT_EQ(rows[0].name, "readme.md");
    EXPECT_EQ(rows[0].parentDir, "/");
    EXPECT_EQ(rows[0].sizeBytes, 10u);
    EXPECT_EQ(rows[0].sizeText, FormatFileSize(10));
    EXPECT_EQ(rows[0].dateText, FormatDateTime(b.lastModified));
    EXPECT_EQ(rows[0].sourceIndex, 0u);

    EXPECT_EQ(rows[1].name, "doc.txt");
    EXPECT_EQ(rows[1].parentDir, "/home/user");
    EXPECT_EQ(rows[1].sizeBytes, 2048u);
    EXPECT_EQ(rows[1].sourceIndex, 1u);
}

TEST(BuildDisplayEntries, EmptyResultsProduceEmptyRows) {
    IndexPool pool;
    std::vector<SearchResult> results;
    EXPECT_TRUE(BuildDisplayEntries(pool, results).empty());
}
