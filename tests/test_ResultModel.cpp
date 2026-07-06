#include <QSignalSpy>
#include <QTest>

#include "ui/ResultModel.h"

using indexed::DisplayEntry;
using indexed::ResultModel;

namespace {

DisplayEntry MakeEntry(std::string name, std::string parentDir, std::string sizeText,
                       std::string dateText, uint64_t sizeBytes, uint64_t lastModifiedNs,
                       size_t sourceIndex) {
    DisplayEntry entry;
    entry.name = std::move(name);
    entry.parentDir = std::move(parentDir);
    entry.sizeText = std::move(sizeText);
    entry.dateText = std::move(dateText);
    entry.sizeBytes = sizeBytes;
    entry.lastModifiedNs = lastModifiedNs;
    entry.sourceIndex = sourceIndex;
    return entry;
}

}  // namespace

class TestResultModel : public QObject {
    Q_OBJECT

private slots:
    void emptyModel_hasNoRowsButFourColumns();
    void setEntries_populatesRowsAndColumns();
    void data_returnsFormattedTextPerColumn();
    void data_sizeColumnIsRightAligned();
    void data_otherColumnsHaveNoAlignmentOverride();
    void headerData_returnsColumnLabels();
    void setEntries_emitsModelReset();
    void sort_sizeIsNumericNotLexical();
    void sort_dateIsNumericNotLexical();
    void sort_nameIsCaseInsensitiveLexical();
    void entryAt_tracksSourceIndexAfterSort();
};

void TestResultModel::emptyModel_hasNoRowsButFourColumns() {
    ResultModel model;
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.columnCount(), 4);
}

void TestResultModel::setEntries_populatesRowsAndColumns() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    entries.push_back(MakeEntry("a.txt", "/home", "10 B", "2024-01-01 00:00", 10, 1000, 0));
    entries.push_back(MakeEntry("b.txt", "/home", "20 B", "2024-01-02 00:00", 20, 2000, 1));
    model.SetEntries(entries);

    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.columnCount(), 4);
}

void TestResultModel::data_returnsFormattedTextPerColumn() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    entries.push_back(
        MakeEntry("doc.txt", "/home/user", "2.00 KB", "2024-03-04 12:30", 2048, 12345, 0));
    model.SetEntries(entries);

    const QModelIndex nameIdx = model.index(0, ResultModel::kName);
    const QModelIndex pathIdx = model.index(0, ResultModel::kPath);
    const QModelIndex sizeIdx = model.index(0, ResultModel::kSize);
    const QModelIndex dateIdx = model.index(0, ResultModel::kDateModified);

    QCOMPARE(model.data(nameIdx, Qt::DisplayRole).toString(), QStringLiteral("doc.txt"));
    QCOMPARE(model.data(pathIdx, Qt::DisplayRole).toString(), QStringLiteral("/home/user"));
    QCOMPARE(model.data(sizeIdx, Qt::DisplayRole).toString(), QStringLiteral("2.00 KB"));
    QCOMPARE(model.data(dateIdx, Qt::DisplayRole).toString(), QStringLiteral("2024-03-04 12:30"));
}

void TestResultModel::data_sizeColumnIsRightAligned() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    entries.push_back(MakeEntry("a.txt", "/home", "10 B", "2024-01-01 00:00", 10, 1000, 0));
    model.SetEntries(entries);

    const QModelIndex sizeIdx = model.index(0, ResultModel::kSize);
    const QVariant alignment = model.data(sizeIdx, Qt::TextAlignmentRole);
    QVERIFY(alignment.isValid());
    QCOMPARE(alignment.toInt(), static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
}

void TestResultModel::data_otherColumnsHaveNoAlignmentOverride() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    entries.push_back(MakeEntry("a.txt", "/home", "10 B", "2024-01-01 00:00", 10, 1000, 0));
    model.SetEntries(entries);

    const QModelIndex nameIdx = model.index(0, ResultModel::kName);
    QVERIFY(!model.data(nameIdx, Qt::TextAlignmentRole).isValid());
}

void TestResultModel::headerData_returnsColumnLabels() {
    ResultModel model;
    QCOMPARE(model.headerData(ResultModel::kName, Qt::Horizontal).toString(),
             QStringLiteral("Name"));
    QCOMPARE(model.headerData(ResultModel::kPath, Qt::Horizontal).toString(),
             QStringLiteral("Path"));
    QCOMPARE(model.headerData(ResultModel::kSize, Qt::Horizontal).toString(),
             QStringLiteral("Size"));
    QCOMPARE(model.headerData(ResultModel::kDateModified, Qt::Horizontal).toString(),
             QStringLiteral("Date Modified"));
}

void TestResultModel::setEntries_emitsModelReset() {
    ResultModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    std::vector<DisplayEntry> entries;
    entries.push_back(MakeEntry("a.txt", "/home", "10 B", "2024-01-01 00:00", 10, 1000, 0));
    model.SetEntries(entries);

    QCOMPARE(resetSpy.count(), 1);
}

void TestResultModel::sort_sizeIsNumericNotLexical() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    // Lexical sort of "9 B" vs "10 B" would put "10 B" first; numeric sort
    // on sizeBytes must put 9 first ascending.
    entries.push_back(MakeEntry("nine.txt", "/d", "9 B", "2024-01-01 00:00", 9, 100, 0));
    entries.push_back(MakeEntry("ten.txt", "/d", "10 B", "2024-01-01 00:00", 10, 200, 1));
    model.SetEntries(entries);

    model.sort(ResultModel::kSize, Qt::AscendingOrder);
    QCOMPARE(model.EntryAt(0).sizeBytes, 9u);
    QCOMPARE(model.EntryAt(1).sizeBytes, 10u);

    model.sort(ResultModel::kSize, Qt::DescendingOrder);
    QCOMPARE(model.EntryAt(0).sizeBytes, 10u);
    QCOMPARE(model.EntryAt(1).sizeBytes, 9u);
}

void TestResultModel::sort_dateIsNumericNotLexical() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    entries.push_back(MakeEntry("a.txt", "/d", "1 B", "2024-01-09 00:00", 1, 9, 0));
    entries.push_back(MakeEntry("b.txt", "/d", "1 B", "2024-01-10 00:00", 1, 10, 1));
    model.SetEntries(entries);

    model.sort(ResultModel::kDateModified, Qt::AscendingOrder);
    QCOMPARE(model.EntryAt(0).lastModifiedNs, 9u);
    QCOMPARE(model.EntryAt(1).lastModifiedNs, 10u);

    model.sort(ResultModel::kDateModified, Qt::DescendingOrder);
    QCOMPARE(model.EntryAt(0).lastModifiedNs, 10u);
    QCOMPARE(model.EntryAt(1).lastModifiedNs, 9u);
}

void TestResultModel::sort_nameIsCaseInsensitiveLexical() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    entries.push_back(MakeEntry("banana.txt", "/d", "1 B", "2024-01-01 00:00", 1, 1, 0));
    entries.push_back(MakeEntry("Apple.txt", "/d", "1 B", "2024-01-01 00:00", 1, 2, 1));
    model.SetEntries(entries);

    model.sort(ResultModel::kName, Qt::AscendingOrder);
    QCOMPARE(model.EntryAt(0).name, std::string("Apple.txt"));
    QCOMPARE(model.EntryAt(1).name, std::string("banana.txt"));
}

void TestResultModel::entryAt_tracksSourceIndexAfterSort() {
    ResultModel model;
    std::vector<DisplayEntry> entries;
    entries.push_back(MakeEntry("z.txt", "/d", "10 B", "2024-01-01 00:00", 10, 1, 42));
    entries.push_back(MakeEntry("a.txt", "/d", "1 B", "2024-01-01 00:00", 1, 2, 7));
    model.SetEntries(entries);

    model.sort(ResultModel::kName, Qt::AscendingOrder);

    // After sorting by name ascending, "a.txt" (sourceIndex 7) comes first.
    QCOMPARE(model.EntryAt(0).sourceIndex, 7u);
    QCOMPARE(model.EntryAt(0).name, std::string("a.txt"));
    QCOMPARE(model.EntryAt(1).sourceIndex, 42u);
    QCOMPARE(model.EntryAt(1).name, std::string("z.txt"));
}

QTEST_MAIN(TestResultModel)
#include "test_ResultModel.moc"
