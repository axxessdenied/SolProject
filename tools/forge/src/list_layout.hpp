#pragma once

// How tall a scrolling list should be (engine plan Phase 9 stage M).
//
// Every list in this tool used to be a `BeginChild` with a hardcoded pixel
// height, all six written before stage K made the panels dockable and
// resizable. Dragging a panel taller added no rows at all: `freighter_cockpit`
// showed 9 of its 28 parts and `hull.tex` 5 of its 60 panel rows, in an 817 px
// window, whatever the author did with the mouse.
//
// ⚑⚑ THIS IS A PURE FUNCTION OF NUMBERS AND CALLS NOTHING, WHICH IS THE WHOLE
// REASON IT IS ITS OWN HEADER. `sol_forge_tests` compiles `mesh_library.cpp`
// alone and links no ImGui, no device and no window (see tools/forge/
// CMakeLists.txt), so a rule that took an `ImGuiWindow*` would be untestable in
// the only suite that could test it. The call sites read the style and pass it
// in. Phase 15's linkage rule, applied before the promise rather than after.
//
// ⚑⚑ AND THE FLAG THAT LOOKS LIKE THE ANSWER IS A TRAP, WHICH IS WHY NONE OF
// THIS IS `ImGuiChildFlags_ResizeY`. That flag gives a child a draggable bottom
// edge and enables .ini saving - with stage K's `forge.ini` already wired, one
// bitwise-or looks like the entire stage. But `imgui.cpp` (BeginChildEx) states
// that `Begin()` switches the size condition to `ImGuiCond_FirstUseEver` for
// any axis with `ResizeX/Y` set: the size argument becomes a FIRST-SIGHT
// default, so the list would be sized once by whichever document happened to be
// open first and then frozen into the ini permanently, tracking neither the
// content nor the panel. That is stage J's headline defect - "the climb was
// paid on every launch" - rebuilt in a form that also persists the mistake.

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace forge {

// The three style numbers a list's height depends on. Read at the call site
// (`GetTextLineHeightWithSpacing` for a Selectable list, `GetFrameHeightWithSpacing`
// for one built out of drag widgets) so this header stays ImGui-free.
struct ListMetrics
{
    float rowPitch = 0.0f;   // one row, including the spacing that follows it
    float rowSpacing = 0.0f; // ItemSpacing.y - the LAST row does not pay it
    float padding = 0.0f;    // WindowPadding.y - paid at the top and the bottom
};

// No list shrinks below this many rows. It earns its place on the SMALL case,
// which is the half nobody asks for: `cube.forge` and `gate_membrane.forge`
// have one part each and used to get a 170 px empty box, spending ~150 px of a
// scarce column on nothing. Without a floor they would collapse to a sliver and
// the panel would jitter as a document grew from one part to three.
inline constexpr std::size_t kMinListRows = 4;

// The exact child height at which `rows` rows have nothing left to scroll.
//
// ⚑ Verified against the tool rather than derived on paper: at the shipped
// 170 px the parts list reported `scrollMaxY 318` for 28 rows, and 170 + 318 =
// 488 = 28*17 - 4 + 2*8. The panel list reported 1252 for 60 rows at 140 px,
// and 140 + 1252 = 1392 = 60*23 - 4 + 2*8. Both exact.
[[nodiscard]] constexpr float listHeightForRows(const ListMetrics& metrics, std::size_t rows) noexcept
{
    const float frame = metrics.padding * 2.0f;
    if (rows == 0) {
        return frame;
    }
    return static_cast<float>(rows) * metrics.rowPitch - metrics.rowSpacing + frame;
}

// How many whole rows a child of `height` can show. The inverse of the above,
// and what a test asserts against so the two cannot drift apart.
[[nodiscard]] constexpr std::size_t listRowsForHeight(const ListMetrics& metrics, float height) noexcept
{
    if (metrics.rowPitch <= 0.0f) {
        return 0;
    }
    const float inner = height - metrics.padding * 2.0f + metrics.rowSpacing;
    if (inner <= 0.0f) {
        return 0;
    }
    return static_cast<std::size_t>(inner / metrics.rowPitch);
}

// The height to hand `BeginChild`: as tall as the content wants, never shorter
// than `minRows`, never taller than `share` of the panel it lives in.
//
// ⚑ THE CAP IS THE WINDOW, NOT `GetContentRegionAvail()`, AND THAT IS A REAL
// DECISION. Available space is order-dependent - it shrinks as each section
// above consumes it and goes negative once a window overflows - so a list's
// height would depend on whether some unrelated header above it happened to be
// collapsed, and a box that resizes when you fold something else is worse than
// one that never moves. A window's height is stable within the frame and is
// what "the room you were given" actually means.
//
// ⚑ `share` is per call site rather than one constant, because the windows
// differ: `Mesh` carries two lists plus the points panel, `Texture` carries up
// to three plus a 256 px preview, and `Report` and `View` carry none. One
// global share would either starve `Texture` or let `Mesh`'s first list eat the
// window.
// ⚑⚑ THE CONTENT IS A HEIGHT RATHER THAN A ROW COUNT BECAUSE A LIST IS NOT
// NECESSARILY MADE OF ONE KIND OF ROW, AND ASSUMING IT WAS COST A PREDICTION.
// The mesh list interleaves `CollapsingHeader` group headers with `Selectable`
// entries, and a header is a FRAMED item: 23 px against a text row's 17. Sized
// as ten uniform rows it came out 12 px short and still scrolled - measured, not
// reasoned about. `rowsHeight` is the sum of each submitted row's own pitch.
[[nodiscard]] constexpr float listHeightForContent(const ListMetrics& metrics,
                                                   float rowsHeight,
                                                   std::size_t minRows,
                                                   float share,
                                                   float windowHeight) noexcept
{
    const float frame = metrics.padding * 2.0f;
    const float floorHeight = listHeightForRows(metrics, minRows);
    const float wanted = rowsHeight <= 0.0f ? frame : rowsHeight - metrics.rowSpacing + frame;
    // The floor wins a short panel outright: clamping to a cap below the floor
    // would invert the range and hand back a negative height.
    const float cap = std::max(floorHeight, share * windowHeight);
    return std::clamp(wanted, floorHeight, cap);
}

// The common case: every row is the same kind.
[[nodiscard]] constexpr float listHeight(const ListMetrics& metrics,
                                         std::size_t rows,
                                         std::size_t minRows,
                                         float share,
                                         float windowHeight) noexcept
{
    return listHeightForContent(
        metrics, static_cast<float>(rows) * metrics.rowPitch, minRows, share, windowHeight);
}

// A list long enough to need finding rather than reading. Below this a filter
// box is just a row of wasted height in a space-starved tool, which is the very
// thing this stage exists to stop: `cube.forge` has one part and `gate.forge`
// ten, while `freighter_cockpit.forge` has 28 and a 40-object Blender scene has
// 40. ⚑ The threshold is on the DOCUMENT rather than on whether the list
// currently scrolls, deliberately - tying it to the height would make the box
// appear and vanish as the author dragged the panel, and a control that moves
// under the hand is the "save moves" trap this programme has recorded three
// times.
inline constexpr std::size_t kFilterFromRows = 12;

// Case-insensitive substring match, for filtering a list by a typed fragment.
// ⚑ ASCII-only `tolower` by hand rather than <cctype>: `std::tolower` on a
// char with the high bit set is undefined behaviour, and part ids come from
// Blender object names, which are UTF-8 and routinely carry accented letters.
[[nodiscard]] constexpr bool listMatchesFilter(std::string_view label, std::string_view needle) noexcept
{
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > label.size()) {
        return false;
    }
    const auto fold = [](char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    for (std::size_t start = 0; start + needle.size() <= label.size(); ++start) {
        std::size_t i = 0;
        while (i < needle.size() && fold(label[start + i]) == fold(needle[i])) {
            ++i;
        }
        if (i == needle.size()) {
            return true;
        }
    }
    return false;
}

} // namespace forge
