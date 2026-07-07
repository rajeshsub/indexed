#include "indexer/StatusFile.h"

#include <charconv>
#include <sstream>

namespace indexed {

namespace {

// Escaping mirrors IniFile's convention (indexed-plan.md §7.5): `\\`->`\\\\`,
// `\n`->`\\n`, so Message can carry an embedded literal newline without
// corrupting the one-key-per-line format. Duplicated rather than shared
// since IniFile's escape helpers are file-local to IniFile.cpp and this is
// a handful of lines -- not worth widening IniFile's public API for.
std::string Escape(std::string_view value) {
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

std::string Unescape(std::string_view escaped) {
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

const char* StateName(IndexerState state) {
    switch (state) {
        case IndexerState::Idle:
            return "Idle";
        case IndexerState::Scanning:
            return "Scanning";
        case IndexerState::LoadingIndex:
            return "LoadingIndex";
        case IndexerState::WatchingForChanges:
            return "WatchingForChanges";
        case IndexerState::Error:
            return "Error";
    }
    return "Idle";
}

std::optional<IndexerState> ParseStateName(std::string_view name) {
    if (name == "Idle") {
        return IndexerState::Idle;
    }
    if (name == "Scanning") {
        return IndexerState::Scanning;
    }
    if (name == "LoadingIndex") {
        return IndexerState::LoadingIndex;
    }
    if (name == "WatchingForChanges") {
        return IndexerState::WatchingForChanges;
    }
    if (name == "Error") {
        return IndexerState::Error;
    }
    return std::nullopt;
}

std::optional<uint64_t> ParseUint64(std::string_view text) {
    uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::string SerializeStatus(const IndexerStatus& status) {
    std::ostringstream out;
    out << "State=" << StateName(status.state) << '\n';
    out << "Message=" << Escape(status.message) << '\n';
    out << "FilesIndexed=" << status.filesIndexed << '\n';
    out << "IndexAgeSeconds=" << status.indexAgeSeconds << '\n';
    out << "LocationsCount=" << status.locations.size() << '\n';
    for (const std::string& location : status.locations) {
        out << "Location=" << Escape(location) << '\n';
    }
    out << "SkippedCount=" << status.skippedPaths.size() << '\n';
    for (const std::string& skipped : status.skippedPaths) {
        out << "Skipped=" << Escape(skipped) << '\n';
    }
    return out.str();
}

std::optional<IndexerStatus> ParseStatus(std::string_view text) {
    // Line-oriented parse: split on '\n', then match each expected key in
    // its fixed emitted order. Any structural deviation -- missing key,
    // wrong key, unparsable number, unknown state name, or a torn/truncated
    // read -- fails the whole parse rather than returning a partial record;
    // callers treat nullopt as "try again once the file settles."
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            if (start != text.size()) {
                return std::nullopt;  // trailing content with no newline: torn write
            }
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }

    auto takeField = [&lines](size_t index,
                              std::string_view key) -> std::optional<std::string_view> {
        if (index >= lines.size()) {
            return std::nullopt;
        }
        std::string_view line = lines[index];
        if (line.size() < key.size() + 1 || line.substr(0, key.size()) != key ||
            line[key.size()] != '=') {
            return std::nullopt;
        }
        return line.substr(key.size() + 1);
    };

    IndexerStatus status;
    size_t idx = 0;

    auto stateField = takeField(idx++, "State");
    if (!stateField) {
        return std::nullopt;
    }
    auto state = ParseStateName(*stateField);
    if (!state) {
        return std::nullopt;
    }
    status.state = *state;

    auto messageField = takeField(idx++, "Message");
    if (!messageField) {
        return std::nullopt;
    }
    status.message = Unescape(*messageField);

    auto filesField = takeField(idx++, "FilesIndexed");
    if (!filesField) {
        return std::nullopt;
    }
    auto filesIndexed = ParseUint64(*filesField);
    if (!filesIndexed) {
        return std::nullopt;
    }
    status.filesIndexed = *filesIndexed;

    auto ageField = takeField(idx++, "IndexAgeSeconds");
    if (!ageField) {
        return std::nullopt;
    }
    auto age = ParseUint64(*ageField);
    if (!age) {
        return std::nullopt;
    }
    status.indexAgeSeconds = *age;

    auto locCountField = takeField(idx++, "LocationsCount");
    if (!locCountField) {
        return std::nullopt;
    }
    auto locCount = ParseUint64(*locCountField);
    if (!locCount) {
        return std::nullopt;
    }
    for (uint64_t i = 0; i < *locCount; ++i) {
        auto location = takeField(idx++, "Location");
        if (!location) {
            return std::nullopt;
        }
        status.locations.push_back(Unescape(*location));
    }

    auto skipCountField = takeField(idx++, "SkippedCount");
    if (!skipCountField) {
        return std::nullopt;
    }
    auto skipCount = ParseUint64(*skipCountField);
    if (!skipCount) {
        return std::nullopt;
    }
    for (uint64_t i = 0; i < *skipCount; ++i) {
        auto skipped = takeField(idx++, "Skipped");
        if (!skipped) {
            return std::nullopt;
        }
        status.skippedPaths.push_back(Unescape(*skipped));
    }

    return status;
}

}  // namespace indexed
