#include "command_menu.hpp"

#include "game_ui.hpp"

#include <array>
#include <cstdio>

namespace game {

namespace {

// The vocabulary, in the order a player reads it: the thing they most often
// want first, the standing manoeuvres in the middle, and the way out last.
//
// ⚑ Cancel is an ENTRY as well as a key, for the same reason it is a key at
// all: without it, a standing order ends only by docking or by losing its
// subject. A menu that can start six things and stop none of them is a trap.
constexpr std::array<CommandMenuEntry, 7> kEntries = {{
    {CommandMenuAction::Command, CommandMode::Autopilot, "Autopilot To"},
    {CommandMenuAction::Command, CommandMode::Orbit, "Orbit"},
    {CommandMenuAction::Command, CommandMode::MatchSpeed, "Match Speed"},
    {CommandMenuAction::Command, CommandMode::KeepDistance, "Keep Distance"},
    {CommandMenuAction::Command, CommandMode::Follow, "Follow"},
    {CommandMenuAction::Command, CommandMode::Hold, "Hold Station"},
    {CommandMenuAction::Command, CommandMode::None, "Cancel Command"},
}};

constexpr CommandMenuEntry kRequestDocking = {
    CommandMenuAction::RequestDocking, CommandMode::None, "Request Docking"};
constexpr CommandMenuEntry kHail = {CommandMenuAction::Hail, CommandMode::None, "Hail"};
constexpr CommandMenuEntry kScanPulse = {CommandMenuAction::ScanPulse, CommandMode::None, "Scan Pulse"};

// How far off the selection is. Negative with nothing selected, which every
// caller treats as "no distance to state" rather than as a distance of zero.
[[nodiscard]] double selectionRange(const SpaceWorld& world)
{
    const TargetInfo info = world.currentTargetInfo();
    if (info.nav.name.empty()) {
        return -1.0;
    }
    return length(info.nav.position - world.shipState().position);
}

// ⚑ Nothing selected reads as OUT of range, not as at zero. selectionRange
// returns -1 with no selection, and a bare `range > limit` would let that
// through as available — the sign trap that turns "no target" into "in range".
[[nodiscard]] bool inRange(double range, double limit)
{
    return range >= 0.0 && range <= limit;
}

// "Request Docking - 412.4 km". The same formatter the target readout uses, so
// the number on the row and the number on the HUD are one number: a menu that
// said 412 beside a HUD saying 412.4 would read as two different measurements.
[[nodiscard]] std::string labelWithRange(const char* label, double meters)
{
    if (meters < 0.0) {
        return label;
    }
    char distance[48];
    formatDistance(meters, distance, sizeof(distance));
    return std::string(label) + " - " + distance;
}

// Whether the selection is a station the docking channel would answer for.
// A berth is deliberately NOT one: it is a point 200 m off a station that you
// only have because you are already cleared, so "request docking" there is a
// request you have already had granted.
[[nodiscard]] bool selectionIsStation(const SpaceWorld& world)
{
    if (world.targetIsContact()) {
        return false;
    }
    const std::size_t index = world.currentTargetIndex();
    return index < world.navTargets().size() && world.navTargetKind(index) == SpaceWorld::NavKind::Station;
}

} // namespace

bool commandMenuEntryApplies(const SpaceWorld& world, const CommandMenuEntry& entry)
{
    switch (entry.action) {
    case CommandMenuAction::RequestDocking:
        return selectionIsStation(world);
    case CommandMenuAction::Hail:
        return world.currentTargetInfo().isShip;
    case CommandMenuAction::ScanPulse:
    case CommandMenuAction::Command:
        break;
    }
    return true;
}

std::span<const CommandMenuEntry> commandMenuEntries()
{
    return {kEntries.data(), kEntries.size()};
}

bool commandMenuEntryEnabled(const SpaceWorld& world, const CommandMenuEntry& entry, std::string& reason)
{
    reason.clear();
    // Docked is the flat refusal for everything the menu offers: a ship on a
    // pad may not be given a flying order at all (the same guard engageCommand
    // applies), its scanner is off, and the two comms verbs are what you use
    // INSTEAD of being docked.
    if (world.isDocked()) {
        reason = "docked";
        return false;
    }
    switch (entry.action) {
    case CommandMenuAction::RequestDocking:
        // ⚑ The row is offered for the station that was picked, and it is live
        // exactly when requestDocking() would hail THAT station. Stations are
        // sited 100,000-400,000 km from their hub in random directions, so
        // "the nearest station within 20 km" and "the station you just
        // clicked" are the same station whenever this is enabled at all —
        // which is why the picked station's own range is the honest number to
        // print, and why no second call had to be invented to hail it. That is
        // an assumption about how the galaxy is built rather than about this
        // file, so it is pinned by a test rather than by this comment.
        if (world.hasClearance()) {
            reason = "already cleared for berth " + std::to_string(world.clearance().berth + 1);
            return false;
        }
        if (!inRange(selectionRange(world), SpaceWorld::kDockRequestRange)) {
            reason = "within 20 km";
            return false;
        }
        return true;
    case CommandMenuAction::Hail:
        if (!inRange(selectionRange(world), SpaceWorld::kHailRange)) {
            reason = "within 20 km";
            return false;
        }
        return true;
    case CommandMenuAction::ScanPulse:
        if (world.pulseCharge() < 1.0f) {
            char charge[32];
            std::snprintf(charge,
                          sizeof(charge),
                          "charging (%.0f%%)",
                          static_cast<double>(world.pulseCharge()) * 100.0);
            reason = charge;
            return false;
        }
        return true;
    case CommandMenuAction::Command:
        break;
    }
    if (entry.mode == CommandMode::None) {
        if (world.commandMode() == CommandMode::None) {
            reason = "nothing to cancel";
            return false;
        }
        return true;
    }
    // Already doing it. Offered as disabled rather than dropped, so the command
    // block keeps the same shape every time it opens — a list whose rows move
    // around is a list you have to read instead of aim at.
    if (world.commandMode() == entry.mode) {
        reason = "already engaged";
        return false;
    }
    if (commandNeedsTarget(entry.mode) && world.currentTargetInfo().nav.name.empty()) {
        reason = "no target";
        return false;
    }
    return true;
}

void fillCommandMenu(const SpaceWorld& world, std::vector<CommandMenuRow>& out)
{
    out.clear();
    const double range = selectionRange(world);

    const auto push = [&](const CommandMenuEntry& entry, std::string label) {
        CommandMenuRow row;
        row.entry = entry;
        row.label = std::move(label);
        row.enabled = commandMenuEntryEnabled(world, entry, row.reason);
        out.push_back(std::move(row));
    };

    // The verbs about the thing that was picked come first: the menu was opened
    // ON something, and what is specific to it outranks the manoeuvre
    // vocabulary that is available everywhere.
    if (commandMenuEntryApplies(world, kRequestDocking)) {
        push(kRequestDocking, labelWithRange(kRequestDocking.label, range));
    }
    if (commandMenuEntryApplies(world, kHail)) {
        push(kHail, labelWithRange(kHail.label, range));
    }
    // The pulse is about the space around the ship rather than about the
    // selection, so it applies to every right-click including one that hit
    // nothing — which is exactly when a player wants to know what is out there.
    push(kScanPulse, kScanPulse.label);

    for (const CommandMenuEntry& entry : kEntries) {
        push(entry, entry.label);
    }
}

std::string commandMenuTitle(const SpaceWorld& world)
{
    return world.currentTargetInfo().nav.name;
}

CommandMenuPick buildCommandMenu(sol::ui::UiContext& ui,
                                 const SpaceWorld& world,
                                 sol::core::Vec2 anchor,
                                 sol::ui::Rect& boundsOut)
{
    std::vector<CommandMenuRow> rows;
    fillCommandMenu(world, rows);
    std::vector<sol::ui::UiContext::MenuItem> items;
    items.reserve(rows.size());
    for (const CommandMenuRow& row : rows) {
        items.push_back({.label = row.label, .enabled = row.enabled, .reason = row.reason});
    }
    const std::string title = commandMenuTitle(world);
    const int index = ui.contextMenu(anchor, {items.data(), items.size()}, &boundsOut, title);
    if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) {
        return {};
    }
    return {.picked = true, .entry = rows[static_cast<std::size_t>(index)].entry};
}

void applyCommandMenu(SpaceWorld& world, const CommandMenuEntry& entry)
{
    // ⚑ Every branch is the same call the bound key makes, deliberately. A menu
    // entry and its key binding must do the identical thing or they are two
    // features that look like one — which is the whole reason the phase's exit
    // criterion is to fly the loop twice, once by menu and once by key.
    switch (entry.action) {
    case CommandMenuAction::RequestDocking:
        (void)world.requestDocking();
        return;
    case CommandMenuAction::Hail:
        (void)world.hailTarget(); // says why on the comms panel when it cannot
        return;
    case CommandMenuAction::ScanPulse:
        (void)world.pulseScan();
        return;
    case CommandMenuAction::Command:
        break;
    }
    if (entry.mode == CommandMode::None) {
        world.clearCommand();
        return;
    }
    (void)world.engageCommand(entry.mode);
}

} // namespace game
