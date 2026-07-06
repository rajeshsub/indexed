#pragma once

#include <QAbstractTableModel>

#include "ui/DisplayEntry.h"
#include <vector>

namespace indexed {

// QAbstractTableModel over a DisplayEntry snapshot (indexed-plan.md §6.1,
// §19). ResultView owns column widths/header behavior; this class only owns
// the data + sorting/mapping logic. A fresh snapshot is loaded wholesale via
// SetEntries every time a search completes -- there's no incremental
// update path in v0.1.0.
class ResultModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        kName = 0,
        kPath = 1,
        kSize = 2,
        kDateModified = 3,
        kColumnCount = 4,
    };

    explicit ResultModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    // Sorts the internal snapshot in place. Size/Date sort numerically on
    // DisplayEntry::sizeBytes/lastModifiedNs (not the formatted text, which
    // would sort "10 B" before "9 B" lexically). Name/Path sort
    // case-insensitively -- a reasonable default for a file-search tool
    // where case-only differences are rare and users expect "a" and "A"
    // to interleave, not cluster.
    //
    // Implemented as a full beginResetModel()/endResetModel(), not the
    // layoutAboutToBeChanged/changePersistentIndex/layoutChanged dance that
    // would preserve selection: results re-populate on every search anyway
    // (SetEntries fully replaces the snapshot), so there's little for a
    // sort to preserve across, and the simpler reset is easier to reason
    // about for v0.1.0.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // Replaces the whole snapshot, e.g. when a new search completes.
    void SetEntries(std::vector<DisplayEntry> entries);

    // Maps a row back to the DisplayEntry it displays, e.g. for
    // ResultView/MainWindow action handling (open/reveal/copy/etc.).
    const DisplayEntry& EntryAt(int row) const;

private:
    std::vector<DisplayEntry> entries_;
};

}  // namespace indexed
