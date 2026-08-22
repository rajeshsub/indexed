#include "storage/IndexPool.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace indexed {

bool operator==(const TokenSpan& a, const TokenSpan& b) {
    return a.offset == b.offset && a.length == b.length;
}

namespace {
bool IsSeparator(char c) {
    return c == ' ' || c == '_' || c == '-' || c == '.';
}
}  // namespace

std::vector<TokenSpan> TokenizeToSpans(std::string_view text) {
    std::vector<TokenSpan> spans;

    size_t tokenStart = 0;
    bool inToken = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (IsSeparator(text[i])) {
            if (inToken) {
                spans.push_back(TokenSpan{static_cast<uint32_t>(tokenStart),
                                          static_cast<uint32_t>(i - tokenStart)});
                inToken = false;
            }
        } else if (!inToken) {
            tokenStart = i;
            inToken = true;
        }
    }
    if (inToken) {
        spans.push_back(TokenSpan{static_cast<uint32_t>(tokenStart),
                                  static_cast<uint32_t>(text.size() - tokenStart)});
    }

    return spans;
}

void ApplyTokenSpans(std::string_view text, std::span<const TokenSpan> spans,
                     std::vector<std::string_view>& out) {
    out.clear();
    out.reserve(spans.size());
    std::transform(spans.begin(), spans.end(), std::back_inserter(out),
                   [text](const TokenSpan& span) { return text.substr(span.offset, span.length); });
}

std::string CaseFoldAscii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return result;
}

void IndexPool::AddEntry(const FileEntry& entry) {
    EntryMeta meta;
    meta.size = entry.size;
    meta.lastModified = entry.lastModified;
    meta.attributes = entry.attributes;

    meta.pathOffset = pathPool_.size();
    meta.pathLen = static_cast<uint32_t>(entry.path.size());
    pathPool_.insert(pathPool_.end(), entry.path.begin(), entry.path.end());

    // nameStart is the basename's offset within the path, e.g. path
    // "/a/b/c.txt" with name "c.txt" -> nameStart = 5.
    meta.nameStart = static_cast<uint16_t>(entry.path.size() - entry.name.size());

    std::string nameLower = CaseFoldAscii(entry.name);
    meta.nameLowerOffset = nameLowerPool_.size();
    meta.nameLowerLen = static_cast<uint32_t>(nameLower.size());
    nameLowerPool_.insert(nameLowerPool_.end(), nameLower.begin(), nameLower.end());

    std::vector<TokenSpan> spans = TokenizeToSpans(nameLower);
    nameTokenStart_.push_back(static_cast<uint32_t>(nameTokenSpans_.size()));
    // A name can't have more separator-delimited tokens than it has bytes;
    // uint16_t is the same "a filename component is bounded" assumption
    // ADR-0006 already makes for nameStart.
    nameTokenCount_.push_back(static_cast<uint16_t>(spans.size()));
    nameTokenSpans_.insert(nameTokenSpans_.end(), spans.begin(), spans.end());

    meta_.push_back(meta);
}

size_t IndexPool::Count() const {
    return meta_.size();
}

IndexPool::EntryView IndexPool::GetEntry(size_t index) const {
    const EntryMeta& meta = meta_.at(index);
    EntryView view;
    view.path = std::string_view(pathPool_.data() + meta.pathOffset, meta.pathLen);
    view.name = view.path.substr(meta.nameStart);
    view.nameLower =
        std::string_view(nameLowerPool_.data() + meta.nameLowerOffset, meta.nameLowerLen);
    view.size = meta.size;
    view.lastModified = meta.lastModified;
    view.attributes = meta.attributes;
    view.nameTokenSpans = std::span<const TokenSpan>(
        nameTokenSpans_.data() + nameTokenStart_[index], nameTokenCount_[index]);
    return view;
}

void IndexPool::MarkDeleted(size_t index) {
    meta_.at(index).deleted = 1;
}

bool IndexPool::IsDeleted(size_t index) const {
    return meta_.at(index).deleted != 0;
}

std::optional<size_t> IndexPool::FindByPath(std::string_view path) const {
    for (size_t i = 0; i < meta_.size(); ++i) {
        if (IsDeleted(i)) {
            continue;
        }
        if (GetEntry(i).path == path) {
            return i;
        }
    }
    return std::nullopt;
}

const std::vector<EntryMeta>& IndexPool::Meta() const {
    return meta_;
}

const std::vector<char>& IndexPool::PathPool() const {
    return pathPool_;
}

const std::vector<char>& IndexPool::NameLowerPool() const {
    return nameLowerPool_;
}

IndexPool IndexPool::LoadFromPathPool(std::vector<EntryMeta> meta, std::vector<char> pathPool) {
    IndexPool pool;
    pool.pathPool_ = std::move(pathPool);
    pool.meta_ = std::move(meta);

    // nameLower is never persisted (docs/adr/0003-binary-index-format.md); rebuild it
    // from the path pool + nameStart/pathLen, and fix up each entry's nameLower
    // offset/length to point into the freshly rebuilt pool.
    pool.nameLowerPool_.clear();
    for (EntryMeta& m : pool.meta_) {
        std::string_view path(pool.pathPool_.data() + m.pathOffset, m.pathLen);
        std::string_view name = path.substr(m.nameStart);
        std::string nameLower = CaseFoldAscii(name);

        m.nameLowerOffset = pool.nameLowerPool_.size();
        m.nameLowerLen = static_cast<uint32_t>(nameLower.size());
        pool.nameLowerPool_.insert(pool.nameLowerPool_.end(), nameLower.begin(), nameLower.end());

        std::vector<TokenSpan> spans = TokenizeToSpans(nameLower);
        pool.nameTokenStart_.push_back(static_cast<uint32_t>(pool.nameTokenSpans_.size()));
        pool.nameTokenCount_.push_back(static_cast<uint16_t>(spans.size()));
        pool.nameTokenSpans_.insert(pool.nameTokenSpans_.end(), spans.begin(), spans.end());
    }
    return pool;
}

}  // namespace indexed
