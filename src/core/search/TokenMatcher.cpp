#include "search/TokenMatcher.h"

#include <algorithm>

namespace {

bool IsSeparator(char c) {
    return c == ' ' || c == '_' || c == '-' || c == '.';
}

}  // namespace

namespace indexed {

std::vector<std::string_view> Tokenize(std::string_view text) {
    std::vector<std::string_view> tokens;

    size_t tokenStart = 0;
    bool inToken = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (IsSeparator(text[i])) {
            if (inToken) {
                tokens.push_back(text.substr(tokenStart, i - tokenStart));
                inToken = false;
            }
        } else if (!inToken) {
            tokenStart = i;
            inToken = true;
        }
    }
    if (inToken) {
        tokens.push_back(text.substr(tokenStart, text.size() - tokenStart));
    }

    return tokens;
}

bool MatchesAllTokens(const std::vector<std::string_view>& queryTokens,
                      const std::vector<std::string_view>& nameTokens) {
    return std::all_of(
        queryTokens.begin(), queryTokens.end(), [&nameTokens](std::string_view queryToken) {
            return std::any_of(nameTokens.begin(), nameTokens.end(),
                               [queryToken](std::string_view nameToken) {
                                   return nameToken.find(queryToken) != std::string_view::npos;
                               });
        });
}

}  // namespace indexed
