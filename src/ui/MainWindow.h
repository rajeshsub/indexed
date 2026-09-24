#pragma once

#include <QAction>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QMainWindow>
#include <QProcess>
#include <QTimer>

#include "indexer/IndexService.h"
#include "indexer/Indexer.h"
#include "platform/MountEnumerator.h"
#include "search/ISearchEngine.h"
#include "settings/Settings.h"
#include "storage/IndexStore.h"
#include "ui/IndexFileWatcher.h"
#include "ui/ResultModel.h"
#include "ui/ResultView.h"
#include "ui/SearchCoordinator.h"
#include "ui/SearchLineEdit.h"
#include "ui/SettingsDialog.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace indexed {

// The app shell (indexed-plan.md §19): search box over a virtual results
// list with a status bar and Search/Index/Help menus. Pure chrome + wiring
// -- every piece with real logic (ResultModel, ResultView, SearchLineEdit,
// SearchCoordinator, IndexService, StatusText, the dialogs) lives in its own
// tested class. Index work, thread joins and file-manager D-Bus calls never
// run on the UI thread: index work goes through IndexService, confirmation
// and failure dialogs are non-modal, and tests/test_MainWindow.cpp fails if
// any user action pauses the event loop for over 100 ms (docs/adr/0014).
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // All collaborators are owned by main.cpp and outlive the window.
    // idxFilePath/logPath come from PathUtils::ResolveDataDirs.
    // monitorFactory builds the GUI's own (unprivileged) live monitors;
    // fileOperationRunner performs Move to Trash / Delete; helperCommand is
    // the program + arguments that start the elevated helper. The last three
    // are injectable so tests can drive the real window without inotify, the
    // user's Trash, or pkexec.
    MainWindow(Settings& settings, IndexStore& store, ISearchEngine& engine,
               IFileSystemScanner& scanner, ChangeMonitorFactory monitorFactory,
               FileOperationRunner fileOperationRunner, QStringList helperCommand,
               std::string idxFilePath, std::string logPath, QWidget* parent = nullptr);

    // Default FileOperationRunner: QFile::moveToTrash / std::filesystem::remove.
    static std::optional<std::string> RunFileOperation(const FileOperation& operation);
    ~MainWindow() override;

    // Asks IndexService to index (load-if-fresh unless force) and then run
    // the GUI's own live monitoring, unless an elevated helper is running
    // (§7.2/§9). Returns immediately; a rebuild already running is cancelled
    // and restarted (docs/adr/0014).
    void StartIndexing(bool force);

private:
    void BuildMenus();
    void WireSearch();
    void WireResultActions();
    void OpenPath(const QString& path);
    void OnOpenPathChecked(const QString& path, bool exists);
    // Rebuild Index Now: local rebuild, or SIGUSR1 to the helper when elevated.
    void RebuildIndex();
    void RevealPath(const QString& path);
    void TrashPaths(const QStringList& paths);
    // Marks the rows pending and hands the operations to IndexService's
    // file-operations thread; OnFileOperationsFinished reports the outcome.
    void RunFileOperations(FileOperation::Type type, const QStringList& paths, const QString& verb);
    void OnFileOperationsFinished(const FileOperationReport& report);
    // Shift+Delete: irreversible unlink, gated behind a confirm dialog.
    void DeletePermanently(const QStringList& paths);
    // Shows a warning listing paths a trash/delete could not act on; no-op
    // when the list is empty.
    void ReportDeletionFailures(const QString& verb, const QStringList& failed);
    // Re-runs the on-screen query so index mutations (live monitoring, a
    // delete) show without the user retyping. Debounced via liveRefreshTimer_.
    void RefreshVisibleResults();
    void ShowSettingsDialog();

public:
    // Applies an accepted Settings dialog result: saves the settings and
    // updates the index for the root changes. Public so tests can drive it
    // without a modal dialog.
    void ApplySettings(const SettingsDialogResult& result);

private:
    void ShowAbout();
    void UpdateIdleStatus();
    void UpdateSearchOptionsLabel();
    void SetSearchUiEnabled(bool enabled);
    ScanOptions CurrentScanOptions() const;
    // IndexService callbacks, run on the UI thread.
    void SetIndexBusy(bool busy);
    void OnIndexStatus(const IndexerStatus& status);
    void OnIndexChanged(const IndexChange& change);

    // "Elevate for full-system access" (Index menu): launches
    // `pkexec indexed-helper` (indexed-plan.md §9.2). Once elevated, the GUI
    // stops scanning/monitoring itself and instead watches the helper's
    // indexed.idx/indexed.status files for updates, per §7.7 -- Settings
    // changes and Rebuild Index Now become SIGHUP/SIGUSR1 to the helper
    // instead of local Indexer calls (§9.3).
    void ElevateForFullAccess();
    void OnHelperStarted();
    void OnHelperFinished();
    void SendSignalToHelper(int signal);
    void OnHelperFileChanged(const QString& path);
    void UpdateStatusFromHelperFile();

    // Hotplug (§7.6): polls MountEnumerator::WaitForChange on a background
    // thread; on change, diffs the enumerated mount set and posts a
    // transient status-bar notice for newly mounted/unmounted filesystems.
    void StartHotplugWatcher();
    void StopHotplugWatcher();

    Settings& settings_;
    IndexStore& store_;
    IFileSystemScanner& scanner_;
    ChangeMonitorFactory monitorFactory_;
    FileOperationRunner fileOperationRunner_;
    QStringList helperCommand_;
    std::string idxFilePath_;
    std::string logPath_;
    std::string statusFilePath_;

    std::unique_ptr<IndexService> service_;
    QTimer* liveRefreshTimer_ = nullptr;
    // Paths with a Trash/Delete in flight, shown greyed until it finishes;
    // one verb per requested batch, in request order.
    std::unordered_set<std::string> pendingPaths_;
    std::deque<QString> pendingOperationVerbs_;
    // Shared with detached Open existence checks: they post their result
    // only while `alive` (cleared, under the mutex, on destruction).
    struct Liveness {
        std::mutex mutex;
        bool alive = true;
    };
    std::shared_ptr<Liveness> liveness_ = std::make_shared<Liveness>();
    uint64_t lastEntryCount_ = 0;
    bool indexBusy_ = false;
    bool hasIndex_ = false;
    // Set when a Settings change or rebuild is signalled to the helper, which
    // may exit without applying it (see OnHelperFinished).
    bool helperMayHaveMissedRequest_ = false;
    // Set by RefreshVisibleResults so the next ResultsReady preserves the
    // user's selection/current row/focus across the model reset; cleared on
    // every ResultsReady so a subsequent user search resets normally.
    bool preserveSelectionOnNextResults_ = false;

    QProcess* helperProcess_ = nullptr;
    QFileSystemWatcher* helperWatcher_ = nullptr;
    IndexFileWatcher* indexWatcher_ = nullptr;
    bool elevated_ = false;

    MountEnumerator mountEnumerator_;
    std::atomic<bool> hotplugStop_{false};
    std::thread hotplugThread_;

    SearchLineEdit* searchBox_ = nullptr;
    ResultView* resultView_ = nullptr;
    ResultModel* resultModel_ = nullptr;
    SearchCoordinator* coordinator_ = nullptr;

    QAction* regexAction_ = nullptr;
    QAction* caseAction_ = nullptr;
    QAction* wholeWordAction_ = nullptr;
    QAction* matchPathAction_ = nullptr;
    QAction* diacriticsAction_ = nullptr;
    QAction* elevateAction_ = nullptr;

    // Permanent status-bar widgets (indexed-plan.md §19 follow-up): unlike
    // statusBar()->showMessage()'s transient message slot -- which search
    // results, indexing progress, and hotplug notices all share and
    // overwrite each other in -- these two are never hidden or replaced by a
    // transient message, so the index summary survives a search instead of
    // being clobbered by the result count. indexStatusLabel_ sits left of
    // searchOptionsLabel_ (added first: QStatusBar packs permanent widgets
    // left-to-right in addPermanentWidget() call order).
    QLabel* indexStatusLabel_ = nullptr;
    QLabel* searchOptionsLabel_ = nullptr;

    uint64_t lastBuildAgeSeconds_ = 0;
};

}  // namespace indexed
