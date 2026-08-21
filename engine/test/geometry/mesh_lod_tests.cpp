// LOD chain generation and level selection (engine plan Phase 9 stage F).
//
// ⚑ These assert INVARIANTS AND RELATIONS over the committed assets, never
// their exact counts. Phase 16 deleted the shipped-mesh ratchet for a reason:
// a broken asset must fail and a legitimately edited one must not, and a
// pinned triangle count is the same ratchet in different clothes. The measured
// numbers live in the plan's record, not in an assertion here.

#include "sol/assets/forge_doc.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/assets/mesh_lod.hpp"
#include "sol/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sol;
using assets::EditMesh;
using assets::LodChain;
using assets::LodOptions;
using assets::MeshData;

namespace {

// Every committed source. `gate_membrane` is the only open surface in the repo
// and is deliberately in the list: it is the one that proves the floor refuses
// rather than the one that proves a chain generates.
constexpr const char* kAssets[] = {"asteroid", "cockpit", "cube",   "gate",
                                   "gate_membrane", "ship", "station"};

[[nodiscard]] bool buildAsset(const char* name, MeshData& out)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    assets::ForgeDoc doc;
    std::string error;
    if (!assets::parseForge(text.data(), text.size(), path.c_str(), doc, &error)) {
        return false;
    }
    return assets::buildForge(doc, out, &error);
}

// A flat grid of quads: open, zero volume, and big enough to clear the floor.
// It exists to exercise the path no committed asset can reach - the only open
// mesh in the repo is 64 triangles, well under the floor - because a branch
// that is only ever taken defensively is a branch nothing has checked.
[[nodiscard]] MeshData buildOpenGrid(std::uint32_t cells)
{
    MeshData mesh;
    const float step = 1.0f / static_cast<float>(cells);
    for (std::uint32_t y = 0; y <= cells; ++y) {
        for (std::uint32_t x = 0; x <= cells; ++x) {
            assets::MeshVertex vertex = {};
            vertex.position[0] = static_cast<float>(x) * step;
            vertex.position[1] = 0.0f;
            vertex.position[2] = static_cast<float>(y) * step;
            vertex.normal[1] = 1.0f;
            vertex.uv[0] = static_cast<float>(x) * step;
            vertex.uv[1] = static_cast<float>(y) * step;
            mesh.vertices.push_back(vertex);
        }
    }
    const std::uint32_t stride = cells + 1;
    for (std::uint32_t y = 0; y < cells; ++y) {
        for (std::uint32_t x = 0; x < cells; ++x) {
            const std::uint32_t a = (y * stride) + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + stride;
            const std::uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}

} // namespace

// ⚑⚑ THE BAND THAT CATCHES THE ONE MISTAKE THIS STAGE IS MOST LIKELY TO MAKE.
// Following `mesh_edit.hpp`'s own correct advice - recompute the normals after
// a collapse - with FLAT normals unshares every corner at three vertices per
// triangle, and the station's half-triangle level then cooks to about 49% MORE
// bytes than the source it replaces. A level that is not smaller is not a
// level, and nothing about its triangle count would have said so.
SOL_TEST(everyGeneratedLevelIsSmallerInCookedBytesThanTheOneAboveIt)
{
    std::uint32_t chainsSeen = 0;
    for (const char* name : kAssets) {
        MeshData source;
        SOL_REQUIRE(buildAsset(name, source));
        const LodChain chain = assets::buildLodChain(source);
        std::size_t previous = assets::cookedMeshBytes(source);
        for (const assets::MeshLevel& level : chain.levels) {
            if (level.cookedBytes >= previous) {
                std::printf("  %s: a level cooks to %zu bytes against %zu above it\n", name,
                            level.cookedBytes, previous);
            }
            SOL_CHECK(level.cookedBytes < previous);
            SOL_CHECK(level.triangles > 0);
            previous = level.cookedBytes;
        }
        if (!chain.levels.empty()) {
            ++chainsSeen;
        }
    }
    // Without this the whole test passes on a generator that returns nothing -
    // the vacuous pass Phase 15's contact-direction test shipped with.
    SOL_CHECK(chainsSeen > 0);
}

// ⚑⚑ A CUBE DECIMATED TO TWO TRIANGLES IS STILL CLOSED, STILL MANIFOLD AND
// STILL BORDER-FREE WHILE ENCLOSING NOTHING. Phase 16's invariants cannot tell
// a solid from a flat sliver, so the topology checks below are the cheap half
// and the VOLUME BAND is the half that does the work - the third time stage E
// and stage F have landed on that same fact.
SOL_TEST(everyLevelOfAClosedSourceStaysASolidWithinTheVolumeBand)
{
    const LodOptions options;
    std::uint32_t levelsSeen = 0;
    for (const char* name : kAssets) {
        MeshData source;
        SOL_REQUIRE(buildAsset(name, source));
        const EditMesh base = assets::toEditMesh(source);
        if (!assets::buildAdjacency(base).isClosed()) {
            continue; // an open film has no volume to hold
        }
        const double sourceVolume = assets::signedVolume(base);
        const LodChain chain = assets::buildLodChain(source, options);
        for (const assets::MeshLevel& level : chain.levels) {
            const EditMesh mesh = assets::toEditMesh(level.mesh);
            const assets::MeshAdjacency adjacency = assets::buildAdjacency(mesh);
            SOL_CHECK(adjacency.isManifold());
            SOL_CHECK(adjacency.isClosed());
            SOL_CHECK(adjacency.borderEdgeCount() == 0);
            // Wound outwards, which a reversed level would fail and the three
            // checks above would not.
            SOL_CHECK(assets::signedVolume(mesh) > 0.0);
            const double drift =
                std::abs(assets::signedVolume(mesh) - sourceVolume) / std::abs(sourceVolume);
            if (drift > options.maxVolumeDrift) {
                std::printf("  %s: a level moved %.2f%% of the volume\n", name, drift * 100.0);
            }
            SOL_CHECK(drift <= options.maxVolumeDrift);
            ++levelsSeen;
        }
    }
    SOL_CHECK(levelsSeen > 0);
}

// ⚑⚑ THE RADIUS GROWS, WHICH IS THE OPPOSITE OF THE OBVIOUS PREDICTION. A
// quadric's optimal position is not constrained to the original hull, so a
// collapse pushes points OUTWARD - measured, the station gains 1.89% at a tenth
// of its triangles. `models.toml` already gives it radius 100 against a mesh
// that measures 102, which the E1-E3 debt slice recorded as the collision
// sphere sitting INSIDE the hull, so an unbounded LOD widens a defect this
// project has already measured and written down.
SOL_TEST(noLevelsSilhouetteLeavesTheRadiusBand)
{
    const LodOptions options;
    for (const char* name : kAssets) {
        MeshData source;
        SOL_REQUIRE(buildAsset(name, source));
        const float sourceRadius = assets::boundingRadius(assets::toEditMesh(source));
        const LodChain chain = assets::buildLodChain(source, options);
        for (const assets::MeshLevel& level : chain.levels) {
            const double drift =
                std::abs(static_cast<double>(level.boundingRadius - sourceRadius)) /
                static_cast<double>(sourceRadius);
            if (drift > options.maxRadiusDrift) {
                std::printf("  %s: a level moved the radius %.2f%% (%.4f -> %.4f)\n", name,
                            drift * 100.0, static_cast<double>(sourceRadius),
                            static_cast<double>(level.boundingRadius));
            }
            SOL_CHECK(drift <= options.maxRadiusDrift);
        }
    }
}

// ⚑⚑ THE ASSERTION `optimizeIndices` MAKES ABOUT ITSELF, OVER THE INPUTS THAT
// BREAK IT. Its own test runs on a synthetic icosphere and passes; run the same
// claim over the committed assets and it FAILED on two of seven before stage F
// made the pass keep the better of the two orders - the gate 0.804 -> 0.895 and
// the station 0.701 -> 0.810, because `MeshBuilder` already emits a near-optimal
// order and a greedy heuristic cannot beat one that is already good.
//
// This is E4d's lesson inverted: there a synthetic input was needed because no
// asset could fail, here the synthetic input PASSED what every real asset broke.
SOL_TEST(indexOptimisationNeverRaisesTheCacheMissRatioOnACommittedAsset)
{
    for (const char* name : kAssets) {
        MeshData source;
        SOL_REQUIRE(buildAsset(name, source));
        EditMesh mesh = assets::toEditMesh(source);
        const float before = assets::averageCacheMissRatio(mesh);
        const double volume = assets::signedVolume(mesh);
        const std::uint32_t triangles = mesh.triangleCount();

        assets::optimizeIndices(mesh);
        const float after = assets::averageCacheMissRatio(mesh);
        if (after > before) {
            std::printf("  %s: cache miss ratio got worse, %.3f -> %.3f\n", name,
                        static_cast<double>(before), static_cast<double>(after));
        }
        SOL_CHECK(after <= before);
        // Reordering is not remodelling, asserted on the real assets too.
        SOL_CHECK(mesh.triangleCount() == triangles);
        SOL_CHECK(std::abs(assets::signedVolume(mesh) - volume) < 1e-6);
    }
}

// A hand-built part has no redundancy to give up: measured, the 12-triangle
// cube loses 66.67% of its volume at half its triangles while the station loses
// 0.35%. So the floor refuses, and the refusal says why in words an author can
// act on rather than returning an empty list.
SOL_TEST(aMeshUnderTheFloorIsRefusedAndTheReasonNamesTheFloor)
{
    MeshData cube;
    SOL_REQUIRE(buildAsset("cube", cube));
    const LodChain chain = assets::buildLodChain(cube);
    SOL_CHECK(chain.levels.empty());
    SOL_CHECK(chain.stopReason.find("floor") != std::string::npos);

    // And the floor is what refused it, not some other limit: lower the floor
    // and the same mesh generates. This is the can-fail half - without it the
    // test passes on a generator that refuses everything for any reason.
    LodOptions permissive;
    permissive.minimumSourceTriangles = 2;
    permissive.maxVolumeDrift = 1.0;
    permissive.maxRadiusDrift = 1.0;
    const LodChain forced = assets::buildLodChain(cube, permissive);
    SOL_CHECK(!forced.levels.empty());
}

// ⚑⚑ THE FINDING ITSELF, PINNED DIRECTLY RATHER THAN THROUGH THE GUARD THAT
// ACTS ON IT. `mesh_edit.hpp` correctly warns that a collapse leaves normals
// stale and says to recompute them; doing exactly that with FLAT normals
// unshares every corner at three vertices per triangle, and the level comes out
// costing more than the mesh it replaces - the station's first level cooks to
// about 49% MORE bytes than its source. Smooth keeps the corners shared.
//
// ⚑ It is asserted on an asset where BOTH shadings clear the generator's byte
// guard, so the comparison is between two levels that were actually built
// rather than between one level and a refusal. Testing this through the guard
// alone would be near-tautological: the guard rejects the flat level, so the
// invariant test never sees it and passes either way. That is what running the
// mutation showed, and it is why this test exists separately.
SOL_TEST(reshadingFlatCostsMoreBytesThanSmoothWhichIsWhyLevelsAreSmoothShaded)
{
    MeshData asteroid;
    SOL_REQUIRE(buildAsset("asteroid", asteroid));

    LodOptions smooth; // the shipping configuration
    LodOptions flat = smooth;
    flat.smoothAngleDegrees = 0.0f; // per the header's advice, taken literally

    const LodChain smoothChain = assets::buildLodChain(asteroid, smooth);
    const LodChain flatChain = assets::buildLodChain(asteroid, flat);
    SOL_REQUIRE(!smoothChain.levels.empty());
    SOL_REQUIRE(!flatChain.levels.empty());

    // Same geometry either way - the shading choice must not move a triangle.
    SOL_CHECK(smoothChain.levels[0].triangles == flatChain.levels[0].triangles);
    if (flatChain.levels[0].cookedBytes <= smoothChain.levels[0].cookedBytes) {
        std::printf("  flat %zu bytes vs smooth %zu\n", flatChain.levels[0].cookedBytes,
                    smoothChain.levels[0].cookedBytes);
    }
    SOL_CHECK(flatChain.levels[0].cookedBytes > smoothChain.levels[0].cookedBytes);
}

// ⚑⚑ EACH BAND IS SEPARATELY CAN-FAIL, AND THE INPUT HAD TO BE BUILT BECAUSE
// NO COMMITTED ASSET CAN TRIP EITHER ONE AT THE SHIPPING VALUES. Measured, the
// deepest drift any of the seven reaches inside two levels is 0.78% of radius
// and 4.13% of volume, comfortably inside the 1% and 10% bands - so deleting
// either check outright would leave every other test in this file GREEN. That
// is E4d's situation exactly, and the answer is the same: build the input.
//
// Tightening one band to zero while leaving the other wide is what isolates
// them, so each failure can only have come from the band under test. Note this
// asserts the MECHANISM - that the band is consulted and says which one it was
// - and not the shipping values, which would be a ratchet.
SOL_TEST(eachAcceptanceBandRefusesOnItsOwnAndSaysWhichOneItWas)
{
    MeshData station;
    SOL_REQUIRE(buildAsset("station", station));

    LodOptions volumeOnly;
    volumeOnly.maxVolumeDrift = 0.0;
    volumeOnly.maxRadiusDrift = 1.0;
    const LodChain byVolume = assets::buildLodChain(station, volumeOnly);
    SOL_CHECK(byVolume.levels.empty());
    SOL_CHECK(byVolume.stopReason.find("volume") != std::string::npos);

    LodOptions radiusOnly;
    radiusOnly.maxVolumeDrift = 1.0;
    radiusOnly.maxRadiusDrift = 0.0;
    const LodChain byRadius = assets::buildLodChain(station, radiusOnly);
    SOL_CHECK(byRadius.levels.empty());
    SOL_CHECK(byRadius.stopReason.find("radius") != std::string::npos);

    // Both wide: the same mesh generates, so the refusals above are the bands
    // talking and not the floor or the topology.
    LodOptions wide;
    wide.maxVolumeDrift = 1.0;
    wide.maxRadiusDrift = 1.0;
    SOL_CHECK(!assets::buildLodChain(station, wide).levels.empty());
}

// The open path, which no committed asset can reach. A film has no volume, so
// the band that would be measured against zero is skipped rather than being
// made to mean nothing - and `preserveBorders` is what keeps its outline, which
// is the only reason its radius still holds.
SOL_TEST(anOpenSurfaceGeneratesLevelsWithoutAVolumeToHold)
{
    const MeshData grid = buildOpenGrid(16); // 512 triangles, over the floor
    const EditMesh base = assets::toEditMesh(grid);
    SOL_REQUIRE(base.triangleCount() == 512);
    SOL_REQUIRE(!assets::buildAdjacency(base).isClosed());

    const LodChain chain = assets::buildLodChain(grid);
    SOL_CHECK(!chain.levels.empty());
    std::size_t previous = assets::cookedMeshBytes(grid);
    for (const assets::MeshLevel& level : chain.levels) {
        SOL_CHECK(level.cookedBytes < previous);
        previous = level.cookedBytes;
        // The outline is what a border-preserving collapse must not move.
        SOL_CHECK(std::abs(level.boundingRadius - assets::boundingRadius(base)) < 1e-5f);
    }
}

// ⚑ Level selection is a pure function of one number, and no committed asset
// can demonstrate a wrong pick - so the input is BUILT rather than found, per
// E4d. The thresholds are radii in pixels and the chain is clamped to the
// levels that actually exist, which is what stops a two-level model reaching
// for a third.
SOL_TEST(levelSelectionWalksTheThresholdsAndClampsToWhatExists)
{
    constexpr std::uint32_t kLevels = 3; // level 0 plus two generated

    // Above the first threshold: the most detailed level, always.
    SOL_CHECK(assets::selectMeshLevel(1000.0f, kLevels) == 0);
    SOL_CHECK(assets::selectMeshLevel(assets::kLevelSwitchPixels[0], kLevels) == 0);
    // Below it: one step down. The boundary belongs to the level above, so a
    // radius exactly on the threshold does not switch.
    SOL_CHECK(assets::selectMeshLevel(assets::kLevelSwitchPixels[0] - 0.01f, kLevels) == 1);
    SOL_CHECK(assets::selectMeshLevel(assets::kLevelSwitchPixels[1], kLevels) == 1);
    SOL_CHECK(assets::selectMeshLevel(assets::kLevelSwitchPixels[1] - 0.01f, kLevels) == 2);
    SOL_CHECK(assets::selectMeshLevel(0.0f, kLevels) == 2);

    // A model with no chain draws level 0 however small it gets - which is
    // every model in the game today.
    SOL_CHECK(assets::selectMeshLevel(0.0f, 1) == 0);
    SOL_CHECK(assets::selectMeshLevel(0.0f, 0) == 0);
    // And a two-level model clamps rather than reaching for a third.
    SOL_CHECK(assets::selectMeshLevel(0.0f, 2) == 1);

    // A bad number degrades into extra work, never into a visibly wrong draw.
    SOL_CHECK(assets::selectMeshLevel(std::nanf(""), kLevels) == 0);
}
