// Drives the real MainWindow (docs/adr/0014). The latency test is the audit
// evidence that the UI thread never blocks: every user action that touches
// the index is exercised against a 1M-entry index while a 5 ms timer
// measures the longest gap between event-loop turns.

#include <sys/prctl.h>

#include <QAction>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include "indexer/IFileSystemScanner.h"
#include "search/SearchEngine.h"
#include "settings/PathUtils.h"
#include "settings/Settings.h"
#include "storage/IndexSerializer.h"
#include "storage/IndexStore.h"
#include "ui/MainWindow.h"
#include "ui/ResultModel.h"
#include "ui/ResultView.h"
#include "ui/SearchLineEdit.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

using namespace indexed;
namespace fs = std::filesystem;

namespace {

// 1M in CI. The 5M manual check in docs/adr/0014 runs this same test with
// INDEXED_LATENCY_ENTRIES=5000000.
const int kEntries = qEnvironmentVariableIntValue("INDEXED_LATENCY_ENTRIES") > 0
                         ? qEnvironmentVariableIntValue("INDEXED_LATENCY_ENTRIES")
                         : 1'000'000;
constexpr qint64 kMaxGapMs = 100;
const std::string kRoot = "/synthetic";
const std::string kExtraRoot = "/extra";
const QString kStubConnection = QStringLiteral("indexed-test-file-manager");

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// "item0000042x": no separator characters, so searching for it is a plain
// substring match that hits exactly one file (a name with '_' or '.' would
// be token-matched against thousands).
std::string ItemQuery(int i) {
    std::string digits = std::to_string(i);
    return "item" + std::string(7 - digits.size(), '0') + digits + "x";
}

FileEntry SyntheticEntry(const std::string& root, int i) {
    FileEntry entry;
    entry.name = ItemQuery(i) + ".txt";
    entry.path = root + "/dir_" + std::to_string(i % 1000) + "/" + entry.name;
    entry.size = static_cast<uint64_t>(i);
    entry.lastModified = 1;
    return entry;
}

// Emits kEntries entries for kRoot and 1,000 for any other root, honouring
// cancellation. Can be held mid-scan to observe the window while indexing.
class SyntheticScanner : public IFileSystemScanner {
public:
    bool FastScanAvailable(const std::string&) const override { return false; }
    void Scan(const ScanOptions& options, ScanCallback onEntry, ProgressCallback onProgress,
              const std::atomic<bool>& cancelToken) override {
        for (const std::string& root : options.rootPaths) {
            const int count = root == kRoot ? kEntries : 1000;
            for (int i = 0; i < count; ++i) {
                if (i % 1000 == 0) {
                    while (hold.load() && !cancelToken.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (cancelToken.load()) {
                        return;
                    }
                    onProgress(static_cast<uint64_t>(i), root);
                }
                onEntry(SyntheticEntry(root, i));
            }
        }
    }
    std::atomic<bool> hold{false};
};

// Records the longest gap between 5 ms timer ticks: how long the UI thread
// went without returning to its event loop.
class GapProbe {
public:
    GapProbe() {
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setInterval(5);
        QObject::connect(&timer_, &QTimer::timeout,
                         [this]() { maxGapMs_ = std::max(maxGapMs_, sinceLastTick_.restart()); });
    }
    void Start() {
        maxGapMs_ = 0;
        sinceLastTick_.start();
        timer_.start();
    }
    qint64 Stop() {
        timer_.stop();
        maxGapMs_ = std::max(maxGapMs_, sinceLastTick_.elapsed());
        return maxGapMs_;
    }

private:
    QTimer timer_;
    QElapsedTimer sinceLastTick_;
    qint64 maxGapMs_ = 0;
};

// Pumps the event loop until `done` or the timeout.
bool PumpUntil(const std::function<bool()>& done, int timeoutMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

QMessageBox* VisibleMessageBox() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* box = qobject_cast<QMessageBox*>(widget); box != nullptr && box->isVisible()) {
            return box;
        }
    }
    return nullptr;
}

// Answers the next message boxes that appear, in order, with `buttons`
// (works for modal and non-modal boxes alike), recording each box's text.
struct AnsweredBoxes {
    std::vector<QString> texts;
};

std::shared_ptr<AnsweredBoxes> AnswerMessageBoxes(
    std::vector<QMessageBox::StandardButton> buttons) {
    auto answered = std::make_shared<AnsweredBoxes>();
    auto* poll = new QTimer();
    poll->setInterval(5);
    auto lastBox = std::make_shared<QMessageBox*>(nullptr);
    QObject::connect(poll, &QTimer::timeout, [poll, buttons, answered, lastBox]() {
        QMessageBox* box = VisibleMessageBox();
        if (box == nullptr || box == *lastBox) {
            return;
        }
        *lastBox = box;
        answered->texts.push_back(box->text());
        box->button(buttons[answered->texts.size() - 1])->click();
        if (answered->texts.size() == buttons.size()) {
            poll->stop();
            poll->deleteLater();
        }
    });
    poll->start();
    return answered;
}

// A FileManager1 service that accepts ShowItems and never replies: the
// "file manager hung" case. Reveal must fall back after its timeout without
// the UI ever waiting on it.
class HangingFileManager : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
public:
    int calls = 0;
public slots:
    void ShowItems(const QStringList&, const QString&) {
        setDelayedReply(true);
        ++calls;
    }
};

// Intercepts QDesktopServices::openUrl so Reveal/Open never launch a real
// file manager or viewer during tests.
class UrlSink : public QObject {
    Q_OBJECT
public:
    int opened = 0;
public slots:
    void Open(const QUrl&) { ++opened; }
};

}  // namespace

class TestMainWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void searchBoxIsDisabledOnlyForRebuildsAndSettingsChanges();
    void decliningElevationReturnsToLocalIndexing();
    void aSettingsChangeSentToAHelperThatExitsIsNotLost();
    void elevatingDuringTheFirstScanKeepsSearchDisabledUntilAnIndexArrives();
    void trashAndDeleteShowRowsAsPendingThenRemoveThem();
    void uiNeverBlocksForMoreThan100Ms();

private:
    std::unique_ptr<MainWindow> MakeWindow(FileOperationRunner runner,
                                           QStringList helperCommand = {"sleep", "60"});
    SearchLineEdit* SearchBox(MainWindow& window) { return window.findChild<SearchLineEdit*>(); }
    ResultModel* Model(MainWindow& window) {
        return qobject_cast<ResultModel*>(window.findChild<ResultView*>()->model());
    }
    ResultView* View(MainWindow& window) { return window.findChild<ResultView*>(); }
    bool WaitForSearchReady(MainWindow& window) {
        return PumpUntil([&]() { return SearchBox(window)->isEnabled(); });
    }
    // Waits for rows matching `query` itself: a refresh of the previous
    // query can still be in flight and must not count as this search's result.
    bool SearchFor(MainWindow& window, const QString& query) {
        SearchBox(window)->setText(query);
        emit SearchBox(window)->SearchRequested(query);
        const std::string needle = query.toStdString();
        return PumpUntil([&]() {
            ResultModel* model = Model(window);
            return model->rowCount() > 0 &&
                   model->EntryAt(0).name.find(needle) != std::string::npos;
        });
    }
    SettingsDialogResult SettingsWithRoots(std::vector<std::string> roots) {
        SettingsDialogResult result;
        result.selectedRoots = std::move(roots);
        result.reindexIntervalHours = 48;
        return result;
    }

    std::string dir_;
    std::string templateIdx_;
    std::string idx_;
    std::string config_;
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<IndexStore> store_;
    SearchEngine engine_;
    SyntheticScanner scanner_;
    UrlSink urls_;
    QProcess busDaemon_;
    HangingFileManager fileManager_;
};

void TestMainWindow::initTestCase() {
    // A private session bus with a FileManager1 that never answers, so
    // Reveal goes through its real asynchronous D-Bus path without touching
    // the desktop's file manager.
    // Dies with the test process, even if the test crashes or times out.
    busDaemon_.setChildProcessModifier([]() { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
    busDaemon_.start("dbus-daemon", {"--session", "--nofork", "--print-address"});
    QVERIFY2(busDaemon_.waitForStarted(5000) && busDaemon_.waitForReadyRead(5000),
             "dbus-daemon is required (Reveal latency check)");
    const QByteArray busAddress = busDaemon_.readLine().trimmed();
    qputenv("DBUS_SESSION_BUS_ADDRESS", busAddress);
    // The stub gets its own connection, like a real file manager in another
    // process: a call to a service on the caller's own connection is handled
    // locally and would return at once even if the UI made it synchronously.
    QDBusConnection stubBus =
        QDBusConnection::connectToBus(QString::fromUtf8(busAddress), kStubConnection);
    QVERIFY(stubBus.isConnected());
    QVERIFY(stubBus.registerService("org.freedesktop.FileManager1"));
    QVERIFY(stubBus.registerObject("/org/freedesktop/FileManager1", &fileManager_,
                                   QDBusConnection::ExportAllSlots));
    QDesktopServices::setUrlHandler("file", &urls_, "Open");

    dir_ = (fs::temp_directory_path() /
            ("indexed_test_mainwindow_" + std::to_string(QCoreApplication::applicationPid())))
               .string();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    templateIdx_ = dir_ + "/template.idx";
    IndexPool pool;
    for (int i = 0; i < kEntries; ++i) {
        pool.AddEntry(SyntheticEntry(kRoot, i));
    }
    QVERIFY(IndexSerializer::Save(templateIdx_, pool, NowNs(), 0));
}

// The synthetic index files are large (about 200 MB at 1M entries, over
// 1 GB at 5M) and /tmp is often RAM-backed: never leave them behind.
void TestMainWindow::cleanupTestCase() {
    std::error_code ignored;
    fs::remove_all(dir_, ignored);
    QDBusConnection::disconnectFromBus(kStubConnection);
    busDaemon_.kill();
    busDaemon_.waitForFinished(2000);
}

void TestMainWindow::init() {
    scanner_.hold = false;
    idx_ = dir_ + "/indexed.idx";
    config_ = dir_ + "/indexed.conf";
    fs::remove(config_);
    fs::copy_file(templateIdx_, idx_, fs::copy_options::overwrite_existing);
    settings_ = std::make_unique<Settings>(config_, dir_);
    settings_->Load();
    settings_->SetSelectedRoots({kRoot});
    settings_->SetExcludedPaths({});
    QVERIFY(settings_->Save());
    store_ = std::make_unique<IndexStore>();
}

void TestMainWindow::cleanup() {
    store_.reset();
    settings_.reset();
}

std::unique_ptr<MainWindow> TestMainWindow::MakeWindow(FileOperationRunner runner,
                                                       QStringList helperCommand) {
    auto window = std::make_unique<MainWindow>(
        *settings_, *store_, engine_, scanner_,
        // No live monitors: the synthetic roots don't exist on disk.
        [](const std::string&) { return std::unique_ptr<IChangeMonitor>(); }, std::move(runner),
        std::move(helperCommand), idx_, dir_ + "/indexed.log");
    window->show();
    return window;
}

// Test 18
void TestMainWindow::searchBoxIsDisabledOnlyForRebuildsAndSettingsChanges() {
    auto window = MakeWindow(MainWindow::RunFileOperation);
    window->StartIndexing(/*force=*/false);
    QVERIFY(WaitForSearchReady(*window));

    scanner_.hold = true;
    window->StartIndexing(/*force=*/true);
    QVERIFY(PumpUntil([&]() { return !SearchBox(*window)->isEnabled(); }, 5000));
    scanner_.hold = false;
    QVERIFY(WaitForSearchReady(*window));

    scanner_.hold = true;
    window->ApplySettings(SettingsWithRoots({kRoot, kExtraRoot}));
    QVERIFY(PumpUntil([&]() { return !SearchBox(*window)->isEnabled(); }, 5000));
    scanner_.hold = false;
    QVERIFY(WaitForSearchReady(*window));

    // Elevated: a reload of the helper's save swaps in without disabling.
    window->findChild<QAction*>("elevateAction")->trigger();
    QVERIFY(PumpUntil([&]() { return !window->findChild<QAction*>("elevateAction")->isEnabled(); },
                      5000));
    bool everDisabled = false;
    IndexPool helperPool;
    helperPool.AddEntry(SyntheticEntry(kRoot, 7));
    QVERIFY(IndexSerializer::Save(idx_, helperPool, NowNs(), 0));
    const auto reloaded = [&]() {
        everDisabled = everDisabled || !SearchBox(*window)->isEnabled();
        std::shared_lock lock(store_->GetSearchMutex());
        return store_->GetPool().Count() == 1;
    };
    QVERIFY(PumpUntil(reloaded, 10000));
    QElapsedTimer settle;
    settle.start();
    PumpUntil([&]() {
        everDisabled = everDisabled || !SearchBox(*window)->isEnabled();
        return settle.elapsed() > 200;
    });
    QVERIFY(!everDisabled);
}

// Review finding: pkexec starts (QProcess::started) before the password
// prompt; declining makes it exit. The window must go back to indexing
// locally instead of staying "elevated" with no helper.
void TestMainWindow::decliningElevationReturnsToLocalIndexing() {
    auto window = MakeWindow(MainWindow::RunFileOperation, QStringList{"false"});
    window->StartIndexing(false);
    QVERIFY(WaitForSearchReady(*window));
    QAction* elevate = window->findChild<QAction*>("elevateAction");

    elevate->trigger();

    QVERIFY(PumpUntil([&]() { return elevate->isEnabled() && SearchBox(*window)->isEnabled(); },
                      10000));
    QTest::qWait(300);
    QVERIFY(elevate->isEnabled());
    QVERIFY(SearchFor(*window, QString::fromStdString(ItemQuery(9))));
}

// Review finding: pkexec starts before its password prompt, so a Settings
// change can be signalled to a "helper" that then exits without applying
// it. Returning to local indexing must apply it (full rebuild), not reload
// the old index.
void TestMainWindow::aSettingsChangeSentToAHelperThatExitsIsNotLost() {
    auto window = MakeWindow(MainWindow::RunFileOperation, QStringList{"sleep", "5"});
    window->StartIndexing(false);
    QVERIFY(WaitForSearchReady(*window));
    QAction* elevate = window->findChild<QAction*>("elevateAction");
    elevate->trigger();
    QVERIFY(PumpUntil([&]() { return !elevate->isEnabled(); }, 5000));

    window->ApplySettings(SettingsWithRoots({kRoot, kExtraRoot}));  // SIGHUP ends `sleep`

    QVERIFY(PumpUntil([&]() { return elevate->isEnabled(); }, 10000));
    QVERIFY(PumpUntil(
        [&]() {
            std::shared_lock lock(store_->GetSearchMutex());
            return store_->GetPool().Count() == static_cast<size_t>(kEntries) + 1000;
        },
        60000));
}

// Review finding: elevating cancels a first scan that hasn't produced an
// index yet; search must stay disabled until the helper's index arrives.
void TestMainWindow::elevatingDuringTheFirstScanKeepsSearchDisabledUntilAnIndexArrives() {
    fs::remove(idx_);
    auto window = MakeWindow(MainWindow::RunFileOperation);
    scanner_.hold = true;
    window->StartIndexing(false);
    QTest::qWait(200);

    window->findChild<QAction*>("elevateAction")->trigger();
    QVERIFY(PumpUntil([&]() { return !window->findChild<QAction*>("elevateAction")->isEnabled(); },
                      5000));
    QTest::qWait(300);  // the cancelled scan reports "not busy"
    QVERIFY(!SearchBox(*window)->isEnabled());

    IndexPool helperPool;
    helperPool.AddEntry(SyntheticEntry(kRoot, 4));
    QVERIFY(IndexSerializer::Save(idx_, helperPool, NowNs(), 0));
    QVERIFY(PumpUntil([&]() { return SearchBox(*window)->isEnabled(); }, 10000));
    scanner_.hold = false;
}

// Test 19
void TestMainWindow::trashAndDeleteShowRowsAsPendingThenRemoveThem() {
    std::atomic<bool> holdOps{true};
    auto runner = [&holdOps](const FileOperation& op) -> std::optional<std::string> {
        while (holdOps.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (op.path.find(ItemQuery(2)) != std::string::npos ||
            op.path.find(ItemQuery(3)) != std::string::npos) {
            return std::string("permission denied");
        }
        return std::nullopt;
    };
    auto window = MakeWindow(runner);
    window->StartIndexing(false);
    QVERIFY(WaitForSearchReady(*window));
    QVERIFY(SearchFor(*window, QString::fromStdString(ItemQuery(1))));
    QCOMPARE(Model(*window)->rowCount(), 1);
    const QString trashed = QString::fromStdString(Model(*window)->FullPath(0));

    // Released from another thread, so a UI that (wrongly) ran the
    // operation itself would stall here rather than deadlock the test.
    std::thread releaser([&holdOps]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        holdOps = false;
    });
    emit View(*window)->TrashRequested({trashed});
    const bool pendingRightAway = Model(*window)->rowCount() == 1 && Model(*window)->IsPending(0);
    const bool removed = PumpUntil([&]() { return Model(*window)->rowCount() == 0; }, 10000);
    releaser.join();
    QVERIFY(pendingRightAway);
    QVERIFY(removed);

    // Two failures in one request: one dialog listing both.
    const QString failA = QString::fromStdString(SyntheticEntry(kRoot, 2).path);
    const QString failB = QString::fromStdString(SyntheticEntry(kRoot, 3).path);
    auto answered = AnswerMessageBoxes({QMessageBox::Yes, QMessageBox::Ok});
    emit View(*window)->DeletePermanentlyRequested({failA, failB});
    QVERIFY(PumpUntil([&]() { return answered->texts.size() == 2; }, 10000));
    QVERIFY(answered->texts[1].contains(failA));
    QVERIFY(answered->texts[1].contains(failB));
    QTest::qWait(300);
    QVERIFY(VisibleMessageBox() == nullptr);  // one dialog, not one per failure
}

// Test 20: the audit test.
void TestMainWindow::uiNeverBlocksForMoreThan100Ms() {
#if defined(__SANITIZE_ADDRESS__)
    QSKIP(
        "UI timings under AddressSanitizer aren't representative; the debug and release "
        "CI jobs run this check.");
#endif
    auto slowRunner = [](const FileOperation&) -> std::optional<std::string> {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // e.g. a cross-drive trash
        return std::nullopt;
    };
    GapProbe probe;
    QStringList failures;
    const auto measure = [&](const char* action, const std::function<void()>& trigger,
                             const std::function<bool()>& done) {
        probe.Start();
        trigger();
        const bool finished = PumpUntil(done);
        const qint64 gap = probe.Stop();
        qInfo("%-40s longest UI pause %lld ms", action, static_cast<long long>(gap));
        if (!finished) {
            failures << QString("%1: did not finish").arg(action);
        } else if (gap > kMaxGapMs) {
            failures << QString("%1: UI blocked for %2 ms").arg(action).arg(gap);
        }
    };

    std::unique_ptr<MainWindow> window;
    // Done once the search box has been disabled (the work started) and is
    // enabled again (it finished), so the whole operation is measured.
    const auto busyCycle = [&]() {
        auto sawBusy = std::make_shared<bool>(false);
        return [&, sawBusy]() {
            *sawBusy = *sawBusy || !SearchBox(*window)->isEnabled();
            return *sawBusy && SearchBox(*window)->isEnabled();
        };
    };
    measure(
        "startup load",
        [&]() {
            window = MakeWindow(slowRunner);
            window->StartIndexing(false);
        },
        [&]() { return SearchBox(*window)->isEnabled(); });

    measure(
        "typing ten queries",
        [&]() {
            for (const char* query : {"it", "ite", "item", "item0", "item00", "item001", "item0012",
                                      "item00123", "item001234", "item0012345"}) {
                SearchBox(*window)->setText(query);
                emit SearchBox(*window)->SearchRequested(query);
                QCoreApplication::processEvents();
            }
        },
        [&]() {
            return Model(*window)->rowCount() > 0 &&
                   !window->findChild<SearchCoordinator*>()->IsSearching();
        });

    measure("rebuild", [&]() { window->StartIndexing(true); }, busyCycle());

    measure(
        "rebuild again mid-rebuild",
        [&]() {
            window->StartIndexing(true);
            QVERIFY(PumpUntil([&]() { return !SearchBox(*window)->isEnabled(); }, 5000));
            window->StartIndexing(true);
        },
        [&]() { return SearchBox(*window)->isEnabled(); });
    {
        // The cancelled first rebuild was never swapped in: the index is the
        // second one, complete.
        std::shared_lock lock(store_->GetSearchMutex());
        QCOMPARE(store_->GetPool().Count(), static_cast<size_t>(kEntries));
    }

    measure(
        "settings: add a root",
        [&]() { window->ApplySettings(SettingsWithRoots({kRoot, kExtraRoot})); }, busyCycle());

    measure(
        "settings: remove a root", [&]() { window->ApplySettings(SettingsWithRoots({kRoot})); },
        busyCycle());

    QVERIFY(SearchFor(*window, QString::fromStdString(ItemQuery(5))));
    QCOMPARE(Model(*window)->rowCount(), 1);
    const QString trashed = QString::fromStdString(Model(*window)->FullPath(0));
    measure(
        "move to Trash (slow)", [&]() { emit View(*window)->TrashRequested({trashed}); },
        [&]() { return Model(*window)->rowCount() == 0; });

    QVERIFY(SearchFor(*window, QString::fromStdString(ItemQuery(6))));
    QCOMPARE(Model(*window)->rowCount(), 1);
    const QString deleted = QString::fromStdString(Model(*window)->FullPath(0));
    measure(
        "delete permanently (slow)",
        [&]() {
            AnswerMessageBoxes({QMessageBox::Yes});
            emit View(*window)->DeletePermanentlyRequested({deleted});
        },
        [&]() { return Model(*window)->rowCount() == 0; });

    QVERIFY(SearchFor(*window, QString::fromStdString(ItemQuery(8))));
    const QString revealed = QString::fromStdString(Model(*window)->FullPath(0));
    const int openedBefore = urls_.opened;
    const int showItemsBefore = fileManager_.calls;
    measure(
        "reveal (file manager never replies)",
        [&]() { emit View(*window)->RevealRequested(revealed); },
        [&]() { return urls_.opened > openedBefore; });  // the fallback, after the call times out
    QCOMPARE(fileManager_.calls, showItemsBefore + 1);

    std::shared_ptr<AnsweredBoxes> openAnswer;
    measure(
        "open a file that no longer exists",
        [&]() {
            openAnswer = AnswerMessageBoxes({QMessageBox::No});
            emit View(*window)->OpenRequested(QString::fromStdString(dir_ + "/gone.txt"));
        },
        [&]() { return openAnswer->texts.size() == 1; });

    window->findChild<QAction*>("elevateAction")->trigger();
    QVERIFY(PumpUntil([&]() { return !window->findChild<QAction*>("elevateAction")->isEnabled(); },
                      5000));
    // Only the rename is the trigger: the helper writes the file in another
    // process, so copying it must not count as UI time.
    fs::copy_file(templateIdx_, idx_ + ".new", fs::copy_options::overwrite_existing);
    measure(
        "reload the helper's save", [&]() { fs::rename(idx_ + ".new", idx_); },
        [&]() {
            return window->findChild<QLabel*>("indexStatusLabel")
                ->text()
                .contains(QString::fromStdString(FormatFileCount(static_cast<uint64_t>(kEntries))));
        });

    scanner_.hold = false;
    window.reset();
    window = MakeWindow(slowRunner);
    window->StartIndexing(false);
    QVERIFY(WaitForSearchReady(*window));
    window->StartIndexing(true);
    QVERIFY(PumpUntil([&]() { return !SearchBox(*window)->isEnabled(); }, 5000));
    measure(
        "close during a rebuild", [&]() { window->close(); },
        [&]() { return !window->isVisible(); });
    QElapsedTimer teardown;
    teardown.start();
    window.reset();
    qInfo("%-40s %lld ms", "teardown after close", static_cast<long long>(teardown.elapsed()));
    if (teardown.elapsed() > 1000) {
        failures << QString("teardown after close took %1 ms").arg(teardown.elapsed());
    }

    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));
}

QTEST_MAIN(TestMainWindow)
#include "test_MainWindow.moc"
