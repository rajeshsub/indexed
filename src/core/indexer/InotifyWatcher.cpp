#include "indexer/InotifyWatcher.h"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace indexed {

namespace {

namespace fs = std::filesystem;

constexpr int kPollTimeoutMs = 150;
constexpr uint32_t kWatchMask =
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE;
// Matches WalkScanner's getdents buffer sizing convention: a generous fixed
// size rather than trying to size exactly for one event.
constexpr size_t kEventBufferSize = 65536;

std::string JoinPath(const std::string& dir, std::string_view name) {
    if (dir.size() == 1 && dir.front() == '/') {
        return "/" + std::string(name);
    }
    return dir + "/" + std::string(name);
}

// Tracks which filesystem paths are currently watched, in both directions:
// wd -> path to reconstruct full paths from events (an inotify event only
// carries a wd + a relative name), and path -> wd to find the watch for a
// directory that's being deleted/moved away so it can be torn down.
class WatchTable {
public:
    void Add(int wd, const std::string& path) {
        wdToPath_[wd] = path;
        pathToWd_[path] = wd;
    }

    // Removes by wd. Used both when we explicitly rm_watch a directory and
    // when the kernel reports IN_IGNORED (a watch it auto-removed, e.g. on
    // deletion) -- either way our bookkeeping must not go stale.
    void RemoveByWd(int wd) {
        auto it = wdToPath_.find(wd);
        if (it == wdToPath_.end()) {
            return;
        }
        pathToWd_.erase(it->second);
        wdToPath_.erase(it);
    }

    std::string PathFor(int wd) const {
        auto it = wdToPath_.find(wd);
        return it != wdToPath_.end() ? it->second : std::string();
    }

    bool WdFor(const std::string& path, int& outWd) const {
        auto it = pathToWd_.find(path);
        if (it == pathToWd_.end()) {
            return false;
        }
        outWd = it->second;
        return true;
    }

private:
    std::unordered_map<int, std::string> wdToPath_;
    std::unordered_map<std::string, int> pathToWd_;
};

// A MOVED_FROM event buffered within the current read() batch, awaiting a
// possible matching MOVED_TO (same cookie) later in the same batch.
struct PendingMove {
    std::string path;
    bool isDir = false;
};

// Adds a watch for `dir` itself. Returns false (and, if the failure was
// specifically ENOSPC, flips *limitExceeded) without throwing/crashing --
// per §7.2, hitting fs.inotify.max_user_watches just means that one
// directory's contents won't get live updates; everything else watched
// successfully keeps working.
bool AddWatch(int fd, const std::string& dir, WatchTable& watches,
              std::atomic<bool>& limitExceeded) {
    int wd = inotify_add_watch(fd, dir.c_str(), kWatchMask);
    if (wd < 0) {
        if (errno == ENOSPC) {
            limitExceeded.store(true, std::memory_order_relaxed);
        }
        return false;
    }
    watches.Add(wd, dir);
    return true;
}

// Recursively adds watches for `root` and every non-symlink subdirectory
// beneath it, mirroring WalkScanner's no-follow-symlinks policy (symlinks
// are never watched, never descended into). Used both for the initial tree
// walk and to (re-)watch a subtree that appeared live via IN_CREATE or
// IN_MOVED_TO. inotify_add_watch is idempotent per-inode: re-adding a
// directory that's already watched (e.g. a directory renamed within the
// tree, whose nested watches survive the rename since they're keyed by
// inode, not path) just updates its mask and returns the existing wd, which
// self-heals this table's path entry to the new location.
void AddWatchesRecursive(int fd, const std::string& root, WatchTable& watches,
                         std::atomic<bool>& limitExceeded) {
    AddWatch(fd, root, watches, limitExceeded);

    std::error_code walkEc;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                        walkEc);
    fs::recursive_directory_iterator end;
    for (; !walkEc && it != end; it.increment(walkEc)) {
        const fs::directory_entry& entry = *it;
        std::error_code symEc;
        if (entry.is_symlink(symEc) || symEc) {
            continue;  // never followed, never watched
        }
        std::error_code dirEc;
        if (entry.is_directory(dirEc) && !dirEc) {
            AddWatch(fd, entry.path().string(), watches, limitExceeded);
        }
    }
}

}  // namespace

bool InotifyWatcher::IsAvailable(const std::string& root) const {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        return false;
    }
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

void InotifyWatcher::StartMonitoring(const std::string& root, ChangeCallback onChange,
                                     const std::atomic<bool>& stopToken) {
    std::error_code rootEc;
    if (!fs::is_directory(root, rootEc) || rootEc) {
        return;
    }

    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) {
        return;
    }

    WatchTable watches;
    AddWatchesRecursive(fd, root, watches, watchLimitExceeded_);

    std::vector<char> buffer(kEventBufferSize);

    while (!stopToken.load(std::memory_order_relaxed)) {
        struct pollfd pfd{fd, POLLIN, 0};
        int pollRc = poll(&pfd, 1, kPollTimeoutMs);
        if (pollRc <= 0) {
            continue;  // timeout or EINTR/error: loop back and recheck stopToken
        }

        // One "batch" is everything drainable from this poll wakeup.
        // Buffered here so a MOVED_FROM can be paired with a MOVED_TO
        // carrying the same cookie later in the same batch (§7.2).
        std::unordered_map<uint32_t, PendingMove> pendingMovedFrom;

        ssize_t nread = read(fd, buffer.data(), buffer.size());
        while (nread > 0) {
            ssize_t offset = 0;
            while (offset < nread) {
                auto* event = reinterpret_cast<struct inotify_event*>(buffer.data() + offset);
                offset += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);

                if ((event->mask & IN_IGNORED) != 0) {
                    watches.RemoveByWd(event->wd);
                    continue;
                }
                if ((event->mask & IN_Q_OVERFLOW) != 0) {
                    continue;  // some events lost; nothing addressable here
                }

                std::string parentPath = watches.PathFor(event->wd);
                if (parentPath.empty()) {
                    continue;  // watch we no longer track (already removed)
                }
                std::string name = event->len > 0 ? std::string(event->name) : std::string();
                std::string path = name.empty() ? parentPath : JoinPath(parentPath, name);
                bool isDir = (event->mask & IN_ISDIR) != 0;

                if ((event->mask & IN_CREATE) != 0) {
                    if (isDir) {
                        AddWatchesRecursive(fd, path, watches, watchLimitExceeded_);
                    }
                    onChange(FileChangeEvent{FileChangeType::Added, path, std::string()});
                } else if ((event->mask & IN_DELETE) != 0) {
                    // If `path` was itself a watched directory, its own
                    // watch generates IN_DELETE_SELF + IN_IGNORED
                    // independently, which the IN_IGNORED branch above
                    // cleans up from the table -- no extra bookkeeping
                    // needed here.
                    onChange(FileChangeEvent{FileChangeType::Removed, path, std::string()});
                } else if ((event->mask & IN_MOVED_FROM) != 0) {
                    pendingMovedFrom[event->cookie] = PendingMove{path, isDir};
                } else if ((event->mask & IN_MOVED_TO) != 0) {
                    auto pendingIt = pendingMovedFrom.find(event->cookie);
                    if (pendingIt != pendingMovedFrom.end()) {
                        onChange(
                            FileChangeEvent{FileChangeType::Renamed, path, pendingIt->second.path});
                        pendingMovedFrom.erase(pendingIt);
                    } else {
                        // Lone MOVED_TO: moved in from outside the watched
                        // tree (§7.2), report as Added.
                        onChange(FileChangeEvent{FileChangeType::Added, path, std::string()});
                    }
                    if (isDir) {
                        AddWatchesRecursive(fd, path, watches, watchLimitExceeded_);
                    }
                } else if ((event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) != 0) {
                    onChange(FileChangeEvent{FileChangeType::Modified, path, std::string()});
                }
            }
            nread = read(fd, buffer.data(), buffer.size());
        }

        // Anything left unmatched at the end of this batch moved outside
        // the watched tree (§7.2: "a lone IN_MOVED_FROM ... moved outside
        // the watched tree"): report as Removed, and if it was a directory,
        // stop watching it.
        for (const auto& cookieAndPending : pendingMovedFrom) {
            const PendingMove& pending = cookieAndPending.second;
            onChange(FileChangeEvent{FileChangeType::Removed, pending.path, std::string()});
            if (pending.isDir) {
                int wd = -1;
                if (watches.WdFor(pending.path, wd)) {
                    inotify_rm_watch(fd, wd);
                    watches.RemoveByWd(wd);
                }
            }
        }
    }

    close(fd);
}

bool InotifyWatcher::WatchLimitExceeded() const {
    return watchLimitExceeded_.load(std::memory_order_relaxed);
}

}  // namespace indexed
