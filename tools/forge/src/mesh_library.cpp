#include "mesh_library.hpp"

#include "gltf.hpp"
#include "texture.hpp"

#include "sol/assets/forge_doc.hpp"
#include "sol/assets/texture_doc.hpp"
#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace forge {

using namespace sol;

namespace {

[[nodiscard]] bool endsWith(const std::string& text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] std::string fileName(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] std::string fileStem(const std::string& path)
{
    std::string name = fileName(path);
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

void collect(const std::string& directory, const char* extension, const char* tag,
             std::vector<AssetEntry>& out)
{
    std::vector<std::string> files = platform::listFiles(directory.c_str());
    std::sort(files.begin(), files.end());
    for (const std::string& path : files) {
        if (endsWith(path, extension)) {
            out.push_back({fileName(path) + tag, path, fileStem(path)});
        }
    }
}

} // namespace

std::vector<AssetEntry> listMeshes(const std::string& sourceDirectory,
                                   const std::string& cookedDirectory)
{
    std::vector<AssetEntry> entries;
    // `.forge` first: it is the only kind of source that can be EDITED here,
    // and the others are what an asset came out as rather than what it is.
    collect(sourceDirectory, ".forge", "  (parts)", entries);
    collect(sourceDirectory, ".gltf", "  (source)", entries);
    collect(sourceDirectory, ".glb", "  (source)", entries);
    collect(cookedDirectory, ".smesh", "  (cooked)", entries);
    return entries;
}

std::vector<AssetEntry> listTextures(const std::string& sourceDirectory,
                                     const std::string& cookedDirectory)
{
    std::vector<AssetEntry> entries;
    collect(sourceDirectory, ".tex", "  (source)", entries);
    collect(cookedDirectory, ".stex", "  (cooked)", entries);
    return entries;
}

bool isPartSource(const AssetEntry& entry)
{
    return endsWith(entry.path, ".forge");
}

bool isTextureSource(const AssetEntry& entry)
{
    return endsWith(entry.path, ".tex");
}

bool loadTexture(const AssetEntry& entry, assets::TextureData& out, std::string* error)
{
    if (!isTextureSource(entry)) {
        if (assets::loadTexture(entry.path.c_str(), out)) {
            return true;
        }
        if (error != nullptr) {
            *error = "cannot load " + entry.label;
        }
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(entry.path.c_str(), bytes)) {
        if (error != nullptr) {
            *error = "cannot read " + entry.path;
        }
        return false;
    }
    assets::TextureDoc doc;
    std::string parseError;
    if (!assets::parseTexture(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                              entry.path.c_str(), doc, &parseError)) {
        SOL_LOG_ERROR("forge: %s", parseError.c_str());
        if (error != nullptr) {
            *error = parseError;
        }
        return false;
    }
    return buildTextureData(doc, out, error);
}

bool buildTextureData(const assets::TextureDoc& doc, assets::TextureData& out, std::string* error)
{
    assets::TextureImage image;
    std::string buildError;
    if (!assets::buildTexture(doc, image, &buildError)) {
        SOL_LOG_ERROR("forge: %s", buildError.c_str());
        if (error != nullptr) {
            *error = buildError;
        }
        return false;
    }

    cooker::ImageRgba rgba;
    rgba.width = image.width;
    rgba.height = image.height;
    rgba.pixels = std::move(image.pixels);
    out = cooker::encodeTexture(rgba);
    return true;
}

bool loadMesh(const AssetEntry& entry, assets::MeshData& out)
{
    if (isPartSource(entry)) {
        std::vector<std::uint8_t> bytes;
        if (!platform::readFileBytes(entry.path.c_str(), bytes)) {
            return false;
        }
        assets::ForgeDoc doc;
        std::string error;
        if (!assets::parseForge(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                                entry.path.c_str(), doc, &error)) {
            SOL_LOG_ERROR("forge: %s", error.c_str());
            return false;
        }
        if (!assets::buildForge(doc, out, &error)) {
            SOL_LOG_ERROR("forge: %s", error.c_str());
            return false;
        }
        return true;
    }
    if (endsWith(entry.path, ".gltf") || endsWith(entry.path, ".glb")) {
        return cooker::importGltf(entry.path.c_str(), out);
    }
    return assets::loadMesh(entry.path.c_str(), out);
}

MeshReport reportMesh(const assets::MeshData& data)
{
    // Welding is what turns a draw buffer into a surface: the topology
    // questions below (manifold, closed, border edges) are meaningless over
    // unshared corners, where every triangle is its own island.
    const assets::EditMesh mesh = assets::toEditMesh(data);
    const assets::MeshAdjacency adjacency = assets::buildAdjacency(mesh);
    const assets::MeshBounds box = assets::bounds(mesh);

    MeshReport report;
    report.renderVertices = static_cast<std::uint32_t>(data.vertices.size());
    report.positions = static_cast<std::uint32_t>(mesh.positions.size());
    report.triangles = mesh.triangleCount();
    report.boundsMin = box.min;
    report.boundsMax = box.max;
    report.boundingRadius = assets::boundingRadius(mesh);
    report.surfaceArea = assets::surfaceArea(mesh);
    report.signedVolume = assets::signedVolume(mesh);
    report.manifold = adjacency.isManifold();
    report.closed = adjacency.isClosed();
    report.borderEdges = adjacency.borderEdgeCount();
    report.cacheMissRatio = assets::averageCacheMissRatio(mesh);
    return report;
}

bool ModelMatch::radiusAgrees() const
{
    const float tolerance = std::max(1e-4f, std::fabs(authoredRadius) * 0.001f);
    return std::fabs(radiusDelta) <= tolerance;
}

float ModelMatch::radiusDeltaPercent() const
{
    if (authoredRadius == 0.0f) {
        return 0.0f;
    }
    return (radiusDelta / authoredRadius) * 100.0f;
}

bool loadModelCatalog(const std::string& dataDirectory, assets::DefDatabase& out, std::string* error)
{
    out.clear();
    if (dataDirectory.empty()) {
        return true;
    }
    return out.mergeDirectory(dataDirectory.c_str(), error);
}

std::vector<ModelMatch> matchModels(const assets::DefDatabase& defs, const AssetEntry& entry,
                                    const MeshReport& report)
{
    std::vector<ModelMatch> matches;
    for (const assets::ModelDef& model : defs.models()) {
        if (model.mesh != entry.stem) {
            continue;
        }
        ModelMatch match;
        match.id = model.id;
        match.texture = model.texture;
        match.authoredRadius = model.radius;
        match.authoredAvoidRadius =
            model.avoidRadius > 0.0f ? model.avoidRadius : model.radius;
        match.emissive = model.emissive;
        match.solid = model.solid;
        match.radiusDelta = report.boundingRadius - model.radius;
        matches.push_back(std::move(match));
    }
    return matches;
}

} // namespace forge
