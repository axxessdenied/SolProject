# 020 — Captains are people, and a captain's ship exists two ways

- **Date**: 2026-09-02
- **Status**: accepted; **decision 1's framing corrected 2026-09-02 by stage A**
  (see the correction below). The decision itself stands, and it got cheaper.
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

### 1. A captain is a named instance — and Phase 35 already built one

> **⚑⚑⚑⚑ Correction, 2026-09-02, from stage A.** This section first read
> *"the first person in this game"*. That is false, and cheaply so: **Phase 35
> shipped one.** `CastSeat` seats a named person in every one of the galaxy's 62
> rooms, `CastMemory{who, visits, regard}` is a **saved, sparse relationship
> ledger**, and `castKeyForCharacter` vs `castKeyForSeat` already draws the line
> this decision needed — in a comment that states it outright: *"A UNIQUE IS A
> PERSON AND A REGULAR IS A CHAIR."* `regard` is live, earned by taking a lead
> and spent at `kRegardForFront`.
>
> **So a captain reuses that identity SPACE rather than opening a second one**,
> and the parallel-table defect Phase 34's risk register names is refused at
> design time instead of discovered later. Two consequences follow, both good:
> the "new kind of save-format promise" this document lists below **does not
> exist** (a captain's name is *copied in*, exactly as `CastSeat` copies its
> own, so there is no def to go missing on load); and Phase 40's commander has
> an identity scheme waiting for it.
>
> *The general lesson: "this game has never had X" is a claim about the code,
> and it ages exactly like a roadmap estimate. Grep before writing it into a
> decision doc.*

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

**Consequence, as it turned out**: there is **no** new save-format promise. The
phase spec's risk register expected one — *"a fitting can be dropped on load
with a warning; a named captain the player hired cannot"* — and the answer is
that a captain's **name is copied in**, following `CastSeat`'s own stated reason
(*"a regular's name exists in no def at all"*). A captain references no def, so
nothing about them can go missing. The save writes `m_captains` and nothing
else; who is standing in a crew hall is re-derived from the seed on load, and a
captain you **dismiss** is on offer there again, which costs no storage and
reads as somebody going back to looking for work.

**Where captains come from (the user, at stage A): the crew hall now, the bar
later.** A candidate is generated per `sol.mod_crew_hall` dock from the seed,
reusing the cast's name tables and key space — GDD §14.2's own words, *"hired at
a crew hall"*. **Hiring the regular you already drink with is a second door,
deferred rather than dropped**, and it is the lever that would finally make the
Bar load-bearing. It was not taken in stage A because it breaks an invariant
`castKeyForSeat` states outright: a regular *is* the chair, so hiring one out of
it leaves the seat regenerating the same seed-derived name — two of them — and
vacating a chair is real machinery this stage did not need.

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

---

## Amendment 2 (2026-09-02, Phase 39 stage B) — what "a cut of what their ship earns" turned out to mean

Decision 3 above says *a cut of what their ship earns* and stops there. Building
the haul forced two questions it does not answer, and the user ruled on both
before a line was written.

**The cut is of the PROFIT, never of the sale.** The hold's cost is subtracted
first, and a haul that loses money pays the captain nothing (it does not bill
them either — a captain has no purse of their own). Chosen over a cut of the
gross, which on a thin margin takes more than the run made: that would turn a
bad route into an active drain and make decision 3's own *"an idle captain costs
nothing rather than bleeding"* false of a **working** one. It also makes the
8–20% band mean something a player can shop on, because it is a share of the
upside rather than a toll on turnover.

**The player's credits fund the cargo, charged at the moment the captain buys.**
A haul ties up capital: the purchase leaves the account at departure and the
sale lands at arrival, so a fleet is a business that has to be financed and a
broke player cannot run one — and the attrition roll costs cargo that was
actually paid for. Chosen over settling net at arrival, which never touches the
account and makes a captain a pure faucet whose only downside is a die roll.
**The price of this ruling is real and is new to this game: credits move while
the player is not looking.** It is not the recurring drain `006` refused — it is
an investment that returns — but it is the first time the number changes without
the player pressing anything, and the Crew tab carries the ledger for exactly
that reason.

**Consequence for stage E.** *"Sell when the price clears X"* stops being a
convenience and becomes the instrument for a measured problem: a captain
committed at departure and settling ~200 seconds later loses about 17% when the
far market moves against them, and a floor is what lets the player refuse that
trade rather than watch it.

## Amendment 3 (2026-09-03, Phase 39 stage C) — what the stationary half proved about section 2

Section 2 says a captain's ship exists two ways and the **order** decides which.
Shipping the stationary half leaves that decision intact and sharpens two things
about it.

**A stationary order has ONE representation, not a preferred one.** `CaptainMine`
mirrors no `sim::` type, and that is the point rather than an omission: because
the bubble is held open for as long as the order stands, the hull in the sky
**is** the record. There is no second clock to keep in step, so the
control-world guard the itinerant half needs has no stationary counterpart to
write — and `CaptainHaul`'s reason for holding an `EconomyTrader` field for
field (*"the two would slowly stop meaning the same thing"*) simply does not
arise.

**But the coarse fleet's arithmetic is still needed, for one thing, and that is
not a violation of the split.** An asteroid field sits 8e7–4e8 m from the
barycentre and so does the dock a captain sells at, so the run between them is a
crossing of a playfield that a 120 m/s hull takes days over. `keepTraderOnSchedule`
exists for exactly that — *"the record moves it faster than any hull flies"* —
so a stationary captain's crossing is paced at
`MiningParams::fieldMaxDistance / EconomyParams::traderLegSeconds`, borrowed from
the constant whose own comment reads *"in-system travel per endpoint"*. ⚑ **The
split is about where the RECORD lives, not about who may use the coarse layer's
numbers.**

⚑⚑ **And section 4's ruling needed no special case for mining, which is worth
recording because it could easily have.** *"A cut of the PROFIT, never of the
sale"* was written against a haul, where the hold's cost must come off first or a
thin margin pays the captain more than the run made. Ore out of the ground cost
nothing, so the basis is zero and the profit **is** the gross — the same sentence
evaluated against a different outlay gives a mining captain a straight share of
what the ore fetched. What did have to move is the **ledger**: `earned`, `paid`
and `losses` came out of `CaptainHaul` onto the `Captain`, because a person who
hauls, stands down and is then sent to a rock is one person with one record of
what they have made.

⚑ **A practical consequence nobody predicted, and it is about content rather than
code.** `assignCaptain` refuses the ship the player is currently flying and a
pilot has one body, so a spare hull cannot be ferried to a station that does not
sell hulls — you would be stranded in it. **A captain can therefore only be given
a hull at a shipyard**, which makes "post a miner" a question about where the
ship counters are: 78 of 81 systems have rock, 29 of those also have a crew hall,
and exactly **one** has rock and a dock that sells hulls.
