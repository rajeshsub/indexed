#pragma once

#include <gmock/gmock.h>

#include "indexer/IChangeMonitor.h"

namespace indexed {

class MockChangeMonitor : public IChangeMonitor {
public:
    MOCK_METHOD(bool, IsAvailable, (const std::string& root), (const, override));
    MOCK_METHOD(void, StartMonitoring,
                (const std::string& root, ChangeCallback onChange,
                 const std::atomic<bool>& stopToken),
                (override));
};

}  // namespace indexed
