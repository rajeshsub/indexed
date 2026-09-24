#include <gtest/gtest.h>

#include "indexer/SaveThrottle.h"
#include <cstdint>

using indexed::SaveThrottle;

namespace {

constexpr uint64_t kSecond = 1'000'000'000ULL;
constexpr uint64_t kInterval = 2 * kSecond;
constexpr uint64_t kStart = 1000 * kSecond;

}  // namespace

TEST(SaveThrottle, SavesWhenDirtyAndTheIntervalHasPassed) {
    SaveThrottle throttle(kInterval, kStart);
    throttle.MarkDirty();

    EXPECT_TRUE(throttle.ShouldSave(kStart + kInterval));
}

TEST(SaveThrottle, WaitsOutTheIntervalBeforeSaving) {
    SaveThrottle throttle(kInterval, kStart);
    throttle.MarkDirty();

    EXPECT_FALSE(throttle.ShouldSave(kStart + kSecond));
    EXPECT_FALSE(throttle.ShouldSave(kStart + kInterval - 1));
    EXPECT_TRUE(throttle.ShouldSave(kStart + kInterval));
}

TEST(SaveThrottle, ABurstOfChangesWithinOneIntervalCausesOneSave) {
    SaveThrottle throttle(kInterval, kStart);
    int saves = 0;
    // 1,000 changes spread over 2 s, polled every 200 ms as the helper's
    // main loop does, then 2 s more of polling with no further changes.
    for (uint64_t t = kStart; t <= kStart + 2 * kInterval; t += kSecond / 5) {
        if (t < kStart + kInterval) {
            for (int i = 0; i < 100; ++i) {
                throttle.MarkDirty();
            }
        }
        if (throttle.ShouldSave(t)) {
            ++saves;
        }
    }

    EXPECT_EQ(saves, 1);
}

TEST(SaveThrottle, NeverSavesWhenNothingChanged) {
    SaveThrottle throttle(kInterval, kStart);

    EXPECT_FALSE(throttle.ShouldSave(kStart + kInterval));
    EXPECT_FALSE(throttle.ShouldSave(kStart + 3600 * kSecond));
}

TEST(SaveThrottle, AFullRebuildSaveClearsPendingChanges) {
    SaveThrottle throttle(kInterval, kStart);
    throttle.MarkDirty();
    throttle.MarkSaved(kStart + kSecond);

    EXPECT_FALSE(throttle.ShouldSave(kStart + 10 * kInterval));
}

TEST(SaveThrottle, AChangeAfterASaveIsSavedOnTheNextInterval) {
    SaveThrottle throttle(kInterval, kStart);
    throttle.MarkDirty();
    ASSERT_TRUE(throttle.ShouldSave(kStart + kInterval));

    throttle.MarkDirty();
    EXPECT_FALSE(throttle.ShouldSave(kStart + kInterval + kSecond));
    EXPECT_TRUE(throttle.ShouldSave(kStart + 2 * kInterval));
}

// The helper flushes a pending change on exit instead of losing it.
TEST(SaveThrottle, ReportsPendingChangesUntilSaved) {
    SaveThrottle throttle(kInterval, kStart);
    EXPECT_FALSE(throttle.HasPendingChanges());

    throttle.MarkDirty();
    EXPECT_TRUE(throttle.HasPendingChanges());

    ASSERT_TRUE(throttle.ShouldSave(kStart + kInterval));
    EXPECT_FALSE(throttle.HasPendingChanges());
}

// A large index takes long to write; saving it back-to-back would keep the
// helper busy saving (ADR 0014). The gap adapts to 5x the last save.
TEST(SaveThrottle, SlowSavesStretchTheInterval) {
    SaveThrottle throttle(kInterval, kStart);
    throttle.MarkDirty();
    ASSERT_TRUE(throttle.ShouldSave(kStart + kInterval));
    throttle.RecordSaveDuration(kSecond);  // the save ran 1 s; next idle gap: 5 s

    throttle.MarkDirty();
    const uint64_t saveEnded = kStart + kInterval + kSecond;
    EXPECT_FALSE(throttle.ShouldSave(saveEnded + kInterval));
    EXPECT_FALSE(throttle.ShouldSave(saveEnded + 5 * kSecond - 1));
    EXPECT_TRUE(throttle.ShouldSave(saveEnded + 5 * kSecond));
}

TEST(SaveThrottle, FastSavesKeepTheMinimumInterval) {
    SaveThrottle throttle(kInterval, kStart);
    throttle.MarkDirty();
    ASSERT_TRUE(throttle.ShouldSave(kStart + kInterval));
    throttle.RecordSaveDuration(10'000'000ULL);  // 10 ms

    throttle.MarkDirty();
    const uint64_t saveEnded = kStart + kInterval + 10'000'000ULL;
    EXPECT_FALSE(throttle.ShouldSave(saveEnded + kInterval - 1));
    EXPECT_TRUE(throttle.ShouldSave(saveEnded + kInterval));
}
