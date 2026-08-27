# Mods

Each **first-level subdirectory** of this directory is one mod. They are laid
over the base game's `game/data` in name order, so `mods/zebra/ships.toml`
beats `mods/alpha/ships.toml`, and both beat `game/data/ships.toml`. Ids
replace wholesale rather than merging field by field, and a layer that fails
to parse leaves the previous state intact rather than half-applying.

A mod directory mirrors `game/data`: `ships.toml`, `weapons.toml`,
`factions.toml`, `scripts/init.lua` and so on. Only the files you want to
change need to be present.

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
directly in `mods/` has no `/` in it and only subdirectories are considered
(`game/src/content.cpp`, `GameContent::initialize`).
