#pragma once

// The `undo` / `redo` pair, drawn the same way in all three panels.
//
// ⚑⚑ IT IS ONE FUNCTION BECAUSE THERE IS ONE UNDO. Before stage Q the tool had
// three buttons doing three different things - `undo drag` in the Mesh panel,
// `undo` in the Texture panel and `undo def` in the Report panel, each reaching
// only its own editor - and a `Ctrl+Z` that did a fourth thing, always the
// mesh's, whatever the author was looking at. Writing the pair once is what
// stops that growing back: a rule applied in three places is a defect in the
// two nobody looked at.
//
// ⚑ SEPARATE FROM `edit_history.hpp` ON PURPOSE, so that header stays free of
// ImGui and the history itself can be tested headless in `forge.unit`. The
// drawing is the part with no test; the ordering is the part with all of them.

#include "edit_history.hpp"

#include <imgui.h>

namespace forge {

inline void drawHistoryButtons(EditHistory& history)
{
    // ⚑ The label says what would be taken back, which is the whole benefit of
    // the global model being legible: "undo: move part" is the tool telling the
    // author which of their gestures the button is aimed at, in a design where
    // that is no longer obvious from which panel the button is in.
    ImGui::BeginDisabled(!history.canUndo());
    if (ImGui::Button("undo")) {
        history.requestUndo();
    }
    if (history.canUndo() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("undo: %s", history.undoLabel().c_str());
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!history.canRedo());
    if (ImGui::Button("redo")) {
        history.requestRedo();
    }
    if (history.canRedo() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("redo: %s", history.redoLabel().c_str());
    }
    ImGui::EndDisabled();
}

} // namespace forge
