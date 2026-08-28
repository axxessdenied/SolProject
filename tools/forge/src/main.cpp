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

#include "cook.hpp"
#include "def_editor.hpp"
#include "forge_view.hpp"
#include "history_buttons.hpp"
#include "list_layout_style.hpp"
#include "mesh_library.hpp"
#include "orbit_camera.hpp"
#include "part_editor.hpp"
#include "point_tool.hpp"
#include "project_paths.hpp"
#include "sound_preview.hpp"
#include "status_line.hpp"
#include "texture_editor.hpp"
#include "waveform.hpp"

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

// ⚑⚑ ALL THREE, AND THE MISSING TWO WERE THE STAGE V DEFECT. `SOL_MODEL_DATA_DIR`
// had no fallback at all and was used raw, so a build without it did not fall
// back - it failed to compile, which is why nothing ever noticed that a release
// build was the only configuration that would have wanted one. Empty means
// "this build bakes no source tree", which `resolveProjectPaths` reads as a
// shipping build.
#if !defined(SOL_ASSETS_SOURCE_DIR)
#define SOL_ASSETS_SOURCE_DIR ""
#endif
#if !defined(SOL_MODEL_DATA_DIR)
#define SOL_MODEL_DATA_DIR ""
#endif
#if !defined(SOL_FORGE_INBOX_DIR)
#define SOL_FORGE_INBOX_DIR ""
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
    PanelToggle items[6];

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
[[nodiscard]] int drawAssetList(const char* id,
                                const std::vector<forge::AssetEntry>& entries,
                                int selected,
                                float share,
                                float& visibleContent)
{
    int clicked = -1;
    const forge::ListMetrics rowMetrics = forge::textRowMetrics();
    const float headerPitch = forge::frameRowMetrics().rowPitch;
    const float height = forge::listHeightForContent(
        rowMetrics, visibleContent, forge::kMinListRows, share, ImGui::GetWindowHeight());
    float submitted = 0.0f;
    if (ImGui::BeginChild(id, {0.0f, height}, ImGuiChildFlags_Borders)) {
        std::size_t first = 0;
        while (first < entries.size()) {
            std::size_t end = first;
            while (end < entries.size() && entries[end].group == entries[first].group) {
                ++end;
            }
            char header[96];
            std::snprintf(header,
                          sizeof(header),
                          "%s (%zu)###%s",
                          entries[first].group.c_str(),
                          end - first,
                          entries[first].group.c_str());
            submitted += headerPitch; // the header is a row of the list too, and a taller one
            // Build output arrives closed: it is the majority of the list and
            // the minority of the interest, and it is the half that grows on
            // its own. Everything an author can actually edit arrives open.
            if (ImGui::CollapsingHeader(header, entries[first].cooked ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
                for (std::size_t row = first; row < end; ++row) {
                    if (ImGui::Selectable(entries[row].label.c_str(), static_cast<int>(row) == selected)) {
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

void addWireBox(renderer::DebugDrawRenderer& lines, core::Vec3 min, core::Vec3 max, core::Vec4 color)
{
    const core::Vec3 corner[8] = {
        {min.x, min.y, min.z},
        {max.x, min.y, min.z},
        {max.x, min.y, max.z},
        {min.x, min.y, max.z},
        {min.x, max.y, min.z},
        {max.x, max.y, min.z},
        {max.x, max.y, max.z},
        {min.x, max.y, max.z},
    };
    static constexpr int kEdges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
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
    const float cell = core::clamp(std::pow(10.0f, std::round(std::log10(raw))), 0.01f, 1000.0f);

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
    lines.line({0.0f, -extent * 0.25f, 0.0f}, {0.0f, extent * 0.25f, 0.0f}, {0.25f, 0.60f, 0.30f, 1.0f});
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

// ⚑ The extension check that used to live here went with stage R: deciding
// which files in the inbox are drops is now `forgeIsPendingDrop`, in the
// headless slice, because it has to answer the archive question at the same
// time and that one is worth a test rather than a comment.

// Trims `text` with a trailing ellipsis until it fits `width` pixels, and
// returns it unchanged when it already does.
//
// ⚑ Clipping is the only option open to the status bar: the bar reports its own
// height into the viewport work area, so wrapping a long message would resize
// the dockspace under the author's hands mid-edit. `saved <path>` and
// `opened <name> (28 parts)` are both unbounded, so this is reachable and not
// theoretical.
//
// ⚑ Binary search rather than a character walk - `CalcTextSize` shapes the run
// each call, and this runs every frame the bar draws.
std::string elideToWidth(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    static const char* kEllipsis = "...";
    std::size_t lo = 0;
    std::size_t hi = text.size();
    while (lo < hi) {
        // `hi > lo` here, so `mid >= lo + 1 >= 1` and `mid - 1` cannot wrap.
        const std::size_t mid = lo + (hi - lo + 1) / 2;
        if (ImGui::CalcTextSize((text.substr(0, mid) + kEllipsis).c_str()).x <= width) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return text.substr(0, lo) + kEllipsis;
}

} // namespace

// ⚑⚑⚑ THE PANEL'S FIRST PICTURE OF A SOUND (PHASE 26 STAGE C). Everything the
// Sound panel has said since Phase 24 stage U1 has been a number - seconds,
// frames, peak - and a number cannot distinguish a cue that clips once from one
// that clips throughout, or show an author that the fade they wrote eats half
// the cue. This draws the thing those numbers summarise.
//
// ⚑⚑ IT READS THE PREVIEW'S BANK RATHER THAN THE FILE, so it costs no I/O and
// no parsing: `SoundPreview::rebuild` already decoded every listed cue, and
// what plays is what is drawn, by construction rather than by agreement. It
// therefore works identically for a `.snd`, a `.wav` and a cooked `.saud` -
// three source kinds, one picture, because all three are int16 by the time they
// reach the bank.
//
// ⚑ NO PLAYHEAD, DELIBERATELY. `Mixer::Voice::cursor` is device-thread state
// with no accessor, and `mixer.hpp`'s rule about not touching the bank while
// the device runs is load-bearing enough that `SoundPreview` throws the whole
// mixer away on a rebuild. A moving cursor is a new atomic in that file, which
// is a change worth making on its own evidence rather than as a decoration on
// this one.
void drawWaveform(const forge::SoundPreview& preview, int index, const forge::SoundReport& report)
{
    const sol::audio::SoundClip* clip = preview.clip(index);
    const float height = ImGui::GetFontSize() * 4.5f;
    const float width = std::max(ImGui::GetContentRegionAvail().x, 32.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = origin;
    const ImVec2 max = ImVec2(origin.x + width, origin.y + height);
    draw->AddRectFilled(min, max, IM_COL32(18, 20, 24, 255));
    draw->AddRect(min, max, IM_COL32(70, 76, 86, 255));

    if (clip == nullptr || clip->samples.empty()) {
        // A row that would not decode is still LISTED - an author needs to see
        // the thing that is broken - so the picture says so rather than being
        // absent and leaving a gap nobody can interpret.
        const char* label = "no samples";
        const ImVec2 size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(min.x + ((width - size.x) * 0.5f), min.y + ((height - size.y) * 0.5f)),
                      IM_COL32(150, 90, 90, 255),
                      label);
        return;
    }

    const float centre = min.y + (height * 0.5f);
    const float halfHeight = height * 0.5f;

    // ⚑ THE FULL-SCALE GUIDES ARE DRAWN EVEN WHEN NOTHING REACHES THEM, so the
    // vertical scale is fixed rather than fitted. An envelope normalised to its
    // own loudest sample would draw a quiet cue and a clipping one identically,
    // which is precisely the comparison `peak` exists to let an author make.
    draw->AddLine(ImVec2(min.x, centre), ImVec2(max.x, centre), IM_COL32(70, 76, 86, 255));
    if (report.peak > 0.0f) {
        const float top = centre - (report.peak * halfHeight);
        const float bottom = centre + (report.peak * halfHeight);
        const ImU32 guide = report.peak >= 0.999f ? IM_COL32(190, 90, 80, 130) : IM_COL32(90, 130, 90, 110);
        draw->AddLine(ImVec2(min.x, top), ImVec2(max.x, top), guide);
        draw->AddLine(ImVec2(min.x, bottom), ImVec2(max.x, bottom), guide);
    }

    const auto columns = static_cast<std::size_t>(width);
    const std::vector<forge::WaveformColumn> envelope =
        forge::waveformEnvelope(clip->samples, clip->channelCount, columns);
    for (std::size_t i = 0; i < envelope.size(); ++i) {
        const float x = min.x + static_cast<float>(i) + 0.5f;
        float top = centre - (envelope[i].high * halfHeight);
        float bottom = centre - (envelope[i].low * halfHeight);
        // A column of pure silence would be a zero-length line and draw
        // nothing, leaving a gap that reads as missing data rather than as
        // quiet. One pixel is the honest minimum.
        if (bottom - top < 1.0f) {
            top = centre - 0.5f;
            bottom = centre + 0.5f;
        }
        draw->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(120, 200, 170, 220));
    }

    ImGui::TextDisabled("%.3f s, full scale at the edges", static_cast<double>(report.seconds));
}

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

    // ⚑⚑⚑ PHASE 24 STAGE V: EVERY DIRECTORY THIS TOOL TOUCHES COMES FROM ONE
    // RULE, AND THE RULE IS TESTED. What was here before was five expressions
    // spelled out inline - three of them reading defines that a release build
    // baked from this machine's source tree, and two of them (`cooked/` and
    // `shaders/`) hard-wired beside the executable with no define at all. Those
    // last two were the ones that mattered most to a shipped tool: the Cook
    // button writes `cooked/`, so an installed Forge would have cooked an
    // author's mod into the TOOL's folder, where the game never looks.
    //
    // ⚑⚑ WHERE BLENDER DROPS IS STILL NOT UNDER `assets/` (stage L). The cooker
    // scans the source tree RECURSIVELY into one flat output directory keyed on
    // the file STEM, so a `ship.gltf` anywhere beneath it collides with
    // `ship.forge` - and that guard aborts the ENTIRE cook rather than skipping
    // the pair. The glTF is transport: once imported, the `.forge` is the source.
    const std::vector<std::string> arguments(argv, argv + argc);
    const std::string requestedProject = forge::parseProjectArgument(arguments);
    const forge::DevPaths devPaths = {
        .assets = SOL_ASSETS_SOURCE_DIR, .data = SOL_MODEL_DATA_DIR, .inbox = SOL_FORGE_INBOX_DIR};
    const forge::ProjectPaths paths = forge::resolveProjectPaths(requestedProject, devPaths, executableDir);
    // ⚑ Asked of the RULE, never re-derived here - see `ProjectPaths::isProject`.

    const std::string& cookedDirectory = paths.cooked;
    const std::string& assetsDirectory = paths.assets;
    const std::string& inboxDirectory = paths.inbox;

    // ⚑⚑ MADE RATHER THAN REQUIRED, AND ONLY FOR A PROJECT. A new mod is an
    // empty directory, and `listFiles` cannot tell a missing directory from an
    // empty one - which is the property `game/mods/README.md` records as the
    // reason `mods/` ships empty-but-present. So the drop target the Blender
    // bridge writes into and the `cooked/` the Cook button fills have to exist
    // before anything has been authored. The dev tree's directories are the
    // repo's and are not this tool's to invent, which is why the branch is here
    // and not inside the rule.
    if (paths.isProject && !forge::createProjectDirectories(paths)) {
        SOL_LOG_WARN("forge: could not create every directory under %s", paths.data.c_str());
    }
    SOL_LOG_INFO("forge: project %s (%s)", paths.data.c_str(), paths.isProject ? "project" : "source tree");
    SOL_LOG_INFO("forge: assets %s | cooked %s | inbox %s",
                 assetsDirectory.c_str(),
                 cookedDirectory.c_str(),
                 inboxDirectory.c_str());
    for (const std::string& directory : paths.shaderSearchPath) {
        SOL_LOG_INFO("forge: shaders %s", directory.c_str());
    }

    rhi::Swapchain swapchain;
    if (!swapchain.create(context, window.width(), window.height(), /*vsync=*/true)) {
        return EXIT_FAILURE;
    }

    forge::ForgeView view;
    if (!view.initialize(context, swapchain, paths.shaderSearchPath)) {
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
    if (!imguiHost.initialize(window,
                              context,
                              swapchain.imageFormat(),
                              VK_FORMAT_D32_SFLOAT,
                              swapchain.imageCount(),
                              hostOptions)) {
        return EXIT_FAILURE;
    }
    view.setImGuiHost(&imguiHost);

    std::vector<forge::AssetEntry> meshEntries = forge::listMeshes(assetsDirectory, cookedDirectory);

    // Every texture is uploaded once at startup - there are a handful, they are
    // BC1, and it means switching one costs no device idle in the middle of a
    // frame. A `.tex` source is evaluated and encoded here exactly as the cooker
    // would, so what shades the mesh is the compressed image the game loads.
    //
    // ⚑⚑ A LAMBDA RATHER THAN A LOOP SINCE STAGE U2, BECAUSE THERE ARE NOW TWO
    // CALLERS AND THEY MUST NOT BE TWO ANSWERS TO WHAT THIS TOOL CAN DRAW. The
    // second is `reloadTextures`, below, which runs after a Cook - and the whole
    // hazard it manages is that the set it swaps in has to be built by exactly
    // the same rules as the one it replaces. Returns how many were LISTED, which
    // is not how many loaded: the difference is a texture the tool refused, and
    // the caller is what decides whether that is a log line or a status line.
    const auto loadTextureSet = [&](std::vector<renderer::GpuTexture>& outTextures,
                                    std::vector<std::string>& outLabels,
                                    std::vector<forge::AssetEntry>& outEntries) {
        const std::vector<forge::AssetEntry> listed = forge::listTextures(assetsDirectory, cookedDirectory);
        for (const forge::AssetEntry& entry : listed) {
            assets::TextureData data;
            std::string textureError;
            if (!forge::loadTexture(entry, data, &textureError)) {
                SOL_LOG_WARN("forge: cannot load texture %s: %s", entry.path.c_str(), textureError.c_str());
                continue;
            }
            outTextures.push_back(view.meshes().createTexture(data));
            outLabels.push_back(entry.label);
            outEntries.push_back(entry);
        }
        // ⚑⚑⚑ PHASE 24 STAGE V: THE SET IS NEVER EMPTY, WHICH IS HOW A BRAND-NEW
        // MOD OPENS AT ALL. An empty project used to be `EXIT_FAILURE` here -
        // right while the only project was this repo, wrong for the first thing
        // an installed tool is ever pointed at. Stage U2's invariant ("two lines
        // draw `textures[textureIndex]` with no range check, entitled to because
        // startup treats an empty set as fatal") is kept TRUE rather than
        // relaxed, so nothing downstream had to learn a new case.
        //
        // ⚑ Inside the lambda and not beside it, because `reloadTextures` is the
        // second caller and its whole hazard is that the set it swaps in must be
        // built by the same rules as the one it replaces. An author who cooks a
        // project back to empty gets the placeholder, not the last image that
        // worked.
        if (outTextures.empty()) {
            assets::TextureData placeholder;
            std::string placeholderError;
            if (forge::builtinCheckerTexture(placeholder, &placeholderError)) {
                outTextures.push_back(view.meshes().createTexture(placeholder));
                outLabels.push_back(forge::builtinTextureLabel());
                // ⚑ An entry with no path and no STEM. No path so the editor
                // takes its "nothing to edit here" branch, and no stem so
                // `textureStems` cannot offer it to a `[[model]]` row - the game
                // has no built-in checker, and a row naming one would be a mod
                // that is invisible on every machine but this one.
                outEntries.push_back(
                    forge::AssetEntry{.label = forge::builtinTextureLabel(), .group = "built-in"});
            } else {
                SOL_LOG_ERROR("forge: cannot build the placeholder texture: %s", placeholderError.c_str());
            }
        }
        return listed.size();
    };

    std::vector<renderer::GpuTexture> textures;
    std::vector<std::string> textureLabels;
    std::vector<forge::AssetEntry> loadedTextureEntries;
    const std::size_t listedTextures = loadTextureSet(textures, textureLabels, loadedTextureEntries);
    SOL_LOG_INFO("forge: %zu mesh(es) and %zu texture(s) under %s and %s",
                 meshEntries.size(),
                 listedTextures,
                 assetsDirectory.c_str(),
                 cookedDirectory.c_str());
    // ⚑⚑ STILL FATAL, AND NOW FOR A REASON THAT IS ACTUALLY FATAL. It used to
    // fire whenever the project held no textures, which is the normal state of a
    // mod nobody has authored into yet; `loadTextureSet` supplies the built-in
    // placeholder for that case. What is left can only be the placeholder itself
    // failing to build - the parser or the encoder refusing a document compiled
    // into this binary - and there is nothing an author can do about that and
    // nothing left to draw with.
    if (textures.empty()) {
        SOL_LOG_ERROR("forge: no textures and no placeholder - this build is broken");
        return EXIT_FAILURE;
    }
    if (listedTextures == 0) {
        SOL_LOG_INFO("forge: no textures under %s or %s yet - showing %s",
                     assetsDirectory.c_str(),
                     cookedDirectory.c_str(),
                     forge::builtinTextureLabel());
    }

    // ⚑ The authored side of every mesh in this game. The tool reads the game's
    // DATA directory, never its code (AGENTS.md 4) - `DefDatabase` is engine and
    // models.toml is a file - which is what lets a viewer say "this hull is
    // 1.1584 m and the sim thinks it is 1.0" instead of leaving that to be
    // noticed by a person holding a panel next to a text file, which is exactly
    // how stage C found it.
    // ⚑ In a project this is the project directory itself: a mod's def
    // documents sit at its top level, beside `assets/` (game/mods/README.md).
    assets::DefDatabase defs;
    const std::string& dataDirectory = paths.data;
    {
        std::string defError;
        if (!forge::loadModelCatalog(dataDirectory, defs, &defError)) {
            SOL_LOG_WARN("forge: cannot read model defs: %s", defError.c_str());
        }
        SOL_LOG_INFO("forge: %zu [[model]] row(s) from %s", defs.models().size(), dataDirectory.c_str());
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
    //
    // ⚑⚑ DERIVED FROM WHAT UPLOADED, NEVER FROM WHAT WAS LISTED, and that is the
    // rule stage U2 had to carry across a reload rather than quietly drop. A
    // stem offered here is a promise that the viewport can draw it; a `.png` the
    // decoder refused is listed, is not in `textures`, and must not reach this
    // combo - otherwise stage H's whole point, that a row cannot name something
    // that is not there, becomes untrue the first time somebody exports a 16-bit
    // image. ⚑ Deduplicated, because `hull.tex` and `cooked/hull.stex` are one
    // stem in two places and always have been.
    std::vector<std::string> textureStems;
    const auto refreshTextureStems = [&]() {
        textureStems.clear();
        for (const forge::AssetEntry& textureEntry : loadedTextureEntries) {
            // ⚑ The built-in placeholder carries no stem, and this is where that
            // stops it being nameable (stage V). Everything above about a stem
            // being a promise the viewport can draw it goes one step further for
            // this row: the GAME cannot draw it, because it exists only inside
            // this binary. A skipped empty stem is the whole enforcement.
            if (textureEntry.stem.empty()) {
                continue;
            }
            if (std::find(textureStems.begin(), textureStems.end(), textureEntry.stem) ==
                textureStems.end()) {
                textureStems.push_back(textureEntry.stem);
            }
        }
    };
    refreshTextureStems();

    // ⚑⚑⚑ PHASE 25 STAGE D: THE SHADER STEMS ON THE SEARCH PATH, so a material
    // names its shader from a list rather than from a text field. A stem that
    // resolves to no `.spv` costs the material its pipeline and every model
    // wearing it its picture, and the only signal is a log line - the schema
    // cannot refuse it, because whether a shader exists is not a fact about the
    // def file. This is the same argument as the texture combo, one asset kind
    // over.
    //
    // ⚑ THE DIRECTORIES ARE THE ONES THE REGISTRY ACTUALLY SEARCHES, so the
    // list cannot offer a stem the loader will then fail to find. Listed once:
    // the shaders are a build output, and `Cook` does not compile any (decision
    // 011 puts the GLSL compiler in the author's hands, not in this tool).
    //
    // ⚑⚑ EVERY ENTRY OF THE SEARCH PATH, AND DEDUPLICATED, SINCE STAGE V. A
    // project's `shaders/` sits in front of the install's, and the whole point
    // of that order is that a mod may replace ONE stage of a pair the engine
    // already ships - so `mesh` has to keep appearing once whether the project
    // overrides it or not. Offering it twice would put two identical rows in
    // front of an author and make the combo read as though the two were
    // different shaders.
    std::vector<std::string> vertexShaderStems;
    std::vector<std::string> fragmentShaderStems;
    {
        const auto collectStems =
            [](const std::string& path, const char* suffix, std::vector<std::string>& out) {
                for (const std::string& file : platform::listFiles(path.c_str())) {
                    const std::size_t slash = file.find_last_of("/\\");
                    const std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
                    const std::size_t suffixLength = std::strlen(suffix);
                    if (name.size() <= suffixLength ||
                        name.compare(name.size() - suffixLength, suffixLength, suffix) != 0) {
                        continue;
                    }
                    out.push_back(name.substr(0, name.size() - suffixLength));
                }
                std::sort(out.begin(), out.end());
                out.erase(std::unique(out.begin(), out.end()), out.end());
            };
        for (const std::string& directory : paths.shaderSearchPath) {
            collectStems(directory, ".vert.spv", vertexShaderStems);
            collectStems(directory, ".frag.spv", fragmentShaderStems);
        }
    }

    // ⚑⚑⚑ THE VIEWPORT'S MATERIALS ARE THE GAME'S, BUILT FROM `materials.toml`
    // RATHER THAN FROM ONE STOCK ROW. Stage B put the tool's viewport through
    // `MaterialRegistry` deliberately, with a comment saying stage D was what
    // would give it real ones; this is that call. What draws the open mesh is
    // now the shader pair, the pipeline state, the declared slots and the packed
    // params the GAME will use, so "the Forge draws what the game draws" is true
    // by construction rather than by two call sites agreeing.
    //
    // ⚑ FAILURE IS NOT FATAL AND MUST NOT BE. `build` returns false only when
    // NOTHING built; a single material whose shader will not load, or whose
    // declaration its SPIR-V disagrees with, simply gets no pipeline - and the
    // viewport then draws nothing for it, which is stage C's named refusal
    // arriving in front of an author instead of in a log they are not reading.
    std::string materialStatus;
    const auto textureSlotIndex = [&](const std::string& stem) {
        // ⚑ THE FIRST MATCH, WHICH IS THE SOURCE RATHER THAN THE COOKED SIBLING,
        // because `listTextures` puts sources first and a source is the one an
        // author is editing. A slot pointing at `cooked/glow.stex` would go on
        // showing the last cook while the `.tex` beside it changed under the
        // hand that was changing it.
        for (std::size_t i = 0; i < loadedTextureEntries.size(); ++i) {
            if (loadedTextureEntries[i].stem == stem) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    // Writes every material's set 1. ⚑ Separate from the build because a
    // TEXTURE reload has to redo this and must not redo the pipelines: the
    // images behind those descriptors have been destroyed and recreated, but
    // nothing about any shader has changed.
    const auto writeMaterialSets = [&]() {
        std::vector<sol::rhi::MaterialTextureBinding> bindings;
        std::size_t unresolved = 0;
        const std::vector<assets::MaterialDef>& materials = defEditor.materials();
        for (std::uint32_t m = 0; m < materials.size(); ++m) {
            if (view.materials().materialSet(m) == VK_NULL_HANDLE) {
                continue; // declares nothing, or has already been refused by name
            }
            bindings.clear();
            bool resolved = true;
            for (const assets::MaterialSlot& slot : materials[m].slots) {
                const int index = textureSlotIndex(slot.texture);
                if (index < 0) {
                    SOL_LOG_ERROR("forge: material '%s': no texture '%s' for slot '%s'",
                                  materials[m].id.c_str(),
                                  slot.texture.c_str(),
                                  slot.name.c_str());
                    resolved = false;
                    break;
                }
                const renderer::GpuTexture& texture = textures[static_cast<std::size_t>(index)];
                bindings.push_back({.view = texture.image.view, .sampler = texture.sampler});
            }
            if (!resolved || !view.materials().writeMaterialSet(m, bindings)) {
                ++unresolved;
            }
        }
        return unresolved;
    };
    const auto rebuildMaterials = [&]() {
        if (!defEditor.loaded()) {
            return;
        }
        // ⚑ The pipelines and descriptor sets being destroyed may still be named
        // by a frame the GPU has not finished, exactly as with a texture. This
        // runs from the pending block above every `Begin`, so no draw list can
        // yet name what it frees - but the SUBMITTED frames still can.
        context.waitIdle();
        if (!view.materials().build(defEditor.materials())) {
            materialStatus = "no materials built - the shader directory is unreadable";
            return;
        }
        const std::size_t unresolved = writeMaterialSets();
        std::size_t undrawable = 0;
        for (std::uint32_t m = 0; m < defEditor.materials().size(); ++m) {
            undrawable += view.materials().pipeline(m) == VK_NULL_HANDLE ? 1 : 0;
        }
        char line[128];
        std::snprintf(line,
                      sizeof(line),
                      "materials: %zu (%zu pipeline(s), %zu undrawable)",
                      view.materials().materialCount(),
                      view.materials().pipelineCount(),
                      undrawable + unresolved);
        materialStatus = line;
        SOL_LOG_INFO("forge: %s", materialStatus.c_str());
    };
    rebuildMaterials();

    // ⚑⚑ RAISED, NEVER PERFORMED WHERE THEY ARE RAISED - the fourth and fifth
    // members of the pending block near the top of the frame, and here rather
    // than beside the other three only because `reloadTextures` below has to be
    // able to set one and is built long before them.
    //
    // ⚑ THE SPLIT IS NOT AN OPTIMISATION. A rebuild destroys and recreates every
    // pipeline and every descriptor set in the registry and needs an idle
    // device; rewriting one material's params is a memcpy into a buffer that
    // stays mapped. Doing the first on every frame of a drag would idle the
    // device twenty-four times a second to change one float.
    bool materialRebuildPending = false;
    // The material whose params moved, or kNoMaterial. Not a bool, because the
    // write needs to know which buffer.
    std::uint32_t materialParamsPending = forge::DefEditor::kNoMaterial;
    // ⚑⚑ A TEXTURE RELOAD INVALIDATES EVERY SET 1 THIS TOOL HAS WRITTEN, and
    // that is the subtle one. `writeMaterialSet` records an image view and a
    // sampler; `reloadTextures` destroys every image and creates new ones. The
    // pipelines are untouched - no shader changed - so this rewrites the sets
    // and deliberately does NOT rebuild.
    bool materialSetsStale = false;

    renderer::GpuMesh openMesh = {};
    forge::MeshReport report;
    std::vector<forge::ModelMatch> modelMatches;
    int openIndex = -1;
    // Stage M: the height the mesh list actually submitted last frame. Not a row
    // count, because a collapsed group is one row and a group header is taller
    // than an entry. See drawAssetList.
    float meshListContent = 0.0f;
    forge::StatusLine status;
    // Stage Q4: the bar stamps its own clock when the line's serial moves, so a
    // message carries an age without StatusLine having to know what a clock is.
    unsigned long long statusSerialSeen = 0;
    double statusStampedAt = -1000.0;

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

    // ⚑⚑ STAGE U1: THE THIRD ASSET KIND, AND THE FIRST THAT IS NOT A PICTURE.
    // The listing, the samples behind it and the device are one object because
    // they have one lifetime: the mixer thread reads the bank directly, so
    // nothing may re-list without also rebuilding what the device is playing
    // from. Keeping them apart is how those two get out of step.
    std::vector<forge::AssetEntry> soundEntries = forge::listSounds(assetsDirectory, cookedDirectory);
    forge::SoundPreview soundPreview;
    soundPreview.rebuild(soundEntries);
    int soundIndex = -1;
    float soundListContent = 0.0f;

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
    bool showSound = true;
    bool showMaterial = true;
    bool showView = true;
    bool resetLayout = false;
    bool focusMeshPending = false;
    // ⚑⚑⚑ THE CASE THE DEFAULT LAYOUT CANNOT COVER: A SAVED LAYOUT THAT
    // PREDATES A PANEL. `forge.ini` always wins over the built-in default,
    // which is right and is the whole of stage K - but it means an author who
    // has arranged this tool even once has an ini that has never heard of
    // `Sound`, so a new panel arrives as a floating window on top of their
    // work and the only remedy on offer is `Reset layout`: throwing the
    // arrangement away in order to see the new thing. That trades a small
    // annoyance for a bigger one, and it will recur at every stage that adds a
    // panel, so it is worth solving once rather than apologising for.
    //
    // ⚑ A window nobody has ever placed is docked beside its nearest sibling
    // ONCE and is the author's from then on - ImGui writes it into the ini like
    // any other window and this never fires again.
    bool adoptSoundPanel = true;
    // ⚑ Stage 25-D, and the SECOND panel to need this - which is the point of
    // having solved it once. Every author who has arranged this tool has an ini
    // that has never heard of `Material`.
    bool adoptMaterialPanel = true;

    // ⚑ Registered here rather than inside the host, because WHICH panels exist
    // is the Forge's business and the host is shared with the game.
    PanelToggles panelToggles = {{{"Mesh", &showMesh},
                                  {"Report", &showReport},
                                  {"Texture", &showTexture},
                                  {"Sound", &showSound},
                                  {"Material", &showMaterial},
                                  {"View", &showView}}};
    // ⚑ ImGui only rewrites the ini when something MARKS it dirty, and it has no
    // idea these bools exist - so a toggle would be forgotten unless the change
    // is reported. Compared per frame rather than at each of the several places
    // a panel can close (menu item, window X, `Reset layout`), because a rule
    // spread over three call sites is a rule that gets missed at one of them.
    bool wasShown[6] = {showMesh, showReport, showTexture, showSound, showMaterial, showView};
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

    // ⚑⚑ STAGE Q: ONE HISTORY FOR THE WHOLE TOOL. It is declared here, beside
    // the editors it orders, and handed to each of them - `Ctrl+Z` undoes the
    // last thing the author did, in whichever of the three documents they did
    // it. Before Q, `Ctrl+Z` meant the mesh editor whatever was on screen, and
    // the parameter widgets were not in any history at all.
    forge::EditHistory history;
    editor.setHistory(&history);
    textureEditor.setHistory(&history);
    defEditor.setHistory(&history);

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
            modelMatches = forge::matchModels(defs, meshEntries[static_cast<std::size_t>(openIndex)], report);
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
        texturePreview = ImGui_ImplVulkan_AddTexture(
            texture.sampler, texture.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
        if (editingTextureIndex < 0 || editingTextureIndex >= static_cast<int>(textures.size())) {
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
            // document behind it to change. ⚑ Stage U2 puts a second kind of row
            // through this branch and it needs no new code: an imported `.png`
            // has no document behind it either, and this tool is not a bitmap
            // editor. Selecting one still shades the mesh, which is the half of
            // "does my texture work" the author came here to see.
            textureEditor.close();
            editingTextureIndex = -1;
            return;
        }
        std::string textureStatus;
        if (!textureEditor.openFile(loadedTextureEntries[static_cast<std::size_t>(index)].path,
                                    textureStatus)) {
            status = textureStatus;
            editingTextureIndex = -1;
            return;
        }
        editingTextureIndex = index;
        status = textureStatus;
    };

    // ⚑⚑⚑ PHASE 24 STAGE U2: THE WHOLE TEXTURE SET, RE-LISTED AND RE-UPLOADED,
    // WHICH IS WHAT LETS A PAINTED IMAGE REACH THE MESH WITHOUT A RELAUNCH. The
    // Cook button has refreshed meshes since stage T and sounds since U1 and
    // said out loud that textures were the exception, "because a texture cannot
    // be re-listed without being re-uploaded". This is that upload.
    //
    // ⚑⚑ THE NEW SET IS BUILT BEFORE THE OLD ONE IS DESTROYED, AND THE SWAP IS
    // REFUSED IF IT WOULD BE EMPTY. Two lines draw `textures[textureIndex]` and
    // `textureLabels[textureIndex]` with no range check at all - they are
    // entitled to, because startup treats an empty set as fatal and the set has
    // never been able to shrink until now. Keeping that invariant true is
    // cheaper and safer than teaching the View combo and the frame submission to
    // handle a case that only a swept project can produce, and it degrades the
    // right way: an author who deletes every texture keeps drawing the last ones
    // that worked instead of watching the tool exit.
    //
    // ⚑ Selection is restored BY PATH rather than by index. A cook adds cooked
    // siblings and sweeps orphans, so the row a number pointed at is not the row
    // it points at afterwards - the same reason the Cook button resets
    // `openIndex` and `soundIndex` rather than keeping them.
    const auto reloadTextures = [&]() {
        std::vector<renderer::GpuTexture> nextTextures;
        std::vector<std::string> nextLabels;
        std::vector<forge::AssetEntry> nextEntries;
        loadTextureSet(nextTextures, nextLabels, nextEntries);
        if (nextTextures.empty()) {
            status = "no textures loaded; keeping the ones already open";
            return;
        }

        const auto pathAt = [&](int index) {
            return index >= 0 && index < static_cast<int>(loadedTextureEntries.size())
                       ? loadedTextureEntries[static_cast<std::size_t>(index)].path
                       : std::string();
        };
        const std::string shownPath = pathAt(textureIndex);
        const std::string editedPath = pathAt(editingTextureIndex);

        // ⚑ The old images may still be named by a frame the GPU has not
        // finished, exactly as in `rebuildTexture` - and `texturePreview` is a
        // descriptor set pointing into one of them, so it goes first. This whole
        // lambda runs above every `Begin` in the tool (see the pending block), so
        // no draw list can yet be naming what it frees.
        context.waitIdle();
        if (texturePreview != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(texturePreview);
            texturePreview = VK_NULL_HANDLE;
        }
        for (renderer::GpuTexture& texture : textures) {
            view.meshes().destroyTexture(texture);
        }
        textures = std::move(nextTextures);
        textureLabels = std::move(nextLabels);
        loadedTextureEntries = std::move(nextEntries);
        refreshTextureStems();

        const auto indexOf = [&](const std::string& path) {
            if (path.empty()) {
                return -1;
            }
            for (std::size_t i = 0; i < loadedTextureEntries.size(); ++i) {
                if (loadedTextureEntries[i].path == path) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        const int shownAgain = indexOf(shownPath);
        textureIndex = shownAgain >= 0 ? shownAgain : 0;
        // ⚑⚑ THE EDITOR IS RE-POINTED AND NEVER RE-OPENED. `openTextureAt` would
        // read the document back off disk and throw away whatever the author has
        // typed since their last save - and Cook is a button an author presses
        // WHILE editing, which is exactly when that would hurt. If the file being
        // edited has gone, the index goes to -1 and the editor stays open with
        // nothing to shade, which is the state `new texture` already produces.
        editingTextureIndex = textureEditor.isOpen() ? indexOf(editedPath) : -1;
        // ⚑ Every set 1 this tool wrote names an image that has just been
        // destroyed. See `materialSetsStale`.
        materialSetsStale = true;
        refreshTexturePreview();

        // ⚑⚑ A REFUSED TEXTURE IS LOGGED AND DELIBERATELY NOT PUT IN THE STATUS
        // BAR, AND THE FIRST DRAFT HAD IT THE OTHER WAY ROUND. Writing
        // "1 texture(s) could not be loaded" here reads like the more helpful
        // choice until you work out when it fires: the only realistic way a
        // `.png` fails to LOAD is that it also failed to COOK - both go through
        // `decodePng` - so this line would land one frame after "cook: 4 cooked,
        // 5 up to date, 1 failed" and OVERWRITE it, trading the counts for a
        // restatement. ⚑ A message that replaces a better message is worse than
        // no message, and inventing a texture-only exception to how every other
        // asset kind reports a failure would be worse again.
    };

    const auto openMeshAt = [&](int index) {
        if (index < 0 || index >= static_cast<int>(meshEntries.size())) {
            return;
        }
        assets::MeshData data;
        if (!forge::loadMesh(meshEntries[index], data)) {
            status = "failed to open " + meshEntries[index].label;
            SOL_LOG_ERROR("forge: %s", status.text().c_str());
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
                     meshEntries[index].label.c_str(),
                     report.triangles,
                     report.renderVertices,
                     report.positions,
                     static_cast<double>(report.boundingRadius));
        // Logged as well as drawn, because `--frames N` with stdout redirected
        // is how this tool gets read by anything that is not a person.
        for (const forge::ModelMatch& match : modelMatches) {
            if (match.radiusAgrees()) {
                SOL_LOG_INFO("forge: [[model]] %s radius %.4f m agrees with the mesh",
                             match.id.c_str(),
                             static_cast<double>(match.authoredRadius));
            } else {
                SOL_LOG_WARN("forge: [[model]] %s authors radius %.4f m, the mesh measures %.4f m "
                             "(%+.4f, %+.1f%%)",
                             match.id.c_str(),
                             static_cast<double>(match.authoredRadius),
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
    // ⚑⚑ WHAT IS HELD IN MEMORY IS ONLY WHAT DISK CANNOT SAY (stage R). A drop
    // that imports LEAVES the inbox for `imported/`, so nothing needs
    // remembering to stop it importing twice - the file is not there any more.
    // A drop that FAILS stays where it is, and without this would be retried
    // twice a second for the rest of the session. That asymmetry is the whole
    // of the state: pending is a file in the inbox, done is a file in the
    // archive, and failed is the one fact neither directory can carry.
    struct FailedDrop
    {
        std::string path;
        std::uint64_t modified = 0;
        std::string error;
    };

    std::vector<FailedDrop> inboxFailed;
    int inboxPollCountdown = 0;
    std::string inboxStatus;
    bool inboxAuto = true;
    // ⚑ Raised by an import that wrote a texture, and READ by the frame loop -
    // which is the only thing that may act on it, because a reload frees every
    // descriptor the frame in flight is still drawing with (stage U2). It cannot
    // set `textureReloadPending` itself: that lives with the other deferrals,
    // far below, and this lambda is built before it. Same shape as
    // `InboxImport::openRow` - the import reports what happened and the loop
    // decides when it is safe to act.
    bool inboxWroteTexture = false;
    const std::string inboxArchive = forge::forgeInboxArchive(inboxDirectory);

    // What one drop's import did, so a drain of several can report once and
    // open once instead of doing both per file.
    struct InboxImport
    {
        bool ok = false;
        int openRow = -1;    // the mesh row to open, or -1 for none
        std::string message; // what a single import would have said on its own
    };

    const auto importFromInbox = [&](const std::string& gltfPath) -> InboxImport {
        InboxImport result;
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
                if (!assets::parseForge(reinterpret_cast<const char*>(bytes.data()),
                                        bytes.size(),
                                        target.c_str(),
                                        doc,
                                        &parseError)) {
                    result.message = "cannot merge into " + stem + ".forge: " + parseError;
                    SOL_LOG_ERROR("forge: %s", result.message.c_str());
                    return result;
                }
            }
        }

        forge::ImportOutcome outcome;
        std::string importError;
        if (!forge::importGltfIntoDoc(gltfPath, assetsDirectory + "/textures", doc, outcome, &importError)) {
            result.message = importError;
            SOL_LOG_ERROR("forge: %s", result.message.c_str());
            return result;
        }

        const std::string text = assets::writeForge(doc);
        if (!platform::writeFileBytes(target.c_str(), text.data(), text.size())) {
            result.message = "cannot write " + target;
            SOL_LOG_ERROR("forge: %s", result.message.c_str());
            return result;
        }

        // ⚑⚑⚑ FILED BEFORE ANYTHING ELSE IS REPORTED, BECAUSE THIS IS THE
        // RECORD THAT THE IMPORT HAPPENED (stage R). Until the drop leaves the
        // inbox it is indistinguishable from one still waiting, which is the
        // whole defect this stage closes. It is deliberately NOT fatal: the
        // `.forge` is already written, so failing the import here would report
        // a failure for work that succeeded. The drop is left where it is and
        // named, which is the honest outcome - it will be offered again.
        if (!platform::createDirectories(inboxArchive.c_str()) ||
            !platform::moveFile(gltfPath.c_str(),
                                forge::forgeArchivedDropPath(gltfPath, inboxDirectory).c_str())) {
            SOL_LOG_ERROR(
                "forge: imported %s but could not file it under %s", gltfPath.c_str(), inboxArchive.c_str());
        }

        result.message = stem + ".forge  " + std::to_string(outcome.added.size()) + " added, " +
                         std::to_string(outcome.replaced.size()) + " replaced";
        // ⚑ A rename is NAMED, not just counted. It is the one outcome an
        // author cannot reconstruct from the Parts list afterwards - a part
        // under a new name is indistinguishable from a new part beside a
        // deleted one, which is the whole confusion this stage removes.
        if (!outcome.renamed.empty()) {
            result.message += ", " + std::to_string(outcome.renamed.size()) + " renamed (" +
                              outcome.renamed.front().first + " -> " + outcome.renamed.front().second;
            result.message += outcome.renamed.size() > 1
                                  ? ", +" + std::to_string(outcome.renamed.size() - 1) + " more)"
                                  : ")";
        }
        if (!outcome.kept.empty()) {
            result.message += ", " + std::to_string(outcome.kept.size()) + " kept";
        }
        // Named on the same argument as a rename: the stem is what the author
        // has to pick out of the `[[model]]` combo afterwards, and a count alone
        // would send them to the Texture panel to guess which row is new.
        if (!outcome.textures.empty()) {
            result.message += ", " + std::to_string(outcome.textures.size()) + " texture(s) (" +
                              outcome.textures.front().second;
            result.message += outcome.textures.size() > 1
                                  ? ", +" + std::to_string(outcome.textures.size() - 1) + " more)"
                                  : ")";
        }
        // ⚑⚑ ON THE BAR AND NOT ONLY IN THE LOG, WHICH IS THE WHOLE VALUE OF
        // THESE. Every one of them is a case where the GEOMETRY imported
        // perfectly and the texture did not - the viewport looks like a clean
        // success, the parts are all there, and the only sign that anything was
        // dropped is a mesh that stays whatever texture its `[[model]]` row
        // already named. An author with no sentence here has no reason to go
        // and read a log at all, so they would find this at ship time.
        for (const auto& [object, why] : outcome.imageNotes) {
            SOL_LOG_WARN("forge: %s %s", object.c_str(), why.c_str());
        }
        if (!outcome.imageNotes.empty()) {
            result.message +=
                "; " + outcome.imageNotes.front().first + " " + outcome.imageNotes.front().second;
            if (outcome.imageNotes.size() > 1) {
                result.message += " (+" + std::to_string(outcome.imageNotes.size() - 1) + " more)";
            }
        }
        inboxWroteTexture = inboxWroteTexture || !outcome.textures.empty();
        for (const auto& [was, is] : outcome.renamed) {
            SOL_LOG_INFO("forge: renamed part '%s' -> '%s'", was.c_str(), is.c_str());
        }
        SOL_LOG_INFO("forge: imported %s -> %s", gltfPath.c_str(), result.message.c_str());

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
                    //
                    // ⚑⚑ NAMED RATHER THAN OPENED, because a drain can carry
                    // several (stage R): opening each in turn leaves the author
                    // looking at the last one having watched three others flash
                    // past. The caller opens one, once, when it knows how many
                    // there were.
                    if (!editor.dirty()) {
                        result.openRow = static_cast<int>(i);
                    }
                    break;
                }
            }
        }
        result.ok = true;
        return result;
    };

    // ⚑⚑⚑ ONE RULE, AND IT REPLACED TWO CONTRADICTORY GUESSES (stage R):
    // WHATEVER IS IN THE INBOX IS PENDING, AND PENDING DRAINS. There used to be
    // a launch census that assumed everything present had already been imported
    // - so a drop made while the Forge was shut was never imported, ever - and
    // an `Import now` that assumed nothing had, so one click re-imported every
    // stale drop at once. Both existed only because "done" was not written down
    // anywhere. It is a directory now, so neither guess is needed and the launch
    // case and the running case are the same case.
    const auto pollInbox = [&](bool announceEmpty) {
        const std::vector<std::string> pending =
            forge::forgePendingDrops(platform::listFiles(inboxDirectory.c_str()), inboxDirectory);

        int imported = 0;
        int openRow = -1;
        std::string lastMessage;
        for (const std::string& path : pending) {
            const std::uint64_t modified = platform::fileModificationTime(path.c_str());
            const auto failed = std::find_if(inboxFailed.begin(),
                                             inboxFailed.end(),
                                             [&path](const FailedDrop& f) { return f.path == path; });
            // A drop that could not be imported stays in the inbox, so without
            // this it would be retried on every poll for the rest of the
            // session. Retried only when the file itself changes, which is the
            // author having done something about it.
            if (failed != inboxFailed.end() && failed->modified == modified) {
                continue;
            }

            const InboxImport result = importFromInbox(path);
            if (result.ok) {
                ++imported;
                lastMessage = result.message;
                if (result.openRow >= 0) {
                    openRow = result.openRow;
                }
                if (failed != inboxFailed.end()) {
                    inboxFailed.erase(failed);
                }
            } else if (failed != inboxFailed.end()) {
                failed->modified = modified;
                failed->error = result.message;
            } else {
                inboxFailed.push_back({path, modified, result.message});
            }
        }

        // ⚑⚑ ONE MESSAGE AND ONE OPENED DOCUMENT FOR A DRAIN OF ANY SIZE.
        // Q4 made the status line an event, and four events in one frame is
        // three the author never sees - measured before this stage, where one
        // click imported four drops and the bar named only the last.
        if (imported == 1) {
            inboxStatus = lastMessage;
        } else if (imported > 1) {
            inboxStatus = "imported " + std::to_string(imported) + " drops";
        }
        if (imported > 0) {
            // ⚑⚑ OPENED FIRST AND ANNOUNCED SECOND, AND THE ORDER IS THE WHOLE
            // POINT: `openMeshAt` writes its own message. Announcing before
            // opening let a four-drop drain report `opened Cube_039.forge (40
            // parts)` and nothing about the other three - this stage's own
            // defect reappearing inside its own fix. Caught by driving it
            // rather than by reading it.
            if (openRow >= 0) {
                openMeshAt(openRow);
            }
            status = inboxStatus;
        } else if (announceEmpty) {
            inboxStatus = pending.empty()
                              ? "nothing waiting in " + inboxDirectory
                              : std::to_string(pending.size()) + " drop(s) waiting, none importable";
            status = inboxStatus;
        }
        return imported;
    };

    // The archive is made once, here, rather than re-derived per move.
    if (!platform::createDirectories(inboxArchive.c_str())) {
        SOL_LOG_ERROR("forge: cannot create %s - imported drops will stay in the inbox",
                      inboxArchive.c_str());
    }
    SOL_LOG_INFO("forge: watching %s (%zu waiting, filing into %s)",
                 inboxDirectory.c_str(),
                 forge::forgePendingDrops(platform::listFiles(inboxDirectory.c_str()), inboxDirectory).size(),
                 inboxArchive.c_str());

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
    bool redoPressed = false;
    // ⚑⚑⚑ EVERY TEXTURE REBUILD IS RAISED HERE AND PERFORMED AT ONE POINT NEAR
    // THE TOP OF THE FRAME, BECAUSE REBUILDING MID-FRAME FREES A DESCRIPTOR SET
    // THIS FRAME'S DRAW LIST ALREADY NAMES. `rebuildTexture` ends in
    // `refreshTexturePreview`, which calls `ImGui_ImplVulkan_RemoveTexture` ->
    // `vkFreeDescriptorSets` IMMEDIATELY (imgui_impl_vulkan.cpp:1253 - no fence,
    // no frame-in-flight tracking). The Texture panel has already recorded that
    // handle via `AddImage` (`texture_editor.cpp`) by the time `drawPreview`
    // returns true, so the bind recorded at render time names a freed set.
    //
    // ⚑ `context.waitIdle()` inside `rebuildTexture` does NOT cover this: it
    // waits for SUBMITTED work, and this frame has not been submitted yet.
    //
    // ⚑⚑ IT IS INTERMITTENT BY NATURE, WHICH IS WHY IT SURVIVED: free-then-
    // allocate usually hands the SAME pool slot straight back, leaving the
    // recorded handle accidentally valid. When the pool hands out a fresh slot
    // instead, the same code faults - the user's log shows handles stepping +8
    // per burst, i.e. no reuse, while eight driven gestures here never once
    // failed to reuse. Do not treat "it did not reproduce" as "it is fixed".
    bool textureRebuildPending = false;
    // ⚑⚑ SAME REASON, SECOND ROUTE: `openTextureAt` ALSO ends in
    // `refreshTexturePreview`, so a panel that switches texture mid-frame frees
    // the set too. The Textures list happens to sit ABOVE the preview and the
    // `View` combo sits BELOW it - so one is safe today and one is not, and
    // which is which depends on the order four dockable panels are submitted in.
    // ⚑ Both go through here rather than one, because "safe as long as nobody
    // moves that call" is the shape of defect this tool keeps rediscovering.
    int pendingTextureOpen = -1;
    // ⚑⚑ A THIRD ROUTE TO THE SAME FREE (stage U2), AND THE LARGEST: a reload
    // frees `texturePreview` AND every image behind it. The Cook button happens
    // to be submitted above all four panels today, so doing it inline would work
    // - which is exactly the "safe as long as nobody moves that call" the
    // paragraph above says this tool keeps rediscovering. So it is raised too.
    bool textureReloadPending = false;
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
        viewport.focal = ui::focalLength(viewport.height, std::tan(forge::kCameraVerticalFov * 0.5f));
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

        // ⚑⚑⚑ STAGE Q: ONE PLACE THAT UNDOES, AND IT ASKS THE HISTORY WHICH
        // EDITOR RATHER THAN ASSUMING ONE. Before Q this called `editor.undo()`
        // unconditionally, so a press while working on a texture silently
        // deleted a part from a mesh document that was not even on screen -
        // measured at `332 tri -> 320 tri` with only the Texture panel visible.
        //
        // ⚑ Both chords are edge triggered: a held chord must step once, not
        // sixty times. The keyboard guard is unchanged and is about ImGui
        // owning the keys while a text field is active, NOT about which panel
        // is focused - focus is not usable here, because clicking the 3D
        // viewport drops it entirely. See `edit_history.hpp`.
        const bool controlDown =
            window.isKeyDown(platform::Key::LeftControl) && !imguiHost.wantsKeyboardCapture();
        const bool undoDown = controlDown && window.isKeyDown(platform::Key::Z);
        // ⚑ `Ctrl+Y` as well as `Ctrl+Shift+Z`, because both are muscle memory
        // and neither costs anything - and `Shift` is otherwise the pan
        // modifier here, which is a mouse gesture and cannot collide.
        const bool redoDown = controlDown && (window.isKeyDown(platform::Key::Y) ||
                                              (window.isKeyDown(platform::Key::Z) && shiftDown));

        // ⚑ TAKEN UNCONDITIONALLY AND BEFORE THE `||`, not inside it: these are
        // consume-once reads, and short-circuiting past one leaves a button
        // press pending that would fire again on the next frame. The panels'
        // buttons arrive here one frame late, because the panel pass runs below
        // this - 16 ms, and it is the same ordering stage O measured.
        const bool undoButton = history.takeUndoRequest();
        const bool redoButton = history.takeRedoRequest();
        const bool undoNow = (undoDown && !undoPressed) || undoButton;
        const bool redoNow = (redoDown && !redoPressed) || redoButton;

        // Redo is tested FIRST because Ctrl+Shift+Z satisfies the undo chord
        // too, and taking a step back when the author asked for one forward is
        // worse than either doing nothing.
        if (redoNow) {
            std::string label;
            const bool stepped = history.redo(
                [&](forge::EditHistory::Editor which) {
                    switch (which) {
                    case forge::EditHistory::Editor::Mesh:
                        return editor.redoStep();
                    case forge::EditHistory::Editor::Texture:
                        return textureEditor.redoStep();
                    case forge::EditHistory::Editor::Def:
                        return defEditor.redoStep();
                    }
                    return false;
                },
                &label);
            if (stepped) {
                status = "redo: " + label;
                rebuildFromEditor(/*reframe=*/false);
                rebuildTexture();
            }
        } else if (undoNow) {
            std::string label;
            const bool stepped = history.undo(
                [&](forge::EditHistory::Editor which) {
                    switch (which) {
                    case forge::EditHistory::Editor::Mesh:
                        return editor.undoStep();
                    case forge::EditHistory::Editor::Texture:
                        return textureEditor.undoStep();
                    case forge::EditHistory::Editor::Def:
                        return defEditor.undoStep();
                    }
                    return false;
                },
                &label);
            if (stepped) {
                status = "undo: " + label;
                // ⚑ BOTH REBUILDS, UNCONDITIONALLY, and that is the price of
                // the global model: the step may have landed in a document
                // this frame is not showing, and the one that did change is
                // the one that must be re-uploaded. Neither call does any work
                // when its editor is closed.
                rebuildFromEditor(/*reframe=*/false);
                textureRebuildPending = true;
            }
        }
        undoPressed = undoDown;
        redoPressed = redoDown;

        // ⚑ Stage U3's import RAISES a request rather than reloading where it
        // writes, and this is where the raise becomes one. An import runs from
        // `pollInbox`, below the panels, so reloading there would free
        // `texturePreview` and every image behind it out from under the frame
        // still being submitted - the exact hazard the block below exists for.
        // Translated here rather than set directly at the import because the
        // import lambda is built long before `textureReloadPending`.
        if (inboxWroteTexture) {
            inboxWroteTexture = false;
            textureReloadPending = true;
        }

        // ⚑⚑ THE ONE PLACE A TEXTURE IS REBUILT, AND IT IS HERE BECAUSE THIS IS
        // ABOVE EVERY `Begin` IN THE TOOL - the menu bar, both side bars and all
        // four panels are submitted below, so no draw list can yet be naming the
        // descriptor set this may free. Requests raised by the panels therefore
        // land on the NEXT frame (16 ms, invisible, and the same trade stage Q
        // already took for the panels' undo buttons); the undo/redo block just
        // above is early enough to be served in this one.
        //
        // ⚑ Collapsing to one call per frame is a side benefit rather than the
        // point: a drag in the picker used to rebuild once per widget per frame.
        // ⚑ The reload goes FIRST because it invalidates the other two: a
        // pending index refers to the list as it was before the cook, and a
        // pending rebuild would write into a slot that is about to be destroyed.
        // Both are dropped rather than translated - the same call the Cook button
        // already makes for `openIndex` and `soundIndex`, for the same reason.
        if (textureReloadPending) {
            textureReloadPending = false;
            pendingTextureOpen = -1;
            textureRebuildPending = false;
            reloadTextures();
        }
        if (pendingTextureOpen >= 0) {
            const int index = pendingTextureOpen;
            pendingTextureOpen = -1;
            openTextureAt(index);
        }
        if (textureRebuildPending) {
            textureRebuildPending = false;
            rebuildTexture();
        }
        // ⚑⚑ AFTER THE TEXTURE WORK, ALWAYS, AND THE ORDER IS THE WHOLE POINT.
        // A material's set 1 names image views owned by `textures`; a reload
        // destroys and recreates all of them. Rewriting the sets first would
        // record handles that the reload one line later invalidates - which is
        // the same "raised and served in one place" discipline the three above
        // exist for, in the one direction that is not obvious.
        if (materialRebuildPending) {
            materialRebuildPending = false;
            materialSetsStale = false;
            // A rebuild repacks every params buffer from the new reflection, so
            // a write pending against the OLD buffer is not just redundant, it
            // is a write into memory that has been unmapped.
            materialParamsPending = forge::DefEditor::kNoMaterial;
            rebuildMaterials();
        }
        if (materialSetsStale) {
            materialSetsStale = false;
            context.waitIdle();
            (void)writeMaterialSets();
        }
        if (materialParamsPending != forge::DefEditor::kNoMaterial) {
            const std::uint32_t index = materialParamsPending;
            materialParamsPending = forge::DefEditor::kNoMaterial;
            // ⚑ NO `waitIdle`. The buffer is host-visible and stays mapped, and
            // a param is read per fragment rather than per submission - so the
            // worst a frame in flight can see is the new value one frame early,
            // which is what "the mesh changes while your hand is on the number"
            // means. Idling the device here would make a drag stutter to buy
            // nothing an author could perceive.
            if (index < defEditor.materials().size()) {
                (void)view.materials().setParams(index, defEditor.materials()[index].params);
            }
        }

        // ⚑⚑ WHICH MESH IS OPEN, TOLD TO THE DEF EDITOR ONCE, ABOVE EVERY
        // `Begin`. It used to be a side effect of the Report panel drawing its
        // rows, which was true until stage D put the Material panel in the same
        // dock node and made Report a tab BEHIND it - a hidden ImGui window is
        // not submitted, so opening a second mesh left the tool drawing it with
        // the first one's material. One place, every frame, no window involved.
        defEditor.setOpenMesh(openIndex >= 0 && openIndex < static_cast<int>(meshEntries.size())
                                  ? meshEntries[static_cast<std::size_t>(openIndex)].stem
                                  : std::string());

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
            camera.pan(
                mouseDelta.x, mouseDelta.y, static_cast<float>(window.height()), forge::kCameraVerticalFov);
        }
        if (!imguiHost.wantsMouseCapture()) {
            const float wheel = window.wheelDelta();
            if (wheel != 0.0f) {
                camera.dolly(wheel);
            }
        }
        const bool frameDown = window.isKeyDown(platform::Key::F) && !imguiHost.wantsKeyboardCapture();
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
            addWireBox(view.debugDraw(), {-half, -half, -half}, {half, half, half}, reference.color);
        }
        if (showBounds && (openIndex >= 0 || editor.isOpen())) {
            addWireBox(view.debugDraw(), report.boundsMin, report.boundsMax, {0.30f, 0.75f, 0.35f, 1.0f});
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
                ImGui::MenuItem("Sound", nullptr, &showSound);
                ImGui::MenuItem("Material", nullptr, &showMaterial);
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
                    // ⚑⚑ AN HONEST "LOOK NOW", AND IT USED TO BE ANYTHING BUT
                    // (stage R). It cleared everything the tool had seen, so
                    // one click re-imported every stale drop in the directory -
                    // measured at four documents written from a single press.
                    // With an archive there is nothing to forget: what is still
                    // in the inbox is what has not been imported.
                    (void)pollInbox(/*announceEmpty=*/true);
                }
                ImGui::Separator();
                ImGui::TextDisabled("drop .gltf into");
                ImGui::TextDisabled("%s", inboxDirectory.c_str());
                // ⚑ The two states disk cannot show at a glance: what is
                // waiting, and what was tried and could not be read. Everything
                // else is visible in the archive directory itself.
                const std::vector<std::string> waiting =
                    forge::forgePendingDrops(platform::listFiles(inboxDirectory.c_str()), inboxDirectory);
                if (!waiting.empty()) {
                    ImGui::Separator();
                    for (const std::string& path : waiting) {
                        const auto failedDrop =
                            std::find_if(inboxFailed.begin(),
                                         inboxFailed.end(),
                                         [&path](const FailedDrop& f) { return f.path == path; });
                        if (failedDrop != inboxFailed.end()) {
                            ImGui::TextDisabled("failed  %s", forgeFileStem(path).c_str());
                            ImGui::TextDisabled("        %s", failedDrop->error.c_str());
                        } else {
                            ImGui::TextDisabled("waiting  %s", forgeFileStem(path).c_str());
                        }
                    }
                }
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
                    showMesh = showReport = showTexture = showSound = showMaterial = showView = true;
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
        const ImGuiWindowFlags kBarFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        const float toolbarHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
        if (ImGui::BeginViewportSideBar(
                "##toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, toolbarHeight, kBarFlags)) {
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

            // ⚑⚑ THE COOK (Phase 24 stage T), AND IT IS A BUTTON RATHER THAN A
            // MENU ITEM BECAUSE OF WHAT IT IS. The Blender bridge got a menu
            // because it is meant to be invisible - you press `Send to Forge`
            // over there and the mesh is here. A cook is the opposite: a
            // deliberate act, repeated after every edit, whose result you wait
            // for. It is never disabled, because it is the one action in this
            // bar that needs nothing open.
            //
            // ⚑⚑ IT CALLS THE COOKER'S OWN DISPATCH RATHER THAN A SECOND COPY
            // OF IT, AND THAT IS THE WHOLE POINT OF THE STAGE. Until now the
            // dispatch lived in `tools/cooker/src/main.cpp`, which nothing can
            // link; it is in `sol_cooker_lib` now, which this tool already
            // linked for the glTF importer. So the button runs exactly what
            // `cooker.exe` runs, logs the same lines, and needs no cooker
            // executable shipped beside the Forge - which is what keeps
            // `packaging/check_layout.cmake`'s `cooker*` exclusion true.
            //
            // ⚑ It blocks the frame, deliberately. The committed set is 26
            // files and 2,298 triangles, so the pause is not worth a thread or
            // a progress bar - and a cook that returned before it had finished
            // would be a button that lies about the thing an author is about to
            // go and look at.
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 4.0f);
            if (ImGui::Button("Cook")) {
                const cooker::CookReport cookReport = cooker::cookDirectory(assetsDirectory, cookedDirectory);
                status = "cook: " + cookReport.summary();

                // A cook adds cooked files, and sweeps the ones whose source is
                // gone - so the list read at startup is now a list of something
                // else, and `openIndex` indexes straight into it. Same pair as
                // `Rescan` for the same reason.
                meshEntries = forge::listMeshes(assetsDirectory, cookedDirectory);
                openIndex = -1;

                // ⚑ A sound has no GPU side at all, so `rebuild` is the whole of
                // it - close the device, refill the bank, reopen.
                soundEntries = forge::listSounds(assetsDirectory, cookedDirectory);
                soundIndex = -1;
                soundPreview.rebuild(soundEntries);

                // ⚑⚑⚑ AND TEXTURES ARE REFRESHED TOO SINCE STAGE U2, WHICH IS
                // WHAT CLOSES THE LAST OF THE THREE LOOPS INSIDE ONE LAUNCH.
                // This line used to be a paragraph explaining why it could not be
                // done: a texture cannot be re-listed without being re-uploaded,
                // `textureStems` is derived from what uploaded, and offering a
                // stem the viewport cannot draw is the mistake stage H exists to
                // make inexpressible. All three are still true - `reloadTextures`
                // SATISFIES them rather than working around them, which is why it
                // is a lambda carrying an invariant and not two lines here.
                //
                // ⚑ RAISED, NOT PERFORMED. See `textureReloadPending`.
                textureReloadPending = true;
            }
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
        // ⚑ The bar is submitted ABOVE the panels that write to it, so a
        // message raised by a panel is stamped on the NEXT frame (16 ms,
        // invisible, and the same trade every other cross-pass value in this
        // loop already takes). What has to hold is that the stamp and the text
        // move together, and they do, because both follow the serial.
        if (status.serial() != statusSerialSeen) {
            statusSerialSeen = status.serial();
            statusStampedAt = ImGui::GetTime();
        }
        const forge::StatusAppearance statusLook =
            forge::statusAppearance(static_cast<float>(ImGui::GetTime() - statusStampedAt));

        const float statusHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
        if (ImGui::BeginViewportSideBar(
                "##status", ImGui::GetMainViewport(), ImGuiDir_Down, statusHeight, kBarFlags)) {
            if (openIndex >= 0 || editor.isOpen()) {
                ImGui::TextDisabled("%u tri   vol %.4g m3   %s   %u border edge%s",
                                    report.triangles,
                                    report.signedVolume,
                                    !report.manifold ? "not manifold"
                                    : report.closed  ? "closed"
                                                     : "open",
                                    report.borderEdges,
                                    report.borderEdges == 1 ? "" : "s");
            } else {
                ImGui::TextDisabled("no mesh open");
            }

            // Right-aligned, because it is the one reading here that is about the
            // TOOL rather than about the mesh.
            char frameText[96];
            std::snprintf(frameText,
                          sizeof(frameText),
                          "%.1f fps  (%.2f ms)   grid %.2g m",
                          frameMilliseconds > 0.0f ? 1000.0f / frameMilliseconds : 0.0f,
                          frameMilliseconds,
                          static_cast<double>(gridCell));
            const float frameWidth = ImGui::CalcTextSize(frameText).x;
            // Measured BEFORE the message is drawn, because it is what bounds it.
            const float frameStart = ImGui::GetWindowWidth() - frameWidth - 16.0f;

            // ⚑⚑⚑ THE MESSAGE LINE LIVES HERE NOW, AND MOVING IT IS A DEFECT FIX
            // RATHER THAN A TIDY-UP. It had exactly one draw site in the whole
            // tool - inside `Begin("Mesh")`, inside `CollapsingHeader("Meshes")`,
            // below the mesh list and the Reload/New parts/Rescan buttons - so
            // `undo: cell` was invisible whenever the Mesh panel was closed,
            // tabbed behind another panel, or that one header was collapsed.
            //
            // ⚑⚑ WHICH IS EXACTLY THE CASE IT EXISTS FOR. A cross-document undo
            // is BY DEFINITION one the author is not looking at - that is what
            // makes it worth announcing - so the announcement was hidden
            // precisely when it was the only evidence anything had happened.
            // Stage Q stopped undo ACTING on a document you cannot see and left
            // it REPORTING into one. Found by the user in about a minute.
            //
            // ⚑ The bar is the only surface with the property the message needs,
            // and it is the same argument that carried the summary block here:
            // `BeginViewportSideBar` sets NoDocking, so this line cannot be
            // closed, docked, tabbed or dragged off.
            // ⚑⚑ STAGE Q4: THE MESSAGE IS DRAWN AS AN EVENT NOW - it arrives
            // lit, settles into chrome over `kStatusFlashSeconds`, and is gone
            // after `kStatusLifetimeSeconds`. The flash is what makes a REPEAT
            // visible: two undos of two `cell` edits write the same string
            // both times, so before this the bar was pixel-identical and the
            // second press read as ignored. It re-fires on every write,
            // identical text or not, because the serial moves on every write.
            if (!status.empty() && statusLook.visible) {
                ImGui::SameLine(0.0f, 32.0f);
                const float available = frameStart - ImGui::GetCursorPosX() - 16.0f;
                if (available > 0.0f) {
                    // Lerped rather than switched: a hard cut back to grey is
                    // itself a change of state, and it would draw the eye a
                    // second time to say nothing.
                    const ImVec4 settled = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    const ImVec4 fresh = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    const float t = statusLook.highlight;
                    const ImVec4 colour(settled.x + (fresh.x - settled.x) * t,
                                        settled.y + (fresh.y - settled.y) * t,
                                        settled.z + (fresh.z - settled.z) * t,
                                        settled.w + (fresh.w - settled.w) * t);
                    // `display()` carries the `(xN)` count, which is the half of
                    // the repeat fix that survives you looking away.
                    ImGui::TextColored(colour, "%s", elideToWidth(status.display(), available).c_str());
                }
            }

            ImGui::SameLine(frameStart);
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
        ImGui::DockSpaceOverViewport(
            dockspaceId, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

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
            leftId = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.24f, nullptr, &centreId);
            rightId = ImGui::DockBuilderSplitNode(centreId, ImGuiDir_Right, 0.32f, nullptr, &centreId);
            ImGui::DockBuilderDockWindow("Mesh", leftId);
            ImGui::DockBuilderDockWindow("Texture", leftId);
            // Stage U1's panel joins the left column beside `Texture` for the
            // reason those two are already there: both are an asset list, a
            // preview of that asset as the game receives it, and the def rows
            // naming it. `Sound` is that shape a third time.
            ImGui::DockBuilderDockWindow("Sound", leftId);
            // Stage 25-D. Beside `Report` rather than in the asset column: it
            // is not a list of files, it is what the open mesh is DRAWN with -
            // and it is read against the report and the viewport at the same
            // time, which is exactly what the right column is for.
            ImGui::DockBuilderDockWindow("Material", rightId);
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
                    // ⚑ `status` was printed here until the undo message needed
                    // to be seen from outside this panel. MOVED, not copied: two
                    // draw sites for one string is a line that reads differently
                    // depending on which one you happen to be looking at, and
                    // this was the site nobody was looking at. It is in the
                    // bottom bar now, beside the summary block.
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
                if (editor.isOpen() &&
                    ImGui::CollapsingHeader("Points, edges & faces", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // ⚑ It can change the document since E5: a split and an extrude
                    // are presses rather than drags, so this panel is a third place
                    // an edit can come from and it needs the same rebuild.
                    if (points.drawPanel(editor, editor.hoveredPart())) {
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
                    ImGui::Text("size (m)       %8.3f %8.3f %8.3f",
                                static_cast<double>(size.x),
                                static_cast<double>(size.y),
                                static_cast<double>(size.z));
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
                if (openIndex >= 0 && ImGui::CollapsingHeader("Def rows", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // The panel measures against the editor's own validated reading
                    // of the text, so neither call needs the boot-time catalog.
                    (void)defEditor.drawModelRows(
                        meshEntries[static_cast<std::size_t>(openIndex)], report, textureStems);
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
                    // Stage Q: `undo def` was the third button in this tool
                    // that undid only its own editor. It is the same pair as
                    // the other two panels now, meaning the same thing.
                    forge::drawHistoryButtons(history);
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
                            const float switchPixels =
                                assets::kLevelSwitchPixels[i < std::size(assets::kLevelSwitchPixels)
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
                        ImGui::Text("lod%d  %u tri (%.0f%%)  %zu B",
                                    static_cast<int>(i) + 1,
                                    level.triangles,
                                    report.triangles > 0
                                        ? 100.0 * level.triangles / static_cast<double>(report.triangles)
                                        : 0.0,
                                    level.cookedBytes);
                        // Signed, both of them: a level that GREW its volume is as
                        // wrong as one that shrank, and the radius growing outward
                        // is the one that pushes the hull past its collision sphere.
                        ImGui::TextDisabled(
                            "      volume %+.2f%%   radius %+.2f%%   from %.0f m",
                            level.volumeDrift * 100.0,
                            level.radiusDrift * 100.0,
                            static_cast<double>(report.boundingRadius * focal / switchPixels));
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
                    const float texHeight = forge::listHeight(
                        forge::textRowMetrics(), textureLabels.size(), forge::kMinListRows, 0.20f, texOuter);
                    if (ImGui::BeginChild("##textures", {0.0f, texHeight}, ImGuiChildFlags_Borders)) {
                        for (int i = 0; i < static_cast<int>(textureLabels.size()); ++i) {
                            if (ImGui::Selectable(textureLabels[static_cast<std::size_t>(i)].c_str(),
                                                  i == textureIndex)) {
                                pendingTextureOpen = i;
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
                            if (textureEditor.drawPreview(texturePreview, ImGui::GetContentRegionAvail().x)) {
                                // ⚑ RAISED, NOT PERFORMED. `drawPreview` has
                                // already recorded `texturePreview` into this
                                // frame's draw list, so freeing it here is a
                                // use-after-free that only shows when the pool
                                // declines to hand the same slot back.
                                textureRebuildPending = true;
                            }
                            ImGui::TextDisabled("as cooked (BC1) - click a shape, drag to move it");
                        } else {
                            // A cooked texture has no document behind it, so there is
                            // nothing to pick: it stays the picture it always was.
                            ImGui::Image(reinterpret_cast<ImTextureID>(texturePreview), {200.0f, 200.0f});
                            ImGui::SameLine();
                            ImGui::TextDisabled("as cooked\n(BC1)");
                        }
                    }
                    if (ImGui::Button("new texture")) {
                        textureEditor.openNew(assetsDirectory + "/textures");
                        editingTextureIndex = -1;
                        textureRebuildPending = true;
                    }
                    ImGui::SameLine();
                    // ⚑ "cooked textures are read-only" until stage U2, and it
                    // stopped being true the moment a `.png` could be selected:
                    // an imported image is read-only and is not cooked, so the
                    // one sentence that used to cover the whole else-branch now
                    // covers half of it and misnames the other half. A hint that
                    // is wrong about why is worse than no hint - it sends an
                    // author looking for a cooked file they never made.
                    const bool importedSelected =
                        textureIndex >= 0 && textureIndex < static_cast<int>(loadedTextureEntries.size()) &&
                        forge::isImportedTexture(
                            loadedTextureEntries[static_cast<std::size_t>(textureIndex)]);
                    ImGui::TextDisabled("%s",
                                        textureEditor.isOpen() ? "editing the selected source"
                                        : importedSelected     ? "imported images are read-only here"
                                                               : "cooked textures are read-only");
                    ImGui::Separator();
                    if (textureEditor.draw()) {
                        textureRebuildPending = true;
                    }
                }
            }
            ImGui::End();
        }

        // ⚑⚑⚑ THE THIRD ASSET KIND (PHASE 24 STAGE U1), AND THE ONE THIS TOOL
        // COULD NOT SEE AT ALL. `assets/sounds/` has held nine files since
        // Phase 8t and the Forge listed none of them, so the only way to hear
        // whether a cue was too loud was to build the game, fly to something
        // that fires it, and edit `sounds.toml` in a text editor between
        // attempts. This panel is that loop collapsed into one window.
        //
        // ⚑ It is the SAME SHAPE as `Texture` on purpose - a list, the asset as
        // the game receives it, then the def rows that name it - because the
        // shape is now the pattern rather than a coincidence, and an author who
        // has learned one of these three panels has learned all of them.
        if (showSound) {
            // ⚑ HERE rather than beside the dock builder, and that placement is
            // the correction to a first draft that was wrong: `SetNextWindowDockID`
            // applies to the NEXT `Begin`, and three other windows are submitted
            // between the layout block and this one - so setting it up there
            // would have docked `Mesh` into the texture node and left `Sound`
            // floating anyway. A "next window" call belongs against its window.
            if (adoptSoundPanel) {
                const ImGuiWindow* soundWindow = ImGui::FindWindowByName("Sound");
                const ImGuiWindow* textureWindow = ImGui::FindWindowByName("Texture");
                if (soundWindow != nullptr && soundWindow->DockId != 0) {
                    // Already placed - by the default layout, by the ini, or by
                    // the author dragging it. Nothing to adopt, ever again.
                    adoptSoundPanel = false;
                } else if (textureWindow != nullptr && textureWindow->DockId != 0) {
                    // `DockId == 0` is the whole test: it is what "no saved
                    // layout has ever placed this window" means. Asked of
                    // `Texture` too, because docking into a node that does not
                    // exist yet is docking into nothing.
                    ImGui::SetNextWindowDockID(textureWindow->DockId, ImGuiCond_Always);
                    ImGui::MarkIniSettingsDirty();
                    adoptSoundPanel = false;
                }
            }
            if (ImGui::Begin("Sound", &showSound, kPanelFlags)) {
                if (ImGui::CollapsingHeader("Sounds", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const int clicked =
                        drawAssetList("##sounds", soundEntries, soundIndex, 0.30f, soundListContent);
                    if (clicked >= 0) {
                        soundIndex = clicked;
                        // ⚑ Selecting PLAYS it, which is the one place this
                        // panel departs from `Texture` - and the reason is that
                        // a picture is shown by being selected while a sound is
                        // not. A list that made you click twice to hear
                        // anything would be a picture viewer with the picture
                        // left out.
                        soundPreview.play(soundIndex, 1.0f, 0.0f, 0);
                    }

                    const bool haveSound =
                        soundIndex >= 0 && static_cast<std::size_t>(soundIndex) < soundEntries.size();
                    ImGui::BeginDisabled(!soundPreview.canPlay(soundIndex));
                    if (ImGui::Button("play file")) {
                        // ⚑ Gain 1 and no jitter: this is "what is in the
                        // file", the raw material. The cue as the GAME fires it
                        // is the button on the def row below, and keeping the
                        // two separate is what lets an author tell a quiet
                        // recording from a low `gain`.
                        soundPreview.play(soundIndex, 1.0f, 0.0f, 0);
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("stop")) {
                        soundPreview.stopAll();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Rescan")) {
                        soundEntries = forge::listSounds(assetsDirectory, cookedDirectory);
                        soundIndex = -1;
                        // ⚑ The pair, and it is not optional: the preview's
                        // banks are indexed BY POSITION in this listing, so a
                        // re-list without a rebuild plays the wrong sound - or
                        // reads past the end of the bank.
                        soundPreview.rebuild(soundEntries);
                        status = std::to_string(soundEntries.size()) + " sound(s)";
                    }

                    ImGui::TextDisabled(
                        "%s | %u voice(s)", soundPreview.status().c_str(), soundPreview.activeVoices());

                    if (haveSound) {
                        const forge::AssetEntry& entry = soundEntries[static_cast<std::size_t>(soundIndex)];
                        const forge::SoundReport& soundReport = soundPreview.report(soundIndex);
                        ImGui::Separator();
                        ImGui::Text("%s", entry.label.c_str());
                        if (soundReport.sampleRate == 0) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
                            ImGui::TextWrapped("this file did not decode - see the log");
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::Text("%.3f s   %u Hz   %s",
                                        static_cast<double>(soundReport.seconds),
                                        soundReport.sampleRate,
                                        soundReport.channelCount == 1 ? "mono" : "stereo");
                            ImGui::Text("frames         %u", soundReport.frames);
                            // ⚑ The number `gain` is a number ABOUT. A cue
                            // recorded at 0.2 and one clipping at 1.0 want
                            // opposite edits and the gain slider cannot tell
                            // you which you have.
                            ImGui::Text("peak           %.3f", static_cast<double>(soundReport.peak));
                            if (soundReport.peak >= 0.999f) {
                                ImGui::TextDisabled("  at full scale: this cue may already clip");
                            }

                            // ⚑⚑⚑ THE SHAPE THOSE THREE NUMBERS ARE SUMMARIES OF
                            // (PHASE 26 STAGE C). A cue that peaks at 0.85 once and a cue
                            // that sits at 0.85 for a tenth of a second print the SAME
                            // three lines above, and they are not the same cue. The
                            // envelope keeps the extremes of every column, so the `peak`
                            // printed above is literally the top of the drawing rather
                            // than a number beside an unrelated picture.
                            drawWaveform(soundPreview, soundIndex, soundReport);
                        }
                        // ⚑ THE SENTENCE THAT MAKES THE PREVIEW HONEST, and it
                        // is stage G's BC1 argument arriving at a second
                        // format. A `.wav` is imported by the cooker's own
                        // importer into the exact struct a `.saud` serialises,
                        // so what plays here is sample-for-sample what the game
                        // would load - which is why the pair in the list can be
                        // played against each other and sound identical.
                        ImGui::TextDisabled("%s",
                                            forge::isSoundSource(entry)
                                                ? "as cooked: imported exactly as the cooker would"
                                                : "the cooked .saud the game loads");
                        ImGui::SeparatorText("in the game");
                        forge::DefEditor::Audition audition;
                        (void)defEditor.drawSoundRows(entry, audition);
                        if (audition.wanted) {
                            soundPreview.play(
                                soundIndex, audition.gain, audition.pitchJitter, audition.maxInstances);
                        }
                        if (ImGui::Button("save defs##sound")) {
                            (void)defEditor.save(defStatus);
                        }
                        ImGui::SameLine();
                        forge::drawHistoryButtons(history);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", defEditor.dirty() ? "* unsaved" : "saved");
                        ImGui::TextDisabled("%s", defStatus.c_str());
                    } else {
                        ImGui::TextDisabled("select a sound to hear it and to see the cue that "
                                            "names it");
                    }
                }
            }
            ImGui::End();
        }

        // ⚑⚑⚑ THE FOURTH ASSET KIND AND THE FIRST THAT IS NOT A FILE (PHASE 25
        // STAGE D). A mesh, a texture and a sound are each something on disk
        // this tool lists; a material is a ROW, and the one it shows is not
        // picked from a list at all - it is whatever the open mesh's `[[model]]`
        // row names. That is deliberate: a list you pick from could put a
        // surface on this mesh that the game will never draw on it, and the one
        // claim this viewport has always been able to make is that what you are
        // looking at is what ships.
        if (showMaterial) {
            if (adoptMaterialPanel) {
                // Stage U1's fix, second customer. See `adoptSoundPanel`.
                const ImGuiWindow* materialWindow = ImGui::FindWindowByName("Material");
                const ImGuiWindow* reportWindow = ImGui::FindWindowByName("Report");
                if (materialWindow != nullptr && materialWindow->DockId != 0) {
                    adoptMaterialPanel = false;
                } else if (reportWindow != nullptr && reportWindow->DockId != 0) {
                    ImGui::SetNextWindowDockID(reportWindow->DockId, ImGuiCond_Always);
                    ImGui::MarkIniSettingsDirty();
                    adoptMaterialPanel = false;
                }
            }
            if (ImGui::Begin("Material", &showMaterial, kPanelFlags)) {
                if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (openIndex < 0) {
                        ImGui::TextDisabled("no mesh open");
                    } else {
                        const std::string shownStem =
                            textureIndex >= 0 && textureIndex < static_cast<int>(loadedTextureEntries.size())
                                ? loadedTextureEntries[static_cast<std::size_t>(textureIndex)].stem
                                : std::string();
                        std::string wantTexture;
                        const forge::DefEditor::MaterialEdit edit = defEditor.drawMaterialRows(
                            textureStems, vertexShaderStems, fragmentShaderStems, shownStem, &wantTexture);
                        // ⚑ RAISED, NOT PERFORMED - this panel is submitted well
                        // below the top of the frame, and a rebuild frees
                        // pipelines and descriptor sets that this frame's draw
                        // list will name. Same rule as every texture rebuild in
                        // this tool, for the same reason.
                        if (edit == forge::DefEditor::MaterialEdit::Structure) {
                            materialRebuildPending = true;
                        } else if (edit == forge::DefEditor::MaterialEdit::Params) {
                            materialParamsPending = defEditor.openMaterialIndex();
                        }
                        if (!wantTexture.empty()) {
                            for (std::size_t i = 0; i < loadedTextureEntries.size(); ++i) {
                                if (loadedTextureEntries[i].stem == wantTexture) {
                                    pendingTextureOpen = static_cast<int>(i);
                                    break;
                                }
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::Button("save defs")) {
                            (void)defEditor.save(defStatus);
                            status = defStatus;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", defEditor.dirty() ? "* unsaved" : "saved");
                        if (!materialStatus.empty()) {
                            ImGui::TextDisabled("%s", materialStatus.c_str());
                        }
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
                                pendingTextureOpen = i;
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

        // ⚑⚑ STAGE Q, AND IT IS THE SAME "ONE PLACE NOTICES" IDIOM AS THE BLOCK
        // BELOW. A new edit ends the forward branch, and the entries it discards
        // can name a DIFFERENT editor from the one being edited - so the editor
        // that made the edit cannot drop those snapshots, because it does not
        // know they exist. The history raises the flag; this reads it once and
        // sweeps all three. Consume-once, so a second read in the same frame
        // cannot sweep twice.
        if (history.takeRedoDiscarded()) {
            editor.clearRedo();
            textureEditor.clearRedo();
            defEditor.clearRedo();
        }

        // ⚑ One place that notices a panel opened or closed, whichever of the
        // three routes did it. Without this the toggle is real for the session
        // and gone on the next launch, which is exactly what was reported.
        {
            const bool shown[6] = {showMesh, showReport, showTexture, showSound, showMaterial, showView};
            for (int i = 0; i < 6; ++i) {
                if (shown[i] != wasShown[i]) {
                    wasShown[i] = shown[i];
                    ImGui::MarkIniSettingsDirty();
                }
            }
        }

        // --- the markers, AFTER the panel ---
        //
        // ⚑⚑⚑ THE OVERLAY IS BUILT AGAINST THE CAMERA THIS FRAME WILL RENDER
        // WITH, AND THAT IS WHY IT LIVES DOWN HERE RATHER THAN WITH THE OTHER
        // DEBUG GEOMETRY. `viewport.view` far above is deliberately the camera
        // as it was when the press was read - picking must agree with the image
        // the cursor was over - but these lines are handed to the renderer,
        // which draws them with `camera.view()` as it stands at `frame.view`
        // below. Any camera move between the two is an error in the overlay, and
        // the camera moves in more places than the obvious one: the
        // orbit/pan/dolly block, the `F` key, the `Levels` radios in the Report
        // panel (which frame the range each level is for), and any panel action
        // that rebuilds with `reframe` - opening a mesh, `Reload`. The first two
        // are above; the rest are inside the panel, which is why submitting
        // AFTER the whole panel is the only placement that covers all of them
        // rather than the ones anybody happened to think of.
        //
        // ⚑⚑ IT MATTERS FOR EXACTLY ONE THING, AND THAT THING IS WHY IT HID FOR
        // FIVE STAGES. A point cross and an edge line are world-space positions
        // that come out right whichever matrix computed them. Stage N's part box
        // is the exception, and the exception is the trick that makes it work at
        // all: it RELOCATES its corners to 0.12 m from the eye to beat the depth
        // test, so it is only correct drawn from the eye it was scaled about,
        // and any mismatch is magnified by `distance / 0.12`. Measured at 41x on
        // `freighter_cockpit` - 0.3739 m of orbit became 15.5 m of slip on a
        // 7.4 m model, flung clean off the mesh rather than lagging it.
        //
        // ⚑ Last of the debug lines on purpose, which this ordering preserves -
        // the panel submits none. `DebugDrawRenderer::line()` drops silently
        // once its 8192 vertices are spent, so whatever is drawn last is what
        // goes missing, and the grid and the scale boxes are the frame's fixed
        // furniture while the markers are the variable part.
        // ⚑ Not while a level is previewed. The markers come from the DOCUMENT
        // and the mesh on screen is a generated level, so drawing them together
        // puts 162 crosses over a hull with 80 triangles that has no such
        // points - an editable-looking overlay on something that is not the
        // thing being edited. A preview is a look, not an edit surface.
        forge::PointTool::Viewport overlay = viewport;
        overlay.view = camera.view();
        overlay.cameraDistance = camera.distance();
        // ⚑⚑ STAGE O: TAKEN UNCONDITIONALLY, OUTSIDE THE GUARD BELOW. The read
        // is what clears the value, so a frame that skips the markers - points
        // hidden, no document, a level being previewed - must still take it, or
        // the next frame that DOES draw inherits whichever row the cursor was
        // over before the panel stopped reporting.
        // ⚑ Being after the panel, this is THIS frame's hover rather than the
        // previous frame's: the row was submitted a few lines ago. The lag the
        // consume-once rule was designed around is gone; the rule stays, because
        // what it actually guards is the producer not running at all.
        const std::size_t rowHoverPart = editor.takeHoveredPart();
        if (showPoints && editor.isOpen() && previewLevel == 0) {
            points.drawMarkers(view.debugDraw(), overlay, editor.selectedPart(), rowHoverPart);
        }

        // --- draw ---
        forge::FrameDesc frame;
        frame.view = camera.view();
        const float cosElevation = std::cos(sunElevation);
        frame.sunDirection = {
            cosElevation * std::sin(sunAzimuth), std::sin(sunElevation), cosElevation * std::cos(sunAzimuth)};
        frame.exposure = exposure;
        // Stage F: the preview draws a generated level in the authored mesh's
        // place. One item either way - the level is a mesh like any other, and
        // making the viewport learn about levels would be a second rule.
        renderer::GpuMesh* drawn = &openMesh;
        if (previewLevel > 0 && static_cast<std::size_t>(previewLevel) <= levelMeshes.size()) {
            drawn = &levelMeshes[static_cast<std::size_t>(previewLevel) - 1];
        }
        if (drawn->indexCount > 0) {
            // ⚑⚑⚑ PHASE 25 STAGE D: THE ITEM CARRIES ITS MATERIAL. `emissive`
            // and `alpha` come from the material too, and the View panel's
            // `emissive` slider now ADDS to it rather than replacing it - it is
            // a lighting control for looking at a mesh, not an edit of the
            // surface, and the two were indistinguishable while there was only
            // one of them.
            //
            // ⚑ THE FALLBACK IS THE STOCK ROW AT INDEX 0, which is what
            // `ForgeView::initialize` built and what this tool drew before
            // materials existed - reached when there is no def data, when no
            // `[[model]]` row names this mesh, or when the row names a material
            // nothing defines. All three are states the Material panel reports
            // in words; none of them is a reason to show a black viewport.
            const std::uint32_t openMaterial = defEditor.openMaterialIndex();
            const bool haveMaterial = openMaterial < defEditor.materials().size();
            const assets::MaterialDef* surface =
                haveMaterial ? &defEditor.materials()[openMaterial] : nullptr;
            frame.items.push_back({
                .mesh = drawn,
                .texture = &textures[static_cast<std::size_t>(textureIndex)],
                .model = core::Mat4::identity(),
                .emissive = emissive + (surface != nullptr ? surface->emissive : 0.0f),
                .material = haveMaterial ? openMaterial : 0,
                .alpha = surface != nullptr && surface->translucent ? surface->alpha : 1.0f,
            });
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
    // ⚑ Explicit rather than left to the destructor, and it is the same reason
    // every line below is: this joins a thread that is still reading the sound
    // bank, and a teardown order that is stated is a teardown order that can be
    // checked. It is first because it is the only one that is not the GPU's.
    soundPreview.shutdown();
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
