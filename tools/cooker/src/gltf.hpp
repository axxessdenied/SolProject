#pragma once

#include "sol/assets/asset_loader.hpp"

#include <cstdint>
#include <vector>

namespace sol::cooker {

// Imports a .gltf (JSON, external or data-URI buffers) or .glb file.
// All mesh primitives in the default scene are baked through their node
// transforms and merged into one mesh.
[[nodiscard]] bool importGltf(const char* path, assets::MeshData& out);

// Exposed for tests.
[[nodiscard]] bool decodeBase64(const char* text, std::size_t length, std::vector<std::uint8_t>& out);

} // namespace sol::cooker
