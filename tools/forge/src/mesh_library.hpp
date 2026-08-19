#pragma once

// What the Forge can open, and what it says about it (engine plan Phase 9
// stage C). The numbers here are the ones a `[[model]]` row and a collision
// sphere are built from, which is why the viewer reports them beside the
// picture rather than leaving them to be guessed from it.

#include "sol/assets/asset_loader.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/core/math/vec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace forge {

// One openable file on disk. `label` is what the list shows; the extension is
// what decides which loader runs.
struct AssetEntry
{
    std::string label;
    std::string path;
};

// Authored `.gltf` under the source tree first, then cooked `.smesh` beside
// the executable - in that order because the authored file is the one an
// author has just changed, and the cooked one is a build behind it.
[[nodiscard]] std::vector<AssetEntry> listMeshes(const std::string& sourceDirectory,
                                                 const std::string& cookedDirectory);

// Cooked `.stex` beside the executable. A mesh does not name its texture (that
// is the `[[model]]` row's job, and stage H's), so the viewer lets one be
// picked.
[[nodiscard]] std::vector<AssetEntry> listTextures(const std::string& cookedDirectory);

// Dispatches on extension: `.gltf`/`.glb` through the cooker's importer,
// `.smesh` through the runtime loader. Both give the same buffer the game
// would draw, which is the point.
[[nodiscard]] bool loadMesh(const AssetEntry& entry, sol::assets::MeshData& out);

// Everything the viewer prints about a mesh.
//
// ⚑ `renderVertices` and `positions` are different numbers and the gap between
// them is information, not noise: a corner carries a normal and a uv, so a
// hard edge splits one point into several corners. A cube is 8 positions under
// 24 corners; a smooth revolve is nearly 1:1.
struct MeshReport
{
    std::uint32_t renderVertices = 0;
    std::uint32_t positions = 0;
    std::uint32_t triangles = 0;
    sol::core::Vec3 boundsMin{};
    sol::core::Vec3 boundsMax{};
    // The radius a `[[model]]` row would carry: the furthest point from the
    // origin the mesh is authored around.
    float boundingRadius = 0.0f;
    double surfaceArea = 0.0;
    double signedVolume = 0.0;
    bool manifold = false;
    bool closed = false;
    std::uint32_t borderEdges = 0;
    float cacheMissRatio = 0.0f;
};

[[nodiscard]] MeshReport reportMesh(const sol::assets::MeshData& data);

} // namespace forge
