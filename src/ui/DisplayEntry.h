#pragma once

#include "search/ISearchEngine.h"
#include "storage/IndexPool.h"
#include <cstdint>
#include <string>
#include <vector>

namespace indexed {

// One row in the results list (indexed-plan.md §19): a formatted, GUI-ready
// snapshot of a single SearchResult joined against its IndexPool entry.
// Qt-free so it's usable from ResultModel/ResultView without pulling Qt into
// this header, and so BuildDisplayEntries is unit-testable without a
// QApplication.
struct DisplayEntry {
    std::string name;             // basename
    std::string parentDir;        // directory containing the file (Path column
                                  // shows this, not the full path -- winindex
                                  // strips the basename for that column)
    std::string sizeText;         // formatted via FormatFileSize
    std::string dateText;         // formatted via FormatDateTime
    uint64_t sizeBytes = 0;       // raw value, for numeric (not lexical) sort
    uint64_t lastModifiedNs = 0;  // raw value, for numeric (not lexical) sort
    size_t sourceIndex = 0;       // index into the SearchResult vector this
                                  // row was built from, so actions (open,
                                  // reveal, copy, etc.) can map a row back to
                                  // its IndexPool entry.
};

// Joins each result against pool to produce one DisplayEntry per result, in
// the same order as results.
std::vector<DisplayEntry> BuildDisplayEntries(const IndexPool& pool,
                                              const std::vector<SearchResult>& results);

}  // namespace indexed
