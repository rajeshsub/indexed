#include "ui/FirstRunDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace indexed {

FirstRunDialog::FirstRunDialog(std::vector<MountInfo> mounts, std::string homeDir,
                               std::vector<std::string> defaultExcludedPaths, QWidget* parent)
    : QDialog(parent), mounts_(std::move(mounts)), homeDir_(std::move(homeDir)) {
    setWindowTitle(tr("indexed — First Run Setup"));

    BuildUi();
    PopulateMountList();
    PreselectMounts();

    for (const std::string& path : defaultExcludedPaths) {
        excludedList_->addItem(QString::fromStdString(path));
    }
}

void FirstRunDialog::BuildUi() {
    auto* mainLayout = new QVBoxLayout(this);

    // Mount list.
    mainLayout->addWidget(new QLabel(tr("Select the drives/mounts to index:")));
    mountList_ = new QListWidget(this);
    mountList_->setObjectName(QStringLiteral("mountList"));
    mainLayout->addWidget(mountList_);

    // Automatic Reindex group.
    auto* reindexGroup = new QGroupBox(tr("Automatic Reindex"), this);
    auto* reindexLayout = new QHBoxLayout(reindexGroup);

    manualOnlyCheck_ = new QCheckBox(tr("Manual only"), reindexGroup);
    manualOnlyCheck_->setObjectName(QStringLiteral("manualOnlyCheck"));
    reindexLayout->addWidget(manualOnlyCheck_);

    intervalSpin_ = new QSpinBox(reindexGroup);
    intervalSpin_->setObjectName(QStringLiteral("intervalSpin"));
    intervalSpin_->setRange(1, 9999);
    intervalSpin_->setValue(48);
    reindexLayout->addWidget(intervalSpin_);

    intervalUnitCombo_ = new QComboBox(reindexGroup);
    intervalUnitCombo_->setObjectName(QStringLiteral("intervalUnitCombo"));
    intervalUnitCombo_->addItem(tr("Hours"));
    intervalUnitCombo_->addItem(tr("Days"));
    reindexLayout->addWidget(intervalUnitCombo_);

    mainLayout->addWidget(reindexGroup);

    connect(manualOnlyCheck_, &QCheckBox::toggled, this, &FirstRunDialog::OnManualOnlyToggled);

    // Excluded folders group.
    auto* excludedGroup = new QGroupBox(tr("Excluded folders"), this);
    auto* excludedLayout = new QVBoxLayout(excludedGroup);

    excludedList_ = new QListWidget(excludedGroup);
    excludedList_->setObjectName(QStringLiteral("excludedList"));
    excludedLayout->addWidget(excludedList_);

    auto* excludedButtonsLayout = new QHBoxLayout();
    addExcludedButton_ = new QPushButton(tr("Add..."), excludedGroup);
    addExcludedButton_->setObjectName(QStringLiteral("addExcludedButton"));
    removeExcludedButton_ = new QPushButton(tr("Remove"), excludedGroup);
    removeExcludedButton_->setObjectName(QStringLiteral("removeExcludedButton"));
    excludedButtonsLayout->addWidget(addExcludedButton_);
    excludedButtonsLayout->addWidget(removeExcludedButton_);
    excludedButtonsLayout->addStretch();
    excludedLayout->addLayout(excludedButtonsLayout);

    mainLayout->addWidget(excludedGroup);

    connect(addExcludedButton_, &QPushButton::clicked, this, &FirstRunDialog::OnAddExcludedFolder);
    connect(removeExcludedButton_, &QPushButton::clicked, this,
            &FirstRunDialog::OnRemoveExcludedFolder);

    // OK/Cancel.
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox->setObjectName(QStringLiteral("buttonBox"));
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
}

void FirstRunDialog::PopulateMountList() {
    for (const MountInfo& mount : mounts_) {
        QString display = QString::fromStdString(mount.mountPoint);
        if (!mount.label.empty()) {
            display +=
                QStringLiteral(" (") + QString::fromStdString(mount.label) + QStringLiteral(")");
        }

        auto* item = new QListWidgetItem(display);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, QString::fromStdString(mount.mountPoint));
        mountList_->addItem(item);
    }
}

void FirstRunDialog::PreselectMounts() {
    // Longest-prefix-match of homeDir_ among the known mount points; "/" is
    // always a prefix of every absolute path, so it never needs the "next
    // char is a separator" check that other mount points do.
    std::string bestPrefix;
    for (const MountInfo& mount : mounts_) {
        const std::string& mountPoint = mount.mountPoint;
        bool isPrefix = false;
        if (mountPoint == "/") {
            isPrefix = true;
        } else if (homeDir_ == mountPoint) {
            isPrefix = true;
        } else if (homeDir_.size() > mountPoint.size() &&
                   homeDir_.compare(0, mountPoint.size(), mountPoint) == 0 &&
                   homeDir_[mountPoint.size()] == '/') {
            isPrefix = true;
        }

        if (isPrefix && mountPoint.size() > bestPrefix.size()) {
            bestPrefix = mountPoint;
        }
    }

    for (int i = 0; i < mountList_->count(); ++i) {
        QListWidgetItem* item = mountList_->item(i);
        const std::string mountPoint = item->data(Qt::UserRole).toString().toStdString();
        // Pre-select both the mount containing $HOME (longest-prefix match)
        // and "/" itself, per indexed-plan.md §19 -- these may be the same
        // mount or two different ones.
        if (mountPoint == bestPrefix || mountPoint == "/") {
            item->setCheckState(Qt::Checked);
        }
    }
}

FirstRunResult FirstRunDialog::Result() const {
    FirstRunResult result;

    for (int i = 0; i < mountList_->count(); ++i) {
        const QListWidgetItem* item = mountList_->item(i);
        if (item->checkState() == Qt::Checked) {
            result.selectedRoots.push_back(item->data(Qt::UserRole).toString().toStdString());
        }
    }

    if (manualOnlyCheck_->isChecked()) {
        result.reindexIntervalHours = 0;
    } else {
        const int value = intervalSpin_->value();
        const bool isDays = intervalUnitCombo_->currentIndex() == 1;
        result.reindexIntervalHours = isDays ? value * 24 : value;
    }

    for (int i = 0; i < excludedList_->count(); ++i) {
        result.excludedPaths.push_back(excludedList_->item(i)->text().toStdString());
    }

    return result;
}

QString FirstRunDialog::PickFolder() {
    return QFileDialog::getExistingDirectory(this, tr("Select Folder to Exclude"),
                                             QString::fromStdString(homeDir_));
}

void FirstRunDialog::OnManualOnlyToggled(bool checked) {
    intervalSpin_->setEnabled(!checked);
    intervalUnitCombo_->setEnabled(!checked);
}

void FirstRunDialog::OnAddExcludedFolder() {
    const QString dir = PickFolder();
    if (!dir.isEmpty()) {
        excludedList_->addItem(dir);
    }
}

void FirstRunDialog::OnRemoveExcludedFolder() {
    QListWidgetItem* item = excludedList_->currentItem();
    if (item != nullptr) {
        delete excludedList_->takeItem(excludedList_->row(item));
    }
}

}  // namespace indexed
