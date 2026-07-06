#pragma once

#include <string>
#include <string_view>

namespace indexed {

// Minimal timestamped append-only log (indexed-plan.md §7.5) -- no log
// levels or rotation for v0.1.0. Writes to the path given at construction,
// creating parent directories on first Log() call if needed.
class Logger {
public:
    explicit Logger(std::string logPath);

    // Appends a "[YYYY-MM-DD HH:MM:SS] message\n" line to logPath. Returns
    // false if the write failed (e.g. parent directory couldn't be created).
    bool Log(std::string_view message);

private:
    std::string logPath_;
};

}  // namespace indexed
