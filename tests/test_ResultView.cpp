#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include "ui/ResultView.h"

using indexed::DisplayEntry;
using indexed::ResultModel;
using indexed::ResultView;

namespace {

DisplayEntry MakeEntry(const std::string& name, const std::string& parentDir, uint64_t size,
                       uint64_t mtimeNs) {
    DisplayEntry e;
    e.name = name;
    e.parentDir = parentDir;
    e.sizeBytes = size;
    e.lastModifiedNs = mtimeNs;
    e.sizeText = std::to_string(size) + " B";
    e.dateText = "2024-01-01 00:00";
    return e;
}

std::vector<DisplayEntry> ThreeEntries() {
    return {
        MakeEntry("alpha.txt", "/home/user", 100, 3'000'000'000ULL),
        MakeEntry("beta.txt", "/home/user/docs", 900, 1'000'000'000ULL),
        MakeEntry("gamma.txt", "/", 50, 2'000'000'000ULL),
    };
}

}  // namespace

class TestResultView : public QObject {
    Q_OBJECT

private slots:
    void columnWidthsMatchSpec();
    void viewBehaviorFlagsMatchSpec();
    void fullPathJoinsParentDirAndName();
    void fullPathCollapsesRootSlash();
    void selectedFullPathsFollowSelection();
    void enterEmitsOpenRequestedForSingleSelection();
    void enterWithMultiSelectionEmitsNothing();
    void ctrlEnterEmitsRevealRequested();
    void ctrlCCopiesFullPathsToClipboard();
    void ctrlXEmitsCutRequested();
    void deleteEmitsTrashRequested();
    void contextMenuHasExpectedActions();
    void contextMenuOpenDisabledForMultiSelection();
    void dragMimeDataIsUriList();
    void sizeHeaderFirstClickSortsDescending();

private:
    // Fresh view+model wired together, populated with ThreeEntries().
    ResultView* NewPopulatedView();

    std::vector<QObject*> owned_;
};

ResultView* TestResultView::NewPopulatedView() {
    auto* model = new ResultModel(this);
    model->SetEntries(ThreeEntries());
    auto* view = new ResultView;
    view->resize(800, 400);
    view->SetResultModel(model);
    owned_.push_back(view);
    return view;
}

void TestResultView::columnWidthsMatchSpec() {
    ResultView* view = NewPopulatedView();
    QCOMPARE(view->columnWidth(ResultModel::kName), 250);
    QCOMPARE(view->columnWidth(ResultModel::kPath), 350);
    QCOMPARE(view->columnWidth(ResultModel::kSize), 90);
    QCOMPARE(view->columnWidth(ResultModel::kDateModified), 140);
}

void TestResultView::viewBehaviorFlagsMatchSpec() {
    ResultView* view = NewPopulatedView();
    QCOMPARE(view->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(view->selectionMode(), QAbstractItemView::ExtendedSelection);
    QVERIFY(view->alternatingRowColors());
    QVERIFY(view->isSortingEnabled());
    QVERIFY(!view->rootIsDecorated());
    QVERIFY(view->header()->sectionsMovable());
    QVERIFY(view->dragEnabled());
}

void TestResultView::fullPathJoinsParentDirAndName() {
    ResultView* view = NewPopulatedView();
    QCOMPARE(view->FullPathForRow(0), QString("/home/user/alpha.txt"));
}

void TestResultView::fullPathCollapsesRootSlash() {
    ResultView* view = NewPopulatedView();
    QCOMPARE(view->FullPathForRow(2), QString("/gamma.txt"));
}

void TestResultView::selectedFullPathsFollowSelection() {
    ResultView* view = NewPopulatedView();
    view->selectionModel()->select(view->model()->index(0, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    view->selectionModel()->select(view->model()->index(2, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const QStringList paths = view->SelectedFullPaths();
    QCOMPARE(paths.size(), 2);
    QVERIFY(paths.contains("/home/user/alpha.txt"));
    QVERIFY(paths.contains("/gamma.txt"));
}

void TestResultView::enterEmitsOpenRequestedForSingleSelection() {
    ResultView* view = NewPopulatedView();
    QSignalSpy spy(view, &ResultView::OpenRequested);
    view->setCurrentIndex(view->model()->index(1, 0));
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QString("/home/user/docs/beta.txt"));
}

void TestResultView::enterWithMultiSelectionEmitsNothing() {
    ResultView* view = NewPopulatedView();
    QSignalSpy spy(view, &ResultView::OpenRequested);
    view->selectionModel()->select(view->model()->index(0, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    view->selectionModel()->select(view->model()->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(spy.count(), 0);
}

void TestResultView::ctrlEnterEmitsRevealRequested() {
    ResultView* view = NewPopulatedView();
    QSignalSpy spy(view, &ResultView::RevealRequested);
    view->setCurrentIndex(view->model()->index(0, 0));
    QTest::keyClick(view, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QString("/home/user/alpha.txt"));
}

void TestResultView::ctrlCCopiesFullPathsToClipboard() {
    ResultView* view = NewPopulatedView();
    view->setCurrentIndex(view->model()->index(1, 0));
    QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), QString("/home/user/docs/beta.txt"));
}

void TestResultView::ctrlXEmitsCutRequested() {
    ResultView* view = NewPopulatedView();
    QSignalSpy spy(view, &ResultView::CutRequested);
    view->setCurrentIndex(view->model()->index(0, 0));
    QTest::keyClick(view, Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{"/home/user/alpha.txt"});
}

void TestResultView::deleteEmitsTrashRequested() {
    ResultView* view = NewPopulatedView();
    QSignalSpy spy(view, &ResultView::TrashRequested);
    view->setCurrentIndex(view->model()->index(2, 0));
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList{"/gamma.txt"});
}

void TestResultView::contextMenuHasExpectedActions() {
    ResultView* view = NewPopulatedView();
    view->setCurrentIndex(view->model()->index(0, 0));
    QMenu* menu = view->BuildContextMenu(view);
    QVERIFY(menu->findChild<QAction*>("openAction") != nullptr);
    QVERIFY(menu->findChild<QAction*>("revealAction") != nullptr);
    QVERIFY(menu->findChild<QAction*>("copyPathAction") != nullptr);
    QVERIFY(menu->findChild<QAction*>("copyNameAction") != nullptr);
    QVERIFY(menu->findChild<QAction*>("openAction")->isEnabled());
    delete menu;
}

void TestResultView::contextMenuOpenDisabledForMultiSelection() {
    ResultView* view = NewPopulatedView();
    view->selectionModel()->select(view->model()->index(0, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    view->selectionModel()->select(view->model()->index(1, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QMenu* menu = view->BuildContextMenu(view);
    QVERIFY(!menu->findChild<QAction*>("openAction")->isEnabled());
    delete menu;
}

void TestResultView::dragMimeDataIsUriList() {
    ResultView* view = NewPopulatedView();
    QMimeData* mime = view->BuildDragMimeData({0, 2});
    QVERIFY(mime->hasUrls());
    const QList<QUrl> urls = mime->urls();
    QCOMPARE(urls.size(), 2);
    QCOMPARE(urls[0], QUrl::fromLocalFile("/home/user/alpha.txt"));
    QCOMPARE(urls[1], QUrl::fromLocalFile("/gamma.txt"));
    delete mime;
}

void TestResultView::sizeHeaderFirstClickSortsDescending() {
    ResultView* view = NewPopulatedView();
    view->show();
    // The user's first click on the Size header section.
    QHeaderView* header = view->header();
    const int x = header->sectionViewportPosition(ResultModel::kSize) +
                  header->sectionSize(ResultModel::kSize) / 2;
    QTest::mouseClick(header->viewport(), Qt::LeftButton, {}, QPoint(x, header->height() / 2));
    QCOMPARE(view->header()->sortIndicatorSection(), static_cast<int>(ResultModel::kSize));
    QCOMPARE(view->header()->sortIndicatorOrder(), Qt::DescendingOrder);
    // Largest first after the descending sort.
    QCOMPARE(view->Model()->EntryAt(0).sizeBytes, 900u);
}

QTEST_MAIN(TestResultView)
#include "test_ResultView.moc"
