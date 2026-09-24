#include <fcntl.h>
#include <gtest/gtest.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/Elevation.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using indexed::ElevationError;
using indexed::OpenForRootWrite;
using indexed::OpenRegularFileForRootRead;
using indexed::RemoveFileForRootWrite;
using indexed::ReplaceFileForRootWrite;
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

// ---------------------------------------------------------------------
// ReplaceFileForRootWrite
// ---------------------------------------------------------------------

TEST(ReplaceFileForRootWrite, ReplacesContentAndLeavesNoTempFile) {
    std::string baseDir = TempDirPath("replace_ok");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";
    {
        std::ofstream existing(filePath);
        existing << "old";
    }

    EXPECT_EQ(ReplaceFileForRootWrite(filePath, getuid(), getgid(), baseDir, "new-contents"),
              ElevationError::kNone);

    EXPECT_EQ(ReadFile(filePath), "new-contents");
    EXPECT_FALSE(std::filesystem::exists(filePath + ".root.tmp"));

    std::filesystem::remove_all(baseDir);
}

TEST(ReplaceFileForRootWrite, ResultIsOwnedByTargetUidWithMode0600) {
    std::string baseDir = TempDirPath("replace_owner");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";

    ASSERT_EQ(ReplaceFileForRootWrite(filePath, getuid(), getgid(), baseDir, "x"),
              ElevationError::kNone);

    struct stat st{};
    ASSERT_EQ(lstat(filePath.c_str(), &st), 0);
    EXPECT_TRUE(S_ISREG(st.st_mode));
    EXPECT_EQ(st.st_uid, getuid());
    EXPECT_EQ(st.st_gid, getgid());
    EXPECT_EQ(st.st_mode & 07777, 0600u);

    std::filesystem::remove_all(baseDir);
}

TEST(ReplaceFileForRootWrite, RejectsWhenParentDirectoryIsASymlink) {
    std::string baseDir = TempDirPath("replace_parent_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string realDir = baseDir + "_realdir";
    std::filesystem::remove_all(realDir);
    ASSERT_TRUE(std::filesystem::create_directories(realDir));
    std::string symlinkedSubdir = baseDir + "/subdir";
    ASSERT_EQ(symlink(realDir.c_str(), symlinkedSubdir.c_str()), 0);

    EXPECT_EQ(ReplaceFileForRootWrite(symlinkedSubdir + "/indexed.idx", getuid(), getgid(), baseDir,
                                      "payload"),
              ElevationError::kSymlinkInPath);

    EXPECT_TRUE(std::filesystem::is_empty(realDir));

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove_all(realDir);
}

TEST(ReplaceFileForRootWrite, RejectsWhenDirectoryOwnedByDifferentUid) {
    std::string baseDir = TempDirPath("replace_ownership_mismatch");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";

    EXPECT_EQ(ReplaceFileForRootWrite(filePath, getuid() + 1, getgid(), baseDir, "payload"),
              ElevationError::kOwnershipMismatch);

    EXPECT_TRUE(std::filesystem::is_empty(baseDir));

    std::filesystem::remove_all(baseDir);
}

TEST(ReplaceFileForRootWrite, NeverFollowsAPlantedTempSymlink) {
    std::string baseDir = TempDirPath("replace_temp_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string victim = TempDirPath("replace_temp_symlink_victim");
    {
        std::ofstream out(victim);
        out << "do-not-touch";
    }
    std::string filePath = baseDir + "/indexed.idx";
    ASSERT_EQ(symlink(victim.c_str(), (filePath + ".root.tmp").c_str()), 0);

    EXPECT_EQ(ReplaceFileForRootWrite(filePath, getuid(), getgid(), baseDir, "payload"),
              ElevationError::kNone);

    EXPECT_EQ(ReadFile(victim), "do-not-touch");
    EXPECT_EQ(ReadFile(filePath), "payload");
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(filePath + ".root.tmp")));

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove(victim);
}

TEST(ReplaceFileForRootWrite, ReplacesASymlinkAtTheFinalPathWithoutFollowingIt) {
    std::string baseDir = TempDirPath("replace_final_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string victim = TempDirPath("replace_final_symlink_victim");
    {
        std::ofstream out(victim);
        out << "do-not-touch";
    }
    std::string filePath = baseDir + "/indexed.idx";
    ASSERT_EQ(symlink(victim.c_str(), filePath.c_str()), 0);

    EXPECT_EQ(ReplaceFileForRootWrite(filePath, getuid(), getgid(), baseDir, "payload"),
              ElevationError::kNone);

    EXPECT_EQ(ReadFile(victim), "do-not-touch");
    EXPECT_FALSE(std::filesystem::is_symlink(filePath));
    EXPECT_EQ(ReadFile(filePath), "payload");

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove(victim);
}

// The unprivileged GUI's IndexSerializer::Save uses "<name>.tmp"; the helper
// must never unlink or rename a save the GUI has in flight.
TEST(ReplaceFileForRootWrite, LeavesTheUnprivilegedSavesTempFileAlone) {
    std::string baseDir = TempDirPath("replace_gui_temp");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";
    {
        std::ofstream guiTemp(filePath + ".tmp");
        guiTemp << "gui-in-flight";
    }

    ASSERT_EQ(ReplaceFileForRootWrite(filePath, getuid(), getgid(), baseDir, "helper"),
              ElevationError::kNone);

    EXPECT_EQ(ReadFile(filePath + ".tmp"), "gui-in-flight");
    EXPECT_EQ(ReadFile(filePath), "helper");

    std::filesystem::remove_all(baseDir);
}

// ---------------------------------------------------------------------
// OpenRegularFileForRootRead
// ---------------------------------------------------------------------

TEST(OpenRegularFileForRootRead, OpensAnOwnedRegularFile) {
    std::string baseDir = TempDirPath("read_ok");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";
    {
        std::ofstream out(filePath);
        out << "index-bytes";
    }

    int fd = -1;
    ASSERT_EQ(OpenRegularFileForRootRead(filePath, getuid(), baseDir, &fd), ElevationError::kNone);
    ASSERT_GE(fd, 0);
    char buffer[16] = {};
    EXPECT_EQ(read(fd, buffer, sizeof(buffer)), 11);
    EXPECT_EQ(std::string(buffer, 11), "index-bytes");
    close(fd);

    std::filesystem::remove_all(baseDir);
}

TEST(OpenRegularFileForRootRead, RejectsAFifoWithoutBlocking) {
    std::string baseDir = TempDirPath("read_fifo");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.idx";
    ASSERT_EQ(mkfifo(filePath.c_str(), 0600), 0);

    int fd = -1;
    EXPECT_EQ(OpenRegularFileForRootRead(filePath, getuid(), baseDir, &fd),
              ElevationError::kNotRegularFile);
    EXPECT_EQ(fd, -1);

    std::filesystem::remove_all(baseDir);
}

TEST(OpenRegularFileForRootRead, RejectsASymlinkToAnotherFile) {
    std::string baseDir = TempDirPath("read_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string victim = TempDirPath("read_symlink_victim");
    {
        std::ofstream out(victim);
        out << "someone-elses-index";
    }
    std::string filePath = baseDir + "/indexed.idx";
    ASSERT_EQ(symlink(victim.c_str(), filePath.c_str()), 0);

    int fd = -1;
    EXPECT_EQ(OpenRegularFileForRootRead(filePath, getuid(), baseDir, &fd),
              ElevationError::kSymlinkInPath);
    EXPECT_EQ(fd, -1);

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove(victim);
}

// A FIFO planted where the log or status file should be must be refused
// before it can block the helper. O_RDWR is used here only because it never
// blocks on a FIFO, so a regression shows up as a failed assertion rather
// than a hung test; the fix must reject the FIFO for any flags.
TEST(OpenForRootWrite, RejectsAFifo) {
    std::string baseDir = TempDirPath("fifo");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.status";
    ASSERT_EQ(mkfifo(filePath.c_str(), 0600), 0);

    int fd = -1;
    EXPECT_NE(OpenForRootWrite(filePath, getuid(), baseDir, O_RDWR | O_CREAT, 0600, &fd),
              ElevationError::kNone);
    EXPECT_EQ(fd, -1);

    std::filesystem::remove_all(baseDir);
}

// With fs.protected_hardlinks=0 a user can hard-link a file they can't write
// into their own directory; root must not truncate or write through it.
TEST(OpenForRootWrite, RejectsAHardLinkedFileWithoutTruncatingIt) {
    std::string baseDir = TempDirPath("hardlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string original = baseDir + "/precious";
    {
        std::ofstream out(original);
        out << "do-not-truncate";
    }
    std::string filePath = baseDir + "/indexed.status";
    ASSERT_EQ(link(original.c_str(), filePath.c_str()), 0);

    int fd = -1;
    EXPECT_EQ(
        OpenForRootWrite(filePath, getuid(), baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd),
        ElevationError::kNotRegularFile);
    EXPECT_EQ(fd, -1);
    EXPECT_EQ(ReadFile(original), "do-not-truncate");

    std::filesystem::remove_all(baseDir);
}

TEST(OpenForRootWrite, StillTruncatesAnAcceptedFileWhenAsked) {
    std::string baseDir = TempDirPath("truncate_ok");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.status";
    {
        std::ofstream out(filePath);
        out << "old status";
    }

    int fd = -1;
    ASSERT_EQ(
        OpenForRootWrite(filePath, getuid(), baseDir, O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd),
        ElevationError::kNone);
    EXPECT_EQ(write(fd, "new", 3), 3);
    close(fd);
    EXPECT_EQ(ReadFile(filePath), "new");

    std::filesystem::remove_all(baseDir);
}

TEST(OpenForRootWrite, RejectsWhenBaseDirIsASymlink) {
    std::string realDir = TempDirPath("base_symlink_real");
    ASSERT_TRUE(std::filesystem::create_directories(realDir));
    std::string baseDir = TempDirPath("base_symlink");
    ASSERT_EQ(symlink(realDir.c_str(), baseDir.c_str()), 0);

    int fd = -1;
    EXPECT_EQ(OpenForRootWrite(baseDir + "/indexed.status", getuid(), baseDir,
                               O_WRONLY | O_CREAT | O_TRUNC, 0600, &fd),
              ElevationError::kSymlinkInPath);
    EXPECT_EQ(fd, -1);
    EXPECT_TRUE(std::filesystem::is_empty(realDir));

    std::filesystem::remove(baseDir);
    std::filesystem::remove_all(realDir);
}

// ---------------------------------------------------------------------
// RemoveFileForRootWrite
// ---------------------------------------------------------------------

TEST(RemoveFileForRootWrite, RemovesAFileInsideBaseDir) {
    std::string baseDir = TempDirPath("remove_ok");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string filePath = baseDir + "/indexed.log";
    std::ofstream(filePath) << "old root-owned log";

    EXPECT_EQ(RemoveFileForRootWrite(filePath, getuid(), baseDir), ElevationError::kNone);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(filePath)));

    std::filesystem::remove_all(baseDir);
}

TEST(RemoveFileForRootWrite, RemovesASymlinkEntryWithoutTouchingItsTarget) {
    std::string baseDir = TempDirPath("remove_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string victim = TempDirPath("remove_symlink_victim");
    std::ofstream(victim) << "do-not-touch";
    std::string filePath = baseDir + "/indexed.log";
    ASSERT_EQ(symlink(victim.c_str(), filePath.c_str()), 0);

    EXPECT_EQ(RemoveFileForRootWrite(filePath, getuid(), baseDir), ElevationError::kNone);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(filePath)));
    EXPECT_EQ(ReadFile(victim), "do-not-touch");

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove(victim);
}

TEST(RemoveFileForRootWrite, RefusesWhenAParentDirectoryIsASymlink) {
    std::string baseDir = TempDirPath("remove_parent_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(baseDir));
    std::string realDir = baseDir + "_realdir";
    std::filesystem::remove_all(realDir);
    ASSERT_TRUE(std::filesystem::create_directories(realDir));
    std::ofstream(realDir + "/indexed.log") << "keep";
    ASSERT_EQ(symlink(realDir.c_str(), (baseDir + "/subdir").c_str()), 0);

    EXPECT_EQ(RemoveFileForRootWrite(baseDir + "/subdir/indexed.log", getuid(), baseDir),
              ElevationError::kSymlinkInPath);
    EXPECT_EQ(ReadFile(realDir + "/indexed.log"), "keep");

    std::filesystem::remove_all(baseDir);
    std::filesystem::remove_all(realDir);
}
