#pragma once

#include <cstdint>
#include <string>

namespace indexed {

// "B / KB / MB / GB", 2 decimals at KB and above, plain integer for B
// (indexed-plan.md §19).
std::string FormatFileSize(uint64_t bytes);

// "YYYY-MM-DD HH:MM" in local time (indexed-plan.md §19). nsSinceEpoch is
// nanoseconds since the Unix epoch, matching FileEntry::lastModified.
std::string FormatDateTime(uint64_t nsSinceEpoch);

}  // namespace indexed
