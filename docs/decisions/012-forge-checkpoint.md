# 012 — The Phase 24 checkpoint: what the Forge cooks, when, and what a project is

- **Date**: 2026-08-27
- **Status**: accepted

## Context

Phase 24's spec put a checkpoint after stage T, for the reason Phase 9's
existed: *"S and T are the two structural stages, and what they teach changes
the honest estimate for U and V."* It named its three questions in advance so
they could not drift into whatever seemed convenient once the stages were
flown. This is the record of holding it, with stages S (`b6f49d6`) and T
(`54b79c4`) shipped and both verified on Windows and Linux (`e295126`).

The estimates were re-read against the code first, per this project's standing
rule. Four things came back different from what the spec assumed, and one of
them was a number this session had itself got wrong.

- **U1's "zero C++" is not quite true: the Forge does not link `sol::audio`.**
  The diagnosis was right that audio is data-driven end to end and that the
  gap is a *view* problem — but playing a cue in the tool needs that library
  on the link line. It is one `mixer.cpp`, and `platform::AudioDevice` is
  already linked, so the cost is a CMake line plus open/mix/play. `GameAudio`
  is game *policy* (cue names, categories, volumes) and is deliberately not
  what the tool wants; `Mixer::play` is.
- **U2 gained a surface stage T made possible and the spec did not
  anticipate.** `hull.tex` beside `hull.png` is a stem collision, and the
  cooker aborts the *entire* cook on one. Now that the dispatch is a library
  call, the Forge can ask `cookCollisions` and say so **before** cooking,
  rather than the author discovering it as a failed build.
- **U3 is unchanged and still the expensive one.** `tools/cooker/src/gltf.cpp`
  has no handling of `images`, `textures` or `materials` at all. It does carry
  base64 data-URI and external-file loading for *buffers*, which an embedded
  image would reuse — so the work is reachable, not cheap.
- **A full cook of the base game takes 220–235 ms, not the ~1.5 s this session
  first claimed.** Eight `.forge` sources at roughly 29 ms each; everything
  else is skipped. The wrong figure came from a drive script's sleep being
  read as the cook's cost, and it was the number the "cook on save" question
  turned on — so it is written down here rather than quietly corrected.

## Decision

### 1. One Cook button, over whatever project the Forge is pointed at

The Forge has **no mod concept whatsoever** today — nothing in `tools/forge/`
mentions one. It cooks the base game only because in a dev build the project
*is* the base game. Rather than inventing a mod picker in stage U and then
possibly rebuilding it in stage V, the button keeps cooking "the project", and
**stage V is where what a project points at is decided**.

⚑ The load-bearing observation, because it removes work rather than deferring
it: **a modder never needs the Forge to cook the base game.** Those cooked
assets arrive with the game install. Even a retexture mod, which replaces a
base-game stem, cooks into its own `cooked/` — that is what stage S's search
order means. So "cook the base game as well" is a *developer* convenience that
already works, not a capability the programme's exit criterion needs.

**Rejected:** a mod picker in stage U (front-loads V's concept into U, and
risks building the selection UI twice). **Rejected:** base game only, forever
(contradicts the exit criterion, which is a person who has never built this
repo authoring a mod).

### 2. A cook does not happen on save

At 230 ms a cook on save would be tolerable *today*. It is refused because of
how it scales: a `.forge` **always** re-cooks, deliberately — a staleness
check that cannot know how many LOD outputs to look for is exactly how a stale
level survives a re-cook and gets drawn at distance — so the cost is linear in
the number of part trees and never falls. A sixty-mesh project is ~1.7 s, and
it would land on the action an author repeats most.

Cook stays one click on chrome that cannot be moved, closed or docked away.

**Rejected:** cook on save (one action instead of two, but the hitch grows
with the project and lands on the most repeated action). **Noted as a
different feature, not adopted:** cooking only the saved asset, ~29 ms and
imperceptible — it needs a single-asset entry point `sol_cooker_lib` does not
have, and it is worth raising again if authors start reporting the two-step as
friction rather than being asked to predict whether they will.

### 3. A project is a directory

**Three reasons, and the third is the one that decides it.** The Forge already
derives everything it knows from three directories, so a project *file* would
be a fourth thing to keep in sync with them. `game/mods/README.md` already
defines a mod as a directory with a fixed layout, so **a mod is already a
project** — a file would give mods two identities. And the game finds mods by
scanning first-level subdirectories with no manifest of any kind, so a project
file would exist only for the tool and be **invisible to the thing that
actually loads the mod**.

**Rejected:** a project file (buys somewhere to put settings a directory
cannot carry, at the cost of inventing a format the game ignores). ⚑ The one
thing a directory genuinely cannot hold is *which game install to test
against*; that is a Forge setting and belongs beside `forge.ini`, not in a
per-project file. Stage V should place it there rather than reaching for a
manifest.

## Consequences

- Stage U builds no mod-selection UI. Stage V owns the project concept whole.
- Stage V's project is a directory containing `assets/`, `data/` and
  optionally `mods/` — which is what `game/mods/README.md` already documents a
  mod as, so the two definitions converge rather than compete.
- `SOL_MODEL_DATA_DIR` still has no executable-relative fallback
  (`tools/forge/src/main.cpp:490`) and none of the Forge's three path defines
  is gated on `SOL_DEV_DATA_PATHS`. That remains stage V's shipping defect to
  fix, unchanged by this checkpoint.
- The written stage order is re-confirmed: **`24-S → 24-T → 25-A → 25-B →
  25-C → 24-U → 25-D → 25-E → 24-V`**. Phase 25 stages A–C depend on nothing
  in Phase 24, and both claims they rest on were spot-checked and hold —
  `static_assert(sizeof(PushConstants) == 128)` is real, and
  `rhi::createTextureSetLayout` is the engine's only descriptor-set-layout
  factory, used by four renderers.
