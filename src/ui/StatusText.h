#pragma once

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

}  // namespace indexed
