#include "ui/SearchLineEdit.h"

#include <QKeyEvent>
#include <QTimer>

namespace {

const QString kEnterSearchTermStatus = QStringLiteral("Enter a search term to begin.");
const QString kTypeAtLeastTwoStatus = QStringLiteral("Type at least 2 characters to search…");
constexpr int kDebounceMs = 150;
constexpr int kMinQueryLength = 2;

}  // namespace

namespace indexed {

SearchLineEdit::SearchLineEdit(QWidget* parent)
    : QLineEdit(parent), debounceTimer_(new QTimer(this)) {
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(kDebounceMs);

    connect(debounceTimer_, &QTimer::timeout, this, &SearchLineEdit::OnDebounceTimeout);
    connect(this, &QLineEdit::textChanged, this, &SearchLineEdit::OnTextChanged);

    // The visible QLineEdit placeholder always reflects the empty-box
    // message; StatusMessageChanged additionally surfaces the same text (and
    // the "type at least 2 characters" text) for a status bar to display.
    setPlaceholderText(kEnterSearchTermStatus);
}

void SearchLineEdit::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
        emit NavigateResultsRequested(static_cast<Qt::Key>(event->key()));
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void SearchLineEdit::OnTextChanged(const QString& text) {
    debounceTimer_->stop();

    if (text.length() < kMinQueryLength) {
        emit StatusMessageChanged(text.isEmpty() ? kEnterSearchTermStatus : kTypeAtLeastTwoStatus);
        // Only a >= 2 char query can have produced visible results; signal a
        // clear exactly when dropping back below that threshold, not on
        // every keystroke of a still-too-short query.
        if (hadSearchableQuery_) {
            hadSearchableQuery_ = false;
            emit SearchCleared();
        }
        return;
    }

    hadSearchableQuery_ = true;
    pendingQuery_ = text;
    debounceTimer_->start();
}

void SearchLineEdit::OnDebounceTimeout() {
    emit SearchRequested(pendingQuery_);
}

}  // namespace indexed
