#include "point_tool.hpp"

#include "part_editor.hpp"

#include "sol/ui/pick.hpp"

#include <imgui.h>

#include <cmath>

namespace forge {

using namespace sol;
using assets::ForgeDoc;
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

} // namespace

void PointTool::refresh(const ForgeDoc& doc)
{
    const std::size_t previousCount = m_points.size();
    m_points.clear();
    m_unavailable.clear();

    std::string error;
    if (!assets::forgePoints(doc, m_points, &error)) {
        m_unavailable = error;
        m_hover = kNone;
        m_selected = kNone;
        m_dragging = false;
        return;
    }

    // ⚑ The selection survives a rebuild whose point COUNT is unchanged, which
    // is every rebuild a drag causes. Dropping it would mean the grab was lost
    // on the first frame of every drag - the mesh is rebuilt from the document
    // after each accepted edit, and that rebuild lands inside the drag.
    if (m_points.size() != previousCount) {
        m_hover = kNone;
        m_selected = kNone;
        m_dragging = false;
    }
    if (m_selected >= m_points.size()) {
        m_selected = kNone;
    }
    if (m_hover >= m_points.size()) {
        m_hover = kNone;
    }
}

void PointTool::close()
{
    m_points.clear();
    m_unavailable.clear();
    m_hover = kNone;
    m_selected = kNone;
    m_dragging = false;
    m_error.clear();
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

    // A modeller aiming at a vertex is aiming precisely, so the grab is tighter
    // than the HUD's 28 px - which exists because a fighter at four kilometres
    // is three pixels across and precision selection was the complaint.
    constexpr float kGrabPixels = 12.0f;
    const std::size_t hit = ui::pickNearestPoint(screen, viewport.cursor, kGrabPixels);
    return hit == ui::kNoPick ? kNone : candidates[hit];
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
        m_hover = pickAt(viewport);
    }

    if (viewport.leftPressed && !m_dragging) {
        m_selected = m_hover;
        m_error.clear();
        if (m_selected != kNone) {
            m_dragging = true;
            // Depth along the camera's forward axis, fixed for the drag.
            const core::Vec3 cameraSpace =
                core::transformPoint(viewport.view, asVec3(m_points[m_selected].position));
            m_dragDepth = -cameraSpace.z;
            // ⚑ One undo entry per DRAG, not per frame. A drag rebuilds the
            // document on every accepted edit, so pushing per edit would bury
            // the state before the drag under sixty identical ones.
            editor.beginEdit();
        }
        return false;
    }

    if (!viewport.leftDown) {
        m_dragging = false;
        return false;
    }
    if (m_selected == kNone || m_selected >= m_points.size()) {
        m_dragging = false;
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
    if (!editor.movePoint(m_points[m_selected], move, m_error)) {
        // The move was refused - an unmovable point, or a part scaled flat.
        // Ending the drag is what stops it being reported once per frame.
        m_dragging = false;
        return false;
    }
    return true;
}

void PointTool::drawMarkers(renderer::DebugDrawRenderer& lines, const Viewport& viewport) const
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

void PointTool::drawPanel(const ForgeDoc& doc) const
{
    if (!m_unavailable.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
        ImGui::TextUnformatted(m_unavailable.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        return;
    }
    if (m_points.empty()) {
        ImGui::TextDisabled("no points - open a .forge document");
        return;
    }

    std::size_t movable = 0;
    for (const ForgePoint& point : m_points) {
        if (point.movable()) {
            ++movable;
        }
    }
    ImGui::Text("points         %zu", m_points.size());
    ImGui::Text("movable        %zu", movable);

    if (movable == 0) {
        // ⚑ The honest message, and it names the reason rather than the
        // symptom. It got SHORTER at E2: a box corner used to be the example of
        // something with no answer and now it is the commonest thing that has
        // one, so what is left here is genuinely only the swept surfaces.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("nothing here has a parametric answer to a dragged point - a torus "
                            "ring vertex is two segment indices and there is no number behind it. "
                            "Baking a part is what makes its vertices authored numbers.");
        ImGui::PopTextWrapPos();
    } else if (movable < m_points.size()) {
        ImGui::TextDisabled("%zu point(s) need a bake first", m_points.size() - movable);
    }
    if (movable > kMarkerBudget) {
        ImGui::TextDisabled("over %zu points: only the hovered and selected are marked",
                            kMarkerBudget);
    }

    if (!m_error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.95f, 0.45f, 0.35f, 1.0f}, "%s", m_error.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Separator();
    if (m_selected == kNone || m_selected >= m_points.size()) {
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
    ImGui::TextDisabled("click a marker to select, drag to move");
    ImGui::TextDisabled("hold X, Y or Z to lock the drag to an axis");
    ImGui::TextDisabled("ctrl+Z undoes the whole drag");
}

} // namespace forge
