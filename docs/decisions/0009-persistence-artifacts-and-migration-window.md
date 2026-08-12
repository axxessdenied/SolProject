# ADR 0009 — Persistence artifacts and migration window

**Status:** Accepted

**Date:** 2026-08-12

## Context

ADR 0004 requires versioned persistence from first use and guaranteed migrations beginning with public alpha, but leaves artifact shapes and the retained support window open. Campaign saves, shareable blueprints, editable content, and settings have different readability, size, and recovery needs.

## Decision

- Encode project-owned settings, data-defined content, and content/mod manifests as schema-validated UTF-8 JSON unless an owning milestone demonstrates that another human-readable format is materially better.
- Store craft and assembly blueprints as versioned packages with a UTF-8 JSON manifest. The package may later contain binary payloads and preview images, but required content identifiers, versions, topology metadata, and compatibility diagnostics remain readable without loading a campaign.
- Store campaign saves as versioned chunked binary containers with a small human-readable UTF-8 JSON manifest describing format version, campaign identity/time, required content, chunks, and integrity information.
- Select concrete archive, compression, and binary-encoding libraries only in the owning persistence milestone after dependency review.
- Write saves transactionally. Migration or replacement must preserve the original or a recoverable backup until the new artifact passes integrity validation.
- Continue allowing explicit pre-alpha invalidation under ADR 0004.
- Guarantee that version 1.0 can migrate supported saves and blueprints from every public-alpha release, directly or through maintained chained migrations.
- Beginning with 1.0, guarantee migration within the current major release series and from the final supported save/blueprint schema of the immediately preceding major series. Older non-final previous-major artifacts may require the final upgrader from their own major series.
- Never silently discard unknown chunks, required content, identities, or state during migration. Preserve explicitly designated optional/forward-compatible chunks or reject the artifact with actionable diagnostics.

## Consequences

- Human-authored/shareable data remains inspectable while large campaign state can use efficient binary representation.
- A package/container is a logical format boundary; this ADR does not preselect ZIP, a serialization library, compression, or an on-disk object layout.
- Public alpha incurs fixtures and migration maintenance through 1.0. Each later major release must preserve the final upgrader and fixtures required by the one-major compatibility window.
- Settings may use tolerant/defaulted migrations appropriate to preferences; campaign and blueprint compatibility guarantees must not be weakened by treating them like disposable settings.

## Validation

- Every artifact parser validates schema/container version and required content before constructing runtime state.
- Round-trip tests preserve stable identifiers, blueprint topology, classified exact quantities, campaign chronology, and content references.
- Migration tests load golden fixtures for every guaranteed source version and verify that failure leaves the source artifact recoverable.
- Corruption, unknown required chunks, missing mods, and unsupported future versions produce distinct diagnostics.
