# example-systems — a mod that adds places to the galaxy

One file, `systems.toml`, and no change to anything in `game/data`. It adds
five systems: **the Sable Chain**, a four-system constellation placed as a unit
with the lanes between its members intact, and **Sable Watch**, an ordinary
authored system placed by gate distance from the chain's mouth.

Read `systems.toml` itself — it is written to be read, and it documents the
`[[constellation]]` def kind row by row. The `[[system]]` schema is documented
the same way in `game/data/systems.toml`.

## It is in the repository and NOT in the shipped package

Every first-level subdirectory of `game/mods/` is an **active layer** with no
enable/disable anywhere in the game, so a mod checked in here would change the
galaxy of every player who installs a build. This one is excluded from the
install rule by name (`game/CMakeLists.txt`), which is `docs/decisions/018`'s
decision 6, taken before it was written.

The consequence, stated rather than left to be rediscovered: **the shipped game
does not demonstrate the modding path it advertises.** `mods/` still ships
empty-but-present, for the reason `game/mods/README.md` gives.

## Running it

A dev build reads `game/mods/` straight from the source tree, so it is already
active — the boot log's system and lane counts are five systems and several
lanes higher than the base game's, and `sol.system_by_id("example.sable_end")`
in the dev console returns a real index.

To take it out, delete the directory or move it aside and restart. Editing
`systems.toml` needs a restart too: def hot-reload reaches the parser and
deliberately not the galaxy.

⚑ **A save records a digest of the authored content it was made against and
refuses to load against a different one.** Installing or removing this mod
mid-campaign therefore rejects the old saves cleanly instead of loading them
into a galaxy that has quietly reshaped around them.
