#include "ui/UiStallDetector.h"

#include <memory>
#include <utility>

namespace indexed {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kPingInterval = std::chrono::milliseconds(50);
constexpr auto kReplyPoll = std::chrono::milliseconds(10);
constexpr auto kOngoingAfter = std::chrono::seconds(5);

}  // namespace

UiStallDetector::UiStallDetector(std::chrono::milliseconds threshold, Reporter report,
                                 QObject* parent)
    : QObject(parent), threshold_(threshold), report_(std::move(report)) {
    thread_ = std::thread([this]() { Run(); });
}

UiStallDetector::~UiStallDetector() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void UiStallDetector::Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        const Clock::time_point sentAt = Clock::now();
        auto repliedAtNs = std::make_shared<std::atomic<int64_t>>(0);
        // Queued to the UI thread; dropped by Qt if this object is destroyed
        // before it runs.
        QMetaObject::invokeMethod(
            this, [repliedAtNs]() { repliedAtNs->store(Clock::now().time_since_epoch().count()); },
            Qt::QueuedConnection);

        bool reportedOngoing = false;
        while (!stopping_ && repliedAtNs->load() == 0) {
            cv_.wait_for(lock, kReplyPoll);
            if (!reportedOngoing && Clock::now() - sentAt >= kOngoingAfter) {
                reportedOngoing = true;
                report_(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sentAt),
                    /*ongoing=*/true);
            }
        }
        if (stopping_) {
            return;
        }

        const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::time_point(Clock::duration(repliedAtNs->load())) - sentAt);
        if (latency > threshold_) {
            report_(latency, /*ongoing=*/false);
        }
        cv_.wait_for(lock, kPingInterval, [this]() { return stopping_; });
    }
}

}  // namespace indexed
