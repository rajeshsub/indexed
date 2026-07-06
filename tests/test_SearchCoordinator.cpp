#include <QSignalSpy>
#include <QTest>

#include "ui/SearchCoordinator.h"
#include <atomic>
#include <chrono>
#include <thread>

using indexed::DisplayEntry;
using indexed::FileEntry;
using indexed::IndexPool;
using indexed::IndexStore;
using indexed::ISearchEngine;
using indexed::SearchCoordinator;
using indexed::SearchOptions;
using indexed::SearchResult;

namespace {

// Deterministic stand-in engine: records queries, returns one result per
// call, optionally blocking until released to exercise cancellation.
class StubEngine : public ISearchEngine {
public:
    std::vector<SearchResult> Search(const IndexPool& pool, std::string_view query,
                                     const SearchOptions& /*options*/,
                                     const std::atomic<bool>& cancelToken) override {
        ++calls;
        lastQuery = std::string(query);
        while (blockUntilReleased.load() && !cancelToken.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (cancelToken.load()) {
            wasCancelled = true;
            return {};
        }
        std::vector<SearchResult> results;
        if (pool.Count() > 0) {
            results.push_back({0, 0, 1});
        }
        return results;
    }

    std::atomic<int> calls{0};
    std::atomic<bool> blockUntilReleased{false};
    std::atomic<bool> wasCancelled{false};
    std::string lastQuery;
};

}  // namespace

class TestSearchCoordinator : public QObject {
    Q_OBJECT

private slots:
    void init();
    void shortQueryEmitsQueryTooShortAndNeverSearches();
    void debounceCoalescesRapidKeystrokes();
    void resultsArriveAsDisplayEntries();
    void newQueryCancelsRunningSearch();

private:
    StubEngine engine_;
    IndexStore store_;
    bool storePopulated_ = false;
};

void TestSearchCoordinator::init() {
    engine_.calls = 0;
    engine_.blockUntilReleased = false;
    engine_.wasCancelled = false;
    if (!storePopulated_) {
        FileEntry entry;
        entry.name = "hit.txt";
        entry.path = "/data/hit.txt";
        entry.size = 42;
        entry.lastModified = 1'700'000'000ULL * 1'000'000'000ULL;
        store_.BeginWrite();
        store_.AddEntry(entry);
        store_.EndWrite();
        storePopulated_ = true;
    }
}

void TestSearchCoordinator::shortQueryEmitsQueryTooShortAndNeverSearches() {
    SearchCoordinator coordinator(engine_, store_, /*debounceMs=*/10);
    QSignalSpy tooShort(&coordinator, &SearchCoordinator::QueryTooShort);
    QSignalSpy ready(&coordinator, &SearchCoordinator::ResultsReady);

    coordinator.SetQuery("a");
    QVERIFY(tooShort.wait(200));
    QCOMPARE(ready.count(), 0);
    QCOMPARE(engine_.calls.load(), 0);
}

void TestSearchCoordinator::debounceCoalescesRapidKeystrokes() {
    SearchCoordinator coordinator(engine_, store_, /*debounceMs=*/50);
    QSignalSpy ready(&coordinator, &SearchCoordinator::ResultsReady);

    coordinator.SetQuery("do");
    coordinator.SetQuery("doc");
    coordinator.SetQuery("docs");
    QVERIFY(ready.wait(2000));

    QCOMPARE(engine_.calls.load(), 1);
    QCOMPARE(engine_.lastQuery, std::string("docs"));
}

void TestSearchCoordinator::resultsArriveAsDisplayEntries() {
    SearchCoordinator coordinator(engine_, store_, /*debounceMs=*/10);
    QSignalSpy ready(&coordinator, &SearchCoordinator::ResultsReady);

    coordinator.SetQuery("hit");
    QVERIFY(ready.wait(2000));

    const auto entries = ready.at(0).at(0).value<std::vector<DisplayEntry>>();
    QCOMPARE(entries.size(), size_t{1});
    QCOMPARE(entries[0].name, std::string("hit.txt"));
    QCOMPARE(entries[0].parentDir, std::string("/data"));
    QCOMPARE(ready.at(0).at(1).toBool(), false);
}

void TestSearchCoordinator::newQueryCancelsRunningSearch() {
    SearchCoordinator coordinator(engine_, store_, /*debounceMs=*/10);
    QSignalSpy ready(&coordinator, &SearchCoordinator::ResultsReady);

    engine_.blockUntilReleased = true;
    coordinator.SetQuery("first");
    QTRY_VERIFY_WITH_TIMEOUT(coordinator.IsSearching(), 2000);

    // Still blocking: the only way the first search ends is via its cancel
    // token, which the restart path sets when the second query's debounce
    // fires. Wait for that observable cancel before releasing the block.
    coordinator.SetQuery("second");
    QTRY_VERIFY_WITH_TIMEOUT(engine_.wasCancelled.load(), 2000);

    engine_.blockUntilReleased = false;  // let the *second* search complete
    QVERIFY(ready.wait(2000));
    QCOMPARE(engine_.lastQuery, std::string("second"));
}

QTEST_MAIN(TestSearchCoordinator)
#include "test_SearchCoordinator.moc"
