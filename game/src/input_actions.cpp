#include "input_actions.hpp"

#include <cstring>

namespace game {

namespace {

using sol::platform::InputChord;
using sol::platform::Key;
using sol::platform::MouseButton;

struct ActionInfo
{
    Action action;
    ActionGroup group;
    const char* id;
    const char* label;
    InputChord defaultChord;
};

// One row per action, in enum order - the order the Controls screen lists
// them. A single table so an id, a label, a group and a default can never
// drift apart across four switch statements.
const ActionInfo kActions[] = {
    {Action::PitchUp, ActionGroup::Flight, "pitch_up", "Pitch Up", InputChord::ofKey(Key::Up)},
    {Action::PitchDown, ActionGroup::Flight, "pitch_down", "Pitch Down", InputChord::ofKey(Key::Down)},
    {Action::YawLeft, ActionGroup::Flight, "yaw_left", "Yaw Left", InputChord::ofKey(Key::Left)},
    {Action::YawRight, ActionGroup::Flight, "yaw_right", "Yaw Right", InputChord::ofKey(Key::Right)},
    {Action::RollLeft, ActionGroup::Flight, "roll_left", "Roll Left", InputChord::ofKey(Key::Q)},
    {Action::RollRight, ActionGroup::Flight, "roll_right", "Roll Right", InputChord::ofKey(Key::E)},
    {Action::ThrustForward,
     ActionGroup::Flight,
     "thrust_forward",
     "Thrust Forward",
     InputChord::ofKey(Key::W)},
    {Action::ThrustReverse,
     ActionGroup::Flight,
     "thrust_reverse",
     "Thrust Reverse",
     InputChord::ofKey(Key::S)},
    {Action::StrafeLeft, ActionGroup::Flight, "strafe_left", "Strafe Left", InputChord::ofKey(Key::A)},
    {Action::StrafeRight, ActionGroup::Flight, "strafe_right", "Strafe Right", InputChord::ofKey(Key::D)},
    {Action::StrafeUp, ActionGroup::Flight, "strafe_up", "Strafe Up", InputChord::ofKey(Key::Space)},
    {Action::StrafeDown,
     ActionGroup::Flight,
     "strafe_down",
     "Strafe Down",
     InputChord::ofKey(Key::LeftControl)},
    {Action::Boost, ActionGroup::Flight, "boost", "Boost", InputChord::ofKey(Key::LeftShift)},

    {Action::ToggleAssist, ActionGroup::Systems, "toggle_assist", "Flight Assist", InputChord::ofKey(Key::X)},
    {Action::ToggleCruise, ActionGroup::Systems, "toggle_cruise", "Cruise", InputChord::ofKey(Key::Tab)},
    {Action::PipWeapons,
     ActionGroup::Systems,
     "pip_weapons",
     "Power to Weapons",
     InputChord::ofKey(Key::Num1)},
    {Action::PipEngines,
     ActionGroup::Systems,
     "pip_engines",
     "Power to Engines",
     InputChord::ofKey(Key::Num2)},
    {Action::PipShields,
     ActionGroup::Systems,
     "pip_shields",
     "Power to Shields",
     InputChord::ofKey(Key::Num3)},
    {Action::PipBalance, ActionGroup::Systems, "pip_balance", "Balance Power", InputChord::ofKey(Key::Num4)},
    // Phase 31 stage C3. P is the last unbound letter in the shipped layout,
    // which is the whole of why it is P; all three mouse buttons have been
    // spoken for since 8j and the number row is the pips. It cycles rather
    // than selecting, because four "select group N" rows would be four
    // Controls entries to reach four positions one key already walks in order.
    {Action::CycleFireGroup,
     ActionGroup::Systems,
     "cycle_fire_group",
     "Cycle Fire Group",
     InputChord::ofKey(Key::P)},

    {Action::CycleNavTarget,
     ActionGroup::Targeting,
     "cycle_nav_target",
     "Cycle Nav Target",
     InputChord::ofKey(Key::T)},
    // Phase 15. U and N rather than a modifier on T and C: a chord holds one
    // key with no modifier field, and LeftShift is Boost, so the natural
    // Shift+T would fire the engines on every step backwards. Each sits in the
    // same physical cluster as the key it reverses (U beside T's row, N beside
    // C's) and both rebind like any other row.
    {Action::CycleNavTargetBack,
     ActionGroup::Targeting,
     "cycle_nav_target_back",
     "Cycle Nav Target Back",
     InputChord::ofKey(Key::U)},
    {Action::CycleContact,
     ActionGroup::Targeting,
     "cycle_contact",
     "Cycle Contact",
     InputChord::ofKey(Key::C)},
    {Action::CycleContactBack,
     ActionGroup::Targeting,
     "cycle_contact_back",
     "Cycle Contact Back",
     InputChord::ofKey(Key::N)},
    {Action::SelectObjective,
     ActionGroup::Targeting,
     "select_objective",
     "Select Objective",
     InputChord::ofKey(Key::O)},
    {Action::NearestHostile,
     ActionGroup::Targeting,
     "nearest_hostile",
     "Nearest Hostile",
     InputChord::ofKey(Key::H)},
    {Action::DockSalvage,
     ActionGroup::Targeting,
     "dock_salvage",
     "Dock / Salvage",
     InputChord::ofKey(Key::G)},
    {Action::ScanPulse, ActionGroup::Targeting, "scan_pulse", "Scan Pulse", InputChord::ofKey(Key::R)},
    {Action::Bookmark, ActionGroup::Targeting, "bookmark", "Drop Bookmark", InputChord::ofKey(Key::B)},
    // Phase 8s. Its own verb rather than a fifth rung on the interact key,
    // which already means five things and would have to guess between a ship
    // and a station when both are in range. Y because both mnemonic keys are
    // gone - H is Nearest Hostile and C is Cycle Contact, both in this group -
    // and Y is free in the same physical cluster as T and H.
    {Action::HailTarget, ActionGroup::Targeting, "hail_target", "Hail Target", InputChord::ofKey(Key::Y)},

    // Commands (Phase 28). The id strings are new, so these arrive unbound for
    // an existing player only where the default below is empty — the three that
    // ship with a key get it on first run like any other default.
    //
    // ⚑ Only three of the seven are bound, and that is the phase's own rule:
    // the common ones ship with a key and the rest ship available. J, K and L
    // were the letters left — and J in particular is free BECAUSE Phase 8v
    // removed Jump, which is the enum comment above paying off.
    {Action::Autopilot, ActionGroup::Commands, "autopilot", "Autopilot", InputChord::ofKey(Key::F)},
    {Action::CommandOrbit, ActionGroup::Commands, "command_orbit", "Orbit Target", InputChord::ofKey(Key::K)},
    {Action::CommandMatchSpeed, ActionGroup::Commands, "command_match_speed", "Match Speed", InputChord{}},
    {Action::CommandKeepDistance,
     ActionGroup::Commands,
     "command_keep_distance",
     "Keep Distance",
     InputChord{}},
    {Action::CommandHold, ActionGroup::Commands, "command_hold", "Hold Station", InputChord::ofKey(Key::J)},
    {Action::CommandFollow, ActionGroup::Commands, "command_follow", "Follow Target", InputChord{}},
    // The way out. Without it a standing order ends only by docking or by
    // losing its subject, which would make every one of them a trap.
    {Action::CommandCancel,
     ActionGroup::Commands,
     "command_cancel",
     "Cancel Command",
     InputChord::ofKey(Key::L)},

    {Action::CycleCamera, ActionGroup::Views, "cycle_camera", "Cycle Camera", InputChord::ofKey(Key::V)},
    {Action::OpenMap, ActionGroup::Views, "open_map", "Map", InputChord::ofKey(Key::M)},
    {Action::OpenShipInfo, ActionGroup::Views, "open_ship_info", "Ship Readout", InputChord::ofKey(Key::I)},
    {Action::Fire, ActionGroup::Views, "fire", "Fire / Mine", InputChord::ofMouse(MouseButton::Middle)},
    {Action::Select, ActionGroup::Views, "select", "Select Target", InputChord::ofMouse(MouseButton::Left)},
    {Action::LookAround,
     ActionGroup::Views,
     "look_around",
     "Mouse Steering",
     InputChord::ofMouse(MouseButton::Right)},
    // Phase 8m. A key rather than a button because all three mouse buttons are
    // spoken for above, and Z rather than Left Alt because Windows routes Alt
    // through WM_SYSKEYDOWN and the window menu - the same message path Phase
    // 8h had to leave falling through to DefWindowProc for Alt+F4.
    {Action::FreeLook, ActionGroup::Views, "free_look", "Free Look", InputChord::ofKey(Key::Z)},
};

static_assert(sizeof(kActions) / sizeof(kActions[0]) == kActionCount,
              "every action needs exactly one row: id, label, group and default together");

const ActionInfo& info(Action action)
{
    const std::uint32_t index = static_cast<std::uint32_t>(action);
    return kActions[index < kActionCount ? index : 0];
}

// Reserved because they are how the player reaches, uses and leaves the very
// screen that does the rebinding - or because they are dev tooling rather than
// controls. Arrows, Tab and Space are deliberately NOT here: they double as
// menu navigation, but a menu and the cockpit never own the keyboard at the
// same time, which has been true since long before bindings existed.
constexpr Key kReservedKeys[] = {
    Key::Escape,
    Key::Enter,
    Key::Backspace,
    Key::Delete,
    Key::Home,
    Key::End,
    Key::F1,
    Key::F2,
    Key::F3,
    Key::F5,
    Key::F9,
    Key::F10,
};

} // namespace

const char* actionGroupLabel(ActionGroup group)
{
    switch (group) {
    case ActionGroup::Flight:
        return "Flight";
    case ActionGroup::Systems:
        return "Systems";
    case ActionGroup::Targeting:
        return "Targeting & Navigation";
    case ActionGroup::Commands:
        return "Ship Commands";
    case ActionGroup::Views:
        return "Views & Mouse";
    case ActionGroup::Count:
        break;
    }
    return "";
}

ActionGroup actionGroup(Action action)
{
    return info(action).group;
}

const char* actionId(Action action)
{
    return info(action).id;
}

const char* actionLabel(Action action)
{
    return info(action).label;
}

Action actionFromId(const char* id, bool& found)
{
    found = false;
    if (id == nullptr) {
        return Action::PitchUp;
    }
    for (const ActionInfo& entry : kActions) {
        if (std::strcmp(entry.id, id) == 0) {
            found = true;
            return entry.action;
        }
    }
    return Action::PitchUp;
}

void installDefaultBindings(sol::platform::BindingTable& table)
{
    table.setActionCount(kActionCount);
    for (const ActionInfo& entry : kActions) {
        table.bind(static_cast<std::uint32_t>(entry.action), entry.defaultChord);
    }
}

bool isReservedChord(sol::platform::InputChord chord)
{
    if (chord.kind != sol::platform::ChordKind::Key) {
        return false; // every mouse button is fair game
    }
    for (const Key key : kReservedKeys) {
        if (chord.asKey() == key) {
            return true;
        }
    }
    return false;
}

const char* boundChordName(const sol::platform::BindingTable& table, Action action)
{
    const InputChord chord = table.chordFor(static_cast<std::uint32_t>(action));
    return chord.bound() ? sol::platform::chordName(chord) : "";
}

KeyboardRouting routeKeyboard(bool inFlight, bool bookmarkPromptOpen, bool imguiWantsKeyboard)
{
    // ImGui first, and it takes everything. The dev console is drawn over the
    // game rather than inside it, so when it holds the keyboard there is no
    // consumer left underneath: not the ship, and not the game's own UI. This
    // is the row the old single bool got wrong.
    if (imguiWantsKeyboard) {
        return {};
    }

    KeyboardRouting routing;
    // A game text field suppresses gameplay without suppressing the UI - the
    // prompt is *made of* menu keys, so Backspace has to reach it while W must
    // not reach the throttle. Outside flight there is no gameplay to suppress
    // and the menus own the keyboard anyway.
    routing.gameplay = inFlight && !bookmarkPromptOpen;
    routing.menus = !inFlight || bookmarkPromptOpen;
    // Shortcuts ask only "does a text field have the keyboard", because they
    // are the keys whose meaning does not depend on where the player is. `i`
    // in a bookmark name must not leave the cockpit, but Ship Readout opens
    // from flight and from a station alike, so it cannot borrow `gameplay`.
    routing.shortcuts = !bookmarkPromptOpen;
    // ⚑ Unconditional here, because the only thing that ever takes text away
    // is ImGui and that case returned above. It looks like a constant and it
    // is not one: it is `!imguiWantsKeyboard`, written where a test can see it
    // rather than inside main.cpp, which is the translation unit no suite can
    // link. On Wayland this is the whole of Phase 20's text protection.
    routing.text = true;
    return routing;
}

} // namespace game
