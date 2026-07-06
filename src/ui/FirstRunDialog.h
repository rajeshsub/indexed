#pragma once

#include <QDialog>
#include <QString>

#include "core/platform/MountEnumerator.h"
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace indexed {

// Everything the user chose in FirstRunDialog (indexed-plan.md §19), shaped
// so the caller can drop it straight into a Settings instance via
// SetSelectedRoots/SetReindexIntervalHours/SetExcludedPaths.
struct FirstRunResult {
    std::vector<std::string> selectedRoots;
    int reindexIntervalHours = 48;  // 0 == "Manual only" checked
    std::vector<std::string> excludedPaths;
};

// "indexed — First Run Setup" (indexed-plan.md §19): multi-select mount
// list, Automatic Reindex group (Manual only / interval+unit), Excluded
// folders list with Add.../Remove.
//
// Takes mounts/homeDir/defaultExcludedPaths by dependency injection rather
// than calling MountEnumerator::Enumerate()/Settings::DefaultExcludedPaths()
// itself, so it's constructible and testable with fixture data and no real
// mounts/root privileges. The caller (MainWindow/main.cpp) gathers those and
// is responsible for materializing a Settings instance from Result()
// afterward.
class FirstRunDialog : public QDialog {
    Q_OBJECT

public:
    FirstRunDialog(std::vector<MountInfo> mounts, std::string homeDir,
                   std::vector<std::string> defaultExcludedPaths, QWidget* parent = nullptr);

    // Reads the dialog's current field state (selected mounts, computed
    // reindex-interval hours, current excluded-folders list). Meaningful
    // once exec()/accept() has returned QDialog::Accepted.
    FirstRunResult Result() const;

protected:
    // Folder picker backing the Excluded-folders "Add..." button. Defaults
    // to a real QFileDialog::getExistingDirectory (starting at homeDir);
    // overridden by tests to return a canned path instead of popping a
    // native dialog that can't run headlessly.
    virtual QString PickFolder();

private slots:
    void OnManualOnlyToggled(bool checked);
    void OnAddExcludedFolder();
    void OnRemoveExcludedFolder();

private:
    void BuildUi();
    void PopulateMountList();
    void PreselectMounts();

    std::vector<MountInfo> mounts_;
    std::string homeDir_;

    QListWidget* mountList_ = nullptr;
    QCheckBox* manualOnlyCheck_ = nullptr;
    QSpinBox* intervalSpin_ = nullptr;
    QComboBox* intervalUnitCombo_ = nullptr;
    QListWidget* excludedList_ = nullptr;
    QPushButton* addExcludedButton_ = nullptr;
    QPushButton* removeExcludedButton_ = nullptr;
};

}  // namespace indexed
