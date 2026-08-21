#include "sol/assets/mesh_edit.hpp"

#include "sol/core/math/vec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace sol::assets {
namespace {

using core::Vec2;
using core::Vec3;

constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

[[nodiscard]] bool withinTolerance(Vec3 a, Vec3 b, float tolerance)
{
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.z - b.z) <= tolerance;
}

[[nodiscard]] bool withinTolerance(Vec2 a, Vec2 b, float tolerance)
{
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}

[[nodiscard]] Vec3 toVec3(const float (&v)[3])
{
    return {v[0], v[1], v[2]};
}

// Spatial hash sized to the weld tolerance, so a point within tolerance is
// never more than one cell away on any axis and 27 cells is an exhaustive
// search. Insertion order decides which of several candidates wins, which is
// what makes welding deterministic.
class PositionWelder
{
public:
    explicit PositionWelder(float tolerance)
        : m_tolerance(tolerance)
        , m_cell(tolerance > 0.0f ? tolerance : 1e-6f)
    {
    }

    std::uint32_t add(Vec3 point, std::vector<Vec3>& points)
    {
        const std::int64_t cx = cellOf(point.x);
        const std::int64_t cy = cellOf(point.y);
        const std::int64_t cz = cellOf(point.z);
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto found = m_cells.find(keyOf(cx + dx, cy + dy, cz + dz));
                    if (found == m_cells.end()) {
                        continue;
                    }
                    for (const std::uint32_t index : found->second) {
                        if (withinTolerance(points[index], point, m_tolerance)) {
                            return index;
                        }
                    }
                }
            }
        }
        const auto index = static_cast<std::uint32_t>(points.size());
        points.push_back(point);
        m_cells[keyOf(cx, cy, cz)].push_back(index);
        return index;
    }

private:
    [[nodiscard]] std::int64_t cellOf(float value) const
    {
        return static_cast<std::int64_t>(std::floor(static_cast<double>(value) / m_cell));
    }

    [[nodiscard]] static std::uint64_t keyOf(std::int64_t x, std::int64_t y, std::int64_t z)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const std::int64_t component : {x, y, z}) {
            auto bits = static_cast<std::uint64_t>(component);
            for (int i = 0; i < 8; ++i) {
                hash ^= bits & 0xFFull;
                hash *= 1099511628211ull;
                bits >>= 8;
            }
        }
        return hash;
    }

    float m_tolerance;
    double m_cell;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_cells;
};

// Merges corners that stand at the same point and agree on normal and uv.
// Positions are left alone - this is the second half of a weld, and the half
// that recomputeNormals needs on its own.
void dedupeCorners(EditMesh& mesh, const WeldOptions& options)
{
    std::vector<std::vector<std::uint32_t>> cornersAt(mesh.positions.size());
    std::vector<EditVertex> merged;
    merged.reserve(mesh.vertices.size());
    std::vector<std::uint32_t> remap(mesh.vertices.size(), kNoIndex);

    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const EditVertex& vertex = mesh.vertices[i];
        std::uint32_t match = kNoIndex;
        for (const std::uint32_t candidate : cornersAt[vertex.position]) {
            if (withinTolerance(merged[candidate].normal, vertex.normal, options.normalTolerance) &&
                withinTolerance(merged[candidate].uv, vertex.uv, options.uvTolerance)) {
                match = candidate;
                break;
            }
        }
        if (match == kNoIndex) {
            match = static_cast<std::uint32_t>(merged.size());
            merged.push_back(vertex);
            cornersAt[vertex.position].push_back(match);
        }
        remap[i] = match;
    }

    for (std::uint32_t& index : mesh.indices) {
        index = remap[index];
    }
    mesh.vertices = std::move(merged);
}

[[nodiscard]] Vec3 faceNormalOf(const EditMesh& mesh, std::uint32_t face)
{
    const Vec3 p0 = mesh.positions[mesh.facePosition(face, 0)];
    const Vec3 p1 = mesh.positions[mesh.facePosition(face, 1)];
    const Vec3 p2 = mesh.positions[mesh.facePosition(face, 2)];
    const Vec3 normal = core::cross(p1 - p0, p2 - p0);
    const float length = core::length(normal);
    return length > 0.0f ? normal / length : Vec3{};
}

[[nodiscard]] float faceAreaOf(const EditMesh& mesh, std::uint32_t face)
{
    const Vec3 p0 = mesh.positions[mesh.facePosition(face, 0)];
    const Vec3 p1 = mesh.positions[mesh.facePosition(face, 1)];
    const Vec3 p2 = mesh.positions[mesh.facePosition(face, 2)];
    return 0.5f * core::length(core::cross(p1 - p0, p2 - p0));
}

// --- quadric error metric ---------------------------------------------------

// Symmetric 4x4 stored as its upper triangle: the error of a point against a
// set of planes, which is what a collapse is scored on.
struct Quadric
{
    double m[10] = {};

    void addPlane(double a, double b, double c, double d, double weight)
    {
        m[0] += weight * a * a;
        m[1] += weight * a * b;
        m[2] += weight * a * c;
        m[3] += weight * a * d;
        m[4] += weight * b * b;
        m[5] += weight * b * c;
        m[6] += weight * b * d;
        m[7] += weight * c * c;
        m[8] += weight * c * d;
        m[9] += weight * d * d;
    }

    void add(const Quadric& other)
    {
        for (int i = 0; i < 10; ++i) {
            m[i] += other.m[i];
        }
    }

    [[nodiscard]] double evaluate(Vec3 point) const
    {
        const double x = point.x;
        const double y = point.y;
        const double z = point.z;
        return (m[0] * x * x) + (2 * m[1] * x * y) + (2 * m[2] * x * z) + (2 * m[3] * x) +
               (m[4] * y * y) + (2 * m[5] * y * z) + (2 * m[6] * y) + (m[7] * z * z) + (2 * m[8] * z) +
               m[9];
    }
};

// Solves for the point of least error: the quadric's own minimum, where the
// planes around the collapsing edge actually meet. Fails on a singular system,
// which is exactly the flat and the straight-crease cases.
[[nodiscard]] bool solveQuadric(const Quadric& q, Vec3& out)
{
    const double a[3][3] = {{q.m[0], q.m[1], q.m[2]}, {q.m[1], q.m[4], q.m[5]}, {q.m[2], q.m[5], q.m[7]}};
    const double rhs[3] = {-q.m[3], -q.m[6], -q.m[8]};
    const auto determinant = [](const double m[3][3]) {
        return (m[0][0] * ((m[1][1] * m[2][2]) - (m[1][2] * m[2][1]))) -
               (m[0][1] * ((m[1][0] * m[2][2]) - (m[1][2] * m[2][0]))) +
               (m[0][2] * ((m[1][0] * m[2][1]) - (m[1][1] * m[2][0])));
    };
    const double det = determinant(a);
    const double scale = std::abs(q.m[0]) + std::abs(q.m[4]) + std::abs(q.m[7]);
    if (scale <= 0.0 || std::abs(det) < 1e-9 * scale * scale * scale) {
        return false;
    }
    double solved[3];
    for (int column = 0; column < 3; ++column) {
        double m[3][3];
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 3; ++c) {
                m[row][c] = c == column ? rhs[row] : a[row][c];
            }
        }
        solved[column] = determinant(m) / det;
    }
    out = {static_cast<float>(solved[0]), static_cast<float>(solved[1]),
           static_cast<float>(solved[2])};
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

struct Placement
{
    Vec3 point{};
    double cost = 0.0;
};

struct CollapseCandidate
{
    double cost = 0.0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t versionA = 0;
    std::uint32_t versionB = 0;

    // Ties broken on the endpoints so the queue is a total order and a
    // decimation is reproducible run to run.
    [[nodiscard]] bool operator>(const CollapseCandidate& rhs) const
    {
        if (cost != rhs.cost) {
            return cost > rhs.cost;
        }
        if (a != rhs.a) {
            return a > rhs.a;
        }
        return b > rhs.b;
    }
};

} // namespace

// --- adjacency --------------------------------------------------------------

bool MeshAdjacency::isManifold() const
{
    return std::all_of(edges.begin(), edges.end(),
                       [](const Edge& edge) { return edge.faceCount <= 2; });
}

bool MeshAdjacency::isClosed() const
{
    return std::all_of(edges.begin(), edges.end(),
                       [](const Edge& edge) { return edge.faceCount == 2; });
}

std::uint32_t MeshAdjacency::borderEdgeCount() const
{
    return static_cast<std::uint32_t>(
        std::count_if(edges.begin(), edges.end(), [](const Edge& edge) { return edge.faceCount == 1; }));
}

const MeshAdjacency::Edge* MeshAdjacency::findEdge(std::uint32_t a, std::uint32_t b) const
{
    if (a > b) {
        std::swap(a, b);
    }
    using Key = std::pair<std::uint32_t, std::uint32_t>;
    const auto found = std::lower_bound(edges.begin(), edges.end(), Key{a, b},
                                        [](const Edge& edge, const Key& key) {
                                            return Key{edge.a, edge.b} < key;
                                        });
    if (found == edges.end() || found->a != a || found->b != b) {
        return nullptr;
    }
    return &*found;
}

MeshAdjacency buildAdjacency(const EditMesh& mesh)
{
    MeshAdjacency adjacency;
    const std::uint32_t faceCount = mesh.triangleCount();

    struct EdgeRef
    {
        std::uint32_t a = 0;
        std::uint32_t b = 0;
        std::uint32_t face = 0;
    };
    std::vector<EdgeRef> refs;
    refs.reserve(static_cast<std::size_t>(faceCount) * 3);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            std::uint32_t a = mesh.facePosition(face, corner);
            std::uint32_t b = mesh.facePosition(face, (corner + 1) % 3);
            if (a == b) {
                continue; // a degenerate edge is not an adjacency
            }
            if (a > b) {
                std::swap(a, b);
            }
            refs.push_back({a, b, face});
        }
    }
    std::sort(refs.begin(), refs.end(), [](const EdgeRef& lhs, const EdgeRef& rhs) {
        return std::tuple{lhs.a, lhs.b, lhs.face} < std::tuple{rhs.a, rhs.b, rhs.face};
    });

    adjacency.edgeFaces.reserve(refs.size());
    for (std::size_t i = 0; i < refs.size();) {
        std::size_t j = i;
        MeshAdjacency::Edge edge;
        edge.a = refs[i].a;
        edge.b = refs[i].b;
        edge.firstFace = static_cast<std::uint32_t>(adjacency.edgeFaces.size());
        while (j < refs.size() && refs[j].a == edge.a && refs[j].b == edge.b) {
            adjacency.edgeFaces.push_back(refs[j].face);
            ++j;
        }
        edge.faceCount = static_cast<std::uint32_t>(j - i);
        adjacency.edges.push_back(edge);
        i = j;
    }

    adjacency.positionFaceStart.assign(mesh.positions.size() + 1, 0);
    const auto uniqueCorners = [&](std::uint32_t face, std::uint32_t out[3]) {
        std::uint32_t count = 0;
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            const std::uint32_t position = mesh.facePosition(face, corner);
            bool seen = false;
            for (std::uint32_t k = 0; k < count; ++k) {
                seen = seen || out[k] == position;
            }
            if (!seen) {
                out[count++] = position;
            }
        }
        return count;
    };

    std::uint32_t corners[3];
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        const std::uint32_t count = uniqueCorners(face, corners);
        for (std::uint32_t k = 0; k < count; ++k) {
            ++adjacency.positionFaceStart[corners[k] + 1];
        }
    }
    for (std::size_t i = 1; i < adjacency.positionFaceStart.size(); ++i) {
        adjacency.positionFaceStart[i] += adjacency.positionFaceStart[i - 1];
    }
    adjacency.positionFaces.assign(adjacency.positionFaceStart.back(), 0);
    std::vector<std::uint32_t> cursor(adjacency.positionFaceStart.begin(),
                                      adjacency.positionFaceStart.end() - 1);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        const std::uint32_t count = uniqueCorners(face, corners);
        for (std::uint32_t k = 0; k < count; ++k) {
            adjacency.positionFaces[cursor[corners[k]]++] = face;
        }
    }
    return adjacency;
}

// --- conversion and welding -------------------------------------------------

EditMesh toEditMesh(const MeshData& data, const WeldOptions& options)
{
    EditMesh mesh;
    mesh.indices = data.indices;

    PositionWelder welder(options.positionTolerance);
    mesh.vertices.reserve(data.vertices.size());
    for (const MeshVertex& source : data.vertices) {
        EditVertex vertex;
        vertex.position = welder.add(toVec3(source.position), mesh.positions);
        vertex.normal = toVec3(source.normal);
        vertex.uv = {source.uv[0], source.uv[1]};
        mesh.vertices.push_back(vertex);
    }

    dedupeCorners(mesh, options);
    removeUnused(mesh);
    return mesh;
}

MeshData toMeshData(const EditMesh& mesh)
{
    MeshData data;
    data.vertices.reserve(mesh.vertices.size());
    for (const EditVertex& vertex : mesh.vertices) {
        const Vec3 point = mesh.positions[vertex.position];
        MeshVertex out{};
        out.position[0] = point.x;
        out.position[1] = point.y;
        out.position[2] = point.z;
        out.normal[0] = vertex.normal.x;
        out.normal[1] = vertex.normal.y;
        out.normal[2] = vertex.normal.z;
        out.uv[0] = vertex.uv.x;
        out.uv[1] = vertex.uv.y;
        data.vertices.push_back(out);
    }
    data.indices = mesh.indices;
    return data;
}

void weld(EditMesh& mesh, const WeldOptions& options)
{
    // Re-welding through the buffer form is not a shortcut: a position merge
    // renumbers the graph, and rebuilding is what keeps corners, positions and
    // indices consistent with each other by construction.
    mesh = toEditMesh(toMeshData(mesh), options);
}

void append(EditMesh& mesh, const EditMesh& addition)
{
    const auto positionOffset = static_cast<std::uint32_t>(mesh.positions.size());
    const auto vertexOffset = static_cast<std::uint32_t>(mesh.vertices.size());

    mesh.positions.insert(mesh.positions.end(), addition.positions.begin(), addition.positions.end());
    mesh.vertices.reserve(mesh.vertices.size() + addition.vertices.size());
    for (EditVertex vertex : addition.vertices) {
        vertex.position += positionOffset;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.reserve(mesh.indices.size() + addition.indices.size());
    for (const std::uint32_t index : addition.indices) {
        mesh.indices.push_back(index + vertexOffset);
    }
}

std::uint32_t removeUnused(EditMesh& mesh)
{
    std::vector<bool> vertexUsed(mesh.vertices.size(), false);
    for (const std::uint32_t index : mesh.indices) {
        vertexUsed[index] = true;
    }
    std::vector<bool> positionUsed(mesh.positions.size(), false);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (vertexUsed[i]) {
            positionUsed[mesh.vertices[i].position] = true;
        }
    }

    std::vector<std::uint32_t> positionRemap(mesh.positions.size(), kNoIndex);
    std::vector<Vec3> positions;
    positions.reserve(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        if (positionUsed[i]) {
            positionRemap[i] = static_cast<std::uint32_t>(positions.size());
            positions.push_back(mesh.positions[i]);
        }
    }

    std::vector<std::uint32_t> vertexRemap(mesh.vertices.size(), kNoIndex);
    std::vector<EditVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (!vertexUsed[i]) {
            continue;
        }
        vertexRemap[i] = static_cast<std::uint32_t>(vertices.size());
        EditVertex vertex = mesh.vertices[i];
        vertex.position = positionRemap[vertex.position];
        vertices.push_back(vertex);
    }

    const auto removed = static_cast<std::uint32_t>(mesh.vertices.size() - vertices.size());
    for (std::uint32_t& index : mesh.indices) {
        index = vertexRemap[index];
    }
    mesh.positions = std::move(positions);
    mesh.vertices = std::move(vertices);
    return removed;
}

std::uint32_t removeDegenerateFaces(EditMesh& mesh, float areaEpsilon)
{
    const std::uint32_t faceCount = mesh.triangleCount();
    std::vector<std::uint32_t> kept;
    kept.reserve(mesh.indices.size());
    std::uint32_t removed = 0;
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        const std::uint32_t p0 = mesh.facePosition(face, 0);
        const std::uint32_t p1 = mesh.facePosition(face, 1);
        const std::uint32_t p2 = mesh.facePosition(face, 2);
        const bool repeated = p0 == p1 || p1 == p2 || p2 == p0;
        if (repeated || faceAreaOf(mesh, face) <= areaEpsilon) {
            ++removed;
            continue;
        }
        kept.push_back(mesh.indices[(face * 3) + 0]);
        kept.push_back(mesh.indices[(face * 3) + 1]);
        kept.push_back(mesh.indices[(face * 3) + 2]);
    }
    if (removed == 0) {
        return 0;
    }
    mesh.indices = std::move(kept);
    removeUnused(mesh);
    return removed;
}

// --- normals ----------------------------------------------------------------

void recomputeNormals(EditMesh& mesh, float smoothAngleDegrees)
{
    const std::uint32_t faceCount = mesh.triangleCount();
    std::vector<Vec3> faceNormals(faceCount);
    std::vector<float> faceAreas(faceCount);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        faceNormals[face] = faceNormalOf(mesh, face);
        faceAreas[face] = faceAreaOf(mesh, face);
    }

    std::vector<Vec3> cornerNormals(mesh.indices.size());
    if (smoothAngleDegrees <= 0.0f) {
        for (std::uint32_t face = 0; face < faceCount; ++face) {
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                cornerNormals[(face * 3) + corner] = faceNormals[face];
            }
        }
    } else {
        // Faces sharing an edge and meeting shallower than the threshold join
        // one smoothing group; the union-find runs PER POSITION, because two
        // faces may smooth together at one end of a shared edge and crease at
        // the other.
        const MeshAdjacency adjacency = buildAdjacency(mesh);
        const auto slotOf = [&](std::uint32_t position, std::uint32_t face) {
            const std::uint32_t start = adjacency.positionFaceStart[position];
            const std::uint32_t end = adjacency.positionFaceStart[position + 1];
            for (std::uint32_t slot = start; slot < end; ++slot) {
                if (adjacency.positionFaces[slot] == face) {
                    return slot;
                }
            }
            return kNoIndex;
        };

        std::vector<std::uint32_t> parent(adjacency.positionFaces.size());
        for (std::size_t i = 0; i < parent.size(); ++i) {
            parent[i] = static_cast<std::uint32_t>(i);
        }
        const auto findRoot = [&parent](std::uint32_t slot) {
            while (parent[slot] != slot) {
                parent[slot] = parent[parent[slot]];
                slot = parent[slot];
            }
            return slot;
        };
        const auto unite = [&](std::uint32_t lhs, std::uint32_t rhs) {
            const std::uint32_t rootL = findRoot(lhs);
            const std::uint32_t rootR = findRoot(rhs);
            if (rootL != rootR) {
                parent[std::max(rootL, rootR)] = std::min(rootL, rootR);
            }
        };

        const float cosThreshold = std::cos(smoothAngleDegrees * 3.14159265358979323846f / 180.0f);
        for (const MeshAdjacency::Edge& edge : adjacency.edges) {
            if (edge.faceCount != 2) {
                continue;
            }
            const std::uint32_t f0 = adjacency.edgeFaces[edge.firstFace];
            const std::uint32_t f1 = adjacency.edgeFaces[edge.firstFace + 1];
            if (core::dot(faceNormals[f0], faceNormals[f1]) < cosThreshold) {
                continue;
            }
            for (const std::uint32_t position : {edge.a, edge.b}) {
                const std::uint32_t slot0 = slotOf(position, f0);
                const std::uint32_t slot1 = slotOf(position, f1);
                if (slot0 != kNoIndex && slot1 != kNoIndex) {
                    unite(slot0, slot1);
                }
            }
        }

        std::vector<Vec3> groupNormals(parent.size(), Vec3{});
        const auto positionCount = static_cast<std::uint32_t>(mesh.positions.size());
        for (std::uint32_t position = 0; position < positionCount; ++position) {
            const std::uint32_t start = adjacency.positionFaceStart[position];
            const std::uint32_t end = adjacency.positionFaceStart[position + 1];
            for (std::uint32_t slot = start; slot < end; ++slot) {
                const std::uint32_t face = adjacency.positionFaces[slot];
                groupNormals[findRoot(slot)] += faceNormals[face] * faceAreas[face];
            }
        }
        for (Vec3& normal : groupNormals) {
            const float length = core::length(normal);
            if (length > 0.0f) {
                normal = normal / length;
            }
        }

        for (std::uint32_t face = 0; face < faceCount; ++face) {
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                const std::uint32_t position = mesh.facePosition(face, corner);
                const std::uint32_t slot = slotOf(position, face);
                cornerNormals[(face * 3) + corner] =
                    slot == kNoIndex ? faceNormals[face] : groupNormals[findRoot(slot)];
            }
        }
    }

    std::vector<EditVertex> vertices;
    vertices.reserve(mesh.indices.size());
    for (std::size_t corner = 0; corner < mesh.indices.size(); ++corner) {
        EditVertex vertex = mesh.vertices[mesh.indices[corner]];
        vertex.normal = cornerNormals[corner];
        vertices.push_back(vertex);
    }
    mesh.vertices = std::move(vertices);
    mesh.indices.resize(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        mesh.indices[i] = static_cast<std::uint32_t>(i);
    }
    dedupeCorners(mesh, {});
    removeUnused(mesh);
}

// --- index order ------------------------------------------------------------

namespace {

constexpr std::uint32_t kOptimizerCacheSize = 32;
constexpr float kCacheDecayPower = 1.5f;
constexpr float kLastTriangleScore = 0.75f;
constexpr float kValenceBoostScale = 2.0f;
constexpr float kValenceBoostPower = 0.5f;

[[nodiscard]] float vertexScore(int cachePosition, std::uint32_t remainingFaces)
{
    if (remainingFaces == 0) {
        return -1.0f;
    }
    float score = 0.0f;
    if (cachePosition >= 0) {
        if (cachePosition < 3) {
            // The last triangle's own vertices score flat, or the run would
            // just walk back over what it has already drawn.
            score = kLastTriangleScore;
        } else {
            const float scale = 1.0f / static_cast<float>(kOptimizerCacheSize - 3);
            score = std::pow(1.0f - (static_cast<float>(cachePosition - 3) * scale), kCacheDecayPower);
        }
    }
    return score + (kValenceBoostScale * std::pow(static_cast<float>(remainingFaces), -kValenceBoostPower));
}

} // namespace

void optimizeIndices(EditMesh& mesh)
{
    const std::uint32_t faceCount = mesh.triangleCount();
    const auto vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    if (faceCount == 0 || vertexCount == 0) {
        return;
    }

    // ⚑ Measured over the committed assets at stage F: this reorder makes the
    // cache WORSE on the two best-shared meshes in the repo - gate 0.804 ->
    // 0.895 and station 0.701 -> 0.810 - because `MeshBuilder` emits boxes and
    // beams in an order already near the ~0.6 floor, and a greedy heuristic
    // cannot beat an order that is already good. This function promises an
    // improvement, so it keeps the BETTER of the two orders rather than the
    // newer one: still worth running on imported soup, a no-op on everything
    // authored here. It survived because its only test runs on an icosphere.
    const float scoreBefore = averageCacheMissRatio(mesh);
    const std::vector<std::uint32_t> indicesBefore = mesh.indices;
    const std::vector<EditVertex> verticesBefore = mesh.vertices;

    // Faces per vertex, CSR.
    std::vector<std::uint32_t> faceStart(vertexCount + 1, 0);
    for (const std::uint32_t index : mesh.indices) {
        ++faceStart[index + 1];
    }
    for (std::uint32_t i = 1; i <= vertexCount; ++i) {
        faceStart[i] += faceStart[i - 1];
    }
    std::vector<std::uint32_t> vertexFaces(mesh.indices.size(), 0);
    {
        std::vector<std::uint32_t> cursor(faceStart.begin(), faceStart.end() - 1);
        for (std::uint32_t face = 0; face < faceCount; ++face) {
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                vertexFaces[cursor[mesh.indices[(face * 3) + corner]]++] = face;
            }
        }
    }

    std::vector<std::uint32_t> remainingFaces(vertexCount, 0);
    for (std::uint32_t v = 0; v < vertexCount; ++v) {
        remainingFaces[v] = faceStart[v + 1] - faceStart[v];
    }
    std::vector<int> cachePosition(vertexCount, -1);
    std::vector<float> scores(vertexCount, 0.0f);
    for (std::uint32_t v = 0; v < vertexCount; ++v) {
        scores[v] = vertexScore(-1, remainingFaces[v]);
    }

    std::vector<float> faceScores(faceCount, 0.0f);
    std::vector<bool> faceDrawn(faceCount, false);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        faceScores[face] = scores[mesh.indices[(face * 3) + 0]] + scores[mesh.indices[(face * 3) + 1]] +
                           scores[mesh.indices[(face * 3) + 2]];
    }

    // One slot past the cache so a vertex can fall off the end before it is
    // scored out.
    std::vector<std::uint32_t> cache;
    cache.reserve(kOptimizerCacheSize + 3);

    std::vector<std::uint32_t> order;
    order.reserve(mesh.indices.size());

    std::uint32_t best = 0;
    float bestScore = -1.0f;
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        if (faceScores[face] > bestScore) {
            bestScore = faceScores[face];
            best = face;
        }
    }

    std::uint32_t scanCursor = 0;
    for (std::uint32_t drawn = 0; drawn < faceCount; ++drawn) {
        const std::uint32_t face = best;
        faceDrawn[face] = true;
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            order.push_back(mesh.indices[(face * 3) + corner]);
        }

        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            const std::uint32_t vertex = mesh.indices[(face * 3) + corner];
            --remainingFaces[vertex];
            const auto found = std::find(cache.begin(), cache.end(), vertex);
            if (found != cache.end()) {
                cache.erase(found);
            }
            cache.insert(cache.begin(), vertex);
        }
        while (cache.size() > kOptimizerCacheSize + 3) {
            cache.pop_back();
        }

        for (std::size_t i = 0; i < cache.size(); ++i) {
            const std::uint32_t vertex = cache[i];
            cachePosition[vertex] = i < kOptimizerCacheSize ? static_cast<int>(i) : -1;
            scores[vertex] = vertexScore(cachePosition[vertex], remainingFaces[vertex]);
        }

        bestScore = -1.0f;
        for (const std::uint32_t vertex : cache) {
            for (std::uint32_t slot = faceStart[vertex]; slot < faceStart[vertex + 1]; ++slot) {
                const std::uint32_t candidate = vertexFaces[slot];
                if (faceDrawn[candidate]) {
                    continue;
                }
                faceScores[candidate] = scores[mesh.indices[(candidate * 3) + 0]] +
                                        scores[mesh.indices[(candidate * 3) + 1]] +
                                        scores[mesh.indices[(candidate * 3) + 2]];
                if (faceScores[candidate] > bestScore) {
                    bestScore = faceScores[candidate];
                    best = candidate;
                }
            }
        }
        if (bestScore < 0.0f) {
            // Dead end: the cache has nothing left to offer, so take the next
            // undrawn face in order rather than rescoring the whole mesh.
            while (scanCursor < faceCount && faceDrawn[scanCursor]) {
                ++scanCursor;
            }
            if (scanCursor < faceCount) {
                best = scanCursor;
            }
        }
    }

    mesh.indices = std::move(order);

    // Fetch order: renumber the vertices in the order the new index run first
    // touches them, so the vertex buffer is read forwards.
    std::vector<std::uint32_t> remap(vertexCount, kNoIndex);
    std::vector<EditVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (std::uint32_t& index : mesh.indices) {
        if (remap[index] == kNoIndex) {
            remap[index] = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back(mesh.vertices[index]);
        }
        index = remap[index];
    }
    mesh.vertices = std::move(vertices);

    if (averageCacheMissRatio(mesh) > scoreBefore) {
        mesh.indices = indicesBefore;
        mesh.vertices = verticesBefore;
    }
}

float averageCacheMissRatio(const EditMesh& mesh, std::uint32_t cacheSize)
{
    const std::uint32_t faceCount = mesh.triangleCount();
    if (faceCount == 0 || cacheSize == 0) {
        return 0.0f;
    }
    std::vector<std::uint32_t> cache;
    cache.reserve(cacheSize);
    std::uint32_t misses = 0;
    for (const std::uint32_t index : mesh.indices) {
        if (std::find(cache.begin(), cache.end(), index) != cache.end()) {
            continue;
        }
        ++misses;
        cache.push_back(index);
        if (cache.size() > cacheSize) {
            cache.erase(cache.begin());
        }
    }
    return static_cast<float>(misses) / static_cast<float>(faceCount);
}

// --- decimation -------------------------------------------------------------

std::uint32_t decimate(EditMesh& mesh, const DecimateOptions& options)
{
    const auto positionCount = static_cast<std::uint32_t>(mesh.positions.size());
    std::uint32_t faceCount = mesh.triangleCount();
    if (faceCount == 0 || options.targetTriangles >= faceCount) {
        return faceCount;
    }

    // A position-level working copy: topology is the only thing a collapse
    // reasons about, and the render corners are remapped onto the result.
    std::vector<std::array<std::uint32_t, 3>> faces(faceCount);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        faces[face] = {mesh.facePosition(face, 0), mesh.facePosition(face, 1), mesh.facePosition(face, 2)};
    }
    std::vector<bool> faceAlive(faceCount, true);
    std::vector<bool> positionAlive(positionCount, true);
    std::vector<Vec3> points = mesh.positions;
    std::vector<std::vector<std::uint32_t>> incident(positionCount);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        for (const std::uint32_t position : faces[face]) {
            if (std::find(incident[position].begin(), incident[position].end(), face) ==
                incident[position].end()) {
                incident[position].push_back(face);
            }
        }
    }

    const auto planeOf = [&](std::uint32_t face, double& a, double& b, double& c, double& d) {
        const Vec3 p0 = points[faces[face][0]];
        const Vec3 p1 = points[faces[face][1]];
        const Vec3 p2 = points[faces[face][2]];
        const Vec3 normal = core::cross(p1 - p0, p2 - p0);
        const double length = core::length(normal);
        if (length <= 0.0) {
            a = b = c = d = 0.0;
            return 0.0;
        }
        a = normal.x / length;
        b = normal.y / length;
        c = normal.z / length;
        d = -((a * p0.x) + (b * p0.y) + (c * p0.z));
        return length * 0.5; // area, the plane's weight
    };

    std::vector<Quadric> quadrics(positionCount);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        double a = 0;
        double b = 0;
        double c = 0;
        double d = 0;
        const double area = planeOf(face, a, b, c, d);
        for (const std::uint32_t position : faces[face]) {
            quadrics[position].addPlane(a, b, c, d, area);
        }
    }

    // A border position is one touching an edge with a single face.
    const auto rebuildBorders = [&]() {
        std::vector<bool> border(positionCount, false);
        std::unordered_map<std::uint64_t, std::uint32_t> edgeUse;
        for (std::uint32_t face = 0; face < faceCount; ++face) {
            if (!faceAlive[face]) {
                continue;
            }
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                std::uint32_t a = faces[face][corner];
                std::uint32_t b = faces[face][(corner + 1) % 3];
                if (a == b) {
                    continue;
                }
                if (a > b) {
                    std::swap(a, b);
                }
                ++edgeUse[(static_cast<std::uint64_t>(a) << 32) | b];
            }
        }
        for (const auto& [key, uses] : edgeUse) {
            if (uses == 1) {
                border[static_cast<std::uint32_t>(key >> 32)] = true;
                border[static_cast<std::uint32_t>(key & 0xFFFFFFFFu)] = true;
            }
        }
        return border;
    };
    const std::vector<bool> isBorder = rebuildBorders();

    const auto neighboursOf = [&](std::uint32_t position, std::vector<std::uint32_t>& out) {
        out.clear();
        for (const std::uint32_t face : incident[position]) {
            if (!faceAlive[face]) {
                continue;
            }
            for (const std::uint32_t other : faces[face]) {
                if (other != position && std::find(out.begin(), out.end(), other) == out.end()) {
                    out.push_back(other);
                }
            }
        }
    };

    const auto sharedFacesOf = [&](std::uint32_t a, std::uint32_t b, std::vector<std::uint32_t>& out) {
        out.clear();
        for (const std::uint32_t face : incident[a]) {
            if (!faceAlive[face]) {
                continue;
            }
            const auto& corners = faces[face];
            if (corners[0] == b || corners[1] == b || corners[2] == b) {
                out.push_back(face);
            }
        }
    };

    // The link condition: a collapse is manifold-safe exactly when the only
    // vertices adjacent to both ends are the ones opposite the faces the edge
    // already shares. Anything else pinches the surface.
    std::vector<std::uint32_t> neighboursA;
    std::vector<std::uint32_t> neighboursB;
    std::vector<std::uint32_t> shared;
    std::vector<std::uint32_t> linkShared;
    std::vector<std::uint32_t> opposite;
    const auto linkConditionHolds = [&](std::uint32_t a, std::uint32_t b) {
        neighboursOf(a, neighboursA);
        neighboursOf(b, neighboursB);
        sharedFacesOf(a, b, linkShared);
        opposite.clear();
        for (const std::uint32_t face : linkShared) {
            for (const std::uint32_t position : faces[face]) {
                if (position != a && position != b) {
                    opposite.push_back(position);
                }
            }
        }
        std::size_t common = 0;
        for (const std::uint32_t position : neighboursA) {
            if (std::find(neighboursB.begin(), neighboursB.end(), position) == neighboursB.end()) {
                continue;
            }
            ++common;
            if (std::find(opposite.begin(), opposite.end(), position) == opposite.end()) {
                return false;
            }
        }
        return common == opposite.size();
    };

    const auto collapseFlipsAFace = [&](std::uint32_t a, std::uint32_t b, Vec3 target) {
        for (const std::uint32_t position : {a, b}) {
            for (const std::uint32_t face : incident[position]) {
                if (!faceAlive[face]) {
                    continue;
                }
                const auto& corners = faces[face];
                const bool touchesBoth = (corners[0] == a || corners[1] == a || corners[2] == a) &&
                                         (corners[0] == b || corners[1] == b || corners[2] == b);
                if (touchesBoth) {
                    continue; // this face disappears in the collapse
                }
                Vec3 moved[3];
                for (int k = 0; k < 3; ++k) {
                    moved[k] = (corners[k] == a || corners[k] == b) ? target : points[corners[k]];
                }
                const Vec3 before = core::cross(points[corners[1]] - points[corners[0]],
                                                points[corners[2]] - points[corners[0]]);
                const Vec3 after = core::cross(moved[1] - moved[0], moved[2] - moved[0]);
                if (core::length(after) <= 0.0f || core::dot(before, after) <= 0.0f) {
                    return true;
                }
            }
        }
        return false;
    };

    // Either end, or the midpoint. Always available, and always ON the chord.
    const auto subsetPlacement = [&](const Quadric& combined, std::uint32_t a, std::uint32_t b) {
        const Vec3 candidates[3] = {points[a], points[b], (points[a] + points[b]) * 0.5f};
        Placement best{candidates[0], combined.evaluate(candidates[0])};
        for (int i = 1; i < 3; ++i) {
            const double cost = combined.evaluate(candidates[i]);
            if (cost < best.cost) {
                best = {candidates[i], cost};
            }
        }
        best.cost = std::max(best.cost, 0.0);
        return best;
    };

    // ⚑ The solve is not a refinement, it is the difference between an LOD and
    // a shrunken one. A subset placement can only ever put the survivor on the
    // chord, and on a convex hull every chord is inside the surface - so a
    // sphere decimates by CONTRACTING, measured at 0.63 of its volume by 80
    // triangles against 0.87 with the solve. The quadric's own minimum is
    // where the surrounding planes meet, off the chord and outside it.
    const auto solvedPlacement = [&](const Quadric& combined, std::uint32_t a, std::uint32_t b,
                                     Placement& out) {
        Vec3 solved;
        if (!solveQuadric(combined, solved)) {
            return false;
        }
        // A system that is merely ill-conditioned rather than singular solves
        // to a point across the model; keep the answer only while it is local
        // to the edge it replaces.
        const Vec3 midpoint = (points[a] + points[b]) * 0.5f;
        if (core::length(solved - midpoint) > 0.5f * core::length(points[b] - points[a])) {
            return false;
        }
        out = {solved, std::max(combined.evaluate(solved), 0.0)};
        return true;
    };

    const auto edgeCost = [&](std::uint32_t a, std::uint32_t b) {
        Quadric combined = quadrics[a];
        combined.add(quadrics[b]);
        const Placement subset = subsetPlacement(combined, a, b);
        Placement solved;
        return solvedPlacement(combined, a, b, solved) ? std::min(solved.cost, subset.cost) : subset.cost;
    };

    std::vector<std::uint32_t> version(positionCount, 0);
    std::priority_queue<CollapseCandidate, std::vector<CollapseCandidate>, std::greater<>> queue;
    const auto pushEdge = [&](std::uint32_t a, std::uint32_t b) {
        if (a == b) {
            return;
        }
        if (a > b) {
            std::swap(a, b);
        }
        if (options.preserveBorders && (isBorder[a] || isBorder[b])) {
            return;
        }
        queue.push({edgeCost(a, b), a, b, version[a], version[b]});
    };

    for (std::uint32_t face = 0; face < faceCount; ++face) {
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            pushEdge(faces[face][corner], faces[face][(corner + 1) % 3]);
        }
    }

    std::uint32_t liveFaces = faceCount;
    while (liveFaces > options.targetTriangles && !queue.empty()) {
        const CollapseCandidate candidate = queue.top();
        queue.pop();
        const std::uint32_t a = candidate.a;
        const std::uint32_t b = candidate.b;
        if (!positionAlive[a] || !positionAlive[b]) {
            continue;
        }
        if (version[a] != candidate.versionA || version[b] != candidate.versionB) {
            continue; // stale: one end moved since this was scored
        }
        if (options.maxError > 0.0 && candidate.cost > options.maxError) {
            break;
        }
        sharedFacesOf(a, b, shared);
        if (shared.empty()) {
            continue; // no longer an edge
        }
        if (!linkConditionHolds(a, b)) {
            continue;
        }

        // Try the quadric's minimum, and fall back to the chord when it folds
        // a face over. Rejecting the whole collapse instead would stall
        // decimation exactly on creased geometry - which is what this game's
        // hulls are almost entirely made of.
        Quadric combined = quadrics[a];
        combined.add(quadrics[b]);
        Placement chosen = subsetPlacement(combined, a, b);
        Placement solved;
        if (solvedPlacement(combined, a, b, solved) && solved.cost < chosen.cost &&
            !collapseFlipsAFace(a, b, solved.point)) {
            chosen = solved;
        } else if (collapseFlipsAFace(a, b, chosen.point)) {
            continue;
        }
        const Vec3 target = chosen.point;

        // Commit: b folds into a, the shared faces die, everything else that
        // pointed at b now points at a.
        points[a] = target;
        quadrics[a].add(quadrics[b]);
        positionAlive[b] = false;
        for (const std::uint32_t face : shared) {
            faceAlive[face] = false;
            --liveFaces;
        }
        for (const std::uint32_t face : incident[b]) {
            if (!faceAlive[face]) {
                continue;
            }
            for (std::uint32_t& position : faces[face]) {
                if (position == b) {
                    position = a;
                }
            }
            if (std::find(incident[a].begin(), incident[a].end(), face) == incident[a].end()) {
                incident[a].push_back(face);
            }
        }
        incident[b].clear();

        // Only `a` moved and only `a`'s quadric grew, so only edges touching it
        // are stale. Bumping the ring's versions as well would invalidate the
        // still-correct entries for edges BETWEEN two neighbours, and nothing
        // would ever re-push those - the queue would bleed candidates and the
        // decimation would stall short of its target.
        ++version[a];
        neighboursOf(a, neighboursA);
        const std::vector<std::uint32_t> ring = neighboursA;
        for (const std::uint32_t neighbour : ring) {
            pushEdge(a, neighbour);
        }
    }

    // Fold the result back onto the render mesh.
    std::vector<std::uint32_t> positionRemap(positionCount, kNoIndex);
    std::vector<Vec3> newPositions;
    for (std::uint32_t position = 0; position < positionCount; ++position) {
        if (positionAlive[position]) {
            positionRemap[position] = static_cast<std::uint32_t>(newPositions.size());
            newPositions.push_back(points[position]);
        }
    }
    // A dead position was folded into a live one; find which by walking the
    // faces, whose corners were rewritten as the collapses committed.
    std::vector<std::uint32_t> survivorOf(positionCount, kNoIndex);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        if (!faceAlive[face]) {
            continue;
        }
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            const std::uint32_t original = mesh.facePosition(face, corner);
            survivorOf[original] = faces[face][corner];
        }
    }

    std::vector<std::uint32_t> newIndices;
    newIndices.reserve(static_cast<std::size_t>(liveFaces) * 3);
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        if (!faceAlive[face]) {
            continue;
        }
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            newIndices.push_back(mesh.indices[(face * 3) + corner]);
        }
    }

    std::vector<EditVertex> newVertices = mesh.vertices;
    for (std::size_t i = 0; i < newVertices.size(); ++i) {
        const std::uint32_t original = newVertices[i].position;
        const std::uint32_t survivor = survivorOf[original] == kNoIndex ? original : survivorOf[original];
        newVertices[i].position = positionRemap[survivor] == kNoIndex ? 0 : positionRemap[survivor];
    }

    mesh.positions = std::move(newPositions);
    mesh.vertices = std::move(newVertices);
    mesh.indices = std::move(newIndices);
    removeDegenerateFaces(mesh);
    removeUnused(mesh);
    return mesh.triangleCount();
}

// --- measures ---------------------------------------------------------------

double signedVolume(const EditMesh& mesh)
{
    // The triple product in double, not float: a station is authored at a
    // hundred metres, and a float triple product there loses more than the
    // invariance this measure is used to assert.
    double volume = 0.0;
    const std::uint32_t faceCount = mesh.triangleCount();
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        const core::DVec3 p0 = core::toDVec3(mesh.positions[mesh.facePosition(face, 0)]);
        const core::DVec3 p1 = core::toDVec3(mesh.positions[mesh.facePosition(face, 1)]);
        const core::DVec3 p2 = core::toDVec3(mesh.positions[mesh.facePosition(face, 2)]);
        const core::DVec3 crossed{(p1.y * p2.z) - (p1.z * p2.y), (p1.z * p2.x) - (p1.x * p2.z),
                                  (p1.x * p2.y) - (p1.y * p2.x)};
        volume += ((p0.x * crossed.x) + (p0.y * crossed.y) + (p0.z * crossed.z)) / 6.0;
    }
    return volume;
}

double surfaceArea(const EditMesh& mesh)
{
    double area = 0.0;
    const std::uint32_t faceCount = mesh.triangleCount();
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        area += faceAreaOf(mesh, face);
    }
    return area;
}

MeshBounds bounds(const EditMesh& mesh)
{
    if (mesh.positions.empty()) {
        return {};
    }
    MeshBounds result{mesh.positions.front(), mesh.positions.front()};
    for (const Vec3& point : mesh.positions) {
        result.min = {std::min(result.min.x, point.x), std::min(result.min.y, point.y),
                      std::min(result.min.z, point.z)};
        result.max = {std::max(result.max.x, point.x), std::max(result.max.y, point.y),
                      std::max(result.max.z, point.z)};
    }
    return result;
}

float boundingRadius(const EditMesh& mesh)
{
    float longest = 0.0f;
    for (const Vec3& point : mesh.positions) {
        longest = std::max(longest, core::lengthSquared(point));
    }
    return std::sqrt(longest);
}

} // namespace sol::assets
