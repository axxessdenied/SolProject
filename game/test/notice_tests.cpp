// Somewhere to be stopped, and how often (engine plan Phase 36 stage B).
//
// ⚑⚑⚑⚑ THE RULING WAS "BOTH POSTURES" AND THE MEASUREMENT CHANGED HOW IT IS
// PAID FOR. A picket at every gate is +214 hulls on the shipped galaxy against
// a patrol force of 99, and a curve on the baseline is +102 — both roughly
// double the ambient hull count, which is the tax `017` warns about arriving
// through the front door. Splitting the EXISTING wing costs nothing, watches 40
// gates across 39 of the 52 policed systems, and leaves the worst sky at the 4
// hulls it already had. A garrison is finite; where it stands is the decision.
//
// ⚑⚑ THE ENABLING CHANGE IS SMALLER THAN THE FEATURE AND IT FIXED A BUG ON THE
// WAY. `pilot_patrol_offset` resolved against `stationPosition()` — `m_targets[0]`,
// ONE point for the whole system — so a patrol spawned at a gate flew 600,000 km
// home on its first think. It now resolves against `nearestPost`, which also
// means that in the 45 shipped systems with more than one station, patrols stop
// all forming up around station 0.
//
// ⚑ Notice is a DECISION here and nothing more. Stage C is what hangs a hail, a
// hold and a scan off it; the frequency has to be right before there is an
// interaction to get wrong.

#include "asset_paths.hpp"
#include "content.hpp"
#include "space_world.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

namespace {

using Notice = game::SpaceWorld::NoticeReason;
using Post = game::SpaceWorld::PatrolPost;

// The shipped galaxy, through the real content path INCLUDING the mod layer.
// ⚑ The mod layer is load-bearing: without it the galaxy is 81 systems where
// the running game has 85, and every count below would be measured against a
// galaxy no player ever flies.
struct Galaxy
{
    game::SpaceWorld world;
    game::GameContent content;

    Galaxy()
    {
        world.spawn(game::kDefaultUniverseSeed);
        const std::string mods = std::string(SOL_DEF_DATA_DIR) + "/../mods";
        SOL_CHECK(content.initialize(SOL_DEF_DATA_DIR, game::discoverModLayers(mods), &world));
        SOL_CHECK(world.generateUniverse(content.defs()));
    }

    // A policed system whose jurisdiction actually has a legality table, and
    // which has both a station and a gate to post to.
    [[nodiscard]] std::uint32_t lawfulSystem(float minSecurity = 0.6f) const
    {
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            const sol::assets::FactionDef* law = world.jurisdictionOf(s);
            if (law == nullptr || (law->contraband.empty() && law->restricted.empty())) {
                continue;
            }
            if (world.systemSecurityBaseline(s) < minSecurity) {
                continue;
            }
            const sol::sim::SystemSpec& spec = world.galaxy().systems[s];
            if (!spec.stations.empty() && !spec.gates.empty()) {
                return s;
            }
        }
        return 0xffff'ffffu;
    }

    // A system held by somebody who declares nothing illegal. On the shipped
    // galaxy the Freight Guild holds 25 of 85 and is the LARGEST holder.
    [[nodiscard]] std::uint32_t tablelessSystem() const
    {
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            const sol::assets::FactionDef* law = world.jurisdictionOf(s);
            if (law != nullptr && law->contraband.empty() && law->restricted.empty() &&
                world.systemSecurityBaseline(s) > 0.3f) {
                return s;
            }
        }
        return 0xffff'ffffu;
    }
};

// Runs `minutes` of notice rolls and reports how many stops happened.
std::uint32_t stopsOver(game::SpaceWorld& world, int minutes)
{
    const std::uint32_t before = world.lastNotice().count;
    constexpr double kStep = 1.0 / 60.0;
    for (int i = 0; i < minutes * 60 * 60; ++i) {
        (void)world.considerNotice(kStep);
    }
    return world.lastNotice().count - before;
}

} // namespace

// ⚑⚑⚑⚑ BOTH POSTURES EXIST AND NEITHER EMPTIED THE OTHER. The station keeps at
// least one hull by construction (`wing - 1`), so this cannot pass by moving the
// whole garrison to a gate — which is the failure mode a "some are at gates"
// assertion on its own would wave through.
SOL_TEST(a_garrison_stands_at_both_a_station_and_a_gate)
{
    Galaxy g;
    int systemsWithBoth = 0, systemsSeen = 0;
    for (std::uint32_t s = 0; s < g.world.galaxy().systems.size() && systemsSeen < 30; ++s) {
        // patrolsFor floors at 1, and a single-hull garrison has nothing to
        // split, so only systems whose wing is 2+ can show both postures.
        if (game::patrolsFor(g.world.systemSecurityBaseline(s)) < 2) {
            continue;
        }
        if (g.world.galaxy().systems[s].gates.empty() || !g.world.enterSystem(s)) {
            continue;
        }
        ++systemsSeen;
        std::vector<Post> posts;
        g.world.patrolPosts(posts);
        const bool anyGate = std::any_of(posts.begin(), posts.end(), [](const Post& p) { return p.atGate; });
        const bool anyStation =
            std::any_of(posts.begin(), posts.end(), [](const Post& p) { return !p.atGate; });
        if (anyGate && anyStation) {
            ++systemsWithBoth;
        }
    }
    SOL_REQUIRE(systemsSeen > 0); // anti-vacuity: the galaxy has such systems
    SOL_CHECK(systemsWithBoth == systemsSeen);
}

// ⚑⚑⚑ AND IT COST NOTHING. The whole reason this shape was chosen over a picket
// per gate is that the hull count does not move; if a later change starts ADDING
// pickets, this is what says so.
SOL_TEST(posting_a_picket_adds_no_hulls_to_the_sky)
{
    Galaxy g;
    int checked = 0;
    for (std::uint32_t s = 0; s < g.world.galaxy().systems.size() && checked < 25; ++s) {
        const float baseline = g.world.systemSecurityBaseline(s);
        if (baseline <= 0.0f || !g.world.enterSystem(s)) {
            continue;
        }
        ++checked;
        std::vector<Post> posts;
        g.world.patrolPosts(posts);
        SOL_CHECK(posts.size() == game::patrolsFor(baseline));
    }
    SOL_REQUIRE(checked > 0);
}

// ⚑⚑⚑ A PATROL HOLDS ITS OWN POST. This is the assertion that would have caught
// the bug the feature was built on top of: with the old `stationPosition()`
// anchor a gate picket's waypoint was 600,000 km away, and the gate-to-station
// separation on the shipped galaxy is 233,203 km at its NARROWEST — so a
// picket that had been re-anchored on the station would sit hundreds of
// thousands of km from the post this reports.
SOL_TEST(a_picket_holds_its_gate_and_not_the_station)
{
    Galaxy g;
    const std::uint32_t system = g.lawfulSystem(0.0f);
    SOL_REQUIRE(system != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(system));

    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    SOL_REQUIRE(!posts.empty());
    for (const Post& post : posts) {
        // Spawned within a few km of whatever it was posted to. The nearest
        // OTHER post is at least 233,203 km away, so this is not a coincidence
        // any placement could satisfy.
        SOL_CHECK(post.distanceToPost < 50'000.0);
    }

    // ⚑⚑⚑⚑ AND IT IS STILL ITS OWN POST AFTER THE PILOT HAS THOUGHT. Spawn
    // position alone proves nothing about the anchor: the old
    // `stationPosition()` rule re-aimed every picket at the system's FIRST
    // station on its first think, and until that think ran the two rules were
    // indistinguishable. So the world is ticked past the 2 Hz think cadence and
    // the WAYPOINT is what gets asserted.
    for (int i = 0; i < 90; ++i) {
        g.content.tick(1.0 / 30.0);
        g.world.tick(1.0 / 30.0);
    }
    g.world.patrolPosts(posts);
    SOL_REQUIRE(!posts.empty());
    bool sawGate = false;
    for (const Post& post : posts) {
        // The patrol legs are +/-2 km around the post, so a waypoint further
        // out than 50 km is a waypoint at somebody else's post.
        SOL_CHECK(post.waypointDistanceToPost < 50'000.0);
        sawGate = sawGate || post.atGate;
    }
    SOL_CHECK(sawGate); // anti-vacuity: a gate posture existed to be re-aimed
}

// ⚑⚑⚑⚑ WHERE THE LAW HAS NOTHING TO SAY, NOBODY IS STOPPED. Measured: only 26
// of 85 systems are held by a faction with a non-empty table. The Freight Guild
// holds 25 and declares nothing illegal, and 29 more are clan-held. Stopping a
// pilot for a jurisdiction that cannot charge them with anything is exactly the
// tax `017` names, so it does not happen — and this is the half of the balance
// that a frequency test alone would miss entirely.
SOL_TEST(a_jurisdiction_with_no_table_does_not_stop_anybody)
{
    Galaxy g;
    const std::uint32_t tableless = g.tablelessSystem();
    SOL_REQUIRE(tableless != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(tableless));

    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    SOL_REQUIRE(!posts.empty()); // there ARE patrols here; they just do not ask
    SOL_REQUIRE(g.world.warpTo(posts[0].post, 2'000.0));
    SOL_REQUIRE(g.world.setTransponder(false)); // the loudest a pilot can be
    g.world.clearNoticeCooldown();

    SOL_CHECK(stopsOver(g.world, 30) == 0);
}

// ⚑⚑⚑⚑ THE PHASE'S WHOLE BALANCE, AND BOTH HALVES ARE REQUIREMENTS. `017`:
// "notice must be RARE in policed core space for a clean pilot and COMMON for a
// dark one." A rule that never fires satisfies the first and fails the second,
// which is why the dark floor is asserted as well as the clean ceiling.
//
// ⚑ The pilot is parked ON TOP OF a patrol for the whole measurement, which is
// the worst case and not a typical one: in play you pass through a patrol's
// envelope for tens of seconds, not for half an hour.
SOL_TEST(a_clean_pilot_is_rarely_bothered_and_a_dark_one_usually_is)
{
    Galaxy g;
    const std::uint32_t system = g.lawfulSystem();
    SOL_REQUIRE(system != 0xffff'ffffu);

    // ⚑ No SOL_REQUIRE inside the lambda: the macro expands to a bare `return`,
    // so a value-returning lambda deduces `void` and will not compile. The
    // preconditions are checked here instead, once, before either pass.
    SOL_REQUIRE(g.world.enterSystem(system));
    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    SOL_REQUIRE(!posts.empty());
    const sol::core::DVec3 beside = posts[0].post;
    SOL_REQUIRE(g.world.warpTo(beside, 2'000.0));

    const auto measure = [&](bool lit) -> std::uint32_t {
        (void)g.world.warpTo(beside, 2'000.0);
        (void)g.world.setTransponder(lit);
        g.world.clearNoticeCooldown();
        return stopsOver(g.world, 30);
    };

    const std::uint32_t clean = measure(true);
    const std::uint32_t dark = measure(false);

    // Rare, but not never: a checkpoint that never checks anybody is scenery.
    SOL_CHECK(clean <= 4);
    // Common. The 90 s cooldown caps this near 20 in thirty minutes, so the
    // floor is what carries the claim.
    SOL_CHECK(dark >= 8);
    // And the ORDERING is the assertion that survives retuning the numbers.
    SOL_CHECK(dark > clean * 2);
}

// The cooldown is what keeps a refusal from being re-rolled every frame. Without
// it the mechanic is a machine gun rather than an event, and no frequency
// measurement above would notice, because they all run for half an hour.
SOL_TEST(one_stop_does_not_become_a_burst)
{
    Galaxy g;
    const std::uint32_t system = g.lawfulSystem();
    SOL_REQUIRE(system != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(system));

    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    SOL_REQUIRE(!posts.empty());
    SOL_REQUIRE(g.world.warpTo(posts[0].post, 2'000.0));
    SOL_REQUIRE(g.world.setTransponder(false));
    g.world.clearNoticeCooldown();

    // Roll until the first stop, then keep rolling for less than the cooldown.
    std::uint32_t guard = 0;
    while (g.world.considerNotice(1.0 / 60.0) == Notice::None && guard < 600'000u) {
        ++guard;
    }
    SOL_REQUIRE(guard < 600'000u); // it did fire
    const std::uint32_t after = g.world.lastNotice().count;

    // ⚑⚑⚑⚑ A FIXED WINDOW, NOT ONE DERIVED FROM THE PARAMETER UNDER TEST. The
    // first version of this read `cooldownSeconds - 1.0` and looped that many
    // frames — so setting the cooldown to ZERO made the window negative, the
    // loop body never ran, and the test passed while asserting nothing.
    // Mutation testing caught it; nothing else would have. What is being
    // asserted is the DESIGN — a stop is an event and not a stream — so the
    // minute is written here in the test rather than read from the thing it is
    // supposed to be checking.
    constexpr int kQuietSeconds = 60;
    for (int i = 0; i < kQuietSeconds * 60; ++i) {
        SOL_CHECK(g.world.considerNotice(1.0 / 60.0) == Notice::None);
    }
    SOL_CHECK(g.world.lastNotice().count == after);
}

// ⚑⚑⚑ A PATROL HAS TO BE ABLE TO SEE YOU. Parked 2 km from a patrol every test
// above would pass with the range check deleted outright — mutation testing
// found that too. This is the only assertion here that puts real distance
// between the player and the law.
SOL_TEST(a_patrol_on_the_far_side_of_the_system_does_not_notice_you)
{
    Galaxy g;
    const std::uint32_t system = g.lawfulSystem();
    SOL_REQUIRE(system != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(system));

    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    SOL_REQUIRE(!posts.empty());

    // Far enough that nothing in the system is within the 80 km envelope. The
    // shipped galaxy puts gates 600,000 km from stations, so a point well
    // outside every post is easy to find and is where a player actually spends
    // most of a transit.
    SOL_REQUIRE(g.world.warpTo(posts[0].post, 5.0e6));
    SOL_REQUIRE(g.world.setTransponder(false)); // as conspicuous as it gets
    g.world.clearNoticeCooldown();

    SOL_CHECK(stopsOver(g.world, 30) == 0);

    // And the same pilot, moved into range, IS noticed — so the zero above is
    // the range rule and not some other refusal swallowing the whole test.
    SOL_REQUIRE(g.world.warpTo(posts[0].post, 2'000.0));
    g.world.clearNoticeCooldown();
    SOL_CHECK(stopsOver(g.world, 30) > 0);
}

// The reason is recorded, and it is the reason that applies rather than the
// first one in the enum. A dark pilot is stopped FOR being dark even when they
// are also wanted, because that is the one a patrol can see from outside.
SOL_TEST(a_stop_records_why_it_happened)
{
    Galaxy g;
    const std::uint32_t system = g.lawfulSystem();
    SOL_REQUIRE(system != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(system));

    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    SOL_REQUIRE(!posts.empty());
    SOL_REQUIRE(g.world.warpTo(posts[0].post, 2'000.0));
    SOL_REQUIRE(g.world.setTransponder(false));
    g.world.clearNoticeCooldown();

    Notice reason = Notice::None;
    std::uint32_t guard = 0;
    while ((reason = g.world.considerNotice(1.0 / 60.0)) == Notice::None && guard < 600'000u) {
        ++guard;
    }
    SOL_REQUIRE(reason == Notice::Dark);
    SOL_CHECK(g.world.lastNotice().reason == Notice::Dark);
    SOL_CHECK(g.world.lastNotice().factionIndex == g.world.systemOwnerFaction(system));
    SOL_CHECK(g.world.lastNotice().distance <= g.world.noticeParams().range);
    SOL_CHECK(std::string(game::SpaceWorld::noticeReasonName(Notice::Dark)) == "running dark");
}
