#include "settings/IniFile.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace indexed {

namespace {

std::string EscapeValue(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string UnescapeValue(std::string_view escaped) {
    std::string out;
    out.reserve(escaped.size());
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '\\' && i + 1 < escaped.size()) {
            char next = escaped[++i];
            out += (next == 'n') ? '\n' : next;
        } else {
            out += escaped[i];
        }
    }
    return out;
}

std::string_view Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

bool IniFile::Load(const std::string& path) {
    values_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        // Missing file -> empty/defaults, not an error. Any other reason a
        // file that exists can't be opened (permissions, is-a-directory) is
        // a genuine failure.
        std::error_code ec;
        return !std::filesystem::exists(path, ec);
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string_view::npos) {
            continue;  // malformed line, ignore
        }
        std::string key(Trim(trimmed.substr(0, eq)));
        std::string value = UnescapeValue(trimmed.substr(eq + 1));
        values_[key] = std::move(value);
    }
    return true;
}

bool IniFile::Save(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    for (const auto& [key, value] : values_) {
        file << key << '=' << EscapeValue(value) << '\n';
    }
    return file.good();
}

std::optional<std::string> IniFile::GetString(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void IniFile::SetString(const std::string& key, std::string value) {
    values_[key] = std::move(value);
}

int IniFile::GetInt(const std::string& key, int defaultValue) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return defaultValue;
    }
    try {
        return std::stoi(it->second);
    } catch (const std::exception&) {
        return defaultValue;
    }
}

void IniFile::SetInt(const std::string& key, int value) {
    values_[key] = std::to_string(value);
}

bool IniFile::GetBool(const std::string& key, bool defaultValue) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return defaultValue;
    }
    if (it->second == "1") {
        return true;
    }
    if (it->second == "0") {
        return false;
    }
    return defaultValue;
}

void IniFile::SetBool(const std::string& key, bool value) {
    values_[key] = value ? "1" : "0";
}

bool IniFile::Has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

}  // namespace indexed
