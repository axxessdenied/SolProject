#include "TerrainLod.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sol::render::detail {
namespace {

/// The six cube faces, as an origin corner and two edge vectors in cube space.
struct CubeFace {
    Vec3d origin;
    Vec3d uAxis;
    Vec3d vAxis;
};

constexpr std::array<CubeFace, 6> kCubeFaces{
    CubeFace{{-1.0, -1.0, 1.0}, {2.0, 0.0, 0.0}, {0.0, 2.0, 0.0}},   // +Z
    CubeFace{{1.0, -1.0, -1.0}, {-2.0, 0.0, 0.0}, {0.0, 2.0, 0.0}},  // -Z
    CubeFace{{1.0, -1.0, 1.0}, {0.0, 0.0, -2.0}, {0.0, 2.0, 0.0}},   // +X
    CubeFace{{-1.0, -1.0, -1.0}, {0.0, 0.0, 2.0}, {0.0, 2.0, 0.0}},  // -X
    CubeFace{{-1.0, 1.0, 1.0}, {2.0, 0.0, 0.0}, {0.0, 0.0, -2.0}},   // +Y
    CubeFace{{-1.0, -1.0, -1.0}, {2.0, 0.0, 0.0}, {0.0, 0.0, 2.0}},  // -Y
};

/// Integer hash. Deterministic across runs and machines, which the LOD gate depends on: it
/// compares consecutive frames, so terrain that varied between evaluations would read as
/// popping the LOD scheme did not cause.
std::uint32_t hashCoordinates(std::int32_t x, std::int32_t y, std::int32_t z)
{
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8DA6B343U
                      ^ static_cast<std::uint32_t>(y) * 0xD8163841U
                      ^ static_cast<std::uint32_t>(z) * 0xCB1AB31FU;
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    h *= 0x297A2D39U;
    h ^= h >> 15U;
    return h;
}

double hashToUnit(std::int32_t x, std::int32_t y, std::int32_t z)
{
    return static_cast<double>(hashCoordinates(x, y, z)) / 4294967295.0;
}

/// Smoothstep-interpolated value noise. Cheap, continuous, and adequate for relief whose only
/// job is to give LOD transitions something to be visible against.
double valueNoise(const Vec3d& p)
{
    const double fx = std::floor(p.x);
    const double fy = std::floor(p.y);
    const double fz = std::floor(p.z);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const auto iz = static_cast<std::int32_t>(fz);

    const double tx = p.x - fx;
    const double ty = p.y - fy;
    const double tz = p.z - fz;

    const auto smooth = [](double t) { return t * t * (3.0 - 2.0 * t); };
    const double sx = smooth(tx);
    const double sy = smooth(ty);
    const double sz = smooth(tz);

    double result = 0.0;
    for (int dz = 0; dz < 2; ++dz) {
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const double weight = (dx == 0 ? 1.0 - sx : sx) * (dy == 0 ? 1.0 - sy : sy)
                                      * (dz == 0 ? 1.0 - sz : sz);
                result += weight * hashToUnit(ix + dx, iy + dy, iz + dz);
            }
        }
    }
    return result;
}

/// Maps a point on the cube to the sphere with less area distortion than plain normalisation.
Vec3d cubeToSphere(const Vec3d& cube)
{
    const double x2 = cube.x * cube.x;
    const double y2 = cube.y * cube.y;
    const double z2 = cube.z * cube.z;
    return Vec3d{
        cube.x * std::sqrt(1.0 - (y2 * 0.5) - (z2 * 0.5) + (y2 * z2 / 3.0)),
        cube.y * std::sqrt(1.0 - (z2 * 0.5) - (x2 * 0.5) + (z2 * x2 / 3.0)),
        cube.z * std::sqrt(1.0 - (x2 * 0.5) - (y2 * 0.5) + (x2 * y2 / 3.0)),
    };
}

/// Surface position for a point on a cube face, planet-relative and in metres, plus the
/// terrain height that produced it.
struct SurfaceSample {
    Vec3d position;
    double height = 0.0;
};

SurfaceSample surfacePoint(const CubeFace& face, double u, double v, const TerrainConfig& config)
{
    const Vec3d cube{
        face.origin.x + (face.uAxis.x * u) + (face.vAxis.x * v),
        face.origin.y + (face.uAxis.y * u) + (face.vAxis.y * v),
        face.origin.z + (face.uAxis.z * u) + (face.vAxis.z * v),
    };
    const Vec3d direction = normalise(cubeToSphere(cube));
    const double height = terrainHeight(direction, config);
    return SurfaceSample{direction * (config.radiusMetres + height), height};
}

struct QuadtreeNode {
    std::uint32_t face = 0;
    double u0 = 0.0;
    double v0 = 0.0;
    double size = 1.0;
    std::uint32_t level = 0;
};

/// Range within which a node of this level subdivides.
double subdivisionRange(const QuadtreeNode& node, const TerrainConfig& config)
{
    // A node's world extent is roughly its face fraction times the planet's diameter.
    const double worldSize = node.size * config.radiusMetres * 2.0;
    return worldSize * config.subdivisionFactor;
}

} // namespace

double terrainHeight(const Vec3d& unitDirection, const TerrainConfig& config)
{
    // Ten octaves spanning roughly 1 200 km down to 230 m of wavelength.
    //
    // The high-frequency end is not decoration, it is what lets the LOD gate measure anything
    // at all, and the requirement is specific rather than general. A patch's fine and coarse
    // grids differ only where the terrain varies *between* adjacent grid samples, so what a
    // transition can reveal is exactly the energy between the two grids' spacings. At the
    // levels this test exercises those spacings are about 1.6 km and 3.1 km.
    //
    // Two earlier spectra failed for that reason and are worth recording. Four octaves topped
    // out near 1 000 km: frame differences never reached 0.03 of a luminance level and no
    // scheme could pop. Seven octaves reached 4 km — visible terrain at last, but still no
    // energy in the 1.6-3.1 km band the transition actually uncovers, so disabling morphing
    // changed no pixel by more than 15 of 255 levels at any quality setting tried.
    //
    // Amplitude now falls at 0.72 per octave rather than 0.62, which puts roughly 4% of the
    // relief at 3 km wavelength instead of 2% at 4 km. Combined with the stress-scene relief
    // the LOD harness selects, that is a real slope where the grids differ rather than a
    // rounding error.
    double amplitude = 1.0;
    double frequency = 32.0;
    double sum = 0.0;
    double normalisation = 0.0;

    for (int octave = 0; octave < 10; ++octave) {
        sum += amplitude * valueNoise(unitDirection * frequency);
        normalisation += amplitude;
        amplitude *= 0.72;
        frequency *= 2.6;
    }

    const double unit = (sum / normalisation) - 0.5;
    return unit * config.reliefMetres;
}

void buildTerrain(
    const TerrainConfig& config,
    const Vec3d& cameraPlanetRelative,
    TerrainFrame& out)
{
    out.vertices.clear();
    out.indices.clear();
    out.patches.clear();
    out.nodesVisited = 0;

    const std::uint32_t grid = config.gridResolution;
    const std::uint32_t verticesPerEdge = grid + 1;

    // Emits one node as a patch, generating its grid and its coarse-level counterpart.
    const auto emit = [&](const QuadtreeNode& node, double morph) {
        const CubeFace& face = kCubeFaces[node.face];
        const auto baseVertex = static_cast<std::int32_t>(out.vertices.size());
        const auto firstIndex = static_cast<std::uint32_t>(out.indices.size());

        for (std::uint32_t j = 0; j < verticesPerEdge; ++j) {
            for (std::uint32_t i = 0; i < verticesPerEdge; ++i) {
                const double fu = static_cast<double>(i) / static_cast<double>(grid);
                const double fv = static_cast<double>(j) / static_cast<double>(grid);

                const double u = node.u0 + (fu * node.size);
                const double v = node.v0 + (fv * node.size);

                // The coarse counterpart: the same vertex sampled on a grid of half the
                // resolution, which is where this vertex lands one level up. Snapping the
                // index to even values is exactly how the parent grid samples this patch.
                const std::uint32_t ci = i & ~1U;
                const std::uint32_t cj = j & ~1U;
                const double cfu = static_cast<double>(ci) / static_cast<double>(grid);
                const double cfv = static_cast<double>(cj) / static_cast<double>(grid);
                const double cu = node.u0 + (cfu * node.size);
                const double cv = node.v0 + (cfv * node.size);

                const SurfaceSample fine = surfacePoint(face, u, v, config);
                const SurfaceSample coarse = surfacePoint(face, cu, cv, config);

                // The one narrowing point. Both operands are ~6.4e6 m and their difference is
                // small near the viewer; subtracting in double and only then converting keeps
                // that difference exact, which is what a float cannot do the other way round.
                const Vec3d fineRelative = fine.position - cameraPlanetRelative;
                const Vec3d coarseRelative = coarse.position - cameraPlanetRelative;

                const auto toUnit = [&config](double height) {
                    return static_cast<float>((height / config.reliefMetres) + 0.5);
                };

                out.vertices.push_back(TerrainVertex{
                    .position = {static_cast<float>(fineRelative.x),
                                 static_cast<float>(fineRelative.y),
                                 static_cast<float>(fineRelative.z)},
                    .coarsePosition = {static_cast<float>(coarseRelative.x),
                                       static_cast<float>(coarseRelative.y),
                                       static_cast<float>(coarseRelative.z)},
                    .heightUnit = toUnit(fine.height),
                    .coarseHeightUnit = toUnit(coarse.height),
                });
            }
        }

        for (std::uint32_t j = 0; j < grid; ++j) {
            for (std::uint32_t i = 0; i < grid; ++i) {
                const std::uint32_t topLeft = (j * verticesPerEdge) + i;
                const std::uint32_t topRight = topLeft + 1;
                const std::uint32_t bottomLeft = topLeft + verticesPerEdge;
                const std::uint32_t bottomRight = bottomLeft + 1;

                out.indices.push_back(topLeft);
                out.indices.push_back(bottomLeft);
                out.indices.push_back(topRight);
                out.indices.push_back(topRight);
                out.indices.push_back(bottomLeft);
                out.indices.push_back(bottomRight);
            }
        }

        out.patches.push_back(TerrainPatch{
            .firstIndex = firstIndex,
            .indexCount = grid * grid * 6,
            .vertexOffset = baseVertex,
            .morph = config.morphEnabled ? static_cast<float>(morph) : 0.0F,
            .level = node.level,
        });
    };

    // Depth-first selection. Explicit stack rather than recursion so the traversal cost is
    // visible and bounded.
    std::vector<QuadtreeNode> stack;
    stack.reserve(256);
    for (std::uint32_t face = 0; face < kCubeFaces.size(); ++face) {
        stack.push_back(QuadtreeNode{.face = face, .u0 = 0.0, .v0 = 0.0, .size = 1.0, .level = 0});
    }

    while (!stack.empty()) {
        const QuadtreeNode node = stack.back();
        stack.pop_back();
        ++out.nodesVisited;

        const Vec3d centre =
            surfacePoint(kCubeFaces[node.face], node.u0 + (node.size * 0.5),
                         node.v0 + (node.size * 0.5), config)
                .position;
        const double distance = length(centre - cameraPlanetRelative);
        const double range = subdivisionRange(node, config);

        const bool canSubdivide = node.level < config.maxLevel;
        if (canSubdivide && distance < range) {
            const double half = node.size * 0.5;
            for (int child = 0; child < 4; ++child) {
                stack.push_back(QuadtreeNode{
                    .face = node.face,
                    .u0 = node.u0 + ((child & 1) != 0 ? half : 0.0),
                    .v0 = node.v0 + ((child & 2) != 0 ? half : 0.0),
                    .size = half,
                    .level = node.level + 1,
                });
            }
            continue;
        }

        // Morph factor, measured against the *parent's* range rather than this node's.
        //
        // The distinction is the whole mechanism, and getting it wrong is silent. A node is
        // emitted precisely when `distance >= range`, so morphing against `range` puts every
        // emitted node past the end of its own band and pins the factor at 1 — the terrain
        // then renders permanently at the coarse position, morphing nothing, and a popping
        // test sees a consistent image either way and reports no pops. That is a passing
        // result from a mechanism that is not running.
        //
        // The node actually survives until its *parent* would stop subdividing, at twice this
        // range. Blending across the top of that interval means the geometry has already
        // become the parent's by the time the parent takes over, which is what leaves nothing
        // to pop.
        double morph = 0.0;
        if (node.level > 0) {
            const double parentRange = range * 2.0;
            const double bandStart = parentRange * (1.0 - config.morphBand);
            if (distance > bandStart && parentRange > bandStart) {
                morph = std::clamp((distance - bandStart) / (parentRange - bandStart), 0.0, 1.0);
            }
        }

        // Cull nodes hidden behind the planet, conservatively.
        //
        // A point P is above the horizon from a camera at C when its projection along the
        // camera direction reaches the horizon plane, which sits at `occluder^2 / |C|` from
        // the planet centre. That is the exact test; what matters for the LOD gate is that it
        // is applied to the node's whole extent rather than to its centre, and that both radii
        // err toward drawing.
        //
        // The previous version subtracted `node.size * 2.0` — a fraction of a cube face — from
        // a cosine. Mixing a size fraction into an angular quantity is dimensionally
        // meaningless, and the result was a threshold that admitted and dropped patches while
        // they were still on screen. Each crossing appeared or removed a patch in a single
        // frame, which is a pop that no amount of LOD morphing can smooth: morphing blends
        // tessellation changes, not visibility changes. The LOD gate measured exactly that,
        // as two pops that survived with morphing enabled and occurred at the same descent
        // steps with it disabled.
        //
        // Conservative on both sides is what removes the pop rather than merely moving it. The
        // occluder is shrunk by the relief band, since terrain that dips below the reference
        // radius occludes less; the node's bounding radius is grown by it, since terrain that
        // rises above the radius is visible sooner. A node therefore becomes eligible to draw
        // while it is still genuinely hidden, and contributes nothing on the frame it appears.
        const double cameraDistance = length(cameraPlanetRelative);
        const double occluderRadius = std::max(0.0, config.radiusMetres - config.reliefMetres);

        if (cameraDistance > occluderRadius) {
            const Vec3d cameraDirection = normalise(cameraPlanetRelative);
            const double horizonPlane = (occluderRadius * occluderRadius) / cameraDistance;

            // Half the node's diagonal across the sphere, plus the relief band. The 0.75
            // factor covers a square patch's corner-to-centre distance with margin.
            const double nodeWorldSize = node.size * config.radiusMetres * 2.0;
            const double boundingRadius = (nodeWorldSize * 0.75) + config.reliefMetres;

            if (dot(centre, cameraDirection) + boundingRadius < horizonPlane) {
                continue;
            }
        }

        emit(node, morph);
    }
}

} // namespace sol::render::detail
