#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include "indexer/IFileSystemScanner.h"
#include "storage/Crc32.h"
#include "storage/IndexPool.h"
#include "storage/IndexSerializer.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using indexed::Crc32;
using indexed::FileEntry;
using indexed::IndexPool;
using indexed::IndexSerializer;

namespace {

FileEntry MakeEntry(std::string name, std::string path, uint64_t size, uint64_t lastModified,
                    uint32_t attributes = 0) {
    FileEntry entry;
    entry.name = std::move(name);
    entry.path = std::move(path);
    entry.size = size;
    entry.lastModified = lastModified;
    entry.attributes = attributes;
    return entry;
}

// Header is u32 magic + u16 version + u64 timestamp + u64 entryCount + u32 crc32
// (docs/adr/0003-binary-index-format.md, indexed-plan.md §10).
constexpr std::streamoff kHeaderSize = 26;

std::string TempFilePath(const std::string& name) {
    return ::testing::TempDir() + "indexed_test_serializer_" + name + ".idx";
}

}  // namespace

TEST(Crc32Test, MatchesStandardCheckValue) {
    // Standard CRC-32 (reflected IEEE 802.3 / zlib-compatible, poly 0xEDB88320)
    // check value for the ASCII bytes "123456789".
    EXPECT_EQ(Crc32("123456789"), 0xCBF43926u);
}

TEST(IndexSerializerTest, RoundTripsEntriesAndTimestamps) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/home/user/a.txt", 10, 1000));
    pool.AddEntry(MakeEntry("Report.PDF", "/home/user/docs/Report.PDF", 4096, 123456789,
                            indexed::kAttrHidden));
    pool.AddEntry(MakeEntry("z.log", "/var/log/z.log", 0, 999999, indexed::kAttrDirectory));

    const std::string path = TempFilePath("roundtrip");
    constexpr uint64_t kBuildTs = 1717000000000000000ull;
    constexpr uint64_t kMonitorStopTs = 1717000500000000000ull;

    ASSERT_TRUE(IndexSerializer::Save(path, pool, kBuildTs, kMonitorStopTs));

    IndexSerializer::LoadResult result = IndexSerializer::Load(path);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.buildTimestampNs, kBuildTs);
    EXPECT_EQ(result.lastMonitorStopNs, kMonitorStopTs);
    ASSERT_EQ(result.pool.Count(), pool.Count());

    for (size_t i = 0; i < pool.Count(); ++i) {
        auto expected = pool.GetEntry(i);
        auto actual = result.pool.GetEntry(i);
        EXPECT_EQ(actual.name, expected.name);
        EXPECT_EQ(actual.nameLower, expected.nameLower);
        EXPECT_EQ(actual.path, expected.path);
        EXPECT_EQ(actual.size, expected.size);
        EXPECT_EQ(actual.lastModified, expected.lastModified);
        EXPECT_EQ(actual.attributes, expected.attributes);
    }

    std::remove(path.c_str());
}

TEST(IndexSerializerTest, RejectsMagicMismatch) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/a.txt", 1, 1));

    const std::string path = TempFilePath("bad_magic");
    ASSERT_TRUE(IndexSerializer::Save(path, pool, 1, 0));

    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());
        char corrupted = 'X';
        file.seekp(0);
        file.write(&corrupted, 1);
    }

    IndexSerializer::LoadResult result = IndexSerializer::Load(path);
    EXPECT_FALSE(result.success);

    std::remove(path.c_str());
}

TEST(IndexSerializerTest, RejectsVersionMismatch) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/a.txt", 1, 1));

    const std::string path = TempFilePath("bad_version");
    ASSERT_TRUE(IndexSerializer::Save(path, pool, 1, 0));

    {
        // Version is the u16 right after the u32 magic, i.e. byte offset 4.
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());
        const uint16_t badVersion = 99;
        file.seekp(4);
        file.write(reinterpret_cast<const char*>(&badVersion), sizeof(badVersion));
    }

    IndexSerializer::LoadResult result = IndexSerializer::Load(path);
    EXPECT_FALSE(result.success);

    std::remove(path.c_str());
}

TEST(IndexSerializerTest, RejectsCrcCorruption) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/a.txt", 1, 1));
    pool.AddEntry(MakeEntry("longer-name-file.dat", "/some/dir/longer-name-file.dat", 555, 777));

    const std::string path = TempFilePath("bad_crc");
    ASSERT_TRUE(IndexSerializer::Save(path, pool, 1, 0));

    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());

        file.seekg(0, std::ios::end);
        std::streamoff fileSize = file.tellg();
        ASSERT_GT(fileSize, kHeaderSize);

        // Flip a byte just past the header, inside the payload region covered by CRC.
        file.seekg(kHeaderSize);
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(~byte);
        file.seekp(kHeaderSize);
        file.write(&byte, 1);
    }

    IndexSerializer::LoadResult result = IndexSerializer::Load(path);
    EXPECT_FALSE(result.success);

    std::remove(path.c_str());
}

TEST(IndexSerializerTest, MissingFileFailsCleanly) {
    IndexSerializer::LoadResult result = IndexSerializer::Load(TempFilePath("does_not_exist_xyz"));
    EXPECT_FALSE(result.success);
}

TEST(IndexSerializerTest, RejectsTrailingPayloadBytes) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/a.txt", 1, 1));

    const std::string path = TempFilePath("trailing_bytes");
    ASSERT_TRUE(IndexSerializer::Save(path, pool, 1, 0));

    std::vector<char> bytes;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in.is_open());
        std::streamoff size = in.tellg();
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<size_t>(size));
        ASSERT_TRUE(in.read(bytes.data(), size));
    }

    // Recompute the header's CRC over payload+garbage so the file still passes
    // the CRC check; a correct loader must still reject it because the parsed
    // fields don't account for every byte in the payload.
    std::string_view payload(bytes.data() + kHeaderSize, bytes.size() - kHeaderSize);
    std::string extended(payload);
    extended.push_back('\xAB');
    const uint32_t newCrc = Crc32(extended);

    std::vector<char> rewritten(bytes.begin(), bytes.begin() + kHeaderSize);
    // CRC is the last 4 bytes of the header (u32 magic + u16 version + u64
    // timestamp + u64 entryCount + u32 crc32).
    constexpr std::streamoff kCrcOffset =
        kHeaderSize - static_cast<std::streamoff>(sizeof(uint32_t));
    std::memcpy(rewritten.data() + kCrcOffset, &newCrc, sizeof(newCrc));
    rewritten.insert(rewritten.end(), extended.begin(), extended.end());

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(rewritten.data(), static_cast<std::streamsize>(rewritten.size()));
    }

    IndexSerializer::LoadResult result = IndexSerializer::Load(path);
    EXPECT_FALSE(result.success);

    std::remove(path.c_str());
}

TEST(IndexSerializerTest, SaveDoesNotClobberExistingFileOnFailure) {
    const std::string dir = ::testing::TempDir() + "indexed_test_atomic_dir";
    ::mkdir(dir.c_str(), 0755);
    const std::string path = dir + "/index.idx";

    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/a.txt", 1, 1));
    ASSERT_TRUE(IndexSerializer::Save(path, pool, 1, 0));

    std::string originalContent;
    {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        originalContent.assign(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
    }

    // Strip write permission on the directory so creating the temp file (the
    // first step of a same-path atomic Save) fails, forcing Save to fail
    // before it ever gets to renaming over the existing index.
    ASSERT_EQ(::chmod(dir.c_str(), 0500), 0);

    IndexPool pool2;
    pool2.AddEntry(MakeEntry("b.txt", "/b.txt", 2, 2));
    EXPECT_FALSE(IndexSerializer::Save(path, pool2, 2, 2));

    ::chmod(dir.c_str(), 0755);

    std::string afterContent;
    {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        afterContent.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(afterContent, originalContent);

    std::remove(path.c_str());
    ::rmdir(dir.c_str());
}

TEST(IndexSerializerTest, SaveLeavesNoStrayTempFile) {
    IndexPool pool;
    pool.AddEntry(MakeEntry("a.txt", "/a.txt", 1, 1));

    const std::string path = TempFilePath("no_stray_temp");
    ASSERT_TRUE(IndexSerializer::Save(path, pool, 1, 0));

    const std::string tempPath = path + ".tmp";
    struct stat st{};
    EXPECT_NE(stat(path.c_str(), &st), -1);
    EXPECT_EQ(stat(tempPath.c_str(), &st), -1);

    std::remove(path.c_str());
}
