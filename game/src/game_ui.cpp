#include "game_ui.hpp"

#include "sol/ui/radar_projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace game {

using sol::assets::Font;
using sol::assets::FontStyleRecord;
using sol::core::Vec2;
using sol::core::Vec3;
using sol::ui::Color;
using sol::ui::DrawList;
using sol::ui::Rect;
using sol::ui::rgba;
using sol::ui::TextAlign;

namespace {

constexpr float kPi = 3.14159265f;

// HUD palette. The station screen and the menus run off the UiContext theme;
// the flight HUD is its own thing - it draws over the world rather than over a
// background, so it needs its own contrast rules.
constexpr Color kPanel = rgba(0x0E141ACCu);
constexpr Color kTextPrimary = rgba(0xE8F0F8FFu);
constexpr Color kTextDim = rgba(0x8FA6BCFFu);
constexpr Color kAccent = rgba(0x58C8F0FFu);
constexpr Color kHull = rgba(0xE0704CFFu);
constexpr Color kShield = rgba(0x58A8F0FFu);
constexpr Color kMeterTrack = rgba(0x1E2A36FFu);
constexpr Color kReticle = rgba(0x8CDCA0C8u);
constexpr Color kReticleHit = rgba(0xFF5039C8u);
constexpr Color kLead = rgba(0xFF825AE6u);
constexpr Color kTargetMark = rgba(0xFFC850DCu);
constexpr Color kHostile = rgba(0xE64D40FFu);
constexpr Color kFriendly = rgba(0x59D966FFu);
constexpr Color kNeutral = rgba(0xB4B4B4FFu);
constexpr Color kWarning = rgba(0xE68C4DFFu);
constexpr Color kPipWeapons = rgba(0xFF9973FFu);
constexpr Color kPipEngines = rgba(0x8CDCA0FFu);
constexpr Color kPipShields = rgba(0x80BFFFFFu);

constexpr float kMargin = 24.0f;
constexpr float kPadding = 14.0f;
constexpr float kRowHeight = 22.0f;
constexpr float kMeterHeight = 8.0f;
constexpr float kFlightPanelWidth = 320.0f;
constexpr float kFlightPanelHeight = 168.0f;
constexpr float kPowerPanelWidth = 208.0f;
constexpr float kTargetPanelWidth = 300.0f;

// Every style the HUD draws with, resolved once per frame.
struct Styles
{
    const FontStyleRecord* heading = nullptr;
    const FontStyleRecord* body = nullptr;
    const FontStyleRecord* strong = nullptr;
    const FontStyleRecord* small = nullptr;
};

[[nodiscard]] bool has(const char* text)
{
    return text != nullptr && text[0] != '\0';
}

[[nodiscard]] float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] Color mix(const Color& from, const Color& to, float t)
{
    const float k = clamp01(t);
    return {from.r + (to.r - from.r) * k, from.g + (to.g - from.g) * k,
            from.b + (to.b - from.b) * k, from.a + (to.a - from.a) * k};
}

[[nodiscard]] Color attitudeColor(const char* attitude)
{
    if (attitude == nullptr) {
        return kNeutral;
    }
    if (std::strcmp(attitude, "hostile") == 0) {
        return kHostile;
    }
    if (std::strcmp(attitude, "friendly") == 0) {
        return kFriendly;
    }
    return kNeutral;
}

void formatDistance(double meters, char* buffer, std::size_t size)
{
    if (meters < 10'000.0) {
        std::snprintf(buffer, size, "%.0f m", meters);
    } else if (meters < 1.0e9) {
        std::snprintf(buffer, size, "%.1f km", meters / 1000.0);
    } else {
        std::snprintf(buffer, size, "%.2f Mkm", meters / 1.0e9);
    }
}

void formatSpeed(float metersPerSecond, char* buffer, std::size_t size)
{
    if (metersPerSecond < 10'000.0f) {
        std::snprintf(buffer, size, "%.0f m/s", static_cast<double>(metersPerSecond));
    } else {
        std::snprintf(buffer, size, "%.0f km/s", static_cast<double>(metersPerSecond) / 1000.0);
    }
}

void formatClosing(float metersPerSecond, char* buffer, std::size_t size)
{
    if (std::abs(metersPerSecond) < 10'000.0f) {
        std::snprintf(buffer, size, "%+.0f m/s", static_cast<double>(metersPerSecond));
    } else {
        std::snprintf(buffer, size, "%+.0f km/s", static_cast<double>(metersPerSecond) / 1000.0);
    }
}

void drawMeter(DrawList& list, const Rect& box, float fraction, const Color& fill)
{
    list.addRoundedRect(box, box.height() * 0.5f, kMeterTrack);
    const float clamped = clamp01(fraction);
    if (clamped <= 0.0f) {
        return;
    }
    // Keep the filled end round even when nearly empty.
    const float width = box.width() * clamped;
    const Rect filled = {box.min,
                         {box.min.x + std::max(width, box.height()), box.max.y}};
    list.addRoundedRect(filled, box.height() * 0.5f, fill);
}

// A labelled bar: "HULL [=====   ] 62%". Returns nothing; the caller advances.
void drawLabelledMeter(DrawList& list, const Styles& styles, const Rect& row, const char* label,
                       float fraction, const Color& fill)
{
    constexpr float kLabelWidth = 46.0f;
    constexpr float kValueWidth = 44.0f;
    list.addTextInBox(*styles.small, {row.min, {row.min.x + kLabelWidth, row.max.y}}, label, kTextDim);

    const float meterY = row.min.y + (row.height() - kMeterHeight) * 0.5f;
    const Rect meterBox = {{row.min.x + kLabelWidth + 6.0f, meterY},
                           {row.max.x - kValueWidth - 10.0f, meterY + kMeterHeight}};
    drawMeter(list, meterBox, fraction, fill);

    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0f%%", static_cast<double>(clamp01(fraction) * 100.0f));
    list.addTextInBox(*styles.small, {{row.max.x - kValueWidth, row.min.y}, row.max}, buffer, kTextDim,
                      TextAlign::Right);
}

// A status pill. Returns the width it consumed, so a row of them can be laid
// out left to right without measuring twice.
float drawChip(DrawList& list, const Font& font, const FontStyleRecord& style, Vec2 topLeft,
               float height, const char* text, const Color& color)
{
    constexpr float kChipPadding = 8.0f;
    const float width = font.measureWidth(style, text) + kChipPadding * 2.0f;
    const Rect box = {topLeft, {topLeft.x + width, topLeft.y + height}};
    list.addRoundedRect(box, height * 0.35f, color.withAlpha(0.18f));
    list.addTextInBox(style, box, text, color, TextAlign::Center);
    return width;
}

// Camera-space direction to a screen point. `inFront` is false when the target
// is behind the near plane, where the projection would mirror it.
struct Projection
{
    Vec2 position;
    bool inFront = false;
};

[[nodiscard]] Projection project(const Vec3& directionCamera, Vec2 center, float focal)
{
    if (directionCamera.z >= -0.01f) { // camera looks down -Z
        return {};
    }
    const float depth = -directionCamera.z;
    return {{center.x + (directionCamera.x / depth) * focal,
             center.y - (directionCamera.y / depth) * focal},
            true};
}

// Boresight crosshair, shield facings, and the screen-edge flash while hits are
// landing (decisions/002: fore arc above, aft arc below).
void drawReticle(DrawList& list, Vec2 center, Vec2 screenSize, const sol::ui::FlightHud& hud)
{
    const float flash = clamp01(hud.damageFlash);
    if (flash > 0.0f) {
        // Nested outlines fake a gradient inward from the screen edge; a single
        // band would read as a hard red frame.
        for (int ring = 0; ring < 4; ++ring) {
            const float inset = static_cast<float>(ring) * 10.0f;
            const float alpha = 0.30f * flash * (1.0f - static_cast<float>(ring) * 0.25f);
            list.addRectOutline({{inset, inset}, {screenSize.x - inset, screenSize.y - inset}},
                                kReticleHit.withAlpha(alpha), 10.0f);
        }
    }

    const Color color = mix(kReticle, kReticleHit, flash);
    list.addCircle(center, 10.0f, color, 1.5f, 24);
    list.addLine({center.x - 18.0f, center.y}, {center.x - 10.0f, center.y}, color, 1.5f);
    list.addLine({center.x + 10.0f, center.y}, {center.x + 18.0f, center.y}, color, 1.5f);
    list.addLine({center.x, center.y - 18.0f}, {center.x, center.y - 10.0f}, color, 1.5f);

    // Arc length tracks the facing's remaining strength, over a dim track so a
    // collapsed shield reads as gone rather than as missing UI.
    constexpr float kFullSpan = kPi * 0.35f;
    const auto shieldArc = [&](float fraction, bool fore) {
        const float centerAngle = fore ? -kPi * 0.5f : kPi * 0.5f;
        list.addArc(center, 24.0f, centerAngle - kFullSpan, centerAngle + kFullSpan,
                    kShield.withAlpha(0.20f), 2.5f, 24);
        const float half = kFullSpan * clamp01(fraction);
        if (half > 0.0f) {
            list.addArc(center, 24.0f, centerAngle - half, centerAngle + half,
                        kShield.withAlpha(0.82f), 2.5f, 24);
        }
    };
    shieldArc(hud.shieldFore, true);
    shieldArc(hud.shieldAft, false);
}

// Where to put the shots so they meet the target's path.
void drawLeadMarker(DrawList& list, Vec2 center, float focal, const sol::ui::FlightHud& hud)
{
    if (!hud.hasLead) {
        return;
    }
    const Projection lead = project(hud.leadDirectionCamera, center, focal);
    if (!lead.inFront) {
        return;
    }
    list.addCircle(lead.position, 5.0f, kLead, 1.8f, 16);
    list.addLine({lead.position.x - 9.0f, lead.position.y}, {lead.position.x - 5.0f, lead.position.y},
                 kLead, 1.5f);
    list.addLine({lead.position.x + 5.0f, lead.position.y}, {lead.position.x + 9.0f, lead.position.y},
                 kLead, 1.5f);
}

// Diamond over an on-screen target with its range beside it; an edge arrow
// pointing the way when it is off screen or behind.
void drawTargetMarker(DrawList& list, const Styles& styles, Vec2 center, Vec2 screenSize, float focal,
                      const sol::ui::FlightHud& hud)
{
    const Projection target = project(hud.targetDirectionCamera, center, focal);
    constexpr float kEdgeMargin = 24.0f;
    const bool onScreen = target.inFront && target.position.x > kEdgeMargin &&
                          target.position.x < (screenSize.x - kEdgeMargin) &&
                          target.position.y > kEdgeMargin &&
                          target.position.y < (screenSize.y - kEdgeMargin);
    if (onScreen) {
        list.addCircle(target.position, 14.0f, kTargetMark, 2.0f, 4); // a 4-segment ring is a diamond
        char distance[32] = {};
        formatDistance(hud.targetDistanceMeters, distance, sizeof(distance));
        list.addTextInBox(*styles.small,
                          {{target.position.x + 20.0f, target.position.y - 11.0f},
                           {target.position.x + 160.0f, target.position.y + 11.0f}},
                          distance, kTargetMark);
        return;
    }

    // Screen-space direction to the target: y grows downward, and a target
    // behind the camera is pointed at by its lateral component alone.
    const float x = hud.targetDirectionCamera.x;
    const float y = -hud.targetDirectionCamera.y;
    const float length = std::sqrt(x * x + y * y);
    const float nx = length > 0.0001f ? x / length : 0.0f;
    const float ny = length > 0.0001f ? y / length : -1.0f;
    const float ringRadius = std::min(screenSize.x, screenSize.y) * 0.38f;
    const Vec2 tip = {center.x + nx * ringRadius, center.y + ny * ringRadius};
    const Vec2 back = {center.x + nx * (ringRadius - 16.0f), center.y + ny * (ringRadius - 16.0f)};
    const Vec2 side = {-ny * 7.0f, nx * 7.0f};
    list.addTriangle(tip, {back.x + side.x, back.y + side.y}, {back.x - side.x, back.y - side.y},
                     kTargetMark);
}

// Bottom left: where the ship is, how fast, and what it has left.
void drawFlightPanel(DrawList& list, const Font& font, const Styles& styles, Vec2 screenSize,
                     const sol::ui::FlightHud& hud, Rect& panelOut)
{
    const Rect panel = {{kMargin, screenSize.y - kMargin - kFlightPanelHeight},
                        {kMargin + kFlightPanelWidth, screenSize.y - kMargin}};
    panelOut = panel;
    list.addRoundedRect(panel, 8.0f, kPanel);
    list.addRoundedRect({{panel.min.x, panel.min.y}, {panel.min.x + 3.0f, panel.max.y}}, 1.5f, kAccent);

    // Clip the contents so a long system name cannot spill out.
    list.pushClip({{panel.min.x + kPadding, panel.min.y}, {panel.max.x - kPadding, panel.max.y}});

    const float left = panel.min.x + kPadding;
    const float right = panel.max.x - kPadding;
    float y = panel.min.y + kPadding * 0.5f;

    const Rect titleRow = {{left, y}, {right, y + 34.0f}};
    list.addTextInBox(*styles.heading, titleRow, hud.systemName, kTextPrimary);
    y += 36.0f;

    char buffer[64] = {};
    formatSpeed(hud.speedMetersPerSecond, buffer, sizeof(buffer));
    list.addTextInBox(*styles.body, {{left, y}, {right, y + kRowHeight}}, buffer, kTextDim);
    list.addTextInBox(*styles.small, {{left, y}, {right, y + kRowHeight}}, hud.cameraMode,
                      kTextDim.withAlpha(0.7f), TextAlign::Right);
    y += kRowHeight + 4.0f;

    drawLabelledMeter(list, styles, {{left, y}, {right, y + 20.0f}}, "HULL", hud.hull, kHull);
    y += 20.0f;
    drawLabelledMeter(list, styles, {{left, y}, {right, y + 20.0f}}, "SH-F", hud.shieldFore, kShield);
    y += 20.0f;
    drawLabelledMeter(list, styles, {{left, y}, {right, y + 20.0f}}, "SH-A", hud.shieldAft, kShield);
    y += 24.0f;

    // Flight-mode chips, laid out until the row runs out; anything that would
    // overflow is dropped rather than clipped to an unreadable stub.
    constexpr float kChipHeight = 20.0f;
    struct Chip
    {
        const char* text;
        Color color;
        bool show;
    };
    const Chip chips[] = {
        {hud.assist ? "ASSIST" : "MANUAL", hud.assist ? kFriendly : kWarning, true},
        {"BOOST", kWarning, hud.boost},
        {"CRUISE", kAccent, hud.cruise},
        {"AUTO", rgba(0xCC99FFFFu), hud.autopilot},
    };
    float x = left;
    for (const Chip& chip : chips) {
        if (!chip.show) {
            continue;
        }
        const float width = font.measureWidth(*styles.small, chip.text) + 16.0f;
        if (x + width > right) {
            break;
        }
        x += drawChip(list, font, *styles.small, {x, y}, kChipHeight, chip.text, chip.color) + 6.0f;
    }

    list.popClip();
}

// Bottom right: power distribution (decisions/003) and weapon capacitor.
void drawPowerPanel(DrawList& list, const Styles& styles, Vec2 screenSize,
                    const sol::ui::FlightHud& hud)
{
    const float rows = 4.0f; // WEP / ENG / SYS / CAP
    const float height = kPadding + rows * 20.0f + kPadding * 0.5f;
    const Rect panel = {{screenSize.x - kMargin - kPowerPanelWidth, screenSize.y - kMargin - height},
                        {screenSize.x - kMargin, screenSize.y - kMargin}};
    list.addRoundedRect(panel, 8.0f, kPanel);
    list.pushClip(panel);

    const float left = panel.min.x + kPadding;
    const float right = panel.max.x - kPadding;
    float y = panel.min.y + kPadding * 0.5f;

    constexpr float kLabelWidth = 38.0f;
    const int pipMax = std::clamp(hud.pipMax, 1, 12);
    const struct
    {
        const char* label;
        int pips;
        Color color;
    } bars[] = {
        {"WEP", hud.pipsWeapons, kPipWeapons},
        {"ENG", hud.pipsEngines, kPipEngines},
        {"SYS", hud.pipsShields, kPipShields},
    };

    for (const auto& bar : bars) {
        const Rect row = {{left, y}, {right, y + 20.0f}};
        list.addTextInBox(*styles.small, {row.min, {left + kLabelWidth, row.max.y}}, bar.label,
                          kTextDim);

        // Pips are discrete on purpose: the player counts them at a glance
        // rather than reading a bar's length.
        const float pipsLeft = left + kLabelWidth + 4.0f;
        const float spacing = 4.0f;
        const float pipWidth =
            ((right - pipsLeft) - spacing * static_cast<float>(pipMax - 1)) / static_cast<float>(pipMax);
        const float pipTop = y + (20.0f - 12.0f) * 0.5f;
        for (int pip = 0; pip < pipMax; ++pip) {
            const float pipX = pipsLeft + static_cast<float>(pip) * (pipWidth + spacing);
            const Rect cell = {{pipX, pipTop}, {pipX + pipWidth, pipTop + 12.0f}};
            list.addRoundedRect(cell, 2.0f, pip < bar.pips ? bar.color : kMeterTrack);
        }
        y += 20.0f;
    }

    const Rect capRow = {{left, y}, {right, y + 20.0f}};
    list.addTextInBox(*styles.small, {capRow.min, {left + kLabelWidth, capRow.max.y}}, "CAP", kTextDim);
    const float meterY = y + (20.0f - kMeterHeight) * 0.5f;
    drawMeter(list, {{left + kLabelWidth + 4.0f, meterY}, {right - 44.0f, meterY + kMeterHeight}},
              hud.weaponCharge, kPipWeapons);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0f%%",
                  static_cast<double>(clamp01(hud.weaponCharge) * 100.0f));
    list.addTextInBox(*styles.small, {{right - 40.0f, y}, {right, y + 20.0f}}, buffer, kTextDim,
                      TextAlign::Right);

    list.popClip();
}

// Top right: who is targeted, whose side they are on, and what shape they are in.
void drawTargetReadout(DrawList& list, const Styles& styles, Vec2 screenSize,
                       const sol::ui::FlightHud& hud, Rect& panelOut)
{
    panelOut = {{screenSize.x - kMargin, kMargin}, {screenSize.x - kMargin, kMargin}};
    if (!has(hud.targetName)) {
        return;
    }
    const bool showFaction = has(hud.targetFaction);
    const float height = kPadding + 26.0f + (showFaction ? 20.0f : 0.0f) + 20.0f +
                         (hud.targetIsShip ? 3.0f * 18.0f + 4.0f : 0.0f) + kPadding * 0.5f;
    const Rect panel = {{screenSize.x - kMargin - kTargetPanelWidth, kMargin},
                        {screenSize.x - kMargin, kMargin + height}};
    panelOut = panel;
    list.addRoundedRect(panel, 8.0f, kPanel);
    list.addRoundedRect({{panel.max.x - 3.0f, panel.min.y}, {panel.max.x, panel.max.y}}, 1.5f,
                        kTargetMark);
    list.pushClip({{panel.min.x + kPadding, panel.min.y}, {panel.max.x - kPadding, panel.max.y}});

    const float left = panel.min.x + kPadding;
    const float right = panel.max.x - kPadding;
    float y = panel.min.y + kPadding * 0.5f;

    list.addTextInBox(*styles.strong, {{left, y}, {right, y + 26.0f}}, hud.targetName, kTextPrimary);
    y += 26.0f;

    if (showFaction) {
        char buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer), "%s [%s]", hud.targetFaction,
                      has(hud.targetAttitude) ? hud.targetAttitude : "unknown");
        list.addTextInBox(*styles.small, {{left, y}, {right, y + 20.0f}}, buffer,
                          attitudeColor(hud.targetAttitude));
        y += 20.0f;
    }

    char distance[32] = {};
    formatDistance(hud.targetDistanceMeters, distance, sizeof(distance));
    char closing[32] = {};
    formatClosing(hud.closingSpeedMetersPerSecond, closing, sizeof(closing));
    list.addTextInBox(*styles.small, {{left, y}, {right, y + 20.0f}}, distance, kTextDim);
    // Closing is signed: negative means the gap is opening.
    list.addTextInBox(*styles.small, {{left, y}, {right, y + 20.0f}}, closing,
                      hud.closingSpeedMetersPerSecond < 0.0f ? kWarning : kTextDim, TextAlign::Right);
    y += 24.0f;

    if (hud.targetIsShip) {
        drawLabelledMeter(list, styles, {{left, y}, {right, y + 18.0f}}, "SH-F", hud.targetShieldFore,
                          kShield);
        y += 18.0f;
        drawLabelledMeter(list, styles, {{left, y}, {right, y + 18.0f}}, "SH-A", hud.targetShieldAft,
                          kShield);
        y += 18.0f;
        drawLabelledMeter(list, styles, {{left, y}, {right, y + 18.0f}}, "HULL", hud.targetHull, kHull);
    }

    list.popClip();
}

// Contextual prompts sit under the crosshair, where the eye already is.
void drawPrompts(DrawList& list, const Font& font, const Styles& styles, Vec2 center,
                 const sol::ui::FlightHud& hud)
{
    struct Prompt
    {
        const char* text;
        bool show;
    };
    // Undocking is the station screen's business: this HUD only ever draws in
    // flight, so a docked prompt here would be unreachable. Salvage shares the
    // interact key with docking; a station in range wins, since you cannot be
    // parked on a pad and inside a wreck at once.
    const Prompt prompts[] = {
        {"[J] JUMP", hud.gateInRange},
        {"[G] DOCK", hud.dockInRange},
        {"[G] SALVAGE", hud.salvageInRange && !hud.dockInRange},
    };
    constexpr float kChipHeight = 24.0f;
    constexpr float kChipGap = 8.0f;

    float total = 0.0f;
    for (const Prompt& prompt : prompts) {
        if (prompt.show) {
            total += font.measureWidth(*styles.small, prompt.text) + 16.0f + kChipGap;
        }
    }
    if (total <= 0.0f) {
        return;
    }
    total -= kChipGap;

    float x = center.x - total * 0.5f;
    const float y = center.y + 56.0f;
    for (const Prompt& prompt : prompts) {
        if (!prompt.show) {
            continue;
        }
        x += drawChip(list, font, *styles.small, {x, y}, kChipHeight, prompt.text, kAccent) + kChipGap;
    }
}

// --- Contact radar (Phase 8h) ------------------------------------------------

constexpr float kRadarRadius = 78.0f;
constexpr float kRadarStalkLimit = 13.0f;
// Contacts are positioned on a disc inset by the stalk's reach, so a rim
// contact with a tall stalk still lands inside the rim. Without the inset the
// blips visibly escape the circle, which reads as a drawing bug rather than
// as altitude. Costs a sixth of the radius and keeps every bearing exact -
// the alternative, clamping the blip back afterwards, would put a high far
// contact at the same place as a lower nearer one.
constexpr float kRadarPlotRadius = kRadarRadius - kRadarStalkLimit;

// Colour per contact kind. Ships take their colour from allegiance because
// that is the question being asked of them; scenery is coded by what it is.
[[nodiscard]] Color radarColor(const sol::ui::RadarContact& contact)
{
    switch (contact.kind) {
    case sol::ui::RadarKind::Ship:
        switch (contact.attitude) {
        case sol::ui::RadarAttitude::Hostile: return kHostile;
        case sol::ui::RadarAttitude::Friendly: return kFriendly;
        case sol::ui::RadarAttitude::Neutral: break;
        }
        return kNeutral;
    case sol::ui::RadarKind::Station: return kAccent;
    case sol::ui::RadarKind::Gate: return rgba(0x9E7BFFFFu);
    case sol::ui::RadarKind::Planet:
    case sol::ui::RadarKind::Star: return rgba(0x7E8C99FFu);
    case sol::ui::RadarKind::Signal: return rgba(0x63D8C4FFu);
    case sol::ui::RadarKind::Field: return rgba(0xC2A36BFFu);
    case sol::ui::RadarKind::Wreck: return rgba(0xA88C6EFFu);
    case sol::ui::RadarKind::Bookmark: return rgba(0xFFC850FFu);
    // Deliberately nothing like the bookmark's gold: "somewhere I chose" and
    // "somewhere I was sent" must never read alike.
    case sol::ui::RadarKind::Objective: return rgba(0xFF66C4FFu);
    }
    return kNeutral;
}

// Bottom centre: the contact disc. Top of the disc is dead ahead and it turns
// with the ship, so a dot's position is a heading to steer, not a map bearing.
// Sits between the flight panel (bottom left) and the power panel (bottom
// right), which is the only bottom space neither of them claims.
void drawRadar(DrawList& list, const Font& font, const Styles& styles, Vec2 screenSize,
               const sol::ui::FlightHud& hud)
{
    constexpr float kRangeLabelBand = 20.0f;
    const Vec2 center = {screenSize.x * 0.5f,
                         screenSize.y - kMargin - kRangeLabelBand - kRadarRadius};

    // The disc itself: a filled ground, a rim, and one intermediate ring so
    // the log compression has something to be read against.
    list.addRoundedRect({{center.x - kRadarRadius, center.y - kRadarRadius},
                         {center.x + kRadarRadius, center.y + kRadarRadius}},
                        kRadarRadius, kPanel);
    list.addCircle(center, kRadarRadius, kAccent.withAlpha(0.45f), 1.5f, 48);
    list.addCircle(center, kRadarRadius * 0.5f, kAccent.withAlpha(0.16f), 1.0f, 36);

    // Cross hairs mark the ship's own axes: up the disc is forward.
    list.addLine({center.x, center.y - kRadarRadius}, {center.x, center.y + kRadarRadius},
                 kAccent.withAlpha(0.12f), 1.0f);
    list.addLine({center.x - kRadarRadius, center.y}, {center.x + kRadarRadius, center.y},
                 kAccent.withAlpha(0.12f), 1.0f);

    const float range = sol::ui::radarRange(hud.radarRangeMeters > 0.0
                                                ? hud.radarRangeMeters
                                                : sol::ui::kRadarRangeMeters);
    for (const sol::ui::RadarContact& contact : hud.radarContacts) {
        const Vec2 point = sol::ui::radarPoint(contact.offset, center, kRadarPlotRadius, range);
        const bool beyond = sol::ui::radarBeyondRange(contact.offset, range);
        const Color color = radarColor(contact);

        // The stalk carries height, which is the one thing a flat disc cannot
        // say by itself. Drawn first so the dot sits on top of its own line.
        const float stalk = sol::ui::radarStalk(contact.offset, kRadarStalkLimit);
        if (std::abs(stalk) > 0.5f) {
            list.addLine(point, {point.x, point.y + stalk}, color.withAlpha(0.5f), 1.0f);
        }
        const Vec2 dot = {point.x, point.y + stalk};

        // The objective reads a size up: on a disc of thirty contacts, the one
        // the player was actually told to reach should not need finding.
        const bool objective = contact.kind == sol::ui::RadarKind::Objective;
        const float size = contact.isTarget || objective ? 4.0f : 3.0f;
        if (beyond) {
            // Past the outer ring: hollow, so "beyond the radar" never reads
            // as "at the edge of the radar". Drawn a touch larger and with
            // enough segments to read as a ring rather than as a lumpy
            // polygon at three pixels.
            list.addCircle(dot, size + 1.0f, color, 1.25f, 16);
        } else {
            list.addRoundedRect({{dot.x - size, dot.y - size}, {dot.x + size, dot.y + size}},
                                size, color);
        }
        if (contact.isTarget) {
            list.addCircle(dot, size + 3.5f, kTargetMark, 1.25f, 12);
        } else if (objective) {
            list.addCircle(dot, size + 2.5f, color.withAlpha(0.55f), 1.0f, 12);
        }
    }

    // The ship, dead centre, pointing up the disc.
    list.addTriangle({center.x, center.y - 5.0f}, {center.x - 3.5f, center.y + 4.0f},
                     {center.x + 3.5f, center.y + 4.0f}, kTextPrimary);

    // Label the rim with the distance it stands for; a disc whose scale is a
    // secret is a decoration.
    char buffer[32] = {};
    formatDistance(hud.radarRangeMeters > 0.0 ? hud.radarRangeMeters
                                              : sol::ui::kRadarRangeMeters,
                   buffer, sizeof(buffer));
    const float width = font.measureWidth(*styles.small, buffer);
    list.addTextInBox(*styles.small,
                      {{center.x - width * 0.5f, center.y + kRadarRadius + 4.0f},
                       {center.x + width * 0.5f, center.y + kRadarRadius + kRangeLabelBand}},
                      buffer, kTextDim.withAlpha(0.75f), TextAlign::Center);
}

// Scanner block: the pulse's charge, what the held scan is resolving, and how
// many contacts are still unidentified here. It sits under the target readout
// because it is the same kind of information - what the sensors have found -
// and because the top left belongs to the dev overlay in dev builds.
void drawScannerPanel(DrawList& list, const Styles& styles, Vec2 screenSize,
                      const Rect& targetPanel, const sol::ui::FlightHud& hud)
{
    const bool scanning = has(hud.scanTarget);
    const bool contacts = hud.contactsUnresolved > 0 || hud.sitesOpen > 0;
    // Prospecting (Phase 8f): what the nose is on, and what is still in it.
    const bool prospecting = has(hud.prospectName);
    const bool collecting = hud.collectedUnits > 0.0f;
    if (!scanning && !contacts && !prospecting && !collecting && hud.pulseCharge >= 1.0f
        && !has(hud.routeNextHop)) {
        return; // nothing to say: keep the view clear
    }
    constexpr float kWidth = 268.0f;
    const float rows = 1.0f + (scanning ? 1.0f : 0.0f) + (contacts ? 1.0f : 0.0f)
                       + (prospecting ? 2.0f : 0.0f) + (collecting ? 1.0f : 0.0f)
                       + (has(hud.routeNextHop) ? 1.0f : 0.0f);
    const float height = kPadding + rows * 20.0f + kPadding * 0.5f;
    const float top = targetPanel.max.y + (targetPanel.height() > 0.0f ? 8.0f : 0.0f);
    const Rect panel = {{screenSize.x - kMargin - kWidth, top},
                        {screenSize.x - kMargin, top + height}};
    list.addRoundedRect(panel, 8.0f, kPanel);
    list.pushClip({{panel.min.x + kPadding, panel.min.y}, {panel.max.x - kPadding, panel.max.y}});

    const float left = panel.min.x + kPadding;
    const float right = panel.max.x - kPadding;
    float y = panel.min.y + kPadding * 0.5f;

    // [R] reads as ready in accent, charging in dim, with the bar underneath.
    const bool ready = hud.pulseCharge >= 1.0f;
    list.addTextInBox(*styles.small, {{left, y}, {left + 90.0f, y + 20.0f}}, "[R] SCAN",
                      ready ? kAccent : kTextDim);
    const float meterY = y + (20.0f - kMeterHeight) * 0.5f;
    drawMeter(list, {{left + 96.0f, meterY}, {right, meterY + kMeterHeight}}, hud.pulseCharge,
              ready ? kAccent : kTextDim);
    y += 20.0f;

    if (scanning) {
        list.addTextInBox(*styles.small, {{left, y}, {right - 46.0f, y + 20.0f}}, hud.scanTarget,
                          kTextPrimary);
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%.0f%%",
                      static_cast<double>(clamp01(hud.scanProgress) * 100.0f));
        list.addTextInBox(*styles.small, {{right - 44.0f, y}, {right, y + 20.0f}}, buffer, kAccent,
                          TextAlign::Right);
        y += 20.0f;
    }
    if (contacts) {
        char buffer[96] = {};
        std::snprintf(buffer, sizeof(buffer), "%d unidentified - %d site(s) open",
                      hud.contactsUnresolved, hud.sitesOpen);
        list.addTextInBox(*styles.small, {{left, y}, {right, y + 20.0f}}, buffer, kWarning);
        y += 20.0f;
    }
    if (prospecting) {
        // In range reads in accent, out of range dim: the whole readout is
        // there to answer "can I cut this from here, and is it worth it".
        const Color tone = hud.prospectInRange ? kAccent : kTextDim;
        list.addTextInBox(*styles.small, {{left, y}, {right - 70.0f, y + 20.0f}},
                          hud.prospectName, tone);
        char range[32] = {};
        std::snprintf(range, sizeof(range), "%.1f km", hud.prospectDistance / 1000.0);
        list.addTextInBox(*styles.small, {{right - 68.0f, y}, {right, y + 20.0f}}, range, tone,
                          TextAlign::Right);
        y += 20.0f;
        char amount[64] = {};
        std::snprintf(amount, sizeof(amount), hud.prospectIsWreck ? "%.0f units aboard"
                                                                  : "%.0f of %.0f units left",
                      static_cast<double>(hud.prospectLeft),
                      static_cast<double>(hud.prospectTotal));
        list.addTextInBox(*styles.small, {{left, y}, {right - 46.0f, y + 20.0f}}, amount,
                          kTextPrimary);
        if (!hud.prospectIsWreck && hud.prospectTotal > 0.0f) {
            const float meterY2 = y + (20.0f - kMeterHeight) * 0.5f;
            drawMeter(list, {{right - 44.0f, meterY2}, {right, meterY2 + kMeterHeight}},
                      clamp01(hud.prospectLeft / hud.prospectTotal), tone);
        }
        y += 20.0f;
    }
    if (collecting) {
        char buffer[96] = {};
        std::snprintf(buffer, sizeof(buffer), "+%.1f %s", static_cast<double>(hud.collectedUnits),
                      hud.collectedName);
        list.addTextInBox(*styles.small, {{left, y}, {right, y + 20.0f}}, buffer, kAccent);
        y += 20.0f;
    }
    if (has(hud.routeNextHop)) {
        char buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer), "ROUTE > %s", hud.routeNextHop);
        list.addTextInBox(*styles.small, {{left, y}, {right, y + 20.0f}}, buffer, kAccent);
    }
    list.popClip();
}

// A ring closing around the crosshair as the target scan completes: the
// progress belongs where the player's eye already is, not only in a panel.
void drawScanRing(DrawList& list, Vec2 center, const sol::ui::FlightHud& hud)
{
    if (!has(hud.scanTarget) || hud.scanProgress <= 0.0f) {
        return;
    }
    const float sweep = 2.0f * kPi * clamp01(hud.scanProgress);
    list.addCircle(center, 32.0f, kAccent.withAlpha(0.18f), 2.0f, 32);
    list.addArc(center, 32.0f, -kPi * 0.5f, -kPi * 0.5f + sweep, kAccent.withAlpha(0.9f), 2.0f, 32);
}

// The tracked mission (Phase 8c) rides above the flight panel: title, current
// objective, and the deadline counting down.
void drawMissionLine(DrawList& list, const Styles& styles, const Rect& flightPanel,
                     const sol::ui::FlightHud& hud)
{
    if (!has(hud.missionTitle)) {
        return;
    }
    constexpr float kWidth = 420.0f;
    const float bottom = flightPanel.min.y - 8.0f;
    const Rect titleRow = {{flightPanel.min.x, bottom - 44.0f}, {flightPanel.min.x + kWidth, bottom - 22.0f}};
    const Rect objectiveRow = {{flightPanel.min.x, bottom - 22.0f}, {flightPanel.min.x + kWidth, bottom}};

    list.pushClip({titleRow.min, objectiveRow.max});
    list.addTextInBox(*styles.strong, titleRow, hud.missionTitle, kAccent);
    if (hud.missionDeadline > 0.0) {
        char buffer[32] = {};
        const int seconds = static_cast<int>(hud.missionDeadline);
        std::snprintf(buffer, sizeof(buffer), "%d:%02d", seconds / 60, seconds % 60);
        list.addTextInBox(*styles.small, titleRow, buffer, kWarning, TextAlign::Right);
    }
    if (has(hud.missionObjective)) {
        list.addTextInBox(*styles.small, objectiveRow, hud.missionObjective, kTextDim);
    }
    list.popClip();
}

} // namespace

void buildFlightUi(DrawList& list, const Font& font, Vec2 screenSize, const sol::ui::FlightHud& hud)
{
    if (!hud.active || !font.valid() || screenSize.x <= 0.0f || screenSize.y <= 0.0f) {
        return;
    }
    Styles styles;
    styles.heading = font.style("heading");
    styles.body = font.style("body");
    styles.strong = font.style("body_strong");
    styles.small = font.style("hud");
    if (styles.heading == nullptr || styles.body == nullptr || styles.strong == nullptr ||
        styles.small == nullptr) {
        return;
    }

    // World-space markers project against the same virtual screen the panels
    // are laid out in, so the UI scale moves the whole HUD together.
    const Vec2 center = {screenSize.x * 0.5f, screenSize.y * 0.5f};
    const float tanHalfFov = hud.tanHalfFovY > 0.0001f ? hud.tanHalfFovY : 1.0f;
    const float focal = (screenSize.y * 0.5f) / tanHalfFov;

    drawReticle(list, center, screenSize, hud);
    drawScanRing(list, center, hud);
    drawLeadMarker(list, center, focal, hud);
    if (has(hud.targetName)) {
        drawTargetMarker(list, styles, center, screenSize, focal, hud);
    }
    drawPrompts(list, font, styles, center, hud);

    Rect flightPanel = {};
    Rect targetPanel = {};
    drawFlightPanel(list, font, styles, screenSize, hud, flightPanel);
    drawPowerPanel(list, styles, screenSize, hud);
    drawTargetReadout(list, styles, screenSize, hud, targetPanel);
    drawMissionLine(list, styles, flightPanel, hud);
    drawScannerPanel(list, styles, screenSize, targetPanel, hud);
    drawRadar(list, font, styles, screenSize, hud);
}

} // namespace game
