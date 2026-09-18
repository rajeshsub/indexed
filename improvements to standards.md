# Improvements to engineering-standards, from the persistence/monitoring review (2026-09-18)

Source: external AI review of `IndexSerializer`, `Indexer`, `IndexPool`, `WalkScanner`.
Six findings total; three carried real merit (atomic save, trailing-byte validation, doc
gaps), one was a non-issue (host-endian format -- already an explicit, documented ADR
trade-off), and two require a developer decision before any implementation
(`/grill-me` candidates, not something the skill should let an agent decide unilaterally).
This file proposes standards changes so the skill surfaces these classes of issue earlier
and routes the opinion-requiring ones to `/grill-me` instead of either skipping them or
guessing at a resolution.

## Items that need developer grilling first (do these first per developer instruction)

These are that the skill should learn to **flag and route to `/grill-me`**, not resolve
silently and not silently skip either. An agent choosing a tombstone-compaction policy or
a staleness-consistency guarantee on its own is exactly the kind of unreviewed design
decision `engineering-standards` exists to prevent.

1. **Tombstone/compaction policy for append-only stores.** `IndexPool::MarkDeleted` never
   reclaims space; only a full rebuild currently compacts. Grill-worthy questions: is a
   compaction pass ever needed, what would trigger it (entry count? tombstone ratio? time?
   manual only?), should tombstone count / memory growth be exposed to the user (status
   bar? log line? nowhere?), and does periodic reindexing already count as compaction (it
   does, incidentally -- `StartIndexing`'s stale-rebuild path replaces the whole pool) --
   if so, is that sufficient and does it just need documenting instead of new code?
   **Standards change:** any append-only/tombstone data structure design should require an
   explicit "reclamation policy: none (rebuild-only) / triggered / periodic" line recorded
   in the relevant ADR before merging, decided via `/grill-me`, not left implicit.

2. **Staleness/consistency guarantee strength.** The current design (timestamp age check +
   live monitoring + periodic rescan) is a deliberate, reasonable trade-off and is already
   mostly documented in `docs/adr/0007-fanotify-vs-inotify-monitoring.md`. But whether
   that's the *permanent* target guarantee, or whether a future milestone should add
   stronger consistency (e.g., a lightweight validation pass, or surfacing "possibly
   stale" more prominently in the UI beyond the status bar) is a product decision, not an
   engineering default. **Standards change:** when a design relies on "cache + monitor,
   not a transactional snapshot" semantics, the skill should require that trade-off to be
   both (a) ADR-recorded (already the norm here) and (b) explicitly surfaced in
   user-facing docs (README/settings UI), and should prompt `/grill-me` if a reviewer
   later asks "is this guarantee strong enough" rather than letting an agent unilaterally
   decide the answer is "yes, ship as-is" or silently add validation code nobody asked for.

## Items with merit that were fixed without needing developer input

3. **Non-atomic index persistence.** `IndexSerializer::Save` wrote directly to the final
   path with `std::ios::trunc`; a crash mid-write destroyed the last good index (the CRC
   caught the corruption on next load and forced a rebuild, so it was recoverable but
   needlessly destructive). Fixed: write to a sibling temp file, flush, `fsync`, then
   `rename()` over the target (atomic on the same filesystem). **Standards change:** add a
   checklist item under "persistence code" -- *any code that overwrites a file another
   part of the system depends on for recovery/state must write-temp+rename, not
   truncate-in-place* -- so this class of bug is caught at review time on the first pass,
   not by an external audit later.

4. **Format parser didn't reject trailing payload bytes.** `IndexSerializer::Load` never
   checked that the parse offset reached the end of the buffer after consuming the
   declared fields. Not an active corruption vector (CRC covers the full payload, so an
   attacker would need to also recompute a matching CRC), but it made the parser more
   permissive than the format's own declared structure, which would bite a future format
   version. Fixed: added an `offset != buffer.size()` check that fails the load.
   **Standards change:** add "deserializers must verify full-buffer consumption, not just
   checksum validity" to the binary-format checklist alongside the existing magic/version/
   CRC checks -- checksum correctness and structural completeness are two different
   properties and the skill's current guidance only prompted for the former.

## Item with no merit (informational only, no standards change needed)

5. **Host-endian on-disk format.** Already an explicit, ADR-recorded trade-off
   (`docs/adr/0003-binary-index-format.md`) for a single-platform, non-portable, internal
   cache. This is what "resolved decisions log" ADRs are *for* -- the skill worked as
   intended here. No change needed, but worth noting as a positive example: an external
   review flagging something already deliberately decided and documented is the system
   correctly having already litigated it once, not a gap.

## Meta observation

Every finding here maps cleanly onto one of two skill gaps: (a) a missing item on a
domain-specific checklist (atomic writes, full-buffer parsing), or (b) an opinion-bearing
design fork that should have triggered `/grill-me` at design time rather than being
implicit in the code. Strengthening `engineering-standards` to (1) carry a short checklist
per code domain (persistence, binary formats, concurrent mutation, filesystem scanning)
and (2) explicitly name "reclamation/compaction policy" and "consistency guarantee
strength" as decision forks requiring `/grill-me` sign-off would have caught #1-4 at
design/review time instead of needing an external pass after the fact.
