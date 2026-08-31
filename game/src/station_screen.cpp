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
using sol::ui::inset;
using sol::ui::MissionRow;
using sol::ui::MountRow;
using sol::ui::OutfitRow;
using sol::ui::Rect;
using sol::ui::rgba;
using sol::ui::Row;
using sol::ui::StationAction;
using sol::ui::StationPanel;
using sol::ui::TextAlign;
using sol::ui::TradeLegality;
using sol::ui::TradeRow;
using sol::ui::UiContext;

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
    "Trade", "Outfitting", "Shipyard", "Crew", "Factions", "Missions", "Survey", "Refinery"};

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
void clipped(UiContext& ui,
             const Rect& cell,
             std::string_view text,
             const Color& color,
             const char* style = nullptr,
             TextAlign align = TextAlign::Left)
{
    if (cell.empty() || text.empty()) {
        return;
    }
    ui.drawList().pushClip(cell);
    ui.label({{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}}, text, color, style, align);
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
    ui.drawList().addLine(
        {row.min.x, row.max.y - 1.0f}, {row.max.x, row.max.y - 1.0f}, ui.theme().panelEdge, 1.0f);
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

// --- Catalog lists (components, weapons, ships, crew) ---

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
[[nodiscard]] CatalogCells
catalogCells(const UiContext& ui, const Rect& row, bool showPrice, bool reserveSecondary)
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
    // components of the same slot.
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
    // Phase 31 stage B: grey the primary button on a row with no `targetMount`.
    // A station action carries no way to report a refusal back to the player,
    // so a button that would be refused has to be a button that is not offered.
    bool requireTargetMount = false;
};

struct CatalogClick
{
    int row = -1;
    bool secondary = false;
};

[[nodiscard]] CatalogClick catalogList(UiContext& ui,
                                       Column& column,
                                       std::string_view id,
                                       std::span<const OutfitRow> rows,
                                       const CatalogStyle& style)
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
        const CatalogCells cells = catalogCells(ui, row, style.showPrice, style.secondary != nullptr);
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
            clipped(ui, cells.price, buffer, ui.theme().textPrimary, ui.theme().bodyStyle, TextAlign::Right);
        }
        const bool enabled = !style.requireTargetMount || item.targetMount[0] != '\0';
        if (ui.button(inset(cells.primary, 2.0f), style.primary, enabled)) {
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

// --- Mounts (Phase 31 stage B) ---

// One row per place on the hull. Deliberately the SAME five-column shape the
// catalogs use - name, detail, number, button - because a player reading down
// this tab should not have to re-learn where the button is halfway through it.
struct MountClick
{
    int row = -1;
};

[[nodiscard]] MountClick
mountList(UiContext& ui, Column& column, std::span<const MountRow> rows, std::string_view selected)
{
    MountClick click;
    if (rows.empty()) {
        // A hull with no mounts fits nothing, and that is a legal def rather
        // than an error - so it says so, instead of showing an empty box.
        emptyNote(ui, column, "this hull has no mounts: nothing can be fitted to it");
        return click;
    }

    ui.pushId("mounts");
    char buffer[160] = {};
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const MountRow& mount = rows[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        const bool filled = mount.fitted[0] != '\0';
        const CatalogCells cells = catalogCells(ui, row, /*showPrice=*/filled, /*reserveSecondary=*/false);
        ui.pushId(i);

        // The mount's own identity leads and what is in it follows, because
        // the mount is the thing that does not change: an author's `id`, its
        // kind and its size are what a save, a def file and the Forge all
        // agree on, and the fitting is what the player is about to swap.
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s%s  %s %s",
                      mount.id == selected ? "> " : "  ",
                      mount.id,
                      mount.size,
                      mount.kind);
        clipped(ui, cells.name, buffer, ui.theme().textPrimary, ui.theme().strongStyle);
        if (filled) {
            std::snprintf(buffer, sizeof(buffer), "%s - %s", mount.fitted, mount.detail);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%s", mount.detail);
        }
        clipped(ui, cells.detail, buffer, filled ? ui.theme().textDim : ui.theme().textDisabled);
        if (filled) {
            std::snprintf(buffer, sizeof(buffer), "+%.0f cr", static_cast<double>(mount.resale));
            clipped(ui, cells.price, buffer, ui.theme().textDim, ui.theme().bodyStyle, TextAlign::Right);
        }
        if (ui.button(inset(cells.primary, 2.0f), filled ? "Remove" : "Select")) {
            click = {.row = i};
        }

        ui.popId();
    }
    ui.popId();
    return click;
}

// --- Trade ---

// Width of the "best price seen elsewhere" column (Phase 8g). It carries a
// price, a system name, and an age, so it needs more room than a number.
constexpr float kElsewhereWidth = 190.0f;
// Wide enough for CONTRABAND in the small style with room to spare, taken
// out of the commodity-name cell and only on the rows that carry a tag
// (Phase 33 stage D). An unmarked row is laid out exactly as it was.
constexpr float kLegalityWidth = 96.0f;

struct TradeCells
{
    Rect name;
    Rect price;
    Rect elsewhere;
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
    cells.elsewhere = cursor.cellFromRight(kElsewhereWidth);
    cells.price = cursor.cellFromRight(kNumberWidth);
    cells.name = cursor.remaining();
    return cells;
}

void buildTradeTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
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
        const Rect rest = cursor.remaining();
        ui.label(rest, "Units per trade", theme.textDim, theme.bodyStyle, TextAlign::Right);
        // ⚑⚑ WHOSE LAW YOU ARE STANDING IN (Phase 33 stage D), on the left of a
        // row whose own label is right-aligned, so it costs no height. It is
        // the ONLY thing on this screen that separates "the holder has no
        // opinion about any of this" from "nobody holds this place at all" -
        // both of which leave every row below unmarked - and gdd.md §13 turns
        // on that difference.
        char lawBuffer[96] = {};
        if (panel.trade.jurisdiction[0] != '\0') {
            std::snprintf(lawBuffer, sizeof(lawBuffer), "Under %s law", panel.trade.jurisdiction);
        } else {
            std::snprintf(lawBuffer, sizeof(lawBuffer), "No jurisdiction - nobody polices this system");
        }
        clipped(ui, rest, lawBuffer, theme.textDim, theme.smallStyle);
    }

    // The market report (Phase 8g). 8e sells the player's survey data to the
    // station; this is the same trade the other way round.
    const Rect intelRow = outer.row(kRowHeight);
    {
        Row cursor(intelRow, theme.spacing);
        char buffer[96] = {};
        const Rect button = cursor.cellFromRight(kButtonWidth * 1.6f);
        if (ui.button(inset(button, 2.0f), "Buy data", panel.trade.canBuyIntel)) {
            panel.action = {.kind = StationAction::Kind::BuyMarketIntel};
        }
        if (panel.trade.intelMarkets > 0) {
            std::snprintf(buffer,
                          sizeof(buffer),
                          "Market report: %u markets nearby, %.0f cr",
                          panel.trade.intelMarkets,
                          panel.trade.intelPrice);
        } else {
            std::snprintf(buffer, sizeof(buffer), "No markets in reach to report on");
        }
        ui.label(cursor.remaining(), buffer, theme.textDim, theme.bodyStyle, TextAlign::Right);
    }

    const Rect captionRow = outer.row(24.0f);
    const Rect view = outer.remaining();
    const float contentHeight = listHeight(ui, panel.trade.rows.size());
    const float barInset = scrollbarInset(ui, view, contentHeight);
    const TradeCells captions =
        tradeCells(ui, {captionRow.min, {captionRow.max.x - barInset, captionRow.max.y}});
    clipped(ui, captions.name, "Commodity", theme.textDim, theme.smallStyle);
    clipped(ui, captions.price, "Price", theme.textDim, theme.smallStyle, TextAlign::Right);
    clipped(ui, captions.elsewhere, "Best elsewhere", theme.textDim, theme.smallStyle, TextAlign::Right);
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

        // ⚑⚑ THE TAG TAKES ITS SPACE OUT OF THE NAME CELL AND IS DRAWN FIRST,
        // so a long commodity name elides and the word CONTRABAND never does.
        // That ordering is the whole reason this is not one formatted string:
        // the tag is the only part of the row a player can be arrested over.
        //
        // ⚑ No new theme colour. `negative` for forbidden and `accent` for
        // licensed are the two the theme already has, and they happen to say
        // the right things - danger and information - while the WORD carries
        // the meaning either way. A third severity colour would be a change to
        // every screen in the game for one column.
        Rect nameCell = cells.name;
        if (goods.legality != TradeLegality::Legal) {
            const bool forbidden = goods.legality == TradeLegality::Contraband;
            Row nameCursor(cells.name, theme.spacing);
            const Rect tag = nameCursor.cellFromRight(kLegalityWidth);
            nameCell = nameCursor.remaining();
            clipped(ui,
                    tag,
                    forbidden ? "CONTRABAND" : "RESTRICTED",
                    forbidden ? theme.negative : theme.accent,
                    theme.smallStyle,
                    TextAlign::Right);
        }
        clipped(ui, nameCell, goods.name, theme.textPrimary, theme.strongStyle);
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(goods.price));
        clipped(ui, cells.price, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);

        // Best price seen elsewhere. Green when it beats the local price by
        // enough to be worth the trip, dim when the reading has gone stale —
        // the market has moved since, and pretending otherwise would make
        // bought intel a one-time unlock instead of something worth
        // refreshing.
        if (goods.hasElsewhere) {
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%.2f  %s  %s",
                          static_cast<double>(goods.elsewherePrice),
                          goods.elsewhereName,
                          goods.elsewhereAge);
            const bool worthIt = goods.elsewherePrice > goods.price * 1.15f;
            const auto color = goods.elsewhereStale ? theme.textDim
                               : worthIt            ? theme.positive
                                                    : theme.textPrimary;
            clipped(ui, cells.elsewhere, buffer, color, theme.smallStyle, TextAlign::Right);
        } else {
            clipped(ui, cells.elsewhere, "-", theme.textDim, theme.smallStyle, TextAlign::Right);
        }

        std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(goods.stock));
        clipped(ui, cells.stock, buffer, theme.textDim, theme.bodyStyle, TextAlign::Right);
        std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(goods.cargo));
        clipped(ui,
                cells.held,
                buffer,
                goods.cargo > 0.0f ? theme.accent : theme.textDim,
                theme.bodyStyle,
                TextAlign::Right);

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

void buildOutfittingTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = kRowHeight * 3.0f + theme.spacing * 3.0f +
                                (kSectionHeight + theme.spacing) * 3.0f +
                                listHeight(ui, std::max<std::size_t>(panel.mounts.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.components.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.weapons.size(), 1));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Outfitting]);
    Column column(list, 0.0f, theme.spacing);

    clipped(ui, column.row(kRowHeight), panel.fitSummary, theme.textPrimary, theme.bodyStyle);
    char buffer[160] = {};
    std::snprintf(buffer, sizeof(buffer), "Insurance deductible at current fit: %.0f cr", panel.deductible);
    clipped(ui, column.row(kRowHeight), buffer, theme.textDim, theme.smallStyle);

    // ⚑ THE MOUNT LIST IS THE SCREEN NOW, and the catalogs are what fills it.
    // A ship is its mounts (decisions/014), so the first thing this tab shows
    // is the ship rather than the shop.
    sectionHeader(ui, column.row(kSectionHeight), "Mounts (Select narrows the catalogs below to one place)");
    const MountClick mount = mountList(ui, column, panel.mounts, state.selectedMount);
    if (mount.row >= 0) {
        const MountRow& row = panel.mounts[static_cast<std::size_t>(mount.row)];
        if (row.fitted[0] != '\0') {
            panel.action = {.kind = StationAction::Kind::SellFitting, .mount = row.id, .index = mount.row};
        } else {
            // Selecting the row already selected clears it, which is the only
            // way back to "put it wherever it goes" without leaving the tab.
            state.selectedMount = state.selectedMount == row.id ? std::string() : row.id;
        }
    }
    if (!state.selectedMount.empty()) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "Showing only what '%s' takes. Select it again for every mount.",
                      state.selectedMount.c_str());
    } else {
        std::snprintf(buffer, sizeof(buffer), "Fitting goes to the first free mount that takes it.");
    }
    clipped(ui, column.row(kRowHeight), buffer, theme.textDim, theme.smallStyle);

    // ⚑ A GREYED FIT IS THE REFUSAL, SAID BEFORE THE CLICK. `targetMount` is
    // the game's own answer to "where would this go", so a row with none has
    // no place on this hull - or none the selected mount allows. This matters
    // more than it did before mounts: a station action has no channel to
    // report a refusal back, so under the old slot counts a refused Buy simply
    // did nothing, and a mis-aimed Fit is far easier to attempt.
    sectionHeader(ui, column.row(kSectionHeight), "Components");
    const CatalogClick component = catalogList(
        ui, column, "components", panel.components, {.primary = "Fit", .requireTargetMount = true});
    if (component.row >= 0) {
        const OutfitRow& row = panel.components[static_cast<std::size_t>(component.row)];
        panel.action = {.kind = StationAction::Kind::BuyFitting,
                        .id = row.id,
                        .mount = row.targetMount,
                        .index = component.row};
    }

    sectionHeader(ui, column.row(kSectionHeight), "Weapons (a swap sells the old one back at resale)");
    const CatalogClick weapon =
        catalogList(ui, column, "weapons", panel.weapons, {.primary = "Fit", .requireTargetMount = true});
    if (weapon.row >= 0) {
        const OutfitRow& row = panel.weapons[static_cast<std::size_t>(weapon.row)];
        panel.action = {.kind = StationAction::Kind::BuyFitting,
                        .id = row.id,
                        .mount = row.targetMount,
                        .index = weapon.row};
    }

    ui.endScroll();
}

// --- Shipyard ---

void buildShipyardTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, std::max<std::size_t>(panel.fleet.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.shipCatalog.size(), 1));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Shipyard]);
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
        clipped(ui,
                cells.detail,
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
        panel.action = {.kind = StationAction::Kind::BuyShip,
                        .id = panel.shipCatalog[static_cast<std::size_t>(ship.row)].id,
                        .index = ship.row};
    }

    ui.endScroll();
}

// --- Crew ---

void buildCrewTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, std::max<std::size_t>(panel.crewAboard.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.crewCatalog.size(), 1));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Crew]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), "Aboard");
    const CatalogClick aboard = catalogList(ui,
                                            column,
                                            "aboard",
                                            panel.crewAboard,
                                            {.primary = "Dismiss",
                                             .secondary = nullptr,
                                             .empty = "(no crew aboard)",
                                             .showPrice = false,
                                             .showCount = false});
    if (aboard.row >= 0) {
        panel.action = {.kind = StationAction::Kind::FireCrew,
                        .id = panel.crewAboard[static_cast<std::size_t>(aboard.row)].id,
                        .index = aboard.row};
    }

    sectionHeader(ui, column.row(kSectionHeight), "For hire (one-time fee, no refund)");
    const CatalogClick hire = catalogList(ui, column, "hire", panel.crewCatalog, {.primary = "Hire"});
    if (hire.row >= 0) {
        panel.action = {.kind = StationAction::Kind::HireCrew,
                        .id = panel.crewCatalog[static_cast<std::size_t>(hire.row)].id,
                        .index = hire.row};
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

void buildFactionsTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, panel.factions.size()) +
                                listHeight(ui, noteLineCount(panel.factionNotes));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Factions]);
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

        const Color color = faction.standing < -30.0f  ? theme.negative
                            : faction.standing > 30.0f ? theme.positive
                                                       : theme.textDim;
        clipped(ui, nameCell, faction.name, theme.textPrimary, theme.strongStyle);
        // -100..100 mapped onto the bar, so the midpoint is "no history".
        ui.meter({{meterCell.min.x, row.min.y + (row.height() - 10.0f) * 0.5f},
                  {meterCell.max.x, row.min.y + (row.height() + 10.0f) * 0.5f}},
                 (faction.standing + 100.0f) / 200.0f,
                 color);
        std::snprintf(buffer, sizeof(buffer), "%+.0f", static_cast<double>(faction.standing));
        clipped(ui, valueCell, buffer, color, theme.bodyStyle, TextAlign::Right);
        std::snprintf(buffer, sizeof(buffer), "%s, %s", faction.attitude, faction.detail);
        clipped(ui, detailCell, buffer, theme.textDim, theme.smallStyle);
    }

    // Since Phase 8u this block leads with contested systems and borders that
    // have moved, and only then lists raids - so the header names all three.
    sectionHeader(ui, column.row(kSectionHeight), "War and raids");
    if (panel.factionNotes == nullptr || panel.factionNotes[0] == '\0') {
        emptyNote(ui, column, "(quiet lately)");
    } else {
        std::string_view notes(panel.factionNotes);
        while (!notes.empty()) {
            const std::size_t breakAt = notes.find('\n');
            clipped(ui, column.row(kRowHeight), notes.substr(0, breakAt), theme.textDim, theme.bodyStyle);
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

void buildMissionsTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = (kSectionHeight + theme.spacing) * 2.0f +
                                listHeight(ui, std::max<std::size_t>(panel.missionOffers.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.missionJournal.size(), 1));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Missions]);
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

        clipped(
            ui, cells.title, offer.title, offer.campaign ? kCampaign : theme.textPrimary, theme.strongStyle);
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
        clipped(ui, cells.title, buffer, mission.campaign ? kCampaign : theme.textPrimary, theme.strongStyle);
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
void buildSurveyTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const float contentHeight = kSectionHeight + theme.spacing +
                                listHeight(ui, std::max<std::size_t>(panel.surveyData.size(), 1)) +
                                kRowHeight + theme.spacing;
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Survey]);
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

// --- Refinery (Phase 8f) ---

// Refining is a service, not a market: you hand over ore and come back for
// metal. The tab is therefore two facts (what it takes, what it pays) and two
// buttons — order, and collect what a previous order finished.
void buildRefineryTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const sol::ui::RefinePanel& refinery = panel.refinery;
    const float contentHeight = kSectionHeight + theme.spacing + kRowHeight * 5.0f + theme.spacing * 5.0f;
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Refinery]);
    Column column(list, 0.0f, theme.spacing);

    if (!refinery.refines) {
        sectionHeader(ui, column.row(kSectionHeight), "Refining");
        emptyNote(ui, column, "(this station refines nothing)");
        ui.endScroll();
        return;
    }

    char buffer[160] = {};
    std::snprintf(buffer, sizeof(buffer), "%s to %s", refinery.inputName, refinery.outputName);
    sectionHeader(ui, column.row(kSectionHeight), buffer);

    const auto factRow = [&](const char* label, const char* value) {
        const Rect row = column.row(kRowHeight);
        Row cursor(row, theme.spacing);
        const Rect valueCell = cursor.cellFromRight(row.width() * 0.5f);
        clipped(ui, cursor.remaining(), label, theme.textDim);
        clipped(ui, valueCell, value, theme.textPrimary, theme.bodyStyle, TextAlign::Right);
    };

    std::snprintf(buffer,
                  sizeof(buffer),
                  "%.2f %s per unit, %.0f cr fee",
                  static_cast<double>(refinery.ratio),
                  refinery.outputName,
                  static_cast<double>(refinery.feePerUnit));
    factRow("Rate", buffer);
    std::snprintf(buffer, sizeof(buffer), "%.0f units", static_cast<double>(refinery.inputHeld));
    factRow("Aboard", buffer);
    if (refinery.waitSeconds >= 0.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f s", refinery.waitSeconds);
        factRow("Next order ready in", buffer);
    } else {
        factRow("Next order ready in", "-");
    }

    // Orders move in the same fixed steps trading uses; the world clamps to
    // what is actually in the hold.
    const Rect orderRow = column.row(kRowHeight);
    Row order(orderRow, theme.spacing);
    clipped(ui, order.cell(orderRow.width() * 0.4f), "Refine", theme.textPrimary, theme.strongStyle);
    for (int i = 0; i < kTradeAmountCount; ++i) {
        const Rect cell = order.cell(kButtonWidth);
        ui.pushId(i);
        if (ui.button(inset(cell, 2.0f), kTradeAmountLabels[i], refinery.inputHeld > 0.0f)) {
            panel.action = {.kind = StationAction::Kind::OrderRefine, .index = -1, .units = kTradeAmounts[i]};
        }
        ui.popId();
    }

    const Rect collectRow = column.row(kRowHeight);
    Row collect(collectRow, theme.spacing);
    const Rect collectCell = collect.cellFromRight(kButtonWidth + 40.0f);
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%.0f %s waiting",
                  static_cast<double>(refinery.readyUnits),
                  refinery.outputName);
    clipped(ui, collect.remaining(), buffer, theme.accent, theme.strongStyle, TextAlign::Right);
    if (ui.button(
            inset(collectCell, 2.0f), "Collect", refinery.readyUnits > 0.0f && refinery.cargoSpace > 0.0f)) {
        panel.action = {.kind = StationAction::Kind::CollectRefined, .index = -1};
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
        clipped(ui, cursor.remaining(), panel.trade.stationName, theme.textPrimary, theme.headingStyle);
        char buffer[96] = {};
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", panel.trade.credits);
        clipped(ui, creditCell, buffer, theme.accent, theme.strongStyle, TextAlign::Right);
        std::snprintf(buffer,
                      sizeof(buffer),
                      "Cargo %.0f / %.0f",
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
    case StationScreenState::Refinery:
        buildRefineryTab(ui, panel, state, content);
        break;
    default:
        break;
    }

    // One line of context per tab, where a player would look for it.
    static constexpr const char* const kHints[StationScreenState::TabCount] = {
        "Prices move with stock; NPC traders share this market.",
        "Fitting draws power and fills a mount; removing refunds at resale value.",
        "A ship you buy is stored here until you switch to it.",
        "Crew bonuses apply to the ship they are aboard.",
        "Standing moves with what you do in a faction's space.",
        "Accepting a contract starts its clock; abandoning one costs standing.",
        "Scan data sells anywhere; the further out it was taken, the more it pays.",
        "Refining takes time: leave the order and come back for it.",
    };

    Row footer(footerRow, theme.spacing);
    const Rect undockCell = footer.cellFromRight(150.0f);
    ui.label(footer.remaining(), kHints[state.tab], theme.textDisabled, theme.smallStyle);
    const bool undock = ui.button(undockCell, "Undock");

    ui.popId();
    return undock;
}

} // namespace game
