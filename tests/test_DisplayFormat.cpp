#include <gtest/gtest.h>

#include "ui/DisplayFormat.h"

using indexed::FormatDateTime;
using indexed::FormatFileSize;

TEST(DisplayFormat, BytesBelowKiloShowPlainInteger) {
    EXPECT_EQ(FormatFileSize(0), "0 B");
    EXPECT_EQ(FormatFileSize(1), "1 B");
    EXPECT_EQ(FormatFileSize(1023), "1023 B");
}

TEST(DisplayFormat, KilobytesShowTwoDecimals) {
    EXPECT_EQ(FormatFileSize(1024), "1.00 KB");
    EXPECT_EQ(FormatFileSize(1536), "1.50 KB");
    EXPECT_EQ(FormatFileSize(1024 * 1024 - 1), "1024.00 KB");
}

TEST(DisplayFormat, MegabytesShowTwoDecimals) {
    EXPECT_EQ(FormatFileSize(1024ULL * 1024), "1.00 MB");
    EXPECT_EQ(FormatFileSize(1024ULL * 1024 * 1024 - 1), "1024.00 MB");
}

TEST(DisplayFormat, GigabytesShowTwoDecimals) {
    EXPECT_EQ(FormatFileSize(1024ULL * 1024 * 1024), "1.00 GB");
    EXPECT_EQ(FormatFileSize(5ULL * 1024 * 1024 * 1024), "5.00 GB");
}

TEST(DisplayFormat, DateTimeFormatsAsYyyyMmDdHhMm) {
    // 2024-01-15 12:34:56 UTC = 1705321696s since epoch. FormatDateTime
    // renders in local time, so only check shape + that it round-trips
    // through the same local conversion used to build the fixture.
    const time_t seconds = 1705321696;
    struct tm localTm{};
    localtime_r(&seconds, &localTm);
    char expected[32];
    strftime(expected, sizeof(expected), "%Y-%m-%d %H:%M", &localTm);

    const uint64_t ns = static_cast<uint64_t>(seconds) * 1'000'000'000ULL;
    EXPECT_EQ(FormatDateTime(ns), std::string(expected));
}

TEST(DisplayFormat, DateTimeIgnoresSubMinuteNanoseconds) {
    const time_t seconds = 1705321696;
    const uint64_t ns = static_cast<uint64_t>(seconds) * 1'000'000'000ULL + 999'999'999ULL;
    EXPECT_EQ(FormatDateTime(ns),
              FormatDateTime(static_cast<uint64_t>(seconds) * 1'000'000'000ULL));
}
