#pragma once

// The ImGui side of stage M's list sizing: reads the live style into the plain
// numbers `list_layout.hpp` works in.
//
// ⚑ It is a SECOND header on purpose. `list_layout.hpp` must stay free of ImGui
// so `sol_forge_tests` can link the rule (that target compiles `mesh_library.cpp`
// alone and pulls in no ImGui at all), and the three drawing translation units
// must not each grow their own copy of this conversion - a rule applied in two
// places is a defect in the one nobody looked at, which this programme has now
// paid for four times.

#include "list_layout.hpp"

#include <imgui.h>

namespace forge {

// A list of `Selectable` rows: one text line plus the spacing after it.
[[nodiscard]] inline ListMetrics textRowMetrics()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return {ImGui::GetTextLineHeightWithSpacing(), style.ItemSpacing.y, style.WindowPadding.y};
}

// A list whose rows are framed widgets - a `DragInt4` of a rect, a
// `DragScalarN` of a panel. Taller than a text row by twice the frame padding,
// which is why the panel list showed 5 rows where a row count suggested 8.
[[nodiscard]] inline ListMetrics frameRowMetrics()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return {ImGui::GetFrameHeightWithSpacing(), style.ItemSpacing.y, style.WindowPadding.y};
}

} // namespace forge
