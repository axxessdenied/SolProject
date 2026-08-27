# 011 — Mods ship SPIR-V; the GLSL compiler stays build-time only

- **Date**: 2026-08-26
- **Status**: accepted

## Context

AGENTS §5's dependency table scopes **glslang (or shaderc)** to *"GLSL →
SPIR-V, **build-time only**"*, and the section's standing rule is that
*"adding, upgrading, or expanding the scope of any dependency requires
explicit approval from the user first."*

Phase 25 makes shaders authorable as data, and `010-forge-ships.md` puts the
Forge on end users' machines. Together those turn a dormant question live:
**when a mod author writes a shader, what compiles it, and where does that
compiler live?** A tool that compiles GLSL on a machine outside this repo is a
scope expansion of a build-time dependency, which §5 says cannot happen
quietly.

The question was put to the user with two honest options. Decided 2026-08-26:
**a mod ships SPIR-V.**

## Decision

**A mod's distributable form for a shader is `.spv`, and no GLSL compiler is
distributed by this project.** AGENTS §5's table does not change.

The Forge compiles GLSL by **invoking a compiler the author already has** —
found at configure time, spawned as a process, and simply absent on a machine
without the Vulkan SDK. That is not a new mechanism: `game/src/shader_watcher.cpp`
has done exactly this in dev builds since before Phase 22, down to the failure
path (*"shader hot-reload unavailable (no compiler/source path)"*, and a failed
compile keeps the previous pipeline rather than breaking the frame).

So the toolchain requirement lands on the person writing a shader, and on
nobody else: **a mod author adding meshes, textures, sounds or def rows needs
no SDK at all.**

## Alternatives considered

**Vendor shaderc into the Forge's package.** The convenient answer, and a §5
table change. Rejected on proportion: shaderc pulls glslang and SPIRV-Tools —
hundreds of thousands of lines and tens of megabytes of third-party source —
to spare shader-writing mod authors one SDK install. This project has declined
a far smaller ask on the same grounds (*"SDL3 was offered as an explicit change
to this table and declined"*), and 009's own recorded precedent is that an
approval is *"for a named, bounded use, not as a general opening of the
policy."*

**Compile GLSL at runtime in the game.** Rejected harder than the above: it
puts a compiler inside a player's process to serve a feature only authors use,
and it would make a mod's shader a thing that can fail on load on somebody
else's machine, at their frame rate, with their driver.

**Write a GLSL front-end in this repo.** §5's *"everything else is written in
this repo"* has a limit and a GLSL compiler is orders of magnitude past it.
Named only because the rule invites the question.

## Consequences

- **Shaders become the one asset kind with a toolchain prerequisite**, and that
  asymmetry must be stated wherever modding is documented rather than
  discovered. Meshes, textures, sounds and defs stay SDK-free.
- **A `.spv` is a build output committed into a mod folder, which is a thing
  this repo otherwise refuses to do.** `.forge` is the source and `.smesh` is
  not committed; here the compiled artifact *is* the shipped one, because the
  game cannot consume GLSL and nothing ships a compiler. **The convention that
  goes with it: ship the `.glsl` beside the `.spv`** so the mod stays editable
  by its next owner. That is a convention, not a mechanism — nothing enforces
  it, and a mod that omits its sources is merely unfriendly, not broken.
- **The Forge's shader editing is inert without a compiler and must say so**,
  in the tool, at the moment an author reaches for it. A greyed control with a
  reason beats a button that silently does nothing; `ShaderWatcher`'s log line
  is the precedent for the wording, not for the placement.
- **The engine's SPIR-V loading path is unchanged and stays the only path.**
  `rhi::createShaderModuleFromFile` reads `.spv` and always has; a mod's shader
  arrives through the same call as the game's fourteen.
- **Revisit condition**: if "you need the Vulkan SDK" turns out to be the thing
  that stops mod shaders existing at all, vendoring a compiler into the *tool*
  package — never the game — is the change to argue for, and it needs its own
  approval under §5.
