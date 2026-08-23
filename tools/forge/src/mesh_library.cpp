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

void collect(const std::string& directory, const char* extension, const char* group, bool cooked,
             std::vector<AssetEntry>& out)
{
    std::vector<std::string> files = platform::listFiles(directory.c_str());
    std::sort(files.begin(), files.end());
    for (const std::string& path : files) {
        if (endsWith(path, extension)) {
            out.push_back({fileName(path), path, fileStem(path), group, cooked});
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
    // ⚑ The ORDER is what makes the groups contiguous, and the list draw
    // depends on that - do not sort `entries` afterwards.
    collect(sourceDirectory, ".forge", "parts", /*cooked=*/false, entries);
    collect(sourceDirectory, ".gltf", "source", /*cooked=*/false, entries);
    collect(sourceDirectory, ".glb", "source", /*cooked=*/false, entries);
    collect(cookedDirectory, ".smesh", "cooked", /*cooked=*/true, entries);
    return entries;
}

std::vector<AssetEntry> listTextures(const std::string& sourceDirectory,
                                     const std::string& cookedDirectory)
{
    std::vector<AssetEntry> entries;
    collect(sourceDirectory, ".tex", "source", /*cooked=*/false, entries);
    collect(cookedDirectory, ".stex", "cooked", /*cooked=*/true, entries);
    return entries;
}

bool isPartSource(const AssetEntry& entry)
{
    return endsWith(entry.path, ".forge");
}

std::string forgePartIdFromName(const std::string& name)
{
    std::string id;
    id.reserve(name.size());
    for (const char c : name) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '_';
        if (keep) {
            id.push_back(c);
        } else if (!id.empty() && id.back() != '_') {
            id.push_back('_');
        }
    }
    while (!id.empty() && id.back() == '_') {
        id.pop_back();
    }
    return id.empty() ? std::string("part") : id;
}

bool importGltfIntoDoc(const std::string& gltfPath, assets::ForgeDoc& doc, ImportOutcome& outcome,
                       std::string* error)
{
    outcome = {};

    std::vector<cooker::GltfPart> imported;
    if (!cooker::importGltfParts(gltfPath.c_str(), imported)) {
        if (error != nullptr) {
            *error = "cannot import " + fileName(gltfPath);
        }
        return false;
    }

    if (doc.name.empty()) {
        doc.name = fileStem(gltfPath);
    }

    // ⚑ Ids are resolved against BOTH the document and what this import has
    // already placed: a glTF may carry two nodes of the same name, and letting
    // the second overwrite the first would silently drop a part - the failure
    // mode a merge-by-id is supposed to prevent.
    std::vector<std::string> claimed;
    const auto isClaimed = [&claimed](const std::string& id) {
        return std::find(claimed.begin(), claimed.end(), id) != claimed.end();
    };

    for (const cooker::GltfPart& part : imported) {
        std::string id = forgePartIdFromName(part.name);
        if (isClaimed(id)) {
            std::string candidate;
            for (int suffix = 2; suffix < 10000; ++suffix) {
                candidate = id + "_" + std::to_string(suffix);
                if (!isClaimed(candidate) && doc.indexOf(candidate) == std::string::npos) {
                    break;
                }
            }
            id = candidate;
        }
        claimed.push_back(id);

        assets::ForgePart baked = assets::forgeBakePart(id, part.mesh);

        const std::size_t existing = doc.indexOf(id);
        if (existing == std::string::npos) {
            doc.parts.push_back(std::move(baked));
            outcome.added.push_back(id);
        } else {
            // Whole replacement, keeping only what is not Blender's to own: the
            // tree position and the author's comment above it.
            baked.parent = doc.parts[existing].parent;
            baked.leading = doc.parts[existing].leading;
            doc.parts[existing] = std::move(baked);
            outcome.replaced.push_back(id);
        }
    }

    for (const assets::ForgePart& part : doc.parts) {
        if (!isClaimed(part.id)) {
            outcome.kept.push_back(part.id);
        }
    }
    return true;
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

std::vector<MissingModelRef> missingModelRefs(const assets::DefDatabase& defs)
{
    std::vector<MissingModelRef> missing;
    for (const assets::ShipDef& ship : defs.ships()) {
        if (defs.findModel(ship.model.c_str()) == nullptr) {
            missing.push_back({"ship", ship.id, ship.model});
        }
    }
    for (const assets::StationDef& station : defs.stations()) {
        if (defs.findModel(station.model.c_str()) == nullptr) {
            missing.push_back({"station", station.id, station.model});
        }
    }
    return missing;
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

// --- the texture preview's geometry (stage I) --------------------------------

int texturePreviewScale(int textureWidth, float availableWidth)
{
    if (textureWidth <= 0) {
        return 1;
    }
    const int fits = static_cast<int>(availableWidth) / textureWidth;
    return fits < 1 ? 1 : fits;
}

bool texturePixelAt(core::Vec2 cursor, core::Vec2 origin, int scale, int width, int height, int& x,
                    int& y)
{
    if (scale <= 0 || width <= 0 || height <= 0) {
        return false;
    }
    const float localX = cursor.x - origin.x;
    const float localY = cursor.y - origin.y;
    // Guarded before the cast, because a cast to int truncates TOWARD ZERO and
    // a cursor one pixel above the image would land on row 0 rather than off it.
    if (localX < 0.0f || localY < 0.0f) {
        return false;
    }
    const int px = static_cast<int>(localX) / scale;
    const int py = static_cast<int>(localY) / scale;
    if (px >= width || py >= height) {
        return false;
    }
    x = px;
    y = py;
    return true;
}

int textureDragOffset(float startCursor, float cursor, int scale)
{
    if (scale <= 0) {
        return 0;
    }
    return static_cast<int>(
        std::lround((cursor - startCursor) / static_cast<float>(scale)));
}

} // namespace forge
