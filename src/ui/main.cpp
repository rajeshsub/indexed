#include <pwd.h>
#include <unistd.h>

#include <QApplication>
#include <QDialog>

#include "indexer/WalkScanner.h"
#include "platform/MountEnumerator.h"
#include "search/SearchEngine.h"
#include "settings/Logger.h"
#include "settings/PathUtils.h"
#include "settings/Settings.h"
#include "storage/IndexStore.h"
#include "ui/FirstRunDialog.h"
#include "ui/MainWindow.h"
#include <cstdlib>
#include <filesystem>

namespace {

std::string HomeDir() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return home;
    }
    const passwd* pw = getpwuid(getuid());
    return pw != nullptr ? pw->pw_dir : "/";
}

}  // namespace

int main(int argc, char* argv[]) {
    // Qt's GNOME platform theme plugin probes xdg-desktop-portal over D-Bus
    // for dark-mode/font settings during QApplication's constructor. On any
    // system where that portal is absent or misconfigured (common outside a
    // full GNOME session -- XFCE, minimal installs, sandboxes) it fails and
    // logs a "Could not activate remote peer" warning; the failure is
    // harmless and already handled gracefully (indexed just doesn't get
    // portal-sourced theming), so silence only this one log category rather
    // than leaving users to wonder if something broke. Additive: an
    // operator-supplied QT_LOGGING_RULES is preserved, not clobbered.
    const QByteArray existingLoggingRules = qgetenv("QT_LOGGING_RULES");
    qputenv("QT_LOGGING_RULES", existingLoggingRules.isEmpty()
                                    ? QByteArray("qt.qpa.theme.gnome.warning=false")
                                    : existingLoggingRules + ";qt.qpa.theme.gnome.warning=false");

    QApplication app(argc, argv);

    const indexed::DataDirs dirs = indexed::ResolveDataDirs();
    indexed::EnsureDirectory(std::filesystem::path(dirs.configPath).parent_path().string());
    indexed::EnsureDirectory(std::filesystem::path(dirs.indexPath).parent_path().string());
    indexed::EnsureDirectory(std::filesystem::path(dirs.logPath).parent_path().string());

    const std::string home = HomeDir();
    indexed::Settings settings(dirs.configPath, home);
    settings.Load();
    indexed::Logger logger(dirs.logPath);
    logger.Log("indexed starting");

    if (!settings.FirstRunComplete()) {
        indexed::MountEnumerator mounts;
        indexed::FirstRunDialog firstRun(mounts.Enumerate(), home,
                                         indexed::Settings::DefaultExcludedPaths(home));
        if (firstRun.exec() != QDialog::Accepted) {
            return 0;  // user declined setup; nothing to index yet
        }
        const indexed::FirstRunResult result = firstRun.Result();
        settings.SetSelectedRoots(result.selectedRoots);
        settings.SetExcludedPaths(result.excludedPaths);
        settings.SetReindexIntervalHours(result.reindexIntervalHours);
        settings.SetFirstRunComplete(true);
        settings.Save();
        logger.Log("first-run setup complete");
    }

    indexed::IndexStore store;
    indexed::SearchEngine engine;
    indexed::WalkScanner scanner;

    indexed::MainWindow window(settings, store, engine, scanner, dirs.indexPath, dirs.logPath);
    window.show();
    window.StartIndexing(/*force=*/false);

    return app.exec();
}
