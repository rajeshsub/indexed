#include <gtest/gtest.h>
#include <unistd.h>

#include "settings/Logger.h"
#include "settings/PathUtils.h"
#include <filesystem>
#include <fstream>
#include <string>

using indexed::CanonicalizeBestEffort;
using indexed::CollapseRedundantRoots;
using indexed::DataDirs;
using indexed::EnsureDirectory;
using indexed::ExecutableDir;
using indexed::FormatAge;
using indexed::FormatFileCount;
using indexed::FormatLocationList;
using indexed::IsPortableMode;
using indexed::Logger;
using indexed::LogLevel;
using indexed::ResolveDataDirs;
using indexed::XdgEnv;

namespace {

std::string TempDirPath(const std::string& name) {
    std::string path = ::testing::TempDir() + "indexed_test_pathutils_" + name + "_" +
                       std::to_string(static_cast<long>(getpid()));
    std::filesystem::remove_all(path);
    return path;
}

}  // namespace

TEST(PathUtils, ExecutableDirResolvesToARealDirectory) {
    std::string dir = ExecutableDir();
    ASSERT_FALSE(dir.empty());
    EXPECT_TRUE(std::filesystem::is_directory(dir));
}

TEST(PathUtils, IsPortableModeFalseWhenNoConfNextToExecutable) {
    std::string dir = TempDirPath("not_portable");
    ASSERT_TRUE(std::filesystem::create_directories(dir));

    EXPECT_FALSE(IsPortableMode(dir));

    std::filesystem::remove_all(dir);
}

TEST(PathUtils, IsPortableModeTrueWhenConfSitsNextToExecutable) {
    std::string dir = TempDirPath("portable");
    ASSERT_TRUE(std::filesystem::create_directories(dir));
    {
        std::ofstream conf(dir + "/indexed.conf");
        conf << "FirstRunComplete=1\n";
    }

    EXPECT_TRUE(IsPortableMode(dir));

    std::filesystem::remove_all(dir);
}

TEST(PathUtils, ResolveDataDirsUsesXdgDefaultsUnderHomeWhenNotPortable) {
    std::string dir = TempDirPath("xdg_defaults");
    ASSERT_TRUE(std::filesystem::create_directories(dir));

    XdgEnv xdg;
    xdg.home = "/home/testuser";
    // All XDG_* overrides unset -> fall back to defaults under home.
    DataDirs dirs = ResolveDataDirs(dir, xdg);

    EXPECT_EQ(dirs.configPath, "/home/testuser/.config/indexed/indexed.conf");
    EXPECT_EQ(dirs.indexPath, "/home/testuser/.cache/indexed/indexed.idx");
    EXPECT_EQ(dirs.logPath, "/home/testuser/.local/state/indexed/indexed.log");

    std::filesystem::remove_all(dir);
}

TEST(PathUtils, ResolveDataDirsHonorsXdgOverrides) {
    std::string dir = TempDirPath("xdg_overrides");
    ASSERT_TRUE(std::filesystem::create_directories(dir));

    XdgEnv xdg;
    xdg.home = "/home/testuser";
    xdg.xdgConfigHome = "/custom/config";
    xdg.xdgCacheHome = "/custom/cache";
    xdg.xdgStateHome = "/custom/state";

    DataDirs dirs = ResolveDataDirs(dir, xdg);

    EXPECT_EQ(dirs.configPath, "/custom/config/indexed/indexed.conf");
    EXPECT_EQ(dirs.indexPath, "/custom/cache/indexed/indexed.idx");
    EXPECT_EQ(dirs.logPath, "/custom/state/indexed/indexed.log");

    std::filesystem::remove_all(dir);
}

TEST(PathUtils, ResolveDataDirsUsesExecutableDirWhenPortable) {
    std::string dir = TempDirPath("portable_resolve");
    ASSERT_TRUE(std::filesystem::create_directories(dir));
    {
        std::ofstream conf(dir + "/indexed.conf");
        conf << "FirstRunComplete=1\n";
    }

    XdgEnv xdg;
    xdg.home = "/home/testuser";
    DataDirs dirs = ResolveDataDirs(dir, xdg);

    EXPECT_EQ(dirs.configPath, dir + "/indexed.conf");
    EXPECT_EQ(dirs.indexPath, dir + "/indexed.idx");
    EXPECT_EQ(dirs.logPath, dir + "/indexed.log");

    std::filesystem::remove_all(dir);
}

TEST(PathUtils, EnsureDirectoryCreatesNestedMissingDirectories) {
    std::string base = TempDirPath("ensure_dir");
    std::string nested = base + "/a/b/c";

    ASSERT_FALSE(std::filesystem::exists(base));
    EXPECT_TRUE(EnsureDirectory(nested));
    EXPECT_TRUE(std::filesystem::is_directory(nested));

    // Idempotent: calling again on an already-existing directory succeeds.
    EXPECT_TRUE(EnsureDirectory(nested));

    std::filesystem::remove_all(base);
}

TEST(PathUtils, EnsureDirectoryFailsWhenPathIsAnExistingFile) {
    std::string base = TempDirPath("ensure_dir_file_conflict");
    ASSERT_TRUE(std::filesystem::create_directories(base));
    std::string filePath = base + "/blocked";
    {
        std::ofstream file(filePath);
        file << "occupied\n";
    }

    EXPECT_FALSE(EnsureDirectory(filePath));

    std::filesystem::remove_all(base);
}

TEST(PathUtils, FormatFileCountAddsThousandsSeparators) {
    EXPECT_EQ(FormatFileCount(0), "0");
    EXPECT_EQ(FormatFileCount(9), "9");
    EXPECT_EQ(FormatFileCount(999), "999");
    EXPECT_EQ(FormatFileCount(1000), "1,000");
    EXPECT_EQ(FormatFileCount(1234567), "1,234,567");
    EXPECT_EQ(FormatFileCount(999999999), "999,999,999");
}

TEST(PathUtils, FormatAgeBoundaries) {
    EXPECT_EQ(FormatAge(0), "just indexed");
    EXPECT_EQ(FormatAge(59), "just indexed");
    EXPECT_EQ(FormatAge(60), "1 min old");
    EXPECT_EQ(FormatAge(119), "1 min old");
    EXPECT_EQ(FormatAge(3599), "59 min old");
    EXPECT_EQ(FormatAge(3600), "1 hrs old");
    EXPECT_EQ(FormatAge(86399), "23 hrs old");
    EXPECT_EQ(FormatAge(86400), "1 days, 0 hrs old");
    EXPECT_EQ(FormatAge(90000), "1 days, 1 hrs old");
    EXPECT_EQ(FormatAge(2 * 86400 + 3 * 3600), "2 days, 3 hrs old");
}

TEST(PathUtils, FormatLocationListJoinsAndStripsTrailingSlash) {
    EXPECT_EQ(FormatLocationList({}), "");
    EXPECT_EQ(FormatLocationList({"/home/user"}), "/home/user");
    EXPECT_EQ(FormatLocationList({"/home/user/"}), "/home/user");
    EXPECT_EQ(FormatLocationList({"/home/user/", "/mnt/data/"}), "/home/user, /mnt/data");
    // A bare root is left alone rather than stripped to an empty string.
    EXPECT_EQ(FormatLocationList({"/"}), "/");
}

TEST(PathUtils, CollapseRedundantRootsEmptyAndSingle) {
    EXPECT_TRUE(CollapseRedundantRoots({}).empty());
    EXPECT_EQ(CollapseRedundantRoots({"/home/user"}), (std::vector<std::string>{"/home/user"}));
}

TEST(PathUtils, CollapseRedundantRootsDropsExactDuplicates) {
    EXPECT_EQ(CollapseRedundantRoots({"/home/user", "/home/user"}),
              (std::vector<std::string>{"/home/user"}));
}

TEST(PathUtils, CollapseRedundantRootsDropsNestedUnderAncestor) {
    // The reported bug: "/" plus "/home" -> just "/".
    EXPECT_EQ(CollapseRedundantRoots({"/", "/home"}), (std::vector<std::string>{"/"}));
    EXPECT_EQ(CollapseRedundantRoots({"/home", "/"}), (std::vector<std::string>{"/"}));
}

TEST(PathUtils, CollapseRedundantRootsDropsDeeplyNestedEntries) {
    EXPECT_EQ(CollapseRedundantRoots({"/home/user", "/home/user/projects/indexed", "/home/user/a"}),
              (std::vector<std::string>{"/home/user"}));
}

TEST(PathUtils, CollapseRedundantRootsKeepsDisjointSiblings) {
    EXPECT_EQ(CollapseRedundantRoots({"/home/a", "/home/b"}),
              (std::vector<std::string>{"/home/a", "/home/b"}));
}

TEST(PathUtils, CollapseRedundantRootsComparesWholePathComponents) {
    // "/home/user" must NOT absorb "/home/userfoo" -- string prefix is not
    // a path-component prefix.
    EXPECT_EQ(CollapseRedundantRoots({"/home/user", "/home/userfoo"}),
              (std::vector<std::string>{"/home/user", "/home/userfoo"}));
}

TEST(PathUtils, CollapseRedundantRootsSortsResultForStableDisplay) {
    EXPECT_EQ(CollapseRedundantRoots({"/mnt/z", "/mnt/a", "/mnt/m"}),
              (std::vector<std::string>{"/mnt/a", "/mnt/m", "/mnt/z"}));
}

TEST(PathUtils, CollapseRedundantRootsStripsTrailingSlashesBeforeComparing) {
    EXPECT_EQ(CollapseRedundantRoots({"/home/", "/home/user/"}),
              (std::vector<std::string>{"/home"}));
}

TEST(PathUtils, CollapseRedundantRootsResolvesSymlinkedRoots) {
    std::string base = TempDirPath("collapse_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(base + "/real/sub"));
    std::error_code ec;
    std::filesystem::create_directory_symlink(base + "/real", base + "/link", ec);
    ASSERT_FALSE(ec);

    // "<base>/link" resolves to "<base>/real", which is an ancestor of
    // "<base>/real/sub" -> the nested entry is dropped.
    std::vector<std::string> collapsed =
        CollapseRedundantRoots({base + "/link", base + "/real/sub"});
    EXPECT_EQ(collapsed, (std::vector<std::string>{base + "/real"}));

    std::filesystem::remove_all(base);
}

TEST(PathUtils, CollapseRedundantRootsKeepsNonExistentPathsViaTextualFallback) {
    // realpath() fails on a path that doesn't exist; the entry is still kept
    // (trailing-slash-normalized) rather than silently dropped.
    EXPECT_EQ(CollapseRedundantRoots({"/no/such/path/x", "/no/such/path/y"}),
              (std::vector<std::string>{"/no/such/path/x", "/no/such/path/y"}));
}

TEST(PathUtils, CanonicalizeBestEffortStripsTrailingSlashOnMissingPath) {
    EXPECT_EQ(CanonicalizeBestEffort("/no/such/path/"), "/no/such/path");
    EXPECT_EQ(CanonicalizeBestEffort("/"), "/");
}

TEST(Logger, LogAppendsTimestampedLineAndCreatesParentDirectory) {
    std::string dir = TempDirPath("logger");
    std::string logPath = dir + "/nested/indexed.log";
    ASSERT_FALSE(std::filesystem::exists(dir));

    // Debug threshold: the default Log() level (Info) would otherwise be
    // suppressed by the default Warning threshold (docs/adr/0009) -- this
    // test exercises the write mechanics (timestamping, directory creation),
    // not level filtering, which test_Logger.cpp covers separately.
    Logger logger(logPath, LogLevel::Debug);
    ASSERT_TRUE(logger.Log("first message"));
    ASSERT_TRUE(logger.Log("second message"));

    std::ifstream file(logPath);
    ASSERT_TRUE(file.is_open());
    std::string firstLine;
    std::string secondLine;
    std::getline(file, firstLine);
    std::getline(file, secondLine);

    EXPECT_NE(firstLine.find("first message"), std::string::npos);
    EXPECT_NE(secondLine.find("second message"), std::string::npos);
    // Timestamp prefix looks like "[YYYY-MM-DD HH:MM:SS] ".
    EXPECT_EQ(firstLine.front(), '[');
    EXPECT_NE(firstLine.find(']'), std::string::npos);

    std::filesystem::remove_all(dir);
}
