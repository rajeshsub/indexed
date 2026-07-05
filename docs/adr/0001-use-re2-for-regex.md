Status: Accepted

## Context

`indexed` needs regex search over filenames and full paths (Alt+1 toggle, mirrors
winindex). The C++ standard library offers `std::regex`, but it has known
quadratic/exponential worst-case behaviour on certain patterns, making it dangerous for
user-supplied input.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|----------------|-----------|
| a. `std::regex` | No external deps allowed | Zero | None | Backtracking DoS on adversarial patterns; slow compile and match |
| b. PCRE2 | Perl-compat patterns needed | CMake fetch + ~2 MB | Full Perl regex | Large; backtracking by default |
| c. RE2 (Google) | Safe, predictable performance | CMake fetch + abseil | All RE2 syntax | Linear-time guarantee; no lookaheads or backreferences |

## Decision

Use **RE2** (option c) via CMake `FetchContent`, pinned to a stable tag. Unchanged from
winindex's ADR-0001 — RE2 is natively cross-platform and UTF-8, so the port carries the
rationale forward without modification. Linux gains a small simplification: RE2 is
UTF-8-native, so unlike winindex there is no UTF-16→UTF-8 conversion needed before
matching.

RE2 guarantees linear-time matching regardless of pattern complexity, eliminating the
backtracking denial-of-service risk inherent in `std::regex` and PCRE2's default mode.
abseil is also required by RE2 and is fetched alongside it.

## Consequences

- User-supplied regex patterns cannot cause catastrophic backtracking.
- RE2 syntax is supported; lookaheads/backreferences are not (rarely needed for filename search).
- Two additional FetchContent targets (re2, absl) add to cold-configure time.
