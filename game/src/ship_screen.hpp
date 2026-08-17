#pragma once

#include "sol/ui/context.hpp"
#include "sol/ui/screens.hpp"

namespace game {

// Where the ship screen is scrolled. main.cpp owns one across frames; the
// screen itself is rebuilt every frame like every other one.
struct ShipScreenState
{
    float scroll = 0.0f;
};

// The ship information screen (engine plan Phase 8h). Read-only: it reports
// what the ship IS, which is the one thing the station pad cannot do while the
// consequences are happening. Returns true on the frame the player closed it.
[[nodiscard]] bool buildShipScreen(sol::ui::UiContext& ui, const sol::ui::ShipInfoPanel& panel,
                                   ShipScreenState& state);

} // namespace game
