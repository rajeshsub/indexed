#include <gtest/gtest.h>

#include "indexer/IndexService.h"
#include "storage/IndexSerializer.h"
#include "storage/IndexStore.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using indexed::ChangeCallback;
using indexed::FileChangeEvent;
using indexed::FileChangeType;
using indexed::FileEntry;
using indexed::FileOperation;
using indexed::FileOperationReport;
using indexed::IChangeMonitor;
using indexed::IndexChange;
using indexed::IndexerState;
using indexed::IndexerStatus;
using indexed::IndexPool;
using indexed::IndexSerializer;
using indexed::IndexService;
using indexed::IndexServiceCallbacks;
using indexed::IndexStore;
using indexed::ScanOptions;

namespace {

using Clock = std::chrono::steady_clock;
bool WaitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = Clock::now() + timeout;
    while (!condition()) {
        if (Clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::string TempIndexPath(const std::string& name) {
    return ::testing::TempDir() + "indexed_test_indexservice_" + name + ".idx";
}

FileEntry MakeEntry(const std::string& path) {
    FileEntry entry;
    entry.path = path;
    entry.name = path.substr(path.find_last_of('/') + 1);
    entry.size = 1;
    return entry;
}

// Emits "<root>/a.txt" and "<root>/b.txt" per requested root. When held, a
// scan blocks after its first entry until Release() or its cancel token.
class HeldScanner : public indexed::IFileSystemScanner {
public:
    bool FastScanAvailable(const std::string&) const override { return false; }

    void Scan(const ScanOptions& options, indexed::ScanCallback onEntry,
              indexed::ProgressCallback onProgress, const std::atomic<bool>& cancelToken) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scannedRoots_.push_back(options.rootPaths);
            ++running_;
        }
        bool first = true;
        for (const std::string& root : options.rootPaths) {
            for (const char* name : {"a.txt", "b.txt"}) {
                onEntry(MakeEntry(root + "/" + name));
                for (int i = 0; i < progressPerEntry_; ++i) {
                    onProgress(static_cast<uint64_t>(i), root);
                }
                if (first) {
                    first = false;
                    while (hold_.load() && !cancelToken.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (cancelToken.load()) {
                        sawCancel_.store(true);
                        std::lock_guard<std::mutex> lock(mutex_);
                        --running_;
                        return;
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        --running_;
    }

    void Hold() { hold_.store(true); }
    void Release() { hold_.store(false); }
    void SetProgressPerEntry(int count) { progressPerEntry_ = count; }
    bool SawCancel() const { return sawCancel_.load(); }
    int Running() {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }
    std::vector<std::vector<std::string>> ScannedRoots() {
        std::lock_guard<std::mutex> lock(mutex_);
        return scannedRoots_;
    }

private:
    std::mutex mutex_;
    std::vector<std::vector<std::string>> scannedRoots_;
    int running_ = 0;
    int progressPerEntry_ = 0;
    std::atomic<bool> hold_{false};
    std::atomic<bool> sawCancel_{false};
};

// IndexStore that can hold LoadPool (a reload) and counts calls.
class CountingStore : public IndexStore {
public:
    void LoadPool(IndexPool pool, uint64_t buildTimestampNs, uint64_t lastMonitorStopNs) override {
        ++loads_;
        while (holdLoads_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        IndexStore::LoadPool(std::move(pool), buildTimestampNs, lastMonitorStopNs);
    }
    std::atomic<int> loads_{0};
    std::atomic<bool> holdLoads_{false};
};

// IndexStore whose RemoveEntriesUnderPath can be held, to keep a Settings
// "remove a root" command running while later ones are queued.
class HoldingRemoveStore : public IndexStore {
public:
    void RemoveEntriesUnderPath(std::string_view pathPrefix) override {
        ++removals_;
        while (holdRemovals_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        IndexStore::RemoveEntriesUnderPath(pathPrefix);
    }
    std::atomic<int> removals_{0};
    std::atomic<bool> holdRemovals_{false};
};

std::vector<std::string> LivePaths(IndexStore& store) {
    std::shared_lock lock(store.GetSearchMutex());
    std::vector<std::string> paths;
    const IndexPool& pool = store.GetPool();
    for (size_t i = 0; i < pool.Count(); ++i) {
        if (!pool.IsDeleted(i)) {
            paths.emplace_back(pool.GetEntry(i).path);
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

// Records every callback, thread-safely.
struct Recorder {
    std::mutex mutex;
    std::vector<IndexChange> changes;
    std::vector<bool> busy;
    std::vector<IndexerStatus> statuses;
    std::vector<FileOperationReport> reports;
    std::vector<std::thread::id> callbackThreads;

    IndexServiceCallbacks Callbacks() {
        IndexServiceCallbacks callbacks;
        callbacks.onStatus = [this](const IndexerStatus& status) {
            std::lock_guard<std::mutex> lock(mutex);
            statuses.push_back(status);
        };
        callbacks.onBusyChanged = [this](bool value) {
            std::lock_guard<std::mutex> lock(mutex);
            busy.push_back(value);
        };
        callbacks.onIndexChanged = [this](const IndexChange& change) {
            std::lock_guard<std::mutex> lock(mutex);
            changes.push_back(change);
            callbackThreads.push_back(std::this_thread::get_id());
        };
        callbacks.onFileOperationsFinished = [this](const FileOperationReport& report) {
            std::lock_guard<std::mutex> lock(mutex);
            reports.push_back(report);
        };
        return callbacks;
    }
    size_t ChangeCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return changes.size();
    }
    size_t ReportCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return reports.size();
    }
};

uint64_t FakeClock() {
    return 1'000'000'000'000ULL;
}

ScanOptions Roots(std::vector<std::string> roots) {
    ScanOptions options;
    options.rootPaths = std::move(roots);
    return options;
}

std::optional<std::string> AlwaysSucceeds(const FileOperation&) {
    return std::nullopt;
}

}  // namespace

// 1
TEST(IndexService, RequestReturnsWhileTheScanIsStillRunning) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    scanner.Hold();
    IndexService service(scanner, store, nullptr, TempIndexPath("returns"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);

    service.RequestIndexing(/*force=*/true, Roots({"/r"}), 3600, /*monitorAfter=*/false);

    ASSERT_TRUE(WaitUntil([&] { return scanner.Running() == 1; }));
    EXPECT_EQ(recorder.ChangeCount(), 0u);  // we got here while the scan is held
    scanner.Release();
    EXPECT_TRUE(WaitUntil([&] { return recorder.ChangeCount() == 1; }));
    std::remove(TempIndexPath("returns").c_str());
}

// 2
TEST(IndexService, ReportsCompletionFromItsOwnThread) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    IndexService service(scanner, store, nullptr, TempIndexPath("completion"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);

    service.RequestIndexing(/*force=*/true, Roots({"/r"}), 3600, false);
    service.Flush();

    std::lock_guard<std::mutex> lock(recorder.mutex);
    ASSERT_EQ(recorder.changes.size(), 1u);
    EXPECT_EQ(recorder.changes[0].kind, IndexChange::Kind::Built);
    EXPECT_EQ(recorder.changes[0].entryCount, 2u);
    EXPECT_NE(recorder.callbackThreads[0], std::this_thread::get_id());
    ASSERT_FALSE(recorder.statuses.empty());
    EXPECT_EQ(recorder.statuses.back().state, IndexerState::Idle);
    EXPECT_EQ(recorder.busy, (std::vector<bool>{true, false}));
    std::remove(TempIndexPath("completion").c_str());
}

// 3
TEST(IndexService, ANewRebuildCancelsTheRunningScan) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    scanner.Hold();
    IndexService service(scanner, store, nullptr, TempIndexPath("cancel"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);

    service.RequestIndexing(true, Roots({"/old"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.Running() == 1; }));
    service.RequestIndexing(true, Roots({"/new"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.SawCancel(); }));
    scanner.Release();  // lets the second scan, now held too, finish
    service.Flush();

    EXPECT_TRUE(scanner.SawCancel());
    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/new/a.txt", "/new/b.txt"}));
    std::remove(TempIndexPath("cancel").c_str());
}

// 4
TEST(IndexService, ASettingsChangeCancelsTheRunningScan) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    scanner.Hold();
    IndexService service(scanner, store, nullptr, TempIndexPath("settings_cancel"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);

    service.RequestIndexing(true, Roots({"/old"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.Running() == 1; }));
    service.RequestSettingsChange({"/old"}, Roots({"/new"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.SawCancel(); }));
    scanner.Release();
    service.Flush();

    EXPECT_TRUE(scanner.SawCancel());
    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/new/a.txt", "/new/b.txt"}));
    std::remove(TempIndexPath("settings_cancel").c_str());
}

// 5
TEST(IndexService, SettingsChangeAppliesTheRootDiff) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    const std::string idx = TempIndexPath("settings_diff");
    IndexService service(scanner, store, nullptr, idx, AlwaysSucceeds, recorder.Callbacks(),
                         FakeClock);
    service.RequestIndexing(true, Roots({"/a"}), 3600, false);
    service.Flush();

    // Added only: an incremental scan of just the new root, then saved.
    service.RequestSettingsChange({"/a"}, Roots({"/a", "/b"}), 3600, false);
    service.Flush();
    ASSERT_EQ(scanner.ScannedRoots().size(), 2u);
    EXPECT_EQ(scanner.ScannedRoots().back(), (std::vector<std::string>{"/b"}));
    EXPECT_EQ(LivePaths(store),
              (std::vector<std::string>{"/a/a.txt", "/a/b.txt", "/b/a.txt", "/b/b.txt"}));
    EXPECT_EQ(IndexSerializer::Load(idx).pool.Count(), 4u);

    // Removed only: no scan, entries gone, saved.
    const size_t scansBefore = scanner.ScannedRoots().size();
    service.RequestSettingsChange({"/a", "/b"}, Roots({"/b"}), 3600, false);
    service.Flush();
    EXPECT_EQ(scanner.ScannedRoots().size(), scansBefore);
    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/b/a.txt", "/b/b.txt"}));
    EXPECT_EQ(IndexSerializer::Load(idx).pool.Count(), 2u);

    // Both: a full rebuild on the new roots.
    service.RequestSettingsChange({"/b"}, Roots({"/c"}), 3600, false);
    service.Flush();
    ASSERT_EQ(scanner.ScannedRoots().size(), scansBefore + 1);
    EXPECT_EQ(scanner.ScannedRoots().back(), (std::vector<std::string>{"/c"}));
    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/c/a.txt", "/c/b.txt"}));

    std::lock_guard<std::mutex> lock(recorder.mutex);
    EXPECT_EQ(recorder.changes.back().kind, IndexChange::Kind::SettingsApplied);
    std::remove(idx.c_str());
}

// 6
TEST(IndexService, ReloadRequestsCoalesce) {
    HeldScanner scanner;
    CountingStore store;
    Recorder recorder;
    const std::string idx = TempIndexPath("reload_coalesce");
    IndexPool pool;
    pool.AddEntry(MakeEntry("/helper/x.txt"));
    ASSERT_TRUE(IndexSerializer::Save(idx, pool, FakeClock(), 0));
    IndexService service(scanner, store, nullptr, idx, AlwaysSucceeds, recorder.Callbacks(),
                         FakeClock);

    store.holdLoads_.store(true);
    service.RequestReload();
    ASSERT_TRUE(WaitUntil([&] { return store.loads_.load() == 1; }));
    for (int i = 0; i < 10; ++i) {
        service.RequestReload();
    }
    store.holdLoads_.store(false);
    service.Flush();

    EXPECT_LE(store.loads_.load(), 2);
    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/helper/x.txt"}));
    std::remove(idx.c_str());
}

// 7
TEST(IndexService, AFailedReloadKeepsTheLiveIndex) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    const std::string idx = TempIndexPath("reload_missing");
    IndexService service(scanner, store, nullptr, idx, AlwaysSucceeds, recorder.Callbacks(),
                         FakeClock);
    service.RequestIndexing(true, Roots({"/r"}), 3600, false);
    service.Flush();
    std::remove(idx.c_str());
    const size_t changesBefore = recorder.ChangeCount();

    service.RequestReload();
    service.Flush();

    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/r/a.txt", "/r/b.txt"}));
    EXPECT_EQ(recorder.ChangeCount(), changesBefore);
}

// 8
TEST(IndexService, FileOperationsRunInTheBackgroundAndUpdateTheIndex) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    std::atomic<bool> holdOps{true};
    auto runner = [&holdOps](const FileOperation& op) -> std::optional<std::string> {
        while (holdOps.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (op.path.find("b.txt") != std::string::npos) {
            return std::string("permission denied");
        }
        return std::nullopt;
    };
    IndexService service(scanner, store, nullptr, TempIndexPath("fileops"), runner,
                         recorder.Callbacks(), FakeClock);
    holdOps.store(false);
    service.RequestIndexing(true, Roots({"/r"}), 3600, false);
    service.Flush();
    holdOps.store(true);

    service.RequestFileOperations({{FileOperation::Type::MoveToTrash, "/r/a.txt"},
                                   {FileOperation::Type::DeletePermanently, "/r/b.txt"}});
    EXPECT_EQ(recorder.ReportCount(), 0u);  // returned while the operation is held
    holdOps.store(false);
    ASSERT_TRUE(WaitUntil([&] { return recorder.ReportCount() == 1; }));

    std::lock_guard<std::mutex> lock(recorder.mutex);
    EXPECT_EQ(recorder.reports[0].succeeded, (std::vector<std::string>{"/r/a.txt"}));
    ASSERT_EQ(recorder.reports[0].failed.size(), 1u);
    EXPECT_EQ(recorder.reports[0].failed[0].first, "/r/b.txt");
    EXPECT_EQ(recorder.reports[0].failed[0].second, "permission denied");
    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/r/b.txt"}));
    std::remove(TempIndexPath("fileops").c_str());
}

// 9
TEST(IndexService, FileOperationsDoNotWaitBehindAScan) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    scanner.Hold();
    IndexService service(scanner, store, nullptr, TempIndexPath("fileops_scan"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);
    service.RequestIndexing(true, Roots({"/r"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.Running() == 1; }));

    service.RequestFileOperations({{FileOperation::Type::MoveToTrash, "/elsewhere/x"}});

    EXPECT_TRUE(WaitUntil([&] { return recorder.ReportCount() == 1; }));
    EXPECT_EQ(scanner.Running(), 1);  // the scan is still held
    scanner.Release();
    std::remove(TempIndexPath("fileops_scan").c_str());
}

// 10
TEST(IndexService, ShutdownDuringAScanIsPrompt) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    scanner.Hold();
    IndexService service(scanner, store, nullptr, TempIndexPath("shutdown_scan"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);
    service.RequestIndexing(true, Roots({"/r"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.Running() == 1; }));

    const auto start = Clock::now();
    service.Shutdown();

    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(500));
    EXPECT_TRUE(scanner.SawCancel());
    EXPECT_TRUE(LivePaths(store).empty());  // the partial scan was never swapped in
}

// 11
TEST(IndexService, ShutdownLetsAFileOperationFinish) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    std::atomic<bool> holdOps{true};
    std::atomic<int> finished{0};
    auto runner = [&](const FileOperation&) -> std::optional<std::string> {
        while (holdOps.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++finished;
        return std::nullopt;
    };
    IndexService service(scanner, store, nullptr, TempIndexPath("shutdown_ops"), runner,
                         recorder.Callbacks(), FakeClock);
    service.RequestFileOperations({{FileOperation::Type::MoveToTrash, "/r/a.txt"}});

    std::atomic<bool> shutdownReturned{false};
    std::thread closer([&] {
        service.Shutdown();
        shutdownReturned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(shutdownReturned.load());
    holdOps.store(false);
    closer.join();

    EXPECT_EQ(finished.load(), 1);
    EXPECT_EQ(recorder.ReportCount(), 1u);
}

namespace {

// Counts concurrently running monitors; its stop takes a while, like an
// InotifyWatcher finishing a poll.
class SlowStoppingMonitor : public IChangeMonitor {
public:
    SlowStoppingMonitor(std::atomic<int>& active, std::atomic<int>& maxActive,
                        std::atomic<int>& started)
        : active_(active), maxActive_(maxActive), started_(started) {}
    bool IsAvailable(const std::string&) const override { return true; }
    void StartMonitoring(const std::string&, ChangeCallback,
                         const std::atomic<bool>& stopToken) override {
        const int now = ++active_;
        int seen = maxActive_.load();
        while (now > seen && !maxActive_.compare_exchange_weak(seen, now)) {
        }
        ++started_;
        while (!stopToken.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        --active_;
    }

private:
    std::atomic<int>& active_;
    std::atomic<int>& maxActive_;
    std::atomic<int>& started_;
};

}  // namespace

// 12
TEST(IndexService, MonitoringIsStoppedBeforeARebuildAndRestartedAfter) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    std::atomic<int> started{0};
    auto factory = [&](const std::string&) -> std::unique_ptr<IChangeMonitor> {
        return std::make_unique<SlowStoppingMonitor>(active, maxActive, started);
    };
    IndexService service(scanner, store, factory, TempIndexPath("monitoring"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);
    service.RequestIndexing(true, Roots({"/r"}), 3600, /*monitorAfter=*/true);
    ASSERT_TRUE(WaitUntil([&] { return started.load() == 1; }));

    const auto start = Clock::now();
    service.RequestIndexing(true, Roots({"/r"}), 3600, true);
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(50));  // didn't wait for the stop

    ASSERT_TRUE(WaitUntil([&] { return started.load() == 2; }));
    EXPECT_EQ(maxActive.load(), 1);
    service.Shutdown();
    EXPECT_EQ(active.load(), 0);
    std::remove(TempIndexPath("monitoring").c_str());
}

// Extra: a file removed while a rebuild is running must not reappear once
// the rebuild (which may have seen it) is swapped in.
TEST(IndexService, AFileRemovedDuringARebuildStaysRemoved) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    scanner.Hold();
    IndexService service(scanner, store, nullptr, TempIndexPath("removed_during"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);
    service.RequestIndexing(true, Roots({"/r"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.Running() == 1; }));  // a.txt already seen

    service.RequestFileOperations({{FileOperation::Type::MoveToTrash, "/r/a.txt"}});
    ASSERT_TRUE(WaitUntil([&] { return recorder.ReportCount() == 1; }));
    scanner.Release();
    service.Flush();

    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/r/b.txt"}));
    // The saved index must not bring it back after a restart either.
    IndexSerializer::LoadResult saved = IndexSerializer::Load(TempIndexPath("removed_during"));
    ASSERT_TRUE(saved.success);
    ASSERT_EQ(saved.pool.Count(), 1u);
    EXPECT_EQ(saved.pool.GetEntry(0).path, "/r/b.txt");
    std::remove(TempIndexPath("removed_during").c_str());
}

// The same removed-during-a-scan protection must hold for a Settings change
// that only adds a root (the incremental IndexPaths path), not just a full
// rebuild: a file trashed while that scan is running must not be brought
// back by the persist that follows it, and must not survive a restart.
TEST(IndexService, AFileRemovedDuringASettingsChangeFullRebuildStaysRemoved) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    // bothChanged() forces the full-rebuild branch (BeginWrite/staging,
    // same swap-in race FinishScan protects against for a plain rebuild),
    // not the incremental IndexPaths branch -- that one applies directly to
    // the live pool and never stages, so a concurrent removal can't race it.
    const std::string idx = TempIndexPath("removed_during_settings_rebuild");
    IndexService service(scanner, store, nullptr, idx, AlwaysSucceeds, recorder.Callbacks(),
                         FakeClock);
    service.RequestIndexing(true, Roots({"/old"}), 3600, false);
    service.Flush();

    scanner.Hold();
    service.RequestSettingsChange({"/old"}, Roots({"/new"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return scanner.Running() == 1; }));  // /new/a.txt already seen

    service.RequestFileOperations({{FileOperation::Type::MoveToTrash, "/new/a.txt"}});
    ASSERT_TRUE(WaitUntil([&] { return recorder.ReportCount() == 1; }));
    scanner.Release();
    service.Flush();

    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/new/b.txt"}));
    IndexSerializer::LoadResult saved = IndexSerializer::Load(idx);
    ASSERT_TRUE(saved.success);
    ASSERT_EQ(saved.pool.Count(), 1u);
    EXPECT_EQ(saved.pool.GetEntry(0).path, "/new/b.txt");
    std::remove(idx.c_str());
}

namespace {

// Fires one Removed event for `path` as soon as monitoring starts, then
// blocks until told to stop -- enough to exercise the Indexer -> IndexService
// -> IndexServiceCallbacks::onLiveChange forwarding path without needing a
// real filesystem watch.
class FiresOneRemovalMonitor : public IChangeMonitor {
public:
    explicit FiresOneRemovalMonitor(std::string path) : path_(std::move(path)) {}
    bool IsAvailable(const std::string&) const override { return true; }
    void StartMonitoring(const std::string&, ChangeCallback onChange,
                         const std::atomic<bool>& stopToken) override {
        onChange(FileChangeEvent{FileChangeType::Removed, path_, std::string()});
        while (!stopToken.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    std::string path_;
};

}  // namespace

// The service passes the underlying Indexer's mutation callback (fired when
// a live-monitoring change is applied to the store) straight through to
// IndexServiceCallbacks::onLiveChange, so a MainWindow watching for it
// learns about filesystem changes picked up outside a scan.
TEST(IndexService, ForwardsLiveMonitoringChangesToOnLiveChange) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    std::atomic<int> liveChanges{0};
    IndexServiceCallbacks callbacks = recorder.Callbacks();
    callbacks.onLiveChange = [&liveChanges]() { ++liveChanges; };
    auto factory = [](const std::string& root) -> std::unique_ptr<IChangeMonitor> {
        return std::make_unique<FiresOneRemovalMonitor>(root + "/a.txt");
    };
    IndexService service(scanner, store, factory, TempIndexPath("live_change"), AlwaysSucceeds,
                         callbacks, FakeClock);

    service.RequestIndexing(true, Roots({"/r"}), 3600, /*monitorAfter=*/true);
    service.Flush();

    ASSERT_TRUE(WaitUntil([&] { return liveChanges.load() >= 1; }));
    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/r/b.txt"}));
    service.Shutdown();
    std::remove(TempIndexPath("live_change").c_str());
}

// A caller that doesn't care about index-changed notifications (passes a
// default-constructed IndexServiceCallbacks, leaving onIndexChanged unset)
// must not crash when one would otherwise be reported.
TEST(IndexService, WorksWithNoIndexChangedCallbackRegistered) {
    HeldScanner scanner;
    IndexStore store;
    IndexServiceCallbacks callbacks;  // every field left null
    IndexService service(scanner, store, nullptr, TempIndexPath("no_callback"), AlwaysSucceeds,
                         callbacks, FakeClock);

    service.RequestIndexing(true, Roots({"/r"}), 3600, false);
    service.Flush();  // must not crash despite onIndexChanged being unset

    std::remove(TempIndexPath("no_callback").c_str());
}

// Extra: scan progress arrives per directory from every scanner thread;
// forwarding each one would flood a UI event queue.
TEST(IndexService, ScanProgressIsThrottled) {
    HeldScanner scanner;
    IndexStore store;
    Recorder recorder;
    scanner.SetProgressPerEntry(5000);
    IndexService service(scanner, store, nullptr, TempIndexPath("throttle"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);

    service.RequestIndexing(true, Roots({"/r"}), 3600, false);
    service.Flush();

    std::lock_guard<std::mutex> lock(recorder.mutex);
    const auto scanning = std::count_if(
        recorder.statuses.begin(), recorder.statuses.end(),
        [](const IndexerStatus& status) { return status.state == IndexerState::Scanning; });
    EXPECT_LT(scanning, 50);
    ASSERT_FALSE(recorder.statuses.empty());
    EXPECT_EQ(recorder.statuses.back().state, IndexerState::Idle);
    std::remove(TempIndexPath("throttle").c_str());
}

// Review finding: a Settings change cancelled while still queued must not
// leave the next one diffing against roots the index never got. S1 removes
// /b (running), S2 adds /c (queued), S3 adds /d and cancels S2: the final
// index must cover /a, /c and /d.
TEST(IndexService, ASettingsChangeCancelledBeforeItRanIsNotLost) {
    HeldScanner scanner;
    HoldingRemoveStore store;
    Recorder recorder;
    IndexService service(scanner, store, nullptr, TempIndexPath("skipped_settings"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);
    service.RequestIndexing(true, Roots({"/a", "/b"}), 3600, false);
    service.Flush();

    store.holdRemovals_.store(true);
    service.RequestSettingsChange({"/a", "/b"}, Roots({"/a"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return store.removals_.load() == 1; }));
    service.RequestSettingsChange({"/a"}, Roots({"/a", "/c"}), 3600, false);
    service.RequestSettingsChange({"/a", "/c"}, Roots({"/a", "/c", "/d"}), 3600, false);
    store.holdRemovals_.store(false);
    service.Flush();

    EXPECT_EQ(LivePaths(store), (std::vector<std::string>{"/a/a.txt", "/a/b.txt", "/c/a.txt",
                                                          "/c/b.txt", "/d/a.txt", "/d/b.txt"}));
    std::remove(TempIndexPath("skipped_settings").c_str());
}

// Review finding: a rebuild superseded before it ran leaves the index
// unknown to the next Settings change too, which must then rebuild in full
// rather than apply its diff on top.
TEST(IndexService, ARebuildCancelledBeforeItRanForcesAFullRebuildNext) {
    HeldScanner scanner;
    HoldingRemoveStore store;
    Recorder recorder;
    IndexService service(scanner, store, nullptr, TempIndexPath("skipped_rebuild"), AlwaysSucceeds,
                         recorder.Callbacks(), FakeClock);
    service.RequestIndexing(true, Roots({"/a", "/x"}), 3600, false);
    service.Flush();

    store.holdRemovals_.store(true);
    service.RequestSettingsChange({"/a", "/x"}, Roots({"/a"}), 3600, false);
    ASSERT_TRUE(WaitUntil([&] { return store.removals_.load() == 1; }));
    service.RequestIndexing(true, Roots({"/a"}), 3600, false);
    service.RequestSettingsChange({"/a"}, Roots({"/a", "/b"}), 3600, false);
    store.holdRemovals_.store(false);
    service.Flush();

    EXPECT_EQ(scanner.ScannedRoots().back(), (std::vector<std::string>{"/a", "/b"}));
    std::remove(TempIndexPath("skipped_rebuild").c_str());
}
