#pragma once

#include "sol/assets/font.hpp"
#include "sol/core/math/vec.hpp"
#include "sol/ui/dev_ui.hpp"
#include "sol/ui/draw_list.hpp"

namespace game {

// The custom game UI (engine plan Phase 8d), drawn on the in-repo UI stack
// rather than Dear ImGui. This is the flight readout: the first surface moved
// off the provisional dev HUD, which stays up beside it until every panel has
// an equivalent here.
//
// Pure geometry building - no input, no game state mutation - so it can be
// exercised headlessly.
void buildFlightUi(sol::ui::DrawList& list, const sol::assets::Font& font,
                   sol::core::Vec2 screenSize, const sol::ui::FlightHud& hud);

} // namespace game
