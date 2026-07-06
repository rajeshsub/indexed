#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace indexed {

enum class FileChangeType { Added, Removed, Renamed, Modified };

struct FileChangeEvent {
    FileChangeType type;
    std::string path;
    std::string oldPath;  // only set for Renamed
};

using ChangeCallback = std::function<void(const FileChangeEvent&)>;

// Implemented by FanotifyMonitor (privileged) / InotifyWatcher (unprivileged
// fallback) in M5. Declared now so Indexer (M3) can depend on it and be
// tested against a mock without waiting on the M5 implementations.
class IChangeMonitor {
public:
    virtual ~IChangeMonitor() = default;
    virtual bool IsAvailable(const std::string& root) const = 0;
    // Blocks until stopToken is set; invokes onChange for each live change on
    // the calling thread.
    virtual void StartMonitoring(const std::string& root, ChangeCallback onChange,
                                 const std::atomic<bool>& stopToken) = 0;
};

}  // namespace indexed
