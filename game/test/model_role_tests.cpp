// The role contract, asserted against the game's OWN committed data (Phase 19).
//
// ⚑ This suite is the only one that can hold this test, and that was checked
// before it was promised rather than after. The role vocabulary lives in
// `game/src/model_roles.hpp`, which `assets.unit` cannot link; the committed
// def files live in `game/data`, which only `assets.unit` previously had a
// path to. `sol_game_tests` links `sol_game_lib` and now gets the data dir,
// so it can see both halves. Same reasoning as stage H putting the def
// cross-check in `mesh_library.cpp` rather than in the widget.

#include "model_roles.hpp"
#include "space_world.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/core/log.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;

namespace {

[[nodiscard]] std::string readWholeFile(const std::string& path)
{
    std::string text;
    if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, read);
        }
        std::fclose(file);
    }
    return text;
}

// The whole base-game def set, merged the way the game merges it. Loaded from
// the source tree rather than inlined precisely because the point is to
// assert something about the files that actually ship.
[[nodiscard]] bool loadCommittedDefs(DefDatabase& db, std::string& error)
{
    for (const char* stem : {"commodities",
                             "components",
                             "crew",
                             "factions",
                             "models",
                             "ships",
                             "sounds",
                             "stations",
                             "weapons"}) {
        const std::string path = std::string(SOL_DEF_DATA_DIR) + "/" + stem + ".toml";
        const std::string text = readWholeFile(path);
        if (text.empty()) {
            error = "cannot read " + path;
            return false;
        }
        if (!db.mergeToml(text.c_str(), text.size(), path.c_str(), &error)) {
            return false;
        }
    }
    return true;
}

} // namespace

// Every slot the engine draws into is filled by the shipped data, and filled
// by a model that exists. This is the assertion that would have caught the
// phase's own worst outcome: a role added to C++ and forgotten in the file.
SOL_TEST(committed_defs_fill_every_model_role)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(loadCommittedDefs(db, error));
    SOL_CHECK(db.validateRoles(game::modelRoles(), &error));

    for (const char* role : game::modelRoles()) {
        SOL_CHECK(db.roleModelIndex(role) != DefDatabase::kNoModel);
    }
}

// ⚑⚑ THE CONTRACT THAT HAS NO VISUAL TELL, WHICH IS WHY IT IS PINNED RATHER
// THAN COMMENTED. A rock, an ore chunk and a bolt are each drawn at an
// instance scale that means something in metres - a rock's scale IS its
// radius - and `modelBaseRadius() * scale` is what the collision set, the
// avoidance set and the mining/salvage beam sweep all read. Point one of these
// roles at a model authored at any other radius and every instance silently
// changes size AND hit sphere, with nothing on screen to say why.
//
// Re-pointing a role is now a one-line data edit, which is exactly what makes
// this reachable by accident.
SOL_TEST(unit_radius_roles_name_a_model_authored_at_radius_one)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(loadCommittedDefs(db, error));

    for (const char* role : game::unitRadiusRoles()) {
        const sol::assets::RoleDef* row = db.findRole(role);
        SOL_REQUIRE(row != nullptr);
        const sol::assets::ModelDef* model = db.findModel(row->model.c_str());
        SOL_REQUIRE(model != nullptr);
        if (model->radius != 1.0f) {
            std::printf("  role '%s' -> model '%s' has radius %f, expected 1.0\n",
                        role,
                        row->model.c_str(),
                        static_cast<double>(model->radius));
        }
        SOL_CHECK(model->radius == 1.0f);
    }
}

// The roles that are NOT under that contract are drawn at scale 1 because
// their meshes are authored at their real size, so their radius is whatever
// the mesh is. Asserted so the two groups cannot quietly merge: adding a role
// to `kUnitRadiusRoles` that is drawn at scale 1 would be as wrong as leaving
// one out.
SOL_TEST(real_size_roles_are_not_under_the_unit_radius_contract)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(loadCommittedDefs(db, error));

    // The gate is 70 m of aperture and the cockpit is an 8 m room; neither is
    // a unit-radius model and neither is scaled at the spawn site.
    SOL_CHECK(db.findModel(db.findRole(game::kRoleGate)->model.c_str())->radius > 1.0f);
    SOL_CHECK(db.findModel(db.findRole(game::kRoleCockpit)->model.c_str())->radius > 1.0f);

    bool gateIsUnderContract = false;
    for (const char* role : game::unitRadiusRoles()) {
        gateIsUnderContract = gateIsUnderContract || std::string(role) == game::kRoleGate;
    }
    SOL_CHECK(!gateIsUnderContract);
}

// ⚑⚑ THE CLAIM THE WHOLE OVERRIDE HALF OF PHASE 19 RESTS ON, AND IT IS THE
// KIND OF CLAIM THAT ROTS SILENTLY: "adding these keys changed nothing for
// the defs the game ships". It is only true while an EMPTY override resolves
// to exactly the role's model, so that is asserted rather than commented.
SOL_TEST(an_unset_model_override_resolves_to_its_role)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(loadCommittedDefs(db, error));

    const game::ModelId rock = game::modelOverrideOr(db, "", "test", game::kRoleRock, true);
    SOL_CHECK(game::modelIndex(rock) == db.roleModelIndex(game::kRoleRock));

    // Every weapon and commodity the base game ships leaves these unset, which
    // is what makes stage C invisible in play until somebody writes a name.
    //
    // ⚑⚑ IT IS `boltModel` NOW, AND THE WEAPON'S `model` IS DELIBERATELY NOT
    // IN THIS LOOP. Phase 31 stage E split the one key in two and filled the
    // new half in `weapons.toml` - a gun IS drawn on the hull, which is the
    // whole of that stage - so a check that every weapon's `model` is empty
    // would now be asserting that stage E did not ship.
    //
    // The BOLT override is still unset on all four, so Phase 19's claim about
    // itself is untouched and still measurable, which is exactly why the two
    // had to become two keys rather than one key with two meanings.
    for (const sol::assets::WeaponDef& weapon : db.weapons()) {
        SOL_CHECK(weapon.boltModel.empty());
    }
    for (const sol::assets::CommodityDef& commodity : db.commodities()) {
        SOL_CHECK(commodity.model.empty());
        SOL_CHECK(commodity.chunkModel.empty());
    }
}

// ⚑⚑ THIS TEST EXISTS BECAUSE THE OBVIOUS ONE ABOVE CANNOT FAIL, AND A
// MUTATION IS WHAT PROVED IT. Deleting `modelOverrideOr`'s empty-name branch
// leaves every RESULT identical - `modelIdFromName("")` finds nothing and
// falls back to the same role - so all twelve tests stayed green on a version
// with the branch gone. The branch's real job is the LOG: without it, the
// normal case (an unset override, which is every def the game ships) warns
// once per commodity per system load.
//
// Stage F's lesson in a new shape: a test that cannot distinguish the rule
// from its fallback is testing the fallback. Assert the thing the branch
// actually controls.
namespace {

int g_warnings = 0;

void countWarnings(sol::core::LogLevel level, const char* message, void* userData)
{
    (void)message;
    (void)userData;
    if (level == sol::core::LogLevel::Warn) {
        ++g_warnings;
    }
}

} // namespace

SOL_TEST(an_unset_override_is_silent_while_a_broken_one_warns)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(loadCommittedDefs(db, error));

    sol::core::setLogSink(&countWarnings, nullptr);

    g_warnings = 0;
    (void)game::modelOverrideOr(db, "", "commodity 'sol.ore'", game::kRoleRock, true);
    const int afterUnset = g_warnings;

    g_warnings = 0;
    (void)game::modelOverrideOr(db, "no_such_model", "commodity 'sol.ore'", game::kRoleRock, true);
    const int afterBroken = g_warnings;

    // ⚑ The unit-radius contract warns too, and it is a SEPARATE warning from
    // the unknown-name one: a model that exists but is the wrong size is a
    // different mistake from a name nothing defines.
    g_warnings = 0;
    (void)game::modelOverrideOr(db, "station", "commodity 'sol.ore'", game::kRoleRock, true);
    const int afterWrongRadius = g_warnings;

    sol::core::setLogSink(nullptr, nullptr);

    SOL_CHECK(afterUnset == 0);
    SOL_CHECK(afterBroken > 0);
    SOL_CHECK(afterWrongRadius > 0);
}

// Phase 19 stage D. The seat belongs to the SHIP, so the answer has to come
// from the active ship's def rather than being resolved once for the life of
// the process the way `main.cpp` used to.
//
// ⚑ This test could not have been written before the resolve moved. It is the
// only model resolution the game had outside `sol_game_lib`, and a test binary
// links the library, not the executable - so "the cockpit is right" was
// unassertable for as long as the literal sat in main().
namespace {

// Two interiors and two hulls, so "it followed the def" and "it fell back to
// the role" are distinguishable answers rather than the same model twice.
constexpr const char* kCockpitDefs = R"(
[[model]]
id = "seat_default"
mesh = "cockpit"
texture = "cockpit"
radius = 8.0

[[model]]
id = "seat_freighter"
mesh = "cockpit"
texture = "cockpit"
radius = 8.0

[[model]]
id = "hull"
mesh = "ship"
texture = "hull"
radius = 8.0

[[role]]
id = "cockpit"
model = "seat_default"
)";

[[nodiscard]] std::string shipsNaming(const char* cockpitKey)
{
    std::string toml = R"(
[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "hull"
)";
    toml += cockpitKey;
    return toml;
}

} // namespace

SOL_TEST(the_cockpit_comes_from_the_active_ships_def)
{
    // A hull that names its own interior gets it.
    {
        DefDatabase defs;
        std::string error;
        SOL_REQUIRE(defs.mergeToml(kCockpitDefs, std::strlen(kCockpitDefs), "m.toml", &error));
        const std::string ships = shipsNaming("cockpit = \"seat_freighter\"\n");
        SOL_REQUIRE(defs.mergeToml(ships.c_str(), ships.size(), "s.toml", &error));

        game::SpaceWorld world;
        world.spawn(1701);
        SOL_CHECK(world.generateUniverse(defs));
        world.applyDefs(defs);
        SOL_CHECK(game::modelIndex(world.cockpitModel()) == defs.modelIndex("seat_freighter"));
    }
    // A hull that names none falls back to the role, which is what all three
    // shipped hulls do - so stage D is invisible until somebody authors a
    // second interior.
    {
        DefDatabase defs;
        std::string error;
        SOL_REQUIRE(defs.mergeToml(kCockpitDefs, std::strlen(kCockpitDefs), "m.toml", &error));
        const std::string ships = shipsNaming("");
        SOL_REQUIRE(defs.mergeToml(ships.c_str(), ships.size(), "s.toml", &error));

        game::SpaceWorld world;
        world.spawn(1701);
        SOL_CHECK(world.generateUniverse(defs));
        world.applyDefs(defs);
        SOL_CHECK(game::modelIndex(world.cockpitModel()) == defs.modelIndex("seat_default"));
    }
}

// Phase 19 stage E. `WreckRecord::defId` is commented "the victim's ship def"
// and has been since the record existed; the spawn site ignored it and drew
// one model at one size for every death.
//
// ⚑ THIS IS THE PHASE'S ONE BEHAVIOUR CHANGE, NOT A COSMETIC ONE. The salvage
// beam sweeps `modelBaseRadius() * scale`, so a wreck's size is how hard it is
// to hit - which is why the assertion is on the SCALE and not on the model.
SOL_TEST(a_wreck_is_drawn_as_the_ship_that_died)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kCockpitDefs, std::strlen(kCockpitDefs), "m.toml", &error));
    constexpr const char* kFleet = R"(
[[role]]
id = "wreck"
model = "hull"

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "hull"
scale = 1.0

[[ship]]
id = "sol.freighter"
name = "Freighter"
model = "hull"
scale = 4.0
)";
    SOL_REQUIRE(defs.mergeToml(kFleet, std::strlen(kFleet), "s.toml", &error));

    game::SpaceWorld world;
    world.spawn(1701);
    SOL_CHECK(world.generateUniverse(defs));
    world.applyDefs(defs);

    const std::uint32_t system = world.currentSystemIndex();
    world.mining().addWreck(system, {0.0, 0.0, 5000.0}, "sol.shuttle", "Shuttle", 1);
    world.mining().addWreck(system, {0.0, 0.0, 6000.0}, "sol.freighter", "Freighter", 2);

    // Wreck records become entities on the sim's own schedule, so a tick is
    // what brings them into the bubble.
    world.tick(0.05);

    // Read back through the renderer's own entry point rather than reaching
    // into the ECS: what is asserted is what would actually be drawn.
    std::vector<game::RenderInstance> instances;
    world.buildRenderInstances(0.0f, false, instances);

    bool sawShuttleWreck = false;
    bool sawFreighterWreck = false;
    for (const game::RenderInstance& instance : instances) {
        sawShuttleWreck = sawShuttleWreck || std::abs(instance.scale.x - 1.4f) < 1e-4f;
        sawFreighterWreck = sawFreighterWreck || std::abs(instance.scale.x - 5.6f) < 1e-4f;
    }

    // 1.0 x 1.4 and 4.0 x 1.4 - the two hulls differ only in scale, so their
    // derelicts must too. Before stage E both read 1.4, and 5.6 is a size
    // nothing else in this world is drawn at.
    SOL_CHECK(sawShuttleWreck);
    SOL_CHECK(sawFreighterWreck);
}

SOL_TEST(a_set_model_override_wins_and_a_broken_one_falls_back)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(loadCommittedDefs(db, error));

    // A name that exists is taken literally - that IS the feature.
    const game::ModelId named = game::modelOverrideOr(db, "station", "test", game::kRoleRock, false);
    SOL_CHECK(game::modelIndex(named) == db.modelIndex("station"));

    // A name nothing defines warns and lands on the role, rather than
    // refusing the way an unfilled ROLE does. The asymmetry is deliberate:
    // one broken override is one broken ore in somebody's mod, while an
    // unfilled role is the game having no answer at all.
    const game::ModelId broken = game::modelOverrideOr(db, "no_such_model", "test", game::kRoleRock, true);
    SOL_CHECK(game::modelIndex(broken) == db.roleModelIndex(game::kRoleRock));
}

// ⚑⚑⚑ THE OTHER HALF OF THE SAME SPLIT (Phase 31 stage E), AND IT IS A
// CLAIM ABOUT SHIPPED CONTENT RATHER THAN ABOUT CODE. A gun's `model` resolves
// under a DIFFERENT rule to every override above it: empty means NOT DRAWN,
// not "whatever the role says". That rule makes it possible to ship a weapon
// nobody can see, silently, and the only thing standing between this stage and
// that is the four names in `weapons.toml`.
//
// ⚑ It also pins the pairing rather than only the presence. Both mining lasers
// wear the emitter and both cannons wear the cannon, which is what makes a
// freighter's two rings tell you WHICH one holds the beam from outside the
// ship - the reason there are two meshes at all.
SOL_TEST(every_shipped_gun_names_a_model_that_exists)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(loadCommittedDefs(db, error));

    SOL_REQUIRE(!db.weapons().empty());
    for (const sol::assets::WeaponDef& weapon : db.weapons()) {
        if (weapon.model.empty()) {
            std::printf("  weapon '%s' names no model, so it would not be drawn\n", weapon.id.c_str());
        }
        SOL_CHECK(!weapon.model.empty());
        SOL_CHECK(db.findModel(weapon.model.c_str()) != nullptr);
    }

    SOL_CHECK(db.findWeapon("sol.pulse_cannon")->model == "cannon");
    SOL_CHECK(db.findWeapon("sol.heavy_cannon")->model == "cannon");
    SOL_CHECK(db.findWeapon("sol.mining_laser")->model == "emitter");
    SOL_CHECK(db.findWeapon("sol.mining_laser_mk2")->model == "emitter");

    // ⚑ A FITTING IS NOT UNDER THE UNIT-RADIUS CONTRACT and must not drift into
    // it. It is drawn at the HULL's scale - the same multiplier the mount's
    // `at` already carries - so its mesh is authored at real size, exactly as
    // the gate and the cockpit are. A gun authored at radius 1 would be a
    // metre-wide barrel on a shuttle and four on a freighter, which is a size
    // nobody chose.
    SOL_CHECK(db.findModel("cannon")->radius > 1.0f);
    SOL_CHECK(db.findModel("emitter")->radius > 1.0f);
    bool fittingIsUnderContract = false;
    for (const char* role : game::unitRadiusRoles()) {
        fittingIsUnderContract = fittingIsUnderContract || std::string(role) == game::kRoleFitting;
    }
    SOL_CHECK(!fittingIsUnderContract);
}
