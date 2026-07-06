#pragma once

#include <istream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace indexed {

// One parsed, classified entry from /proc/self/mountinfo. See
// indexed-plan.md §7.6.
struct MountInfo {
    std::string mountPoint;
    std::string device;  // e.g. "/dev/sda1"; may be a non-device source
                         // (e.g. "server:/export") for network filesystems.
    std::string fsType;  // e.g. "ext4", "nfs4"
    std::string label;   // "" if unavailable / lookup failed
    bool removable = false;
    bool isNetwork = false;
};

// Resolves the label/removable-media facts about a block device, given its
// mountinfo source path (e.g. "/dev/sda1"). Extracted behind an interface so
// ParseMountInfo's fixture-based tests don't need real block devices or root
// privileges: tests inject a stub; production code uses BlkidDeviceInfoResolver.
class IDeviceInfoResolver {
public:
    virtual ~IDeviceInfoResolver() = default;

    // "" if the device has no label, isn't a real device, or the lookup fails.
    // Never throws.
    virtual std::string ResolveLabel(const std::string& device) const = 0;

    // false if the device isn't removable, isn't a real device, or the lookup
    // fails. Never throws.
    virtual bool ResolveRemovable(const std::string& device) const = 0;
};

// Real resolver: label via libblkid (blkid_get_tag_value), removable via
// /sys/block/<parent-device>/removable.
class BlkidDeviceInfoResolver : public IDeviceInfoResolver {
public:
    std::string ResolveLabel(const std::string& device) const override;
    bool ResolveRemovable(const std::string& device) const override;
};

// Parses /proc/self/mountinfo (real or fixture), classifying each mount as
// index-eligible ("real"), pseudo (always dropped from the result — proc,
// sysfs, tmpfs, cgroup*, ... — never index-eligible), or network (kept in
// the result with isNetwork=true, since the caller/UI decides whether to
// offer it as a root — it should just not be a *default*-selected one).
class MountEnumerator {
public:
    // Uses a real BlkidDeviceInfoResolver.
    MountEnumerator();
    // Test/DI seam: inject a stub resolver to avoid touching real hardware.
    explicit MountEnumerator(std::shared_ptr<IDeviceInfoResolver> resolver);

    // Core, testable parsing logic: takes mountinfo-formatted text directly
    // (fixture strings in tests; the real file's contents in production via
    // Enumerate()). Never throws; a malformed line is skipped rather than
    // aborting the whole parse.
    std::vector<MountInfo> ParseMountInfo(std::istream& input) const;
    std::vector<MountInfo> ParseMountInfo(std::string_view content) const;

    // Thin wrapper around ParseMountInfo: reads the real /proc/self/mountinfo.
    // Returns an empty vector if the file can't be opened.
    std::vector<MountInfo> Enumerate() const;

    // Opens /proc/self/mountinfo for hotplug polling (see WaitForChange).
    // Returns -1 on failure.
    static int OpenMountInfoFd();

    // Blocks on fd (as returned by OpenMountInfoFd) waiting for the kernel's
    // POLLPRI notification that mountinfo changed. timeoutMs < 0 blocks
    // indefinitely; 0 returns immediately. Returns true if a change was
    // signaled, false on timeout or error.
    static bool WaitForChange(int fd, int timeoutMs = -1);

private:
    std::shared_ptr<IDeviceInfoResolver> resolver_;
};

}  // namespace indexed
