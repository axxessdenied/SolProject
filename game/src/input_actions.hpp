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
    // Phase 31 stage C3. In Systems rather than Targeting because it is a
    // setting on the ship's own console - the same kind of thing as the four
    // pip rows above it - and not a way of choosing what to shoot at.
    CycleFireGroup,

    // Targeting & navigation.
    //
    // Phase 15: the two *Back actions are separate rows rather than a modifier
    // on their forward key, and that is not a compromise. InputChord is one
    // key or one mouse button with no modifier field at all, and even with one
    // Shift is already Boost - so Shift+T would light the engines every time a
    // player stepped back through a target list. Inserting mid-enum is safe
    // because settings serialize an action by its id string, not its ordinal.
    CycleNavTarget,
    CycleNavTargetBack,
    CycleContact,
    CycleContactBack,
    SelectObjective,
    NearestHostile,
    // Phase 8v removed Jump: you fly THROUGH a gate now, so there is no key to
    // press. It is gone rather than left in the Controls screen doing nothing,
    // for the reason game_ui.cpp already gives about "[J] JUMP" — a listed
    // binding that does nothing is a confident lie. An old settings.toml still
    // carrying `jump = "J"` logs one "unknown action - ignored" and is fine.
    DockSalvage,
    ScanPulse,
    Bookmark,
    HailTarget,

    // Commands (Phase 28): manoeuvres the player orders and the ship holds.
    //
    // ⚑ Autopilot MOVED HERE from Targeting above, and only its group changed.
    // Bindings serialize by id string rather than by ordinal (see the Phase 15
    // note above), so "autopilot" keeps whatever key a player has already put
    // it on — but the Controls screen now lists it beside its six siblings,
    // which is the honest place for it once it is one member of an enum rather
    // than the only thing the ship could be told to do.
    //
    // ⚑ Every action in this block MUST stay contiguous and in the same order
    // as kActions in the .cpp: info() indexes that table by ordinal, and the
    // Controls screen emits a heading whenever actionGroup() changes as it
    // walks 0..kActionCount, so a group split in two would print twice.
    Autopilot,
    CommandOrbit,
    CommandMatchSpeed,
    CommandKeepDistance,
    CommandHold,
    CommandFollow,
    CommandCancel,

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

// The five sections the Controls screen lists, in this order. Commands (Phase
// 28) is the fifth: the four that came before it have no home for "tell the
// ship to hold this manoeuvre", which is a different kind of thing from
// picking a target or opening a view.
enum class ActionGroup : std::uint32_t
{
    Flight = 0,
    Systems,
    Targeting,
    Commands,
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

// Who is allowed to act on a keypress this frame (Phase 20).
//
// The answers are separate because the consumers disagree. A key is a physical
// fact by the time the game sees it - the platform layer records keyDown[]
// *before* the dev-UI hook swallows the message, deliberately, so that a key
// ImGui takes the "up" for cannot latch down forever - so the only thing
// standing between a focused text field and the ship's throttle is a gate at
// the consumer, and the consumers want different things from the same key.
struct KeyboardRouting
{
    // Cockpit-only actions and the flight mapper: thrust, pips, targeting.
    bool gameplay = false;
    // The game's own UI: menu navigation, sliders, and the text-editing keys
    // the bookmark prompt is built out of.
    bool menus = false;
    // Keys that mean the same thing from more than one state and so belong to
    // neither of the above: the Map and Ship Readout keys open from flight,
    // from a station or from themselves, and Esc reaches the pause menu from
    // flight or a station. They are gated only on whether a text field has the
    // keyboard, which is why they need their own answer rather than borrowing
    // one that also encodes *where* the player is.
    bool shortcuts = false;
    // ⚑⚑ Phase 21. Typed CHARACTERS, which is a different question from every
    // row above and belongs to a different layer on each platform. On Windows
    // the dev UI's message hook swallows WM_CHAR before the window ever records
    // it, so `Window::textInput()` is already empty when ImGui holds the
    // keyboard and this field is a no-op that agrees. Wayland has no such hook
    // - the platform layer does not know ImGui exists and Phase 21 decision 2
    // forbids teaching it - so on Linux THIS FIELD IS THE ONLY THING standing
    // between a focused dev console and the bookmark prompt underneath it.
    //
    // ⚑ It is deliberately not `menus`. Menus additionally asks *where the
    // player is*, and folding that in would suppress text in flight for a
    // reason that has nothing to do with who owns the keyboard - the same
    // one-argument-too-many mistake `jumping` nearly made below.
    bool text = false;
};

// The truth table this game needs, and the reason it is a function.
//
//   nothing owns the keyboard   -> gameplay in flight, menus outside it,
//                                  shortcuts always, text always
//   the bookmark prompt owns it -> gameplay OFF, menus ON, shortcuts OFF,
//                                  text ON (it is the field being typed into)
//   ImGui owns it               -> ALL four off
//
// The middle row is why a single `typing` bool could not express this: the
// bookmark prompt wants gameplay suppressed *and* the UI fed, because its own
// backspace and Enter are menu keys. ImGui wants neither, and a bool that
// means "suppress gameplay" and "feed the UI" at the same time has no way to
// say so. Phase 20 was that bool answering the ImGui case with the bookmark
// prompt's answer, which is how typing in the dev console flew the ship.
//
// ⚑ This answers "who owns the keyboard" and NOTHING ELSE. In particular it
// does not take `jumping`: whether the ship is steerable during a jump is a
// question about game state, not about who is typing, and the two want
// different answers at different call sites - the discrete actions stand down
// for a jump and the flight mapper never did. Folding it in here would repeat
// exactly the conflation this phase exists to undo.
[[nodiscard]] KeyboardRouting routeKeyboard(bool inFlight, bool bookmarkPromptOpen, bool imguiWantsKeyboard);

} // namespace game
