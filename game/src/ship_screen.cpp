#include "ship_screen.hpp"

#include "sol/ui/layout.hpp"

#include <algorithm>
#include <cstdio>

namespace game {

using sol::core::Vec2;
using sol::ui::Color;
using sol::ui::Column;
using sol::ui::InfoRow;
using sol::ui::Rect;
using sol::ui::rgba;
using sol::ui::Row;
using sol::ui::TextAlign;
using sol::ui::UiContext;

namespace {

constexpr float kHeaderHeight = 46.0f;
constexpr float kFooterHeight = 40.0f;
constexpr float kRowHeight = 22.0f;
constexpr float kSectionHeader = 26.0f;
constexpr float kButtonWidth = 118.0f;
// The little in-row button a fitted gun carries (Phase 31 stage C3). Narrow on
// purpose: it holds "GRP 2" and nothing longer will ever go in it. The gap is
// generous because the thing to its left is a gun's stat line, which is the
// longest string on this screen and is already being clipped - without it the
// last surviving glyph sits flush against the control.
constexpr float kRowButtonWidth = 58.0f;
constexpr float kRowButtonGap = 12.0f;
constexpr float kMeterHeight = 8.0f;
// Two columns: the fit and the manifest on the right, the numbers on the left.
// One long column would put the cargo manifest below the fold on every ship.
constexpr float kGutter = 20.0f;

constexpr Color kHull = rgba(0xE0704CFFu);
constexpr Color kShield = rgba(0x58A8F0FFu);

void sectionTitle(UiContext& ui, Column& column, const char* title)
{
    const Rect row = column.row(kSectionHeader);
    ui.label(row, title, ui.theme().accent, ui.theme().smallStyle);
    // A hairline under the heading, so the groups read as groups without
    // spending vertical space on padding.
    ui.drawList().addRect({{row.min.x, row.max.y - 1.0f}, {row.max.x, row.max.y}}, ui.theme().panelEdge);
}

// "label ............ value   detail [button]". The value is right-aligned
// against the detail column so the numbers form a column of their own to scan
// down. Returns the row's `action` on the frame its button was clicked, and
// nullptr otherwise - which is every frame for every row that has no button.
const char* infoLine(UiContext& ui, Column& column, const InfoRow& info)
{
    const Rect row = column.row(kRowHeight);
    if (row.empty()) {
        return nullptr;
    }
    const bool hasDetail = info.detail != nullptr && info.detail[0] != '\0';
    const bool hasButton =
        info.button != nullptr && info.button[0] != '\0' && info.action != nullptr && info.action[0] != '\0';
    // The detail column is reserved whether or not THIS row has one, so every
    // value right-aligns to the same edge and the numbers form a column to
    // scan down. Sizing it per row instead made rows without a detail push
    // their value out to the panel edge, which is the opposite of readable.
    //
    // ⚑ A BUTTON TAKES ITS WIDTH OUT OF THE DETAIL COLUMN rather than out of
    // the row, so a section with one button in it keeps every value in the
    // same column as a section with none. A gun's detail is the longest string
    // on this screen and it is already clipped; the numbers lining up is what
    // the column is for.
    const float detailWidth = std::min(row.width() * 0.46f, 250.0f);
    const Rect labelCell = {row.min, {row.max.x - detailWidth, row.max.y}};
    ui.drawList().pushClip(row);
    ui.label(labelCell, info.label, ui.theme().textDim, ui.theme().smallStyle);
    ui.label({{labelCell.min.x, row.min.y}, {labelCell.max.x - 8.0f, row.max.y}},
             info.value,
             ui.theme().textPrimary,
             ui.theme().smallStyle,
             TextAlign::Right);
    const float detailRight = row.max.x - (hasButton ? kRowButtonWidth + kRowButtonGap : 0.0f);
    if (hasDetail) {
        ui.label({{row.max.x - detailWidth + 4.0f, row.min.y}, {detailRight, row.max.y}},
                 info.detail,
                 ui.theme().textDisabled,
                 ui.theme().smallStyle);
    }
    ui.drawList().popClip();
    if (!hasButton) {
        return nullptr;
    }
    // Outside the clip, because a button that is half-drawn is still fully
    // clickable and a control the player can only see part of is worse than
    // one they cannot see at all.
    ui.pushId(info.action);
    const Rect buttonCell = {{row.max.x - kRowButtonWidth, row.min.y + 1.0f}, {row.max.x, row.max.y - 1.0f}};
    const bool clicked = ui.button(buttonCell, info.button);
    ui.popId();
    return clicked ? info.action : nullptr;
}

void meterLine(UiContext& ui, Column& column, const char* label, float fraction, const Color& fill)
{
    const Rect row = column.row(kRowHeight);
    constexpr float kLabelWidth = 84.0f;
    constexpr float kValueWidth = 52.0f;
    ui.label(
        {row.min, {row.min.x + kLabelWidth, row.max.y}}, label, ui.theme().textDim, ui.theme().smallStyle);
    const float y = row.min.y + (row.height() - kMeterHeight) * 0.5f;
    const Rect box = {{row.min.x + kLabelWidth, y}, {row.max.x - kValueWidth, y + kMeterHeight}};
    ui.meter(box, fraction, fill);
    char buffer[32] = {};
    std::snprintf(
        buffer, sizeof(buffer), "%.0f%%", static_cast<double>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f));
    ui.label({{row.max.x - kValueWidth + 6.0f, row.min.y}, row.max},
             buffer,
             ui.theme().textDim,
             ui.theme().smallStyle,
             TextAlign::Right);
}

// Total height a section will need, so the two columns can be measured against
// the panel before either is drawn.
[[nodiscard]] float sectionHeight(std::size_t rows)
{
    return kSectionHeader + static_cast<float>(rows) * kRowHeight + 6.0f;
}

// Returns the action of whatever row button was clicked, or nullptr. A section
// with no buttons in it can never return anything, which is every section this
// screen had before stage C3.
const char* drawSection(UiContext& ui,
                        Column& column,
                        const char* title,
                        std::span<const InfoRow> rows,
                        const char* emptyNote = nullptr)
{
    sectionTitle(ui, column, title);
    if (rows.empty() && emptyNote != nullptr) {
        ui.label(column.row(kRowHeight), emptyNote, ui.theme().textDisabled, ui.theme().smallStyle);
    }
    const char* action = nullptr;
    for (const InfoRow& row : rows) {
        if (const char* clicked = infoLine(ui, column, row)) {
            action = clicked;
        }
    }
    column.skip(6.0f);
    return action;
}

} // namespace

bool buildShipScreen(UiContext& ui, sol::ui::ShipInfoPanel& panel, ShipScreenState& state)
{
    panel.action = "";
    const Vec2 screen = ui.screenSize();
    ui.drawList().addRect({{0.0f, 0.0f}, screen}, ui.theme().background);
    ui.pushId("ship");

    const float width = std::min(screen.x - 80.0f, 1180.0f);
    const float height = std::min(screen.y - 70.0f, 780.0f);
    const Rect frame = {{(screen.x - width) * 0.5f, (screen.y - height) * 0.5f},
                        {(screen.x + width) * 0.5f, (screen.y + height) * 0.5f}};
    ui.panel(frame);

    Column outer(frame, ui.theme().padding, ui.theme().spacing);

    // Header: what this ship is, and what it is worth.
    const Rect header = outer.row(kHeaderHeight);
    ui.label(header, panel.shipName, ui.theme().textPrimary, ui.theme().headingStyle);
    char credits[48] = {};
    std::snprintf(credits, sizeof(credits), "%.0f cr", panel.credits);
    ui.label(header, credits, ui.theme().textDim, ui.theme().bodyStyle, TextAlign::Right);
    ui.label(outer.row(20.0f), panel.shipClass, ui.theme().textDim, ui.theme().smallStyle);
    ui.label(outer.row(20.0f), panel.fitSummary, ui.theme().textDisabled, ui.theme().smallStyle);
    outer.skip(4.0f);

    // Condition and pips: what the ship is doing right now, above the static
    // numbers, because that is what a glance mid-flight is for.
    const Rect condition = outer.row(kRowHeight * 3.0f + 4.0f);
    Row conditionSplit(condition, kGutter);
    Column left(conditionSplit.cell(condition.width() * 0.5f - kGutter * 0.5f), 0.0f, 0.0f);
    meterLine(ui, left, "HULL", panel.hull, kHull);
    meterLine(ui, left, "SHIELD-F", panel.shieldFore, kShield);
    meterLine(ui, left, "SHIELD-A", panel.shieldAft, kShield);
    Column right(conditionSplit.remaining(), 0.0f, 0.0f);
    const float pipMax = panel.pipMax > 0 ? static_cast<float>(panel.pipMax) : 1.0f;
    meterLine(ui, right, "WEP", static_cast<float>(panel.pipsWeapons) / pipMax, rgba(0xFF9973FFu));
    meterLine(ui, right, "ENG", static_cast<float>(panel.pipsEngines) / pipMax, rgba(0x8CDCA0FFu));
    meterLine(ui, right, "SYS", static_cast<float>(panel.pipsShields) / pipMax, rgba(0x80BFFFFFu));
    outer.skip(6.0f);

    const Rect bodyAll = outer.remaining();
    const Rect footer = {{bodyAll.min.x, bodyAll.max.y - kFooterHeight}, bodyAll.max};
    const Rect body = {bodyAll.min, {bodyAll.max.x, footer.min.y - ui.theme().spacing}};

    // Two columns, scrolled together: the left is the numbers, the right is
    // what produces them plus what is in the hold.
    const float contentHeight =
        std::max(sectionHeight(panel.flight.size()) + sectionHeight(panel.defence.size()) +
                     sectionHeight(panel.utility.size()),
                 sectionHeight(panel.fitted.size()) + sectionHeight(panel.cargo.size()));
    const Rect view = ui.beginScroll(body, contentHeight, state.scroll);
    Row split(view, kGutter);
    const float columnWidth = view.width() * 0.5f - kGutter * 0.5f;
    Column numbers(split.cell(columnWidth), 0.0f, 0.0f);
    (void)drawSection(ui, numbers, "FLIGHT", panel.flight);
    (void)drawSection(ui, numbers, "DEFENCE", panel.defence);
    (void)drawSection(ui, numbers, "SENSORS & HOLD", panel.utility);
    Column fit(split.remaining(), 0.0f, 0.0f);
    if (const char* clicked = drawSection(ui, fit, "FITTED", panel.fitted, "nothing fitted")) {
        panel.action = clicked;
    }
    (void)drawSection(ui, fit, "CARGO", panel.cargo, "hold empty");
    ui.endScroll();

    Row buttons(footer, ui.theme().spacing);
    const Rect closeCell = buttons.cellFromRight(kButtonWidth);
    char hold[64] = {};
    std::snprintf(hold,
                  sizeof(hold),
                  "Hold %.1f / %.1f units",
                  static_cast<double>(panel.cargoUsed),
                  static_cast<double>(panel.cargoCapacity));
    ui.label(buttons.remaining(), hold, ui.theme().textDim, ui.theme().smallStyle);
    bool closed = ui.button(closeCell, "Close");

    ui.popId();
    if (ui.cancelRequested()) {
        closed = true;
    }
    return closed;
}

} // namespace game
