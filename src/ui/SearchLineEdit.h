#pragma once

#include <QLineEdit>
#include <QString>

class QTimer;

namespace indexed {

// The app's search box (indexed-plan.md §19). A QLineEdit that:
//  - debounces typed input by 150ms before emitting SearchRequested, so the
//    search engine isn't re-run on every keystroke;
//  - reports status text ("Enter a search term...", "Type at least 2
//    characters...") via a signal rather than owning a status bar reference;
//  - forwards Up/Down key presses as a signal so MainWindow can move focus
//    into whatever results list it owns, without SearchLineEdit depending on
//    a concrete list type.
class SearchLineEdit : public QLineEdit {
    Q_OBJECT

public:
    explicit SearchLineEdit(QWidget* parent = nullptr);

signals:
    // Emitted 150ms after the user stops typing, only when the query has 2
    // or more characters.
    void SearchRequested(const QString& query);

    // Emitted whenever the "status" implied by the current text changes:
    // empty text -> "Enter a search term to begin."; 1 character ->
    // "Type at least 2 characters to search...". Not emitted once the query
    // reaches 2+ characters (SearchRequested takes over at that point).
    void StatusMessageChanged(const QString& message);

    // Emitted when Up or Down is pressed while this widget has focus, so
    // MainWindow can route focus to its results list.
    void NavigateResultsRequested(Qt::Key key);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void OnTextChanged(const QString& text);
    void OnDebounceTimeout();

    QTimer* debounceTimer_;
    QString pendingQuery_;
};

}  // namespace indexed
