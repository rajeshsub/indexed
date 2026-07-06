#include "indexer/WalkScanner.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace indexed {

namespace {

constexpr size_t kGetdentsBufferSize = 65536;
constexpr auto kQueuePollInterval = std::chrono::milliseconds(5);

std::string StripTrailingSlash(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

// Resolves `path` to a canonical absolute form via realpath() when possible
// (matches indexed-plan.md §7.1: "compare on canonical absolute paths").
// Falls back to a trailing-slash-normalized copy of the input when the path
// does not (yet) resolve, so callers can still compare consistently.
std::string CanonicalizeBestEffort(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (resolved != nullptr) {
        std::string result(resolved);
        std::free(resolved);
        return StripTrailingSlash(result);
    }
    return StripTrailingSlash(path);
}

std::string JoinPath(const std::string& dir, std::string_view name) {
    if (dir.size() == 1 && dir.front() == '/') {
        return "/" + std::string(name);
    }
    return dir + "/" + std::string(name);
}

bool IsPathUnderOrEqual(std::string_view path, const std::string& excluded) {
    if (path == excluded) {
        return true;
    }
    return path.size() > excluded.size() && path.compare(0, excluded.size(), excluded) == 0 &&
           path[excluded.size()] == '/';
}

bool IsPathExcluded(std::string_view path, const std::vector<std::string>& excludedNormalized) {
    return std::any_of(
        excludedNormalized.begin(), excludedNormalized.end(),
        [&](const std::string& excluded) { return IsPathUnderOrEqual(path, excluded); });
}

// One pending directory to walk. `allowedDev` is the st_dev boundary its
// direct children are compared against: children on a different device are
// a mount point and are pruned (entry still emitted, not descended into)
// unless that child is itself one of the caller's selected roots.
struct DirJob {
    std::string path;
    dev_t allowedDev;
};

// Shared work queue draining directory jobs across worker threads. This is a
// single shared queue rather than per-thread deques with explicit stealing:
// at directory-job granularity a shared queue already load-balances well
// without the added complexity, while still satisfying "work-stealing pool"
// in spirit (any idle worker can immediately pick up any pending job).
class DirectoryQueue {
public:
    void Push(DirJob job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    // Returns nullopt once there is no more work anywhere (queue empty and
    // no job currently being processed by any worker) or cancellation has
    // been requested. Polls cancelToken on a short interval so external
    // cancellation is noticed promptly even without a matching Push/Done.
    std::optional<DirJob> Pop(const std::atomic<bool>& cancelToken) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (cancelToken.load(std::memory_order_relaxed)) {
                return std::nullopt;
            }
            if (!jobs_.empty()) {
                DirJob job = std::move(jobs_.front());
                jobs_.pop_front();
                ++inFlight_;
                return job;
            }
            if (inFlight_ == 0) {
                return std::nullopt;
            }
            cv_.wait_for(lock, kQueuePollInterval);
        }
    }

    void Done() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --inFlight_;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<DirJob> jobs_;
    int inFlight_ = 0;
};

void ProcessDirectory(const DirJob& job, const std::vector<std::string>& canonicalRoots,
                      const std::vector<std::string>& excludedNormalized, DirectoryQueue& queue,
                      const ScanCallback& onEntry, const ProgressCallback& onProgress,
                      std::atomic<uint64_t>& filesFound, const std::atomic<bool>& cancelToken) {
    int fd = open(job.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }

    std::vector<char> buffer(kGetdentsBufferSize);
    while (!cancelToken.load(std::memory_order_relaxed)) {
        long nread = syscall(SYS_getdents64, fd, buffer.data(), buffer.size());
        if (nread <= 0) {
            break;
        }

        long offset = 0;
        while (offset < nread) {
            auto* entry = reinterpret_cast<struct dirent64*>(buffer.data() + offset);
            if (entry->d_reclen == 0) {
                break;  // defensive: malformed record, avoid an infinite loop
            }
            offset += entry->d_reclen;

            std::string_view name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            if (entry->d_type == DT_LNK) {
                continue;  // fast path: known symlink, never followed/emitted
            }

            struct statx stx{};
            int statxRc = statx(fd, entry->d_name, AT_STATX_DONT_SYNC | AT_SYMLINK_NOFOLLOW,
                                STATX_SIZE | STATX_MTIME | STATX_TYPE | STATX_MODE, &stx);
            if (statxRc != 0) {
                continue;  // vanished/inaccessible between listing and stat; skip
            }
            if (S_ISLNK(stx.stx_mode)) {
                continue;  // d_type was DT_UNKNOWN; statx is the authority
            }

            std::string childPath = JoinPath(job.path, name);
            if (IsPathExcluded(childPath, excludedNormalized)) {
                continue;
            }

            bool isDir = S_ISDIR(stx.stx_mode);
            uint32_t attributes = 0;
            if (isDir) {
                attributes |= kAttrDirectory;
            }
            if (!name.empty() && name.front() == '.') {
                attributes |= kAttrHidden;
            }

            FileEntry fileEntry;
            fileEntry.name = std::string(name);
            fileEntry.path = childPath;
            fileEntry.size = stx.stx_size;
            fileEntry.lastModified =
                static_cast<uint64_t>(stx.stx_mtime.tv_sec) * 1'000'000'000ULL +
                static_cast<uint64_t>(stx.stx_mtime.tv_nsec);
            fileEntry.attributes = attributes;

            onEntry(fileEntry);
            filesFound.fetch_add(1, std::memory_order_relaxed);

            if (isDir) {
                dev_t childDev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
                bool isExplicitRoot = std::find(canonicalRoots.begin(), canonicalRoots.end(),
                                                childPath) != canonicalRoots.end();
                if (isExplicitRoot || childDev == job.allowedDev) {
                    queue.Push(DirJob{childPath, isExplicitRoot ? childDev : job.allowedDev});
                }
            }
        }
    }

    close(fd);
    onProgress(filesFound.load(std::memory_order_relaxed), job.path);
}

}  // namespace

bool WalkScanner::FastScanAvailable(const std::string& /*root*/) const {
    return false;
}

void WalkScanner::Scan(const ScanOptions& options, ScanCallback onEntry,
                       ProgressCallback onProgress, const std::atomic<bool>& cancelToken) {
    std::vector<std::string> canonicalRoots(options.rootPaths.size());
    std::transform(options.rootPaths.begin(), options.rootPaths.end(), canonicalRoots.begin(),
                   CanonicalizeBestEffort);

    std::vector<std::string> excludedNormalized(options.excludedPaths.size());
    std::transform(options.excludedPaths.begin(), options.excludedPaths.end(),
                   excludedNormalized.begin(), CanonicalizeBestEffort);

    DirectoryQueue queue;
    for (const auto& root : canonicalRoots) {
        if (IsPathExcluded(root, excludedNormalized)) {
            continue;
        }
        struct statx stx{};
        if (statx(AT_FDCWD, root.c_str(), AT_STATX_DONT_SYNC, STATX_TYPE, &stx) != 0) {
            continue;  // root does not exist / not accessible; skip it
        }
        dev_t rootDev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
        queue.Push(DirJob{root, rootDev});
    }

    std::atomic<uint64_t> filesFound{0};
    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) {
        workerCount = 1;
    }

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned int i = 0; i < workerCount; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                std::optional<DirJob> job = queue.Pop(cancelToken);
                if (!job.has_value()) {
                    break;
                }
                ProcessDirectory(*job, canonicalRoots, excludedNormalized, queue, onEntry,
                                 onProgress, filesFound, cancelToken);
                queue.Done();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

}  // namespace indexed
