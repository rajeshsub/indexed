#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace indexed {

// Attribute bitfield for FileEntry::attributes / EntryMeta::attributes.
enum FileAttribute : uint32_t {
    kAttrDirectory = 1u << 0,
    kAttrSymlink = 1u << 1,
    kAttrHidden = 1u << 2,
};

struct FileEntry {
    std::string name;  // basename only
    std::string path;  // full absolute path
    uint64_t size = 0;
    uint64_t lastModified = 0;  // nanoseconds since Unix epoch (statx stx_mtime-derived)
    uint32_t attributes = 0;    // bitfield: see FileAttribute
};

using ScanCallback = std::function<void(const FileEntry&)>;
using ProgressCallback = std::function<void(uint64_t filesFound, const std::string& currentDir)>;

struct ScanOptions {
    std::vector<std::string> rootPaths;
    std::vector<std::string> excludedPaths;
};

// Implemented by WalkScanner (M3). Declared now so storage/ and future
// callers can depend on FileEntry without waiting on the M3 implementation.
class IFileSystemScanner {
public:
    virtual ~IFileSystemScanner() = default;
    virtual bool FastScanAvailable(const std::string& root) const = 0;
    virtual void Scan(const ScanOptions& options, ScanCallback onEntry, ProgressCallback onProgress,
                      const std::atomic<bool>& cancelToken) = 0;
};

}  // namespace indexed
