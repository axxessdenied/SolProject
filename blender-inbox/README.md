# The Blender inbox

Blender drops glTF here; the Forge picks it up and converts it to a `.forge`
part tree under `assets/meshes/`. Nothing in this directory is source, and
nothing in it is committed except this file.

Install the addon from `tools/blender/` and use **Send to Forge** (`File ▸
Export`, or the `Forge` panel in the 3D viewport sidebar). The Forge polls this
directory about twice a second while it is running, so the mesh appears without
alt-tabbing back to press anything — and if the Forge is **not** running, the
drop is imported the next time you open it.

## What is waiting and what is done

**A file in this directory is waiting to be imported. A file in `imported/`
has been.** That is the whole state, and it is a directory rather than a note
somewhere so you can read it without the Forge open.

The glTF is moved into `imported/` the moment its `.forge` is written, which is
what stops it being imported a second time. Nothing prunes `imported/`; delete
what you no longer want. To re-import something, drag it back out here.

A drop that **fails** to import stays put and is listed under the `Blender`
menu with its error. It is retried when you change the file, or next time the
Forge starts — not sixty times a second in between.

## Why this is not under `assets/`

The cooker walks `assets/` **recursively** and writes every output into one flat
directory keyed on the file **stem**. So `ship.gltf` anywhere under `assets/`
and `ship.forge` in `assets/meshes/` both cook to `ship.smesh` — and the
collision guard does not skip that pair, it **aborts the entire cook**:

```
cooker: .../ship.gltf and .../ship.forge both cook to .../ship.smesh
cooker: 1 output collision(s); nothing cooked
```

An inbox under `assets/` would therefore break the whole build the first time an
import succeeded. Keeping the glTF outside it also keeps the story straight:
a glTF is what another program can open, and the `.forge` is what an asset *is*.

## What an import does

- One `.forge` part per Blender **object**, named after it (`Hull.001` becomes
  the part id `Hull_001`).
- Each part is literal geometry — the same thing the Forge's `bake` button
  produces — so every tool in the Forge works on it: points, edges, faces,
  extrude, split, the Report, def rows, LOD levels.
- Re-sending the same file **replaces those parts by id** and leaves everything
  else alone, so a part you added in the Forge survives a re-export.
- It merges into the document you have **open**, not the file on disk, so
  unsaved work is not silently discarded — and the import is undoable like any
  other edit.
