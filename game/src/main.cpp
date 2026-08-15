#include "content.hpp"
#include "fly_camera.hpp"
#include "scene_renderer.hpp"
#include "shader_watcher.hpp"
#include "ship_camera.hpp"
#include "space_world.hpp"

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

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
        input.angular.x = m_stick.x;
        input.angular.y = m_stick.y;
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

    static constexpr float kStickSensitivity = 0.0035f;
    static constexpr float kRecenterRate = 2.5f;

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
    windowDesc.title = "Sol";
    if (!window.create(windowDesc)) {
        return EXIT_FAILURE;
    }

    sol::rhi::Context context;
    sol::rhi::ContextDesc contextDesc = {};
    contextDesc.appName = "Sol";
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
    bool previousPip1 = false;
    bool previousPip2 = false;
    bool previousPip3 = false;
    bool previousPip4 = false;
    bool showDebugDraw = false;

    SOL_LOG_INFO("Entering frame loop (%ux%u). RMB+mouse steer, WASD/QE/Space/Ctrl thrust, "
                 "Shift boost, Tab cruise, X assist, V camera, T target, ESC quits.",
                 window.width(), window.height());

    double lastFrameTime = sol::platform::timeSeconds();
    std::uint64_t frameCount = 0;
    bool failed = false;

    while (true) {
        window.pumpEvents();
        if (window.shouldClose() || window.isKeyDown(sol::platform::Key::Escape)) {
            break;
        }
        if (window.isMinimized()) {
            sol::platform::sleepMilliseconds(10);
            continue;
        }

        const double now = sol::platform::timeSeconds();
        const float deltaSeconds = sol::core::clamp(static_cast<float>(now - lastFrameTime), 0.0f, 0.1f);
        lastFrameTime = now;

        // Camera mode cycle (V): first person -> chase -> free.
        const bool vDown = window.isKeyDown(sol::platform::Key::V);
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

        const bool tDown = window.isKeyDown(sol::platform::Key::T);
        if (tDown && !previousT) {
            world.cycleTarget();
            SOL_LOG_INFO("Target: %s", world.currentTargetInfo().nav.name.c_str());
        }
        previousT = tDown;

        // Jump through the nearest in-range gate (decisions/004 gate travel).
        const bool jDown = window.isKeyDown(sol::platform::Key::J);
        if (jDown && !previousJ) {
            if (world.jumpNearestGate(kGateActivationRange)) {
                SOL_LOG_INFO("Arrived in '%s'", world.currentSystemName());
            } else {
                SOL_LOG_INFO("No gate within %.0f km", kGateActivationRange / 1000.0);
            }
        }
        previousJ = jDown;

        // Dock/undock at the nearest station (G toggles).
        const bool gDown = window.isKeyDown(sol::platform::Key::G);
        if (gDown && !previousG) {
            if (world.isDocked()) {
                (void)world.undock();
            } else if (!world.tryDockNearestStation(kDockRange)) {
                SOL_LOG_INFO("No station within %.0f km", kDockRange / 1000.0);
            }
        }
        previousG = gDown;

        // Power triage (decisions/003): 1/2/3 pip WEP/ENG/SYS, 4 balances.
        const bool pip1 = window.isKeyDown(sol::platform::Key::Num1);
        const bool pip2 = window.isKeyDown(sol::platform::Key::Num2);
        const bool pip3 = window.isKeyDown(sol::platform::Key::Num3);
        const bool pip4 = window.isKeyDown(sol::platform::Key::Num4);
        if (pip1 && !previousPip1) world.playerAddPip(sol::sim::PowerSystem::Weapons);
        if (pip2 && !previousPip2) world.playerAddPip(sol::sim::PowerSystem::Engines);
        if (pip3 && !previousPip3) world.playerAddPip(sol::sim::PowerSystem::Shields);
        if (pip4 && !previousPip4) world.playerBalancePips();
        previousPip1 = pip1;
        previousPip2 = pip2;
        previousPip3 = pip3;
        previousPip4 = pip4;

        // In free-cam mode the mouse/keys drive the debug camera, not the ship.
        if (cameraMode == game::CameraMode::Free) {
            freeCamera.update(window, deltaSeconds);
            world.setShipInput({});
        } else {
            sol::sim::FlightInput input = inputMapper.update(window, deltaSeconds);
            input.trigger = window.isMouseButtonDown(sol::platform::MouseButton::Left) &&
                            !devUi.wantsMouseCapture();
            world.setShipInput(input);
        }

        simLoop.beginFrame(deltaSeconds);
        while (simLoop.shouldTick()) {
            world.tick(simLoop.tickDelta());
            content.tick(simLoop.tickDelta());
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
        // Provisional trade screen while docked (engine plan: real game UI
        // replaces the ImGui screens in Phase 8).
        sol::ui::TradePanel tradePanel;
        const bool showTrade = world.isDocked();
        if (showTrade) {
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
            tradePanel.stationName = world.dockedStationName();
            tradePanel.credits = world.playerCredits();
            tradePanel.cargoUsed = world.playerCargoTotal();
            tradePanel.cargoCapacity = world.playerCargoCapacity();
            tradePanel.rows = tradeRows;
        }
        devUi.beginFrame(stats, hud, showTrade ? &tradePanel : nullptr);
        if (showTrade && tradePanel.action.row >= 0) {
            const std::uint32_t commodity =
                static_cast<std::uint32_t>(tradePanel.action.row);
            if (tradePanel.action.isBuy) {
                (void)world.playerBuy(commodity, tradePanel.action.units);
            } else {
                (void)world.playerSell(commodity, tradePanel.action.units);
            }
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
