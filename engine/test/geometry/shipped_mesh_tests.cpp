#include "shipped_meshes.hpp"

#include "sol/assets/mesh_edit.hpp"
#include "sol/core/hash.hpp"
#include "sol/test/test.hpp"

#include <cstdint>
#include <cstdio>
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
    {"cube", 24, 36, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, 0xD54F551B2AE3C265ull,
     0x6032C0607D875265ull, 0x7F80BDFB898C8865ull, 0x38B7B89A9FF6B085ull},
    {"station", 749, 3204, {-102.0f, -62.75f, -102.0f}, {102.0f, 62.75f, 102.0f}, 0x9164D80B9F285D5Cull,
     0xCBFC1809D25FCDA2ull, 0xF708CF4F0DAB071Aull, 0x365C17E6DFC64487ull},
    {"ship", 48, 48, {-3.5f, -1.7f, -7.0f}, {3.5f, 3.8f, 5.0f}, 0x6FDCF786BC787F0Aull,
     0x549928798A5CCC71ull, 0x9C40FB9BBE5B2855ull, 0xC6F41FFC2CC5CE45ull},
    {"asteroid", 960, 960, {-0.88261f, -0.93068194f, -0.9930186f},
     {0.9194313f, 1.1016827f, 1.0695491f}, 0x1345E8CF33314B84ull, 0xEEE801D275D83D4Cull,
     0x237C8E77FE444289ull, 0x9C9A64472BE9E2A5ull},
    {"cockpit", 426, 630, {-1.03001f, 0.04f, -6.95f}, {1.03001f, 1.495f, -4.55f},
     0xE47E0E3AECDB31CDull, 0x52BD83BD5AEDB9DFull, 0x50D536A3D4B02F8Dull, 0xC3C7FAA905C5C338ull},
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
    std::printf("  %s.%s: got 0x%016llX, want 0x%016llX\n", mesh, attribute,
                static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
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

} // namespace

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

// Every mesh in this game is built out of closed solids, and the weld is what
// finds that out: 24 unshared corners collapse onto 8 points, and only then
// does a box have edges to be adjacent across. A mesh that failed this would
// have a hole in it that no screenshot would necessarily show.
SOL_TEST(everyShippedMeshWeldsIntoAClosedManifoldSurface)
{
    const MeshData meshes[] = {shipped::buildCube(), shipped::buildStation(), shipped::buildShip(),
                              shipped::buildAsteroid(), shipped::buildCockpit()};
    for (const MeshData& mesh : meshes) {
        const assets::EditMesh edit = assets::toEditMesh(mesh);
        const assets::MeshAdjacency adjacency = assets::buildAdjacency(edit);
        SOL_CHECK(adjacency.isManifold());
        SOL_CHECK(adjacency.isClosed());
        SOL_CHECK(adjacency.borderEdgeCount() == 0);
    }
}

SOL_TEST(weldingTheCubeFindsEightPointsUnderTwentyFourCorners)
{
    const assets::EditMesh cube = assets::toEditMesh(shipped::buildCube());
    SOL_CHECK(cube.positions.size() == 8);
    SOL_CHECK(cube.vertices.size() == 24); // every corner has its own face normal
    SOL_CHECK(cube.triangleCount() == 12);

    const assets::MeshAdjacency adjacency = assets::buildAdjacency(cube);
    SOL_CHECK(adjacency.edges.size() == 18); // 12 box edges + one diagonal per face
    SOL_CHECK(assets::signedVolume(cube) > 0.999 && assets::signedVolume(cube) < 1.001);
}
