#pragma once

#include "sol/core/math/vec.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace sol::ui {

struct OverlayStats
{
    float fps = 0.0f;
    float frameMilliseconds = 0.0f;
    core::DVec3 cameraPosition;
    float cameraSpeed = 0.0f;
    std::uint32_t drawCalls = 0;
    std::uint64_t simTicks = 0;
    std::uint32_t simEntities = 0;
    float simAlpha = 0.0f;
};

// Provisional flight HUD (engine plan 2.9: ImGui until the real game UI
// exists; Phase 4 teaches us what the HUD needs).
struct FlightHud
{
    bool active = false;
    float speedMetersPerSecond = 0.0f;
    bool assist = true;
    bool boost = false;
    bool cruise = false;
    bool autopilot = false;
    const char* cameraMode = "";
    const char* targetName = "";
    double targetDistanceMeters = 0.0;
    float closingSpeedMetersPerSecond = 0.0f;
    core::Vec3 targetDirectionCamera; // unit, camera space (-Z forward)
    float tanHalfFovY = 1.0f;

    // Power pips (decisions/003); pipMax caps each bar, charge is 0..1.
    int pipsWeapons = 2;
    int pipsEngines = 2;
    int pipsShields = 2;
    int pipMax = 4;
    float weaponCharge = 1.0f;

    // Defenses (decisions/002), all 0..1: fore/aft shield arcs around the
    // crosshair plus hull in the readout strip.
    float shieldFore = 1.0f;
    float shieldAft = 1.0f;
    float hull = 1.0f;

    // Combat feedback: targeted-ship readout, projectile lead marker, and a
    // crosshair flash while the player is taking hits.
    bool targetIsShip = false;
    float targetShieldFore = 0.0f;
    float targetShieldAft = 0.0f;
    float targetHull = 0.0f;
    bool hasLead = false;
    core::Vec3 leadDirectionCamera; // unit, camera space
    float damageFlash = 0.0f;       // 0..1

    // Universe context (Phase 7): current system, and jump/dock prompts
    // while the ship is within activation range.
    const char* systemName = "";
    bool gateInRange = false;
    bool dockInRange = false;
    bool docked = false;
    const char* dockedStationName = "";
};

// Provisional trade screen (Phase 7; replaced by real game UI in Phase 8).
// The game fills rows from the docked market; a clicked button reports back
// through `action` for the game to execute against the world.
struct TradeRow
{
    const char* name = "";
    float price = 0.0f; // credits/unit right now
    float stock = 0.0f; // station stock, units
    float cargo = 0.0f; // in the player's hold, units
};

struct TradeAction
{
    int row = -1; // index into rows; -1 = no click this frame
    float units = 0.0f;
    bool isBuy = false;
};

struct TradePanel
{
    const char* stationName = "";
    double credits = 0.0;
    float cargoUsed = 0.0f;
    float cargoCapacity = 0.0f;
    std::span<const TradeRow> rows;
    TradeAction action; // out
};

// Catalog/inventory row for the outfitting, shipyard, and crew tabs
// (Phase 8a; still the provisional dev UI).
struct OutfitRow
{
    const char* id = "";
    const char* name = "";
    const char* detail = ""; // slot/kind/role + stat summary, prebuilt
    float price = 0.0f;
    int fitted = 0; // instances currently on the active ship (catalogs)
};

struct FleetRow
{
    const char* name = "";
    bool active = false;
    bool storedHere = false; // stored at THIS station (switch/sell allowed)
    float value = 0.0f;      // hull + fit at list price
};

// What the player clicked this frame; the game executes it.
struct StationAction
{
    enum class Kind : std::uint32_t
    {
        None = 0,
        BuyModule,
        SellModule,
        BuyWeapon,
        BuyShip,
        SellShip,
        SwitchShip,
        HireCrew,
        FireCrew,
    };
    Kind kind = Kind::None;
    const char* id = ""; // def id (module/weapon/ship/crew actions)
    int index = -1;      // fleet index (sell/switch ship)
};

// The docked-station screen: Trade plus the Phase 8a Outfitting, Shipyard,
// and Crew tabs.
struct StationPanel
{
    TradePanel trade;
    const char* fitSummary = "";  // active ship fit + budgets, prebuilt
    double deductible = 0.0;      // current insurance quote
    std::span<const OutfitRow> modules; // catalog
    std::span<const OutfitRow> weapons; // catalog ("fitted" flags the mount)
    std::span<const OutfitRow> crewCatalog;
    std::span<const OutfitRow> crewAboard;
    std::span<const OutfitRow> shipCatalog;
    std::span<const FleetRow> fleet;
    StationAction action; // out
};

// Dear ImGui dev/debug overlay (never player-facing UI - see engine plan 2.9).
class DevUi
{
public:
    [[nodiscard]] bool initialize(platform::Window& window, rhi::Context& context,
                                  VkFormat colorFormat, VkFormat depthFormat,
                                  std::uint32_t swapchainImageCount);
    void shutdown();

    // Once per frame, before recording; builds the overlay + console windows
    // and, when hud.active, the flight HUD. A non-null station panel (docked)
    // also builds the provisional trade/outfitting/shipyard/crew screen.
    void beginFrame(const OverlayStats& stats, const FlightHud& hud = {},
                    StationPanel* station = nullptr);

    // Records draw data; must be called inside the scene's dynamic rendering pass.
    void render(VkCommandBuffer commandBuffer);

    // Call instead of render() when the frame is abandoned (swapchain out of date).
    void discardFrame();

    // Console command line: submitted text goes to the handler (e.g. a Lua
    // VM); without one the input line is hidden and the console is log-only.
    using CommandHandler = void (*)(const char* command, void* userData);
    void setCommandHandler(CommandHandler handler, void* userData);

    // True while ImGui wants the mouse (e.g. clicking the console) - game
    // actions like the weapon trigger should stand down.
    [[nodiscard]] bool wantsMouseCapture() const;

private:
    void buildWindows(const OverlayStats& stats);
    void buildConsoleInput();
    void buildFlightHud(const FlightHud& hud);
    void buildStationPanel(StationPanel& station);
    void buildTradeTab(TradePanel& trade);
    void buildOutfittingTab(StationPanel& station);
    void buildShipyardTab(StationPanel& station);
    void buildCrewTab(StationPanel& station);
    static int consoleTextCallback(ImGuiInputTextCallbackData* data);

    bool m_initialized = false;
    bool m_frameOpen = false;
    bool m_showConsole = true;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    CommandHandler m_commandHandler = nullptr;
    void* m_commandUserData = nullptr;
    char m_commandBuffer[512] = {};
    std::vector<std::string> m_commandHistory;
    int m_historyIndex = -1; // -1 = editing a fresh line
};

} // namespace sol::ui
