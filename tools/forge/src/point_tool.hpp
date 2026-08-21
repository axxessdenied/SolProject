#pragma once

// Direct point editing in the viewport (engine plan Phase 9 stages E1 and E2) -
// the stage where the Forge stops being a parameter panel and becomes a
// modeller.
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
// ⚑ E2 ADDED THE CLASS WHERE THE CORNER IS NOT A NUMBER AT ALL - a box corner,
// which solves back to `center` and `size` with the opposite corner pinned, and
// a beam corner, which solves back to the end it stands at. That makes the drag
// a RESIZE or a RE-AIM rather than a move, so more than the grabbed point has
// to shift, and the panel says which before it happens. Everything the tool
// itself does is unchanged: this file still only projects, hovers and drags.
//
// ⚑ E4b ADDED THE EDGE, AND THE TOOL TAKES A SET WHERE IT USED TO TAKE A POINT.
// An edge is two points moved by one delta, and that is emphatically NOT the
// point move called twice: a box puts one `center`+`size` pair behind all eight
// of its corners, so a loop applies the resize once per grabbed point and the
// edge travels twice as far as the hand. `forgeMovePoints` collapses the writes
// across the set; this file only decides WHICH points are in it.
//
// ⚑ What lives HERE rather than in the engine is only what needs a screen: the
// projection, the hover, the drag and the markers. Everything a test could
// assert without a device is in `sol::assets`, which is the same line stage B
// drew and the reason the geometry suite can prove this at all.

#include "sol/assets/forge_doc.hpp"
#include "sol/core/math/math.hpp"
#include "sol/renderer/debug_draw_renderer.hpp"

#include <cstddef>
#include <span>
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

    // ⚑ THE EDGE BUDGET, AND IT IS THE FIRST TIME THAT CAP HAS BEEN WITHIN ONE
    // ORDER OF MAGNITUDE OF FIRING. A point cross is three segments - six
    // vertices - and the largest asset welds to 552 points, so 600 was never
    // reached. An edge is one segment, but there are an order of magnitude more
    // of them: `station.forge` is 552 welded points and 1068 triangles, so for a
    // closed surface E = 3F/2 = 1602 edges = 3,204 vertices, against about 270
    // for the grid and the reference boxes. Euler agrees exactly - nine boxes at
    // chi=2 and one torus at chi=0 give chi=18, and 552 - 1602 + 1068 = 18.
    //
    // ⚑ The two budgets do not ADD, because the two modes do not draw together:
    // edge mode draws no point crosses beyond the ends of what is selected. That
    // is why 2400 edges is safe rather than merely optimistic - 4,800 vertices
    // plus the furniture leaves about 3,100 spare.
    static constexpr std::size_t kEdgeBudget = 2400;

    // What a click selects. ONE element at a time - multi-selection is E5's
    // problem, and E1 took one point before this stage took a set.
    enum class Mode
    {
        Point,
        Edge,
        Face,
    };

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

    // ⚑ Clears BOTH selections rather than only the one being left. Keeping a
    // highlight alive in the mode you cannot see is how "I clicked and nothing
    // happened" gets written down as a defect.
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const { return m_mode; }

    // Hover, pick and drag. Returns true when the document changed and the
    // caller must rebuild the mesh.
    [[nodiscard]] bool update(const Viewport& viewport, PartEditor& editor);

    void drawMarkers(sol::renderer::DebugDrawRenderer& lines, const Viewport& viewport) const;
    // The panel section. Draws nothing that edits the DOCUMENT - every edit is
    // a drag - but it is no longer const, because the mode radio is a control
    // and a mode is what the next click means.
    void drawPanel(const sol::assets::ForgeDoc& doc);

    // ⚑⚑ TRUE FOR THE WHOLE BUTTON PRESS, INCLUDING AFTER A REFUSAL, AND THAT
    // IS WHAT THE E4b DRIVE FOUND. `main.cpp` hands the camera any press the
    // point tool does not claim, and it re-decides the moment this goes false -
    // so a drag that was refused on its first moving frame handed the REMAINING
    // fifteen frames to the orbit camera and swung the model about thirty
    // degrees. The author reads a refusal message and a spinning view at once,
    // which is indistinguishable from the tool doing something random.
    //
    // ⚑ It was latent from E2, where the refusal path is the same, and E2's own
    // check missed it because a one-step drag delivers one enormous delta and
    // has NO remaining frames to hand over. A guard tested with a single frame
    // cannot see what happens to the frames after it.
    [[nodiscard]] bool dragging() const { return m_dragging; }

private:
    // Screen position of a point, and whether it is in front of the camera.
    [[nodiscard]] std::size_t pickAt(const Viewport& viewport) const;
    // Nearest edge to the cursor by screen-space point-to-segment distance.
    [[nodiscard]] std::size_t pickEdgeAt(const Viewport& viewport) const;
    // The face the cursor's ray enters first, WIDENED to its coplanar group -
    // see forgeFaceGroup for why the widening cannot wait until the write.
    // Fills m_group; returns the seed face or kNone.
    [[nodiscard]] std::size_t pickFaceAt(const Viewport& viewport,
                                         std::vector<std::uint32_t>& group) const;
    // Fills m_dragSet with the points the current selection moves - one in
    // Point mode, two in Edge mode, the group's corners in Face mode. Empty
    // when nothing is selected.
    void gatherSelection();
    // Drops every hover and every selection. One place, because there are
    // three of each now and forgetting one leaves a highlight alive in a mode
    // nobody is looking at.
    void clearSelection();

    std::vector<sol::assets::ForgePoint> m_points;
    // ⚑ From the SAME traversal as the points, which is the whole reason
    // `forgeTopology` exists rather than a second call. Deriving the edges from
    // a second weld is the two-implementations trap this programme has paid for
    // twice, and `MeshAdjacency` numbers a different vector entirely.
    std::vector<sol::assets::ForgeEdge> m_edges;
    std::vector<sol::assets::ForgeFace> m_faces;
    // The drag's set, rebuilt each frame it is needed. A member rather than a
    // local so a drag does not reallocate sixty times a second.
    std::vector<sol::assets::ForgePoint> m_dragSet;
    // The selected and hovered face GROUPS, as face indices. Held rather than
    // recomputed per frame because the flood is a search and the hover runs
    // every frame the cursor moves.
    std::vector<std::uint32_t> m_group;
    std::vector<std::uint32_t> m_hoverGroup;
    // Why there are no points, when there are none - a `[build]` post-pass or
    // a document that does not build. Said out loud rather than left as an
    // empty panel, because "nothing happens when I click" is not a diagnosis.
    std::string m_unavailable;

    Mode m_mode = Mode::Point;
    std::size_t m_hover = kNone;
    std::size_t m_selected = kNone;
    // Into m_edges. Separate from m_selected because the two number different
    // vectors, and one index meaning two things is a bug this repo has shipped.
    std::size_t m_hoverEdge = kNone;
    std::size_t m_selectedEdge = kNone;
    // Into m_faces: the SEED of each group, which is the triangle the ray
    // actually entered. The group itself is in m_group / m_hoverGroup.
    std::size_t m_hoverFace = kNone;
    std::size_t m_selectedFace = kNone;
    bool m_dragging = false;
    // The move was refused and this press will not be retried - but the press
    // is still HELD, so the tool keeps it away from the camera until release.
    // Separate from m_dragging for exactly that reason: one says who owns the
    // button, the other says whether there is any point trying again.
    bool m_refused = false;
    // ⚑ Fixed at the moment the drag starts. Recomputing it per frame makes a
    // point accelerate as it moves toward the camera and stall as it moves
    // away, which reads as the tool fighting the hand. For an edge it is the
    // MIDPOINT's depth: the two ends are at different distances and there is
    // one cursor, so the honest single answer is the middle of what was grabbed.
    float m_dragDepth = 1.0f;
    std::string m_error;
    // ⚑ Set when the last accepted move DISCARDED a component - a box face
    // pulled off its own normal. Sticky for the drag rather than per frame,
    // because a message that flickers with the mouse is a message nobody reads.
    bool m_dropped = false;
};

} // namespace forge
