#pragma once

#include <QDialog>
#include <QString>

#include <string>
#include <vector>

class QListWidget;
class QCheckBox;
class QLineEdit;
class QComboBox;

namespace indexed {

// Values SettingsDialog is pre-populated with (indexed-plan.md §19: "Settings
// dialog"). Passed in by dependency injection -- rather than the dialog
// constructing a Settings object itself -- so it's unit-testable with
// hand-built fixture data, no real config file involved.
struct SettingsDialogInitialState {
    std::vector<std::string> selectedRoots;
    std::vector<std::string> excludedPaths;
    int reindexIntervalHours = 48;  // 0 = manual only
};

// Values read back out after exec() returns QDialog::Accepted.
struct SettingsDialogResult {
    std::vector<std::string> selectedRoots;
    std::vector<std::string> excludedPaths;
    int reindexIntervalHours = 48;
};

// Diff between the roots the dialog was opened with and the roots the user
// left it with. Pure data -- no Indexer/Settings object involved -- so the
// eventual caller (MainWindow) can decide incremental IndexPaths/RemovePaths
// vs. a full rebuild (indexed-plan.md §19: "On OK, diff old vs new roots").
struct RootsDiff {
    std::vector<std::string> added;    // in newRoots, not in oldRoots
    std::vector<std::string> removed;  // in oldRoots, not in newRoots

    // True when both added and removed are non-empty -- caller's cue that an
    // incremental update isn't enough and a full rebuild is warranted.
    bool bothChanged() const;
};

RootsDiff DiffRoots(const std::vector<std::string>& oldRoots,
                    const std::vector<std::string>& newRoots);

// "indexed — Settings" (indexed-plan.md §19): editing counterpart to
// FirstRunDialog. Paths-to-index list + Add Location…/Remove, the same
// Automatic Reindex group (manual-only checkbox + interval spinbox +
// Hours/Days combo), Excluded folders list + Add…/Remove, OK/Cancel.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(SettingsDialogInitialState initial, QWidget* parent = nullptr);

    // Valid after exec() returns QDialog::Accepted.
    SettingsDialogResult Result() const;

    // Reads the current widget state into result_ before delegating to
    // QDialog::accept(). Overriding the (virtual) base slot -- rather than
    // adding a separate OnAccept slot -- is the standard Qt pattern: the OK
    // button's QDialogButtonBox::accepted signal is wired straight to
    // &QDialog::accept, and virtual dispatch lands here.
    void accept() override;

protected:
    // Picks a folder for either "Add" button. Defaults to
    // QFileDialog::getExistingDirectory; overridden by tests to return a
    // canned path without popping a real OS dialog.
    virtual QString PickFolder();

private slots:
    void OnAddPath();
    void OnRemovePath();
    void OnAddExcluded();
    void OnRemoveExcluded();
    void OnManualOnlyToggled(bool checked);

private:
    void BuildUi();

    SettingsDialogInitialState initial_;
    SettingsDialogResult result_;

    QListWidget* pathList_ = nullptr;
    QListWidget* excludedList_ = nullptr;
    QCheckBox* manualOnlyCheck_ = nullptr;
    QLineEdit* intervalEdit_ = nullptr;
    QComboBox* unitCombo_ = nullptr;
};

}  // namespace indexed
