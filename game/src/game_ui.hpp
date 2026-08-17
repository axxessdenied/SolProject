#pragma once

#include "sol/assets/font.hpp"
#include "sol/core/math/vec.hpp"
#include "sol/ui/draw_list.hpp"
#include "sol/ui/screens.hpp"

namespace game {

// The HUD's screen margin. Exposed since Phase 8j because the radar disc's
// position depends on it and a click has to be tested against the disc where
// it was actually drawn.
inline constexpr float kHudMargin = 24.0f;

// The custom game UI (engine plan Phase 8d), drawn on the in-repo UI stack
// rather than Dear ImGui. This is the flight HUD, at parity with the
// provisional dev one: crosshair with shield facings, projectile lead marker,
// target diamond or edge arrow, flight readout, power pips, target readout,
// contextual prompts, and the tracked mission line.
//
// Pure geometry building - no input, no game state mutation - so it can be
// exercised headlessly.
void buildFlightUi(sol::ui::DrawList& list, const sol::assets::Font& font,
                   sol::core::Vec2 screenSize, const sol::ui::FlightHud& hud);

} // namespace game
