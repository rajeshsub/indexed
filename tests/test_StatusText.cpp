#include <gtest/gtest.h>

#include "settings/PathUtils.h"
#include "ui/StatusText.h"

using indexed::IndexSummaryText;
using indexed::ResultCountText;

TEST(StatusText, ResultCountFormatsWithThousandsSeparators) {
    EXPECT_EQ(ResultCountText(384, false), "384 result(s)");
    EXPECT_EQ(ResultCountText(1234, false), "1,234 result(s)");
    EXPECT_EQ(ResultCountText(0, false), "0 result(s)");
}

TEST(StatusText, CappedResultCountAppendsRefineMessage) {
    EXPECT_EQ(ResultCountText(10000, true),
              "10,000 result(s) \xE2\x80\x94 showing first 10,000. Refine your search\xE2\x80\xA6");
}

TEST(StatusText, IndexSummaryJoinsCountLocationsAndAge) {
    const std::string summary = IndexSummaryText(1234567, {"/", "/home"}, 2 * 3600);
    EXPECT_EQ(summary, indexed::FormatFileCount(1234567) + " files | " +
                           indexed::FormatLocationList({"/", "/home"}) + " | " +
                           indexed::FormatAge(2 * 3600));
}

TEST(StatusText, IndexSummaryWithNoLocations) {
    const std::string summary = IndexSummaryText(0, {}, 0);
    EXPECT_EQ(summary, indexed::FormatFileCount(0) + " files | " + indexed::FormatLocationList({}) +
                           " | " + indexed::FormatAge(0));
}
