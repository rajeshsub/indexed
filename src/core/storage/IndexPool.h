#pragma once

#include "indexer/IFileSystemScanner.h"
#include "storage/EntryMeta.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace indexed {

// ASCII case-fold: 'A'-'Z' -> 'a'-'z', all other bytes (including UTF-8
// continuation bytes, always >= 0x80) pass through unchanged. Used to build
// IndexPool's nameLowerPool, and reused by SearchEngine (M2) to fold path
// text and query text on the fly for matchPath/case-insensitive search,
// since there is no precomputed pathLower pool (docs/adr/0006 defers it,
// matching winindex's own pathLower deferral).
std::string CaseFoldAscii(std::string_view text);

// Byte offset + length of one token within the text it was tokenized from.
// Offsets/lengths only (no string_view) so a span set stays valid to cache
// against a stable backing buffer and later reapply against a *different*
// string that has byte-identical layout at those offsets -- e.g. an
// entry's name vs. its nameLower, since CaseFoldAscii is a 1:1
// byte-length/position-preserving ASCII map that never touches the four
// separator characters (docs/adr/0012-cached-name-token-spans.md).
struct TokenSpan {
    uint32_t offset = 0;
    uint32_t length = 0;
};

bool operator==(const TokenSpan& a, const TokenSpan& b);

// Same splitting rules as TokenMatcher::Tokenize (space/underscore/hyphen/
// period separators, no empty tokens), but returns byte offset/length spans
// instead of string_views. Lives here (not TokenMatcher.h) because IndexPool
// is the sole caller, needing it to populate its own name-token-span cache
// at AddEntry/LoadFromPathPool time -- see docs/adr/0012.
std::vector<TokenSpan> TokenizeToSpans(std::string_view text);

// Slices `text` at each span in `spans` (in order), writing the resulting
// string_views into `out`. `out` is cleared first; its existing capacity is
// reused (same reused-buffer contract as TokenMatcher::TokenizeInto).
void ApplyTokenSpans(std::string_view text, std::span<const TokenSpan> spans,
                     std::vector<std::string_view>& out);

// Flat-pool in-memory index layout (docs/adr/0006-pool-based-index-layout.md):
// name/path strings live in two contiguous byte pools instead of per-entry
// heap allocations, so a sequential search scan stays cache-resident instead
// of chasing scattered pointers. nameLowerPool is never persisted to disk --
// LoadFromPathPool rebuilds it from pathPool via ASCII case-fold.
class IndexPool {
public:
    struct EntryView {
        std::string_view name;
        std::string_view nameLower;
        std::string_view path;
        uint64_t size = 0;
        uint64_t lastModified = 0;
        uint32_t attributes = 0;
        // Cached token spans for `name`/`nameLower` (docs/adr/0012), same
        // splitting rules as TokenMatcher::Tokenize: a name with no
        // separators is one span covering the whole name; a
        // separators-only (or empty) name is zero spans.
        std::span<const TokenSpan> nameTokenSpans;
    };

    // Appends entry to the pools; entries are never physically removed
    // (offsets must stay stable), see MarkDeleted for removal.
    void AddEntry(const FileEntry& entry);

    size_t Count() const;
    EntryView GetEntry(size_t index) const;

    void MarkDeleted(size_t index);
    bool IsDeleted(size_t index) const;

    // Linear scan; acceptable at the scale of incremental changes applied by
    // IndexStore (M1). Revisit with a path->index map only if profiling shows
    // it's needed.
    std::optional<size_t> FindByPath(std::string_view path) const;

    const std::vector<EntryMeta>& Meta() const;
    const std::vector<char>& PathPool() const;
    const std::vector<char>& NameLowerPool() const;

    // Reconstructs a pool from serialized meta + pathPool (as IndexSerializer::Load
    // does), rebuilding nameLowerPool since it is never persisted on disk.
    static IndexPool LoadFromPathPool(std::vector<EntryMeta> meta, std::vector<char> pathPool);

private:
    std::vector<EntryMeta> meta_;
    std::vector<char> nameLowerPool_;
    std::vector<char> pathPool_;

    // Flat pool of name-token spans shared across every entry (docs/adr/0012),
    // in-memory only like nameLowerPool_ -- never persisted, rebuilt in
    // LoadFromPathPool. nameTokenStart_/nameTokenCount_ index into it,
    // parallel to meta_ (not folded into EntryMeta itself: that struct is
    // the on-disk record and this cache never touches disk).
    std::vector<TokenSpan> nameTokenSpans_;
    std::vector<uint32_t> nameTokenStart_;
    std::vector<uint16_t> nameTokenCount_;
};

}  // namespace indexed
