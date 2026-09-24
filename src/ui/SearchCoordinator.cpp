#include "ui/SearchCoordinator.h"

#include <algorithm>
#include <shared_mutex>
#include <utility>

namespace indexed {

namespace {
// qRegisterMetaType must run once before the first queued emission of the
// signal payload; a namespace-scope initializer keeps it out of every
// constructor call.
const int kDisplayEntryVectorMetaType = qRegisterMetaType<std::vector<DisplayEntry>>();
}  // namespace

SearchCoordinator::SearchCoordinator(ISearchEngine& engine, IndexStore& store, int debounceMs,
                                     QObject* parent)
    : QObject(parent), engine_(engine), store_(store) {
    Q_UNUSED(kDisplayEntryVectorMetaType);
    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(debounceMs);
    connect(&debounceTimer_, &QTimer::timeout, this, &SearchCoordinator::StartSearch);
}

SearchCoordinator::~SearchCoordinator() {
    CancelRunningSearch();
    for (SearchWorker& worker : workers_) {
        worker.thread.join();
    }
}

void SearchCoordinator::SetQuery(const QString& query) {
    pendingQuery_ = query;
    debounceTimer_.start();
}

SearchOptions SearchCoordinator::Options() const {
    return options_;
}

void SearchCoordinator::SetOptions(const SearchOptions& options) {
    options_ = options;
}

bool SearchCoordinator::IsSearching() const {
    return searching_.load();
}

void SearchCoordinator::StartSearch() {
    CancelRunningSearch();
    ReapFinishedWorkers();

    if (pendingQuery_.size() < 2) {
        if (!pendingQuery_.isEmpty()) {
            emit QueryTooShort();
        }
        return;
    }

    auto token = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<bool>>(false);
    cancelToken_ = token;
    searching_.store(true);

    const std::string query = pendingQuery_.toStdString();
    const SearchOptions options = options_;

    std::thread thread([this, query, options, token, done]() {
        std::vector<DisplayEntry> entries;
        bool capped = false;
        {
            std::shared_lock lock(store_.GetSearchMutex());
            const IndexPool& pool = store_.GetPool();
            const std::vector<SearchResult> results = engine_.Search(pool, query, options, *token);
            capped = results.size() >= kMaxSearchResults;
            if (!token->load()) {
                entries = BuildDisplayEntries(pool, results);
            }
        }
        if (!token->load()) {
            // The cancel check is repeated on the GUI thread, where cancels
            // happen, so a search superseded while its results were in
            // flight never replaces the newer one's.
            QMetaObject::invokeMethod(
                this,
                [this, token, entries = std::move(entries), capped]() mutable {
                    if (!token->load()) {
                        searching_.store(false);
                        emit ResultsReady(std::move(entries), capped);
                    }
                },
                Qt::QueuedConnection);
        }
        done->store(true);
    });
    workers_.push_back(SearchWorker{std::move(thread), done});
}

void SearchCoordinator::CancelRunningSearch() {
    // Never joins: a superseded search may take a while to notice its cancel
    // (e.g. waiting for the store's lock), and the GUI thread must not wait
    // for it (docs/adr/0014). It finishes on its own and is reaped later.
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    searching_.store(false);
}

void SearchCoordinator::ReapFinishedWorkers() {
    auto finished = std::partition(workers_.begin(), workers_.end(),
                                   [](const SearchWorker& worker) { return !worker.done->load(); });
    for (auto it = finished; it != workers_.end(); ++it) {
        it->thread.join();  // already done: returns immediately
    }
    workers_.erase(finished, workers_.end());
}

}  // namespace indexed
