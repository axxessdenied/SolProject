#include "texture_editor.hpp"

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

bool TextureEditor::undo()
{
    if (m_undo.empty()) {
        return false;
    }
    m_doc = std::move(m_undo.back());
    m_undo.pop_back();
    m_selected = std::min(m_selected, static_cast<int>(m_doc.layers.size()) - 1);
    m_dirty = true;
    m_buildError.clear();
    return true;
}

bool TextureEditor::drawOpList()
{
    bool changed = false;
    if (ImGui::BeginChild("##ops", {0.0f, 120.0f}, ImGuiChildFlags_Borders)) {
        for (std::size_t i = 0; i < m_doc.layers.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            char label[96];
            std::snprintf(label, sizeof(label), "%zu  %s", i,
                          assets::textureOpName(m_doc.layers[i].op));
            if (ImGui::Selectable(label, static_cast<int>(i) == m_selected)) {
                m_selected = static_cast<int>(i);
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
            break;
        }
        case TextureParamKind::Color:
            edited = colorEdit(spec.name, value.color);
            break;
        case TextureParamKind::ColorOffset:
            edited = offsetEdit(spec.name, value.color);
            break;
        case TextureParamKind::RectList: {
            ImGui::Text("%s  (%zu)", spec.name, value.rects.size());
            if (ImGui::BeginChild("##rects", {0.0f, 140.0f}, ImGuiChildFlags_Borders)) {
                for (std::size_t i = 0; i < value.rects.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    int row[4] = {value.rects[i].x, value.rects[i].y, value.rects[i].w,
                                  value.rects[i].h};
                    ImGui::SetNextItemWidth(-40.0f);
                    if (ImGui::DragInt4("##r", row, 0.5f, -4096, 4096)) {
                        value.rects[i] = {row[0], row[1], std::max(0, row[2]),
                                          std::max(0, row[3])};
                        edited = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) {
                        value.rects.erase(value.rects.begin() + static_cast<std::ptrdiff_t>(i));
                        edited = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if (ImGui::SmallButton("add rect")) {
                value.rects.push_back(value.rects.empty() ? TextureRect{0, 0, 16, 16}
                                                          : value.rects.back());
                edited = true;
            }
            break;
        }
        case TextureParamKind::PanelList: {
            ImGui::Text("%s  (%zu)", spec.name, value.panels.size());
            if (ImGui::BeginChild("##panels", {0.0f, 140.0f}, ImGuiChildFlags_Borders)) {
                for (std::size_t i = 0; i < value.panels.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    int row[5] = {value.panels[i].x, value.panels[i].y, value.panels[i].w,
                                  value.panels[i].h, value.panels[i].shade};
                    ImGui::SetNextItemWidth(-40.0f);
                    if (ImGui::DragScalarN("##p", ImGuiDataType_S32, row, 5, 0.5f)) {
                        value.panels[i] = {row[0], row[1], std::max(0, row[2]),
                                           std::max(0, row[3]), std::clamp(row[4], 0, 255)};
                        edited = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) {
                        value.panels.erase(value.panels.begin() + static_cast<std::ptrdiff_t>(i));
                        edited = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if (ImGui::SmallButton("add panel")) {
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
                int shown = static_cast<int>(value.integers[i]);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::DragInt("##v", &shown, 0.5f, -4096, 4096)) {
                    value.integers[i] = shown;
                    edited = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    value.integers.erase(value.integers.begin() +
                                         static_cast<std::ptrdiff_t>(i));
                    edited = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
                if ((i % 3) != 2 && i + 1 < value.integers.size()) {
                    ImGui::SameLine();
                }
            }
            if (ImGui::SmallButton("add line")) {
                value.integers.push_back(value.integers.empty() ? 0 : value.integers.back());
                edited = true;
            }
            break;
        }
        }
        ImGui::PopID();
        // Written back only on an actual edit, so redrawing the panel never
        // dirties a document nobody changed.
        if (edited) {
            beginEdit();
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
    return changed;
}

} // namespace forge
