-- Campaign spine, Act 1 opening (Phase 8c; decisions/008). Loaded after
-- init.lua (boot scripts run sorted, init first). The spine rides the same
-- mission machinery as the sandbox: campaign_offer posts the current
-- stage's mission on any major faction's board (init.lua's mission_board
-- calls it), and mission_event adds the authored flavor — dialog and
-- scripted ambushes — on objective transitions. Declining or abandoning
-- costs nothing and the mission is re-offered on the next dock: the spine
-- is ignorable, and the sandbox is complete without it.

local function firstClan()
    for line in string.gmatch(sol.factions(), "[^\n]+") do
        local index, name = string.match(line, "^(%d+): (.-) %(pirate clan%)$")
        if index ~= nil then
            return tonumber(index), name
        end
    end
    return nil, nil
end

local function neighborWithStation()
    local i = 1
    while true do
        local dest = sol.gate_destination(i)
        if dest < 0 then
            return nil
        end
        if sol.station_count(dest) > 0 then
            return dest
        end
        i = i + 1
    end
end

-- One entry per stage; build() assembles the objectives for the station the
-- player is docked at (home = where the offer is accepted).
local act = {
    {
        id = "act1.m1", title = "Shakedown Cruise", reward = 400, rep = 2,
        intro = "Dispatch: 'New pilot? Run the calibration loop so we know your ship holds together.'",
        outro = "Dispatch: 'Clean loop. There's more work if you want it.'",
        build = function()
            local here = sol.system_index()
            sol.mission_obj_flyto(here, 0, 1500, -8000, 1200,
                                  "Fly to the calibration beacon (8 km out)")
            sol.mission_obj_dock(here, sol.docked_station_index(),
                                 "Return and dock at the station")
        end,
    },
    {
        id = "act1.m2", title = "Teeth in the Dark", reward = 900, rep = 4,
        intro = "Dispatch: 'Raiders are probing the traffic lanes. Fly the picket point - and expect company.'",
        outro = "Dispatch: 'Two kills confirmed. The clans will remember that.'",
        ambush = 2,
        build = function()
            local here = sol.system_index()
            sol.mission_obj_flyto(here, -6000, 800, 2000, 1200,
                                  "Fly the picket point (6 km out)")
            local clan, clanName = firstClan()
            if clan ~= nil then
                sol.mission_obj_kill(clan, 2, -1,
                                     string.format("Destroy 2 %s raiders", clanName))
            end
            sol.mission_obj_dock(here, sol.docked_station_index(),
                                 "Return and dock at the station")
        end,
    },
    {
        id = "act1.m3", title = "Relief Run", reward = 1400, rep = 5,
        intro = "Dispatch: 'The raids left a neighbor system short on food. Buy 15 units and get them there.'",
        outro = "Relief coordinator: 'Shelves are stocked. You were seen doing this, pilot.'",
        build = function()
            local dest = neighborWithStation()
            if dest == nil then
                dest = sol.system_index() -- isolated system: hand in at home
            end
            sol.mission_obj_deliver(dest, 0, "sol.food", 15,
                                    "Deliver 15 food to the neighboring system")
        end,
    },
    {
        id = "act1.m4", title = "Line in the Dark", reward = 2500, rep = 8,
        intro = "Dispatch: 'The clan is massing for a push. Hold the outer picket until the line breaks.'",
        outro = "Dispatch: 'The push is broken. Act one of your career, closed. More is coming.'",
        ambush = 3,
        build = function()
            local here = sol.system_index()
            sol.mission_obj_flyto(here, 8000, -1000, 6000, 1500,
                                  "Take position at the outer picket (10 km out)")
            local clan, clanName = firstClan()
            if clan ~= nil then
                sol.mission_obj_kill(clan, 3, -1,
                                     string.format("Break the %s push: destroy 3 raiders",
                                                   clanName))
            end
            sol.mission_obj_dock(here, sol.docked_station_index(),
                                 "Return and dock at the station")
        end,
    },
}

local function stageOf(id)
    for i = 1, #act do
        if act[i].id == id then
            return i - 1
        end
    end
    return nil
end

local activeCampaignId = nil -- avoids re-post warnings while one is running

function campaign_offer(owner, ownerName)
    local stage = sol.campaign_stage()
    local mission = act[stage + 1]
    if mission == nil or mission.id == activeCampaignId then
        return
    end
    if sol.mission_begin(mission.title, owner, mission.reward, mission.rep, 0,
                         mission.id) then
        mission.build()
        sol.mission_post()
    end
end

function mission_event(id, kind, objective)
    local stage = stageOf(id)
    local mission = stage ~= nil and act[stage + 1] or nil
    if mission == nil then
        return
    end
    if kind == "accepted" then
        activeCampaignId = id
        print("[campaign] " .. mission.intro)
    elseif kind == "objective" then
        -- The ambush springs when the fly-to (objective 1) completes.
        if mission.ambush ~= nil and objective == 1 then
            local clan, clanName = firstClan()
            if clan ~= nil then
                for _ = 1, mission.ambush do
                    sol.spawn_pilot_faction("sol.interceptor", "fighter", clan)
                end
                print(string.format("[campaign] %s ships on the scanner!", clanName))
            end
        end
    elseif kind == "completed" then
        activeCampaignId = nil
        sol.set_campaign_stage(stage + 1)
        print("[campaign] " .. mission.outro)
        if stage + 1 >= #act then
            print("[campaign] ACT 1 COMPLETE")
        end
    elseif kind == "failed" or kind == "abandoned" then
        activeCampaignId = nil
        print("[campaign] The offer stands; check the board when you're ready.")
    end
end

print("campaign.lua ready - act 1, stage " .. sol.campaign_stage())
