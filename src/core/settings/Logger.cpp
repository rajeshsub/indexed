#include "settings/Logger.h"

#include "settings/PathUtils.h"
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

}  // namespace

Logger::Logger(std::string logPath) : logPath_(std::move(logPath)) {}

bool Logger::Log(std::string_view message) {
    std::filesystem::path parent = std::filesystem::path(logPath_).parent_path();
    if (!parent.empty() && !EnsureDirectory(parent.string())) {
        return false;
    }

    std::ofstream file(logPath_, std::ios::app);
    if (!file.is_open()) {
        return false;
    }
    file << '[' << Timestamp() << "] " << message << '\n';
    return file.good();
}

}  // namespace indexed
