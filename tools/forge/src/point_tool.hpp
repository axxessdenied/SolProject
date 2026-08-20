#pragma once

// Direct point editing in the viewport (engine plan Phase 9 stage E1) - the
// stage where the Forge stops being a parameter panel and becomes a modeller.
//
// ⚑ It does NOT edit geometry. It edits the AUTHORED NUMBERS behind geometry,
// and the difference is the whole finding this stage was spec'd on: a
// `flat_triangle`'s `p0`/`p1`/`p2` ARE its three corners, so dragging one of
// them writes three numbers into a TOML file and the part stays parametric,
// diffable and editable. The engine half - `sol::assets::forgePoints` and
// `forgeMovePoint` - resolves a picked point to every authored value standing
// at it and writes all of them at once. `ship.forge`'s front corner is five
// parts under three parameter names, and writing four of them opens a seam.
//
// ⚑ What lives HERE rather than in the engine is only what needs a screen: the
// projection, the hover, the drag and the markers. Everything a test could
// assert without a device is in `sol::assets`, which is the same line stage B
// drew and the reason the geometry suite can prove this at all.

#include "sol/assets/forge_doc.hpp"
#include "sol/core/math/math.hpp"
#include "sol/renderer/debug_draw_renderer.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace forge {

class PartEditor;

class PointTool
{
public:
    // Nothing is selected, and nothing can be.
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // ⚑ The marker budget, and it is a real limit rather than a tidy number.
    // `DebugDrawRenderer` holds 8192 vertices and `line()` returns SILENTLY
    // when it is full, so a marker on every point of the baked asteroid would
    // want more lines than exist and lose them without a word - the failure
    // this repo keeps meeting from the other end. Past this many movable
    // points the tool marks only what is hovered or selected, and says so.
    static constexpr std::size_t kMarkerBudget = 600;

    struct Viewport
    {
        sol::core::Vec2 cursor{};      // window pixels
        sol::core::Vec2 cursorDelta{}; // this frame's mouse movement, pixels
        sol::core::Vec2 center{};      // window centre, pixels
        float focal = 1.0f;            // pick.hpp's focalLength for this window
        float height = 1.0f;           // viewport height in pixels
        float verticalFov = 1.0f;      // radians
        float cameraDistance = 1.0f;   // for sizing the markers
        sol::core::Mat4 view = sol::core::Mat4::identity();
        bool leftPressed = false; // went down this frame
        bool leftDown = false;
        bool uiCaptured = false; // ImGui wants the mouse: the viewport gets nothing
        // -1 free in the view plane, 0/1/2 to lock the drag to world X/Y/Z.
        int axisLock = -1;
    };

    // Recomputed from the document after every rebuild. Keeps the selection
    // when the point count is unchanged, because a drag rebuilds continuously
    // and losing the grab on the first frame of it would make the tool unusable.
    void refresh(const sol::assets::ForgeDoc& doc);
    void close();

    // Hover, pick and drag. Returns true when the document changed and the
    // caller must rebuild the mesh.
    [[nodiscard]] bool update(const Viewport& viewport, PartEditor& editor);

    void drawMarkers(sol::renderer::DebugDrawRenderer& lines, const Viewport& viewport) const;
    // The panel section. Draws nothing that edits: every edit is a drag.
    void drawPanel(const sol::assets::ForgeDoc& doc) const;

    [[nodiscard]] bool dragging() const { return m_dragging; }

private:
    // Screen position of a point, and whether it is in front of the camera.
    [[nodiscard]] std::size_t pickAt(const Viewport& viewport) const;

    std::vector<sol::assets::ForgePoint> m_points;
    // Why there are no points, when there are none - a `[build]` post-pass or
    // a document that does not build. Said out loud rather than left as an
    // empty panel, because "nothing happens when I click" is not a diagnosis.
    std::string m_unavailable;

    std::size_t m_hover = kNone;
    std::size_t m_selected = kNone;
    bool m_dragging = false;
    // ⚑ Fixed at the moment the drag starts. Recomputing it per frame makes a
    // point accelerate as it moves toward the camera and stall as it moves
    // away, which reads as the tool fighting the hand.
    float m_dragDepth = 1.0f;
    std::string m_error;
};

} // namespace forge
