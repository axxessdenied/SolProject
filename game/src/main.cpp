#include "content.hpp"
#include "fly_camera.hpp"
#include "game_ui.hpp"
#include "map_screen.hpp"
#include "map_ui.hpp"
#include "menu_screens.hpp"
#include "scene_renderer.hpp"
#include "shader_watcher.hpp"
#include "ship_camera.hpp"
#include "space_world.hpp"
#include "station_screen.hpp"
#include "station_ui.hpp"

#include "sol/core/log.hpp"
#include "sol/core/version.hpp"
#include "sol/sim/fixed_loop.hpp"
#include "sol/sim/flight.hpp"
#include "sol/sim/power.hpp"
#include "sol/sim/weapons.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/dev_ui.hpp"
#include "sol/ui/screens.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

#if defined(NDEBUG)
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

// --frames N: render N frames, then exit (for automated runs).
std::uint64_t parseMaxFrames(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            return std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    return 0;
}

// --seed N: universe seed (same seed => same galaxy).
constexpr std::uint64_t kDefaultUniverseSeed = 1701;
std::uint64_t parseUniverseSeed(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0) {
            return std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    return kDefaultUniverseSeed;
}

// --hardcore: ironman mode (decisions/007) — death deletes the save.
bool parseHardcore(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--hardcore") == 0) {
            return true;
        }
    }
    return false;
}

// A gate accepts a jump inside this range (provisional until docking tuning).
constexpr double kGateActivationRange = 10'000.0;

// A station accepts a dock request inside this range.
constexpr double kDockRange = 2'000.0;

// Latches per-frame window state into a flight-model input. Rotation combines
// a self-centering virtual stick fed by the mouse (hold RMB) with full-deflection
// arrow keys; Q/E roll. WASD + Space/Ctrl thrust, Shift boost, Tab toggles
// cruise, X toggles assist.
class ShipInputMapper
{
public:
    [[nodiscard]] sol::sim::FlightInput update(sol::platform::Window& window, float deltaSeconds)
    {
        using sol::platform::Key;
        using sol::platform::MouseButton;

        const bool steering = window.isMouseButtonDown(MouseButton::Right);
        window.setCursorLocked(steering);
        if (steering) {
            const sol::core::Vec2 delta = window.mouseDelta();
            m_stick.x -= delta.y * kStickSensitivity; // mouse up = nose up
            m_stick.y -= delta.x * kStickSensitivity; // mouse left = yaw left
        }
        // Self-centering, Elite-style relative mouse.
        const float recenter = std::exp(-kRecenterRate * deltaSeconds);
        m_stick.x = sol::core::clamp(m_stick.x * recenter, -1.0f, 1.0f);
        m_stick.y = sol::core::clamp(m_stick.y * recenter, -1.0f, 1.0f);

        sol::sim::FlightInput input;
        input.angular.x = applyDeadZone(m_stick.x);
        input.angular.y = applyDeadZone(m_stick.y);
        if (window.isKeyDown(Key::Up)) input.angular.x += 1.0f;
        if (window.isKeyDown(Key::Down)) input.angular.x -= 1.0f;
        if (window.isKeyDown(Key::Left)) input.angular.y += 1.0f;
        if (window.isKeyDown(Key::Right)) input.angular.y -= 1.0f;
        if (window.isKeyDown(Key::Q)) input.angular.z -= 1.0f; // roll left
        if (window.isKeyDown(Key::E)) input.angular.z += 1.0f;

        if (window.isKeyDown(Key::W)) input.linear.z -= 1.0f; // main drive
        if (window.isKeyDown(Key::S)) input.linear.z += 1.0f;
        if (window.isKeyDown(Key::A)) input.linear.x -= 1.0f;
        if (window.isKeyDown(Key::D)) input.linear.x += 1.0f;
        if (window.isKeyDown(Key::Space)) input.linear.y += 1.0f;
        if (window.isKeyDown(Key::LeftControl)) input.linear.y -= 1.0f;

        input.boost = window.isKeyDown(Key::LeftShift);
        if (pressed(window, Key::X, m_previousAssistKey)) {
            m_assist = !m_assist;
        }
        if (pressed(window, Key::Tab, m_previousCruiseKey)) {
            m_cruise = !m_cruise;
        }
        input.assist = m_assist;
        input.cruise = m_cruise;
        return input;
    }

private:
    [[nodiscard]] static bool pressed(sol::platform::Window& window, sol::platform::Key key,
                                      bool& previous)
    {
        const bool down = window.isKeyDown(key);
        const bool edge = down && !previous;
        previous = down;
        return edge;
    }

    // Small deflections command nothing (easier fine alignment; also kills
    // the drift the recenter exponential never quite decays away); the
    // response is rescaled past the edge so it stays continuous.
    [[nodiscard]] static float applyDeadZone(float value)
    {
        const float magnitude = std::fabs(value);
        if (magnitude <= kStickDeadZone) {
            return 0.0f;
        }
        return std::copysign((magnitude - kStickDeadZone) / (1.0f - kStickDeadZone), value);
    }

    static constexpr float kStickSensitivity = 0.0035f;
    static constexpr float kRecenterRate = 2.5f;
    static constexpr float kStickDeadZone = 0.08f;

    sol::core::Vec2 m_stick;
    bool m_assist = true;
    bool m_cruise = false;
    bool m_previousAssistKey = false;
    bool m_previousCruiseKey = false;
};

void consoleCommandHandler(const char* command, void* userData)
{
    static_cast<game::GameContent*>(userData)->executeConsole(command);
}

} // namespace

int main(int argc, char** argv)
{
    const std::uint64_t maxFrames = parseMaxFrames(argc, argv);

    SOL_LOG_INFO("Sol Engine %s on %s", sol::core::engineVersionString(), sol::platform::platformName());

    sol::platform::Window window;
    sol::platform::WindowDesc windowDesc = {};
    windowDesc.title = "The Stars Don't Wait";
    if (!window.create(windowDesc)) {
        return EXIT_FAILURE;
    }

    sol::rhi::Context context;
    sol::rhi::ContextDesc contextDesc = {};
    contextDesc.appName = "The Stars Don't Wait";
    contextDesc.enableValidation = kEnableValidation;
    if (!context.initialize(contextDesc, window.nativeHandle())) {
        return EXIT_FAILURE;
    }

    sol::rhi::Swapchain swapchain;
    if (!swapchain.create(context, window.width(), window.height())) {
        return EXIT_FAILURE;
    }

    const std::string executableDir = sol::platform::executableDirectory();
    const std::string shaderDirectory = executableDir + "shaders/";
    const std::string cookedDirectory = executableDir + "cooked/";
    const std::string savePath = executableDir + "world.sav";

    game::SceneRenderer renderer;
    if (!renderer.initialize(context, swapchain, shaderDirectory.c_str(), cookedDirectory.c_str())) {
        return EXIT_FAILURE;
    }

    sol::ui::DevUi devUi;
    if (!devUi.initialize(window, context, swapchain.imageFormat(), VK_FORMAT_D32_SFLOAT,
                          swapchain.imageCount())) {
        return EXIT_FAILURE;
    }
    renderer.setDevUi(&devUi);

    // Game UI (Phase 8d). One context builds every custom surface - the flight
    // readout and the menu shell - into a single draw list per frame.
    sol::ui::UiContext ui;
    ui.setFont(&renderer.uiFont(), renderer.uiFontTexture());

    // The shell the game never had: it boots to a menu instead of straight
    // into the cockpit.
    game::GameState state = game::GameState::MainMenu;
    game::GameState settingsReturnState = game::GameState::MainMenu;
    game::MainMenuState mainMenuState;
    game::StationScreenState stationScreen; // tab + scroll positions, across frames
    game::MapScreenState mapScreen;         // map tab, scroll, and selection
    mainMenuState.hasSave = sol::platform::fileModificationTime(savePath.c_str()) != 0;

    const std::string settingsPath = executableDir + "settings.toml";
    game::Settings settings;
    (void)settings.load(settingsPath.c_str()); // absent is normal on a first run

#if !defined(SOL_SHADER_SOURCE_DIR)
    #define SOL_SHADER_SOURCE_DIR ""
#endif
#if !defined(SOL_GLSLC_PATH)
    #define SOL_GLSLC_PATH ""
#endif
    game::ShaderWatcher shaderWatcher(SOL_SHADER_SOURCE_DIR, SOL_GLSLC_PATH, shaderDirectory);

    // The player ship exists from here; the galaxy itself is generated by
    // GameContent::initialize once the defs are loaded (Phase 7).
    sol::sim::FixedLoop simLoop(60.0);
    game::SpaceWorld world;
    world.spawn(parseUniverseSeed(argc, argv));
    world.setHardcore(parseHardcore(argc, argv));
    if (world.hardcore()) {
        SOL_LOG_INFO("HARDCORE run: death deletes the save");
    }

    // Phase 5 data-driven content: defs + Lua from the source tree in dev
    // builds (hot-reloadable), from the install layout otherwise.
#if !defined(SOL_DATA_SOURCE_DIR)
    #define SOL_DATA_SOURCE_DIR ""
#endif
#if !defined(SOL_MODS_SOURCE_DIR)
    #define SOL_MODS_SOURCE_DIR ""
#endif
    const std::string dataDirectory =
        std::strlen(SOL_DATA_SOURCE_DIR) > 0 ? SOL_DATA_SOURCE_DIR : executableDir + "data";
    const std::string modsDirectory =
        std::strlen(SOL_MODS_SOURCE_DIR) > 0 ? SOL_MODS_SOURCE_DIR : executableDir + "mods";
    game::GameContent content;
    if (!content.initialize(dataDirectory, modsDirectory, &world)) {
        return EXIT_FAILURE;
    }
    devUi.setCommandHandler(&consoleCommandHandler, &content);
    ShipInputMapper inputMapper;
    game::ShipCamera shipCamera;
    game::FlyCamera freeCamera;
    game::CameraMode cameraMode = game::CameraMode::ThirdPerson;
    std::vector<game::RenderInstance> renderInstances;
    std::vector<game::ParticleInstance> particleInstances;
    game::SceneInfo sceneInfo;
    std::vector<sol::ui::TradeRow> tradeRows;
    std::vector<sol::ui::OutfitRow> moduleRows;
    std::vector<sol::ui::OutfitRow> weaponRows;
    std::vector<sol::ui::OutfitRow> crewCatalogRows;
    std::vector<sol::ui::OutfitRow> crewAboardRows;
    std::vector<sol::ui::OutfitRow> shipRows;
    std::vector<sol::ui::FleetRow> fleetRows;
    std::vector<sol::ui::FactionRow> factionRows;
    std::vector<sol::ui::MissionRow> missionOfferRows;
    std::vector<sol::ui::MissionRow> missionJournalRows;
    std::vector<sol::ui::SurveyRow> surveyRows;
    std::vector<sol::ui::MapSystemRow> mapSystemRows;
    std::vector<sol::ui::MapLaneRow> mapLaneRows;
    std::vector<sol::ui::MapMarkerRow> mapMarkerRows;
    std::string missionHudObjective;
    std::string routeHopName;
    game::ProspectInfo prospect; // backs the HUD's mining readout per frame
    std::deque<std::string> stationText; // backs generated row text per frame
    std::deque<std::string> mapText;     // same, for the map screen
    SOL_LOG_INFO("Space world: %u entities in '%s' (%zu-system galaxy).", world.entityCount(),
                 world.currentSystemName(), world.galaxy().systems.size());

    float smoothedFps = 0.0f;
    bool previousF3 = false;
    bool previousF5 = false;
    bool previousF9 = false;
    bool previousF10 = false;
    bool previousV = false;
    bool previousT = false;
    bool previousJ = false;
    bool previousG = false;
    bool previousF = false;
    bool previousR = false;
    bool previousM = false;
    bool previousPip1 = false;
    bool previousPip2 = false;
    bool previousPip3 = false;
    bool previousPip4 = false;
    bool previousEscape = false;
    bool previousNavTab = false;
    bool previousNavUp = false;
    bool previousNavDown = false;
    bool previousNavActivate = false;
    bool previousNavSpace = false;
    bool previousMouseDown = false;
    bool quitRequested = false;
    bool showDebugDraw = false;

    SOL_LOG_INFO("Entering frame loop (%ux%u). RMB+mouse steer, WASD/QE/Space/Ctrl thrust, "
                 "Shift boost, Tab cruise, X assist, F autopilot, V camera, T target, ESC quits.",
                 window.width(), window.height());

    double lastFrameTime = sol::platform::timeSeconds();
    std::uint64_t frameCount = 0;
    bool failed = false;

    while (true) {
        window.pumpEvents();
        if (window.shouldClose() || quitRequested) {
            break;
        }
        if (window.isMinimized()) {
            sol::platform::sleepMilliseconds(10);
            continue;
        }

        const double now = sol::platform::timeSeconds();
        const float deltaSeconds = sol::core::clamp(static_cast<float>(now - lastFrameTime), 0.0f, 0.1f);
        lastFrameTime = now;

        // Who owns the keyboard this frame. The ship takes orders only in
        // flight, so Tab means "next widget" on a screen and "cruise" in the
        // cockpit without either fighting the other. Docking counts as a
        // screen - the station UI wants those keys - but not as a pause: the
        // market and the mission board are on the clock while the player
        // shops, so the sim keeps running.
        const bool inFlight = state == game::GameState::Flying;
        const bool docked = state == game::GameState::Docked;
        const bool onMap = state == game::GameState::Map;
        // The map is a screen, not a pause: the economy, the factions, and
        // whatever is shooting at you all keep running while it is open.
        const bool simRunning = inFlight || docked || onMap;
        const bool uiHasKeys = !inFlight;                 // menus, station, map
        const bool inMenuScreen = uiHasKeys && !docked;   // where Esc means "back out"
        const auto gameplayKey = [&](sol::platform::Key key) {
            return inFlight && window.isKeyDown(key);
        };

        // Esc opens the pause menu; the menus handle backing out themselves.
        const bool escapeDown = window.isKeyDown(sol::platform::Key::Escape);
        const bool escapeEdge = escapeDown && !previousEscape;
        previousEscape = escapeDown;
        // The map handles Esc itself (it closes), so it is excluded here even
        // though its clock is running.
        if (escapeEdge && (inFlight || docked)) {
            state = game::GameState::Paused;
            window.setCursorLocked(false);
        }

        // Camera mode cycle (V): first person -> chase -> free.
        const bool vDown = gameplayKey(sol::platform::Key::V);
        if (vDown && !previousV) {
            switch (cameraMode) {
            case game::CameraMode::FirstPerson:
                cameraMode = game::CameraMode::ThirdPerson;
                shipCamera.snapTo(world.shipRenderTransform(simLoop.alpha()));
                break;
            case game::CameraMode::ThirdPerson:
                cameraMode = game::CameraMode::Free;
                freeCamera.setPosition(
                    shipCamera.thirdPerson(world.shipRenderTransform(simLoop.alpha()), 0.0f).position);
                break;
            case game::CameraMode::Free:
                cameraMode = game::CameraMode::FirstPerson;
                break;
            }
        }
        previousV = vDown;

        const bool tDown = gameplayKey(sol::platform::Key::T);
        if (tDown && !previousT) {
            world.cycleTarget();
            SOL_LOG_INFO("Target: %s", world.currentTargetInfo().nav.name.c_str());
        }
        previousT = tDown;

        // Jump through the nearest in-range gate (decisions/004 gate travel).
        const bool jDown = gameplayKey(sol::platform::Key::J);
        if (jDown && !previousJ) {
            if (world.jumpNearestGate(kGateActivationRange)) {
                SOL_LOG_INFO("Arrived in '%s'", world.currentSystemName());
            } else {
                SOL_LOG_INFO("No gate within %.0f km", kGateActivationRange / 1000.0);
            }
        }
        previousJ = jDown;

        // Autopilot to the selected target (F toggles; manual input cancels).
        const bool fDown = gameplayKey(sol::platform::Key::F);
        if (fDown && !previousF) {
            if (world.autopilotActive()) {
                world.disengageAutopilot();
                SOL_LOG_INFO("Autopilot: disengaged");
            } else if (!world.engageAutopilot()) {
                SOL_LOG_INFO("Autopilot: no target (or docked)");
            }
        }
        previousF = fDown;

        // Dock/undock at the nearest station (G toggles). Undocking is the one
        // gameplay key the station screen leaves live, beside its own button.
        // G is also the salvage key (Phase 8e): one interact key, and a
        // station in range wins over a wreck.
        const bool gDown = (inFlight || docked) && window.isKeyDown(sol::platform::Key::G);
        if (gDown && !previousG) {
            if (world.isDocked()) {
                (void)world.undock();
            } else if (!world.tryDockNearestStation(kDockRange)
                       && !world.trySalvageNearest(game::SpaceWorld::kSalvageRange)) {
                SOL_LOG_INFO("Nothing to dock with or salvage within %.0f km",
                             kDockRange / 1000.0);
            }
        }
        previousG = gDown;

        // Scan pulse (R): reveals contacts within the fitted scanner's range.
        const bool rDown = gameplayKey(sol::platform::Key::R);
        if (rDown && !previousR) {
            if (world.pulseScan() < 0) {
                SOL_LOG_INFO("Scanner still charging (%.0f%%)",
                             static_cast<double>(world.pulseCharge() * 100.0f));
            }
        }
        previousR = rDown;

        // Map (M) opens from flight or from a station and closes from itself.
        const bool mDown = (inFlight || docked || onMap)
                           && window.isKeyDown(sol::platform::Key::M)
                           && !devUi.wantsMouseCapture();
        if (mDown && !previousM) {
            if (onMap) {
                state = world.isDocked() ? game::GameState::Docked : game::GameState::Flying;
            } else {
                state = game::GameState::Map;
                window.setCursorLocked(false);
            }
        }
        previousM = mDown;

        // Power triage (decisions/003): 1/2/3 pip WEP/ENG/SYS, 4 balances.
        const bool pip1 = gameplayKey(sol::platform::Key::Num1);
        const bool pip2 = gameplayKey(sol::platform::Key::Num2);
        const bool pip3 = gameplayKey(sol::platform::Key::Num3);
        const bool pip4 = gameplayKey(sol::platform::Key::Num4);
        if (pip1 && !previousPip1) world.playerAddPip(sol::sim::PowerSystem::Weapons);
        if (pip2 && !previousPip2) world.playerAddPip(sol::sim::PowerSystem::Engines);
        if (pip3 && !previousPip3) world.playerAddPip(sol::sim::PowerSystem::Shields);
        if (pip4 && !previousPip4) world.playerBalancePips();
        previousPip1 = pip1;
        previousPip2 = pip2;
        previousPip3 = pip3;
        previousPip4 = pip4;

        // In free-cam mode the mouse/keys drive the debug camera, not the ship.
        if (!inFlight) {
            // The mapper reads the window directly, so it is skipped entirely
            // rather than fed neutral input - otherwise Tab would toggle
            // cruise while the player is tabbing through a menu. It also owns
            // the cursor lock, so release it here or a screen opened mid-turn
            // inherits a captured mouse.
            world.setShipInput({});
            window.setCursorLocked(false);
        } else if (cameraMode == game::CameraMode::Free) {
            freeCamera.update(window, deltaSeconds);
            world.setShipInput({});
        } else {
            sol::sim::FlightInput input = inputMapper.update(window, deltaSeconds);
            input.trigger = window.isMouseButtonDown(sol::platform::MouseButton::Left) &&
                            !devUi.wantsMouseCapture();
            world.setShipInput(input);
        }

        // A menu stops the clock: no accumulation, so unpausing does not
        // fast-forward the galaxy by however long the player was reading.
        if (simRunning) {
            simLoop.beginFrame(deltaSeconds);
            while (simLoop.shouldTick()) {
                world.tick(simLoop.tickDelta());
                content.tick(simLoop.tickDelta());
            }
        }
        const float simAlpha = simLoop.alpha();

        const game::Transform shipTransform = world.shipRenderTransform(simAlpha);
        game::CameraFrame camera;
        switch (cameraMode) {
        case game::CameraMode::FirstPerson:
            camera = shipCamera.firstPerson(shipTransform);
            break;
        case game::CameraMode::ThirdPerson:
            camera = shipCamera.thirdPerson(shipTransform, deltaSeconds);
            break;
        case game::CameraMode::Free:
            camera = freeCamera.frame();
            break;
        }

        const bool includeShip = cameraMode != game::CameraMode::FirstPerson;
        world.buildRenderInstances(simAlpha, includeShip, renderInstances);
        world.buildParticleInstances(simAlpha, particleInstances);

        sceneInfo.sun = {world.sun().position, world.sun().radius, 0};
        sceneInfo.planets.clear();
        for (std::size_t i = 0; i < world.planets().size(); ++i) {
            const game::CelestialBody& planet = world.planets()[i];
            sceneInfo.planets.push_back({planet.position, planet.radius,
                                         world.currentSystemIndex() * 7u +
                                             static_cast<std::uint32_t>(i)});
        }

        // Debug draw (F3): ship axes, velocity arrow, target ray.
        const bool f3Down = window.isKeyDown(sol::platform::Key::F3);
        if (f3Down && !previousF3) {
            showDebugDraw = !showDebugDraw;
        }
        previousF3 = f3Down;

        const sol::sim::ShipState shipState = world.shipState();
        const game::TargetInfo target = world.currentTargetInfo();
        const sol::core::DVec3 toTargetWorld = target.nav.position - shipState.position;
        const double targetDistance =
            sol::core::clamp(length(toTargetWorld) - target.nav.surfaceRadius, 0.0, 1.0e18);
        const sol::core::DVec3 targetDirection = normalize(toTargetWorld);

        if (showDebugDraw) {
            sol::renderer::DebugDrawRenderer& debugDraw = renderer.debugDraw();
            const sol::core::Vec3 shipRelative =
                (shipTransform.position - camera.position).toVec3();
            debugDraw.axes(shipRelative, shipTransform.orientation, 12.0f);
            const double speed = length(shipState.velocity);
            if (speed > 0.5) {
                const sol::core::Vec3 velocityDirection = toVec3(normalize(shipState.velocity));
                debugDraw.arrow(shipRelative, shipRelative + velocityDirection * 25.0f,
                                {0.3f, 1.0f, 0.4f, 1.0f});
            }
            debugDraw.line(shipRelative, shipRelative + toVec3(targetDirection) * 60.0f,
                           {1.0f, 0.8f, 0.3f, 1.0f});
        }

        // Hardcore death (decisions/007): the save goes with the run.
        if (world.consumeHardcoreDeath()) {
            if (sol::platform::deleteFile(savePath.c_str())) {
                SOL_LOG_WARN("hardcore death: save deleted (%s)", savePath.c_str());
            } else {
                SOL_LOG_ERROR("hardcore death: save delete FAILED (%s)", savePath.c_str());
            }
        }

        // World save/load round trip: F9 saves, F10 loads (edge-triggered).
        const bool f9Down = window.isKeyDown(sol::platform::Key::F9);
        if (f9Down && !previousF9) {
            SOL_LOG_INFO(world.saveTo(savePath.c_str()) ? "world saved to %s"
                                                        : "world save FAILED (%s)",
                         savePath.c_str());
        }
        previousF9 = f9Down;
        const bool f10Down = window.isKeyDown(sol::platform::Key::F10);
        if (f10Down && !previousF10) {
            SOL_LOG_INFO(world.loadFrom(savePath.c_str()) ? "world loaded from %s"
                                                          : "world load FAILED (%s)",
                         savePath.c_str());
        }
        previousF10 = f10Down;

        // Shader hot-reload: automatic on file change, F5 to force.
        const bool f5Down = window.isKeyDown(sol::platform::Key::F5);
        const bool forceReload = f5Down && !previousF5;
        previousF5 = f5Down;
        if (shaderWatcher.poll(now, forceReload)) {
            context.waitIdle();
            if (!renderer.reloadShaders()) {
                SOL_LOG_ERROR("pipeline reload failed; keeping previous shaders");
            }
        }

        // Data-def + Lua script hot-reload (automatic on file change).
        content.poll(now);

        if (deltaSeconds > 0.0f) {
            const float instantFps = 1.0f / deltaSeconds;
            smoothedFps = smoothedFps == 0.0f ? instantFps
                                              : sol::core::lerp(smoothedFps, instantFps, 0.05f);
        }
        sol::ui::OverlayStats stats;
        stats.fps = smoothedFps;
        stats.frameMilliseconds = deltaSeconds * 1000.0f;
        stats.cameraPosition = camera.position;
        stats.cameraSpeed = static_cast<float>(length(shipState.velocity));
        stats.drawCalls = renderer.drawCallCount();
        stats.simTicks = simLoop.tickCount();
        stats.simEntities = world.entityCount();
        stats.simAlpha = simAlpha;

        sol::ui::FlightHud hud;
        hud.active = true;
        hud.speedMetersPerSecond = stats.cameraSpeed;
        hud.assist = world.shipInput().assist;
        hud.boost = world.shipInput().boost;
        hud.cruise = world.shipInput().cruise;
        hud.autopilot = world.autopilotActive();
        switch (cameraMode) {
        case game::CameraMode::FirstPerson: hud.cameraMode = "COCKPIT"; break;
        case game::CameraMode::ThirdPerson: hud.cameraMode = "CHASE"; break;
        case game::CameraMode::Free: hud.cameraMode = "FREECAM"; break;
        }
        hud.targetName = target.nav.name.c_str();
        hud.targetDistanceMeters = targetDistance;
        hud.closingSpeedMetersPerSecond =
            static_cast<float>(dot(shipState.velocity, targetDirection));
        hud.targetDirectionCamera =
            rotate(conjugate(camera.orientation), toVec3(targetDirection));
        hud.tanHalfFovY = std::tan(game::kCameraVerticalFov * 0.5f);
        const sol::sim::PowerState& power = world.playerPower();
        hud.pipsWeapons = power.pips.weapons;
        hud.pipsEngines = power.pips.engines;
        hud.pipsShields = power.pips.shields;
        hud.pipMax = world.powerTuning().maxPerSystem;
        hud.weaponCharge = power.weaponCharge / world.powerTuning().weaponCapacitor;
        const game::ShipDefense& defense = world.playerDefense();
        hud.shieldFore = defense.tuning.shieldStrength > 0.0f
                             ? defense.state.shieldFore / defense.tuning.shieldStrength
                             : 0.0f;
        hud.shieldAft = defense.tuning.shieldStrength > 0.0f
                            ? defense.state.shieldAft / defense.tuning.shieldStrength
                            : 0.0f;
        hud.hull = defense.tuning.hull > 0.0f ? defense.state.hull / defense.tuning.hull : 0.0f;
        hud.damageFlash = world.playerDamageFlash();
        hud.systemName = world.currentSystemName();
        const double gateDistance = world.nearestGateDistance();
        hud.gateInRange = gateDistance >= 0.0 && gateDistance <= kGateActivationRange;
        hud.docked = world.isDocked();
        hud.dockedStationName = world.dockedStationName();
        const double stationDistance = world.nearestStationDistance();
        hud.dockInRange =
            !hud.docked && stationDistance >= 0.0 && stationDistance <= kDockRange;
        hud.targetIsShip = target.isShip;
        hud.targetShieldFore = target.shieldFore;
        hud.targetShieldAft = target.shieldAft;
        hud.targetHull = target.hull;
        hud.targetFaction = target.factionName.c_str();
        hud.targetAttitude = target.attitude;
        // Scanning (Phase 8e): charge, the held scan, and what this system
        // still has waiting to be identified or emptied.
        hud.pulseCharge = world.pulseCharge();
        hud.scanProgress = world.scanProgress();
        hud.scanTarget = world.scanTargetName();
        for (const game::SignalInstance& signal : world.signals()) {
            const std::uint32_t system = world.currentSystemIndex();
            if (!world.survey().signalDiscovered(system, signal.index)) {
                continue;
            }
            if (!world.survey().signalResolved(system, signal.index)) {
                ++hud.contactsUnresolved;
            } else if (!world.survey().signalEmptied(system, signal.index)) {
                ++hud.sitesOpen;
            }
        }
        const double salvageDistance = world.nearestSalvageDistance();
        hud.salvageInRange =
            salvageDistance >= 0.0 && salvageDistance <= game::SpaceWorld::kSalvageRange;
        const std::uint32_t nextHop = world.survey().nextHop();
        if (nextHop < world.galaxy().systems.size()) {
            routeHopName = world.galaxy().systems[nextHop].name;
            hud.routeNextHop = routeHopName.c_str();
        }
        // Prospecting and collection (Phase 8f).
        prospect = world.prospectAhead();
        if (prospect.valid) {
            hud.prospectName = prospect.name.c_str();
            hud.prospectIsWreck = prospect.wreck;
            hud.prospectInRange = prospect.inRange;
            hud.prospectLeft = prospect.unitsLeft;
            hud.prospectTotal = prospect.unitsTotal;
            hud.prospectDistance = prospect.distance;
        }
        hud.collectedUnits = world.lastCollectedUnits();
        hud.collectedName = world.lastCollectedName();
        // Tracked mission line (Phase 8c).
        const sol::sim::MissionSim& missions = world.missionSim();
        if (missions.tracked() < missions.active().size()) {
            const sol::sim::Mission& tracked = missions.active()[missions.tracked()];
            const sol::sim::MissionObjective& objective =
                tracked.objectives[tracked.currentObjective];
            missionHudObjective = objective.text;
            if (objective.kind == sol::sim::ObjectiveKind::Kill) {
                missionHudObjective += " (" + std::to_string(objective.kills) + " left)";
            } else if (objective.kind == sol::sim::ObjectiveKind::Deliver) {
                missionHudObjective +=
                    " (" + std::to_string(static_cast<int>(objective.units)) + " units)";
            }
            hud.missionTitle = tracked.title.c_str();
            hud.missionObjective = missionHudObjective.c_str();
            hud.missionDeadline = tracked.deadline;
        }
        const game::ShipWeapon& playerWeapon = world.playerWeapon();
        if (target.isShip && playerWeapon.kind == game::WeaponKind::Projectile &&
            playerWeapon.projectileSpeed > 1.0f) {
            sol::core::DVec3 leadDirection;
            (void)sol::sim::computeInterceptDirection(
                shipState.position, shipState.velocity, target.nav.position, target.velocity,
                static_cast<double>(playerWeapon.projectileSpeed), leadDirection);
            hud.leadDirectionCamera =
                rotate(conjugate(camera.orientation), toVec3(leadDirection));
            hud.hasLead = true;
        }
        // Provisional docked-station screen: Trade + Phase 8a Outfitting /
        // Shipyard / Crew tabs (engine plan: real game UI is its own item).
        sol::ui::StationPanel stationPanel;
        const bool showStation = world.isDocked();
        if (showStation) {
            const std::uint32_t market = world.dockedMarket();
            tradeRows.clear();
            for (std::uint32_t i = 0;
                 i < static_cast<std::uint32_t>(world.commodityIds().size()); ++i) {
                const sol::assets::CommodityDef* def =
                    content.defs().findCommodity(world.commodityIds()[i].c_str());
                tradeRows.push_back({
                    .name = def != nullptr ? def->name.c_str()
                                           : world.commodityIds()[i].c_str(),
                    .price = world.economy().price(market, i),
                    .stock = world.economy().stock(market, i),
                    .cargo = world.playerCargo(i),
                });
            }
            stationPanel.trade.stationName = world.dockedStationName();
            stationPanel.trade.credits = world.playerCredits();
            stationPanel.trade.cargoUsed = world.playerCargoTotal();
            stationPanel.trade.cargoCapacity = world.playerCargoCapacity();
            stationPanel.trade.rows = tradeRows;
            game::fillStationOutfitting(world, content.defs(), stationText, stationPanel,
                                        moduleRows, weaponRows, crewCatalogRows, crewAboardRows,
                                        shipRows, fleetRows, factionRows);
            game::fillStationMissions(world, stationText, stationPanel, missionOfferRows,
                                      missionJournalRows); // after: shares stationText
            // Survey ledger (Phase 8e): sellable at any station, whole.
            surveyRows.clear();
            for (const sol::sim::SurveyEntry& entry : world.survey().ledger()) {
                const char* kindName = entry.kind == sol::sim::SurveyKind::System ? "system data"
                                       : entry.kind == sol::sim::SurveyKind::Body ? "body scan"
                                       : entry.kind == sol::sim::SurveyKind::Site ? "site survey"
                                                                                  : "full survey";
                std::string detail = kindName;
                detail += entry.region == sol::sim::Region::Core       ? ", core"
                          : entry.region == sol::sim::Region::Frontier ? ", frontier"
                                                                       : ", fringe";
                if (entry.firstDiscovery) {
                    detail += ", uncharted space";
                }
                stationText.push_back(std::move(detail));
                surveyRows.push_back(
                    {.system = world.galaxy().systems[entry.system].name.c_str(),
                     .detail = stationText.back().c_str(),
                     .value = static_cast<float>(entry.value)});
            }
            stationPanel.surveyData = surveyRows;
            stationPanel.surveyValue = world.survey().ledgerValue();

            // Refining (Phase 8f): a service, not a market — only stations
            // whose archetype refines something offer it at all.
            sol::ui::RefinePanel& refinery = stationPanel.refinery;
            refinery.refines = world.dockedStationRefines();
            if (refinery.refines) {
                const std::uint32_t input = world.refineInputCommodity();
                const std::uint32_t output = world.refineOutputCommodity();
                const auto commodityName = [&](std::uint32_t index) {
                    const sol::assets::CommodityDef* def =
                        index < world.commodityIds().size()
                            ? content.defs().findCommodity(world.commodityIds()[index].c_str())
                            : nullptr;
                    return def != nullptr ? def->name.c_str() : "?";
                };
                refinery.inputName = commodityName(input);
                refinery.outputName = commodityName(output);
                refinery.inputHeld = world.playerCargo(input);
                refinery.ratio = world.mining().params().refineRatio;
                refinery.feePerUnit = world.mining().params().refineFeePerUnit;
                refinery.readyUnits = world.refinedReadyHere();
                refinery.waitSeconds = world.refineWaitHere();
                refinery.cargoSpace = world.playerCargoCapacity() - world.playerCargoTotal();
            }
        }
        // The map screen (Phase 8e) reads only what the player knows.
        sol::ui::MapPanel mapPanel;
        if (state == game::GameState::Map) {
            game::fillMapPanel(world, mapText, mapPanel, mapSystemRows, mapLaneRows,
                               mapMarkerRows);
        }
        // --- Custom game UI (Phase 8d), rebuilt every frame ---
        // Everything below works in virtual UI pixels: the layout, the cursor,
        // and the renderer all divide by the same scale, so the setting moves
        // widgets and hit-testing together instead of drifting apart.
        renderer.setUiScale(settings.uiScale);
        const float uiScale = settings.uiScale > 0.0f ? settings.uiScale : 1.0f;
        const sol::core::Vec2 uiSize = {static_cast<float>(swapchain.extent().width) / uiScale,
                                        static_cast<float>(swapchain.extent().height) / uiScale};

        sol::ui::InputState uiInput;
        const sol::core::Vec2 cursor = window.mousePosition();
        uiInput.mousePosition = {cursor.x / uiScale, cursor.y / uiScale};
        uiInput.mouseDown = window.isMouseButtonDown(sol::platform::MouseButton::Left) &&
                            !devUi.wantsMouseCapture();
        uiInput.mousePressed = uiInput.mouseDown && !previousMouseDown;
        uiInput.mouseReleased = !uiInput.mouseDown && previousMouseDown;
        previousMouseDown = uiInput.mouseDown;
        uiInput.scrollDelta = window.wheelDelta();

        // Navigation keys are edge-triggered: holding Tab must not race
        // through every widget on the screen in one frame.
        const auto menuKeyEdge = [&](sol::platform::Key key, bool& previous) {
            const bool down = uiHasKeys && window.isKeyDown(key) && !devUi.wantsMouseCapture();
            const bool edge = down && !previous;
            previous = down;
            return edge;
        };
        const bool tabEdge = menuKeyEdge(sol::platform::Key::Tab, previousNavTab);
        const bool downEdge = menuKeyEdge(sol::platform::Key::Down, previousNavDown);
        const bool upEdge = menuKeyEdge(sol::platform::Key::Up, previousNavUp);
        uiInput.navNext = tabEdge || downEdge;
        uiInput.navPrevious = upEdge;
        uiInput.navActivate = menuKeyEdge(sol::platform::Key::Enter, previousNavActivate) ||
                              menuKeyEdge(sol::platform::Key::Space, previousNavSpace);
        // Sliders step while held, so left/right stay level-triggered - but
        // they stand down for ImGui on the same terms as every other nav key,
        // or a cursor resting on the dev overlay would disable half of them.
        const bool arrowsLive = uiHasKeys && !devUi.wantsMouseCapture();
        uiInput.navLeft = arrowsLive && window.isKeyDown(sol::platform::Key::Left);
        uiInput.navRight = arrowsLive && window.isKeyDown(sol::platform::Key::Right);
        // Only a menu treats Esc as "back out". While docked it opened the
        // pause menu above, and reading it here as well would cancel that menu
        // on the very frame it appeared.
        uiInput.navCancel = escapeEdge && inMenuScreen;

        ui.beginFrame(uiInput, uiSize);
        game::MenuAction menuAction = game::MenuAction::None;
        bool undockRequested = false;
        bool mapClosed = false;
        switch (state) {
        case game::GameState::MainMenu:
            menuAction = game::buildMainMenu(ui, mainMenuState);
            break;
        case game::GameState::Paused:
            menuAction = game::buildPauseMenu(ui, world.hardcore());
            break;
        case game::GameState::Settings:
            menuAction = game::buildSettingsScreen(ui, settings);
            break;
        case game::GameState::Flying:
            // The dev HUD stays up beside this one during the changeover.
            game::buildFlightUi(ui.drawList(), renderer.uiFont(), ui.screenSize(), hud);
            break;
        case game::GameState::Docked:
            // Docked, the station screen owns the view; the flight readout has
            // nothing to say about a parked ship.
            undockRequested = game::buildStationScreen(ui, stationPanel, stationScreen);
            break;
        case game::GameState::Map:
            // The map owns the view (it dims what is behind it), but the ship
            // is still flying under it - this state does not stop the clock.
            mapClosed = game::buildMapScreen(ui, mapPanel, mapScreen);
            break;
        }
        ui.endFrame();
        renderer.setUiDrawList(&ui.drawList());

        switch (menuAction) {
        case game::MenuAction::None:
            break;
        case game::MenuAction::NewGame:
            world.setHardcore(mainMenuState.hardcore);
            if (world.hardcore()) {
                SOL_LOG_INFO("HARDCORE run: death deletes the save");
            }
            state = game::GameState::Flying;
            break;
        case game::MenuAction::ContinueGame:
            SOL_LOG_INFO(world.loadFrom(savePath.c_str()) ? "world loaded from %s"
                                                          : "world load FAILED (%s)",
                         savePath.c_str());
            state = game::GameState::Flying;
            break;
        case game::MenuAction::Resume:
            state = game::GameState::Flying;
            break;
        case game::MenuAction::SaveGame:
            SOL_LOG_INFO(world.saveTo(savePath.c_str()) ? "world saved to %s"
                                                        : "world save FAILED (%s)",
                         savePath.c_str());
            mainMenuState.hasSave = true;
            state = game::GameState::Flying;
            break;
        case game::MenuAction::LoadGame:
            SOL_LOG_INFO(world.loadFrom(savePath.c_str()) ? "world loaded from %s"
                                                          : "world load FAILED (%s)",
                         savePath.c_str());
            state = game::GameState::Flying;
            break;
        case game::MenuAction::OpenSettings:
            settingsReturnState = state;
            state = game::GameState::Settings;
            break;
        case game::MenuAction::CloseSettings:
            // Settings persist on the way out, so a crash mid-session cannot
            // lose them and nothing has to remember to save later.
            if (!settings.save(settingsPath.c_str())) {
                SOL_LOG_WARN("could not write %s", settingsPath.c_str());
            }
            state = settingsReturnState;
            break;
        case game::MenuAction::QuitGame:
            quitRequested = true;
            break;
        }

        // Docking is driven by the world, so the state follows it rather than
        // the menus trying to track it. This tests the state as it stands now,
        // not `inFlight` from the top of the frame: Esc may have opened the
        // pause menu since, and re-deriving from a stale flag would slam it
        // shut again on the same frame.
        if (undockRequested) {
            (void)world.undock();
        }
        // Map clicks act on the world; engaging the autopilot also drops the
        // map, since the point of that button is to go there.
        if (state == game::GameState::Map) {
            if (game::executeMapAction(world, mapPanel.action)) {
                mapClosed = true;
            }
            if (mapClosed) {
                state = world.isDocked() ? game::GameState::Docked : game::GameState::Flying;
            }
        }
        if (state == game::GameState::Flying || state == game::GameState::Docked) {
            state = world.isDocked() ? game::GameState::Docked : game::GameState::Flying;
        }

        // Dev overlay and console only: every player-facing screen is on the
        // custom stack now.
        devUi.beginFrame(stats);
        if (showStation && stationPanel.trade.action.row >= 0) {
            const std::uint32_t commodity =
                static_cast<std::uint32_t>(stationPanel.trade.action.row);
            if (stationPanel.trade.action.isBuy) {
                (void)world.playerBuy(commodity, stationPanel.trade.action.units);
            } else {
                (void)world.playerSell(commodity, stationPanel.trade.action.units);
            }
        }
        if (showStation) {
            game::executeStationAction(world, stationPanel.action);
        }

        bool needRecreate = window.consumeResize();
        if (needRecreate) {
            devUi.discardFrame();
        }
        if (!needRecreate) {
            switch (renderer.drawFrame(camera, renderInstances, particleInstances, sceneInfo)) {
            case game::SceneRenderer::DrawResult::Success:
                ++frameCount;
                break;
            case game::SceneRenderer::DrawResult::NeedSwapchainRecreate:
                needRecreate = true;
                devUi.discardFrame();
                break;
            case game::SceneRenderer::DrawResult::Failure:
                failed = true;
                break;
            }
        }
        if (failed) {
            break;
        }

        if (needRecreate) {
            context.waitIdle();
            if (swapchain.recreate(window.width(), window.height())) {
                if (!renderer.onSwapchainRecreated()) {
                    failed = true;
                    break;
                }
            }
            // A failed recreate (zero extent) is retried once the window is restored.
        }

        if (maxFrames > 0 && frameCount >= maxFrames) {
            break;
        }
    }

    context.waitIdle();
    devUi.shutdown();
    renderer.shutdown();
    swapchain.destroy();
    context.shutdown();
    window.destroy();

    const std::uint64_t validationMessages = sol::rhi::Context::validationMessageCount();
    if (failed || validationMessages > 0) {
        SOL_LOG_ERROR("Exited with failure=%d, %llu validation message(s)",
                      failed ? 1 : 0,
                      static_cast<unsigned long long>(validationMessages));
        return EXIT_FAILURE;
    }

    SOL_LOG_INFO("Clean shutdown after %llu frame(s), 0 validation messages",
                 static_cast<unsigned long long>(frameCount));
    return EXIT_SUCCESS;
}
