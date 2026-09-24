#include "settings/RootsDiff.h"

#include <unordered_set>

namespace indexed {

bool RootsDiff::bothChanged() const {
    return !added.empty() && !removed.empty();
}

RootsDiff DiffRoots(const std::vector<std::string>& oldRoots,
                    const std::vector<std::string>& newRoots) {
    const std::unordered_set<std::string> oldSet(oldRoots.begin(), oldRoots.end());
    const std::unordered_set<std::string> newSet(newRoots.begin(), newRoots.end());

    RootsDiff diff;
    for (const auto& root : newRoots) {
        if (oldSet.find(root) == oldSet.end()) {
            diff.added.push_back(root);
        }
    }
    for (const auto& root : oldRoots) {
        if (newSet.find(root) == newSet.end()) {
            diff.removed.push_back(root);
        }
    }
    return diff;
}

}  // namespace indexed
