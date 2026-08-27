#include "mesh_library.hpp"

#include "gltf.hpp"
#include "png.hpp"
#include "sound.hpp"
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

[[nodiscard]] bool startsWith(const std::string& text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// A drop directory is written to by another program, so both halves of a path
// comparison have to be normalised before they can be compared at all:
// `listFiles` emits '/', but a directory handed in from a build definition or a
// command line may carry '\' and a trailing separator.
[[nodiscard]] std::string normalisedPath(const std::string& path)
{
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

[[nodiscard]] std::string normalisedDirectory(const std::string& directory)
{
    std::string out = normalisedPath(directory);
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

// Lower-cased, because Blender will happily hand back `.GLTF` on a
// case-insensitive filesystem and a drop that imports must not depend on which.
[[nodiscard]] std::string lowerExtension(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string extension = path.substr(dot);
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return extension;
}

void collect(const std::string& directory,
             const char* extension,
             const char* group,
             bool cooked,
             std::vector<AssetEntry>& out)
{
    std::vector<std::string> files = platform::listFiles(directory.c_str());
    std::sort(files.begin(), files.end());
    for (const std::string& path : files) {
        // ⚑⚑ COMPARED THROUGH `lowerExtension` RATHER THAN `endsWith` SO THIS
        // AGREES WITH `cookKindForSource`, which has been case-insensitive since
        // Phase 5 "because Blender will hand back `.GLTF` and mean the same
        // thing". The two have disagreed ever since, and it took stage U2's
        // `.png` to make the disagreement reachable: a paint program on Windows
        // writes `.PNG` far more readily than Blender writes `.GLTF`.
        //
        // ⚑ And the failure it produces is the worst available shape - the
        // cooker cooks the file and the tool cannot see it, so an author is told
        // their texture does not exist by the only window they are looking at.
        if (lowerExtension(path) == extension) {
            out.push_back({fileName(path), path, fileStem(path), group, cooked});
        }
    }
}

} // namespace

std::vector<AssetEntry> listMeshes(const std::string& sourceDirectory, const std::string& cookedDirectory)
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

std::vector<AssetEntry> listTextures(const std::string& sourceDirectory, const std::string& cookedDirectory)
{
    std::vector<AssetEntry> entries;
    // ⚑ `.tex` before `.png` for `listMeshes`'s reason rather than by alphabet:
    // the document is the only kind of texture this tool can EDIT, and an
    // imported image is what a texture arrived as rather than what it is.
    collect(sourceDirectory, ".tex", "source", /*cooked=*/false, entries);
    // ⚑⚑ STAGE U2, AND IT IS ONE LINE BECAUSE THE COOKER ALREADY AGREED WITH IT.
    // `CookKind::Texture` has meant `.png` -> `.stex` since Phase 5, and
    // `cookTexture` shares `writeTextureImage` with the document path - so a
    // painted image was already a first-class asset everywhere except in the one
    // window an author actually looks at.
    //
    // ⚑ Both source extensions land in ONE group, exactly as `.wav` and `.ogg`
    // do: the distinction a group draws is authored-versus-built, and a painted
    // image is no more built than a document is. Which of the two a row is stays
    // visible anyway - the label carries the extension, and `isTextureSource` is
    // what decides whether the editor opens on it.
    collect(sourceDirectory, ".png", "source", /*cooked=*/false, entries);
    collect(cookedDirectory, ".stex", "cooked", /*cooked=*/true, entries);
    return entries;
}

std::vector<AssetEntry> listSounds(const std::string& sourceDirectory, const std::string& cookedDirectory)
{
    std::vector<AssetEntry> entries;
    // ⚑ Both source extensions land in ONE group rather than a `wav` run and an
    // `ogg` run. The distinction the groups draw is authored-versus-built, and
    // which decoder read a file is not something an author is choosing between:
    // `sounds.toml` already says every cue "can be replaced one file at a time
    // by a recorded .ogg", i.e. the two are the same row in two spellings.
    collect(sourceDirectory, ".wav", "source", /*cooked=*/false, entries);
    collect(sourceDirectory, ".ogg", "source", /*cooked=*/false, entries);
    collect(cookedDirectory, ".saud", "cooked", /*cooked=*/true, entries);
    return entries;
}

bool isPartSource(const AssetEntry& entry)
{
    return endsWith(entry.path, ".forge");
}

bool isSoundSource(const AssetEntry& entry)
{
    return endsWith(entry.path, ".wav") || endsWith(entry.path, ".ogg");
}

std::string forgePartIdFromName(const std::string& name)
{
    std::string id;
    id.reserve(name.size());
    for (const char c : name) {
        const bool keep =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
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

std::string importedTextureStem(const std::string& documentStem, const std::string& imageName)
{
    // ⚑ LOWER-CASED, and that is not cosmetic either. Every texture committed to
    // this repo is lower case, Blender hands back whatever the author typed, and
    // U2 has already been bitten once by the listing and the cooker disagreeing
    // about case - so the one place that INVENTS a filename should only ever
    // invent one spelling.
    std::string stem = forgePartIdFromName(documentStem) + "_" + forgePartIdFromName(imageName);
    for (char& c : stem) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return stem;
}

bool importGltfIntoDoc(const std::string& gltfPath,
                       const std::string& texturesDirectory,
                       assets::ForgeDoc& doc,
                       ImportOutcome& outcome,
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

    // --- the textures, and deliberately BEFORE the parts (stage U3) ----------
    //
    // ⚑ A pass of its own, ahead of the id matching, because an imported
    // texture's name does not depend on how a part resolved: it is the mesh
    // document's stem and the image's own name, and both are known here.
    // Threading it through the loop below would tie a file on disk to a rename
    // rule that has nothing to do with it - and that loop already carries the
    // hardest bookkeeping in this tool.
    const std::string documentStem = fileStem(gltfPath);
    // ⚑ Stems SEEN rather than stems WRITTEN, and the difference shows up only
    // in the case where both rules fire at once: two objects sharing one
    // material produce one stem, and if that stem also collides with a
    // hand-authored `.tex` then tracking only what was written would report the
    // same refusal once per object. One image is one line, whatever it did.
    std::vector<std::string> seenStems;
    for (const cooker::GltfPart& part : imported) {
        if (!part.imageNote.empty()) {
            outcome.imageNotes.emplace_back(part.name, part.imageNote);
        }
        if (part.imageBytes.empty() || texturesDirectory.empty()) {
            continue;
        }

        // ⚑ The IMAGE's name first and the object's only as a fallback. Two
        // objects sharing one material share one image, and naming the file
        // after the object would write those identical bytes twice under two
        // names - which an author then has to keep in step by hand for the rest
        // of the asset's life. The object name is what is left when a GLB packs
        // an image that never had one.
        const std::string label = part.imageName.empty() ? part.name : part.imageName;
        const std::string stem = importedTextureStem(documentStem, label);
        if (std::find(seenStems.begin(), seenStems.end(), stem) != seenStems.end()) {
            continue; // already dealt with this import: the shared-material case
        }
        seenStems.push_back(stem);

        // ⚑⚑ THE ONE COLLISION THE PREFIX CANNOT RULE OUT, AND IT IS WORTH A
        // CHECK BECAUSE OF WHAT IT COSTS: an author who has hand-authored
        // `ship_hull.tex` gets a `ship_hull.png` written beside it, and that
        // pair does not fail the texture - it ABORTS THE WHOLE COOK, every
        // asset, until they work out which of two files to delete. Refusing one
        // image by name is a far smaller failure than that, and it leaves the
        // document they authored alone.
        const std::string documentSibling = texturesDirectory + "/" + stem + ".tex";
        if (platform::fileModificationTime(documentSibling.c_str()) != 0) {
            outcome.imageNotes.emplace_back(
                part.name, "would be written as " + stem + ".png, which collides with " + stem + ".tex");
            continue;
        }

        const std::string target = texturesDirectory + "/" + stem + ".png";
        if (!platform::createDirectories(texturesDirectory.c_str()) ||
            !platform::writeFileBytes(target.c_str(), part.imageBytes.data(), part.imageBytes.size())) {
            outcome.imageNotes.emplace_back(part.name, "could not be written to " + target);
            continue;
        }
        outcome.textures.emplace_back(part.name, stem);
    }

    // ⚑ Claimed by INDEX rather than by id, because stage P lets a match be
    // renamed: an id is no longer a stable handle on a part for the duration of
    // this loop. Two nodes may also carry the same name, or - after a
    // `Shift+D` that Blender let copy the uid - the same origin, and letting
    // the second take the first's part would silently drop one.
    std::vector<std::size_t> taken;
    const auto isTaken = [&taken](std::size_t index) {
        return std::find(taken.begin(), taken.end(), index) != taken.end();
    };

    // Free of everything except `self`, which is what lets a part keep the id
    // it already has rather than suffixing itself out of its own name.
    const auto isIdFree = [&doc](const std::string& id, std::size_t self) {
        for (std::size_t i = 0; i < doc.parts.size(); ++i) {
            if (i != self && doc.parts[i].id == id) {
                return false;
            }
        }
        return true;
    };
    const auto freeId = [&isIdFree](const std::string& wanted, std::size_t self) {
        if (isIdFree(wanted, self)) {
            return wanted;
        }
        for (int suffix = 2; suffix < 10000; ++suffix) {
            std::string candidate = wanted + "_" + std::to_string(suffix);
            if (isIdFree(candidate, self)) {
                return candidate;
            }
        }
        return wanted;
    };

    for (const cooker::GltfPart& part : imported) {
        const std::string wanted = forgePartIdFromName(part.name);

        // The origin finds the part whose object was renamed. Only an unclaimed
        // one: a duplicated object arrives carrying its source's uid, and it is
        // a NEW part rather than a second claim on an existing one.
        std::size_t target = std::string::npos;
        if (!part.originId.empty()) {
            for (std::size_t i = 0; i < doc.parts.size(); ++i) {
                if (!isTaken(i) && doc.parts[i].origin == part.originId) {
                    target = i;
                    break;
                }
            }
        }
        // The name is the fallback, and only onto a part that has never been
        // identified - see the header. This is what matches a document written
        // before stage P, once, after which it carries an origin.
        if (target == std::string::npos) {
            for (std::size_t i = 0; i < doc.parts.size(); ++i) {
                if (!isTaken(i) && doc.parts[i].origin.empty() && doc.parts[i].id == wanted) {
                    target = i;
                    break;
                }
            }
        }

        if (target == std::string::npos) {
            const std::string id = freeId(wanted, std::string::npos);
            assets::ForgePart baked = assets::forgeBakePart(id, part.mesh);
            baked.origin = part.originId;
            taken.push_back(doc.parts.size());
            doc.parts.push_back(std::move(baked));
            outcome.added.push_back(id);
            continue;
        }

        const std::string previousId = doc.parts[target].id;
        const std::string id = freeId(wanted, target);

        // Whole replacement, keeping only what is not Blender's to own: the
        // tree position and the author's comment above it.
        assets::ForgePart baked = assets::forgeBakePart(id, part.mesh);
        baked.parent = doc.parts[target].parent;
        baked.leading = doc.parts[target].leading;
        baked.origin = part.originId;
        doc.parts[target] = std::move(baked);
        taken.push_back(target);

        if (id == previousId) {
            outcome.replaced.push_back(id);
            continue;
        }
        // ⚑ A part is named by its children, so a rename that stops there
        // leaves them parented to something that no longer exists - which
        // `worldTransforms` reads as "no parent" and silently draws at the
        // document origin.
        for (assets::ForgePart& child : doc.parts) {
            if (child.parent == previousId) {
                child.parent = id;
            }
        }
        outcome.renamed.emplace_back(previousId, id);
    }

    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        if (!isTaken(i)) {
            outcome.kept.push_back(doc.parts[i].id);
        }
    }
    return true;
}

bool isTextureSource(const AssetEntry& entry)
{
    return lowerExtension(entry.path) == ".tex";
}

bool isImportedTexture(const AssetEntry& entry)
{
    return lowerExtension(entry.path) == ".png";
}

bool loadTexture(const AssetEntry& entry, assets::TextureData& out, std::string* error)
{
    // ⚑⚑⚑ THE IMPORTED IMAGE IS TESTED FIRST AND BY ITS OWN EXTENSION, NOT BY
    // FALLING OFF `!isTextureSource`, AND THAT ORDER IS THE WHOLE BUG STAGE U2
    // COULD HAVE SHIPPED. The `.stex` branch below used to be spelled "anything
    // that is not a document", which was exact while `.tex` and `.stex` were the
    // only two things in the list - and becomes wrong in the NEW case only: a
    // `.png` would have been handed to the runtime `.stex` loader, failed on the
    // header, and reported "cannot load hull.png" about a file that is perfectly
    // good. A predicate that was right for two kinds is not right for three.
    if (isImportedTexture(entry)) {
        std::vector<std::uint8_t> pngBytes;
        if (!platform::readFileBytes(entry.path.c_str(), pngBytes)) {
            if (error != nullptr) {
                *error = "cannot read " + entry.path;
            }
            return false;
        }
        cooker::ImageRgba image;
        if (!cooker::decodePng(pngBytes.data(), pngBytes.size(), image)) {
            // ⚑ `decodePng` names its own reason in the log - 8-bit only, no
            // interlacing, unsupported colour type - and inventing a second
            // diagnosis here would be less specific than the first. Same bargain
            // `loadSound` already makes for a bad wav.
            if (error != nullptr) {
                *error = "cannot decode " + entry.label + " (see the log)";
            }
            return false;
        }
        // ⚑⚑ THROUGH `encodeTexture`, WHICH IS STAGE G'S RULE ARRIVING AT THE
        // SECOND TEXTURE SOURCE: what an author sees is the BC1 chain the game
        // uploads, not the RGBA that came out of the paint program. It matters
        // more here than for a `.tex`, because an imported photograph is exactly
        // the kind of image BC1 mangles - and the mangling is the thing this
        // tool exists to show before it is in the game.
        out = cooker::encodeTexture(image);
        return true;
    }

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
    if (!assets::parseTexture(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size(),
                              entry.path.c_str(),
                              doc,
                              &parseError)) {
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
        if (!assets::parseForge(
                reinterpret_cast<const char*>(bytes.data()), bytes.size(), entry.path.c_str(), doc, &error)) {
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

bool loadSound(const AssetEntry& entry, assets::SoundData& out, std::string* error)
{
    if (!isSoundSource(entry)) {
        if (assets::loadSound(entry.path.c_str(), out)) {
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
    const bool ok = endsWith(entry.path, ".ogg") ? cooker::importOgg(bytes.data(), bytes.size(), out)
                                                 : cooker::importWav(bytes.data(), bytes.size(), out);
    if (!ok && error != nullptr) {
        // ⚑ The importers log their own reason and return a bare bool, so the
        // sentence here names the FILE and points at the log rather than
        // inventing a second diagnosis that would be less specific than the
        // first. Same bargain `loadMesh` already makes for a bad glTF.
        *error = "cannot import " + entry.label + " (see the log)";
    }
    return ok;
}

SoundReport reportSound(const assets::SoundData& data)
{
    SoundReport report;
    report.sampleRate = data.sampleRate;
    report.channelCount = data.channelCount;
    report.frames = data.frameCount();
    if (data.sampleRate > 0) {
        report.seconds = static_cast<float>(report.frames) / static_cast<float>(data.sampleRate);
    }
    // ⚑⚑ TWO DELIBERATE CHOICES ABOUT ONE ASYMMETRIC NUMBER, both measured
    // rather than assumed. int16 runs -32768..32767, so the loudest possible
    // sample is a TROUGH with no positive counterpart:
    //
    // (1) The magnitude is kept in `int32`. Integer promotion already saves the
    //     negation itself - `-sample` on an int16 is computed as int and gives
    //     32768 - but a magnitude stored back into an `int16` wraps to -32768
    //     and then loses every comparison, so a clipped cue would report as
    //     SILENT in the panel an author consults to find out whether it clips.
    //     The width is what makes that unwritable, and the test pins it.
    // (2) The divisor is 32768 and not 32767, so that trough is exactly 1.0.
    //     Measured: over 32767 it is 1.000031, i.e. a "peak" above full scale.
    std::int32_t loudest = 0;
    for (const std::int16_t sample : data.samples) {
        const std::int32_t magnitude = sample < 0 ? -static_cast<std::int32_t>(sample) : sample;
        loudest = magnitude > loudest ? magnitude : loudest;
    }
    report.peak = static_cast<float>(loudest) / 32768.0f;
    return report;
}

std::string forgeInboxArchive(const std::string& inboxDirectory)
{
    return normalisedDirectory(inboxDirectory) + "/imported";
}

bool forgeIsPendingDrop(const std::string& path, const std::string& inboxDirectory)
{
    const std::string extension = lowerExtension(path);
    if (extension != ".gltf" && extension != ".glb") {
        return false;
    }
    // ⚑ The archive sits INSIDE the directory being listed and `listFiles` is
    // recursive, so without this a filed drop is imported again on the next
    // poll, filed again, and the tool never stops. Compared as a directory
    // prefix (with the separator) rather than as a substring, so a sibling
    // named `imported_backup/` is not swept up by it.
    const std::string archivePrefix = forgeInboxArchive(inboxDirectory) + "/";
    return !startsWith(normalisedPath(path), archivePrefix);
}

std::vector<std::string> forgePendingDrops(std::vector<std::string> listed, const std::string& inboxDirectory)
{
    listed.erase(std::remove_if(listed.begin(),
                                listed.end(),
                                [&inboxDirectory](const std::string& path) {
                                    return !forgeIsPendingDrop(path, inboxDirectory);
                                }),
                 listed.end());
    std::sort(listed.begin(), listed.end());
    return listed;
}

std::string forgeArchivedDropPath(const std::string& dropPath, const std::string& inboxDirectory)
{
    return forgeInboxArchive(inboxDirectory) + "/" + fileName(dropPath);
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

std::vector<ModelMatch>
matchModels(const assets::DefDatabase& defs, const AssetEntry& entry, const MeshReport& report)
{
    std::vector<ModelMatch> matches;
    for (const assets::ModelDef& model : defs.models()) {
        if (model.mesh != entry.stem) {
            continue;
        }
        ModelMatch match;
        match.id = model.id;
        // ⚑ Phase 25 stage A: the texture and the glow belong to the model's
        // MATERIAL now, and a row that names one carries neither itself. The
        // database resolves the index for every row, so the fallback below is
        // for a tool that loaded defs without validating them - it shows blank
        // rather than showing a stale value that used to be true.
        if (model.materialIndex < defs.materials().size()) {
            const assets::MaterialDef& material = defs.materials()[model.materialIndex];
            match.texture = material.texture;
            match.emissive = material.emissive;
        }
        match.authoredRadius = model.radius;
        match.authoredAvoidRadius = model.avoidRadius > 0.0f ? model.avoidRadius : model.radius;
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

bool texturePixelAt(core::Vec2 cursor, core::Vec2 origin, int scale, int width, int height, int& x, int& y)
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
    return static_cast<int>(std::lround((cursor - startCursor) / static_cast<float>(scale)));
}

} // namespace forge
