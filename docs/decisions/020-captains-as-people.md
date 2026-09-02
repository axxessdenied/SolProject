# 020 — Captains are people, and a captain's ship exists two ways

- **Date**: 2026-09-02
- **Status**: accepted
- **Supersedes for this branch**: `006-crew-passive-bonuses.md` (its final
  consequence: *"Any richer crew system post-v1 must grow out of this data
  model or consciously replace it."* This is the conscious replacement.)
- **Amends**: `015-multi-system-simulation.md` (see the amendment recorded
  there, 2026-09-02, second)

## Context

GDD §14.2 puts captains on the board for v2: *"A captain is crew you give a
ship to instead of a bonus. Hired at a crew hall, they take a ship out of your
storage and fly it. A captain accepts the same command vocabulary as your own
ship plus standing orders that outlive the session: mine here, haul between
there and there, patrol this, escort that, sell when the price clears X."*

Priced against the code on 2026-09-02, while writing the Phase 39 spec. Four
things the sentence assumes are not true of the tree:

- **Crew are defs, not people.** `OwnedShip::crewIds` is a
  `std::vector<std::string>` of *catalog ids*, and `CrewDef` is
  `{id, name, role, price, modifiers, gate, source}` — nothing that
  distinguishes one hire from another. Two hired Engineers are one string
  twice; `fireCrew(id)` dismisses *a* copy. **There is no object in the game
  a standing order could be given to.**
- **You cannot touch a stored hull.** Eleven sites in `space_world.cpp` index
  `m_fleet[m_activeShip]` directly, and every fleet mutation is one of them.
  `sellShip` and `switchShip` are the only calls that name a stored ship, and
  both refuse unless you are docked at that exact station.
- **`015`'s retention policy collides with Phase 38's cap.** Phase 38 left
  `bubbleRetentionSeconds` a hook and wrote Phase 39's clause into a comment.
  But `kMaxInstantiatedSystems` is **6** and `instantiateSystem` returns
  `false` past it; `releaseCooledBubbles` counts `holdSeconds` down
  unconditionally, so an indefinite hold needs the refresh `kCoolingSeconds`
  forbids in its own comment; `enforceBubbleCap` picks the minimum hold, which
  is arbitrary among equal sentinels; and that function's safety argument —
  *"nothing dangles: every reference that outlives a bubble is coarse-layer
  state keyed by SYSTEM"* — stops being true the moment an `OwnedShip` names
  an entity inside one.
- **A hauling captain is not in a system.** `maxTradeJumps` is 5, so a haul
  crosses up to six of eighty systems, and each one entered gets a full sky
  built for it (`fillSystemSky`: statics, rocks, ambient wings — 25–136
  entities and 4–15 ships, per Phase 38's own measurement).

## Decision

### 1. A captain is a named instance — the first person in this game

A `Captain` is a record with identity: a name, an id, the fleet index of the
hull it flies, and its cut. It is hired and dismissed at the Crew tab, beside
the three passive-bonus crew rows, **which stay exactly as they are**.
`CrewDef` is not extended, not flagged, and not replaced; `006`'s model keeps
serving the thing it was designed for.

Rejected: a `can_captain` flag on `CrewDef`. It reuses the catalog and the
tab's machinery, and it leaves *my captain* unable to be a particular person —
which Phase 40 immediately needs, because a fleet has a commander.

Rejected: no person at all, with the standing order carried by the ship. The
cheapest answer, and it deletes §14.2's fantasy rather than shipping it.

**Consequence**: a person is a new kind of save-format promise. A fitting whose
def has gone missing can be dropped on load with a warning; a named captain the
player hired cannot be. The missing-def behaviour is decided in the phase's
first stage.

### 2. A captain's ship exists two ways, and the order decides which

- **Stationary orders** — mine here, patrol this, guard this station — are
  **full entities in a held bubble**. This is `015` unamended: the system stays
  instantiated while the order stands.
- **Itinerant orders** — haul between there and there, escort that — **ride the
  coarse layer while between systems and promote to a full entity whenever the
  player is co-located.** `Depart` / `Jump` / `Arrive` already decompose a
  haul, and `TraderPuppet` already states the relationship: *"the entity is a
  view and the trader is the record."*

`015` declined *"Coarse away, real when near"* for a recorded reason: *"my
escort died to a raid while I was two jumps away becomes a die roll rather than
a fight that happened."* **That objection is about combat, and this decision
honours it exactly where it applies.** It does not reach a freighter crossing
600,000 km of empty lane on a schedule — which is what `TraderLeg::Depart`
already is, for 120 haulers, and which nobody has called a weaker claim about
this galaxy.

**The precedent for the coarse half already shipped.** `RefineJob` is a player
asset, in a named system, converting value on a coarse clock, with no bubble
and no entity — and no one has ever argued it makes the galaxy a spreadsheet.
`SurveyEntry` is the same shape. GDD §14.4's *"a ship you own that is not in
your system has to actually exist"* is therefore already not a claim about
everything the player owns; it is a claim about ships that can be **shot at**,
and that is what the split preserves.

Rejected: `015` literally, with every captain holding a bubble. It contradicts
a public, test-asserted cap at the sixth system, turns a two-minute transient
into the steady state, and makes one hauling captain simulate a rolling window
of somebody else's traffic to move 150 units of ore.

Rejected: flat coarse everywhere. Cheapest, and it gives away the thing the
user declined to give away at `015`.

### 3. Ownership is carried, not inferred

Every hostility decision in the game routes through `ShipPilot::factionIndex`
— 59 references in `space_world.cpp`, 31 in `content.cpp`, against a
92-reference faction table. There is **no player faction row**, and the
unaffiliated value `0xffff'ffffu` is documented as *"unconditionally
player-hostile"*. A captain's hull is a third case the model has never had.

**Ownership is carried on the ship and the predicates are derived from it**,
rather than encoded as the absence or presence of a faction number. This is
Phase 37's ruling one layer out: that phase turned `bool pirate` into a carried
`FactionKind` because *"a second bool beside it would have been the same bug
with more states — `pirate && shadow` is nonsense and nothing but convention
would have stopped it."*

### 4. A captain is paid a cut of what their ship earns

Not a wage on a clock, and not a one-time fee.

There is **no recurring cost anywhere against `m_playerCredits`** — `upkeep`
appears only on station archetypes, which the player never pays — and every
player transaction in the game is instantaneous. `006` refuses wages by name.

A cut is charged at an event that already exists: a haul completing, ore
delivered, a bounty paid. No new clock; an idle captain costs nothing rather
than bleeding the player slowly; a bad route is visibly worse rather than
silently expensive.

Rejected: a one-time hire fee, which makes every captain pure upside after the
first payment and reduces the fleet to how many hulls you can afford.

Rejected: a wage per unit of sim time. The conventional answer, the one `006`
refused, and the one that introduces the game's first drain running while the
player is not looking — a real feel decision that should be taken on its own
evidence if it is ever taken.

## Consequences

- **Phase 39 is five stages**, one per order set plus the person and the two
  halves of the representation. The user took the widest scope on orders (all
  five of GDD §14.2), so the phase is wide rather than deep.
- **`kSaveVersion` bumps** in the phase's first stage: captains, their
  assignments, their orders and their ledgers are all new.
- **Phase 38's `bubbleRetentionSeconds` hook is not one clause.** Three
  functions change, and one of them is a comment that must be rewritten rather
  than left standing while false.
- **The fence around this decision is Phase 40.** Every order here is given to
  exactly one captain; a commander, a formation, and an order resolved by
  composition are the next phase.
- **`006` stays accepted for what it covers.** Passive-bonus crew are not
  deprecated, not migrated, and not touched. A ship can carry both.
