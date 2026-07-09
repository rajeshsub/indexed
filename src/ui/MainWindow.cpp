#include "ui/MainWindow.h"

#include <unistd.h>

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
#include "indexer/InotifyWatcher.h"
#include "indexer/StatusFile.h"
#include "storage/IndexSerializer.h"
#include "ui/DisplayEntry.h"
#include "ui/SettingsDialog.h"
#include "ui/StatusText.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>

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
      logPath_(std::move(logPath)),
      statusFilePath_(
          (std::filesystem::path(idxFilePath_).parent_path() / "indexed.status").string()) {
    setWindowTitle("indexed");
    resize(900, 600);

    // Unprivileged default (§7.2/§9): InotifyWatcher per root. The GUI
    // process never has CAP_SYS_ADMIN, so this is the only backend it could
    // ever use for its own local monitoring; FanotifyMonitor only runs
    // inside the privileged indexed-helper (see ElevateForFullAccess).
    indexer_ = std::make_unique<Indexer>(scanner_, store_, [](const std::string&) {
        return std::unique_ptr<IChangeMonitor>(std::make_unique<InotifyWatcher>());
    });

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

    // Permanent widgets: never hidden/overwritten by a transient
    // showMessage() (unlike the result count, indexing progress, and
    // hotplug notices, which all share that transient slot).
    indexStatusLabel_ = new QLabel(tr("No index yet."), this);
    statusBar()->addPermanentWidget(indexStatusLabel_);
    searchOptionsLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(searchOptionsLabel_);

    // SearchLineEdit owns the 150 ms debounce + 2-char gate, so the
    // coordinator's own debounce is set to zero -- double-debouncing would
    // just add latency.
    coordinator_ = new SearchCoordinator(engine, store_, /*debounceMs=*/0, this);

    BuildMenus();  // creates regexAction_ etc. -- must run before the label read below
    UpdateSearchOptionsLabel();
    WireSearch();
    WireResultActions();

    searchBox_->setFocus();
    StartHotplugWatcher();
}

MainWindow::~MainWindow() {
    StopHotplugWatcher();
    StopLocalMonitoring();
    if (elevated_ && helperProcess_ && helperProcess_->state() == QProcess::Running) {
        SendSignalToHelper(SIGTERM);
        helperProcess_->waitForFinished(3000);
    }
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
            UpdateSearchOptionsLabel();
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
    connect(rebuild, &QAction::triggered, this, [this]() {
        if (elevated_) {
            SendSignalToHelper(SIGUSR1);  // §9.3: reindex-now request
        } else {
            StartIndexing(/*force=*/true);
        }
    });
    indexMenu->addSeparator();
    QAction* settingsAction = indexMenu->addAction(tr("Settings…"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::ShowSettingsDialog);
    indexMenu->addSeparator();
    elevateAction_ = indexMenu->addAction(tr("Elevate for Full-System Access…"));
    connect(elevateAction_, &QAction::triggered, this, &MainWindow::ElevateForFullAccess);

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
    connect(searchBox_, &SearchLineEdit::SearchCleared, this, [this]() {
        coordinator_->SetQuery(QString());  // cancels any in-flight search worker
        resultModel_->SetEntries({});
    });
    connect(coordinator_, &SearchCoordinator::ResultsReady, this,
            [this](std::vector<DisplayEntry> entries, bool capped) {
                if (searchBox_->text().size() < 2) {
                    return;  // box was cleared while this search was in flight
                }
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
    indexStatusLabel_->setText(QString::fromStdString(IndexSummaryText(
        store_.GetPool().Count(), settings_.SelectedRoots(), lastBuildAgeSeconds_)));
}

void MainWindow::UpdateSearchOptionsLabel() {
    SearchOptions options;
    options.useRegex = regexAction_->isChecked();
    options.caseSensitive = caseAction_->isChecked();
    options.wholeWord = wholeWordAction_->isChecked();
    options.matchPath = matchPathAction_->isChecked();
    options.ignoreDiacritics = diacriticsAction_->isChecked();
    searchOptionsLabel_->setText(QString::fromStdString(SearchOptionsText(options)));
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
                if (!elevated_) {
                    StartLocalMonitoring();
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::StartLocalMonitoring() {
    StopLocalMonitoring();
    localMonitorStop_.store(false);
    localMonitorThread_ = std::thread(
        [this]() { indexer_->StartLiveMonitoring(settings_.SelectedRoots(), localMonitorStop_); });
}

void MainWindow::StopLocalMonitoring() {
    localMonitorStop_.store(true);
    if (localMonitorThread_.joinable()) {
        localMonitorThread_.join();
    }
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

    if (elevated_) {
        // The helper owns indexing while elevated (§7.7); it re-reads the
        // INI itself on SIGHUP rather than the GUI diffing roots locally.
        SendSignalToHelper(SIGHUP);
        return;
    }

    // §19: diff old vs new roots -> incremental IndexPaths/RemovePaths;
    // full rebuild when both sides changed. Either incremental path must
    // also persist the index (or a restart within the reindex interval
    // load-if-fresh's the pre-change index back) and restart local
    // monitoring so it covers the new root set.
    const RootsDiff diff = DiffRoots(oldRoots, result.selectedRoots);
    if (!diff.added.empty() && !diff.removed.empty()) {
        StartIndexing(/*force=*/true);
        return;
    }
    JoinIndexThread();
    if (!diff.added.empty()) {
        StopLocalMonitoring();
        SetSearchUiEnabled(false);  // same UX as a full rebuild (§19)
        statusBar()->showMessage(tr("Indexing…"));
        indexThread_ = std::thread([this, added = diff.added, excluded = result.excludedPaths]() {
            indexer_->IndexPaths(added, excluded);
            indexer_->PersistIndex(idxFilePath_, NowNs());
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    lastBuildAgeSeconds_ = store_.GetIndexAgeSeconds(NowNs());
                    SetSearchUiEnabled(true);
                    UpdateIdleStatus();
                    searchBox_->setFocus();
                    if (!elevated_) {
                        StartLocalMonitoring();
                    }
                },
                Qt::QueuedConnection);
        });
    } else if (!diff.removed.empty()) {
        StopLocalMonitoring();
        indexer_->RemovePaths(diff.removed);
        indexer_->PersistIndex(idxFilePath_, NowNs());
        lastBuildAgeSeconds_ = store_.GetIndexAgeSeconds(NowNs());
        UpdateIdleStatus();
        StartLocalMonitoring();
    }
}

void MainWindow::ElevateForFullAccess() {
    if (elevated_) {
        return;  // one polkit prompt per session (§9.2)
    }

    StopLocalMonitoring();

    helperProcess_ = new QProcess(this);
    helperProcess_->start("pkexec", {"indexed-helper"});
    if (!helperProcess_->waitForStarted(3000)) {
        statusBar()->showMessage(
            tr("Could not start indexed-helper (declined, or pkexec/indexed-helper not found)."),
            8000);
        helperProcess_->deleteLater();
        helperProcess_ = nullptr;
        StartLocalMonitoring();  // elevation failed; resume the unprivileged path
        return;
    }

    elevated_ = true;
    elevateAction_->setEnabled(false);
    elevateAction_->setText(tr("Elevated (full-system access active)"));
    statusBar()->showMessage(tr("Elevated: full-system indexing and monitoring active."), 5000);

    // idxFilePath_ already exists (this GUI's own earlier unprivileged scan
    // wrote it); indexed.status is new -- created only by the helper -- so
    // its containing directory is watched too, to catch its first
    // appearance (indexed-plan.md §9.3).
    helperWatcher_ = new QFileSystemWatcher(this);
    helperWatcher_->addPath(QString::fromStdString(idxFilePath_));
    const QString statusDir =
        QString::fromStdString(std::filesystem::path(statusFilePath_).parent_path().string());
    helperWatcher_->addPath(statusDir);
    connect(helperWatcher_, &QFileSystemWatcher::fileChanged, this,
            &MainWindow::OnHelperFileChanged);
    connect(helperWatcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) {
        const QString statusQPath = QString::fromStdString(statusFilePath_);
        if (QFileInfo::exists(statusQPath) && !helperWatcher_->files().contains(statusQPath)) {
            helperWatcher_->addPath(statusQPath);
        }
        UpdateStatusFromHelperFile();
    });
}

void MainWindow::SendSignalToHelper(int signal) {
    if (helperProcess_ != nullptr && helperProcess_->state() == QProcess::Running) {
        kill(static_cast<pid_t>(helperProcess_->processId()), signal);
    }
}

void MainWindow::OnHelperFileChanged(const QString& path) {
    if (path == QString::fromStdString(idxFilePath_)) {
        ReloadIndexFromDisk();
    } else if (path == QString::fromStdString(statusFilePath_)) {
        UpdateStatusFromHelperFile();
    }
}

void MainWindow::ReloadIndexFromDisk() {
    const IndexSerializer::LoadResult result = IndexSerializer::Load(idxFilePath_);
    if (!result.success) {
        return;  // torn read mid-write by the helper; the next change retries
    }
    store_.LoadPool(result.pool, result.buildTimestampNs, result.lastMonitorStopNs);
    lastBuildAgeSeconds_ = store_.GetIndexAgeSeconds(NowNs());
    if (searchBox_->text().size() >= 2) {
        coordinator_->SetQuery(searchBox_->text());  // refresh visible results
    } else {
        UpdateIdleStatus();
    }
}

void MainWindow::UpdateStatusFromHelperFile() {
    std::ifstream file(statusFilePath_, std::ios::binary);
    if (!file.is_open()) {
        return;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::optional<IndexerStatus> status = ParseStatus(contents.str());
    if (!status) {
        return;  // torn read mid-write; the next change retries
    }
    if (status->state == IndexerState::WatchingForChanges || status->state == IndexerState::Idle) {
        indexStatusLabel_->setText(QString::fromStdString(
            IndexSummaryText(status->filesIndexed, status->locations, status->indexAgeSeconds)));
    } else {
        statusBar()->showMessage(QString::fromStdString(status->message));
    }
}

void MainWindow::StartHotplugWatcher() {
    hotplugStop_.store(false);
    hotplugThread_ = std::thread([this]() {
        const int fd = MountEnumerator::OpenMountInfoFd();
        if (fd < 0) {
            return;
        }
        std::vector<MountInfo> previous = mountEnumerator_.Enumerate();
        while (!hotplugStop_.load()) {
            if (!MountEnumerator::WaitForChange(fd, /*timeoutMs=*/500) || hotplugStop_.load()) {
                continue;
            }
            std::vector<MountInfo> current = mountEnumerator_.Enumerate();
            for (const MountInfo& mount : current) {
                const bool existed = std::any_of(
                    previous.begin(), previous.end(),
                    [&](const MountInfo& p) { return p.mountPoint == mount.mountPoint; });
                if (!existed) {
                    const QString msg =
                        tr("Filesystem %1 mounted — add it in Settings to index it.")
                            .arg(QString::fromStdString(mount.mountPoint));
                    QMetaObject::invokeMethod(
                        this, [this, msg]() { statusBar()->showMessage(msg, 8000); },
                        Qt::QueuedConnection);
                }
            }
            for (const MountInfo& mount : previous) {
                const bool stillMounted = std::any_of(
                    current.begin(), current.end(),
                    [&](const MountInfo& m) { return m.mountPoint == mount.mountPoint; });
                if (!stillMounted) {
                    const QString msg = tr("Filesystem %1 unmounted.")
                                            .arg(QString::fromStdString(mount.mountPoint));
                    QMetaObject::invokeMethod(
                        this, [this, msg]() { statusBar()->showMessage(msg, 8000); },
                        Qt::QueuedConnection);
                }
            }
            previous = std::move(current);
        }
        ::close(fd);
    });
}

void MainWindow::StopHotplugWatcher() {
    hotplugStop_.store(true);
    if (hotplugThread_.joinable()) {
        hotplugThread_.join();
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
