#pragma once

#include "sol/assets/asset_loader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::cooker {

// One mesh-bearing node of an imported glTF, with its node transform already
// baked in and its name carried through (engine plan Phase 9 stage L).
struct GltfPart
{
    // The glTF node's name, then its mesh's, then empty. This is the Blender
    // object name in practice, and it is what a `.forge` part id is derived
    // from - which is how a re-import knows which part it is replacing.
    std::string name;

    // ⚑ The exporting tool's own identity for this object, read from the node's
    // `extras.sol_forge_uid` and EMPTY when the file does not carry one (stage
    // P). It is what survives a rename, which the name by definition does not -
    // so a re-import can tell "this object was renamed" from "this is a new
    // object", which are the same event when you only have the name.
    std::string originId;

    assets::MeshData mesh;
};

// Every node in the default scene that carries triangles, kept SEPARATE.
//
// ⚑ The separation is the whole reason this exists beside `importGltf`. The
// cooker wants one buffer to write; the Forge's import wants a PART TREE, and a
// merged soup would hand an author one opaque part with the Parts panel
// useless - which is most of what they came to the tool for.
[[nodiscard]] bool importGltfParts(const char* path, std::vector<GltfPart>& out);

// Imports a .gltf (JSON, external or data-URI buffers) or .glb file.
// All mesh primitives in the default scene are baked through their node
// transforms and merged into one mesh.
//
// ⚑ Implemented as a merge over `importGltfParts`, so the two cannot disagree
// about what a node means.
[[nodiscard]] bool importGltf(const char* path, assets::MeshData& out);

// Writes a mesh back out as a self-contained .gltf with a base64 data-URI
// buffer - the layout the assets in this repo already use.
//
// ⚑ This is an INTEROP action, not a pipeline step (engine plan Phase 9 stage
// D). `.forge` is the source an asset is edited from; a glTF is what another
// program can open, and round-tripping a game asset out through it and back in
// would lose the part tree that makes it editable at all.
//
// Indices are written as UNSIGNED_INT. The PowerShell generator this replaces
// wrote UNSIGNED_SHORT, which put a 65,535-vertex ceiling on an authored mesh
// that neither the .smesh format nor the pipeline ever had.
[[nodiscard]] std::string exportGltf(const assets::MeshData& mesh, const char* name);

// Exposed for tests.
[[nodiscard]] bool decodeBase64(const char* text, std::size_t length, std::vector<std::uint8_t>& out);
[[nodiscard]] std::string encodeBase64(const std::uint8_t* data, std::size_t length);

} // namespace sol::cooker
