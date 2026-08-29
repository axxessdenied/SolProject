#pragma once

#include "sol/assets/font.hpp"
#include "sol/core/math/vec.hpp"
#include "sol/ui/draw_list.hpp"
#include "sol/ui/screens.hpp"

#include <cstddef>

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
void buildFlightUi(sol::ui::DrawList& list,
                   const sol::assets::Font& font,
                   sol::core::Vec2 screenSize,
                   const sol::ui::FlightHud& hud);

// A range the way the HUD says it: metres up close, kilometres out to a
// million, megametres past that.
//
// ⚑ DECLARED HERE RATHER THAN COPIED A THIRD TIME. It was file-local to
// game_ui.cpp, and map_ui.cpp already has its own near-identical copy which has
// quietly drifted to a different precision (%.0f km against this one's %.1f).
// The context menu needs the same number the target readout is showing beside
// it - "Request Docking - 412.4 km" against a HUD reading 412.4 km - so it
// takes THIS one. The map's copy is left alone: changing what the map prints is
// a visible change that belongs to whoever is working on the map.
void formatDistance(double meters, char* buffer, std::size_t size);

} // namespace game
