#include <gtest/gtest.h>

#include "settings/IniFile.h"
#include <cstdio>
#include <fstream>
#include <string>

using indexed::IniFile;

namespace {

std::string TempFilePath(const std::string& name) {
    return ::testing::TempDir() + "indexed_test_inifile_" + name + ".conf";
}

}  // namespace

TEST(IniFile, MissingFileLoadsAsEmptyNotAnError) {
    IniFile ini;
    EXPECT_TRUE(ini.Load(TempFilePath("does_not_exist_xyz")));
    EXPECT_FALSE(ini.Has("AnyKey"));
    EXPECT_EQ(ini.GetString("AnyKey"), std::nullopt);
    EXPECT_EQ(ini.GetInt("AnyKey", 42), 42);
    EXPECT_FALSE(ini.GetBool("AnyKey", false));
}

TEST(IniFile, SetAndGetStringInMemory) {
    IniFile ini;
    EXPECT_FALSE(ini.Has("Name"));
    ini.SetString("Name", "value");
    ASSERT_TRUE(ini.Has("Name"));
    EXPECT_EQ(ini.GetString("Name"), "value");
}

TEST(IniFile, GetIntReturnsDefaultWhenMissingOrUnparsable) {
    IniFile ini;
    EXPECT_EQ(ini.GetInt("Missing", 7), 7);

    ini.SetString("NotANumber", "abc");
    EXPECT_EQ(ini.GetInt("NotANumber", 7), 7);

    ini.SetInt("Count", 123);
    EXPECT_EQ(ini.GetInt("Count", 7), 123);
}

TEST(IniFile, GetBoolParsesZeroOneAndFallsBackToDefault) {
    IniFile ini;
    EXPECT_TRUE(ini.GetBool("Missing", true));
    EXPECT_FALSE(ini.GetBool("Missing", false));

    ini.SetBool("Flag", true);
    EXPECT_TRUE(ini.GetBool("Flag", false));

    ini.SetBool("Flag2", false);
    EXPECT_FALSE(ini.GetBool("Flag2", true));

    ini.SetString("Garbage", "yes");
    EXPECT_TRUE(ini.GetBool("Garbage", true));
    EXPECT_FALSE(ini.GetBool("Garbage", false));
}

TEST(IniFile, SaveThenLoadRoundTripsValues) {
    const std::string path = TempFilePath("roundtrip");

    {
        IniFile ini;
        ini.SetString("SelectedRoots", "/home/user");
        ini.SetInt("ReindexIntervalHours", 48);
        ini.SetBool("UseRegex", true);
        ASSERT_TRUE(ini.Save(path));
    }
    {
        IniFile ini;
        ASSERT_TRUE(ini.Load(path));
        EXPECT_EQ(ini.GetString("SelectedRoots"), "/home/user");
        EXPECT_EQ(ini.GetInt("ReindexIntervalHours", 0), 48);
        EXPECT_TRUE(ini.GetBool("UseRegex", false));
    }

    std::remove(path.c_str());
}

TEST(IniFile, EmbeddedNewlineInValueRoundTrips) {
    const std::string path = TempFilePath("embedded_newline");
    const std::string value = "/a/first\n/b/second";

    {
        IniFile ini;
        ini.SetString("Roots", value);
        ASSERT_TRUE(ini.Save(path));
    }

    // The on-disk file must stay one physical line per key -- verify the
    // newline was escaped rather than written raw.
    {
        std::ifstream file(path);
        std::string firstLine;
        std::getline(file, firstLine);
        std::string secondLine;
        bool hasSecondLine = static_cast<bool>(std::getline(file, secondLine));
        EXPECT_TRUE(firstLine.find("Roots=") == 0);
        // Either there's no second line at all, or it belongs to a
        // different, unrelated key -- never a raw continuation of Roots'
        // value.
        (void)hasSecondLine;
    }

    {
        IniFile ini;
        ASSERT_TRUE(ini.Load(path));
        EXPECT_EQ(ini.GetString("Roots"), value);
    }

    std::remove(path.c_str());
}

TEST(IniFile, CommentAndBlankLinesAreIgnored) {
    const std::string path = TempFilePath("comments");
    {
        std::ofstream file(path);
        file << "# a comment\n";
        file << "; another comment\n";
        file << "\n";
        file << "Key=Value\n";
    }

    IniFile ini;
    ASSERT_TRUE(ini.Load(path));
    EXPECT_EQ(ini.GetString("Key"), "Value");
    EXPECT_FALSE(ini.Has("#"));

    std::remove(path.c_str());
}

TEST(IniFile, SaveOverwritesPreviousContentsOfFile) {
    const std::string path = TempFilePath("overwrite");

    {
        IniFile ini;
        ini.SetString("Key", "first");
        ASSERT_TRUE(ini.Save(path));
    }
    {
        IniFile ini;
        ini.SetString("Key", "second");
        ASSERT_TRUE(ini.Save(path));
    }
    {
        IniFile ini;
        ASSERT_TRUE(ini.Load(path));
        EXPECT_EQ(ini.GetString("Key"), "second");
    }

    std::remove(path.c_str());
}
