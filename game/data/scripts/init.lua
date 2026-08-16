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

print("init.lua ready - ships: " .. sol.ships())
