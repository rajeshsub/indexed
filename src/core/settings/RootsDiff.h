#pragma once

#include <string>
#include <vector>

namespace indexed {

// Diff between the roots before and after a Settings change. Pure data, so
// the caller (IndexService) can decide incremental IndexPaths/RemovePaths
// vs. a full rebuild (indexed-plan.md §19: "On OK, diff old vs new roots").
struct RootsDiff {
    std::vector<std::string> added;    // in newRoots, not in oldRoots
    std::vector<std::string> removed;  // in oldRoots, not in newRoots

    // True when both added and removed are non-empty -- caller's cue that an
    // incremental update isn't enough and a full rebuild is warranted.
    bool bothChanged() const;
};

RootsDiff DiffRoots(const std::vector<std::string>& oldRoots,
                    const std::vector<std::string>& newRoots);

}  // namespace indexed
