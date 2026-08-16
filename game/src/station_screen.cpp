#include "station_screen.hpp"

#include "sol/ui/layout.hpp"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string_view>

namespace game {

using sol::ui::Color;
using sol::ui::Column;
using sol::ui::FactionRow;
using sol::ui::FleetRow;
using sol::ui::MissionRow;
using sol::ui::OutfitRow;
using sol::ui::Rect;
using sol::ui::rgba;
using sol::ui::Row;
using sol::ui::StationAction;
using sol::ui::StationPanel;
using sol::ui::TextAlign;
using sol::ui::TradeRow;
using sol::ui::UiContext;
using sol::ui::inset;

namespace {

constexpr float kRowHeight = 30.0f;
constexpr float kHeaderHeight = 44.0f;
constexpr float kTabHeight = 34.0f;
constexpr float kFooterHeight = 40.0f;
constexpr float kSectionHeight = 26.0f;
constexpr float kButtonWidth = 78.0f;
constexpr float kNumberWidth = 82.0f;

// Campaign missions read gold on the board, the same tell the dev screen used.
constexpr Color kCampaign = rgba(0xF2CC59FFu);

constexpr const char* const kTabLabels[StationScreenState::TabCount] = {
    "Trade", "Outfitting", "Shipyard", "Crew", "Factions", "Missions", "Survey"};

// The amounts a trade button moves. Fixed steps rather than a text field: the
// world clamps to credits, hold space, and stock anyway, so "buy 100" means
// "as much of 100 as I can".
constexpr float kTradeAmounts[] = {1.0f, 10.0f, 100.0f};
constexpr const char* const kTradeAmountLabels[] = {"1", "10", "100"};
constexpr int kTradeAmountCount = 3;

[[nodiscard]] float listHeight(const UiContext& ui, std::size_t rows)
{
    return static_cast<float>(rows) * (kRowHeight + ui.theme().spacing);
}

// Text that must not spill into the next column - detail strings are generated
// from defs and can be any length.
void clipped(UiContext& ui, const Rect& cell, std::string_view text, const Color& color,
             const char* style = nullptr, TextAlign align = TextAlign::Left)
{
    if (cell.empty() || text.empty()) {
        return;
    }
    ui.drawList().pushClip(cell);
    ui.label({{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}}, text, color, style,
             align);
    ui.drawList().popClip();
}

// Alternating fills: the eye needs the banding to follow a row across five
// columns of numbers.
void rowBackground(UiContext& ui, const Rect& row, int index)
{
    if (index % 2 == 1) {
        ui.drawList().addRect(row, ui.theme().control.withAlpha(0.35f));
    }
}

void sectionHeader(UiContext& ui, const Rect& row, std::string_view text)
{
    ui.label(row, text, ui.theme().textDim, ui.theme().strongStyle);
    ui.drawList().addLine({row.min.x, row.max.y - 1.0f}, {row.max.x, row.max.y - 1.0f},
                          ui.theme().panelEdge, 1.0f);
}

void emptyNote(UiContext& ui, Column& column, std::string_view text)
{
    ui.label(column.row(kRowHeight), text, ui.theme().textDisabled);
}

// Width the scrollbar will claim, so a header row above a scroll region lines
// up with the rows inside it (beginScroll applies the same rule).
[[nodiscard]] float scrollbarInset(const UiContext& ui, const Rect& view, float contentHeight)
{
    return contentHeight > view.height() ? ui.theme().scrollbarWidth + ui.theme().spacing : 0.0f;
}

// --- Catalog lists (modules, weapons, ships, crew) ---

struct CatalogCells
{
    Rect name;
    Rect detail;
    Rect price;
    Rect primary;
    Rect secondary;
};

// Buttons are reserved per list rather than per row, so a row without a Sell
// button still lines its Buy button up with the ones above it.
[[nodiscard]] CatalogCells catalogCells(const UiContext& ui, const Rect& row, bool showPrice,
                                        bool reserveSecondary)
{
    Row cursor(row, ui.theme().spacing);
    CatalogCells cells;
    if (reserveSecondary) {
        cells.secondary = cursor.cellFromRight(kButtonWidth);
    }
    cells.primary = cursor.cellFromRight(kButtonWidth);
    if (showPrice) {
        cells.price = cursor.cellFromRight(kNumberWidth);
    }
    const Rect rest = cursor.remaining();
    Row split(rest, ui.theme().spacing);
    // Detail gets the larger share: it is what actually differs between two
    // modules of the same slot.
    cells.name = split.cell(rest.width() * 0.38f);
    cells.detail = split.remaining();
    return cells;
}

struct CatalogStyle
{
    const char* primary = "Buy";
    const char* secondary = nullptr; // drawn on rows that have one fitted
    const char* empty = "(nothing available)";
    bool showPrice = true;
    bool showCount = true;
};

struct CatalogClick
{
    int row = -1;
    bool secondary = false;
};

[[nodiscard]] CatalogClick catalogList(UiContext& ui, Column& column, std::string_view id,
                                       std::span<const OutfitRow> rows, const CatalogStyle& style)
{
    CatalogClick click;
    if (rows.empty()) {
        emptyNote(ui, column, style.empty);
        return click;
    }

    ui.pushId(id);
    char buffer[160] = {};
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const OutfitRow& item = rows[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        const CatalogCells cells =
            catalogCells(ui, row, style.showPrice, style.secondary != nullptr);
        ui.pushId(i);

        if (style.showCount && item.fitted > 0) {
            std::snprintf(buffer, sizeof(buffer), "%s (x%d)", item.name, item.fitted);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%s", item.name);
        }
        clipped(ui, cells.name, buffer, ui.theme().textPrimary, ui.theme().strongStyle);
        clipped(ui, cells.detail, item.detail, ui.theme().textDim);
        if (style.showPrice) {
            std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(item.price));
            clipped(ui, cells.price, buffer, ui.theme().textPrimary, ui.theme().bodyStyle,
                    TextAlign::Right);
        }
        if (ui.button(inset(cells.primary, 2.0f), style.primary)) {
            click = {.row = i, .secondary = false};
        }
        if (style.secondary != nullptr && item.fitted > 0 &&
            ui.button(inset(cells.secondary, 2.0f), style.secondary)) {
            click = {.row = i, .secondary = true};
        }

        ui.popId();
    }
    ui.popId();
    return click;
}

// --- Trade ---

struct TradeCells
{
    Rect name;
    Rect price;
    Rect stock;
    Rect held;
    Rect buy;
    Rect sell;
};

[[nodiscard]] TradeCells tradeCells(const UiContext& ui, const Rect& row)
{
    Row cursor(row, ui.theme().spacing);
    TradeCells cells;
    cells.sell = cursor.cellFromRight(kButtonWidth);
    cells.buy = cursor.cellFromRight(kButtonWidth);
    cells.held = cursor.cellFromRight(kNumberWidth);
    cells.stock = cursor.cellFromRight(kNumberWidth);
    cells.price = cursor.cellFromRight(kNumberWidth);
    cells.name = cursor.remaining();
    return cells;
}

void buildTradeTab(UiContext& ui, StationPanel& panel, StationScreenState& state,
                   const Rect& content)
{
    const auto& theme = ui.theme();
    Column outer(content, 0.0f, theme.spacing);

    // The amount selector and the column captions stay put while the list
    // scrolls under them.
    const Rect amountRow = outer.row(kRowHeight);
    {
        Row cursor(amountRow, theme.spacing);
        ui.pushId("amount");
        for (int i = kTradeAmountCount - 1; i >= 0; --i) {
            const Rect cell = cursor.cellFromRight(52.0f);
            if (ui.selectable(cell, kTradeAmountLabels[i], state.tradeAmount == i)) {
                state.tradeAmount = i;
            }
        }
        ui.popId();
        ui.label(cursor.remaining(), "Units per trade", theme.textDim, theme.bodyStyle,
                 TextAlign::Right);
    }

    const Rect captionRow = outer.row(24.0f);
    const Rect view = outer.remaining();
    const float contentHeight = listHeight(ui, panel.trade.rows.size());
    const float barInset = scrollbarInset(ui, view, contentHeight);
    const TradeCells captions =
        tradeCells(ui, {captionRow.min, {captionRow.max.x - barInset, captionRow.max.y}});
    clipped(ui, captions.name, "Commodity", theme.textDim, theme.smallStyle);
    clipped(ui, captions.price, "Price", theme.textDim, theme.smallStyle, TextAlign::Right);
    clipped(ui, captions.stock, "Stock", theme.textDim, theme.smallStyle, TextAlign::Right);
    clipped(ui, captions.held, "Held", theme.textDim, theme.smallStyle, TextAlign::Right);

    const Rect list = ui.beginScroll(view, contentHeight, state.scroll[StationScreenState::Trade]);
    Column column(list, 0.0f, theme.spacing);
    const float amount = kTradeAmounts[state.tradeAmount];
    const float cargoFree = panel.trade.cargoCapacity - panel.trade.cargoUsed;

    ui.pushId("goods");
    char buffer[64] = {};
    for (int i = 0; i < static_cast<int>(panel.trade.rows.size()); ++i) {
        const TradeRow& goods = panel.trade.rows[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        const TradeCells cells = tradeCells(ui, row);
        ui.pushId(i);

        clipped(ui, cells.name, goods.name, theme.textPrimary, theme.strongStyle);
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(goods.price));
        clipped(ui, cells.price, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);
        std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(goods.stock));
        clipped(ui, cells.stock, buffer, theme.textDim, theme.bodyStyle, TextAlign::Right);
        std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(goods.cargo));
        clipped(ui, cells.held, buffer, goods.cargo > 0.0f ? theme.accent : theme.textDim,
                theme.bodyStyle, TextAlign::Right);

        // The world clamps a trade to credits, hold space, and stock, so a
        // button is disabled only when it could do nothing at all.
        const bool canBuy = goods.stock > 0.0f && cargoFree >= 1.0f &&
                            panel.trade.credits >= static_cast<double>(goods.price);
        const bool canSell = goods.cargo > 0.0f;
        if (ui.button(inset(cells.buy, 2.0f), "Buy", canBuy)) {
            panel.trade.action = {.row = i, .units = amount, .isBuy = true};
        }
        if (ui.button(inset(cells.sell, 2.0f), "Sell", canSell)) {
            panel.trade.action = {.row = i, .units = amount, .isBuy = false};
        }

        ui.popId();
    }
    ui.popId();
    ui.endScroll();
}

// --- Outfitting ---

void buildOutfittingTab(UiContext& ui, StationPanel& panel, StationScreenState& state,
                        const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = kRowHeight * 2.0f + theme.spacing * 2.0f +
                                (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, std::max<std::size_t>(panel.modules.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.weapons.size(), 1));
    const Rect list =
        ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Outfitting]);
    Column column(list, 0.0f, theme.spacing);

    clipped(ui, column.row(kRowHeight), panel.fitSummary, theme.textPrimary, theme.bodyStyle);
    char buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "Insurance deductible at current fit: %.0f cr",
                  panel.deductible);
    clipped(ui, column.row(kRowHeight), buffer, theme.textDim, theme.smallStyle);

    sectionHeader(ui, column.row(kSectionHeight), "Modules (Sell removes one fitted instance at resale)");
    const CatalogClick module = catalogList(ui, column, "modules", panel.modules,
                                            {.primary = "Buy", .secondary = "Sell"});
    if (module.row >= 0) {
        panel.action = {module.secondary ? StationAction::Kind::SellModule
                                         : StationAction::Kind::BuyModule,
                        panel.modules[static_cast<std::size_t>(module.row)].id, module.row};
    }

    sectionHeader(ui, column.row(kSectionHeight), "Weapon mount (swapping sells the old weapon at resale)");
    const CatalogClick weapon =
        catalogList(ui, column, "weapons", panel.weapons, {.primary = "Mount"});
    if (weapon.row >= 0) {
        panel.action = {StationAction::Kind::BuyWeapon,
                        panel.weapons[static_cast<std::size_t>(weapon.row)].id, weapon.row};
    }

    ui.endScroll();
}

// --- Shipyard ---

void buildShipyardTab(UiContext& ui, StationPanel& panel, StationScreenState& state,
                      const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, std::max<std::size_t>(panel.fleet.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.shipCatalog.size(), 1));
    const Rect list =
        ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Shipyard]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), "Your fleet");
    if (panel.fleet.empty()) {
        emptyNote(ui, column, "(no ships)");
    }
    ui.pushId("fleet");
    char buffer[96] = {};
    for (int i = 0; i < static_cast<int>(panel.fleet.size()); ++i) {
        const FleetRow& ship = panel.fleet[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        const CatalogCells cells = catalogCells(ui, row, true, true);
        ui.pushId(i);

        clipped(ui, cells.name, ship.name, theme.textPrimary, theme.strongStyle);
        clipped(ui, cells.detail,
                ship.active ? "active" : (ship.storedHere ? "stored here" : "stored elsewhere"),
                ship.active ? theme.accent : theme.textDim);
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(ship.value));
        clipped(ui, cells.price, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);

        // Only a ship parked at this station can be switched to or sold; the
        // rest are listed so the fleet is still legible from anywhere.
        const bool here = !ship.active && ship.storedHere;
        if (ui.button(inset(cells.primary, 2.0f), "Switch", here)) {
            panel.action = {.kind = StationAction::Kind::SwitchShip, .index = i};
        }
        if (ui.button(inset(cells.secondary, 2.0f), "Sell", here)) {
            panel.action = {.kind = StationAction::Kind::SellShip, .index = i};
        }

        ui.popId();
    }
    ui.popId();

    sectionHeader(ui, column.row(kSectionHeight), "For sale (a new ship is stored here until you switch)");
    const CatalogClick ship =
        catalogList(ui, column, "ships", panel.shipCatalog, {.primary = "Buy", .showCount = false});
    if (ship.row >= 0) {
        panel.action = {StationAction::Kind::BuyShip,
                        panel.shipCatalog[static_cast<std::size_t>(ship.row)].id, ship.row};
    }

    ui.endScroll();
}

// --- Crew ---

void buildCrewTab(UiContext& ui, StationPanel& panel, StationScreenState& state,
                  const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, std::max<std::size_t>(panel.crewAboard.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.crewCatalog.size(), 1));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Crew]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), "Aboard");
    const CatalogClick aboard = catalogList(ui, column, "aboard", panel.crewAboard,
                                            {.primary = "Dismiss",
                                             .secondary = nullptr,
                                             .empty = "(no crew aboard)",
                                             .showPrice = false,
                                             .showCount = false});
    if (aboard.row >= 0) {
        panel.action = {StationAction::Kind::FireCrew,
                        panel.crewAboard[static_cast<std::size_t>(aboard.row)].id, aboard.row};
    }

    sectionHeader(ui, column.row(kSectionHeight), "For hire (one-time fee, no refund)");
    const CatalogClick hire =
        catalogList(ui, column, "hire", panel.crewCatalog, {.primary = "Hire"});
    if (hire.row >= 0) {
        panel.action = {StationAction::Kind::HireCrew,
                        panel.crewCatalog[static_cast<std::size_t>(hire.row)].id, hire.row};
    }

    ui.endScroll();
}

// --- Factions ---

// Counts the lines in the prebuilt raid note, which is newline-separated.
[[nodiscard]] std::size_t noteLineCount(const char* notes)
{
    if (notes == nullptr || notes[0] == '\0') {
        return 1; // the "(quiet lately)" placeholder
    }
    std::size_t lines = 1;
    for (const char* c = notes; *c != '\0'; ++c) {
        lines += *c == '\n' ? 1 : 0;
    }
    return lines;
}

void buildFactionsTab(UiContext& ui, StationPanel& panel, StationScreenState& state,
                      const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, panel.factions.size()) +
                                listHeight(ui, noteLineCount(panel.factionNotes));
    const Rect list =
        ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Factions]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), "Standing");
    if (panel.factions.empty()) {
        emptyNote(ui, column, "(no factions)");
    }
    char buffer[64] = {};
    for (int i = 0; i < static_cast<int>(panel.factions.size()); ++i) {
        const FactionRow& faction = panel.factions[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);

        Row cursor(row, theme.spacing);
        const Rect detailCell = cursor.cellFromRight(row.width() * 0.34f);
        const Rect valueCell = cursor.cellFromRight(56.0f);
        const Rect meterCell = cursor.cellFromRight(160.0f);
        const Rect nameCell = cursor.remaining();

        const Color color = faction.standing < -30.0f ? theme.negative
                            : faction.standing > 30.0f ? theme.positive
                                                       : theme.textDim;
        clipped(ui, nameCell, faction.name, theme.textPrimary, theme.strongStyle);
        // -100..100 mapped onto the bar, so the midpoint is "no history".
        ui.meter({{meterCell.min.x, row.min.y + (row.height() - 10.0f) * 0.5f},
                  {meterCell.max.x, row.min.y + (row.height() + 10.0f) * 0.5f}},
                 (faction.standing + 100.0f) / 200.0f, color);
        std::snprintf(buffer, sizeof(buffer), "%+.0f", static_cast<double>(faction.standing));
        clipped(ui, valueCell, buffer, color, theme.bodyStyle, TextAlign::Right);
        std::snprintf(buffer, sizeof(buffer), "%s, %s", faction.attitude, faction.detail);
        clipped(ui, detailCell, buffer, theme.textDim, theme.smallStyle);
    }

    sectionHeader(ui, column.row(kSectionHeight), "Recent raids");
    if (panel.factionNotes == nullptr || panel.factionNotes[0] == '\0') {
        emptyNote(ui, column, "(quiet lately)");
    } else {
        std::string_view notes(panel.factionNotes);
        while (!notes.empty()) {
            const std::size_t breakAt = notes.find('\n');
            clipped(ui, column.row(kRowHeight), notes.substr(0, breakAt), theme.textDim,
                    theme.bodyStyle);
            if (breakAt == std::string_view::npos) {
                break;
            }
            notes.remove_prefix(breakAt + 1);
        }
    }

    ui.endScroll();
}

// --- Missions ---

struct MissionCells
{
    Rect title;
    Rect detail;
    Rect reward;
    Rect primary;
    Rect secondary;
};

[[nodiscard]] MissionCells missionCells(const UiContext& ui, const Rect& row, bool showReward)
{
    Row cursor(row, ui.theme().spacing);
    MissionCells cells;
    cells.secondary = cursor.cellFromRight(kButtonWidth);
    cells.primary = cursor.cellFromRight(kButtonWidth);
    if (showReward) {
        cells.reward = cursor.cellFromRight(kNumberWidth);
    }
    const Rect rest = cursor.remaining();
    Row split(rest, ui.theme().spacing);
    cells.title = split.cell(rest.width() * 0.34f);
    cells.detail = split.remaining();
    return cells;
}

void buildMissionsTab(UiContext& ui, StationPanel& panel, StationScreenState& state,
                      const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight =
        (kSectionHeight + theme.spacing) * 2.0f +
        listHeight(ui, std::max<std::size_t>(panel.missionOffers.size(), 1)) +
        listHeight(ui, std::max<std::size_t>(panel.missionJournal.size(), 1));
    const Rect list =
        ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Missions]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), "Board");
    if (panel.missionOffers.empty()) {
        emptyNote(ui, column, "(no offers)");
    }
    ui.pushId("offers");
    char buffer[64] = {};
    for (int i = 0; i < static_cast<int>(panel.missionOffers.size()); ++i) {
        const MissionRow& offer = panel.missionOffers[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        const MissionCells cells = missionCells(ui, row, true);
        ui.pushId(i);

        clipped(ui, cells.title, offer.title, offer.campaign ? kCampaign : theme.textPrimary,
                theme.strongStyle);
        clipped(ui, cells.detail, offer.detail, theme.textDim);
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(offer.reward));
        clipped(ui, cells.reward, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);
        // A tier the player's standing does not clear stays visible but dead,
        // so the reason to build reputation is on the board rather than hidden.
        if (ui.button(inset(cells.primary, 2.0f), "Accept", offer.acceptable)) {
            panel.action = {.kind = StationAction::Kind::AcceptMission, .index = i};
        }

        ui.popId();
    }
    ui.popId();

    sectionHeader(ui, column.row(kSectionHeight), "Journal");
    if (panel.missionJournal.empty()) {
        emptyNote(ui, column, "(no active missions)");
    }
    ui.pushId("journal");
    for (int i = 0; i < static_cast<int>(panel.missionJournal.size()); ++i) {
        const MissionRow& mission = panel.missionJournal[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        const MissionCells cells = missionCells(ui, row, false);
        ui.pushId(i);

        std::snprintf(buffer, sizeof(buffer), "%s%s", mission.tracked ? "* " : "", mission.title);
        clipped(ui, cells.title, buffer, mission.campaign ? kCampaign : theme.textPrimary,
                theme.strongStyle);
        clipped(ui, cells.detail, mission.detail, theme.textDim);
        if (ui.button(inset(cells.primary, 2.0f), "Track", !mission.tracked)) {
            panel.action = {.kind = StationAction::Kind::TrackMission, .index = i};
        }
        if (ui.button(inset(cells.secondary, 2.0f), "Abandon")) {
            panel.action = {.kind = StationAction::Kind::AbandonMission, .index = i};
        }

        ui.popId();
    }
    ui.popId();

    ui.endScroll();
}

// --- Survey (Phase 8e) ---

// Survey data is an intangible: it costs no hold space and sells whole, so the
// tab is a ledger with one button rather than a per-line market.
void buildSurveyTab(UiContext& ui, StationPanel& panel, StationScreenState& state,
                    const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = kSectionHeight + theme.spacing +
                                listHeight(ui, std::max<std::size_t>(panel.surveyData.size(), 1)) +
                                kRowHeight + theme.spacing;
    const Rect list =
        ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Survey]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), "Unsold survey data");
    if (panel.surveyData.empty()) {
        emptyNote(ui, column, "(nothing scanned since your last sale)");
    }
    char buffer[64] = {};
    for (int i = 0; i < static_cast<int>(panel.surveyData.size()); ++i) {
        const sol::ui::SurveyRow& entry = panel.surveyData[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        Row cursor(row, theme.spacing);
        const Rect valueCell = cursor.cellFromRight(kNumberWidth);
        const Rect rest = cursor.remaining();
        Row split(rest, theme.spacing);
        const Rect systemCell = split.cell(rest.width() * 0.34f);
        clipped(ui, systemCell, entry.system, theme.textPrimary, theme.strongStyle);
        clipped(ui, split.remaining(), entry.detail, theme.textDim);
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(entry.value));
        clipped(ui, valueCell, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);
    }

    const Rect totalRow = column.row(kRowHeight);
    Row totals(totalRow, theme.spacing);
    const Rect sellCell = totals.cellFromRight(kButtonWidth + 40.0f);
    std::snprintf(buffer, sizeof(buffer), "Total %.0f cr", panel.surveyValue);
    clipped(ui, totals.remaining(), buffer, theme.accent, theme.strongStyle, TextAlign::Right);
    if (ui.button(inset(sellCell, 2.0f), "Sell All", !panel.surveyData.empty())) {
        panel.action = {.kind = StationAction::Kind::SellSurveyData, .index = -1};
    }

    ui.endScroll();
}

} // namespace

bool buildStationScreen(UiContext& ui, StationPanel& panel, StationScreenState& state)
{
    const auto& theme = ui.theme();
    // The screen owns the view while it is up: everything behind it is dimmed
    // rather than competing for the eye.
    ui.drawList().addRect({{0.0f, 0.0f}, ui.screenSize()}, theme.background);
    ui.pushId("station");

    const float width = std::min(ui.screenSize().x - 80.0f, 1180.0f);
    const float height = std::min(ui.screenSize().y - 60.0f, 820.0f);
    const Rect frame = {{(ui.screenSize().x - width) * 0.5f, (ui.screenSize().y - height) * 0.5f},
                        {(ui.screenSize().x + width) * 0.5f, (ui.screenSize().y + height) * 0.5f}};
    ui.panel(frame);

    Column column(frame, theme.padding, theme.spacing);

    // Header: where you are on the left, what you are spending on the right.
    const Rect headerRow = column.row(kHeaderHeight);
    {
        Row cursor(headerRow, theme.spacing * 3.0f);
        const Rect cargoCell = cursor.cellFromRight(190.0f);
        const Rect creditCell = cursor.cellFromRight(190.0f);
        clipped(ui, cursor.remaining(), panel.trade.stationName, theme.textPrimary,
                theme.headingStyle);
        char buffer[96] = {};
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", panel.trade.credits);
        clipped(ui, creditCell, buffer, theme.accent, theme.strongStyle, TextAlign::Right);
        std::snprintf(buffer, sizeof(buffer), "Cargo %.0f / %.0f",
                      static_cast<double>(panel.trade.cargoUsed),
                      static_cast<double>(panel.trade.cargoCapacity));
        clipped(ui, cargoCell, buffer, theme.textDim, theme.bodyStyle, TextAlign::Right);
    }

    (void)ui.tabs(column.row(kTabHeight), kTabLabels, state.tab);

    // Footer first: the content region is whatever the tabs and the footer
    // leave, and it has to be known before the tab draws into it.
    const Rect body = column.remaining();
    const Rect footerRow = {{body.min.x, body.max.y - kFooterHeight}, {body.max.x, body.max.y}};
    const Rect content = {body.min, {body.max.x, footerRow.min.y - theme.spacing}};

    switch (state.tab) {
    case StationScreenState::Trade:
        buildTradeTab(ui, panel, state, content);
        break;
    case StationScreenState::Outfitting:
        buildOutfittingTab(ui, panel, state, content);
        break;
    case StationScreenState::Shipyard:
        buildShipyardTab(ui, panel, state, content);
        break;
    case StationScreenState::Crew:
        buildCrewTab(ui, panel, state, content);
        break;
    case StationScreenState::Factions:
        buildFactionsTab(ui, panel, state, content);
        break;
    case StationScreenState::Missions:
        buildMissionsTab(ui, panel, state, content);
        break;
    case StationScreenState::Survey:
        buildSurveyTab(ui, panel, state, content);
        break;
    default:
        break;
    }

    // One line of context per tab, where a player would look for it.
    static constexpr const char* const kHints[StationScreenState::TabCount] = {
        "Prices move with stock; NPC traders share this market.",
        "Fitting draws power and fills slots; selling refunds at resale value.",
        "A ship you buy is stored here until you switch to it.",
        "Crew bonuses apply to the ship they are aboard.",
        "Standing moves with what you do in a faction's space.",
        "Accepting a contract starts its clock; abandoning one costs standing.",
        "Scan data sells anywhere; the further out it was taken, the more it pays.",
    };

    Row footer(footerRow, theme.spacing);
    const Rect undockCell = footer.cellFromRight(150.0f);
    ui.label(footer.remaining(), kHints[state.tab], theme.textDisabled, theme.smallStyle);
    const bool undock = ui.button(undockCell, "Undock");

    ui.popId();
    return undock;
}

} // namespace game
