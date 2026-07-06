#include <gtest/gtest.h>

#include "indexer/IFileSystemScanner.h"
#include "storage/IndexPool.h"
#include "storage/IndexStore.h"
#include <mutex>
#include <shared_mutex>
#include <utility>

using indexed::FileEntry;
using indexed::IndexStore;

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

}  // namespace

TEST(IndexStore, BeginWriteAddEntryEndWritePopulatesPool) {
    IndexStore store;

    store.BeginWrite();
    store.AddEntry(MakeEntry("a.txt", "/home/user/a.txt"));
    store.AddEntry(MakeEntry("b.txt", "/home/user/b.txt"));
    store.EndWrite();

    EXPECT_EQ(store.GetPool().Count(), 2u);
    EXPECT_TRUE(store.GetPool().FindByPath("/home/user/a.txt").has_value());
    EXPECT_TRUE(store.GetPool().FindByPath("/home/user/b.txt").has_value());
}

TEST(IndexStore, GetPoolUnchangedUntilEndWriteCalled) {
    IndexStore store;

    store.BeginWrite();
    store.AddEntry(MakeEntry("a.txt", "/home/user/a.txt"));
    store.EndWrite();

    ASSERT_EQ(store.GetPool().Count(), 1u);

    // Start a second generation but do not call EndWrite yet — readers must
    // still see the first generation's pool.
    store.BeginWrite();
    store.AddEntry(MakeEntry("c.txt", "/home/user/c.txt"));
    store.AddEntry(MakeEntry("d.txt", "/home/user/d.txt"));

    EXPECT_EQ(store.GetPool().Count(), 1u);
    EXPECT_TRUE(store.GetPool().FindByPath("/home/user/a.txt").has_value());
    EXPECT_FALSE(store.GetPool().FindByPath("/home/user/c.txt").has_value());

    store.EndWrite();

    EXPECT_EQ(store.GetPool().Count(), 2u);
    EXPECT_FALSE(store.GetPool().FindByPath("/home/user/a.txt").has_value());
    EXPECT_TRUE(store.GetPool().FindByPath("/home/user/c.txt").has_value());
    EXPECT_TRUE(store.GetPool().FindByPath("/home/user/d.txt").has_value());
}

TEST(IndexStore, ApplyAddMakesEntryFindable) {
    IndexStore store;
    store.BeginWrite();
    store.EndWrite();

    store.ApplyAdd(MakeEntry("new.txt", "/home/user/new.txt"));

    auto found = store.GetPool().FindByPath("/home/user/new.txt");
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(store.GetPool().IsDeleted(*found));
}

TEST(IndexStore, ApplyRemoveMarksMatchingEntryDeleted) {
    IndexStore store;
    store.BeginWrite();
    store.AddEntry(MakeEntry("a.txt", "/home/user/a.txt"));
    store.EndWrite();

    store.ApplyRemove("/home/user/a.txt");

    auto found = store.GetPool().FindByPath("/home/user/a.txt");
    // FindByPath skips deleted entries, so it must not find it via that path;
    // inspect via a fresh linear pass instead to confirm it's deleted, not gone.
    EXPECT_FALSE(found.has_value());

    bool sawDeletedEntry = false;
    for (size_t i = 0; i < store.GetPool().Count(); ++i) {
        if (store.GetPool().GetEntry(i).path == "/home/user/a.txt") {
            EXPECT_TRUE(store.GetPool().IsDeleted(i));
            sawDeletedEntry = true;
        }
    }
    EXPECT_TRUE(sawDeletedEntry);
}

TEST(IndexStore, ApplyRemoveOfUnknownPathIsNoop) {
    IndexStore store;
    store.BeginWrite();
    store.EndWrite();

    EXPECT_NO_THROW(store.ApplyRemove("/does/not/exist"));
    EXPECT_EQ(store.GetPool().Count(), 0u);
}

TEST(IndexStore, ApplyRenameMarksOldDeletedAndAddsNew) {
    IndexStore store;
    store.BeginWrite();
    store.AddEntry(MakeEntry("old.txt", "/home/user/old.txt"));
    store.EndWrite();

    store.ApplyRename("/home/user/old.txt", "/home/user/new.txt");

    EXPECT_FALSE(store.GetPool().FindByPath("/home/user/old.txt").has_value());

    auto newEntry = store.GetPool().FindByPath("/home/user/new.txt");
    ASSERT_TRUE(newEntry.has_value());
    EXPECT_FALSE(store.GetPool().IsDeleted(*newEntry));
}

TEST(IndexStore, ApplyRenameOfUnknownOldPathStillAddsNewEntry) {
    IndexStore store;
    store.BeginWrite();
    store.EndWrite();

    store.ApplyRename("/home/user/missing.txt", "/home/user/new.txt");

    auto newEntry = store.GetPool().FindByPath("/home/user/new.txt");
    ASSERT_TRUE(newEntry.has_value());
    EXPECT_FALSE(store.GetPool().IsDeleted(*newEntry));
}

TEST(IndexStore, RemoveEntriesUnderPathMatchesOnlyTrueDescendants) {
    IndexStore store;
    store.BeginWrite();
    store.AddEntry(MakeEntry("a.txt", "/home/user/a.txt"));
    store.AddEntry(MakeEntry("b.txt", "/home/user/sub/b.txt"));
    store.AddEntry(MakeEntry("c.txt", "/home/user2/c.txt"));
    store.AddEntry(MakeEntry("userx.txt", "/home/userx.txt"));
    store.AddEntry(MakeEntry("user", "/home/user"));  // exact match to the prefix itself
    store.EndWrite();

    store.RemoveEntriesUnderPath("/home/user");

    auto isDeletedByPath = [&](std::string_view path) {
        for (size_t i = 0; i < store.GetPool().Count(); ++i) {
            if (store.GetPool().GetEntry(i).path == path) {
                return store.GetPool().IsDeleted(i);
            }
        }
        ADD_FAILURE() << "path not found: " << path;
        return false;
    };

    EXPECT_TRUE(isDeletedByPath("/home/user/a.txt"));
    EXPECT_TRUE(isDeletedByPath("/home/user/sub/b.txt"));
    EXPECT_FALSE(isDeletedByPath("/home/user2/c.txt"));
    EXPECT_FALSE(isDeletedByPath("/home/userx.txt"));
    EXPECT_TRUE(isDeletedByPath("/home/user"));
}

TEST(IndexStore, LoadPoolInstallsPoolAndBothTimestamps) {
    IndexStore store;
    store.BeginWrite();
    store.AddEntry(MakeEntry("stale.txt", "/home/user/stale.txt"));
    store.EndWrite();
    ASSERT_EQ(store.GetPool().Count(), 1u);

    indexed::IndexPool loaded;
    loaded.AddEntry(MakeEntry("loaded.txt", "/home/user/loaded.txt"));
    loaded.AddEntry(MakeEntry("loaded2.txt", "/home/user/loaded2.txt"));

    constexpr uint64_t kBuildTs = 42ULL;
    constexpr uint64_t kMonitorStopTs = 99ULL;
    store.LoadPool(std::move(loaded), kBuildTs, kMonitorStopTs);

    EXPECT_EQ(store.GetPool().Count(), 2u);
    EXPECT_FALSE(store.GetPool().FindByPath("/home/user/stale.txt").has_value());
    EXPECT_TRUE(store.GetPool().FindByPath("/home/user/loaded.txt").has_value());
    EXPECT_TRUE(store.GetPool().FindByPath("/home/user/loaded2.txt").has_value());
    EXPECT_EQ(store.GetIndexAgeSeconds(kBuildTs), 0u);
    EXPECT_EQ(store.GetLastMonitorStop(), kMonitorStopTs);
}

TEST(IndexStore, GetIndexAgeSecondsReturnsDeltaFromBuildTimestamp) {
    IndexStore store;
    constexpr uint64_t kBuildNs = 1'000'000'000ULL;
    constexpr uint64_t kNowNs = kBuildNs + 100ULL * 1'000'000'000ULL;  // +100 seconds

    store.SetBuildTimestamp(kBuildNs);
    EXPECT_EQ(store.GetIndexAgeSeconds(kNowNs), 100u);
}

TEST(IndexStore, LastMonitorStopRoundTrips) {
    IndexStore store;
    EXPECT_EQ(store.GetLastMonitorStop(), 0u);

    store.SetLastMonitorStop(123456789ULL);
    EXPECT_EQ(store.GetLastMonitorStop(), 123456789ULL);
}

TEST(IndexStore, SearchMutexSupportsSharedAndExclusiveLocking) {
    IndexStore store;

    {
        std::shared_lock<std::shared_mutex> lock(store.GetSearchMutex());
        EXPECT_TRUE(lock.owns_lock());
    }
    {
        std::unique_lock<std::shared_mutex> lock(store.GetSearchMutex());
        EXPECT_TRUE(lock.owns_lock());
    }
}
