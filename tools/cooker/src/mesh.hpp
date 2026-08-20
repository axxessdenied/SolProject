#pragma once

// The mesh cook, in the library rather than in `main.cpp`.
//
// ⚑ IT LIVES HERE BECAUSE OF WHERE IT DID NOT. `sol_cooker_lib` is what
// `cooker.unit` links, and until this slice every `cook*` function and the
// `.smesh` writer sat in the cooker's `main.cpp` - outside it. That is why the
// D checkpoint's gap, "nothing compares the cooked `.smesh` against anything",
// survived three slices of being written down as still open: no amount of
// diligence inside the test suite could reach the code it was about.
//
// ⚑ The shape is the SOUND path's, not a new one. `encodeSound` is a pure
// function returning bytes and `main.cpp` does nothing but write them, which is
// exactly why the sound format has a round-trip test and the mesh format never
// did. The asymmetry was the bug.

#include "sol/assets/asset_loader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::cooker {

// Serialises to the .smesh layout in sol/assets/formats.hpp: header, then
// vertices, then indices.
//
// ⚑ The header's `magic` and `version` come from `MeshFileHeader`'s own default
// member initialisers rather than being set here, and `assets::loadMesh`
// validates both - so a future explicit constructor or a `= {0}` would produce
// files the game rejects while the cooker reported success. `cooker.unit` pins
// it.
[[nodiscard]] std::vector<std::uint8_t> encodeMesh(const assets::MeshData& mesh);

// Reads a `.forge` part tree and evaluates it (engine plan Phase 9 stage D).
// Unlike a glTF this is a SOURCE file - the shape the mesh is built from rather
// than the triangles it came out as - so the cook is an evaluation rather than
// an import.
//
// Split from the write so a test can drive the same three steps the cooker
// does instead of restating them, which would be a second description of the
// cook standing beside the first.
[[nodiscard]] bool importForgeMesh(const char* path, assets::MeshData& out, std::string* error);

} // namespace sol::cooker
