#include <gtest/gtest.h>

#include "indexer/IFileSystemScanner.h"
#include "storage/IndexPool.h"

using indexed::FileEntry;
using indexed::IndexPool;

namespace {

FileEntry MakeEntry(std::string name, std::string path, uint64_t size, uint64_t lastModified,
                    uint32_t attributes = 0) {
    FileEntry entry;
    entry.name = std::move(name);
    entry.path = std::move(path);
    entry.size = size;
    entry.lastModified = lastModified;
    entry.attributes = attributes;
    return entry;
}

}  // namespace

TEST(IndexPool, AddEntryIncrementsCount) {
    IndexPool pool;
    EXPECT_EQ(pool.Count(), 0u);

    pool.AddEntry(MakeEntry("a.txt", "/home/user/a.txt", 10, 100));
    EXPECT_EQ(pool.Count(), 1u);

    pool.AddEntry(MakeEntry("b.txt", "/home/user/b.txt", 20, 200));
    EXPECT_EQ(pool.Count(), 2u);
}

TEST(IndexPool, GetEntryReturnsStoredFields) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.pdf", "/home/user/docs/Report.pdf", 4096, 123456789,
                            indexed::kAttrHidden));

    auto entry = pool.GetEntry(0);
    EXPECT_EQ(entry.name, "Report.pdf");
    EXPECT_EQ(entry.path, "/home/user/docs/Report.pdf");
    EXPECT_EQ(entry.size, 4096u);
    EXPECT_EQ(entry.lastModified, 123456789u);
    EXPECT_EQ(entry.attributes, indexed::kAttrHidden);
}

TEST(IndexPool, NameLowerIsCaseFoldedAscii) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("Report.PDF", "/home/user/Report.PDF", 1, 1));

    EXPECT_EQ(pool.GetEntry(0).nameLower, "report.pdf");
}

TEST(IndexPool, MultipleEntriesPreserveIndependentOffsets) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("short.txt", "/a/short.txt", 1, 1));
    pool.AddEntry(
        MakeEntry("a-much-longer-filename.dat", "/a/b/c/a-much-longer-filename.dat", 2, 2));
    pool.AddEntry(MakeEntry("z.log", "/z.log", 3, 3));

    EXPECT_EQ(pool.GetEntry(0).name, "short.txt");
    EXPECT_EQ(pool.GetEntry(0).path, "/a/short.txt");

    EXPECT_EQ(pool.GetEntry(1).name, "a-much-longer-filename.dat");
    EXPECT_EQ(pool.GetEntry(1).path, "/a/b/c/a-much-longer-filename.dat");

    EXPECT_EQ(pool.GetEntry(2).name, "z.log");
    EXPECT_EQ(pool.GetEntry(2).path, "/z.log");
}

TEST(IndexPool, MarkDeletedSetsIsDeleted) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/a.txt", 1, 1));
    pool.AddEntry(MakeEntry("b.txt", "/b.txt", 1, 1));

    EXPECT_FALSE(pool.IsDeleted(0));
    pool.MarkDeleted(0);
    EXPECT_TRUE(pool.IsDeleted(0));
    EXPECT_FALSE(pool.IsDeleted(1));
}

TEST(IndexPool, FindByPathLocatesEntry) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/home/user/a.txt", 1, 1));
    pool.AddEntry(MakeEntry("b.txt", "/home/user/b.txt", 1, 1));

    auto found = pool.FindByPath("/home/user/b.txt");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, 1u);

    EXPECT_FALSE(pool.FindByPath("/nonexistent").has_value());
}

TEST(IndexPool, LoadFromPathPoolRebuildsNameLowerAndAllFields) {
    IndexPool original;
    original.AddEntry(MakeEntry("Alpha.TXT", "/x/Alpha.TXT", 111, 222, indexed::kAttrDirectory));
    original.AddEntry(MakeEntry("Beta.log", "/x/y/Beta.log", 333, 444));

    // Simulate what IndexSerializer::Load does: reconstruct from meta + pathPool only
    // (nameLower is never persisted on disk — see docs/adr/0003-binary-index-format.md).
    IndexPool reloaded = IndexPool::LoadFromPathPool(original.Meta(), original.PathPool());

    ASSERT_EQ(reloaded.Count(), 2u);

    auto e0 = reloaded.GetEntry(0);
    EXPECT_EQ(e0.name, "Alpha.TXT");
    EXPECT_EQ(e0.nameLower, "alpha.txt");
    EXPECT_EQ(e0.path, "/x/Alpha.TXT");
    EXPECT_EQ(e0.size, 111u);
    EXPECT_EQ(e0.lastModified, 222u);
    EXPECT_EQ(e0.attributes, indexed::kAttrDirectory);

    auto e1 = reloaded.GetEntry(1);
    EXPECT_EQ(e1.name, "Beta.log");
    EXPECT_EQ(e1.nameLower, "beta.log");
    EXPECT_EQ(e1.path, "/x/y/Beta.log");
}
