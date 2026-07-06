#include <QSignalSpy>
#include <QtTest>

#include "ui/SearchLineEdit.h"

using indexed::SearchLineEdit;

class TestSearchLineEdit : public QObject {
    Q_OBJECT

private slots:
    void emptyTextShowsEnterSearchTermStatus();
    void oneCharacterShowsTypeAtLeastTwoStatus();
    void debounceFiresSearchRequestedAfter150ms();
    void debounceRestartsOnEachKeystroke();
    void belowTwoCharactersNeverEmitsSearchRequested();
    void downKeyEmitsNavigateResultsRequested();
    void upKeyEmitsNavigateResultsRequestedToo();
    void clearingBackToEmptyReemitsEnterSearchTermStatus();
};

void TestSearchLineEdit::emptyTextShowsEnterSearchTermStatus() {
    SearchLineEdit edit;

    // A freshly-constructed, untouched box is empty; the "enter a search
    // term" message is surfaced as the QLineEdit's own placeholder text.
    QCOMPARE(edit.placeholderText(), QString("Enter a search term to begin."));

    // Typing something and then clearing it back to empty in one shot
    // (2+ chars straight to 0) should also emit the status signal.
    QTest::keyClicks(&edit, "ab");
    QSignalSpy statusSpy(&edit, &SearchLineEdit::StatusMessageChanged);
    edit.clear();

    QVERIFY(statusSpy.count() >= 1);
    const QList<QVariant> lastCall = statusSpy.takeLast();
    QCOMPARE(lastCall.at(0).toString(), QString("Enter a search term to begin."));
}

void TestSearchLineEdit::oneCharacterShowsTypeAtLeastTwoStatus() {
    SearchLineEdit edit;
    QSignalSpy statusSpy(&edit, &SearchLineEdit::StatusMessageChanged);

    QTest::keyClicks(&edit, "a");

    QVERIFY(statusSpy.count() >= 1);
    const QList<QVariant> lastCall = statusSpy.takeLast();
    QCOMPARE(lastCall.at(0).toString(), QString("Type at least 2 characters to search…"));
}

void TestSearchLineEdit::debounceFiresSearchRequestedAfter150ms() {
    SearchLineEdit edit;
    QSignalSpy searchSpy(&edit, &SearchLineEdit::SearchRequested);

    QTest::keyClicks(&edit, "ab");

    // Should not fire immediately.
    QCOMPARE(searchSpy.count(), 0);

    QVERIFY(searchSpy.wait(500));
    QCOMPARE(searchSpy.count(), 1);
    QCOMPARE(searchSpy.at(0).at(0).toString(), QString("ab"));
}

void TestSearchLineEdit::debounceRestartsOnEachKeystroke() {
    SearchLineEdit edit;
    QSignalSpy searchSpy(&edit, &SearchLineEdit::SearchRequested);

    QTest::keyClick(&edit, Qt::Key_A);
    QTest::qWait(80);
    QTest::keyClick(&edit, Qt::Key_B);
    QTest::qWait(80);
    QTest::keyClick(&edit, Qt::Key_C);

    // 160ms have elapsed across the two waits, but each keystroke should
    // have restarted the 150ms timer, so nothing should have fired yet.
    QCOMPARE(searchSpy.count(), 0);

    QVERIFY(searchSpy.wait(500));
    QCOMPARE(searchSpy.count(), 1);
    QCOMPARE(searchSpy.at(0).at(0).toString(), QString("abc"));
}

void TestSearchLineEdit::belowTwoCharactersNeverEmitsSearchRequested() {
    SearchLineEdit edit;
    QSignalSpy searchSpy(&edit, &SearchLineEdit::SearchRequested);

    QTest::keyClicks(&edit, "a");
    QTest::qWait(300);

    QCOMPARE(searchSpy.count(), 0);
}

void TestSearchLineEdit::downKeyEmitsNavigateResultsRequested() {
    SearchLineEdit edit;
    QSignalSpy navSpy(&edit, &SearchLineEdit::NavigateResultsRequested);

    QTest::keyClick(&edit, Qt::Key_Down);

    QCOMPARE(navSpy.count(), 1);
    QCOMPARE(navSpy.at(0).at(0).value<Qt::Key>(), Qt::Key_Down);
}

void TestSearchLineEdit::upKeyEmitsNavigateResultsRequestedToo() {
    SearchLineEdit edit;
    QSignalSpy navSpy(&edit, &SearchLineEdit::NavigateResultsRequested);

    QTest::keyClick(&edit, Qt::Key_Up);

    QCOMPARE(navSpy.count(), 1);
    QCOMPARE(navSpy.at(0).at(0).value<Qt::Key>(), Qt::Key_Up);
}

void TestSearchLineEdit::clearingBackToEmptyReemitsEnterSearchTermStatus() {
    SearchLineEdit edit;
    QTest::keyClicks(&edit, "a");

    QSignalSpy statusSpy(&edit, &SearchLineEdit::StatusMessageChanged);
    QTest::keyClick(&edit, Qt::Key_Backspace);

    QVERIFY(statusSpy.count() >= 1);
    const QList<QVariant> lastCall = statusSpy.takeLast();
    QCOMPARE(lastCall.at(0).toString(), QString("Enter a search term to begin."));
}

QTEST_MAIN(TestSearchLineEdit)
#include "test_SearchLineEdit.moc"
