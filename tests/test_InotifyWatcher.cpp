#include <gtest/gtest.h>

#include "indexer/InotifyWatcher.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using indexed::FileChangeEvent;
using indexed::FileChangeType;
using indexed::InotifyWatcher;

namespace {

namespace fs = std::filesystem;

// Creates a unique temp directory under the system temp dir, removed on
// destruction. Mirrors tests/test_WalkScanner.cpp's TempDir helper.
class TempDir {
public:
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "inotifywatcher_test_XXXXXX").string();
        std::vector<char> buffer(tmpl.begin(), tmpl.end());
        buffer.push_back('\0');
        char* result = mkdtemp(buffer.data());
        path_ = result != nullptr ? std::string(result) : std::string();
    }

    ~TempDir() {
        if (!path_.empty()) {
            std::error_code errorCode;
            fs::remove_all(path_, errorCode);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& Path() const { return path_; }

private:
    std::string path_;
};

void WriteFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// Thread-safe sink: StartMonitoring invokes onChange on the monitoring
// thread while the test thread mutates the tree and inspects results
// concurrently.
class EventCollector {
public:
    void OnChange(const FileChangeEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }

    std::vector<FileChangeEvent> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<FileChangeEvent> events_;
};

bool HasEvent(const std::vector<FileChangeEvent>& events, FileChangeType type,
              const std::string& path) {
    return std::any_of(events.begin(), events.end(), [&](const FileChangeEvent& event) {
        return event.type == type && event.path == path;
    });
}

bool HasRenameEvent(const std::vector<FileChangeEvent>& events, const std::string& oldPath,
                    const std::string& newPath) {
    return std::any_of(events.begin(), events.end(), [&](const FileChangeEvent& event) {
        return event.type == FileChangeType::Renamed && event.path == newPath &&
               event.oldPath == oldPath;
    });
}

// Polls `predicate` (instead of a fixed sleep) so the test finishes as soon
// as the expected event arrives -- inotify delivery is asynchronous, so a
// naive fixed sleep would be both slow (worst case) and flaky (best case).
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

// Runs `watcher` on a background thread and joins it (with a generous
// timeout enforced by the caller via stopToken + WaitFor) at scope exit.
class MonitorSession {
public:
    MonitorSession(InotifyWatcher& watcher, const std::string& root, EventCollector& collector)
        : stopToken_(false), thread_([&watcher, root, &collector, this]() {
              watcher.StartMonitoring(
                  root, [&collector](const FileChangeEvent& event) { collector.OnChange(event); },
                  stopToken_);
          }) {}

    ~MonitorSession() {
        stopToken_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    MonitorSession(const MonitorSession&) = delete;
    MonitorSession& operator=(const MonitorSession&) = delete;

    std::atomic<bool>& StopToken() { return stopToken_; }

private:
    std::atomic<bool> stopToken_;
    std::thread thread_;
};

// Watch registration happens on the monitoring thread, asynchronously with
// respect to the test thread constructing MonitorSession -- there's a real
// window where the test could mutate the tree before the initial
// inotify_add_watch() has actually happened, in which case the kernel never
// generates an event for that mutation at all (not merely delayed). Rather
// than bridge that with a fixed sleep (inherently racy: right on a slow CI
// box, wrong -- and slow -- everywhere else), poll for readiness using
// disposable sentinel files: each attempt creates a uniquely-named file and
// waits briefly for its own Added event, retrying with a new name until one
// is observed. Once that happens, the watch is provably live.
void WaitUntilWatcherReady(const std::string& dir, EventCollector& collector) {
    constexpr int kMaxAttempts = 100;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::string sentinel = dir + "/.ready_sentinel_" + std::to_string(attempt);
        WriteFile(sentinel, "x");
        bool seen =
            WaitFor([&] { return HasEvent(collector.Snapshot(), FileChangeType::Added, sentinel); },
                    std::chrono::milliseconds(50));
        std::error_code errorCode;
        fs::remove(sentinel, errorCode);
        if (seen) {
            return;
        }
    }
}

}  // namespace

TEST(InotifyWatcher, IsAvailableTrueForRealDirectoryFalseOtherwise) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    InotifyWatcher watcher;
    EXPECT_TRUE(watcher.IsAvailable(tempDir.Path()));
    EXPECT_FALSE(watcher.IsAvailable(tempDir.Path() + "/does_not_exist"));
}

TEST(InotifyWatcher, WatchLimitNotExceededByDefault) {
    InotifyWatcher watcher;
    EXPECT_FALSE(watcher.WatchLimitExceeded());
}

TEST(InotifyWatcher, CreatedFileReportsAdded) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    InotifyWatcher watcher;
    EventCollector collector;
    MonitorSession session(watcher, tempDir.Path(), collector);
    WaitUntilWatcherReady(tempDir.Path(), collector);

    std::string filePath = tempDir.Path() + "/new_file.txt";
    WriteFile(filePath, "hello");

    EXPECT_TRUE(
        WaitFor([&] { return HasEvent(collector.Snapshot(), FileChangeType::Added, filePath); }));
}

TEST(InotifyWatcher, DeletedFileReportsRemoved) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());
    std::string filePath = tempDir.Path() + "/to_delete.txt";
    WriteFile(filePath, "bye");

    InotifyWatcher watcher;
    EventCollector collector;
    MonitorSession session(watcher, tempDir.Path(), collector);
    WaitUntilWatcherReady(tempDir.Path(), collector);

    std::error_code errorCode;
    fs::remove(filePath, errorCode);
    ASSERT_FALSE(errorCode) << errorCode.message();

    EXPECT_TRUE(
        WaitFor([&] { return HasEvent(collector.Snapshot(), FileChangeType::Removed, filePath); }));
}

TEST(InotifyWatcher, ModifiedFileReportsModified) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());
    std::string filePath = tempDir.Path() + "/to_modify.txt";
    WriteFile(filePath, "v1");

    InotifyWatcher watcher;
    EventCollector collector;
    MonitorSession session(watcher, tempDir.Path(), collector);
    WaitUntilWatcherReady(tempDir.Path(), collector);

    WriteFile(filePath, "v2 longer contents");

    EXPECT_TRUE(WaitFor(
        [&] { return HasEvent(collector.Snapshot(), FileChangeType::Modified, filePath); }));
}

TEST(InotifyWatcher, RenamedFileWithinTreeReportsRenamedWithOldAndNewPath) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());
    std::string oldPath = tempDir.Path() + "/before.txt";
    std::string newPath = tempDir.Path() + "/after.txt";
    WriteFile(oldPath, "contents");

    InotifyWatcher watcher;
    EventCollector collector;
    MonitorSession session(watcher, tempDir.Path(), collector);
    WaitUntilWatcherReady(tempDir.Path(), collector);

    std::error_code errorCode;
    fs::rename(oldPath, newPath, errorCode);
    ASSERT_FALSE(errorCode) << errorCode.message();

    EXPECT_TRUE(WaitFor([&] { return HasRenameEvent(collector.Snapshot(), oldPath, newPath); }));
}

TEST(InotifyWatcher, NewSubdirectoryIsWatchedLiveAndFileInsideReported) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    InotifyWatcher watcher;
    EventCollector collector;
    MonitorSession session(watcher, tempDir.Path(), collector);
    WaitUntilWatcherReady(tempDir.Path(), collector);

    std::string subdir = tempDir.Path() + "/live_subdir";
    fs::create_directory(subdir);

    // Wait for the new directory itself to be picked up before creating a
    // file inside it, so there's no race between the watcher registering
    // the new watch and the test writing into it.
    ASSERT_TRUE(
        WaitFor([&] { return HasEvent(collector.Snapshot(), FileChangeType::Added, subdir); }));

    std::string nestedFile = subdir + "/nested.txt";
    WriteFile(nestedFile, "nested contents");

    EXPECT_TRUE(
        WaitFor([&] { return HasEvent(collector.Snapshot(), FileChangeType::Added, nestedFile); }));
}

TEST(InotifyWatcher, AlreadyPopulatedDirectoryMovedInReportsItsContents) {
    // The real-world "mkdir d && touch d/f" and "mv populated_dir into the
    // tree" case: contents exist before the watcher can watch the new
    // directory. The watcher must walk what is already there, not only
    // react to future events inside it.
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());
    TempDir stagingDir;  // a sibling temp dir, outside the watched root
    ASSERT_FALSE(stagingDir.Path().empty());

    // Build a populated subtree entirely outside the watched root.
    const std::string staged = stagingDir.Path() + "/payload";
    fs::create_directories(staged + "/sub");
    WriteFile(staged + "/top.txt", "a");
    WriteFile(staged + "/sub/deep.txt", "b");

    InotifyWatcher watcher;
    EventCollector collector;
    MonitorSession session(watcher, tempDir.Path(), collector);
    WaitUntilWatcherReady(tempDir.Path(), collector);

    // Atomic move into the watched tree: no window for the test to "wait
    // for the dir, then write" -- the contents are already present.
    const std::string moved = tempDir.Path() + "/payload";
    std::error_code ec;
    fs::rename(staged, moved, ec);
    ASSERT_FALSE(ec);

    EXPECT_TRUE(WaitFor(
        [&] { return HasEvent(collector.Snapshot(), FileChangeType::Added, moved + "/top.txt"); }));
    EXPECT_TRUE(WaitFor([&] {
        return HasEvent(collector.Snapshot(), FileChangeType::Added, moved + "/sub/deep.txt");
    }));

    // A file created inside the moved-in nested subdir is still picked up,
    // proving the subtree's watches were registered too.
    const std::string later = moved + "/sub/later.txt";
    WriteFile(later, "c");
    EXPECT_TRUE(
        WaitFor([&] { return HasEvent(collector.Snapshot(), FileChangeType::Added, later); }));
}

// The stop check inside WatchAndEmitNewSubtree's own walk (distinct from
// AddWatchesRecursive's initial-walk check, docs/adr/0014): a large,
// already-populated directory moved into the watched tree live must not
// force StartMonitoring to finish walking it before honoring a stop
// request.
TEST(InotifyWatcher, StopDuringALiveMovedInSubtreeWalkReturnsPromptly) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());
    TempDir stagingDir;
    ASSERT_FALSE(stagingDir.Path().empty());

    const std::string staged = stagingDir.Path() + "/payload";
    constexpr int kDirCount = 20000;
    for (int i = 0; i < kDirCount; ++i) {
        fs::create_directories(staged + "/d" + std::to_string(i));
        WriteFile(staged + "/d" + std::to_string(i) + "/f.txt", "x");
    }

    InotifyWatcher watcher;
    EventCollector collector;
    std::atomic<bool> stopToken{false};
    std::atomic<bool> returned{false};
    std::thread monitorThread([&]() {
        watcher.StartMonitoring(
            tempDir.Path(),
            [&collector](const FileChangeEvent& event) { collector.OnChange(event); }, stopToken);
        returned.store(true, std::memory_order_relaxed);
    });
    WaitUntilWatcherReady(tempDir.Path(), collector);

    const std::string moved = tempDir.Path() + "/payload";
    std::error_code ec;
    fs::rename(staged, moved, ec);
    ASSERT_FALSE(ec);

    // Races the walk from a second thread rather than stopping inline, so
    // the walk has actually started (and is mid-flight, not merely queued)
    // when the flag flips.
    std::thread stopper([&]() { stopToken.store(true, std::memory_order_relaxed); });
    stopper.join();

    ASSERT_TRUE(
        WaitFor([&] { return returned.load(std::memory_order_relaxed); }, std::chrono::seconds(5)))
        << "StartMonitoring never returned";
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    // The decisive assertion: with the stop check working, the walk cannot
    // have reported anywhere near all kDirCount moved-in files -- it was cut
    // short mid-iteration. Without the check (see the mutation this test
    // guards against), it reports all of them regardless of when stop was
    // requested, because nothing inside the loop ever looks at stopToken.
    const std::vector<FileChangeEvent> events = collector.Snapshot();
    const auto addedCount = std::count_if(events.begin(), events.end(), [&](const auto& event) {
        return event.type == FileChangeType::Added && event.path.rfind(moved, 0) == 0;
    });
    EXPECT_LT(addedCount, kDirCount)
        << "the moved-in subtree walk ran to completion instead of honoring stopToken";
}

TEST(InotifyWatcher, StopTokenCausesPromptReturn) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    InotifyWatcher watcher;
    EventCollector collector;
    std::atomic<bool> stopToken{false};
    std::atomic<bool> returned{false};

    std::thread monitorThread([&]() {
        watcher.StartMonitoring(
            tempDir.Path(),
            [&collector](const FileChangeEvent& event) { collector.OnChange(event); }, stopToken);
        returned.store(true, std::memory_order_relaxed);
    });

    // Give it a moment to actually get into its poll loop before stopping.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stopToken.store(true, std::memory_order_relaxed);

    bool returnedInTime =
        WaitFor([&] { return returned.load(std::memory_order_relaxed); }, std::chrono::seconds(2));
    EXPECT_TRUE(returnedInTime) << "StartMonitoring did not return promptly after stopToken set";

    if (monitorThread.joinable()) {
        monitorThread.join();
    }
}

// Stopping monitoring must not wait for the initial whole-tree watch walk
// (ADR 0014): with the stop flag already set, the walk ends early instead
// of visiting every directory.
TEST(InotifyWatcher, InitialWalkStopsEarlyOnceStopIsRequested) {
    TempDir dir;
    ASSERT_FALSE(dir.Path().empty());
    for (int i = 0; i < 300; ++i) {
        fs::create_directories(dir.Path() + "/d" + std::to_string(i) + "/sub");
    }

    std::atomic<bool> stop{true};
    InotifyWatcher watcher;
    watcher.StartMonitoring(dir.Path(), [](const FileChangeEvent&) {}, stop);

    EXPECT_LE(watcher.InitialWatchCount(), 1u);
}

TEST(InotifyWatcher, InitialWalkWatchesEveryDirectoryWhenNotStopped) {
    TempDir dir;
    ASSERT_FALSE(dir.Path().empty());
    for (int i = 0; i < 20; ++i) {
        fs::create_directories(dir.Path() + "/d" + std::to_string(i) + "/sub");
    }

    std::atomic<bool> stop{false};
    InotifyWatcher watcher;
    std::thread monitor(
        [&]() { watcher.StartMonitoring(dir.Path(), [](const FileChangeEvent&) {}, stop); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (watcher.InitialWatchCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true);
    monitor.join();

    EXPECT_EQ(watcher.InitialWatchCount(), 41u);  // root + 20 dirs + 20 subdirs
}
