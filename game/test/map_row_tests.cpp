#include "map_ui.hpp"
#include "space_world.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/survey.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::ui::kNoNavTarget;
using sol::ui::MapLaneRow;
using sol::ui::MapMarkerRow;
using sol::ui::MapPanel;
using sol::ui::MapSystemRow;

namespace {

// Enough of the real defs for the generator and no more: it reads factions()
// for the territory split and stations() for the archetype weights on this
// path. Inline rather than loaded from game/data so retuning a shipped asset
// cannot move this test's numbers.
constexpr const char* kDefs = R"(
[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[faction]]
id = "sol.guild"
name = "Freight Guild"
color = [0.95, 0.75, 0.2]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5

[[station]]
id = "sol.station_mine"
name = "Mining Outpost"
weight_core = 0.5
weight_frontier = 1.5
weight_fringe = 2.0
produces_from = "field"
)";

// A generated galaxy at the shipped seed, which is what puts real fog over
// the start system - the thing the whole defect depends on.
//
// ⚑ The log line this prints reads "0 faction(s)" and that is correct, not a
// broken fixture: the runtime faction table is built by initializeFactions(),
// which GameContent calls separately from generateUniverse(). The galaxy, its
// systems, its stations and the fog over them are all real; only faction
// colours and relations are absent, and nothing on the nav-target path reads
// them.
struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    Fixture()
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        SOL_REQUIRE(defs.factions().size() == 2);
        SOL_REQUIRE(defs.stations().size() == 2);
        world.spawn(1701);
        world.generateUniverse(defs);
    }

    void fill(MapPanel& panel, std::vector<MapMarkerRow>& markers)
    {
        game::fillMapPanel(world, text, panel, systems, lanes, markers);
    }

    std::deque<std::string> text;
    std::vector<MapSystemRow> systems;
    std::vector<MapLaneRow> lanes;
};

} // namespace

// ⚑ The Phase 15 defect. The map's Set Target and Autopilot used to hand
// `selectedMarker` - a ROW number - to selectTarget(), which indexes nav
// slots. The local fill walks every slot and skips the fogged ones, so the
// two stop agreeing at the first undiscovered station. This asserts the row
// carries the slot it came from.
SOL_TEST(map_marker_rows_carry_their_own_nav_slot)
{
    Fixture fixture;
    MapPanel panel;
    std::vector<MapMarkerRow> markers;
    fixture.fill(panel, markers);

    SOL_REQUIRE(!markers.empty());
    const std::span<const game::NavTarget> targets = fixture.world.navTargets();

    for (const MapMarkerRow& row : markers) {
        // Every row names a real, visible slot...
        SOL_REQUIRE(row.navTarget != kNoNavTarget);
        SOL_REQUIRE(row.navTarget < targets.size());
        SOL_CHECK(fixture.world.navTargetVisible(row.navTarget));
        // ...and it is the slot whose name the player is reading.
        SOL_CHECK(std::strcmp(row.name, targets[row.navTarget].name.c_str()) == 0);
    }
}

// ⚑ The assertion that makes the one above mean something. Without fog the
// row number and the slot agree by accident and every check here passes
// against the broken code, so the test has to prove the shift is real - and
// that a row number would therefore have named the wrong target.
SOL_TEST(the_fog_really_does_shift_a_row_off_its_slot)
{
    Fixture fixture;
    MapPanel panel;
    std::vector<MapMarkerRow> markers;
    fixture.fill(panel, markers);

    const std::span<const game::NavTarget> targets = fixture.world.navTargets();
    SOL_REQUIRE(targets.size() > markers.size()); // something is hidden

    bool anyShifted = false;
    for (std::size_t row = 0; row < markers.size(); ++row) {
        if (markers[row].navTarget != row) {
            anyShifted = true;
            // The old code would have passed `row` here. Prove that names a
            // different target than the one on screen - either a wrong one or
            // a hidden one, which selectTarget refuses without saying so.
            const bool wouldBeWrong = row >= targets.size() || !fixture.world.navTargetVisible(row) ||
                                      std::strcmp(markers[row].name, targets[row].name.c_str()) != 0;
            SOL_CHECK(wouldBeWrong);
        }
    }
    SOL_CHECK(anyShifted);
}

// A remote system has no live nav list at all, so its rows must not look like
// slots. The footer already swaps the two buttons out on a remote view; this
// is the guard underneath that, so a future caller cannot reintroduce 8q's
// "delete whichever bookmark happened to occupy that slot back home".
SOL_TEST(remote_marker_rows_name_no_nav_slot)
{
    Fixture fixture;
    MapPanel panel;
    std::vector<MapMarkerRow> markers;

    // A remote view needs a system the player has HEARD of: a fresh game knows
    // exactly one (Phase 8z), and the fill quietly falls back to "wherever the
    // player is" for anything Unknown - which would make this test vacuous
    // rather than failing. So chart a neighbour first.
    const std::uint32_t current = fixture.world.currentSystemIndex();
    const std::uint32_t remote = current == 0 ? 1u : 0u;
    fixture.world.survey().setKnowledge(fixture.world.galaxy(), remote, sol::sim::KnowledgeState::Charted);
    panel.viewSystem = static_cast<int>(remote);
    fixture.fill(panel, markers);

    SOL_REQUIRE(!panel.viewIsCurrent);
    for (const MapMarkerRow& row : markers) {
        SOL_CHECK(row.navTarget == kNoNavTarget);
    }
}
