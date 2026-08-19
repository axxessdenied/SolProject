#include "content.hpp"
#include "fly_camera.hpp"
#include "game_audio.hpp"
#include "game_ui.hpp"
#include "input_actions.hpp"
#include "map_screen.hpp"
#include "map_ui.hpp"
#include "menu_screens.hpp"
#include "scene_renderer.hpp"
#include "shader_watcher.hpp"
#include "ship_camera.hpp"
#include "ship_screen.hpp"
#include "ship_ui.hpp"
#include "space_world.hpp"
#include "station_screen.hpp"
#include "station_ui.hpp"
#include "target_pick.hpp"

#include "sol/core/log.hpp"
#include "sol/core/profiler.hpp"
#include "sol/core/version.hpp"
#include "sol/sim/fixed_loop.hpp"
#include "sol/sim/flight.hpp"
#include "sol/sim/power.hpp"
#include "sol/sim/weapons.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/platform/input_bindings.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/cockpit_frame.hpp"
#include "sol/ui/dev_ui.hpp"
#include "sol/ui/radar_projection.hpp"
#include "sol/ui/screens.hpp"

#include <algorithm>
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

// Latches per-frame input state into a flight-model input. Rotation combines a
// self-centering virtual stick fed by the mouse (hold the Mouse Steering
// binding) with full-deflection key axes. Every axis is an Action since Phase
// 8k, so the layout here is a default rather than a rule; the edges for the
// assist and cruise toggles come from the binding table, which is why this
// class no longer carries a latch per key.
class ShipInputMapper
{
public:
    [[nodiscard]] sol::sim::FlightInput update(sol::platform::Window& window, float deltaSeconds,
                                               const sol::platform::BindingTable& bindings,
                                               float mouseSensitivity, bool invertPitch)
    {
        // Free-look owns the mouse while it is held (Phase 8m): the player is
        // turning their head, not the ship. Both modes want the cursor locked,
        // and this class is the one owner of that lock, so it decides here
        // rather than leaving main.cpp to overwrite the flag later in the frame.
        const bool freeLooking = game::held(bindings, game::Action::FreeLook);
        const bool steering = game::held(bindings, game::Action::LookAround) && !freeLooking;
        window.setCursorLocked(steering || freeLooking);
        if (steering) {
            const sol::core::Vec2 delta = window.mouseDelta();
            // Phase 8k: the sensitivity slider has been in the settings menu
            // since 8d reading nothing. Pitch inversion applies to the mouse
            // only - a player who wants the key axes the other way round now
            // swaps two bindings.
            const float scale = kStickSensitivity * mouseSensitivity;
            const float pitch = invertPitch ? -delta.y : delta.y;
            m_stick.x -= pitch * scale; // mouse up = nose up
            m_stick.y -= delta.x * scale; // mouse left = yaw left
        }
        // Self-centering, Elite-style relative mouse.
        const float recenter = std::exp(-kRecenterRate * deltaSeconds);
        m_stick.x = sol::core::clamp(m_stick.x * recenter, -1.0f, 1.0f);
        m_stick.y = sol::core::clamp(m_stick.y * recenter, -1.0f, 1.0f);

        using game::Action;
        const auto axis = [&bindings](Action positive, Action negative) {
            return (game::held(bindings, positive) ? 1.0f : 0.0f) -
                   (game::held(bindings, negative) ? 1.0f : 0.0f);
        };

        sol::sim::FlightInput input;
        input.angular.x = applyDeadZone(m_stick.x) + axis(Action::PitchUp, Action::PitchDown);
        input.angular.y = applyDeadZone(m_stick.y) + axis(Action::YawLeft, Action::YawRight);
        input.angular.z = axis(Action::RollRight, Action::RollLeft);

        input.linear.z = axis(Action::ThrustReverse, Action::ThrustForward); // -Z is the main drive
        input.linear.x = axis(Action::StrafeRight, Action::StrafeLeft);
        input.linear.y = axis(Action::StrafeUp, Action::StrafeDown);

        input.boost = game::held(bindings, Action::Boost);
        if (game::pressed(bindings, Action::ToggleAssist)) {
            m_assist = !m_assist;
        }
        if (game::pressed(bindings, Action::ToggleCruise)) {
            m_cruise = !m_cruise;
        }
        input.assist = m_assist;
        input.cruise = m_cruise;
        return input;
    }

private:
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

    const std::string executableDir = sol::platform::executableDirectory();
    const std::string shaderDirectory = executableDir + "shaders/";
    const std::string cookedDirectory = executableDir + "cooked/";
    const std::string savePath = executableDir + "world.sav";

    // Settings load before the swapchain exists, because V-Sync is a swapchain
    // present mode (Phase 8k) and creating it the wrong way round would mean
    // rebuilding it on the first frame of every run.
    const std::string settingsPath = executableDir + "settings.toml";
    game::Settings settings;
    (void)settings.load(settingsPath.c_str()); // absent is normal on a first run

    sol::rhi::Swapchain swapchain;
    if (!swapchain.create(context, window.width(), window.height(), settings.vsync)) {
        return EXIT_FAILURE;
    }

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
    game::ShipScreenState shipScreen;       // ship readout scroll
    // Refilled each frame and kept alive across the HUD build, which only
    // carries a span into it.
    std::vector<sol::ui::RadarContact> radarContacts;
    std::vector<sol::ui::CommsLine> commsLines; // same lifetime rule (Phase 8r)
    mainMenuState.hasSave = sol::platform::fileModificationTime(savePath.c_str()) != 0;

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
    // The model catalog (Phase 9). It cannot go in the renderer's initialize
    // because the pipelines come up before the defs are read, and a missing
    // mesh is a hard failure exactly as the hardcoded loads used to be.
    if (!renderer.loadModels(content.defs().models(), cookedDirectory.c_str())) {
        return EXIT_FAILURE;
    }
    // Resolved once: the cockpit is the one drawable the game layer pushes
    // itself rather than reading off a RenderShape.
    const game::ModelId cockpitModel =
        static_cast<game::ModelId>(content.defs().modelIndex("cockpit"));

    devUi.setCommandHandler(&consoleCommandHandler, &content);
    // The console edits the same binding table the Controls screen does, so a
    // rebind can be driven and asserted on without clicking through the list.
    content.setBindings(&settings.bindings);

    // Audio (Phase 8t). No output device is not a failure: the game runs
    // silently and every cue site is guarded, so this return value is logged
    // rather than acted on.
    game::GameAudio audio;
    if (!audio.initialize(content.defs(), cookedDirectory)) {
        SOL_LOG_WARN("audio: running without sound");
    }
    audio.setVolumes(settings.masterVolume, settings.effectsVolume);
    world.setAudio(&audio);
    content.setAudio(&audio);
    ShipInputMapper inputMapper;
    game::ShipCamera shipCamera;
    game::FlyCamera freeCamera;
    // The cockpit is where the game starts (Phase 8m): it is the view the whole
    // HUD is now mounted in, and a player who never presses the camera key
    // should be sitting in it.
    game::CameraMode cameraMode = game::CameraMode::Cockpit;
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
    // Trade rows get their own buffer: fillStationOutfitting clears
    // stationText, and it runs after the trade rows are built.
    std::deque<std::string> tradeText;
    // Commodity display names for the map's trade overlay. The roster is
    // fixed for the run, so this is built once rather than every frame.
    std::vector<const char*> commodityNames;
    for (const std::string& id : world.commodityIds()) {
        const sol::assets::CommodityDef* def = content.defs().findCommodity(id.c_str());
        commodityNames.push_back(def != nullptr ? def->name.c_str() : id.c_str());
    }
    std::deque<std::string> mapText;     // same, for the map screen
    std::deque<std::string> shipText;    // same, for the ship readout
    std::vector<sol::ui::InfoRow> shipFlightRows;
    std::vector<sol::ui::InfoRow> shipDefenceRows;
    std::vector<sol::ui::InfoRow> shipUtilityRows;
    std::vector<sol::ui::InfoRow> shipFittedRows;
    std::vector<sol::ui::InfoRow> shipCargoRows;
    SOL_LOG_INFO("Space world: %u entities in '%s' (%zu-system galaxy).", world.entityCount(),
                 world.currentSystemName(), world.galaxy().systems.size());

    float smoothedFps = 0.0f;
    // Dev tooling keeps its own latches: F3/F5/F9/F10 are reserved chords that
    // never reach the binding table, because they are tooling rather than
    // controls (Phase 8k).
    bool previousF3 = false;
    bool previousF5 = false;
    bool previousF9 = false;
    bool previousF10 = false;
    bool previousEscape = false;
    bool previousNavTab = false;
    bool previousNavUp = false;
    bool previousNavDown = false;
    bool previousNavActivate = false;
    bool previousNavSpace = false;
    // Text-editing key edges (Phase 8h); Left/Right are read twice, level for
    // sliders and edge for the caret, which is why they need their own latch.
    bool previousEditLeft = false;
    bool previousEditRight = false;
    bool previousEditHome = false;
    bool previousEditEnd = false;
    bool previousEditBackspace = false;
    bool previousEditDelete = false;
    bool previousEditSubmit = false;
    // The bookmark naming prompt (Phase 8h), open across frames while the
    // player types. Its position is latched when B is pressed, so drifting
    // while naming does not move where the bookmark lands.
    sol::ui::BookmarkPrompt bookmarkPrompt;
    sol::core::DVec3 bookmarkPosition;
    std::string bookmarkWhere; // backs prompt.whereSummary across frames
    bool previousMouseDown = false;
    bool quitRequested = false;
    bool showDebugDraw = false;
    // Every gameplay chord's up/down state, sampled once per frame and handed
    // to the binding table, which owns the press edges that used to be a
    // `previousX` bool per key (Phase 8k).
    sol::platform::InputSnapshot inputSnapshot;
    game::ControlsScreenState controlsScreen;
    bool previousVsync = settings.vsync;

    SOL_LOG_INFO("Entering frame loop (%ux%u). Controls are rebindable in Settings; the shipped "
                 "layout is RMB+mouse steer, MMB fire, LMB select, WASD/QE/Space/Ctrl thrust, "
                 "Shift boost, Tab cruise, X assist, F autopilot, V camera, T target, ESC quits.",
                 window.width(), window.height());

    double lastFrameTime = sol::platform::timeSeconds();
    std::uint64_t frameCount = 0;
    bool failed = false;

    // On for the whole run: this is dev tooling on the same side of the line
    // as the overlay and the console (engine plan 2.9), and ~40 clock reads a
    // frame is not a cost worth a toggle. The class defaults to off so tests
    // and any other client start silent.
    sol::core::Profiler& profiler = sol::core::frameProfiler();
    profiler.setEnabled(true);

    while (true) {
        // Publishes the frame just finished. Before pumpEvents so the event
        // pump is inside the frame it belongs to rather than straddling two.
        profiler.beginFrame();
        // Phase 8o: GPU timings for frames the device has finished with, put
        // in here and nowhere else. The profiler fixes a zone's parent the
        // first time it sees it, so publishing from inside any CPU zone would
        // graft the whole gpu.* tree under it permanently.
        renderer.publishGpuTimings();
        {
            SOL_PROFILE_ZONE("input.events");
            window.pumpEvents();
        }
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
        const bool onShipInfo = state == game::GameState::ShipInfo;
        // The map is a screen, not a pause: the economy, the factions, and
        // whatever is shooting at you all keep running while it is open. The
        // ship readout is the same kind of screen.
        //
        // A jump IS a pause (Phase 8v), for the same reason a menu is: between
        // systems the ship is in neither, so there is nobody for the sim to
        // resolve it against. It is a ride, not a piloting segment.
        const bool jumping = world.jumpActive();
        const bool simRunning = (inFlight || docked || onMap || onShipInfo) && !jumping;
        // A text field open over the flight view owns the keyboard, on exactly
        // the terms a menu does (Phase 8h): the letters are a name being typed,
        // not thrust and target commands. Without this "Rich Rock" flies the
        // ship, and Enter and Backspace never reach the field at all.
        const bool typing = bookmarkPrompt.open;
        const bool uiHasKeys = !inFlight || typing;       // menus, station, map
        const bool inMenuScreen = uiHasKeys && !docked;   // where Esc means "back out"

        // Bindings (Phase 8k). Sampled once, here, so every action this frame
        // reads one consistent picture of the keyboard and mouse - and so the
        // press edges live in the table rather than in a `previousX` bool per
        // key. Sampling before the dev-UI hook is the 8h rule that keeps a key
        // ImGui swallowed from latching down forever.
        inputSnapshot.sample(window);
        settings.bindings.beginFrame(inputSnapshot);
        const sol::platform::BindingTable& binds = settings.bindings;

        // A gameplay action only counts in the cockpit, and never while a text
        // field is open over it: the letters are a name being typed.
        const bool gameplayLive = inFlight && !typing && !jumping;
        const auto gameplayPressed = [&](game::Action action) {
            return gameplayLive && game::pressed(binds, action);
        };

        // Esc opens the pause menu; the menus handle backing out themselves.
        const bool escapeDown = window.isKeyDown(sol::platform::Key::Escape);
        const bool escapeEdge = escapeDown && !previousEscape;
        previousEscape = escapeDown;
        // The map handles Esc itself (it closes), so it is excluded here even
        // though its clock is running - and so does an open text field, where
        // Esc means "abandon what I was typing" rather than "pause".
        if (escapeEdge && (inFlight || docked) && !typing) {
            state = game::GameState::Paused;
            window.setCursorLocked(false);
        }

        // Camera mode cycle: cockpit -> chase -> free.
        if (gameplayPressed(game::Action::CycleCamera)) {
            switch (cameraMode) {
            case game::CameraMode::Cockpit:
                cameraMode = game::CameraMode::ThirdPerson;
                shipCamera.snapTo(world.shipRenderTransform(simLoop.alpha()));
                // Leaving the seat takes the head with it: coming back should
                // find the eye down the nose rather than wherever it was left.
                shipCamera.snapLookAhead();
                break;
            case game::CameraMode::ThirdPerson:
                cameraMode = game::CameraMode::Free;
                freeCamera.setPosition(
                    shipCamera.thirdPerson(world.shipRenderTransform(simLoop.alpha()), 0.0f).position);
                break;
            case game::CameraMode::Free:
                cameraMode = game::CameraMode::Cockpit;
                break;
            }
        }

        // The bookmark key writes down where the ship is (Phase 8h). The
        // position is latched now rather than read on accept, so drifting
        // while typing the name does not move where the bookmark ends up.
        //
        // Opened on RELEASE, not press, and the character the key produces is
        // withheld from the UI for as long as the key is down and on the frame
        // it comes up (see bookmarkKeyEcho below). Without that the field
        // receives the very "b" that opened it — which it did, and neither
        // half of the guard alone was enough: the character does not reliably
        // land on the same frame as the key event that generated it.
        const bool bHeld = gameplayLive && game::held(binds, game::Action::Bookmark);
        const bool bReleased = gameplayLive && game::released(binds, game::Action::Bookmark);
        if (bReleased && !bookmarkPrompt.open) {
            bookmarkPosition = world.shipState().position;
            bookmarkPrompt.open = true;
            bookmarkPrompt.full =
                world.survey().bookmarkCountIn(world.currentSystemIndex())
                >= world.survey().params().maxBookmarksPerSystem;
            bookmarkPrompt.name = world.suggestBookmarkName(bookmarkPosition);
            bookmarkWhere = std::string(world.currentSystemName()) + ", " + bookmarkPrompt.name;
            bookmarkPrompt.whereSummary = bookmarkWhere.c_str();
            bookmarkPrompt.focusRequested = true;
            bookmarkPrompt.nameIsSuggestion = true;
        }
        // True while the opening keystroke could still be echoing as text.
        const bool bookmarkKeyEcho = bHeld || bReleased;

        // One key walks the nav points, another walks the ships (Phase 8h):
        // two questions, two bindings, one selection.
        if (gameplayPressed(game::Action::CycleNavTarget)) {
            world.cycleNavTarget();
            SOL_LOG_INFO("Target: %s", world.currentTargetInfo().nav.name.c_str());
        }

        if (gameplayPressed(game::Action::CycleContact)) {
            world.cycleContact();
            const game::TargetInfo contact = world.currentTargetInfo();
            if (contact.isShip) {
                SOL_LOG_INFO("Contact: %s [%s]", contact.nav.name.c_str(),
                             contact.attitude[0] != '\0' ? contact.attitude : "unaffiliated");
            } else {
                SOL_LOG_INFO("No contacts in this system");
            }
        }

        // Selects the tracked mission's destination outright (Phase 8i). Not a
        // cycle: the whole point of the item is that the player never has to
        // hunt for where they were sent, and hunting through twenty nav slots
        // to find it is the same complaint one level down.
        if (gameplayPressed(game::Action::SelectObjective)) {
            if (world.selectObjective()) {
                SOL_LOG_INFO("Objective: %s", world.currentTargetInfo().nav.name.c_str());
            } else {
                const std::string where = world.objectiveDestinationText();
                // Honest about which of the two "no" answers this is: nothing
                // tracked at all, or tracked but not a place in this system.
                SOL_LOG_INFO("No objective marker here%s%s",
                             where.empty() ? "" : " - objective is at ", where.c_str());
            }
        }

        // Jumps straight back to the nearest hostile (Phase 8i). The contact
        // cycle's first press already lands there from a standing start, but
        // mid-cycle it keeps walking, and this is the way back.
        if (gameplayPressed(game::Action::NearestHostile)) {
            if (world.selectNearestHostile()) {
                const game::TargetInfo hostile = world.currentTargetInfo();
                SOL_LOG_INFO("Nearest hostile: %s [%s]", hostile.nav.name.c_str(),
                             hostile.attitude[0] != '\0' ? hostile.attitude : "unaffiliated");
            } else {
                SOL_LOG_INFO("Nothing hostile in this system");
            }
        }

        // Gate travel has no key since Phase 8v (decisions/004 still governs
        // what a gate IS): you fly through the frame, SpaceWorld::tickGateCrossing
        // catches the crossing, and the arrival is logged when the tunnel swaps.

        // Autopilot to the selected target (toggles; manual input cancels).
        if (gameplayPressed(game::Action::Autopilot)) {
            if (world.autopilotActive()) {
                world.disengageAutopilot();
                SOL_LOG_INFO("Autopilot: disengaged");
            } else if (!world.engageAutopilot()) {
                SOL_LOG_INFO("Autopilot: no target (or docked)");
            }
        }

        // Dock/undock at the nearest station (toggles). Undocking is the one
        // gameplay action the station screen leaves live, beside its own
        // button - so this one reads `inFlight || docked` rather than going
        // through gameplayPressed. It is also the salvage action (Phase 8e):
        // one interact binding, and a station in range wins over a wreck.
        if ((inFlight || docked) && game::pressed(binds, game::Action::DockSalvage)) {
            // The precedence ladder (Phase 8r). Rungs 1-3 are exactly what
            // shipped before this item, so nothing that worked moves: a wreck
            // five kilometres from a station still salvages on this key. Rung
            // 4 is the only addition and it fires where nothing happened
            // before, which is what makes "hail, then fly in" a sequence
            // instead of a formality.
            if (world.isDocked()) {
                (void)world.undock();
            } else if (world.hasClearance()) {
                // Already cleared: the key means "cancel", because the one
                // thing a cleared pilot cannot otherwise do is change their
                // mind, and a clearance holds a berth for three minutes.
                world.clearClearance("Approach cancelled.");
            } else if (world.nearestStationDistance() >= 0.0
                       && world.nearestStationDistance() <= kDockRange) {
                (void)world.requestDocking();
            } else if (!world.trySalvageNearest(game::SpaceWorld::kSalvageRange)) {
                if (!world.requestDocking()) {
                    SOL_LOG_INFO("Nothing to hail or salvage within %.0f km",
                                 game::SpaceWorld::kDockRequestRange / 1000.0);
                }
            }
        }

        // Hail the selected ship (Phase 8s). Its own verb rather than a fifth
        // rung on the ladder above: that key already means five things, and a
        // ship selected while parked near a station is exactly the case where
        // it would have to guess which one was meant.
        if (gameplayPressed(game::Action::HailTarget)) {
            (void)world.hailTarget(); // says why on the comms panel when it can't
        }

        // Scan pulse: reveals contacts within the fitted scanner's range.
        if (gameplayPressed(game::Action::ScanPulse)) {
            if (world.pulseScan() < 0) {
                SOL_LOG_INFO("Scanner still charging (%.0f%%)",
                             static_cast<double>(world.pulseCharge() * 100.0f));
            }
        }

        // The map opens from flight or from a station and closes from itself.
        if ((inFlight || docked || onMap) && !typing && !devUi.wantsMouseCapture()
            && game::pressed(binds, game::Action::OpenMap)) {
            if (onMap) {
                state = world.isDocked() ? game::GameState::Docked : game::GameState::Flying;
            } else {
                state = game::GameState::Map;
                window.setCursorLocked(false);
            }
        }

        // Ship readout, on the same terms as the map: opens from flight or
        // from a station, closes from itself, and does not stop the clock.
        // Suppressed while a text field is open, or "i" in a bookmark name
        // would leave the cockpit.
        if ((inFlight || docked || onShipInfo) && !typing && !devUi.wantsMouseCapture()
            && game::pressed(binds, game::Action::OpenShipInfo)) {
            if (onShipInfo) {
                state = world.isDocked() ? game::GameState::Docked : game::GameState::Flying;
            } else {
                state = game::GameState::ShipInfo;
                window.setCursorLocked(false);
            }
        }

        // Power triage (decisions/003): three pip bindings, one that balances.
        if (gameplayPressed(game::Action::PipWeapons)) {
            world.playerAddPip(sol::sim::PowerSystem::Weapons);
        }
        if (gameplayPressed(game::Action::PipEngines)) {
            world.playerAddPip(sol::sim::PowerSystem::Engines);
        }
        if (gameplayPressed(game::Action::PipShields)) {
            world.playerAddPip(sol::sim::PowerSystem::Shields);
        }
        if (gameplayPressed(game::Action::PipBalance)) {
            world.playerBalancePips();
        }

        // In free-cam mode the mouse/keys drive the debug camera, not the ship.
        if (!inFlight || typing) {
            // The mapper reads the window directly, so it is skipped entirely
            // rather than fed neutral input - otherwise Tab would toggle
            // cruise while the player is tabbing through a menu, and typing a
            // bookmark name would fly the ship. It also owns the cursor lock,
            // so release it here or a screen opened mid-turn inherits a
            // captured mouse.
            world.setShipInput({});
            window.setCursorLocked(false);
        } else if (cameraMode == game::CameraMode::Free) {
            freeCamera.update(window, deltaSeconds);
            world.setShipInput({});
        } else {
            sol::sim::FlightInput input = inputMapper.update(
                window, deltaSeconds, binds, settings.mouseSensitivity, settings.invertPitch);
            // Fire defaults to MIDDLE mouse since Phase 8j, which is what frees
            // left mouse to select - and since 8k it is a binding like any
            // other. Mining moves with it: 8f deliberately made the mining beam
            // the fire button and that ruling stands.
            input.trigger = game::held(binds, game::Action::Fire) && !devUi.wantsMouseCapture();
            world.setShipInput(input);
        }

        // A menu stops the clock: no accumulation, so unpausing does not
        // fast-forward the galaxy by however long the player was reading.
        if (simRunning) {
            // The counter is the tick count, not a size: the fixed loop runs
            // up to eight ticks per rendered frame, so a sim time without it
            // cannot be read as a per-tick cost.
            SOL_PROFILE_ZONE_NAMED(simZone, "sim");
            simLoop.beginFrame(deltaSeconds);
            while (simLoop.shouldTick()) {
                world.tick(simLoop.tickDelta());
                content.tick(simLoop.tickDelta());
                SOL_PROFILE_COUNT(simZone, 1);
            }
        }

        // The jump transition (Phase 8v), on real frame time rather than sim
        // ticks — it runs precisely while the sim is stopped. This sits after
        // the fixed loop and outside it, which is what makes the system change
        // it performs safe: loadSystem must not run with a tick in flight.
        if (inFlight || jumping) {
            world.advanceJumpTransition(deltaSeconds);
        }
        const float simAlpha = simLoop.alpha();

        const game::Transform shipTransform = world.shipRenderTransform(simAlpha);

        // Free-look (Phase 8m): only in the cockpit, and only while gameplay
        // owns the keyboard — a menu or the bookmark prompt stops the head the
        // same way it stops the ship. Released, the eye eases back on its own,
        // so this runs every frame rather than only while the key is down.
        const bool freeLook = gameplayLive && cameraMode == game::CameraMode::Cockpit &&
                              game::held(binds, game::Action::FreeLook);
        shipCamera.updateLook(window.mouseDelta(), freeLook, deltaSeconds,
                              settings.mouseSensitivity, settings.invertPitch);

        game::CameraFrame camera;
        switch (cameraMode) {
        case game::CameraMode::Cockpit:
            camera = shipCamera.cockpit(shipTransform);
            break;
        case game::CameraMode::ThirdPerson:
            camera = shipCamera.thirdPerson(shipTransform, deltaSeconds);
            break;
        case game::CameraMode::Free:
            camera = freeCamera.frame();
            break;
        }
        const bool inCockpit = cameraMode == game::CameraMode::Cockpit;

        // Where the HUD is mounted (Phase 8m), computed as soon as the camera
        // exists because the pick below has to be answered against the frame
        // the disc and the panels are actually drawn from. In the cockpit the
        // mount points are projected out of the frame, so they follow the dash
        // at any field of view, window size or UI scale and swing with the
        // head; outside it the frame answers with the screen corners the HUD
        // used before there was a cockpit, and chase view is unchanged.
        sol::ui::HudFrame hudFrame;
        sol::core::Vec3 boresightCamera = {0.0f, 0.0f, -1.0f};
        {
            const float hudScale = settings.uiScale > 0.0f ? settings.uiScale : 1.0f;
            const sol::core::Vec2 hudScreen = {
                static_cast<float>(swapchain.extent().width) / hudScale,
                static_cast<float>(swapchain.extent().height) / hudScale};
            const float tanHalfFov = std::tan(game::kCameraVerticalFov * 0.5f);
            hudFrame = inCockpit
                           ? sol::ui::cockpitFrame(shipCamera.headOffset(), hudScreen, tanHalfFov)
                           : sol::ui::screenFrame(
                                 hudScreen, game::kHudMargin,
                                 sol::ui::radarCenter(hudScreen, game::kHudMargin));
            // The nose, in camera space. An identity head offset makes this
            // exactly (0,0,-1), which is what it was before free-look existed.
            boresightCamera =
                rotate(conjugate(shipCamera.headOffset()), sol::core::Vec3{0.0f, 0.0f, -1.0f});
        }

        // Click-to-select (Phase 8j). The view frame is published as soon as
        // the camera exists, and the click is answered here rather than with
        // the rest of the mouse input further down, so the selection it makes
        // is the one this frame's HUD and radar are built from.
        {
            const float pickScale = settings.uiScale > 0.0f ? settings.uiScale : 1.0f;
            game::ViewFrame view;
            view.cameraPosition = camera.position;
            view.cameraOrientation = camera.orientation;
            view.screenSize = {static_cast<float>(swapchain.extent().width) / pickScale,
                               static_cast<float>(swapchain.extent().height) / pickScale};
            view.tanHalfFovY = std::tan(game::kCameraVerticalFov * 0.5f);
            view.hud = hudFrame;
            view.boresightCamera = boresightCamera;
            view.valid = true;
            world.setViewFrame(view);
            // The listener is the same frame the pick and the HUD use (Phase
            // 8j's argument, one item on): what you hear and where you are
            // told things are must come from one view, or the verification is
            // about a different game than the one on screen.
            audio.setListener(camera.position, camera.orientation);

            if (gameplayLive && !devUi.wantsMouseCapture()
                && game::pressed(binds, game::Action::Select)) {
                // While the cursor is captured for mouse-look its position is
                // meaningless by contract, so the click asks the same question
                // at the boresight: target what the ship is pointing at.
                const sol::core::Vec2 cursor = window.mousePosition();
                const game::PickResult pick =
                    window.isCursorLocked()
                        ? game::pickBoresight(world)
                        : game::pickTarget(world, {cursor.x / pickScale, cursor.y / pickScale});
                game::selectPicked(world, pick);
            }
        }

        // The hull stays hidden in the cockpit: the eye sits 5 m up the nose of
        // a 12 m faceted wedge with no interior, so drawing ship.gltf around it
        // would park the camera inside a solid shell. Anything of the ship that
        // should be visible from the seat is authored into cockpit.gltf, which
        // is the only mesh that knows where the seat is.
        {
            SOL_PROFILE_ZONE("render.buildInstances");
            world.buildRenderInstances(simAlpha, !inCockpit, renderInstances);
        }
        if (inCockpit) {
            // Attached to the SHIP, not the camera. That is what lets free-look
            // look around the frame instead of dragging it along, and what puts
            // the moving sun on the dash.
            renderInstances.push_back({.position = shipTransform.position,
                                       .rotation = shipTransform.orientation,
                                       .scale = {1.0f, 1.0f, 1.0f},
                                       .model = cockpitModel});
        }
        world.buildParticleInstances(simAlpha, particleInstances);

        sceneInfo.sun = {world.sun().position, world.sun().radius, 0};
        sceneInfo.planets.clear();
        for (std::size_t i = 0; i < world.planets().size(); ++i) {
            const game::CelestialBody& planet = world.planets()[i];
            sceneInfo.planets.push_back({planet.position, planet.radius,
                                         world.currentSystemIndex() * 7u +
                                             static_cast<std::uint32_t>(i)});
        }

        // The jump tunnel (Phase 8v). The streaks converge on the SHIP's nose
        // rather than the camera's, so the tunnel stays anchored to where the
        // ship is actually going even in chase view.
        {
            const sol::sim::JumpTransition& jump = world.jumpTransition();
            sceneInfo.skyWarp = static_cast<float>(jump.warp());
            sceneInfo.skyScale = static_cast<float>(jump.skyScale());
            sceneInfo.travelDirection =
                rotate(shipTransform.orientation, sol::core::Vec3{0.0f, 0.0f, -1.0f});
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
        stats.profiler = &profiler;

        sol::ui::FlightHud hud;
        hud.active = true;
        hud.speedMetersPerSecond = stats.cameraSpeed;
        hud.assist = world.shipInput().assist;
        hud.boost = world.shipInput().boost;
        hud.cruise = world.shipInput().cruise;
        hud.autopilot = world.autopilotActive();
        switch (cameraMode) {
        case game::CameraMode::Cockpit: hud.cameraMode = "COCKPIT"; break;
        case game::CameraMode::ThirdPerson: hud.cameraMode = "CHASE"; break;
        case game::CameraMode::Free: hud.cameraMode = "FREECAM"; break;
        }
        // The surface the HUD hangs off, and where the nose is on it (Phase 8m).
        // Both were settled above, before the pick, so the panels the player
        // sees and the click that answers them read one frame.
        hud.frame = hudFrame;
        hud.boresightDirectionCamera = boresightCamera;
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
        hud.gateDistanceMeters = gateDistance;
        // kGateActivationRange stopped being an activation range in Phase 8v —
        // nothing activates at 10 km any more. It is now the range at which the
        // approach readout appears, which is the meaning gateInRange already had.
        if (const game::GateInstance* gate = world.nearestGate(); gate != nullptr) {
            hud.gateDestination = world.galaxy().systems[gate->toSystem].name.c_str();
        } else {
            hud.gateDestination = "";
        }
        hud.jumping = jumping;
        hud.docked = world.isDocked();
        hud.dockedStationName = world.dockedStationName();
        const double stationDistance = world.nearestStationDistance();
        hud.dockInRange =
            !hud.docked && stationDistance >= 0.0 && stationDistance <= kDockRange;
        // Docking clearance (Phase 8r): what the station said, and where it
        // told you to park.
        hud.stationInHailRange = !hud.docked && stationDistance >= 0.0
                                 && stationDistance <= game::SpaceWorld::kDockRequestRange;
        hud.cleared = world.hasClearance();
        if (hud.cleared) {
            hud.clearedBerth = static_cast<int>(world.clearance().berth) + 1;
            hud.clearedBerthDistanceMeters =
                length(world.clearedBerthPoint() - world.shipState().position);
        }
        commsLines.clear();
        for (const game::SpaceWorld::CommsMessage& message : world.comms()) {
            // The last two seconds are the fade, so a line dims out of the
            // corner of the eye instead of disappearing mid-read.
            const double fade = sol::core::clamp(message.secondsLeft * 0.5, 0.0, 1.0);
            commsLines.push_back({.from = message.from.c_str(),
                                  .text = message.text.c_str(),
                                  .fade = static_cast<float>(fade)});
        }
        hud.comms = commsLines;
        // Contact radar (Phase 8h): everything around the ship, not just the
        // one thing targeted. The vector outlives the span the HUD carries.
        game::fillRadarContacts(world, radarContacts);
        hud.radarContacts = radarContacts;
        hud.radarRangeMeters = sol::ui::kRadarRangeMeters;
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
        // Prompt keys (Phase 8k). These used to be string literals in the HUD,
        // which a rebind turned into a confident lie.
        hud.interactKey = game::boundChordName(binds, game::Action::DockSalvage);
        hud.scanKey = game::boundChordName(binds, game::Action::ScanPulse);
        hud.hailKey = game::boundChordName(binds, game::Action::HailTarget);
        // Phase 8s: the chip shows only when there is somebody to talk to, and
        // "the selection is a ship" is the world's question rather than the
        // HUD's - it has never known what a contact is.
        if (world.targetIsContact()) {
            const game::TargetInfo contact = world.currentTargetInfo();
            hud.shipInHailRange =
                contact.isShip
                && length(contact.nav.position - world.shipState().position)
                       <= game::SpaceWorld::kHailRange;
        }
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
            } else if (objective.kind == sol::sim::ObjectiveKind::Hold) {
                // The contest meter is the progress bar this objective has
                // (Phase 8u), and it lives in the sim rather than on the
                // objective - so it is read here, where the world is.
                const int percent = static_cast<int>(
                    world.factionSim().contestOf(objective.system).pressure * 100.0f + 0.5f);
                missionHudObjective += " (pressure " + std::to_string(percent) + "%)";
            }
            // ...and where that actually is (Phase 8i). The mission's prose
            // names the errand, not the place; without this a Dock objective
            // never says which station and nothing off-system says which
            // system, which is half of "I had no way to find it".
            const std::string destination = world.objectiveDestinationText();
            if (!destination.empty()) {
                missionHudObjective += " - " + destination;
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
            tradeText.clear();
            for (std::uint32_t i = 0;
                 i < static_cast<std::uint32_t>(world.commodityIds().size()); ++i) {
                const sol::assets::CommodityDef* def =
                    content.defs().findCommodity(world.commodityIds()[i].c_str());
                sol::ui::TradeRow row{
                    .name = def != nullptr ? def->name.c_str()
                                           : world.commodityIds()[i].c_str(),
                    .price = world.economy().price(market, i),
                    .stock = world.economy().stock(market, i),
                    .cargo = world.playerCargo(i),
                };
                // Market intel (Phase 8g): the best price this commodity has
                // been seen at anywhere else, and how old that reading is.
                std::uint32_t system = 0;
                double age = 0.0;
                bool stale = false;
                if (world.bestKnownPrice(i, &system, &row.elsewherePrice, &age, &stale)) {
                    row.hasElsewhere = true;
                    row.elsewhereStale = stale;
                    row.elsewhereName = world.galaxy().systems[system].name.c_str();
                    tradeText.push_back(game::formatAge(age));
                    row.elsewhereAge = tradeText.back().c_str();
                }
                tradeRows.push_back(row);
            }
            stationPanel.trade.stationName = world.dockedStationName();
            stationPanel.trade.credits = world.playerCredits();
            stationPanel.trade.cargoUsed = world.playerCargoTotal();
            stationPanel.trade.cargoCapacity = world.playerCargoCapacity();
            stationPanel.trade.rows = tradeRows;
            stationPanel.trade.intelMarkets = world.intelMarketCount();
            stationPanel.trade.intelPrice = world.intelPrice();
            stationPanel.trade.canBuyIntel = stationPanel.trade.intelMarkets > 0 &&
                                             world.playerCredits() >= stationPanel.trade.intelPrice;
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
            // The trade overlay's commodity is view state the screen owns
            // across frames; fillMapPanel reads it to decide what to color by.
            mapPanel.tradeCommodity = mapScreen.tradeCommodity;
            mapPanel.commodityNames = commodityNames;
            // And which system the System tab is looking at (Phase 8q) - the
            // galaxy tab's own selection, so finding a system on one tab and
            // looking inside it on the other is one gesture rather than two
            // selections that can disagree.
            mapPanel.viewSystem = mapScreen.selectedSystem;
            game::fillMapPanel(world, mapText, mapPanel, mapSystemRows, mapLaneRows,
                               mapMarkerRows);
        }
        sol::ui::ShipInfoPanel shipPanel;
        if (state == game::GameState::ShipInfo) {
            game::fillShipInfoPanel(world, content.defs(), shipText, shipPanel, shipFlightRows,
                                    shipDefenceRows, shipUtilityRows, shipFittedRows,
                                    shipCargoRows);
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

        // Text entry (Phase 8h). The characters come from the platform layer
        // rather than from key states, so the keyboard layout is respected;
        // the editing keys are edges, because a caret that moved once per
        // frame while a key was held would be unusable.
        uiInput.text = bookmarkKeyEcho ? std::string_view{} : window.textInput();
        uiInput.editLeft = menuKeyEdge(sol::platform::Key::Left, previousEditLeft);
        uiInput.editRight = menuKeyEdge(sol::platform::Key::Right, previousEditRight);
        uiInput.editHome = menuKeyEdge(sol::platform::Key::Home, previousEditHome);
        uiInput.editEnd = menuKeyEdge(sol::platform::Key::End, previousEditEnd);
        uiInput.editBackspace = menuKeyEdge(sol::platform::Key::Backspace, previousEditBackspace);
        uiInput.editDelete = menuKeyEdge(sol::platform::Key::Delete, previousEditDelete);
        uiInput.editSubmit = menuKeyEdge(sol::platform::Key::Enter, previousEditSubmit);

        game::MenuAction menuAction = game::MenuAction::None;
        bool undockRequested = false;
        bool mapClosed = false;
        {
        SOL_PROFILE_ZONE("ui.build");
        ui.beginFrame(uiInput, uiSize, deltaSeconds);
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
        case game::GameState::Controls:
            // The capture reads the raw chord edge rather than any binding, so
            // a key can be assigned to an action whatever else already holds
            // it - the steal is the screen's business, not the table's caller.
            menuAction = game::buildControlsScreen(ui, settings, controlsScreen,
                                                   settings.bindings.captured(), escapeEdge);
            break;
        case game::GameState::Flying:
            // The dev HUD stays up beside this one during the changeover.
            game::buildFlightUi(ui.drawList(), renderer.uiFont(), ui.screenSize(), hud);
            // The bookmark prompt sits over the HUD rather than replacing it:
            // the galaxy keeps running, and writing down a waypoint should not
            // feel like leaving the cockpit.
            game::buildBookmarkPrompt(ui, bookmarkPrompt);
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
        case game::GameState::ShipInfo:
            if (game::buildShipScreen(ui, shipPanel, shipScreen)) {
                state = world.isDocked() ? game::GameState::Docked : game::GameState::Flying;
            }
            break;
        }
        ui.endFrame();
        }
        // One cue per frame however many widgets fired: two controls cannot
        // meaningfully be pressed in the same frame, and stacking clicks turns
        // a tap into a rattle.
        if (ui.activationsThisFrame() > 0) {
            audio.play2D(audio.cues().uiClick);
        }
        // Pushed every frame so a slider is heard while it is being dragged;
        // GameAudio drops the ones that did not change.
        audio.setVolumes(settings.masterVolume, settings.effectsVolume);
        renderer.setUiDrawList(&ui.drawList());

        // Bookmark prompt outcome (Phase 8h). The latched position is used,
        // not where the ship has drifted to while the name was typed.
        if (bookmarkPrompt.accepted) {
            if (world.addBookmarkAt(bookmarkPosition, bookmarkPrompt.name)) {
                SOL_LOG_INFO("Bookmarked '%s'", bookmarkPrompt.name.c_str());
            } else {
                SOL_LOG_INFO("Too many bookmarks in this system");
            }
            bookmarkPrompt.open = false;
        } else if (bookmarkPrompt.cancelled) {
            bookmarkPrompt.open = false;
        }

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
        case game::MenuAction::OpenControls:
            state = game::GameState::Controls;
            break;
        case game::MenuAction::CloseControls:
            // Bindings persist the moment the player leaves the list, on the
            // same terms the sliders do: a crash must not cost them the layout
            // they just built.
            if (!settings.save(settingsPath.c_str())) {
                SOL_LOG_WARN("could not write %s", settingsPath.c_str());
            }
            state = game::GameState::Settings;
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
            // The overlay picker is view state, so it is handled here rather
            // than in executeMapAction, which acts on the world.
            if (mapPanel.action.kind == sol::ui::MapAction::Kind::SetTradeCommodity) {
                mapScreen.tradeCommodity = mapPanel.action.index;
            }
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
        {
            SOL_PROFILE_ZONE("ui.devOverlay");
            devUi.beginFrame(stats);
        }
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
        // V-Sync is a swapchain present mode (Phase 8k), so toggling it rides
        // the recreate path every window resize already exercises rather than
        // getting a second one written for it.
        if (settings.vsync != previousVsync) {
            previousVsync = settings.vsync;
            needRecreate = true;
            SOL_LOG_INFO("V-Sync %s", settings.vsync ? "on" : "off");
        }
        if (needRecreate) {
            devUi.discardFrame();
        }
        if (!needRecreate) {
            // The CPU side of the frame's end. Kept as one zone so the number
            // stays comparable to 8n's, with the four children that say which
            // part of it is a wait (scene_renderer.cpp) and the gpu.* zones
            // that say what the GPU did underneath it (Phase 8o).
            SOL_PROFILE_ZONE_NAMED(renderZone, "render.submit");
            SOL_PROFILE_COUNT(renderZone, renderInstances.size());
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
            if (swapchain.recreate(window.width(), window.height(), settings.vsync)) {
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
    // The device thread reads the bank, so it stops before anything that owns
    // samples goes away (Phase 8t).
    audio.shutdown();
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
