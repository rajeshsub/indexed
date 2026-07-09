#pragma once

#include <QAction>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QMainWindow>
#include <QProcess>

#include "indexer/Indexer.h"
#include "platform/MountEnumerator.h"
#include "search/ISearchEngine.h"
#include "settings/Settings.h"
#include "storage/IndexStore.h"
#include "ui/ResultModel.h"
#include "ui/ResultView.h"
#include "ui/SearchCoordinator.h"
#include "ui/SearchLineEdit.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace indexed {

// The app shell (indexed-plan.md §19): search box over a virtual results
// list with a status bar and Search/Index/Help menus. Pure chrome + wiring
// -- every piece with real logic (ResultModel, ResultView, SearchLineEdit,
// SearchCoordinator, StatusText, the dialogs) lives in its own tested class;
// this file is verified manually via the running app, per the M4 test-tier
// decision in indexed-plan.md §15.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // All collaborators are owned by main.cpp and outlive the window.
    // idxFilePath/logPath come from PathUtils::ResolveDataDirs.
    MainWindow(Settings& settings, IndexStore& store, ISearchEngine& engine,
               IFileSystemScanner& scanner, std::string idxFilePath, std::string logPath,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    // Kicks off StartIndexing on a background thread (load-if-fresh unless
    // force). Safe to call again after completion (Rebuild Index Now).
    // Starts the GUI's own unprivileged InotifyWatcher-based monitoring on
    // completion, unless an elevated helper is running (§7.2/§9).
    void StartIndexing(bool force);

private:
    void BuildMenus();
    void WireSearch();
    void WireResultActions();
    void OpenPath(const QString& path);
    void RevealPath(const QString& path);
    void CutToClipboard(const QStringList& paths);
    void TrashPaths(const QStringList& paths);
    void ShowSettingsDialog();
    void ShowAbout();
    void UpdateIdleStatus();
    void UpdateSearchOptionsLabel();
    void SetSearchUiEnabled(bool enabled);
    ScanOptions CurrentScanOptions() const;
    void JoinIndexThread();

    // Unprivileged live monitoring (default, no polkit prompt): InotifyWatcher
    // per selected root, applied into store_ by the GUI's own Indexer.
    void StartLocalMonitoring();
    void StopLocalMonitoring();

    // "Elevate for full-system access" (Index menu): launches
    // `pkexec indexed-helper` (indexed-plan.md §9.2). Once elevated, the GUI
    // stops scanning/monitoring itself and instead watches the helper's
    // indexed.idx/indexed.status files for updates, per §7.7 -- Settings
    // changes and Rebuild Index Now become SIGHUP/SIGUSR1 to the helper
    // instead of local Indexer calls (§9.3).
    void ElevateForFullAccess();
    void SendSignalToHelper(int signal);
    void OnHelperFileChanged(const QString& path);
    void ReloadIndexFromDisk();
    void UpdateStatusFromHelperFile();

    // Hotplug (§7.6): polls MountEnumerator::WaitForChange on a background
    // thread; on change, diffs the enumerated mount set and posts a
    // transient status-bar notice for newly mounted/unmounted filesystems.
    void StartHotplugWatcher();
    void StopHotplugWatcher();

    Settings& settings_;
    IndexStore& store_;
    IFileSystemScanner& scanner_;
    std::string idxFilePath_;
    std::string logPath_;
    std::string statusFilePath_;

    std::unique_ptr<Indexer> indexer_;
    std::thread indexThread_;

    std::atomic<bool> localMonitorStop_{false};
    std::thread localMonitorThread_;

    QProcess* helperProcess_ = nullptr;
    QFileSystemWatcher* helperWatcher_ = nullptr;
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
