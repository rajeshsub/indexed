#include "search/SearchEngine.h"

#include <re2/re2.h>
#include <utf8proc.h>

#include "search/SimdSearch.h"
#include "search/TokenMatcher.h"
#include <cstdlib>
#include <memory>

namespace indexed {

namespace {

bool IsSeparator(char c) {
    return c == ' ' || c == '_' || c == '-' || c == '.';
}

bool ContainsSeparator(std::string_view text) {
    return text.find_first_of(" _-.") != std::string_view::npos;
}

// Decomposes text (NFKD-like) and strips combining marks (accents,
// umlauts, etc.) via utf8proc, optionally case-folding in the same pass.
// Falls back to ASCII case-fold (or the original text, if caseSensitive) on
// any utf8proc failure, so malformed input never crashes the search.
std::string FoldDiacritics(std::string_view text, bool caseSensitive) {
    int options = UTF8PROC_STABLE | UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT | UTF8PROC_STRIPMARK;
    if (!caseSensitive) {
        options |= UTF8PROC_CASEFOLD;
    }

    utf8proc_uint8_t* dest = nullptr;
    utf8proc_ssize_t len = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
                                        static_cast<utf8proc_ssize_t>(text.size()), &dest,
                                        static_cast<utf8proc_option_t>(options));
    if (len < 0 || dest == nullptr) {
        return caseSensitive ? std::string(text) : CaseFoldAscii(text);
    }
    std::string result(reinterpret_cast<char*>(dest), static_cast<size_t>(len));
    free(dest);  // NOLINT(cppcoreguidelines-owning-memory) - utf8proc_map allocates via malloc
    return result;
}

// Returns the text to search for this entry (path or name per options),
// case-folded/diacritics-folded per options. `scratch` owns any on-the-fly
// computed text; the returned view may instead point directly into the
// pool's own storage (zero-copy) when a precomputed or unmodified form
// already satisfies the request.
std::string_view GetSearchTarget(const IndexPool::EntryView& entry, const SearchOptions& options,
                                 std::string& scratch) {
    if (options.matchPath) {
        if (options.ignoreDiacritics) {
            scratch = FoldDiacritics(entry.path, options.caseSensitive);
            return scratch;
        }
        if (options.caseSensitive) {
            return entry.path;
        }
        scratch = CaseFoldAscii(entry.path);
        return scratch;
    }
    if (options.ignoreDiacritics) {
        scratch = FoldDiacritics(entry.name, options.caseSensitive);
        return scratch;
    }
    if (options.caseSensitive) {
        return entry.name;
    }
    return entry.nameLower;
}

std::string GetQueryText(std::string_view query, const SearchOptions& options) {
    if (options.ignoreDiacritics) {
        return FoldDiacritics(query, options.caseSensitive);
    }
    return options.caseSensitive ? std::string(query) : CaseFoldAscii(query);
}

bool MatchesWholeWord(std::string_view subject, size_t matchStart, size_t matchLen) {
    bool leftOk = matchStart == 0 || IsSeparator(subject[matchStart - 1]);
    size_t matchEnd = matchStart + matchLen;
    bool rightOk = matchEnd == subject.size() || IsSeparator(subject[matchEnd]);
    return leftOk && rightOk;
}

}  // namespace

std::vector<SearchResult> SearchEngine::Search(const IndexPool& pool, std::string_view query,
                                               const SearchOptions& options,
                                               const std::atomic<bool>& cancelToken) {
    std::vector<SearchResult> results;

    // Regex mode never lowercases the query (that would corrupt character
    // classes/quantifiers) - RE2's own case-insensitivity option is used
    // instead, matched against unfolded (never nameLower/CaseFoldAscii'd)
    // text. ignoreDiacritics is intentionally not applied in regex mode: a
    // pattern's literal characters can't be safely diacritics-folded without
    // the same corruption risk (v0.1.0 scope decision, not a bug). The whole
    // user pattern is wrapped in one capturing group so PartialMatch's Arg
    // mechanism (which binds to numbered groups, not the overall match) can
    // still recover the overall match span for highlight purposes.
    std::unique_ptr<RE2> regex;
    if (options.useRegex) {
        RE2::Options re2Options;
        re2Options.set_case_sensitive(options.caseSensitive);
        std::string inner =
            options.wholeWord ? "\\b(?:" + std::string(query) + ")\\b" : std::string(query);
        regex = std::make_unique<RE2>("(" + inner + ")", re2Options);
        if (!regex->ok()) {
            return results;
        }
    }

    const bool tokenMode = !options.useRegex && ContainsSeparator(query);
    std::vector<std::string_view> queryTokens;
    std::string queryText;
    if (!options.useRegex) {
        queryText = GetQueryText(query, options);
        if (tokenMode) {
            queryTokens = Tokenize(queryText);
        }
    }

    for (size_t i = 0; i < pool.Count(); ++i) {
        if (cancelToken.load(std::memory_order_relaxed)) {
            break;
        }
        if (pool.IsDeleted(i)) {
            continue;
        }

        IndexPool::EntryView entry = pool.GetEntry(i);

        if (options.useRegex) {
            std::string_view subject = options.matchPath ? entry.path : entry.name;
            absl::string_view piece(subject.data(), subject.size());
            absl::string_view match;
            if (RE2::PartialMatch(piece, *regex, &match)) {
                SearchResult result;
                result.entryIndex = i;
                result.matchStart = static_cast<size_t>(match.data() - subject.data());
                result.matchLen = match.size();
                results.push_back(result);
            }
        } else {
            std::string scratch;
            std::string_view subject = GetSearchTarget(entry, options, scratch);

            if (tokenMode) {
                std::vector<std::string_view> nameTokens = Tokenize(subject);
                if (MatchesAllTokens(queryTokens, nameTokens)) {
                    SearchResult result;
                    result.entryIndex = i;
                    if (!queryTokens.empty()) {
                        size_t pos = FindSubstring(subject, queryTokens.front());
                        if (pos != std::string_view::npos) {
                            result.matchStart = pos;
                            result.matchLen = queryTokens.front().size();
                        }
                    }
                    results.push_back(result);
                }
            } else {
                size_t pos = FindSubstring(subject, queryText);
                if (pos != std::string_view::npos &&
                    (!options.wholeWord || MatchesWholeWord(subject, pos, queryText.size()))) {
                    SearchResult result;
                    result.entryIndex = i;
                    result.matchStart = pos;
                    result.matchLen = queryText.size();
                    results.push_back(result);
                }
            }
        }

        if (results.size() >= kMaxSearchResults) {
            break;
        }
    }

    return results;
}

}  // namespace indexed
