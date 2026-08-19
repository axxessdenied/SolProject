#include "bc1.hpp"
#include "font.hpp"
#include "gltf.hpp"
#include "png.hpp"
#include "sound.hpp"

#include "sol/assets/forge_doc.hpp"
#include "sol/assets/formats.hpp"
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

bool isUpToDate(const std::string& source, const std::string& output)
{
    const std::uint64_t sourceTime = platform::fileModificationTime(source.c_str());
    const std::uint64_t outputTime = platform::fileModificationTime(output.c_str());
    return outputTime != 0 && sourceTime != 0 && outputTime >= sourceTime;
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

    assets::TextureFileHeader header = {};
    header.width = image.width;
    header.height = image.height;
    header.format = assets::TextureFormat::BC1;

    // Full mip chain, box-filtered.
    std::vector<std::vector<std::uint8_t>> mips;
    cooker::ImageRgba level = std::move(image);
    while (true) {
        mips.push_back(cooker::encodeBc1(level));
        if (level.width == 1 && level.height == 1) {
            break;
        }
        level = cooker::downsampleHalf(level);
    }
    header.mipCount = static_cast<std::uint32_t>(mips.size());

    std::vector<std::uint8_t> fileBytes(sizeof(header));
    std::memcpy(fileBytes.data(), &header, sizeof(header));
    for (const std::vector<std::uint8_t>& mip : mips) {
        fileBytes.insert(fileBytes.end(), mip.begin(), mip.end());
    }

    if (!platform::writeFileBytes(output.c_str(), fileBytes.data(), fileBytes.size())) {
        SOL_LOG_ERROR("cooker: cannot write %s", output.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%ux%u, %u mips)", source.c_str(), output.c_str(), header.width,
                 header.height, header.mipCount);
    return true;
}

bool writeMesh(const assets::MeshData& mesh, const std::string& source, const std::string& output)
{
    assets::MeshFileHeader header = {};
    header.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    header.indexCount = static_cast<std::uint32_t>(mesh.indices.size());

    std::vector<std::uint8_t> fileBytes(sizeof(header) +
                                        mesh.vertices.size() * sizeof(assets::MeshVertex) +
                                        mesh.indices.size() * sizeof(std::uint32_t));
    std::uint8_t* cursor = fileBytes.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    std::memcpy(cursor, mesh.vertices.data(), mesh.vertices.size() * sizeof(assets::MeshVertex));
    cursor += mesh.vertices.size() * sizeof(assets::MeshVertex);
    std::memcpy(cursor, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));

    if (!platform::writeFileBytes(output.c_str(), fileBytes.data(), fileBytes.size())) {
        SOL_LOG_ERROR("cooker: cannot write %s", output.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%u vertices, %u indices)", source.c_str(), output.c_str(),
                 header.vertexCount, header.indexCount);
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
    std::vector<std::uint8_t> sourceBytes;
    if (!platform::readFileBytes(source.c_str(), sourceBytes)) {
        SOL_LOG_ERROR("cooker: cannot read %s", source.c_str());
        return false;
    }

    assets::ForgeDoc doc;
    std::string error;
    if (!assets::parseForge(reinterpret_cast<const char*>(sourceBytes.data()), sourceBytes.size(),
                            source.c_str(), doc, &error)) {
        SOL_LOG_ERROR("cooker: %s", error.c_str());
        return false;
    }

    assets::MeshData mesh;
    if (!assets::buildForge(doc, mesh, &error)) {
        SOL_LOG_ERROR("cooker: %s: %s", source.c_str(), error.c_str());
        return false;
    }
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        SOL_LOG_ERROR("cooker: %s builds no geometry", source.c_str());
        return false;
    }
    return writeMesh(mesh, source, output);
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
    if (!cooker::bakeFont(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size(),
                          directoryOf(source), font, &error)) {
        SOL_LOG_ERROR("cooker: %s: %s", source.c_str(), error.c_str());
        return false;
    }

    const std::vector<std::uint8_t> fileBytes = cooker::encodeFont(font);
    if (!platform::writeFileBytes(output.c_str(), fileBytes.data(), fileBytes.size())) {
        SOL_LOG_ERROR("cooker: cannot write %s", output.c_str());
        return false;
    }
    SOL_LOG_INFO("cooked %s -> %s (%zu styles, %zu glyphs, %ux%u atlas)", source.c_str(), output.c_str(),
                 font.styles.size(), font.glyphs.size(), font.atlasWidth, font.atlasHeight);
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
    SOL_LOG_INFO("cooked %s -> %s (%u frames, %u ch, %u Hz)", source.c_str(), output.c_str(),
                 sound.frameCount(), sound.channelCount, sound.sampleRate);
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
        } else if (extension == ".gltf" || extension == ".glb") {
            job.output = outputDirectory + "/" + fileStem(source) + ".smesh";
            job.cook = &cookMesh;
        } else if (extension == ".forge") {
            job.output = outputDirectory + "/" + fileStem(source) + ".smesh";
            job.cook = &cookForge;
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
                SOL_LOG_ERROR("cooker: %s and %s both cook to %s", jobs[i].source.c_str(),
                              jobs[j].source.c_str(), jobs[i].output.c_str());
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

    SOL_LOG_INFO("cooker: %d cooked, %d up to date, %d failed", cooked, skipped, failed);
    return failed == 0 ? 0 : 1;
}
