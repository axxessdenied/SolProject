-- Base-game boot script (mod zero). Runs at startup and re-runs live when
-- saved (Phase 5 hot-reload); on_tick is called every sim tick (60 Hz).
--
-- Console (F1) evaluates Lua in this same VM. API surface, "sol" table:
--   sol.spawn_ship(id)              spawn a def-driven ship ahead of the player
--   sol.spawn_pilot(id, role)       ...with an AI pilot: fighter/trader/patrol
--   sol.spawn_pilot_faction(id, role, faction)  ...sworn to sol.factions() index (1-based)
--   sol.pilot_attack_player(ship)   pilot commands (ship = entity handle)
--   sol.pilot_engage_enemy(ship)    attack nearest war enemy / hostile player
--   sol.pilot_flee(ship) / sol.pilot_idle(ship)
--   sol.pilot_patrol_offset(ship, dx, dy, dz)  waypoint relative to station
--   sol.pilot_hull(ship)            hull fraction 0..1
--   sol.factions() / sol.rep()      faction table / player standings (Phase 8b)
--   sol.relations() / sol.raids()   non-neutral pairs + wars / warm raid sites
--   sol.set_rep(faction, value)     dev cheat: set a standing (-100..100)
--   sol.faction_candidates(faction) raid options "system:name:relation;..."
--   sol.faction_raid(faction, system)  commit one (validated against reach)
--   sol.ships() / sol.target_name() / sol.target_distance() / sol.speed()
--   sol.entity_count()              live sim entities
--   sol.mission_board() / sol.missions()   offers at the docked station / journal
--   sol.accept_mission(i) / sol.abandon_mission(i) / sol.track_mission(i)
--   sol.mission_candidates()        raw shortage/bounty candidates (dev)
--   sol.campaign_stage() / sol.set_campaign_stage(n)  spine progress (dev)
--   sol.mission_begin/deadline/min_rep/obj_*/post     board-hook builder
--   sol.knowledge() / sol.signals()  what is known / found sites here (Phase 8e)
--   sol.pulse() / sol.scan()        fire a scan pulse / resolve the target (dev)
--   sol.salvage()                   empty the nearest resolved site in range
--   sol.survey_ledger() / sol.sell_survey()   unsold scan data / sell it here
--   sol.route(systemName) / sol.chart(systemName)  plot a route / dev cheat
--   sol.set_loot(cargo, credits, module)      inside signal_loot / wreck_loot
--   sol.fields() / sol.rocks(n)     asteroid fields here / one field's rocks (Phase 8f)
--   sol.wrecks() / sol.mine()       known wrecks / cut what the nose is on (dev)
--   sol.target(namePart)            select any nav target by name
--   sol.warp_rock()                 dev: park just off the nearest rock, nose on it
--   sol.refine(units) / sol.collect()         order refining here / take the output
--   sol.refine_jobs()               outstanding refinery orders, anywhere

local announceInterval = 20.0 -- seconds; edit + save to see hot-reload
local timer = 0.0

function on_tick(dt)
    timer = timer + dt
    if timer >= announceInterval then
        timer = 0.0
        print(string.format("[nav] %s: %.0f km out, speed %.1f m/s",
            sol.target_name(), sol.target_distance() / 1000.0, sol.speed()))
    end
end

-- NPC pilot strategy (engine plan: Lua decides states, C++ steering flies
-- them). Called at 2 Hz per pilot with role, state, and the player's
-- attitude toward the pilot's faction ("hostile"/"neutral"/"friendly", or
-- "none" for unaffiliated console spawns, which stay player-hostile).
local patrolLegs = {
    { 2000.0,  400.0,     0.0},
    {    0.0,  400.0, -2000.0},
    {-2000.0,  400.0,     0.0},
    {    0.0,  400.0,  2000.0},
}
local patrolLeg = {}   -- per-ship current leg (keyed by handle)
local traderLegs = {
    {    0.0, -200.0,  3000.0},   -- "planet-side" run
    {  500.0,  200.0,  -600.0},   -- back to the station approach
}
local traderLeg = {}

local function nextLeg(ship, legs, memory)
    local key = tostring(ship) -- handles are fresh userdata each call; the
    local index = (memory[key] or 0) % #legs + 1 -- string form is stable
    memory[key] = index
    local leg = legs[index]
    sol.pilot_patrol_offset(ship, leg[1], leg[2], leg[3])
end

function pilot_think(ship, role, state, attitude)
    local hull = sol.pilot_hull(ship)
    if role == "fighter" then
        -- Raiders: fight the local war (patrols) first; failing that, any
        -- player their clan doesn't consider a friend is prey.
        if hull < 0.3 then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state ~= "attack" then
            if not sol.pilot_engage_enemy(ship) and attitude ~= "friendly" then
                sol.pilot_attack_player(ship)
            end
        end
    elseif role == "patrol" then
        -- Keep the diamond around the station, but answer war enemies and
        -- hostile players (C++ picks the target from the relations matrix).
        if hull < 0.4 then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state ~= "attack" and sol.pilot_engage_enemy(ship) then
            -- engaged; nothing else to decide this think
        elseif state == "idle" then
            nextLeg(ship, patrolLegs, patrolLeg)
        end
    elseif role == "trader" then
        if hull < 0.6 then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state == "idle" then
            nextLeg(ship, traderLegs, traderLeg)
        end
    end
end

-- Faction strategy (Phase 8b; engine plan 2.8: decision rules live in Lua).
-- Called once per faction per decision tick with its personality and one
-- seeded roll — the only entropy, so the rule stays a pure function.
-- Policy: pirates raid whoever they hate most; majors direct their raids at
-- rival MAJORS when one is in reach (that is how cold rivalries become
-- wars), falling back to clan suppression sweeps otherwise.
function faction_think(faction, name, pirate, aggression, forgiveness, roll)
    if roll >= aggression then
        return -- staying home this cycle
    end
    local candidates = sol.faction_candidates(faction)
    if candidates == "" then
        return -- nobody in reach worth hitting
    end
    local bestSystem, bestName, bestRelation
    local bestMajorSystem, bestMajorName, bestMajorRelation
    for entry in string.gmatch(candidates, "[^;]+") do
        local system, target, relation, kind =
            string.match(entry, "^(%d+):(.-):(%-?[%d%.]+):(%a)$")
        relation = tonumber(relation)
        if relation ~= nil then
            if bestRelation == nil or relation < bestRelation then
                bestRelation, bestSystem, bestName = relation, tonumber(system), target
            end
            if kind == "m" and (bestMajorRelation == nil or relation < bestMajorRelation) then
                bestMajorRelation, bestMajorSystem, bestMajorName =
                    relation, tonumber(system), target
            end
        end
    end
    if not pirate and bestMajorSystem ~= nil then
        bestSystem, bestName, bestRelation = bestMajorSystem, bestMajorName, bestMajorRelation
    end
    if bestSystem ~= nil and sol.faction_raid(faction, bestSystem) then
        print(string.format("[factions] %s raids %s (relation %.0f)",
            name, bestName, bestRelation))
    end
end

-- Mission board (Phase 8c). Called when the player docks (and on the docked
-- refresh cadence) with candidates enumerated from live sim state — market
-- shortages and warm raid sites — plus one seeded roll, the only entropy.
-- The C++ side validates every post against the same candidates, so this
-- policy only decides selection and pricing. Campaign offers ride the same
-- board via campaign_offer (scripts/campaign.lua, loaded after init).
--
-- Haul entry:   system:station:commodityId:units:severity:jumps:sysName:stName
-- Bounty entry: system:clan:intensity:jumps:systemName:clanName
function mission_board(stationName, owner, ownerName, ownerPirate, hauls, bounties, roll)
    if campaign_offer ~= nil and not ownerPirate then
        campaign_offer(owner, ownerName)
    end

    -- Haul contracts: worst shortages first, up to three. The contract asks
    -- for a playable slice of the gap; pay scales with volume, distance, and
    -- how empty the shelves are; the worst offers are rep-gated tiers.
    local haulList = {}
    for entry in string.gmatch(hauls, "[^;]+") do
        local system, station, commodity, units, severity, jumps, sysName, stName =
            string.match(entry, "^(%d+):(%d+):([^:]+):([%d%.]+):([%d%.]+):(%d+):([^:]+):([^:]+)$")
        if system ~= nil then
            haulList[#haulList + 1] = {
                system = tonumber(system), station = tonumber(station),
                commodity = commodity, units = tonumber(units),
                severity = tonumber(severity), jumps = tonumber(jumps),
                sysName = sysName, stName = stName,
            }
        end
    end
    table.sort(haulList, function(a, b) return a.severity > b.severity end)
    for i = 1, math.min(3, #haulList) do
        local h = haulList[i]
        local units = math.floor(math.min(h.units, 30 + 25 * h.jumps))
        local reward = math.floor(units * (6 + 4 * h.jumps) * (1 + h.severity)
                                  * (0.9 + 0.2 * roll))
        if sol.mission_begin(string.format("Haul: %d %s to %s", units,
                                           string.gsub(h.commodity, "^sol%.", ""), h.stName),
                             owner, reward, 2 + h.jumps, 2, "") then
            sol.mission_deadline(480 + 420 * h.jumps)
            if h.severity > 0.92 and units > 60 then
                sol.mission_min_rep(10) -- the best contracts go to trusted pilots
            end
            sol.mission_obj_deliver(h.system, h.station, h.commodity, units,
                                    string.format("Deliver %d %s to %s (%s)", units,
                                                  string.gsub(h.commodity, "^sol%.", ""),
                                                  h.stName, h.sysName))
            sol.mission_post()
        end
    end

    -- Bounties: the warmest raid sites, up to two. Kill counts scale with
    -- the standing raid intensity.
    local bountyList = {}
    for entry in string.gmatch(bounties, "[^;]+") do
        local system, clan, intensity, jumps, sysName, clanName =
            string.match(entry, "^(%d+):(%d+):([%d%.]+):(%d+):([^:]+):([^:]+)$")
        if system ~= nil then
            bountyList[#bountyList + 1] = {
                system = tonumber(system), clan = tonumber(clan),
                intensity = tonumber(intensity), jumps = tonumber(jumps),
                sysName = sysName, clanName = clanName,
            }
        end
    end
    table.sort(bountyList, function(a, b) return a.intensity > b.intensity end)
    for i = 1, math.min(2, #bountyList) do
        local b = bountyList[i]
        local kills = math.min(2 + math.floor(b.intensity), 4)
        local reward = math.floor(kills * (350 + 150 * b.jumps + 120 * b.intensity)
                                  * (0.9 + 0.2 * roll))
        if sol.mission_begin(string.format("Bounty: %s in %s", b.clanName, b.sysName),
                             owner, reward, 3 + b.jumps, 2, "") then
            -- A bounty needs a clock for the same reason a haul does, and for
            -- one more: raiders are spawned by raid intensity, not placed, so
            -- a system that goes quiet can leave a contract with no remaining
            -- targets. Without a deadline that mission is unfinishable AND
            -- unexpiring, holding one of four active slots until the player
            -- pays standing to abandon it. Time to cross, hunt, and re-hunt.
            sol.mission_deadline(600 + 420 * b.jumps + 240 * kills)
            sol.mission_obj_kill(b.clan, kills, b.system,
                                 string.format("Destroy %d %s raiders in %s", kills,
                                               b.clanName, b.sysName))
            sol.mission_post()
        end
    end
end

print("init.lua ready - ships: " .. sol.ships())

-- Exploration (Phase 8e). C++ resolves a site and fills in a default table
-- first, then calls this with the site's own seeded roll — the only entropy,
-- so what a wreck holds is fixed by the world seed rather than by when you
-- happened to scan it. sol.set_loot(cargo, credits, module) replaces the
-- default; returning without calling it keeps the C++ table.
local derelictCargo = {"sol.machinery", "sol.ore", "sol.machinery"}
local cacheCargo = {"sol.machinery", "sol.food"}

function signal_loot(kind, system, region, roll)
    local depth = (region == "fringe" and 2.0) or (region == "frontier" and 1.4) or 1.0
    if kind == "Cache" then
        local commodity = cacheCargo[1 + math.floor(roll * #cacheCargo) % #cacheCargo]
        local units = math.floor((6 + 14 * roll) * depth)
        sol.set_loot(string.format("%s:%d", commodity, units),
                     math.floor((150 + 900 * roll) * depth), "")
    else
        local commodity = derelictCargo[1 + math.floor(roll * #derelictCargo) % #derelictCargo]
        local units = math.floor((8 + 18 * roll) * depth)
        -- A quarter of derelicts in real frontier space still have a module
        -- bolted on; the scanner is the one worth flying out for.
        local salvage = (roll > 0.75 and region ~= "core") and "sol.survey_scanner_mk1" or ""
        sol.set_loot(string.format("%s:%d", commodity, units), 0, salvage)
    end
end

function signal_found(kind, system)
    print(string.format("[scan] contact in %s", system))
end

-- Salvage (Phase 8f). Same shape as signal_loot, for a hull that died where
-- you could see it: C++ composes a default from the ship that actually died,
-- then calls this with the wreck's own seeded roll. Once the beam has been
-- into the hull nothing can rewrite it, so this is the only moment.
function wreck_loot(shipDef, system, faction, roll)
    -- Pirates carry what they took; everyone else carries what they were
    -- hauling. Scrap is the hull, and C++ has already valued that.
    local pirateHaul = {"sol.metal", "sol.machinery"}
    if faction ~= "" and roll > 0.6 then
        local commodity = pirateHaul[1 + math.floor(roll * #pirateHaul) % #pirateHaul]
        sol.set_loot(string.format("sol.ore:%d,%s:%d", math.floor(6 + 10 * roll),
                                   commodity, math.floor(2 + 8 * roll)),
                     math.floor(80 + 400 * roll), "")
    end
end

function rock_mined(commodity, units)
    print(string.format("[mining] rock finished: %.0f units of %s", units, commodity))
end
