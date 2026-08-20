#include "sol/assets/mesh_build.hpp"

#include "sol/core/assert.hpp"

#include <cmath>
#include <utility>

namespace sol::assets {
namespace {

constexpr double kPi = 3.14159265358979323846;

// The corner order every quad-emitting primitive here shares, matching the
// generator script it was ported from.
constexpr BuildUv kQuadUvs[4] = {{0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}};

[[nodiscard]] double dot3(BuildPoint a, BuildPoint b)
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] BuildPoint cross3(BuildPoint a, BuildPoint b)
{
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

[[nodiscard]] BuildPoint normalize3(BuildPoint v)
{
    const double length = std::sqrt(dot3(v, v));
    if (length <= 0.0) {
        return {};
    }
    return {v.x / length, v.y / length, v.z / length};
}

// ⚑ Subtraction from zero, not IEEE negation, and the difference is a real
// one: `-x` turns +0 into -0, and PowerShell's unary minus does not. Ninety-two
// of the cockpit's 1278 normal floats are a +0 that IEEE negation would write
// as -0, so a port that used `-x` would re-author a shipped asset over a sign
// bit nothing can see. Every opposite-facing normal below goes through here.
// (Measured against the cockpit.gltf the generator emitted, before that file
// was retired for cockpit.forge; the sign bits are unchanged and still asserted
// by the geometry suite, which is why this stays.)
[[nodiscard]] double flipped(double v)
{
    return 0.0 - v;
}

[[nodiscard]] BuildPoint flipped(BuildPoint v)
{
    return {0.0 - v.x, 0.0 - v.y, 0.0 - v.z};
}

} // namespace

BuildTransform BuildTransform::fromTrs(BuildPoint translation, BuildPoint rotationRadians,
                                       BuildPoint scale)
{
    const double cx = std::cos(rotationRadians.x);
    const double sx = std::sin(rotationRadians.x);
    const double cy = std::cos(rotationRadians.y);
    const double sy = std::sin(rotationRadians.y);
    const double cz = std::cos(rotationRadians.z);
    const double sz = std::sin(rotationRadians.z);

    // Rz * Ry * Rx, columns first.
    const BuildPoint rx{cy * cz, cy * sz, 0.0 - sy};
    const BuildPoint ry{(sx * sy * cz) - (cx * sz), (sx * sy * sz) + (cx * cz), sx * cy};
    const BuildPoint rz{(cx * sy * cz) + (sx * sz), (cx * sy * sz) - (sx * cz), cx * cy};

    BuildTransform out;
    out.x = {rx.x * scale.x, rx.y * scale.x, rx.z * scale.x};
    out.y = {ry.x * scale.y, ry.y * scale.y, ry.z * scale.y};
    out.z = {rz.x * scale.z, rz.y * scale.z, rz.z * scale.z};
    out.translation = translation;
    return out;
}

BuildPoint BuildTransform::transformPoint(BuildPoint p) const
{
    return {(x.x * p.x) + (y.x * p.y) + (z.x * p.z) + translation.x,
            (x.y * p.x) + (y.y * p.y) + (z.y * p.z) + translation.y,
            (x.z * p.x) + (y.z * p.y) + (z.z * p.z) + translation.z};
}

BuildPoint BuildTransform::transformDirection(BuildPoint v) const
{
    return {(x.x * v.x) + (y.x * v.y) + (z.x * v.z), (x.y * v.x) + (y.y * v.y) + (z.y * v.z),
            (x.z * v.x) + (y.z * v.y) + (z.z * v.z)};
}

double BuildTransform::determinant() const
{
    return dot3(x, cross3(y, z));
}

bool BuildTransform::isIdentity() const
{
    return x.x == 1.0 && x.y == 0.0 && x.z == 0.0 && y.x == 0.0 && y.y == 1.0 && y.z == 0.0 &&
           z.x == 0.0 && z.y == 0.0 && z.z == 1.0 && translation.x == 0.0 &&
           translation.y == 0.0 && translation.z == 0.0;
}

bool BuildTransform::inverse(BuildTransform& out) const
{
    // Rows of the inverse linear part, before the division.
    const BuildPoint rowX = cross3(y, z);
    const BuildPoint rowY = cross3(z, x);
    const BuildPoint rowZ = cross3(x, y);
    const double det = dot3(x, rowX);
    if (det == 0.0) {
        return false;
    }
    const double scale = 1.0 / det;

    // Rows above, columns here: this struct stores basis COLUMNS, so the
    // transpose happens in the assignment rather than in a separate step.
    out.x = {rowX.x * scale, rowY.x * scale, rowZ.x * scale};
    out.y = {rowX.y * scale, rowY.y * scale, rowZ.y * scale};
    out.z = {rowX.z * scale, rowY.z * scale, rowZ.z * scale};
    out.translation = flipped(out.transformDirection(translation));
    return true;
}

BuildTransform operator*(const BuildTransform& parent, const BuildTransform& child)
{
    BuildTransform out;
    out.x = parent.transformDirection(child.x);
    out.y = parent.transformDirection(child.y);
    out.z = parent.transformDirection(child.z);
    out.translation = parent.transformPoint(child.translation);
    return out;
}

void MeshBuilder::clear()
{
    m_vertices.clear();
    m_indices.clear();
}

// The normal matrix is the inverse transpose of the linear part. It is built as
// the adjugate - the cross products of the basis columns - rather than a true
// inverse, because that needs no division: it comes out scaled by the
// determinant, and every normal is renormalised on the way out anyway, so a
// singular transform degrades to a zero normal instead of dividing by zero.
//
// ⚑ The MAGNITUDE of that determinant scaling washes out in the renormalise;
// its SIGN does not. adj(M) = det(M) * inverse-transpose(M), so under a
// mirroring transform the adjugate points every normal exactly backwards - the
// mirrored face of a box would light as though it faced into the solid. Undoing
// the sign is what the negation below is for, and it is separate from the
// winding swap: one fixes which way the surface faces, the other which way it
// is wound.
void MeshBuilder::setTransform(const BuildTransform& transform)
{
    m_transform = transform;
    m_hasTransform = !transform.isIdentity();

    const double det = transform.determinant();
    m_mirrored = det < 0.0;

    m_normalTransform = {};
    m_normalTransform.x = cross3(transform.y, transform.z);
    m_normalTransform.y = cross3(transform.z, transform.x);
    m_normalTransform.z = cross3(transform.x, transform.y);
    if (m_mirrored) {
        m_normalTransform.x = flipped(m_normalTransform.x);
        m_normalTransform.y = flipped(m_normalTransform.y);
        m_normalTransform.z = flipped(m_normalTransform.z);
    }
    m_normalTransform.translation = {0, 0, 0};
}

std::uint32_t MeshBuilder::addVertex(BuildPoint position, BuildPoint normal, BuildUv uv)
{
    const auto index = static_cast<std::uint32_t>(m_vertices.size());
    if (m_hasTransform) {
        position = m_transform.transformPoint(position);
        normal = normalize3(m_normalTransform.transformDirection(normal));
    }
    m_vertices.push_back({position, normal, uv});
    return index;
}

void MeshBuilder::addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    if (m_mirrored) {
        std::swap(b, c);
    }
    m_indices.push_back(a);
    m_indices.push_back(b);
    m_indices.push_back(c);
}

void MeshBuilder::addQuad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
{
    addTriangle(a, b, c);
    addTriangle(a, c, d);
}

void MeshBuilder::addBox(BuildPoint center, BuildPoint size, double tile)
{
    const double hx = size.x / 2;
    const double hy = size.y / 2;
    const double hz = size.z / 2;

    struct Face
    {
        BuildPoint normal;
        BuildPoint corners[4];
        double spanU;
        double spanV;
    };

    // Each face: outward normal, then four corners CCW viewed from outside.
    // The spans are how wide and tall the face physically is, so a tiled uv is
    // sized from the surface rather than stretched over it.
    const Face faces[6] = {
        {{0, 0, 1},
         {{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}},
         size.x,
         size.y},
        {{0, 0, -1},
         {{hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}, {hx, hy, -hz}},
         size.x,
         size.y},
        {{1, 0, 0},
         {{hx, -hy, hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {hx, hy, hz}},
         size.z,
         size.y},
        {{-1, 0, 0},
         {{-hx, -hy, -hz}, {-hx, -hy, hz}, {-hx, hy, hz}, {-hx, hy, -hz}},
         size.z,
         size.y},
        {{0, 1, 0},
         {{-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz}, {-hx, hy, -hz}},
         size.x,
         size.z},
        {{0, -1, 0},
         {{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz}, {-hx, -hy, hz}},
         size.x,
         size.z},
    };

    for (const Face& face : faces) {
        const double spanU = tile > 0 ? face.spanU * tile : 1.0;
        const double spanV = tile > 0 ? face.spanV * tile : 1.0;
        const auto base = static_cast<std::uint32_t>(m_vertices.size());
        for (int i = 0; i < 4; ++i) {
            const BuildPoint& p = face.corners[i];
            addVertex({p.x + center.x, p.y + center.y, p.z + center.z}, face.normal,
                      {kQuadUvs[i].u * spanU, kQuadUvs[i].v * spanV});
        }
        addQuad(base, base + 1, base + 2, base + 3);
    }
}

void MeshBuilder::addBeam(BuildPoint from, BuildPoint to, double width, double height, double tile)
{
    BuildPoint dir{to.x - from.x, to.y - from.y, to.z - from.z};
    const double length = std::sqrt(dot3(dir, dir));
    if (length <= 0.0) {
        return;
    }
    dir = {dir.x / length, dir.y / length, dir.z / length};

    // Any reference not parallel to the run; a vertical beam takes Z.
    const BuildPoint reference =
        std::abs(dir.y) > 0.9 ? BuildPoint{0.0, 0.0, 1.0} : BuildPoint{0.0, 1.0, 0.0};
    const BuildPoint side = normalize3(cross3(dir, reference));
    const BuildPoint up = cross3(side, dir);

    const double halfWidth = width / 2;
    const double halfHeight = height / 2;

    // Corners 0-3 at `from` and 4-7 at `to`, each ring (-s,-u) (+s,-u) (+s,+u) (-s,+u).
    BuildPoint corners[8];
    const BuildPoint ends[2] = {from, to};
    const double signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 4; ++i) {
            const double s = signs[i][0];
            const double u = signs[i][1];
            corners[(e * 4) + i] = {(ends[e].x + (side.x * s * halfWidth)) + (up.x * u * halfHeight),
                                    (ends[e].y + (side.y * s * halfWidth)) + (up.y * u * halfHeight),
                                    (ends[e].z + (side.z * s * halfWidth)) + (up.z * u * halfHeight)};
        }
    }

    struct Face
    {
        BuildPoint normal;
        int corner[4];
        double spanU;
        double spanV;
    };

    // (side, up, dir) is left-handed here - cross(side, up) = -dir - so each
    // face's winding is picked to make cross(v1-v0, v2-v0) come out along its
    // own outward normal. Caps are width x height; the sides run the length.
    const Face faces[6] = {
        {dir, {4, 7, 6, 5}, width, height},
        {flipped(dir), {0, 1, 2, 3}, width, height},
        {side, {1, 5, 6, 2}, length, height},
        {flipped(side), {0, 3, 7, 4}, length, height},
        {up, {3, 2, 6, 7}, length, width},
        {flipped(up), {0, 4, 5, 1}, length, width},
    };

    for (const Face& face : faces) {
        const double spanU = tile > 0 ? face.spanU * tile : 1.0;
        const double spanV = tile > 0 ? face.spanV * tile : 1.0;
        const auto base = static_cast<std::uint32_t>(m_vertices.size());
        for (int i = 0; i < 4; ++i) {
            addVertex(corners[face.corner[i]], face.normal,
                      {kQuadUvs[i].u * spanU, kQuadUvs[i].v * spanV});
        }
        addQuad(base, base + 1, base + 2, base + 3);
    }
}

void MeshBuilder::addTorus(double majorRadius, double tubeRadius, std::uint32_t segU, std::uint32_t segV,
                           double uTiles)
{
    SOL_ASSERT(segU >= 3 && segV >= 3);
    const auto base = static_cast<std::uint32_t>(m_vertices.size());
    for (std::uint32_t i = 0; i <= segU; ++i) {
        const double phi = (2 * kPi * static_cast<double>(i)) / static_cast<double>(segU);
        const double cp = std::cos(phi);
        const double sp = std::sin(phi);
        for (std::uint32_t j = 0; j <= segV; ++j) {
            const double theta = (2 * kPi * static_cast<double>(j)) / static_cast<double>(segV);
            const double ct = std::cos(theta);
            const double st = std::sin(theta);
            addVertex({(majorRadius + (tubeRadius * ct)) * cp, tubeRadius * st,
                       (majorRadius + (tubeRadius * ct)) * sp},
                      {ct * cp, st, ct * sp},
                      {(uTiles * static_cast<double>(i)) / static_cast<double>(segU),
                       static_cast<double>(j) / static_cast<double>(segV)});
        }
    }

    const std::uint32_t stride = segV + 1;
    for (std::uint32_t i = 0; i < segU; ++i) {
        for (std::uint32_t j = 0; j < segV; ++j) {
            const std::uint32_t a = base + (i * stride) + j;
            const std::uint32_t b = a + stride;
            // CCW viewed from outside the tube.
            addTriangle(a, a + 1, b + 1);
            addTriangle(a, b + 1, b);
        }
    }
}

void MeshBuilder::addFlatTriangle(BuildPoint p0, BuildPoint p1, BuildPoint p2, BuildUv uv0, BuildUv uv1,
                                  BuildUv uv2)
{
    const BuildPoint edge0{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    const BuildPoint edge1{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
    const BuildPoint normal = normalize3(cross3(edge0, edge1));
    const auto base = static_cast<std::uint32_t>(m_vertices.size());
    addVertex(p0, normal, uv0);
    addVertex(p1, normal, uv1);
    addVertex(p2, normal, uv2);
    addTriangle(base, base + 1, base + 2);
}

void MeshBuilder::addRevolve(std::span<const BuildProfilePoint> profile, std::uint32_t segments,
                             double uTiles, bool capEnds)
{
    if (profile.size() < 2 || segments < 3) {
        return;
    }
    const auto rings = static_cast<std::uint32_t>(profile.size());

    // Segment normals in the (radius, height) plane: for a profile ordered from
    // one end of the surface to the other, (dy, -dx) points away from the axis.
    std::vector<BuildProfilePoint> segmentNormals(rings - 1);
    std::vector<double> arc(rings, 0.0);
    for (std::uint32_t k = 0; k + 1 < rings; ++k) {
        const double dx = profile[k + 1].x - profile[k].x;
        const double dy = profile[k + 1].y - profile[k].y;
        const double length = std::sqrt((dx * dx) + (dy * dy));
        segmentNormals[k] = length > 0.0 ? BuildProfilePoint{dy / length, flipped(dx / length)}
                                         : BuildProfilePoint{1.0, 0.0};
        arc[k + 1] = arc[k] + length;
    }
    const double totalArc = arc.back();

    std::vector<BuildProfilePoint> pointNormals(rings);
    for (std::uint32_t k = 0; k < rings; ++k) {
        const BuildProfilePoint before = segmentNormals[k > 0 ? k - 1 : 0];
        const BuildProfilePoint after = segmentNormals[k + 1 < rings ? k : rings - 2];
        const double nx = before.x + after.x;
        const double ny = before.y + after.y;
        const double length = std::sqrt((nx * nx) + (ny * ny));
        pointNormals[k] = length > 0.0 ? BuildProfilePoint{nx / length, ny / length} : before;
    }

    const auto base = static_cast<std::uint32_t>(m_vertices.size());
    for (std::uint32_t i = 0; i <= segments; ++i) {
        const double phi = (2 * kPi * static_cast<double>(i)) / static_cast<double>(segments);
        const double cp = std::cos(phi);
        const double sp = std::sin(phi);
        for (std::uint32_t k = 0; k < rings; ++k) {
            const double v = totalArc > 0.0 ? arc[k] / totalArc
                                            : static_cast<double>(k) / static_cast<double>(rings - 1);
            addVertex({profile[k].x * cp, profile[k].y, profile[k].x * sp},
                      normalize3({pointNormals[k].x * cp, pointNormals[k].y, pointNormals[k].x * sp}),
                      {(uTiles * static_cast<double>(i)) / static_cast<double>(segments), v});
        }
    }

    for (std::uint32_t i = 0; i < segments; ++i) {
        for (std::uint32_t k = 0; k + 1 < rings; ++k) {
            const std::uint32_t a = base + (i * rings) + k;
            addQuad(a, a + 1, a + rings + 1, a + rings);
        }
    }

    if (!capEnds) {
        return;
    }
    // A cap is its own fan on the axis rather than a reuse of the ring
    // vertices: the ring's normal points outward, and a cap's does not.
    const struct
    {
        std::uint32_t ring;
        double normalY;
    } caps[2] = {{0, -1.0}, {rings - 1, 1.0}};
    for (const auto& cap : caps) {
        if (profile[cap.ring].x <= 0.0) {
            continue;
        }
        const double y = profile[cap.ring].y;
        const BuildPoint normal{0.0, cap.normalY, 0.0};
        for (std::uint32_t i = 0; i < segments; ++i) {
            const double phi0 = (2 * kPi * static_cast<double>(i)) / static_cast<double>(segments);
            const double phi1 = (2 * kPi * static_cast<double>(i + 1)) / static_cast<double>(segments);
            const double r = profile[cap.ring].x;
            const BuildPoint p0{r * std::cos(phi0), y, r * std::sin(phi0)};
            const BuildPoint p1{r * std::cos(phi1), y, r * std::sin(phi1)};
            const BuildUv uv0{(std::cos(phi0) * 0.5) + 0.5, (std::sin(phi0) * 0.5) + 0.5};
            const BuildUv uv1{(std::cos(phi1) * 0.5) + 0.5, (std::sin(phi1) * 0.5) + 0.5};
            const auto first = static_cast<std::uint32_t>(m_vertices.size());
            addVertex({0.0, y, 0.0}, normal, {0.5, 0.5});
            if (cap.normalY < 0.0) {
                addVertex(p0, normal, uv0);
                addVertex(p1, normal, uv1);
            } else {
                addVertex(p1, normal, uv1);
                addVertex(p0, normal, uv0);
            }
            addTriangle(first, first + 1, first + 2);
        }
    }
}

void MeshBuilder::addExtrude(std::span<const BuildProfilePoint> outline, BuildPoint from, BuildPoint to,
                             double tile, bool capEnds)
{
    if (outline.size() < 3) {
        return;
    }
    BuildPoint dir{to.x - from.x, to.y - from.y, to.z - from.z};
    const double runLength = std::sqrt(dot3(dir, dir));
    if (runLength <= 0.0) {
        return;
    }
    dir = {dir.x / runLength, dir.y / runLength, dir.z / runLength};
    const BuildPoint reference =
        std::abs(dir.y) > 0.9 ? BuildPoint{0.0, 0.0, 1.0} : BuildPoint{0.0, 1.0, 0.0};
    const BuildPoint side = normalize3(cross3(dir, reference));
    const BuildPoint up = cross3(side, dir);

    const auto pointAt = [&](BuildPoint end, BuildProfilePoint p) {
        return BuildPoint{end.x + (side.x * p.x) + (up.x * p.y), end.y + (side.y * p.x) + (up.y * p.y),
                          end.z + (side.z * p.x) + (up.z * p.y)};
    };

    const auto count = static_cast<std::uint32_t>(outline.size());
    for (std::uint32_t k = 0; k < count; ++k) {
        const BuildProfilePoint a = outline[k];
        const BuildProfilePoint b = outline[(k + 1) % count];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double edgeLength = std::sqrt((dx * dx) + (dy * dy));
        if (edgeLength <= 0.0) {
            continue;
        }
        // Outward normal of a CCW outline edge, lifted into the run's frame.
        const double nx = dy / edgeLength;
        const double ny = flipped(dx / edgeLength);
        const BuildPoint normal{(side.x * nx) + (up.x * ny), (side.y * nx) + (up.y * ny),
                                (side.z * nx) + (up.z * ny)};
        const double spanU = tile > 0 ? runLength * tile : 1.0;
        const double spanV = tile > 0 ? edgeLength * tile : 1.0;
        const BuildPoint quad[4] = {pointAt(from, a), pointAt(to, a), pointAt(to, b), pointAt(from, b)};
        const auto base = static_cast<std::uint32_t>(m_vertices.size());
        for (int i = 0; i < 4; ++i) {
            addVertex(quad[i], normal, {kQuadUvs[i].u * spanU, kQuadUvs[i].v * spanV});
        }
        addQuad(base, base + 1, base + 2, base + 3);
    }

    if (!capEnds) {
        return;
    }
    BuildProfilePoint centroid{};
    for (const BuildProfilePoint& p : outline) {
        centroid.x += p.x;
        centroid.y += p.y;
    }
    centroid.x /= static_cast<double>(count);
    centroid.y /= static_cast<double>(count);

    const auto capUv = [&](BuildProfilePoint p) {
        return tile > 0 ? BuildUv{p.x * tile, p.y * tile} : BuildUv{p.x, p.y};
    };
    // cross(side, up) is -dir, so a CCW outline's own normal points back down
    // the run: the `from` cap keeps that order and the `to` cap reverses it.
    for (std::uint32_t k = 0; k < count; ++k) {
        const BuildProfilePoint a = outline[k];
        const BuildProfilePoint b = outline[(k + 1) % count];
        const BuildPoint backNormal = flipped(dir);
        auto first = static_cast<std::uint32_t>(m_vertices.size());
        addVertex(pointAt(from, centroid), backNormal, capUv(centroid));
        addVertex(pointAt(from, a), backNormal, capUv(a));
        addVertex(pointAt(from, b), backNormal, capUv(b));
        addTriangle(first, first + 1, first + 2);

        first = static_cast<std::uint32_t>(m_vertices.size());
        addVertex(pointAt(to, centroid), dir, capUv(centroid));
        addVertex(pointAt(to, b), dir, capUv(b));
        addVertex(pointAt(to, a), dir, capUv(a));
        addTriangle(first, first + 1, first + 2);
    }
}

MeshData MeshBuilder::build() const
{
    MeshData data;
    data.vertices.reserve(m_vertices.size());
    for (const BuildVertex& vertex : m_vertices) {
        MeshVertex out{};
        out.position[0] = static_cast<float>(vertex.position.x);
        out.position[1] = static_cast<float>(vertex.position.y);
        out.position[2] = static_cast<float>(vertex.position.z);
        out.normal[0] = static_cast<float>(vertex.normal.x);
        out.normal[1] = static_cast<float>(vertex.normal.y);
        out.normal[2] = static_cast<float>(vertex.normal.z);
        out.uv[0] = static_cast<float>(vertex.uv.u);
        out.uv[1] = static_cast<float>(vertex.uv.v);
        data.vertices.push_back(out);
    }
    data.indices = m_indices;
    return data;
}

} // namespace sol::assets
