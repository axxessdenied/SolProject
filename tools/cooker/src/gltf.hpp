#pragma once

#include "sol/assets/asset_loader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::cooker {

// Imports a .gltf (JSON, external or data-URI buffers) or .glb file.
// All mesh primitives in the default scene are baked through their node
// transforms and merged into one mesh.
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
