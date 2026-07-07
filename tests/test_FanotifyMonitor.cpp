#include <gtest/gtest.h>

#include "indexer/FanotifyMonitor.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using indexed::FanotifyMonitor;
using indexed::FileChangeEvent;
using indexed::FileChangeType;
using indexed::IFileHandleResolver;

namespace {

// Stub resolver for fixture tests: ParseFanotifyEvent's hand-built byte
// records carry a fake file_handle that no real open_by_handle_at call could
// resolve (it was never issued by a live fanotify session), so path
// resolution is injected here instead -- same seam pattern as
// MountEnumerator's IDeviceInfoResolver.
class StubFileHandleResolver : public IFileHandleResolver {
public:
    std::string ResolveDirPath(int mountFd, const struct file_handle* handle) const override {
        lastMountFd = mountFd;
        lastHandleSeen = (handle != nullptr);
        if (forceFail) {
            return "";
        }
        return dirPath;
    }

    std::string dirPath = "/mnt/data/sub";
    bool forceFail = false;
    mutable int lastMountFd = -1;
    mutable bool lastHandleSeen = false;
};

FanotifyMonitor MakeMonitorWithStub(std::shared_ptr<StubFileHandleResolver> stub) {
    return FanotifyMonitor(stub);
}

// Hand-builds the raw byte layout the kernel would produce for one fanotify
// event under FAN_REPORT_DFID_NAME: a fanotify_event_metadata header
// immediately followed by a fanotify_event_info_fid record (info header +
// fsid + a struct file_handle + the changed entry's trailing,
// null-terminated name). Mirrors MountEnumerator's fixture-text approach,
// just binary instead of text.
std::vector<unsigned char> BuildFanotifyEventBytes(uint64_t mask, const std::string& name,
                                                   bool includeDfidNameInfo = true,
                                                   bool includeNameTerminator = true) {
    constexpr unsigned int kHandleBytes = 8;
    size_t handleTotal = sizeof(struct file_handle) + kHandleBytes;
    size_t infoLen = sizeof(fanotify_event_info_header) + sizeof(__kernel_fsid_t) + handleTotal +
                     name.size() + (includeNameTerminator ? 1 : 0);
    size_t metadataLen = sizeof(fanotify_event_metadata);
    size_t eventLen = metadataLen + (includeDfidNameInfo ? infoLen : 0);

    std::vector<unsigned char> buf(eventLen, 0);
    auto* metadata = reinterpret_cast<fanotify_event_metadata*>(buf.data());
    metadata->event_len = static_cast<__u32>(eventLen);
    metadata->vers = FANOTIFY_METADATA_VERSION;
    metadata->reserved = 0;
    metadata->metadata_len = static_cast<__u16>(metadataLen);
    metadata->mask = mask;
    metadata->fd = FAN_NOFD;
    metadata->pid = 0;

    if (!includeDfidNameInfo) {
        return buf;
    }

    auto* fidInfo = reinterpret_cast<fanotify_event_info_fid*>(buf.data() + metadataLen);
    fidInfo->hdr.info_type = FAN_EVENT_INFO_TYPE_DFID_NAME;
    fidInfo->hdr.pad = 0;
    fidInfo->hdr.len = static_cast<__u16>(infoLen);

    auto* handle = reinterpret_cast<struct file_handle*>(fidInfo->handle);
    handle->handle_bytes = kHandleBytes;
    handle->handle_type = 1;
    std::memset(handle->f_handle, 0xAB, kHandleBytes);

    char* namePtr = reinterpret_cast<char*>(fidInfo->handle) + handleTotal;
    std::memcpy(namePtr, name.data(), name.size());
    if (includeNameTerminator) {
        namePtr[name.size()] = '\0';
    }

    return buf;
}

const fanotify_event_metadata* AsMetadata(const std::vector<unsigned char>& bytes) {
    return reinterpret_cast<const fanotify_event_metadata*>(bytes.data());
}

}  // namespace

// --- IsAvailable against the real kernel/capabilities of this sandbox ---

TEST(FanotifyMonitor, IsAvailableReturnsFalseWithoutCapSysAdmin) {
    // This sandbox process has no CAP_SYS_ADMIN, confirmed by direct syscall
    // testing: fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME, ...)
    // alone actually SUCCEEDS here (kernel >= 5.9's "unprivileged fanotify
    // listener" support kicks in once an FID-reporting flag is passed), but
    // the follow-up fanotify_mark(FAN_MARK_ADD | FAN_MARK_FILESYSTEM, ...)
    // probe IsAvailable() performs fails with EPERM, exactly as it would for
    // StartMonitoring's real whole-filesystem mark. IsAvailable() must
    // translate that into a plain `false` rather than throwing or crashing.
    // This is a positive assertion that the capability-detection code path
    // actually runs, not a skipped/xfail test -- it will correctly flip to
    // true if this ever runs as root with a >=5.9 kernel (e.g. inside
    // indexed-helper), which is exactly the contract we want.
    FanotifyMonitor monitor;
    EXPECT_FALSE(monitor.IsAvailable("/"));
}

// --- ParseFanotifyEvent: fixture-based, no real fanotify fd needed ---

TEST(FanotifyMonitor, ParsesCreateEventAsAdded) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(FAN_CREATE, "newfile.txt");

    std::optional<FileChangeEvent> event = monitor.ParseFanotifyEvent(AsMetadata(bytes), 42);

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, FileChangeType::Added);
    EXPECT_EQ(event->path, "/mnt/data/sub/newfile.txt");
    EXPECT_EQ(stub->lastMountFd, 42);
    EXPECT_TRUE(stub->lastHandleSeen);
}

TEST(FanotifyMonitor, ParsesDeleteEventAsRemoved) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(FAN_DELETE, "gone.txt");

    std::optional<FileChangeEvent> event = monitor.ParseFanotifyEvent(AsMetadata(bytes), 1);

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, FileChangeType::Removed);
    EXPECT_EQ(event->path, "/mnt/data/sub/gone.txt");
}

TEST(FanotifyMonitor, ParsesModifyEventAsModified) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(FAN_MODIFY, "changed.txt");

    std::optional<FileChangeEvent> event = monitor.ParseFanotifyEvent(AsMetadata(bytes), 1);

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, FileChangeType::Modified);
    EXPECT_EQ(event->path, "/mnt/data/sub/changed.txt");
}

// Documented tradeoff (class comment): FAN_REPORT_DFID_NAME carries no
// rename cookie, so a lone MOVED_FROM/MOVED_TO can't be correlated into a
// single Renamed event -- each is reported standalone instead.
TEST(FanotifyMonitor, ParsesLoneMovedFromAsRemoved) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(FAN_MOVED_FROM, "old.txt");

    std::optional<FileChangeEvent> event = monitor.ParseFanotifyEvent(AsMetadata(bytes), 1);

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, FileChangeType::Removed);
    EXPECT_EQ(event->path, "/mnt/data/sub/old.txt");
}

TEST(FanotifyMonitor, ParsesLoneMovedToAsAdded) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(FAN_MOVED_TO, "new.txt");

    std::optional<FileChangeEvent> event = monitor.ParseFanotifyEvent(AsMetadata(bytes), 1);

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, FileChangeType::Added);
    EXPECT_EQ(event->path, "/mnt/data/sub/new.txt");
}

TEST(FanotifyMonitor, UnmappedMaskReturnsNullopt) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    // FAN_ONDIR alone (no CREATE/DELETE/MOVE/MODIFY bit) maps to nothing.
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(FAN_ONDIR, "somedir");

    EXPECT_FALSE(monitor.ParseFanotifyEvent(AsMetadata(bytes), 1).has_value());
}

TEST(FanotifyMonitor, MissingDfidNameInfoReturnsNullopt) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes =
        BuildFanotifyEventBytes(FAN_CREATE, "x.txt", /*includeDfidNameInfo=*/false);

    EXPECT_FALSE(monitor.ParseFanotifyEvent(AsMetadata(bytes), 1).has_value());
}

TEST(FanotifyMonitor, MissingNameTerminatorReturnsNullopt) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(
        FAN_CREATE, "x.txt", /*includeDfidNameInfo=*/true, /*includeNameTerminator=*/false);

    EXPECT_FALSE(monitor.ParseFanotifyEvent(AsMetadata(bytes), 1).has_value());
}

TEST(FanotifyMonitor, NullMetadataReturnsNullopt) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    EXPECT_FALSE(monitor.ParseFanotifyEvent(nullptr, 1).has_value());
}

TEST(FanotifyMonitor, ResolverFailureReturnsNullopt) {
    auto stub = std::make_shared<StubFileHandleResolver>();
    stub->forceFail = true;
    FanotifyMonitor monitor = MakeMonitorWithStub(stub);
    std::vector<unsigned char> bytes = BuildFanotifyEventBytes(FAN_CREATE, "x.txt");

    EXPECT_FALSE(monitor.ParseFanotifyEvent(AsMetadata(bytes), 1).has_value());
}

// --- StartMonitoring on an unavailable system (this sandbox) ---

TEST(FanotifyMonitor, StartMonitoringReturnsPromptlyWhenUnavailable) {
    // Contract (class comment): StartMonitoring re-checks availability
    // itself and returns immediately without hanging or crashing when
    // fanotify can't be used -- callers are not required to call
    // IsAvailable() first. In this sandbox (no CAP_SYS_ADMIN) fanotify_init
    // fails, so this should return well before stopToken is ever set.
    FanotifyMonitor monitor;
    std::atomic<bool> stopToken{false};
    bool callbackInvoked = false;

    auto start = std::chrono::steady_clock::now();
    monitor.StartMonitoring(
        "/", [&](const FileChangeEvent&) { callbackInvoked = true; }, stopToken);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_FALSE(callbackInvoked);
}
