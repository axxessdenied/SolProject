# 008 — Story: authored campaign spine over a sandbox

- **Date**: 2026-08-15
- **Status**: accepted

## Context

GDD open question Q7, due at Phase 8: pure sandbox, sandbox with
authored anchor questlines, or an authored campaign. The GDD leaned
"sandbox + anchors". Decided by the user 2026-08-15, **against the
leaning**: v1 gets a campaign spine.

## Decision

**v1 ships an authored main storyline — a campaign spine — that drives
progression, with the full sandbox running around it.** The galaxy,
economy, factions, and every sandbox activity exist and run whether or
not the player follows the spine; the spine is the authored reason to
move through them and the pacing device for unlocks (per decisions/004,
the jump drive is a late-game unlock — the spine is its natural home).

This revises GDD Pillar 4 ("no forced story"): the sandbox remains the
foundation and the spine must be ignorable after its opening — a player
who walks away from the campaign keeps a complete game. "Campaign
drives progression" means headline unlocks and authored content ride
the spine, not that sandbox play is gated on story missions.

## Alternatives considered

**Sandbox + anchor questlines** (GDD leaning) — cheapest authored
content, but anchors without a spine tend to feel like set dressing;
the user wants an authored arc worth following.

**Pure sandbox** — slimmest mission system, but leans entirely on
emergent motivation and makes the late-game unlock pacing arbitrary.

## Consequences

- **Biggest content commitment of the three options** — writing,
  scripted missions, unique locations. This lands on the Phase 8+
  mission-system item: its spec must support authored, stateful,
  multi-step questlines as first-class citizens, not only procedural
  generators.
- Campaign progress joins the save format (when the mission system
  lands — nothing to store before then).
- GDD Pillar 4 and §8 Activities are reworded in the same change set;
  the "systems over scripts" ethos now reads "authored content rides on
  top of real systems", which the simulation-first architecture
  already supports.
- The sandbox-completeness guarantee above is the guardrail against
  scope creep toward a linear story game: any design that makes the
  sandbox unreachable without story progress violates this decision.
