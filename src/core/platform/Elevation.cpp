#include "platform/Elevation.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <vector>

namespace indexed {

namespace fs = std::filesystem;

std::optional<TargetUser> ResolveTargetUser() {
    // PKEXEC_UID is set by polkit itself on the helper's environment -- it
    // is NOT the environment the invoking (potentially malicious) process
    // controls the way it controls $HOME/$XDG_CONFIG_HOME/etc. Never fall
    // back to those.
    const char* pkexecUid = std::getenv("PKEXEC_UID");
    if (pkexecUid == nullptr || pkexecUid[0] == '\0') {
        return std::nullopt;
    }

    // Strict parse: the whole string must be a valid non-negative integer,
    // no partial parse ("123abc"), no negative numbers, no empty string.
    errno = 0;
    char* end = nullptr;
    long uidLong = std::strtol(pkexecUid, &end, 10);
    if (errno != 0 || end == pkexecUid || *end != '\0' || uidLong < 0) {
        return std::nullopt;
    }
    uid_t uid = static_cast<uid_t>(uidLong);

    // getpwuid_r rather than getpwuid: avoids the shared static-buffer
    // return of getpwuid, which is unnecessary risk in security-critical
    // code even though this process is single-threaded today.
    struct passwd pwd{};
    struct passwd* result = nullptr;
    std::vector<char> buf(16384);
    int rc = getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result);
    if (rc != 0 || result == nullptr) {
        return std::nullopt;
    }

    TargetUser user;
    user.uid = uid;
    user.gid = pwd.pw_gid;
    user.homeDir = (pwd.pw_dir != nullptr) ? pwd.pw_dir : "";
    user.username = (pwd.pw_name != nullptr) ? pwd.pw_name : "";
    return user;
}

namespace {

// Classifies why opening directory component `name` (relative to parentFd,
// or an absolute path when parentFd is AT_FDCWD) failed. Only used to pick
// an error code after the open has already been refused, so the lstat race
// here has no security impact.
ElevationError ClassifyDirectoryOpenFailure(int parentFd, const std::string& name) {
    struct stat st{};
    if (fstatat(parentFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return ElevationError::kStatFailed;
    }
    return S_ISLNK(st.st_mode) ? ElevationError::kSymlinkInPath : ElevationError::kStatFailed;
}

// Opens one directory with O_NOFOLLOW (relative to parentFd) and requires
// it, by fstat on the opened fd, to be a directory owned by targetUid.
ElevationError OpenOwnedDirectory(int parentFd, const std::string& name, uid_t targetUid,
                                  int* outFd) {
    const int fd = openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return ClassifyDirectoryOpenFailure(parentFd, name);
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return ElevationError::kStatFailed;
    }
    if (st.st_uid != targetUid) {
        close(fd);
        return ElevationError::kOwnershipMismatch;
    }
    *outFd = fd;
    return ElevationError::kNone;
}

// The directory half of every primitive below: `path` must be lexically
// under `baseDir`, then baseDir and each directory down to path's parent is
// opened relative to the previous one's fd (never re-resolved by path) and
// must be a real directory owned by targetUid. Because each step is an
// openat(O_NOFOLLOW) on an fd that was itself checked, swapping any
// component for a symlink at any moment is refused rather than followed.
// On success *outDirFd is the parent directory (caller closes) and
// *outName the final component.
ElevationError OpenValidatedParent(const std::string& path, uid_t targetUid,
                                   const std::string& baseDir, int* outDirFd,
                                   std::string* outName) {
    // Normalize lexically -- resolving symlinks here would defeat the point.
    const std::string base = fs::path(baseDir).lexically_normal().string();
    const std::string target = fs::path(path).lexically_normal().string();
    if (target.size() <= base.size() || target.compare(0, base.size(), base) != 0 ||
        target[base.size()] != '/') {
        return ElevationError::kPathNotUnderBase;
    }

    const fs::path relative(target.substr(base.size() + 1));
    std::vector<std::string> components;
    std::transform(relative.begin(), relative.end(), std::back_inserter(components),
                   [](const fs::path& part) { return part.string(); });
    if (components.empty()) {
        return ElevationError::kPathNotUnderBase;
    }

    int dirFd = -1;
    ElevationError error = OpenOwnedDirectory(AT_FDCWD, base, targetUid, &dirFd);
    if (error != ElevationError::kNone) {
        return error;
    }
    for (size_t i = 0; i + 1 < components.size(); ++i) {
        int nextFd = -1;
        error = OpenOwnedDirectory(dirFd, components[i], targetUid, &nextFd);
        close(dirFd);
        if (error != ElevationError::kNone) {
            return error;
        }
        dirFd = nextFd;
    }

    *outDirFd = dirFd;
    *outName = components.back();
    return ElevationError::kNone;
}

bool WriteAll(int fd, std::string_view contents) {
    while (!contents.empty()) {
        const ssize_t written = write(fd, contents.data(), contents.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        contents.remove_prefix(static_cast<size_t>(written));
    }
    return true;
}

}  // namespace

ElevationError OpenForRootWrite(const std::string& path, uid_t targetUid,
                                const std::string& baseDir, int flags, mode_t mode, int* outFd) {
    int dirFd = -1;
    std::string name;
    ElevationError error = OpenValidatedParent(path, targetUid, baseDir, &dirFd, &name);
    if (error != ElevationError::kNone) {
        return error;
    }

    // O_TRUNC is applied only after the checks below, so a hard link or
    // other unexpected file is refused before a single byte of it changes.
    // O_NOFOLLOW refuses a symlink at the final component; O_NONBLOCK keeps
    // a FIFO from blocking the open (it is then refused as non-regular).
    const int openFlags = (flags & ~(O_TRUNC | O_CREAT)) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    bool created = false;
    int fd = openat(dirFd, name.c_str(), openFlags);
    if (fd < 0 && errno == ENOENT && (flags & O_CREAT) != 0) {
        fd = openat(dirFd, name.c_str(), openFlags | O_CREAT | O_EXCL, mode);
        created = fd >= 0;
    }
    if (fd < 0) {
        const bool symlink = errno == ELOOP;
        close(dirFd);
        return symlink ? ElevationError::kSymlinkInPath : ElevationError::kOpenFailed;
    }

    struct stat st{};
    const bool acceptable = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
                            (created || st.st_uid == targetUid);
    if (!acceptable) {
        close(fd);
        if (created) {
            unlinkat(dirFd, name.c_str(), 0);
        }
        close(dirFd);
        return ElevationError::kNotRegularFile;
    }
    // A file root just created belongs to the target user, like one the
    // unprivileged GUI would have created itself.
    const bool prepared = (!created || fchown(fd, targetUid, static_cast<gid_t>(-1)) == 0) &&
                          ((flags & O_TRUNC) == 0 || ftruncate(fd, 0) == 0) &&
                          fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) == 0;
    if (!prepared) {
        close(fd);
        if (created) {
            unlinkat(dirFd, name.c_str(), 0);
        }
        close(dirFd);
        return ElevationError::kOpenFailed;
    }

    close(dirFd);
    *outFd = fd;
    return ElevationError::kNone;
}

ElevationError RemoveFileForRootWrite(const std::string& path, uid_t targetUid,
                                      const std::string& baseDir) {
    int dirFd = -1;
    std::string name;
    ElevationError error = OpenValidatedParent(path, targetUid, baseDir, &dirFd, &name);
    if (error != ElevationError::kNone) {
        return error;
    }
    const bool removed = unlinkat(dirFd, name.c_str(), 0) == 0 || errno == ENOENT;
    close(dirFd);
    return removed ? ElevationError::kNone : ElevationError::kWriteFailed;
}

ElevationError OpenRegularFileForRootRead(const std::string& path, uid_t targetUid,
                                          const std::string& baseDir, int* outFd) {
    return OpenForRootWrite(path, targetUid, baseDir, O_RDONLY, 0, outFd);
}

ElevationError ReplaceFileForRootWrite(const std::string& path, uid_t targetUid, gid_t targetGid,
                                       const std::string& baseDir, std::string_view contents,
                                       ReplaceDurability durability) {
    int dirFd = -1;
    std::string name;
    ElevationError error = OpenValidatedParent(path, targetUid, baseDir, &dirFd, &name);
    if (error != ElevationError::kNone) {
        return error;
    }
    const std::string tempName = name + ".root.tmp";
    const bool synced = durability == ReplaceDurability::kSynced;

    // A temp file left by a crashed save would make O_EXCL fail forever.
    // unlinkat removes a symlink itself, never what it points to.
    unlinkat(dirFd, tempName.c_str(), 0);
    const int tempFd =
        openat(dirFd, tempName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (tempFd < 0) {
        close(dirFd);
        return ElevationError::kOpenFailed;
    }

    const bool written = WriteAll(tempFd, contents) && fchown(tempFd, targetUid, targetGid) == 0 &&
                         fchmod(tempFd, 0600) == 0 && (!synced || fsync(tempFd) == 0);
    close(tempFd);
    if (!written || renameat(dirFd, tempName.c_str(), dirFd, name.c_str()) != 0) {
        unlinkat(dirFd, tempName.c_str(), 0);
        close(dirFd);
        return ElevationError::kWriteFailed;
    }

    const bool dirSynced = !synced || fsync(dirFd) == 0;
    close(dirFd);
    return dirSynced ? ElevationError::kNone : ElevationError::kWriteFailed;
}

}  // namespace indexed
