#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>

#include "ui/SettingsDialog.h"

namespace {

// Test-only subclass overriding the injectable folder picker so "Add" button
// clicks don't pop a real OS QFileDialog (which can't run headlessly).
class TestableSettingsDialog : public indexed::SettingsDialog {
public:
    explicit TestableSettingsDialog(indexed::SettingsDialogInitialState initial,
                                    QWidget* parent = nullptr)
        : indexed::SettingsDialog(std::move(initial), parent) {}

    QString nextPickedFolder;

protected:
    QString PickFolder() override { return nextPickedFolder; }
};

}  // namespace

class TestSettingsDialog : public QObject {
    Q_OBJECT

private slots:
    void prepopulatesPathsAndExclusions();
    void prepopulatesManualOnly();
    void prepopulatesDaysConversion();
    void prepopulatesDefaultFortyEightHoursAsTwoDays();
    void removeButtonRemovesSelectedRowFromItsOwnList();
    void addButtonAppendsToRightListViaPickFolder();
    void addButtonIgnoresEmptyPick();
    void resultReflectsEditsAfterAccept();
    void diffRootsNoChanges();
    void diffRootsPureAdditions();
    void diffRootsPureRemovals();
    void diffRootsBothChanged();
};

void TestSettingsDialog::prepopulatesPathsAndExclusions() {
    indexed::SettingsDialogInitialState initial;
    initial.selectedRoots = {"/home", "/data"};
    initial.excludedPaths = {"/proc", "/tmp"};
    initial.reindexIntervalHours = 5;  // not a multiple of 24 -> stays in Hours

    TestableSettingsDialog dlg(initial);

    auto* pathList = dlg.findChild<QListWidget*>("pathList");
    QVERIFY(pathList != nullptr);
    QCOMPARE(pathList->count(), 2);
    QCOMPARE(pathList->item(0)->text(), QString("/home"));
    QCOMPARE(pathList->item(1)->text(), QString("/data"));

    auto* excludedList = dlg.findChild<QListWidget*>("excludedList");
    QVERIFY(excludedList != nullptr);
    QCOMPARE(excludedList->count(), 2);
    QCOMPARE(excludedList->item(0)->text(), QString("/proc"));
    QCOMPARE(excludedList->item(1)->text(), QString("/tmp"));

    auto* manualOnly = dlg.findChild<QCheckBox*>("manualOnlyCheck");
    QVERIFY(manualOnly != nullptr);
    QVERIFY(!manualOnly->isChecked());

    auto* intervalEdit = dlg.findChild<QLineEdit*>("intervalEdit");
    QVERIFY(intervalEdit != nullptr);
    QCOMPARE(intervalEdit->text(), QString("5"));

    auto* unitCombo = dlg.findChild<QComboBox*>("unitCombo");
    QVERIFY(unitCombo != nullptr);
    QCOMPARE(unitCombo->currentIndex(), 0);  // Hours
}

void TestSettingsDialog::prepopulatesManualOnly() {
    indexed::SettingsDialogInitialState initial;
    initial.reindexIntervalHours = 0;  // manual only

    TestableSettingsDialog dlg(initial);

    auto* manualOnly = dlg.findChild<QCheckBox*>("manualOnlyCheck");
    QVERIFY(manualOnly->isChecked());

    auto* intervalEdit = dlg.findChild<QLineEdit*>("intervalEdit");
    QVERIFY(!intervalEdit->isEnabled());

    auto* unitCombo = dlg.findChild<QComboBox*>("unitCombo");
    QVERIFY(!unitCombo->isEnabled());
}

void TestSettingsDialog::prepopulatesDaysConversion() {
    indexed::SettingsDialogInitialState initial;
    initial.reindexIntervalHours = 72;  // 3 days, evenly divisible

    TestableSettingsDialog dlg(initial);

    auto* intervalEdit = dlg.findChild<QLineEdit*>("intervalEdit");
    QCOMPARE(intervalEdit->text(), QString("3"));

    auto* unitCombo = dlg.findChild<QComboBox*>("unitCombo");
    QCOMPARE(unitCombo->currentIndex(), 1);  // Days
}

void TestSettingsDialog::prepopulatesDefaultFortyEightHoursAsTwoDays() {
    // 48 hours is an exact multiple of 24 (>= 24), so it displays as "2 Days"
    // rather than "48 Hours" -- matches winindex's SettingsDialog::OnInit
    // conversion rule exactly (any interval%24==0 && interval>=24 collapses
    // to the Days unit).
    indexed::SettingsDialogInitialState initial;
    initial.reindexIntervalHours = 48;

    TestableSettingsDialog dlg(initial);

    auto* intervalEdit = dlg.findChild<QLineEdit*>("intervalEdit");
    QCOMPARE(intervalEdit->text(), QString("2"));

    auto* unitCombo = dlg.findChild<QComboBox*>("unitCombo");
    QCOMPARE(unitCombo->currentIndex(), 1);  // Days
}

void TestSettingsDialog::removeButtonRemovesSelectedRowFromItsOwnList() {
    indexed::SettingsDialogInitialState initial;
    initial.selectedRoots = {"/home", "/data"};
    initial.excludedPaths = {"/proc", "/tmp"};

    TestableSettingsDialog dlg(initial);
    auto* pathList = dlg.findChild<QListWidget*>("pathList");
    auto* excludedList = dlg.findChild<QListWidget*>("excludedList");
    auto* removePathButton = dlg.findChild<QPushButton*>("removePathButton");
    auto* removeExcludedButton = dlg.findChild<QPushButton*>("removeExcludedButton");
    QVERIFY(removePathButton != nullptr);
    QVERIFY(removeExcludedButton != nullptr);

    pathList->setCurrentRow(0);
    QTest::mouseClick(removePathButton, Qt::LeftButton);

    QCOMPARE(pathList->count(), 1);
    QCOMPARE(pathList->item(0)->text(), QString("/data"));
    // Removing from the path list must not touch the excluded list.
    QCOMPARE(excludedList->count(), 2);

    excludedList->setCurrentRow(1);
    QTest::mouseClick(removeExcludedButton, Qt::LeftButton);

    QCOMPARE(excludedList->count(), 1);
    QCOMPARE(excludedList->item(0)->text(), QString("/proc"));
    // Unrelated to the path-list removal above.
    QCOMPARE(pathList->count(), 1);
}

void TestSettingsDialog::addButtonAppendsToRightListViaPickFolder() {
    indexed::SettingsDialogInitialState initial;
    initial.selectedRoots = {"/home"};
    initial.excludedPaths = {"/proc"};

    TestableSettingsDialog dlg(initial);
    auto* pathList = dlg.findChild<QListWidget*>("pathList");
    auto* excludedList = dlg.findChild<QListWidget*>("excludedList");
    auto* addPathButton = dlg.findChild<QPushButton*>("addPathButton");
    auto* addExcludedButton = dlg.findChild<QPushButton*>("addExcludedButton");
    QVERIFY(addPathButton != nullptr);
    QVERIFY(addExcludedButton != nullptr);

    dlg.nextPickedFolder = "/new/path";
    QTest::mouseClick(addPathButton, Qt::LeftButton);

    QCOMPARE(pathList->count(), 2);
    QCOMPARE(pathList->item(1)->text(), QString("/new/path"));
    QCOMPARE(excludedList->count(), 1);  // untouched

    // cppcheck-suppress redundantAssignment  // read via PickFolder() override through Qt dispatch
    // cppcheck-suppress unreadVariable
    dlg.nextPickedFolder = "/new/excluded";
    QTest::mouseClick(addExcludedButton, Qt::LeftButton);

    QCOMPARE(excludedList->count(), 2);
    QCOMPARE(excludedList->item(1)->text(), QString("/new/excluded"));
    QCOMPARE(pathList->count(), 2);  // untouched
}

void TestSettingsDialog::addButtonIgnoresEmptyPick() {
    indexed::SettingsDialogInitialState initial;
    initial.selectedRoots = {"/home"};

    TestableSettingsDialog dlg(initial);
    auto* pathList = dlg.findChild<QListWidget*>("pathList");
    auto* addPathButton = dlg.findChild<QPushButton*>("addPathButton");

    // cppcheck-suppress unreadVariable  // read via PickFolder() override through Qt dispatch
    dlg.nextPickedFolder = QString();  // user hit Cancel in the folder picker
    QTest::mouseClick(addPathButton, Qt::LeftButton);

    QCOMPARE(pathList->count(), 1);
}

void TestSettingsDialog::resultReflectsEditsAfterAccept() {
    indexed::SettingsDialogInitialState initial;
    initial.selectedRoots = {"/home", "/data"};
    initial.excludedPaths = {"/proc"};
    initial.reindexIntervalHours = 48;

    TestableSettingsDialog dlg(initial);

    auto* pathList = dlg.findChild<QListWidget*>("pathList");
    auto* removePathButton = dlg.findChild<QPushButton*>("removePathButton");
    pathList->setCurrentRow(0);
    QTest::mouseClick(removePathButton, Qt::LeftButton);  // removes "/home"

    dlg.nextPickedFolder = "/excl/new";
    auto* addExcludedButton = dlg.findChild<QPushButton*>("addExcludedButton");
    QTest::mouseClick(addExcludedButton, Qt::LeftButton);

    auto* manualOnly = dlg.findChild<QCheckBox*>("manualOnlyCheck");
    manualOnly->setChecked(true);

    dlg.accept();

    QCOMPARE(dlg.result(), static_cast<int>(QDialog::Accepted));
    indexed::SettingsDialogResult result = dlg.Result();
    QCOMPARE(result.selectedRoots, (std::vector<std::string>{"/data"}));
    QCOMPARE(result.excludedPaths, (std::vector<std::string>{"/proc", "/excl/new"}));
    QCOMPARE(result.reindexIntervalHours, 0);
}

void TestSettingsDialog::diffRootsNoChanges() {
    std::vector<std::string> oldRoots{"/home", "/data"};
    std::vector<std::string> newRoots{"/home", "/data"};

    indexed::RootsDiff diff = indexed::DiffRoots(oldRoots, newRoots);

    QVERIFY(diff.added.empty());
    QVERIFY(diff.removed.empty());
    QVERIFY(!diff.bothChanged());
}

void TestSettingsDialog::diffRootsPureAdditions() {
    std::vector<std::string> oldRoots{"/home"};
    std::vector<std::string> newRoots{"/home", "/data"};

    indexed::RootsDiff diff = indexed::DiffRoots(oldRoots, newRoots);

    QCOMPARE(diff.added, (std::vector<std::string>{"/data"}));
    QVERIFY(diff.removed.empty());
    QVERIFY(!diff.bothChanged());
}

void TestSettingsDialog::diffRootsPureRemovals() {
    std::vector<std::string> oldRoots{"/home", "/data"};
    std::vector<std::string> newRoots{"/home"};

    indexed::RootsDiff diff = indexed::DiffRoots(oldRoots, newRoots);

    QVERIFY(diff.added.empty());
    QCOMPARE(diff.removed, (std::vector<std::string>{"/data"}));
    QVERIFY(!diff.bothChanged());
}

void TestSettingsDialog::diffRootsBothChanged() {
    std::vector<std::string> oldRoots{"/home", "/data"};
    std::vector<std::string> newRoots{"/home", "/media"};

    indexed::RootsDiff diff = indexed::DiffRoots(oldRoots, newRoots);

    QCOMPARE(diff.added, (std::vector<std::string>{"/media"}));
    QCOMPARE(diff.removed, (std::vector<std::string>{"/data"}));
    QVERIFY(diff.bothChanged());
}

QTEST_MAIN(TestSettingsDialog)
#include "test_SettingsDialog.moc"
