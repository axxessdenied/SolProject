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

#include "def_editor.hpp"
#include "forge_view.hpp"
#include "mesh_library.hpp"
#include "orbit_camera.hpp"
#include "part_editor.hpp"
#include "point_tool.hpp"
#include "texture_editor.hpp"

#include "sol/assets/mesh_lod.hpp"
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
#include <imgui_impl_vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
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
    std::vector<forge::AssetEntry> textureEntries =
        forge::listTextures(assetsDirectory, cookedDirectory);
    SOL_LOG_INFO("forge: %zu mesh(es) and %zu texture(s) under %s and %s", meshEntries.size(),
                 textureEntries.size(), assetsDirectory.c_str(), cookedDirectory.c_str());

    // Every texture is uploaded once at startup - there are a handful, they are
    // BC1, and it means switching one costs no device idle in the middle of a
    // frame. A `.tex` source is evaluated and encoded here exactly as the cooker
    // would, so what shades the mesh is the compressed image the game loads.
    std::vector<renderer::GpuTexture> textures;
    std::vector<std::string> textureLabels;
    std::vector<forge::AssetEntry> loadedTextureEntries;
    for (const forge::AssetEntry& entry : textureEntries) {
        assets::TextureData data;
        if (!forge::loadTexture(entry, data)) {
            SOL_LOG_WARN("forge: cannot load texture %s", entry.path.c_str());
            continue;
        }
        textures.push_back(view.meshes().createTexture(data));
        textureLabels.push_back(entry.label);
        loadedTextureEntries.push_back(entry);
    }
    if (textures.empty()) {
        SOL_LOG_ERROR("forge: no textures under %s or %s - build the cooker target first",
                      assetsDirectory.c_str(), cookedDirectory.c_str());
        return EXIT_FAILURE;
    }

    // ⚑ The authored side of every mesh in this game. The tool reads the game's
    // DATA directory, never its code (AGENTS.md 4) - `DefDatabase` is engine and
    // models.toml is a file - which is what lets a viewer say "this hull is
    // 1.1584 m and the sim thinks it is 1.0" instead of leaving that to be
    // noticed by a person holding a panel next to a text file, which is exactly
    // how stage C found it.
    assets::DefDatabase defs;
    const std::string dataDirectory = SOL_MODEL_DATA_DIR;
    {
        std::string defError;
        if (!forge::loadModelCatalog(dataDirectory, defs, &defError)) {
            SOL_LOG_WARN("forge: cannot read model defs: %s", defError.c_str());
        }
        SOL_LOG_INFO("forge: %zu [[model]] row(s) from %s", defs.models().size(),
                     dataDirectory.c_str());
    }

    // Stage H: the same rows as a DOCUMENT, so they can be written as well as
    // read. Two views of one file on purpose - `defs` is what the game would
    // load and `defEditor` is what an author is typing into - and the editor
    // re-derives its own database from the text after every accepted edit, so
    // the panel never shows a number the game would not agree with.
    forge::DefEditor defEditor;
    defEditor.load(dataDirectory);
    std::string defStatus = defEditor.loaded() ? "def rows loaded" : defEditor.error();
    // The stems the texture combo offers, which is exactly what the tool can
    // open rather than whatever a text field might be made to say.
    std::vector<std::string> textureStems;
    for (const forge::AssetEntry& textureEntry : loadedTextureEntries) {
        if (std::find(textureStems.begin(), textureStems.end(), textureEntry.stem) ==
            textureStems.end()) {
            textureStems.push_back(textureEntry.stem);
        }
    }

    renderer::GpuMesh openMesh = {};
    forge::MeshReport report;
    std::vector<forge::ModelMatch> modelMatches;
    int openIndex = -1;
    std::string status = "no mesh open";

    // Stage F: the level chain the cooker would produce for whatever is open,
    // and its meshes uploaded so a level can be LOOKED AT rather than only
    // counted. `previewLevel` 0 is the authored mesh, which is what the tool
    // has always drawn.
    assets::LodChain chain;
    std::vector<renderer::GpuMesh> levelMeshes;
    int previewLevel = 0;

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

    // Stage G: the open `.tex` document, and which uploaded texture it stands
    // behind. -1 means the editor's document has no slot on the GPU yet, which
    // is the state a brand new texture starts in.
    forge::TextureEditor textureEditor;
    int editingTextureIndex = -1;

    // ⚑ The flat preview, and it is not a luxury: every number in this document
    // is a PIXEL COORDINATE, and a texture judged only on a lumpy asteroid is a
    // set of numbers whose effect you cannot see. The mesh answers "does it look
    // right in the game"; this answers "is the panel where I put it".
    //
    // ImGui's Vulkan backend hands out its own descriptor set for an image, so
    // this is a second view of the SAME GpuTexture the mesh samples - there is
    // no second upload and no second encode to disagree with the first.
    VkDescriptorSet texturePreview = VK_NULL_HANDLE;

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
        for (renderer::GpuMesh& level : levelMeshes) {
            view.meshes().destroyMesh(level);
        }
        levelMeshes.clear();
        previewLevel = 0;

        openMesh = view.meshes().createMesh(data);
        report = forge::reportMesh(data);
        // Re-matched on every upload, not only on open: editing a part changes
        // the measured radius, so a check that ran once would go stale the
        // moment the tool was used for what it is for.
        if (openIndex >= 0) {
            modelMatches = forge::matchModels(defs, meshEntries[static_cast<std::size_t>(openIndex)],
                                              report);
        }

        // ⚑ Rebuilt on every upload for the same reason the model match is: an
        // edit changes what the cook would produce, and a chain computed once
        // would go stale the moment the tool was used for what it is for. It is
        // the cooker's own function, so the panel cannot drift from the files -
        // this shows what WILL be cooked, not a second opinion about it.
        chain = assets::buildLodChain(data);
        levelMeshes.reserve(chain.levels.size());
        for (const assets::MeshLevel& level : chain.levels) {
            levelMeshes.push_back(view.meshes().createMesh(level.mesh));
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

    // Points the flat preview at whichever texture is currently shading the
    // mesh. Called wherever that texture changes IDENTITY or CONTENT, since a
    // descriptor set outlives neither.
    const auto refreshTexturePreview = [&]() {
        if (texturePreview != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(texturePreview);
            texturePreview = VK_NULL_HANDLE;
        }
        if (textureIndex < 0 || textureIndex >= static_cast<int>(textures.size())) {
            return;
        }
        const renderer::GpuTexture& texture = textures[static_cast<std::size_t>(textureIndex)];
        texturePreview = ImGui_ImplVulkan_AddTexture(texture.sampler, texture.image.view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    // Stage G. The image is rebuilt and re-uploaded on every accepted edit, for
    // the same reason the mesh is: a 256x256 document is cheaper to evaluate
    // whole than to work out what changed, and an author moving a panel wants
    // to see the hull change while their hand is still on the number.
    const auto rebuildTexture = [&]() {
        assets::TextureData data;
        std::string error;
        if (!forge::buildTextureData(textureEditor.doc(), data, &error)) {
            textureEditor.setBuildError(error);
            return;
        }
        textureEditor.setBuildError({});
        if (editingTextureIndex < 0 ||
            editingTextureIndex >= static_cast<int>(textures.size())) {
            return;
        }
        // The texture being replaced may still be referenced by a frame the GPU
        // has not finished, exactly as with a mesh.
        context.waitIdle();
        const auto slot = static_cast<std::size_t>(editingTextureIndex);
        view.meshes().destroyTexture(textures[slot]);
        textures[slot] = view.meshes().createTexture(data);
        if (editingTextureIndex == textureIndex) {
            refreshTexturePreview();
        }
    };

    const auto openTextureAt = [&](int index) {
        if (index < 0 || index >= static_cast<int>(loadedTextureEntries.size())) {
            return;
        }
        textureIndex = index;
        refreshTexturePreview();
        if (!forge::isTextureSource(loadedTextureEntries[static_cast<std::size_t>(index)])) {
            // A cooked `.stex` can be looked at and not edited: there is no
            // document behind it to change.
            textureEditor.close();
            editingTextureIndex = -1;
            return;
        }
        std::string textureStatus;
        if (!textureEditor.openFile(
                loadedTextureEntries[static_cast<std::size_t>(index)].path, textureStatus)) {
            status = textureStatus;
            editingTextureIndex = -1;
            return;
        }
        editingTextureIndex = index;
        status = textureStatus;
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
    // The texture the mesh is already wearing is the one to open, so the panel
    // opens on something rather than on "nothing selected" beside a hull that
    // visibly has a texture on it.
    openTextureAt(textureIndex);

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
        // ⚑ And the press is withheld entirely while a level is previewed, for
        // the inverse of E4's own rule. That rule is "an element you can see is
        // pickable"; the markers are not drawn during a preview, so picking one
        // would select something invisible - and a drag would then edit the
        // document while the screen shows a mesh the document does not contain.
        // The camera still gets the press, so orbiting a level still works.
        viewport.leftPressed = leftPressed && !shiftDown && previewLevel == 0;
        viewport.leftDown = leftDown && previewLevel == 0;
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
        // ⚑ Not while a level is previewed. The markers come from the DOCUMENT
        // and the mesh on screen is a generated level, so drawing them together
        // puts 162 crosses over a hull with 80 triangles that has no such
        // points - an editable-looking overlay on something that is not the
        // thing being edited. A preview is a look, not an edit surface.
        if (showPoints && editor.isOpen() && previewLevel == 0) {
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

            // ⚑⚑ THE STATUS BLOCK THAT SITS OUTSIDE THE TABS, AND IT EXISTS TO
            // PAY FOR THEM. Splitting the panel put `Points, edges & faces` and
            // the Report on different tabs, so an author dragging a corner lost
            // the numbers that corner moves. What is here has no other channel:
            // the viewport shows the SHAPE, the part list shows the TREE, and
            // nothing else shows what the mesh now MEASURES.
            //
            // ⚑⚑ THE SLOT HOLDS `volume` RATHER THAN `radius`, AND THE TWO ARE
            // NOT INTERCHANGEABLE - THEY ANSWER DIFFERENT KINDS OF QUESTION.
            // `volume` is a PER-DRAG canary: E5 measured that a wall or a split
            // triangle wound backwards leaves a mesh closed, manifold AND
            // border-free, so the signed volume is the only number here that
            // catches an inside-out extrude, and it moves the instant one
            // happens. `radius` is a STATIC check - authored-versus-measured -
            // and it already has a better home on the `Report` tab, where the
            // [[model]] row prints the authored value beside it, warns when they
            // disagree, and offers stage H's `use measured` button. A number you
            // watch while your hand moves belongs here; a number you reconcile
            // against a def belongs beside the def.
            //
            // ⚑ IT STATES, IT DOES NOT JUDGE - deliberately, and this is the
            // trap it was written around. Colouring "not closed" or a zero
            // volume as a fault would paint `gate_membrane` amber: it is a FILM,
            // so it has a border loop by construction and encloses nothing,
            // which is why Phase 16's invariants exclude it BY NAME. A verdict
            // here would need that same exclusion list, and a status line has no
            // business carrying one. The Report says `closed no / border edges
            // 32` in the plain colour; so does this.
            //
            // ⚑⚑ THE WIDTH DISCIPLINE, KEPT BECAUSE IT COST A RUN TO LEARN. An
            // earlier single line ran `%.4f` with three-space columns, fit the
            // asteroid with 8 px to spare, and TRUNCATED `station.forge` to
            // `... vol 3.783e+05 m3   c` - clipping the health word off the
            // largest mesh in the repo, i.e. losing the status on exactly the
            // asset most likely to have a problem. ImGui does not wrap by
            // default; it draws past the edge and the window clips it, SILENTLY.
            // Hence `PushTextWrapPos` below, and hence checking a format against
            // the widest asset rather than the open one.
            //
            // ⚑⚑ TWO LINES, AND THE BUDGET IS MEASURED RATHER THAN ESTIMATED:
            // the content region is 350 px at 7.00 px/char, i.e. 50 characters.
            // These two lines fit one line's worth of text TODAY - but the
            // worst case the FORMAT can produce does not: a large non-manifold
            // mesh ("1068 tri   vol 3.783e+05 m3   not manifold   32 border
            // edges") needs 420. ⚑ Collapsing to one line would therefore fit
            // by coincidence of which asset happens to be abnormal, which is
            // J2's "checked against one asset" lesson wearing the layout's
            // clothes. Size against what the format can emit, not against what
            // the repo currently holds. Two lines are grouped instead: what the
            // mesh MEASURES above, what it IS below - and the fixed height is
            // what keeps the tab bar from moving between assets.
            //
            // ⚑⚑ AND THE BORDER COUNT IS NOT REDUNDANT WITH `closed`, WHICH IS
            // WHY IT EARNS THE SECOND LINE. `isClosed()` asks that every edge
            // carry exactly two faces; `borderEdgeCount()` counts edges carrying
            // exactly one. An edge with THREE faces makes a mesh not-closed with
            // ZERO border edges - so "open" alone cannot tell a hole from a
            // non-manifold junction, and the count is what separates them.
            ImGui::PushTextWrapPos(0.0f);
            if (openIndex >= 0 || editor.isOpen()) {
                ImGui::TextDisabled("%u tri   vol %.4g m3", report.triangles,
                                    report.signedVolume);
                ImGui::TextDisabled("%s   %u border edge%s",
                                    !report.manifold ? "not manifold"
                                    : report.closed  ? "closed"
                                                     : "open",
                                    report.borderEdges, report.borderEdges == 1 ? "" : "s");
            } else {
                ImGui::TextDisabled("no mesh open");
                // ⚑ Holds the second line even with nothing open, so the status
                // block is a FIXED height and the tab bar never moves. A panel
                // whose furniture shifts with state is the trap this file has
                // now met three times from the other side ("save moves"), and
                // here it would move the one row every drive recipe clicks.
                ImGui::NewLine();
            }
            ImGui::PopTextWrapPos();
            ImGui::Separator();

            // ⚑⚑ STAGE J: FOUR TABS, AND THE COUNT WAS DECIDED BY ARITHMETIC.
            // Every stage since C added a section to one scrolling column, and by
            // stage I the panel was 2,901 px of content in an 860 px window - so
            // `Textures`, at 750 px the LARGEST section in the tool, began 903 px
            // below the fold. The tabs are not decoration: a three-way split
            // (mesh / texture / view) leaves the mesh tab still needing 929 px of
            // scroll, because `Parts` and `Def rows` are the two biggest mesh
            // sections and end up together. Splitting the mesh half at the seam
            // these sections already describe in prose - what you EDIT above, what
            // the edit MEASURES below - gets the worst tab down to 144 px.
            //
            // ⚑ Nothing switches tab on its own. Opening a `.tex` does not jump
            // here and a bake does not jump to the Report: the tab belongs to the
            // author, not to the tool. A panel that moves under the hand is the
            // failure the "save moves" trap has now been recorded three times.
            if (ImGui::BeginTabBar("##sections")) {
                // What you author. The mesh list is here rather than above the
                // bar because opening a mesh is the first thing you do TO a mesh.
                if (ImGui::BeginTabItem("Mesh")) {
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
                        // ⚑ It can change the document since E5: a split and an extrude
                        // are presses rather than drags, so this panel is a third place
                        // an edit can come from and it needs the same rebuild.
                        if (points.drawPanel(editor)) {
                            rebuildFromEditor(/*reframe=*/false);
                        }
                    }
                    ImGui::EndTabItem();
                }

                // What the mesh you authored MEASURES, what NAMES it, and what it
                // decimates to - three readings of the thing edited on the first
                // tab, which is why they travel together.
                if (ImGui::BeginTabItem("Report")) {
                    if ((openIndex >= 0 || editor.isOpen()) &&
                        ImGui::CollapsingHeader("Report", ImGuiTreeNodeFlags_DefaultOpen)) {
                        const core::Vec3 size = report.boundsMax - report.boundsMin;
                        ImGui::Text("triangles      %u", report.triangles);
                        // Corners and points are different numbers and the gap is the
                        // shading: a hard edge splits one point into several corners.
                        ImGui::Text("render verts   %u", report.renderVertices);
                        ImGui::Text("welded points  %u", report.positions);
                        ImGui::Text("bounds min     %8.3f %8.3f %8.3f",
                                    static_cast<double>(report.boundsMin.x),
                                    static_cast<double>(report.boundsMin.y),
                                    static_cast<double>(report.boundsMin.z));
                        ImGui::Text("bounds max     %8.3f %8.3f %8.3f",
                                    static_cast<double>(report.boundsMax.x),
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

                    }

                    // ⚑ Stage H, and it is deliberately its own section rather than a
                    // tail on the Report. The Report describes the MESH; these rows are
                    // the CONTENT that names it, and until this stage the tool could
                    // print them and not change one - which is how the four radius
                    // mismatches in this game came to be reported for five stages by a
                    // warning whose only remedy was a text editor.
                    if (openIndex >= 0 &&
                        ImGui::CollapsingHeader("Def rows", ImGuiTreeNodeFlags_DefaultOpen)) {
                        // The panel measures against the editor's own validated reading
                        // of the text, so neither call needs the boot-time catalog.
                        (void)defEditor.drawModelRows(meshEntries[static_cast<std::size_t>(openIndex)],
                                                      report, textureStems);
                        // Stage H3: the content that names those models. Below them
                        // because a ship row is a consequence of the model row above it,
                        // exactly as Points sits below Parts.
                        ImGui::SeparatorText("in the game");
                        (void)defEditor.drawContentRows();
                        if (ImGui::Button("save defs")) {
                            if (defEditor.save(defStatus)) {
                                // The boot catalog is what every other panel reads, so
                                // it has to follow the file rather than drift from it.
                                std::string defError;
                                if (!forge::loadModelCatalog(dataDirectory, defs, &defError)) {
                                    defStatus = defError;
                                }
                                modelMatches = forge::matchModels(
                                    defs, meshEntries[static_cast<std::size_t>(openIndex)], report);
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("undo def")) {
                            if (!defEditor.undo()) {
                                defStatus = "nothing to undo";
                            }
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", defEditor.dirty() ? "* unsaved" : "saved");
                        ImGui::TextDisabled("%s", defStatus.c_str());
                    }

                    // Stage F. Below the Report because a level is a consequence of the
                    // mesh the Report describes, and because the first number an author
                    // wants is the one they are giving up.
                    if ((openIndex >= 0 || editor.isOpen()) &&
                        ImGui::CollapsingHeader("Levels", ImGuiTreeNodeFlags_DefaultOpen)) {
                        const float focal = ui::focalLength(static_cast<float>(window.height()),
                                                            std::tan(forge::kCameraVerticalFov * 0.5f));
                        if (ImGui::RadioButton("authored", previewLevel == 0)) {
                            previewLevel = 0;
                            frameOpenMesh();
                        }
                        for (std::size_t i = 0; i < chain.levels.size(); ++i) {
                            const int number = static_cast<int>(i) + 1;
                            char label[32];
                            std::snprintf(label, sizeof(label), "lod%d", number);
                            ImGui::SameLine();
                            if (ImGui::RadioButton(label, previewLevel == number)) {
                                previewLevel = number;
                                // ⚑ "At the distance the LOD is for" is the whole point
                                // of the preview, and it is not a framing: the camera
                                // stands exactly where this level takes over, which is
                                // where the projected radius crosses its threshold.
                                // Seeing a decimated hull filling the screen proves
                                // nothing, because that is not where it is ever drawn.
                                const float switchPixels = assets::kLevelSwitchPixels
                                    [i < std::size(assets::kLevelSwitchPixels)
                                         ? i
                                         : std::size(assets::kLevelSwitchPixels) - 1];
                                const core::Vec3 center = (report.boundsMin + report.boundsMax) * 0.5f;
                                camera.placeAt(center, report.boundingRadius * focal / switchPixels);
                            }
                        }

                        ImGui::Separator();
                        for (std::size_t i = 0; i < chain.levels.size(); ++i) {
                            const assets::MeshLevel& level = chain.levels[i];
                            const float switchPixels =
                                assets::kLevelSwitchPixels[i < std::size(assets::kLevelSwitchPixels)
                                                               ? i
                                                               : std::size(assets::kLevelSwitchPixels) - 1];
                            ImGui::Text("lod%d  %u tri (%.0f%%)  %zu B", static_cast<int>(i) + 1,
                                        level.triangles,
                                        report.triangles > 0
                                            ? 100.0 * level.triangles / static_cast<double>(report.triangles)
                                            : 0.0,
                                        level.cookedBytes);
                            // Signed, both of them: a level that GREW its volume is as
                            // wrong as one that shrank, and the radius growing outward
                            // is the one that pushes the hull past its collision sphere.
                            ImGui::TextDisabled("      volume %+.2f%%   radius %+.2f%%   from %.0f m",
                                                level.volumeDrift * 100.0, level.radiusDrift * 100.0,
                                                static_cast<double>(report.boundingRadius * focal /
                                                                    switchPixels));
                        }

                        // ⚑ Always shown, whether or not anything was generated. A
                        // refusal is the normal answer here - four of the seven
                        // committed meshes are under the floor - and an author who is
                        // told nothing cannot tell "too small to be worth it" from
                        // "the tool is broken".
                        ImGui::PushTextWrapPos(0.0f);
                        if (chain.levels.empty()) {
                            ImGui::TextDisabled("no levels: %s", chain.stopReason.c_str());
                        } else {
                            ImGui::TextDisabled("chain stops here: %s", chain.stopReason.c_str());
                        }
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::EndTabItem();
                }

                // The other document. It gets a whole tab because it IS a whole
                // editor - a peer of the mesh half, not a section of it.
                if (ImGui::BeginTabItem("Texture")) {
                    // Stage G. Selecting a texture here does BOTH things - it shades the
                    // open mesh with it and, if it is a `.tex`, opens it for editing -
                    // because two lists that each did half would be two answers to
                    // "which texture am I looking at".
                    if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (ImGui::BeginChild("##textures", {0.0f, 90.0f}, ImGuiChildFlags_Borders)) {
                            for (int i = 0; i < static_cast<int>(textureLabels.size()); ++i) {
                                if (ImGui::Selectable(textureLabels[static_cast<std::size_t>(i)].c_str(),
                                                      i == textureIndex)) {
                                    openTextureAt(i);
                                }
                            }
                        }
                        ImGui::EndChild();
                        if (texturePreview != VK_NULL_HANDLE) {
                            // ⚑⚑ STAGE I: 1:1, AND THE SIZE IS THE FEATURE. This shipped
                            // at a flat 200 px for a 256 px document, which puts 1.28
                            // texture pixels under every screen pixel - so a drag could
                            // only produce offsets of round(n * 1.28), and 56 of the 257
                            // possible offsets could not be produced at all. The first
                            // one missing is 2. Every value in this document is an exact
                            // integer, and a fractional preview is what made that untrue.
                            if (textureEditor.isOpen()) {
                                if (textureEditor.drawPreview(texturePreview,
                                                              ImGui::GetContentRegionAvail().x)) {
                                    rebuildTexture();
                                }
                                ImGui::TextDisabled("as cooked (BC1) - click a shape, drag to move it");
                            } else {
                                // A cooked texture has no document behind it, so there is
                                // nothing to pick: it stays the picture it always was.
                                ImGui::Image(reinterpret_cast<ImTextureID>(texturePreview),
                                             {200.0f, 200.0f});
                                ImGui::SameLine();
                                ImGui::TextDisabled("as cooked\n(BC1)");
                            }
                        }
                        if (ImGui::Button("new texture")) {
                            textureEditor.openNew(assetsDirectory + "/textures");
                            editingTextureIndex = -1;
                            rebuildTexture();
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", textureEditor.isOpen()
                                                      ? "editing the selected source"
                                                      : "cooked textures are read-only");
                        ImGui::Separator();
                        if (textureEditor.draw()) {
                            rebuildTexture();
                        }
                    }
                    ImGui::EndTabItem();
                }

                // Neither document: how you are LOOKING at whichever one is open.
                if (ImGui::BeginTabItem("View")) {
                    if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
                        const auto shading = static_cast<std::size_t>(textureIndex);
                        if (ImGui::BeginCombo("texture", textureLabels[shading].c_str())) {
                            for (int i = 0; i < static_cast<int>(textureLabels.size()); ++i) {
                                if (ImGui::Selectable(textureLabels[static_cast<std::size_t>(i)].c_str(),
                                                      i == textureIndex)) {
                                    openTextureAt(i);
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
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
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
        // Stage F: the preview draws a generated level in the authored mesh's
        // place. One item either way - the level is a mesh like any other, and
        // making the viewport learn about levels would be a second rule.
        renderer::GpuMesh* drawn = &openMesh;
        if (previewLevel > 0 && static_cast<std::size_t>(previewLevel) <= levelMeshes.size()) {
            drawn = &levelMeshes[static_cast<std::size_t>(previewLevel) - 1];
        }
        if (drawn->indexCount > 0) {
            frame.items.push_back({drawn, &textures[static_cast<std::size_t>(textureIndex)],
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
    // Before the ImGui host goes down: the set belongs to its descriptor pool.
    if (texturePreview != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texturePreview);
        texturePreview = VK_NULL_HANDLE;
    }
    for (renderer::GpuMesh& level : levelMeshes) {
        view.meshes().destroyMesh(level);
    }
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
