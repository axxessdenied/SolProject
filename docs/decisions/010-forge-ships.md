# 010 — The Forge ships: the editor becomes something a player receives

- **Date**: 2026-08-26
- **Status**: accepted

## Context

Phase 9's decision 1, taken before a line of the Forge existed, was **"a
separate in-engine editor executable, not a mode inside the game — the tool
gets the real renderer, real lighting and real scale, and the shipping binary
carries no editor."** For seventeen phases that promise was a comment in
`tools/forge/CMakeLists.txt` and nothing more.

Phase 22 stage C turned it into a test. `packaging/check_layout.cmake` forbids
`forge*` and `cooker*` in an installed tree, and its own comment explains why
the negative half needed a test at all: *"what the package must NOT contain is
enforced in game/CMakeLists.txt by omission, and an omission is invisible.
Someone adding an install(TARGETS) line for the Forge would break a promise
this project has made since Phase 9 — and nothing else in the tree would
notice."*

Phase 24 was then picked by the user with a purpose stated in the request
itself: *"that way when the end-user gets a hold of forge they will be able to
use it as well to develop mods for the game."* **That purpose and that
exclusion cannot both stand.** The conflict was surfaced as an explicit
reversal to be approved rather than deleted in passing, because the exclusion
is a decision with a test behind it and not an accident. Decided by the user
2026-08-26: **ship the Forge.**

## Decision

**The Forge is distributed to end users**, and Phase 9's *"the shipping binary
carries no editor"* is narrowed to what it was always actually protecting:
**the GAME binary carries no editor.**

Both halves matter and the second is what keeps the original promise intact:

- **The game package is unchanged.** `sol` still ships with no ImGui window, no
  editor code path, and — since Phase 23 — no dev UI on screen at boot.
  `check_layout.cmake` keeps forbidding `forge*` in the **game** tree, so the
  test that guards the old promise is *retained*, not deleted.
- **The Forge ships as its own package**, a separate CPack artifact rather than
  extra files inside the game's. This is the one sub-call the approval did not
  spell out, and it is made this way for three reasons: a player who will never
  mod should not download an editor or be asked what the second executable is;
  the game package's layout test stays meaningful precisely because the game
  package stays exactly the game; and the Forge needs a **project directory**
  concept (Phase 24 stage V) that a player's install does not have and should
  not grow.

**`check_layout.cmake` gains a second assertion rather than losing its first**:
what a *tool* package must contain, and what it must not. The Forge package
does not carry the game's saves, the game's data, or a GLSL compiler
(`011-mod-shaders-spirv.md`).

## Alternatives considered

**Keep the exclusion; mods are authored only by people who build the repo.**
The status quo, and the honest reading of it is that "moddable" would then mean
"moddable by C++ developers with a Vulkan SDK". Rejected because it makes
Phase 24 pointless: every authoring surface the Forge gains would be authored
into a wall for everyone outside this repo, which is the exact failure the
phase exists to remove.

**Ship the Forge inside the game package.** Simpler — one archive, one
`cpack` — and rejected for the three reasons under the decision. The deciding
one is the test: if the editor lives in the game tree, `check_layout.cmake`
can no longer assert that the game tree is only the game, and the guard that
caught this whole question in the first place is the guard that gets weakened.

**Ship a cut-down Forge — viewer only, no editing.** Rejected as the worst of
both: it carries ImGui, the renderer and the size, and delivers none of the
authoring the request was about.

## Consequences

- **AGENTS §5's Dear ImGui row stays literally true and becomes easy to
  misread.** It says *"never shipping game UI"*, and the Forge is not game UI
  — but ImGui now reaches end users inside a distributed binary. The row is
  annotated to point here so a future reader does not have to reconstruct the
  distinction. **The rule being protected is "the game's UI is first-party",
  and that is unchanged.**
- **A second package means a second thing that can rot**, and it rots silently:
  nothing today builds or tests the Forge in a shipping configuration. Phase 24
  stage V owns making `release`/`linux-release` produce and check it, and the
  standing packaging qualifier applies in full — **unpacking outside the repo
  is still the same machine, and nothing short of a copy to another machine
  proves any of it.**
- **The Forge's baked absolute paths become a shipping defect rather than a
  cosmetic one.** `SOL_MODEL_DATA_DIR` has no executable-relative fallback and
  is used raw (`tools/forge/src/main.cpp:489`), and none of the three path
  defines is gated on `SOL_DEV_DATA_PATHS`. Today that bakes this machine's
  directory names — including a user name — into any release build of the
  tool. Phase 22 found and fixed exactly this for `sol`; stage V owes the same
  for `forge`.
- **What "the game" means as a distributable is now two answers, and both need
  saying in the README.** A player downloads one archive; a mod author
  downloads two.
- **Reversal condition**: if the Forge turns out to be unshippable for a reason
  this decision did not anticipate — a licence, a size, a support burden — the
  fallback is not the old exclusion but a **source-only tool**: the repo builds
  it, the package does not carry it. That is strictly worse for the stated
  purpose and should be argued for on evidence, not assumed.
