#pragma once

#include "indexer/IChangeMonitor.h"
#include <atomic>
#include <string>

namespace indexed {

// Unprivileged live-monitoring backend for M5 (indexed-plan.md §7.2), built
// on inotify(7). This is the fallback IChangeMonitor used when
// FanotifyMonitor's CAP_SYS_ADMIN requirement isn't available: inotify needs
// no special privilege, at the cost of two limitations relative to fanotify
// -- no single filesystem-wide mark, and a finite fs.inotify.max_user_watches
// ceiling (see WatchLimitExceeded()).
//
// Directories under `root` are watched recursively at StartMonitoring time;
// new subdirectories created (or moved in) live are discovered and watched
// as they appear, so the watch tree keeps growing without a restart.
// Symlinks are never watched and never descended into, mirroring
// WalkScanner's no-follow-symlinks policy.
class InotifyWatcher : public IChangeMonitor {
public:
    // True for any real, existing directory (inotify needs no special
    // privilege to watch it). False if `root` doesn't exist, or in the rare
    // case inotify_init1() itself fails (e.g. fd exhaustion).
    bool IsAvailable(const std::string& root) const override;

    // Blocks (polling the inotify fd) until stopToken is set. Not
    // reentrant: one InotifyWatcher instance monitors one root at a time.
    void StartMonitoring(const std::string& root, ChangeCallback onChange,
                         const std::atomic<bool>& stopToken) override;

    // True once any directory under the watched tree could not be watched
    // because inotify_add_watch() failed with ENOSPC
    // (fs.inotify.max_user_watches exhausted). Not part of the
    // IChangeMonitor interface -- an extra accessor a future caller
    // (Indexer/GUI) can poll to surface "live monitoring incomplete" (§7.2).
    // Never resets to false once set for the lifetime of this instance.
    bool WatchLimitExceeded() const;

private:
    std::atomic<bool> watchLimitExceeded_{false};
};

}  // namespace indexed
