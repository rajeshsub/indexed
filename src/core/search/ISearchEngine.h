#pragma once

#include "storage/IndexPool.h"
#include <atomic>
#include <cstddef>
#include <string_view>
#include <vector>

namespace indexed {

// Mirrors winindex's SearchOptions; see indexed-plan.md §7.4.
struct SearchOptions {
    bool useRegex = false;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool matchPath = false;
    bool ignoreDiacritics = false;
};

// One match. matchStart/matchLen are byte offsets into whichever text was
// searched (path if matchPath, else name) for UI highlight purposes. In
// token-set mode (multi-word query), these locate the first query token's
// match; full multi-region highlight is a future UI concern, not required
// for v0.1.0's SearchResult shape.
struct SearchResult {
    size_t entryIndex = 0;
    size_t matchStart = 0;
    size_t matchLen = 0;
};

// Cooperative cap: Search() stops collecting once this many results are
// found (§7.4 "Caps: max 10 000 results").
inline constexpr size_t kMaxSearchResults = 10000;

class ISearchEngine {
public:
    virtual ~ISearchEngine() = default;

    // Scans pool (skipping deleted entries) for entries matching query under
    // options. Checks cancelToken cooperatively; may return early (partial
    // results) if it becomes true mid-scan.
    virtual std::vector<SearchResult> Search(const IndexPool& pool, std::string_view query,
                                             const SearchOptions& options,
                                             const std::atomic<bool>& cancelToken) = 0;
};

}  // namespace indexed
