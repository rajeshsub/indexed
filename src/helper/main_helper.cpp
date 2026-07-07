// indexed-helper: the privileged scanning + live-monitoring process
// (indexed-plan.md §9). Launched via `pkexec indexed-helper` by the GUI's
// "Elevate for full-system access" action; never launched directly by a
// user. Runs as root for the duration of the GUI session: performs the
// initial (or forced) scan, then holds live monitoring (fanotify preferred,
// inotify fallback) until SIGTERM.

#include <fcntl.h>
#include <unistd.h>

#include "Version.h"
#include "indexer/FanotifyMonitor.h"
#include "indexer/IChangeMonitor.h"
#include "indexer/Indexer.h"
#include "indexer/InotifyWatcher.h"
#include "indexer/StatusFile.h"
#include "indexer/WalkScanner.h"
#include "platform/Elevation.h"
#include "settings/Logger.h"
#include "settings/PathUtils.h"
#include "settings/Settings.h"
#include "storage/IndexStore.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace indexed;

// Signal handlers only touch async-signal-safe atomics; all real work
// happens on the main loop that polls these flags (indexed-plan.md §9.3).
std::atomic<bool> g_stop{false};
std::atomic<bool> g_reloadSettings{false};
std::atomic<bool> g_reindexNow{false};

extern "C" void HandleSignal(int sig) {
    switch (sig) {
        case SIGTERM:
            g_stop.store(true);
            break;
        case SIGHUP:
            g_reloadSettings.store(true);
            break;
        case SIGUSR1:
            g_reindexNow.store(true);
            break;
        default:
            break;
    }
}

void InstallSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = HandleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
}

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// Opens `path` for root-write via Elevation::OpenForRootWrite and holds the
// resulting fd for the guard's lifetime. FdPath() returns "/proc/self/fd/N",
// which existing path-based APIs (IndexSerializer::Save/Load via Indexer,
// Logger::Log) can be pointed at instead of the real path: a *second*, later
// open() of that magic symlink resolves to the exact already-validated inode
// this guard opened, not to whatever `path` currently names on disk -- so a
// symlink swapped in after this constructor runs cannot redirect any
// subsequent write, closing the TOCTOU gap without needing to modify those
// APIs to accept a raw fd (indexed-plan.md §9.2, docs/adr/0008).
class RootWriteGuard {
public:
    RootWriteGuard(const std::string& path, uid_t targetUid, int flags, mode_t mode) {
        const std::string baseDir = std::filesystem::path(path).parent_path().string();
        error_ = OpenForRootWrite(path, targetUid, baseDir, flags, mode, &fd_);
    }

    ~RootWriteGuard() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    RootWriteGuard(const RootWriteGuard&) = delete;
    RootWriteGuard& operator=(const RootWriteGuard&) = delete;

    bool Ok() const { return error_ == ElevationError::kNone; }
    ElevationError Error() const { return error_; }
    std::string FdPath() const { return "/proc/self/fd/" + std::to_string(fd_); }

private:
    int fd_ = -1;
    ElevationError error_ = ElevationError::kOpenFailed;
};

const char* ElevationErrorName(ElevationError error) {
    switch (error) {
        case ElevationError::kNone:
            return "none";
        case ElevationError::kPathNotUnderBase:
            return "path not under base directory";
        case ElevationError::kSymlinkInPath:
            return "symlink in path";
        case ElevationError::kOwnershipMismatch:
            return "ownership mismatch";
        case ElevationError::kStatFailed:
            return "stat failed (missing directory?)";
        case ElevationError::kOpenFailed:
            return "open failed";
    }
    return "unknown";
}

// Prefers fanotify (whole-mount, this process's privilege level permitting)
// and falls back to inotify per root, matching the selection policy in
// docs/adr/0007-fanotify-vs-inotify-monitoring.md. A null return means
// neither backend is usable for that root; Indexer::StartLiveMonitoring
// already treats a null monitor as "skip this root" (rescan-only degraded
// mode), so no special handling is needed here.
std::unique_ptr<IChangeMonitor> ChooseMonitor(const std::string& root) {
    auto fanotify = std::make_unique<FanotifyMonitor>();
    if (fanotify->IsAvailable(root)) {
        return fanotify;
    }
    auto inotify = std::make_unique<InotifyWatcher>();
    if (inotify->IsAvailable(root)) {
        return inotify;
    }
    return nullptr;
}

}  // namespace

int main() {
    std::optional<TargetUser> targetUser = ResolveTargetUser();
    if (!targetUser) {
        std::fprintf(stderr,
                     "indexed-helper: PKEXEC_UID missing or invalid -- this binary must be "
                     "launched via `pkexec indexed-helper`, never directly.\n");
        return 1;
    }

    // Never read $HOME/$XDG_* here (docs/adr/0008): force the default XDG
    // derivation from the PKEXEC_UID-resolved home directory instead of any
    // inherited environment variable, spoofable or not.
    XdgEnv xdg;
    xdg.home = targetUser->homeDir;
    const DataDirs dirs = ResolveDataDirs(ExecutableDir(), xdg);
    const std::string statusPath =
        (std::filesystem::path(dirs.indexPath).parent_path() / "indexed.status").string();

    // Precondition: the GUI's own unprivileged startup (main.cpp) already
    // created these XDG directories via EnsureDirectory before a user could
    // ever reach the "Elevate" action that launches this binary. The helper
    // deliberately never creates directories itself as root -- only writes
    // files into directories it can prove are already owned by the target
    // user (Elevation::OpenForRootWrite's contract).
    RootWriteGuard idxGuard(dirs.indexPath, targetUser->uid, O_RDWR | O_CREAT, 0600);
    if (!idxGuard.Ok()) {
        std::fprintf(stderr, "indexed-helper: refusing to open index file %s: %s\n",
                     dirs.indexPath.c_str(), ElevationErrorName(idxGuard.Error()));
        return 1;
    }
    RootWriteGuard logGuard(dirs.logPath, targetUser->uid, O_WRONLY | O_CREAT, 0600);
    if (!logGuard.Ok()) {
        std::fprintf(stderr, "indexed-helper: refusing to open log file %s: %s\n",
                     dirs.logPath.c_str(), ElevationErrorName(logGuard.Error()));
        return 1;
    }

    Logger logger(logGuard.FdPath());
    logger.Log("indexed-helper starting, target user " + targetUser->username);

    Settings settings(dirs.configPath, targetUser->homeDir);
    settings.Load();

    WalkScanner scanner;
    IndexStore store;

    auto writeStatus = [&](const IndexerStatus& status) {
        RootWriteGuard statusGuard(statusPath, targetUser->uid, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!statusGuard.Ok()) {
            return;  // best-effort; a missed progress update isn't fatal
        }
        const std::string text = SerializeStatus(status);
        int fd = open(statusGuard.FdPath().c_str(), O_WRONLY | O_TRUNC);
        if (fd >= 0) {
            [[maybe_unused]] ssize_t written = write(fd, text.data(), text.size());
            close(fd);
        }
    };

    Indexer indexer(scanner, store, ChooseMonitor, writeStatus);
    InstallSignalHandlers();

    auto currentOptions = [&]() {
        ScanOptions options;
        options.rootPaths = settings.SelectedRoots();
        options.excludedPaths = settings.ExcludedPaths();
        return options;
    };
    auto staleThresholdSeconds = [&]() {
        return static_cast<uint64_t>(settings.ReindexIntervalHours()) * 3600ULL;
    };

    indexer.StartIndexing(/*force=*/false, currentOptions(), idxGuard.FdPath(), NowNs(),
                          staleThresholdSeconds());
    logger.Log("initial indexing complete, starting live monitoring");

    std::atomic<bool> monitorStop{false};
    std::thread monitorThread(
        [&]() { indexer.StartLiveMonitoring(currentOptions().rootPaths, monitorStop); });

    while (!g_stop.load()) {
        if (g_reloadSettings.exchange(false)) {
            settings.Load();
            logger.Log("settings reloaded, rebuilding index");
            monitorStop.store(true);
            monitorThread.join();
            indexer.StartIndexing(/*force=*/true, currentOptions(), idxGuard.FdPath(), NowNs(),
                                  staleThresholdSeconds());
            monitorStop.store(false);
            monitorThread = std::thread(
                [&]() { indexer.StartLiveMonitoring(currentOptions().rootPaths, monitorStop); });
        }
        if (g_reindexNow.exchange(false)) {
            logger.Log("reindex requested");
            monitorStop.store(true);
            monitorThread.join();
            indexer.StartIndexing(/*force=*/true, currentOptions(), idxGuard.FdPath(), NowNs(),
                                  staleThresholdSeconds());
            monitorStop.store(false);
            monitorThread = std::thread(
                [&]() { indexer.StartLiveMonitoring(currentOptions().rootPaths, monitorStop); });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    logger.Log("SIGTERM received, stopping monitoring and exiting");
    monitorStop.store(true);
    monitorThread.join();

    return 0;
}
