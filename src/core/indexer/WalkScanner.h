#pragma once

#include "indexer/IFileSystemScanner.h"

namespace indexed {

// Iterative, multi-threaded filesystem walker (§7.1 of indexed-plan.md).
//
// Uses the raw getdents64 syscall over an open(O_DIRECTORY) fd (not
// readdir()/std::filesystem) plus statx per entry for size/mtime/type, and
// drains a shared queue of pending directories across
// std::thread::hardware_concurrency() worker threads.
//
// Symlinks are always skipped: never followed, never descended into, and
// never emitted via onEntry (mirrors winindex skipping NTFS reparse
// points). Because a symlink is never emitted, kAttrSymlink is defined on
// FileEntry::attributes for interface completeness but WalkScanner itself
// never sets it.
//
// onEntry/onProgress may be invoked concurrently from multiple worker
// threads; callers that accumulate results must synchronize internally.
class WalkScanner : public IFileSystemScanner {
public:
    // Always false in v0.1.0: there is no ext4/btrfs/xfs analog of the NTFS
    // MFT fast-enumeration path, so this scanner never claims one is
    // available. Kept for interface/future-scanner compatibility.
    bool FastScanAvailable(const std::string& root) const override;

    void Scan(const ScanOptions& options, ScanCallback onEntry, ProgressCallback onProgress,
              const std::atomic<bool>& cancelToken) override;
};

}  // namespace indexed
