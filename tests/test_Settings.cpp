#include <gtest/gtest.h>

#include "settings/Settings.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

using indexed::Settings;

namespace {

std::string TempFilePath(const std::string& name) {
    return ::testing::TempDir() + "indexed_test_settings_" + name + ".conf";
}

bool Contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

TEST(Settings, MissingFileLoadsAllDefaults) {
    Settings settings(TempFilePath("does_not_exist_xyz"), "/home/testuser");
    ASSERT_TRUE(settings.Load());

    EXPECT_TRUE(settings.SelectedRoots().empty());
    EXPECT_EQ(settings.ReindexIntervalHours(), 48);
    EXPECT_FALSE(settings.UseRegex());
    EXPECT_FALSE(settings.CaseSensitive());
    EXPECT_FALSE(settings.WholeWord());
    EXPECT_FALSE(settings.MatchPath());
    EXPECT_FALSE(settings.IgnoreDiacritics());
    EXPECT_FALSE(settings.FirstRunComplete());

    // Fresh install -> ExcludedPaths materializes from the default list.
    EXPECT_EQ(settings.ExcludedPaths(), Settings::DefaultExcludedPaths("/home/testuser"));
}

TEST(Settings, DefaultExcludedPathsContainsExpectedSystemNoiseAndExpandsHome) {
    std::vector<std::string> defaults = Settings::DefaultExcludedPaths("/home/testuser");

    EXPECT_TRUE(Contains(defaults, "/proc"));
    EXPECT_TRUE(Contains(defaults, "/sys"));
    EXPECT_TRUE(Contains(defaults, "/dev"));
    EXPECT_TRUE(Contains(defaults, "/run"));
    EXPECT_TRUE(Contains(defaults, "/tmp"));
    EXPECT_TRUE(Contains(defaults, "/var/cache"));
    EXPECT_TRUE(Contains(defaults, "/var/tmp"));
    EXPECT_TRUE(Contains(defaults, "/var/lib/docker"));
    EXPECT_TRUE(Contains(defaults, "/var/lib/containers"));
    EXPECT_TRUE(Contains(defaults, "/snap"));
    EXPECT_TRUE(Contains(defaults, "/var/lib/flatpak"));
    EXPECT_TRUE(Contains(defaults, "/lost+found"));

    // `~` expanded against the supplied home directory.
    EXPECT_TRUE(Contains(defaults, "/home/testuser/.cache"));
    EXPECT_TRUE(Contains(defaults, "/home/testuser/.local/share/Trash"));
    EXPECT_TRUE(Contains(defaults, "/home/testuser/.local/share/containers"));
    EXPECT_TRUE(Contains(defaults, "/home/testuser/.var/app"));

    // Must NOT broadly exclude general user directories.
    EXPECT_FALSE(Contains(defaults, "/home/testuser/.config"));
    EXPECT_FALSE(Contains(defaults, "/home/testuser/.local/share"));
}

TEST(Settings, SaveThenLoadRoundTripsAllFields) {
    const std::string path = TempFilePath("roundtrip");
    std::filesystem::remove(path);

    {
        Settings settings(path, "/home/testuser");
        ASSERT_TRUE(settings.Load());
        settings.SetSelectedRoots({"/home/testuser/Documents", "/mnt/data"});
        settings.SetExcludedPaths({"/proc", "/sys"});
        settings.SetReindexIntervalHours(0);
        settings.SetUseRegex(true);
        settings.SetCaseSensitive(true);
        settings.SetWholeWord(true);
        settings.SetMatchPath(true);
        settings.SetIgnoreDiacritics(true);
        settings.SetFirstRunComplete(true);
        ASSERT_TRUE(settings.Save());
    }
    {
        Settings settings(path, "/home/testuser");
        ASSERT_TRUE(settings.Load());
        EXPECT_EQ(settings.SelectedRoots(),
                  (std::vector<std::string>{"/home/testuser/Documents", "/mnt/data"}));
        EXPECT_EQ(settings.ExcludedPaths(), (std::vector<std::string>{"/proc", "/sys"}));
        EXPECT_EQ(settings.ReindexIntervalHours(), 0);
        EXPECT_TRUE(settings.UseRegex());
        EXPECT_TRUE(settings.CaseSensitive());
        EXPECT_TRUE(settings.WholeWord());
        EXPECT_TRUE(settings.MatchPath());
        EXPECT_TRUE(settings.IgnoreDiacritics());
        EXPECT_TRUE(settings.FirstRunComplete());
    }

    std::filesystem::remove(path);
}

TEST(Settings, SavedEmptyExcludedPathsIsNotReplacedByDefaultsOnNextLoad) {
    const std::string path = TempFilePath("saved_empty_excludes");
    std::filesystem::remove(path);

    {
        Settings settings(path, "/home/testuser");
        ASSERT_TRUE(settings.Load());
        settings.SetExcludedPaths({});  // user deliberately clears the list
        ASSERT_TRUE(settings.Save());
    }
    {
        Settings settings(path, "/home/testuser");
        ASSERT_TRUE(settings.Load());
        // Must stay empty -- defaults are only injected when the key was
        // never saved, not whenever the saved list happens to be empty.
        EXPECT_TRUE(settings.ExcludedPaths().empty());
    }

    std::filesystem::remove(path);
}

TEST(Settings, SaveRejectsSelectedRootContainingLiteralNewline) {
    const std::string path = TempFilePath("reject_newline_root");
    std::filesystem::remove(path);

    Settings settings(path, "/home/testuser");
    ASSERT_TRUE(settings.Load());
    settings.SetSelectedRoots({"/home/testuser/bad\nname"});

    EXPECT_FALSE(settings.Save());
    EXPECT_FALSE(settings.LastError().empty());
    // Rejected save must not have written a file at all.
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(Settings, SaveRejectsExcludedPathContainingLiteralNewline) {
    const std::string path = TempFilePath("reject_newline_excluded");
    std::filesystem::remove(path);

    Settings settings(path, "/home/testuser");
    ASSERT_TRUE(settings.Load());
    settings.SetExcludedPaths({"/valid", "/bad\npath"});

    EXPECT_FALSE(settings.Save());
    EXPECT_FALSE(settings.LastError().empty());
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(Settings, MultipleRootsAndExcludedPathsSurviveNewlineJoinRoundTrip) {
    const std::string path = TempFilePath("multi_roundtrip");
    std::filesystem::remove(path);

    std::vector<std::string> roots = {"/a", "/b/c", "/d e f"};
    std::vector<std::string> excludes = {"/proc", "/sys", "/home/testuser/.cache"};

    {
        Settings settings(path, "/home/testuser");
        ASSERT_TRUE(settings.Load());
        settings.SetSelectedRoots(roots);
        settings.SetExcludedPaths(excludes);
        ASSERT_TRUE(settings.Save());
    }
    {
        Settings settings(path, "/home/testuser");
        ASSERT_TRUE(settings.Load());
        EXPECT_EQ(settings.SelectedRoots(), roots);
        EXPECT_EQ(settings.ExcludedPaths(), excludes);
    }

    std::filesystem::remove(path);
}
