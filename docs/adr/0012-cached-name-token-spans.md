Status: Accepted

## Context

Token-set search (`TokenMatcher`/`SearchEngine::Search`) re-tokenizes each entry's name on
every search call: `MatchesAllTokens` needs the current entry's name split into tokens, and
that split was recomputed from scratch per entry, per search. A prior fix
(`TokenizeInto` -- see `src/core/search/TokenMatcher.h`) removed the per-entry heap
allocation by reusing one buffer across the scan, but the O(name length) tokenizing scan
itself still runs on every entry, on every keystroke of as-you-type search (the same
entries get re-tokenized repeatedly as the query narrows). Benchmarked via `/grill-me`
before committing to this: proceed on the reasoning that removing repeated re-tokenization
is a win regardless of whether it or `MatchesAllTokens`'s nested substring loop currently
dominates -- the before/after benchmark itself settles which is now the bottleneck.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|-----------------|-----------|
| a. Flat pool: single `vector<TokenSpan>` shared across all entries + per-entry `(tokenStart, tokenCount)` indices in `IndexPool` | Matches this project's existing flat-pool philosophy (ADR-0006) -- sequential scan stays cache-resident | Medium -- one more parallel array, computed at `AddEntry`/rebuilt at `LoadFromPathPool`, same lifecycle as `nameLowerPool_` | A future path-token cache (if ADR-0006's `pathLower` deferral is ever revisited) follows the identical pattern | One more array to keep in sync at the two call sites that already touch `nameLowerPool_` |
| b. Per-entry `vector<vector<TokenSpan>>` | Simpler diff, no offset/count bookkeeping | Low -- less code | None needed, but inconsistent with the rest of `IndexPool` | N small heap allocations per index build/load instead of per search -- still a large win over today, but breaks the flat-pool pattern the rest of the class commits to, and `LoadFromPathPool` re-pays all N allocations on every app launch |

## Decision

Adopt **option a**, extending `IndexPool` with:

- `std::vector<TokenSpan> nameTokenSpans_` (`TokenSpan { uint32_t offset; uint32_t length; }`)
  -- one flat pool of token spans, shared across every entry.
- Per-entry `tokenStart`/`tokenCount` (parallel arrays or folded into a small in-memory-only
  struct alongside `meta_` -- implementation detail, not `EntryMeta` itself) indexing into
  `nameTokenSpans_`.

**Computed from `nameLower`, not `name`.** `CaseFoldAscii` is a 1:1, byte-length-preserving
ASCII map that never touches the four separator characters (` `, `_`, `-`, `.`), so token
offsets computed against `nameLower` are valid unchanged offsets into `entry.name` too. One
cached span set therefore serves **both** case-sensitive and case-insensitive name-target
token search -- no need to cache twice.

**Scope: name-target only.** `matchPath` and `ignoreDiacritics` fall back to the existing
(already-optimized, allocation-free) live `TokenizeInto` path:
- No `pathLower`/path-token cache exists -- ADR-0006 deliberately deferred `pathLower`.
  Building a path-token cache now would silently reopen that decision; out of scope here.
- `ignoreDiacritics` runs text through `FoldDiacritics` (utf8proc decompose + strip-marks),
  which can change byte length and positions -- cached name-offsets don't transfer to
  diacritics-folded text.

**Lifecycle: in-memory only, like `nameLowerPool_`.** Computed at `AddEntry` time, rebuilt
in `LoadFromPathPool` alongside `nameLowerPool_`'s own rebuild. Never persisted to disk --
no `indexed.idx` format version bump, no migration path needed.

**Correctness verification: differential test required.** This cache is a correctness-
critical invariant sitting in hot-path algorithmic code (wrong offsets produce silently
wrong or missing search results, not a crash) -- exactly the class of change CLAUDE.md's
differential-testing rule targets. A test compares the cached-span token set against a live
`TokenizeInto(entry.nameLower)` baseline over a large randomized filename corpus (varied
separator patterns, lengths, and edge cases: empty name, leading/trailing separators,
consecutive separators, no separators, single-character names), not just hand-picked cases.

## Consequences

- `TokenSetSearch` in name-target mode (the common case, per the plan's search-as-you-type
  design) no longer re-tokenizes any entry's name at search time -- only span-pool lookups.
- `matchPath` and `ignoreDiacritics` token search performance is unchanged from today
  (still live `TokenizeInto` per entry); no regression, no improvement for that combo.
- `IndexPool::AddEntry` and `LoadFromPathPool` both gain a tokenization step and a new
  parallel-array write; a differential test guards against the two ever drifting out of
  sync with live tokenization.
- No on-disk format change -- `IndexSerializer`/`indexed.idx` version 1 is untouched.
