#include "ui/SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_set>

namespace indexed {

namespace {

// Default shown in the interval field whenever reindexing isn't manual-only
// but the field would otherwise be blank/invalid (mirrors winindex's
// kReindexDefaultHours fallback).
constexpr int kDefaultReindexHours = 48;

std::vector<std::string> ListWidgetItems(const QListWidget* list) {
    std::vector<std::string> items;
    items.reserve(static_cast<size_t>(list->count()));
    for (int i = 0; i < list->count(); ++i) {
        items.push_back(list->item(i)->text().toStdString());
    }
    return items;
}

}  // namespace

bool RootsDiff::bothChanged() const {
    return !added.empty() && !removed.empty();
}

RootsDiff DiffRoots(const std::vector<std::string>& oldRoots,
                    const std::vector<std::string>& newRoots) {
    const std::unordered_set<std::string> oldSet(oldRoots.begin(), oldRoots.end());
    const std::unordered_set<std::string> newSet(newRoots.begin(), newRoots.end());

    RootsDiff diff;
    for (const auto& root : newRoots) {
        if (oldSet.find(root) == oldSet.end()) {
            diff.added.push_back(root);
        }
    }
    for (const auto& root : oldRoots) {
        if (newSet.find(root) == newSet.end()) {
            diff.removed.push_back(root);
        }
    }
    return diff;
}

SettingsDialog::SettingsDialog(SettingsDialogInitialState initial, QWidget* parent)
    : QDialog(parent), initial_(std::move(initial)) {
    BuildUi();
}

void SettingsDialog::BuildUi() {
    setWindowTitle(QStringLiteral("indexed — Settings"));

    auto* mainLayout = new QVBoxLayout(this);

    // --- Paths to index ---
    auto* pathsGroup = new QGroupBox(QStringLiteral("Paths to index"), this);
    auto* pathsLayout = new QVBoxLayout(pathsGroup);

    pathList_ = new QListWidget(pathsGroup);
    pathList_->setObjectName(QStringLiteral("pathList"));
    pathList_->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const auto& root : initial_.selectedRoots) {
        pathList_->addItem(QString::fromStdString(root));
    }
    pathsLayout->addWidget(pathList_);

    auto* pathsButtonsLayout = new QHBoxLayout();
    auto* addPathButton = new QPushButton(QStringLiteral("Add Location…"), pathsGroup);
    addPathButton->setObjectName(QStringLiteral("addPathButton"));
    auto* removePathButton = new QPushButton(QStringLiteral("Remove"), pathsGroup);
    removePathButton->setObjectName(QStringLiteral("removePathButton"));
    pathsButtonsLayout->addWidget(addPathButton);
    pathsButtonsLayout->addWidget(removePathButton);
    pathsButtonsLayout->addStretch();
    pathsLayout->addLayout(pathsButtonsLayout);

    mainLayout->addWidget(pathsGroup);

    // --- Automatic Reindex ---
    auto* reindexGroup = new QGroupBox(QStringLiteral("Automatic Reindex"), this);
    auto* reindexLayout = new QHBoxLayout(reindexGroup);

    manualOnlyCheck_ = new QCheckBox(QStringLiteral("Manual only"), reindexGroup);
    manualOnlyCheck_->setObjectName(QStringLiteral("manualOnlyCheck"));
    reindexLayout->addWidget(manualOnlyCheck_);

    intervalEdit_ = new QLineEdit(reindexGroup);
    intervalEdit_->setObjectName(QStringLiteral("intervalEdit"));
    reindexLayout->addWidget(intervalEdit_);

    unitCombo_ = new QComboBox(reindexGroup);
    unitCombo_->setObjectName(QStringLiteral("unitCombo"));
    unitCombo_->addItem(QStringLiteral("Hours"));
    unitCombo_->addItem(QStringLiteral("Days"));
    reindexLayout->addWidget(unitCombo_);
    reindexLayout->addStretch();

    mainLayout->addWidget(reindexGroup);

    const bool manualOnly = (initial_.reindexIntervalHours == 0);
    manualOnlyCheck_->setChecked(manualOnly);
    if (manualOnly) {
        intervalEdit_->setText(QString::number(kDefaultReindexHours));
        unitCombo_->setCurrentIndex(0);
    } else {
        const int hours = initial_.reindexIntervalHours;
        if (hours % 24 == 0 && hours >= 24) {
            intervalEdit_->setText(QString::number(hours / 24));
            unitCombo_->setCurrentIndex(1);  // Days
        } else {
            intervalEdit_->setText(QString::number(hours));
            unitCombo_->setCurrentIndex(0);  // Hours
        }
    }
    intervalEdit_->setEnabled(!manualOnly);
    unitCombo_->setEnabled(!manualOnly);

    // --- Excluded folders ---
    auto* excludedGroup = new QGroupBox(QStringLiteral("Excluded folders"), this);
    auto* excludedLayout = new QVBoxLayout(excludedGroup);

    excludedList_ = new QListWidget(excludedGroup);
    excludedList_->setObjectName(QStringLiteral("excludedList"));
    excludedList_->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const auto& path : initial_.excludedPaths) {
        excludedList_->addItem(QString::fromStdString(path));
    }
    excludedLayout->addWidget(excludedList_);

    auto* excludedButtonsLayout = new QHBoxLayout();
    auto* addExcludedButton = new QPushButton(QStringLiteral("Add…"), excludedGroup);
    addExcludedButton->setObjectName(QStringLiteral("addExcludedButton"));
    auto* removeExcludedButton = new QPushButton(QStringLiteral("Remove"), excludedGroup);
    removeExcludedButton->setObjectName(QStringLiteral("removeExcludedButton"));
    excludedButtonsLayout->addWidget(addExcludedButton);
    excludedButtonsLayout->addWidget(removeExcludedButton);
    excludedButtonsLayout->addStretch();
    excludedLayout->addLayout(excludedButtonsLayout);

    mainLayout->addWidget(excludedGroup);

    // --- OK/Cancel ---
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttonBox);

    connect(addPathButton, &QPushButton::clicked, this, &SettingsDialog::OnAddPath);
    connect(removePathButton, &QPushButton::clicked, this, &SettingsDialog::OnRemovePath);
    connect(addExcludedButton, &QPushButton::clicked, this, &SettingsDialog::OnAddExcluded);
    connect(removeExcludedButton, &QPushButton::clicked, this, &SettingsDialog::OnRemoveExcluded);
    connect(manualOnlyCheck_, &QCheckBox::toggled, this, &SettingsDialog::OnManualOnlyToggled);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

SettingsDialogResult SettingsDialog::Result() const {
    return result_;
}

void SettingsDialog::accept() {
    result_.selectedRoots = ListWidgetItems(pathList_);
    result_.excludedPaths = ListWidgetItems(excludedList_);

    if (manualOnlyCheck_->isChecked()) {
        result_.reindexIntervalHours = 0;
    } else {
        bool ok = false;
        int value = intervalEdit_->text().toInt(&ok);
        if (!ok || value <= 0) {
            value = kDefaultReindexHours;
        }
        if (unitCombo_->currentIndex() == 1) {  // Days -> hours
            value *= 24;
        }
        result_.reindexIntervalHours = value;
    }

    QDialog::accept();
}

QString SettingsDialog::PickFolder() {
    return QFileDialog::getExistingDirectory(this);
}

void SettingsDialog::OnAddPath() {
    const QString folder = PickFolder();
    if (folder.isEmpty()) {
        return;
    }
    for (int i = 0; i < pathList_->count(); ++i) {
        if (pathList_->item(i)->text() == folder) {
            return;  // already present
        }
    }
    pathList_->addItem(folder);
}

void SettingsDialog::OnRemovePath() {
    const int row = pathList_->currentRow();
    if (row >= 0) {
        delete pathList_->takeItem(row);
    }
}

void SettingsDialog::OnAddExcluded() {
    const QString folder = PickFolder();
    if (folder.isEmpty()) {
        return;
    }
    excludedList_->addItem(folder);
}

void SettingsDialog::OnRemoveExcluded() {
    const int row = excludedList_->currentRow();
    if (row >= 0) {
        delete excludedList_->takeItem(row);
    }
}

void SettingsDialog::OnManualOnlyToggled(bool checked) {
    intervalEdit_->setEnabled(!checked);
    unitCombo_->setEnabled(!checked);
}

}  // namespace indexed
