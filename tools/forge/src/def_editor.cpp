#include "def_editor.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace forge {

using namespace sol;
using assets::DefDoc;
using assets::DefRow;

namespace {

constexpr const char* kModelsFile = "models.toml";

// ⚑ THE FILE GETS EXACTLY WHAT THE PANEL SHOWED, which is Phase 14's rule in a
// second format. A measured radius written at full float precision lands in
// `models.toml` as `1.1583778` among neighbours carrying one decimal - and it
// makes the button labelled "use measured 1.1584 m" write something that is not
// 1.1584, which is a small lie in the UI on top of an unreadable line. These are
// the precisions the widgets display: metres at 0.1 mm, exactly Phase 14's grid.
constexpr int kMetreDecimals = 4;
constexpr int kUnitDecimals = 3;

[[nodiscard]] std::string readWholeFile(const std::string& path, bool& ok)
{
    std::vector<std::uint8_t> bytes;
    ok = platform::readFileBytes(path.c_str(), bytes);
    return ok ? std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())
              : std::string{};
}

// The schema default for a key the row does not carry. ⚑ Shown rather than
// written: a key appears in the file the moment an author edits it, and not
// before, which is `writeForge`'s rule ("a parameter the source never mentioned
// stays unmentioned") arriving one format over.
[[nodiscard]] float floatOr(const DefRow& row, const char* key, float fallback)
{
    const assets::DefKey* found = row.find(key);
    if (found == nullptr) {
        return fallback;
    }
    return static_cast<float>(std::strtod(std::string(found->value()).c_str(), nullptr));
}

[[nodiscard]] bool boolOr(const DefRow& row, const char* key, bool fallback)
{
    const assets::DefKey* found = row.find(key);
    return found == nullptr ? fallback : found->value() == "true";
}

} // namespace

void DefEditor::load(const std::string& dataDirectory)
{
    m_dataDirectory = dataDirectory;
    m_loaded = false;
    m_modelsDirty = false;
    m_error.clear();
    m_undo.clear();
    if (dataDirectory.empty()) {
        m_error = "no data directory";
        return;
    }
    bool ok = false;
    const std::string path = dataDirectory + "/" + kModelsFile;
    const std::string text = readWholeFile(path, ok);
    if (!ok) {
        m_error = std::string("cannot read ") + kModelsFile;
        return;
    }
    if (!assets::parseDefs(text.c_str(), text.size(), kModelsFile, m_models, &m_error)) {
        return;
    }
    m_loaded = revalidate();
}

bool DefEditor::revalidate()
{
    const std::string text = assets::writeDefs(m_models);
    assets::DefDatabase candidate;
    std::string error;
    if (!candidate.mergeToml(text.c_str(), text.size(), kModelsFile, &error)) {
        m_error = error;
        return false;
    }
    m_defs = std::move(candidate);
    m_error.clear();
    return true;
}

void DefEditor::beginEdit()
{
    if (!m_loaded) {
        return;
    }
    m_undo.push_back(m_models);
    if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
    }
}

void DefEditor::noteActivation()
{
    if (ImGui::IsItemActivated()) {
        beginEdit();
    }
}

bool DefEditor::undo()
{
    if (m_undo.empty()) {
        return false;
    }
    m_models = std::move(m_undo.back());
    m_undo.pop_back();
    m_modelsDirty = true;
    (void)revalidate();
    return true;
}

bool DefEditor::save(std::string& status)
{
    if (!m_loaded || !m_modelsDirty) {
        status = m_modelsDirty ? "nothing loaded" : "no def changes to save";
        return false;
    }
    // ⚑ Validated again at the moment of writing rather than trusting the last
    // edit's result. The panel refuses to leave an invalid document behind, but
    // "refuses to leave" and "is valid now" are two different claims and only
    // one of them is worth putting a file on disk for.
    if (!revalidate()) {
        status = std::string("refused: ") + m_error;
        return false;
    }
    const std::string text = assets::writeDefs(m_models);
    const std::string path = m_dataDirectory + "/" + kModelsFile;
    if (!platform::writeFileBytes(path.c_str(), text.data(), text.size())) {
        status = "cannot write " + path;
        return false;
    }
    m_modelsDirty = false;
    status = "saved " + path;
    SOL_LOG_INFO("forge: wrote %s (%zu bytes)", path.c_str(), text.size());
    return true;
}

bool DefEditor::drawModelRows(const AssetEntry& entry, const MeshReport& report,
                              const std::vector<std::string>& textureStems)
{
    if (!m_loaded) {
        ImGui::TextDisabled("def rows unavailable: %s",
                            m_error.empty() ? "no data directory" : m_error.c_str());
        return false;
    }

    bool changed = false;
    std::size_t drawn = 0;
    for (DefRow& row : m_models.rows) {
        if (row.type != "model") {
            continue;
        }
        const assets::DefKey* mesh = row.find("mesh");
        if (mesh == nullptr || mesh->unquoted() != entry.stem) {
            continue;
        }
        ++drawn;
        const std::string id(row.id());
        ImGui::PushID(id.c_str());
        ImGui::Text("[[model]] %s", id.c_str());

        // ⚑ The texture is a COMBO over what the tool can actually open, not a
        // text field. A `[[model]]` naming a texture that does not exist passes
        // the schema and fails at load, so the safest edit is the one that
        // cannot express the mistake.
        const std::string texture(row.find("texture") != nullptr
                                      ? row.find("texture")->unquoted()
                                      : std::string_view{});
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("texture", texture.c_str())) {
            for (const std::string& stem : textureStems) {
                const bool selected = stem == texture;
                if (ImGui::Selectable(stem.c_str(), selected) && !selected) {
                    beginEdit();
                    row.set("texture", assets::defString(stem));
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        float radius = floatOr(row, "radius", 1.0f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("radius", &radius, 0.01f, 0.0001f, 100000.0f, "%.4f m")) {
            row.set("radius", assets::defNumber(radius, kMetreDecimals));
            changed = true;
        }
        noteActivation();

        // The stage-C warning, and the button it never had. `ModelMatch` owns
        // the agreement rule so that the panel and `forge.unit` cannot drift.
        ModelMatch match;
        match.authoredRadius = radius;
        match.radiusDelta = report.boundingRadius - radius;
        if (match.radiusAgrees()) {
            ImGui::TextDisabled("  matches the mesh (%.4f m)",
                                static_cast<double>(report.boundingRadius));
        } else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
            if (match.radiusDelta > 0.0f) {
                ImGui::Text("  MESH IS %.4f m (%.1f%%) LARGER: the collision sphere sits inside "
                            "the hull, so ships pass through the picture",
                            static_cast<double>(match.radiusDelta),
                            static_cast<double>(match.radiusDeltaPercent()));
            } else {
                ImGui::Text("  MESH IS %.4f m (%.1f%%) SMALLER: the sphere reaches past what is "
                            "drawn, so ships stop short of nothing",
                            static_cast<double>(-match.radiusDelta),
                            static_cast<double>(-match.radiusDeltaPercent()));
            }
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            char label[64];
            std::snprintf(label, sizeof(label), "use measured %.4f m",
                          static_cast<double>(report.boundingRadius));
            if (ImGui::Button(label)) {
                beginEdit();
                row.set("radius", assets::defNumber(report.boundingRadius, kMetreDecimals));
                changed = true;
            }
        }

        // 0 in the file means "the same as radius", and the panel says so rather
        // than showing a bare zero that reads as "steering ignores this".
        float avoid = floatOr(row, "avoid_radius", 0.0f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("avoid_radius", &avoid, 0.05f, 0.0f, 100000.0f, "%.2f m")) {
            row.set("avoid_radius", assets::defNumber(avoid, kMetreDecimals));
            changed = true;
        }
        noteActivation();
        if (avoid == 0.0f) {
            ImGui::TextDisabled("  0 = the same as radius");
        } else if (avoid < radius) {
            ImGui::TextDisabled("  below radius: the schema will refuse this");
        }

        float emissive = floatOr(row, "emissive", 0.0f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("emissive", &emissive, 0.005f, 0.0f, 4.0f, "%.3f")) {
            row.set("emissive", assets::defNumber(emissive, kUnitDecimals));
            changed = true;
        }
        noteActivation();

        bool solid = boolOr(row, "solid", true);
        if (ImGui::Checkbox("solid", &solid)) {
            beginEdit();
            row.set("solid", assets::defBool(solid));
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(solid ? "(blocks flight)" : "(fly through)");

        bool translucent = boolOr(row, "translucent", false);
        if (ImGui::Checkbox("translucent", &translucent)) {
            beginEdit();
            row.set("translucent", assets::defBool(translucent));
            changed = true;
        }
        if (translucent) {
            float alpha = floatOr(row, "alpha", 1.0f);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragFloat("alpha", &alpha, 0.005f, 0.0f, 1.0f, "%.3f")) {
                row.set("alpha", assets::defNumber(alpha, kUnitDecimals));
                changed = true;
            }
            noteActivation();
        }
        ImGui::PopID();
        ImGui::Separator();
    }

    if (drawn == 0) {
        // ⚑ The gap the whole programme exists to close: a mesh authored, saved
        // and cooked, that the game still has no way to name.
        ImGui::TextDisabled("no [[model]] row names this mesh");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("without one the cooked mesh cannot be referenced by any def, "
                            "which is what stage A made possible and nothing has ever done "
                            "from inside this tool");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("create [[model]] row")) {
            beginEdit();
            DefRow& row = m_models.append("model");
            // Prefilled from what is on screen: the stem names the mesh, the
            // measurement sets the radius, and the texture is whichever one the
            // author is looking at.
            row.set("id", assets::defString(entry.stem));
            row.set("mesh", assets::defString(entry.stem));
            row.set("texture",
                    assets::defString(textureStems.empty() ? "hull" : textureStems.front()));
            row.set("radius", assets::defNumber(report.boundingRadius, kMetreDecimals));
            changed = true;
        }
    }

    if (changed) {
        m_modelsDirty = true;
        if (!revalidate()) {
            // Kept in the document so the author can see and fix what they
            // typed; the last valid database stands until they do.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", m_error.c_str());
            ImGui::PopStyleColor();
        }
    } else if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }
    return changed;
}

} // namespace forge
