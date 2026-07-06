#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace indexed {

// Minimal, hand-rolled INI-style key=value store -- the Qt-free substrate
// Settings is built on (indexed-plan.md §7.5). Flat/section-less: winindex's
// schema has no need for `[section]` headers. `#`/`;`-prefixed lines and
// blank lines are comments/ignored. Values may contain embedded newlines
// (Settings joins path lists with `\n`); Save()/Load() transparently
// escape/unescape `\n` and `\\` so the on-disk file stays one physical line
// per key regardless of what a value contains.
class IniFile {
public:
    // Loads key=value pairs from path, replacing any keys already held.
    // A missing file is not an error -- it loads as empty (defaults apply).
    // Returns false only on a genuine I/O error reading a file that exists
    // (e.g. permission denied, or the path is a directory).
    bool Load(const std::string& path);

    // Writes all keys back to path (one `key=value` line each, overwriting
    // the file). Returns false on I/O failure (e.g. parent directory missing
    // -- callers are expected to have ensured it exists).
    bool Save(const std::string& path) const;

    std::optional<std::string> GetString(const std::string& key) const;
    void SetString(const std::string& key, std::string value);

    int GetInt(const std::string& key, int defaultValue) const;
    void SetInt(const std::string& key, int value);

    bool GetBool(const std::string& key, bool defaultValue) const;
    void SetBool(const std::string& key, bool value);

    bool Has(const std::string& key) const;

private:
    std::unordered_map<std::string, std::string> values_;
};

}  // namespace indexed
