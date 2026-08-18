#pragma once

// What this game can be asked to do (Phase 8k). The enum is game-layer because
// the engine has no business knowing what "autopilot" means; the table it is
// keyed into is engine-layer because chord parsing and conflict resolution are
// not game logic. Same split pick.hpp and target_pick.cpp already draw.

#include "sol/platform/input_bindings.hpp"

#include <cstdint>

namespace game {

enum class Action : std::uint32_t
{
    // Flight (held). Defaults are exactly the layout the game shipped with,
    // so a player who never opens the Controls screen notices nothing.
    PitchUp = 0,
    PitchDown,
    YawLeft,
    YawRight,
    RollLeft,
    RollRight,
    ThrustForward,
    ThrustReverse,
    StrafeLeft,
    StrafeRight,
    StrafeUp,
    StrafeDown,
    Boost,

    // Systems.
    ToggleAssist,
    ToggleCruise,
    PipWeapons,
    PipEngines,
    PipShields,
    PipBalance,

    // Targeting & navigation.
    CycleNavTarget,
    CycleContact,
    SelectObjective,
    NearestHostile,
    Autopilot,
    Jump,
    DockSalvage,
    ScanPulse,
    Bookmark,
    HailTarget,

    // Views & mouse.
    CycleCamera,
    OpenMap,
    OpenShipInfo,
    Fire,
    Select,
    LookAround,
    FreeLook,

    Count,
};

constexpr std::uint32_t kActionCount = static_cast<std::uint32_t>(Action::Count);

// The four sections the Controls screen lists, in this order.
enum class ActionGroup : std::uint32_t
{
    Flight = 0,
    Systems,
    Targeting,
    Views,
    Count,
};

[[nodiscard]] const char* actionGroupLabel(ActionGroup group);
[[nodiscard]] ActionGroup actionGroup(Action action);

// The id written to settings.toml. Stable forever: renaming one silently
// resets that binding for every existing player.
[[nodiscard]] const char* actionId(Action action);

// What the Controls screen calls it.
[[nodiscard]] const char* actionLabel(Action action);

[[nodiscard]] Action actionFromId(const char* id, bool& found);

// Installs the shipped layout over every action.
void installDefaultBindings(sol::platform::BindingTable& table);

// Chords the player may not take: the keys that reach and leave the very
// screen doing the rebinding, and the dev tooling. Locking the player out of
// their own pause menu is not a configuration the game should let them build.
[[nodiscard]] bool isReservedChord(sol::platform::InputChord chord);

// Convenience for the fills: the name of whatever chord drives an action, or
// "" when it is unbound.
[[nodiscard]] const char* boundChordName(const sol::platform::BindingTable& table, Action action);

inline bool pressed(const sol::platform::BindingTable& table, Action action)
{
    return table.pressed(static_cast<std::uint32_t>(action));
}

inline bool held(const sol::platform::BindingTable& table, Action action)
{
    return table.held(static_cast<std::uint32_t>(action));
}

inline bool released(const sol::platform::BindingTable& table, Action action)
{
    return table.released(static_cast<std::uint32_t>(action));
}

} // namespace game
