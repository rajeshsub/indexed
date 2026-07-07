#pragma once

#include "indexer/Indexer.h"
#include <optional>
#include <string>
#include <string_view>

namespace indexed {

// Wire format for the helper<->GUI status file (indexed-plan.md §9.3): the
// helper periodically rewrites this file; the GUI watches it via inotify to
// surface scan progress without a request/response channel. Hand-rolled
// key=value-per-line text (same style as IniFile) rather than a binary
// format -- this file is small, infrequently parsed, and human-inspectable
// during development is a real asset here.
std::string SerializeStatus(const IndexerStatus& status);

// Returns nullopt on any malformed input (missing/unparsable fields) rather
// than throwing -- the GUI must tolerate reading the file mid-write by the
// helper (a torn read), where nullopt just means "try again next change".
std::optional<IndexerStatus> ParseStatus(std::string_view text);

}  // namespace indexed
