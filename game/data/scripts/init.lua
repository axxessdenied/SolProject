-- Base-game boot script (mod zero). Runs at startup and re-runs live when
-- saved (Phase 5 hot-reload); on_tick is called every sim tick (60 Hz).
--
-- Console (F1) evaluates Lua in this same VM. API surface, "sol" table:
--   sol.spawn_ship(id)              spawn a def-driven ship ahead of the player
--   sol.spawn_pilot(id, role)       ...with an AI pilot: fighter/trader/patrol
--   sol.pilot_attack_player(ship)   pilot commands (ship = entity handle)
--   sol.pilot_flee(ship) / sol.pilot_idle(ship)
--   sol.pilot_patrol_offset(ship, dx, dy, dz)  waypoint relative to station
--   sol.pilot_hull(ship)            hull fraction 0..1
--   sol.ships()                     comma-separated ship def ids
--   sol.target_name()               current nav target
--   sol.target_distance()           meters to it
--   sol.speed()                     player speed, m/s
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
-- them). Called at 2 Hz per pilot with the current role and state.
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

function pilot_think(ship, role, state)
    local hull = sol.pilot_hull(ship)
    if role == "fighter" then
        if hull < 0.3 then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state ~= "attack" then
            sol.pilot_attack_player(ship)
        end
    elseif role == "patrol" then
        -- Fly the diamond around the station; new leg each think while close
        -- to the current waypoint is handled by just cycling on idle.
        if state == "idle" then nextLeg(ship, patrolLegs, patrolLeg) end
        if state == "patrol" and hull < 0.5 then sol.pilot_flee(ship) end
    elseif role == "trader" then
        if hull < 0.6 then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state == "idle" then
            nextLeg(ship, traderLegs, traderLeg)
        end
    end
end

print("init.lua ready - ships: " .. sol.ships())
