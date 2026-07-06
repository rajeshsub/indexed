#pragma once

#include "search/ISearchEngine.h"

namespace indexed {

// Concrete ISearchEngine: substring (SIMD-accelerated), token-set, and RE2
// regex modes, with caseSensitive/wholeWord/matchPath/ignoreDiacritics
// options. See indexed-plan.md §7.4.
class SearchEngine : public ISearchEngine {
public:
    std::vector<SearchResult> Search(const IndexPool& pool, std::string_view query,
                                     const SearchOptions& options,
                                     const std::atomic<bool>& cancelToken) override;
};

}  // namespace indexed
