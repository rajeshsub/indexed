#pragma once

#include "search/ISearchEngine.h"
#include <cstdint>
#include <string>
#include <vector>

namespace indexed {

// Status-bar message builders (indexed-plan.md §19), Qt-free for plain gtest.

// "384 result(s)", or the refine-your-search cap message when capped is true.
std::string ResultCountText(size_t count, bool capped);

// "1,234,567 files | / , /home | 2 hrs old" -- the idle-with-index summary.
// locations are the selected roots; ageSeconds feeds FormatAge.
std::string IndexSummaryText(uint64_t fileCount, const std::vector<std::string>& locations,
                             uint64_t ageSeconds);

// "Regex: ON | Case: OFF | Whole Word: OFF | Match Path: OFF | Diacritics: OFF"
// -- always all five toggles, in Search-menu order, so the active search
// mode is visible without opening the menu (indexed-plan.md §19 follow-up:
// a regex that fails to compile returns zero results with no other signal,
// so seeing "Regex: ON" at a glance is the difference between "no matches"
// and "my pattern is broken").
std::string SearchOptionsText(const SearchOptions& options);

}  // namespace indexed
