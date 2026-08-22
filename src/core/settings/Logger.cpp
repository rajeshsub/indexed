#include "settings/Logger.h"

#include "settings/PathUtils.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace indexed {

namespace {

std::string Timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf{};
    localtime_r(&now, &tmBuf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(buf);
}

std::string ToUpper(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

}  // namespace

LogLevel ParseLogLevel(std::string_view name, LogLevel fallback) {
    std::string upper = ToUpper(name);
    if (upper == "ERROR") {
        return LogLevel::Error;
    }
    if (upper == "WARNING") {
        return LogLevel::Warning;
    }
    if (upper == "INFO") {
        return LogLevel::Info;
    }
    if (upper == "DEBUG") {
        return LogLevel::Debug;
    }
    return fallback;
}

std::string_view LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Warning:
            return "WARNING";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Debug:
            return "DEBUG";
    }
    return "UNKNOWN";
}

Logger::Logger(std::string logPath, LogLevel level) : logPath_(std::move(logPath)), level_(level) {}

bool Logger::Log(std::string_view message, LogLevel level) {
    // Lower enumerator value = more severe (Error=0 .. Debug=3); a message is
    // loggable when it's at least as severe as the configured threshold.
    if (level > level_) {
        return true;
    }

    std::filesystem::path parent = std::filesystem::path(logPath_).parent_path();
    if (!parent.empty() && !EnsureDirectory(parent.string())) {
        return false;
    }

    std::ofstream file(logPath_, std::ios::app);
    if (!file.is_open()) {
        return false;
    }
    file << '[' << Timestamp() << "] [" << LogLevelName(level) << "] " << message << '\n';
    return file.good();
}

LogLevel Logger::Level() const {
    return level_;
}

}  // namespace indexed
