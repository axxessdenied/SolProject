// The chase rig, and what the seat can see of its own hull (Phase 32 stage B).
//
// ⚑ These are tests about FRAMING rather than about the arithmetic that
// produces it. `chaseOffset` scaling linearly is trivially true by inspection;
// what is not is whether the eye it puts you at can see the whole ship, which
// is the phase's own written exit criterion and the thing that was false. So
// every test below computes the angle a hull actually subtends from where the
// camera ends up and compares it against `kCameraVerticalFov`.
//
// ⚑ This suite can hold them because it links `sol_game_lib` (the rig, the
// field of view) and has `SOL_DEF_DATA_DIR` (the committed hulls). Neither half
// is enough alone - the same split `model_role_tests` documents.

#include "scene_renderer.hpp"
#include "ship_camera.hpp"
#include "space_world.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;

namespace {

// A hull as the CAMERA has to treat it: a sphere of `radius` metres centred on
// the ship's origin, because a collision radius times an instance scale is the
// only size the running game knows (see `SpaceWorld::hullRadius`).
struct Hull
{
    std::string id;
    float radius = 0.0f;
};

// Every shipped hull with its size in world metres. Read off the defs rather
// than hard-coded, so a rescale or a re-measured model has to come back through
// these tests.
[[nodiscard]] std::vector<Hull> shippedHulls(const DefDatabase& defs)
{
    std::vector<Hull> hulls;
    for (const sol::assets::ShipDef& ship : defs.ships()) {
        for (const sol::assets::ModelDef& model : defs.models()) {
            if (model.id == ship.model) {
                hulls.push_back({ship.id, model.radius * ship.scale});
                break;
            }
        }
    }
    return hulls;
}

[[nodiscard]] const sol::assets::ModelDef* findModel(const DefDatabase& defs, const char* id)
{
    for (const sol::assets::ModelDef& model : defs.models()) {
        if (model.id == id) {
            return &model;
        }
    }
    return nullptr;
}

// The half-angle from the camera's own view axis out to the far edge of the
// hull sphere, in radians. Two terms, and both of them matter: the ship is not
// at the centre of the frame (the rig sits above it and pitches down by less
// than it is raised), and the hull is not a point.
//
// ⚑ `asin`, not `atan`, and that is what makes the failing case legible rather
// than a rounding argument: the tangent from the eye to a sphere of radius r at
// distance d makes an angle asin(r/d), and for d <= r there is no tangent at
// all because the eye is INSIDE the hull. That is not a framing failure, it is
// the camera being in the cargo bay, so it answers half a turn rather than NaN.
[[nodiscard]] float hullHalfAngle(sol::core::Vec3 offset, float radius)
{
    const float distance = length(offset);
    if (distance <= radius) {
        return sol::core::radians(180.0f);
    }
    // The ship's origin sits at -offset from the eye, so the angle between the
    // view axis and the ship is the depression of that direction less whatever
    // `kChasePitch` has already taken out.
    const float depression = std::atan2(offset.y, offset.z);
    const float offAxis = std::abs(depression + game::ShipCamera::kChasePitch);
    return offAxis + std::asin(radius / distance);
}

constexpr float kHalfFov = game::kCameraVerticalFov * 0.5f;

} // namespace

// ⚑⚑ THE EXIT CRITERION, AS A TEST: "fly the shipped freighter in chase view
// and see the whole ship". Before this stage the freighter failed it outright -
// its hull sphere is 32 m and the rig sat 31 m from the origin, so the camera
// was INSIDE the ship it was chasing. That is the "ten metres off the tail"
// Phase 31 stage C1 recorded and declined to fix.
SOL_TEST(the_chase_camera_frames_every_shipped_hull)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    const std::vector<Hull> hulls = shippedHulls(defs);
    // Three hulls ship; a parse that produced none would satisfy the loop by
    // never entering it.
    //
    // ⚑⚑ FOUR SINCE PHASE 37 STAGE D. The Ghostline is the first covert hull and
    // it reuses `model = "ship"` at scale 0.9, so it frames like the other two
    // that do - but the count is here so that a fourth hull cannot arrive
    // without somebody checking the camera can see it.
    SOL_CHECK(hulls.size() == 4);

    for (const Hull& hull : hulls) {
        const float half = hullHalfAngle(game::ShipCamera::chaseOffset(hull.radius), hull.radius);
        if (half > kHalfFov) {
            std::printf("  %s: %.1f m hull subtends %.3f rad, frame is %.3f\n",
                        hull.id.c_str(),
                        static_cast<double>(hull.radius),
                        static_cast<double>(half),
                        static_cast<double>(kHalfFov));
        }
        SOL_CHECK(half <= kHalfFov);
    }
}

// ⚑⚑⚑ THE COUNTERFACTUAL, AND WITHOUT IT THE TEST ABOVE PROVES LESS THAN IT
// LOOKS. All three shipped hulls draw ONE mesh with ONE authored radius, so a
// rig keyed on `ShipDef::scale` instead of on metres produces byte-identical
// numbers for every one of them - the two readings of "scales with the hull it
// is chasing" only come apart once a second mesh exists. What committed content
// CAN tell apart is scaling from not scaling, so this pins that half down: the
// fixed stand-off has to fail somewhere, or the test above is green on a rig
// that was never changed.
SOL_TEST(the_unscaled_stand_off_does_not_frame_every_shipped_hull)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    std::uint32_t unframed = 0;
    for (const Hull& hull : shippedHulls(defs)) {
        if (hullHalfAngle(game::ShipCamera::kChaseOffset, hull.radius) > kHalfFov) {
            ++unframed;
        }
    }
    SOL_CHECK(unframed > 0);
}

// The reference hull is untouched to the bit. The shuttle is what every number
// in the rig was tuned against and what the game opens in, so this stage is not
// allowed to move it by a millimetre.
SOL_TEST(the_reference_hull_keeps_the_stand_off_it_was_tuned_with)
{
    const sol::core::Vec3 offset = game::ShipCamera::chaseOffset(game::ShipCamera::kChaseReferenceRadius);
    SOL_CHECK(offset.x == game::ShipCamera::kChaseOffset.x);
    SOL_CHECK(offset.y == game::ShipCamera::kChaseOffset.y);
    SOL_CHECK(offset.z == game::ShipCamera::kChaseOffset.z);

    // A hull with no model def at all takes `modelBaseRadius`'s own 8 m
    // fallback, so it is the reference hull too rather than a special case.
    SOL_CHECK(game::ShipCamera::chaseOffset(8.0f).z == game::ShipCamera::kChaseOffset.z);
}

// ⚑⚑ THE OTHER HALF OF THE EXIT: "set its scale to 10 in a scratch def and see
// the whole ship again". A class-3 hull is 120-300 m by gdd.md §11.1, so its
// collision sphere is tens of metres; the rig has to frame the top of that band
// as well as the bottom. Swept across every class §11.1 names rather than
// checked at one size, because a linear rig either frames all of them or none
// of them and saying so is what makes this a claim about the RULE.
SOL_TEST(the_chase_camera_frames_hulls_across_the_class_bands)
{
    for (float radius = 4.0f; radius <= 1500.0f; radius *= 1.5f) {
        const float half = hullHalfAngle(game::ShipCamera::chaseOffset(radius), radius);
        if (half > kHalfFov) {
            std::printf("  %.1f m hull subtends %.3f rad, frame is %.3f\n",
                        static_cast<double>(radius),
                        static_cast<double>(half),
                        static_cast<double>(kHalfFov));
        }
        SOL_CHECK(half <= kHalfFov);
    }
}

// ⚑⚑⚑ E1's COCKPIT RULING, AS AN INVARIANT ABOUT THE COMMITTED DATA. The seat
// draws its own fittings only while the cabin's authored reach covers the hull
// (see `hideSeatFittings`), and the shuttle satisfies that by construction -
// models.toml says the cockpit "comes out just inside the ship's own 8 m". So a
// sentence in a comment is holding up the thing Phase 31 stage E1 was fought
// over: the pilot seeing their own cannon fire 1.6 m ahead of them. Re-measure
// either mesh and this goes red rather than the gun quietly disappearing.
SOL_TEST(the_starting_hull_is_covered_by_the_cabin_authored_for_it)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    const sol::assets::ModelDef* cockpit = findModel(defs, "cockpit");
    SOL_REQUIRE(cockpit != nullptr);

    std::uint32_t covered = 0;
    for (const Hull& hull : shippedHulls(defs)) {
        if (hull.id != "sol.shuttle" && hull.id != "sol.interceptor") {
            continue;
        }
        if (hull.radius > cockpit->radius) {
            std::printf("  %s: %.2f m hull outgrew a %.2f m cabin\n",
                        hull.id.c_str(),
                        static_cast<double>(hull.radius),
                        static_cast<double>(cockpit->radius));
        }
        SOL_CHECK(hull.radius <= cockpit->radius);
        ++covered;
    }
    SOL_CHECK(covered == 2);
}

// And the case the rule exists for, in shipped content rather than in a
// hypothetical. The freighter draws four times the mesh the cabin was authored
// for, so there are twenty-four metres of undrawn hull between the pilot and
// the tail; ships.toml says of that cabin, in as many words, that it "is not a
// bigger cabin than the shuttle's, it is a different one at the same size".
SOL_TEST(a_hull_larger_than_its_cabin_hides_its_own_fittings_from_the_seat)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    const sol::assets::ModelDef* cockpit = findModel(defs, "freighter_cockpit");
    SOL_REQUIRE(cockpit != nullptr);

    bool checked = false;
    for (const Hull& hull : shippedHulls(defs)) {
        if (hull.id != "sol.freighter") {
            continue;
        }
        checked = true;
        SOL_CHECK(hull.radius > cockpit->radius);
    }
    SOL_CHECK(checked);
}
