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
#include "sol/assets/mesh_lod.hpp"

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

// Where a level lives (engine plan Phase 9 stage F). Level 0 is the output path
// itself and level N is `<stem>.lodN.smesh` beside it.
//
// ⚑ SIBLING FILES RATHER THAN SECTIONS OF ONE FILE, AND THE REASON IS A SHARED
// CONSTANT: `formats.hpp` validates `.smesh`, `.stex`, `.sfon` and `.saud`
// against ONE `kFormatVersion`, so a multi-level mesh header would cost either
// a global bump that tells the font and sound loaders their formats changed
// when they did not, or a per-format split. Siblings cost neither, and they
// match `models.toml`'s own standing rule that a mesh reaching the game is a
// cooked file rather than a C++ change.
[[nodiscard]] std::string meshLevelPath(const std::string& outputPath, std::uint32_t level);

// Writes level 0 and every generated level beside it, and DELETES any stray
// level left over from a previous cook.
//
// ⚑ The deletion is not tidiness. A cooked directory is not rebuilt from
// scratch, so an asset that has been edited below the triangle floor - or one
// whose second level stops clearing a band - leaves a `.lod2.smesh` on disk
// that the runtime would happily load and draw at distance. The stale level is
// invisible precisely where nobody is looking closely.
//
// `levelsWritten` counts level 0, so it is never zero on success.
[[nodiscard]] bool writeMeshLevels(const assets::MeshData& level0, const assets::LodChain& chain,
                                   const std::string& outputPath, std::uint32_t& levelsWritten,
                                   std::string* error);

// The highest level number `writeMeshLevels` will look for when clearing
// strays. Higher than any chain this policy can produce, so a level cannot
// hide above the scan.
inline constexpr std::uint32_t kMaxMeshLevels = 8;

} // namespace sol::cooker
