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
#include "indexer/SaveThrottle.h"
#include "indexer/StatusFile.h"
#include "indexer/WalkScanner.h"
#include "platform/Elevation.h"
#include "settings/HelperSettings.h"
#include "settings/Logger.h"
#include "settings/PathUtils.h"
#include "settings/Settings.h"
#include "storage/IndexSerializer.h"
#include "storage/IndexStore.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
// which a path-based API that appends or rewrites in place (Logger::Log) can
// be pointed at instead of the real path: a *second*, later
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
        case ElevationError::kWriteFailed:
            return "write, sync or rename failed";
        case ElevationError::kNotRegularFile:
            return "not a regular file owned by the target user";
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
    //
    // The index is not opened here: every access goes through the hardened
    // loader/saver below, which validate on each call (saves replace the file
    // by rename, so an fd held from startup would point at a stale inode).
    std::optional<RootWriteGuard> logGuard;
    logGuard.emplace(dirs.logPath, targetUser->uid, O_WRONLY | O_CREAT, 0600);
    if (logGuard->Error() == ElevationError::kNotRegularFile) {
        // indexed 0.3.1 and earlier created the log as root; it now has to be
        // the target user's own file, so clear it and let it be recreated.
        const std::string logDir = std::filesystem::path(dirs.logPath).parent_path().string();
        if (RemoveFileForRootWrite(dirs.logPath, targetUser->uid, logDir) ==
            ElevationError::kNone) {
            logGuard.emplace(dirs.logPath, targetUser->uid, O_WRONLY | O_CREAT, 0600);
        }
    }
    if (!logGuard->Ok()) {
        std::fprintf(stderr, "indexed-helper: refusing to open log file %s: %s\n",
                     dirs.logPath.c_str(), ElevationErrorName(logGuard->Error()));
        return 1;
    }

    Settings settings(dirs.configPath, targetUser->homeDir);
    settings.SetExcludedPaths(Settings::DefaultExcludedPaths(targetUser->homeDir));
    const ElevationError settingsError =
        LoadSettingsForRoot(dirs.configPath, *targetUser, settings);
    if (settingsError != ElevationError::kNone && settingsError != ElevationError::kOpenFailed) {
        std::fprintf(stderr, "indexed-helper: refusing to read settings %s: %s; using defaults\n",
                     dirs.configPath.c_str(), ElevationErrorName(settingsError));
    }

    Logger logger(logGuard->FdPath(), settings.LogLevel());
    // Process start: a security-relevant state change for a root-running
    // process, logged at Warning so it survives the default threshold
    // (docs/adr/0009) rather than only appearing in verbose mode.
    logger.Log("indexed-helper starting, target user " + targetUser->username, LogLevel::Warning);

    WalkScanner scanner;
    IndexStore store;

    // Replaced by rename rather than truncated in place, so the GUI never
    // reads a half-written status; unsynced, since it is display-only. Scan
    // progress arrives per directory from every WalkScanner worker thread at
    // once: concurrent replacements of one path would share a temp file,
    // hence the mutex, and the GUI only needs a few updates a second, so
    // progress is written at most every 100 ms (state changes always are).
    const std::string statusBaseDir = std::filesystem::path(statusPath).parent_path().string();
    constexpr auto kScanStatusInterval = std::chrono::milliseconds(100);
    std::mutex statusMutex;
    std::chrono::steady_clock::time_point lastScanStatus{};
    auto writeStatus = [&](const IndexerStatus& status) {
        const std::string text = SerializeStatus(status);
        std::lock_guard<std::mutex> lock(statusMutex);
        if (status.state == IndexerState::Scanning) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastScanStatus < kScanStatusInterval) {
                return;
            }
            lastScanStatus = now;
        }
        // Best-effort: a missed progress update isn't fatal.
        ReplaceFileForRootWrite(statusPath, targetUser->uid, targetUser->gid, statusBaseDir, text,
                                ReplaceDurability::kUnsynced);
    };

    const std::string indexBaseDir = std::filesystem::path(dirs.indexPath).parent_path().string();
    IndexFileIo indexFileIo;
    indexFileIo.save = [&](const std::string& idxFilePath, const std::vector<char>& bytes) {
        const ElevationError error =
            ReplaceFileForRootWrite(idxFilePath, targetUser->uid, targetUser->gid, indexBaseDir,
                                    std::string_view(bytes.data(), bytes.size()));
        if (error != ElevationError::kNone) {
            logger.Log(std::string("failed to save index: ") + ElevationErrorName(error),
                       LogLevel::Error);
            return false;
        }
        return true;
    };
    indexFileIo.load = [&](const std::string& idxFilePath) {
        int fd = -1;
        const ElevationError error =
            OpenRegularFileForRootRead(idxFilePath, targetUser->uid, indexBaseDir, &fd);
        if (error != ElevationError::kNone) {
            // A missing index is normal (first elevation); anything else means
            // something unexpected sits at the index path.
            if (error != ElevationError::kOpenFailed) {
                logger.Log(std::string("refusing to load index: ") + ElevationErrorName(error),
                           LogLevel::Warning);
            }
            return IndexSerializer::LoadResult{};
        }
        IndexSerializer::LoadResult result =
            IndexSerializer::Load("/proc/self/fd/" + std::to_string(fd));
        close(fd);
        return result;
    };

    // Live-monitoring changes only reach the GUI through the index file, so
    // they are saved too, leaving an idle gap after each save of at least 2 s
    // and at least 5x that save's duration (docs/adr/0014).
    constexpr uint64_t kLiveSaveIntervalNs = 2'000'000'000ULL;
    SaveThrottle liveSaves(kLiveSaveIntervalNs, NowNs());
    Indexer indexer(
        scanner, store, ChooseMonitor, writeStatus, [&liveSaves]() { liveSaves.MarkDirty(); },
        indexFileIo);
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

    indexer.StartIndexing(/*force=*/false, currentOptions(), dirs.indexPath, NowNs(),
                          staleThresholdSeconds());
    liveSaves.MarkSaved(NowNs());
    logger.Log("initial indexing complete, starting live monitoring");

    std::atomic<bool> monitorStop{false};
    std::thread monitorThread(
        [&]() { indexer.StartLiveMonitoring(currentOptions().rootPaths, monitorStop); });

    while (!g_stop.load()) {
        if (g_reloadSettings.exchange(false)) {
            const ElevationError reloadError =
                LoadSettingsForRoot(dirs.configPath, *targetUser, settings);
            if (reloadError != ElevationError::kNone &&
                reloadError != ElevationError::kOpenFailed) {
                logger.Log(std::string("refusing to read settings, keeping previous: ") +
                               ElevationErrorName(reloadError),
                           LogLevel::Warning);
            }
            logger.Log("settings reloaded, rebuilding index");
            monitorStop.store(true);
            monitorThread.join();
            indexer.StartIndexing(/*force=*/true, currentOptions(), dirs.indexPath, NowNs(),
                                  staleThresholdSeconds());
            liveSaves.MarkSaved(NowNs());
            monitorStop.store(false);
            monitorThread = std::thread(
                [&]() { indexer.StartLiveMonitoring(currentOptions().rootPaths, monitorStop); });
        }
        if (g_reindexNow.exchange(false)) {
            logger.Log("reindex requested");
            monitorStop.store(true);
            monitorThread.join();
            indexer.StartIndexing(/*force=*/true, currentOptions(), dirs.indexPath, NowNs(),
                                  staleThresholdSeconds());
            liveSaves.MarkSaved(NowNs());
            monitorStop.store(false);
            monitorThread = std::thread(
                [&]() { indexer.StartLiveMonitoring(currentOptions().rootPaths, monitorStop); });
        }
        if (liveSaves.ShouldSave(NowNs())) {
            const uint64_t saveStartNs = NowNs();
            if (!indexer.PersistLiveChanges(dirs.indexPath)) {
                liveSaves.MarkDirty();  // retry on the next interval
            }
            liveSaves.RecordSaveDuration(NowNs() - saveStartNs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Process stop: matches the Warning-level start line above.
    logger.Log("SIGTERM received, stopping monitoring and exiting", LogLevel::Warning);
    monitorStop.store(true);
    monitorThread.join();
    if (liveSaves.HasPendingChanges()) {
        indexer.PersistLiveChanges(dirs.indexPath);
    }

    return 0;
}
