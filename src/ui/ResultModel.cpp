#include "ui/ResultModel.h"

#include <algorithm>
#include <cctype>

namespace indexed {

namespace {

// Case-insensitive lexical comparison for Name/Path columns. Bytewise
// tolower is sufficient here: DisplayEntry::name/parentDir come from
// filesystem paths, which are treated as opaque byte strings elsewhere in
// this codebase (see IndexPool), so a locale-aware collation isn't
// warranted -- and would make sort order depend on the runtime locale.
bool LessCaseInsensitive(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(a[i]));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(b[i]));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

}  // namespace

ResultModel::ResultModel(QObject* parent) : QAbstractTableModel(parent) {}

int ResultModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(entries_.size());
}

int ResultModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return kColumnCount;
}

QVariant ResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<size_t>(index.row()) >= entries_.size()) {
        return QVariant();
    }

    const DisplayEntry& entry = entries_[static_cast<size_t>(index.row())];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case kName:
                return QString::fromStdString(entry.name);
            case kPath:
                return QString::fromStdString(entry.parentDir);
            case kSize:
                return QString::fromStdString(entry.sizeText);
            case kDateModified:
                return QString::fromStdString(entry.dateText);
            default:
                return QVariant();
        }
    }

    if (role == Qt::TextAlignmentRole && index.column() == kSize) {
        return QVariant(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
    }

    return QVariant();
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    switch (section) {
        case kName:
            return QStringLiteral("Name");
        case kPath:
            return QStringLiteral("Path");
        case kSize:
            return QStringLiteral("Size");
        case kDateModified:
            return QStringLiteral("Date Modified");
        default:
            return QVariant();
    }
}

void ResultModel::sort(int column, Qt::SortOrder order) {
    beginResetModel();

    const bool ascending = (order == Qt::AscendingOrder);
    switch (column) {
        case kName:
            std::sort(entries_.begin(), entries_.end(),
                      [ascending](const DisplayEntry& a, const DisplayEntry& b) {
                          return ascending ? LessCaseInsensitive(a.name, b.name)
                                           : LessCaseInsensitive(b.name, a.name);
                      });
            break;
        case kPath:
            std::sort(entries_.begin(), entries_.end(),
                      [ascending](const DisplayEntry& a, const DisplayEntry& b) {
                          return ascending ? LessCaseInsensitive(a.parentDir, b.parentDir)
                                           : LessCaseInsensitive(b.parentDir, a.parentDir);
                      });
            break;
        case kSize:
            std::sort(entries_.begin(), entries_.end(),
                      [ascending](const DisplayEntry& a, const DisplayEntry& b) {
                          return ascending ? a.sizeBytes < b.sizeBytes : b.sizeBytes < a.sizeBytes;
                      });
            break;
        case kDateModified:
            std::sort(entries_.begin(), entries_.end(),
                      [ascending](const DisplayEntry& a, const DisplayEntry& b) {
                          return ascending ? a.lastModifiedNs < b.lastModifiedNs
                                           : b.lastModifiedNs < a.lastModifiedNs;
                      });
            break;
        default:
            break;
    }

    endResetModel();
}

void ResultModel::SetEntries(std::vector<DisplayEntry> entries) {
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

const DisplayEntry& ResultModel::EntryAt(int row) const {
    return entries_.at(static_cast<size_t>(row));
}

}  // namespace indexed
