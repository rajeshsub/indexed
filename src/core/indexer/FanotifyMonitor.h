#pragma once

#include <fcntl.h>
#include <sys/fanotify.h>

#include "indexer/IChangeMonitor.h"
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace indexed {

// Resolves a file handle (as delivered inside fanotify's FAN_REPORT_DFID_NAME
// info record) plus the mount fd it was reported against, into that
// directory's absolute path. Extracted behind an interface so
// FanotifyMonitor::ParseFanotifyEvent's fixture-based tests don't need a real
// fanotify fd / live kernel-issued file handle (only obtainable from an
// actual privileged fanotify session) -- tests inject a stub; production
// code uses ProcFdFileHandleResolver. Mirrors MountEnumerator's
// IDeviceInfoResolver seam.
class IFileHandleResolver {
public:
    virtual ~IFileHandleResolver() = default;

    // Returns "" if resolution fails (open_by_handle_at or readlink error,
    // or handle is null). Never throws.
    virtual std::string ResolveDirPath(int mountFd, const struct file_handle* handle) const = 0;
};

// Real resolver: open_by_handle_at(mountFd, handle, O_RDONLY) to get an fd
// for the directory the handle refers to, then readlink("/proc/self/fd/<fd>")
// to recover its path. Needs CAP_DAC_READ_SEARCH (assumed available
// alongside CAP_SYS_ADMIN inside indexed-helper per indexed-plan.md §9.1).
class ProcFdFileHandleResolver : public IFileHandleResolver {
public:
    std::string ResolveDirPath(int mountFd, const struct file_handle* handle) const override;
};

// Privileged, whole-filesystem live-monitoring backend for IChangeMonitor
// (indexed-plan.md §7.2). Preferred over InotifyWatcher when available:
// requires kernel >= 5.9 (FAN_REPORT_DFID_NAME) and CAP_SYS_ADMIN (+
// CAP_DAC_READ_SEARCH for path resolution), both only present when running
// inside indexed-helper under root via pkexec (§9). On an unprivileged
// machine (e.g. this dev sandbox) IsAvailable() honestly reports false.
//
// Contract: callers do NOT need to call IsAvailable() before
// StartMonitoring() -- StartMonitoring re-checks availability itself and
// returns promptly (without attempting fanotify_mark or entering the poll
// loop) if fanotify can't be used, so it degrades gracefully on its own.
//
// Rename correlation: FAN_REPORT_DFID_NAME mode carries no rename cookie to
// pair a FAN_MOVED_FROM with its matching FAN_MOVED_TO the way inotify's
// IN_MOVED_FROM/IN_MOVED_TO cookie does, and the two can arrive arbitrarily
// far apart (or not both arrive, e.g. move-out-of-tree). Rather than build a
// fragile heuristic pairing window, a lone FAN_MOVED_FROM is reported as
// Removed and a lone FAN_MOVED_TO as Added -- the same pragmatic tradeoff
// InotifyWatcher's sibling implementation documents for its own case.
class FanotifyMonitor : public IChangeMonitor {
public:
    // Uses a real ProcFdFileHandleResolver.
    FanotifyMonitor();
    // Test/DI seam: inject a stub resolver to avoid needing a real fanotify
    // fd / file handle.
    explicit FanotifyMonitor(std::shared_ptr<IFileHandleResolver> resolver);

    // Attempts fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME,
    // O_RDONLY), then a transient FAN_MARK_ADD | FAN_MARK_FILESYSTEM probe
    // mark on `root` (removed again immediately). Returns true only if both
    // succeed; false for any failure along the way (EINVAL: kernel < 5.9
    // doesn't support FAN_REPORT_DFID_NAME; EPERM on the mark step: no
    // CAP_SYS_ADMIN; anything else) -- never throws.
    //
    // Note: fanotify_init() alone is NOT a sufficient probe on kernels >=
    // 5.9 -- it succeeds for unprivileged callers once an FID-reporting flag
    // like FAN_REPORT_DFID_NAME is passed ("unprivileged fanotify
    // listeners"). CAP_SYS_ADMIN is enforced later, specifically by
    // FAN_MARK_FILESYSTEM (the whole-filesystem mark StartMonitoring needs),
    // which is why that step is what's actually probed here. Confirmed by
    // direct syscall testing: in this sandbox (no CAP_SYS_ADMIN),
    // fanotify_init() alone succeeds but the FAN_MARK_FILESYSTEM mark fails
    // with EPERM.
    bool IsAvailable(const std::string& root) const override;

    // Blocks until stopToken is set, invoking onChange on the calling thread
    // for each live change under the filesystem containing `root`. Returns
    // promptly without blocking if fanotify is unavailable (see class
    // comment). Internally: fanotify_init, fanotify_mark(FAN_MARK_ADD |
    // FAN_MARK_FILESYSTEM, ...) on `root`, then poll()s the fd with a short
    // timeout so stopToken is checked regularly between reads.
    void StartMonitoring(const std::string& root, ChangeCallback onChange,
                         const std::atomic<bool>& stopToken) override;

    // Core, testable parsing logic (mirrors MountEnumerator::ParseMountInfo):
    // parses one raw fanotify event record -- a fanotify_event_metadata
    // header followed by its FAN_REPORT_DFID_NAME info record (file handle +
    // trailing name) -- into a FileChangeEvent. `mountFd` is passed through
    // to the injected IFileHandleResolver to resolve the handle to a
    // directory path. Returns nullopt if: the mask doesn't map to any
    // FileChangeType (see class comment), no DFID_NAME info record is
    // present, the record is malformed/truncated, or path resolution fails.
    // Never throws.
    std::optional<FileChangeEvent> ParseFanotifyEvent(const fanotify_event_metadata* metadata,
                                                      int mountFd) const;

private:
    std::shared_ptr<IFileHandleResolver> resolver_;
};

}  // namespace indexed
