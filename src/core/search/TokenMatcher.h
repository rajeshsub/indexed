#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace indexed {

// Splits text on any of the four separator characters: ' ', '_', '-', '.'.
// Consecutive separators (or leading/trailing separators) produce NO empty
// tokens — only non-empty runs of non-separator characters are tokens. Text
// with no separators at all produces a single token equal to the whole text.
// Empty input produces zero tokens.
std::vector<std::string_view> Tokenize(std::string_view text);

// True if every token in queryTokens has an exact string match somewhere in
// nameTokens (order-independent — queryTokens need not appear in the same
// order they occur in nameTokens). Duplicate tokens are NOT special-cased:
// if queryTokens contains "guitar" twice, only ONE occurrence of "guitar" in
// nameTokens is still sufficient (this is a per-token existence check, not a
// multiset/count-based match). An empty queryTokens vector trivially
// matches (returns true) — vacuous truth, no query tokens means nothing is
// required to be present.
bool MatchesAllTokens(const std::vector<std::string_view>& queryTokens,
                      const std::vector<std::string_view>& nameTokens);

}  // namespace indexed
