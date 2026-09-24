#include "ui/MainWindow.h"

#include <unistd.h>

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include "Version.h"
#include "indexer/StatusFile.h"
#include "settings/PathUtils.h"
#include "ui/DisplayEntry.h"
#include "ui/SettingsDialog.h"
#include "ui/StatusText.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace indexed {

namespace {

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

MainWindow::MainWindow(Settings& settings, IndexStore& store, ISearchEngine& engine,
                       IFileSystemScanner& scanner, ChangeMonitorFactory monitorFactory,
                       FileOperationRunner fileOperationRunner, QStringList helperCommand,
                       std::string idxFilePath, std::string logPath, QWidget* parent)
    : QMainWindow(parent),
      settings_(settings),
      store_(store),
      scanner_(scanner),
      monitorFactory_(std::move(monitorFactory)),
      fileOperationRunner_(std::move(fileOperationRunner)),
      helperCommand_(std::move(helperCommand)),
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
    // Coalesces bursts of live filesystem changes (e.g. a large copy from a
    // USB drive or a network share) into a single on-screen refresh.
    liveRefreshTimer_ = new QTimer(this);
    liveRefreshTimer_->setSingleShot(true);
    liveRefreshTimer_->setInterval(400);
    connect(liveRefreshTimer_, &QTimer::timeout, this, &MainWindow::RefreshVisibleResults);

    // Every piece of index work runs on IndexService's threads; its
    // callbacks hop back here before touching any widget (docs/adr/0014).
    IndexServiceCallbacks callbacks;
    callbacks.onStatus = [this](const IndexerStatus& status) {
        QMetaObject::invokeMethod(
            this, [this, status]() { OnIndexStatus(status); }, Qt::QueuedConnection);
    };
    callbacks.onBusyChanged = [this](bool busy) {
        QMetaObject::invokeMethod(
            this, [this, busy]() { SetIndexBusy(busy); }, Qt::QueuedConnection);
    };
    callbacks.onIndexChanged = [this](const IndexChange& change) {
        QMetaObject::invokeMethod(
            this, [this, change]() { OnIndexChanged(change); }, Qt::QueuedConnection);
    };
    callbacks.onLiveChange = [this]() {
        QMetaObject::invokeMethod(
            this, [this]() { liveRefreshTimer_->start(); }, Qt::QueuedConnection);
    };
    callbacks.onFileOperationsFinished = [this](const FileOperationReport& report) {
        QMetaObject::invokeMethod(
            this, [this, report]() { OnFileOperationsFinished(report); }, Qt::QueuedConnection);
    };
    service_ = std::make_unique<IndexService>(scanner_, store_, monitorFactory_, idxFilePath_,
                                              fileOperationRunner_, std::move(callbacks), NowNs);

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
    indexStatusLabel_->setObjectName("indexStatusLabel");
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

    // §19: nothing to search until an index exists; the first completed
    // load or scan enables it (SetIndexBusy / OnIndexChanged).
    SetSearchUiEnabled(false);
    searchBox_->setFocus();
    StartHotplugWatcher();
}

std::optional<std::string> MainWindow::RunFileOperation(const FileOperation& operation) {
    if (operation.type == FileOperation::Type::MoveToTrash) {
        if (QFile(QString::fromStdString(operation.path)).moveToTrash()) {
            return std::nullopt;
        }
        return std::string("could not move to Trash");
    }
    std::error_code ec;
    if (std::filesystem::remove(operation.path, ec)) {
        return std::nullopt;
    }
    return ec ? ec.message() : std::string("no such file");
}

MainWindow::~MainWindow() {
    // First, so no service callback can be posted to a half-destroyed
    // window. Cancels a running scan; lets a file operation finish.
    service_->Shutdown();
    StopHotplugWatcher();
    if (helperProcess_ != nullptr) {
        // Stopping (or destroying) the helper on exit must not call back into
        // this half-destroyed window.
        disconnect(helperProcess_, nullptr, this, nullptr);
    }
    if (elevated_ && helperProcess_ && helperProcess_->state() == QProcess::Running) {
        SendSignalToHelper(SIGTERM);
        helperProcess_->waitForFinished(3000);
    }
    {
        std::lock_guard<std::mutex> lock(liveness_->mutex);
        liveness_->alive = false;
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
    connect(rebuild, &QAction::triggered, this, &MainWindow::RebuildIndex);
    indexMenu->addSeparator();
    QAction* settingsAction = indexMenu->addAction(tr("Settings…"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::ShowSettingsDialog);
    indexMenu->addSeparator();
    elevateAction_ = indexMenu->addAction(tr("Elevate for Full-System Access…"));
    elevateAction_->setObjectName("elevateAction");
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
                const bool preserve = preserveSelectionOnNextResults_;
                preserveSelectionOnNextResults_ = false;
                if (searchBox_->text().size() < 2) {
                    return;  // box was cleared while this search was in flight
                }
                const size_t count = entries.size();
                const ResultView::SelectionSnapshot selection =
                    preserve ? resultView_->SnapshotSelection() : ResultView::SelectionSnapshot{};
                resultModel_->SetEntries(std::move(entries));
                // SetEntries's beginResetModel()/endResetModel() replaces the
                // data in engine order; it doesn't re-run whatever sort the
                // header is currently showing (that only happens on a header
                // click), so a sort chosen before this search would silently
                // stop applying -- while the header's arrow kept claiming it
                // was still active -- without this.
                const QHeaderView* header = resultView_->header();
                if (header->sortIndicatorSection() >= 0) {
                    resultView_->sortByColumn(header->sortIndicatorSection(),
                                              header->sortIndicatorOrder());
                }
                if (preserve) {
                    resultView_->RestoreSelection(selection);
                }
                statusBar()->showMessage(QString::fromStdString(ResultCountText(count, capped)));
            });
}

void MainWindow::WireResultActions() {
    connect(resultView_, &ResultView::OpenRequested, this,
            [this](const QString& path) { OpenPath(path); });
    connect(resultView_, &ResultView::RevealRequested, this,
            [this](const QString& path) { RevealPath(path); });
    connect(resultView_, &ResultView::TrashRequested, this,
            [this](const QStringList& paths) { TrashPaths(paths); });
    connect(resultView_, &ResultView::DeletePermanentlyRequested, this,
            [this](const QStringList& paths) { DeletePermanently(paths); });
}

void MainWindow::OpenPath(const QString& path) {
    // The existence check can hang on a dead network mount, so it runs off
    // the UI thread (docs/adr/0014).
    // Detached, so a stat hung on a dead mount can't hold up closing the
    // app either; the result is posted only while the window still exists.
    std::thread([this, path, liveness = liveness_]() {
        const bool exists = QFileInfo::exists(path);
        std::lock_guard<std::mutex> lock(liveness->mutex);
        if (liveness->alive) {
            QMetaObject::invokeMethod(
                this, [this, path, exists]() { OnOpenPathChecked(path, exists); },
                Qt::QueuedConnection);
        }
    }).detach();
}

void MainWindow::OnOpenPathChecked(const QString& path, bool exists) {
    if (exists) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Question, tr("indexed"),
                                tr("The file no longer exists. It may have been moved or "
                                   "deleted.\nRebuild the index now?"),
                                QMessageBox::Yes | QMessageBox::No, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box]() {
        if (box->clickedButton() == box->button(QMessageBox::Yes)) {
            RebuildIndex();
        }
    });
    box->open();
}

void MainWindow::RevealPath(const QString& path) {
    // FileManager1 D-Bus reveal, asynchronously: a hung or missing file
    // manager must never stall the UI (docs/adr/0014). Falls back to opening
    // the parent directory whenever the call fails (indexed-plan.md §17
    // risk 9).
    const auto fallback = [path]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    };
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        fallback();
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1",
                                                       "/org/freedesktop/FileManager1",
                                                       "org.freedesktop.FileManager1", "ShowItems");
    call << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, /*timeout=*/5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [fallback](QDBusPendingCallWatcher* finished) {
                if (finished->isError()) {
                    fallback();
                }
                finished->deleteLater();
            });
}

void MainWindow::DeletePermanently(const QStringList& paths) {
    const QString question =
        paths.size() == 1
            ? tr("Permanently delete this file? This cannot be undone.\n\n%1").arg(paths.first())
            : tr("Permanently delete these %1 files? This cannot be undone.\n\n%2")
                  .arg(paths.size())
                  .arg(paths.join('\n'));
    auto* box = new QMessageBox(QMessageBox::Warning, tr("indexed"), question,
                                QMessageBox::Yes | QMessageBox::No, this);
    box->setDefaultButton(QMessageBox::No);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box, paths]() {
        if (box->clickedButton() == box->button(QMessageBox::Yes)) {
            RunFileOperations(FileOperation::Type::DeletePermanently, paths, tr("delete"));
        }
    });
    box->open();
}

void MainWindow::TrashPaths(const QStringList& paths) {
    RunFileOperations(FileOperation::Type::MoveToTrash, paths, tr("move to Trash"));
}

void MainWindow::RunFileOperations(FileOperation::Type type, const QStringList& paths,
                                   const QString& verb) {
    std::vector<FileOperation> operations;
    operations.reserve(static_cast<size_t>(paths.size()));
    for (const QString& path : paths) {
        operations.push_back(FileOperation{type, path.toStdString()});
        pendingPaths_.insert(path.toStdString());
    }
    resultModel_->SetPendingPaths(pendingPaths_);
    pendingOperationVerbs_.push_back(verb);
    service_->RequestFileOperations(std::move(operations));
}

void MainWindow::OnFileOperationsFinished(const FileOperationReport& report) {
    for (const std::string& path : report.succeeded) {
        pendingPaths_.erase(path);
    }
    QStringList failed;
    for (const auto& [path, reason] : report.failed) {
        pendingPaths_.erase(path);
        failed.append(
            tr("%1 (%2)").arg(QString::fromStdString(path), QString::fromStdString(reason)));
    }
    resultModel_->SetPendingPaths(pendingPaths_);
    // Batches finish in the order they were requested.
    const QString verb =
        pendingOperationVerbs_.empty() ? QString() : pendingOperationVerbs_.front();
    if (!pendingOperationVerbs_.empty()) {
        pendingOperationVerbs_.pop_front();
    }
    ReportDeletionFailures(verb, failed);
}

void MainWindow::ReportDeletionFailures(const QString& verb, const QStringList& failed) {
    if (failed.isEmpty()) {
        return;
    }
    auto* box = new QMessageBox(
        QMessageBox::Warning, tr("indexed"),
        tr("Could not %1 %n file(s):\n\n%2", nullptr, static_cast<int>(failed.size()))
            .arg(verb, failed.join('\n')),
        QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

void MainWindow::RefreshVisibleResults() {
    // Re-run whatever query is on screen so index changes (live monitoring,
    // a delete, a trash) are reflected without the user retyping. A query
    // under 2 chars shows no results, so there is nothing to refresh.
    if (searchBox_->text().size() >= 2) {
        // This re-query triggers a full ResultModel reset, which would drop
        // the user's selection, current row, and keyboard focus while they
        // may be mid-interaction with a result. Flag it so the ResultsReady
        // handler restores those by path; a user-initiated search does not
        // set this and resets normally.
        preserveSelectionOnNextResults_ = true;
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
    indexStatusLabel_->setText(QString::fromStdString(
        IndexSummaryText(lastEntryCount_, settings_.SelectedRoots(), lastBuildAgeSeconds_)));
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
    const uint64_t staleSeconds = static_cast<uint64_t>(settings_.ReindexIntervalHours()) * 3600ULL;
    service_->RequestIndexing(force, CurrentScanOptions(), staleSeconds,
                              /*monitorAfter=*/!elevated_);
}

void MainWindow::RebuildIndex() {
    if (elevated_) {
        SendSignalToHelper(SIGUSR1);  // §9.3: reindex-now request; the helper owns the index
    } else {
        StartIndexing(/*force=*/true);
    }
}

void MainWindow::SetIndexBusy(bool busy) {
    indexBusy_ = busy;
    // §19: search is disabled while (re)indexing, and until an index exists
    // at all (a cancelled first scan leaves none).
    SetSearchUiEnabled(!busy && hasIndex_);
    if (busy) {
        statusBar()->showMessage(tr("Indexing…"));
    } else {
        statusBar()->showMessage(tr("Ready."));
        searchBox_->setFocus();  // §19: auto-focus after indexing
    }
}

void MainWindow::OnIndexStatus(const IndexerStatus& status) {
    if (status.state == IndexerState::Scanning) {
        statusBar()->showMessage(
            tr("Indexing… %1 files")
                .arg(QString::fromStdString(FormatFileCount(status.filesIndexed))));
    }
}

void MainWindow::OnIndexChanged(const IndexChange& change) {
    lastEntryCount_ = change.entryCount;
    lastBuildAgeSeconds_ = change.indexAgeSeconds;
    hasIndex_ = true;
    if (!indexBusy_) {
        SetSearchUiEnabled(true);  // e.g. the first index arrives from the elevated helper
    }
    UpdateIdleStatus();
    RefreshVisibleResults();
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
    ApplySettings(result);
}

void MainWindow::ApplySettings(const SettingsDialogResult& result) {
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

    // §19: old vs new roots -> incremental add/remove, or a full rebuild
    // when both changed; the index is saved and monitoring restarted on the
    // new roots, all on IndexService's worker (docs/adr/0014).
    const uint64_t staleSeconds = static_cast<uint64_t>(settings_.ReindexIntervalHours()) * 3600ULL;
    service_->RequestSettingsChange(oldRoots, CurrentScanOptions(), staleSeconds,
                                    /*monitorAfter=*/true);
}

void MainWindow::ElevateForFullAccess() {
    if (elevated_ || helperProcess_ != nullptr) {
        return;  // elevated or elevating already: one helper at a time (§9.2)
    }

    // Reacts to QProcess's signals instead of waitForStarted(), so the UI
    // keeps running while pkexec starts (docs/adr/0014).
    helperProcess_ = new QProcess(this);
    connect(helperProcess_, &QProcess::started, this, &MainWindow::OnHelperStarted);
    connect(helperProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || elevated_) {
            return;
        }
        statusBar()->showMessage(
            tr("Could not start indexed-helper (declined, or pkexec/indexed-helper not found)."),
            8000);
        helperProcess_->deleteLater();
        helperProcess_ = nullptr;
    });
    connect(helperProcess_, &QProcess::finished, this, &MainWindow::OnHelperFinished);
    helperProcess_->start(helperCommand_.first(), helperCommand_.mid(1));
}

void MainWindow::OnHelperFinished() {
    // pkexec starts (QProcess::started) before its password prompt, so a
    // declined prompt, or a helper that exits for any reason, ends up here:
    // go back to indexing locally rather than waiting on a helper that is gone.
    if (indexWatcher_ != nullptr) {
        indexWatcher_->deleteLater();
        indexWatcher_ = nullptr;
    }
    if (helperWatcher_ != nullptr) {
        helperWatcher_->deleteLater();
        helperWatcher_ = nullptr;
    }
    helperProcess_->deleteLater();
    helperProcess_ = nullptr;
    const bool wasElevated = elevated_;
    elevated_ = false;
    elevateAction_->setEnabled(true);
    elevateAction_->setText(tr("Elevate for Full-System Access…"));
    if (wasElevated) {
        statusBar()->showMessage(tr("Full-system access ended; indexing locally again."), 8000);
        // A Settings change or rebuild signalled to it may never have been
        // applied (pkexec starts before its password prompt), so rebuild
        // rather than reload a possibly outdated index.
        StartIndexing(/*force=*/helperMayHaveMissedRequest_);
    }
    helperMayHaveMissedRequest_ = false;
}

void MainWindow::OnHelperStarted() {
    elevated_ = true;
    elevateAction_->setEnabled(false);
    elevateAction_->setText(tr("Elevated (full-system access active)"));
    statusBar()->showMessage(tr("Elevated: full-system indexing and monitoring active."), 5000);

    // The helper indexes and monitors from here on.
    service_->RequestStopLocalIndexing();

    // The helper replaces idxFilePath_ by rename on every save, which a bare
    // QFileSystemWatcher stops tracking after the first time; IndexFileWatcher
    // re-arms itself, including when the file doesn't exist yet. The reload
    // itself runs on IndexService's worker. indexed.status is new -- created
    // only by the helper -- so its containing directory is watched too, to
    // catch its first appearance (indexed-plan.md §9.3).
    indexWatcher_ = new IndexFileWatcher(QString::fromStdString(idxFilePath_), this);
    connect(indexWatcher_, &IndexFileWatcher::Changed, this,
            [this]() { service_->RequestReload(); });
    helperWatcher_ = new QFileSystemWatcher(this);
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
    if (signal == SIGHUP || signal == SIGUSR1) {
        helperMayHaveMissedRequest_ = true;
    }
    if (helperProcess_ != nullptr && helperProcess_->state() == QProcess::Running) {
        kill(static_cast<pid_t>(helperProcess_->processId()), signal);
    }
}

void MainWindow::OnHelperFileChanged(const QString& path) {
    if (path == QString::fromStdString(statusFilePath_)) {
        UpdateStatusFromHelperFile();
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
            if (!MountEnumerator::WaitForChange(fd, /*timeoutMs=*/200) || hotplugStop_.load()) {
                continue;
            }
            std::vector<MountInfo> current = mountEnumerator_.Enumerate();
            for (const MountInfo& mount : current) {
                const bool existed = std::any_of(
                    previous.begin(), previous.end(),
                    [&](const MountInfo& p) { return p.mountPoint == mount.mountPoint; });
                if (!existed) {
                    const QString msg =
                        tr("Filesystem %1 mounted -- add it in Settings to index it.")
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
