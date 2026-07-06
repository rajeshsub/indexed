#include "ui/StatusText.h"

#include "settings/PathUtils.h"

namespace indexed {

std::string ResultCountText(size_t count, bool capped) {
    std::string text = FormatFileCount(count) + " result(s)";
    if (capped) {
        text += " — showing first 10,000. Refine your search…";
    }
    return text;
}

std::string IndexSummaryText(uint64_t fileCount, const std::vector<std::string>& locations,
                             uint64_t ageSeconds) {
    return FormatFileCount(fileCount) + " files | " + FormatLocationList(locations) + " | " +
           FormatAge(ageSeconds);
}

}  // namespace indexed
