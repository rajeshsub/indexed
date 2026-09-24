#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <thread>

// Measures how long the calling (UI) thread goes without returning to its
// event loop, and separates a real block from the OS simply not scheduling
// the process (shared-CI-runner contention): a wall-clock stretch only
// counts against the thread if the thread was actually running for most of
// it, per CLOCK_THREAD_CPUTIME_ID. No Qt dependency -- a plain background
// thread samples on a fixed interval instead of a QTimer, so this is usable
// (and testable) from plain gtest as well as from Qt tests. Used by
// tests/test_MainWindow.cpp's latency check (docs/adr/0014) and unit-tested
// directly in tests/test_GapProbe.cpp.

// Wall-clock-vs-CPU-time reading for the calling thread.
struct ThreadTimePoint {
    int64_t wallNs = 0;
    int64_t cpuNs = 0;
};

inline ThreadTimePoint NowThreadTime() {
    ThreadTimePoint point;
    struct timespec wall{};
    clock_gettime(CLOCK_MONOTONIC, &wall);
    point.wallNs = static_cast<int64_t>(wall.tv_sec) * 1'000'000'000LL + wall.tv_nsec;
    struct timespec cpu{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    point.cpuNs = static_cast<int64_t>(cpu.tv_sec) * 1'000'000'000LL + cpu.tv_nsec;
    return point;
}

// True when a wall-clock stretch of `wallMs` is attributable to this thread
// itself not returning to its event loop, rather than the OS simply not
// scheduling the process. The thread must have been running for at least
// 3/4 of the stretch for the stretch to count as the thread's own doing.
inline bool IsSelfInflictedGap(int64_t wallMs, int64_t cpuMs) {
    return cpuMs * 4 >= wallMs * 3;
}

// Records the longest stretch the calling thread went without returning to
// its own message loop. Start()/Stop() must be called from that thread;
// PollThreadTime() must be called from that same thread on a regular
// interval (e.g. once per event-loop turn or from a timer callback) -- this
// class does not spawn its own polling thread, since polling from a
// different thread would measure that thread's CPU time, not the one being
// watched, defeating the whole point. A stretch where the thread barely ran
// at all (CI runner noise, not something the app did) is recorded
// separately via MaxDescheduledMs() instead of counting toward Stop().
class GapProbe {
public:
    void Start() {
        maxBlockedMs_ = 0;
        maxDescheduledMs_ = 0;
        last_ = NowThreadTime();
    }

    // Call periodically from the watched thread; NOT thread-safe to call
    // from elsewhere (see class comment).
    void Poll() { Sample(); }

    // Longest stretch attributable to the watched thread itself not
    // returning to its event loop (what a "does this ever block for over
    // N ms?" claim is actually about).
    int64_t Stop() {
        Sample();
        return maxBlockedMs_;
    }

    // Longest stretch where the process wasn't scheduled at all; reported
    // for visibility, never asserted on.
    int64_t MaxDescheduledMs() const { return maxDescheduledMs_; }

private:
    void Sample() {
        const ThreadTimePoint now = NowThreadTime();
        const int64_t wallMs = (now.wallNs - last_.wallNs) / 1'000'000;
        const int64_t cpuMs = (now.cpuNs - last_.cpuNs) / 1'000'000;
        last_ = now;
        if (wallMs <= 0) {
            return;
        }
        if (IsSelfInflictedGap(wallMs, cpuMs)) {
            maxBlockedMs_ = std::max(maxBlockedMs_, wallMs);
        } else {
            maxDescheduledMs_ = std::max(maxDescheduledMs_, wallMs);
        }
    }

    ThreadTimePoint last_;
    int64_t maxBlockedMs_ = 0;
    int64_t maxDescheduledMs_ = 0;
};
