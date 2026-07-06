#include "settings/PathUtils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace indexed {

namespace fs = std::filesystem;

std::string ExecutableDir() {
    std::error_code ec;
    fs::path exePath = fs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return "";
    }
    return exePath.parent_path().string();
}

bool IsPortableMode(const std::string& executableDir) {
    if (executableDir.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::exists(fs::path(executableDir) / "indexed.conf", ec);
}

namespace {

std::string JoinPath(const std::string& dir, const std::string& file) {
    return (fs::path(dir) / file).string();
}

// overrideValue == "" means "XDG_* not set" -> default location under homeDir.
std::string XdgDirOrDefault(const std::string& overrideValue, const std::string& homeDir,
                            const std::string& defaultSuffix) {
    if (!overrideValue.empty()) {
        return overrideValue;
    }
    return (fs::path(homeDir) / defaultSuffix).string();
}

}  // namespace

DataDirs ResolveDataDirs(const std::string& executableDir, const XdgEnv& xdg) {
    DataDirs dirs;
    if (IsPortableMode(executableDir)) {
        dirs.configPath = JoinPath(executableDir, "indexed.conf");
        dirs.indexPath = JoinPath(executableDir, "indexed.idx");
        dirs.logPath = JoinPath(executableDir, "indexed.log");
        return dirs;
    }

    std::string configHome = XdgDirOrDefault(xdg.xdgConfigHome, xdg.home, ".config");
    std::string cacheHome = XdgDirOrDefault(xdg.xdgCacheHome, xdg.home, ".cache");
    std::string stateHome = XdgDirOrDefault(xdg.xdgStateHome, xdg.home, ".local/state");

    dirs.configPath = JoinPath(JoinPath(configHome, "indexed"), "indexed.conf");
    dirs.indexPath = JoinPath(JoinPath(cacheHome, "indexed"), "indexed.idx");
    dirs.logPath = JoinPath(JoinPath(stateHome, "indexed"), "indexed.log");
    return dirs;
}

DataDirs ResolveDataDirs() {
    XdgEnv xdg;
    const char* home = std::getenv("HOME");
    xdg.home = home != nullptr ? home : "";
    const char* configHome = std::getenv("XDG_CONFIG_HOME");
    xdg.xdgConfigHome = configHome != nullptr ? configHome : "";
    const char* cacheHome = std::getenv("XDG_CACHE_HOME");
    xdg.xdgCacheHome = cacheHome != nullptr ? cacheHome : "";
    const char* stateHome = std::getenv("XDG_STATE_HOME");
    xdg.xdgStateHome = stateHome != nullptr ? stateHome : "";
    return ResolveDataDirs(ExecutableDir(), xdg);
}

bool EnsureDirectory(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        return fs::is_directory(path, ec);
    }
    return fs::create_directories(path, ec);
}

std::string FormatFileCount(uint64_t count) {
    std::string digits = std::to_string(count);
    std::string result;
    result.reserve(digits.size() + digits.size() / 3);

    int sinceGroup = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (sinceGroup != 0 && sinceGroup % 3 == 0) {
            result.push_back(',');
        }
        result.push_back(*it);
        ++sinceGroup;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::string FormatAge(uint64_t ageSeconds) {
    constexpr uint64_t kMinute = 60;
    constexpr uint64_t kHour = 60 * kMinute;
    constexpr uint64_t kDay = 24 * kHour;

    if (ageSeconds < kMinute) {
        return "just indexed";
    }
    if (ageSeconds < kHour) {
        return std::to_string(ageSeconds / kMinute) + " min old";
    }
    if (ageSeconds < kDay) {
        return std::to_string(ageSeconds / kHour) + " hrs old";
    }
    uint64_t days = ageSeconds / kDay;
    uint64_t hours = (ageSeconds % kDay) / kHour;
    return std::to_string(days) + " days, " + std::to_string(hours) + " hrs old";
}

std::string FormatLocationList(const std::vector<std::string>& roots) {
    std::string result;
    for (size_t i = 0; i < roots.size(); ++i) {
        std::string root = roots[i];
        if (root.size() > 1 && root.back() == '/') {
            root.pop_back();
        }
        if (i != 0) {
            result += ", ";
        }
        result += root;
    }
    return result;
}

}  // namespace indexed
