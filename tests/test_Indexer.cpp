#include <gtest/gtest.h>

#include "indexer/Indexer.h"
#include "mocks/MockChangeMonitor.h"
#include "mocks/MockFileSystemScanner.h"
#include "mocks/MockIndexStore.h"
#include "storage/IndexSerializer.h"
#include "storage/IndexStore.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using ::testing::_;
using ::testing::AtLeast;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

using indexed::FileChangeEvent;
using indexed::FileChangeType;
using indexed::FileEntry;
using indexed::IChangeMonitor;
using indexed::Indexer;
using indexed::IndexerState;
using indexed::IndexerStatus;
using indexed::IndexPool;
using indexed::IndexSerializer;
using indexed::MockChangeMonitor;
using indexed::MockFileSystemScanner;
using indexed::MockIndexStore;
using indexed::ScanOptions;

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

std::string TempFilePath(const std::string& name) {
    return ::testing::TempDir() + "indexed_test_indexer_" + name + ".idx";
}

// Backs a NiceMock<MockIndexStore> with a real IndexPool so tests can assert
// on what actually ends up staged/saved, not just that a method was called.
// BeginWrite/AddEntry/EndWrite/GetPool/GetSearchMutex are delegated to
// `pool`/`mutex`; everything else keeps its default gmock no-op unless a
// test overrides it. GetSearchMutex must be wired -- StartIndexing holds it
// for the duration of IndexSerializer::Save (see Indexer.cpp), since Save
// reads GetPool()'s backing vectors without its own locking.
void WireRealBackingPool(NiceMock<MockIndexStore>& store, IndexPool& pool,
                         std::shared_mutex& mutex) {
    ON_CALL(store, BeginWrite()).WillByDefault(Invoke([&pool]() { pool = IndexPool(); }));
    ON_CALL(store, AddEntry(_)).WillByDefault(Invoke([&pool](const FileEntry& entry) {
        pool.AddEntry(entry);
    }));
    ON_CALL(store, GetPool()).WillByDefault(ReturnRef(pool));
    ON_CALL(store, GetSearchMutex()).WillByDefault(ReturnRef(mutex));
}

}  // namespace

TEST(Indexer, StartIndexingForceScansStreamsAndSaves) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    IndexPool backingPool;
    std::shared_mutex backingMutex;
    WireRealBackingPool(store, backingPool, backingMutex);

    const std::string idxPath = TempFilePath("force_scan");
    std::remove(idxPath.c_str());

    ScanOptions options;
    options.rootPaths = {"/home/user"};

    EXPECT_CALL(scanner, Scan(_, _, _, _))
        .WillOnce(Invoke([](const ScanOptions&, indexed::ScanCallback onEntry,
                            indexed::ProgressCallback onProgress, const std::atomic<bool>&) {
            onEntry(MakeEntry("a.txt", "/home/user/a.txt"));
            onEntry(MakeEntry("b.txt", "/home/user/b.txt"));
            onProgress(2, "/home/user");
        }));

    EXPECT_CALL(store, BeginWrite()).Times(1);
    EXPECT_CALL(store, AddEntry(_)).Times(2);
    EXPECT_CALL(store, EndWrite()).Times(1);
    EXPECT_CALL(store, SetBuildTimestamp(1'000'000ULL)).Times(1);
    EXPECT_CALL(store, GetLastMonitorStop()).WillRepeatedly(Return(0ULL));

    std::vector<IndexerStatus> statuses;
    std::mutex statusMutex;
    Indexer indexer(scanner, store, nullptr, [&](const IndexerStatus& status) {
        std::lock_guard<std::mutex> lock(statusMutex);
        statuses.push_back(status);
    });

    indexer.StartIndexing(/*force=*/true, options, idxPath, /*nowNs=*/1'000'000ULL,
                          /*staleThresholdSeconds=*/3600);

    ASSERT_FALSE(statuses.empty());
    EXPECT_EQ(statuses.front().state, IndexerState::Scanning);
    EXPECT_EQ(statuses.back().state, IndexerState::Idle);
    EXPECT_EQ(statuses.back().filesIndexed, 2u);

    // The saved file should round-trip the two scanned entries.
    IndexSerializer::LoadResult result = IndexSerializer::Load(idxPath);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.pool.Count(), 2u);
    EXPECT_EQ(result.buildTimestampNs, 1'000'000ULL);

    std::remove(idxPath.c_str());
}

TEST(Indexer, StartIndexingLoadsNonStaleOnDiskIndexInsteadOfScanning) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    const std::string idxPath = TempFilePath("load_fresh");
    IndexPool onDiskPool;
    onDiskPool.AddEntry(MakeEntry("existing.txt", "/home/user/existing.txt"));
    constexpr uint64_t kBuildTs = 1'000'000'000ULL;  // 1 second, in ns
    constexpr uint64_t kMonitorStopTs = 500'000'000ULL;
    ASSERT_TRUE(IndexSerializer::Save(idxPath, onDiskPool, kBuildTs, kMonitorStopTs));

    // now = build + 10s; threshold = 1 hour -> not stale.
    const uint64_t nowNs = kBuildTs + 10ULL * 1'000'000'000ULL;

    EXPECT_CALL(scanner, Scan(_, _, _, _)).Times(0);
    EXPECT_CALL(store, LoadPool(_, kBuildTs, kMonitorStopTs)).Times(1);

    std::vector<IndexerStatus> statuses;
    Indexer indexer(scanner, store, nullptr,
                    [&](const IndexerStatus& status) { statuses.push_back(status); });

    ScanOptions options;
    options.rootPaths = {"/home/user"};
    indexer.StartIndexing(/*force=*/false, options, idxPath, nowNs,
                          /*staleThresholdSeconds=*/3600);

    ASSERT_FALSE(statuses.empty());
    EXPECT_EQ(statuses.front().state, IndexerState::LoadingIndex);
    EXPECT_EQ(statuses.back().state, IndexerState::Idle);
    EXPECT_EQ(statuses.back().indexAgeSeconds, 10u);

    std::remove(idxPath.c_str());
}

TEST(Indexer, StartIndexingRescansWhenOnDiskIndexIsStale) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    IndexPool backingPool;
    std::shared_mutex backingMutex;
    WireRealBackingPool(store, backingPool, backingMutex);

    const std::string idxPath = TempFilePath("stale");
    IndexPool onDiskPool;
    onDiskPool.AddEntry(MakeEntry("old.txt", "/home/user/old.txt"));
    constexpr uint64_t kBuildTs = 1'000'000'000ULL;
    ASSERT_TRUE(IndexSerializer::Save(idxPath, onDiskPool, kBuildTs, 0));

    // now = build + 2 hours; threshold = 1 hour -> stale, must rescan.
    const uint64_t nowNs = kBuildTs + 2ULL * 3600ULL * 1'000'000'000ULL;

    EXPECT_CALL(scanner, Scan(_, _, _, _))
        .WillOnce(Invoke([](const ScanOptions&, indexed::ScanCallback onEntry,
                            indexed::ProgressCallback, const std::atomic<bool>&) {
            onEntry(MakeEntry("fresh.txt", "/home/user/fresh.txt"));
        }));
    EXPECT_CALL(store, LoadPool(_, _, _)).Times(0);
    EXPECT_CALL(store, GetLastMonitorStop()).WillRepeatedly(Return(0ULL));

    ScanOptions options;
    options.rootPaths = {"/home/user"};
    Indexer indexer(scanner, store, nullptr, nullptr);
    indexer.StartIndexing(/*force=*/false, options, idxPath, nowNs,
                          /*staleThresholdSeconds=*/3600);

    EXPECT_EQ(backingPool.Count(), 1u);
    std::remove(idxPath.c_str());
}

TEST(Indexer, StartIndexingScansWhenNoIndexFileExists) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    IndexPool backingPool;
    std::shared_mutex backingMutex;
    WireRealBackingPool(store, backingPool, backingMutex);

    const std::string idxPath = TempFilePath("missing");
    std::remove(idxPath.c_str());

    EXPECT_CALL(scanner, Scan(_, _, _, _))
        .WillOnce(
            Invoke([](const ScanOptions&, indexed::ScanCallback onEntry, indexed::ProgressCallback,
                      const std::atomic<bool>&) { onEntry(MakeEntry("a.txt", "/a.txt")); }));
    EXPECT_CALL(store, GetLastMonitorStop()).WillRepeatedly(Return(0ULL));

    ScanOptions options;
    options.rootPaths = {"/"};
    Indexer indexer(scanner, store, nullptr, nullptr);
    indexer.StartIndexing(/*force=*/false, options, idxPath, /*nowNs=*/1, 3600);

    EXPECT_EQ(backingPool.Count(), 1u);
    std::remove(idxPath.c_str());
}

TEST(Indexer, IndexPathsScansGivenPathsAndAppliesAdd) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    EXPECT_CALL(scanner, Scan(Field(&ScanOptions::rootPaths, ElementsAre("/mnt/usb")), _, _, _))
        .WillOnce(Invoke([](const ScanOptions&, indexed::ScanCallback onEntry,
                            indexed::ProgressCallback, const std::atomic<bool>&) {
            onEntry(MakeEntry("photo.jpg", "/mnt/usb/photo.jpg"));
            onEntry(MakeEntry("video.mp4", "/mnt/usb/video.mp4"));
        }));
    EXPECT_CALL(store, ApplyAdd(Field(&FileEntry::path, std::string("/mnt/usb/photo.jpg"))))
        .Times(1);
    EXPECT_CALL(store, ApplyAdd(Field(&FileEntry::path, std::string("/mnt/usb/video.mp4"))))
        .Times(1);

    Indexer indexer(scanner, store, nullptr, nullptr);
    indexer.IndexPaths({"/mnt/usb"});
}

TEST(Indexer, IndexPathsForwardsExcludedPathsToScanner) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    EXPECT_CALL(
        scanner,
        Scan(::testing::AllOf(Field(&ScanOptions::rootPaths, ElementsAre("/mnt/usb")),
                              Field(&ScanOptions::excludedPaths, ElementsAre("/mnt/usb/.cache"))),
             _, _, _))
        .Times(1);

    Indexer indexer(scanner, store, nullptr, nullptr);
    indexer.IndexPaths({"/mnt/usb"}, {"/mnt/usb/.cache"});
}

TEST(Indexer, PersistIndexSavesCurrentPoolAndStampsBuildTimestamp) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    IndexPool backingPool;
    backingPool.AddEntry(MakeEntry("kept.txt", "/mnt/usb/kept.txt"));
    std::shared_mutex backingMutex;
    ON_CALL(store, GetPool()).WillByDefault(ReturnRef(backingPool));
    ON_CALL(store, GetSearchMutex()).WillByDefault(ReturnRef(backingMutex));
    EXPECT_CALL(store, GetLastMonitorStop()).WillRepeatedly(Return(7ULL));
    EXPECT_CALL(store, SetBuildTimestamp(42'000'000'000ULL)).Times(1);

    const std::string idxPath = TempFilePath("persist_incremental");
    std::remove(idxPath.c_str());

    Indexer indexer(scanner, store, nullptr, nullptr);
    indexer.PersistIndex(idxPath, /*nowNs=*/42'000'000'000ULL);

    IndexSerializer::LoadResult result = IndexSerializer::Load(idxPath);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.pool.Count(), 1u);
    EXPECT_EQ(result.buildTimestampNs, 42'000'000'000ULL);
    EXPECT_EQ(result.lastMonitorStopNs, 7ULL);

    std::remove(idxPath.c_str());
}

TEST(Indexer, RemovePathsCallsRemoveEntriesUnderPathForEachPath) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    EXPECT_CALL(store, RemoveEntriesUnderPath(std::string_view("/mnt/usb"))).Times(1);
    EXPECT_CALL(store, RemoveEntriesUnderPath(std::string_view("/mnt/old"))).Times(1);

    Indexer indexer(scanner, store, nullptr, nullptr);
    indexer.RemovePaths({"/mnt/usb", "/mnt/old"});
}

TEST(Indexer, ApplyChangeEventAddedRescansPathAndAppliesEntry) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    EXPECT_CALL(scanner,
                Scan(Field(&ScanOptions::rootPaths, ElementsAre("/home/user/new.txt")), _, _, _))
        .WillOnce(Invoke([](const ScanOptions&, indexed::ScanCallback onEntry,
                            indexed::ProgressCallback, const std::atomic<bool>&) {
            onEntry(MakeEntry("new.txt", "/home/user/new.txt", 42, 99));
        }));
    EXPECT_CALL(store, ApplyAdd(Field(&FileEntry::path, std::string("/home/user/new.txt"))))
        .Times(1);

    Indexer indexer(scanner, store, nullptr, nullptr);
    FileChangeEvent event;
    event.type = FileChangeType::Added;
    event.path = "/home/user/new.txt";
    indexer.ApplyChangeEvent(event);
}

TEST(Indexer, ApplyChangeEventRemovedCallsApplyRemove) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    EXPECT_CALL(scanner, Scan(_, _, _, _)).Times(0);
    EXPECT_CALL(store, ApplyRemove(std::string_view("/home/user/gone.txt"))).Times(1);

    Indexer indexer(scanner, store, nullptr, nullptr);
    FileChangeEvent event;
    event.type = FileChangeType::Removed;
    event.path = "/home/user/gone.txt";
    indexer.ApplyChangeEvent(event);
}

TEST(Indexer, ApplyChangeEventRenamedCallsApplyRename) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    EXPECT_CALL(scanner, Scan(_, _, _, _)).Times(0);
    EXPECT_CALL(store, ApplyRename(std::string_view("/home/user/old.txt"),
                                   std::string_view("/home/user/new.txt")))
        .Times(1);

    Indexer indexer(scanner, store, nullptr, nullptr);
    FileChangeEvent event;
    event.type = FileChangeType::Renamed;
    event.oldPath = "/home/user/old.txt";
    event.path = "/home/user/new.txt";
    indexer.ApplyChangeEvent(event);
}

TEST(Indexer, ApplyChangeEventModifiedRemovesStaleThenRescansAndAdds) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    ::testing::Sequence seq;

    EXPECT_CALL(store, ApplyRemove(std::string_view("/home/user/changed.txt"))).InSequence(seq);
    EXPECT_CALL(scanner, Scan(Field(&ScanOptions::rootPaths, ElementsAre("/home/user/changed.txt")),
                              _, _, _))
        .InSequence(seq)
        .WillOnce(Invoke([](const ScanOptions&, indexed::ScanCallback onEntry,
                            indexed::ProgressCallback, const std::atomic<bool>&) {
            onEntry(MakeEntry("changed.txt", "/home/user/changed.txt", 7, 8));
        }));
    EXPECT_CALL(store, ApplyAdd(Field(&FileEntry::path, std::string("/home/user/changed.txt"))))
        .InSequence(seq);

    Indexer indexer(scanner, store, nullptr, nullptr);
    FileChangeEvent event;
    event.type = FileChangeType::Modified;
    event.path = "/home/user/changed.txt";
    indexer.ApplyChangeEvent(event);
}

TEST(Indexer, ApplyChangeEventFiresMutationCallbackOncePerAppliedChange) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    ON_CALL(scanner, Scan(_, _, _, _))
        .WillByDefault(Invoke([](const ScanOptions&, indexed::ScanCallback onEntry,
                                 indexed::ProgressCallback, const std::atomic<bool>&) {
            onEntry(MakeEntry("f.txt", "/home/user/f.txt", 1, 2));
        }));

    int mutations = 0;
    Indexer indexer(scanner, store, nullptr, nullptr, [&mutations]() { ++mutations; });

    FileChangeEvent added;
    added.type = FileChangeType::Added;
    added.path = "/home/user/f.txt";
    indexer.ApplyChangeEvent(added);
    EXPECT_EQ(mutations, 1);

    FileChangeEvent removed;
    removed.type = FileChangeType::Removed;
    removed.path = "/home/user/f.txt";
    indexer.ApplyChangeEvent(removed);
    EXPECT_EQ(mutations, 2);

    FileChangeEvent renamed;
    renamed.type = FileChangeType::Renamed;
    renamed.oldPath = "/home/user/f.txt";
    renamed.path = "/home/user/g.txt";
    indexer.ApplyChangeEvent(renamed);
    EXPECT_EQ(mutations, 3);
}

TEST(Indexer, ApplyChangeEventAddedDoesNotFireMutationCallbackWhenRescanFindsNothing) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    // Scanner reports nothing for the path (file vanished before re-scan).
    EXPECT_CALL(scanner, Scan(_, _, _, _))
        .WillOnce(Invoke([](const ScanOptions&, indexed::ScanCallback, indexed::ProgressCallback,
                            const std::atomic<bool>&) {}));
    EXPECT_CALL(store, ApplyAdd(_)).Times(0);

    int mutations = 0;
    Indexer indexer(scanner, store, nullptr, nullptr, [&mutations]() { ++mutations; });

    FileChangeEvent added;
    added.type = FileChangeType::Added;
    added.path = "/home/user/ghost.txt";
    indexer.ApplyChangeEvent(added);

    EXPECT_EQ(mutations, 0);
}

TEST(Indexer, StartLiveMonitoringStartsOneMonitorPerRootWithCorrectRoot) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    std::mutex requestedRootsMutex;
    std::vector<std::string> requestedRoots;

    Indexer indexer(
        scanner, store,
        [&](const std::string& root) -> std::unique_ptr<IChangeMonitor> {
            {
                std::lock_guard<std::mutex> lock(requestedRootsMutex);
                requestedRoots.push_back(root);
            }
            auto monitor = std::make_unique<NiceMock<MockChangeMonitor>>();
            EXPECT_CALL(*monitor, StartMonitoring(root, _, _)).Times(1);
            return monitor;
        },
        nullptr);

    std::atomic<bool> stopToken{true};
    indexer.StartLiveMonitoring({"/mnt/a", "/mnt/b"}, stopToken);

    std::lock_guard<std::mutex> lock(requestedRootsMutex);
    ASSERT_EQ(requestedRoots.size(), 2u);
    EXPECT_NE(std::find(requestedRoots.begin(), requestedRoots.end(), "/mnt/a"),
              requestedRoots.end());
    EXPECT_NE(std::find(requestedRoots.begin(), requestedRoots.end(), "/mnt/b"),
              requestedRoots.end());
}

TEST(Indexer, StartLiveMonitoringAppliesEventsDeliveredByTheMonitor) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;

    EXPECT_CALL(store, ApplyRemove(std::string_view("/mnt/a/deleted.txt"))).Times(1);

    Indexer indexer(
        scanner, store,
        [](const std::string&) -> std::unique_ptr<IChangeMonitor> {
            auto monitor = std::make_unique<NiceMock<MockChangeMonitor>>();
            ON_CALL(*monitor, StartMonitoring(_, _, _))
                .WillByDefault(Invoke([](const std::string&, indexed::ChangeCallback onChange,
                                         const std::atomic<bool>&) {
                    FileChangeEvent event;
                    event.type = FileChangeType::Removed;
                    event.path = "/mnt/a/deleted.txt";
                    onChange(event);
                }));
            return monitor;
        },
        nullptr);

    std::atomic<bool> stopToken{true};
    indexer.StartLiveMonitoring({"/mnt/a"}, stopToken);
}

TEST(Indexer, StartLiveMonitoringAppliesABurstOfEventsInOrderWithoutDroppingAny) {
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> store;
    ON_CALL(scanner, Scan(_, _, _, _))
        .WillByDefault(Invoke([](const ScanOptions& options, indexed::ScanCallback onEntry,
                                 indexed::ProgressCallback, const std::atomic<bool>&) {
            const std::string& path = options.rootPaths.front();
            onEntry(MakeEntry(path, path));
        }));

    std::mutex callMutex;
    std::vector<std::string> appliedOps;
    ON_CALL(store, ApplyAdd(_)).WillByDefault(Invoke([&](const FileEntry& entry) {
        std::lock_guard<std::mutex> lock(callMutex);
        appliedOps.push_back("add:" + entry.path);
    }));
    ON_CALL(store, ApplyRemove(_)).WillByDefault(Invoke([&](std::string_view path) {
        std::lock_guard<std::mutex> lock(callMutex);
        appliedOps.push_back("remove:" + std::string(path));
    }));
    ON_CALL(store, ApplyRename(_, _))
        .WillByDefault(Invoke([&](std::string_view oldPath, std::string_view newPath) {
            std::lock_guard<std::mutex> lock(callMutex);
            appliedOps.push_back("rename:" + std::string(oldPath) + "->" + std::string(newPath));
        }));

    int mutations = 0;
    std::mutex mutationMutex;

    // A burst: several distinct events delivered back-to-back by one monitor
    // callback invocation, as a real fanotify/inotify backend could deliver
    // after coalescing a flurry of filesystem activity (e.g. an rsync or a
    // git checkout touching many files at once). Every event must still be
    // applied, in delivery order, none dropped.
    Indexer indexer(
        scanner, store,
        [](const std::string&) -> std::unique_ptr<IChangeMonitor> {
            auto monitor = std::make_unique<NiceMock<MockChangeMonitor>>();
            ON_CALL(*monitor, StartMonitoring(_, _, _))
                .WillByDefault(Invoke([](const std::string&, indexed::ChangeCallback onChange,
                                         const std::atomic<bool>&) {
                    FileChangeEvent added1;
                    added1.type = FileChangeType::Added;
                    added1.path = "/mnt/a/one.txt";
                    onChange(added1);

                    FileChangeEvent added2;
                    added2.type = FileChangeType::Added;
                    added2.path = "/mnt/a/two.txt";
                    onChange(added2);

                    FileChangeEvent renamed;
                    renamed.type = FileChangeType::Renamed;
                    renamed.oldPath = "/mnt/a/one.txt";
                    renamed.path = "/mnt/a/one-renamed.txt";
                    onChange(renamed);

                    FileChangeEvent removed;
                    removed.type = FileChangeType::Removed;
                    removed.path = "/mnt/a/two.txt";
                    onChange(removed);
                }));
            return monitor;
        },
        /*statusCallback=*/nullptr,
        [&mutations, &mutationMutex]() {
            std::lock_guard<std::mutex> lock(mutationMutex);
            ++mutations;
        });

    std::atomic<bool> stopToken{true};
    indexer.StartLiveMonitoring({"/mnt/a"}, stopToken);

    std::lock_guard<std::mutex> lock(callMutex);
    ASSERT_EQ(appliedOps.size(), 4u);
    EXPECT_EQ(appliedOps[0], "add:/mnt/a/one.txt");
    EXPECT_EQ(appliedOps[1], "add:/mnt/a/two.txt");
    EXPECT_EQ(appliedOps[2], "rename:/mnt/a/one.txt->/mnt/a/one-renamed.txt");
    EXPECT_EQ(appliedOps[3], "remove:/mnt/a/two.txt");
    EXPECT_EQ(mutations, 4);
}

TEST(Indexer, ApplyChangeEventDuringConcurrentForceRescanBothCompleteWithoutCorruption) {
    // Index reload (a force rescan, e.g. triggered by SIGUSR1 or a
    // stale-index rebuild) can run concurrently with live-monitoring
    // mutations arriving on another thread, since StartIndexing and
    // ApplyChangeEvent are invoked from different threads in the real
    // FanotifyMonitor/InotifyWatcher-backed app. Both must complete without
    // crashing or corrupting the backing store; the real IndexStore's
    // shared_mutex is what's actually under test here (Indexer itself just
    // forwards calls), so this wires a real store instead of a mock.
    NiceMock<MockFileSystemScanner> scanner;
    NiceMock<MockIndexStore> mockStore;
    indexed::IndexStore realStore;

    ON_CALL(mockStore, ApplyAdd(_)).WillByDefault(Invoke([&](const FileEntry& entry) {
        realStore.ApplyAdd(entry);
    }));
    ON_CALL(mockStore, ApplyRemove(_)).WillByDefault(Invoke([&](std::string_view path) {
        realStore.ApplyRemove(path);
    }));
    ON_CALL(mockStore, BeginWrite()).WillByDefault(Invoke([&]() { realStore.BeginWrite(); }));
    ON_CALL(mockStore, AddEntry(_)).WillByDefault(Invoke([&](const FileEntry& entry) {
        realStore.AddEntry(entry);
    }));
    ON_CALL(mockStore, EndWrite()).WillByDefault(Invoke([&]() { realStore.EndWrite(); }));
    ON_CALL(mockStore, GetPool()).WillByDefault(Invoke([&]() -> const IndexPool& {
        return realStore.GetPool();
    }));
    ON_CALL(mockStore, GetSearchMutex()).WillByDefault(Invoke([&]() -> std::shared_mutex& {
        return realStore.GetSearchMutex();
    }));
    ON_CALL(mockStore, SetBuildTimestamp(_)).WillByDefault(Invoke([&](uint64_t ts) {
        realStore.SetBuildTimestamp(ts);
    }));
    ON_CALL(mockStore, GetLastMonitorStop()).WillByDefault(Invoke([&]() {
        return realStore.GetLastMonitorStop();
    }));

    ON_CALL(scanner, Scan(_, _, _, _))
        .WillByDefault(Invoke([](const ScanOptions& options, indexed::ScanCallback onEntry,
                                 indexed::ProgressCallback onProgress, const std::atomic<bool>&) {
            for (const std::string& root : options.rootPaths) {
                onEntry(MakeEntry(root, root));
            }
            onProgress(options.rootPaths.size(), "scan");
        }));

    Indexer indexer(scanner, mockStore, nullptr, nullptr);

    const std::string idxPath = TempFilePath("concurrent_reload");
    std::remove(idxPath.c_str());

    ScanOptions options;
    options.rootPaths = {"/home/user/rescan_root"};

    std::atomic<bool> stop{false};
    std::thread rescanThread([&]() {
        for (int i = 0; i < 20 && !stop.load(); ++i) {
            indexer.StartIndexing(/*force=*/true, options, idxPath, /*nowNs=*/1'000'000ULL + i,
                                  /*staleThresholdSeconds=*/3600);
        }
    });

    std::thread mutateThread([&]() {
        for (int i = 0; i < 200; ++i) {
            FileChangeEvent added;
            added.type = FileChangeType::Added;
            added.path = "/home/user/live" + std::to_string(i) + ".txt";
            indexer.ApplyChangeEvent(added);
        }
    });

    mutateThread.join();
    stop.store(true);
    rescanThread.join();

    std::remove(idxPath.c_str());
    // No crash/UB (caught by ASan/tsan in the pre-push build+ctest gate) is
    // the primary assertion; this final check just confirms the store is
    // still in a usable state afterward.
    EXPECT_NO_THROW({ [[maybe_unused]] size_t count = realStore.GetPool().Count(); });
}
