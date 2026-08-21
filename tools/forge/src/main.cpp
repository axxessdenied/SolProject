// The Forge - this game's asset viewer, and the shell the authoring tool will
// grow inside (engine plan Phase 9 stage C).
//
// What it buys, and it is one thing: SCALE. Every other property of a mesh can
// be read out of a file - the triangle count, the bounds, the radius a model
// row will carry - and this prints all of them. What no text can answer is
// whether the thing you just authored is ship-sized or station-sized under the
// game's own light, at the game's own field of view. That is why the reference
// boxes and the metric grid are here and why the lighting numbers are the
// game's rather than a viewer's own.

#include "forge_view.hpp"
#include "mesh_library.hpp"
#include "orbit_camera.hpp"
#include "part_editor.hpp"
#include "point_tool.hpp"

#include "sol/core/log.hpp"
#include "sol/core/version.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/imgui_host.hpp"
#include "sol/ui/pick.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace sol;

#if defined(NDEBUG)
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

#if !defined(SOL_ASSETS_SOURCE_DIR)
    #define SOL_ASSETS_SOURCE_DIR ""
#endif

// --frames N: render N frames, then exit (for automated runs), exactly as the
// game's smoke test does.
std::uint64_t parseMaxFrames(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            return std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    return 0;
}

// The three sizes a person needs in their eye to judge an asset in this game,
// drawn as wireframe boxes around the ORIGIN. Two are shipped facts rather
// than invented ones - a station's model radius is 100 m and a ship's hull is
// 12 m nose to tail - and the third is a human reach, which is what makes a
// dash instrument judgeable at all.
//
// ⚑ They sit at the origin because that is the frame an asset is authored in,
// and for a mesh authored somewhere else the small box is in the wrong place:
// the cockpit is authored in SHIP space with the eye at (0, 0.8, -5), so its
// arm's-length box belongs five metres forward of where this draws it. Moving
// it would mean the tool knowing a game constant, which is the duplication
// this file already carries once for the lighting and should not carry twice.
struct ScaleReference
{
    const char* label;
    float size; // edge length, metres
    core::Vec4 color;
    bool enabled;
};

void addWireBox(renderer::DebugDrawRenderer& lines, core::Vec3 min, core::Vec3 max,
                core::Vec4 color)
{
    const core::Vec3 corner[8] = {
        {min.x, min.y, min.z}, {max.x, min.y, min.z}, {max.x, min.y, max.z}, {min.x, min.y, max.z},
        {min.x, max.y, min.z}, {max.x, max.y, min.z}, {max.x, max.y, max.z}, {min.x, max.y, max.z},
    };
    static constexpr int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                          {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : kEdges) {
        lines.line(corner[edge[0]], corner[edge[1]], color);
    }
}

// Metric floor grid through the origin. The cell size follows the camera, so
// the grid reads as a ruler at 0.1 m and at 100 m without the author touching
// anything - and the panel prints which one is on screen, because a grid whose
// spacing is a mystery measures nothing.
float addGrid(renderer::DebugDrawRenderer& lines, float cameraDistance)
{
    // ~20 cells across the view; the exponent is what keeps it metric.
    const float raw = cameraDistance > 0.0f ? cameraDistance / 10.0f : 1.0f;
    const float cell =
        core::clamp(std::pow(10.0f, std::round(std::log10(raw))), 0.01f, 1000.0f);

    constexpr int kHalfCells = 20;
    const float extent = cell * static_cast<float>(kHalfCells);
    constexpr core::Vec4 kMinor = {0.16f, 0.17f, 0.20f, 1.0f};
    constexpr core::Vec4 kMajor = {0.30f, 0.32f, 0.36f, 1.0f};
    for (int i = -kHalfCells; i <= kHalfCells; ++i) {
        const float offset = static_cast<float>(i) * cell;
        const core::Vec4 color = (i % 10) == 0 ? kMajor : kMinor;
        if (i != 0) {
            lines.line({offset, 0.0f, -extent}, {offset, 0.0f, extent}, color);
            lines.line({-extent, 0.0f, offset}, {extent, 0.0f, offset}, color);
        }
    }
    // The axes last so they draw over the grid they cross.
    lines.line({-extent, 0.0f, 0.0f}, {extent, 0.0f, 0.0f}, {0.65f, 0.22f, 0.22f, 1.0f});
    lines.line({0.0f, 0.0f, -extent}, {0.0f, 0.0f, extent}, {0.25f, 0.35f, 0.75f, 1.0f});
    lines.line({0.0f, -extent * 0.25f, 0.0f}, {0.0f, extent * 0.25f, 0.0f},
               {0.25f, 0.60f, 0.30f, 1.0f});
    return cell;
}

} // namespace

int main(int argc, char** argv)
{
    const std::uint64_t maxFrames = parseMaxFrames(argc, argv);

    SOL_LOG_INFO("Sol Forge %s on %s", core::engineVersionString(), platform::platformName());

    platform::Window window;
    platform::WindowDesc windowDesc = {};
    windowDesc.title = "Sol Forge";
    windowDesc.width = 1600;
    windowDesc.height = 900;
    if (!window.create(windowDesc)) {
        return EXIT_FAILURE;
    }

    rhi::Context context;
    rhi::ContextDesc contextDesc = {};
    contextDesc.appName = "Sol Forge";
    contextDesc.enableValidation = kEnableValidation;
    if (!context.initialize(contextDesc, window.nativeHandle())) {
        return EXIT_FAILURE;
    }

    const std::string executableDir = platform::executableDirectory();
    const std::string shaderDirectory = executableDir + "shaders/";
    const std::string cookedDirectory = executableDir + "cooked/";
    // Authored sources come from the tree in a dev build, the same way the
    // game reads its defs; an installed tool falls back to a directory beside
    // the executable.
    const std::string assetsDirectory = std::strlen(SOL_ASSETS_SOURCE_DIR) > 0
                                            ? SOL_ASSETS_SOURCE_DIR
                                            : executableDir + "assets";

    rhi::Swapchain swapchain;
    if (!swapchain.create(context, window.width(), window.height(), /*vsync=*/true)) {
        return EXIT_FAILURE;
    }

    forge::ForgeView view;
    if (!view.initialize(context, swapchain, shaderDirectory.c_str())) {
        return EXIT_FAILURE;
    }

    sol::ui::ImGuiHost imguiHost;
    if (!imguiHost.initialize(window, context, swapchain.imageFormat(), VK_FORMAT_D32_SFLOAT,
                              swapchain.imageCount())) {
        return EXIT_FAILURE;
    }
    view.setImGuiHost(&imguiHost);

    std::vector<forge::AssetEntry> meshEntries =
        forge::listMeshes(assetsDirectory, cookedDirectory);
    const std::vector<forge::AssetEntry> textureEntries = forge::listTextures(cookedDirectory);
    SOL_LOG_INFO("forge: %zu mesh(es) under %s and %s, %zu cooked texture(s)", meshEntries.size(),
                 assetsDirectory.c_str(), cookedDirectory.c_str(), textureEntries.size());

    // Every cooked texture is uploaded once at startup - there are three, they
    // are BC1, and it means switching one costs no device idle in the middle
    // of a frame.
    std::vector<renderer::GpuTexture> textures;
    std::vector<std::string> textureLabels;
    for (const forge::AssetEntry& entry : textureEntries) {
        assets::TextureData data;
        if (!assets::loadTexture(entry.path.c_str(), data)) {
            SOL_LOG_WARN("forge: cannot load texture %s", entry.path.c_str());
            continue;
        }
        textures.push_back(view.meshes().createTexture(data));
        textureLabels.push_back(entry.label);
    }
    if (textures.empty()) {
        SOL_LOG_ERROR("forge: no cooked textures under %s - build the cooker target first",
                      cookedDirectory.c_str());
        return EXIT_FAILURE;
    }

    // ⚑ The authored side of every mesh in this game. The tool reads the game's
    // DATA directory, never its code (AGENTS.md 4) - `DefDatabase` is engine and
    // models.toml is a file - which is what lets a viewer say "this hull is
    // 1.1584 m and the sim thinks it is 1.0" instead of leaving that to be
    // noticed by a person holding a panel next to a text file, which is exactly
    // how stage C found it.
    assets::DefDatabase defs;
    {
        const std::string dataDirectory = SOL_MODEL_DATA_DIR;
        std::string defError;
        if (!forge::loadModelCatalog(dataDirectory, defs, &defError)) {
            SOL_LOG_WARN("forge: cannot read model defs: %s", defError.c_str());
        }
        SOL_LOG_INFO("forge: %zu [[model]] row(s) from %s", defs.models().size(),
                     dataDirectory.c_str());
    }

    renderer::GpuMesh openMesh = {};
    forge::MeshReport report;
    std::vector<forge::ModelMatch> modelMatches;
    int openIndex = -1;
    std::string status = "no mesh open";

    // The checker is the default by choice, not by alphabet: the question a
    // viewer is usually being asked is whether the FORM is right, and a hull
    // texture hides a stretched uv and a face pointing the wrong way, both of
    // which a checker shows at a glance.
    int textureIndex = 0;
    for (std::size_t i = 0; i < textureLabels.size(); ++i) {
        if (textureLabels[i].rfind("checker", 0) == 0) {
            textureIndex = static_cast<int>(i);
            break;
        }
    }

    ScaleReference references[] = {
        {"station (200 m)", 200.0f, {0.20f, 0.55f, 0.70f, 1.0f}, true},
        {"ship (12 m)", 12.0f, {0.85f, 0.55f, 0.15f, 1.0f}, true},
        {"arm's length (0.6 m)", 0.6f, {0.70f, 0.30f, 0.65f, 1.0f}, true},
    };

    forge::OrbitCamera camera;
    float emissive = 0.0f;
    float exposure = 1.0f;
    float sunAzimuth = 0.9f;
    float sunElevation = 0.6f;
    bool showGrid = true;
    bool showBounds = true;
    bool showPoints = true;
    float gridCell = 1.0f;

    // ⚑ Two different radii, and using the wrong one misframes half the
    // shipped assets. The report's `radius` is measured from the ORIGIN,
    // because that is what a `[[model]]` row carries and what the sim builds a
    // collision sphere from. Framing wants the radius about the mesh's OWN
    // centre: the cockpit is authored in ship space 4.5-7 m forward of the
    // origin, so framing it at 6.95 m showed a two-metre object from sixteen
    // metres away.
    const auto frameOpenMesh = [&] {
        const core::Vec3 center = (report.boundsMin + report.boundsMax) * 0.5f;
        const float fitRadius = core::length(report.boundsMax - report.boundsMin) * 0.5f;
        camera.frame(center, fitRadius, forge::kCameraVerticalFov);
    };

    forge::PartEditor editor;
    forge::PointTool points;

    // One path from a buffer of triangles to what is on screen, whether those
    // triangles came off disk or out of the part tree a moment ago.
    const auto uploadMesh = [&](const assets::MeshData& data, bool reframe) {
        // The mesh being replaced may still be referenced by a frame the GPU
        // has not finished; nothing here is worth a per-frame deletion queue.
        context.waitIdle();
        if (openMesh.indexCount > 0) {
            view.meshes().destroyMesh(openMesh);
        }
        openMesh = view.meshes().createMesh(data);
        report = forge::reportMesh(data);
        // Re-matched on every upload, not only on open: editing a part changes
        // the measured radius, so a check that ran once would go stale the
        // moment the tool was used for what it is for.
        if (openIndex >= 0) {
            modelMatches = forge::matchModels(defs, meshEntries[static_cast<std::size_t>(openIndex)],
                                              report);
        }
        if (reframe) {
            frameOpenMesh();
        }
    };

    // Rebuilds from the open document. Called on every edit, which is what
    // makes the part tree feel like a model rather than a config file - the
    // meshes in this game are hundreds of triangles, so a full rebuild is
    // cheaper than working out what changed.
    const auto rebuildFromEditor = [&](bool reframe) {
        assets::MeshData data;
        std::string error;
        if (!assets::buildForge(editor.doc(), data, &error)) {
            editor.setBuildError(error);
            return;
        }
        editor.setBuildError({});
        if (data.vertices.empty()) {
            // A tree of nothing but groups is a legal document and an illegal
            // draw; keep the last good mesh rather than uploading an empty one.
            return;
        }
        uploadMesh(data, reframe);
        // The points are resolved from the same document the mesh came from, so
        // they are re-resolved wherever it is rebuilt - which is on every
        // accepted edit, including each frame of a drag.
        points.refresh(editor.doc());
    };

    const auto openMeshAt = [&](int index) {
        if (index < 0 || index >= static_cast<int>(meshEntries.size())) {
            return;
        }
        assets::MeshData data;
        if (!forge::loadMesh(meshEntries[index], data)) {
            status = "failed to open " + meshEntries[index].label;
            SOL_LOG_ERROR("forge: %s", status.c_str());
            return;
        }
        // Before the upload, not after: uploadMesh matches the open asset
        // against the model catalog and needs to know which asset that is.
        openIndex = index;
        uploadMesh(data, /*reframe=*/true);
        status = meshEntries[index].label;

        // A `.forge` is a SOURCE, so opening one loads the tree behind the
        // mesh as well; everything else is triangles that can only be looked at.
        points.close();
        if (forge::isPartSource(meshEntries[index])) {
            std::string openStatus;
            if (editor.openFile(meshEntries[index].path, openStatus)) {
                status = openStatus;
                points.refresh(editor.doc());
            } else {
                status = openStatus;
            }
        }
        SOL_LOG_INFO("forge: %s - %u tris, %u verts (%u points), radius %.3f m",
                     meshEntries[index].label.c_str(), report.triangles, report.renderVertices,
                     report.positions, static_cast<double>(report.boundingRadius));
        // Logged as well as drawn, because `--frames N` with stdout redirected
        // is how this tool gets read by anything that is not a person.
        for (const forge::ModelMatch& match : modelMatches) {
            if (match.radiusAgrees()) {
                SOL_LOG_INFO("forge: [[model]] %s radius %.4f m agrees with the mesh",
                             match.id.c_str(), static_cast<double>(match.authoredRadius));
            } else {
                SOL_LOG_WARN("forge: [[model]] %s authors radius %.4f m, the mesh measures %.4f m "
                             "(%+.4f, %+.1f%%)",
                             match.id.c_str(), static_cast<double>(match.authoredRadius),
                             static_cast<double>(report.boundingRadius),
                             static_cast<double>(match.radiusDelta),
                             static_cast<double>(match.radiusDeltaPercent()));
            }
        }
    };
    openMeshAt(meshEntries.empty() ? -1 : 0);

    core::Vec2 previousMouse = window.mousePosition();
    bool dragOrbit = false;
    bool dragPan = false;
    bool framePressed = false;
    bool previousLeftDown = false;
    bool undoPressed = false;
    double previousTime = platform::timeSeconds();
    float frameMilliseconds = 0.0f;

    std::uint64_t frameCount = 0;
    bool failed = false;
    while (true) {
        window.pumpEvents();
        if (window.shouldClose()) {
            break;
        }
        if (window.isMinimized()) {
            platform::sleepMilliseconds(16);
            continue;
        }

        const double now = platform::timeSeconds();
        frameMilliseconds = static_cast<float>((now - previousTime) * 1000.0);
        previousTime = now;

        // --- camera input ---
        const core::Vec2 mouse = window.mousePosition();
        const core::Vec2 mouseDelta = {mouse.x - previousMouse.x, mouse.y - previousMouse.y};
        previousMouse = mouse;

        const bool leftDown = window.isMouseButtonDown(platform::MouseButton::Left);
        const bool leftPressed = leftDown && !previousLeftDown;
        previousLeftDown = leftDown;
        const bool middleDown = window.isMouseButtonDown(platform::MouseButton::Middle);
        const bool shiftDown = window.isKeyDown(platform::Key::LeftShift);

        // --- point editing (stage E1), before the camera claims the drag ---
        //
        // ⚑ The order is the whole arbitration. LMB is the orbit AND the point
        // drag, and a modeller reaching for a vertex must not spin the model
        // instead. The point tool is offered the press first: if it takes one,
        // the camera never sees a drag to claim. Deciding it the other way -
        // camera first, point tool with whatever is left - is how a tool ends up
        // needing a modifier key to select anything.
        forge::PointTool::Viewport viewport;
        viewport.cursor = mouse;
        viewport.cursorDelta = mouseDelta;
        viewport.center = {static_cast<float>(window.width()) * 0.5f,
                           static_cast<float>(window.height()) * 0.5f};
        viewport.height = static_cast<float>(window.height());
        viewport.verticalFov = forge::kCameraVerticalFov;
        viewport.focal = ui::focalLength(viewport.height,
                                         std::tan(forge::kCameraVerticalFov * 0.5f));
        viewport.cameraDistance = camera.distance();
        viewport.view = camera.view();
        viewport.leftPressed = leftPressed && !shiftDown;
        viewport.leftDown = leftDown;
        viewport.uiCaptured = imguiHost.wantsMouseCapture();
        viewport.axisLock = -1;
        if (!imguiHost.wantsKeyboardCapture()) {
            if (window.isKeyDown(platform::Key::X)) {
                viewport.axisLock = 0;
            } else if (window.isKeyDown(platform::Key::Y)) {
                viewport.axisLock = 1;
            } else if (window.isKeyDown(platform::Key::Z)) {
                viewport.axisLock = 2;
            }
            // ⚑ Stage E4b's element mode, on the digits rather than on a letter
            // for two reasons that both matter here: it is the modeller
            // convention, and every letter this tool could want is taken - X, Y
            // and Z are the axis lock, F frames, and Z is also half of undo. A
            // third digit is where faces land at E4d.
            if (window.isKeyDown(platform::Key::Num1)) {
                points.setMode(forge::PointTool::Mode::Point);
            } else if (window.isKeyDown(platform::Key::Num2)) {
                points.setMode(forge::PointTool::Mode::Edge);
            } else if (window.isKeyDown(platform::Key::Num3)) {
                points.setMode(forge::PointTool::Mode::Face);
            }
        }
        if (points.update(viewport, editor)) {
            rebuildFromEditor(/*reframe=*/false);
        }

        // Ctrl+Z, edge triggered - a held chord must undo once, not sixty times.
        const bool undoDown = window.isKeyDown(platform::Key::LeftControl) &&
                              window.isKeyDown(platform::Key::Z) &&
                              !imguiHost.wantsKeyboardCapture();
        if (undoDown && !undoPressed && editor.undo()) {
            rebuildFromEditor(/*reframe=*/false);
        }
        undoPressed = undoDown;

        // A drag is claimed on its first frame and kept for its duration:
        // deciding every frame would hand the camera a drag that began on a
        // slider the moment the cursor left the panel.
        if (!leftDown && !middleDown) {
            dragOrbit = false;
            dragPan = false;
        } else if (!dragOrbit && !dragPan && !imguiHost.wantsMouseCapture() && !points.dragging()) {
            dragPan = middleDown || (leftDown && shiftDown);
            dragOrbit = !dragPan;
        }
        if (points.dragging()) {
            dragOrbit = false;
            dragPan = false;
        }
        if (dragOrbit) {
            camera.orbit(-mouseDelta.x * 0.008f, mouseDelta.y * 0.008f);
        } else if (dragPan) {
            camera.pan(mouseDelta.x, mouseDelta.y, static_cast<float>(window.height()),
                       forge::kCameraVerticalFov);
        }
        if (!imguiHost.wantsMouseCapture()) {
            const float wheel = window.wheelDelta();
            if (wheel != 0.0f) {
                camera.dolly(wheel);
            }
        }
        const bool frameDown =
            window.isKeyDown(platform::Key::F) && !imguiHost.wantsKeyboardCapture();
        if (frameDown && !framePressed && (openIndex >= 0 || editor.isOpen())) {
            frameOpenMesh();
        }
        framePressed = frameDown;

        // --- viewport geometry ---
        if (showGrid) {
            gridCell = addGrid(view.debugDraw(), camera.distance());
        }
        for (const auto& reference : references) {
            if (!reference.enabled) {
                continue;
            }
            const float half = reference.size * 0.5f;
            addWireBox(view.debugDraw(), {-half, -half, -half}, {half, half, half},
                       reference.color);
        }
        if (showBounds && (openIndex >= 0 || editor.isOpen())) {
            addWireBox(view.debugDraw(), report.boundsMin, report.boundsMax,
                       {0.30f, 0.75f, 0.35f, 1.0f});
        }
        // ⚑ Last of the debug lines on purpose. `DebugDrawRenderer::line()`
        // drops silently once its 8192 vertices are spent, so whatever is drawn
        // last is what goes missing - and the grid and the scale boxes are the
        // frame's fixed furniture while the markers are the variable part. The
        // tool's own budget keeps it well inside; this is the second belt.
        if (showPoints && editor.isOpen()) {
            points.drawMarkers(view.debugDraw(), viewport);
        }

        // --- panel ---
        imguiHost.beginFrame();
        ImGui::SetNextWindowPos({12.0f, 12.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({380.0f, 860.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Forge")) {
            ImGui::TextDisabled("%.1f fps  (%.2f ms)   grid %.2g m",
                                frameMilliseconds > 0.0f ? 1000.0f / frameMilliseconds : 0.0f,
                                frameMilliseconds, static_cast<double>(gridCell));
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Meshes", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginChild("##meshes", {0.0f, 190.0f}, ImGuiChildFlags_Borders)) {
                    for (int i = 0; i < static_cast<int>(meshEntries.size()); ++i) {
                        if (ImGui::Selectable(meshEntries[i].label.c_str(), i == openIndex)) {
                            openMeshAt(i);
                        }
                    }
                }
                ImGui::EndChild();
                if (ImGui::Button("Reload") && openIndex >= 0) {
                    openMeshAt(openIndex);
                }
                ImGui::SameLine();
                if (ImGui::Button("New parts")) {
                    editor.openNew(assetsDirectory + "/meshes");
                    openIndex = -1;
                    rebuildFromEditor(/*reframe=*/true);
                    status = "new part document";
                }
                ImGui::SameLine();
                if (ImGui::Button("Rescan")) {
                    // A save writes a new file into the source tree, and the
                    // list was read once at startup.
                    meshEntries = forge::listMeshes(assetsDirectory, cookedDirectory);
                    openIndex = -1;
                    status = std::to_string(meshEntries.size()) + " assets";
                }
                ImGui::TextDisabled("%s", status.c_str());
            }

            // The authoring half (stage D). It sits above the report on purpose:
            // the numbers below are what the edit above just changed.
            if (ImGui::CollapsingHeader("Parts", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (editor.draw()) {
                    rebuildFromEditor(/*reframe=*/false);
                }
            }

            // Stages E1, E2 and E4b. Below Parts because a point is a
            // consequence of the parts above it, and above the Report for the
            // same reason Parts is.
            if (editor.isOpen() && ImGui::CollapsingHeader("Points, edges & faces",
                                                           ImGuiTreeNodeFlags_DefaultOpen)) {
                points.drawPanel(editor.doc());
            }

            if ((openIndex >= 0 || editor.isOpen()) &&
                ImGui::CollapsingHeader("Report", ImGuiTreeNodeFlags_DefaultOpen)) {
                const core::Vec3 size = report.boundsMax - report.boundsMin;
                ImGui::Text("triangles      %u", report.triangles);
                // Corners and points are different numbers and the gap is the
                // shading: a hard edge splits one point into several corners.
                ImGui::Text("render verts   %u", report.renderVertices);
                ImGui::Text("welded points  %u", report.positions);
                ImGui::Text("bounds min     %8.3f %8.3f %8.3f", static_cast<double>(report.boundsMin.x),
                            static_cast<double>(report.boundsMin.y),
                            static_cast<double>(report.boundsMin.z));
                ImGui::Text("bounds max     %8.3f %8.3f %8.3f", static_cast<double>(report.boundsMax.x),
                            static_cast<double>(report.boundsMax.y),
                            static_cast<double>(report.boundsMax.z));
                ImGui::Text("size (m)       %8.3f %8.3f %8.3f", static_cast<double>(size.x),
                            static_cast<double>(size.y), static_cast<double>(size.z));
                ImGui::Separator();
                ImGui::Text("radius         %.4f m", static_cast<double>(report.boundingRadius));
                ImGui::TextDisabled("  the `radius` a [[model]] row would carry");
                ImGui::Text("surface area   %.4g m2", report.surfaceArea);
                ImGui::Text("volume         %.4g m3", report.signedVolume);
                ImGui::Text("cache misses   %.2f / tri", static_cast<double>(report.cacheMissRatio));
                ImGui::Separator();
                // A hull that is not closed has a hole in it, and a hole is
                // invisible from outside until the camera goes through it.
                ImGui::Text("manifold       %s", report.manifold ? "yes" : "NO");
                ImGui::Text("closed         %s", report.closed ? "yes" : "no");
                ImGui::Text("border edges   %u", report.borderEdges);

                // ⚑ The measured radius set beside the authored one. Stage C
                // could measure and could not read, so its two mismatches had
                // to be spotted by a human comparing a panel with a text file.
                ImGui::Separator();
                if (modelMatches.empty()) {
                    ImGui::TextDisabled("no [[model]] row names this mesh");
                } else {
                    for (const forge::ModelMatch& match : modelMatches) {
                        ImGui::Text("[[model]] %s", match.id.c_str());
                        ImGui::Text("  texture      %s", match.texture.c_str());
                        ImGui::Text("  radius       %.4f m authored",
                                    static_cast<double>(match.authoredRadius));
                        if (match.radiusAgrees()) {
                            ImGui::TextDisabled("  matches the mesh");
                        } else {
                            // ⚑ Wrapped, because the first version of this line
                            // ran off the panel and stopped at "the collision
                            // sphere sits" - which is the fourth time this repo
                            // has shipped a widget that does not fit its box,
                            // and the sentence IS the finding.
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
                            if (match.radiusDelta > 0.0f) {
                                ImGui::Text("  MESH IS %.4f m (%.1f%%) LARGER: the collision "
                                            "sphere sits inside the hull, so ships pass through "
                                            "the picture",
                                            static_cast<double>(match.radiusDelta),
                                            static_cast<double>(match.radiusDeltaPercent()));
                            } else {
                                ImGui::Text("  MESH IS %.4f m (%.1f%%) SMALLER: the sphere reaches "
                                            "past what is drawn, so ships stop short of nothing",
                                            static_cast<double>(-match.radiusDelta),
                                            static_cast<double>(-match.radiusDeltaPercent()));
                            }
                            ImGui::PopStyleColor();
                            ImGui::PopTextWrapPos();
                        }
                        ImGui::Text("  avoid        %.4f m",
                                    static_cast<double>(match.authoredAvoidRadius));
                        ImGui::Text("  emissive     %.3f", static_cast<double>(match.emissive));
                        ImGui::Text("  solid        %s", match.solid ? "yes" : "no (fly through)");
                    }
                }
            }

            if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginCombo("texture", textureLabels[static_cast<std::size_t>(textureIndex)].c_str())) {
                    for (int i = 0; i < static_cast<int>(textureLabels.size()); ++i) {
                        if (ImGui::Selectable(textureLabels[static_cast<std::size_t>(i)].c_str(),
                                              i == textureIndex)) {
                            textureIndex = i;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("emissive", &emissive, 0.0f, 1.0f);
                ImGui::SliderFloat("exposure", &exposure, 0.1f, 4.0f);
                ImGui::SliderFloat("sun azimuth", &sunAzimuth, -core::kPi, core::kPi);
                ImGui::SliderFloat("sun elevation", &sunElevation, -core::kHalfPi, core::kHalfPi);
                ImGui::TextDisabled("sun %.1f, ambient %.3f (the game's own)",
                                    static_cast<double>(forge::kSunIntensity),
                                    static_cast<double>(forge::kAmbient));
            }

            if (ImGui::CollapsingHeader("Scale", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("metric grid", &showGrid);
                ImGui::Checkbox("mesh bounds", &showBounds);
                ImGui::Checkbox("element markers", &showPoints);
                for (auto& reference : references) {
                    ImGui::Checkbox(reference.label, &reference.enabled);
                }
                ImGui::Separator();
                ImGui::TextDisabled("LMB a marker drags a point, LMB else orbits");
                ImGui::TextDisabled("shift+LMB or MMB pan");
                ImGui::TextDisabled("wheel dolly, F frames the mesh");
                ImGui::Text("camera %.3f m out", static_cast<double>(camera.distance()));
            }
        }
        ImGui::End();

        // --- draw ---
        forge::FrameDesc frame;
        frame.view = camera.view();
        const float cosElevation = std::cos(sunElevation);
        frame.sunDirection = {cosElevation * std::sin(sunAzimuth), std::sin(sunElevation),
                              cosElevation * std::cos(sunAzimuth)};
        frame.exposure = exposure;
        if (openMesh.indexCount > 0) {
            frame.items.push_back({&openMesh, &textures[static_cast<std::size_t>(textureIndex)],
                                   core::Mat4::identity(), emissive});
        }

        bool needRecreate = window.consumeResize();
        if (needRecreate) {
            imguiHost.discardFrame();
        }
        if (!needRecreate) {
            switch (view.drawFrame(frame)) {
            case forge::ForgeView::DrawResult::Success:
                ++frameCount;
                break;
            case forge::ForgeView::DrawResult::NeedSwapchainRecreate:
                needRecreate = true;
                imguiHost.discardFrame();
                break;
            case forge::ForgeView::DrawResult::Failure:
                failed = true;
                break;
            }
        }
        if (failed) {
            break;
        }
        if (needRecreate) {
            context.waitIdle();
            if (swapchain.recreate(window.width(), window.height(), /*vsync=*/true)) {
                if (!view.onSwapchainRecreated()) {
                    failed = true;
                    break;
                }
            }
        }
        if (maxFrames > 0 && frameCount >= maxFrames) {
            break;
        }
    }

    context.waitIdle();
    if (openMesh.indexCount > 0) {
        view.meshes().destroyMesh(openMesh);
    }
    for (renderer::GpuTexture& texture : textures) {
        view.meshes().destroyTexture(texture);
    }
    imguiHost.shutdown();
    view.shutdown();
    swapchain.destroy();
    context.shutdown();
    window.destroy();
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
