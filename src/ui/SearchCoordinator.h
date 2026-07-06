#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include "search/ISearchEngine.h"
#include "storage/IndexStore.h"
#include "ui/DisplayEntry.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace indexed {

// Owns the search-as-you-type pipeline (indexed-plan.md §19): 150 ms
// debounce, minimum-2-characters gate, one background search thread at a
// time (starting a new search cancels the previous via its cancelToken),
// results posted back to the owning (GUI) thread as a ready-to-display
// DisplayEntry snapshot.
//
// Depends on ISearchEngine + IndexStore only; the engine is injected so
// tests drive the pipeline with a stub engine and no real index.
class SearchCoordinator : public QObject {
    Q_OBJECT

public:
    // debounceMs is parameterized for tests (150 ms in production per §19).
    SearchCoordinator(ISearchEngine& engine, IndexStore& store, int debounceMs = 150,
                      QObject* parent = nullptr);
    ~SearchCoordinator() override;

    // Call on every keystroke. Starts/restarts the debounce timer. A query
    // under 2 characters cancels any pending/running search and emits
    // QueryTooShort instead of searching.
    void SetQuery(const QString& query);

    SearchOptions Options() const;
    void SetOptions(const SearchOptions& options);

    // True while a background search thread is running.
    bool IsSearching() const;

signals:
    // Emitted on the GUI thread when a search completes. capped is true when
    // the engine hit kMaxSearchResults.
    void ResultsReady(std::vector<indexed::DisplayEntry> entries, bool capped);
    // Emitted when the query is non-empty but under the 2-char minimum.
    void QueryTooShort();

private:
    void StartSearch();
    void CancelRunningSearch();

    ISearchEngine& engine_;
    IndexStore& store_;
    QTimer debounceTimer_;
    QString pendingQuery_;
    SearchOptions options_;

    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancelToken_;
    std::atomic<bool> searching_{false};
};

}  // namespace indexed

// The signal payload crosses threads via queued connections in production;
// register the type once (done in SearchCoordinator.cpp) so QMetaType can
// marshal it.
Q_DECLARE_METATYPE(std::vector<indexed::DisplayEntry>)
