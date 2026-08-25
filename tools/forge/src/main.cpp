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
#include "list_layout_style.hpp"
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
// ⚑ The only place this tool reaches into ImGui's internals, and it is for the
// DockBuilder API, which lives there BY DESIGN rather than by oversight: it is
// how an application builds a default dock layout in code. Everything else here
// is public API.
#include <imgui_internal.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

// --open <substring>: open the first mesh whose stem contains `substring`
// instead of the first row in the list.
//
// ⚑⚑ PERMANENT BECAUSE IT HAS BEEN HAND-ADDED AND DELETED TWICE ALREADY, at
// stages M and N, and this is the third stage that needs it. A drive that has
// to CLICK a mesh row needs that row's pixel, and since stage M list heights
// are `clamp(content, minRows, share x windowHeight)` - so the row moves with
// the window size AND with which document is open. Every such coordinate has
// gone stale at least once (the recorded count of "the UI element your edit
// moves is the one the drive clicks" is six). This reaches any document with
// no coordinates at all, and being permanent it is exercised by the same
// builds as everything else rather than being re-derived from memory each
// time. It is also how `forge.smoke` could one day open something other than
// the alphabetically-first asset.
const char* parseOpenStem(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--open") == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
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

// ⚑⚑ WHICH PANELS ARE OPEN, PERSISTED INTO `forge.ini` ALONGSIDE THE LAYOUT.
// Reported by the user: closing a panel and relaunching brought it back.
//
// ⚑⚑ IT IS THE SAME LESSON AS THE DOCK LAYOUT, ONE LEVEL UP - "a round trip has
// two halves" applied to a SECOND PIECE OF STATE. Stage K taught ImGui to
// remember where the panels are; it never taught anything to remember WHETHER
// they are. `Begin(name, p_open)` treats `p_open` as the APPLICATION's bool:
// ImGui saves a window's position, size, dock node and selected tab, and never
// that flag - so the layout round-tripped perfectly while the visibility beside
// it silently reset every launch. Fixing one piece of a file's contents is not
// fixing the file.
//
// ⚑ A settings HANDLER rather than a file of our own, because the alternative
// is two files with two lifetimes that have to agree: this writes into the same
// `forge.ini`, at the same moment, and is flushed by the same DestroyContext.
// ⚑ It must be registered BEFORE the first NewFrame - that is when ImGui loads
// the ini - which is why it is installed right after the host comes up.
struct PanelToggle
{
    const char* name; // exactly the window name, so the ini reads as itself
    bool* shown;
};

struct PanelToggles
{
    PanelToggle items[4];

    [[nodiscard]] bool* find(const char* name) const
    {
        for (const PanelToggle& item : items) {
            if (std::strcmp(item.name, name) == 0) {
                return item.shown;
            }
        }
        return nullptr;
    }
};

void* panelsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
{
    // One entry, `[ForgePanels][Visible]`. An unknown entry returns null and
    // ImGui skips its lines rather than handing them to ReadLine.
    return std::strcmp(name, "Visible") == 0 ? handler->UserData : nullptr;
}

void panelsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
{
    auto* toggles = static_cast<PanelToggles*>(entry);
    char name[32] = {};
    int shown = 0;
    if (std::sscanf(line, "%31[^=]=%d", name, &shown) != 2) {
        return;
    }
    // ⚑ An unrecognised name is IGNORED rather than treated as an error: a
    // panel renamed or removed in a later stage must not stop the rest of the
    // file loading, and an older ini must still open in a newer tool.
    if (bool* flag = toggles->find(name)) {
        *flag = shown != 0;
    }
}

void panelsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer)
{
    const auto* toggles = static_cast<const PanelToggles*>(handler->UserData);
    buffer->appendf("[%s][Visible]\n", handler->TypeName);
    for (const PanelToggle& item : toggles->items) {
        buffer->appendf("%s=%d\n", item.name, *item.shown ? 1 : 0);
    }
    buffer->append("\n");
}

// ⚑⚑ SESSION 14's NOTE 1: AN ASSET LIST DRAWN AS COLLAPSIBLE RUNS RATHER THAN
// ONE FLAT COLUMN. The mesh list was 21 rows of which 13 were cooked output -
// 62% of it build product that cannot be edited - and the ratio DEGRADES ON ITS
// OWN, because stage F emits a `.lodN.smesh` sibling per level, so every new
// chained model adds up to two more rows nobody opens. The fixed 190 px box
// showed ~11 rows, so four more authored meshes would have pushed the EDITABLE
// rows into a scroll owned by output.
//
// ⚑ It needs no sort and no index: `listMeshes` already emits its categories in
// contiguous runs, so one pass finds each run's end, and knowing the end is
// what lets the header say how much is hidden while it is closed.
//
// ⚑⚑ THE `###` IS LOAD-BEARING, NOT STYLE. ImGui keys a tree node's open/closed
// state on its LABEL, and this label carries a COUNT - so without a stable ID
// suffix, cooking a new asset (13 -> 15) would silently re-open a section the
// author had closed. The visible text may change; the identity may not.
//
// ⚑ Lives here rather than in `mesh_library.cpp` because that file is compiled
// standalone into `sol_forge_tests`, which links no ImGui at all (see the
// tools/forge/CMakeLists.txt comment). A draw helper there would break the
// suite's linkage - Phase 15's lesson, applied before the promise this time.
// Returns the row clicked this frame, or -1.
// ⚑⚑ STAGE M: THIS LIST IS THE ONE WHOSE HEIGHT IS NOT A ROW COUNT TIMES A
// PITCH, AND THAT IS WHY IT CARRIES STATE. Two things make it awkward, and both
// were found by predicting a number and measuring a different one.
//
// (1) A collapsed group contributes ONE row - its header - instead of all of
// its entries, and whether a group is open is not known until `CollapsingHeader`
// has been called, which happens INSIDE the child whose height must already be
// decided. So `visibleContent` is what the last frame actually submitted, owned
// by the caller and fed back in. A one-frame lag on a height is invisible at
// 60 Hz and self-corrects immediately, because every row is still submitted
// whatever the height - a short child scrolls, it does not skip. Frame one uses
// the floor.
//
// (2) ⚑⚑ THE ROWS ARE NOT ALL THE SAME HEIGHT. A `CollapsingHeader` is a framed
// item at `GetFrameHeightWithSpacing()` (23 px) while an entry is a text row at
// 17. Sizing ten mixed rows as ten uniform ones came out 12 px short and left
// the list still scrolling. So the state is a HEIGHT, accumulated per row from
// that row's own pitch, not a count.
[[nodiscard]] int drawAssetList(const char* id, const std::vector<forge::AssetEntry>& entries,
                                int selected, float share, float& visibleContent)
{
    int clicked = -1;
    const forge::ListMetrics rowMetrics = forge::textRowMetrics();
    const float headerPitch = forge::frameRowMetrics().rowPitch;
    const float height = forge::listHeightForContent(rowMetrics, visibleContent,
                                                     forge::kMinListRows, share,
                                                     ImGui::GetWindowHeight());
    float submitted = 0.0f;
    if (ImGui::BeginChild(id, {0.0f, height}, ImGuiChildFlags_Borders)) {
        std::size_t first = 0;
        while (first < entries.size()) {
            std::size_t end = first;
            while (end < entries.size() && entries[end].group == entries[first].group) {
                ++end;
            }
            char header[96];
            std::snprintf(header, sizeof(header), "%s (%zu)###%s", entries[first].group.c_str(),
                          end - first, entries[first].group.c_str());
            submitted += headerPitch; // the header is a row of the list too, and a taller one
            // Build output arrives closed: it is the majority of the list and
            // the minority of the interest, and it is the half that grows on
            // its own. Everything an author can actually edit arrives open.
            if (ImGui::CollapsingHeader(header,
                                        entries[first].cooked ? 0
                                                              : ImGuiTreeNodeFlags_DefaultOpen)) {
                for (std::size_t row = first; row < end; ++row) {
                    if (ImGui::Selectable(entries[row].label.c_str(),
                                          static_cast<int>(row) == selected)) {
                        clicked = static_cast<int>(row);
                    }
                    submitted += rowMetrics.rowPitch;
                }
            }
            first = end;
        }
        visibleContent = submitted;
    }
    ImGui::EndChild();
    return clicked;
}

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

// The Blender bridge's two bits of path arithmetic (stage L). `mesh_library`
// has both privately and this file cannot reach them; they are four lines each
// and promoting them to the header for one caller would be a worse trade.
[[nodiscard]] std::string forgeFileStem(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Lower-cased, because a drop directory is written to by another program and
// Blender will happily hand back `.GLTF` on a case-insensitive filesystem.
[[nodiscard]] std::string forgeLowerExtension(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string extension = path.substr(dot);
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return extension;
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
    // ⚑⚑ WHERE BLENDER DROPS, AND IT IS NOT UNDER `assets/` (stage L). The
    // cooker scans the source tree RECURSIVELY into one flat output directory
    // keyed on the file STEM, so a `ship.gltf` anywhere beneath it collides with
    // `ship.forge` - and that guard aborts the ENTIRE cook rather than skipping
    // the pair. The glTF is transport: once imported, the `.forge` is the source.
    const std::string inboxDirectory =
        std::strlen(SOL_FORGE_INBOX_DIR) > 0 ? SOL_FORGE_INBOX_DIR : executableDir + "blender-inbox";

    rhi::Swapchain swapchain;
    if (!swapchain.create(context, window.width(), window.height(), /*vsync=*/true)) {
        return EXIT_FAILURE;
    }

    forge::ForgeView view;
    if (!view.initialize(context, swapchain, shaderDirectory.c_str())) {
        return EXIT_FAILURE;
    }

    // ⚑⚑ THE FORGE IS THE ONE CLIENT THAT OPTS INTO BOTH OF THESE, AND THEY ARE
    // A PAIR. Docking makes the panel's sections into windows an author can
    // arrange; the ini is what makes that arrangement survive a launch. Without
    // the second, the first is a REGRESSION - stage J's whole finding was that
    // a cost paid on every launch is paid forever, and rebuilding a layout by
    // hand is a bigger version of the climb J deleted.
    //
    // ⚑ It sits beside the executable, i.e. under `build/`, so it is a build
    // artefact rather than repo litter - which is what "no imgui.ini litter"
    // was protecting, and it still holds for the game.
    sol::ui::ImGuiHost imguiHost;
    sol::ui::HostOptions hostOptions;
    hostOptions.docking = true;
    hostOptions.iniPath = executableDir + "forge.ini";
    if (!imguiHost.initialize(window, context, swapchain.imageFormat(), VK_FORMAT_D32_SFLOAT,
                              swapchain.imageCount(), hostOptions)) {
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
    // Stage M: the height the mesh list actually submitted last frame. Not a row
    // count, because a collapsed group is one row and a group header is taller
    // than an entry. See drawAssetList.
    float meshListContent = 0.0f;
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

    // ⚑⚑ SESSION 14's NOTE 2. Stage J's four TABS are four WINDOWS now, and the
    // difference is that an author can have two of them at once. J shipped one
    // known cost - `Points, edges & faces` and the `Report` could no longer be
    // seen together - and the summary block was built to pay for it. Docking
    // pays for it properly: the default layout puts them side by side.
    //
    // ⚑ Once a window can be CLOSED, something has to be able to re-open it,
    // and once a layout PERSISTS, something has to be able to undo it. Those
    // two needs are what the menu bar is for; they did not exist before this
    // stage, which is why the tool had no menu until now.
    bool showMesh = true;
    bool showReport = true;
    bool showTexture = true;
    bool showView = true;
    bool resetLayout = false;
    bool focusMeshPending = false;

    // ⚑ Registered here rather than inside the host, because WHICH panels exist
    // is the Forge's business and the host is shared with the game.
    PanelToggles panelToggles = {{{"Mesh", &showMesh},
                                  {"Report", &showReport},
                                  {"Texture", &showTexture},
                                  {"View", &showView}}};
    // ⚑ ImGui only rewrites the ini when something MARKS it dirty, and it has no
    // idea these bools exist - so a toggle would be forgotten unless the change
    // is reported. Compared per frame rather than at each of the several places
    // a panel can close (menu item, window X, `Reset layout`), because a rule
    // spread over three call sites is a rule that gets missed at one of them.
    bool wasShown[4] = {showMesh, showReport, showTexture, showView};
    {
        ImGuiSettingsHandler handler;
        handler.TypeName = "ForgePanels";
        handler.TypeHash = ImHashStr("ForgePanels");
        handler.ReadOpenFn = &panelsReadOpen;
        handler.ReadLineFn = &panelsReadLine;
        handler.WriteAllFn = &panelsWriteAll;
        handler.UserData = &panelToggles;
        // ⚑ Copied by value into the context, so the local dying here is fine -
        // but `UserData` is NOT copied, and `panelToggles` must outlive the
        // context. It does: both live for the whole of main().
        ImGui::AddSettingsHandler(&handler);
    }

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
    // --- the Blender bridge (stage L) ---------------------------------------
    //
    // ⚑ A POLL RATHER THAN A WATCH, AND RATHER THAN AN IPC. `fileModificationTime`
    // and `listFiles` are already in the platform layer, so noticing a drop costs
    // no new platform surface and nothing Windows-shaped above it (AGENTS.md 4).
    // A socket or a named pipe would buy latency this does not need: the author
    // is alt-tabbing out of Blender, which is hundreds of milliseconds anyway.
    struct InboxEntry
    {
        std::string path;
        std::uint64_t modified = 0;
    };
    std::vector<InboxEntry> inboxSeen;
    int inboxPollCountdown = 0;
    std::string inboxStatus;
    bool inboxAuto = true;

    const auto importFromInbox = [&](const std::string& gltfPath) {
        const std::string stem = forgeFileStem(gltfPath);
        const std::string target = assetsDirectory + "/meshes/" + stem + ".forge";

        // ⚑⚑ THE BASE IS THE OPEN DOCUMENT WHEN THERE IS ONE, AND THE FILE ONLY
        // OTHERWISE. THIS IS THE FIX FOR A DEFECT A USER FOUND: they imported,
        // added a part in the Forge, re-sent from Blender, and their part was
        // gone. The file on disk is NOT the document - it is the document as of
        // the last save - so merging into it discards every edit since, and
        // `openMeshAt` below then reloads that loss over the top.
        //
        // ⚑ The first answer was to ask `dirty()` and refuse, and it was the
        // weaker one twice over: it depends on every edit path remembering to
        // raise that flag (`addPrimitive` did not, which is how this got out),
        // and it makes the author alt-tab and save to accept an import they just
        // asked for. Merging into what is on screen needs no flag to be correct.
        const bool editingTarget = editor.isOpen() && editor.path() == target;
        assets::ForgeDoc doc;
        if (editingTarget) {
            doc = editor.doc();
        } else {
            std::vector<std::uint8_t> bytes;
            if (platform::readFileBytes(target.c_str(), bytes)) {
                std::string parseError;
                if (!assets::parseForge(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                                        target.c_str(), doc, &parseError)) {
                    inboxStatus = "cannot merge into " + stem + ".forge: " + parseError;
                    SOL_LOG_ERROR("forge: %s", inboxStatus.c_str());
                    return false;
                }
            }
        }

        forge::ImportOutcome outcome;
        std::string importError;
        if (!forge::importGltfIntoDoc(gltfPath, doc, outcome, &importError)) {
            inboxStatus = importError;
            SOL_LOG_ERROR("forge: %s", inboxStatus.c_str());
            return false;
        }

        const std::string text = assets::writeForge(doc);
        if (!platform::writeFileBytes(target.c_str(), text.data(), text.size())) {
            inboxStatus = "cannot write " + target;
            SOL_LOG_ERROR("forge: %s", inboxStatus.c_str());
            return false;
        }

        inboxStatus = stem + ".forge  " + std::to_string(outcome.added.size()) + " added, " +
                      std::to_string(outcome.replaced.size()) + " replaced";
        if (!outcome.kept.empty()) {
            inboxStatus += ", " + std::to_string(outcome.kept.size()) + " kept";
        }
        SOL_LOG_INFO("forge: imported %s -> %s", gltfPath.c_str(), inboxStatus.c_str());

        // The list was read at startup and an import may have just added to it.
        meshEntries = forge::listMeshes(assetsDirectory, cookedDirectory);

        if (editingTarget) {
            // ⚑ ADOPTED RATHER THAN RE-OPENED. Re-reading the file would work
            // now that it holds the merge, but it would throw away the undo
            // stack and the selection for no reason - and it would make the
            // import the one edit in this tool a person cannot take back.
            editor.adoptDoc(std::move(doc));
            points.refresh(editor.doc());
            rebuildFromEditor(/*reframe=*/false);
        } else {
            for (std::size_t i = 0; i < meshEntries.size(); ++i) {
                if (meshEntries[i].path == target ||
                    (meshEntries[i].stem == stem && forge::isPartSource(meshEntries[i]))) {
                    // ⚑ Opening it IS the feature - the author pressed a button
                    // in Blender to see it here, so this is not the "save moves"
                    // trap that kept stage J from switching tabs on its own. It
                    // holds back only for an unsaved edit to something ELSE.
                    if (!editor.dirty()) {
                        openMeshAt(static_cast<int>(i));
                    }
                    break;
                }
            }
        }
        status = inboxStatus;
        return true;
    };

    // Every drop that is new or has changed since the last look. Returns how
    // many were imported.
    const auto pollInbox = [&](bool announceEmpty) {
        std::vector<std::string> files = platform::listFiles(inboxDirectory.c_str());
        std::sort(files.begin(), files.end());
        int imported = 0;
        int seen = 0;
        for (const std::string& path : files) {
            const std::string lower = forgeLowerExtension(path);
            if (lower != ".gltf" && lower != ".glb") {
                continue;
            }
            ++seen;
            const std::uint64_t modified = platform::fileModificationTime(path.c_str());
            const auto existing = std::find_if(inboxSeen.begin(), inboxSeen.end(),
                                               [&path](const InboxEntry& e) { return e.path == path; });
            if (existing != inboxSeen.end() && existing->modified == modified) {
                continue;
            }
            if (importFromInbox(path)) {
                ++imported;
            }
            // Recorded either way: a drop that fails to import must not be
            // retried sixty times a second for the rest of the session.
            if (existing != inboxSeen.end()) {
                existing->modified = modified;
            } else {
                inboxSeen.push_back({path, modified});
            }
        }
        if (imported == 0 && announceEmpty) {
            inboxStatus = seen == 0 ? "nothing in " + inboxDirectory
                                    : std::to_string(seen) + " drop(s), none changed";
            status = inboxStatus;
        }
        return imported;
    };

    // ⚑ The FIRST poll only takes a census: anything already sitting in the
    // inbox at launch is a drop from a previous session that has already been
    // imported, and re-importing it would undo whatever the author did in the
    // Forge afterwards. Only a file that changes WHILE the tool is running is a
    // new send from Blender.
    for (const std::string& path : platform::listFiles(inboxDirectory.c_str())) {
        const std::string lower = forgeLowerExtension(path);
        if (lower == ".gltf" || lower == ".glb") {
            inboxSeen.push_back({path, platform::fileModificationTime(path.c_str())});
        }
    }
    SOL_LOG_INFO("forge: watching %s (%zu drop(s) already there)", inboxDirectory.c_str(),
                 inboxSeen.size());

    // ⚑ `--open` picks the row; with no match, or no flag, the first row as
    // before. A miss is logged rather than silent, because a drive that
    // mistypes a stem would otherwise measure the alphabetically-first asset
    // and read as a feature that quietly did nothing.
    int openAt = meshEntries.empty() ? -1 : 0;
    if (const char* wanted = parseOpenStem(argc, argv); wanted != nullptr) {
        bool found = false;
        for (std::size_t i = 0; i < meshEntries.size() && !found; ++i) {
            if (meshEntries[i].stem.find(wanted) != std::string::npos &&
                forge::isPartSource(meshEntries[i])) {
                openAt = static_cast<int>(i);
                found = true;
            }
        }
        if (!found) {
            SOL_LOG_WARN("forge: --open '%s' matched no editable mesh", wanted);
        }
    }
    openMeshAt(openAt);
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
        viewport.middleDown = middleDown;
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
            } else if (window.isKeyDown(platform::Key::Num4)) {
                // Stage N. The fourth digit is the one that selects rather than
                // edits - see PointTool::Mode for why it is a mode at all.
                points.setMode(forge::PointTool::Mode::Part);
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

        // ⚑⚑⚑ THE OVERLAY MUST BE BUILT AGAINST THE CAMERA THIS FRAME WILL
        // RENDER WITH, NOT THE ONE THE USER PICKED AGAINST. Those are two
        // different matrices whenever the camera moved this frame, and the
        // difference is a whole orbit step. `viewport.view` above is deliberately
        // the PRE-orbit camera, because that is the image the cursor was over
        // when the press was read and picking must agree with what was on
        // screen; but everything below here is geometry handed to the renderer,
        // which draws it with `camera.view()` AFTER the orbit (see `frame.view`).
        //
        // ⚑⚑ IT WENT UNNOTICED UNTIL STAGE O BECAUSE ALMOST NOTHING HERE IS
        // VIEW-DEPENDENT. A point cross and an edge line are world-space
        // positions that come out right whichever matrix computed them - the
        // stale view changes nothing about where they land. Stage N's part box
        // is the exception, and the exception is exactly the thing that makes it
        // work: it RELOCATES its corners to 0.12 m from the eye to beat the
        // depth test, and that is only correct when drawn from the eye it was
        // scaled about.
        //
        // ⚑⚑ MEASURED, BECAUSE THE MAGNITUDE IS THE WHOLE POINT: one orbit step
        // moves the eye 0.3739 m, the pull magnifies any eye error by
        // `distance / 0.12` = 41x on `freighter_cockpit`, so the box was landing
        // 15.5 m from its part on a model 4.98 m away and 7.4 m across - flung
        // clean off the mesh rather than lagging it. Zero error on a still
        // camera, which is why every headless drive and every static screenshot
        // passed and a person rotating the view found it in a minute.
        // ⚑ A SEPARATE VIEWPORT RATHER THAN AN ASSIGNMENT INTO THE ONE ABOVE, so
        // `viewport` means exactly one thing for its whole life - the image the
        // press was read against - and a pick added below here cannot silently
        // acquire the wrong camera.
        forge::PointTool::Viewport overlay = viewport;
        overlay.view = camera.view();
        overlay.cameraDistance = camera.distance();

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
        // ⚑⚑ STAGE O: TAKEN UNCONDITIONALLY, OUTSIDE THE GUARD BELOW, AND THAT
        // IS THE POINT OF IT. The read is what clears the value, so a frame
        // that skips the markers - points hidden, no document, a level being
        // previewed - must still take it, or the next frame that DOES draw
        // inherits whichever row the cursor was over before the panel stopped
        // reporting. See `PartEditor::takeHoveredPart`.
        const std::size_t rowHoverPart = editor.takeHoveredPart();
        if (showPoints && editor.isOpen() && previewLevel == 0) {
            points.drawMarkers(view.debugDraw(), overlay, editor.selectedPart(), rowHoverPart);
        }

        // ⚑ Throttled to about twice a second rather than run every frame: it
        // is a directory listing plus a stat per drop, and the thing it is
        // waiting for is a human alt-tabbing out of Blender.
        if (inboxAuto && --inboxPollCountdown <= 0) {
            inboxPollCountdown = 30;
            (void)pollInbox(/*announceEmpty=*/false);
        }

        // --- panel ---
        imguiHost.beginFrame();

        // ⚑⚑ THE MENU BAR EXISTS BECAUSE DOCKING CREATED TWO NEEDS THAT DID NOT
        // EXIST BEFORE IT: a closed window has to be re-openable, and a
        // persisted layout has to be undoable. Both are consequences of the
        // feature rather than decoration on it, which is why the tool went nine
        // stages without a menu and needs one now.
        //
        // ⚑ It is also one of three pieces of chrome that CANNOT be undocked,
        // closed or covered - the others being the toolbar and the status bar
        // below, both of which need exactly that property.
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Panels")) {
                ImGui::MenuItem("Mesh", nullptr, &showMesh);
                ImGui::MenuItem("Report", nullptr, &showReport);
                ImGui::MenuItem("Texture", nullptr, &showTexture);
                ImGui::MenuItem("View", nullptr, &showView);
                ImGui::EndMenu();
            }
            // ⚑ The bridge gets a menu rather than a button because it is
            // mostly meant to be invisible: the author presses `Send to Forge`
            // in Blender and the mesh is here. What needs a home is the state
            // of the thing - where it is watching, whether it is, and a manual
            // poke for when a drop is already sitting there from last session.
            if (ImGui::BeginMenu("Blender")) {
                ImGui::MenuItem("Watch the inbox", nullptr, &inboxAuto);
                if (ImGui::MenuItem("Import now")) {
                    // Forgets what it has seen, so this re-imports a drop that
                    // is already there - which is the whole point of asking.
                    inboxSeen.clear();
                    (void)pollInbox(/*announceEmpty=*/true);
                }
                ImGui::Separator();
                ImGui::TextDisabled("drop .gltf into");
                ImGui::TextDisabled("%s", inboxDirectory.c_str());
                if (!inboxStatus.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", inboxStatus.c_str());
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Layout")) {
                // ⚑ The escape hatch persistence makes mandatory. A layout that
                // survives a launch also survives being wrecked, and without
                // this the only remedy is deleting a file the author has never
                // been told about.
                if (ImGui::MenuItem("Reset layout")) {
                    resetLayout = true;
                    showMesh = showReport = showTexture = showView = true;
                }
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        // ⚑⚑ THE PRIMITIVES TOOLBAR. `add part` has been a COMBO since stage D -
        // two clicks and a read to put a box down, for the one action an author
        // repeats more than any other. A row of buttons is one click.
        //
        // ⚑ It is DERIVED from `forgePrimitives()` rather than listed, so the
        // toolbar cannot drift from the vocabulary the document format actually
        // has; adding a primitive to the enum puts a button here for free.
        //
        // ⚑ `mesh` is the one deliberate omission, and it is not an oversight:
        // `mesh` is what `bake` PRODUCES - literal vertices and indices - so a
        // button that added an empty one would hand the author an invisible part
        // with nothing in it. The `add part` combo still offers it, so nothing
        // has been taken away.
        //
        // ⚑ Both bars are submitted BEFORE the dockspace, because a side bar
        // reports its size into the viewport's work area and the dockspace is
        // sized from what is left.
        const ImGuiWindowFlags kBarFlags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        const float toolbarHeight =
            ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
        if (ImGui::BeginViewportSideBar("##toolbar", ImGui::GetMainViewport(), ImGuiDir_Up,
                                        toolbarHeight, kBarFlags)) {
            ImGui::TextDisabled("add");
            // ⚑ Disabled rather than hidden when nothing is open: a toolbar whose
            // buttons come and go is a toolbar whose positions move, which is the
            // "save moves" trap in its fourth costume.
            ImGui::BeginDisabled(!editor.isOpen());
            for (const assets::ForgePrimitive primitive : assets::forgePrimitives()) {
                if (primitive == assets::ForgePrimitive::Mesh) {
                    continue;
                }
                const char* const name = assets::forgePrimitiveName(primitive);
                ImGui::SameLine();
                if (ImGui::Button(name)) {
                    if (editor.addPrimitive(primitive)) {
                        rebuildFromEditor(/*reframe=*/false);
                        status = std::string("added ") + name;
                    }
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::End();

        // ⚑⚑ THE STATUS BLOCK, WHICH IS ITS OWN BAR ALONG THE BOTTOM. It has now
        // been in three places: above the tab bar (J2-J5), briefly in the menu
        // bar, and here. Its CONTENT has not changed since J5 and is the one part
        // of the Forge a person has signed off - what keeps moving is only where
        // a full-width line can live.
        //
        // ⚑ One line, because the two-line break was never a preference: it was a
        // 350 px panel at 7.00 px/char against a 351 px worst case, J4's "over by
        // exactly one pixel". A bar spanning the window has no such constraint.
        //
        // ⚑ It belongs in the CHROME rather than in a window for the reason it
        // was written at all: it has to be visible while the author's hand is
        // moving, and every window can now be closed, hidden behind a tab or
        // dragged off. A side bar cannot be any of those - `BeginViewportSideBar`
        // sets NoDocking itself.
        //
        // ⚑⚑ THE SLOT HOLDS `volume` RATHER THAN `radius`, AND THE TWO ARE NOT
        // INTERCHANGEABLE - THEY ANSWER DIFFERENT KINDS OF QUESTION. `volume` is
        // a PER-DRAG canary: E5 measured that a wall or a split triangle wound
        // backwards leaves a mesh closed, manifold AND border-free, so the signed
        // volume is the only number here that catches an inside-out extrude, and
        // it moves the instant one happens. `radius` is a STATIC check -
        // authored-versus-measured - and it already has a better home in the
        // `Report`, where the [[model]] row prints the authored value beside it,
        // warns when they disagree, and offers stage H's `use measured` button. A
        // number you watch while your hand moves belongs here; a number you
        // reconcile against a def belongs beside the def.
        //
        // ⚑ IT STATES, IT DOES NOT JUDGE - deliberately, and this is the trap it
        // was written around. Colouring "not closed" or a zero volume as a fault
        // would paint `gate_membrane` amber: it is a FILM, so it has a border
        // loop by construction and encloses nothing, which is why Phase 16's
        // invariants exclude it BY NAME. A verdict here would need that same
        // exclusion list, and a status line has no business carrying one. The
        // Report says `closed no / border edges 32` in the plain colour; so does
        // this.
        const float statusHeight =
            ImGui::GetTextLineHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
        if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down,
                                        statusHeight, kBarFlags)) {
            if (openIndex >= 0 || editor.isOpen()) {
                ImGui::TextDisabled("%u tri   vol %.4g m3   %s   %u border edge%s",
                                    report.triangles, report.signedVolume,
                                    !report.manifold ? "not manifold"
                                    : report.closed  ? "closed"
                                                     : "open",
                                    report.borderEdges, report.borderEdges == 1 ? "" : "s");
            } else {
                ImGui::TextDisabled("no mesh open");
            }

            // Right-aligned, because it is the one reading here that is about the
            // TOOL rather than about the mesh.
            char frameText[96];
            std::snprintf(frameText, sizeof(frameText), "%.1f fps  (%.2f ms)   grid %.2g m",
                          frameMilliseconds > 0.0f ? 1000.0f / frameMilliseconds : 0.0f,
                          frameMilliseconds, static_cast<double>(gridCell));
            const float frameWidth = ImGui::CalcTextSize(frameText).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - frameWidth - 16.0f);
            ImGui::TextDisabled("%s", frameText);
        }
        ImGui::End();

        // ⚑⚑ `PassthruCentralNode` IS WHAT KEEPS THE VIEWPORT USABLE, AND IT IS
        // LOAD-BEARING RATHER THAN COSMETIC. The dockspace covers the whole
        // window, so without it the tool would be a full-screen grey sheet with
        // the 3D view painted underneath and every orbit, dolly and point-drag
        // swallowed before it reached the camera. The flag's contract is exactly
        // the two things needed: an empty central node draws no background, and
        // it lets inputs pass through.
        //
        // ⚑ Submitted BEFORE any window it can host - ImGui's own requirement -
        // and every frame, because a dockspace that stops being submitted
        // undocks everything living in it.
        const ImGuiID dockspaceId = ImHashStr("ForgeDockspace");
        ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_PassthruCentralNode);

        // ⚑⚑ ASKED *AFTER* `DockSpaceOverViewport`, AND ASKED AS "IS IT SPLIT",
        // BOTH OF WHICH COST A RUN TO GET RIGHT. Two traps sit here:
        //
        // (1) Before `DockSpace()` has run for the frame, `DockBuilderGetNode`
        //     returns NULL even when `forge.ini` has just been loaded, because
        //     the node is materialised FROM those settings by that call. Asking
        //     first therefore reports "no layout" on every launch, and the
        //     default gets rebuilt over the author's saved one - which is a
        //     persistence feature that silently does nothing, the worst kind.
        //     The tell was that the ini contained a perfectly good layout while
        //     the tool ignored it: the WRITE half worked and only the READ half
        //     was broken, so checking one of them proved nothing.
        //
        // (2) After that call the node ALWAYS exists, so its existence answers
        //     nothing. A freshly created dockspace is one empty node; a restored
        //     one is a SPLIT. That is the question worth asking.
        const ImGuiDockNode* rootNode = ImGui::DockBuilderGetNode(dockspaceId);
        const bool haveLayout = rootNode != nullptr && rootNode->IsSplitNode();

        // ⚑⚑ AND IT WAITS UNTIL THE BARS HAVE REPORTED THEIR INSETS. A side bar
        // reports its size into the work area FOR THE NEXT FRAME (ImGui's own
        // comment), and the menu bar does the same - so on frame one `WorkSize`
        // is the whole viewport and a layout built there is sized against ~72 px
        // that belong to chrome. Measured: it left the restored tab bar sitting
        // TWO PIXELS below the freshly built one, self-consistent afterwards but
        // never agreeing with itself across the first launch. Gating on "the
        // work area is smaller than the viewport" is the same wait-until-it-
        // exists idiom the Mesh focus uses, and it costs one frame.
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const bool barInsetsKnown = mainViewport->WorkSize.y < mainViewport->Size.y - 1.0f;

        // ⚑ Built only when there is nothing saved, so `forge.ini` always wins.
        // A tool that re-imposed its own default over the author's arrangement
        // on every launch would be the persistence bug with extra steps.
        if ((!haveLayout || resetLayout) && barInsetsKnown) {
            resetLayout = false;
            const ImVec2 workSize = mainViewport->WorkSize;
            ImGui::DockBuilderRemoveNode(dockspaceId);
            // ⚑ `DockSpace` only, and NOT `PassthruCentralNode` beside it. The
            // passthrough is a per-frame flag that `DockSpaceOverViewport`
            // applies above, not a property stored on the node - and the two
            // constants come from DIFFERENT enums (`DockSpace` is private to
            // imgui_internal.h), so or-ing them is a C5054 that this build
            // treats as an error.
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            // ⚑ Sized BEFORE the splits: ImGui's own note is that split ratios
            // are unreliable if the node has no size yet.
            ImGui::DockBuilderSetNodeSize(dockspaceId, workSize);

            // ⚑⚑ THE DEFAULT ANSWERS J's ONE KNOWN COST AND PRESERVES J's
            // HEADLINE AT THE SAME TIME, WHICH IS WHY IT IS TWO FULL-HEIGHT
            // COLUMNS RATHER THAN ONE SPLIT ONE. `Report` on its own to the
            // right is what finally puts it beside `Points, edges & faces`.
            // `Texture` must keep the FULL height or stage J's headline breaks:
            // its editor is 821 px and arrives whole today, and tabbing it into
            // a half-height node would put it back under a scrollbar - undoing
            // the one thing about J a person has already called right.
            ImGuiID leftId = 0;
            ImGuiID centreId = 0;
            ImGuiID rightId = 0;
            leftId = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.24f, nullptr,
                                                 &centreId);
            rightId = ImGui::DockBuilderSplitNode(centreId, ImGuiDir_Right, 0.32f, nullptr,
                                                  &centreId);
            ImGui::DockBuilderDockWindow("Mesh", leftId);
            ImGui::DockBuilderDockWindow("Texture", leftId);
            ImGui::DockBuilderDockWindow("View", leftId);
            ImGui::DockBuilderDockWindow("Report", rightId);
            ImGui::DockBuilderFinish(dockspaceId);
            focusMeshPending = true;
        }

        // ⚑⚑ THE DOCK ORDER SETS THE TAB ORDER, BUT THE WINDOW DOCKED LAST IS
        // THE ONE LEFT SELECTED - so the tool opened on `View`, the least useful
        // of the three, with `Mesh` hidden behind a tab. Tab order and active
        // tab are two decisions and DockBuilder only makes the first.
        //
        // ⚑ It has to WAIT A FRAME. On the frame the layout is built the `Mesh`
        // window has never been submitted, so there is nothing for ImGui to
        // focus and the call is silently dropped - which is exactly what the
        // first attempt did. Asking whether the window exists yet is what makes
        // the deferral self-evident rather than a magic frame counter.
        if (focusMeshPending && ImGui::FindWindowByName("Mesh") != nullptr) {
            ImGui::SetWindowFocus("Mesh");
            focusMeshPending = false;
        }

        // ⚑⚑ STAGE J's FOUR TABS ARE FOUR WINDOWS. J split the panel because it
        // was 2,901 px of content in an 860 px window, and the split worked -
        // but a tab bar can only ever show ONE of its four, which is where J's
        // single known cost came from: `Points, edges & faces` and the `Report`
        // stopped being visible together. Windows remove that ceiling instead of
        // paying for it, and the author decides which two they want.
        //
        // ⚑ Nothing switches window on its own, which is J's rule kept verbatim.
        // Opening a `.tex` does not raise `Texture` and a bake does not raise the
        // `Report`: the layout belongs to the author, not to the tool. A panel
        // that moves under the hand is the "save moves" trap, recorded three
        // times now, and docking would only make it easier to commit.
        //
        // ⚑ Each window is its own Begin/End pair and a HIDDEN one must not get
        // an End at all - hence the nested `if` rather than a `&&`. ImGui
        // requires End for every Begin that RAN, and a short-circuited Begin
        // never ran.
        // ⚑⚑ `NoFocusOnAppearing` IS WHAT MAKES THE SAVED LAYOUT STICK, AND IT
        // IS NOT COSMETIC. On the first frame all four windows transition from
        // hidden to visible, and ImGui focuses a window as it APPEARS - so the
        // last one submitted stole the selection and the tool always opened on
        // `View`, whatever `forge.ini` said. Measured: with the ini recording
        // `Selected=0x8242F0B0` (Mesh) it still opened on View, so the setting
        // was not being ignored on load, it was being OVERWRITTEN a moment
        // after it. ⚑ The dock STRUCTURE persisting is not evidence that the
        // SELECTION does - they are two different pieces of the same file, and
        // only one of them was broken.
        const ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoFocusOnAppearing;

        if (showMesh) {
            if (ImGui::Begin("Mesh", &showMesh, kPanelFlags)) {
                if (ImGui::CollapsingHeader("Meshes", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // ⚑ 230 px rather than J's 190 because the group headers
                    // are new height: 8 `parts` rows at a 17 px pitch plus
                    // three headers needs ~199, so keeping 190 would have
                    // made the list WORSE at showing the very rows this
                    // change is about. It costs the `Mesh` tab ~40 px, which
                    // is the one tab that already scrolls - `Texture`, with
                    // its 5 px of headroom, does not carry this list.
                    const int clicked =
                        drawAssetList("##meshes", meshEntries, openIndex, 0.30f, meshListContent);
                    if (clicked >= 0) {
                        openMeshAt(clicked);
                    }
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
                    if (points.drawPanel(editor, rowHoverPart)) {
                        rebuildFromEditor(/*reframe=*/false);
                    }
                }
            }
            ImGui::End();
        }

        // What the mesh you authored MEASURES, what NAMES it, and what it
        // decimates to - three readings of the thing edited in `Mesh`, which is
        // why they travel together.
        //
        // ⚑ This is the window the default layout puts in its own full-height
        // column, because being visible AT THE SAME TIME as `Mesh` is the whole
        // point: it is the pairing J had to give up.
        if (showReport) {
            if (ImGui::Begin("Report", &showReport, kPanelFlags)) {
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
            }
            ImGui::End();
        }

        // The other document. It gets a whole window because it IS a whole
        // editor - a peer of the mesh half, not a section of it.
        //
        // ⚑ The default layout gives it FULL height for a measured reason: the
        // editor is 821 px and stage J's headline is that it arrives whole. Dock
        // it into a half-height node and it goes back under a scrollbar, undoing
        // the one thing about J a person has already called right.
        if (showTexture) {
            if (ImGui::Begin("Texture", &showTexture, kPanelFlags)) {
                // Stage G. Selecting a texture here does BOTH things - it shades the
                // open mesh with it and, if it is a `.tex`, opens it for editing -
                // because two lists that each did half would be two answers to
                // "which texture am I looking at".
                if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Stage M. ⚑ Six rows, not three: the list carries the cooked
                    // `.stex` siblings as well as the sources, so the shipped
                    // 90 px was already 24 px short of its own content.
                    const float texOuter = ImGui::GetWindowHeight();
                    const float texHeight = forge::listHeight(forge::textRowMetrics(),
                                                              textureLabels.size(),
                                                              forge::kMinListRows, 0.20f, texOuter);
                    if (ImGui::BeginChild("##textures", {0.0f, texHeight},
                                          ImGuiChildFlags_Borders)) {
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
            }
            ImGui::End();
        }

        // Neither document: how you are LOOKING at whichever one is open.
        if (showView) {
            if (ImGui::Begin("View", &showView, kPanelFlags)) {
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
            }
            ImGui::End();
        }

        // ⚑ One place that notices a panel opened or closed, whichever of the
        // three routes did it. Without this the toggle is real for the session
        // and gone on the next launch, which is exactly what was reported.
        {
            const bool shown[4] = {showMesh, showReport, showTexture, showView};
            for (int i = 0; i < 4; ++i) {
                if (shown[i] != wasShown[i]) {
                    wasShown[i] = shown[i];
                    ImGui::MarkIniSettingsDirty();
                }
            }
        }

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
