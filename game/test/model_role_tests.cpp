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

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

#include <cstdio>
#include <string>
#include <vector>

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
    for (const char* stem : {"commodities", "crew", "factions", "models", "modules", "ships",
                             "sounds", "stations", "weapons"}) {
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
            std::printf("  role '%s' -> model '%s' has radius %f, expected 1.0\n", role,
                        row->model.c_str(), static_cast<double>(model->radius));
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
