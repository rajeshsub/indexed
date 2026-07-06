#pragma once

#include <gmock/gmock.h>

#include "indexer/IFileSystemScanner.h"

namespace indexed {

class MockFileSystemScanner : public IFileSystemScanner {
public:
    MOCK_METHOD(bool, FastScanAvailable, (const std::string& root), (const, override));
    MOCK_METHOD(void, Scan,
                (const ScanOptions& options, ScanCallback onEntry, ProgressCallback onProgress,
                 const std::atomic<bool>& cancelToken),
                (override));
};

}  // namespace indexed
