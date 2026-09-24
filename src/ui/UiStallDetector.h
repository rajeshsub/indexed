#pragma once

#include <QObject>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace indexed {

// Detects and reports UI-thread freezes (docs/adr/0014). A background
// thread posts a ping to the UI thread every 50 ms and measures how long the
// UI takes to run it. A reply slower than `threshold` is reported once with
// its duration; a freeze still in progress after 5 s is reported as ongoing
// (once), so a UI that never recovers is recorded too. `report` runs on the
// detector's own thread, never the (possibly frozen) UI thread.
class UiStallDetector : public QObject {
    Q_OBJECT

public:
    using Reporter = std::function<void(std::chrono::milliseconds duration, bool ongoing)>;

    // ongoingAfter is parameterized for tests (5 s in production).
    UiStallDetector(std::chrono::milliseconds threshold, Reporter report, QObject* parent = nullptr,
                    std::chrono::milliseconds ongoingAfter = std::chrono::seconds(5));
    ~UiStallDetector() override;

    UiStallDetector(const UiStallDetector&) = delete;
    UiStallDetector& operator=(const UiStallDetector&) = delete;

private:
    void Run();

    std::chrono::milliseconds threshold_;
    std::chrono::milliseconds ongoingAfter_;
    Reporter report_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace indexed
