#pragma once

// What the Forge can open, and what it says about it (engine plan Phase 9
// stage C). The numbers here are the ones a `[[model]]` row and a collision
// sphere are built from, which is why the viewer reports them beside the
// picture rather than leaving them to be guessed from it.

#include "sol/assets/asset_loader.hpp"
#include "sol/assets/data_defs.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/core/math/vec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace forge {

// One openable file on disk. `label` is what the list shows; the extension is
// what decides which loader runs; `stem` is what a `[[model]]` row names, which
// is how a mesh on disk is tied to the content that uses it.
struct AssetEntry
{
    std::string label;
    std::string path;
    std::string stem;
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

// A `.forge` part tree: the only kind of asset this tool can edit rather than
// just open.
[[nodiscard]] bool isPartSource(const AssetEntry& entry);

// Dispatches on extension: `.forge` is parsed and evaluated, `.gltf`/`.glb` go
// through the cooker's importer, `.smesh` through the runtime loader. All three
// give the same buffer the game would draw, which is the point.
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

// A `[[model]]` row that names the open mesh, with the authored numbers set
// beside the measured ones.
//
// ⚑ This exists because stage C had to find two of these by EYE. The asteroid's
// mesh measures 1.1584 m against an authored 1.0 and the station's 102.0 against
// 100.0, so both collision spheres sit inside their own hulls - true since those
// assets were written, recorded nowhere, and noticed only when a human finally
// looked at the numbers side by side. A viewer that can measure a radius and
// cannot read the authored one is asking to have that happen again.
struct ModelMatch
{
    std::string id;
    std::string texture;
    float authoredRadius = 0.0f;
    // 0 in the row means "the same as radius"; resolved here so the panel does
    // not have to know that rule.
    float authoredAvoidRadius = 0.0f;
    float emissive = 0.0f;
    bool solid = true;
    // measured - authored, in metres. POSITIVE means the sphere the sim builds
    // is smaller than the hull that is drawn: ships pass through the picture.
    float radiusDelta = 0.0f;

    // ⚑ Agreement is RELATIVE, not absolute, and the gate is why: it authors
    // 106.7 against a measured 106.701, which is somebody rounding a number
    // they read off this very panel. A millimetre on a 107 m ring is not a
    // finding; the same millimetre on a 0.6 m part might be. One tenth of one
    // percent separates the four real mismatches in this game (2%, 12%, 13%,
    // 16%) from that one rounding by three orders of magnitude.
    [[nodiscard]] bool radiusAgrees() const;
    // Signed, as a percentage of the authored radius.
    [[nodiscard]] float radiusDeltaPercent() const;
};

// Reads the `[[model]]` rows out of the game's data directory.
//
// ⚑ The tool reads the game's DATA and never its CODE, which is the line
// AGENTS.md 4 draws; `sol::assets::DefDatabase` is engine, and models.toml is a
// file. A missing directory is not an error - an installed tool without the
// source tree beside it simply reports no authored rows.
[[nodiscard]] bool loadModelCatalog(const std::string& dataDirectory,
                                    sol::assets::DefDatabase& out, std::string* error);

// Every row whose `mesh` stem is this asset's, which is usually one and is
// deliberately allowed to be several: six models already share five meshes.
[[nodiscard]] std::vector<ModelMatch> matchModels(const sol::assets::DefDatabase& defs,
                                                  const AssetEntry& entry,
                                                  const MeshReport& report);

} // namespace forge
