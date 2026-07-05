#include "storage/IndexSerializer.h"

#include "storage/Crc32.h"
#include "storage/EntryMeta.h"
#include <cstring>
#include <fstream>
#include <type_traits>

namespace indexed {

namespace {

constexpr uint32_t kMagic = 0x44584449;  // "IDXD", little-endian bytes 'I','D','X','D'.
constexpr uint16_t kVersion = 1;

// Header: u32 magic + u16 version + u64 timestamp + u64 entryCount + u32 crc32.
constexpr size_t kHeaderSize =
    sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t);

// Per-entry disk record: u64 size + u64 lastModified + u32 attributes + u64 pathOffset +
// u32 pathLen + u16 nameStart.
constexpr size_t kEntryRecordSize = sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t) +
                                    sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint16_t);

// Appends a fixed-width integer to buffer via a straightforward native-representation
// byte copy (no explicit endianness handling: target platforms are x86-64/aarch64
// Linux, both little-endian, per the format spec).
template <typename T>
void AppendValue(std::vector<char>& buffer, T value) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "AppendValue requires a trivially copyable type");
    size_t offset = buffer.size();
    buffer.resize(offset + sizeof(T));
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

// Reads a fixed-width integer out of buffer at *offset, advancing it. Returns false
// (leaving value untouched) if the buffer doesn't have enough remaining bytes, so
// truncated/corrupt files are reported as a clean load failure instead of an
// out-of-bounds read.
template <typename T>
bool ReadValue(const std::vector<char>& buffer, size_t& offset, T& value) {
    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

}  // namespace

bool IndexSerializer::Save(const std::string& filepath, const IndexPool& pool,
                           uint64_t buildTimestampNs, uint64_t lastMonitorStopNs) {
    const std::vector<EntryMeta>& meta = pool.Meta();
    const std::vector<char>& pathPool = pool.PathPool();

    std::vector<char> body;
    body.reserve(sizeof(uint64_t) + pathPool.size() + meta.size() * kEntryRecordSize +
                 sizeof(uint64_t));

    AppendValue(body, static_cast<uint64_t>(pathPool.size()));
    body.insert(body.end(), pathPool.begin(), pathPool.end());

    for (const EntryMeta& entry : meta) {
        AppendValue(body, entry.size);
        AppendValue(body, entry.lastModified);
        AppendValue(body, entry.attributes);
        AppendValue(body, entry.pathOffset);
        AppendValue(body, entry.pathLen);
        AppendValue(body, entry.nameStart);
    }

    AppendValue(body, lastMonitorStopNs);

    const uint32_t crc = Crc32(std::string_view(body.data(), body.size()));

    std::vector<char> header;
    header.reserve(kHeaderSize);
    AppendValue(header, kMagic);
    AppendValue(header, kVersion);
    AppendValue(header, buildTimestampNs);
    AppendValue(header, static_cast<uint64_t>(meta.size()));
    AppendValue(header, crc);

    std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return out.good();
}

IndexSerializer::LoadResult IndexSerializer::Load(const std::string& filepath) {
    std::ifstream in(filepath, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }

    const std::streamoff fileSize = in.tellg();
    if (fileSize < 0) {
        return {};
    }
    in.seekg(0, std::ios::beg);

    std::vector<char> buffer(static_cast<size_t>(fileSize));
    if (!buffer.empty() && !in.read(buffer.data(), fileSize)) {
        return {};
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint64_t buildTimestampNs = 0;
    uint64_t entryCount = 0;
    uint32_t crc = 0;
    if (!ReadValue(buffer, offset, magic) || !ReadValue(buffer, offset, version) ||
        !ReadValue(buffer, offset, buildTimestampNs) || !ReadValue(buffer, offset, entryCount) ||
        !ReadValue(buffer, offset, crc)) {
        return {};
    }
    if (magic != kMagic || version != kVersion) {
        return {};
    }

    const std::string_view payload(buffer.data() + offset, buffer.size() - offset);
    if (Crc32(payload) != crc) {
        return {};
    }

    uint64_t pathPoolSize = 0;
    if (!ReadValue(buffer, offset, pathPoolSize)) {
        return {};
    }
    if (pathPoolSize > buffer.size() - offset) {
        return {};
    }
    std::vector<char> pathPool(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                               buffer.begin() + static_cast<std::ptrdiff_t>(offset + pathPoolSize));
    offset += pathPoolSize;

    std::vector<EntryMeta> meta;
    meta.reserve(entryCount);
    for (uint64_t i = 0; i < entryCount; ++i) {
        EntryMeta entry;
        if (!ReadValue(buffer, offset, entry.size) ||
            !ReadValue(buffer, offset, entry.lastModified) ||
            !ReadValue(buffer, offset, entry.attributes) ||
            !ReadValue(buffer, offset, entry.pathOffset) ||
            !ReadValue(buffer, offset, entry.pathLen) ||
            !ReadValue(buffer, offset, entry.nameStart)) {
            return {};
        }
        meta.push_back(entry);
    }

    uint64_t lastMonitorStopNs = 0;
    if (!ReadValue(buffer, offset, lastMonitorStopNs)) {
        return {};
    }

    LoadResult result;
    result.success = true;
    result.pool = IndexPool::LoadFromPathPool(std::move(meta), std::move(pathPool));
    result.buildTimestampNs = buildTimestampNs;
    result.lastMonitorStopNs = lastMonitorStopNs;
    return result;
}

}  // namespace indexed
