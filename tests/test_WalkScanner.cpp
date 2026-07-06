#include <gtest/gtest.h>

#include "indexer/WalkScanner.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using indexed::FileEntry;
using indexed::kAttrDirectory;
using indexed::kAttrHidden;
using indexed::ScanOptions;
using indexed::WalkScanner;

namespace {

namespace fs = std::filesystem;

// Thread-safe sink: WalkScanner may invoke onEntry/onProgress concurrently
// from multiple worker threads, so the test fixture must synchronize.
class CollectingSink {
public:
    void OnEntry(const FileEntry& entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        entriesByPath_[entry.path] = entry;
    }

    void OnProgress(uint64_t filesFound, const std::string& currentDir) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++progressCalls_;
        lastFilesFound_ = filesFound;
        (void)currentDir;
    }

    bool Contains(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entriesByPath_.find(path) != entriesByPath_.end();
    }

    std::optional<FileEntry> Find(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entriesByPath_.find(path);
        if (it == entriesByPath_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entriesByPath_.size();
    }

    bool AnyPathStartsWith(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [path, entry] : entriesByPath_) {
            if (path.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
        return false;
    }

    int ProgressCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return progressCalls_;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, FileEntry> entriesByPath_;
    int progressCalls_ = 0;
    uint64_t lastFilesFound_ = 0;
};

// Creates a unique temp directory under the system temp dir, removed on
// destruction. Using mkdtemp keeps this independent of any fixture beyond
// plain POSIX.
class TempDir {
public:
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "walkscanner_test_XXXXXX").string();
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

}  // namespace

TEST(WalkScanner, FastScanAvailableIsAlwaysFalse) {
    WalkScanner scanner;
    EXPECT_FALSE(scanner.FastScanAvailable("/"));
    EXPECT_FALSE(scanner.FastScanAvailable("/nonexistent"));
}

TEST(WalkScanner, WalksMultiFileMultiDirTree) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    fs::create_directories(tempDir.Path() + "/subdir");
    fs::create_directories(tempDir.Path() + "/.hidden_dir");
    WriteFile(tempDir.Path() + "/top.txt", "top-level file");
    WriteFile(tempDir.Path() + "/subdir/nested.txt", "nested file contents");
    WriteFile(tempDir.Path() + "/.hidden_file", "dotfile");

    CollectingSink sink;
    ScanOptions options;
    options.rootPaths = {tempDir.Path()};
    std::atomic<bool> cancelToken{false};

    WalkScanner scanner;
    scanner.Scan(
        options, [&](const FileEntry& entry) { sink.OnEntry(entry); },
        [&](uint64_t filesFound, const std::string& currentDir) {
            sink.OnProgress(filesFound, currentDir);
        },
        cancelToken);

    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/top.txt"));
    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/subdir"));
    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/subdir/nested.txt"));
    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/.hidden_file"));
    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/.hidden_dir"));
    EXPECT_EQ(sink.Count(), 5u);

    auto topEntry = sink.Find(tempDir.Path() + "/top.txt");
    ASSERT_TRUE(topEntry.has_value());
    EXPECT_EQ(topEntry->name, "top.txt");
    EXPECT_EQ(topEntry->size, std::string("top-level file").size());
    EXPECT_GT(topEntry->lastModified, 0u);
    EXPECT_EQ(topEntry->attributes & kAttrDirectory, 0u);
    EXPECT_EQ(topEntry->attributes & kAttrHidden, 0u);

    auto subdirEntry = sink.Find(tempDir.Path() + "/subdir");
    ASSERT_TRUE(subdirEntry.has_value());
    EXPECT_NE(subdirEntry->attributes & kAttrDirectory, 0u);

    auto hiddenFileEntry = sink.Find(tempDir.Path() + "/.hidden_file");
    ASSERT_TRUE(hiddenFileEntry.has_value());
    EXPECT_NE(hiddenFileEntry->attributes & kAttrHidden, 0u);

    auto hiddenDirEntry = sink.Find(tempDir.Path() + "/.hidden_dir");
    ASSERT_TRUE(hiddenDirEntry.has_value());
    EXPECT_NE(hiddenDirEntry->attributes & kAttrDirectory, 0u);
    EXPECT_NE(hiddenDirEntry->attributes & kAttrHidden, 0u);

    EXPECT_GT(sink.ProgressCalls(), 0);
}

TEST(WalkScanner, ExcludedSubdirectoryIsPruned) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    fs::create_directories(tempDir.Path() + "/keep");
    fs::create_directories(tempDir.Path() + "/excluded/nested");
    WriteFile(tempDir.Path() + "/keep/a.txt", "a");
    WriteFile(tempDir.Path() + "/excluded/b.txt", "b");
    WriteFile(tempDir.Path() + "/excluded/nested/c.txt", "c");

    CollectingSink sink;
    ScanOptions options;
    options.rootPaths = {tempDir.Path()};
    options.excludedPaths = {tempDir.Path() + "/excluded/"};  // trailing slash on purpose
    std::atomic<bool> cancelToken{false};

    WalkScanner scanner;
    scanner.Scan(
        options, [&](const FileEntry& entry) { sink.OnEntry(entry); },
        [&](uint64_t, const std::string&) {}, cancelToken);

    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/keep"));
    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/keep/a.txt"));
    EXPECT_FALSE(sink.Contains(tempDir.Path() + "/excluded"));
    EXPECT_FALSE(sink.Contains(tempDir.Path() + "/excluded/b.txt"));
    EXPECT_FALSE(sink.Contains(tempDir.Path() + "/excluded/nested"));
    EXPECT_FALSE(sink.Contains(tempDir.Path() + "/excluded/nested/c.txt"));
    EXPECT_FALSE(sink.AnyPathStartsWith(tempDir.Path() + "/excluded"));
}

TEST(WalkScanner, SymlinkIsSkippedEntirely) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    fs::create_directories(tempDir.Path() + "/realdir");
    WriteFile(tempDir.Path() + "/realdir/real.txt", "real file");
    WriteFile(tempDir.Path() + "/target.txt", "symlink target contents");

    std::error_code errorCode;
    fs::create_symlink(tempDir.Path() + "/target.txt", tempDir.Path() + "/link_to_file.txt",
                       errorCode);
    ASSERT_FALSE(errorCode) << errorCode.message();
    fs::create_directory_symlink(tempDir.Path() + "/realdir", tempDir.Path() + "/link_to_dir",
                                 errorCode);
    ASSERT_FALSE(errorCode) << errorCode.message();

    CollectingSink sink;
    ScanOptions options;
    options.rootPaths = {tempDir.Path()};
    std::atomic<bool> cancelToken{false};

    WalkScanner scanner;
    scanner.Scan(
        options, [&](const FileEntry& entry) { sink.OnEntry(entry); },
        [&](uint64_t, const std::string&) {}, cancelToken);

    // Real content is still found.
    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/target.txt"));
    EXPECT_TRUE(sink.Contains(tempDir.Path() + "/realdir/real.txt"));

    // Symlinks themselves are never emitted...
    EXPECT_FALSE(sink.Contains(tempDir.Path() + "/link_to_file.txt"));
    EXPECT_FALSE(sink.Contains(tempDir.Path() + "/link_to_dir"));
    // ...and a symlinked directory is never descended into (which would
    // otherwise re-surface real.txt under a second path).
    EXPECT_FALSE(sink.Contains(tempDir.Path() + "/link_to_dir/real.txt"));
}

TEST(WalkScanner, CancellationStopsEarly) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    constexpr int kDirCount = 300;
    for (int i = 0; i < kDirCount; ++i) {
        std::string dirPath = tempDir.Path() + "/dir" + std::to_string(i);
        fs::create_directories(dirPath);
        WriteFile(dirPath + "/file.txt", "contents");
    }

    CollectingSink sink;
    std::atomic<bool> cancelToken{false};
    std::atomic<int> entriesSeen{0};

    ScanOptions options;
    options.rootPaths = {tempDir.Path()};

    WalkScanner scanner;
    scanner.Scan(
        options,
        [&](const FileEntry& entry) {
            sink.OnEntry(entry);
            if (entriesSeen.fetch_add(1, std::memory_order_relaxed) + 1 >= 10) {
                cancelToken.store(true, std::memory_order_relaxed);
            }
        },
        [&](uint64_t, const std::string&) {}, cancelToken);

    // 300 dirs * 2 entries each (dir + file) = 600 possible entries; cancelling
    // after ~10 must leave the walk well short of the full set.
    EXPECT_LT(sink.Count(), 600u);
    EXPECT_GT(sink.Count(), 0u);
}

TEST(WalkScanner, ProgressCallbackFires) {
    TempDir tempDir;
    ASSERT_FALSE(tempDir.Path().empty());

    fs::create_directories(tempDir.Path() + "/a");
    fs::create_directories(tempDir.Path() + "/b");
    WriteFile(tempDir.Path() + "/a/1.txt", "1");
    WriteFile(tempDir.Path() + "/b/2.txt", "22");

    CollectingSink sink;
    ScanOptions options;
    options.rootPaths = {tempDir.Path()};
    std::atomic<bool> cancelToken{false};

    WalkScanner scanner;
    scanner.Scan(
        options, [&](const FileEntry& entry) { sink.OnEntry(entry); },
        [&](uint64_t filesFound, const std::string& currentDir) {
            sink.OnProgress(filesFound, currentDir);
        },
        cancelToken);

    EXPECT_GT(sink.ProgressCalls(), 0);
}

// Mount-boundary crossing (st_dev differs from the root's own st_dev) is not
// covered by an automated test here: reliably fabricating a second mount
// point requires root privileges (a loopback/bind mount) which is not
// available in this test environment. The behavior is implemented per
// indexed-plan.md §7.1 (compare st_dev against the root's captured st_dev,
// descend anyway when the child directory is itself a selected root) but is
// a documented coverage gap rather than faked with a fake filesystem.
