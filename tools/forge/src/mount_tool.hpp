#pragma once

// Placing a hull's mounts on the hull, in 3D (engine plan Phase 31 stage D).
//
// ⚑⚑⚑ IT IS THE SECOND TOOL IN THIS VIEWPORT AND THE FIRST THAT DOES NOT EDIT
// THE MESH. `PointTool` drags the numbers a `.forge` document is built from;
// this drags a number in `ships.toml` while the mesh underneath stands still.
// The two therefore share the camera, the projection and the arbitration for
// the left button (`viewport_pick.hpp`) and nothing else - and they are
// MUTUALLY EXCLUSIVE by design rather than by layering. A click that might move
// a vertex or might move a mount depending on which happened to be nearer is a
// tool you cannot trust, and it would edit whichever of two documents the
// author was not thinking about.
//
// ⚑⚑ WHAT IT IS FOR, IN ONE SENTENCE, AND IT IS A REAL DEFECT RATHER THAN A
// CONVENIENCE. Stage A2 authored every mount position by hand into a text file
// while nothing read them, and stage C1 - the stage that made `at` the muzzle -
// found that every one of them sat INSIDE the shared `ship` mesh. The shuttle's
// `gun_nose` would have fired two and a half metres behind the player's head.
// That was found by a person reading mesh bounds out of a report and comparing
// them to numbers in another file; this tool is that comparison done by looking.
//
// ⚑ A MOUNT WITH NO `at` IS NOT IN THE VIEWPORT AT ALL, which is decisions/014
// rule 2 showing through: `at` present means external - drawn, and shot at
// where it sits - and absent means internal. An internal mount is a row in the
// panel and nothing on the hull, because there is nowhere on the hull it is.
//
// ⚑ Everything a test could assert without a device is elsewhere:
// `mount_rows.hpp` has the document rules and `viewport_pick.hpp` the camera.
// This file projects, hovers, drags and draws, which is the same line stage E
// drew for the point tool and the reason the forge suite can prove any of it.

#include "viewport_pick.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/assets/forge_doc.hpp"
#include "sol/renderer/debug_draw_renderer.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace forge {

class DefEditor;

class MountTool
{
public:
    // ⚑ Mounts are named by STRING, not by index, and that is decisions/014
    // rule 1 arriving in the tool: a save names a fitting by its mount id, so a
    // selection held as "the third mount" would follow the wrong hardpoint the
    // moment an author inserted one above it - which is the operation this tool
    // exists to perform.
    [[nodiscard]] const std::string& hull() const { return m_hull; }

    void setHull(std::string hullId);

    [[nodiscard]] const std::string& selected() const { return m_selected; }

    // The mesh the hull is drawn with, for the surface ray and the inside-out
    // check. Taken from the same `forgeTopology` call the point tool refreshes
    // on, so the two never describe different meshes.
    void refresh(const sol::assets::ForgeDoc& doc);
    void close();

    // Hover, pick, drag and place. Returns true when `ships.toml` changed.
    [[nodiscard]] bool update(const ViewportInput& viewport, DefEditor& editor);

    // The markers. Drawn from the VALIDATED database rather than from the text,
    // so the tool shows the mounts the game would load.
    void drawMarkers(sol::renderer::DebugDrawRenderer& lines,
                     const ViewportInput& viewport,
                     const DefEditor& editor) const;

    // The panel. Returns true when `ships.toml` changed.
    [[nodiscard]] bool drawPanel(DefEditor& editor);

    // True for the whole button press, refusals included - `main.cpp` hands the
    // camera any press this does not claim, and re-deciding mid-drag swings the
    // model under a hand that thought it was moving a mount. Stage E4b measured
    // that at about thirty degrees; the rule is inherited, not rediscovered.
    [[nodiscard]] bool dragging() const { return m_dragging; }

    // Armed by the panel, spent by the next click on the hull. ⚑ A MODE RATHER
    // THAN A CLICK MEANING TWO THINGS: without it, a click on bare hull either
    // creates a mount (so a mis-aimed deselect writes a row) or does not (so
    // there is no gesture for placing one at all).
    [[nodiscard]] bool placing() const { return m_placing; }

private:
    struct Marker
    {
        std::string id;
        sol::core::Vec3 at{};
        // The mount's rest direction and its traverse, in the hull frame. Read
        // straight off the validated `ShipMount`, so a mount that authored
        // neither carries the schema's own defaults - the ship's nose, and no
        // traverse at all - which is exactly what the game would fly.
        sol::core::Vec3 aim{0.0f, 0.0f, -1.0f};
        float arc = 0.0f;
        sol::assets::MountKind kind = sol::assets::MountKind::Utility;
        bool weapon = false;
    };

    // ⚑⚑ WHAT A LEFT-BUTTON DRAG IS HOLDING (stage D2). Before aim was editable
    // there was one answer and a bool would have done; there are two now, and
    // they move different keys of the same row. Held as an enum rather than as
    // two bools because "both at once" is not a state - a press grabs one thing.
    enum class Grab
    {
        None,
        Position, // the ring: moves `at`
        Aim,      // the handle at the end of the aim line: rewrites `aim`
    };

    // Every EXTERNAL mount of the open hull, in mount order.
    void gatherMarkers(const DefEditor& editor, std::vector<Marker>& out) const;
    // The nearest marker to the cursor within the grab radius, or -1.
    [[nodiscard]] int pickMarker(const ViewportInput& viewport, std::span<const Marker> markers) const;
    // Where the SELECTED mount's aim handle sits in the world, and whether it
    // has one at all. ⚑ Only the selected mount has a handle: nine hardpoints
    // with nine handles is eighteen things competing for one cursor, and the
    // handle is always the smaller and less obvious target of the pair.
    [[nodiscard]] bool
    aimHandle(const ViewportInput& viewport, std::span<const Marker> markers, sol::core::Vec3& out) const;
    // Where the cursor's ray enters the mesh, or false when it misses.
    [[nodiscard]] bool
    pickSurface(const ViewportInput& viewport, sol::core::Vec3& point, sol::core::Vec3& normal) const;

    std::vector<sol::assets::ForgePoint> m_points;
    std::vector<sol::assets::ForgeFace> m_faces;
    // The mesh's centre and radius, for the "is this muzzle inside the hull"
    // reading the panel prints. Recomputed with the topology.
    sol::core::Vec3 m_centre{};
    float m_radius = 0.0f;

    std::string m_hull;
    std::string m_selected;
    std::string m_hover;
    // The draft the next placement writes. Held across placements so an author
    // laying four utility mounts sets the kind once.
    sol::assets::MountKind m_placeKind = sol::assets::MountKind::Fixed;
    sol::assets::MountSize m_placeSize = sol::assets::MountSize::Small;
    bool m_placing = false;
    bool m_dragging = false;
    Grab m_grab = Grab::None;
    bool m_refused = false;
    float m_dragDepth = 1.0f;
    // Where the drag has got to. ⚑ ACCUMULATED IN FLOATS HERE rather than read
    // back out of the file every frame: `defNumber` rounds to the precision the
    // panel shows, so a drag that wrote and re-read would quantise its own
    // input and crawl. The file gets the rounded value; the hand moves the real
    // one.
    sol::core::Vec3 m_dragAt{};
    std::string m_status;
};

} // namespace forge
