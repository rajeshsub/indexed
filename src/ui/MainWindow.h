#pragma once

#include <QAction>
#include <QMainWindow>

#include "indexer/Indexer.h"
#include "search/ISearchEngine.h"
#include "settings/Settings.h"
#include "storage/IndexStore.h"
#include "ui/ResultModel.h"
#include "ui/ResultView.h"
#include "ui/SearchCoordinator.h"
#include "ui/SearchLineEdit.h"
#include <memory>
#include <string>
#include <thread>

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
    void SetSearchUiEnabled(bool enabled);
    ScanOptions CurrentScanOptions() const;
    void JoinIndexThread();

    Settings& settings_;
    IndexStore& store_;
    IFileSystemScanner& scanner_;
    std::string idxFilePath_;
    std::string logPath_;

    std::unique_ptr<Indexer> indexer_;
    std::thread indexThread_;

    SearchLineEdit* searchBox_ = nullptr;
    ResultView* resultView_ = nullptr;
    ResultModel* resultModel_ = nullptr;
    SearchCoordinator* coordinator_ = nullptr;

    QAction* regexAction_ = nullptr;
    QAction* caseAction_ = nullptr;
    QAction* wholeWordAction_ = nullptr;
    QAction* matchPathAction_ = nullptr;
    QAction* diacriticsAction_ = nullptr;

    uint64_t lastBuildAgeSeconds_ = 0;
};

}  // namespace indexed
