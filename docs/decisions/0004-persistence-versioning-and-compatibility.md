# ADR 0004 — Persistence versioning and compatibility

**Status:** Accepted

**Date:** 2026-08-12

## Context

Frontiers of Sol is a long-running sandbox with shareable craft/assembly blueprints, data-driven content, and eventual mod support. Persistent schemas will change rapidly during prototypes and internal pre-alpha work, but postponing identifiers and version fields would make later migrations unreliable.

## Decision

- Give every persistent format an explicit schema version from its first implementation.
- Use stable persistent identifiers rather than memory addresses or transient runtime/ECS handles.
- Keep campaign saves, craft/assembly blueprints, settings, and content/mod manifests as distinct formats with independent versions.
- Internal prototypes and pre-alpha builds may invalidate persistent data. Breaking changes must be intentional, documented, and rejected with an actionable message rather than loaded silently.
- Beginning with the first public alpha, supported public releases guarantee an explicit migration path for compatible saves and blueprints across releases.
- Preserve migration tests and fixtures once the public-alpha guarantee begins. Never overwrite the only copy of a user save during migration.
- If a format cannot be migrated safely, fail without modifying the original and explain the incompatible version/content requirement.

## Details owned by ADR 0009

- Exact first public-alpha version number.
- Mod code/plugin compatibility, load order, and distribution.

ADR 0009 selects the artifact families and migration-support window: version 1.0 migrates every supported public-alpha save/blueprint, while releases beginning with 1.0 support their current major series plus the final schema of the immediately preceding major series.

## Consequences

- Version fields, stable IDs, content references, and validation are required in the first persistent prototype.
- Early iteration remains cheap because internal pre-alpha data is explicitly disposable.
- Public alpha creates a durable maintenance and test obligation; it should not begin until schemas are ready for that cost.

## Validation

- Pre-alpha round-trip tests cover the current version and rejection of unknown versions.
- Public-alpha and later changes add golden fixtures and migration tests for every supported source version.
- Migration operates on a copy or creates a recoverable backup before replacing user data.
