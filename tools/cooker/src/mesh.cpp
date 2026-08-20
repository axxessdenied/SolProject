#include "mesh.hpp"

#include "sol/assets/forge_doc.hpp"
#include "sol/assets/formats.hpp"
#include "sol/platform/file_io.hpp"

#include <cstring>

namespace sol::cooker {

std::vector<std::uint8_t> encodeMesh(const assets::MeshData& mesh)
{
    assets::MeshFileHeader header = {};
    header.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    header.indexCount = static_cast<std::uint32_t>(mesh.indices.size());

    std::vector<std::uint8_t> bytes(sizeof(header) +
                                    mesh.vertices.size() * sizeof(assets::MeshVertex) +
                                    mesh.indices.size() * sizeof(std::uint32_t));
    std::uint8_t* cursor = bytes.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    if (!mesh.vertices.empty()) {
        std::memcpy(cursor, mesh.vertices.data(),
                    mesh.vertices.size() * sizeof(assets::MeshVertex));
        cursor += mesh.vertices.size() * sizeof(assets::MeshVertex);
    }
    if (!mesh.indices.empty()) {
        std::memcpy(cursor, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
    }
    return bytes;
}

bool importForgeMesh(const char* path, assets::MeshData& out, std::string* error)
{
    std::vector<std::uint8_t> sourceBytes;
    if (!platform::readFileBytes(path, sourceBytes)) {
        if (error != nullptr) {
            *error = std::string("cannot read ") + path;
        }
        return false;
    }

    assets::ForgeDoc doc;
    if (!assets::parseForge(reinterpret_cast<const char*>(sourceBytes.data()), sourceBytes.size(),
                            path, doc, error)) {
        return false;
    }
    if (!assets::buildForge(doc, out, error)) {
        return false;
    }
    if (out.vertices.empty() || out.indices.empty()) {
        if (error != nullptr) {
            *error = std::string(path) + " builds no geometry";
        }
        return false;
    }
    return true;
}

} // namespace sol::cooker
