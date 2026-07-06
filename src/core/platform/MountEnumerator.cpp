#include "platform/MountEnumerator.h"

#include <blkid/blkid.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace indexed {

namespace {

// Filesystem types that are never index-eligible: kernel/virtual
// filesystems with no real on-disk content to search. Matched on fsType
// per indexed-plan.md §7.6. Not exhaustive of every pseudo-fs Linux can
// mount, but covers the common ones plus this task's explicit list.
const std::unordered_set<std::string_view>& PseudoFsTypes() {
    static const std::unordered_set<std::string_view> kPseudoFsTypes = {
        "proc",    "sysfs",     "devtmpfs",    "devpts",     "tmpfs",  "cgroup",
        "cgroup2", "run",       "rpc_pipefs",  "securityfs", "pstore", "efivarfs",
        "bpf",     "configfs",  "debugfs",     "tracefs",    "mqueue", "fusectl",
        "autofs",  "selinuxfs", "binfmt_misc", "hugetlbfs",  "ramfs",  "nsfs",
    };
    return kPseudoFsTypes;
}

const std::unordered_set<std::string_view>& NetworkFsTypes() {
    static const std::unordered_set<std::string_view> kNetworkFsTypes = {
        "nfs", "nfs4", "cifs", "smb", "sshfs", "fuse.sshfs",
    };
    return kNetworkFsTypes;
}

bool IsPseudoFsType(std::string_view fsType) {
    return PseudoFsTypes().count(fsType) > 0;
}

bool IsNetworkFsType(std::string_view fsType) {
    return NetworkFsTypes().count(fsType) > 0;
}

// mountinfo escapes whitespace/backslash in the root and mount-point fields
// as octal: space -> \040, tab -> \011, newline -> \012, backslash -> \134
// (man 5 proc, "mountinfo"). Any other backslash sequence is passed through
// unchanged (defensive; not expected in practice).
std::string DecodeMountinfoEscapes(std::string_view raw) {
    std::string result;
    result.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size() &&
            std::isdigit(static_cast<unsigned char>(raw[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(raw[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(raw[i + 3]))) {
            int value = (raw[i + 1] - '0') * 64 + (raw[i + 2] - '0') * 8 + (raw[i + 3] - '0');
            result.push_back(static_cast<char>(value));
            i += 3;
        } else {
            result.push_back(raw[i]);
        }
    }
    return result;
}

// Parses one mountinfo line into a MountInfo, or nullopt if the line is
// malformed. Format (man 5 proc, "mountinfo"):
//   ID parentID major:minor root mountPoint options [tag...] - fsType source superOptions
std::optional<MountInfo> ParseMountInfoLine(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> fields;
    std::string field;
    while (iss >> field) {
        fields.push_back(field);
    }

    // Fixed fields 0..5 (mount ID, parent ID, major:minor, root, mount point,
    // mount options) must be present, plus the "-" separator and the three
    // trailing fields after it.
    if (fields.size() < 6) {
        return std::nullopt;
    }

    auto dashIt = std::find(fields.begin() + 6, fields.end(), "-");
    if (dashIt == fields.end()) {
        return std::nullopt;
    }
    size_t dashIndex = static_cast<size_t>(std::distance(fields.begin(), dashIt));
    if (dashIndex + 2 >= fields.size()) {
        return std::nullopt;
    }

    MountInfo info;
    info.mountPoint = DecodeMountinfoEscapes(fields[4]);
    info.fsType = fields[dashIndex + 1];
    info.device = DecodeMountinfoEscapes(fields[dashIndex + 2]);
    return info;
}

// Strips a trailing partition suffix from a /sys/block device name, e.g.
// "sda1" -> "sda", "nvme0n1p3" -> "nvme0n1", "mmcblk0p1" -> "mmcblk0".
// Returns "" if name has no trailing digits to strip (i.e. it's already a
// whole-disk-shaped name).
std::string StripPartitionSuffix(const std::string& name) {
    size_t end = name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1]))) {
        --end;
    }
    if (end == name.size()) {
        return "";  // no trailing digits at all
    }
    // nvme/mmcblk-style names put a "p" between the whole-disk digits and
    // the partition number (e.g. "nvme0n1" + "p3"): strip that too, but
    // only when what remains still ends in a digit, so plain "sda" + "1"
    // (no "p") isn't mishandled.
    if (end > 0 && name[end - 1] == 'p') {
        size_t withoutP = end - 1;
        if (withoutP > 0 && std::isdigit(static_cast<unsigned char>(name[withoutP - 1]))) {
            end = withoutP;
        }
    }
    return name.substr(0, end);
}

bool ReadSysBlockRemovable(const std::string& blockName) {
    std::ifstream file("/sys/block/" + blockName + "/removable");
    if (!file.is_open()) {
        return false;
    }
    int value = 0;
    file >> value;
    return value != 0;
}

bool SysBlockEntryExists(const std::string& blockName) {
    std::ifstream file("/sys/block/" + blockName + "/removable");
    return file.good();
}

}  // namespace

std::string BlkidDeviceInfoResolver::ResolveLabel(const std::string& device) const {
    char* value = blkid_get_tag_value(nullptr, "LABEL", device.c_str());
    if (value == nullptr) {
        return "";
    }
    std::string label(value);
    free(value);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    return label;
}

bool BlkidDeviceInfoResolver::ResolveRemovable(const std::string& device) const {
    // device is typically "/dev/<name>" (whole disk, e.g. "sda", "dm-0") or
    // "/dev/<name><partN>" (partition, e.g. "sda1", "nvme0n1p3").
    // /sys/block/<name>/removable only exists for whole (parent) disks, so
    // if device itself isn't one, strip a trailing partition suffix and
    // retry once.
    constexpr std::string_view kDevPrefix = "/dev/";
    if (device.compare(0, kDevPrefix.size(), kDevPrefix) != 0) {
        return false;
    }
    std::string name = device.substr(kDevPrefix.size());
    if (name.empty()) {
        return false;
    }

    if (SysBlockEntryExists(name)) {
        return ReadSysBlockRemovable(name);
    }

    std::string parent = StripPartitionSuffix(name);
    if (parent.empty() || !SysBlockEntryExists(parent)) {
        return false;
    }
    return ReadSysBlockRemovable(parent);
}

MountEnumerator::MountEnumerator() : resolver_(std::make_shared<BlkidDeviceInfoResolver>()) {}

MountEnumerator::MountEnumerator(std::shared_ptr<IDeviceInfoResolver> resolver)
    : resolver_(std::move(resolver)) {}

std::vector<MountInfo> MountEnumerator::ParseMountInfo(std::istream& input) const {
    std::vector<MountInfo> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::optional<MountInfo> parsed = ParseMountInfoLine(line);
        if (!parsed.has_value()) {
            continue;
        }
        MountInfo info = std::move(parsed.value());

        if (IsPseudoFsType(info.fsType)) {
            continue;  // never index-eligible; drop entirely
        }
        info.isNetwork = IsNetworkFsType(info.fsType);

        if (resolver_) {
            info.label = resolver_->ResolveLabel(info.device);
            info.removable = resolver_->ResolveRemovable(info.device);
        }

        result.push_back(std::move(info));
    }
    return result;
}

std::vector<MountInfo> MountEnumerator::ParseMountInfo(std::string_view content) const {
    std::istringstream iss{std::string(content)};
    return ParseMountInfo(iss);
}

std::vector<MountInfo> MountEnumerator::Enumerate() const {
    std::ifstream file("/proc/self/mountinfo");
    if (!file.is_open()) {
        return {};
    }
    return ParseMountInfo(file);
}

int MountEnumerator::OpenMountInfoFd() {
    return open("/proc/self/mountinfo", O_RDONLY);  // -1 on failure, matching open(2)
}

bool MountEnumerator::WaitForChange(int fd, int timeoutMs) {
    if (fd < 0) {
        return false;
    }
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLPRI;
    int ret = poll(&pfd, 1, timeoutMs);
    if (ret <= 0) {
        return false;  // timeout (0) or error (-1)
    }
    return (pfd.revents & POLLPRI) != 0;
}

}  // namespace indexed
