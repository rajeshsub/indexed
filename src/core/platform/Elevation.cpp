#include "platform/Elevation.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
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
    user.homeDir = (pwd.pw_dir != nullptr) ? pwd.pw_dir : "";
    user.username = (pwd.pw_name != nullptr) ? pwd.pw_name : "";
    return user;
}

namespace {

// Ownership+symlink check for one directory component that must already
// exist. Uses lstat (not stat) so a symlinked directory component is
// caught, not just a symlinked final file.
ElevationError CheckExistingDirectoryOwnership(const std::string& dirPath, uid_t targetUid) {
    struct stat st{};
    if (lstat(dirPath.c_str(), &st) != 0) {
        return ElevationError::kStatFailed;
    }
    if (S_ISLNK(st.st_mode)) {
        return ElevationError::kSymlinkInPath;
    }
    if (!S_ISDIR(st.st_mode)) {
        return ElevationError::kStatFailed;
    }
    if (st.st_uid != targetUid) {
        return ElevationError::kOwnershipMismatch;
    }
    return ElevationError::kNone;
}

}  // namespace

ElevationError OpenForRootWrite(const std::string& path, uid_t targetUid,
                                const std::string& baseDir, int flags, mode_t mode, int* outFd) {
    // Normalize (lexically -- no symlink resolution, which would defeat the
    // whole point) so the prefix check below is robust to things like a
    // trailing slash on baseDir.
    std::string base = fs::path(baseDir).lexically_normal().string();
    std::string target = fs::path(path).lexically_normal().string();

    if (target.size() <= base.size() || target.compare(0, base.size(), base) != 0 ||
        target[base.size()] != '/') {
        return ElevationError::kPathNotUnderBase;
    }

    // baseDir itself is the first component that must exist, be a real
    // directory, and be owned by targetUid.
    ElevationError baseCheck = CheckExistingDirectoryOwnership(base, targetUid);
    if (baseCheck != ElevationError::kNone) {
        return baseCheck;
    }

    // Walk every intermediate directory component between baseDir and the
    // file's parent (exclusive of the final path component -- the file
    // itself is allowed not to exist yet).
    std::string relative = target.substr(base.size() + 1);
    std::vector<std::string> components;
    {
        size_t start = 0;
        while (start < relative.size()) {
            size_t slash = relative.find('/', start);
            if (slash == std::string::npos) {
                components.push_back(relative.substr(start));
                break;
            }
            components.push_back(relative.substr(start, slash - start));
            start = slash + 1;
        }
    }

    std::string current = base;
    for (size_t i = 0; i + 1 < components.size(); ++i) {
        current += "/" + components[i];
        ElevationError check = CheckExistingDirectoryOwnership(current, targetUid);
        if (check != ElevationError::kNone) {
            return check;
        }
    }

    // Final component: don't require it to already exist (O_CREAT may make
    // it), but if something is already there and it's a symlink, reject
    // before even calling open() so the failure reason is precise.
    struct stat finalSt{};
    if (lstat(target.c_str(), &finalSt) == 0 && S_ISLNK(finalSt.st_mode)) {
        return ElevationError::kSymlinkInPath;
    }

    // O_NOFOLLOW is the actual enforcement mechanism: even if a symlink was
    // swapped in between the lstat() above and this open() (TOCTOU), the
    // kernel refuses to follow it and open() fails with ELOOP.
    int fd = open(target.c_str(), flags | O_NOFOLLOW, mode);
    if (fd < 0) {
        if (errno == ELOOP) {
            return ElevationError::kSymlinkInPath;
        }
        return ElevationError::kOpenFailed;
    }

    *outFd = fd;
    return ElevationError::kNone;
}

}  // namespace indexed
