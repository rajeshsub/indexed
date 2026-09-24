#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings/HelperSettings.h"
#include <filesystem>
#include <fstream>
#include <string>

using indexed::ElevationError;
using indexed::LoadSettingsForRoot;
using indexed::LogLevel;
using indexed::Settings;
using indexed::TargetUser;

namespace {

std::string TempDirPath(const std::string& name) {
    std::string path = ::testing::TempDir() + "indexed_test_helpersettings_" + name + "_" +
                       std::to_string(static_cast<long>(getpid()));
    std::filesystem::remove_all(path);
    return path;
}

TargetUser SelfAsTargetUser() {
    TargetUser user;
    user.uid = getuid();
    user.gid = getgid();
    user.homeDir = TempDirPath("home");
    user.username = "test";
    return user;
}

}  // namespace

// LoadSettingsForRoot is what the root helper uses instead of Settings::Load
// on a plain path (docs/adr/0008): it must go through the same fd-relative,
// symlink-and-FIFO-refusing validation as every other privileged read.

TEST(LoadSettingsForRoot, LoadsRealSettingsThroughTheValidatedFd) {
    TargetUser user = SelfAsTargetUser();
    std::filesystem::create_directories(user.homeDir);
    const std::string configDir = TempDirPath("config_ok");
    ASSERT_TRUE(std::filesystem::create_directories(configDir));
    const std::string configPath = configDir + "/indexed.conf";

    Settings written(configPath, user.homeDir);
    written.SetSelectedRoots({"/media/veracrypt10"});
    written.SetReindexIntervalHours(12);
    written.SetLogLevel(LogLevel::Debug);
    ASSERT_TRUE(written.Save());

    Settings loaded(configPath, user.homeDir);
    const ElevationError result = LoadSettingsForRoot(configPath, user, loaded);

    EXPECT_EQ(result, ElevationError::kNone);
    EXPECT_EQ(loaded.SelectedRoots(), written.SelectedRoots());
    EXPECT_EQ(loaded.ReindexIntervalHours(), 12);
    EXPECT_EQ(loaded.LogLevel(), LogLevel::Debug);

    std::filesystem::remove_all(configDir);
    std::filesystem::remove_all(user.homeDir);
}

// A missing config is the ordinary "never saved yet" case, reported as
// kOpenFailed; the caller (the helper) is expected to keep whatever
// defaults or previous settings it already had in `out`.
TEST(LoadSettingsForRoot, ReportsKOpenFailedAndLeavesOutUntouchedWhenConfigIsMissing) {
    TargetUser user = SelfAsTargetUser();
    const std::string configDir = TempDirPath("config_missing");
    ASSERT_TRUE(std::filesystem::create_directories(configDir));
    const std::string configPath = configDir + "/indexed.conf";  // never created

    Settings out(configPath, user.homeDir);
    out.SetSelectedRoots({"/previous/roots"});  // simulates "the settings we already had"

    const ElevationError result = LoadSettingsForRoot(configPath, user, out);

    EXPECT_EQ(result, ElevationError::kOpenFailed);
    EXPECT_EQ(out.SelectedRoots(), std::vector<std::string>{"/previous/roots"});

    std::filesystem::remove_all(configDir);
}

// A FIFO planted where indexed.conf should be must be refused, not opened
// (it would hang the helper); the existing settings must not be touched.
TEST(LoadSettingsForRoot, RefusesAFifoAndLeavesOutUntouched) {
    TargetUser user = SelfAsTargetUser();
    const std::string configDir = TempDirPath("config_fifo");
    ASSERT_TRUE(std::filesystem::create_directories(configDir));
    const std::string configPath = configDir + "/indexed.conf";
    ASSERT_EQ(mkfifo(configPath.c_str(), 0600), 0);

    Settings out(configPath, user.homeDir);
    out.SetSelectedRoots({"/previous/roots"});

    const ElevationError result = LoadSettingsForRoot(configPath, user, out);

    EXPECT_EQ(result, ElevationError::kNotRegularFile);
    EXPECT_EQ(out.SelectedRoots(), std::vector<std::string>{"/previous/roots"});

    std::filesystem::remove_all(configDir);
}

// A symlink swapped in for indexed.conf, pointing at a file the caller
// doesn't own, must be refused rather than followed.
TEST(LoadSettingsForRoot, RefusesASymlinkToAnotherFile) {
    TargetUser user = SelfAsTargetUser();
    const std::string configDir = TempDirPath("config_symlink");
    ASSERT_TRUE(std::filesystem::create_directories(configDir));
    const std::string victim = TempDirPath("config_symlink_victim");
    {
        std::ofstream out(victim);
        out << "[General]\nSelectedRoots=/etc\n";
    }
    const std::string configPath = configDir + "/indexed.conf";
    ASSERT_EQ(symlink(victim.c_str(), configPath.c_str()), 0);

    Settings out(configPath, user.homeDir);
    out.SetSelectedRoots({"/previous/roots"});

    const ElevationError result = LoadSettingsForRoot(configPath, user, out);

    EXPECT_EQ(result, ElevationError::kSymlinkInPath);
    EXPECT_EQ(out.SelectedRoots(), std::vector<std::string>{"/previous/roots"});

    std::filesystem::remove_all(configDir);
    std::filesystem::remove(victim);
}
