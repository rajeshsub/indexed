#pragma once

#include <string>
#include <vector>

namespace indexed {

// Port of winindex Settings, backed by a hand-rolled IniFile (Qt-free).
// Schema: indexed-plan.md §12.1. List-valued keys (SelectedRoots,
// ExcludedPaths) are newline-joined on disk -- see Save()'s embedded-newline
// rejection below.
class Settings {
public:
    // `homeDir` is used only to materialize DefaultExcludedPaths() the first
    // time ExcludedPaths has never been saved (fresh install); passed
    // explicitly (rather than read via getenv) so Settings stays hermetically
    // testable.
    Settings(std::string configPath, std::string homeDir);

    // Loads from configPath. A missing file is not an error -- Settings
    // starts at defaults (§12.1), with ExcludedPaths materialized from
    // DefaultExcludedPaths(homeDir). Returns false only on a genuine I/O
    // error reading a file that does exist (e.g. permission denied).
    bool Load();

    // Writes the current settings to configPath, creating the parent
    // directory if needed. Rejects (returns false, leaves the file
    // untouched, populates LastError()) if any SelectedRoots/ExcludedPaths
    // entry contains a literal '\n': entries are newline-joined on disk, so
    // an embedded newline would be indistinguishable from the separator and
    // silently corrupt the list (indexed-plan.md §12.1, resolved decision --
    // don't relitigate). Vanishingly rare in practice: no path from the
    // GUI's own folder pickers can contain one.
    bool Save();

    // Non-empty after a Save() rejected by the embedded-newline check above;
    // cleared by a successful Save().
    const std::string& LastError() const;

    const std::vector<std::string>& SelectedRoots() const;
    void SetSelectedRoots(std::vector<std::string> roots);

    const std::vector<std::string>& ExcludedPaths() const;
    void SetExcludedPaths(std::vector<std::string> paths);

    int ReindexIntervalHours() const;
    void SetReindexIntervalHours(int hours);

    bool UseRegex() const;
    void SetUseRegex(bool value);

    bool CaseSensitive() const;
    void SetCaseSensitive(bool value);

    bool WholeWord() const;
    void SetWholeWord(bool value);

    bool MatchPath() const;
    void SetMatchPath(bool value);

    bool IgnoreDiacritics() const;
    void SetIgnoreDiacritics(bool value);

    bool FirstRunComplete() const;
    void SetFirstRunComplete(bool value);

    // Default excluded paths (§12.3), with a leading "~" expanded against
    // homeDir. Exposed standalone so callers/tests can inspect the default
    // list without a Settings instance.
    static std::vector<std::string> DefaultExcludedPaths(const std::string& homeDir);

private:
    std::string configPath_;
    std::string homeDir_;
    std::string lastError_;

    std::vector<std::string> selectedRoots_;
    std::vector<std::string> excludedPaths_;
    int reindexIntervalHours_ = 48;
    bool useRegex_ = false;
    bool caseSensitive_ = false;
    bool wholeWord_ = false;
    bool matchPath_ = false;
    bool ignoreDiacritics_ = false;
    bool firstRunComplete_ = false;
};

}  // namespace indexed
