-- Base-game boot script (mod zero). Runs at startup and re-runs live when
-- saved (Phase 5 hot-reload); on_tick is called every sim tick (60 Hz).
--
-- Console (F1) evaluates Lua in this same VM. API surface, "sol" table:
--   sol.spawn_ship(id)      spawn a def-driven ship ahead of the player
--   sol.ships()             comma-separated ship def ids
--   sol.target_name()       current nav target
--   sol.target_distance()   meters to it
--   sol.speed()             player speed, m/s
--   sol.entity_count()      live sim entities

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

print("init.lua ready - ships: " .. sol.ships())
