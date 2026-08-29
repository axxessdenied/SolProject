#include "command_menu.hpp"

#include <array>
#include <vector>

namespace game {

namespace {

// The vocabulary, in the order a player reads it: the thing they most often
// want first, the standing manoeuvres in the middle, and the way out last.
//
// ⚑ Cancel is an ENTRY as well as a key, for the same reason it is a key at
// all: without it, a standing order ends only by docking or by losing its
// subject. A menu that can start six things and stop none of them is a trap.
constexpr std::array<CommandMenuEntry, 7> kEntries = {{
    {CommandMode::Autopilot, "Autopilot To"},
    {CommandMode::Orbit, "Orbit"},
    {CommandMode::MatchSpeed, "Match Speed"},
    {CommandMode::KeepDistance, "Keep Distance"},
    {CommandMode::Follow, "Follow"},
    {CommandMode::Hold, "Hold Station"},
    {CommandMode::None, "Cancel Command"},
}};

} // namespace

std::span<const CommandMenuEntry> commandMenuEntries()
{
    return {kEntries.data(), kEntries.size()};
}

bool commandMenuEntryEnabled(const SpaceWorld& world, const CommandMenuEntry& entry, const char*& reason)
{
    reason = "";
    // Docked is the flat refusal: a ship on a pad may not be given a flying
    // order at all, which is the same guard engageCommand applies.
    if (world.isDocked()) {
        reason = "docked";
        return false;
    }
    if (entry.mode == CommandMode::None) {
        if (world.commandMode() == CommandMode::None) {
            reason = "nothing to cancel";
            return false;
        }
        return true;
    }
    // Already doing it. Offered as disabled rather than dropped, so the menu
    // keeps the same shape every time it opens — a list whose rows move around
    // is a list you have to read instead of aim at.
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

int buildCommandMenu(sol::ui::UiContext& ui,
                     const SpaceWorld& world,
                     sol::core::Vec2 anchor,
                     sol::ui::Rect& boundsOut)
{
    const std::span<const CommandMenuEntry> entries = commandMenuEntries();
    std::vector<sol::ui::UiContext::MenuItem> items;
    items.reserve(entries.size());
    for (const CommandMenuEntry& entry : entries) {
        const char* reason = "";
        const bool enabled = commandMenuEntryEnabled(world, entry, reason);
        items.push_back({.label = entry.label, .enabled = enabled, .reason = reason});
    }
    return ui.contextMenu(anchor, {items.data(), items.size()}, &boundsOut);
}

void applyCommandMenu(SpaceWorld& world, int index)
{
    const std::span<const CommandMenuEntry> entries = commandMenuEntries();
    if (index < 0 || static_cast<std::size_t>(index) >= entries.size()) {
        return;
    }
    const CommandMenuEntry& entry = entries[static_cast<std::size_t>(index)];
    if (entry.mode == CommandMode::None) {
        world.clearCommand();
        return;
    }
    // ⚑ The same call the bound key makes, deliberately. A menu entry and its
    // key binding must do the identical thing or they are two features that
    // look like one — which is the whole reason the phase's exit criterion is
    // to fly the loop twice, once by menu and once by key.
    (void)world.engageCommand(entry.mode);
}

} // namespace game
