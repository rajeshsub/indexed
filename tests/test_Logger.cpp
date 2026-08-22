#include <gtest/gtest.h>

#include "settings/Logger.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using indexed::Logger;
using indexed::LogLevel;

namespace {

std::string TempFilePath(const std::string& name) {
    std::string path = ::testing::TempDir() + "indexed_test_logger_" + name + ".log";
    std::filesystem::remove(path);
    return path;
}

std::string ReadFile(const std::string& path) {
    std::ifstream file(path);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST(Logger, MessageAtThresholdSeverityIsWritten) {
    std::string path = TempFilePath("at_threshold");
    Logger logger(path, LogLevel::Warning);

    EXPECT_TRUE(logger.Log("careful", LogLevel::Warning));

    std::string contents = ReadFile(path);
    EXPECT_NE(contents.find("careful"), std::string::npos);
}

TEST(Logger, MessageBelowThresholdSeverityIsSuppressed) {
    std::string path = TempFilePath("below_threshold");
    Logger logger(path, LogLevel::Warning);

    EXPECT_TRUE(logger.Log("just fyi", LogLevel::Info));

    EXPECT_EQ(ReadFile(path), "");
}

TEST(Logger, MessageAboveThresholdSeverityIsWritten) {
    std::string path = TempFilePath("above_threshold");
    Logger logger(path, LogLevel::Warning);

    EXPECT_TRUE(logger.Log("on fire", LogLevel::Error));

    std::string contents = ReadFile(path);
    EXPECT_NE(contents.find("on fire"), std::string::npos);
}

TEST(Logger, ErrorSeverityIsAlwaysWrittenRegardlessOfThreshold) {
    std::string path = TempFilePath("error_always");
    Logger logger(path, LogLevel::Error);

    EXPECT_TRUE(logger.Log("boom", LogLevel::Error));

    std::string contents = ReadFile(path);
    EXPECT_NE(contents.find("boom"), std::string::npos);
}

TEST(Logger, LogWithNoLevelArgDefaultsToInfo) {
    std::string path = TempFilePath("default_info");
    Logger logger(path, LogLevel::Info);

    EXPECT_TRUE(logger.Log("plain message"));

    std::string contents = ReadFile(path);
    EXPECT_NE(contents.find("plain message"), std::string::npos);
    EXPECT_NE(contents.find("[INFO]"), std::string::npos);
}

TEST(Logger, WrittenLineIncludesLevelName) {
    std::string path = TempFilePath("level_name");
    Logger logger(path, LogLevel::Debug);

    logger.Log("careful", LogLevel::Warning);

    std::string contents = ReadFile(path);
    EXPECT_NE(contents.find("[WARNING]"), std::string::npos);
}

TEST(Logger, ParseLogLevelParsesEachValidNameCaseInsensitively) {
    EXPECT_EQ(indexed::ParseLogLevel("ERROR"), LogLevel::Error);
    EXPECT_EQ(indexed::ParseLogLevel("error"), LogLevel::Error);
    EXPECT_EQ(indexed::ParseLogLevel("Warning"), LogLevel::Warning);
    EXPECT_EQ(indexed::ParseLogLevel("info"), LogLevel::Info);
    EXPECT_EQ(indexed::ParseLogLevel("DEBUG"), LogLevel::Debug);
}

TEST(Logger, ParseLogLevelReturnsFallbackForUnrecognizedString) {
    EXPECT_EQ(indexed::ParseLogLevel("NOT_A_LEVEL", LogLevel::Debug), LogLevel::Debug);
    EXPECT_EQ(indexed::ParseLogLevel("", LogLevel::Error), LogLevel::Error);
}

TEST(Logger, LevelReturnsConstructedThreshold) {
    Logger logger(TempFilePath("level_accessor"), LogLevel::Debug);
    EXPECT_EQ(logger.Level(), LogLevel::Debug);
}

TEST(Logger, DefaultConstructedLevelIsWarning) {
    Logger logger(TempFilePath("default_level"));
    EXPECT_EQ(logger.Level(), LogLevel::Warning);
}
