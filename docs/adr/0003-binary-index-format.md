Status: Accepted

## Context

The in-memory index (millions of `FileEntry` records) must be persisted to disk and
reloaded on startup to avoid re-scanning on every launch. winindex's ADR-0003 chose a
custom binary format with CRC-32 integrity checking; the same trade-offs apply on Linux
with UTF-8 instead of UTF-16 and a fresh format version/magic.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. SQLite | Query flexibility needed | System/fetched dependency | Full SQL queries | Larger dependency; load time dominated by row parsing at millions of entries |
| b. JSON / MessagePack | Human-readable or cross-platform | nlohmann/json or msgpack fetch | Schema evolution with versioning | 3-5x larger than binary; slow parse at >1M records |
| c. Custom binary + CRC-32 | Max load/save speed, single platform, internal cache only | ~300 lines | Versioned header | Not human-readable; schema changes require migration code |

## Decision

Use a **custom binary format** (option c), `indexed.idx`, magic `0x44584449` ("IDXD"),
version 1, with a CRC-32 integrity check over the payload (implemented in
`IndexSerializer`). At millions of records, a flat binary write/read is an order of
magnitude faster than any text or row-oriented format, and the format is purely an
internal, rebuildable cache — cross-platform portability and ad-hoc queryability are not
requirements (§2 non-goals).

Two decisions resolved during the grill-me pass, both hard-to-reverse since they're
baked into the on-disk layout:

- **Pool offset width:** 64-bit (`pathOffset`/`nameLowerOffset` in `EntryMeta`), not
  32-bit. A 32-bit offset caps a single pool at ~4 GiB; widening costs ~8 bytes/entry
  (~80 MB at 10M entries) and permanently removes that ceiling. See
  `docs/adr/0006-pool-based-index-layout.md` for the full pool-layout rationale.
- **Timestamp epoch:** all `u64` timestamp fields (`EntryMeta.lastModified`, the header
  `timestamp`, and the trailer `lastMonitorStop`) store **nanoseconds since the Unix epoch**
  (1970-01-01 00:00:00 UTC), derived from `statx`'s `stx_mtime` (`tv_sec * 1e9 + tv_nsec`).
  Chosen over second-resolution because sub-second precision is available for free from
  `statx` and future-proofs display formatting; display conversion to `YYYY-MM-DD HH:MM`
  local time happens only at render time in the UI.

## Consequences

- Index loads in well under a second even for millions of files.
- Format changes require a version bump; CRC or version/magic mismatch discards and
  silently rebuilds the index, exactly like winindex.
- The `.idx` file is not human-inspectable without a dedicated tool.
- `EntryMeta` is 8 bytes larger per entry than a 32-bit-offset design would be; accepted
  as the right trade against a future forced format migration.
