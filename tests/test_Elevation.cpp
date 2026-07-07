#include <fcntl.h>
#include <gtest/gtest.h>
#include <pwd.h>
#include <unistd.h>

#include "platform/Elevation.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using indexed::ElevationError;
using indexed::OpenForRootWrite;
using indexed::ResolveTargetUser;
using indexed::TargetUser;

namespace {

std::string TempDirPath(const std::string& name) {
    std::string path = ::testing::TempDir() + "indexed_test_elevation_" + name + "_" +
                       std::to_string(static_cast<long>(getpid()));
    std::filesystem::remove_all(path);
    return path;
}

// Reads the entire content of a file for assertions after a (would-be)
// write, so a rejected OpenForRootWrite can be proven to have touched
// nothing.
std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// RAII helper: saves/restores PKEXEC_UID around a test so tests don't leak
// environment state into each other.
class PkexecUidEnvGuard {
public:
    PkexecUidEnvGuard() {
        const char* existing = std::getenv("PKEXEC_UID");
        hadValue_ = existing != nullptr;
        if (hadValue_) {
            savedValue_ = existing;
        }
    }
    ~PkexecUidEnvGuard() {
        if (hadValue_) {
            setenv("PKEXEC_UID", savedValue_.c_str(), 1);
        } else {
            unsetenv("PKEXEC_UID");
        }
    }

private:
    bool hadValue_ = false;
    std::string savedValue_;
};

}  // namespace

// ---------------------------------------------------------------------
// ResolveTargetUser
// ---------------------------------------------------------------------

TEST(ResolveTargetUser, SucceedsAndReturnsCorrectHomeDirForOwnUid) {
    PkexecUidEnvGuard guard;
    uid_t realUid = getuid();
    setenv("PKEXEC_UID", std::to_string(static_cast<unsigned long>(realUid)).c_str(), 1);

    // getpwuid(getuid()) is the trusted, comparable expected value for our
    // own process's real uid -- exactly what ResolveTargetUser should
    // independently arrive at via PKEXEC_UID.
    struct passwd* expected = getpwuid(realUid);
    ASSERT_NE(expected, nullptr);

    std::optional<TargetUser> result = ResolveTargetUser();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->uid, realUid);
    EXPECT_EQ(result->homeDir, expected->pw_dir);
    EXPECT_EQ(result->username, expected->pw_name);
}

TEST(ResolveTargetUser, FailsCleanlyWhenPkexecUidUnset) {
    PkexecUidEnvGuard guard;
    unsetenv("PKEXEC_UID");

    EXPECT_FALSE(ResolveTargetUser().has_value());
}

TEST(ResolveTargetUser, FailsCleanlyWhenPkexecUidIsGarbage) {
    PkexecUidEnvGuard guard;
    setenv("PKEXEC_UID", "not-a-uid", 1);

    EXPECT_FALSE(ResolveTargetUser().has_value());
}

TEST(ResolveTargetUser, FailsCleanlyWhenPkexecUidHasTrailingGarbage) {
    PkexecUidEnvGuard guard;
    setenv("PKEXEC_UID", "123abc", 1);

    EXPECT_FALSE(ResolveTargetUser().has_value());
}

TEST(ResolveTargetUser, FailsCleanlyWhenPkexecUidIsNegative) {
    PkexecUidEnvGuard guard;
    setenv("PKEXEC_UID", "-1", 1);

    EXPECT_FALSE(ResolveTargetUser().has_value());
}

TEST(ResolveTargetUser, FailsCleanlyWhenPkexecUidDoesNotExistOnSystem) {
    PkexecUidEnvGuard guard;
    // 4294967294 (UINT32_MAX - 1) is used as a uid vanishingly unlikely to be
    // assigned to a real account on any test machine (uid_t is 32-bit
    // unsigned on Linux; the very top of the range is conventionally
    // reserved/unused, e.g. 4294967295 == (uid_t)-1 is nobody/invalid).
    setenv("PKEXEC_UID", "4294967294", 1);

    EXPECT_FALSE(ResolveTargetUser().has_value());
}

// ---------------------------------------------------------------------
// OpenForRootWrite
// ---------------------------------------------------------------------

TEST(OpenForRootWrite, SucceedsWritingGenuinelyOwnedPathWithNoSymlinks) {
    std::string baseDir = TempDirPath("owned_ok");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";

    int fd = -1;
    ElevationError err =
        OpenForRootWrite(filePath, getuid(), baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd);

    EXPECT_EQ(err, ElevationError::kNone);
    ASSERT_GE(fd, 0);
    const char* payload = "hello";
    EXPECT_EQ(write(fd, payload, 5), 5);
    close(fd);
    EXPECT_EQ(ReadFile(filePath), "hello");

    std::filesystem::remove_all(baseDir);
}

TEST(OpenForRootWrite, RejectsWhenFinalPathIsASymlink) {
    std::string baseDir = TempDirPath("final_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string realTarget = TempDirPath("final_symlink_victim");
    {
        std::ofstream victim(realTarget);
        victim << "do-not-touch";
    }
    std::string symlinkPath = baseDir + "/indexed.idx";
    ASSERT_EQ(symlink(realTarget.c_str(), symlinkPath.c_str()), 0);

    int fd = -1;
    ElevationError err =
        OpenForRootWrite(symlinkPath, getuid(), baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd);

    EXPECT_EQ(err, ElevationError::kSymlinkInPath);
    EXPECT_EQ(fd, -1);
    // Nothing should have been written through the symlink.
    EXPECT_EQ(ReadFile(realTarget), "do-not-touch");

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove(realTarget);
}

TEST(OpenForRootWrite, RejectsWhenParentDirectoryIsASymlink) {
    std::string baseDir = TempDirPath("parent_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string realDir = baseDir + "_realdir";
    std::filesystem::remove_all(realDir);
    ASSERT_TRUE(std::filesystem::create_directories(realDir));
    std::string symlinkedSubdir = baseDir + "/subdir";
    ASSERT_EQ(symlink(realDir.c_str(), symlinkedSubdir.c_str()), 0);
    std::string filePath = symlinkedSubdir + "/indexed.idx";

    int fd = -1;
    ElevationError err =
        OpenForRootWrite(filePath, getuid(), baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd);

    EXPECT_EQ(err, ElevationError::kSymlinkInPath);
    EXPECT_EQ(fd, -1);
    EXPECT_FALSE(std::filesystem::exists(realDir + "/indexed.idx"));

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove_all(realDir);
}

TEST(OpenForRootWrite, RejectsWhenDirectoryComponentOwnedByDifferentUid) {
    std::string baseDir = TempDirPath("ownership_mismatch");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";

    // baseDir is genuinely owned by our own real uid; deliberately claim a
    // *different* target uid so the ownership check has a real mismatch to
    // catch (no need for actual root to construct this).
    uid_t wrongUid = getuid() + 1;

    int fd = -1;
    ElevationError err =
        OpenForRootWrite(filePath, wrongUid, baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd);

    EXPECT_EQ(err, ElevationError::kOwnershipMismatch);
    EXPECT_EQ(fd, -1);
    EXPECT_FALSE(std::filesystem::exists(filePath));

    std::filesystem::remove_all(baseDir);
}

TEST(OpenForRootWrite, SucceedsCreatingNewFileWhenParentDirPassesButFileMissing) {
    std::string baseDir = TempDirPath("create_new_file");
    std::string nestedDir = baseDir + "/nested";
    ASSERT_TRUE(std::filesystem::create_directories(nestedDir));
    std::string filePath = nestedDir + "/status.txt";
    ASSERT_FALSE(std::filesystem::exists(filePath));

    int fd = -1;
    ElevationError err =
        OpenForRootWrite(filePath, getuid(), baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd);

    EXPECT_EQ(err, ElevationError::kNone);
    ASSERT_GE(fd, 0);
    close(fd);
    EXPECT_TRUE(std::filesystem::exists(filePath));

    std::filesystem::remove_all(baseDir);
}

TEST(OpenForRootWrite, RejectsWhenPathIsNotUnderBaseDir) {
    std::string baseDir = TempDirPath("path_not_under_base");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string outsidePath = TempDirPath("outside_target") + "/indexed.idx";

    int fd = -1;
    ElevationError err =
        OpenForRootWrite(outsidePath, getuid(), baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd);

    EXPECT_EQ(err, ElevationError::kPathNotUnderBase);
    EXPECT_EQ(fd, -1);

    std::filesystem::remove_all(baseDir);
}
