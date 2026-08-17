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
    {Action::PitchDown, ActionGroup::Flight, "pitch_down", "Pitch Down",
     InputChord::ofKey(Key::Down)},
    {Action::YawLeft, ActionGroup::Flight, "yaw_left", "Yaw Left", InputChord::ofKey(Key::Left)},
    {Action::YawRight, ActionGroup::Flight, "yaw_right", "Yaw Right",
     InputChord::ofKey(Key::Right)},
    {Action::RollLeft, ActionGroup::Flight, "roll_left", "Roll Left", InputChord::ofKey(Key::Q)},
    {Action::RollRight, ActionGroup::Flight, "roll_right", "Roll Right",
     InputChord::ofKey(Key::E)},
    {Action::ThrustForward, ActionGroup::Flight, "thrust_forward", "Thrust Forward",
     InputChord::ofKey(Key::W)},
    {Action::ThrustReverse, ActionGroup::Flight, "thrust_reverse", "Thrust Reverse",
     InputChord::ofKey(Key::S)},
    {Action::StrafeLeft, ActionGroup::Flight, "strafe_left", "Strafe Left",
     InputChord::ofKey(Key::A)},
    {Action::StrafeRight, ActionGroup::Flight, "strafe_right", "Strafe Right",
     InputChord::ofKey(Key::D)},
    {Action::StrafeUp, ActionGroup::Flight, "strafe_up", "Strafe Up",
     InputChord::ofKey(Key::Space)},
    {Action::StrafeDown, ActionGroup::Flight, "strafe_down", "Strafe Down",
     InputChord::ofKey(Key::LeftControl)},
    {Action::Boost, ActionGroup::Flight, "boost", "Boost", InputChord::ofKey(Key::LeftShift)},

    {Action::ToggleAssist, ActionGroup::Systems, "toggle_assist", "Flight Assist",
     InputChord::ofKey(Key::X)},
    {Action::ToggleCruise, ActionGroup::Systems, "toggle_cruise", "Cruise",
     InputChord::ofKey(Key::Tab)},
    {Action::PipWeapons, ActionGroup::Systems, "pip_weapons", "Power to Weapons",
     InputChord::ofKey(Key::Num1)},
    {Action::PipEngines, ActionGroup::Systems, "pip_engines", "Power to Engines",
     InputChord::ofKey(Key::Num2)},
    {Action::PipShields, ActionGroup::Systems, "pip_shields", "Power to Shields",
     InputChord::ofKey(Key::Num3)},
    {Action::PipBalance, ActionGroup::Systems, "pip_balance", "Balance Power",
     InputChord::ofKey(Key::Num4)},

    {Action::CycleNavTarget, ActionGroup::Targeting, "cycle_nav_target", "Cycle Nav Target",
     InputChord::ofKey(Key::T)},
    {Action::CycleContact, ActionGroup::Targeting, "cycle_contact", "Cycle Contact",
     InputChord::ofKey(Key::C)},
    {Action::SelectObjective, ActionGroup::Targeting, "select_objective", "Select Objective",
     InputChord::ofKey(Key::O)},
    {Action::NearestHostile, ActionGroup::Targeting, "nearest_hostile", "Nearest Hostile",
     InputChord::ofKey(Key::H)},
    {Action::Autopilot, ActionGroup::Targeting, "autopilot", "Autopilot",
     InputChord::ofKey(Key::F)},
    {Action::Jump, ActionGroup::Targeting, "jump", "Jump Through Gate",
     InputChord::ofKey(Key::J)},
    {Action::DockSalvage, ActionGroup::Targeting, "dock_salvage", "Dock / Salvage",
     InputChord::ofKey(Key::G)},
    {Action::ScanPulse, ActionGroup::Targeting, "scan_pulse", "Scan Pulse",
     InputChord::ofKey(Key::R)},
    {Action::Bookmark, ActionGroup::Targeting, "bookmark", "Drop Bookmark",
     InputChord::ofKey(Key::B)},

    {Action::CycleCamera, ActionGroup::Views, "cycle_camera", "Cycle Camera",
     InputChord::ofKey(Key::V)},
    {Action::OpenMap, ActionGroup::Views, "open_map", "Map", InputChord::ofKey(Key::M)},
    {Action::OpenShipInfo, ActionGroup::Views, "open_ship_info", "Ship Readout",
     InputChord::ofKey(Key::I)},
    {Action::Fire, ActionGroup::Views, "fire", "Fire / Mine",
     InputChord::ofMouse(MouseButton::Middle)},
    {Action::Select, ActionGroup::Views, "select", "Select Target",
     InputChord::ofMouse(MouseButton::Left)},
    {Action::LookAround, ActionGroup::Views, "look_around", "Mouse Steering",
     InputChord::ofMouse(MouseButton::Right)},
    // Phase 8m. A key rather than a button because all three mouse buttons are
    // spoken for above, and Z rather than Left Alt because Windows routes Alt
    // through WM_SYSKEYDOWN and the window menu - the same message path Phase
    // 8h had to leave falling through to DefWindowProc for Alt+F4.
    {Action::FreeLook, ActionGroup::Views, "free_look", "Free Look",
     InputChord::ofKey(Key::Z)},
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
    Key::Escape, Key::Enter, Key::Backspace, Key::Delete, Key::Home, Key::End,
    Key::F1,     Key::F3,    Key::F5,        Key::F9,     Key::F10,
};

} // namespace

const char* actionGroupLabel(ActionGroup group)
{
    switch (group) {
    case ActionGroup::Flight: return "Flight";
    case ActionGroup::Systems: return "Systems";
    case ActionGroup::Targeting: return "Targeting & Navigation";
    case ActionGroup::Views: return "Views & Mouse";
    case ActionGroup::Count: break;
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

} // namespace game
