# Mods

Each **first-level subdirectory** of this directory is one mod. They are laid
over the base game's `game/data` in name order, so `mods/zebra/ships.toml`
beats `mods/alpha/ships.toml`, and both beat `game/data/ships.toml`. Ids
replace wholesale rather than merging field by field, and a layer that fails
to parse leaves the previous state intact rather than half-applying.

## What a mod directory holds

```
mods/my-mod/
  ships.toml        any *.toml def file, mirroring game/data
  models.toml
  scripts/init.lua  boot script, run in layer order
  assets/           SOURCE assets (.forge, .tex, .png, .wav, .glb …)
  cooked/           what the game actually loads (.smesh, .stex, .saud …)
  shaders/          compiled SPIR-V a [[material]] row names (.spv)
```

Only the parts you want are needed. A mod that changes a price is one
`commodities.toml`; a mod that adds a ship needs `cooked/` too.

## Assets, and why there are two directories

The game loads **cooked** assets — `.smesh`, `.stex`, `.saud` — not the files
you author. Cooking is what turns a `.forge` part tree into triangles, a `.tex`
document or a `.png` into BC1 with a mip chain, and a `.wav` into PCM. So a mod
that ships art ships `cooked/`, and keeps `assets/` beside it so the mod stays
editable by whoever has it next.

Cook a mod the same way the build cooks the base game:

```
cooker mods/my-mod/assets mods/my-mod/cooked
```

**Search order is the reverse of the layer order.** An asset is *found*, not
merged, so the game looks in the last-named mod's `cooked/` first, then earlier
mods, then the base game's. That is the same precedence the def files get, said
the other way round: `mods/zebra/cooked/hull.stex` wins, and a mod that ships a
stem the base game already uses replaces it everywhere it is drawn.

Names are keyed on the file **stem**, which is what a `[[model]]` row's `mesh`
and `texture` and a `[[sound]]` row's `asset` name. There is no namespacing —
shipping `hull.stex` means replacing `hull` for the whole game, deliberately,
because that is how a retexture mod works. Prefix your stems (`mymod_hull`) if
you meant to add rather than replace.

## When something is missing

A `[[model]]` row whose mesh or texture is in no layer's `cooked/` **draws
nothing**, and the log names the model, the stem and every directory searched.
The game still boots and the entity still exists — it collides, it targets, it
shows on the map. One broken mod does not stop the game starting.

A `[[sound]]` cue whose `.saud` is missing is skipped with the same kind of
warning, which it has always done.

**The one exception is the base game's own `cooked/`.** If that directory is
empty the game refuses to start and says so: an install missing its assets is
not a mod problem, and booting into an invisible galaxy would blame the wrong
thing.

## One constraint worth knowing before you hit it

**The UI font is replaceable, and its style names are not.** `ui.sfont` is
resolved through the same search, so a mod can replace it — but `hud`, `body`,
`body_strong` and `heading` are read by name throughout the interface, and a
replacement that drops one leaves that text unrenderable. Nothing checks this.

## Materials and shaders

A `[[material]]` row says how a surface is drawn — the texture it samples, the
shader pair that runs, and the pipeline state it runs under — and a `[[model]]`
row names one. Both are ordinary def rows, so a mod adds a material the same way
it adds a ship: a `*.toml` file in the mod directory.

What is different is where the shader itself lives:

```
mods/my-mod/
  materials.toml    [[material]] id = "mymod.glass", fragment_shader = "glass"
  shaders/glass.frag       the GLSL you edit          (convention)
  shaders/glass.frag.spv   what the game loads        (required)
```

`shaders/` is searched exactly like `cooked/` — last-named mod first, then the
base game's own `shaders/` — so a material can name one of the engine's shader
stems (`mesh`, `membrane`, `cockpit`) and only bring the stage it is replacing.
A material names its stems, and the engine appends `.vert.spv` / `.frag.spv`.

**Shaders are the one asset kind with a toolchain prerequisite.** A mod ships
compiled SPIR-V (`.spv`); no GLSL compiler is distributed with the game or the
Forge, so writing one needs the Vulkan SDK installed:

```
glslc --target-env=vulkan1.3 shaders/glass.frag -o shaders/glass.frag.spv
```

Ship your `.frag` beside the `.spv` so the mod stays editable by whoever has it
next — that is a convention, not a mechanism. See
`docs/decisions/011-mod-shaders-spirv.md`. Meshes, textures, sounds and def rows
need no SDK at all.

**A mod cannot replace the engine's own seven pipelines**, only the ones a
material names. `tonemap`, `sky`, `ui`, `particle`, `impostor` and `debug_line`
resolve from the install's `shaders/` and never from a mod. The reason is where
the checking is: a material's shader is checked against the row that declares it
and a mismatch is refused by name, while those seven have no declaration to
check against — their descriptor sets and push blocks are contracts written in
C++, so a swapped `.spv` there is undefined behaviour that only the validation
layer would catch, and that is off in the build a player runs.

**When a shader is missing or does not match**, the material draws nothing and
every model using it draws nothing, with both named in the log along with every
directory searched. The game still boots — one broken mod does not stop it,
exactly as with a missing mesh.

## Why this file exists

Two reasons, and the second one is the interesting one.

Git cannot track an empty directory, so without a file here a fresh clone has
no `game/mods` at all — the same trick `blender-inbox/README.md` uses.

And it had no file here, for seventeen phases. `game/CMakeLists.txt` has baked
`SOL_MODS_SOURCE_DIR` to this path since Phase 5, and this directory has never
existed in the repository. Nothing ever complained, because `listFiles` on a
missing directory returns an empty vector in silence: **the layering being
broken and the layering working with no mods installed produce byte-identical
behaviour.** Phase 22 found it while writing the install rules.

This file is skipped by the layer scan rather than treated as a mod — a path
directly in `mods/` has no `/` after the prefix and only subdirectories are
considered (`game/src/asset_paths.cpp`, `modLayerNames`).
