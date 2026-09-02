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
--   sol.territory() / sol.contest(sys)  who holds what, and live contests
--   sol.set_contest(sys, faction, p) / sol.flip(sys, faction)   dev levers
--   sol.ships() / sol.target_name() / sol.target_distance() / sol.speed()
--   sol.entity_count()              live sim entities
--   sol.mission_board() / sol.missions()   offers at the docked station / journal
--   sol.accept_mission(i) / sol.abandon_mission(i) / sol.track_mission(i)
--   sol.mission_candidates()        raw shortage/bounty candidates (dev)
--   sol.campaign_stage() / sol.set_campaign_stage(n)  spine progress (dev)
--   sol.mission_begin/deadline/min_rep/obj_*/post     board-hook builder
--   sol.mission_lead()              post the draft as a room lead (bar_lead)
--   sol.knowledge() / sol.signals()  what is known / found sites here (Phase 8e)
--   sol.pulse() / sol.scan()        fire a scan pulse / resolve the target (dev)
--   sol.salvage()                   empty the nearest resolved site in range
--   sol.survey_ledger() / sol.sell_survey()   unsold scan data / sell it here
--   sol.route(systemName) / sol.chart(systemName)  plot a route / dev cheat
--   sol.set_loot(cargo, credits, component)      inside signal_loot / wreck_loot
--   sol.fields() / sol.rocks(n)     asteroid fields here / one field's rocks (Phase 8f)
--   sol.wrecks() / sol.mine()       known wrecks / cut what the nose is on (dev)
--   sol.target(namePart)            select any nav target by name
--   sol.warp_rock()                 dev: park just off the nearest rock, nose on it
--   sol.refine(units) / sol.collect()         order refining here / take the output
--   sol.refine_jobs()               outstanding refinery orders, anywhere
--   sol.request_dock()              hail the nearest station for a berth (Phase 8r)
--   sol.clearance() / sol.berths(i) what you are cleared for / a station's ports
--   sol.grant_docking(berth, msg) / sol.deny_docking(msg)   inside dock_request
--   sol.dock()                      dev: dock at once, asking nobody
--   sol.inspect_me()                dev: be stopped now by the nearest patrol
--   sol.add_cargo(id, units)        dev: put a crate in the hold (negative removes)
--   sol.inspection() / sol.hold()   the last stop's ruling / what the law here
--                                   says about what is actually in your hold
--   sol.inspection_pass(msg) / sol.inspection_fine(cr, msg)
--   sol.inspection_seize(bounty, msg)        inside inspection_verdict
--   sol.hail() / sol.hail_target(namePart)   talk to a ship (Phase 8s)
--   sol.tips()                      rumours and remembered prices, with ages
--   sol.hail_reply(msg) / sol.hail_tip_market(msg) / sol.hail_tip_place(msg)
--                                   inside pilot_hail

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
-- them). Called at 2 Hz per pilot with role, state, the player's attitude
-- toward the pilot's faction ("hostile"/"neutral"/"friendly", or "none" for
-- unaffiliated console spawns, which stay player-hostile), and whether that
-- faction is a pirate clan (Phase 8x: what a raider came out for).
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

function pilot_think(ship, role, state, attitude, pirate)
    local hull = sol.pilot_hull(ship)
    if role == "fighter" then
        -- Raiders: answer whatever is shooting at you, then decide what you
        -- came out for. A clan came for cargo, so a hauler outranks the local
        -- war; a faction's fighters came for the war, so a freighter is what
        -- they take when there is no enemy fleet to fight. Failing all of
        -- that, any player their side doesn't consider a friend is prey.
        if hull < 0.3 then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state ~= "attack" then
            local busy = sol.pilot_under_fire(ship) and sol.pilot_engage_threat(ship)
            if not busy and pirate then busy = sol.pilot_hunt_trader(ship) end
            if not busy then busy = sol.pilot_engage_enemy(ship) end
            if not busy and not pirate then busy = sol.pilot_hunt_trader(ship) end
            if not busy and attitude ~= "friendly" then
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
    elseif role == "covert" then
        -- Phase 37 stage D. A covert raider came for the cargo and nothing
        -- else: it never joins the local war, and it breaks off at 0.7 hull
        -- rather than 0.3.
        --
        -- The threshold is the whole character. A fighter at 0.3 has already
        -- lost most of a hull to win the exchange; this one leaves with 70% of
        -- its own still on, which reads from the cockpit as a ship that decides
        -- the trade is not worth it and goes. It is the trader's rule pointed
        -- the other way: a hauler runs because it cannot fight, and this runs
        -- because it would rather not be identified.
        --
        -- No `pilot_engage_enemy`, and that is the line that makes it a
        -- different pilot rather than a timid fighter: a clan's war with the
        -- Navy is not what a hull bought from a fence came out to do. It will
        -- answer something already shooting at it, take a hauler if one is
        -- there, and otherwise leave the player alone unless the player is
        -- hostile to it.
        if hull < 0.7 then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state ~= "attack" then
            local busy = sol.pilot_under_fire(ship) and sol.pilot_engage_threat(ship)
            if not busy then busy = sol.pilot_hunt_trader(ship) end
            if not busy and attitude == "hostile" then
                sol.pilot_attack_player(ship)
            end
        end
    elseif role == "trader" then
        -- A hauler is not a warship. It runs the moment something shoots at
        -- it rather than waiting to lose 40% of its hull first: shields take
        -- the first hits, so a hull-only rule means a freighter flies calmly
        -- through the whole opening exchange. Once the shooting stops it goes
        -- idle, and a puppet's own record puts it back on its lane.
        if hull < 0.6 or sol.pilot_under_fire(ship) then
            if state ~= "flee" then sol.pilot_flee(ship) end
        elseif state == "flee" then
            sol.pilot_idle(ship)
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
-- Contest entry: system:owner:attacker:pressure:jumps:sysName:ownerName:attackerName
-- Escort entry: trader:system:station:commodityId:cargo:danger:jumps:sysName:stName
function mission_board(stationName, owner, ownerName, ownerPirate, hauls, bounties,
                       contests, escorts, roll)
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

    -- War contracts (Phase 8u): the board only ever hears about contests its
    -- own faction is a party to, so this decides which SIDE it is on. The
    -- fight nearest to being lost is posted first, at most one at a time -
    -- a war is meant to feel like one thing happening, not a menu.
    local contestList = {}
    for entry in string.gmatch(contests, "[^;]+") do
        local system, holder, attacker, pressure, jumps, sysName, holderName, attackerName =
            string.match(entry,
                         "^(%d+):(%d+):(%d+):([%d%.]+):(%d+):([^:]+):([^:]+):([^:]+)$")
        if system ~= nil then
            contestList[#contestList + 1] = {
                system = tonumber(system), holder = tonumber(holder),
                attacker = tonumber(attacker), pressure = tonumber(pressure),
                jumps = tonumber(jumps), sysName = sysName,
                holderName = holderName, attackerName = attackerName,
            }
        end
    end
    table.sort(contestList, function(a, b) return a.pressure > b.pressure end)
    if #contestList > 0 then
        local c = contestList[1]
        -- Which side this station is on is not a choice: it is whichever of
        -- the two factions signs the pay. Defending is the common case;
        -- an attacker's own station in reach posts the assault instead.
        local defending = c.holder == owner
        local side = defending and c.holder or c.attacker
        local sideName = defending and c.holderName or c.attackerName
        local title = defending
            and string.format("Defend %s", c.sysName)
            or string.format("Assault %s", c.sysName)
        local text = defending
            and string.format("Break the %s push on %s", c.attackerName, c.sysName)
            or string.format("Take %s from %s", c.sysName, c.holderName)
        -- Pays better than a bounty: it is the same shooting with the outcome
        -- attached, and it can be lost through no fault of the pilot.
        local reward = math.floor((1400 + 500 * c.jumps) * (1 + c.pressure)
                                  * (0.9 + 0.2 * roll))
        -- The penalty is real and it is charged on expiry or abandonment.
        -- Losing the battle is what costs nothing, and that is decided by
        -- the event kind in C++, not by zeroing the number here.
        if sol.mission_begin(title, side, reward, 8, 2, "") then
            -- A contest resolves on its own clock, so the deadline is a
            -- backstop against a stalemate rather than the real pressure.
            sol.mission_deadline(1200 + 600 * c.jumps)
            -- Gate the desperate ones, not the ordinary ones. There is at
            -- most ONE war contract on a board, so a rep gate on the common
            -- case hides the whole feature from a new pilot - which is what
            -- gating a local contest at rep 5 did, since standings start at 0.
            if c.pressure > 0.8 then
                sol.mission_min_rep(10) -- a last stand goes to someone known
            end
            sol.mission_obj_hold(c.system, side, text)
            sol.mission_post()
        end
    end

    -- Escort contracts (Phase 8x): a hauler is leaving this system RIGHT NOW,
    -- and the board will pay for somebody to fly with it. At most one, for the
    -- same reason the war contract is: it is one ship, leaving once.
    local escortList = {}
    for entry in string.gmatch(escorts, "[^;]+") do
        local trader, system, station, commodity, cargo, danger, jumps, sysName, stName =
            string.match(entry,
                         "^(%d+):(%d+):(%d+):([^:]+):([%d%.]+):([%d%.]+):(%d+):([^:]+):([^:]+)$")
        if trader ~= nil then
            escortList[#escortList + 1] = {
                trader = tonumber(trader), system = tonumber(system),
                station = tonumber(station), commodity = commodity,
                cargo = tonumber(cargo), danger = tonumber(danger),
                jumps = tonumber(jumps), sysName = sysName, stName = stName,
            }
        end
    end
    -- Laden first, then by danger: a full hold is the better story and the
    -- bigger loss, but it is a PREFERENCE and not a rule. ⚑ Requiring cargo
    -- was the first version and a drive killed it in one screenshot: both
    -- haulers leaving the player's own start station were deadheading toward a
    -- producer (Phase 8x stage 1's convergence finding), so the laden test
    -- posted nothing at all where a new pilot would first look. That is 8u's
    -- rep-gate mistake again - a filter on the only contract of its kind is a
    -- lockout, not a difficulty curve.
    table.sort(escortList, function(a, b)
        if (a.cargo > 0) ~= (b.cargo > 0) then return a.cargo > 0 end
        return a.danger > b.danger
    end)
    for i = 1, #escortList do
        local e = escortList[i]
        -- The one rule left, and it is about not selling work that isn't work:
        -- a run to a quiet system is one the sim will not threaten, so paying
        -- for it would be paying a pilot to watch a schedule tick over.
        if e.danger > 0.05 then
            -- Pays on the danger, because that is exactly what the escort is
            -- buying: attrition never rolls in the system the player is
            -- standing in, so a pilot who actually flies it turns the odds off.
            -- Cargo is worth something on top - an empty hull is a hull, and a
            -- full one is a hull plus everything the station is waiting for.
            local reward = math.floor((300 + 2200 * e.danger + 250 * e.jumps + 3 * e.cargo)
                                      * (0.9 + 0.2 * roll))
            local title = e.cargo > 0
                and string.format("Escort: %s run to %s",
                                  string.gsub(e.commodity, "^sol%.", ""), e.sysName)
                or string.format("Escort: empty run to %s", e.sysName)
            if sol.mission_begin(title, owner, reward, 3 + e.jumps, 2, "") then
                -- The haul's own clock is 90 s a leg plus 20 s a gate, and a
                -- hauler under fire stops counting down while it fights, so
                -- the deadline is a backstop with room for a real ambush in it.
                sol.mission_deadline(300 + 120 * e.jumps)
                -- No rep gate. There is at most ONE of these on a board and
                -- gating the only contract of its kind hides the whole feature
                -- from a new pilot - the mistake Phase 8u made with its war
                -- contract and had to undo.
                -- No system in parentheses: the HUD line appends the
                -- destination itself, so naming it here printed it twice AND
                -- pushed the journal row past the width of its column.
                sol.mission_obj_escort(e.trader, e.system,
                                       string.format("Keep hauler #%d alive to %s",
                                                     e.trader, e.stName))
                sol.mission_post()
            end
            break
        end
    end
end

print("init.lua ready - ships: " .. sol.ships())

-- Exploration (Phase 8e). C++ resolves a site and fills in a default table
-- first, then calls this with the site's own seeded roll — the only entropy,
-- so what a wreck holds is fixed by the world seed rather than by when you
-- happened to scan it. sol.set_loot(cargo, credits, component) replaces the
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
        -- A quarter of derelicts in real frontier space still have a component
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
    --
    -- The scrap line is "sol.salvage" since Phase 33 stage C, and it had to move
    -- with the C++ default rather than after it: this hook REPLACES the whole
    -- table for four faction wrecks in ten, so a re-point that changed only
    -- defaultWreckLoot would have left 40% of the galaxy's kills still paying
    -- out raw ore - the bug looking exactly like a rounding difference.
    local pirateHaul = {"sol.metal", "sol.machinery"}
    if faction ~= "" and roll > 0.6 then
        local commodity = pirateHaul[1 + math.floor(roll * #pirateHaul) % #pirateHaul]
        sol.set_loot(string.format("sol.salvage:%d,%s:%d", math.floor(6 + 10 * roll),
                                   commodity, math.floor(2 + 8 * roll)),
                     math.floor(80 + 400 * roll), "")
    end
end

function rock_mined(commodity, units)
    print(string.format("[mining] rock finished: %.0f units of %s", units, commodity))
end

-- Docking clearance (Phase 8r). C++ hands over everything the dispatcher can
-- know — who is asking, whose station it is, how they feel about you, how many
-- berths there are, and one seeded roll — and this decides. Answer with exactly
-- one of sol.grant_docking(berth, message) or sol.deny_docking(message); say
-- nothing and C++ falls back to its own rule, which grants unless you are
-- hostile. The roll is the only entropy, so this stays a pure function.
--
-- ⚑ `dark` (Phase 36 stage A) is whether the ship asking has its transponder
-- off. A station will not clear a contact that will not identify itself, and
-- that is the phase's first ruling rather than a flavour choice: without a
-- price paid BEFORE you are ever caught, running dark is strictly dominant and
-- nobody ever switches it back on. C++ enforces the same rule when no script
-- answers, so deleting this branch makes stations chattier, not laxer.
function dock_request(station, owner, standing, berths, hostile, roll, dark)
    if dark then
        sol.deny_docking("Unidentified contact. Squawk or stay out.")
        return
    end
    -- Keep every line short: the comms panel's sender column already says who
    -- is talking, and a line that repeats the station's name is the line that
    -- runs off the end of the panel.
    if hostile then
        sol.deny_docking(string.format("Denied. Leave %s space.", owner))
        return
    end
    -- Berth 1 is the near pad and it goes to people they like. Everyone else
    -- gets sent round the ring, which is the difference a standing bar makes
    -- that a number on a screen does not.
    local berth
    if standing >= 25 then
        berth = 1
        sol.grant_docking(berth, string.format(
            "Good to see you again. Berth %d, the close one.", berth))
    elseif standing <= -15 then
        berth = 2 + math.floor(roll * (berths - 1)) % (berths - 1)
        sol.grant_docking(berth, string.format(
            "Cleared, berth %d. Keep it slow.", berth))
    else
        berth = 1 + math.floor(roll * berths) % berths
        sol.grant_docking(berth, string.format(
            "Cleared for berth %d. Mind your approach.", berth))
    end
end

-- The verdict (Phase 36 stage D). A patrol has finished reading your hold - or
-- you left before it could - and this decides what the local law does about it.
-- C++ hands over everything the officer can know and this rules. Answer with
-- exactly one of:
--
--   sol.inspection_pass(message)           waved on, nothing taken
--   sol.inspection_fine(credits, message)  a bill, capped at what you have
--   sol.inspection_seize(bounty, message)  take the contraband, post that price
--
-- Say nothing and C++ falls back to its own law, which is the same law: seize
-- and post contraband, charge duty on restricted, wave a clean hold through,
-- and put a price on anybody who ran. Deleting this function does not make the
-- galaxy lawless - it makes it less talkative.
--
-- ⚑⚑ `legality` is one of "unpoliced" / "legal" / "restricted" / "contraband",
-- and it is the WORST thing aboard rather than a list: `units` and `value` are
-- how much of THAT tier is in the hold. `hasLaw` is false when this jurisdiction
-- keeps no table at all - the Freight Guild holds a quarter of the galaxy and
-- declares nothing illegal - which is a different fact from a clean hold and is
-- the one line most worth writing differently.
--
-- ⚑ `outcome` is "complied" or "ran", and nothing else ever reaches here: a
-- stop that lapsed or was broken off never read anything, so there is nothing
-- to rule on. A seizure on a "ran" takes no cargo - the ship is gone - and
-- means "post the price anyway", which is the design: running is its own
-- offence whatever was in the hold.
--
-- Keep every line short for the reason dock_request does: the comms panel clips
-- at about fifty characters and the sender column is already spending some.
function inspection_verdict(who, reason, outcome, legality, commodity, units,
                            value, hasLaw, standing, credits, roll)
    if outcome == "ran" then
        sol.inspection_seize(400, "You ran. There's a price on you now.")
        return
    end
    if legality == "contraband" then
        -- A regular does not get a discount on this, and that is the point:
        -- the seizure is what the phase is FOR. What standing buys is the
        -- wording, which is all a faction has left to offer once it has taken
        -- your cargo.
        if standing >= 25 then
            sol.inspection_seize(math.max(250, 25 * units),
                "Sorry. That has to come off, and it's on record.")
        else
            sol.inspection_seize(math.max(250, 25 * units),
                "Contraband. We're seizing it, and posting you.")
        end
        return
    end
    if legality == "restricted" then
        sol.inspection_fine(0.30 * value,
            string.format("Licensed cargo. Duty is %.0f credits.", 0.30 * value))
        return
    end
    if not hasLaw then
        -- ⚑ THE LINE THAT SAYS WHAT THIS PHASE IS ABOUT. Somebody with
        -- jurisdiction, patrols and a scanner stopped you, read your hold, and
        -- has no opinion about any of it. Law is a property of a PLACE.
        sol.inspection_pass("Nothing here we care about. Fly on.")
        return
    end
    sol.inspection_pass("Hold's clean. Safe flying.")
end

-- Pilot comms (Phase 8s). Somebody hailed on the open channel and this decides
-- what they say back. Answer with exactly one of:
--
--   sol.hail_reply(message)        words only
--   sol.hail_tip_market(message)   words, then C++ names a market and records
--                                  its prices into what the player remembers
--   sol.hail_tip_place(message)    words, then C++ names a system and drops a
--                                  labelled bookmark on an unscanned site there
--
-- ⚑ THE MESSAGE IS THE SENTIMENT ONLY. C++ appends the place, and that split is
-- deliberate: a berth was one of four interchangeable integers a script could
-- not get meaningfully wrong, but a tip is a claim about the galaxy. So write a
-- line that ENDS pointing at somewhere ("...worth the trip at") and let C++
-- finish the sentence. `canMarket`/`canPlace` say whether that pilot has
-- anything of each kind left; offering one anyway gets a shrug instead, because
-- the words were written on the premise of a fact that turned out not to exist.
--
-- Say nothing and C++ falls back to its own rule. Keep every line short for the
-- reason dock_request does: the panel clips, and the sender column is already
-- spending a column on the name.
function pilot_hail(name, role, faction, attitude, standing, hostile, canMarket, canPlace, roll)
    if hostile then
        sol.hail_reply("Wrong channel. Break off.")
        return
    end
    if role == "trader" and canMarket then
        -- A hauler's whole job is knowing where a cargo sells, so this is the
        -- one thing they will tell a stranger for free.
        if attitude == "friendly" then
            sol.hail_tip_market("For you? Best book I've seen lately was")
        else
            sol.hail_tip_market("Prices were worth the run at")
        end
        return
    end
    if role == "patrol" and canPlace then
        -- Patrols sweep and log; a neutral one is terse about it, a friendly
        -- one tells you where they stopped looking.
        sol.hail_tip_place("We logged an unswept return out in")
        return
    end
    if canPlace and roll > 0.35 then
        sol.hail_tip_place("Something out there nobody's claimed, over in")
        return
    end
    if canMarket then
        sol.hail_tip_market("Heard the numbers were moving at")
        return
    end
    -- Nothing to give. A galaxy that always has one more tip is a galaxy where
    -- a tip means nothing, so running dry is a real answer.
    if attitude == "friendly" then
        sol.hail_reply("Quiet out here lately. Watch yourself.")
    else
        sol.hail_reply("Nothing for you. Channel's clear.")
    end
end

-- What is being said in the room (Phase 35 stage B). The player docked at a
-- station with a bar, a restaurant, a concourse, a casino or a resort, and this
-- decides what they overhear. Spend up to `lines` of these:
--
--   sol.bar_says(message)      words only, no fact appended
--   sol.bar_shortage(message)  words, then C++ names the good and the dock
--   sol.bar_raid(message)      words, then C++ names the clan and the system
--   sol.bar_front(message)     words, then C++ names both sides and the system
--   sol.bar_hauler(message)    words, then C++ names where the run ends
--
-- ⚑ THE MESSAGE IS THE SENTIMENT ONLY, the same split pilot_hail lives under
-- and for the same reason: a rumour is a claim about the galaxy, and a hook
-- allowed to name the place could name one that is not there, with the player
-- unable to tell a bug from a barfly talking nonsense. So write a line that
-- ENDS pointing somewhere ("They're short of") and let C++ finish it.
--
-- `canShortage`/`canRaid`/`canFront`/`canHauler` say whether a fact of that
-- kind exists right now; spending a line on one that does not gets the line
-- dropped, because the words were written on a premise that turned out false.
-- `lines` is how much this room is worth - a bar says one thing, a resort says
-- three - and it is derived from the room's own power draw.
--
-- ⚑ AT t=0 THREE OF THE FOUR ARE EMPTY GALAXY-WIDE and only the departing
-- hauler is ever available, which is measured rather than assumed: a fresh
-- galaxy stocks every market at half capacity so nothing is short, and nobody
-- has raided anybody yet. Say nothing at all and C++ falls back to its own
-- wording; the house's own lines about this dock are added either way, so the
-- room is never silent.
function bar_talk(room, lines, canShortage, canRaid, canFront, canHauler, canCast, roll)
    local spent = 0
    -- Scarcity first: a war is the thing fewest rooms in the galaxy can tell
    -- you about, and a shortage is the thing almost all of them can.
    if canFront and spent < lines then
        if roll > 0.5 then
            sol.bar_front("Two of the big names are going at it over")
        else
            sol.bar_front("Nobody here will fly through")
        end
        spent = spent + 1
    end
    if canHauler and spent < lines then
        sol.bar_hauler("That crew in the corner are taking")
        spent = spent + 1
    end
    -- Somebody to go and see (Phase 35 stage C). THIRD, and the position is the
    -- scarcity rule above applied rather than a preference: measured, a room can
    -- name an authored character at 42 of the 62 rooms and stops being able to
    -- once the player has met all six, which puts it between the hauler and the
    -- raid. The engine only ever offers an authored person for this, which is
    -- why the number is 42 and not 62.
    if canCast and spent < lines then
        if roll > 0.5 then
            sol.bar_cast("If you're headed that way, ask for")
        else
            sol.bar_cast("You want somebody who knows the lanes, that's")
        end
        spent = spent + 1
    end
    if canRaid and spent < lines then
        if roll > 0.65 then
            sol.bar_raid("Everyone's still talking about how")
        else
            sol.bar_raid("Word came down the lane that")
        end
        spent = spent + 1
    end
    if canShortage and spent < lines then
        sol.bar_shortage("A hauler in last night said they're crying out for")
        spent = spent + 1
    end
    if spent == 0 then
        -- Nothing happening anywhere in reach. A galaxy where the bar always
        -- has news is a galaxy where news means nothing, so a quiet night is a
        -- real answer - and the house still has its own lines below this one.
        sol.bar_says("Quiet night. Nobody's come through with anything worth repeating.")
    end
end

-- Work heard in the room rather than read off a board (Phase 35 stage D).
--
-- C++ has already decided WHICH fact there is work in and hands over that one
-- candidate, in the same colon-delimited shape `mission_board` gets its lists
-- in. This hook writes the terms: title, pay, clock, and the objective. It is
-- the same builder the board uses - only the posting verb differs, because a
-- lead goes into its own list.
--
--   kind      "front" | "hauler" | "raid" | "shortage"
--   candidate one entry, formatted exactly as the board's lists are
--   poster    who pays, 1-based; for a war this is one of the two SIDES and
--             not necessarily whoever owns the dock, which is the whole reason
--             a room can offer a war contract the board beside it cannot
--   regard    how well the person offering it knows you (a lead taken = +1)
--
-- ⚑ RETURNING WITHOUT POSTING IS A REAL ANSWER. C++ adds no row when nothing
-- was posted, and the room still has everything else it said - the house is
-- never mute, but it is not always hiring.
--
-- ⚑ NO REP GATE ANYWHERE IN HERE, DELIBERATELY, AND IT IS THE CLEAREST
-- DIFFERENCE BETWEEN THE TWO SURFACES. The board gates its best hauls at rep 10
-- and its desperate wars at rep 10; this is somebody asking you personally,
-- and being asked personally is exactly what not having a reputation yet
-- looks like. What the room asks for instead is that it knows you, and C++
-- applies that before this is ever called.
function bar_lead(kind, candidate, poster, posterName, regard, roll)
    -- Informal work pays a little over the board for the same job: no contract,
    -- no faction backing you if it goes wrong, and somebody had to vouch.
    local premium = 1.15 + 0.1 * roll

    if kind == "shortage" then
        local system, station, commodity, units, severity, jumps, sysName, stName =
            string.match(candidate, "^(%d+):(%d+):([^:]+):([%d%.]+):([%d%.]+):(%d+):([^:]+):([^:]+)$")
        if system == nil then return end
        system, station = tonumber(system), tonumber(station)
        units, severity, jumps = tonumber(units), tonumber(severity), tonumber(jumps)
        local take = math.floor(math.min(units, 25 + 20 * jumps))
        if take < 1 then return end
        local good = string.gsub(commodity, "^sol%.", "")
        local reward = math.floor(take * (6 + 4 * jumps) * (1 + severity) * premium)
        if sol.mission_begin(string.format("They need %d %s at %s", take, good, stName),
                             poster, reward, 2 + jumps, 2, "") then
            sol.mission_deadline(480 + 420 * jumps)
            sol.mission_obj_deliver(system, station, commodity, take,
                                    string.format("Deliver %d %s to %s (%s)", take, good,
                                                  stName, sysName))
            sol.mission_lead()
        end
        return
    end

    if kind == "raid" then
        local system, clan, intensity, jumps, sysName, clanName =
            string.match(candidate, "^(%d+):(%d+):([%d%.]+):(%d+):([^:]+):([^:]+)$")
        if system == nil then return end
        system, clan = tonumber(system), tonumber(clan)
        intensity, jumps = tonumber(intensity), tonumber(jumps)
        local kills = math.min(2 + math.floor(intensity), 4)
        local reward = math.floor(kills * (350 + 150 * jumps + 120 * intensity) * premium)
        if sol.mission_begin(string.format("Somebody wants %s answered for in %s",
                                           clanName, sysName),
                             poster, reward, 3 + jumps, 2, "") then
            sol.mission_deadline(600 + 420 * jumps + 240 * kills)
            sol.mission_obj_kill(clan, kills, system,
                                 string.format("Destroy %d %s raiders in %s", kills,
                                               clanName, sysName))
            sol.mission_lead()
        end
        return
    end

    if kind == "hauler" then
        local trader, system, station, commodity, cargo, danger, jumps, sysName, stName =
            string.match(candidate,
                         "^(%d+):(%d+):(%d+):([^:]+):([%d%.]+):([%d%.]+):(%d+):([^:]+):([^:]+)$")
        if trader == nil then return end
        trader, system = tonumber(trader), tonumber(system)
        cargo, danger, jumps = tonumber(cargo), tonumber(danger), tonumber(jumps)
        -- ⚑ NO DANGER FLOOR, WHICH IS WHERE THIS PARTS COMPANY WITH THE BOARD.
        -- `mission_board` refuses a run under 0.05 danger because paying a
        -- pilot to watch a schedule tick over is not work. Nobody in a bar is
        -- pricing risk: a crew leaving tonight would rather not leave alone,
        -- and at t=0 this is the ONLY lead in the galaxy - measured, 39 of the
        -- 62 rooms, every one of them a hauler, and none of them laden.
        local reward = math.floor((250 + 1800 * danger + 220 * jumps + 3 * cargo) * premium)
        local title = cargo > 0
            and string.format("A crew leaving for %s wants company", sysName)
            or string.format("Somebody deadheading to %s wants company", sysName)
        if sol.mission_begin(title, poster, reward, 3 + jumps, 2, "") then
            sol.mission_deadline(300 + 120 * jumps)
            sol.mission_obj_escort(trader, system,
                                   string.format("Keep hauler #%d alive to %s", trader, stName))
            sol.mission_lead()
        end
        return
    end

    if kind == "front" then
        local system, holder, attacker, pressure, jumps, sysName, holderName, attackerName =
            string.match(candidate,
                         "^(%d+):(%d+):(%d+):([%d%.]+):(%d+):([^:]+):([^:]+):([^:]+)$")
        if system == nil then return end
        system, holder, attacker = tonumber(system), tonumber(holder), tonumber(attacker)
        pressure, jumps = tonumber(pressure), tonumber(jumps)
        -- Which side is asking is C++'s call, not this hook's: it is the side
        -- that is paying, and it has already been validated as a party to the
        -- fight. All this decides is how it is put.
        local defending = poster == holder
        local title = defending
            and string.format("%s is quietly hiring guns for %s", posterName, sysName)
            or string.format("%s is quietly hiring guns against %s", posterName, sysName)
        local text = defending
            and string.format("Break the %s push on %s", attackerName, sysName)
            or string.format("Take %s from %s", sysName, holderName)
        local reward = math.floor((1400 + 500 * jumps) * (1 + pressure) * premium)
        if sol.mission_begin(title, poster, reward, 8, 2, "") then
            sol.mission_deadline(1200 + 600 * jumps)
            sol.mission_obj_hold(system, poster, text)
            sol.mission_lead()
        end
        return
    end
end
