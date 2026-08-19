#pragma once

// Primitive mesh construction (engine plan Phase 9 stage B). This is the
// vocabulary `tools/scripts/gen_assets.ps1` grew - box, beam, torus, flat
// triangle - ported to C++ and extended with the two sweeps the shipped meshes
// were faking by hand, so a mesh can be authored by something a person can see
// the output of.
//
// Authoring is in DOUBLE and storage is in float: every primitive here is
// analytic, and rounding exactly once at build() is what lets a regenerated
// mesh come out bit-identical to the one it replaces. The emission order of
// every primitive matches the script's, for the same reason.
//
// A builder emits triangle soup - unshared corners, nothing welded. That is
// deliberate: welding is an EDIT (see mesh_edit.hpp), and a primitive that
// welded as it built could not reproduce a faceted hull.

#include "sol/assets/asset_loader.hpp"
#include "sol/core/math/vec.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sol::assets {

// Authoring-space point. DVec3 rather than Vec3 because these are the numbers
// the primitives compute with, not the numbers the GPU is handed.
using BuildPoint = core::DVec3;

struct BuildUv
{
    double u = 0.0;
    double v = 0.0;
};

// A point on a 2D profile curve, in whatever plane the sweep defines.
struct BuildProfilePoint
{
    double x = 0.0;
    double y = 0.0;
};

class MeshBuilder
{
public:
    [[nodiscard]] std::uint32_t vertexCount() const
    {
        return static_cast<std::uint32_t>(m_vertices.size());
    }
    [[nodiscard]] std::uint32_t triangleCount() const
    {
        return static_cast<std::uint32_t>(m_indices.size() / 3);
    }
    void clear();

    std::uint32_t addVertex(BuildPoint position, BuildPoint normal, BuildUv uv);
    void addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c);
    // Quad wound CCW seen from outside; emitted as (a,b,c) + (a,c,d).
    void addQuad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d);

    // Axis-aligned box. `tile` is texture repeats per METER; 0 stretches 0..1
    // across each face, which is right for a hull seen from hundreds of meters
    // and hopeless for a dash a meter from the player's eye.
    void addBox(BuildPoint center, BuildPoint size, double tile = 0.0);

    // Oriented box running from `from` to `to` with a width x height section -
    // what a canopy frame is made of, since addBox is axis-aligned only.
    void addBeam(BuildPoint from, BuildPoint to, double width, double height, double tile = 0.0);

    // Torus in the XZ plane about the origin, smooth in both directions.
    void addTorus(double majorRadius, double tubeRadius, std::uint32_t segU, std::uint32_t segV,
                  double uTiles);

    // Triangle carrying one computed normal - the faceted-hull primitive.
    void addFlatTriangle(BuildPoint p0, BuildPoint p1, BuildPoint p2, BuildUv uv0, BuildUv uv1,
                         BuildUv uv2);

    // Sweeps a profile about the +Y axis. Profile points are (radius, height),
    // ordered from one end of the surface to the other; the surface is smooth
    // both around the sweep and along the profile. `capEnds` closes an end
    // whose radius is non-zero with a fan on the axis.
    void addRevolve(std::span<const BuildProfilePoint> profile, std::uint32_t segments, double uTiles,
                    bool capEnds = false);

    // Extrudes a closed 2D outline along `from` -> `to`. The outline lies in
    // the plane normal to the run, wound CCW seen from `to` looking back.
    // Sides are faceted per outline edge; `capEnds` closes both ends with fans.
    void addExtrude(std::span<const BuildProfilePoint> outline, BuildPoint from, BuildPoint to,
                    double tile = 0.0, bool capEnds = true);

    // Rounds to float and hands back what the pipeline and the cooker want.
    [[nodiscard]] MeshData build() const;

private:
    struct BuildVertex
    {
        BuildPoint position;
        BuildPoint normal;
        BuildUv uv;
    };

    std::vector<BuildVertex> m_vertices;
    std::vector<std::uint32_t> m_indices;
};

} // namespace sol::assets
