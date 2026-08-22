Status: Accepted

## Context

`Logger` (§7.5) shipped in v0.1.0 as a single-level timestamped append log by design (no
levels, no rotation, documented as a deliberate v0.1.0 scope cut in `Logger.h`). An
engineering-standards audit flagged this against the project's logging rule: severity
must be hierarchical and the active level externally configurable without a code change.
`indexed` writes only to a local per-user log file (`<datadir>/indexed.log`), never to a
shared/deployed log aggregator, so the full six-level hierarchy the standard names
(CRITICAL/ERROR/WARNING/INFO/DEBUG/VERBOSE) is more than this app's shape needs.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|-----------------|-----------|
| a. Trimmed 4-level (Error/Warning/Info/Debug), threshold sourced from `indexed.conf` | Desktop app with an existing config-file mechanism already used for every other setting | Add enum + filtering + one `Settings` field | Add Critical/Verbose later if ever needed | None of note |
| b. Full 6-level hierarchy, threshold via env var | Matches the standard's literal list | Same enum cost, plus env-var plumbing | Already maximal | Env var sits oddly for a GUI app normally launched via `.desktop` entry, not a shell |
| c. Trimmed 4-level, env var config | Splits the difference | Same as (a) | Same as (a) | Two config mechanisms (file for everything else, env var for this) for no reason |

## Decision

Use **option a**: a 4-level `LogLevel` enum (`Error`, `Warning`, `Info`, `Debug`), a
`LogLevel` threshold read from `indexed.conf` (new `LogLevel` key, default `WARNING`,
mirrors every other `Settings` field), and `Logger::Log(message, level = Info)` filtering
against that threshold. `Critical`/`Verbose` are not added: nothing in this app currently
warrants either, and the enum extends cheaply if that changes.

## Consequences

- `Logger`'s constructor and `Log()` gain a `LogLevel` parameter; existing call sites
  compile unchanged via the `Info` default, but this is **not behavior-compatible**:
  under the new default `Warning` threshold, `Info`-level calls that used to always
  write are now silently dropped unless the user has raised the configured level. This
  is intentional for the GUI's own two startup lines (`main.cpp`: "indexed starting",
  "first-run setup complete") -- routine, non-security-relevant, fine to be
  verbose-only. It is **not** acceptable left unexamined for `indexed-helper`
  (`src/helper/main_helper.cpp`): that process runs as root, and its start/stop are a
  security-relevant audit trail (docs/adr/0008), so those two lines are logged at
  `Warning` explicitly rather than relying on the `Info` default -- they survive the
  default threshold regardless of what the user has configured. The helper's routine
  lines (settings reloaded, reindex requested, initial indexing complete) stay at
  `Info`, gated by `Settings::LogLevel()` same as the GUI.
- `Settings` gains a `LogLevel()`/`SetLogLevel()` pair and a persisted `LogLevel` key
  (§12.1), following the same `IniFile` Get/Set pattern as every other field.
- Changing the active log level in a running deployment is a config-file edit
  (`indexed.conf`), not a rebuild -- satisfies the standard's "config only" requirement.
  No live-reload: a level change takes effect on next launch, same as every other
  `indexed.conf` setting today.
