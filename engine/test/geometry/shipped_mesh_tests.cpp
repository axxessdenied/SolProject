#include "shipped_meshes.hpp"

#include "sol/assets/forge_doc.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/core/hash.hpp"
#include "sol/test/test.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace sol;
using assets::MeshData;

namespace {

[[nodiscard]] std::uint64_t hashBytes(const void* data, std::size_t size)
{
    return core::fnv1a(std::string_view(static_cast<const char*>(data), size));
}

// The glTF the script writes stores the attributes PLANAR - all positions,
// then all normals, then all uvs, then uint16 indices - so the hashes below
// are taken over that layout rather than over the interleaved MeshVertex the
// pipeline wants. Same numbers, read the way the file holds them.
[[nodiscard]] std::uint64_t hashPositions(const MeshData& mesh)
{
    std::vector<float> flat;
    flat.reserve(mesh.vertices.size() * 3);
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        flat.insert(flat.end(), {vertex.position[0], vertex.position[1], vertex.position[2]});
    }
    return hashBytes(flat.data(), flat.size() * sizeof(float));
}

[[nodiscard]] std::uint64_t hashNormals(const MeshData& mesh)
{
    std::vector<float> flat;
    flat.reserve(mesh.vertices.size() * 3);
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        flat.insert(flat.end(), {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
    }
    return hashBytes(flat.data(), flat.size() * sizeof(float));
}

[[nodiscard]] std::uint64_t hashUvs(const MeshData& mesh)
{
    std::vector<float> flat;
    flat.reserve(mesh.vertices.size() * 2);
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        flat.insert(flat.end(), {vertex.uv[0], vertex.uv[1]});
    }
    return hashBytes(flat.data(), flat.size() * sizeof(float));
}

[[nodiscard]] std::uint64_t hashIndices(const MeshData& mesh)
{
    std::vector<std::uint16_t> flat;
    flat.reserve(mesh.indices.size());
    for (const std::uint32_t index : mesh.indices) {
        flat.push_back(static_cast<std::uint16_t>(index));
    }
    return hashBytes(flat.data(), flat.size() * sizeof(std::uint16_t));
}

struct MeshTruth
{
    const char* name;
    std::uint32_t vertexCount;
    std::uint32_t indexCount;
    float minBound[3];
    float maxBound[3];
    std::uint64_t positionHash;
    std::uint64_t normalHash;
    std::uint64_t uvHash;
    std::uint64_t indexHash;
};

// ⚑ Ground truth read straight out of the committed assets/meshes/*.gltf: the
// accessor counts, the position bounds, and an FNV-1a-64 over each attribute
// array's exact bytes. A hash rather than the arrays themselves because the
// asteroid alone is 960 vertices, and bit-exact is the assertion that matters:
// these five files were emitted by a PowerShell script computing in double and
// rounding to float, and the C++ port has to land on the same floats or it is
// not a port. The bounds and counts are here so a failure says WHICH mesh and
// roughly where, instead of just "a hash differs".
constexpr MeshTruth kTruth[] = {
    {"cube",
     24,
     36,
     {-0.5f, -0.5f, -0.5f},
     {0.5f, 0.5f, 0.5f},
     0xD54F551B2AE3C265ull,
     0x6032C0607D875265ull,
     0x7F80BDFB898C8865ull,
     0x38B7B89A9FF6B085ull},
    {"station",
     749,
     3204,
     {-102.0f, -62.75f, -102.0f},
     {102.0f, 62.75f, 102.0f},
     0x9164D80B9F285D5Cull,
     0xCBFC1809D25FCDA2ull,
     0xF708CF4F0DAB071Aull,
     0x365C17E6DFC64487ull},
    {"ship",
     48,
     48,
     {-3.5f, -1.7f, -7.0f},
     {3.5f, 3.8f, 5.0f},
     0x6FDCF786BC787F0Aull,
     0x549928798A5CCC71ull,
     0x9C40FB9BBE5B2855ull,
     0xC6F41FFC2CC5CE45ull},
    {"asteroid",
     960,
     960,
     {-0.88261f, -0.93068194f, -0.9930186f},
     {0.9194313f, 1.1016827f, 1.0695491f},
     0x1345E8CF33314B84ull,
     0xEEE801D275D83D4Cull,
     0x237C8E77FE444289ull,
     0x9C9A64472BE9E2A5ull},
    {"cockpit",
     426,
     630,
     {-1.03001f, 0.04f, -6.95f},
     {1.03001f, 1.495f, -4.55f},
     0xE47E0E3AECDB31CDull,
     0x52BD83BD5AEDB9DFull,
     0x50D536A3D4B02F8Dull,
     0xC3C7FAA905C5C338ull},
};

[[nodiscard]] const MeshTruth& truthFor(const char* name)
{
    for (const MeshTruth& entry : kTruth) {
        if (std::string_view(entry.name) == name) {
            return entry;
        }
    }
    return kTruth[0];
}

// A bare hash comparison is undiagnosable when it fails, so say what came out.
bool checkHash(const char* mesh, const char* attribute, std::uint64_t actual, std::uint64_t expected)
{
    if (actual == expected) {
        return true;
    }
    std::printf("  %s.%s: got 0x%016llX, want 0x%016llX\n",
                mesh,
                attribute,
                static_cast<unsigned long long>(actual),
                static_cast<unsigned long long>(expected));
    return false;
}

void checkAgainstShipped(const char* name, const MeshData& mesh)
{
    const MeshTruth& truth = truthFor(name);
    SOL_REQUIRE(mesh.vertices.size() == truth.vertexCount);
    SOL_REQUIRE(mesh.indices.size() == truth.indexCount);

    const assets::EditMesh edit = assets::toEditMesh(mesh);
    const assets::MeshBounds box = assets::bounds(edit);
    SOL_CHECK(std::abs(box.min.x - truth.minBound[0]) < 1e-4f);
    SOL_CHECK(std::abs(box.min.y - truth.minBound[1]) < 1e-4f);
    SOL_CHECK(std::abs(box.min.z - truth.minBound[2]) < 1e-4f);
    SOL_CHECK(std::abs(box.max.x - truth.maxBound[0]) < 1e-4f);
    SOL_CHECK(std::abs(box.max.y - truth.maxBound[1]) < 1e-4f);
    SOL_CHECK(std::abs(box.max.z - truth.maxBound[2]) < 1e-4f);

    SOL_CHECK(checkHash(name, "positions", hashPositions(mesh), truth.positionHash));
    SOL_CHECK(checkHash(name, "normals", hashNormals(mesh), truth.normalHash));
    SOL_CHECK(checkHash(name, "uvs", hashUvs(mesh), truth.uvHash));
    SOL_CHECK(checkHash(name, "indices", hashIndices(mesh), truth.indexHash));
}

// ⚑ THE NET NOW REACHES THE COMMITTED FILES, WHICH IT DID NOT BEFORE. Until the
// script's mesh half was retired, these five meshes were proved by re-authoring
// them in C++ above and hashing the result - and the `.gltf` files the game
// actually cooked were never opened by anything. That was survivable only while
// a second implementation held the truth. Now `assets/meshes/*.forge` IS the
// source the cooker reads, so a transcription that is right in this header and
// wrong in the file has to fail, and the only way to make it fail is to read the
// file. The path comes from CMake rather than a walk up from the working
// directory, because a test that guesses where the repo is finds a different
// answer under ctest than under a debugger.
[[nodiscard]] std::string readSourceFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("  cannot open %s\n", path.c_str());
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

[[nodiscard]] bool buildFromForgeSource(const char* name, MeshData& out)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
    const std::string text = readSourceFile(path);
    if (text.empty()) {
        return false;
    }
    assets::ForgeDoc doc;
    std::string error;
    if (!assets::parseForge(text.data(), text.size(), path.c_str(), doc, &error)) {
        std::printf("  %s\n", error.c_str());
        return false;
    }
    if (doc.name != name) {
        std::printf("  %s.forge names itself '%s'\n", name, doc.name.c_str());
        return false;
    }
    if (!assets::buildForge(doc, out, &error)) {
        std::printf("  %s\n", error.c_str());
        return false;
    }
    return true;
}

// A hash comparison says a transcription is wrong; this says WHICH NUMBER is
// wrong, which is the difference between a five-minute fix and an afternoon.
// The two meshes are built from the same recipe by two routes, so the first
// vertex that disagrees is the part that was mistyped.
bool reportFirstDifference(const char* name, const MeshData& fromFile, const MeshData& fromRecipe)
{
    if (fromFile.vertices.size() != fromRecipe.vertices.size()) {
        std::printf("  %s: %zu vertices from the file, %zu from the recipe\n",
                    name,
                    fromFile.vertices.size(),
                    fromRecipe.vertices.size());
        return false;
    }
    for (std::size_t i = 0; i < fromFile.vertices.size(); ++i) {
        const assets::MeshVertex& a = fromFile.vertices[i];
        const assets::MeshVertex& b = fromRecipe.vertices[i];
        for (int c = 0; c < 3; ++c) {
            if (a.position[c] != b.position[c] || a.normal[c] != b.normal[c]) {
                std::printf("  %s vertex %zu: file pos (%.9g %.9g %.9g) nrm (%.9g %.9g %.9g)\n",
                            name,
                            i,
                            a.position[0],
                            a.position[1],
                            a.position[2],
                            a.normal[0],
                            a.normal[1],
                            a.normal[2]);
                std::printf("  %s vertex %zu: want pos (%.9g %.9g %.9g) nrm (%.9g %.9g %.9g)\n",
                            name,
                            i,
                            b.position[0],
                            b.position[1],
                            b.position[2],
                            b.normal[0],
                            b.normal[1],
                            b.normal[2]);
                return false;
            }
        }
        if (a.uv[0] != b.uv[0] || a.uv[1] != b.uv[1]) {
            std::printf("  %s vertex %zu uv: file (%.9g %.9g), want (%.9g %.9g)\n",
                        name,
                        i,
                        a.uv[0],
                        a.uv[1],
                        b.uv[0],
                        b.uv[1]);
            return false;
        }
    }
    return fromFile.indices == fromRecipe.indices;
}

} // namespace

// ⚑⚑ THESE FIVE READ NO ASSET, AND AT PHASE 16 THAT BECAME THE POINT RATHER
// THAN AN ACCIDENT. They compare the C++ recipe in `shipped_meshes.hpp` against
// the hash table above - two frozen constants - so they CANNOT fail because
// somebody edited a `.forge`, and they did not when two real edits arrived.
//
// What they are worth is no longer what their name says. The transcription they
// were written to prove is finished: `gen_assets.ps1`'s mesh half and all five
// `.gltf` files were deleted at the D checkpoint, so there is no longer an
// original to have ported correctly. What survives is an EXACT-OUTPUT PIN ON
// `MeshBuilder` - the recipe drives addBox 17 times, addFlatTriangle 14,
// addBeam 10 and addTorus once, and these hashes are the only thing in the repo
// that would notice a primitive generator changing a vertex order or a uv
// derivation. `mesh_build_tests.cpp` covers the same primitives by PROPERTY
// (volume, ends, Pappus, winding, border loops); exactness is what is left here
// and it costs one frozen header to keep.
//
// So: do not delete these to "finish" retiring the transcription, and do not
// extend them to cover a new asset - a new asset belongs in the invariant tests
// further down, which read the file the game actually loads.
SOL_TEST(cubeReproducesTheShippedMesh)
{
    checkAgainstShipped("cube", shipped::buildCube());
}

SOL_TEST(stationReproducesTheShippedMesh)
{
    checkAgainstShipped("station", shipped::buildStation());
}

SOL_TEST(shipReproducesTheShippedMesh)
{
    checkAgainstShipped("ship", shipped::buildShip());
}

SOL_TEST(asteroidReproducesTheShippedMesh)
{
    checkAgainstShipped("asteroid", shipped::buildAsteroid());
}

SOL_TEST(cockpitReproducesTheShippedMesh)
{
    checkAgainstShipped("cockpit", shipped::buildCockpit());
}

// ⚑⚑ THE COMMITTED SOURCES, READ FROM DISK - AND WHAT THIS ASSERTS CHANGED AT
// PHASE 16. It used to compare each file against the C++ recipe above, vertex by
// vertex, and against the hash table. That was right while the recipe was a
// SECOND IMPLEMENTATION being checked against a PowerShell original - it is what
// let the generator's mesh half be deleted. But the original is gone, the Forge
// can now edit these files, and an exact match makes every genuine edit a test
// failure: it rejected two real human edits before anyone noticed it had stopped
// protecting anything and started forbidding the tool it was built to enable.
//
// So the net still reaches the files, and it now catches a mesh that is BROKEN
// rather than one that is merely CHANGED. What follows is the difference between
// "these numbers moved" and "this is not a solid any more".
//
// ⚑ The five recipe tests above are deliberately untouched. They read no asset
// and cannot block an edit; see the note on their own block.
constexpr const char* const kCommittedMeshes[] = {
    "cube", "station", "ship", "cockpit", "asteroid", "gate", "gate_membrane", "freighter_cockpit"};

// ⚑ `gate_membrane` is a single revolve of the profile [[0,0],[70,0]] - a flat
// disc whose centre collapses to a point. It is a FILM, so it has a border loop
// by construction and the closed-solid invariants below would fail it correctly.
// Named here rather than skipped quietly: an exclusion someone can read is a
// decision, an exclusion buried in a condition is a bug waiting to be restored.
[[nodiscard]] bool isClosedSolid(std::string_view name)
{
    return name != "gate_membrane";
}

SOL_TEST(everyCommittedForgeSourceParsesAndBuildsGeometry)
{
    for (const char* name : kCommittedMeshes) {
        MeshData mesh;
        SOL_REQUIRE(buildFromForgeSource(name, mesh));
        SOL_CHECK(!mesh.vertices.empty());
        SOL_CHECK(!mesh.indices.empty());
        SOL_CHECK(mesh.indices.size() % 3 == 0);
    }
}

// The invariant that actually protects a shipped asset. A hole, an inverted
// face or a face welded to nothing survives every hash you could write about a
// mesh that no longer has the property - and none of them show up in a
// screenshot reliably. This is what the exact match was standing in front of.
SOL_TEST(everyCommittedSolidIsAClosedManifoldWoundOutwards)
{
    for (const char* name : kCommittedMeshes) {
        if (!isClosedSolid(name)) {
            continue;
        }
        MeshData mesh;
        SOL_REQUIRE(buildFromForgeSource(name, mesh));
        const assets::EditMesh edit = assets::toEditMesh(mesh);
        const assets::MeshAdjacency adjacency = assets::buildAdjacency(edit);
        if (!adjacency.isManifold() || !adjacency.isClosed() || adjacency.borderEdgeCount() != 0) {
            std::printf("  %s: manifold %d closed %d border edges %u\n",
                        name,
                        adjacency.isManifold() ? 1 : 0,
                        adjacency.isClosed() ? 1 : 0,
                        adjacency.borderEdgeCount());
        }
        SOL_CHECK(adjacency.isManifold());
        SOL_CHECK(adjacency.isClosed());
        SOL_CHECK(adjacency.borderEdgeCount() == 0);
        // Positive means the triangles wind outwards. An asset built inside-out
        // renders as a shell you can see through from outside and is lit from
        // nowhere - and mesh.frag never flips a normal for a back face, so
        // nothing downstream would rescue it.
        if (assets::signedVolume(edit) <= 0.0) {
            // Negative means wound inside out; exactly zero means it is not a
            // solid at all - a flattened box reads 0, not -1, and saying
            // "inside out" there would send the reader the wrong way.
            std::printf("  %s: signed volume %.6g - %s\n",
                        name,
                        assets::signedVolume(edit),
                        assets::signedVolume(edit) < 0.0 ? "wound inside out" : "no enclosed volume");
        }
        SOL_CHECK(assets::signedVolume(edit) > 0.0);
        // A closed solid may not carry a zero-area triangle: it contributes no
        // pixels, no normal and no adjacency, and it is what a bad edit leaves
        // behind when two points are dragged onto each other. Asserted only for
        // solids - see the membrane's note on isClosedSolid.
        assets::EditMesh scratch = edit;
        const std::uint32_t removed = assets::removeDegenerateFaces(scratch);
        if (removed != 0) {
            std::printf("  %s: %u degenerate face(s)\n", name, removed);
        }
        SOL_CHECK(removed == 0);
    }
}

// Welding is what turns a triangle soup into a surface with edges, and every
// asset here is authored as shared corners. A file that welded to nothing would
// still draw, and every adjacency-based operation in the Forge - point editing
// included - would quietly have nothing to work on.
SOL_TEST(everyCommittedMeshWeldsItsCornersOntoFewerPoints)
{
    for (const char* name : kCommittedMeshes) {
        MeshData mesh;
        SOL_REQUIRE(buildFromForgeSource(name, mesh));
        const assets::EditMesh edit = assets::toEditMesh(mesh);
        SOL_CHECK(!edit.positions.empty());
        SOL_CHECK(edit.positions.size() < edit.vertices.size());
    }
}

// ⚑ The membrane's documented cost, pinned rather than waved past. Revolving a
// profile that touches its own axis collapses the innermost ring to a point, so
// that quad row is degenerate on one edge and rasterises as a triangle fan -
// gate_membrane.forge's own header says so. It is one per segment, and tying
// the count to `segments` is what makes this a statement about the shape rather
// than a number somebody would re-bless. The first run of the invariant tests
// above is what turned that comment into a measurement.
SOL_TEST(theMembranesOnlyDegenerateFacesAreTheFanAtItsCentre)
{
    MeshData mesh;
    SOL_REQUIRE(buildFromForgeSource("gate_membrane", mesh));
    assets::EditMesh edit = assets::toEditMesh(mesh);
    SOL_CHECK(assets::removeDegenerateFaces(edit) == 32); // gate_membrane.forge: segments = 32
}

// The bake is only trustworthy if it is a round trip, so take the same recipe
// through it here rather than trusting the committed file to have been produced
// correctly once. This is what would fail if appendMeshNumber ever wrote a
// number that reparsed to a different float - the failure mode a writer of
// baked geometry actually has.
SOL_TEST(bakingAMeshAndReadingItBackIsTheSameMesh)
{
    assets::ForgeDoc doc;
    doc.name = "asteroid";
    doc.parts.push_back(assets::forgeBakePart("hull", shipped::buildAsteroid()));

    const std::string text = assets::writeForge(doc);
    assets::ForgeDoc reparsed;
    std::string error;
    SOL_REQUIRE(assets::parseForge(text.data(), text.size(), "baked", reparsed, &error));

    MeshData rebuilt;
    SOL_REQUIRE(assets::buildForge(reparsed, rebuilt, &error));
    SOL_CHECK(reportFirstDifference("asteroid", rebuilt, shipped::buildAsteroid()));
}

// ⚑ Phase 16 repointed this at `cube.forge`. It used to weld the C++ recipe,
// which is a frozen constant the game never loads - so it proved the recipe was
// a cube and said nothing at all about the asset. The numbers are the same
// either way; the object under test is not.
SOL_TEST(weldingTheCubeFindsEightPointsUnderTwentyFourCorners)
{
    MeshData mesh;
    SOL_REQUIRE(buildFromForgeSource("cube", mesh));
    const assets::EditMesh cube = assets::toEditMesh(mesh);
    SOL_CHECK(cube.positions.size() == 8);
    SOL_CHECK(cube.vertices.size() == 24); // every corner has its own face normal
    SOL_CHECK(cube.triangleCount() == 12);

    const assets::MeshAdjacency adjacency = assets::buildAdjacency(cube);
    SOL_CHECK(adjacency.edges.size() == 18); // 12 box edges + one diagonal per face

    // ⚑ Everything above is TOPOLOGY and survives an edit; the volume is not,
    // and asserting it here was a size pin - the same ratchet in different
    // clothes, caught by driving a real resize through forge.exe rather than by
    // reading. A dragged cube is still a cube: eight points, twelve triangles,
    // eighteen edges. It is just not a UNIT cube any more, and nothing about
    // this asset's job requires that it be one. That it encloses a positive
    // volume at all is asserted for every solid above.
}
