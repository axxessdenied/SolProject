-- The cast's own words (engine plan Phase 35 stage C; `game/data/characters.toml`
-- is who they are and where they sit). Loaded after init.lua, like campaign.lua:
-- boot scripts run sorted, and `cast` sorts before `init`, so the one thing this
-- file must not do is assume init.lua has run. It does not - it defines a global
-- and nothing else.
--
-- ⚑⚑ ONE HOOK FOR THE WHOLE CAST, DISPATCHING ON THE ID, WHICH IS EXACTLY
-- campaign.lua'S SHAPE. The spine has no function per mission: it has a table
-- keyed by stage id and two global entry points that look a row up in it. A
-- global function per `[[character]]` row would have put a name out of a data
-- file into the C++ that calls it, and a mod could then add a character it could
-- never give a line to.
--
-- ⚑ `visits` and `regard` are the only things the save carries about a person
-- (`kSaveVersion` 33), and they are handed to this hook for one reason: a
-- character who cannot tell a stranger from a regular is the character the
-- phase's own risk register calls worse than none.
--
--   visits == 1   you are meeting them right now, this screen
--   visits == 2   you have been in once before
--   visits >= 3   they know you
--   regard        moves in stage D, when a lead heard here becomes a mission.
--                 Nothing writes it yet and no line below leans on it.
--
-- ⚑ A REGULAR - one of the ~56 rooms with no authored character in them - is
-- called here too, with an empty id. `cast[""]` does not exist, so this file
-- returns and the room falls through to stage A's house lines and stage B's four
-- sources. Nothing is ever mute, which is the rule the whole phase runs on.

local cast = {}

cast["sol.char_amaris"] = {
    -- The one resort in the galaxy. She is the only person in the cast whose
    -- anchor has exactly one seat, and she talks like somebody who knows it.
    "Everything on this station is expensive and I make no apology for it.",
    "Second time. I did wonder whether you were the sort who came back.",
    "You know the rate by now. Sit where you like.",
}

cast["sol.char_okonjo"] = {
    "Quartermaster's office. If you are here about a manifest, I am off duty.",
    "You again. Nothing has been signed off since last time, before you ask.",
    "Sit. I will tell you what I hear, and you will not tell anyone I told you.",
}

cast["sol.char_vey"] = {
    -- Clan space. He is careful the first time and less so afterwards, which is
    -- the whole reason the hook is told how many times you have been in.
    "I do not know you, so I have not heard anything worth repeating.",
    "You came back. Out here that is either brave or it is business.",
    "Ask. Whatever it is, somebody on this dock owes me a favour about it.",
}

cast["sol.char_halloran"] = {
    "Mind the floor, it is still wet. Yard crew drink like the shift never ended.",
    "Back for another look at the berths? They have not got any bigger.",
    "You want the truth about a hull, you ask the people who weld them.",
}

cast["sol.char_bekker"] = {
    "Freight Guild. If you are hauling, I can tell you where not to bother.",
    "Good. Repeat custom is the only kind that is worth anything.",
    "Whatever the board out there says, I will tell you what it actually pays.",
}

cast["sol.char_soto"] = {
    "Nobody comes this far out for the drink.",
    "Twice now. The fringe grows on you or it does not, there is no middle.",
    "I have been prospecting out here longer than that station has had a name.",
}

function character_talk(id, name, trade, visits, regard)
    local lines = cast[id]
    if lines == nil then
        return -- a regular the generator named: the house speaks for them
    end
    local which = 1
    if visits >= 3 then
        which = 3
    elseif visits == 2 then
        which = 2
    end
    -- `bar_person`, not `bar_says`: the two put a different word in the topic
    -- column, and that is the whole reason the second one exists. A line from
    -- the person in front of you and the house's scriptless quiet-night line are
    -- different things, and a test that counts topics by name has to be able to
    -- say which it is looking at.
    sol.bar_person(lines[which])
end

local voices = 0
for _ in pairs(cast) do
    voices = voices + 1
end
-- ⚑ `pairs`, not `#cast`: this table is keyed by id, so the length operator
-- reports 0 and the boot log would have said the cast was empty for as long as
-- anybody kept reading it.
print("cast.lua ready - " .. voices .. " authored voice(s)")
