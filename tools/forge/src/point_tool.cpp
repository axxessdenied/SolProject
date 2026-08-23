#include "point_tool.hpp"

#include "part_editor.hpp"

#include "sol/ui/pick.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace forge {

using namespace sol;
using assets::ForgeDoc;
using assets::ForgeEdge;
using assets::ForgeFace;
using assets::ForgePoint;
using assets::ForgePointWrite;

namespace {

// ⚑ The camera basis, read out of the VIEW matrix rather than rebuilt from the
// orbit camera's yaw and pitch. Two expressions of one thing is how a drag ends
// up going somewhere other than where the cursor went, and this file already
// depends on the view matrix for the projection - so it may as well depend on
// it for the drag.
//
// The view matrix takes world to camera, so its rotation ROWS are the camera's
// axes in world space. Column-major storage (`m[column * 4 + row]`) puts row 0
// at m[0], m[4], m[8].
[[nodiscard]] core::Vec3 cameraRight(const core::Mat4& view)
{
    return {view.m[0], view.m[4], view.m[8]};
}

[[nodiscard]] core::Vec3 cameraUp(const core::Mat4& view)
{
    return {view.m[1], view.m[5], view.m[9]};
}

// ⚑ Row 2 is the camera's BACKWARD axis, not its forward one - the camera looks
// down -Z, so +Z_camera points at the viewer. Adding a little of it to a world
// point pulls that point toward the camera, which is what the edge overlay
// needs and the reason this is not simply `-cameraForward`.
[[nodiscard]] core::Vec3 cameraBackward(const core::Mat4& view)
{
    return {view.m[2], view.m[6], view.m[10]};
}

// The camera's world-space eye. The view matrix maps world to camera as
// `R * (p - eye)` with the rotation stored by rows, so the translation column
// holds `-R * eye` and the eye comes back by applying R's transpose.
[[nodiscard]] core::Vec3 cameraEye(const core::Mat4& view)
{
    const core::Vec3 right = cameraRight(view);
    const core::Vec3 up = cameraUp(view);
    const core::Vec3 backward = cameraBackward(view);
    return {-((view.m[12] * right.x) + (view.m[13] * up.x) + (view.m[14] * backward.x)),
            -((view.m[12] * right.y) + (view.m[13] * up.y) + (view.m[14] * backward.y)),
            -((view.m[12] * right.z) + (view.m[13] * up.z) + (view.m[14] * backward.z))};
}

[[nodiscard]] core::Vec3 asVec3(const assets::BuildPoint& p)
{
    return {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
}

// A point marker: three axis-aligned segments through the point. Not a screen
// facing quad, because the only primitive this frame has is a line and a cross
// reads as a position from any angle a turntable can reach.
void addCross(renderer::DebugDrawRenderer& lines, core::Vec3 at, float half, core::Vec4 color)
{
    lines.line({at.x - half, at.y, at.z}, {at.x + half, at.y, at.z}, color);
    lines.line({at.x, at.y - half, at.z}, {at.x, at.y + half, at.z}, color);
    lines.line({at.x, at.y, at.z - half}, {at.x, at.y, at.z + half}, color);
}

// A part marker (stage N): the twelve edges of its axis-aligned box, drawn IN
// FRONT of the mesh.
//
// ⚑ TWELVE LINES WHATEVER THE PART, WHICH IS THE ENTIRE REASON IT IS A BOX. See
// `part_pick.hpp` - `DebugDrawRenderer` drops lines SILENTLY once its 8192
// vertices are spent, and a per-part wireframe of the baked asteroid would be
// the whole mesh. A part that is flat in one axis draws a rectangle, which is
// correct rather than degenerate: `gate_membrane.forge` IS a flat disc.
//
// ⚑⚑ AND THE PART THAT WAS MEASURED RATHER THAN REASONED ABOUT: THE BOX HAS TO
// BE PULLED TOWARD THE EYE OR IT IS INVISIBLE FOR EXACTLY THE PARTS THIS STAGE
// EXISTS TO FIND. `DebugDrawRenderer` tests depth against the mesh, and an
// INTERIOR part's box is inside the hull by definition - the first live run
// selected `console_starboard` on `freighter_cockpit`, the Parts list scrolled
// to it and highlighted it, and the viewport showed nothing at all. Most parts
// of a cockpit are interior parts, and the reverse gesture (click a ROW, see
// where it is) then shows nothing whatsoever.
//
// ⚑⚑ THE PULL IS A UNIFORM SCALE ABOUT THE EYE, WHICH LEAVES THE OUTLINE
// PIXEL-IDENTICAL. Every corner moves along its own view ray, and a projection
// maps that whole ray to one screen point - so `p' = eye + (p - eye) * k`
// changes the depth and nothing else. That is what makes this an overlay rather
// than a distortion, and it is why a per-corner bias (which is what the EDGE
// markers use) would be wrong here: a bias moves corners at different depths by
// the same amount and skews the box.
//
// ⚑ `k` is chosen so the NEAREST corner lands at `kFrontDepth`, comfortably
// ahead of the 0.05 m near plane, and is never pushed further AWAY than it
// already is. A box that straddles the camera is left alone: it has a corner at
// or behind the eye, there is no scale that fixes that, and the projection
// mirrors such a point rather than clipping it.
void addBox(renderer::DebugDrawRenderer& lines, const PartBounds& bounds,
            const PointTool::Viewport& viewport, core::Vec4 color)
{
    constexpr float kFrontDepth = 0.12f; // metres, against a 0.05 m near plane

    const core::Vec3 lo = asVec3(bounds.min);
    const core::Vec3 hi = asVec3(bounds.max);
    core::Vec3 corner[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z},
                            {lo.x, hi.y, lo.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
                            {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};

    float nearest = 0.0f;
    bool behind = false;
    for (const core::Vec3& p : corner) {
        const float depth = -core::transformPoint(viewport.view, p).z;
        if (depth <= 0.0f) {
            behind = true;
            break;
        }
        nearest = (nearest == 0.0f || depth < nearest) ? depth : nearest;
    }
    if (!behind && nearest > kFrontDepth) {
        const core::Vec3 eye = cameraEye(viewport.view);
        const float k = kFrontDepth / nearest;
        for (core::Vec3& p : corner) {
            p = {eye.x + (p.x - eye.x) * k, eye.y + (p.y - eye.y) * k,
                 eye.z + (p.z - eye.z) * k};
        }
    }

    // Two rings and the four uprights between them.
    for (int i = 0; i < 4; ++i) {
        lines.line(corner[i], corner[(i + 1) % 4], color);
        lines.line(corner[i + 4], corner[((i + 1) % 4) + 4], color);
        lines.line(corner[i], corner[i + 4], color);
    }
}

// Distance in pixels from `cursor` to the segment [a, b].
//
// ⚑ This is the whole edge hit test, and it is why edge picking did NOT need
// Moller-Trumbore: an edge is a segment on screen once both ends are projected,
// so the test is two dimensional. Faces are what need the ray, at E4d.
//
// A segment whose ends project to one pixel degenerates to a point, which is
// the answer rather than a division by zero.
[[nodiscard]] float distanceToSegment(core::Vec2 a, core::Vec2 b, core::Vec2 cursor)
{
    const float runX = b.x - a.x;
    const float runY = b.y - a.y;
    const float lengthSquared = runX * runX + runY * runY;
    float along = 0.0f;
    if (lengthSquared > 1e-6f) {
        along = ((cursor.x - a.x) * runX + (cursor.y - a.y) * runY) / lengthSquared;
        along = along < 0.0f ? 0.0f : (along > 1.0f ? 1.0f : along);
    }
    const float dx = cursor.x - (a.x + runX * along);
    const float dy = cursor.y - (a.y + runY * along);
    return std::sqrt(dx * dx + dy * dy);
}

// A modeller aiming at a vertex is aiming precisely, so the point grab is
// tighter than the HUD's 28 px - which exists because a fighter at four
// kilometres is three pixels across and precision selection was the complaint.
constexpr float kGrabPixels = 12.0f;

// ⚑ Tighter than the point grab rather than looser, and that is deliberate: an
// edge is a long target, so a generous radius buys nothing along its length and
// costs everything across it - two edges of a box corner are a few pixels apart
// near the corner. Aim at the MIDDLE of the edge to disambiguate, which is what
// the panel says.
constexpr float kEdgeGrabPixels = 8.0f;

// ⚑ A face pick has NO grab radius, and that is the difference a ray makes: a
// triangle has an interior, so the cursor is either inside it or it is not.
// Points and edges need a radius because they have no area to be inside of.

} // namespace

void PointTool::refresh(const ForgeDoc& doc)
{
    const std::size_t previousCount = m_points.size();
    const std::size_t previousEdges = m_edges.size();
    m_points.clear();
    m_edges.clear();
    m_faces.clear();
    m_unavailable.clear();

    std::string error;
    // ⚑ ONE call for both halves. `forgeTopology` builds the mesh once and
    // dedups once, so the edges are numbered in the same vector the points are -
    // which is the entire reason it exists instead of `MeshAdjacency`, whose
    // edges index a differently-welded, later-renumbered vector. Two numberings
    // that agree only by accident is a bug this repo has already shipped.
    if (!assets::forgeTopology(doc, m_points, m_edges, m_faces, &error)) {
        m_unavailable = error;
        m_partBounds.clear();
        clearSelection();
        return;
    }
    // Stage N. Sized to the DOCUMENT's part count rather than to the parts that
    // emitted a face, so the index a face carries is the index the panel uses -
    // a part with no geometry gets an entry with `any` false rather than
    // shifting every part after it.
    forgePartBounds(m_points, m_faces, doc.parts.size(), m_partBounds);

    // ⚑ The selection survives a rebuild whose point COUNT is unchanged, which
    // is every rebuild a drag causes. Dropping it would mean the grab was lost
    // on the first frame of every drag - the mesh is rebuilt from the document
    // after each accepted edit, and that rebuild lands inside the drag.
    //
    // ⚑ The edge count is part of that test since E4b, because an edge drag is
    // the same continuous rebuild and a resize is not allowed to renumber it. A
    // move that DID change the topology has moved the thing being held.
    if (m_points.size() != previousCount || m_edges.size() != previousEdges) {
        clearSelection();
    }
    if (m_selected >= m_points.size()) {
        m_selected = kNone;
    }
    if (m_hover >= m_points.size()) {
        m_hover = kNone;
    }
    if (m_selectedEdge >= m_edges.size()) {
        m_selectedEdge = kNone;
    }
    if (m_hoverEdge >= m_edges.size()) {
        m_hoverEdge = kNone;
    }
    if (m_selectedFace >= m_faces.size()) {
        m_selectedFace = kNone;
        m_group.clear();
    }
    if (m_hoverFace >= m_faces.size()) {
        m_hoverFace = kNone;
        m_hoverGroup.clear();
    }
    if (m_hoverPart != kNoPart && m_hoverPart >= m_partBounds.size()) {
        m_hoverPart = kNoPart;
    }

    // ⚑ The extrude's re-selection, consumed here because this is the first
    // moment the raised face exists in a numbering anyone can name. Matched by
    // NORMAL first and centroid second: the walls the extrude raised stand at
    // right angles to the face, so requiring the plane excludes them by
    // construction rather than by hoping the distances come out right.
    if (m_reselect) {
        m_reselect = false;
        constexpr double kCoplanar = 0.9999619; // cos(0.5 degrees), as the flood uses
        std::size_t best = kNone;
        double nearest = 0.0;
        for (std::size_t i = 0; i < m_faces.size(); ++i) {
            const ForgeFace& face = m_faces[i];
            const assets::BuildPoint normal =
                core::normalize(core::cross(m_points[face.b].position - m_points[face.a].position,
                                            m_points[face.c].position - m_points[face.a].position));
            if (core::dot(normal, m_reselectNormal) < kCoplanar) {
                continue;
            }
            const assets::BuildPoint centre{
                (m_points[face.a].position.x + m_points[face.b].position.x +
                 m_points[face.c].position.x) /
                    3.0,
                (m_points[face.a].position.y + m_points[face.b].position.y +
                 m_points[face.c].position.y) /
                    3.0,
                (m_points[face.a].position.z + m_points[face.b].position.z +
                 m_points[face.c].position.z) /
                    3.0};
            const assets::BuildPoint gap = centre - m_reselectCentre;
            const double distance = core::dot(gap, gap);
            if (best == kNone || distance < nearest) {
                best = i;
                nearest = distance;
            }
        }
        if (best != kNone) {
            m_selectedFace = best;
            assets::forgeFaceGroup(m_points, m_faces, best, m_group);
        }
    }
}

// One place, because there are now three selections and forgetting one of them
// leaves a highlight alive in a mode nobody is looking at.
void PointTool::clearSelection()
{
    m_hover = kNone;
    m_selected = kNone;
    m_hoverEdge = kNone;
    m_selectedEdge = kNone;
    m_hoverFace = kNone;
    m_selectedFace = kNone;
    m_hoverPart = kNoPart;
    m_group.clear();
    m_hoverGroup.clear();
    m_dragging = false;
    m_refused = false;
}

void PointTool::close()
{
    m_points.clear();
    m_edges.clear();
    m_faces.clear();
    m_dragSet.clear();
    m_partBounds.clear();
    m_unavailable.clear();
    clearSelection();
    m_error.clear();
    m_note.clear();
    m_dropped = false;
    m_reselect = false;
}

void PointTool::setMode(Mode mode)
{
    if (mode == m_mode) {
        return;
    }
    m_mode = mode;
    clearSelection();
    m_error.clear();
    m_note.clear();
    m_dropped = false;
    m_reselect = false;
}

std::size_t PointTool::pickAt(const Viewport& viewport) const
{
    // ⚑ Projected FORWARDS to where each point was drawn, which is what
    // `sol/ui/pick.hpp` is for and the ruling that whole file is built on: the
    // point a person clicks is the point they were shown, by construction.
    // Inverting the projection against a depth nobody has is the alternative,
    // and it is the wrong one here for the same reason it was wrong in the HUD.
    std::vector<core::Vec2> screen;
    std::vector<std::size_t> candidates;
    screen.reserve(m_points.size());
    candidates.reserve(m_points.size());
    for (std::size_t i = 0; i < m_points.size(); ++i) {
        if (!m_points[i].movable()) {
            continue; // an unmovable point cannot be the answer to a click
        }
        const core::Vec3 cameraSpace =
            core::transformPoint(viewport.view, asVec3(m_points[i].position));
        const ui::ScreenPoint projected = ui::screenPoint(cameraSpace, viewport.center, viewport.focal);
        if (!projected.inFront) {
            continue;
        }
        screen.push_back(projected.position);
        candidates.push_back(i);
    }

    const std::size_t hit = ui::pickNearestPoint(screen, viewport.cursor, kGrabPixels);
    return hit == ui::kNoPick ? kNone : candidates[hit];
}

std::size_t PointTool::pickEdgeAt(const Viewport& viewport) const
{
    // ⚑ Both ends projected FORWARDS, the same ruling the point pick is built
    // on: the edge a person clicks is the edge they were shown. An edge with an
    // end behind the near plane is SKIPPED rather than clipped - the projection
    // mirrors such a point, so clipping it here would be a second expression of
    // the projection, and this file already refuses to keep two of those.
    std::size_t best = kNone;
    float bestDistance = kEdgeGrabPixels;
    for (std::size_t i = 0; i < m_edges.size(); ++i) {
        const ForgeEdge& edge = m_edges[i];
        if (edge.a >= m_points.size() || edge.b >= m_points.size()) {
            continue;
        }
        // An edge either of whose ends has no parametric answer cannot be
        // moved, so it cannot be the answer to a click - the same rule the
        // point pick applies, and for the same reason.
        if (!m_points[edge.a].movable() || !m_points[edge.b].movable()) {
            continue;
        }
        const ui::ScreenPoint a = ui::screenPoint(
            core::transformPoint(viewport.view, asVec3(m_points[edge.a].position)), viewport.center,
            viewport.focal);
        const ui::ScreenPoint b = ui::screenPoint(
            core::transformPoint(viewport.view, asVec3(m_points[edge.b].position)), viewport.center,
            viewport.focal);
        if (!a.inFront || !b.inFront) {
            continue;
        }
        const float distance = distanceToSegment(a.position, b.position, viewport.cursor);
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

std::size_t PointTool::pickRawFaceAt(const Viewport& viewport) const
{
    if (m_faces.empty()) {
        return kNone;
    }
    // ⚑ A RAY, not a projection - and `rayDirectionCamera` is the one place in
    // this programme that runs a projection backwards. It is allowed because a
    // pixel names a DIRECTION exactly; what `pick.hpp` forbids is inverting to
    // recover a position, which would need a depth nobody has. `ui.unit` pins
    // the round trip against `screenPoint` so neither sign is trusted alone.
    const core::Vec3 local =
        ui::rayDirectionCamera(viewport.cursor, viewport.center, viewport.focal);
    const core::Vec3 right = cameraRight(viewport.view);
    const core::Vec3 up = cameraUp(viewport.view);
    const core::Vec3 backward = cameraBackward(viewport.view);
    // local.z is exactly -1, so this is `u*right + v*up - backward`.
    const core::Vec3 direction{(local.x * right.x) + (local.y * up.x) + (local.z * backward.x),
                               (local.x * right.y) + (local.y * up.y) + (local.z * backward.y),
                               (local.x * right.z) + (local.y * up.z) + (local.z * backward.z)};
    const core::Vec3 eye = cameraEye(viewport.view);

    std::size_t face = 0;
    double distance = 0.0;
    if (!assets::forgePickFace(m_points, m_faces, {eye.x, eye.y, eye.z},
                               {direction.x, direction.y, direction.z}, face, distance)) {
        return kNone;
    }
    return face;
}

std::size_t PointTool::pickFaceAt(const Viewport& viewport, std::vector<std::uint32_t>& group) const
{
    group.clear();
    const std::size_t face = pickRawFaceAt(viewport);
    if (face == kNone) {
        return kNone;
    }
    // ⚑ WIDENED HERE, at pick time, and it has to be here. A box face is two
    // triangles and `forgeMovePoints` can express a whole face or nothing, so a
    // tool that carried the picked TRIANGLE forward would be refused on every
    // box in the repo. By the time the write set is built, "three corners" and
    // "an author who meant four" are indistinguishable.
    assets::forgeFaceGroup(m_points, m_faces, face, group);
    return face;
}

std::size_t PointTool::pickPartAt(const Viewport& viewport) const
{
    const std::size_t face = pickRawFaceAt(viewport);
    if (face == kNone) {
        return kNoPart;
    }
    // ⚑ NOT widened, and that is the whole difference from a face pick. E4d's
    // coplanar flood exists because `forgeMovePoints` must be handed a whole
    // face; a part pick already has its answer in the triangle the ray entered,
    // and `forgeFaceGroup` can cross a part boundary anyway (4 of `ship.forge`'s
    // 1,273 groups do), so widening first would make the answer worse as well as
    // slower.
    return forgePartOfFace(m_faces, face, m_partBounds.size());
}

void PointTool::gatherSelection()
{
    m_dragSet.clear();
    // Stage N moves nothing, so it has no drag set. Said here rather than left
    // to fall through the face branch, which would gather the last hovered
    // group and hand a mode that cannot drag something to drag.
    if (m_mode == Mode::Part) {
        return;
    }
    if (m_mode == Mode::Point) {
        if (m_selected != kNone && m_selected < m_points.size()) {
            m_dragSet.push_back(m_points[m_selected]);
        }
        return;
    }
    if (m_mode == Mode::Edge) {
        if (m_selectedEdge == kNone || m_selectedEdge >= m_edges.size()) {
            return;
        }
        const ForgeEdge& edge = m_edges[m_selectedEdge];
        if (edge.a >= m_points.size() || edge.b >= m_points.size()) {
            return;
        }
        m_dragSet.push_back(m_points[edge.a]);
        m_dragSet.push_back(m_points[edge.b]);
        return;
    }

    // ⚑ DEDUPLICATED BY INDEX, because the two triangles of a quad name their
    // shared pair twice. A set with a repeat in it is a doubled write, which is
    // the exact defect `forgeMovePoints` exists to prevent - handing it one from
    // this side would be the tool re-introducing it.
    std::vector<std::uint32_t> corners;
    for (const std::uint32_t index : m_group) {
        if (index >= m_faces.size()) {
            continue;
        }
        const ForgeFace& face = m_faces[index];
        const std::uint32_t three[3] = {face.a, face.b, face.c};
        for (const std::uint32_t corner : three) {
            if (corner < m_points.size() &&
                std::find(corners.begin(), corners.end(), corner) == corners.end()) {
                corners.push_back(corner);
            }
        }
    }
    m_dragSet.reserve(corners.size());
    for (const std::uint32_t corner : corners) {
        m_dragSet.push_back(m_points[corner]);
    }
}

bool PointTool::update(const Viewport& viewport, PartEditor& editor)
{
    if (m_points.empty() || !editor.isOpen()) {
        m_hover = kNone;
        return false;
    }

    // A drag that began in the viewport is not interrupted by the cursor
    // crossing the panel - the same rule main.cpp already applies to the
    // camera, and for the same reason.
    if (viewport.uiCaptured && !m_dragging) {
        m_hover = kNone;
        return false;
    }

    if (!m_dragging) {
        if (m_mode == Mode::Point) {
            m_hover = pickAt(viewport);
        } else if (m_mode == Mode::Edge) {
            m_hoverEdge = pickEdgeAt(viewport);
        } else if (m_mode == Mode::Face) {
            m_hoverFace = pickFaceAt(viewport, m_hoverGroup);
        } else {
            m_hoverPart = pickPartAt(viewport);
        }
    }

    // ⚑⚑ STAGE N's WHOLE PRESS, AND IT DELIBERATELY CLAIMS NOTHING. A part pick
    // starts no drag, so `m_dragging` stays false and `main.cpp` hands the rest
    // of the press to the orbit camera exactly as it does for a click that hit
    // nothing - which means LMB still spins the model in this mode, with no
    // modifier and no change to the arbitration. Selecting a part is not an
    // edit: no `beginEdit`, no rebuild, and the document is not dirtied.
    if (m_mode == Mode::Part) {
        if (viewport.leftPressed) {
            m_error.clear();
            m_note.clear();
            if (m_hoverPart != kNoPart) {
                editor.selectPart(m_hoverPart);
            }
        }
        return false;
    }

    if (viewport.leftPressed && !m_dragging) {
        m_error.clear();
        m_note.clear();
        m_dropped = false;
        core::Vec3 grabbed{};
        bool grabbedAnything = false;
        if (m_mode == Mode::Point) {
            m_selected = m_hover;
            if (m_selected != kNone) {
                grabbed = asVec3(m_points[m_selected].position);
                grabbedAnything = true;
            }
        } else if (m_mode == Mode::Edge) {
            m_selectedEdge = m_hoverEdge;
            if (m_selectedEdge != kNone) {
                // ⚑ The MIDPOINT, because an edge has two depths and the cursor
                // has one. Fixing on either end would make the far half of the
                // edge lag or lead the hand across the length of the drag.
                const ForgeEdge& edge = m_edges[m_selectedEdge];
                const core::Vec3 a = asVec3(m_points[edge.a].position);
                const core::Vec3 b = asVec3(m_points[edge.b].position);
                grabbed = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
                grabbedAnything = true;
            }
        } else {
            m_selectedFace = m_hoverFace;
            m_group = m_hoverGroup;
            if (m_selectedFace != kNone) {
                // The group's centroid, for the same reason an edge takes its
                // midpoint: one cursor, several depths, and the honest single
                // answer is the middle of what was grabbed.
                gatherSelection();
                core::Vec3 sum{};
                for (const ForgePoint& point : m_dragSet) {
                    sum.x += static_cast<float>(point.position.x);
                    sum.y += static_cast<float>(point.position.y);
                    sum.z += static_cast<float>(point.position.z);
                }
                const float count = m_dragSet.empty() ? 1.0f
                                                      : static_cast<float>(m_dragSet.size());
                grabbed = {sum.x / count, sum.y / count, sum.z / count};
                grabbedAnything = !m_dragSet.empty();
            }
        }
        if (grabbedAnything) {
            m_dragging = true;
            m_refused = false;
            // Depth along the camera's forward axis, fixed for the drag.
            m_dragDepth = -core::transformPoint(viewport.view, grabbed).z;
            // ⚑ One undo entry per DRAG, not per frame. A drag rebuilds the
            // document on every accepted edit, so pushing per edit would bury
            // the state before the drag under sixty identical ones.
            editor.beginEdit();
        }
        return false;
    }

    if (!viewport.leftDown) {
        m_dragging = false;
        m_refused = false;
        return false;
    }
    // ⚑ Refused, but still HELD. Returning here keeps `dragging()` true, which
    // is what stops main.cpp handing the rest of this press to the orbit
    // camera - and it is also what stops the refusal being recomputed and
    // re-reported sixty times a second.
    if (m_refused) {
        return false;
    }
    gatherSelection();
    if (m_dragSet.empty()) {
        m_refused = true;
        return false;
    }
    if (viewport.cursorDelta.x == 0.0f && viewport.cursorDelta.y == 0.0f) {
        return false; // nothing moved: do not dirty a document over a held button
    }

    // Pixels to metres at the grabbed point's own depth, so the point stays
    // under the cursor. Same expression as OrbitCamera::pan, at a different
    // depth - the camera pans at its target's, a drag moves at the point's.
    const float depth = m_dragDepth > 0.001f ? m_dragDepth : 0.001f;
    const float metresPerPixel =
        2.0f * depth * std::tan(viewport.verticalFov * 0.5f) / viewport.height;
    const core::Vec3 right = cameraRight(viewport.view);
    const core::Vec3 up = cameraUp(viewport.view);
    // Screen Y grows downward and world up is up, hence the sign on the second.
    core::Vec3 delta = (right * (viewport.cursorDelta.x * metresPerPixel)) -
                       (up * (viewport.cursorDelta.y * metresPerPixel));

    if (viewport.axisLock >= 0 && viewport.axisLock <= 2) {
        // Project onto the locked world axis: the hand still moves in the view
        // plane and the point moves only along the axis it was told to.
        const float along = viewport.axisLock == 0 ? delta.x
                            : viewport.axisLock == 1 ? delta.y
                                                     : delta.z;
        delta = {viewport.axisLock == 0 ? along : 0.0f, viewport.axisLock == 1 ? along : 0.0f,
                 viewport.axisLock == 2 ? along : 0.0f};
    }

    const assets::BuildPoint move{delta.x, delta.y, delta.z};
    // ⚑ ONE call for one point and for two, rather than a branch here. The set
    // move routes a single point straight back through E2's path, so a point
    // drag is bit-for-bit the code it always was and there is no second
    // arrangement of the same arithmetic to drift from the first.
    bool dropped = false;
    if (!editor.movePoints(m_dragSet, move, dropped, m_error)) {
        // The move was refused - an unmovable point, a part scaled flat, or a
        // box edge, which has no parametric answer at all. The press is KEPT
        // (see m_refused) so the camera does not inherit the rest of it.
        m_refused = true;
        return false;
    }
    m_dropped = m_dropped || dropped;
    return true;
}

void PointTool::drawMarkers(renderer::DebugDrawRenderer& lines, const Viewport& viewport,
                            std::size_t selectedPart) const
{
    if (m_points.empty()) {
        return;
    }
    // Sized off the camera distance so a marker is about the same size on
    // screen at 0.2 m and at 200 m - the four orders of magnitude the orbit
    // camera exists to cover.
    const float half = viewport.cameraDistance * 0.012f;

    constexpr core::Vec4 kMovable = {0.35f, 0.55f, 0.85f, 1.0f};
    constexpr core::Vec4 kHover = {0.95f, 0.80f, 0.30f, 1.0f};
    constexpr core::Vec4 kSelected = {0.30f, 0.95f, 0.45f, 1.0f};
    // Dimmer than a point marker on purpose: in edge mode this is a full
    // wireframe over a shaded mesh, and at the point colour it shouts.
    constexpr core::Vec4 kEdge = {0.26f, 0.38f, 0.58f, 1.0f};

    // ⚑⚑ STAGE N DRAWS IN ITS OWN MODE AND NOWHERE ELSE, WHICH IS ALSO THIS
    // STAGE'S FALSIFICATION TEST: modes 1/2/3 must come out pixel-identical to
    // before it existed. A part box is a big shape and the other three modes are
    // about small ones - a box round the whole hull while you are nudging one
    // vertex of it is furniture, not information.
    if (m_mode == Mode::Part) {
        // The selection first and the hover last, so the hover is the one that
        // survives if the buffer is spent - the opposite of the other modes'
        // ordering, and deliberately so: here the hover is what tracks the hand
        // and the selection is already named in the panel.
        if (selectedPart < m_partBounds.size() && m_partBounds[selectedPart].any) {
            addBox(lines, m_partBounds[selectedPart], viewport, kSelected);
        }
        if (m_hoverPart != kNoPart && m_hoverPart != selectedPart &&
            m_hoverPart < m_partBounds.size() && m_partBounds[m_hoverPart].any) {
            addBox(lines, m_partBounds[m_hoverPart], viewport, kHover);
        }
        return;
    }

    if (m_mode == Mode::Edge || m_mode == Mode::Face) {
        // ⚑ Pulled toward the camera by a hair, and the reason is the depth
        // test: `DebugDrawRenderer` tests depth and does not write it, so a line
        // lying exactly in the surface it borders z-fights along its whole
        // length. A point cross straddles the surface and half of it survives;
        // an edge has no half to spare. The bias is a FRACTION of the camera
        // distance rather than a constant, because the orbit camera covers four
        // orders of magnitude and a fixed epsilon is right at exactly one of
        // them.
        const core::Vec3 toward = cameraBackward(viewport.view);
        const float bias = viewport.cameraDistance * 0.0015f;
        const auto addEdge = [&](const ForgeEdge& edge, core::Vec4 color) {
            const core::Vec3 a = asVec3(m_points[edge.a].position);
            const core::Vec3 b = asVec3(m_points[edge.b].position);
            lines.line({a.x + toward.x * bias, a.y + toward.y * bias, a.z + toward.z * bias},
                       {b.x + toward.x * bias, b.y + toward.y * bias, b.z + toward.z * bias},
                       color);
        };

        std::size_t drawable = 0;
        for (const ForgeEdge& edge : m_edges) {
            if (m_points[edge.a].movable() && m_points[edge.b].movable()) {
                ++drawable;
            }
        }
        if (drawable <= kEdgeBudget) {
            for (std::size_t i = 0; i < m_edges.size(); ++i) {
                if (i == m_hoverEdge || i == m_selectedEdge) {
                    continue;
                }
                const ForgeEdge& edge = m_edges[i];
                if (!m_points[edge.a].movable() || !m_points[edge.b].movable()) {
                    continue;
                }
                addEdge(edge, kEdge);
            }
        }
        // A face group is drawn as the three sides of each of its triangles -
        // the quad's outline plus its diagonal - because the only primitive
        // this frame has is a line and a filled highlight is not available.
        const auto addGroup = [&](const std::vector<std::uint32_t>& group, core::Vec4 color) {
            for (const std::uint32_t index : group) {
                if (index >= m_faces.size()) {
                    continue;
                }
                const ForgeFace& face = m_faces[index];
                addEdge({face.a < face.b ? face.a : face.b, face.a < face.b ? face.b : face.a, 0},
                        color);
                addEdge({face.b < face.c ? face.b : face.c, face.b < face.c ? face.c : face.b, 0},
                        color);
                addEdge({face.a < face.c ? face.a : face.c, face.a < face.c ? face.c : face.a, 0},
                        color);
            }
        };

        // Drawn last so they are the ones that survive if anything is dropped -
        // the same order the point markers use, for the same reason.
        if (m_mode == Mode::Face) {
            if (m_hoverFace != kNone && m_hoverFace != m_selectedFace) {
                addGroup(m_hoverGroup, kHover);
            }
            if (m_selectedFace != kNone) {
                addGroup(m_group, kSelected);
                // A cross on every corner the drag will write. Read off the
                // group rather than off m_dragSet, which is only filled while a
                // press is live - the crosses have to survive the release. A
                // shared corner is crossed twice and the second lands exactly on
                // the first, which costs six lines and looks like one mark.
                for (const std::uint32_t index : m_group) {
                    if (index >= m_faces.size()) {
                        continue;
                    }
                    const ForgeFace& face = m_faces[index];
                    const std::uint32_t three[3] = {face.a, face.b, face.c};
                    for (const std::uint32_t corner : three) {
                        addCross(lines, asVec3(m_points[corner].position), half, kSelected);
                    }
                }
            }
            return;
        }
        if (m_hoverEdge != kNone && m_hoverEdge < m_edges.size() &&
            m_hoverEdge != m_selectedEdge) {
            addEdge(m_edges[m_hoverEdge], kHover);
        }
        if (m_selectedEdge != kNone && m_selectedEdge < m_edges.size()) {
            const ForgeEdge& edge = m_edges[m_selectedEdge];
            addEdge(edge, kSelected);
            // The two ends get crosses as well: an author needs to see WHICH
            // two points the drag is about to write, and a highlighted line
            // does not say where it stops.
            addCross(lines, asVec3(m_points[edge.a].position), half, kSelected);
            addCross(lines, asVec3(m_points[edge.b].position), half, kSelected);
        }
        return;
    }

    std::size_t movable = 0;
    for (const ForgePoint& point : m_points) {
        if (point.movable()) {
            ++movable;
        }
    }
    if (movable <= kMarkerBudget) {
        for (std::size_t i = 0; i < m_points.size(); ++i) {
            if (!m_points[i].movable() || i == m_hover || i == m_selected) {
                continue;
            }
            addCross(lines, asVec3(m_points[i].position), half, kMovable);
        }
    }
    // The hover and the selection are drawn whatever the budget, and drawn
    // last so they are the ones that survive if anything is dropped.
    if (m_hover != kNone && m_hover < m_points.size() && m_hover != m_selected) {
        addCross(lines, asVec3(m_points[m_hover].position), half * 1.5f, kHover);
    }
    if (m_selected != kNone && m_selected < m_points.size()) {
        addCross(lines, asVec3(m_points[m_selected].position), half * 2.0f, kSelected);
    }
}

bool PointTool::drawPanel(PartEditor& editor)
{
    const ForgeDoc& doc = editor.doc();
    bool changed = false;
    if (!m_unavailable.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
        ImGui::TextUnformatted(m_unavailable.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        return false;
    }
    if (m_points.empty()) {
        ImGui::TextDisabled("no points - open a .forge document");
        return false;
    }

    // ⚑ A radio rather than only a hotkey. The hotkey is what an author uses
    // after the first session; the radio is what says the mode EXISTS, and an
    // edge mode nobody can find is an edge mode nobody has.
    if (ImGui::RadioButton("points", m_mode == Mode::Point)) {
        setMode(Mode::Point);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("edges", m_mode == Mode::Edge)) {
        setMode(Mode::Edge);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("faces", m_mode == Mode::Face)) {
        setMode(Mode::Face);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("parts", m_mode == Mode::Part)) {
        setMode(Mode::Part);
    }

    // ⚑ Part mode returns before everything below it, and that is deliberate
    // rather than tidy. The rest of this panel is about DRAGGING a point -
    // movable counts, the bake instruction, the split and extrude buttons - and
    // none of it applies to a mode that only selects. Showing it greyed would be
    // four lines of scarce column saying "not this mode".
    if (m_mode == Mode::Part) {
        std::size_t withGeometry = 0;
        for (const PartBounds& bounds : m_partBounds) {
            withGeometry += bounds.any ? 1u : 0u;
        }
        const auto idOf = [&doc](std::size_t part) -> const char* {
            return part < doc.parts.size() ? doc.parts[part].id.c_str() : "-";
        };
        ImGui::Text("parts          %zu", doc.parts.size());
        // ⚑ Said out loud, because a part with no triangles cannot be clicked
        // and "I can see it in the list and cannot pick it" needs a reason on
        // screen rather than a shrug. `gate_membrane.forge` fans 32 degenerate
        // triangles that `forgeTopology` drops.
        if (withGeometry != doc.parts.size()) {
            ImGui::Text("pickable       %zu", withGeometry);
        }
        ImGui::Text("hover          %s", m_hoverPart == kNoPart ? "-" : idOf(m_hoverPart));
        ImGui::Text("selected       %s", idOf(editor.selectedPart()));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("click a part to select it in Parts; drag to orbit as usual.");
        ImGui::PopTextWrapPos();
        return changed;
    }

    std::size_t movable = 0;
    for (const ForgePoint& point : m_points) {
        if (point.movable()) {
            ++movable;
        }
    }
    ImGui::Text("points         %zu", m_points.size());
    ImGui::Text("movable        %zu", movable);
    if (m_mode == Mode::Edge || m_mode == Mode::Face) {
        std::size_t drawable = 0;
        for (const ForgeEdge& edge : m_edges) {
            if (m_points[edge.a].movable() && m_points[edge.b].movable()) {
                ++drawable;
            }
        }
        ImGui::Text("edges          %zu", m_edges.size());
        if (m_mode == Mode::Face) {
            ImGui::Text("faces          %zu", m_faces.size());
        } else {
            ImGui::Text("movable        %zu", drawable);
        }
        if (drawable > kEdgeBudget) {
            ImGui::TextDisabled("over %zu edges: only the hovered and selected are drawn",
                                kEdgeBudget);
        }
    }

    if (movable == 0) {
        // ⚑ The honest message, and it names the reason rather than the
        // symptom. It got SHORTER at E2: a box corner used to be the example of
        // something with no answer and now it is the commonest thing that has
        // one, so what is left here is genuinely only the swept surfaces.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("nothing here has a parametric answer to a dragged point - a torus "
                            "ring vertex is two segment indices and there is no number behind it. "
                            "Select its part in Parts and press \"bake\": that turns the geometry "
                            "into authored vertices without changing what is drawn.");
        ImGui::PopTextWrapPos();
    } else if (movable < m_points.size()) {
        // ⚑ Names the button rather than the concept. E1 and E2 both said "bake
        // its part first" at a tool that had no way to do it; E3 is what makes
        // that an instruction instead of a deferral.
        ImGui::TextDisabled("%zu point(s) need their part baked first", m_points.size() - movable);
    }
    if (movable > kMarkerBudget) {
        ImGui::TextDisabled("over %zu points: only the hovered and selected are marked",
                            kMarkerBudget);
    }

    // ⚑⚑ THE TWO TOPOLOGY EDITS (E5c and E5d), EACH ON THE SELECTION ITS MODE
    // ALREADY HAS. A split takes the selected edge; an extrude takes the
    // selected face GROUP, which is the widening E4d had to do at pick time -
    // three corners of four is refused by the engine, and by the time a write
    // set is built "three corners" and "an author who meant four" cannot be told
    // apart.
    if (m_mode == Mode::Edge && m_selectedEdge != kNone && m_selectedEdge < m_edges.size()) {
        if (ImGui::Button("split edge")) {
            const ForgeEdge& edge = m_edges[m_selectedEdge];
            std::string error;
            if (editor.splitEdge(m_points, m_faces, edge.a, edge.b, error)) {
                m_error.clear();
                // The edge that was selected does not exist any more - it is two
                // now - so there is nothing honest to keep highlighted.
                m_note = "split: a point was added at the middle, and every triangle standing on "
                         "that edge became two. Pick either half.";
                changed = true;
            } else {
                m_error = error;
            }
        }
    }
    if (m_mode == Mode::Face && m_selectedFace != kNone && !m_group.empty()) {
        if (ImGui::Button("extrude")) {
            // Where the raised face should end up, recorded BEFORE the edit so
            // the rebuild can be matched against it - see m_reselect.
            const ForgeFace& seed = m_faces[m_selectedFace];
            const assets::BuildPoint normal = core::normalize(
                core::cross(m_points[seed.b].position - m_points[seed.a].position,
                            m_points[seed.c].position - m_points[seed.a].position));
            assets::BuildPoint centre{0.0, 0.0, 0.0};
            std::size_t corners = 0;
            for (const std::uint32_t index : m_group) {
                const ForgeFace& face = m_faces[index];
                for (const std::uint32_t corner : {face.a, face.b, face.c}) {
                    centre = centre + m_points[corner].position;
                    ++corners;
                }
            }
            centre = {centre.x / static_cast<double>(corners),
                      centre.y / static_cast<double>(corners),
                      centre.z / static_cast<double>(corners)};

            double offset = 0.0;
            std::string error;
            if (editor.extrudeFaces(m_faces, m_group, offset, error)) {
                m_error.clear();
                char note[192];
                std::snprintf(note, sizeof(note),
                              "extruded %zu triangle(s) by %.4f m along the face's own normal - "
                              "drag it to place it.",
                              m_group.size(), offset);
                m_note = note;
                m_reselect = true;
                m_reselectNormal = normal;
                m_reselectCentre = centre + assets::BuildPoint{normal.x * offset,
                                                              normal.y * offset,
                                                              normal.z * offset};
                changed = true;
            } else {
                m_error = error;
            }
        }
        ImGui::SameLine();
        // ⚑ Said before the press rather than after it, because the offset is
        // NOT zero and cannot be: a face duplicated in place welds straight back
        // into the one it came from, since a point's identity here is its
        // position. An author who expected "extrude then move from flush" needs
        // to know that before they wonder what moved.
        ImGui::TextDisabled("(raises it off its own plane first)");
    }

    if (!m_error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.95f, 0.45f, 0.35f, 1.0f}, "%s", m_error.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!m_note.empty() && m_error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.55f, 0.85f, 0.55f, 1.0f}, "%s", m_note.c_str());
        ImGui::PopTextWrapPos();
    }

    // ⚑ Sticky for the drag rather than per frame: this is a consequence the
    // author cannot avoid, so it is the E2 precedent exactly - said in the
    // panel, because a face is exact only along its own normal and a pull off
    // that normal has nowhere to go. It is NOT the refusal above, which is a
    // wrong answer the tool declines to give.
    if (m_dropped) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.95f, 0.80f, 0.30f, 1.0f},
                           "part of that drag had no expressible answer and was dropped: a box "
                           "face moves only along its own normal, because the only other thing "
                           "the numbers can do is slide the whole box.");
        ImGui::PopTextWrapPos();
    }

    // One point's authored values, listed. Shared by the point selection and by
    // each end of an edge, because "written by" means the same thing in both
    // and two copies of it would answer differently the first time either moved.
    const auto listWrites = [&doc](const ForgePoint& point, bool& resizes, bool& reAims) {
        for (const ForgePointWrite& write : point.writes) {
            if (write.part >= doc.parts.size()) {
                continue;
            }
            const char* const id = doc.parts[write.part].id.c_str();
            switch (write.kind) {
            case assets::ForgeWriteKind::Vertex:
                ImGui::TextDisabled("  %s.%s", id, write.param.c_str());
                break;
            case assets::ForgeWriteKind::BakedVertex:
                ImGui::TextDisabled("  %s.vertices[%u]", id, write.element);
                break;
            case assets::ForgeWriteKind::BoxCorner:
                // ⚑ The corner is named as its three signs, because that is
                // what tells an author which corner is about to be PINNED - the
                // opposite one, and there is nowhere else to read that off.
                ImGui::TextDisabled("  %s.center+size   %cx%cy%cz", id,
                                    (write.element & 1u) != 0 ? '+' : '-',
                                    (write.element & 2u) != 0 ? '+' : '-',
                                    (write.element & 4u) != 0 ? '+' : '-');
                resizes = true;
                break;
            case assets::ForgeWriteKind::BeamEnd:
                ImGui::TextDisabled("  %s.%s", id, write.param.c_str());
                reAims = true;
                break;
            }
        }
    };

    ImGui::Separator();
    if (m_mode == Mode::Face) {
        if (m_selectedFace == kNone || m_group.empty()) {
            ImGui::TextDisabled("no face selected");
        } else {
            // ⚑ The two numbers that say the widening HAPPENED. A box face reads
            // "2 triangles, 4 corners" where the ray entered one triangle of
            // three - and if it ever reads 1 and 3 on a box, the flood is broken
            // and the drag is about to be refused.
            std::vector<std::uint32_t> corners;
            for (const std::uint32_t index : m_group) {
                if (index >= m_faces.size()) {
                    continue;
                }
                const ForgeFace& face = m_faces[index];
                const std::uint32_t three[3] = {face.a, face.b, face.c};
                for (const std::uint32_t corner : three) {
                    if (std::find(corners.begin(), corners.end(), corner) == corners.end()) {
                        corners.push_back(corner);
                    }
                }
            }
            ImGui::Text("triangles      %zu", m_group.size());
            ImGui::Text("corners        %zu", corners.size());

            bool resizes = false;
            bool reAims = false;
            bool anyUnmovable = false;
            for (const std::uint32_t corner : corners) {
                const ForgePoint& point = m_points[corner];
                anyUnmovable = anyUnmovable || !point.movable();
                ImGui::Text("at   %8.4f %8.4f %8.4f", point.position.x, point.position.y,
                            point.position.z);
                listWrites(point, resizes, reAims);
            }

            ImGui::PushTextWrapPos(0.0f);
            if (anyUnmovable) {
                ImGui::TextColored({0.95f, 0.80f, 0.30f, 1.0f},
                                   "a corner of this face has no parametric answer - select its "
                                   "part in Parts and press \"bake\" first.");
            } else if (resizes) {
                // ⚑ The case that PAYS OUT, and it is worth saying so rather
                // than only warning. A box face is the one class-(2) selection
                // that is exact: E2's split on the axis the corners agree
                // about, with the opposite face pinned.
                ImGui::TextDisabled("this face RESIZES its box along its own normal and pins the "
                                    "face opposite. A pull off that normal has no answer and is "
                                    "dropped - a box can only slide whole.");
            }
            if (reAims) {
                ImGui::TextDisabled("a beam's corners come from its axis, so this drag RE-AIMS "
                                    "the end they stand at.");
            }
            ImGui::PopTextWrapPos();
        }
    } else if (m_mode == Mode::Edge) {
        if (m_selectedEdge == kNone || m_selectedEdge >= m_edges.size()) {
            ImGui::TextDisabled("no edge selected");
        } else {
            const ForgeEdge& edge = m_edges[m_selectedEdge];
            const ForgePoint& a = m_points[edge.a];
            const ForgePoint& b = m_points[edge.b];
            const double dx = b.position.x - a.position.x;
            const double dy = b.position.y - a.position.y;
            const double dz = b.position.z - a.position.z;
            ImGui::Text("from %8.4f %8.4f %8.4f", a.position.x, a.position.y, a.position.z);
            ImGui::Text("to   %8.4f %8.4f %8.4f", b.position.x, b.position.y, b.position.z);
            ImGui::Text("length %8.4f m", std::sqrt(dx * dx + dy * dy + dz * dz));
            // ⚑ A border edge is a hole in the surface, and that is worth
            // saying where it can be seen. `geometry.unit` asserts every
            // committed solid has none, so one here means an edit opened one.
            if (edge.faceCount == 1) {
                ImGui::TextColored({0.95f, 0.80f, 0.30f, 1.0f},
                                   "border edge - one face, so the surface is open here");
            } else {
                ImGui::TextDisabled("faces  %u", edge.faceCount);
            }

            bool resizes = false;
            bool reAims = false;
            ImGui::Text("end A written by %zu:", a.writes.size());
            listWrites(a, resizes, reAims);
            ImGui::Text("end B written by %zu:", b.writes.size());
            listWrites(b, resizes, reAims);

            ImGui::PushTextWrapPos(0.0f);
            if (resizes) {
                // ⚑ Said BEFORE it happens rather than only when the drag is
                // refused, which is the difference between a tool that has a
                // rule and a tool that appears broken. The bake is one button,
                // in Parts, and E3 built it.
                ImGui::TextColored({0.95f, 0.80f, 0.30f, 1.0f},
                                   "a box has no shear, so an edge of one cannot be moved: "
                                   "select its part in Parts and press \"bake\" first, which "
                                   "makes its corners authored numbers without changing what "
                                   "is drawn.");
            }
            if (reAims) {
                ImGui::TextDisabled("a beam's corners come from its axis, so this drag RE-AIMS "
                                    "the end they stand at, and the far end swings by up to "
                                    "half the section's diagonal.");
            }
            ImGui::PopTextWrapPos();
        }
    } else if (m_selected == kNone || m_selected >= m_points.size()) {
        ImGui::TextDisabled("no point selected");
    } else {
        const ForgePoint& point = m_points[m_selected];
        ImGui::Text("at   %8.4f %8.4f %8.4f", point.position.x, point.position.y, point.position.z);
        // ⚑ The count that is the whole argument for the stage. A person
        // editing this file by hand has to find all of these and get every one
        // right; `ship.forge`'s own header says four where the answer is five.
        ImGui::Text("written by %zu part(s):", point.writes.size());
        bool resizes = false;
        bool reAims = false;
        listWrites(point, resizes, reAims);

        // ⚑ Class (2) said out loud rather than left to be discovered. A drag
        // on a corner that is not a parameter cannot move only that corner, and
        // "I moved this and something else moved" is indistinguishable from a
        // bug when nobody wrote down that it was the answer.
        if (resizes || reAims) {
            ImGui::PushTextWrapPos(0.0f);
            if (resizes) {
                ImGui::TextDisabled("a box corner is not a number in the file, so this drag "
                                    "RESIZES the box and pins the corner opposite.");
            }
            if (reAims) {
                ImGui::TextDisabled("a beam's corners come from its axis, so this drag RE-AIMS "
                                    "that end: its other three corners come with it, and the far "
                                    "end swings by up to half the section's diagonal.");
            }
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::Separator();
    if (m_mode == Mode::Face) {
        ImGui::TextDisabled("click anywhere on a face to select the whole of it");
        ImGui::TextDisabled("a box face moves along its own normal - hold that axis");
    } else if (m_mode == Mode::Edge) {
        ImGui::TextDisabled("click an edge to select, drag to move both its ends");
        ImGui::TextDisabled("aim at the middle of an edge, not at a corner");
    } else {
        ImGui::TextDisabled("click a marker to select, drag to move");
    }
    ImGui::TextDisabled("1, 2 and 3 switch points, edges and faces");
    ImGui::TextDisabled("hold X, Y or Z to lock the drag to an axis");
    ImGui::TextDisabled("ctrl+Z undoes the whole drag");
    return changed;
}

} // namespace forge
