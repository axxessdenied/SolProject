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

    std::vector<std::uint8_t> bytes(sizeof(header) + mesh.vertices.size() * sizeof(assets::MeshVertex) +
                                    mesh.indices.size() * sizeof(std::uint32_t));
    std::uint8_t* cursor = bytes.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    if (!mesh.vertices.empty()) {
        std::memcpy(cursor, mesh.vertices.data(), mesh.vertices.size() * sizeof(assets::MeshVertex));
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
    if (!assets::parseForge(
            reinterpret_cast<const char*>(sourceBytes.data()), sourceBytes.size(), path, doc, error)) {
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

std::string meshLevelPath(const std::string& outputPath, std::uint32_t level)
{
    if (level == 0) {
        return outputPath;
    }
    const std::size_t dot = outputPath.find_last_of('.');
    const std::string stem = dot == std::string::npos ? outputPath : outputPath.substr(0, dot);
    const std::string extension = dot == std::string::npos ? std::string() : outputPath.substr(dot);
    return stem + ".lod" + std::to_string(level) + extension;
}

bool writeMeshLevels(const assets::MeshData& level0,
                     const assets::LodChain& chain,
                     const std::string& outputPath,
                     std::uint32_t& levelsWritten,
                     std::string* error)
{
    levelsWritten = 0;

    const auto write = [&](const assets::MeshData& mesh, std::uint32_t level) {
        const std::string path = meshLevelPath(outputPath, level);
        const std::vector<std::uint8_t> bytes = encodeMesh(mesh);
        if (!platform::writeFileBytes(path.c_str(), bytes.data(), bytes.size())) {
            if (error != nullptr) {
                *error = "cannot write " + path;
            }
            return false;
        }
        ++levelsWritten;
        return true;
    };

    // ⚑ Level 0 is written from the caller's own mesh, untouched by anything
    // in stage F. The bytes the game already loads must not move: `cooker.unit`
    // proves a committed `.forge` reaches the game as the mesh its author
    // built, and E3's bake-save-recook recipe compares a `.smesh` hash.
    if (!write(level0, 0)) {
        return false;
    }
    for (std::size_t i = 0; i < chain.levels.size(); ++i) {
        if (!write(chain.levels[i].mesh, static_cast<std::uint32_t>(i + 1))) {
            return false;
        }
    }

    // Anything above what this cook produced is left over from a previous one.
    for (std::uint32_t level = levelsWritten; level <= kMaxMeshLevels; ++level) {
        const std::string path = meshLevelPath(outputPath, level);
        if (platform::fileModificationTime(path.c_str()) == 0) {
            continue; // not there, which is the normal case
        }
        if (!platform::deleteFile(path.c_str())) {
            if (error != nullptr) {
                *error = "cannot delete stale level " + path;
            }
            return false;
        }
    }
    return true;
}

} // namespace sol::cooker
