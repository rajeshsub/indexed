#pragma once

#include <atomic>
#include <cstdint>

namespace indexed {

// Decides when the root helper writes live-monitoring changes to disk:
// never while nothing changed, and at most once per minInterval however
// many changes land, so a burst (unpacking an archive, a large copy) costs
// one save rather than one per file. Times are nanoseconds, passed in by the
// caller rather than read from a clock, so this is testable without sleeps.
class SaveThrottle {
public:
    SaveThrottle(uint64_t minIntervalNs, uint64_t nowNs);

    // Records that the index changed. Safe to call from any thread.
    void MarkDirty();

    // True when a save is due now; the caller must then save. Clears the
    // pending change and restarts the interval.
    bool ShouldSave(uint64_t nowNs);

    // True while a change is waiting to be saved (e.g. to flush it on exit).
    bool HasPendingChanges() const;

    // Records how long the save just made took. The idle gap after it (from
    // the end of that save) becomes max(minInterval, 5x its duration), so a
    // large index isn't saved more often than it can comfortably be written.
    void RecordSaveDuration(uint64_t durationNs);

    // Records a save made for another reason (a full rebuild), which already
    // includes every pending change.
    void MarkSaved(uint64_t nowNs);

private:
    std::atomic<bool> dirty_{false};
    uint64_t minIntervalNs_;
    uint64_t intervalNs_;
    uint64_t lastSaveNs_;
};

}  // namespace indexed
