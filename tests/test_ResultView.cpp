#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QMimeData>
#include <QMouseEvent>
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
    void ctrlCPutsFileObjectMimeOnClipboard();
    void ctrlXPutsCutMarkerOnClipboard();
    void copyMimeCarriesAllSelectedUris();
    void contextCopyAndCutMatchKeyboard();
    void deleteEmitsTrashRequested();
    void shiftDeleteEmitsDeletePermanentlyRequested();
    void contextMenuHasExpectedActions();
    void contextMenuOpenDisabledForMultiSelection();
    void dragMimeDataIsFileObjectPayload();
    void dragActuallyInitiatesOnMouseDrag();
    void viewIsDragOnly();
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

void TestResultView::ctrlCPutsFileObjectMimeOnClipboard() {
    ResultView* view = NewPopulatedView();
    view->setCurrentIndex(view->model()->index(1, 0));
    QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);

    const QMimeData* mime = QApplication::clipboard()->mimeData();
    QVERIFY(mime->hasUrls());
    QCOMPARE(mime->urls().size(), 1);
    QCOMPARE(mime->urls().first(), QUrl::fromLocalFile("/home/user/docs/beta.txt"));
    QVERIFY(mime->hasFormat("x-special/gnome-copied-files"));
    QVERIFY(mime->data("x-special/gnome-copied-files").startsWith("copy\n"));
    QCOMPARE(mime->text(), QString("/home/user/docs/beta.txt"));  // plain-text fallback
}

void TestResultView::ctrlXPutsCutMarkerOnClipboard() {
    ResultView* view = NewPopulatedView();
    view->setCurrentIndex(view->model()->index(0, 0));
    QTest::keyClick(view, Qt::Key_X, Qt::ControlModifier);

    const QMimeData* mime = QApplication::clipboard()->mimeData();
    QVERIFY(mime->hasUrls());
    QVERIFY(mime->data("x-special/gnome-copied-files").startsWith("cut\n"));
}

void TestResultView::copyMimeCarriesAllSelectedUris() {
    ResultView* view = NewPopulatedView();
    view->selectionModel()->select(view->model()->index(0, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    view->selectionModel()->select(view->model()->index(2, 0),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);

    const QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
    QCOMPARE(urls.size(), 2);
    QVERIFY(urls.contains(QUrl::fromLocalFile("/home/user/alpha.txt")));
    QVERIFY(urls.contains(QUrl::fromLocalFile("/gamma.txt")));
}

void TestResultView::contextCopyAndCutMatchKeyboard() {
    ResultView* view = NewPopulatedView();
    view->setCurrentIndex(view->model()->index(0, 0));
    QMenu* menu = view->BuildContextMenu(view);

    QAction* copy = menu->findChild<QAction*>("copyAction");
    QAction* cut = menu->findChild<QAction*>("cutAction");
    QVERIFY(copy != nullptr);
    QVERIFY(cut != nullptr);

    copy->trigger();
    QVERIFY(QApplication::clipboard()
                ->mimeData()
                ->data("x-special/gnome-copied-files")
                .startsWith("copy\n"));
    cut->trigger();
    QVERIFY(QApplication::clipboard()
                ->mimeData()
                ->data("x-special/gnome-copied-files")
                .startsWith("cut\n"));
    delete menu;
}

void TestResultView::deleteEmitsTrashRequested() {
    ResultView* view = NewPopulatedView();
    QSignalSpy trashSpy(view, &ResultView::TrashRequested);
    QSignalSpy permaSpy(view, &ResultView::DeletePermanentlyRequested);
    view->setCurrentIndex(view->model()->index(2, 0));
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(trashSpy.count(), 1);
    QCOMPARE(permaSpy.count(), 0);
    QCOMPARE(trashSpy.at(0).at(0).toStringList(), QStringList{"/gamma.txt"});
}

void TestResultView::shiftDeleteEmitsDeletePermanentlyRequested() {
    ResultView* view = NewPopulatedView();
    QSignalSpy trashSpy(view, &ResultView::TrashRequested);
    QSignalSpy permaSpy(view, &ResultView::DeletePermanentlyRequested);
    view->setCurrentIndex(view->model()->index(1, 0));
    QTest::keyClick(view, Qt::Key_Delete, Qt::ShiftModifier);
    QCOMPARE(permaSpy.count(), 1);
    QCOMPARE(trashSpy.count(), 0);
    QCOMPARE(permaSpy.at(0).at(0).toStringList(), QStringList{"/home/user/docs/beta.txt"});
}

void TestResultView::contextMenuHasExpectedActions() {
    ResultView* view = NewPopulatedView();
    view->setCurrentIndex(view->model()->index(0, 0));
    QMenu* menu = view->BuildContextMenu(view);
    QVERIFY(menu->findChild<QAction*>("openAction") != nullptr);
    QVERIFY(menu->findChild<QAction*>("revealAction") != nullptr);
    QVERIFY(menu->findChild<QAction*>("copyAction") != nullptr);
    QVERIFY(menu->findChild<QAction*>("cutAction") != nullptr);
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

void TestResultView::dragMimeDataIsFileObjectPayload() {
    ResultView* view = NewPopulatedView();
    QMimeData* mime = view->BuildDragMimeData({0, 2});
    QVERIFY(mime->hasUrls());
    const QList<QUrl> urls = mime->urls();
    QCOMPARE(urls.size(), 2);
    QCOMPARE(urls[0], QUrl::fromLocalFile("/home/user/alpha.txt"));
    QCOMPARE(urls[1], QUrl::fromLocalFile("/gamma.txt"));
    QCOMPARE(mime->text(), QString("/home/user/alpha.txt\n/gamma.txt"));
    // A drag is always a copy (docs/adr/0005): no gnome cut/copy marker.
    QVERIFY(!mime->hasFormat("x-special/gnome-copied-files"));
    delete mime;
}

namespace {

// Records startDrag() calls without opening the modal QDrag::exec, so the
// real mouse-drag gesture can be verified end to end. startDrag is only
// reached when the pressed index is Qt::ItemIsDragEnabled (docs/adr/0013) --
// the whole point of the regression guard below.
class DragProbeView : public ResultView {
public:
    int startDragCalls = 0;

protected:
    void startDrag(Qt::DropActions) override { ++startDragCalls; }
};

}  // namespace

void TestResultView::dragActuallyInitiatesOnMouseDrag() {
    auto* model = new ResultModel(this);
    model->SetEntries(ThreeEntries());
    auto* view = new DragProbeView;
    owned_.push_back(view);
    view->SetResultModel(model);
    view->resize(800, 400);
    view->show();
    QVERIFY(QTest::qWaitForWindowExposed(view));

    const QModelIndex idx = view->model()->index(0, 0);
    QVERIFY(view->model()->flags(idx).testFlag(Qt::ItemIsDragEnabled));

    const QRect rowRect = view->visualRect(idx);
    const QPoint press = rowRect.center();
    const QPoint release = press + QPoint(QApplication::startDragDistance() * 3, 0);

    QTest::mousePress(view->viewport(), Qt::LeftButton, {}, press);
    QMouseEvent move(QEvent::MouseMove, release, view->viewport()->mapToGlobal(release),
                     Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(view->viewport(), &move);
    QTest::mouseRelease(view->viewport(), Qt::LeftButton, {}, release);

    QCOMPARE(view->startDragCalls, 1);
}

void TestResultView::viewIsDragOnly() {
    ResultView* view = NewPopulatedView();
    QCOMPARE(view->dragDropMode(), QAbstractItemView::DragOnly);
    QVERIFY(view->dragEnabled());
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
