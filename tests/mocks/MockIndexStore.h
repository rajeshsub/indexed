#pragma once

#include <gmock/gmock.h>

#include "storage/IIndexStore.h"

namespace indexed {

class MockIndexStore : public IIndexStore {
public:
    MOCK_METHOD(void, BeginWrite, (), (override));
    MOCK_METHOD(void, AddEntry, (const FileEntry& entry), (override));
    MOCK_METHOD(void, EndWrite, (), (override));

    MOCK_METHOD(void, ApplyAdd, (const FileEntry& entry), (override));
    MOCK_METHOD(void, ApplyRemove, (std::string_view path), (override));
    MOCK_METHOD(void, ApplyRename, (std::string_view oldPath, std::string_view newPath),
                (override));
    MOCK_METHOD(void, RemoveEntriesUnderPath, (std::string_view pathPrefix), (override));

    MOCK_METHOD(void, LoadPool,
                (IndexPool pool, uint64_t buildTimestampNs, uint64_t lastMonitorStopNs),
                (override));

    MOCK_METHOD(const IndexPool&, GetPool, (), (const, override));
    MOCK_METHOD(std::shared_mutex&, GetSearchMutex, (), (override));

    MOCK_METHOD(void, SetBuildTimestamp, (uint64_t nsSinceEpoch), (override));
    MOCK_METHOD(uint64_t, GetIndexAgeSeconds, (uint64_t nowNs), (const, override));

    MOCK_METHOD(void, SetLastMonitorStop, (uint64_t nsSinceEpoch), (override));
    MOCK_METHOD(uint64_t, GetLastMonitorStop, (), (const, override));
};

}  // namespace indexed
