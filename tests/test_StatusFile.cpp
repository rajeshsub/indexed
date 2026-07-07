#include <gtest/gtest.h>

#include "indexer/StatusFile.h"

using indexed::IndexerState;
using indexed::IndexerStatus;
using indexed::ParseStatus;
using indexed::SerializeStatus;

TEST(StatusFile, RoundTripsAllFields) {
    IndexerStatus status;
    status.state = IndexerState::Scanning;
    status.message = "Scanning /home/user/docs";
    status.filesIndexed = 12345;
    status.locations = {"/", "/home"};
    status.indexAgeSeconds = 7200;

    const std::string text = SerializeStatus(status);
    const std::optional<IndexerStatus> parsed = ParseStatus(text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->state, IndexerState::Scanning);
    EXPECT_EQ(parsed->message, "Scanning /home/user/docs");
    EXPECT_EQ(parsed->filesIndexed, 12345u);
    EXPECT_EQ(parsed->locations, (std::vector<std::string>{"/", "/home"}));
    EXPECT_EQ(parsed->indexAgeSeconds, 7200u);
}

TEST(StatusFile, RoundTripsEachState) {
    for (IndexerState state :
         {IndexerState::Idle, IndexerState::Scanning, IndexerState::LoadingIndex,
          IndexerState::WatchingForChanges, IndexerState::Error}) {
        IndexerStatus status;
        status.state = state;
        const std::optional<IndexerStatus> parsed = ParseStatus(SerializeStatus(status));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->state, state);
    }
}

TEST(StatusFile, MessageContainingNewlineIsPreservedOnRoundTrip) {
    IndexerStatus status;
    status.message = "line one\nline two";
    const std::optional<IndexerStatus> parsed = ParseStatus(SerializeStatus(status));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->message, "line one\nline two");
}

TEST(StatusFile, EmptyLocationsRoundTripsAsEmpty) {
    IndexerStatus status;
    status.locations = {};
    const std::optional<IndexerStatus> parsed = ParseStatus(SerializeStatus(status));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->locations.empty());
}

TEST(StatusFile, EmptyInputFailsToParse) {
    EXPECT_FALSE(ParseStatus("").has_value());
}

TEST(StatusFile, GarbageInputFailsToParse) {
    EXPECT_FALSE(ParseStatus("not a status file\nrandom garbage").has_value());
}

TEST(StatusFile, TruncatedMidWriteInputFailsToParse) {
    const std::string full = SerializeStatus(IndexerStatus{});
    // Simulate the GUI reading the file while the helper is mid-rewrite: a
    // torn read that cuts off before the closing fields.
    const std::string torn = full.substr(0, full.size() / 2);
    EXPECT_FALSE(ParseStatus(torn).has_value());
}

TEST(StatusFile, UnknownStateNameFailsToParse) {
    EXPECT_FALSE(ParseStatus("State=NotARealState\n").has_value());
}
