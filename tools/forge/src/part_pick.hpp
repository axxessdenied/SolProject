#pragma once

// Which part a click landed on, and where each part sits (engine plan Phase 9
// stage N).
//
// Stage M made a forty-part list fit in its panel; it did not make it
// navigable. A filter is reading - you have to know the part is called
// `Fin_003` before you can type `fin` - and the reason an author is looking at
// a viewport in the first place is that they do not. So the viewport becomes
// the index into the list: click a triangle, select the part that emitted it.
//
// ⚑⚑ THE ANSWER WAS ALREADY IN THE STRUCT. `ForgeFace::part` has recorded which
// part emitted each triangle since E5 - filled by `forgeTopology` out of the
// same single traversal that numbers the points and the edges - and
// `PointTool::pickFaceAt` has cast the ray since E4d. Neither was built for
// this and nothing had ever read the two together.
//
// ⚑⚑ THIS IS A PURE FUNCTION OF SPANS AND CALLS NOTHING, WHICH IS THE WHOLE
// REASON IT IS ITS OWN HEADER - the same ruling `list_layout.hpp` was written
// under one stage earlier. `sol_forge_tests` compiles `mesh_library.cpp` alone
// and links no ImGui, no device and no window (tools/forge/CMakeLists.txt), so
// anything that lived in `point_tool.cpp` would be untestable in the only suite
// that could test it. Phase 15's linkage rule, applied before the promise.

#include "sol/assets/forge_doc.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace forge {

// No part is named. Distinct from the document's own no-owner sentinel below,
// because "the click missed" and "the triangle belongs to nothing" arrive at
// the same place and a caller only has to handle one of them.
inline constexpr std::size_t kNoPart = static_cast<std::size_t>(-1);

// One part's axis-aligned extent, in the frame the mesh is built in.
//
// ⚑ A BOX RATHER THAN A WIREFRAME, AND THE BUDGET IS WHAT DECIDES IT.
// `DebugDrawRenderer` holds 8192 vertices and `line()` returns SILENTLY when
// they are spent, which is why `PointTool::kEdgeBudget` exists at all -
// `station.forge`'s full wireframe is 1,602 edges. A PER-PART wireframe has no
// bound worth the name: `asteroid.forge` is ONE baked part of 99 KB, so its
// worst case is the entire mesh. A box is twelve lines whatever the part, and
// it answers the only question being asked - which lump is that.
struct PartBounds
{
    sol::assets::BuildPoint min{0, 0, 0};
    sol::assets::BuildPoint max{0, 0, 0};
    // False when the part emitted no triangle with area. It is not the same as
    // an empty box: `gate_membrane.forge` fans 32 real triangles and 32
    // degenerate ones, and `forgeTopology` drops the degenerate half, so a part
    // CAN be present in the document and absent from the face list.
    bool any = false;

    [[nodiscard]] constexpr sol::assets::BuildPoint centre() const noexcept
    {
        return {(min.x + max.x) * 0.5, (min.y + max.y) * 0.5, (min.z + max.z) * 0.5};
    }
};

// The part a picked face belongs to, or `kNoPart`.
//
// ⚑⚑ THE GUARD IS THE POINT OF THIS FUNCTION AND THE SENTINEL IS ONE PAST THE
// END. `collectPoints` does `ownerOf.assign(mesh.vertices.size(),
// doc.parts.size())` and only overwrites it inside a part's own vertex range,
// so a vertex produced by a `[build]` post-pass belongs to NO part and
// `ForgeFace::part` legitimately comes back as `doc.parts.size()`. Indexing on
// that is an out-of-range read a debug build catches and a release build does
// not. One place for the check, because the alternative is remembering it at
// every call site - which is how "a rule applied in two places" became a defect
// in this tool four times.
[[nodiscard]] inline std::size_t forgePartOfFace(std::span<const sol::assets::ForgeFace> faces,
                                                 std::size_t face,
                                                 std::size_t partCount) noexcept
{
    if (face >= faces.size()) {
        return kNoPart;
    }
    const std::size_t part = faces[face].part;
    return part < partCount ? part : kNoPart;
}

// Every part's extent, indexed by part, sized to `partCount`. A face naming no
// part contributes to nothing rather than to part zero.
//
// ⚑ Accumulated over FACES rather than over points, because a point is shared:
// `ship.forge`'s front corner stands in five parts at once, so a point-driven
// walk would hand every one of them the same corner and five parts would claim
// a box they do not fill. A triangle is emitted by exactly one part, which is
// the property that makes this well defined at all.
inline void forgePartBounds(std::span<const sol::assets::ForgePoint> points,
                            std::span<const sol::assets::ForgeFace> faces,
                            std::size_t partCount,
                            std::vector<PartBounds>& out)
{
    out.assign(partCount, PartBounds{});
    for (const sol::assets::ForgeFace& face : faces) {
        if (face.part >= partCount) {
            continue; // no owner - see forgePartOfFace
        }
        PartBounds& bounds = out[face.part];
        const std::uint32_t corners[3] = {face.a, face.b, face.c};
        for (const std::uint32_t corner : corners) {
            if (corner >= points.size()) {
                continue;
            }
            const sol::assets::BuildPoint& p = points[corner].position;
            if (!bounds.any) {
                bounds.min = p;
                bounds.max = p;
                bounds.any = true;
                continue;
            }
            bounds.min.x = p.x < bounds.min.x ? p.x : bounds.min.x;
            bounds.min.y = p.y < bounds.min.y ? p.y : bounds.min.y;
            bounds.min.z = p.z < bounds.min.z ? p.z : bounds.min.z;
            bounds.max.x = p.x > bounds.max.x ? p.x : bounds.max.x;
            bounds.max.y = p.y > bounds.max.y ? p.y : bounds.max.y;
            bounds.max.z = p.z > bounds.max.z ? p.z : bounds.max.z;
        }
    }
}

// Which part gets the hover box, given what each surface is pointing at
// (engine plan Phase 9 stage O). `kNoPart` for none.
//
// ⚑⚑ TWO SURFACES CAN POINT AT A PART AND THERE IS ONE BOX, SO THE PRECEDENCE
// HAS TO BE STATED SOMEWHERE RATHER THAN FALL OUT OF WHICH ONE WROTE LAST. The
// viewport wins when it has an answer: it is the surface the ray was cast for,
// and `PointTool::update` clears it the moment ImGui takes the mouse, so the
// two are mutually exclusive in practice and this is a tie-break that should
// never be reached. It is written down anyway - "they cannot both be set" is an
// invariant held by a different file, and this stage exists because the last
// one left a hover uncleared.
//
// ⚑ THE SELECTION SUPPRESSES THE HOVER, WHICH IS THE CLAUSE THAT CARRIES THE
// MEANING. The selected part already has a green box; drawing amber over it is
// two answers to one question, and it would make hovering the row you are on
// look like a state change. `drawMarkers` has enforced this for the viewport
// hover since stage N - this lifts the rule out of the draw so the list hover
// cannot arrive without it.
[[nodiscard]] inline constexpr std::size_t forgeHoverBox(std::size_t viewportHover,
                                                         std::size_t listHover,
                                                         std::size_t selected) noexcept
{
    const std::size_t hover = viewportHover != kNoPart ? viewportHover : listHover;
    return hover == selected ? kNoPart : hover;
}

} // namespace forge
