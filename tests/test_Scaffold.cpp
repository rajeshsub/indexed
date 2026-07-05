// M0 toolchain smoke tests: proves the FetchContent dependencies (re2, utf8proc) that
// core/search and core/settings will depend on from M2/M3 onward actually link and work,
// before any real module code is written.
#include <gtest/gtest.h>
#include <re2/re2.h>
#include <utf8proc.h>

#include "Version.h"

TEST(Version, MatchesProjectVersion) {
    EXPECT_STREQ(indexed::GetVersionString(), "0.1.0");
}

TEST(ToolchainSmoke, Re2PartialMatchFindsSubstringPattern) {
    EXPECT_TRUE(RE2::PartialMatch("hello world", "wor.d"));
    EXPECT_FALSE(RE2::PartialMatch("hello world", "^wor.d$"));
}

TEST(ToolchainSmoke, Utf8procLowercasesAsciiCodepoint) {
    EXPECT_EQ(utf8proc_tolower(static_cast<utf8proc_int32_t>('A')),
              static_cast<utf8proc_int32_t>('a'));
    EXPECT_EQ(utf8proc_tolower(static_cast<utf8proc_int32_t>('z')),
              static_cast<utf8proc_int32_t>('z'));
}
