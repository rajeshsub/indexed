#pragma once

#include <string>
#include <string_view>

namespace indexed {

// Severity hierarchy, most to least severe (engineering-standards rule 17,
// trimmed for this app's shape -- no CRITICAL/VERBOSE tier, nothing here
// currently warrants either; see docs/adr/0009).
enum class LogLevel { Error, Warning, Info, Debug };

// Parses a level name case-insensitively ("ERROR"/"WARNING"/"INFO"/"DEBUG").
// Returns fallback for anything else, including empty/malformed input.
LogLevel ParseLogLevel(std::string_view name, LogLevel fallback = LogLevel::Warning);

// Upper-case name for level (e.g. "WARNING"), as written into a log line.
std::string_view LogLevelName(LogLevel level);

// Timestamped append-only log (indexed-plan.md §7.5), filtered by severity
// threshold (docs/adr/0009). Writes to the path given at construction,
// creating parent directories on first Log() call if needed.
class Logger {
public:
    explicit Logger(std::string logPath, LogLevel level = LogLevel::Warning);

    // Appends a "[YYYY-MM-DD HH:MM:SS] [LEVEL] message\n" line to logPath if
    // `level` is at least as severe as the configured threshold; otherwise a
    // no-op. Returns false only if a write was attempted and failed (e.g.
    // parent directory couldn't be created); returns true when suppressed.
    bool Log(std::string_view message, LogLevel level = LogLevel::Info);

    LogLevel Level() const;

private:
    std::string logPath_;
    LogLevel level_;
};

}  // namespace indexed
