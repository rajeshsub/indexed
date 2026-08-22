Status: Accepted

## Context

Tagged releases publish a prebuilt `indexed-x86_64.AppImage` to GitHub Releases (README
"Releases"). An engineering-standards audit flagged that the release carries no integrity
proof: no checksums, no signed tags, no verification instructions. A stranger downloading
the AppImage currently has no way to confirm it matches the tagged source.

## Options

| Option | Fits when | Cost now | Extension path | Trade-off |
|--------|-----------|----------|-----------------|-----------|
| a. SHA-256 checksums generated in CI, published alongside the artifact; signed tags deferred | Solo maintainer, no existing key-custody or CI-secret infrastructure for signing | One CI step, no secrets to manage | Add GPG-signed tags later without touching this step | Checksums alone don't prove the artifact came from the maintainer, only that it wasn't corrupted/tampered in transit from the point the checksum was published |
| b. GPG-signed tags now, checksums published | Project ready to maintain a signing key long-term | Generate + custody a private key, store it as a CI secret, publish the public key, document verification | Already maximal | Key compromise/rotation becomes an ongoing maintenance burden the project isn't set up for yet |

## Decision

Use **option a**. CI generates a `SHA256SUMS` file alongside `indexed-x86_64.AppImage` on
every tagged release; the README documents how to verify against it. Signed git tags are
deferred until the project has a maintained signing key and a plan for its custody --
tracked as future work, not silently dropped.

## Consequences

- Every tagged release gets a `SHA256SUMS` file generated in the same CI job that builds
  the AppImage, published as a second release asset.
- README's "Releases" section documents `sha256sum -c SHA256SUMS`.
- Tags remain unsigned for now; `git tag -v` will not verify. Revisit this ADR (supersede,
  don't edit) if/when a signing key is set up.
