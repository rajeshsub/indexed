#include "ui/SearchCoordinator.h"

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

    if (pendingQuery_.size() < 2) {
        if (!pendingQuery_.isEmpty()) {
            emit QueryTooShort();
        }
        return;
    }

    auto token = std::make_shared<std::atomic<bool>>(false);
    cancelToken_ = token;
    searching_.store(true);

    const std::string query = pendingQuery_.toStdString();
    const SearchOptions options = options_;

    worker_ = std::thread([this, query, options, token]() {
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
        searching_.store(false);
        if (!token->load()) {
            // Queued connection: the receiver lives on the GUI thread.
            emit ResultsReady(std::move(entries), capped);
        }
    });
}

void SearchCoordinator::CancelRunningSearch() {
    if (cancelToken_) {
        cancelToken_->store(true);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    searching_.store(false);
}

}  // namespace indexed
