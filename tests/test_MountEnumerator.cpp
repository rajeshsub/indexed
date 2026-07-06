#include <gtest/gtest.h>
#include <unistd.h>

#include "platform/MountEnumerator.h"
#include <algorithm>
#include <memory>
#include <string>

using indexed::BlkidDeviceInfoResolver;
using indexed::IDeviceInfoResolver;
using indexed::MountEnumerator;
using indexed::MountInfo;

namespace {

// Fixture mirroring /proc/self/mountinfo's documented format (man 5 proc,
// "mountinfo" section):
//   ID  parentID  major:minor  root  mountPoint  options  [tag...]  -  fsType  source  superOptions
// Covers: a normal real fs, a pseudo fs (tmpfs), another pseudo fs (proc), a
// network fs (nfs4), an octal-escaped space (\040) in the mount point, a
// mount with two optional propagation tags before " - ", and a mount with
// zero optional tags before " - ".
constexpr char kFixture[] =
    R"(36 25 8:1 / /mnt/data rw,relatime shared:1 - ext4 /dev/sdb1 rw,data=ordered
39 38 0:26 / /dev/shm rw,nosuid,nodev shared:3 - tmpfs tmpfs rw,seclabel,inode64,usrquota
48 75 0:24 / /proc rw,nosuid,nodev,noexec,relatime shared:13 - proc proc rw
60 25 0:45 / /mnt/nfs rw,relatime shared:30 - nfs4 192.168.1.5:/export rw,vers=4.2,rsize=1048576
70 25 8:2 / /mnt/My\040Drive rw,relatime shared:40 - ext4 /dev/sdc1 rw
41 25 8:3 / /mnt/multi rw,relatime shared:1 master:2 - xfs /dev/sdd1 rw
10 1 8:4 / /mnt/none rw - ext4 /dev/sde1 rw
)";

const MountInfo* FindByMountPoint(const std::vector<MountInfo>& mounts,
                                  std::string_view mountPoint) {
    auto it = std::find_if(mounts.begin(), mounts.end(),
                           [&](const MountInfo& m) { return m.mountPoint == mountPoint; });
    return it == mounts.end() ? nullptr : &*it;
}

// Stub resolver for fixture tests: keyed on exact device string, so tests
// don't need real block devices to exercise MountEnumerator's plumbing of
// label/removable into the result.
class StubDeviceInfoResolver : public IDeviceInfoResolver {
public:
    std::string ResolveLabel(const std::string& device) const override {
        if (device == "/dev/sdb1")
            return "DATA";
        return "";
    }

    bool ResolveRemovable(const std::string& device) const override {
        return device == "/dev/sdc1";
    }
};

MountEnumerator MakeEnumeratorWithStub() {
    return MountEnumerator(std::make_shared<StubDeviceInfoResolver>());
}

}  // namespace

TEST(MountEnumerator, ParsesNormalExt4MountFields) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    std::vector<MountInfo> mounts = enumerator.ParseMountInfo(kFixture);

    const MountInfo* entry = FindByMountPoint(mounts, "/mnt/data");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->device, "/dev/sdb1");
    EXPECT_EQ(entry->fsType, "ext4");
    EXPECT_FALSE(entry->isNetwork);
}

TEST(MountEnumerator, DropsPseudoFilesystemsFromResult) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    std::vector<MountInfo> mounts = enumerator.ParseMountInfo(kFixture);

    EXPECT_EQ(FindByMountPoint(mounts, "/dev/shm"), nullptr);
    EXPECT_EQ(FindByMountPoint(mounts, "/proc"), nullptr);
}

TEST(MountEnumerator, ClassifiesNetworkFilesystemButKeepsItInResult) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    std::vector<MountInfo> mounts = enumerator.ParseMountInfo(kFixture);

    const MountInfo* entry = FindByMountPoint(mounts, "/mnt/nfs");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->fsType, "nfs4");
    EXPECT_EQ(entry->device, "192.168.1.5:/export");
    EXPECT_TRUE(entry->isNetwork);
}

TEST(MountEnumerator, DecodesOctalEscapedSpaceInMountPoint) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    std::vector<MountInfo> mounts = enumerator.ParseMountInfo(kFixture);

    const MountInfo* entry = FindByMountPoint(mounts, "/mnt/My Drive");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->device, "/dev/sdc1");
}

TEST(MountEnumerator, SkipsMultipleOptionalTaggedFieldsBeforeDashSeparator) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    std::vector<MountInfo> mounts = enumerator.ParseMountInfo(kFixture);

    const MountInfo* entry = FindByMountPoint(mounts, "/mnt/multi");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->fsType, "xfs");
    EXPECT_EQ(entry->device, "/dev/sdd1");
}

TEST(MountEnumerator, ParsesLineWithZeroOptionalFieldsBeforeDashSeparator) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    std::vector<MountInfo> mounts = enumerator.ParseMountInfo(kFixture);

    const MountInfo* entry = FindByMountPoint(mounts, "/mnt/none");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->fsType, "ext4");
    EXPECT_EQ(entry->device, "/dev/sde1");
}

TEST(MountEnumerator, ResolvesLabelAndRemovableViaInjectedResolver) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    std::vector<MountInfo> mounts = enumerator.ParseMountInfo(kFixture);

    const MountInfo* labeled = FindByMountPoint(mounts, "/mnt/data");
    ASSERT_NE(labeled, nullptr);
    EXPECT_EQ(labeled->label, "DATA");
    EXPECT_FALSE(labeled->removable);

    const MountInfo* removable = FindByMountPoint(mounts, "/mnt/My Drive");
    ASSERT_NE(removable, nullptr);
    EXPECT_EQ(removable->label, "");
    EXPECT_TRUE(removable->removable);
}

TEST(MountEnumerator, ParseMountInfoOnEmptyInputReturnsNoEntries) {
    MountEnumerator enumerator = MakeEnumeratorWithStub();
    EXPECT_TRUE(enumerator.ParseMountInfo(std::string_view{}).empty());
}

// --- Integration: real /proc/self/mountinfo on this test machine ---

TEST(MountEnumerator, EnumerateRealMountinfoParsesAtLeastOneEntryWithoutCrashing) {
    MountEnumerator enumerator;
    std::vector<MountInfo> mounts = enumerator.Enumerate();
    EXPECT_FALSE(mounts.empty());
}

TEST(MountEnumerator, EnumerateRealMountinfoRootMountIsNotRemovable) {
    MountEnumerator enumerator;
    std::vector<MountInfo> mounts = enumerator.Enumerate();

    const MountInfo* root = FindByMountPoint(mounts, "/");
    ASSERT_NE(root, nullptr);
    // A root filesystem being removable media would be exceedingly unusual;
    // this exercises BlkidDeviceInfoResolver's /sys/block lookup against
    // whatever real device backs "/" on this machine.
    EXPECT_FALSE(root->removable);
}

// --- BlkidDeviceInfoResolver against real (and deliberately bogus) devices ---

TEST(BlkidDeviceInfoResolver, ResolveLabelReturnsEmptyForNonexistentDevice) {
    BlkidDeviceInfoResolver resolver;
    EXPECT_EQ(resolver.ResolveLabel("/dev/this-device-does-not-exist-xyz"), "");
}

TEST(BlkidDeviceInfoResolver, ResolveRemovableReturnsFalseForNonexistentDevice) {
    BlkidDeviceInfoResolver resolver;
    EXPECT_FALSE(resolver.ResolveRemovable("/dev/this-device-does-not-exist-xyz"));
}

// --- Hotplug polling plumbing ---

TEST(MountEnumerator, OpenMountInfoFdReturnsAnOpenFd) {
    int fd = MountEnumerator::OpenMountInfoFd();
    ASSERT_GE(fd, 0);
    close(fd);
}

TEST(MountEnumerator, WaitForChangeReturnsFalseWhenNoChangeWithinTimeout) {
    int fd = MountEnumerator::OpenMountInfoFd();
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(MountEnumerator::WaitForChange(fd, 20));
    close(fd);
}

TEST(MountEnumerator, WaitForChangeReturnsFalseForInvalidFd) {
    EXPECT_FALSE(MountEnumerator::WaitForChange(-1, 5));
}
