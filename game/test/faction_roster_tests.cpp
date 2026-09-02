// What a faction actually fields for a roster cell (Phase 32 stage C).
//
// ⚑ The rules live in `assets` and the SUBSTITUTIONS live here, which is why
// this suite is beside the game and not beside the def tests. `assets.unit`
// owns what `builds_no` parses to and what a stale roster id refuses; these
// own what the six spawn sites do with the answer, which is the half that was
// written out by hand four times and never in one place.

#include "content.hpp"
#include "space_world.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::RosterCell;

namespace {

[[nodiscard]] std::size_t cell(RosterCell c)
{
    return static_cast<std::size_t>(c);
}

// A faction that patrols in one hull and has said nothing about hauling. Every
// faction in this game looked like this before stage C.
[[nodiscard]] game::GameFaction patroller()
{
    game::GameFaction faction;
    faction.defId = "sol.test";
    faction.name = "Test";
    faction.shipsPatrol = {"sol.interceptor"};
    return faction;
}

} // namespace

// ⚑⚑⚑ THE WHOLE STAGE IN ONE TEST, AND IT IS A PAIR BECAUSE ONE HALF ALONE
// PROVES NOTHING. Two factions with a BYTE-IDENTICAL `shipsTrader` — both empty
// — and the same patrol hulls. The one that has declared it fields no traders
// gets nothing; the one that has merely not said gets the substitution the call
// site asks for. Before `builds_no` these were the same vector and there was no
// way to ask them apart, so the first faction hauled in the second's
// interceptors.
SOL_TEST(a_declared_cell_is_never_substituted_for)
{
    game::GameFaction declared = patroller();
    declared.buildsNo[cell(RosterCell::Trader)] = true;
    const game::GameFaction unspecified = patroller();

    SOL_CHECK(declared.shipsTrader.empty() && unspecified.shipsTrader.empty());

    SOL_CHECK(factionRoster(declared, RosterCell::Trader, RosterCell::Patrol).empty());
    const std::span<const std::string> fallback =
        factionRoster(unspecified, RosterCell::Trader, RosterCell::Patrol);
    SOL_REQUIRE(fallback.size() == 1);
    SOL_CHECK(fallback[0] == "sol.interceptor");
}

// The cell a faction DOES field is untouched by a declaration about another
// one — a per-cell statement, not a flag on the faction.
SOL_TEST(a_declaration_about_one_cell_leaves_the_others_alone)
{
    game::GameFaction faction = patroller();
    faction.shipsRaider = {"sol.freighter"};
    faction.buildsNo[cell(RosterCell::Trader)] = true;

    const std::span<const std::string> raiders =
        factionRoster(faction, RosterCell::Raider, RosterCell::Patrol);
    SOL_REQUIRE(raiders.size() == 1);
    SOL_CHECK(raiders[0] == "sol.freighter");
    const std::span<const std::string> patrols =
        factionRoster(faction, RosterCell::Patrol, RosterCell::Count);
    SOL_REQUIRE(patrols.size() == 1);
    SOL_CHECK(patrols[0] == "sol.interceptor");
}

// ⚑⚑ `RosterCell::Count` IS HOW A SITE SAYS IT HAS NO SUBSTITUTE, and both
// ambient spawns pass it. That is not tidiness: reading a roster bare — which
// is what those two sites did for twenty phases — substitutes nothing, and a
// helper that tabulated one fallback per cell would have quietly given ambient
// patrol spawning a fallback it has never had.
SOL_TEST(a_site_with_no_substitute_gets_nothing_rather_than_another_cell)
{
    const game::GameFaction faction = patroller();
    SOL_CHECK(factionRoster(faction, RosterCell::Trader, RosterCell::Count).empty());
    SOL_CHECK(factionRoster(faction, RosterCell::Raider, RosterCell::Count).empty());
    // And the same cell WITH a substitute named does fall back, so the empty
    // answers above are about the fallback argument and not about the cell.
    SOL_CHECK(factionRoster(faction, RosterCell::Trader, RosterCell::Patrol).size() == 1);
}

// A substitute that is itself declared-empty is not flown either. Otherwise a
// faction that has said it builds no patrol hulls would still put patrol hulls
// in the sky, by the back door of a cell it never mentioned.
SOL_TEST(a_substitute_the_faction_also_refuses_is_not_flown)
{
    game::GameFaction faction = patroller();
    faction.buildsNo[cell(RosterCell::Patrol)] = true;
    faction.shipsPatrol.clear(); // the parser refuses declaring and listing both
    SOL_CHECK(factionRoster(faction, RosterCell::Trader, RosterCell::Patrol).empty());
}

// ⚑⚑⚑ THE SHIPPED CONTENT, WHICH IS WHERE THIS STAGE IS EITHER REAL OR A
// FEATURE WITH NO USER. Both pirate templates declare they field no traders and
// no patrols — the character their `description` has asserted since Phase 8b —
// and the five majors declare nothing, exactly as every faction did before.
SOL_TEST(the_shipped_clans_declare_the_cells_they_do_not_field)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(defs.validateRosters(&error));

    std::uint32_t clans = 0;
    std::uint32_t majors = 0;
    std::uint32_t shadow = 0;
    for (const sol::assets::FactionDef& faction : defs.factions()) {
        // ⚑⚑⚑ THE THIRD KIND DECLARES ALL THREE CELLS AND IT IS THE ONLY ROW IN
        // THE FILE THAT DOES (Phase 37 stage B). These rosters are what a
        // faction spawns in the space it HOLDS, and the black market holds
        // none - so "we field nothing" is not a tuning choice for it the way
        // the clans' two cells were, it is what the kind means. Asserted here
        // rather than left to the majors' `!buildsNo[i]` sweep below, which
        // this row would have failed with no explanation of why.
        if (faction.kind == sol::assets::FactionKind::Shadow) {
            ++shadow;
            for (std::size_t i = 0; i < sol::assets::kRosterCellCount; ++i) {
                SOL_CHECK(faction.buildsNo[i]);
            }
            SOL_CHECK(faction.shipsPatrol.empty());
            SOL_CHECK(faction.shipsRaider.empty());
            SOL_CHECK(faction.shipsTrader.empty());
            continue;
        }
        if (faction.kind == sol::assets::FactionKind::Pirate) {
            ++clans;
            SOL_CHECK(faction.buildsNo[cell(RosterCell::Trader)]);
            SOL_CHECK(faction.buildsNo[cell(RosterCell::Patrol)]);
            // What a clan IS, and the one cell it still fields.
            SOL_CHECK(!faction.buildsNo[cell(RosterCell::Raider)]);
            SOL_CHECK(!faction.shipsRaider.empty());
            continue;
        }
        ++majors;
        for (std::size_t i = 0; i < sol::assets::kRosterCellCount; ++i) {
            if (faction.buildsNo[i]) {
                std::printf("  %s declares builds_no for %s\n",
                            faction.id.c_str(),
                            sol::assets::rosterCellName(static_cast<RosterCell>(i)));
            }
            SOL_CHECK(!faction.buildsNo[i]);
        }
    }
    SOL_CHECK(clans == 2);
    SOL_CHECK(majors == 5);
    // Ruling 1 of Phase 37: exactly ONE, hand-authored, not a template. If a
    // second ever ships, `SpaceWorld::shadowFactionIndex()` stops being an
    // identity and this is the line that says so first.
    SOL_CHECK(shadow == 1);
}

// ⚑⚑⚑ AND WHAT THAT DECLARATION ACTUALLY CHANGED, MEASURED RATHER THAN
// ASSERTED. The same shipped clan, with and without its declaration, asked the
// question the two trader sites ask. With it: nothing, so a clan-held system
// grows no scheduled freight. Without it: the clan's own patrol roster, because
// that is what `shipsTrader.empty() ? shipsPatrol : shipsTrader` did at both
// sites for four phases — so deleting the `ships_trader` line, the obvious way
// to say "they do not haul", would have made them haul in gunships instead.
SOL_TEST(the_clan_declaration_is_what_stops_it_hauling_in_its_own_gunships)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    const sol::assets::FactionDef* clan = defs.findFaction("sol.corsairs");
    SOL_REQUIRE(clan != nullptr);

    game::GameFaction declared;
    declared.defId = clan->id;
    declared.kind = sol::assets::FactionKind::Pirate;
    declared.shipsPatrol = clan->shipsPatrol;
    declared.shipsRaider = clan->shipsRaider;
    declared.shipsTrader = clan->shipsTrader;
    for (std::size_t i = 0; i < sol::assets::kRosterCellCount; ++i) {
        declared.buildsNo[i] = clan->buildsNo[i];
    }
    SOL_CHECK(factionRoster(declared, RosterCell::Trader, RosterCell::Patrol).empty());

    // The counterfactual: the same rosters, the declaration dropped, and a
    // patrol cell filled the way every major's is. This is the pre-stage-C
    // behaviour, and it has to be different or the declaration is decoration.
    game::GameFaction bare = declared;
    for (std::size_t i = 0; i < sol::assets::kRosterCellCount; ++i) {
        bare.buildsNo[i] = false;
    }
    bare.shipsPatrol = {"sol.interceptor"};
    const std::span<const std::string> hauled = factionRoster(bare, RosterCell::Trader, RosterCell::Patrol);
    SOL_REQUIRE(hauled.size() == 1);
    SOL_CHECK(hauled[0] == "sol.interceptor");
}

// ⚑⚑⚑ THE READOUT FITS THE PANEL IT IS PRINTED INTO, MEASURED RATHER THAN
// ASSERTED IN A COMMENT. The first `sol.rosters()` put a faction on one line
// and ran to 88 columns on every major - the two-hull trader cell fell off the
// right edge of a ~76-column console and nothing said so. Only the screenshot
// caught it, which is the SECOND time in this phase: stage A's hull spine lost
// its verdict column to a 360 px dock exactly the same way, and that is the
// column the whole panel existed for.
//
// ⚑ Measured against SHIPPED content, so the guard tightens when a faction
// grows a longer roster - which is precisely when it would clip again.
SOL_TEST(the_roster_readout_fits_the_console_panel)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    const std::vector<std::string> lines = game::rosterLines(defs);
    // Two lines per faction, and eight ship.
    SOL_CHECK(lines.size() == defs.factions().size() * 2);
    SOL_REQUIRE(!lines.empty());

    std::size_t widest = 0;
    for (const std::string& line : lines) {
        if (line.size() > game::kConsoleColumns) {
            std::printf("  %zu columns: %s\n", line.size(), line.c_str());
        }
        SOL_CHECK(line.size() <= game::kConsoleColumns);
        widest = std::max(widest, line.size());
    }
    // ⚑ And it is not fitting by accident on a table of empty cells: the
    // widest shipped line is a real roster, not a row of dashes.
    SOL_CHECK(widest > 30);
}
