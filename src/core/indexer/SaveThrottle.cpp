#include "indexer/SaveThrottle.h"

#include <algorithm>

namespace indexed {

SaveThrottle::SaveThrottle(uint64_t minIntervalNs, uint64_t nowNs)
    : minIntervalNs_(minIntervalNs), intervalNs_(minIntervalNs), lastSaveNs_(nowNs) {}

void SaveThrottle::MarkDirty() {
    dirty_.store(true, std::memory_order_release);
}

bool SaveThrottle::ShouldSave(uint64_t nowNs) {
    if (!dirty_.load(std::memory_order_acquire) || nowNs - lastSaveNs_ < intervalNs_) {
        return false;
    }
    // Cleared before the caller saves: a change landing after this point is
    // either already in that save or marks the index dirty for the next one.
    dirty_.store(false, std::memory_order_release);
    lastSaveNs_ = nowNs;
    return true;
}

bool SaveThrottle::HasPendingChanges() const {
    return dirty_.load(std::memory_order_acquire);
}

void SaveThrottle::RecordSaveDuration(uint64_t durationNs) {
    constexpr uint64_t kSaveCostFactor = 5;
    intervalNs_ = std::max(minIntervalNs_, durationNs * kSaveCostFactor);
    // The interval runs from the end of the save, so the idle gap between
    // saves really is the interval.
    lastSaveNs_ += durationNs;
}

void SaveThrottle::MarkSaved(uint64_t nowNs) {
    dirty_.store(false, std::memory_order_release);
    lastSaveNs_ = nowNs;
}

}  // namespace indexed
