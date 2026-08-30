# 014 — A ship is its mounts: named hardpoints replace one weapon and four slot counts

- **Date**: 2026-08-28
- **Status**: accepted

## Context

`docs/gdd.md` has promised hardpoints since it was written — §5 says *"Hardpoint
sizes/classes per ship"* and §8 says *"ships have hardpoints + module slots"*.
Priced against the code on 2026-08-28, before anything was designed:

- **The word "hardpoint" appears nowhere in the engine.** Six matches across the
  whole repo: three prose comments, one GDD line, and two lines of a TOML parser
  test that uses it as an arbitrary key name.
- **A ship has exactly one weapon.** `ShipDef::weaponId` is a `std::string`
  (`data_defs.hpp:117`). `game/src/ship_ui.cpp:190` says so out loud in a
  comment: *"Weapon first, because it is the one hardpoint."*
- **Everything else is four integers.** `slotsShield`, `slotsEngine`,
  `slotsCargo`, `slotsUtility`, and `ModuleSlot` is a four-member enum. A module
  is a bag of stat multipliers with no position, no size, no identity and no
  condition.
- **`ModuleDef` cannot express "this hull takes a large turret and that one
  does not."** Slot counts are quantities; the ask is about *kinds and sizes in
  places*, which counts cannot say.
- **`FitStat` has 17 members and resolution is order-independent** (adds sum,
  then muls multiply — `loadout.hpp`). That machinery is good and survives; what
  fails is only *where a fitting sits and what may sit there*.

The Depth Arc asks for turret/gun/laser/torpedo/launcher/engine/thruster/shield/
covert mounts *and* upgradeable subsystems *and* visual representation on the
hull *and* the Forge authoring them. None of that is expressible against four
integers, and half of it is not expressible against a position-free module.

## Decision

**A mount is a named, typed, sized place on a hull where exactly one fitting
goes, and it is the only fitting mechanism.** `weaponId` and the four
`slots_*` counts are removed rather than kept alongside.

```toml
[[ship]]
id = "sol.destroyer"

  [[ship.mount]]
  id   = "turret_dorsal_1"
  kind = "turret"
  size = "medium"
  at   = [0.0, 3.2, -1.5]   # metres, hull frame -> external, drawn, shootable
  aim  = [0.0, 1.0, 0.0]
  arc  = 220.0              # degrees of traverse

  [[ship.mount]]
  id   = "internal_reactor"
  kind = "subsystem"
  size = "large"            # no `at` -> internal, never drawn
```

Four rules carry the weight:

1. **`id` is stable and a save refers to a fitting by the mount it occupies.**
   Not by index — an author inserting a mount would silently rearrange every
   existing player's ship.
2. **`at` present means external; absent means internal.** One key decides
   drawn-or-not, shootable-at-a-position-or-not, and nothing else needs a flag.
   An internal mount is still destructible; it is simply not aimed at.
3. **A mount accepts its own size or smaller.** Fitting small kit to a large
   mount wastes the mount. That waste is the player's trade, not an error.
4. **Ship outfitting `[[module]]` is renamed `[[component]]`.** A component is a
   thing that occupies a mount. This frees the word `module` for stations
   (`decisions/016`), where it is the natural noun and where no other word fits.

**Mount condition is in scope** (GDD §5, promoted to [core]). Each mount carries
hit points and a destroyed mount stops working. External mounts resolve hits
against `at`; internal mounts are reachable only once armour and hull are
compromised.

## Alternatives considered

- **External hardpoints beside internal slot counts.** Half the change: mounts
  carry geometry for guns and drives, subsystems stay counts. Rejected because
  the ask explicitly wants *"the type of subsystems they can handle and what kind
  of upgrades"* — a count cannot say which subsystems a hull accepts, so the
  cheaper shape does not answer the question that motivated the work. It also
  guarantees two fitting models, two UI paths and two save representations
  forever.
- **Hardpoints as pure geometry**, with fitting rules untouched. Cheapest, and it
  would deliver the visual half. Rejected for the same reason plus one more: it
  makes the Forge's mount authoring produce data the game does not fit against,
  so the tool would be editing decoration.
- **Keeping `weaponId` as "the primary weapon" with mounts additive.** Rejected —
  a special case that every consumer must branch on, for no benefit once mounts
  exist.

## Consequences

- **The save format breaks.** `OwnedShip` currently holds `weaponId` plus a flat
  `moduleIds` vector; it becomes a mount-id → component-id mapping. `kSaveVersion`
  bumps and older saves are rejected, per this project's standing precedent of
  an exact version check and no migration.
- **Every def file that names a ship changes**, and so do the three shipped hulls,
  `modules.toml` (renamed), and the outfitting screen — which is rebuilt around
  a mount list rather than four slot-type tabs.
- **`loadout.hpp`'s stat resolution survives unchanged.** Adds-then-muls over
  `FitStat` is orthogonal to *where* a fitting sits; only enumeration of the
  fitted set changes. This is the single largest piece of existing machinery the
  decision preserves.
- **Weapons become plural**, which is new sim surface: fire groups, per-mount
  capacitor draw, convergence, and traverse limits on turrets.
- **The Forge gains a mount tool.** Priced as an extension rather than a new
  capability: `point_tool.cpp` already picks and drags named points on a mesh in
  3D (1,287 lines), and `def_editor.cpp` already owns `ships.toml` and validates
  writes through `DefDatabase::mergeToml` — the game's own schema. Placing a
  mount is those two facilities meeting.
- **It unblocks four later phases.** Covert suites (§13) are subsystem mounts;
  EW, remote logistics and command hulls are subsystem mounts; the ship taxonomy
  (§11) is only meaningful because mount budget is what separates a class-2 hull
  from a class-5 one; and the visual representation of a fit is `at` plus a model.
- **What it does not do**: it is not ship *construction*. An author places mounts;
  a player fills them. GDD §9's block-building non-goal is untouched, and the
  distinction is written into that non-goal so it is not re-litigated.

## Amended by Phase 31 stage B (2026-08-29)

Three things the fit model needed that the decision above did not name. Each is
recorded here rather than only in a commit message, because each is a rule a
later stage or a mod author has to obey.

**1. A turret accepts a `fixed` fitting; the reverse is refused.** A weapon def
names the mount kind it goes in, and `mountAcceptsKind` is equality plus that
one asymmetry. A turret is a ring with a traverse motor: bolting a bare gun
into one gives a gun that traverses, so **`arc` is authored on the mount and
never on the weapon**, and none of the four shipped guns had to be authored
twice for the freighter's `turret_dorsal` to take them. `launcher` and `bay`
are left strict — nothing in the game carries ordnance yet, so a relaxation
there would be written blind.

**2. `weapon =` became `fit` on a mount.** Removing `ShipDef::weaponId` left
nothing to say what a hull comes armed with, which an NPC spawn, the starter
ship and a newly bought hull all need. `[[ship.mount]] fit = "<def id>"` is
that, and it is on the mount because that is the only place that can say *which*
gun goes *where*. `resolveLoadout` rewrites those fields to the player's actual
fit, so the resolved def *is* the ship as flown and one code path serves an NPC
hull and the player's own.

**3. `slots_cargo` had nowhere to go, and the merge is a real content change.**
gdd.md §11.5 has no `cargo` mount kind: a cargo pod is a `utility` fitting. On
the freighter a hold pod and a survey scanner now compete for the same five
places where they used to have three and two. Every shipped component was
converted at `size = "small"` so that nothing a player could buy before mounts
became unbuyable after them; **sizing the Mk2 tier up to `medium` is a separate
balance decision and was deliberately not made in passing.**

**And one consequence that is a content gap rather than a rule.** The
freighter's `core_sensor` is the only `subsystem` mount in the base game and no
shipped component is a `subsystem`, so it accepts nothing. Moving the survey
scanners there would have taken them off the player's starting shuttle, which
has no subsystem mount; Phase 32 authors the hulls and kit that earn the kind.

## Amended by Phase 31 stage C2 (2026-08-30)

`aim` and `arc` were authored in stage A2 and read by nothing for three
stages. Making a gun fire down them turned out to need four rules that the
decision above does not name, and each is a rule a later stage, a mod author
or a balance pass has to obey.

**1. `arc` is the FULL cone angle centred on `aim`, not a half-angle.** A 270
ring reaches 135 degrees either side, which is exactly what makes the
freighter's dorsal turret able to fire ahead, astern and to both beams and
blind only straight down through its own hull. Zero is a gun bolted down;
360 has no stop. The example in the Decision section above — `arc = 220.0` on
a dorsal turret — reads as 110 degrees either side under this rule.

**2. A ring is laid by a GUNNER; a bolted gun is aimed by the PILOT.** A gun
with `arc = 0` points where the mount says and fires whenever the trigger is
down: that is the shuttle's nose gun and the player aims it by flying. A gun
with an arc swings onto the ship the pilot has SELECTED, leads it with its own
projectile speed, and follows the nose when there is no selection. Three limits
keep that from being a nuisance:

- **Only a ship is a gunnery target.** A station, a planet, a gate and an ore
  field are all selectable and none of them is something a gun can be laid on.
- **A target beyond that gun's own `range` is not sought.** Without this, the
  shipped freighter stops being minable: its dorsal ring carries the mining
  laser, and a fighter selected three kilometres away would swing the beam off
  the rock in front of the pilot to track something it cannot touch.
- **A ring opens only on someone the player is already at war with.** *Ruled by
  the user after seeing C2 live, against the simpler alternative.* Laying on the
  bare selection makes a trap the game had never had: hail a patrol, forget to
  change the selection, hold the trigger to cut a rock, and a dorsal ring puts a
  bolt into the police while your nose is on the asteroid. A ring is a gunner
  and a gunner does not open on a neutral. What it buys is a shape as well as a
  safety — **you open with the nose and the rings join once it is a fight**,
  because the tier is read live and a neutral that starts shooting back is tier
  0 within the tick. An NPC needs no such gate: its pilot's target *is* its
  enemy, chosen by the brain that decided to attack.

  The predicate is **`SpaceWorld::threatTier`**, promoted out of `contactOrder`
  so the contact cycle's threat ranking and a turret's decision to fire are one
  answer. Two would be a radar that paints a ship red beside a ring that will
  not shoot it — the same "one predicate in one file" move Phase 30 stage D
  made for `securityAnswers`.

**3. A gun that cannot bear holds its fire.** It spends no charge and starts no
cooldown — the same treatment stage C1 gave a gun the capacitor could not pay
for, and for the same reason: it did not fire. The ring still turns as far as
it goes toward what it was laid on, so it is already round the right way the
moment the target crosses into its arc.

**4. The lead marker follows a gun the pilot has to aim.** Stage C1 pointed it
at the first projectile gun; C2 narrows that to the first projectile gun with
no traverse. The marker's whole job is to say where to point the nose, and a
ring does not care where the nose points. A hull whose every projectile gun
traverses shows no marker at all, which is the honest answer rather than a gap.

**What was deliberately NOT added: a traverse RATE.** gdd.md §11.5's mount
vocabulary is `at`, `aim` and `arc`, and `arc` is the traverse it names; a
slew rate is a fourth key no design document asks for. It is also unobservable
until a turret is drawn (stage E) or an NPC flies one (Phase 32), so authoring
the number now would be balance written blind — the same call stage B made
about sizing the Mk2 tier. Nothing here has to be undone if it arrives: a gun's
bearing is a pure function of the hull's orientation, its mount and its target
(`game::layGun`), so stage E can draw a turret down the same function C2 fires
along, and a rate would turn that function's result into state at that point.

**A consequence worth writing down: a ring is currently a strict upgrade on a
bolted mount.** With no slew rate the only thing separating them is coverage,
and coverage only goes one way. That is a hull-design fact rather than a player
choice — an author places the mounts and a player fills them — but it is the
pressure a traverse rate would relieve, and it is why the omission above is
recorded as a decision rather than as an oversight.
