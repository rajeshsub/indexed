Status: Accepted

## Context

winindex's ADR-0006 established a flat-pool in-memory layout after profiling showed
heap-scattered `FileEntry` strings caused severe cache thrashing at scale (624K entries):
chasing millions of scattered heap pointers dominated search latency. `indexed` adopts
the same layout, converting `wchar_t` pools to UTF-8 `char` pools, and additionally
resolves the offset-width question the winindex ADR didn't have to face (NTFS path
components are bounded; a full Linux path is not).

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. `vector<FileEntry>` with per-entry `std::string` | Index is small (<100K entries) | None | None | Cache-hostile at 600K+ entries; scales poorly (this is exactly what winindex's ADR-0006 moved away from) |
| b. Flat pool: single `vector<char>` per pool + `EntryMeta` offset/length records | Sequential scan is the primary access pattern (substring/token/regex search) | Medium — port winindex's design, convert to UTF-8 byte offsets | Add a sorted/trigram index later if needed | O(n) scan but L3-resident; matches winindex's proven design at this scale |
| c. Trigram inverted index | Arbitrary substring at >10M entries with sub-linear lookup required | High complexity | Standard IR approach | Overkill for a single-user workstation's file count; large memory overhead for no proven need |

## Decision

Adopt **option b**, porting winindex's flat-pool design verbatim with `wchar_t`→UTF-8
`char`:

- **`nameLowerPool`** (`vector<char>`): all lowercased filenames concatenated (utf8proc
  case-fold), sized to stay L3-resident. Default search target.
- **`pathPool`** (`vector<char>`): all full paths, original case, concatenated. Used only
  when `matchPath = true` (opt-in).
- **`EntryMeta`** array: fixed-size records holding offsets/lengths into both pools, plus
  `size`, `lastModified`, `attributes`.

**Resolved during grill-me — pool offset width is 64-bit**, not the `uint32_t` a direct
byte-for-byte port of winindex's design would suggest. NTFS path components are bounded,
so winindex could safely use narrower fields; a Linux `pathPool` has no such bound and a
single pool could in principle exceed 4 GiB on a very large index. Since this is an
on-disk/in-memory format that's expensive to change post-hoc, the decision was made once,
early: widen `pathOffset`/`nameLowerOffset` to `uint64_t` now, at a cost of ~8 bytes/entry
(~80 MB at 10M entries) — negligible against the cost of a forced v2 format migration later.
`nameStart` (basename offset within a path) stays `uint16_t`: a single path *component*
never approaches 65535 bytes even though a full path can.

Concurrency: `std::shared_mutex` — search acquires a shared read lock for the scan
duration; the `Indexer` acquires an exclusive write lock to apply incremental changes from
the fanotify/inotify monitor.

`nameLower` is **not** persisted on disk; it is rebuilt at load time via a utf8proc
case-fold pass over `pathPool` (one-time O(n) cost at startup). A `pathLower` pool for
diacritics-folded search is deferred — folded on the fly per search thread, matching
winindex's own deferral of a `pathLower` pool.

## Consequences

- Name-only search scans a contiguous, L3-resident memory region instead of chasing
  scattered heap allocations — the same throughput win winindex's profiling demonstrated.
- `SearchResult` carries `entryIndex` (into the `EntryMeta` array) rather than a pointer;
  the UI looks up display fields via `IndexPool::GetEntry(index)`.
- `EntryMeta` is deliberately larger than winindex's equivalent struct (64-bit offsets vs.
  winindex's compact NTFS-bounded fields) — a considered trade-off, not an oversight; see
  `docs/adr/0003-binary-index-format.md`.
- `static_assert(sizeof(EntryMeta) == N)` plus a round-trip test in `test_IndexPool` is
  required to catch any accidental struct-layout drift.
