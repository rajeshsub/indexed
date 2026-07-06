#include "ui/DisplayEntry.h"

#include "ui/DisplayFormat.h"

namespace indexed {

namespace {

std::string ParentDirOf(std::string_view path) {
    const size_t lastSlash = path.find_last_of('/');
    if (lastSlash == std::string_view::npos) {
        return "";
    }
    if (lastSlash == 0) {
        return "/";
    }
    return std::string(path.substr(0, lastSlash));
}

}  // namespace

std::vector<DisplayEntry> BuildDisplayEntries(const IndexPool& pool,
                                              const std::vector<SearchResult>& results) {
    std::vector<DisplayEntry> rows;
    rows.reserve(results.size());
    for (size_t i = 0; i < results.size(); ++i) {
        const IndexPool::EntryView view = pool.GetEntry(results[i].entryIndex);
        DisplayEntry entry;
        entry.name = std::string(view.name);
        entry.parentDir = ParentDirOf(view.path);
        entry.sizeBytes = view.size;
        entry.lastModifiedNs = view.lastModified;
        entry.sizeText = FormatFileSize(view.size);
        entry.dateText = FormatDateTime(view.lastModified);
        entry.sourceIndex = i;
        rows.push_back(std::move(entry));
    }
    return rows;
}

}  // namespace indexed
