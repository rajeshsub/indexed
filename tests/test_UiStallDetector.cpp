#include <QElapsedTimer>
#include <QTest>

#include "ui/UiStallDetector.h"
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using indexed::UiStallDetector;

namespace {

struct Report {
    std::chrono::milliseconds duration{0};
    bool ongoing = false;
};

class Reports {
public:
    UiStallDetector::Reporter Reporter() {
        return [this](std::chrono::milliseconds duration, bool ongoing) {
            std::lock_guard<std::mutex> lock(mutex_);
            reports_.push_back({duration, ongoing});
        };
    }
    std::vector<Report> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return reports_;
    }

private:
    std::mutex mutex_;
    std::vector<Report> reports_;
};

void RunEventLoopFor(std::chrono::milliseconds duration) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < duration.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

class TestUiStallDetector : public QObject {
    Q_OBJECT

private slots:
    void reportsABlockedUiThreadOnceWithItsDuration() {
        Reports reports;
        UiStallDetector detector(std::chrono::milliseconds(250), reports.Reporter());
        RunEventLoopFor(std::chrono::milliseconds(200));

        std::this_thread::sleep_for(std::chrono::milliseconds(400));  // the UI thread freezes
        RunEventLoopFor(std::chrono::milliseconds(300));

        const std::vector<Report> snapshot = reports.Snapshot();
        QCOMPARE(snapshot.size(), size_t{1});
        QVERIFY(snapshot[0].duration >= std::chrono::milliseconds(250));
        QVERIFY(!snapshot[0].ongoing);
    }

    void reportsNothingWhileTheUiIsResponsive() {
        Reports reports;
        UiStallDetector detector(std::chrono::milliseconds(250), reports.Reporter());

        RunEventLoopFor(std::chrono::milliseconds(700));

        QCOMPARE(reports.Snapshot().size(), size_t{0});
    }
};

QTEST_MAIN(TestUiStallDetector)
#include "test_UiStallDetector.moc"
