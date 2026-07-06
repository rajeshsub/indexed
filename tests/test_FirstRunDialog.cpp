#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSpinBox>
#include <QTest>

#include "ui/FirstRunDialog.h"

using indexed::FirstRunDialog;
using indexed::FirstRunResult;
using indexed::MountInfo;

namespace {

std::vector<MountInfo> MakeFixtureMounts() {
    std::vector<MountInfo> mounts;

    MountInfo root;
    root.mountPoint = "/";
    root.device = "/dev/sda1";
    root.fsType = "ext4";
    mounts.push_back(root);

    MountInfo home;
    home.mountPoint = "/home";
    home.device = "/dev/sda2";
    home.fsType = "ext4";
    mounts.push_back(home);

    MountInfo usb;
    usb.mountPoint = "/media/usb";
    usb.device = "/dev/sdb1";
    usb.fsType = "vfat";
    usb.removable = true;
    mounts.push_back(usb);

    return mounts;
}

Qt::CheckState CheckStateFor(QListWidget* list, const QString& mountPoint) {
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem* item = list->item(i);
        if (item->data(Qt::UserRole).toString() == mountPoint) {
            return item->checkState();
        }
    }
    return Qt::Unchecked;
}

}  // namespace

// Test subclass overriding the virtual folder picker so "Add..." can be
// exercised headlessly without popping a native QFileDialog.
class TestableFirstRunDialog : public FirstRunDialog {
public:
    using FirstRunDialog::FirstRunDialog;

    QString canned;

protected:
    QString PickFolder() override { return canned; }
};

class TestFirstRunDialog : public QObject {
    Q_OBJECT

private slots:
    void MountListPopulatedAndPreselected();
    void DefaultIntervalIs48HoursManualUnchecked();
    void ManualOnlyProducesZeroHours();
    void SpinnerDaysConvertToHours();
    void ExcludedListSeededFromDefaults();
    void RemoveButtonRemovesSelectedRow();
    void AddButtonAppendsFolder();
    void ResultReflectsAllFieldsOnAccept();
};

void TestFirstRunDialog::MountListPopulatedAndPreselected() {
    FirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", {});

    auto* mountList = dialog.findChild<QListWidget*>("mountList");
    QVERIFY(mountList != nullptr);
    QCOMPARE(mountList->count(), 3);

    QCOMPARE(CheckStateFor(mountList, "/"), Qt::Checked);
    QCOMPARE(CheckStateFor(mountList, "/home"), Qt::Checked);
    QCOMPARE(CheckStateFor(mountList, "/media/usb"), Qt::Unchecked);
}

void TestFirstRunDialog::DefaultIntervalIs48HoursManualUnchecked() {
    FirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", {});

    auto* manualOnly = dialog.findChild<QCheckBox*>("manualOnlyCheck");
    auto* spin = dialog.findChild<QSpinBox*>("intervalSpin");
    auto* combo = dialog.findChild<QComboBox*>("intervalUnitCombo");
    QVERIFY(manualOnly != nullptr);
    QVERIFY(spin != nullptr);
    QVERIFY(combo != nullptr);

    QVERIFY(!manualOnly->isChecked());
    QCOMPARE(spin->value(), 48);
    QCOMPARE(combo->currentText(), QString("Hours"));

    QCOMPARE(dialog.Result().reindexIntervalHours, 48);
}

void TestFirstRunDialog::ManualOnlyProducesZeroHours() {
    FirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", {});

    auto* manualOnly = dialog.findChild<QCheckBox*>("manualOnlyCheck");
    QVERIFY(manualOnly != nullptr);
    manualOnly->setChecked(true);

    QCOMPARE(dialog.Result().reindexIntervalHours, 0);
}

void TestFirstRunDialog::SpinnerDaysConvertToHours() {
    FirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", {});

    auto* spin = dialog.findChild<QSpinBox*>("intervalSpin");
    auto* combo = dialog.findChild<QComboBox*>("intervalUnitCombo");
    QVERIFY(spin != nullptr);
    QVERIFY(combo != nullptr);

    spin->setValue(2);
    combo->setCurrentIndex(combo->findText("Days"));

    QCOMPARE(dialog.Result().reindexIntervalHours, 48);
}

void TestFirstRunDialog::ExcludedListSeededFromDefaults() {
    std::vector<std::string> defaults = {"/home/alice/.cache", "/proc"};
    FirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", defaults);

    auto* excludedList = dialog.findChild<QListWidget*>("excludedList");
    QVERIFY(excludedList != nullptr);
    QCOMPARE(excludedList->count(), 2);
    QCOMPARE(excludedList->item(0)->text(), QString("/home/alice/.cache"));
    QCOMPARE(excludedList->item(1)->text(), QString("/proc"));

    FirstRunResult result = dialog.Result();
    QCOMPARE(result.excludedPaths.size(), static_cast<size_t>(2));
    QCOMPARE(QString::fromStdString(result.excludedPaths[0]), QString("/home/alice/.cache"));
    QCOMPARE(QString::fromStdString(result.excludedPaths[1]), QString("/proc"));
}

void TestFirstRunDialog::RemoveButtonRemovesSelectedRow() {
    std::vector<std::string> defaults = {"/home/alice/.cache", "/proc"};
    FirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", defaults);

    auto* excludedList = dialog.findChild<QListWidget*>("excludedList");
    auto* removeButton = dialog.findChild<QPushButton*>("removeExcludedButton");
    QVERIFY(excludedList != nullptr);
    QVERIFY(removeButton != nullptr);

    excludedList->setCurrentRow(0);
    QTest::mouseClick(removeButton, Qt::LeftButton);

    QCOMPARE(excludedList->count(), 1);
    QCOMPARE(excludedList->item(0)->text(), QString("/proc"));
}

void TestFirstRunDialog::AddButtonAppendsFolder() {
    TestableFirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", {});
    dialog.canned = "/opt/newexclude";

    auto* excludedList = dialog.findChild<QListWidget*>("excludedList");
    auto* addButton = dialog.findChild<QPushButton*>("addExcludedButton");
    QVERIFY(excludedList != nullptr);
    QVERIFY(addButton != nullptr);

    QTest::mouseClick(addButton, Qt::LeftButton);

    QCOMPARE(excludedList->count(), 1);
    QCOMPARE(excludedList->item(0)->text(), QString("/opt/newexclude"));
}

void TestFirstRunDialog::ResultReflectsAllFieldsOnAccept() {
    FirstRunDialog dialog(MakeFixtureMounts(), "/home/alice", {"/proc"});

    auto* mountList = dialog.findChild<QListWidget*>("mountList");
    QVERIFY(mountList != nullptr);
    for (int i = 0; i < mountList->count(); ++i) {
        QListWidgetItem* item = mountList->item(i);
        if (item->data(Qt::UserRole).toString() == "/media/usb") {
            item->setCheckState(Qt::Checked);
        }
    }

    dialog.accept();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));

    FirstRunResult result = dialog.Result();
    QCOMPARE(result.selectedRoots.size(), static_cast<size_t>(3));
    QCOMPARE(result.reindexIntervalHours, 48);
    QCOMPARE(result.excludedPaths.size(), static_cast<size_t>(1));
    QCOMPARE(QString::fromStdString(result.excludedPaths[0]), QString("/proc"));
}

QTEST_MAIN(TestFirstRunDialog)
#include "test_FirstRunDialog.moc"
