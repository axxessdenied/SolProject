#include "game_ui.hpp"

#include <cstdio>

namespace game {

using sol::assets::Font;
using sol::assets::FontStyleRecord;
using sol::core::Vec2;
using sol::ui::Color;
using sol::ui::DrawList;
using sol::ui::Rect;
using sol::ui::rgba;
using sol::ui::TextAlign;

namespace {

// Provisional palette; it moves into a Theme once widgets need to share it.
constexpr Color kPanel = rgba(0x0E141ACCu);
constexpr Color kTextPrimary = rgba(0xE8F0F8FFu);
constexpr Color kTextDim = rgba(0x8FA6BCFFu);
constexpr Color kAccent = rgba(0x58C8F0FFu);
constexpr Color kHull = rgba(0xE0704CFFu);
constexpr Color kShield = rgba(0x58A8F0FFu);
constexpr Color kMeterTrack = rgba(0x1E2A36FFu);

constexpr float kPanelWidth = 340.0f;
constexpr float kPanelHeight = 150.0f;
constexpr float kMargin = 24.0f;
constexpr float kPadding = 14.0f;
constexpr float kRowHeight = 22.0f;

void drawMeter(DrawList& list, const Rect& box, float fraction, const Color& fill)
{
    list.addRoundedRect(box, box.height() * 0.5f, kMeterTrack);
    const float clamped = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    if (clamped <= 0.0f) {
        return;
    }
    // Keep the filled end round even when nearly empty.
    const float width = box.width() * clamped;
    const Rect filled = {box.min, {box.min.x + (width < box.height() ? box.height() : width), box.max.y}};
    list.addRoundedRect(filled, box.height() * 0.5f, fill);
}

} // namespace

void buildFlightUi(DrawList& list, const Font& font, Vec2 screenSize, const sol::ui::FlightHud& hud)
{
    if (!hud.active || !font.valid()) {
        return;
    }
    const FontStyleRecord* heading = font.style("heading");
    const FontStyleRecord* body = font.style("body");
    const FontStyleRecord* hudStyle = font.style("hud");
    if (heading == nullptr || body == nullptr || hudStyle == nullptr) {
        return;
    }

    const Rect panel = {{kMargin, screenSize.y - kMargin - kPanelHeight},
                        {kMargin + kPanelWidth, screenSize.y - kMargin}};
    list.addRoundedRect(panel, 8.0f, kPanel);
    list.addRoundedRect({{panel.min.x, panel.min.y}, {panel.min.x + 3.0f, panel.max.y}}, 1.5f, kAccent);

    // Clip the contents so a long station or system name cannot spill out.
    list.pushClip({{panel.min.x + kPadding, panel.min.y}, {panel.max.x - kPadding, panel.max.y}});

    const float left = panel.min.x + kPadding;
    const float right = panel.max.x - kPadding;
    float y = panel.min.y + kPadding * 0.5f;

    const Rect titleRow = {{left, y}, {right, y + 34.0f}};
    list.addTextInBox(*heading, titleRow, hud.docked ? hud.dockedStationName : hud.systemName,
                      kTextPrimary);
    y += 36.0f;

    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0f m/s", static_cast<double>(hud.speedMetersPerSecond));
    list.addTextInBox(*body, {{left, y}, {right, y + kRowHeight}}, buffer, kTextDim);

    const char* mode = hud.autopilot ? "AUTOPILOT" : (hud.cruise ? "CRUISE" : (hud.boost ? "BOOST" : ""));
    if (mode[0] != '\0') {
        list.addTextInBox(*body, {{left, y}, {right, y + kRowHeight}}, mode, kAccent, TextAlign::Right);
    }
    y += kRowHeight + 8.0f;

    // Hull and shields: label, meter, value.
    const float labelWidth = 46.0f;
    const float meterHeight = 8.0f;
    const struct
    {
        const char* label;
        float value;
        Color color;
    } bars[] = {
        {"HULL", hud.hull, kHull},
        {"SHLD", (hud.shieldFore + hud.shieldAft) * 0.5f, kShield},
    };

    for (const auto& bar : bars) {
        const Rect labelBox = {{left, y}, {left + labelWidth, y + kRowHeight}};
        list.addTextInBox(*hudStyle, labelBox, bar.label, kTextDim);

        const float meterY = y + (kRowHeight - meterHeight) * 0.5f;
        const float valueWidth = 44.0f;
        const Rect meterBox = {{left + labelWidth + 6.0f, meterY},
                               {right - valueWidth - 10.0f, meterY + meterHeight}};
        drawMeter(list, meterBox, bar.value, bar.color);

        std::snprintf(buffer, sizeof(buffer), "%.0f%%", static_cast<double>(bar.value * 100.0f));
        list.addTextInBox(*hudStyle, {{right - valueWidth, y}, {right, y + kRowHeight}}, buffer, kTextDim,
                          TextAlign::Right);
        y += kRowHeight;
    }

    list.popClip();

    // Tracked mission sits above the panel, where the eye already is.
    if (hud.missionTitle != nullptr && hud.missionTitle[0] != '\0') {
        const Rect missionRow = {{kMargin, panel.min.y - 26.0f}, {kMargin + kPanelWidth, panel.min.y - 4.0f}};
        list.addTextInBox(*hudStyle, missionRow, hud.missionTitle, kAccent);
    }
}

} // namespace game
