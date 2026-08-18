#include <sol/sim/missions.hpp>

#include <sol/sim/economy.hpp>
#include <sol/sim/faction_sim.hpp>
#include <sol/sim/universe.hpp>

#include <sol/core/serialize.hpp>
#include <sol/test/test.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using sol::sim::BountyCandidate;
using sol::sim::Economy;
using sol::sim::EconomyArchetype;
using sol::sim::EconomyCommodity;
using sol::sim::EconomyParams;
using sol::sim::FactionAgentParams;
using sol::sim::FactionSim;
using sol::sim::FactionSimParams;
using sol::sim::Galaxy;
using sol::sim::HaulCandidate;
using sol::sim::kAnySystem;
using sol::sim::kNoFaction;
using sol::sim::Mission;
using sol::sim::MissionEvent;
using sol::sim::MissionEventKind;
using sol::sim::MissionObjective;
using sol::sim::MissionParams;
using sol::sim::MissionSim;
using sol::sim::ObjectiveKind;
using sol::sim::SystemSpec;

namespace {

// Three systems in a chain, one station each: 0 (major A) - 1 (major B) -
// 2 (pirate clan C); C is at war with both majors from the baseline.
Galaxy chainGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 11;
    for (std::uint32_t i = 0; i < 3; ++i) {
        SystemSpec system;
        system.name = std::string("S") + static_cast<char>('0' + i);
        system.factionIndex = i;
        system.planets.push_back({.name = "P", .position = {}, .radius = 1.0e6});
        system.stations.push_back({.name = "St", .archetype = 0, .position = {}});
        if (i > 0) {
            system.gates.push_back({.toSystem = i - 1, .position = {}});
        }
        if (i < 2) {
            system.gates.push_back({.toSystem = i + 1, .position = {}});
        }
        galaxy.systems.push_back(std::move(system));
    }
    galaxy.links = {{0, 1}, {1, 2}};
    galaxy.clans.push_back({.name = "S2 Raiders", .templateIndex = 0, .seed = 3, .homeSystem = 2});
    return galaxy;
}

FactionSimParams chainFactionParams()
{
    FactionSimParams params;
    params.agents = {FactionAgentParams{.aggression = 1.0f, .forgiveness = 0.5f},
                     FactionAgentParams{.aggression = 0.0f, .forgiveness = 0.5f},
                     FactionAgentParams{.aggression = 1.0f, .forgiveness = 0.1f,
                                        .pirate = true}};
    params.baselineRelations = {0.0f, -35.0f, -60.0f, //
                                -35.0f, 0.0f, -60.0f, //
                                -60.0f, -60.0f, 0.0f};
    params.initialStandings = {0.0f, 0.0f, -20.0f};
    return params;
}

EconomyParams oneCommodityParams()
{
    EconomyParams params;
    params.commodities = {EconomyCommodity{.basePrice = 10.0f}};
    EconomyArchetype archetype;
    archetype.production = {0.0f}; // static stocks: shortages come from raids
    archetype.stockCapacity = 1'000.0f;
    params.archetypes = {archetype};
    params.traderCount = 0;
    return params;
}

// A world where system 1 has been raided into a shortage by clan 2: its
// market stock sits at 5% of capacity and its raid intensity is warm.
struct MissionWorld
{
    Galaxy galaxy = chainGalaxy();
    Economy economy;
    FactionSim factions;
    MissionSim missions;

    explicit MissionWorld(std::uint64_t seed = 7, MissionParams params = {})
    {
        economy.initialize(galaxy, oneCommodityParams(), seed);
        factions.initialize(galaxy, chainFactionParams(), seed);
        missions.initialize(galaxy, params, 3, 1, seed);
        SOL_CHECK(factions.commitRaid(galaxy, &economy, 2, 1)); // intensity 1.0
        economy.raidSystem(1, 0.888888888f);                    // stock -> ~50/1000
    }
};

Mission haulOffer(float units)
{
    Mission mission;
    mission.title = "Relief run";
    mission.poster = 1;
    mission.rewardCredits = 500.0;
    mission.standingReward = 3.0f;
    mission.standingPenalty = 2.0f;
    mission.objectives.push_back({.kind = ObjectiveKind::Deliver,
                                  .system = 1,
                                  .station = 0,
                                  .commodity = 0,
                                  .units = units,
                                  .text = "Deliver goods to S1"});
    return mission;
}

Mission bountyOffer(std::uint32_t kills)
{
    Mission mission;
    mission.title = "Bounty: S2 Raiders";
    mission.poster = 1;
    mission.rewardCredits = 800.0;
    mission.standingReward = 4.0f;
    mission.standingPenalty = 2.0f;
    mission.objectives.push_back({.kind = ObjectiveKind::Kill,
                                  .system = 1,
                                  .faction = 2,
                                  .kills = kills,
                                  .text = "Destroy raiders in S1"});
    return mission;
}

// Four systems in a chain for the contest tests (Phase 8u): faction 0 holds
// 0 AND 1, faction 1 holds 2, clan 2 holds 3. Faction 0 holding two systems
// is what makes system 1 contestable at all — a faction's only system is its
// home and can never change hands.
Galaxy territoryGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 17;
    constexpr std::uint32_t kOwners[4] = {0, 0, 1, 2};
    for (std::uint32_t i = 0; i < 4; ++i) {
        SystemSpec system;
        system.name = std::string("T") + static_cast<char>('0' + i);
        system.factionIndex = kOwners[i];
        system.planets.push_back({.name = "P", .position = {}, .radius = 1.0e6});
        system.stations.push_back({.name = "St", .archetype = 0, .position = {}});
        if (i > 0) {
            system.gates.push_back({.toSystem = i - 1, .position = {}});
        }
        if (i < 3) {
            system.gates.push_back({.toSystem = i + 1, .position = {}});
        }
        galaxy.systems.push_back(std::move(system));
    }
    galaxy.links = {{0, 1}, {1, 2}, {2, 3}};
    galaxy.clans.push_back({.name = "T3 Raiders", .templateIndex = 0, .seed = 3, .homeSystem = 3});
    return galaxy;
}

// System 1 (faction 0's, not their home) is contested by faction 1, with the
// board open at system 0's station.
struct ContestWorld
{
    Galaxy galaxy = territoryGalaxy();
    Economy economy;
    FactionSim factions;
    MissionSim missions;

    explicit ContestWorld(std::uint64_t seed = 7, MissionParams params = {})
    {
        economy.initialize(galaxy, oneCommodityParams(), seed);
        factions.initialize(galaxy, chainFactionParams(), seed);
        missions.initialize(galaxy, params, 3, 1, seed);
        factions.setContest(1, 1, 0.5f);
        SOL_CHECK(factions.contested(1));
        missions.openBoard(0, 0);
    }
};

Mission holdOffer(std::uint32_t system, std::uint32_t faction, std::uint32_t poster)
{
    Mission mission;
    mission.title = "Hold the line";
    mission.poster = poster;
    mission.rewardCredits = 1'200.0;
    mission.standingReward = 6.0f;
    mission.standingPenalty = 3.0f;
    mission.deadline = 900.0;
    mission.objectives.push_back({.kind = ObjectiveKind::Hold,
                                  .system = system,
                                  .faction = faction,
                                  .text = "Hold T1"});
    return mission;
}

} // namespace

SOL_TEST(mission_contest_candidates_need_the_board_to_be_a_party)
{
    ContestWorld world;
    std::vector<sol::sim::ContestCandidate> candidates;

    // The defender's board sees it.
    world.missions.contestCandidates(world.galaxy, world.factions, 0, 0, candidates);
    SOL_REQUIRE(candidates.size() == 1);
    SOL_CHECK(candidates[0].system == 1);
    SOL_CHECK(candidates[0].owner == 0);
    SOL_CHECK(candidates[0].attacker == 1);
    SOL_CHECK(candidates[0].jumps == 1);

    // So does the attacker's, because a station of theirs in reach would pay
    // for the same fight from the other side.
    world.missions.contestCandidates(world.galaxy, world.factions, 0, 1, candidates);
    SOL_CHECK(candidates.size() == 1);

    // A bystander's board does not: a station will not sell work in a war it
    // is not in.
    world.missions.contestCandidates(world.galaxy, world.factions, 0, 2, candidates);
    SOL_CHECK(candidates.empty());
    world.missions.contestCandidates(world.galaxy, world.factions, 0, kNoFaction, candidates);
    SOL_CHECK(candidates.empty());

    // Pressure below the threshold is weather, not a contest, and the board
    // never learns about it.
    world.factions.setContest(1, 1, 0.1f);
    world.missions.contestCandidates(world.galaxy, world.factions, 0, 0, candidates);
    SOL_CHECK(candidates.empty());
}

SOL_TEST(mission_hold_offers_validate_against_a_live_contest)
{
    ContestWorld world;
    std::string error;

    SOL_CHECK(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                       holdOffer(1, 0, 0), &error));
    // Either side of the fight can be named — a defence or an assault.
    SOL_CHECK(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                       holdOffer(1, 1, 1), &error));
    // A bystander cannot be named as the holder, so the board cannot sell a
    // defence of a faction that is not in the fight.
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        holdOffer(1, 2, 0), &error));
    SOL_CHECK(error == "no such contest");
    // Nor a system where nothing is happening.
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        holdOffer(3, 2, 0), &error));
    SOL_CHECK(error == "no such contest");
}

SOL_TEST(mission_hold_completes_when_the_named_side_keeps_the_system)
{
    ContestWorld world;
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         holdOffer(1, 0, 0)));
    SOL_REQUIRE(world.missions.accept(0, 0.0f));
    std::vector<MissionEvent> events;
    world.missions.takeEvents(events);
    events.clear();

    // Someone else's system resolving is not this contract's business.
    world.missions.notifyContestResolved(2, 1);
    world.missions.takeEvents(events);
    SOL_CHECK(events.empty());
    SOL_CHECK(world.missions.active().size() == 1);

    world.missions.notifyContestResolved(1, 0);
    world.missions.takeEvents(events);
    SOL_REQUIRE(events.size() == 2);
    SOL_CHECK(events[0].kind == MissionEventKind::ObjectiveComplete);
    SOL_CHECK(events[1].kind == MissionEventKind::Completed);
    SOL_CHECK(events[1].mission.rewardCredits == 1'200.0);
    SOL_CHECK(world.missions.active().empty());
}

SOL_TEST(mission_hold_lost_is_its_own_event_kind_so_it_can_cost_nothing)
{
    ContestWorld world;
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         holdOffer(1, 0, 0)));
    SOL_REQUIRE(world.missions.accept(0, 0.0f));
    std::vector<MissionEvent> events;
    world.missions.takeEvents(events);
    events.clear();

    // The attacker took it. The player flew the battle and lost it, which is
    // not the same as letting a deadline run out — so it is not Failed, and
    // the game charges no standing for it. Phase 8l recorded this exact
    // unfairness and could not fix it inside its own scope.
    world.missions.notifyContestResolved(1, 1);
    world.missions.takeEvents(events);
    SOL_REQUIRE(events.size() == 1);
    SOL_CHECK(events[0].kind == MissionEventKind::Lost);
    SOL_CHECK(events[0].kind != MissionEventKind::Failed);
    // The penalty travels on the snapshot untouched: the sim states what
    // happened and the game decides what it costs.
    SOL_CHECK(events[0].mission.standingPenalty == 3.0f);
    SOL_CHECK(world.missions.active().empty());
}

SOL_TEST(mission_candidates_come_from_real_shortages_and_raids)
{
    MissionWorld world;

    std::vector<HaulCandidate> hauls;
    world.missions.haulCandidates(world.galaxy, world.economy, 0, 0, hauls);
    SOL_REQUIRE(hauls.size() == 1); // only the raided market is short
    SOL_CHECK(hauls[0].system == 1 && hauls[0].station == 0 && hauls[0].commodity == 0);
    SOL_CHECK(hauls[0].jumps == 1);
    SOL_CHECK(hauls[0].units > 400.0f && hauls[0].units < 460.0f); // gap to 500
    SOL_CHECK(hauls[0].severity > 0.9f);

    // The board's own station never appears even when short...
    world.economy.raidSystem(0, 0.9f);
    world.missions.haulCandidates(world.galaxy, world.economy, 0, 0, hauls);
    SOL_REQUIRE(hauls.size() == 1);
    SOL_CHECK(hauls[0].system == 1);
    // ...but it is a candidate for boards elsewhere.
    world.missions.haulCandidates(world.galaxy, world.economy, 1, 0, hauls);
    SOL_REQUIRE(hauls.size() == 1);
    SOL_CHECK(hauls[0].system == 0);

    std::vector<BountyCandidate> bounties;
    world.missions.bountyCandidates(world.galaxy, world.factions, 0, bounties);
    SOL_REQUIRE(bounties.size() == 1);
    SOL_CHECK(bounties[0].system == 1 && bounties[0].clan == 2);
    SOL_CHECK(bounties[0].intensity == 1.0f);
    SOL_CHECK(bounties[0].jumps == 1);

    // Reach caps candidate enumeration.
    MissionParams shortReach;
    shortReach.candidateReach = 0;
    MissionWorld nearsighted(7, shortReach);
    nearsighted.missions.haulCandidates(nearsighted.galaxy, nearsighted.economy, 0, 0, hauls);
    SOL_CHECK(hauls.empty());
}

SOL_TEST(mission_offers_validate_against_candidates)
{
    MissionWorld world;
    world.missions.openBoard(0, 0);

    // A haul matching the shortage posts; over-sized or fictitious ones don't.
    SOL_CHECK(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                       haulOffer(400.0f)));
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        haulOffer(800.0f))); // more than the gap
    Mission wrongTarget = haulOffer(100.0f);
    wrongTarget.objectives[0].system = 2; // no shortage there
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        wrongTarget));

    // A bounty on the actual raider posts; one on an innocent faction doesn't.
    SOL_CHECK(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                       bountyOffer(3)));
    Mission wrongClan = bountyOffer(3);
    wrongClan.objectives[0].faction = 0;
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        wrongClan));
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        bountyOffer(50))); // over the kill cap

    // Campaign offers skip candidate matching but not range checks, and
    // duplicate ids are refused.
    Mission campaign;
    campaign.title = "Opening move";
    campaign.campaignId = "act1.m1";
    campaign.poster = 0;
    campaign.objectives.push_back({.kind = ObjectiveKind::Dock, .system = 2, .station = 0,
                                   .text = "Dock at S2"});
    campaign.objectives.push_back({.kind = ObjectiveKind::Kill, .system = kAnySystem,
                                   .faction = 2, .kills = 1, .text = "Destroy a raider"});
    SOL_CHECK(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                       campaign));
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        campaign)); // duplicate id
    Mission badRange = campaign;
    badRange.campaignId = "act1.m2";
    badRange.objectives[0].station = 9;
    SOL_CHECK(!world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                        badRange));
    SOL_CHECK(world.missions.offers().size() == 3);

    // openBoard clears; posting needs an open board.
    world.missions.openBoard(1, 0);
    SOL_CHECK(world.missions.offers().empty());
}

SOL_TEST(mission_accept_gates_and_journal_progression)
{
    MissionParams params;
    params.maxActive = 2;
    MissionWorld world(7, params);
    world.missions.openBoard(0, 0);
    Mission gated = haulOffer(400.0f);
    gated.minRep = 10.0f;
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         gated));
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         bountyOffer(2)));

    // The minRep gate holds until standing clears it.
    std::string error;
    SOL_CHECK(!world.missions.accept(0, 0.0f, &error));
    SOL_CHECK(error == "reputation too low");
    SOL_CHECK(world.missions.accept(0, 15.0f));
    SOL_CHECK(world.missions.accept(0, 15.0f)); // the bounty, now index 0
    SOL_CHECK(world.missions.offers().empty());
    SOL_REQUIRE(world.missions.active().size() == 2);

    // Kills only count in the named system, and only the named faction.
    world.missions.notifyKill(2, 0);
    world.missions.notifyKill(0, 1);
    SOL_CHECK(world.missions.active()[1].objectives[0].kills == 2);
    world.missions.notifyKill(2, 1);
    SOL_CHECK(world.missions.active()[1].objectives[0].kills == 1);

    // Partial deliveries persist; the final unit completes the mission.
    SOL_CHECK(world.missions.recordDelivery(0, 0, 0, 100.0f) == 0.0f); // wrong station
    SOL_CHECK(world.missions.recordDelivery(0, 1, 0, 150.0f) == 150.0f);
    SOL_CHECK(world.missions.active()[0].objectives[0].units == 250.0f);
    SOL_CHECK(world.missions.recordDelivery(0, 1, 0, 400.0f) == 250.0f);
    SOL_REQUIRE(world.missions.active().size() == 1); // haul done, bounty remains

    world.missions.notifyKill(2, 1); // last bounty kill
    SOL_CHECK(world.missions.active().empty());

    std::vector<MissionEvent> events;
    world.missions.takeEvents(events);
    SOL_REQUIRE(events.size() == 6);
    SOL_CHECK(events[0].kind == MissionEventKind::Accepted);
    SOL_CHECK(events[1].kind == MissionEventKind::Accepted);
    SOL_CHECK(events[2].kind == MissionEventKind::ObjectiveComplete);
    SOL_CHECK(events[3].kind == MissionEventKind::Completed);
    SOL_CHECK(events[3].mission.title == "Relief run");
    SOL_CHECK(events[3].mission.rewardCredits == 500.0);
    SOL_CHECK(events[5].kind == MissionEventKind::Completed);
    SOL_CHECK(events[5].mission.title == "Bounty: S2 Raiders");
    world.missions.takeEvents(events); // drained
    SOL_CHECK(events.size() == 6);
}

SOL_TEST(mission_campaign_steps_deadlines_and_abandon)
{
    MissionWorld world;
    world.missions.openBoard(0, 0);
    Mission campaign;
    campaign.title = "Shakedown";
    campaign.campaignId = "act1.m1";
    campaign.poster = 0;
    campaign.objectives.push_back({.kind = ObjectiveKind::FlyTo, .system = 0,
                                   .position = {1'000.0, 0.0, 0.0}, .radius = 500.0,
                                   .text = "Fly to the beacon"});
    campaign.objectives.push_back({.kind = ObjectiveKind::Dock, .system = 0, .station = 0,
                                   .text = "Return to the station"});
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         campaign));
    Mission timedHaul = haulOffer(100.0f);
    timedHaul.deadline = 10.0;
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         timedHaul));
    SOL_REQUIRE(world.missions.accept(0, 0.0f));
    SOL_REQUIRE(world.missions.accept(0, 0.0f));

    // FlyTo needs the right system and the radius.
    world.missions.notifyPosition(1, {1'000.0, 0.0, 0.0});
    world.missions.notifyPosition(0, {2'000.0, 0.0, 0.0});
    SOL_CHECK(world.missions.active()[0].currentObjective == 0);
    world.missions.notifyPosition(0, {1'200.0, 0.0, 0.0});
    SOL_CHECK(world.missions.active()[0].currentObjective == 1);
    world.missions.notifyDock(0, 0);
    SOL_REQUIRE(world.missions.active().size() == 1); // campaign complete

    // Deadlines tick only while set; expiry fails the mission.
    world.missions.tick(6.0);
    SOL_CHECK(world.missions.active().size() == 1);
    world.missions.tick(6.0);
    SOL_CHECK(world.missions.active().empty());

    std::vector<MissionEvent> events;
    world.missions.takeEvents(events);
    SOL_REQUIRE(events.size() == 6);
    SOL_CHECK(events[3].kind == MissionEventKind::ObjectiveComplete &&
              events[3].objective == 1);
    SOL_CHECK(events[4].kind == MissionEventKind::Completed &&
              events[4].mission.campaignId == "act1.m1");
    SOL_CHECK(events[5].kind == MissionEventKind::Failed &&
              events[5].mission.standingPenalty == 2.0f);

    // Abandon queues its own consequence.
    world.missions.openBoard(0, 0);
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         haulOffer(100.0f)));
    SOL_REQUIRE(world.missions.accept(0, 0.0f));
    SOL_CHECK(!world.missions.abandon(5));
    SOL_CHECK(world.missions.abandon(0));
    events.clear();
    world.missions.takeEvents(events);
    SOL_REQUIRE(events.size() == 2);
    SOL_CHECK(events[1].kind == MissionEventKind::Abandoned);
}

// Phase 8l: the half of the stolen-bounty defect that lives down here. A
// mission with no deadline is immortal - tick only counts down when one is
// set - so a bounty whose targets are gone held one of four active slots
// forever, and the player's only exit was paying standing to abandon it.
// That behaviour is deliberate and kept; the fix is that init.lua now gives
// bounties a deadline like hauls have always had. This test is what stops
// "deadline 0 should expire" creeping in later and quietly making the real
// fix redundant - and it pins the consequence that matters, which is that
// expiry gives the slot back.
SOL_TEST(mission_without_a_deadline_never_expires_and_expiry_frees_the_slot)
{
    MissionWorld world;
    world.missions.openBoard(0, 0);
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         bountyOffer(2)));
    SOL_REQUIRE(world.missions.accept(0, 0.0f));
    SOL_REQUIRE(world.missions.active().size() == 1);
    SOL_CHECK(world.missions.active()[0].deadline == 0.0);

    // An hour of sim time and it has not moved.
    for (int i = 0; i < 360; ++i) {
        world.missions.tick(10.0);
    }
    SOL_REQUIRE(world.missions.active().size() == 1);
    std::vector<MissionEvent> events;
    world.missions.takeEvents(events);
    SOL_REQUIRE(events.size() == 1); // accepted, and nothing whatsoever since
    SOL_CHECK(events[0].kind == MissionEventKind::Accepted);

    // The same contract with a clock on it leaves on its own.
    world.missions.openBoard(0, 0);
    Mission timed = bountyOffer(2);
    timed.deadline = 60.0;
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions, timed));
    SOL_REQUIRE(world.missions.accept(0, 0.0f));
    SOL_REQUIRE(world.missions.active().size() == 2);
    world.missions.tick(59.0);
    SOL_CHECK(world.missions.active().size() == 2);
    world.missions.tick(2.0);
    SOL_REQUIRE(world.missions.active().size() == 1);
    SOL_CHECK(world.missions.active()[0].deadline == 0.0); // the immortal one remains

    events.clear();
    world.missions.takeEvents(events);
    SOL_REQUIRE(events.size() == 2);
    SOL_CHECK(events[1].kind == MissionEventKind::Failed);
    SOL_CHECK(events[1].mission.standingPenalty == 2.0f);
}

SOL_TEST(mission_save_load_round_trips_exactly)
{
    MissionWorld world;
    world.missions.openBoard(0, 0);
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         haulOffer(400.0f)));
    Mission timedBounty = bountyOffer(3);
    timedBounty.deadline = 300.0;
    SOL_REQUIRE(world.missions.postOffer(world.galaxy, world.economy, world.factions,
                                         timedBounty));
    SOL_REQUIRE(world.missions.accept(1, 0.0f)); // the bounty
    world.missions.notifyKill(2, 1);             // progress: 2 kills left
    world.missions.setCampaignStage(2);
    SOL_CHECK(world.missions.tickBoard(200.0)); // refresh due, accumulator reset

    sol::core::BinaryWriter writer;
    world.missions.save(writer);
    MissionWorld restored(999); // wrong seed on purpose; load restores the rng
    const std::span<const std::byte> bytes(writer.data());
    sol::core::BinaryReader reader(bytes);
    SOL_REQUIRE(restored.missions.load(reader));

    SOL_REQUIRE(restored.missions.offers().size() == 1);
    SOL_CHECK(restored.missions.offers()[0].title == "Relief run");
    SOL_CHECK(restored.missions.offers()[0].objectives[0].units == 400.0f);
    SOL_REQUIRE(restored.missions.active().size() == 1);
    SOL_CHECK(restored.missions.active()[0].objectives[0].kills == 2);
    SOL_CHECK(restored.missions.active()[0].deadline == 300.0);
    SOL_CHECK(restored.missions.active()[0].objectives[0].text ==
              "Destroy raiders in S1");
    SOL_CHECK(restored.missions.campaignStage() == 2);
    SOL_CHECK(restored.missions.boardSystem() == 0);
    SOL_CHECK(restored.missions.boardStation() == 0);

    // Identical trajectories after the restore: rolls and deadlines agree.
    for (int i = 0; i < 8; ++i) {
        SOL_CHECK(world.missions.boardRoll() == restored.missions.boardRoll());
        world.missions.tick(50.0);
        restored.missions.tick(50.0);
        SOL_CHECK(world.missions.active().size() == restored.missions.active().size());
    }

    // A layout mismatch refuses to load (galaxy/defs changed under the save).
    MissionSim mismatched;
    mismatched.initialize(world.galaxy, MissionParams{}, 4, 1, 1);
    sol::core::BinaryReader again(std::span<const std::byte>(writer.data()));
    SOL_CHECK(!mismatched.load(again));
}
