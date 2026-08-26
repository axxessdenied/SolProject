# Send to Forge — the Blender bridge

Model in Blender, press one button, and the mesh is open in the Forge as an
editable `.forge` part tree (engine plan Phase 9 stage L).

## Install

1. Blender ▸ **Edit ▸ Preferences ▸ Add-ons ▸ Install from disk…**
2. Pick `tools/blender/sol_forge_bridge.py` and enable **Send to Forge (SolProject)**.
3. Expand its preferences and set **SolProject root** to this repository's root —
   the directory holding `assets/` and `tools/`. It checks for `assets/meshes`
   and refuses politely if you point it somewhere else.

Verified against **Blender 5.2.0 LTS**. The exporter's argument list changes
between versions, so the addon filters what it passes against the operator's own
RNA rather than naming keywords literally — one file, no per-version branches.

## Use

Leave the Forge running. Then either:

- **View3D sidebar** (`N`) ▸ **Forge** tab ▸ **Send to Forge**, or
- **File ▸ Export ▸ Send to Forge (SolProject)**

The dialog's **Asset name** is the important field: it decides which
`assets/meshes/<name>.forge` this becomes, and **re-sending the same name
updates that asset in place**. It defaults to the `.blend`'s name.

The Forge polls its inbox about twice a second, so the mesh appears without
alt-tabbing back to press anything.

## What arrives

| In Blender | In the Forge |
|---|---|
| One object | One `.forge` part, named after it (`Fin.001` → `Fin_001`) |
| Object transform | Baked into the vertices, so parts do not sit on top of each other |
| Modifiers | Applied — a Subdivision Surface exports as the surface, not the cage |
| Z-up | Converted to the engine's Y-up |
| Materials | **Ignored** — a mesh's texture comes from its `[[model]]` row |
| Armatures, animation | **Ignored** — nothing in this engine has a bone |

Each part is literal geometry, the same thing the Forge's `bake` button
produces, so every tool in the Forge works on it immediately: points, edges and
faces, extrude, split, merge, the Report, def rows, LOD levels.

## Re-sending, and what survives it

A second send **replaces the parts it names and leaves everything else alone**.
So this works:

1. Send a hull from Blender.
2. In the Forge, add a `beam` part and save.
3. Change the hull in Blender and send again.

The hull updates; your beam is still there. Parts are matched by **id**, so
renaming an object in Blender makes a new part rather than updating the old one
— rename it in the Forge too, or delete the stale part.

Two things are deliberately *not* preserved on a re-import: a part's
**placement** and its **geometry**. Both are Blender's to own for a part that
came from Blender, and keeping a placement would apply it on top of a transform
already baked into the vertices. Comments above a part, and its `parent`, do
survive.

An import merges into the document you have **open**, not the file on disk, so
unsaved work is not discarded by a re-send — and it lands on the undo stack
like any other edit, so `Ctrl+Z` takes it back.

## Settings the addon pins, and why

None of these are taste:

- **+Y up** — the engine is Y-up, Blender is Z-up.
- **Apply modifiers** — an unapplied Bevel or Subsurf exports as the cage, which
  is not the shape anyone modelled.
- **No Draco** — the importer in this repo has no Draco decompressor, so a
  compressed file fails at read rather than merely looking wrong.
- **GLB** — one self-contained file, and unlike the embedded-`.gltf` format it
  has never been deprecated.
- **No materials, animation, skins, cameras or lights** — nothing downstream
  reads them.

## Where the file goes

`blender-inbox/` at the repository root — **not** under `assets/`, and that is
forced rather than tidy. The cooker scans `assets/` recursively into one flat
output directory keyed on the file stem, so a `ship.gltf` under `assets/` and
`ship.forge` in `assets/meshes/` both cook to `ship.smesh`, and that guard
aborts the **entire** cook. See `blender-inbox/README.md`.

The glTF is transport. Once imported, the `.forge` is the source, and it is the
one that gets committed.

**You do not have to have the Forge running when you press the button.** A drop
waits in `blender-inbox/` until it is imported, and is moved into
`blender-inbox/imported/` once it has been — so opening the Forge later picks up
everything sent while it was shut, and picks up each drop exactly once.
