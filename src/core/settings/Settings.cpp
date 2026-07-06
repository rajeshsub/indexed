#include "settings/Settings.h"

#include "settings/IniFile.h"
#include "settings/PathUtils.h"
#include <algorithm>
#include <filesystem>

namespace indexed {

namespace {

constexpr const char* kKeySelectedRoots = "SelectedRoots";
constexpr const char* kKeyExcludedPaths = "ExcludedPaths";
constexpr const char* kKeyReindexIntervalHours = "ReindexIntervalHours";
constexpr const char* kKeyUseRegex = "UseRegex";
constexpr const char* kKeyCaseSensitive = "CaseSensitive";
constexpr const char* kKeyWholeWord = "WholeWord";
constexpr const char* kKeyMatchPath = "MatchPath";
constexpr const char* kKeyIgnoreDiacritics = "IgnoreDiacritics";
constexpr const char* kKeyFirstRunComplete = "FirstRunComplete";

std::vector<std::string> SplitLines(const std::string& joined) {
    std::vector<std::string> result;
    if (joined.empty()) {
        return result;
    }
    size_t start = 0;
    while (start <= joined.size()) {
        size_t next = joined.find('\n', start);
        if (next == std::string::npos) {
            result.push_back(joined.substr(start));
            break;
        }
        result.push_back(joined.substr(start, next - start));
        start = next + 1;
    }
    return result;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            result += '\n';
        }
        result += lines[i];
    }
    return result;
}

bool AnyContainsNewline(const std::vector<std::string>& entries) {
    return std::any_of(entries.begin(), entries.end(),
                       [](const std::string& s) { return s.find('\n') != std::string::npos; });
}

std::string ExpandHome(const std::string& path, const std::string& homeDir) {
    if (path == "~") {
        return homeDir;
    }
    if (path.size() > 1 && path[0] == '~' && path[1] == '/') {
        return homeDir + path.substr(1);
    }
    return path;
}

}  // namespace

Settings::Settings(std::string configPath, std::string homeDir)
    : configPath_(std::move(configPath)), homeDir_(std::move(homeDir)) {}

bool Settings::Load() {
    IniFile ini;
    if (!ini.Load(configPath_)) {
        return false;
    }

    selectedRoots_ = SplitLines(ini.GetString(kKeySelectedRoots).value_or(""));

    if (ini.Has(kKeyExcludedPaths)) {
        excludedPaths_ = SplitLines(ini.GetString(kKeyExcludedPaths).value_or(""));
    } else {
        // Fresh install (key never saved) -- materialize the default list
        // (indexed-plan.md §12.3). Once the user saves any list (even an
        // empty one), the key exists and this branch is never taken again.
        excludedPaths_ = DefaultExcludedPaths(homeDir_);
    }

    reindexIntervalHours_ = ini.GetInt(kKeyReindexIntervalHours, 48);
    useRegex_ = ini.GetBool(kKeyUseRegex, false);
    caseSensitive_ = ini.GetBool(kKeyCaseSensitive, false);
    wholeWord_ = ini.GetBool(kKeyWholeWord, false);
    matchPath_ = ini.GetBool(kKeyMatchPath, false);
    ignoreDiacritics_ = ini.GetBool(kKeyIgnoreDiacritics, false);
    firstRunComplete_ = ini.GetBool(kKeyFirstRunComplete, false);
    return true;
}

bool Settings::Save() {
    if (AnyContainsNewline(selectedRoots_) || AnyContainsNewline(excludedPaths_)) {
        lastError_ =
            "Settings::Save: a path contains a literal newline, which cannot be stored "
            "(newline is the list separator for SelectedRoots/ExcludedPaths) -- rejecting save";
        return false;
    }

    std::filesystem::path parent = std::filesystem::path(configPath_).parent_path();
    if (!parent.empty() && !EnsureDirectory(parent.string())) {
        lastError_ = "Settings::Save: failed to create config directory";
        return false;
    }

    // Best-effort: preserve any keys this class doesn't manage by loading
    // the existing file first (if any) before overwriting our own keys.
    IniFile ini;
    ini.Load(configPath_);

    ini.SetString(kKeySelectedRoots, JoinLines(selectedRoots_));
    ini.SetString(kKeyExcludedPaths, JoinLines(excludedPaths_));
    ini.SetInt(kKeyReindexIntervalHours, reindexIntervalHours_);
    ini.SetBool(kKeyUseRegex, useRegex_);
    ini.SetBool(kKeyCaseSensitive, caseSensitive_);
    ini.SetBool(kKeyWholeWord, wholeWord_);
    ini.SetBool(kKeyMatchPath, matchPath_);
    ini.SetBool(kKeyIgnoreDiacritics, ignoreDiacritics_);
    ini.SetBool(kKeyFirstRunComplete, firstRunComplete_);

    if (!ini.Save(configPath_)) {
        lastError_ = "Settings::Save: failed to write config file";
        return false;
    }
    lastError_.clear();
    return true;
}

const std::string& Settings::LastError() const {
    return lastError_;
}

const std::vector<std::string>& Settings::SelectedRoots() const {
    return selectedRoots_;
}

void Settings::SetSelectedRoots(std::vector<std::string> roots) {
    selectedRoots_ = std::move(roots);
}

const std::vector<std::string>& Settings::ExcludedPaths() const {
    return excludedPaths_;
}

void Settings::SetExcludedPaths(std::vector<std::string> paths) {
    excludedPaths_ = std::move(paths);
}

int Settings::ReindexIntervalHours() const {
    return reindexIntervalHours_;
}

void Settings::SetReindexIntervalHours(int hours) {
    reindexIntervalHours_ = hours;
}

bool Settings::UseRegex() const {
    return useRegex_;
}

void Settings::SetUseRegex(bool value) {
    useRegex_ = value;
}

bool Settings::CaseSensitive() const {
    return caseSensitive_;
}

void Settings::SetCaseSensitive(bool value) {
    caseSensitive_ = value;
}

bool Settings::WholeWord() const {
    return wholeWord_;
}

void Settings::SetWholeWord(bool value) {
    wholeWord_ = value;
}

bool Settings::MatchPath() const {
    return matchPath_;
}

void Settings::SetMatchPath(bool value) {
    matchPath_ = value;
}

bool Settings::IgnoreDiacritics() const {
    return ignoreDiacritics_;
}

void Settings::SetIgnoreDiacritics(bool value) {
    ignoreDiacritics_ = value;
}

bool Settings::FirstRunComplete() const {
    return firstRunComplete_;
}

void Settings::SetFirstRunComplete(bool value) {
    firstRunComplete_ = value;
}

std::vector<std::string> Settings::DefaultExcludedPaths(const std::string& homeDir) {
    std::vector<std::string> defaults = {
        "/proc",
        "/sys",
        "/dev",
        "/run",
        "/tmp",
        "/var/cache",
        "/var/tmp",
        "/var/lib/docker",
        "/var/lib/containers",
        "/snap",
        "/var/lib/flatpak",
        "~/.cache",
        "~/.local/share/Trash",
        "~/.local/share/containers",
        "~/.var/app",
        "/lost+found",
    };
    for (std::string& path : defaults) {
        path = ExpandHome(path, homeDir);
    }
    return defaults;
}

}  // namespace indexed
