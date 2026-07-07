#include <gtest/gtest.h>

#include "search/TokenMatcher.h"
#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using indexed::MatchesAllTokens;
using indexed::Tokenize;

namespace {

// Tokenize returns std::string_view tokens borrowing from the input; copy
// them into std::string for easy comparison in tests.
std::vector<std::string> TokenizeToStrings(std::string_view text) {
    std::vector<std::string_view> tokens = Tokenize(text);
    std::vector<std::string> result;
    std::transform(tokens.begin(), tokens.end(), std::back_inserter(result),
                   [](std::string_view token) { return std::string(token); });
    return result;
}

}  // namespace

TEST(TokenMatcher, TokenizeSplitsOnAllFourSeparatorsCombined) {
    EXPECT_EQ(TokenizeToStrings("a_b-c.d e"), std::vector<std::string>({"a", "b", "c", "d", "e"}));
}

TEST(TokenMatcher, TokenizeDropsEmptyTokensFromConsecutiveSeparators) {
    EXPECT_EQ(TokenizeToStrings("__a--b.."), std::vector<std::string>({"a", "b"}));
}

TEST(TokenMatcher, TokenizeDropsEmptyTokensFromLeadingAndTrailingSeparators) {
    EXPECT_EQ(TokenizeToStrings(".a."), std::vector<std::string>({"a"}));
}

TEST(TokenMatcher, TokenizeWithNoSeparatorsReturnsSingleWholeToken) {
    EXPECT_EQ(TokenizeToStrings("guitar"), std::vector<std::string>({"guitar"}));
}

TEST(TokenMatcher, TokenizeEmptyInputReturnsZeroTokens) {
    EXPECT_TRUE(Tokenize("").empty());
}

TEST(TokenMatcher, TokenizeCanonicalPlanExample) {
    EXPECT_EQ(
        TokenizeToStrings("ledzep_just-rosy_june-bug_guitar.flac"),
        std::vector<std::string>({"ledzep", "just", "rosy", "june", "bug", "guitar", "flac"}));
}

TEST(TokenMatcher, MatchesAllTokensCanonicalPlanExampleMatches) {
    std::vector<std::string_view> nameTokens = Tokenize("ledzep_just-rosy_june-bug_guitar.flac");
    std::vector<std::string_view> queryTokens = Tokenize("just rosy guitar");

    EXPECT_TRUE(MatchesAllTokens(queryTokens, nameTokens));
}

TEST(TokenMatcher, MatchesAllTokensFalseWhenOneQueryTokenAbsent) {
    std::vector<std::string_view> nameTokens = Tokenize("ledzep_just-rosy_june-bug_guitar.flac");
    std::vector<std::string_view> queryTokens = Tokenize("just rosy trumpet");

    EXPECT_FALSE(MatchesAllTokens(queryTokens, nameTokens));
}

TEST(TokenMatcher, MatchesAllTokensIsOrderIndependent) {
    std::vector<std::string_view> nameTokens = Tokenize("ledzep_just-rosy_june-bug_guitar.flac");
    std::vector<std::string_view> queryTokens = Tokenize("guitar just rosy");

    EXPECT_TRUE(MatchesAllTokens(queryTokens, nameTokens));
}

TEST(TokenMatcher, MatchesAllTokensEmptyQueryMatchesAnyNameTokens) {
    std::vector<std::string_view> nameTokens = Tokenize("ledzep_just-rosy_june-bug_guitar.flac");
    std::vector<std::string_view> emptyQuery;

    EXPECT_TRUE(MatchesAllTokens(emptyQuery, nameTokens));
}

TEST(TokenMatcher, MatchesAllTokensEmptyQueryAndEmptyNameTokensMatches) {
    std::vector<std::string_view> emptyQuery;
    std::vector<std::string_view> emptyName;

    EXPECT_TRUE(MatchesAllTokens(emptyQuery, emptyName));
}

TEST(TokenMatcher, MatchesAllTokensPartiallyTypedLastTokenMatches) {
    // Search-as-you-type: "just rosy guit" while heading for "guitar" must
    // keep matching (winindex README: every query token "appears somewhere
    // in the filename token set" -- substring, not exact equality).
    std::vector<std::string_view> nameTokens = Tokenize("ledzep_just-rosy_june-bug_guitar.flac");
    std::vector<std::string_view> queryTokens = Tokenize("just rosy guit");

    EXPECT_TRUE(MatchesAllTokens(queryTokens, nameTokens));
}

TEST(TokenMatcher, MatchesAllTokensInnerSubstringOfNameTokenMatches) {
    std::vector<std::string_view> nameTokens = Tokenize("ledzep_just-rosy_june-bug_guitar.flac");
    std::vector<std::string_view> queryTokens = Tokenize("zep guitar");

    EXPECT_TRUE(MatchesAllTokens(queryTokens, nameTokens));
}

TEST(TokenMatcher, MatchesAllTokensSubstringMustNotSpanSeparators) {
    // "epjust" only exists in the name if the '_' separator is ignored;
    // tokens are matched individually, so this must NOT match.
    std::vector<std::string_view> nameTokens = Tokenize("ledzep_just-rosy");
    std::vector<std::string_view> queryTokens = Tokenize("epjust rosy");

    EXPECT_FALSE(MatchesAllTokens(queryTokens, nameTokens));
}

TEST(TokenMatcher, MatchesAllTokensDuplicateQueryTokenMatchesSingleOccurrence) {
    std::vector<std::string_view> nameTokens = Tokenize("just rosy guitar");
    std::vector<std::string_view> queryTokens = Tokenize("guitar guitar");

    EXPECT_TRUE(MatchesAllTokens(queryTokens, nameTokens));
}
