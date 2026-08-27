#include "cook.hpp"

#include "font.hpp"
#include "gltf.hpp"
#include "mesh.hpp"
#include "outputs.hpp"
#include "png.hpp"
#include "sound.hpp"
#include "texture.hpp"

#include "sol/assets/mesh_lod.hpp"
#include "sol/assets/texture_doc.hpp"
#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <cctype>
#include <cstdint>
#include <utility>

namespace sol::cooker {

namespace {

std::string fileStem(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t start = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = path.find_last_of('.');
    return path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
}

std::string fileExtension(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    std::string extension = dot == std::string::npos ? std::string() : path.substr(dot);
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

std::string directoryOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// ⚑ The output directory arrives from argv - or, now, from the Forge - and may
// be spelled `.\build\bin\cooked`, while `platform::listFiles` hands back '/'
// separators. So "is this file directly in the output directory" is a
// comparison between two spellings of the same place. Trailing separators go
// too, because `out/` and `out` are one directory and only one of them ever
// matches.
//
// ⚑ `tools/forge/src/mesh_library.cpp` keeps its own copy of this for the
// inbox, and the two are deliberately not shared yet: they normalise for
// different comparisons and neither has been wrong. If a THIRD appears, or if
// either is ever wrong, the fix belongs in `platform` beside the promise it is
// compensating for - `listFiles` is what says '/' - and not in another local
// helper.
std::string normalizeSeparators(const std::string& path)
{
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

bool isUpToDate(const std::string& source, const std::string& output)
{
    const std::uint64_t sourceTime = platform::fileModificationTime(source.c_str());
    const std::uint64_t outputTime = platform::fileModificationTime(output.c_str());
    return outputTime != 0 && sourceTime != 0 && outputTime >= sourceTime;
}

// A font manifest pulls in the TTFs beside it, so its own timestamp is not
// enough to decide staleness - editing a font file has to force a re-cook too.
bool isUpToDateWithSiblings(const std::string& source, const std::string& output)
{
    if (!isUpToDate(source, output)) {
        return false;
    }
    const std::uint64_t outputTime = platform::fileModificationTime(output.c_str());
    for (const std::string& sibling : platform::listFiles(directoryOf(source).c_str())) {
        const std::uint64_t siblingTime = platform::fileModificationTime(sibling.c_str());
        if (siblingTime != 0 && siblingTime > outputTime) {
            return false;
        }
    }
    return true;
}

bool isCurrent(const CookJob& job)
{
    switch (stalenessRuleFor(job.kind)) {
    case StalenessRule::Timestamp:
        return isUpToDate(job.source, job.output);
    case StalenessRule::TimestampAndSiblings:
        return isUpToDateWithSiblings(job.source, job.output);
    case StalenessRule::AlwaysCook:
        break;
    }
    return false;
}

// Everything after the pixels exist, shared by both texture sources. A `.tex`
// document and an imported `.png` differ only in where the image came from -
// what happens to it afterwards is one implementation, which is the property
// that let stage G change the source format without touching what the game
// loads.
bool writeTextureImage(const ImageRgba& image, const std::string& source, const std::string& output)
{
    const assets::TextureData data = encodeTexture(image);
    const std::vector<std::uint8_t> fileBytes = serializeTexture(data);

    if (!platform::writeFileBytes(output.c_str(), fileBytes.data(), fileBytes.size())) {
        SOL_LOG_ERROR("cooker: cannot write %s", output.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%ux%u, %zu mips)",
                 source.c_str(),
                 output.c_str(),
                 data.width,
                 data.height,
                 data.mips.size());
    return true;
}

// An imported image. Nothing in `assets/textures/` is a PNG since stage G, but
// the decoder stays for the same reason the glTF importer did when stage D
// deleted the last `.gltf`: it is how art from somewhere else gets in.
bool cookTexture(const std::string& source, const std::string& output)
{
    std::vector<std::uint8_t> pngBytes;
    if (!platform::readFileBytes(source.c_str(), pngBytes)) {
        SOL_LOG_ERROR("cooker: cannot read %s", source.c_str());
        return false;
    }
    ImageRgba image;
    if (!decodePng(pngBytes.data(), pngBytes.size(), image)) {
        return false;
    }
    return writeTextureImage(image, source, output);
}

// The authored source: a `.tex` document evaluated to pixels here rather than
// drawn by a script on somebody's machine.
bool cookTextureDoc(const std::string& source, const std::string& output)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(source.c_str(), bytes)) {
        SOL_LOG_ERROR("cooker: cannot read %s", source.c_str());
        return false;
    }
    assets::TextureDoc doc;
    std::string error;
    if (!assets::parseTexture(
            reinterpret_cast<const char*>(bytes.data()), bytes.size(), source.c_str(), doc, &error)) {
        SOL_LOG_ERROR("cooker: %s", error.c_str());
        return false;
    }
    assets::TextureImage built;
    if (!assets::buildTexture(doc, built, &error)) {
        SOL_LOG_ERROR("cooker: %s: %s", source.c_str(), error.c_str());
        return false;
    }
    if (doc.hasUnplaceableComments) {
        SOL_LOG_WARN("cooker: %s carries a comment this format cannot place", source.c_str());
    }

    ImageRgba image;
    image.width = built.width;
    image.height = built.height;
    image.pixels = std::move(built.pixels);
    return writeTextureImage(image, source, output);
}

// ⚑ The format itself is `cooker::encodeMesh`, and this is only the plumbing
// around it. It used to be one function in `main.cpp` - which is why
// `cooker.unit`, which links the library and not that file, could not reach the
// `.smesh` layout at all and the D checkpoint's gap survived three slices.
bool writeMesh(const assets::MeshData& mesh, const std::string& source, const std::string& output)
{
    const std::vector<std::uint8_t> fileBytes = encodeMesh(mesh);
    if (!platform::writeFileBytes(output.c_str(), fileBytes.data(), fileBytes.size())) {
        SOL_LOG_ERROR("cooker: cannot write %s", output.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%zu vertices, %zu indices)",
                 source.c_str(),
                 output.c_str(),
                 mesh.vertices.size(),
                 mesh.indices.size());
    return true;
}

bool cookMesh(const std::string& source, const std::string& output)
{
    assets::MeshData mesh;
    if (!importGltf(source.c_str(), mesh)) {
        return false;
    }
    return writeMesh(mesh, source, output);
}

// A `.forge` part tree (engine plan Phase 9 stage D). Unlike a glTF this is a
// SOURCE file - the shape the mesh is built from rather than the triangles it
// came out as - so the cook is an evaluation rather than an import.
bool cookForge(const std::string& source, const std::string& output)
{
    assets::MeshData mesh;
    std::string error;
    if (!importForgeMesh(source.c_str(), mesh, &error)) {
        SOL_LOG_ERROR("cooker: %s", error.c_str());
        return false;
    }

    // Stage F: the cook produces a level SET. Generation is a policy over the
    // built mesh (assets/mesh_lod.hpp) and most assets are refused by the
    // triangle floor, which is the correct answer and not a failure - so the
    // reason is logged either way and an author never has to guess.
    const assets::LodChain chain = assets::buildLodChain(mesh);
    std::uint32_t levels = 0;
    if (!writeMeshLevels(mesh, chain, output, levels, &error)) {
        SOL_LOG_ERROR("cooker: %s", error.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%zu vertices, %zu indices, %u level(s): %s)",
                 source.c_str(),
                 output.c_str(),
                 mesh.vertices.size(),
                 mesh.indices.size(),
                 levels,
                 chain.stopReason.c_str());
    return true;
}

bool cookFont(const std::string& source, const std::string& output)
{
    std::vector<std::uint8_t> manifestBytes;
    if (!platform::readFileBytes(source.c_str(), manifestBytes)) {
        SOL_LOG_ERROR("cooker: cannot read %s", source.c_str());
        return false;
    }

    BakedFont font;
    std::string error;
    if (!bakeFont(reinterpret_cast<const char*>(manifestBytes.data()),
                  manifestBytes.size(),
                  directoryOf(source),
                  font,
                  &error)) {
        SOL_LOG_ERROR("cooker: %s: %s", source.c_str(), error.c_str());
        return false;
    }

    const std::vector<std::uint8_t> fileBytes = encodeFont(font);
    if (!platform::writeFileBytes(output.c_str(), fileBytes.data(), fileBytes.size())) {
        SOL_LOG_ERROR("cooker: cannot write %s", output.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%zu styles, %zu glyphs, %ux%u atlas)",
                 source.c_str(),
                 output.c_str(),
                 font.styles.size(),
                 font.glyphs.size(),
                 font.atlasWidth,
                 font.atlasHeight);
    return true;
}

bool cookSound(const std::string& source, const std::string& output)
{
    std::vector<std::uint8_t> sourceBytes;
    if (!platform::readFileBytes(source.c_str(), sourceBytes)) {
        SOL_LOG_ERROR("cooker: cannot read %s", source.c_str());
        return false;
    }

    assets::SoundData sound;
    const bool isOgg = fileExtension(source) == ".ogg";
    const bool imported = isOgg ? importOgg(sourceBytes.data(), sourceBytes.size(), sound)
                                : importWav(sourceBytes.data(), sourceBytes.size(), sound);
    if (!imported) {
        SOL_LOG_ERROR("cooker: cannot import %s", source.c_str());
        return false;
    }

    const std::vector<std::uint8_t> fileBytes = encodeSound(sound);
    if (!platform::writeFileBytes(output.c_str(), fileBytes.data(), fileBytes.size())) {
        SOL_LOG_ERROR("cooker: cannot write %s", output.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%u frames, %u ch, %u Hz)",
                 source.c_str(),
                 output.c_str(),
                 sound.frameCount(),
                 sound.channelCount,
                 sound.sampleRate);
    return true;
}

bool runJob(const CookJob& job)
{
    switch (job.kind) {
    case CookKind::Texture:
        return cookTexture(job.source, job.output);
    case CookKind::TextureDoc:
        return cookTextureDoc(job.source, job.output);
    case CookKind::Mesh:
        return cookMesh(job.source, job.output);
    case CookKind::ForgeMesh:
        return cookForge(job.source, job.output);
    case CookKind::Font:
        return cookFont(job.source, job.output);
    case CookKind::Sound:
        return cookSound(job.source, job.output);
    case CookKind::None:
        break;
    }
    return false;
}

// ⚑⚑ OUTPUTS WHOSE SOURCE HAS BEEN DELETED. The cook walks SOURCES, so it can
// never visit an asset that is gone - `foo.forge` deleted leaves `foo.smesh`
// and its levels on disk forever, and a rebuild will not clear them. Stage F's
// `writeMeshLevels` solves the neighbouring case (a chain that got shorter) but
// only for an asset that still cooks.
void sweepStrays(const std::vector<CookJob>& jobs,
                 const std::string& sourceDirectory,
                 const std::string& outputDirectory)
{
    std::vector<std::string> jobOutputs;
    jobOutputs.reserve(jobs.size());
    for (const CookJob& job : jobs) {
        jobOutputs.push_back(job.output);
    }

    // ⚑⚑ THE GUARD THAT MATTERS MOST, AND IT IS A TYPO GUARD RATHER THAN A
    // LOGIC ONE: with no jobs, EVERY cooked file is unclaimed and the sweep
    // would empty the directory. That is exactly what a mistyped source path
    // looks like from in here - `listFiles` on a directory that does not exist
    // returns nothing and reports nothing wrong.
    if (jobOutputs.empty()) {
        SOL_LOG_WARN("cooker: no sources under %s - skipping the stray sweep", sourceDirectory.c_str());
        return;
    }

    const std::vector<std::string> expected = expectedOutputNames(jobOutputs);
    // Only files sitting directly in the output directory: `listFiles`
    // recurses, and a nested tree is not something this cook produced.
    std::vector<std::string> present;
    for (const std::string& path : platform::listFiles(outputDirectory.c_str())) {
        if (directoryOf(path) == normalizeSeparators(outputDirectory)) {
            present.push_back(path);
        }
    }

    int swept = 0;
    for (const std::string& stray : strayOutputNames(expected, present)) {
        if (platform::deleteFile(stray.c_str())) {
            SOL_LOG_INFO("cooker: removed %s - its source is gone", stray.c_str());
            ++swept;
        } else {
            SOL_LOG_WARN("cooker: cannot remove stray %s", stray.c_str());
        }
    }
    if (swept != 0) {
        SOL_LOG_INFO("cooker: swept %d orphaned output(s)", swept);
    }
}

} // namespace

CookKind cookKindForSource(const std::string& path)
{
    const std::string extension = fileExtension(path);
    if (extension == ".png") {
        return CookKind::Texture;
    }
    if (extension == ".tex") {
        return CookKind::TextureDoc;
    }
    if (extension == ".gltf" || extension == ".glb") {
        return CookKind::Mesh;
    }
    if (extension == ".forge") {
        return CookKind::ForgeMesh;
    }
    if (extension == ".font") {
        return CookKind::Font;
    }
    if (extension == ".wav" || extension == ".ogg") {
        return CookKind::Sound;
    }
    return CookKind::None;
}

const char* cookedExtension(CookKind kind)
{
    switch (kind) {
    case CookKind::Texture:
    case CookKind::TextureDoc:
        return ".stex";
    case CookKind::Mesh:
    case CookKind::ForgeMesh:
        return ".smesh";
    case CookKind::Font:
        return ".sfont";
    case CookKind::Sound:
        return ".saud";
    case CookKind::None:
        break;
    }
    return "";
}

StalenessRule stalenessRuleFor(CookKind kind)
{
    switch (kind) {
    case CookKind::ForgeMesh:
        return StalenessRule::AlwaysCook;
    case CookKind::Font:
        return StalenessRule::TimestampAndSiblings;
    case CookKind::Texture:
    case CookKind::TextureDoc:
    case CookKind::Mesh:
    case CookKind::Sound:
    case CookKind::None:
        break;
    }
    return StalenessRule::Timestamp;
}

std::vector<CookJob> planCook(const std::vector<std::string>& sources, const std::string& outputDirectory)
{
    std::vector<CookJob> jobs;
    for (const std::string& source : sources) {
        const CookKind kind = cookKindForSource(source);
        if (kind == CookKind::None) {
            continue; // .gitkeep, .ttf sources named by a .font manifest, etc.
        }
        CookJob job;
        job.source = source;
        job.output = outputDirectory + "/" + fileStem(source) + cookedExtension(kind);
        job.kind = kind;
        jobs.push_back(std::move(job));
    }
    return jobs;
}

std::vector<CookCollision> cookCollisions(const std::vector<CookJob>& jobs)
{
    std::vector<CookCollision> collisions;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        for (std::size_t j = i + 1; j < jobs.size(); ++j) {
            if (jobs[i].output == jobs[j].output) {
                collisions.push_back({jobs[i].source, jobs[j].source, jobs[i].output});
            }
        }
    }
    return collisions;
}

std::string CookReport::summary() const
{
    if (!refusal.empty()) {
        return refusal;
    }
    return std::to_string(cooked) + " cooked, " + std::to_string(skipped) + " up to date, " +
           std::to_string(failed) + " failed";
}

CookReport cookDirectory(const std::string& sourceDirectory, const std::string& outputDirectory)
{
    CookReport report;

    if (!platform::createDirectories(outputDirectory.c_str())) {
        report.refusal = "cannot create output directory " + outputDirectory;
        SOL_LOG_ERROR("cooker: %s", report.refusal.c_str());
        return report;
    }

    const std::vector<CookJob> jobs = planCook(platform::listFiles(sourceDirectory.c_str()), outputDirectory);

    const std::vector<CookCollision> collisions = cookCollisions(jobs);
    if (!collisions.empty()) {
        for (const CookCollision& collision : collisions) {
            SOL_LOG_ERROR("cooker: %s and %s both cook to %s",
                          collision.firstSource.c_str(),
                          collision.secondSource.c_str(),
                          collision.output.c_str());
        }
        report.refusal = std::to_string(collisions.size()) + " output collision(s); nothing cooked";
        SOL_LOG_ERROR("cooker: %s", report.refusal.c_str());
        return report;
    }

    for (const CookJob& job : jobs) {
        if (isCurrent(job)) {
            ++report.skipped;
            continue;
        }
        if (runJob(job)) {
            ++report.cooked;
        } else {
            ++report.failed;
        }
    }

    // ⚑ THE SWEEP IS NOT RUN AFTER A FAILURE. A job that failed may not have
    // written its output, and deleting the previous good one would turn a build
    // error into a missing asset - the cook's own report is "nothing changed",
    // so leave the directory as it is and let the author fix the source.
    if (report.failed == 0) {
        sweepStrays(jobs, sourceDirectory, outputDirectory);
    }

    // ⚑ The tally is logged HERE rather than by the caller so that every caller
    // logs it - the cooker's build output and the Forge's console say the same
    // sentence about the same run, and it is the sentence the status bar shows.
    SOL_LOG_INFO("cooker: %s", report.summary().c_str());
    return report;
}

} // namespace sol::cooker
