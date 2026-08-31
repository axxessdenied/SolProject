#include "mount_tool.hpp"

#include "def_editor.hpp"
#include "mount_rows.hpp"
#include "part_pick.hpp"

#include "sol/ui/pick.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge {

using namespace sol;

namespace {

// The same grab radius the point tool uses, and for the same reason: an author
// aiming at a hardpoint is aiming precisely.
constexpr float kGrabPixels = 12.0f;

constexpr core::Vec4 kMountColor = {0.45f, 0.70f, 0.95f, 1.0f};
constexpr core::Vec4 kWeaponColor = {0.95f, 0.55f, 0.35f, 1.0f};
constexpr core::Vec4 kHoverColor = {0.95f, 0.80f, 0.30f, 1.0f};
constexpr core::Vec4 kSelectedColor = {0.30f, 0.95f, 0.45f, 1.0f};

// Metres at 0.1 mm, the precision every other position in this tool is written
// at (`def_editor.cpp`'s `kMetreDecimals`, Phase 14's grid).
constexpr int kMetreDecimals = 4;

// ⚑⚑ A RING AND A CROSS, NOT THE POINT TOOL'S BARE CROSS, AND THE DIFFERENCE IS
// NOT DECORATION. Both tools draw markers into the same viewport over the same
// mesh; if a mount looked like a vertex, the one question this tool exists to
// answer - "is that hardpoint where I meant it" - would be asked of a picture in
// which the two are indistinguishable. The ring says "a mount is here"; the
// cross inside it says exactly where.
//
// ⚑⚑⚑ DRAWN IN FRONT OF THE MESH, AND THE FIRST LIVE RUN IS WHAT DECIDED IT.
// `DebugDrawRenderer` tests depth against the hull, and a mount is very often
// INSIDE it - the shuttle's `bay_port` sits at (-1.1, 0, 0.6) in a mesh spanning
// z -7..+5, deliberately, because `ships.toml` says the utility mounts are
// interior points. So the first drive drew five markers and showed two smudges
// at the silhouette: every hardpoint an author most needs to find was hidden
// behind the ship it is bolted to. It is stage N's finding arriving at a second
// tool, and it has the same answer.
//
// ⚑⚑ THE PULL IS A UNIFORM SCALE ABOUT THE EYE, WHICH LEAVES THE MARKER
// PIXEL-IDENTICAL. Every point moves along its own view ray and a projection
// maps that whole ray to one screen point, so `p' = eye + (p - eye) * k` changes
// the depth and nothing else - PROVIDED the radius is scaled by the same `k`,
// which is why the ring is built after the pull rather than before it. Getting
// that half wrong gives a ring the size of the window.
//
// ⚑ AND THE PICTURE THEREFORE DOES NOT SAY WHETHER A MOUNT IS BURIED - the
// panel does, in metres, from `surfaceDepthAlongBearing`. That split is
// deliberate: a marker that vanished when it went inside the hull would answer
// the question by being absent, which is indistinguishable from a marker that
// was never drawn at all.
void addMountMarker(renderer::DebugDrawRenderer& lines,
                    const ViewportInput& viewport,
                    core::Vec3 at,
                    float radius,
                    core::Vec4 color)
{
    constexpr float kFrontDepth = 0.12f; // metres, against a 0.05 m near plane

    const float depth = -core::transformPoint(viewport.view, at).z;
    core::Vec3 centre = at;
    float scaled = radius;
    if (depth > kFrontDepth) {
        const core::Vec3 eye = cameraEye(viewport.view);
        const float k = kFrontDepth / depth;
        centre = {eye.x + (at.x - eye.x) * k, eye.y + (at.y - eye.y) * k, eye.z + (at.z - eye.z) * k};
        scaled = radius * k;
    }
    // A marker at or behind the eye is left where it is: there is no scale that
    // brings it in front, and the projection mirrors such a point rather than
    // clipping it. The same refusal `addBox` makes for a box straddling the eye.

    // Screen-facing, so the ring reads as a ring from every angle a turntable
    // can reach - a world-axis circle is a line edge-on, which is exactly the
    // view an author checks a nose gun from.
    const core::Vec3 right = cameraRight(viewport.view);
    const core::Vec3 up = cameraUp(viewport.view);
    constexpr int kSegments = 12;
    core::Vec3 previous{};
    for (int i = 0; i <= kSegments; ++i) {
        const float angle = core::kTwoPi * static_cast<float>(i) / static_cast<float>(kSegments);
        const float c = std::cos(angle) * scaled;
        const float s = std::sin(angle) * scaled;
        const core::Vec3 point{centre.x + right.x * c + up.x * s,
                               centre.y + right.y * c + up.y * s,
                               centre.z + right.z * c + up.z * s};
        if (i > 0) {
            lines.line(previous, point, color);
        }
        previous = point;
    }
    // ⚑ The cross is in the RING'S plane rather than on the world axes: pulled to
    // 0.12 m from the eye, a world-axis cross of the same size sticks out of the
    // ring by whatever each axis is foreshortened by, and reads as three spikes
    // rather than as a centre mark.
    const float tick = scaled * 0.5f;
    lines.line({centre.x - right.x * tick, centre.y - right.y * tick, centre.z - right.z * tick},
               {centre.x + right.x * tick, centre.y + right.y * tick, centre.z + right.z * tick},
               color);
    lines.line({centre.x - up.x * tick, centre.y - up.y * tick, centre.z - up.z * tick},
               {centre.x + up.x * tick, centre.y + up.y * tick, centre.z + up.z * tick},
               color);
}

// The world-space length of the aim line and its handle, as a multiple of the
// marker radius. ⚑ A MULTIPLE OF THE RADIUS AND NOT OF THE HULL, so the line is
// the same length on screen at every zoom - a facing is a direction, and a line
// whose length meant something would be a line an author reads a number off.
constexpr float kAimLength = 4.0f;

// A unit vector perpendicular to `v`. ⚑ Built off whichever world axis `v` is
// LEAST aligned with, because crossing with a fixed axis degenerates to zero
// exactly when the aim points along it - and `ships.toml` has three mounts
// aiming down +Y and four down +Z, so the degenerate case is the common one.
[[nodiscard]] core::Vec3 anyPerpendicular(core::Vec3 v)
{
    const core::Vec3 axis =
        std::fabs(v.x) < std::fabs(v.y)
            ? (std::fabs(v.x) < std::fabs(v.z) ? core::Vec3{1.0f, 0.0f, 0.0f} : core::Vec3{0.0f, 0.0f, 1.0f})
            : (std::fabs(v.y) < std::fabs(v.z) ? core::Vec3{0.0f, 1.0f, 0.0f} : core::Vec3{0.0f, 0.0f, 1.0f});
    return core::normalize(core::cross(v, axis));
}

// Pulls one world point onto the near overlay plane, the same uniform scale
// about the eye `addMountMarker` uses. Returns the pulled point and the factor,
// so a caller drawing a SHAPE can scale its extent by the same amount and keep
// the picture pixel-identical.
[[nodiscard]] core::Vec3 towardEye(const ViewportInput& viewport, core::Vec3 at, float& outScale)
{
    constexpr float kFrontDepth = 0.12f;
    const float depth = -core::transformPoint(viewport.view, at).z;
    if (depth <= kFrontDepth) {
        outScale = 1.0f;
        return at;
    }
    const core::Vec3 eye = cameraEye(viewport.view);
    outScale = kFrontDepth / depth;
    return {eye.x + (at.x - eye.x) * outScale,
            eye.y + (at.y - eye.y) * outScale,
            eye.z + (at.z - eye.z) * outScale};
}

// ⚑⚑⚑ WHERE A MOUNT POINTS AND HOW FAR IT CAN SWING, WHICH IS THE HALF OF THE
// SCHEMA STAGE A2 AUTHORED AND NOBODY COULD SEE. `aim` and `arc` were written
// into `ships.toml` in stage A2, read by nothing until C2, and are still the
// only two mount keys whose value is a claim about the SHAPE of a ship that no
// panel of numbers can check. A 270-degree ring on a dorsal turret either does
// or does not clear the hull it is bolted to, and the way to find out is to
// look at it.
//
// ⚑⚑ `arc` IS THE FULL CONE ANGLE CENTRED ON `aim`, NOT A HALF-ANGLE, so the
// rim below stands at arc/2 either side. That reading is `ships.toml`'s,
// gdd.md 11.5's and decisions/014's, and it is the one number in this schema
// whose meaning a second author would most plausibly guess wrong - which is
// exactly why drawing it is worth more than documenting it again.
//
// ⚑ A rim at a FIXED distance rather than a cone of a fixed radius: at 180
// degrees a cone's radius is infinite and at 270 it is negative, and this
// schema allows both. `at + L * (cos(half) * aim + sin(half) * perp)` is
// well-defined for every angle from 0 to 360.
void addAimCone(renderer::DebugDrawRenderer& lines,
                const ViewportInput& viewport,
                core::Vec3 at,
                core::Vec3 aim,
                float arcDegrees,
                float length,
                core::Vec4 color)
{
    float scale = 1.0f;
    const core::Vec3 centre = towardEye(viewport, at, scale);
    const float reach = length * scale;
    const core::Vec3 tip{centre.x + aim.x * reach, centre.y + aim.y * reach, centre.z + aim.z * reach};
    lines.line(centre, tip, color);
    if (arcDegrees <= 0.0f) {
        return; // bolted down: a direction and no ring to draw
    }

    const float half = core::radians(arcDegrees * 0.5f);
    const core::Vec3 u = anyPerpendicular(aim);
    const core::Vec3 v = core::cross(aim, u);
    const float along = std::cos(half) * reach;
    const float across = std::sin(half) * reach;

    constexpr int kSegments = 16;
    core::Vec3 previous{};
    for (int i = 0; i <= kSegments; ++i) {
        const float t = core::kTwoPi * static_cast<float>(i) / static_cast<float>(kSegments);
        const float c = std::cos(t) * across;
        const float d = std::sin(t) * across;
        const core::Vec3 rim{centre.x + aim.x * along + u.x * c + v.x * d,
                             centre.y + aim.y * along + u.y * c + v.y * d,
                             centre.z + aim.z * along + u.z * c + v.z * d};
        if (i > 0) {
            lines.line(previous, rim, color);
        }
        // Four spokes, so the rim reads as the mouth of a cone rather than as a
        // free-floating circle. Every fourth segment of sixteen.
        if (i % 4 == 0) {
            lines.line(centre, rim, color);
        }
        previous = rim;
    }
}

// ⚑⚑⚑ IS THIS MUZZLE INSIDE THE HULL - THE ONE READING THAT WOULD HAVE CAUGHT
// PHASE 31 STAGE C1's DEFECT BEFORE A PERSON FOUND IT BY HAND, and the reason
// this file bothers with a second ray at all.
//
// The test is one cast: come in from outside along the bearing the mount stands
// on, and see whether the skin is reached BEFORE the mount is. A mount further
// from the centre than the first surface along its own bearing is outside the
// hull; one nearer is buried in it.
//
// ⚑ IT IS A BEARING TEST AND NOT A CONTAINMENT TEST, said out loud because the
// two differ on a concave hull: a mount tucked inside an intake reads as buried
// even though a bolt would leave cleanly along `aim`. That is a warning worth
// having anyway - it is exactly where a muzzle wants a second look - and the
// honest containment test is a parity count `forgePickFace` cannot give, since
// it answers with the NEAREST face rather than with every crossing.
struct SkinClearance
{
    // False when the mesh cannot answer: no faces, or a bearing that misses it.
    bool known = false;
    // ⚑⚑ THE CASE THE FIRST DRAFT ANSWERED WITH SILENCE, AND THE DRIVE FOUND IT
    // ON THE VERY FIRST PRESS. A mount standing exactly at the hull's centre has
    // no bearing to come in along, so the cast cannot run - and "give it a
    // position" puts a mount at exactly the centre, by design, because that is
    // somewhere the author can then see and drag. So the one button that makes
    // an external mount made one in the single position where the reading
    // declined to speak, and the most obviously buried mount in the tool showed
    // no warning at all. The centre of a closed hull is inside it; that is an
    // answer, not a gap.
    bool atCentre = false;
    // Metres from the skin along the mount's own bearing: positive outside,
    // negative buried.
    float metres = 0.0f;
};

[[nodiscard]] SkinClearance skinClearance(std::span<const assets::ForgePoint> points,
                                          std::span<const assets::ForgeFace> faces,
                                          core::Vec3 centre,
                                          float radius,
                                          core::Vec3 at)
{
    SkinClearance clearance;
    if (faces.empty() || radius <= 0.0f) {
        return clearance;
    }
    const core::Vec3 offset{at.x - centre.x, at.y - centre.y, at.z - centre.z};
    const float reach = std::sqrt((offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z));
    if (reach < 1e-4f) {
        clearance.known = true;
        clearance.atCentre = true;
        return clearance;
    }
    const core::Vec3 bearing{offset.x / reach, offset.y / reach, offset.z / reach};
    const float start = radius * 4.0f;
    const core::Vec3 origin{
        centre.x + bearing.x * start, centre.y + bearing.y * start, centre.z + bearing.z * start};
    std::size_t face = 0;
    double distance = 0.0;
    if (!assets::forgePickFace(points,
                               faces,
                               {origin.x, origin.y, origin.z},
                               {-bearing.x, -bearing.y, -bearing.z},
                               face,
                               distance)) {
        return clearance; // the bearing misses the mesh entirely - nothing to be inside of
    }
    clearance.known = true;
    clearance.metres = reach - (start - static_cast<float>(distance));
    return clearance;
}

[[nodiscard]] const char* kindLabel(assets::MountKind kind)
{
    return assets::mountKindName(kind);
}

[[nodiscard]] const char* sizeLabel(assets::MountSize size)
{
    return assets::mountSizeName(size);
}

} // namespace

void MountTool::setHull(std::string hullId)
{
    if (hullId == m_hull) {
        return;
    }
    m_hull = std::move(hullId);
    m_selected.clear();
    m_hover.clear();
    m_placing = false;
    m_dragging = false;
    m_refused = false;
    m_status.clear();
}

void MountTool::refresh(const assets::ForgeDoc& doc)
{
    m_points.clear();
    m_faces.clear();
    m_centre = {};
    m_radius = 0.0f;

    std::vector<assets::ForgeEdge> edges;
    if (!assets::forgeTopology(doc, m_points, edges, m_faces, nullptr)) {
        return;
    }
    if (m_points.empty()) {
        return;
    }
    // The bounding box's centre and the radius that reaches its corners - the
    // frame the bearing test comes in from. Not `MeshReport::boundingRadius`,
    // which is measured about the ORIGIN: this test wants a point the mesh
    // surrounds, and the origin is outside a hull whose author modelled it
    // off-centre.
    assets::BuildPoint lo = m_points.front().position;
    assets::BuildPoint hi = lo;
    for (const assets::ForgePoint& point : m_points) {
        lo.x = std::min(lo.x, point.position.x);
        lo.y = std::min(lo.y, point.position.y);
        lo.z = std::min(lo.z, point.position.z);
        hi.x = std::max(hi.x, point.position.x);
        hi.y = std::max(hi.y, point.position.y);
        hi.z = std::max(hi.z, point.position.z);
    }
    m_centre = {static_cast<float>((lo.x + hi.x) * 0.5),
                static_cast<float>((lo.y + hi.y) * 0.5),
                static_cast<float>((lo.z + hi.z) * 0.5)};
    const core::Vec3 half{static_cast<float>((hi.x - lo.x) * 0.5),
                          static_cast<float>((hi.y - lo.y) * 0.5),
                          static_cast<float>((hi.z - lo.z) * 0.5)};
    m_radius = std::sqrt((half.x * half.x) + (half.y * half.y) + (half.z * half.z));
}

void MountTool::close()
{
    m_points.clear();
    m_faces.clear();
    m_hull.clear();
    m_selected.clear();
    m_hover.clear();
    m_placing = false;
    m_dragging = false;
    m_refused = false;
    m_status.clear();
}

void MountTool::gatherMarkers(const DefEditor& editor, std::vector<Marker>& out) const
{
    out.clear();
    const assets::ShipDef* def = editor.hull(m_hull);
    if (def == nullptr) {
        return;
    }
    for (const assets::ShipMount& mount : def->mounts) {
        if (!mount.external) {
            continue; // internal: a row in the panel and nothing on the hull
        }
        out.push_back({mount.id,
                       {mount.at[0], mount.at[1], mount.at[2]},
                       core::normalize(core::Vec3{mount.aim[0], mount.aim[1], mount.aim[2]}),
                       mount.arc,
                       mount.kind,
                       assets::mountTakesWeapon(mount.kind)});
    }
}

int MountTool::pickMarker(const ViewportInput& viewport, std::span<const Marker> markers) const
{
    int best = -1;
    float bestDistance = kGrabPixels;
    for (int i = 0; i < static_cast<int>(markers.size()); ++i) {
        const ui::ScreenPoint screen =
            ui::screenPoint(core::transformPoint(viewport.view, markers[static_cast<std::size_t>(i)].at),
                            viewport.center,
                            viewport.focal);
        if (!screen.inFront) {
            continue;
        }
        const float dx = screen.position.x - viewport.cursor.x;
        const float dy = screen.position.y - viewport.cursor.y;
        const float distance = std::sqrt((dx * dx) + (dy * dy));
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

bool MountTool::aimHandle(const ViewportInput& viewport,
                          std::span<const Marker> markers,
                          core::Vec3& out) const
{
    if (m_selected.empty()) {
        return false;
    }
    const auto found =
        std::find_if(markers.begin(), markers.end(), [&](const Marker& m) { return m.id == m_selected; });
    if (found == markers.end()) {
        return false;
    }
    // The handle sits at the end of the aim line, in the world - the same place
    // the line is drawn to before the overlay pull, so the pick and the picture
    // agree by construction rather than by two expressions of one length.
    const float reach = viewport.cameraDistance * 0.024f * kAimLength;
    out = {found->at.x + found->aim.x * reach,
           found->at.y + found->aim.y * reach,
           found->at.z + found->aim.z * reach};
    return true;
}

bool MountTool::pickSurface(const ViewportInput& viewport, core::Vec3& point, core::Vec3& normal) const
{
    if (m_faces.empty()) {
        return false;
    }
    const core::Vec3 direction = cameraRay(viewport.view, viewport.cursor, viewport.center, viewport.focal);
    const core::Vec3 eye = cameraEye(viewport.view);
    std::size_t face = 0;
    double distance = 0.0;
    if (!assets::forgePickFace(m_points,
                               m_faces,
                               {eye.x, eye.y, eye.z},
                               {direction.x, direction.y, direction.z},
                               face,
                               distance)) {
        return false;
    }
    point = {eye.x + direction.x * static_cast<float>(distance),
             eye.y + direction.y * static_cast<float>(distance),
             eye.z + direction.z * static_cast<float>(distance)};

    const assets::ForgeFace& hit = m_faces[face];
    const assets::BuildPoint n =
        core::normalize(core::cross(m_points[hit.b].position - m_points[hit.a].position,
                                    m_points[hit.c].position - m_points[hit.a].position));
    normal = {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
    // ⚑ TURNED TO FACE THE CAMERA, because `forgePickFace` hits back faces as
    // readily as front ones and a mount placed on the inside of a wall would
    // take the normal pointing INTO the hull. The author is looking at the
    // surface they clicked, so the outward direction is the one toward the eye.
    const float facing = (normal.x * -direction.x) + (normal.y * -direction.y) + (normal.z * -direction.z);
    if (facing < 0.0f) {
        normal = {-normal.x, -normal.y, -normal.z};
    }
    return true;
}

bool MountTool::update(const ViewportInput& viewport, DefEditor& editor)
{
    if (viewport.uiCaptured || m_hull.empty()) {
        m_hover.clear();
        if (!viewport.leftDown) {
            m_dragging = false;
            m_refused = false;
        }
        return false;
    }

    std::vector<Marker> markers;
    gatherMarkers(editor, markers);

    // The hover, whenever the camera is not holding the button - the same
    // freeze `part_pick.hpp` explains: while you orbit, the model turns under a
    // stationary cursor and a recomputed hover walks through everything that
    // passes beneath it.
    if (!m_dragging && !forgeCameraHoldsMouse(viewport.leftDown, viewport.leftPressed, viewport.middleDown)) {
        const int hovered = pickMarker(viewport, markers);
        m_hover = hovered < 0 ? std::string{} : markers[static_cast<std::size_t>(hovered)].id;
    }

    if (viewport.leftPressed && !m_dragging) {
        m_status.clear();
        // ⚑ ARMED MEANS ARMED: while `m_placing` the markers are not offered at
        // all. Letting a marker under the cursor win would make the button do
        // nothing on the one click an author is most likely to aim at an
        // existing mount - beside the one they already have - and it would
        // cancel the arm with no message to say it had.
        // ⚑⚑ THE AIM HANDLE IS OFFERED THE PRESS BEFORE THE RINGS, and the
        // order is the whole of what makes it usable. The handle belongs to the
        // SELECTED mount and stands a fixed distance from its ring, so on a
        // dense hull it will often overlap a neighbour's ring - and a press
        // that re-selected the neighbour instead of re-aiming the mount the
        // author is working on is a handle that stops working exactly where a
        // hull gets interesting. Position is the commoner gesture; aim is the
        // one the author has already committed to by selecting.
        core::Vec3 handle{};
        const bool haveHandle = !m_placing && aimHandle(viewport, markers, handle);
        if (haveHandle) {
            const ui::ScreenPoint screen =
                ui::screenPoint(core::transformPoint(viewport.view, handle), viewport.center, viewport.focal);
            const float dx = screen.position.x - viewport.cursor.x;
            const float dy = screen.position.y - viewport.cursor.y;
            if (screen.inFront && std::sqrt((dx * dx) + (dy * dy)) < kGrabPixels) {
                m_dragging = true;
                m_grab = Grab::Aim;
                m_refused = false;
                m_dragAt = handle;
                m_dragDepth = -core::transformPoint(viewport.view, handle).z;
                editor.beginEdit("aim mount");
                return false;
            }
        }
        const int hovered = m_placing ? -1 : pickMarker(viewport, markers);
        if (hovered >= 0) {
            const Marker& marker = markers[static_cast<std::size_t>(hovered)];
            m_selected = marker.id;
            m_dragging = true;
            m_grab = Grab::Position;
            m_refused = false;
            m_dragAt = marker.at;
            m_dragDepth = -core::transformPoint(viewport.view, marker.at).z;
            // ⚑ ONE UNDO ENTRY PER DRAG, not per frame. `setMountVector` pushes
            // none of its own for exactly this reason - a drag writes the
            // document sixty times a second and sixty identical entries bury
            // the state the author actually wants back.
            editor.beginEdit("move mount");
            return false;
        }
        if (m_placing) {
            core::Vec3 point{};
            core::Vec3 normal{};
            if (!pickSurface(viewport, point, normal)) {
                m_status = "click ON the hull - a mount goes where the surface is";
                return false;
            }
            MountDraft draft;
            draft.id = mountIdStem(m_placeKind);
            draft.kind = m_placeKind;
            draft.size = m_placeSize;
            draft.external = true;
            draft.at[0] = point.x;
            draft.at[1] = point.y;
            draft.at[2] = point.z;
            // ⚑⚑ THE SURFACE'S OWN NORMAL, WHICH IS THE HALF OF A PLACEMENT
            // THAT A POSITION ALONE CANNOT GIVE. A turret dropped on the dorsal
            // hull faces up, one dropped on the flank faces out, and neither
            // needs three numbers typed against a mesh nobody can measure.
            //
            // ⚑ Written only when it differs from the schema's default. A gun
            // placed on the nose of a hull whose nose is -Z gets no `aim` key
            // at all, which is what an author would have typed and what
            // `writeMountDraft` refuses to invent.
            constexpr float kNoseward = 0.9995f; // ~1.8 degrees off the default
            const float alongNose = -normal.z;
            draft.hasAim =
                !(alongNose > kNoseward && std::fabs(normal.x) < 0.03f && std::fabs(normal.y) < 0.03f);
            draft.aim[0] = normal.x;
            draft.aim[1] = normal.y;
            draft.aim[2] = normal.z;
            m_placing = false;
            if (!editor.addMount(m_hull, draft)) {
                m_status = editor.error();
                return false;
            }
            // The id the document settled on, which is not the draft's when a
            // mount of that kind was already there.
            const assets::ShipDef* def = editor.hull(m_hull);
            m_selected = def != nullptr && !def->mounts.empty() ? def->mounts.back().id : draft.id;
            m_status = "placed '" + m_selected + "'";
            return true;
        }
        // A click on nothing is a deselect, which is the gesture that has to
        // exist for the panel below to ever show no mount.
        m_selected.clear();
        return false;
    }

    if (!viewport.leftDown) {
        m_dragging = false;
        m_grab = Grab::None;
        m_refused = false;
        return false;
    }
    if (!m_dragging || m_refused) {
        return false;
    }
    if (viewport.cursorDelta.x == 0.0f && viewport.cursorDelta.y == 0.0f) {
        return false; // nothing moved: do not dirty a document over a held button
    }

    const core::Vec3 delta = dragDelta(viewport.view,
                                       viewport.cursorDelta,
                                       m_dragDepth,
                                       viewport.verticalFov,
                                       viewport.height,
                                       viewport.axisLock);
    m_dragAt = {m_dragAt.x + delta.x, m_dragAt.y + delta.y, m_dragAt.z + delta.z};

    if (m_grab == Grab::Aim) {
        // ⚑ THE HANDLE MOVES FREELY AND ONLY ITS DIRECTION IS KEPT. `aim` is a
        // facing, so how far the hand dragged is meaningless and only where it
        // ended up matters - which also means the handle can be flung past the
        // hull and the mount still ends up pointing sensibly.
        //
        // ⚑ A drag that lands exactly on the mount is REFUSED rather than
        // normalised: the schema rejects a zero `aim` by name, and a facing of
        // no length is not a direction an author can have meant.
        std::vector<Marker> current;
        gatherMarkers(editor, current);
        const auto found =
            std::find_if(current.begin(), current.end(), [&](const Marker& m) { return m.id == m_selected; });
        if (found == current.end()) {
            m_refused = true;
            return false;
        }
        const core::Vec3 offset{m_dragAt.x - found->at.x, m_dragAt.y - found->at.y, m_dragAt.z - found->at.z};
        const float length = std::sqrt((offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z));
        if (length < 1e-4f) {
            return false; // on top of the mount: no direction yet, and no error either
        }
        const float aim[3] = {offset.x / length, offset.y / length, offset.z / length};
        if (!editor.setMountVector(m_hull, m_selected, "aim", aim)) {
            m_status = editor.error();
            m_refused = true;
            return false;
        }
        return true;
    }

    const float at[3] = {m_dragAt.x, m_dragAt.y, m_dragAt.z};
    if (!editor.setMountVector(m_hull, m_selected, "at", at)) {
        m_status = editor.error();
        m_refused = true;
        return false;
    }
    return true;
}

void MountTool::drawMarkers(renderer::DebugDrawRenderer& lines,
                            const ViewportInput& viewport,
                            const DefEditor& editor) const
{
    std::vector<Marker> markers;
    gatherMarkers(editor, markers);
    if (markers.empty()) {
        return;
    }
    // Sized off the camera distance so a marker is about the same size on
    // screen across the four orders of magnitude the orbit camera covers -
    // `PointTool::drawMarkers`' rule, at a larger radius because a ring has to
    // read as a ring where a cross only has to read as a position.
    const float radius = viewport.cameraDistance * 0.024f;
    for (const Marker& marker : markers) {
        core::Vec4 color = marker.weapon ? kWeaponColor : kMountColor;
        if (marker.id == m_selected) {
            color = kSelectedColor;
        } else if (marker.id == m_hover) {
            color = kHoverColor;
        }
        addMountMarker(lines, viewport, marker.at, radius, color);

        // ⚑⚑ ONLY THE SELECTED MOUNT SHOWS ITS FACING, AND THAT IS A BUDGET
        // DECISION AS MUCH AS A DESIGN ONE. `DebugDrawRenderer` holds 8192
        // vertices and drops lines SILENTLY once they are spent; a cone is a
        // 16-segment rim plus four spokes plus the aim line - about 50 vertices
        // - and the freighter has nine mounts. Nine cones over a grid, the
        // scale references and nine rings is a picture nobody can read even
        // when it fits. The facing of the mount you are working on is the
        // question being asked.
        if (marker.id != m_selected) {
            continue;
        }
        const float length = radius * kAimLength;
        addAimCone(lines, viewport, marker.at, marker.aim, marker.arc, length, color);
        // The handle, at the end of the line, drawn as its own small ring so it
        // is visibly a thing to grab rather than the end of a stroke.
        const core::Vec3 handle{marker.at.x + marker.aim.x * length,
                                marker.at.y + marker.aim.y * length,
                                marker.at.z + marker.aim.z * length};
        addMountMarker(lines, viewport, handle, radius * 0.45f, color);
    }
}

bool MountTool::drawPanel(DefEditor& editor)
{
    bool changed = false;
    if (!editor.loaded()) {
        ImGui::TextDisabled("no def directory: nothing to mount anything on");
        return false;
    }

    // ---- which hull -------------------------------------------------------
    const std::vector<std::string> hulls = editor.hullsOnOpenModel();
    if (hulls.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("no [[ship]] row flies this model, so it has no mounts to place. "
                            "The Report panel can make one.");
        ImGui::PopTextWrapPos();
        return false;
    }
    if (std::find(hulls.begin(), hulls.end(), m_hull) == hulls.end()) {
        setHull(hulls.front());
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("hull", m_hull.c_str())) {
        for (const std::string& candidate : hulls) {
            if (ImGui::Selectable(candidate.c_str(), candidate == m_hull)) {
                setHull(candidate);
            }
        }
        ImGui::EndCombo();
    }
    // ⚑⚑⚑ A COPY, AND IT IS A CORRECTNESS RULE RATHER THAN A STYLE ONE. Every
    // edit below goes through `DefEditor`, which REPLACES its whole
    // `DefDatabase` on success - so a `ShipDef*` read at the top of this
    // function is a dangling pointer the moment any button on it is pressed,
    // and the very next line reads the freed vector. An immediate-mode panel
    // draws THIS frame's state and lets the edit show up on the next one; two
    // dozen mounts is nothing to copy, and the alternative is remembering the
    // hazard at every one of the eleven call sites below.
    const assets::ShipDef* loaded = editor.hull(m_hull);
    if (loaded == nullptr) {
        ImGui::TextDisabled("that hull does not load");
        return false;
    }
    const float hullScale = loaded->scale;
    const std::vector<assets::ShipMount> mounts = loaded->mounts;

    // ⚑⚑ THE SCALE NOTE, AND IT IS LOAD-BEARING RATHER THAN TRIVIA. `at` is
    // metres in the hull frame AT SCALE 1 and the sim multiplies it by the
    // hull's `scale` at the muzzle, so this viewport - which draws the mesh
    // unscaled - IS the frame the numbers are written in. The freighter is the
    // same mesh at 4x, and an author who assumed otherwise would author its
    // turrets four times too far out.
    if (hullScale != 1.0f) {
        ImGui::TextDisabled("  flies at scale %.2f; `at` is authored at scale 1, which is this viewport",
                            static_cast<double>(hullScale));
    }

    // ---- placing ----------------------------------------------------------
    ImGui::Separator();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("kind##place", kindLabel(m_placeKind))) {
        for (std::size_t i = 0; i < assets::kMountKindCount; ++i) {
            const auto kind = static_cast<assets::MountKind>(i);
            if (ImGui::Selectable(kindLabel(kind), kind == m_placeKind)) {
                m_placeKind = kind;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("size##place", sizeLabel(m_placeSize))) {
        for (std::size_t i = 0; i < assets::kMountSizeCount; ++i) {
            const auto size = static_cast<assets::MountSize>(i);
            if (ImGui::Selectable(sizeLabel(size), size == m_placeSize)) {
                m_placeSize = size;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button(m_placing ? "cancel" : "place on hull")) {
        m_placing = !m_placing;
        m_status = m_placing ? "click the hull where the mount goes" : std::string{};
    }
    ImGui::SameLine();
    // ⚑ THE INTERNAL MOUNT HAS ITS OWN BUTTON AND CANNOT BE PLACED BY CLICKING,
    // because there is nowhere to click. decisions/014 rule 2 makes `at` the one
    // key that decides external-or-internal, so "add without a position" is a
    // different gesture rather than the same one with a checkbox - and a
    // subsystem bay is a real thing a hull needs (`core_sensor` is one).
    if (ImGui::Button("add internal")) {
        MountDraft draft;
        draft.id = mountIdStem(m_placeKind);
        draft.kind = m_placeKind;
        draft.size = m_placeSize;
        draft.external = false;
        if (editor.addMount(m_hull, draft)) {
            changed = true;
            m_status = "added an internal mount";
        } else {
            m_status = editor.error();
        }
    }
    if (m_placing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.30f, 1.0f));
        ImGui::TextWrapped("armed: click the hull in the viewport");
        ImGui::PopStyleColor();
    }

    // ---- the mount list ---------------------------------------------------
    ImGui::Separator();
    ImGui::Text("%zu mount(s)", mounts.size());
    for (const assets::ShipMount& mount : mounts) {
        ImGui::PushID(mount.id.c_str());
        const bool selected = mount.id == m_selected;
        char label[192];
        if (mount.external) {
            std::snprintf(label,
                          sizeof(label),
                          "%-16s %-9s %-6s  %6.2f %6.2f %6.2f",
                          mount.id.c_str(),
                          kindLabel(mount.kind),
                          sizeLabel(mount.size),
                          static_cast<double>(mount.at[0]),
                          static_cast<double>(mount.at[1]),
                          static_cast<double>(mount.at[2]));
        } else {
            std::snprintf(label,
                          sizeof(label),
                          "%-16s %-9s %-6s  internal",
                          mount.id.c_str(),
                          kindLabel(mount.kind),
                          sizeLabel(mount.size));
        }
        if (ImGui::Selectable(label, selected)) {
            m_selected = selected ? std::string{} : mount.id;
        }
        ImGui::PopID();
    }

    // ---- the selected mount ----------------------------------------------
    const auto found = std::find_if(
        mounts.begin(), mounts.end(), [&](const assets::ShipMount& m) { return m.id == m_selected; });
    const assets::ShipMount* mount = found == mounts.end() ? nullptr : &*found;
    if (mount == nullptr) {
        ImGui::Separator();
        ImGui::TextDisabled("select a mount, in the list or in the viewport");
        if (!m_status.empty()) {
            ImGui::TextDisabled("%s", m_status.c_str());
        }
        return changed;
    }

    ImGui::Separator();
    ImGui::SeparatorText(mount->id.c_str());

    // The id. ⚑ Refusable, unlike everything else here: an author can type a
    // name another mount already has, and `commitShips` hands back the schema's
    // own message rather than a second one written here.
    char id[64];
    std::snprintf(id, sizeof(id), "%s", mount->id.c_str());
    if (ImGui::InputText("id", id, sizeof(id), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string wanted(id);
        if (!wanted.empty() && wanted != mount->id) {
            if (editor.setMountKey(m_hull, m_selected, "id", assets::defString(wanted))) {
                m_selected = wanted;
                changed = true;
                m_status.clear();
            } else {
                m_status = editor.error();
            }
        }
    }
    ImGui::TextDisabled("  a save names a fitting by this, so renaming one empties it");

    const assets::MountKind kind = mount->kind;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("kind", kindLabel(kind))) {
        for (std::size_t i = 0; i < assets::kMountKindCount; ++i) {
            const auto candidate = static_cast<assets::MountKind>(i);
            if (ImGui::Selectable(kindLabel(candidate), candidate == kind) && candidate != kind) {
                if (editor.setMountKey(m_hull, m_selected, "kind", assets::defString(kindLabel(candidate)))) {
                    changed = true;
                } else {
                    m_status = editor.error();
                }
            }
        }
        ImGui::EndCombo();
    }

    const assets::MountSize size = mount->size;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("size", sizeLabel(size))) {
        for (std::size_t i = 0; i < assets::kMountSizeCount; ++i) {
            const auto candidate = static_cast<assets::MountSize>(i);
            if (ImGui::Selectable(sizeLabel(candidate), candidate == size) && candidate != size) {
                if (editor.setMountKey(m_hull, m_selected, "size", assets::defString(sizeLabel(candidate)))) {
                    changed = true;
                } else {
                    m_status = editor.error();
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("  takes this size or smaller");

    // ---- what it comes with ----------------------------------------------
    const std::vector<DefEditor::Fitting> catalog = editor.fittingsFor(kind);
    const std::string fit = mount->fit;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("fit", fit.empty() ? "(bare)" : fit.c_str())) {
        if (ImGui::Selectable("(bare)", fit.empty()) && !fit.empty()) {
            if (editor.clearMountKey(m_hull, m_selected, "fit")) {
                changed = true;
            } else {
                m_status = editor.error();
            }
        }
        for (const DefEditor::Fitting& candidate : catalog) {
            char row[160];
            std::snprintf(row,
                          sizeof(row),
                          "%s  (%s %s)",
                          candidate.name.c_str(),
                          kindLabel(candidate.kind),
                          sizeLabel(candidate.size));
            if (ImGui::Selectable(row, candidate.id == fit) && candidate.id != fit) {
                if (editor.setMountKey(m_hull, m_selected, "fit", assets::defString(candidate.id))) {
                    changed = true;
                } else {
                    m_status = editor.error();
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("  what the hull SHIPS with; the player refits from here");

    // ⚑⚑ A WARNING RATHER THAN A FILTER, AND THE TWO ARE NOT INTERCHANGEABLE
    // HERE. The game does NOT check a hull's own `fit` against `mountAccepts` -
    // `applyShipDef` looks the id up and bolts it on - so a mismatch flies, and
    // hiding it would make this tool unable to express what the file can say.
    // What it CANNOT do is survive a refit: the outfitting screen greys
    // everything the mount refuses, so a player who removes this fitting can
    // never put it back. That is worth saying and not worth forbidding.
    if (!fit.empty()) {
        const auto fitting = std::find_if(
            catalog.begin(), catalog.end(), [&](const DefEditor::Fitting& f) { return f.id == fit; });
        if (fitting == catalog.end()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("  NO SUCH FITTING: '%s' is not in the %s catalog, so this mount comes "
                               "bare and the game logs a warning at spawn",
                               fit.c_str(),
                               assets::mountTakesWeapon(kind) ? "weapon" : "component");
            ImGui::PopStyleColor();
        } else if (!assets::mountAccepts(*mount, fitting->kind, fitting->size)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
            ImGui::TextWrapped("  '%s' is %s/%s and this mount is %s/%s: it flies, but a player who "
                               "removes it can never put it back",
                               fitting->name.c_str(),
                               kindLabel(fitting->kind),
                               sizeLabel(fitting->size),
                               kindLabel(kind),
                               sizeLabel(size));
            ImGui::PopStyleColor();
        }
    }

    // ---- where it is ------------------------------------------------------
    ImGui::Separator();
    if (mount->external) {
        float at[3] = {mount->at[0], mount->at[1], mount->at[2]};
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::DragFloat3("at", at, 0.01f, -1000.0f, 1000.0f, "%.4f")) {
            if (editor.setMountVector(m_hull, m_selected, "at", at)) {
                changed = true;
            } else {
                m_status = editor.error();
            }
        }
        editor.noteActivation("set mount at");
        ImGui::TextDisabled("  metres, hull frame: +X right, +Y up, -Z forward");

        // ---- where it points, and how far it can swing (stage D2) ---------
        float aim[3] = {mount->aim[0], mount->aim[1], mount->aim[2]};
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::DragFloat3("aim", aim, 0.01f, -1.0f, 1.0f, "%.3f")) {
            // ⚑ REFUSED AT ZERO RATHER THAN NORMALISED TO SOMETHING. The schema
            // rejects a zero `aim` by name, and a slider dragged through zero
            // that silently snapped to an axis would be the tool choosing a
            // facing the author did not.
            const float length = std::sqrt((aim[0] * aim[0]) + (aim[1] * aim[1]) + (aim[2] * aim[2]));
            if (length > 1e-4f) {
                const float unit[3] = {aim[0] / length, aim[1] / length, aim[2] / length};
                if (editor.setMountVector(m_hull, m_selected, "aim", unit)) {
                    changed = true;
                } else {
                    m_status = editor.error();
                }
            }
        }
        editor.noteActivation("set mount aim");
        ImGui::TextDisabled("  where it rests; drag the small ring in the viewport");

        // ⚑⚑ THE ONE SENTENCE THAT STOPS A SECOND AUTHOR GUESSING. `arc` is the
        // FULL cone angle centred on `aim`, so 270 reaches 135 either side -
        // read the other way, every shipped turret would be unable to go blind
        // anywhere. gdd.md 11.5, `ships.toml`'s header and decisions/014 all say
        // so; this is the fourth place, and the only one an author is looking at
        // while they choose the number.
        float arc = mount->arc;
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::DragFloat("arc", &arc, 0.5f, 0.0f, 360.0f, "%.1f deg")) {
            if (editor.setMountNumber(m_hull, m_selected, "arc", arc)) {
                changed = true;
            } else {
                m_status = editor.error();
            }
        }
        editor.noteActivation("set mount arc");
        if (arc <= 0.0f) {
            ImGui::TextDisabled("  bolted down: it points where `aim` says and the pilot flies it");
        } else if (arc >= 360.0f) {
            ImGui::TextDisabled("  no stop at all: it bears on anything");
        } else {
            ImGui::TextDisabled("  the FULL cone, so %.1f deg either side of `aim`",
                                static_cast<double>(arc * 0.5f));
        }
        // ⚑ A traverse on a mount no gun can go in is not an error and is
        // almost certainly a mistake: `arc` is read by `layGun` and by nothing
        // else, so a 270-degree cargo bay is a number that will never be
        // consulted. Said once, quietly, rather than refused.
        if (arc > 0.0f && !assets::mountTakesWeapon(kind)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
            ImGui::TextWrapped("  only a weapon mount traverses: nothing reads `arc` on a %s mount",
                               kindLabel(kind));
            ImGui::PopStyleColor();
        }

        // ⚑⚑⚑ THE READING THIS WHOLE STAGE IS FOR. See
        // `surfaceDepthAlongBearing`: it is a bearing test, and it is scoped to
        // WEAPON mounts because those are the ones where `at` is the MUZZLE.
        // The shipped freighter's utility mounts are interior points on purpose
        // - `ships.toml` says so - so warning about them would be noise that
        // teaches an author to ignore the warning that matters.
        const core::Vec3 here{mount->at[0], mount->at[1], mount->at[2]};
        const SkinClearance clearance = skinClearance(m_points, m_faces, m_centre, m_radius, here);
        const bool buried = clearance.known && (clearance.atCentre || clearance.metres < -1e-3f);
        if (buried && assets::mountTakesWeapon(kind)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
            if (clearance.atCentre) {
                ImGui::TextWrapped("  AT THE HULL'S CENTRE, so inside it. A gun fitted here fires from "
                                   "inside the ship - which on the shuttle is behind the player's head.");
            } else {
                ImGui::TextWrapped("  INSIDE THE HULL by %.2f m along its own bearing. A gun fitted "
                                   "here fires from inside the ship - which on the shuttle is behind "
                                   "the player's head.",
                                   static_cast<double>(-clearance.metres));
            }
            ImGui::PopStyleColor();
        } else if (clearance.atCentre) {
            ImGui::TextDisabled("  at the hull's centre, so inside it");
        } else if (clearance.known) {
            ImGui::TextDisabled("  %+.3f m from the skin along its own bearing",
                                static_cast<double>(clearance.metres));
        }
    } else {
        ImGui::TextDisabled("internal: no `at`, so it is never drawn and never aimed at");
        ImGui::TextDisabled("  it is still destructible - that is Phase 31 stage F");
        if (ImGui::Button("give it a position")) {
            // At the hull's centre, which is somewhere the author can then see
            // and drag. Placing it at the origin would put it wherever the mesh
            // is NOT on a model authored off-centre.
            const float at[3] = {m_centre.x, m_centre.y, m_centre.z};
            editor.beginEdit("make mount external");
            if (editor.setMountVector(m_hull, m_selected, "at", at)) {
                changed = true;
                m_status = "now external - drag it onto the skin";
            } else {
                m_status = editor.error();
            }
        }
    }

    if (mount->external && ImGui::Button("make internal")) {
        // ⚑ `aim` and `arc` go with it, and they have to: the schema REFUSES a
        // row carrying either without an `at` (decisions/014 rule 2 read
        // backwards), so clearing one key alone writes a file the game will not
        // load - and `commitShips` would refuse the edit with a message about a
        // key the author never touched.
        editor.beginEdit("make mount internal");
        const bool ok = editor.clearMountKey(m_hull, m_selected, "aim") &&
                        editor.clearMountKey(m_hull, m_selected, "arc") &&
                        editor.clearMountKey(m_hull, m_selected, "at");
        if (ok) {
            changed = true;
        } else {
            m_status = editor.error();
        }
    }

    ImGui::Separator();
    if (ImGui::Button("remove mount")) {
        if (editor.removeMount(m_hull, m_selected)) {
            m_selected.clear();
            changed = true;
            m_status.clear();
        } else {
            m_status = editor.error();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("the note above it in the file stays");

    if (!m_status.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", m_status.c_str());
        ImGui::PopTextWrapPos();
    }
    return changed;
}

} // namespace forge
