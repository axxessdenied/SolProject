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

[[nodiscard]] std::string stringOr(const DefRow& row, const char* key, const char* fallback)
{
    const assets::DefKey* found = row.find(key);
    return found == nullptr ? std::string(fallback) : std::string(found->unquoted());
}

// An id not already taken in this document, derived from `base` - the same
// shape as `ForgeDoc::uniqueId`, one format over.
[[nodiscard]] std::string uniqueId(const DefDoc& doc, const std::string& type,
                                   const std::string& base)
{
    const auto taken = [&](const std::string& candidate) {
        return doc.find(type, candidate) != nullptr;
    };
    if (!taken(base)) {
        return base;
    }
    for (int n = 2; n < 1000; ++n) {
        const std::string candidate = base + "_" + std::to_string(n);
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return base;
}

} // namespace

void DefEditor::load(const std::string& dataDirectory)
{
    m_dataDirectory = dataDirectory;
    m_loaded = false;
    m_error.clear();
    m_undo.clear();
    m_openModels.clear();
    m_docs[kModels] = Document{.file = "models.toml"};
    m_docs[kShips] = Document{.file = "ships.toml"};
    m_docs[kStations] = Document{.file = "stations.toml"};
    if (dataDirectory.empty()) {
        m_error = "no data directory";
        return;
    }
    for (Document& document : m_docs) {
        bool ok = false;
        const std::string path = dataDirectory + "/" + document.file;
        const std::string text = readWholeFile(path, ok);
        if (!ok) {
            m_error = std::string("cannot read ") + document.file;
            return;
        }
        if (!assets::parseDefs(text.c_str(), text.size(), document.file, document.doc, &m_error)) {
            return;
        }
        document.ok = true;
    }
    m_loaded = revalidate();
}

bool DefEditor::revalidate()
{
    assets::DefDatabase candidate;
    for (const Document& document : m_docs) {
        const std::string text = assets::writeDefs(document.doc);
        std::string error;
        if (!candidate.mergeToml(text.c_str(), text.size(), document.file, &error)) {
            m_error = error;
            return false;
        }
    }
    m_defs = std::move(candidate);
    m_error.clear();
    return true;
}

std::vector<std::string> DefEditor::modelIds() const
{
    std::vector<std::string> ids;
    for (const DefRow& row : m_docs[kModels].doc.rows) {
        if (row.type == "model" && !row.id().empty()) {
            ids.emplace_back(row.id());
        }
    }
    return ids;
}

bool DefEditor::dirty() const
{
    for (const Document& document : m_docs) {
        if (document.dirty) {
            return true;
        }
    }
    return false;
}

void DefEditor::beginEdit(std::size_t document)
{
    if (!m_loaded || document >= kDocumentCount) {
        return;
    }
    m_undo.push_back(UndoEntry{.document = document, .doc = m_docs[document].doc});
    if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
    }
}

void DefEditor::noteActivation(std::size_t document)
{
    if (ImGui::IsItemActivated()) {
        beginEdit(document);
    }
}

bool DefEditor::undo()
{
    if (m_undo.empty()) {
        return false;
    }
    UndoEntry entry = std::move(m_undo.back());
    m_undo.pop_back();
    m_docs[entry.document].doc = std::move(entry.doc);
    m_docs[entry.document].dirty = true;
    (void)revalidate();
    return true;
}

bool DefEditor::save(std::string& status)
{
    if (!m_loaded) {
        status = "nothing loaded";
        return false;
    }
    if (!dirty()) {
        status = "no def changes to save";
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
    // ⚑ Written only where it changed. A def file the author did not touch must
    // not get a new timestamp, or `git status` after a session stops being a
    // readout of what was actually edited.
    std::string written;
    for (Document& document : m_docs) {
        if (!document.dirty) {
            continue;
        }
        const std::string text = assets::writeDefs(document.doc);
        const std::string path = m_dataDirectory + "/" + document.file;
        if (!platform::writeFileBytes(path.c_str(), text.data(), text.size())) {
            status = "cannot write " + path;
            return false;
        }
        document.dirty = false;
        written += (written.empty() ? "" : ", ");
        written += document.file;
        SOL_LOG_INFO("forge: wrote %s (%zu bytes)", path.c_str(), text.size());
    }
    status = "saved " + written;
    return true;
}

bool DefEditor::drawModelRows(const AssetEntry& entry, const MeshReport& report,
                              const std::vector<std::string>& textureStems)
{
    m_openModels.clear();
    if (!m_loaded) {
        ImGui::TextDisabled("def rows unavailable: %s",
                            m_error.empty() ? "no data directory" : m_error.c_str());
        return false;
    }

    bool changed = false;
    for (DefRow& row : m_docs[kModels].doc.rows) {
        if (row.type != "model") {
            continue;
        }
        const assets::DefKey* mesh = row.find("mesh");
        if (mesh == nullptr || mesh->unquoted() != entry.stem) {
            continue;
        }
        const std::string id(row.id());
        m_openModels.push_back(id);
        ImGui::PushID(id.c_str());
        ImGui::Text("[[model]] %s", id.c_str());

        // ⚑ The texture is a COMBO over what the tool can actually open, not a
        // text field. A `[[model]]` naming a texture that does not exist passes
        // the schema and fails at load, so the safest edit is the one that
        // cannot express the mistake.
        const std::string texture = stringOr(row, "texture", "");
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("texture", texture.c_str())) {
            for (const std::string& stem : textureStems) {
                const bool selected = stem == texture;
                if (ImGui::Selectable(stem.c_str(), selected) && !selected) {
                    beginEdit(kModels);
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
        noteActivation(kModels);

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
                beginEdit(kModels);
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
        noteActivation(kModels);
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
        noteActivation(kModels);

        bool solid = boolOr(row, "solid", true);
        if (ImGui::Checkbox("solid", &solid)) {
            beginEdit(kModels);
            row.set("solid", assets::defBool(solid));
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(solid ? "(blocks flight)" : "(fly through)");

        bool translucent = boolOr(row, "translucent", false);
        if (ImGui::Checkbox("translucent", &translucent)) {
            beginEdit(kModels);
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
            noteActivation(kModels);
        }
        ImGui::PopID();
        ImGui::Separator();
    }

    if (m_openModels.empty()) {
        // ⚑ The gap the whole programme exists to close: a mesh authored, saved
        // and cooked, that the game still has no way to name.
        ImGui::TextDisabled("no [[model]] row names this mesh");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("without one the cooked mesh cannot be referenced by any def, "
                            "which is what stage A made possible and nothing has ever done "
                            "from inside this tool");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("create [[model]] row")) {
            beginEdit(kModels);
            DefRow& row = m_docs[kModels].doc.append("model");
            // Prefilled from what is on screen: the stem names the mesh, the
            // measurement sets the radius, and the texture is whichever one the
            // author is looking at.
            row.set("id", assets::defString(uniqueId(m_docs[kModels].doc, "model", entry.stem)));
            row.set("mesh", assets::defString(entry.stem));
            row.set("texture",
                    assets::defString(textureStems.empty() ? "hull" : textureStems.front()));
            row.set("radius", assets::defNumber(report.boundingRadius, kMetreDecimals));
            changed = true;
        }
    }

    if (changed) {
        m_docs[kModels].dirty = true;
        (void)revalidate();
    }
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }
    return changed;
}

bool DefEditor::drawContentRow(std::size_t document, DefRow& row,
                               const std::vector<std::string>& models)
{
    bool changed = false;
    const std::string id(row.id());
    ImGui::PushID(id.c_str());
    ImGui::Text("[[%s]] %s", row.type.c_str(), id.c_str());

    char name[96];
    std::snprintf(name, sizeof(name), "%s", stringOr(row, "name", "").c_str());
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputText("name", name, sizeof(name))) {
        row.set("name", assets::defString(name));
        changed = true;
    }
    noteActivation(document);

    // ⚑ A COMBO, not a text field, and this is the cross-check the schema does
    // not do: `parseShip` reads `model` with `optionalString` and never resolves
    // it, so a typo parses clean and turns up at spawn as a log line behind a
    // fallback that draws something plausible. Offering the ids that exist makes
    // the mistake unspellable.
    const std::string model = stringOr(row, "model", row.type == "station" ? "station" : "ship");
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("model", model.c_str())) {
        for (const std::string& candidate : models) {
            const bool selected = candidate == model;
            if (ImGui::Selectable(candidate.c_str(), selected) && !selected) {
                beginEdit(document);
                row.set("model", assets::defString(candidate));
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (std::find(models.begin(), models.end(), model) == models.end()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
        ImGui::Text("  NO [[model]] ROW NAMED '%s': this parses cleanly and falls back at spawn, "
                    "so the ship flies wearing something else",
                    model.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    // A station has no scale - it is drawn at 1 and its size is its mesh's.
    if (row.type == "ship") {
        float scale = floatOr(row, "scale", 1.0f);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::DragFloat("scale", &scale, 0.01f, 0.01f, 1000.0f, "%.4f")) {
            row.set("scale", assets::defNumber(scale, kMetreDecimals));
            changed = true;
        }
        noteActivation(document);
        ImGui::TextDisabled("  the sim multiplies the model's radius by this");
    }
    ImGui::PopID();
    ImGui::Separator();
    return changed;
}

bool DefEditor::drawContentRows()
{
    if (!m_loaded) {
        return false;
    }
    if (m_openModels.empty()) {
        ImGui::TextDisabled("give the mesh a [[model]] row first");
        return false;
    }

    const std::vector<std::string> models = modelIds();
    const auto usesOpenModel = [&](const DefRow& row) {
        const std::string model =
            stringOr(row, "model", row.type == "station" ? "station" : "ship");
        return std::find(m_openModels.begin(), m_openModels.end(), model) != m_openModels.end();
    };

    bool changed = false;
    std::size_t shown = 0;
    for (const std::size_t document : {kShips, kStations}) {
        for (DefRow& row : m_docs[document].doc.rows) {
            if ((row.type != "ship" && row.type != "station") || !usesOpenModel(row)) {
                continue;
            }
            ++shown;
            if (drawContentRow(document, row, models)) {
                m_docs[document].dirty = true;
                changed = true;
            }
        }
    }
    if (shown == 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("nothing in the game flies or builds this model yet");
        ImGui::PopTextWrapPos();
    }

    // ⚑ The step that closes the programme: a mesh you authored becomes a thing
    // that exists in the world. Everything but the four asset keys takes its
    // schema default, which is a flyable ship - `ShipDef`'s defaults are a
    // complete tuning, not zeroes.
    if (ImGui::Button("create [[ship]] row")) {
        beginEdit(kShips);
        DefDoc& doc = m_docs[kShips].doc;
        DefRow& row = doc.append("ship");
        const std::string base = "sol." + m_openModels.front();
        row.set("id", assets::defString(uniqueId(doc, "ship", base)));
        row.set("name", assets::defString(m_openModels.front()));
        row.set("model", assets::defString(m_openModels.front()));
        row.set("scale", assets::defNumber(1.0f, kMetreDecimals));
        m_docs[kShips].dirty = true;
        changed = true;
    }

    if (changed) {
        (void)revalidate();
    }
    return changed;
}

} // namespace forge
