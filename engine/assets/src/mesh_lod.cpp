#include "sol/assets/mesh_lod.hpp"

#include "sol/assets/formats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iterator>

namespace sol::assets {
namespace {

[[nodiscard]] double relativeDrift(double value, double reference)
{
    if (reference == 0.0) {
        return value == 0.0 ? 0.0 : 1.0;
    }
    return (value - reference) / std::abs(reference);
}

[[nodiscard]] std::string say(const char* format, ...)
{
    // Small, fixed messages; the author reads these in the Forge and in the
    // cooker's log, so they say what happened rather than naming a code.
    char buffer[256] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}

} // namespace

std::size_t cookedMeshBytes(const MeshData& mesh)
{
    return sizeof(MeshFileHeader) + (mesh.vertices.size() * sizeof(MeshVertex)) +
           (mesh.indices.size() * sizeof(std::uint32_t));
}

LodChain buildLodChain(const MeshData& source, const LodOptions& options)
{
    LodChain chain;

    const EditMesh base = toEditMesh(source, options.weld);
    const std::uint32_t sourceTriangles = base.triangleCount();
    if (sourceTriangles < options.minimumSourceTriangles) {
        chain.stopReason =
            say("%u triangles is under the %u-triangle floor - a mesh this small has no "
                "redundancy to give up, and decimating it would cost shape rather than detail",
                sourceTriangles, options.minimumSourceTriangles);
        return chain;
    }

    const double sourceVolume = signedVolume(base);
    const double sourceRadius = static_cast<double>(boundingRadius(base));
    const std::size_t sourceBytes = cookedMeshBytes(source);
    // An open surface has no meaningful volume, so the band that would be
    // measured against zero is skipped rather than being made to mean nothing.
    // Nothing committed reaches here (the only open mesh in the repo is 64
    // triangles, well under the floor), so this path has its own test.
    const bool closedSource = buildAdjacency(base).isClosed();

    std::uint32_t previousTriangles = sourceTriangles;
    std::size_t previousBytes = sourceBytes;

    for (std::uint32_t level = 1; level <= options.maxLevels; ++level) {
        const auto target =
            static_cast<std::uint32_t>(static_cast<float>(sourceTriangles) *
                                       std::pow(options.ratio, static_cast<float>(level)));
        if (target == 0) {
            chain.stopReason = say("level %u would target no triangles at all", level);
            break;
        }

        // ⚑ From the SOURCE every time, never from the level above: error does
        // not compound, and every drift below is measured against the mesh the
        // game actually ships.
        EditMesh mesh = base;
        const std::uint32_t reached = decimate(mesh, {target, 0.0, true});
        if (reached >= previousTriangles) {
            chain.stopReason =
                say("the topology refused to collapse below %u triangles, so level %u would be "
                    "no cheaper than the one above it",
                    reached, level);
            break;
        }

        const double volume = signedVolume(mesh);
        const double radius = static_cast<double>(boundingRadius(mesh));
        const double volumeDrift = relativeDrift(volume, sourceVolume);
        const double radiusDrift = relativeDrift(radius, sourceRadius);

        if (closedSource && std::abs(volumeDrift) > options.maxVolumeDrift) {
            chain.stopReason =
                say("level %u moved %.2f%% of the volume, past the %.2f%% band - at %u triangles "
                    "this is no longer the same shape",
                    level, volumeDrift * 100.0, options.maxVolumeDrift * 100.0, reached);
            break;
        }
        if (std::abs(radiusDrift) > options.maxRadiusDrift) {
            chain.stopReason =
                say("level %u moved the bounding radius %.2f%%, past the %.2f%% band - the "
                    "silhouette would no longer agree with the collision sphere",
                    level, radiusDrift * 100.0, options.maxRadiusDrift * 100.0);
            break;
        }

        // A collapse moves points, so the normals around them are stale.
        // Smooth rather than flat: flat unshares every corner and is what makes
        // a level cost more bytes than the mesh it replaces.
        recomputeNormals(mesh, options.smoothAngleDegrees);
        // Safe to run unconditionally since stage F - it keeps the better of
        // the two orders, so it cannot make a level worse than it found it.
        optimizeIndices(mesh);

        MeshLevel out;
        out.mesh = toMeshData(mesh);
        out.triangles = reached;
        out.boundingRadius = static_cast<float>(radius);
        out.volume = volume;
        out.cookedBytes = cookedMeshBytes(out.mesh);
        out.volumeDrift = volumeDrift;
        out.radiusDrift = radiusDrift;

        if (out.cookedBytes >= previousBytes) {
            chain.stopReason =
                say("level %u cooks to %zu bytes against the %zu above it - a level that is not "
                    "smaller than its source is not a level",
                    level, out.cookedBytes, previousBytes);
            break;
        }

        previousTriangles = out.triangles;
        previousBytes = out.cookedBytes;
        chain.levels.push_back(std::move(out));
    }

    if (chain.stopReason.empty()) {
        chain.stopReason = say("generated %zu level(s), the maximum asked for", chain.levels.size());
    }
    return chain;
}

std::uint32_t selectMeshLevel(float screenRadiusPixels, std::uint32_t levelCount,
                              std::uint32_t previousLevel)
{
    if (levelCount <= 1) {
        return 0;
    }
    constexpr std::uint32_t kThresholdCount =
        static_cast<std::uint32_t>(std::size(kLevelSwitchPixels));
    std::uint32_t level = 0;
    // A NaN radius must not walk the chain: every comparison against it is
    // false, which leaves the level at 0 - the most detailed, never the
    // cheapest, so a bad number degrades into extra work rather than into a
    // visibly wrong draw.
    while (level < kThresholdCount && screenRadiusPixels < kLevelSwitchPixels[level]) {
        ++level;
    }
    level = std::min(level, levelCount - 1);

    // ⚑ Phase 18. With no history this is exactly the stateless answer above,
    // which is what keeps a first sight - and every model with no chain -
    // bit-identical to stage F.
    if (previousLevel == kNoPreviousLevel || previousLevel >= levelCount) {
        return level;
    }
    // ⚑ THE MARGIN IS ONLY EVER SPENT GIVING DETAIL BACK. Dropping detail
    // still happens exactly at the threshold Phase 17 measured, so those two
    // numbers keep the pixel budget behind them; it is the return trip that
    // has to climb past the threshold by `kLevelSwitchHysteresis` before it
    // counts. Written as `>=` against the raised bar so a level that has not
    // earned its way back simply stays put.
    if (level < previousLevel) {
        // Going UP in detail: every threshold between here and the level we
        // are holding must be cleared with the margin, not just the nearest -
        // a single frame can span two bands if the camera cuts.
        std::uint32_t held = previousLevel;
        while (held > level
               && screenRadiusPixels
                      >= kLevelSwitchPixels[held - 1] * (1.0f + kLevelSwitchHysteresis)) {
            --held;
        }
        return held;
    }
    // Going DOWN in detail, or unchanged: the measured threshold, unmodified.
    return level;
}

} // namespace sol::assets
