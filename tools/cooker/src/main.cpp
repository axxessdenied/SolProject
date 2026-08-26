#include "bc1.hpp"
#include "font.hpp"
#include "gltf.hpp"
#include "mesh.hpp"
#include "outputs.hpp"
#include "png.hpp"
#include "sound.hpp"
#include "texture.hpp"

#include "sol/assets/formats.hpp"
#include "sol/assets/texture_doc.hpp"
#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace sol;

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

// ⚑ The output directory arrives from argv and may be spelled `.\build\bin\cooked`,
// while `platform::listFiles` hands back '/' separators - so "is this file
// directly in the output directory" is a comparison between two spellings of
// the same place. Trailing separators go too, because `out/` and `out` are one
// directory and only one of them ever matches.
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

// ⚑ A `.forge` cooks to a SET of files since stage F - level 0 plus however
// many levels the policy accepts - and how many that is cannot be known without
// building the mesh and decimating it. A staleness check that cannot know how
// many outputs to look for is exactly how a stale level survives a re-cook and
// gets drawn at distance, where nobody is looking closely. So this one does not
// try to be clever: a part tree always cooks. It is cheap (the seven committed
// assets are 2,298 triangles between them) and it is the only version of this
// check that cannot be wrong.
bool isForgeUpToDate(const std::string&, const std::string&)
{
    return false;
}

// A font manifest pulls in the TTFs beside it, so its own timestamp is not
// enough to decide staleness - editing a font file has to force a re-cook too.
bool isFontUpToDate(const std::string& manifest, const std::string& output)
{
    if (!isUpToDate(manifest, output)) {
        return false;
    }
    const std::uint64_t outputTime = platform::fileModificationTime(output.c_str());
    for (const std::string& sibling : platform::listFiles(directoryOf(manifest).c_str())) {
        const std::uint64_t siblingTime = platform::fileModificationTime(sibling.c_str());
        if (siblingTime != 0 && siblingTime > outputTime) {
            return false;
        }
    }
    return true;
}

// Everything after the pixels exist, shared by both texture sources. A `.tex`
// document and an imported `.png` differ only in where the image came from -
// what happens to it afterwards is one implementation, which is the property
// that let stage G change the source format without touching what the game
// loads.
bool writeTextureImage(const cooker::ImageRgba& image, const std::string& source, const std::string& output)
{
    const assets::TextureData data = cooker::encodeTexture(image);
    const std::vector<std::uint8_t> fileBytes = cooker::serializeTexture(data);

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
    cooker::ImageRgba image;
    if (!cooker::decodePng(pngBytes.data(), pngBytes.size(), image)) {
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

    cooker::ImageRgba image;
    image.width = built.width;
    image.height = built.height;
    image.pixels = std::move(built.pixels);
    return writeTextureImage(image, source, output);
}

// ⚑ The format itself is `cooker::encodeMesh`, in the library, and this is only
// the plumbing around it. It used to be one function here - which is why
// `cooker.unit`, which links the library and not this file, could not reach the
// `.smesh` layout at all and the D checkpoint's gap survived three slices.
bool writeMesh(const assets::MeshData& mesh, const std::string& source, const std::string& output)
{
    const std::vector<std::uint8_t> fileBytes = cooker::encodeMesh(mesh);
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
    if (!cooker::importGltf(source.c_str(), mesh)) {
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
    if (!cooker::importForgeMesh(source.c_str(), mesh, &error)) {
        SOL_LOG_ERROR("cooker: %s", error.c_str());
        return false;
    }

    // Stage F: the cook produces a level SET. Generation is a policy over the
    // built mesh (assets/mesh_lod.hpp) and most assets are refused by the
    // triangle floor, which is the correct answer and not a failure - so the
    // reason is logged either way and an author never has to guess.
    const assets::LodChain chain = assets::buildLodChain(mesh);
    std::uint32_t levels = 0;
    if (!cooker::writeMeshLevels(mesh, chain, output, levels, &error)) {
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

    cooker::BakedFont font;
    std::string error;
    if (!cooker::bakeFont(reinterpret_cast<const char*>(manifestBytes.data()),
                          manifestBytes.size(),
                          directoryOf(source),
                          font,
                          &error)) {
        SOL_LOG_ERROR("cooker: %s: %s", source.c_str(), error.c_str());
        return false;
    }

    const std::vector<std::uint8_t> fileBytes = cooker::encodeFont(font);
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
    const bool imported = isOgg ? cooker::importOgg(sourceBytes.data(), sourceBytes.size(), sound)
                                : cooker::importWav(sourceBytes.data(), sourceBytes.size(), sound);
    if (!imported) {
        SOL_LOG_ERROR("cooker: cannot import %s", source.c_str());
        return false;
    }

    const std::vector<std::uint8_t> fileBytes = cooker::encodeSound(sound);
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

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        SOL_LOG_ERROR("usage: cooker <source-assets-dir> <output-dir>");
        return 1;
    }
    const std::string sourceDirectory = argv[1];
    const std::string outputDirectory = argv[2];

    if (!platform::createDirectories(outputDirectory.c_str())) {
        SOL_LOG_ERROR("cooker: cannot create output directory %s", outputDirectory.c_str());
        return 1;
    }

    struct Job
    {
        std::string source;
        std::string output;
        bool (*cook)(const std::string&, const std::string&) = nullptr;
        bool (*isCurrent)(const std::string&, const std::string&) = &isUpToDate;
    };

    std::vector<Job> jobs;
    for (const std::string& source : platform::listFiles(sourceDirectory.c_str())) {
        const std::string extension = fileExtension(source);
        Job job;
        job.source = source;

        if (extension == ".png") {
            job.output = outputDirectory + "/" + fileStem(source) + ".stex";
            job.cook = &cookTexture;
        } else if (extension == ".tex") {
            job.output = outputDirectory + "/" + fileStem(source) + ".stex";
            job.cook = &cookTextureDoc;
        } else if (extension == ".gltf" || extension == ".glb") {
            job.output = outputDirectory + "/" + fileStem(source) + ".smesh";
            job.cook = &cookMesh;
        } else if (extension == ".forge") {
            job.output = outputDirectory + "/" + fileStem(source) + ".smesh";
            job.cook = &cookForge;
            job.isCurrent = &isForgeUpToDate;
        } else if (extension == ".font") {
            job.output = outputDirectory + "/" + fileStem(source) + ".sfont";
            job.cook = &cookFont;
            job.isCurrent = &isFontUpToDate;
        } else if (extension == ".wav" || extension == ".ogg") {
            job.output = outputDirectory + "/" + fileStem(source) + ".saud";
            job.cook = &cookSound;
        } else {
            continue; // .gitkeep, .ttf sources named by a .font manifest, etc.
        }
        jobs.push_back(std::move(job));
    }

    // ⚑ Output paths are keyed on the file STEM, so two sources that differ
    // only by extension collide - and whichever cooked last would silently win,
    // with a staleness check that then reported the loser as up to date. The
    // hazard has been here since Phase 5 and was unreachable while `.gltf` was
    // the only mesh source; `.forge` is what makes `gate.forge` beside
    // `gate.gltf` a thing an author can type. Fail loudly instead.
    int failed = 0;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        for (std::size_t j = i + 1; j < jobs.size(); ++j) {
            if (jobs[i].output == jobs[j].output) {
                SOL_LOG_ERROR("cooker: %s and %s both cook to %s",
                              jobs[i].source.c_str(),
                              jobs[j].source.c_str(),
                              jobs[i].output.c_str());
                ++failed;
            }
        }
    }
    if (failed != 0) {
        SOL_LOG_ERROR("cooker: %d output collision(s); nothing cooked", failed);
        return 1;
    }

    int cooked = 0;
    int skipped = 0;
    for (const Job& job : jobs) {
        if (job.isCurrent(job.source, job.output)) {
            ++skipped;
            continue;
        }
        if (job.cook(job.source, job.output)) {
            ++cooked;
        } else {
            ++failed;
        }
    }

    // ⚑⚑ THE SWEEP: OUTPUTS WHOSE SOURCE HAS BEEN DELETED. The loop above walks
    // SOURCES, so it can never visit an asset that is gone - `foo.forge` deleted
    // leaves `foo.smesh` and its levels on disk forever, and a rebuild will not
    // clear them. Stage F's `writeMeshLevels` solves the neighbouring case (a
    // chain that got shorter) but only for an asset that still cooks.
    //
    // ⚑ NOT RUN AFTER A FAILURE. A job that failed may not have written its
    // output, and deleting the previous good one would turn a build error into
    // a missing asset - the cook's own report is "nothing changed", so leave the
    // directory as it is and let the author fix the source.
    if (failed == 0) {
        std::vector<std::string> jobOutputs;
        jobOutputs.reserve(jobs.size());
        for (const Job& job : jobs) {
            jobOutputs.push_back(job.output);
        }

        // ⚑⚑ THE GUARD THAT MATTERS MOST, AND IT IS A TYPO GUARD RATHER THAN A
        // LOGIC ONE: with no jobs, EVERY cooked file is unclaimed and the sweep
        // would empty the directory. That is exactly what a mistyped source
        // path looks like from in here - `listFiles` on a directory that does
        // not exist returns nothing and reports nothing wrong.
        if (jobOutputs.empty()) {
            SOL_LOG_WARN("cooker: no sources under %s - skipping the stray sweep", sourceDirectory.c_str());
        } else {
            const std::vector<std::string> expected = cooker::expectedOutputNames(jobOutputs);
            // Only files sitting directly in the output directory: `listFiles`
            // recurses, and a nested tree is not something this cook produced.
            std::vector<std::string> present;
            for (const std::string& path : platform::listFiles(outputDirectory.c_str())) {
                if (directoryOf(path) == normalizeSeparators(outputDirectory)) {
                    present.push_back(path);
                }
            }

            int swept = 0;
            for (const std::string& stray : cooker::strayOutputNames(expected, present)) {
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
    }

    SOL_LOG_INFO("cooker: %d cooked, %d up to date, %d failed", cooked, skipped, failed);
    return failed == 0 ? 0 : 1;
}
