#include "texture_editor.hpp"

#include "list_layout_style.hpp"
#include "mesh_library.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <utility>
#include <vector>

namespace forge {

using namespace sol;
using assets::TextureColor;
using assets::TextureDoc;
using assets::TextureLayer;
using assets::TextureOp;
using assets::TextureParamKind;
using assets::TextureParamSpec;
using assets::TexturePanel;
using assets::TextureRect;
using assets::TextureValue;

namespace {

[[nodiscard]] std::string fileStem(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t start = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = path.find_last_of('.');
    return path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
}

[[nodiscard]] std::string directoryOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// A colour swatch that edits three 0-255 channels.
//
// ⚑ ImGui stores colour as float, so the value is converted on the way in and
// rounded on the way back - and only when the widget reports an edit, so an
// untouched channel is never rewritten. v/255 -> float -> round(*255) is exact
// for every 8-bit value, which is what makes that safe.
[[nodiscard]] bool colorEdit(const char* label, TextureColor& color)
{
    float shown[3] = {static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                      static_cast<float>(color.b) / 255.0f};
    if (!ImGui::ColorEdit3(label, shown, ImGuiColorEditFlags_Uint8)) {
        return false;
    }
    color.r = static_cast<int>(std::lround(shown[0] * 255.0f));
    color.g = static_cast<int>(std::lround(shown[1] * 255.0f));
    color.b = static_cast<int>(std::lround(shown[2] * 255.0f));
    return true;
}

// A tint is an OFFSET and may be negative, which no colour picker can express -
// so it is three numbers, and saying so is better than clamping it at zero and
// making "cool these panels down" quietly impossible.
[[nodiscard]] bool offsetEdit(const char* label, TextureColor& color)
{
    int shown[3] = {color.r, color.g, color.b};
    if (!ImGui::DragInt3(label, shown, 0.25f, -255, 255)) {
        return false;
    }
    color = {shown[0], shown[1], shown[2]};
    return true;
}

} // namespace

void TextureEditor::openNew(const std::string& directory)
{
    m_doc = TextureDoc{};
    // The header a new file starts life with. It lives here rather than in
    // writeTexture for the reason writeForge documents: the writer is faithful
    // and invents nothing, so a file that opens without a header must save
    // without one.
    m_doc.header = "# Authored with the Forge (engine plan Phase 9 stage G). This file is\n"
                   "# the SOURCE: the texture beside it is built from these ops, and the\n"
                   "# cooker evaluates them - no image editor is in the loop.\n";
    m_doc.width = 256;
    m_doc.height = 256;

    TextureLayer layer;
    layer.op = TextureOp::Fill;
    TextureValue color;
    color.color = {96, 100, 108};
    layer.set("color", color);
    m_doc.layers.push_back(std::move(layer));

    m_path = directory.empty() ? std::string("untitled.tex") : directory + "/untitled.tex";
    m_saveName = "untitled.tex";
    m_selected = 0;
    m_open = true;
    m_dirty = true;
    m_buildError.clear();
    m_undo.clear();
    resetPick();
}

bool TextureEditor::openFile(const std::string& path, std::string& status)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path.c_str(), bytes)) {
        status = "cannot read " + path;
        return false;
    }
    TextureDoc doc;
    std::string error;
    if (!assets::parseTexture(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                              path.c_str(), doc, &error)) {
        status = error;
        SOL_LOG_ERROR("forge: %s", error.c_str());
        return false;
    }
    m_doc = std::move(doc);
    m_path = path;
    m_saveName = fileStem(path) + ".tex";
    m_selected = m_doc.layers.empty() ? -1 : 0;
    m_open = true;
    m_dirty = false;
    m_buildError.clear();
    m_undo.clear();
    resetPick();
    status = "opened " + m_saveName + " (" + std::to_string(m_doc.layers.size()) + " ops, " +
             std::to_string(m_doc.width) + "x" + std::to_string(m_doc.height) + ")";
    return true;
}

bool TextureEditor::save(std::string& status)
{
    if (!m_open) {
        return false;
    }
    const std::string directory = directoryOf(m_path);
    const std::string target = directory.empty() ? m_saveName : directory + "/" + m_saveName;
    const std::string text = assets::writeTexture(m_doc);
    if (!platform::writeFileBytes(target.c_str(), text.data(), text.size())) {
        status = "cannot write " + target;
        SOL_LOG_ERROR("forge: %s", status.c_str());
        return false;
    }
    m_path = target;
    m_dirty = false;
    status = "saved " + target;
    SOL_LOG_INFO("forge: %s (%zu bytes)", status.c_str(), text.size());
    return true;
}

void TextureEditor::close()
{
    m_doc = TextureDoc{};
    m_undo.clear();
    m_path.clear();
    m_saveName.clear();
    m_buildError.clear();
    m_selected = -1;
    m_open = false;
    m_dirty = false;
    resetPick();
}

// Everything the preview gesture holds about a document, cleared together -
// a stale hit map or a half-finished drag surviving into a NEW document is the
// shape of bug that writes to the wrong file.
void TextureEditor::resetPick()
{
    m_hitMap.clear();
    m_hitMapDirty = true;
    m_dragHit = {};
    m_dragging = false;
    m_dragMoved = false;
    m_selectedRow = -1;
    m_scrollToSelectedRow = false;
}

void TextureEditor::beginEdit()
{
    if (!m_open) {
        return;
    }
    m_undo.push_back(m_doc);
    if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
    }
}

void TextureEditor::noteActivation()
{
    if (ImGui::IsItemActivated()) {
        beginEdit();
    }
}

bool TextureEditor::undo()
{
    if (m_undo.empty()) {
        return false;
    }
    m_doc = std::move(m_undo.back());
    m_undo.pop_back();
    m_selected = std::min(m_selected, static_cast<int>(m_doc.layers.size()) - 1);
    m_dirty = true;
    m_hitMapDirty = true;
    m_buildError.clear();
    return true;
}

bool TextureEditor::beginPickedRow(std::size_t ordinal)
{
    if (m_selectedRow < 0 || static_cast<std::size_t>(m_selectedRow) != ordinal) {
        return false;
    }
    // ⚑ Scrolled to ONCE, on the frame the pick happened. Doing it every frame
    // would fight an author trying to scroll the list by hand, which is the
    // same mistake as a map that snaps back while you are panning it.
    if (m_scrollToSelectedRow) {
        ImGui::SetScrollHereY(0.5f);
        m_scrollToSelectedRow = false;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(96, 72, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(124, 94, 32, 255));
    return true;
}

void TextureEditor::endPickedRow(bool picked)
{
    if (picked) {
        ImGui::PopStyleColor(2);
    }
}

void TextureEditor::refreshHitMap()
{
    if (!m_hitMapDirty) {
        return;
    }
    if (!assets::textureAttribution(m_doc, m_hitMap)) {
        m_hitMap.clear();
    }
    m_hitMapDirty = false;
}

bool TextureEditor::drawPreview(void* image, float availableWidth)
{
    if (!m_open || m_doc.width <= 0 || m_doc.height <= 0 || image == nullptr) {
        return false;
    }

    const int scale = texturePreviewScale(m_doc.width, availableWidth);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{static_cast<float>(m_doc.width * scale),
                      static_cast<float>(m_doc.height * scale)};

    // ⚑ The button is submitted FIRST and the image drawn into the window's own
    // list behind it. Submitting an Image and then an InvisibleButton over it
    // would work too, but this way the last-submitted item - which is what
    // IsItemActivated names - is unambiguously the surface being pressed.
    ImGui::InvisibleButton("##preview", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(image), origin,
                                         {origin.x + size.x, origin.y + size.y});

    bool changed = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if (ImGui::IsItemActivated()) {
        m_dragging = false;
        m_dragMoved = false;
        int px = 0;
        int py = 0;
        if (texturePixelAt({mouse.x, mouse.y}, {origin.x, origin.y}, scale, m_doc.width,
                           m_doc.height, px, py)) {
            refreshHitMap();
            const std::size_t index =
                static_cast<std::size_t>(py) * m_doc.width + static_cast<std::size_t>(px);
            const assets::TextureHit hit =
                index < m_hitMap.size() ? m_hitMap[index] : assets::TextureHit{};
            if (hit.valid()) {
                // Selection follows the pick ACROSS ops, because two lists that
                // disagreed about what is selected would be two answers to
                // "what am I editing".
                m_selected = hit.layer;
                m_selectedRow = hit.row;
                m_scrollToSelectedRow = hit.row >= 0;
            }
            if (hit.movable() && assets::textureRowPosition(m_doc, hit, m_dragStartPosition[0],
                                                            m_dragStartPosition[1])) {
                m_dragHit = hit;
                m_dragging = true;
                m_dragStartCursor[0] = mouse.x;
                m_dragStartCursor[1] = mouse.y;
            }
        }
    }

    if (m_dragging && active) {
        const int dx = textureDragOffset(m_dragStartCursor[0], mouse.x, scale);
        const int dy = textureDragOffset(m_dragStartCursor[1], mouse.y, scale);
        // ⚑ Nothing is written until the row has actually moved once - so a
        // click that only selects leaves the file byte-identical and leaves no
        // undo entry. After that first movement every frame is applied, INCLUDING
        // one that comes back to zero, or a drag wandering back to where it
        // started would leave the row stranded at its furthest point.
        if (dx != 0 || dy != 0 || m_dragMoved) {
            if (!m_dragMoved) {
                beginEdit();
                m_dragMoved = true;
            }
            if (assets::textureSetRowPosition(m_doc, m_dragHit, m_dragStartPosition[0] + dx,
                                              m_dragStartPosition[1] + dy)) {
                m_dirty = true;
                m_hitMapDirty = true;
                changed = true;
            }
        }
    }
    if (!active) {
        m_dragging = false;
    }

    // The selected row, outlined in the picture. Without it the only feedback
    // for a pick is a highlighted line in a list below the fold, and picking one
    // of sixty overlapping panels is exactly where that is not enough.
    assets::TextureRect bounds;
    const assets::TextureHit selection{m_selected, m_selectedRow};
    if (assets::textureRowBounds(m_doc, selection, bounds)) {
        const ImVec2 min{origin.x + static_cast<float>(bounds.x * scale),
                         origin.y + static_cast<float>(bounds.y * scale)};
        const ImVec2 max{min.x + static_cast<float>(bounds.w * scale),
                         min.y + static_cast<float>(bounds.h * scale)};
        ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 190, 60, 255));
    }

    if (hovered && !m_dragging) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return changed;
}

bool TextureEditor::drawOpList()
{
    bool changed = false;
    // Stage M: the op list is short by nature - four layers is a whole texture -
    // so it sizes to its content and the 40 px it used to waste goes to the
    // param list below, which is the one that never has enough.
    const float outerHeight = ImGui::GetWindowHeight();
    const float opsHeight =
        listHeight(textRowMetrics(), m_doc.layers.size(), kMinListRows, 0.25f, outerHeight);
    if (ImGui::BeginChild("##ops", {0.0f, opsHeight}, ImGuiChildFlags_Borders)) {
        for (std::size_t i = 0; i < m_doc.layers.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            char label[96];
            std::snprintf(label, sizeof(label), "%zu  %s", i,
                          assets::textureOpName(m_doc.layers[i].op));
            if (ImGui::Selectable(label, static_cast<int>(i) == m_selected)) {
                m_selected = static_cast<int>(i);
                // Picking an op from the list is not picking a row inside it,
                // and leaving the old ordinal would highlight an unrelated line.
                m_selectedRow = -1;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (ImGui::BeginCombo("##add", "add op")) {
        for (const TextureOp op : assets::textureOps()) {
            if (ImGui::Selectable(assets::textureOpName(op))) {
                beginEdit();
                TextureLayer layer;
                layer.op = op;
                m_doc.layers.push_back(std::move(layer));
                m_selected = static_cast<int>(m_doc.layers.size()) - 1;
                m_dirty = true;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    const bool hasSelection = m_selected >= 0 && m_selected < static_cast<int>(m_doc.layers.size());
    const auto selected = static_cast<std::size_t>(m_selected);
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("delete") && hasSelection) {
        beginEdit();
        m_doc.layers.erase(m_doc.layers.begin() + static_cast<std::ptrdiff_t>(selected));
        m_selected = m_doc.layers.empty() ? -1 : std::min(m_selected, static_cast<int>(m_doc.layers.size()) - 1);
        m_dirty = true;
        changed = true;
    }
    ImGui::SameLine();
    // ⚑ Order IS the picture here - a fill moved below the panels erases them -
    // so moving an op up and down is an edit rather than a view preference.
    if (ImGui::Button("up") && hasSelection && selected > 0) {
        beginEdit();
        std::swap(m_doc.layers[selected], m_doc.layers[selected - 1]);
        m_selected = static_cast<int>(selected) - 1;
        m_dirty = true;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("down") && hasSelection && selected + 1 < m_doc.layers.size()) {
        beginEdit();
        std::swap(m_doc.layers[selected], m_doc.layers[selected + 1]);
        m_selected = static_cast<int>(selected) + 1;
        m_dirty = true;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool TextureEditor::drawParams(TextureLayer& layer)
{
    bool changed = false;
    // ⚑ The row ordinal the preview reports runs across a whole OP, and `lines`
    // is the op with two lists in it - so a running base is what turns "row 2"
    // into "the first horizontal seam". It matches the order `drawLayerInto`
    // paints in, which is where the ordinal is defined.
    std::size_t rowBase = 0;
    for (const TextureParamSpec& spec : assets::textureParams(layer.op)) {
        TextureValue value = layer.value(spec.name);
        bool edited = false;
        ImGui::PushID(spec.name);
        switch (spec.kind) {
        case TextureParamKind::Integer: {
            int shown = static_cast<int>(value.integer);
            if (ImGui::DragInt(spec.name, &shown, 0.25f, 1, 4096)) {
                value.integer = shown;
                edited = true;
            }
            noteActivation();
            break;
        }
        case TextureParamKind::Color:
            edited = colorEdit(spec.name, value.color);
            noteActivation();
            break;
        case TextureParamKind::ColorOffset:
            edited = offsetEdit(spec.name, value.color);
            noteActivation();
            break;
        case TextureParamKind::RectList: {
            ImGui::Text("%s  (%zu)", spec.name, value.rects.size());
            // Stage M. ⚑ frameRowMetrics, not textRowMetrics: these rows are
            // DragInt4 widgets at 23 px, not 17 px text lines, which is why the
            // shipped 140 px showed 5 rows where a row count suggested 8.
            const float rectsOuter = ImGui::GetWindowHeight();
            const float rectsHeight =
                listHeight(frameRowMetrics(), value.rects.size(), kMinListRows, 0.45f, rectsOuter);
            if (ImGui::BeginChild("##rects", {0.0f, rectsHeight}, ImGuiChildFlags_Borders)) {
                for (std::size_t i = 0; i < value.rects.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const bool picked = beginPickedRow(rowBase + i);
                    int row[4] = {value.rects[i].x, value.rects[i].y, value.rects[i].w,
                                  value.rects[i].h};
                    ImGui::SetNextItemWidth(-40.0f);
                    if (ImGui::DragInt4("##r", row, 0.5f, -4096, 4096)) {
                        value.rects[i] = {row[0], row[1], std::max(0, row[2]),
                                          std::max(0, row[3])};
                        edited = true;
                    }
                    noteActivation();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) {
                        beginEdit();
                        value.rects.erase(value.rects.begin() + static_cast<std::ptrdiff_t>(i));
                        edited = true;
                        endPickedRow(picked);
                        ImGui::PopID();
                        break;
                    }
                    endPickedRow(picked);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            rowBase += value.rects.size();
            if (ImGui::SmallButton("add rect")) {
                beginEdit();
                value.rects.push_back(value.rects.empty() ? TextureRect{0, 0, 16, 16}
                                                          : value.rects.back());
                edited = true;
            }
            break;
        }
        case TextureParamKind::PanelList: {
            ImGui::Text("%s  (%zu)", spec.name, value.panels.size());
            // Stage M, and this is the worst case in the tool: `hull.tex` has 60
            // of these and the shipped height showed 5 of them, 8%.
            const float panelsOuter = ImGui::GetWindowHeight();
            const float panelsHeight =
                listHeight(frameRowMetrics(), value.panels.size(), kMinListRows, 0.45f, panelsOuter);
            if (ImGui::BeginChild("##panels", {0.0f, panelsHeight}, ImGuiChildFlags_Borders)) {
                for (std::size_t i = 0; i < value.panels.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const bool picked = beginPickedRow(rowBase + i);
                    int row[5] = {value.panels[i].x, value.panels[i].y, value.panels[i].w,
                                  value.panels[i].h, value.panels[i].shade};
                    ImGui::SetNextItemWidth(-40.0f);
                    if (ImGui::DragScalarN("##p", ImGuiDataType_S32, row, 5, 0.5f)) {
                        value.panels[i] = {row[0], row[1], std::max(0, row[2]),
                                           std::max(0, row[3]), std::clamp(row[4], 0, 255)};
                        edited = true;
                    }
                    noteActivation();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) {
                        beginEdit();
                        value.panels.erase(value.panels.begin() + static_cast<std::ptrdiff_t>(i));
                        edited = true;
                        endPickedRow(picked);
                        ImGui::PopID();
                        break;
                    }
                    endPickedRow(picked);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            rowBase += value.panels.size();
            if (ImGui::SmallButton("add panel")) {
                beginEdit();
                value.panels.push_back(value.panels.empty() ? TexturePanel{0, 0, 32, 24, 100}
                                                            : value.panels.back());
                edited = true;
            }
            break;
        }
        case TextureParamKind::IntegerList: {
            ImGui::Text("%s  (%zu)", spec.name, value.integers.size());
            for (std::size_t i = 0; i < value.integers.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const bool picked = beginPickedRow(rowBase + i);
                int shown = static_cast<int>(value.integers[i]);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::DragInt("##v", &shown, 0.5f, -4096, 4096)) {
                    value.integers[i] = shown;
                    edited = true;
                }
                noteActivation();
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    beginEdit();
                    value.integers.erase(value.integers.begin() +
                                         static_cast<std::ptrdiff_t>(i));
                    edited = true;
                    endPickedRow(picked);
                    ImGui::PopID();
                    break;
                }
                endPickedRow(picked);
                ImGui::PopID();
                if ((i % 3) != 2 && i + 1 < value.integers.size()) {
                    ImGui::SameLine();
                }
            }
            rowBase += value.integers.size();
            if (ImGui::SmallButton("add line")) {
                beginEdit();
                value.integers.push_back(value.integers.empty() ? 0 : value.integers.back());
                edited = true;
            }
            break;
        }
        }
        ImGui::PopID();
        // Written back only on an actual edit, so redrawing the panel never
        // dirties a document nobody changed.
        //
        // ⚑ NO beginEdit() HERE. A drag reports an edit on every frame it
        // moves, and pushing undo from this path is what made one `undo` step
        // back a single frame of a gesture (146 -> 143 on a drag from 72, live).
        // The gesture opens once, in noteActivation(), when the widget becomes
        // active. The buttons below DO push their own, because a press is one
        // gesture and one entry.
        if (edited) {
            layer.set(spec.name, value);
            m_dirty = true;
            changed = true;
        }
    }
    return changed;
}

bool TextureEditor::drawSelectedOp()
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_doc.layers.size())) {
        ImGui::TextDisabled("no op selected");
        return false;
    }
    TextureLayer& layer = m_doc.layers[static_cast<std::size_t>(m_selected)];

    bool changed = false;
    if (ImGui::BeginCombo("kind", assets::textureOpName(layer.op))) {
        for (const TextureOp op : assets::textureOps()) {
            if (ImGui::Selectable(assets::textureOpName(op), op == layer.op) && op != layer.op) {
                beginEdit();
                // Parameters are kept across a kind change, exactly as the part
                // editor keeps them across a primitive change: the schema
                // decides which are read, so a fill turned into a checker and
                // back still has the colour it started with.
                layer.op = op;
                m_dirty = true;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    changed = drawParams(layer) || changed;
    return changed;
}

bool TextureEditor::draw()
{
    if (!m_open) {
        ImGui::TextDisabled("no texture open");
        return false;
    }

    bool changed = false;
    ImGui::Text("%s%s  %dx%d", m_saveName.c_str(), m_dirty ? " *" : "", m_doc.width, m_doc.height);

    char nameBuffer[128];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_saveName.c_str());
    if (ImGui::InputText("file", nameBuffer, sizeof(nameBuffer))) {
        m_saveName = nameBuffer;
    }

    if (ImGui::Button("save")) {
        std::string status;
        if (!save(status)) {
            m_buildError = status;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("undo") && undo()) {
        changed = true;
    }
    ImGui::EndDisabled();

    if (m_doc.hasUnplaceableComments) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.95f, 0.75f, 0.35f, 1.0f},
                           "this file carries a comment after a value or inside an array, which "
                           "this format cannot place - saving will drop it");
        ImGui::PopTextWrapPos();
    }
    if (!m_buildError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.95f, 0.45f, 0.35f, 1.0f}, "%s", m_buildError.c_str());
        ImGui::PopTextWrapPos();
    }

    changed = drawOpList() || changed;
    ImGui::Separator();
    changed = drawSelectedOp() || changed;
    // ⚑ One place, so no edit path can forget it: any change to the document
    // invalidates which row is on top at a pixel.
    if (changed) {
        m_hitMapDirty = true;
    }
    return changed;
}

} // namespace forge
