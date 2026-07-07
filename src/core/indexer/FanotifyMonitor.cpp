#include "indexer/FanotifyMonitor.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace indexed {

namespace {

// How long each poll() waits before re-checking stopToken. Short enough that
// StartMonitoring reacts to a stop request promptly, long enough to not
// busy-loop.
constexpr int kPollTimeoutMs = 150;

// Read buffer for one or more batched fanotify_event_metadata records.
constexpr size_t kReadBufferSize = 4096;

// Maps a fanotify event mask to the FileChangeType it represents, per
// indexed-plan.md §7.2 and FanotifyMonitor's class-comment rename-correlation
// tradeoff. Returns nullopt for masks this monitor doesn't surface as a
// change (e.g. a lone FAN_ONDIR, or FAN_CLOSE_WRITE if ever requested).
// Checked in a fixed priority order since a single event's mask is not
// expected to carry more than one of these bits at once in practice.
std::optional<FileChangeType> ClassifyMask(uint64_t mask) {
    if (mask & FAN_CREATE) {
        return FileChangeType::Added;
    }
    if (mask & FAN_DELETE) {
        return FileChangeType::Removed;
    }
    if (mask & FAN_MOVED_FROM) {
        return FileChangeType::Removed;  // no rename correlation; see class comment
    }
    if (mask & FAN_MOVED_TO) {
        return FileChangeType::Added;  // no rename correlation; see class comment
    }
    if (mask & FAN_MODIFY) {
        return FileChangeType::Modified;
    }
    return std::nullopt;
}

// Finds the FAN_EVENT_INFO_TYPE_DFID_NAME info record within an event's
// variable-length info section (the bytes between the fixed
// fanotify_event_metadata header and the end of the event, per event_len).
// Returns nullptr if none is present or the section is malformed/truncated.
const fanotify_event_info_fid* FindDfidNameInfo(const fanotify_event_metadata* metadata) {
    if (metadata->event_len < metadata->metadata_len) {
        return nullptr;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(metadata);
    size_t offset = metadata->metadata_len;
    size_t total = metadata->event_len;

    while (offset + sizeof(fanotify_event_info_header) <= total) {
        const auto* hdr = reinterpret_cast<const fanotify_event_info_header*>(base + offset);
        if (hdr->len == 0 || offset + hdr->len > total) {
            return nullptr;  // malformed: zero-length or overruns the event
        }
        if (hdr->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
            return reinterpret_cast<const fanotify_event_info_fid*>(hdr);
        }
        offset += hdr->len;
    }
    return nullptr;
}

// Extracts the struct file_handle* and trailing name string out of a
// FAN_EVENT_INFO_TYPE_DFID_NAME info record. Returns nullopt if the record
// is too short to hold a well-formed file_handle + at least an empty name.
struct HandleAndName {
    const struct file_handle* handle = nullptr;
    std::string name;
};

std::optional<HandleAndName> ExtractHandleAndName(const fanotify_event_info_fid* fidInfo) {
    size_t recordLen = fidInfo->hdr.len;
    size_t prefixLen = offsetof(fanotify_event_info_fid, handle);
    if (recordLen < prefixLen + sizeof(struct file_handle)) {
        return std::nullopt;
    }
    const auto* handle = reinterpret_cast<const struct file_handle*>(fidInfo->handle);
    size_t handleTotal = sizeof(struct file_handle) + handle->handle_bytes;
    if (prefixLen + handleTotal > recordLen) {
        return std::nullopt;  // handle_bytes claims more than the record has
    }
    const char* namePtr = reinterpret_cast<const char*>(fidInfo->handle) + handleTotal;
    size_t nameCapacity = recordLen - prefixLen - handleTotal;
    // Name is null-terminated within the record; guard against a missing
    // terminator (malformed record) by bounding the scan to nameCapacity.
    size_t nameLen = 0;
    while (nameLen < nameCapacity && namePtr[nameLen] != '\0') {
        ++nameLen;
    }
    if (nameLen == nameCapacity) {
        return std::nullopt;  // no null terminator found within bounds
    }
    return HandleAndName{handle, std::string(namePtr, nameLen)};
}

// Joins a resolved directory path with a reported entry name.
std::string JoinPath(const std::string& dirPath, const std::string& name) {
    if (!dirPath.empty() && dirPath.back() == '/') {
        return dirPath + name;
    }
    return dirPath + "/" + name;
}

}  // namespace

std::string ProcFdFileHandleResolver::ResolveDirPath(int mountFd,
                                                     const struct file_handle* handle) const {
    if (handle == nullptr) {
        return "";
    }
    // open_by_handle_at takes a non-const handle pointer; the kernel doesn't
    // modify it, but the glibc prototype isn't const-qualified.
    int fd = open_by_handle_at(mountFd, const_cast<struct file_handle*>(handle), O_RDONLY);
    if (fd < 0) {
        return "";
    }
    char procPath[64];
    std::snprintf(procPath, sizeof(procPath), "/proc/self/fd/%d", fd);

    char resolved[PATH_MAX];
    ssize_t len = readlink(procPath, resolved, sizeof(resolved) - 1);
    close(fd);
    if (len < 0) {
        return "";
    }
    resolved[len] = '\0';
    return std::string(resolved);
}

FanotifyMonitor::FanotifyMonitor() : resolver_(std::make_shared<ProcFdFileHandleResolver>()) {}

FanotifyMonitor::FanotifyMonitor(std::shared_ptr<IFileHandleResolver> resolver)
    : resolver_(std::move(resolver)) {}

bool FanotifyMonitor::IsAvailable(const std::string& root) const {
    int fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME, O_RDONLY);
    if (fd < 0) {
        // EINVAL (kernel < 5.9, doesn't know FAN_REPORT_DFID_NAME) or
        // anything else -- unavailable. Note this step alone does NOT probe
        // CAP_SYS_ADMIN: since Linux 5.9, fanotify_init() succeeds for
        // unprivileged callers as long as an FID-reporting flag (FID,
        // DIR_FID, or as here DFID_NAME) is passed -- confirmed by direct
        // syscall testing in this sandbox, where this call succeeds despite
        // no CAP_SYS_ADMIN. The actual privileged operation StartMonitoring
        // needs -- FAN_MARK_ADD | FAN_MARK_FILESYSTEM -- is gated
        // separately, so it's probed below instead of relying on init alone.
        return false;
    }
    // FAN_MARK_FILESYSTEM (whole-filesystem marks, as StartMonitoring uses)
    // requires CAP_SYS_ADMIN even in the post-5.9 unprivileged-listener
    // world; this is where an unprivileged caller genuinely fails with
    // EPERM. Mark and immediately unmark `root` as a real, minimal probe
    // (closing fd also drops any leftover mark, so this is transient either
    // way).
    bool marked = fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_CREATE, AT_FDCWD,
                                root.c_str()) == 0;
    if (marked) {
        fanotify_mark(fd, FAN_MARK_REMOVE | FAN_MARK_FILESYSTEM, FAN_CREATE, AT_FDCWD,
                      root.c_str());
    }
    close(fd);
    return marked;
}

std::optional<FileChangeEvent> FanotifyMonitor::ParseFanotifyEvent(
    const fanotify_event_metadata* metadata, int mountFd) const {
    if (metadata == nullptr) {
        return std::nullopt;
    }
    std::optional<FileChangeType> type = ClassifyMask(metadata->mask);
    if (!type.has_value()) {
        return std::nullopt;
    }

    const fanotify_event_info_fid* fidInfo = FindDfidNameInfo(metadata);
    if (fidInfo == nullptr) {
        return std::nullopt;
    }
    std::optional<HandleAndName> handleAndName = ExtractHandleAndName(fidInfo);
    if (!handleAndName.has_value()) {
        return std::nullopt;
    }

    std::string dirPath =
        resolver_ ? resolver_->ResolveDirPath(mountFd, handleAndName->handle) : std::string();
    if (dirPath.empty()) {
        return std::nullopt;  // couldn't reconstruct the parent path
    }

    FileChangeEvent event;
    event.type = *type;
    event.path = JoinPath(dirPath, handleAndName->name);
    return event;
}

void FanotifyMonitor::StartMonitoring(const std::string& root, ChangeCallback onChange,
                                      const std::atomic<bool>& stopToken) {
    int fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME, O_RDONLY);
    if (fd < 0) {
        // Unavailable (no CAP_SYS_ADMIN, unsupported kernel, etc.) -- return
        // promptly rather than attempting fanotify_mark. See class comment:
        // callers don't need to check IsAvailable() themselves first.
        return;
    }

    uint64_t mask =
        FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO | FAN_MODIFY | FAN_ONDIR;
    if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask, AT_FDCWD, root.c_str()) < 0) {
        close(fd);
        return;
    }

    int mountFd = open(root.c_str(), O_RDONLY);
    if (mountFd < 0) {
        close(fd);
        return;
    }

    std::vector<unsigned char> buffer(kReadBufferSize);
    while (!stopToken.load()) {
        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pollResult = poll(&pfd, 1, kPollTimeoutMs);
        if (pollResult <= 0) {
            continue;  // timeout or interrupted/error: loop back to check stopToken
        }

        ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
        if (bytesRead <= 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            break;  // unexpected read error: stop monitoring
        }

        const auto* metadata = reinterpret_cast<const fanotify_event_metadata*>(buffer.data());
        ssize_t remaining = bytesRead;
        while (FAN_EVENT_OK(metadata, remaining)) {
            if (metadata->vers == FANOTIFY_METADATA_VERSION) {
                std::optional<FileChangeEvent> event = ParseFanotifyEvent(metadata, mountFd);
                if (event.has_value() && onChange) {
                    onChange(*event);
                }
            }
            if (metadata->fd >= 0) {
                close(metadata->fd);
            }
            metadata = FAN_EVENT_NEXT(metadata, remaining);
        }
    }

    close(mountFd);
    close(fd);
}

}  // namespace indexed
