#include "ui/MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include "Version.h"
#include "ui/DisplayEntry.h"
#include "ui/SettingsDialog.h"
#include "ui/StatusText.h"
#include <chrono>

namespace indexed {

namespace {

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

MainWindow::MainWindow(Settings& settings, IndexStore& store, ISearchEngine& engine,
                       IFileSystemScanner& scanner, std::string idxFilePath, std::string logPath,
                       QWidget* parent)
    : QMainWindow(parent),
      settings_(settings),
      store_(store),
      scanner_(scanner),
      idxFilePath_(std::move(idxFilePath)),
      logPath_(std::move(logPath)) {
    setWindowTitle("indexed");
    resize(900, 600);

    // No live monitors in M4 -- the factory hands Indexer a null monitor per
    // root; FanotifyMonitor/InotifyWatcher arrive in M5.
    indexer_ = std::make_unique<Indexer>(
        scanner_, store_, [](const std::string&) { return std::unique_ptr<IChangeMonitor>(); });

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    searchBox_ = new SearchLineEdit(central);
    QFont searchFont = searchBox_->font();
    searchFont.setPixelSize(20);
    searchBox_->setFont(searchFont);
    layout->addWidget(searchBox_);

    resultModel_ = new ResultModel(this);
    resultView_ = new ResultView(central);
    resultView_->SetResultModel(resultModel_);
    layout->addWidget(resultView_);

    setCentralWidget(central);
    statusBar()->showMessage("Ready.");

    // SearchLineEdit owns the 150 ms debounce + 2-char gate, so the
    // coordinator's own debounce is set to zero -- double-debouncing would
    // just add latency.
    coordinator_ = new SearchCoordinator(engine, store_, /*debounceMs=*/0, this);

    BuildMenus();
    WireSearch();
    WireResultActions();

    searchBox_->setFocus();
}

MainWindow::~MainWindow() {
    JoinIndexThread();
}

void MainWindow::JoinIndexThread() {
    if (indexThread_.joinable()) {
        indexThread_.join();
    }
}

void MainWindow::BuildMenus() {
    QMenu* searchMenu = menuBar()->addMenu(tr("&Search"));
    const auto addToggle = [this, searchMenu](const QString& text, const QString& shortcut) {
        QAction* action = searchMenu->addAction(text);
        action->setCheckable(true);
        action->setShortcut(QKeySequence(shortcut));
        action->setEnabled(false);  // disabled until an index exists (§19)
        connect(action, &QAction::toggled, this, [this](bool) {
            SearchOptions options;
            options.useRegex = regexAction_->isChecked();
            options.caseSensitive = caseAction_->isChecked();
            options.wholeWord = wholeWordAction_->isChecked();
            options.matchPath = matchPathAction_->isChecked();
            options.ignoreDiacritics = diacriticsAction_->isChecked();
            coordinator_->SetOptions(options);
            // Re-run the current query under the new options.
            if (searchBox_->text().size() >= 2) {
                coordinator_->SetQuery(searchBox_->text());
            }
        });
        return action;
    };
    regexAction_ = addToggle(tr("Regular Expression"), "Alt+1");
    caseAction_ = addToggle(tr("Case Sensitive"), "Alt+2");
    wholeWordAction_ = addToggle(tr("Whole Word"), "Alt+3");
    matchPathAction_ = addToggle(tr("Match Path"), "Alt+4");
    diacriticsAction_ = addToggle(tr("Ignore Diacritics"), "Alt+5");

    QMenu* indexMenu = menuBar()->addMenu(tr("&Index"));
    QAction* rebuild = indexMenu->addAction(tr("Rebuild Index Now"));
    connect(rebuild, &QAction::triggered, this, [this]() { StartIndexing(/*force=*/true); });
    indexMenu->addSeparator();
    QAction* settingsAction = indexMenu->addAction(tr("Settings…"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::ShowSettingsDialog);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* openLog = helpMenu->addAction(tr("Open Log File"));
    connect(openLog, &QAction::triggered, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(logPath_)));
    });
    helpMenu->addSeparator();
    QAction* about = helpMenu->addAction(tr("About indexed…"));
    connect(about, &QAction::triggered, this, &MainWindow::ShowAbout);
}

void MainWindow::WireSearch() {
    connect(searchBox_, &SearchLineEdit::SearchRequested, this,
            [this](const QString& query) { coordinator_->SetQuery(query); });
    connect(searchBox_, &SearchLineEdit::StatusMessageChanged, this,
            [this](const QString& message) { statusBar()->showMessage(message); });
    connect(searchBox_, &SearchLineEdit::NavigateResultsRequested, this, [this](Qt::Key) {
        if (resultModel_->rowCount() > 0) {
            resultView_->setFocus();
            if (!resultView_->currentIndex().isValid()) {
                resultView_->setCurrentIndex(resultModel_->index(0, 0));
            }
        }
    });
    connect(coordinator_, &SearchCoordinator::ResultsReady, this,
            [this](std::vector<DisplayEntry> entries, bool capped) {
                const size_t count = entries.size();
                resultModel_->SetEntries(std::move(entries));
                statusBar()->showMessage(QString::fromStdString(ResultCountText(count, capped)));
            });
}

void MainWindow::WireResultActions() {
    connect(resultView_, &ResultView::OpenRequested, this,
            [this](const QString& path) { OpenPath(path); });
    connect(resultView_, &ResultView::RevealRequested, this,
            [this](const QString& path) { RevealPath(path); });
    connect(resultView_, &ResultView::CutRequested, this,
            [this](const QStringList& paths) { CutToClipboard(paths); });
    connect(resultView_, &ResultView::TrashRequested, this,
            [this](const QStringList& paths) { TrashPaths(paths); });
}

void MainWindow::OpenPath(const QString& path) {
    if (!QFileInfo::exists(path)) {
        const auto answer = QMessageBox::question(
            this, tr("indexed"),
            tr("The file no longer exists. It may have been moved or deleted.\n"
               "Rebuild the index now?"));
        if (answer == QMessageBox::Yes) {
            StartIndexing(/*force=*/true);
        }
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::RevealPath(const QString& path) {
    // FileManager1 D-Bus reveal, falling back to opening the parent dir via
    // xdg-open semantics whenever the call fails (no such interface, no
    // session bus -- indexed-plan.md §17 risk 9).
    QDBusInterface fileManager("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                               "org.freedesktop.FileManager1");
    bool ok = false;
    if (fileManager.isValid()) {
        const QDBusMessage reply = fileManager.call(
            "ShowItems", QStringList{QUrl::fromLocalFile(path).toString()}, QString());
        ok = reply.type() != QDBusMessage::ErrorMessage;
    }
    if (!ok) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    }
}

void MainWindow::CutToClipboard(const QStringList& paths) {
    // Standard file-manager cut convention: uri-list plus the GNOME cut
    // marker so paste in Nautilus/Thunar moves instead of copies.
    auto* mime = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnomeData = "cut";
    for (const QString& path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls.append(url);
        gnomeData += "\n" + url.toString().toUtf8();
    }
    mime->setUrls(urls);
    mime->setData("x-special/gnome-copied-files", gnomeData);
    QApplication::clipboard()->setMimeData(mime);
}

void MainWindow::TrashPaths(const QStringList& paths) {
    for (const QString& path : paths) {
        QFile file(path);
        if (file.moveToTrash()) {
            store_.ApplyRemove(path.toStdString());
        }
    }
    // Refresh the current query so trashed entries disappear immediately.
    if (searchBox_->text().size() >= 2) {
        coordinator_->SetQuery(searchBox_->text());
    }
}

ScanOptions MainWindow::CurrentScanOptions() const {
    ScanOptions options;
    options.rootPaths = settings_.SelectedRoots();
    options.excludedPaths = settings_.ExcludedPaths();
    return options;
}

void MainWindow::SetSearchUiEnabled(bool enabled) {
    searchBox_->setEnabled(enabled);
    regexAction_->setEnabled(enabled);
    caseAction_->setEnabled(enabled);
    wholeWordAction_->setEnabled(enabled);
    matchPathAction_->setEnabled(enabled);
    diacriticsAction_->setEnabled(enabled);
}

void MainWindow::UpdateIdleStatus() {
    statusBar()->showMessage(QString::fromStdString(IndexSummaryText(
        store_.GetPool().Count(), settings_.SelectedRoots(), lastBuildAgeSeconds_)));
}

void MainWindow::StartIndexing(bool force) {
    JoinIndexThread();
    SetSearchUiEnabled(false);  // §19: disable search box while indexing
    statusBar()->showMessage(tr("Indexing…"));

    const ScanOptions options = CurrentScanOptions();
    const uint64_t staleSeconds = static_cast<uint64_t>(settings_.ReindexIntervalHours()) * 3600ULL;

    indexThread_ = std::thread([this, force, options, staleSeconds]() {
        const uint64_t nowNs = NowNs();
        indexer_->StartIndexing(force, options, idxFilePath_, nowNs, staleSeconds);
        const uint64_t ageSeconds = store_.GetIndexAgeSeconds(NowNs());
        // Hop back to the GUI thread before touching any widget.
        QMetaObject::invokeMethod(
            this,
            [this, ageSeconds]() {
                lastBuildAgeSeconds_ = ageSeconds;
                SetSearchUiEnabled(true);
                UpdateIdleStatus();
                searchBox_->setFocus();  // §19: auto-focus after indexing
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::ShowSettingsDialog() {
    SettingsDialogInitialState initial;
    initial.selectedRoots = settings_.SelectedRoots();
    initial.excludedPaths = settings_.ExcludedPaths();
    initial.reindexIntervalHours = settings_.ReindexIntervalHours();

    SettingsDialog dialog(initial, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const SettingsDialogResult result = dialog.Result();
    const std::vector<std::string> oldRoots = settings_.SelectedRoots();
    settings_.SetSelectedRoots(result.selectedRoots);
    settings_.SetExcludedPaths(result.excludedPaths);
    settings_.SetReindexIntervalHours(result.reindexIntervalHours);
    if (!settings_.Save()) {
        QMessageBox::warning(this, tr("indexed"), QString::fromStdString(settings_.LastError()));
        return;
    }

    // §19: diff old vs new roots -> incremental IndexPaths/RemovePaths;
    // full rebuild when both sides changed.
    const RootsDiff diff = DiffRoots(oldRoots, result.selectedRoots);
    if (!diff.added.empty() && !diff.removed.empty()) {
        StartIndexing(/*force=*/true);
        return;
    }
    JoinIndexThread();
    if (!diff.added.empty()) {
        indexThread_ = std::thread([this, added = diff.added]() {
            indexer_->IndexPaths(added);
            QMetaObject::invokeMethod(this, [this]() { UpdateIdleStatus(); }, Qt::QueuedConnection);
        });
    } else if (!diff.removed.empty()) {
        indexer_->RemovePaths(diff.removed);
        UpdateIdleStatus();
    }
}

void MainWindow::ShowAbout() {
    QMessageBox::about(this, tr("About indexed"),
                       tr("<b>indexed v%1</b><br>Blazingly fast Linux file search and "
                          "indexer.<br><a href=\"https://github.com/rajeshsub/indexed\">"
                          "github.com/rajeshsub/indexed</a>")
                           .arg(QString::fromStdString(std::string(kVersion))));
}

}  // namespace indexed
