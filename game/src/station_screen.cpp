#include "station_screen.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/ui/layout.hpp"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string_view>

namespace game {

using sol::ui::CaptainRow;
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
// ⚑⚑⚑⚑ 82 UNTIL THE PHASE 39 STAGE A FLIGHT, WHERE THE SHIPYARD DREW
// "50000 cr" FOR A HULL THAT COSTS 60,000. This is Phase 33's contraband-tag
// lesson arriving on a different column, and the identical failure: the cell is
// drawn RIGHT-ALIGNED, so an overflow eats the START of the string rather than
// the end - which is why it reads as a plausible smaller number instead of as
// damage. Eight glyphs ("24000 cr", "60000 cr") overflowed 82 px by about one
// glyph; seven ("9200 cr", "8000 cr") fit. 100 clears nine, which covers a
// six-figure hull value.
//
// ⚑⚑⚑ PRE-EXISTING SINCE PHASE 8a AND FOUND BY A DRIVE RATHER THAN BY A
// TEST, for the reason Phase 33 already wrote down: the synthetic test font
// ships only `hud` and `heading` styles, so a width assertion in `ui.unit`
// would be measuring a different font from the one the game draws. The
// instrument for this column is a screenshot, and the stage that added a second
// reader of it is the stage that noticed.
constexpr float kNumberWidth = 100.0f;

// Campaign missions read gold on the board, the same tell the dev screen used.
constexpr Color kCampaign = rgba(0xF2CC59FFu);

constexpr const char* const kTabLabels[StationScreenState::TabCount] = {
    "Trade",
    "Outfitting",
    "Shipyard",
    "Crew",
    "Factions",
    "Missions",
    "Survey",
    "Refinery",
    "Bar",
    // ⚑ Two words the player reads, where the def spelling is `black_market`.
    // The strip owns its labels; the assets layer owns the keyword.
    "Black Market"};

// ---------------------------------------------------------------------------
// The tab strip is a filter over what the station is made of (Phase 34 stage C).
//
// ⚑⚑⚑ THIS BLOCK IS THE HALF OF A PARALLEL PAIR THAT `data_defs.hpp` NAMED AND
// COULD NOT PIN, AND THE ASSERTIONS BELOW ARE WHAT IT WAS PROMISED. Phase 34's
// own risk register calls "two tables silently parallel" the defect this phase
// produces if it produces one; `sol::assets::StationScreen` is the def
// vocabulary and `StationScreenState::Tab` is the strip, they are deliberately
// NOT one enum - the assets layer must not learn what a tab is, and this layer
// owns the labels - and the mapping between them is the identity. That is a
// claim, so it is checked entry by entry rather than asserted on the counts and
// hoped for: a rotated table agrees with itself perfectly.
// ---------------------------------------------------------------------------

static_assert(static_cast<std::size_t>(StationScreenState::TabCount) == sol::assets::kStationScreenCount,
              "the def vocabulary and the tab strip must have the same entries");
static_assert(std::size(kTabLabels) == sol::assets::kStationScreenCount);
static_assert(static_cast<int>(StationScreenState::Trade) ==
              static_cast<int>(sol::assets::StationScreen::Trade));
static_assert(static_cast<int>(StationScreenState::Outfitting) ==
              static_cast<int>(sol::assets::StationScreen::Outfitting));
static_assert(static_cast<int>(StationScreenState::Shipyard) ==
              static_cast<int>(sol::assets::StationScreen::Shipyard));
static_assert(static_cast<int>(StationScreenState::Crew) ==
              static_cast<int>(sol::assets::StationScreen::Crew));
static_assert(static_cast<int>(StationScreenState::Factions) ==
              static_cast<int>(sol::assets::StationScreen::Factions));
static_assert(static_cast<int>(StationScreenState::Missions) ==
              static_cast<int>(sol::assets::StationScreen::Missions));
static_assert(static_cast<int>(StationScreenState::Survey) ==
              static_cast<int>(sol::assets::StationScreen::Survey));
static_assert(static_cast<int>(StationScreenState::Refinery) ==
              static_cast<int>(sol::assets::StationScreen::Refinery));
static_assert(static_cast<int>(StationScreenState::Bar) == static_cast<int>(sol::assets::StationScreen::Bar));
static_assert(static_cast<int>(StationScreenState::BlackMarket) ==
              static_cast<int>(sol::assets::StationScreen::BlackMarket));

// The identity mapping above is what lets one bit index serve both, so
// `panel.screens` is read here with the tab's own number.
[[nodiscard]] constexpr std::uint32_t tabBit(int tab)
{
    return 1u << static_cast<std::uint32_t>(tab);
}

// Whether the STATION is equipped for a tab - the union over its modules, and
// nothing about the player.
[[nodiscard]] bool stationOffers(const StationPanel& panel, int tab)
{
    return (panel.screens & tabBit(tab)) != 0u;
}

// ⚑⚑⚑⚑ THE RULING STAGE C WAS OWED, AND IT IS ONE SENTENCE: A TAB IS ON THE
// STRIP WHEN THE STATION IS EQUIPPED FOR IT, OR WHEN THE PLAYER'S OWN HALF OF
// IT HAS SOMETHING IN IT.
//
// The spec asked only for "a filter over the composition", and a plain union
// would have been wrong in a way no test would have reported: three of these
// eight tabs are a facility AND the only place the player can see something
// they own. Shipyard is "Your fleet" above "For sale"; Crew is "Aboard" above
// "For hire"; Missions is "Journal" below "Board". Measured on the shipped
// galaxy, a plain union hides your fleet at 110 docks of 125, your crew at 77
// and your journal at 55 - and a delivery still completes on `notifyDock`, so
// you would be unable to watch a contract you are in the middle of.
//
// ⚑ Two more are not the station's to withhold at all, and no module offers
// either: `Factions` is standings, which is a fact about the galaxy, and
// `Survey` sells a scan ledger that is yours, with shipped hint text that says
// it "sells anywhere". Unconditional. That also means the strip is never empty
// - every station has at least these two - which matters more than it reads:
// a plain union leaves about six stations of 125 with no tabs at all.
//
// ⚑ Outfitting is NOT on the list, deliberately. Its mount rows look like the
// player's half, but every button on them - Fit, Remove for a refund - is work
// an outfitter does, so a hull with mounts and no outfitter has nothing to
// offer but the reading, and the fit summary is already in the header.
[[nodiscard]] bool playerHasBusiness(const StationPanel& panel, int tab)
{
    switch (tab) {
    case StationScreenState::Factions:
    case StationScreenState::Survey:
        return true; // never the station's to withhold
    case StationScreenState::Shipyard:
        // More than the ship you are flying: a hull parked somewhere is a thing
        // you own and cannot otherwise see. One ship is not a fleet.
        return panel.fleet.size() > 1;
    case StationScreenState::Crew:
        return !panel.crewAboard.empty();
    case StationScreenState::Missions:
        return !panel.missionJournal.empty();
    default:
        return false;
    }
}

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
// Wide enough for CONTRABAND in the small style, taken out of the commodity-name
// cell and only on the rows that carry a tag (Phase 33 stage D). An unmarked row
// is laid out exactly as it was.
//
// ⚑⚑⚑⚑ 96 UNTIL THE PHASE 33 EXIT FLIGHT, WHERE IT DREW "ONTRABAND". The value
// was an estimate and its comment claimed "with room to spare"; the shipped font
// renders this style at about 10.6 px a glyph, so ten glyphs need ~106 px and
// both words lost their first letter off the left edge - the tag is drawn
// right-aligned, so an overflow eats the START of the word. RESTRICTED was
// clipped too and looked merely close.
//
// ⚑⚑⚑ NO TEST COULD HAVE CAUGHT IT, AND THAT IS THE POINT WORTH KEEPING. The
// synthetic test font ships only `hud` and `heading` styles, so a width
// assertion in `ui.unit` would be measuring a different font from the one the
// game draws - which is this phase's own lesson (a fixture that stands in for
// shipped content is only as good as the part of it that it mirrors). The
// instrument here is a screenshot, and 116 is measured off one rather than
// guessed a second time: it clears the widest word by ~10 px and takes 20 px
// from a name cell whose longest tagged entry is "Hull Plate".
constexpr float kLegalityWidth = 116.0f;

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

// ⚑⚑⚑⚑ ONE GOODS ROW, DRAWN BY BOTH COUNTERS (Phase 37 stage C). The dock
// has two of them at a station with a back room, and the row is the same
// object either side of the curtain: same price, same stock, same Buy. Lifting
// it out of `buildTradeTab` rather than writing a second loop is this file's
// own standing rule - `game::formatDistance` and `map_ui.cpp`'s copy of it are
// the counter-example, and that one has already drifted.
//
// ⚑⚑ `visual` IS SEPARATE FROM `index` BECAUSE THE TWO SHELVES ARE FILTERED
// VIEWS OF ONE LIST. `index` is what the trade action means - a position in
// `panel.trade.rows`, which is what maps back to a commodity - and `visual` is
// only the alternating row background. Folding them would make the second
// counter's Buy button move the wrong crate.
void tradeGoodsRow(
    UiContext& ui, StationPanel& panel, Column& column, int index, int visual, float amount, float cargoFree)
{
    const auto& theme = ui.theme();
    const TradeRow& goods = panel.trade.rows[static_cast<std::size_t>(index)];
    char buffer[64] = {};
    const Rect row = column.row(kRowHeight);
    rowBackground(ui, row, visual);
    const TradeCells cells = tradeCells(ui, row);
    ui.pushId(index);

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
    const bool canBuy =
        goods.stock > 0.0f && cargoFree >= 1.0f && panel.trade.credits >= static_cast<double>(goods.price);
    const bool canSell = goods.cargo > 0.0f;
    if (ui.button(inset(cells.buy, 2.0f), "Buy", canBuy)) {
        panel.trade.action = {.row = index, .units = amount, .isBuy = true};
    }
    if (ui.button(inset(cells.sell, 2.0f), "Sell", canSell)) {
        panel.trade.action = {.row = index, .units = amount, .isBuy = false};
    }

    ui.popId();
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
    // ⚑ Counted rather than taken from the span, since Phase 37 stage C: the
    // back room's rows are in `panel.trade.rows` and are not drawn here, and a
    // scroll region sized for rows that are not there is a screen that scrolls
    // past its own end.
    std::size_t shown = 0;
    for (const TradeRow& goods : panel.trade.rows) {
        shown += goods.backRoom ? 0u : 1u;
    }
    const float contentHeight = listHeight(ui, shown);
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
    int visual = 0;
    for (int i = 0; i < static_cast<int>(panel.trade.rows.size()); ++i) {
        // ⚑⚑⚑ THE BACK ROOM'S STOCK IS NOT ON THIS SHELF (Phase 37 stage C).
        // Until this stage the illicit goods were listed here, at market price,
        // with a Buy button and no sign of whose counter it was - which is
        // exactly what the stage sequence predicted and exactly what it was
        // asked to fix. They are the same rows; they are drawn on the Black
        // Market tab instead.
        if (panel.trade.rows[static_cast<std::size_t>(i)].backRoom) {
            continue;
        }
        tradeGoodsRow(ui, panel, column, i, visual++, amount, cargoFree);
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
    const bool sells = stationOffers(panel, StationScreenState::Shipyard);
    const float contentHeight =
        (kSectionHeight + theme.spacing) * 2.0f +
        listHeight(ui, std::max<std::size_t>(panel.fleet.size(), 1)) +
        listHeight(ui, sells ? std::max<std::size_t>(panel.shipCatalog.size(), 1) : 1);
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
        // ⚑ WHO HOLDS IT BEATS WHERE IT IS (Phase 39 stage A): a hull with a
        // captain on it is the one fact that changes what the buttons below
        // will do, and "stored here" would be true of it as well.
        const bool held = ship.captain[0] != '\0';
        clipped(ui,
                cells.detail,
                held ? ship.captain
                     : (ship.active ? "active" : (ship.storedHere ? "stored here" : "stored elsewhere")),
                (ship.active || held) ? theme.accent : theme.textDim);
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(ship.value));
        clipped(ui, cells.price, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);

        // Only a ship parked at this station can be switched to or sold; the
        // rest are listed so the fleet is still legible from anywhere.
        const bool here = !ship.active && ship.storedHere && !held;
        if (ui.button(inset(cells.primary, 2.0f), "Switch", here)) {
            panel.action = {.kind = StationAction::Kind::SwitchShip, .index = i};
        }
        if (ui.button(inset(cells.secondary, 2.0f), "Sell", here)) {
            panel.action = {.kind = StationAction::Kind::SellShip, .index = i};
        }

        ui.popId();
    }
    ui.popId();

    // The facility half. This tab is on the strip either because the station
    // sells hulls or because the player owns one they are not flying; when it
    // is only the latter, the section says so rather than reading as a shop
    // that happens to be out of stock.
    if (!sells) {
        sectionHeader(ui, column.row(kSectionHeight), "For sale");
        emptyNote(ui, column, "(no ship sales here - your fleet is listed above wherever you dock)");
        ui.endScroll();
        return;
    }
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

// One person, employed or on offer. Deliberately the same five-column shape the
// catalogs and `mountList` use, and the selection marker is `mountList`'s `> `
// for the same reason: a player reading down this tab should not have to
// re-learn what a highlighted row looks like.
struct CaptainClick
{
    int row = -1;
    bool secondary = false;
};

[[nodiscard]] CaptainClick captainList(UiContext& ui,
                                       Column& column,
                                       std::string_view id,
                                       std::span<const CaptainRow> rows,
                                       const char* primary,
                                       const char* secondary,
                                       const char* empty)
{
    CaptainClick click;
    if (rows.empty()) {
        emptyNote(ui, column, empty);
        return click;
    }
    ui.pushId(id);
    char buffer[160] = {};
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const CaptainRow& person = rows[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        const CatalogCells cells = catalogCells(ui, row, false, secondary != nullptr);
        ui.pushId(i);

        std::snprintf(buffer, sizeof(buffer), "%s%s", person.selected ? "> " : "  ", person.name);
        clipped(ui, cells.name, buffer, ui.theme().textPrimary, ui.theme().strongStyle);
        clipped(ui, cells.detail, person.detail, ui.theme().textDim);
        if (ui.button(inset(cells.primary, 2.0f), primary)) {
            click = {.row = i, .secondary = false};
        }
        // ⚑⚑ A CAPTAIN HOLDING A HULL CANNOT BE DISMISSED, AND THE BUTTON
        // SAYS SO BY BEING GREY RATHER THAN BY VANISHING - Phase 28's decision
        // 3, and the only channel a refusal has here, because a station action
        // carries no way to report one back.
        if (secondary != nullptr && ui.button(inset(cells.secondary, 2.0f), secondary, !person.assigned)) {
            click = {.row = i, .secondary = true};
        }

        ui.popId();
    }
    ui.popId();
    return click;
}

void buildCrewTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const bool hires = stationOffers(panel, StationScreenState::Crew);
    // ⚑ Which hulls a Give would be legal for: parked on THIS dock, not the
    // one you are flying, and nobody else's. Counted here so the section can
    // be left out entirely when there is nothing to hand over.
    std::size_t giveable = 0;
    for (const FleetRow& ship : panel.fleet) {
        giveable += (!ship.active && ship.storedHere) ? 1u : 0u;
    }
    const bool hasCaptains = !panel.captains.empty() || hires;
    const float contentHeight =
        (kSectionHeight + theme.spacing) * (hasCaptains ? 5.0f : 2.0f) +
        (hasCaptains ? listHeight(ui, std::max<std::size_t>(panel.captains.size(), 1)) +
                           listHeight(ui, std::max<std::size_t>(giveable, 1)) +
                           listHeight(ui, hires ? std::max<std::size_t>(panel.captainHires.size(), 1) : 1)
                     : 0.0f) +
        listHeight(ui, std::max<std::size_t>(panel.crewAboard.size(), 1)) +
        listHeight(ui, hires ? std::max<std::size_t>(panel.crewCatalog.size(), 1) : 1);
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Crew]);
    Column column(list, 0.0f, theme.spacing);

    if (hasCaptains) {
        sectionHeader(ui, column.row(kSectionHeight), "Captains");
        const CaptainClick employed = captainList(
            ui, column, "captains", panel.captains, "Select", "Dismiss", "(nobody flies for you yet)");
        if (employed.row >= 0) {
            const bool holds = panel.captains[static_cast<std::size_t>(employed.row)].assigned;
            if (employed.secondary) {
                panel.action = {.kind = StationAction::Kind::DismissCaptain, .index = employed.row};
            } else if (holds) {
                // Selecting somebody who already has a ship would aim the Give
                // buttons at a captain no hull could legally go to, so the
                // primary means "hand it back" on those rows instead.
                panel.action = {.kind = StationAction::Kind::RecallCaptain, .index = employed.row};
            } else {
                panel.action = {.kind = StationAction::Kind::SelectCaptain, .index = employed.row};
            }
        }

        sectionHeader(ui, column.row(kSectionHeight), "Ships parked here");
        if (giveable == 0) {
            emptyNote(ui, column, "(no ship of yours is parked on this dock)");
        } else {
            ui.pushId("give");
            char buffer[96] = {};
            for (int i = 0; i < static_cast<int>(panel.fleet.size()); ++i) {
                const FleetRow& ship = panel.fleet[static_cast<std::size_t>(i)];
                if (ship.active || !ship.storedHere) {
                    continue;
                }
                const Rect row = column.row(kRowHeight);
                rowBackground(ui, row, i);
                const CatalogCells cells = catalogCells(ui, row, true, false);
                ui.pushId(i);
                clipped(ui, cells.name, ship.name, ui.theme().textPrimary, theme.strongStyle);
                clipped(ui,
                        cells.detail,
                        ship.captain[0] != '\0' ? ship.captain : "unassigned",
                        ship.captain[0] != '\0' ? theme.accent : theme.textDim);
                std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(ship.value));
                clipped(ui, cells.price, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);
                const bool free = ship.captain[0] == '\0';
                if (ui.button(inset(cells.primary, 2.0f), "Give", free && panel.selectedCaptain >= 0)) {
                    panel.action = {.kind = StationAction::Kind::AssignCaptain, .index = i};
                }
                ui.popId();
            }
            ui.popId();
        }

        sectionHeader(ui, column.row(kSectionHeight), "Looking for a berth");
        if (!hires) {
            emptyNote(ui, column, "(no crew hall here - nobody is looking for a berth)");
        } else {
            const CaptainClick hire = captainList(ui,
                                                  column,
                                                  "hall",
                                                  panel.captainHires,
                                                  "Hire",
                                                  nullptr,
                                                  "(everybody here already flies for you)");
            if (hire.row >= 0) {
                panel.action = {.kind = StationAction::Kind::HireCaptain, .index = hire.row};
            }
        }
    }

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

    if (!hires) {
        sectionHeader(ui, column.row(kSectionHeight), "For hire");
        emptyNote(ui, column, "(no crew hall here - nobody is looking for a berth)");
        ui.endScroll();
        return;
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
    // A board is a facility; a journal is yours. The distinction is the whole
    // reason this tab survives a station with no board at all.
    const bool hasBoard = stationOffers(panel, StationScreenState::Missions);
    const float contentHeight =
        (kSectionHeight + theme.spacing) * 2.0f +
        listHeight(ui, hasBoard ? std::max<std::size_t>(panel.missionOffers.size(), 1) : 1) +
        listHeight(ui, std::max<std::size_t>(panel.missionJournal.size(), 1));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Missions]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), "Board");
    if (!hasBoard) {
        emptyNote(ui, column, "(no mission board here)");
    } else if (panel.missionOffers.empty()) {
        emptyNote(ui, column, "(no offers)");
    }
    ui.pushId("offers");
    char buffer[64] = {};
    // ⚑ A station with no board posts nothing (`GameContent::runMissionBoard`
    // returns before the hook runs), so this list is already empty there. Read
    // from the strip anyway rather than trusting that: offers are SAVED and a
    // docked load deliberately does not re-roll them, so a file written before
    // the board became a facility can hand this tab rows to draw underneath a
    // header that has just said there is no board. Saying both would be worse
    // than saying either.
    for (int i = 0; hasBoard && i < static_cast<int>(panel.missionOffers.size()); ++i) {
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

    // ⚑⚑ THE "(this station refines nothing)" NOTE USED TO BE HERE AND STAGE C
    // DELETED IT, WHICH IS AS MUCH OF THIS STAGE AS ANY TAB IT ADDS. A tab that
    // is present, selectable and empty on every station that does not refine is
    // the absent case in its worst form: it costs a click to learn nothing. The
    // tab is now on the strip only when a module offers the `refinery` screen,
    // and `parseModule` refuses a module that offers it without naming the pair
    // - so `refines` false here means the panel and the strip disagree, which
    // is a bug rather than a station.
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

// --- The Bar (Phase 35 stage A) ---

// ⚑⚑⚑⚑ THE ROOM HAS TO HAVE SOMETHING TO SAY ON THE DAY IT SHIPS, AND THAT IS
// A RULING RATHER THAN A FLOURISH. Stage B is where a barkeep reads the live
// galaxy - shortages, raids, fronts - and MEASURED, at t=0 the galaxy has none
// of them: every market opens at half capacity so nothing is short, nobody has
// raided so no intensity is warm, and 18 of the 64 docks with a room would open
// on an empty screen. t=0 is exactly when a new player first docks. So the
// source that ships FIRST is the one that is never empty: the dock you are
// standing on. Everything below is true the instant the galaxy exists.
//
// ⚑⚑ IT IS ALSO THE FIRST TIME A PLAYER CAN SEE ANY OF IT. Phase 34 composed
// 125 stations out of modules and left three of its own findings owed to a
// human for want of a surface: what a station cannot hold (stage D), what plant
// it was fitted with (stage B), and who runs its fence (stage E). A bar is
// where you would hear all three.
//
// ⚑ AND THE LINE THIS TAB MUST NOT CROSS: it is not a second ship screen.
// "What the house cannot take" is content because it is scarce and it changes
// what you fly here with; a readout of every module on the station is a data
// dump wearing a room's name.
void buildBarTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    const std::size_t rows = std::max<std::size_t>(panel.barTalk.size(), 1);
    const bool named = panel.barKeeper[0] != '\0';
    const float contentHeight =
        kSectionHeight + theme.spacing + (named ? kRowHeight + theme.spacing : 0.0f) + listHeight(ui, rows);
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::Bar]);
    Column column(list, 0.0f, theme.spacing);

    sectionHeader(ui, column.row(kSectionHeight), panel.barRoom[0] != '\0' ? panel.barRoom : "The room");
    // Who is talking, above everything they say (Phase 35 stage C). Dim and
    // unboxed on purpose: it is an attribution rather than a row, and a room
    // whose regular is nobody in particular should not look like a room with a
    // heading missing.
    if (named) {
        ui.label(column.row(kRowHeight), panel.barKeeper, theme.textDim, theme.strongStyle);
    }
    if (panel.barTalk.empty()) {
        // Unreachable on the shipped galaxy - the house facts below are never
        // all absent - and kept because a tab that draws nothing at all is
        // worse than one that says so. See the note on the strip: the tab is
        // only ever here because a module put a room on this station.
        emptyNote(ui, column, "(nobody is saying anything)");
    }
    ui.pushId("talk");
    for (int i = 0; i < static_cast<int>(panel.barTalk.size()); ++i) {
        const sol::ui::InfoRow& line = panel.barTalk[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        Row cursor(row, theme.spacing);
        // A fixed topic column, so five sentences of very different lengths
        // still read as a list rather than as a paragraph. The sentence takes
        // the rest and is clipped, because it is generated from def names and
        // faction names and has no length anybody controls.
        const Rect topic = cursor.cell(150.0f);
        clipped(ui, topic, line.label, theme.textDim, theme.strongStyle);
        // ⚑⚑ THE BUTTON IS TAKEN OFF THE END OF THE ROW *BEFORE* THE SENTENCE
        // CLAIMS THE REST, WHICH IS THE ONLY ORDER THAT WORKS. `remaining()`
        // hands back everything left, so measuring the sentence first would
        // leave the button drawing on top of it - and only ONE row in a room
        // ever has a button, so the bug would be invisible on the other four.
        // Every line keeps its full width when there is nothing to take.
        if (line.action[0] != '\0') {
            ui.pushId(i);
            if (ui.button(inset(cursor.cellFromRight(90.0f), 2.0f), line.button)) {
                // The action goes back UNREAD, which is `InfoRow`'s contract in
                // its own words - "the screen never interprets either string".
                // `stationAction` turns it into a lead index on the way out.
                panel.action = {.kind = StationAction::Kind::AcceptLead, .id = line.action};
            }
            ui.popId();
        }
        clipped(ui, cursor.remaining(), line.value, theme.textPrimary);
    }
    ui.popId();

    ui.endScroll();
}

} // namespace

// --- The back room (Phase 37 stage C) ---------------------------------------

// ⚑⚑⚑⚑ THE FENCE'S COUNTER, AND IT IS A PLACE RATHER THAN A FORMATTING
// CHANGE. Before this stage the illicit goods sat on the ordinary Trade tab at
// eight docks - market price, Buy button, no legality label, and nothing at all
// saying whose counter it was. The playtest note on it was exact: "what the
// stage sequence predicted, and still worth a look." This is the look.
//
// ⚑⚑⚑ THE HEADING IS THE FEATURE. `017` says the black market is a FACTION
// and not a place, so the one thing this screen must do that the Trade tab
// could not is say the faction's name - the same name the Factions tab carries
// a standing for. Everything else here is a shelf.
//
// ⚑⚑ TWO SECTIONS, NOT TWO TABS, because they are one counter: cargo the
// lawful galaxy has nowhere to put, and kit no lawful outfitter carries. A
// player who came to sell a crate should see what the money is for without
// changing screens.
void buildBlackMarketTab(UiContext& ui, StationPanel& panel, StationScreenState& state, const Rect& content)
{
    const auto& theme = ui.theme();
    std::size_t goods = 0;
    for (const TradeRow& row : panel.trade.rows) {
        goods += row.backRoom ? 1u : 0u;
    }
    const float contentHeight = (kSectionHeight + theme.spacing) * 3.0f +
                                listHeight(ui, std::max<std::size_t>(goods, 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.blackMarketCatalog.size(), 1)) +
                                listHeight(ui, std::max<std::size_t>(panel.blackMarketShips.size(), 1));
    const Rect list = ui.beginScroll(content, contentHeight, state.scroll[StationScreenState::BlackMarket]);
    Column column(list, 0.0f, theme.spacing);

    // Whose back room this is. Never empty where the tab is on the strip - the
    // tab is on the strip exactly where a shadow module composed, and a shadow
    // module has an operator - so there is no "unknown" case to word.
    char heading[128] = {};
    std::snprintf(heading,
                  sizeof(heading),
                  "%s%s",
                  panel.fenceOperator[0] != '\0' ? panel.fenceOperator : "The back room",
                  panel.fenceOperator[0] != '\0' ? " - the back room" : "");
    sectionHeader(ui, column.row(kSectionHeight), heading);

    const float amount = kTradeAmounts[state.tradeAmount];
    const float cargoFree = panel.trade.cargoCapacity - panel.trade.cargoUsed;
    ui.pushId("contraband");
    int visual = 0;
    for (int i = 0; i < static_cast<int>(panel.trade.rows.size()); ++i) {
        if (!panel.trade.rows[static_cast<std::size_t>(i)].backRoom) {
            continue;
        }
        tradeGoodsRow(ui, panel, column, i, visual++, amount, cargoFree);
    }
    if (goods == 0) {
        clipped(ui, column.row(kRowHeight), "Nothing in the hold today.", theme.textDim, theme.bodyStyle);
    }
    ui.popId();

    sectionHeader(ui, column.row(kSectionHeight), "Kit");
    ui.pushId("blackmarket");
    for (int i = 0; i < static_cast<int>(panel.blackMarketCatalog.size()); ++i) {
        const OutfitRow& item = panel.blackMarketCatalog[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        Row cursor(row, theme.spacing);
        ui.pushId(i);
        // ⚑⚑ A LOCKED ROW TAKES A WIDER CELL THAN A BUTTON, because the whole
        // reason it is on the screen is the sentence in it. "Needs 25 with The
        // Ninth Shift" drawn into a 78px Buy-button cell elides to "Need..." -
        // the same defect Phase 35 stage A and Phase 36's roster readout each
        // paid for once, and both times only a screenshot caught it. The width
        // is measured against the longest reason this content can produce, and
        // `the_locked_row_is_on_the_shelf_and_says_what_it_would_take` keeps
        // the two in step.
        const bool locked = item.lockedReason[0] != '\0';
        const Rect action = cursor.cellFromRight(locked ? kButtonWidth * 3.2f : kButtonWidth);
        const Rect priceCell = cursor.cellFromRight(kNumberWidth);
        const Rect nameCell = cursor.cellFromRight(cursor.remaining().width() * 0.42f);
        clipped(ui, cursor.remaining(), item.name, theme.textPrimary, theme.strongStyle);
        clipped(ui, nameCell, item.detail, theme.textDim, theme.smallStyle);
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(item.price));
        clipped(ui, priceCell, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);

        // ⚑⚑⚑⚑ A LOCKED ROW SAYS WHAT IT COSTS, AND THAT IS THE WHOLE REASON
        // IT IS ON THE SCREEN. Every other catalog in this game answers a gate
        // by leaving the row OFF; this counter stocks the one thing in the game
        // a player cannot have yet, and an empty shelf would have been
        // indistinguishable from a fence with nothing to sell. The word is in
        // `theme.accent` rather than `negative` because it is a price, not a
        // refusal - somebody can pay it.
        if (locked) {
            clipped(ui, action, item.lockedReason, theme.accent, theme.smallStyle, TextAlign::Right);
        } else if (ui.button(inset(action, 2.0f), "Buy", item.targetMount[0] != '\0')) {
            panel.action = {
                .kind = StationAction::Kind::BuyFitting, .id = item.id, .mount = item.targetMount};
        }
        ui.popId();
    }
    if (panel.blackMarketCatalog.empty()) {
        clipped(ui, column.row(kRowHeight), "No kit on offer here.", theme.textDim, theme.bodyStyle);
    }
    ui.popId();

    // ⚑⚑⚑ HULLS WITH NO HISTORY (Phase 37 stage D). A `sol.mod_ghost_dock`
    // strips a hull into parts with no history - that is the module's authored
    // description and its whole economic function - so a hull sold over the same
    // counter is the trade running the other way. `owned` rather than a Fit
    // target, because a ship is not fitted to anything: buying one parks it here
    // until the player switches to it, which is what the Shipyard tab says too.
    sectionHeader(ui, column.row(kSectionHeight), "Hulls");
    ui.pushId("blackmarketships");
    for (int i = 0; i < static_cast<int>(panel.blackMarketShips.size()); ++i) {
        const OutfitRow& item = panel.blackMarketShips[static_cast<std::size_t>(i)];
        const Rect row = column.row(kRowHeight);
        rowBackground(ui, row, i);
        Row cursor(row, theme.spacing);
        ui.pushId(i);
        const bool locked = item.lockedReason[0] != '\0';
        const Rect action = cursor.cellFromRight(locked ? kButtonWidth * 3.2f : kButtonWidth);
        const Rect ownedCell = cursor.cellFromRight(kNumberWidth * 0.8f);
        const Rect priceCell = cursor.cellFromRight(kNumberWidth);
        const Rect detailCell = cursor.cellFromRight(cursor.remaining().width() * 0.5f);
        clipped(ui, cursor.remaining(), item.name, theme.textPrimary, theme.strongStyle);
        clipped(ui, detailCell, item.detail, theme.textDim, theme.smallStyle);
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "%.0f cr", static_cast<double>(item.price));
        clipped(ui, priceCell, buffer, theme.textPrimary, theme.bodyStyle, TextAlign::Right);
        std::snprintf(buffer, sizeof(buffer), "owned %d", item.fitted);
        clipped(ui,
                ownedCell,
                buffer,
                item.fitted > 0 ? theme.accent : theme.textDim,
                theme.smallStyle,
                TextAlign::Right);
        if (locked) {
            clipped(ui, action, item.lockedReason, theme.accent, theme.smallStyle, TextAlign::Right);
        } else if (ui.button(inset(action, 2.0f), "Buy")) {
            panel.action = {.kind = StationAction::Kind::BuyShip, .id = item.id};
        }
        ui.popId();
    }
    if (panel.blackMarketShips.empty()) {
        clipped(ui, column.row(kRowHeight), "No hulls in the bay.", theme.textDim, theme.bodyStyle);
    }
    ui.popId();
    ui.endScroll();
}

bool stationTabOnStrip(const StationPanel& panel, int tab)
{
    if (tab < 0 || tab >= StationScreenState::TabCount) {
        return false;
    }
    return stationOffers(panel, tab) || playerHasBusiness(panel, tab);
}

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

    // The strip this station draws, in tab order (Phase 34 stage C).
    int strip[StationScreenState::TabCount] = {};
    const char* labels[StationScreenState::TabCount] = {};
    int stripCount = 0;
    for (int tab = 0; tab < StationScreenState::TabCount; ++tab) {
        if (!stationTabOnStrip(panel, tab)) {
            continue;
        }
        strip[stripCount] = tab;
        labels[stripCount] = kTabLabels[tab];
        ++stripCount;
    }

    // ⚑⚑⚑ `state.tab` IS THE TAB'S IDENTITY, NEVER ITS PLACE ON THIS STRIP, AND
    // THAT DISTINCTION IS THE BUG THIS STAGE WOULD OTHERWISE HAVE SHIPPED.
    // `UiContext::tabs` ends with `clamp(selected, 0, count - 1)` because it was
    // written for a fixed strip; hand it a remembered `Refinery` at a station
    // with three tabs and it silently returns a DIFFERENT tab, which is the
    // player's place lost with no way to notice. So the index is derived per
    // station, and what is remembered is only written back when the player
    // actually moved - a Refinery you were reading survives a dock at an
    // outpost that has none, and is still there at the next refinery.
    int selected = 0;
    for (int i = 0; i < stripCount; ++i) {
        if (strip[i] == state.tab) {
            selected = i;
            break;
        }
    }
    if (ui.tabs(column.row(kTabHeight), std::span(labels, static_cast<std::size_t>(stripCount)), selected)) {
        state.tab = strip[selected];
    }
    // What to draw this frame: the remembered tab when this station has it, and
    // otherwise the first one it does have. `stripCount` is at least two by
    // construction - Factions and Survey are unconditional - but a screen that
    // draws nothing is a better failure than one that indexes past its labels.
    const int shown = stripCount > 0 ? strip[selected] : -1;

    // Footer first: the content region is whatever the tabs and the footer
    // leave, and it has to be known before the tab draws into it.
    const Rect body = column.remaining();
    const Rect footerRow = {{body.min.x, body.max.y - kFooterHeight}, {body.max.x, body.max.y}};
    const Rect content = {body.min, {body.max.x, footerRow.min.y - theme.spacing}};

    switch (shown) {
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
    case StationScreenState::Bar:
        buildBarTab(ui, panel, state, content);
        break;
    case StationScreenState::BlackMarket:
        buildBlackMarketTab(ui, panel, state, content);
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
        "What you hear in here is true; whether it is worth anything is your problem.",
        "Nobody lawful has a hold for any of this; standing here is not standing anywhere else.",
    };

    Row footer(footerRow, theme.spacing);
    const Rect undockCell = footer.cellFromRight(150.0f);
    if (shown >= 0) {
        ui.label(footer.remaining(), kHints[shown], theme.textDisabled, theme.smallStyle);
    }
    const bool undock = ui.button(undockCell, "Undock");

    ui.popId();
    return undock;
}

} // namespace game
