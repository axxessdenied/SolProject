#include "def_editor.hpp"

#include "def_surface.hpp"

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

// ⚑ One separator, whatever the caller supplied. Stage V hands this a PROJECT
// directory, which comes from `project_paths.hpp` with a trailing '/' already
// on it, where `game/data` never had one - and `C:/mod//models.toml` would have
// gone on working while reading back in every message the tool prints.
[[nodiscard]] std::string joinPath(const std::string& directory, const std::string& file)
{
    if (!directory.empty() && (directory.back() == '/' || directory.back() == '\\')) {
        return directory + file;
    }
    return directory + "/" + file;
}

[[nodiscard]] std::string readWholeFile(const std::string& path, bool& ok)
{
    std::vector<std::uint8_t> bytes;
    ok = platform::readFileBytes(path.c_str(), bytes);
    return ok ? std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) : std::string{};
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
[[nodiscard]] std::string uniqueId(const DefDoc& doc, const std::string& type, const std::string& base)
{
    const auto taken = [&](const std::string& candidate) { return doc.find(type, candidate) != nullptr; };
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
    forgetHistory();
    m_openModels.clear();
    m_openMaterial = kNoMaterial;
    m_docs[kModels] = Document{.file = "models.toml"};
    m_docs[kShips] = Document{.file = "ships.toml"};
    m_docs[kStations] = Document{.file = "stations.toml"};
    m_docs[kSounds] = Document{.file = "sounds.toml"};
    m_docs[kMaterials] = Document{.file = "materials.toml"};
    m_docs[kWeapons] = Document{.file = "weapons.toml"};
    m_docs[kComponents] = Document{.file = "components.toml"};
    if (dataDirectory.empty()) {
        m_error = "no data directory";
        return;
    }
    for (Document& document : m_docs) {
        bool ok = false;
        const std::string path = joinPath(dataDirectory, document.file);
        const std::string text = readWholeFile(path, ok);
        // ⚑⚑⚑ A MISSING FILE IS AN EMPTY DOCUMENT, NOT A REFUSAL, AND PHASE 24
        // STAGE V IS WHAT MADE THAT THE COMMON CASE. All five had to exist,
        // which was true of `game/data` and of nothing else: a mod supplies only
        // the def files it wants - "a mod that changes a price is one
        // commodities.toml" (game/mods/README.md) - and a NEW mod supplies none
        // at all. The old rule made the first thing an installed Forge is
        // pointed at report "cannot read models.toml" and refuse to edit
        // anything, which is the whole authoring surface closed on the whole
        // audience the tool was shipped for.
        //
        // ⚑⚑ MISSING AND UNPARSEABLE STAY DIFFERENT. An absent file is a
        // decision an author made; a file that will not parse is a mistake, and
        // silently treating it as empty would put an empty document in front of
        // somebody whose work is still on disk - and then WRITE it back over
        // them on the next save. The parse below is unchanged and still fatal.
        if (!ok) {
            document.ok = true;
            continue;
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

void DefEditor::beginEdit(std::string label)
{
    if (!m_loaded) {
        return;
    }
    UndoEntry entry;
    for (std::size_t i = 0; i < kDocumentCount; ++i) {
        entry.docs[i] = m_docs[i].doc;
        entry.dirty[i] = m_docs[i].dirty;
    }
    m_undo.push_back(std::move(entry));
    m_redo.clear();
    if (m_history != nullptr) {
        m_history->note(EditHistory::Editor::Def, std::move(label));
    }
    if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
        if (m_history != nullptr) {
            m_history->evicted(EditHistory::Editor::Def);
        }
    }
}

void DefEditor::noteActivation(const char* label)
{
    if (ImGui::IsItemActivated()) {
        beginEdit(label);
    }
}

void DefEditor::forgetHistory()
{
    m_undo.clear();
    m_redo.clear();
    if (m_history != nullptr) {
        m_history->forget(EditHistory::Editor::Def);
    }
}

// The state as it stands, for the opposite stack. ⚑ Taken BEFORE the swap in
// both directions, so a redo puts back exactly what the undo took - which is
// the same promise the one-document version made, now over the whole set.
DefEditor::UndoEntry DefEditor::snapshot() const
{
    UndoEntry entry;
    for (std::size_t i = 0; i < kDocumentCount; ++i) {
        entry.docs[i] = m_docs[i].doc;
        entry.dirty[i] = m_docs[i].dirty;
    }
    return entry;
}

void DefEditor::restore(UndoEntry& entry)
{
    for (std::size_t i = 0; i < kDocumentCount; ++i) {
        m_docs[i].doc = std::move(entry.docs[i]);
        m_docs[i].dirty = entry.dirty[i];
    }
    (void)revalidate();
}

bool DefEditor::undoStep()
{
    if (m_undo.empty()) {
        return false;
    }
    UndoEntry entry = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(snapshot());
    restore(entry);
    return true;
}

bool DefEditor::redoStep()
{
    if (m_redo.empty()) {
        return false;
    }
    UndoEntry entry = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back(snapshot());
    restore(entry);
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
        const std::string path = joinPath(m_dataDirectory, document.file);
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

void DefEditor::setOpenMesh(const std::string& stem)
{
    // ⚑⚑⚑ THIS USED TO LIVE INSIDE `drawModelRows`, AND STAGE D MADE THAT A
    // DEFECT RATHER THAN FINDING ONE. `drawModelRows` is submitted by the
    // Report panel; the Material panel arrived docked into the same node, so
    // Report became a TAB BEHIND IT - and a hidden ImGui window is not
    // submitted at all. Opening `ship.forge` with the Material tab in front
    // therefore left both of these holding `cockpit`, and the viewport drew the
    // ship wearing the cockpit's material: a surface the game will never put on
    // it, which is the exact failure this panel was shaped to make impossible.
    //
    // ⚑ So the caller sets it once a frame, above every panel, and no panel
    // owns it. The cost is walking eight rows; the alternative is a rule that
    // one window must be visible for another to be correct, which is the shape
    // of defect this tool keeps rediscovering.
    m_openModels.clear();
    m_openMaterial = kNoMaterial;
    if (!m_loaded || stem.empty()) {
        return;
    }
    for (const DefRow& row : m_docs[kModels].doc.rows) {
        if (row.type != "model") {
            continue;
        }
        const assets::DefKey* mesh = row.find("mesh");
        if (mesh != nullptr && mesh->unquoted() == stem && !row.id().empty()) {
            m_openModels.emplace_back(row.id());
        }
    }
    if (m_openModels.empty()) {
        return;
    }
    const std::uint32_t model = m_defs.modelIndex(m_openModels.front().c_str());
    if (model < m_defs.models().size()) {
        m_openMaterial = m_defs.models()[model].materialIndex;
    }
}

bool DefEditor::drawModelRows(const AssetEntry& entry,
                              const MeshReport& report,
                              const std::vector<std::string>& textureStems)
{
    setOpenMesh(entry.stem);
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
        ImGui::PushID(id.c_str());
        ImGui::Text("[[model]] %s", id.c_str());

        // ⚑⚑ PHASE 25 STAGE A: A ROW THAT NAMES A MATERIAL OWNS NONE OF THE
        // FOUR SURFACE KEYS, AND THE SCHEMA REFUSES A ROW CARRYING BOTH. Every
        // control below that writes one is therefore hidden rather than
        // disabled - a greyed-out combo still reads as "this row has a
        // texture", which is exactly the wrong thing to say. Authoring the
        // material itself is stage D; until then this panel's job is to not
        // write a file the game will refuse.
        const assets::DefKey* materialKey = row.find("material");
        const bool surfaceIsMaterials = materialKey != nullptr;
        if (surfaceIsMaterials) {
            ImGui::Text("material %s", std::string(materialKey->unquoted()).c_str());
            ImGui::TextDisabled("  the surface lives on that [[material]] row, not here");
        } else {
            // ⚑ The texture is a COMBO over what the tool can actually open,
            // not a text field. A `[[model]]` naming a texture that does not
            // exist passes the schema and fails at load, so the safest edit is
            // the one that cannot express the mistake.
            const std::string texture = stringOr(row, "texture", "");
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("texture", texture.c_str())) {
                for (const std::string& stem : textureStems) {
                    const bool selected = stem == texture;
                    if (ImGui::Selectable(stem.c_str(), selected) && !selected) {
                        beginEdit("set texture");
                        row.set("texture", assets::defString(stem));
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        }

        float radius = floatOr(row, "radius", 1.0f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("radius", &radius, 0.01f, 0.0001f, 100000.0f, "%.4f m")) {
            row.set("radius", assets::defNumber(radius, kMetreDecimals));
            changed = true;
        }
        noteActivation("set radius");

        // The stage-C warning, and the button it never had. `ModelMatch` owns
        // the agreement rule so that the panel and `forge.unit` cannot drift.
        ModelMatch match;
        match.authoredRadius = radius;
        match.radiusDelta = report.boundingRadius - radius;
        if (match.radiusAgrees()) {
            ImGui::TextDisabled("  matches the mesh (%.4f m)", static_cast<double>(report.boundingRadius));
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
            std::snprintf(
                label, sizeof(label), "use measured %.4f m", static_cast<double>(report.boundingRadius));
            if (ImGui::Button(label)) {
                beginEdit("use measured radius");
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
        noteActivation("set avoid_radius");
        if (avoid == 0.0f) {
            ImGui::TextDisabled("  0 = the same as radius");
        } else if (avoid < radius) {
            ImGui::TextDisabled("  below radius: the schema will refuse this");
        }

        if (!surfaceIsMaterials) {
            float emissive = floatOr(row, "emissive", 0.0f);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragFloat("emissive", &emissive, 0.005f, 0.0f, 4.0f, "%.3f")) {
                row.set("emissive", assets::defNumber(emissive, kUnitDecimals));
                changed = true;
            }
            noteActivation("set emissive");
        }

        bool solid = boolOr(row, "solid", true);
        if (ImGui::Checkbox("solid", &solid)) {
            beginEdit("toggle solid");
            row.set("solid", assets::defBool(solid));
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(solid ? "(blocks flight)" : "(fly through)");

        if (!surfaceIsMaterials) {
            bool translucent = boolOr(row, "translucent", false);
            if (ImGui::Checkbox("translucent", &translucent)) {
                beginEdit("toggle translucent");
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
                noteActivation("set alpha");
            }
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
            beginEdit("create [[model]] row");
            DefRow& row = m_docs[kModels].doc.append("model");
            // Prefilled from what is on screen: the stem names the mesh, the
            // measurement sets the radius, and the texture is whichever one the
            // author is looking at.
            row.set("id", assets::defString(uniqueId(m_docs[kModels].doc, "model", entry.stem)));
            row.set("mesh", assets::defString(entry.stem));
            row.set("texture", assets::defString(textureStems.empty() ? "hull" : textureStems.front()));
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
    // ⚑ Re-resolved after the revalidate above: an edit re-derives the whole
    // database, and `resolveMaterials` rebuilds every synthesised row FROM
    // SCRATCH after each merge, so an index taken before it names a material
    // from the previous one.
    setOpenMesh(entry.stem);
    return changed;
}

DefRow* DefEditor::materialRow(const std::string& id)
{
    return m_docs[kMaterials].doc.find("material", id);
}

void DefEditor::promoteMaterial(DefRow& model)
{
    const std::string modelId(model.id());
    // ⚑ ONE ENTRY FOR BOTH FILES. See `UndoEntry`: half of this move is a model
    // naming a material that does not exist, which is a state no author asked
    // for and the schema refuses.
    beginEdit("make a [[material]] row");
    DefRow& row = m_docs[kMaterials].doc.append("material");
    // ⚑⚑ THE ID IS `sol.` PLUS THE MODEL'S, WHICH IS THE CONVENTION THE TWO
    // SHIPPED ROWS ALREADY FOLLOW RATHER THAN ONE INVENTED HERE: model
    // `cockpit` wears `sol.cockpit` and model `gate_membrane` wears
    // `sol.gate_membrane`. Model ids are bare in this game and every other def
    // kind's are prefixed, so a promoted material that took the model's id
    // verbatim would be the only unprefixed row in `materials.toml` - visible
    // in a file an author reads, and wrong in exactly the way that teaches the
    // next person the wrong rule. ⚑ `sol.auto.` is reserved for the DERIVED
    // rows, so this can never collide with the one it replaces.
    const std::string base = modelId.rfind("sol.", 0) == 0 ? modelId : "sol." + modelId;
    row.set("id", assets::defString(uniqueId(m_docs[kMaterials].doc, "material", base)));
    moveSurfaceToMaterial(model, row);

    m_docs[kMaterials].dirty = true;
    m_docs[kModels].dirty = true;
    (void)revalidate();
}

bool DefEditor::drawSlotTable(DefRow& row, const std::vector<std::string>& textureStems)
{
    assets::DefKey* key = row.find("textures");
    if (key == nullptr) {
        return false;
    }

    // ⚑⚑ THE ENTRIES ARE READ IN FULL BEFORE ANY OF THEM IS WRITTEN. A splice
    // changes the length of the line every later entry's range is measured in,
    // so drawing straight off `inlineEntries()` would show garbage for the rest
    // of the table on the frame an edit happened - and those stale ranges are
    // what the next combo would then splice into.
    struct Slot
    {
        std::string name;
        std::string texture;
    };

    std::vector<Slot> slots;
    for (const assets::DefInlineEntry& entry : key->inlineEntries()) {
        std::string value(key->text.substr(entry.valueBegin, entry.valueEnd - entry.valueBegin));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        slots.push_back({entry.name, std::move(value)});
    }

    bool changed = false;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const Slot& slot = slots[i];
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo(slot.name.c_str(), slot.texture.c_str())) {
            for (const std::string& stem : textureStems) {
                const bool selected = stem == slot.texture;
                if (ImGui::Selectable(stem.c_str(), selected) && !selected) {
                    beginEdit("set texture slot");
                    // Found again by NAME rather than by the range read above,
                    // which an earlier splice this frame may already have moved.
                    if (assets::DefKey* live = row.find("textures"); live != nullptr) {
                        changed = live->setInlineValue(slot.name, assets::defString(stem)) || changed;
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("set 1 binding %zu", i);
    }
    return changed;
}

bool DefEditor::drawParamTable(DefRow& row)
{
    assets::DefKey* key = row.find("params");
    if (key == nullptr) {
        return false;
    }

    struct Param
    {
        std::string name;
        float value = 0.0f;
    };

    std::vector<Param> params;
    for (const assets::DefInlineEntry& entry : key->inlineEntries()) {
        const std::string value(key->text.substr(entry.valueBegin, entry.valueEnd - entry.valueBegin));
        params.push_back({entry.name, static_cast<float>(std::strtod(value.c_str(), nullptr))});
    }

    bool changed = false;
    for (Param& param : params) {
        ImGui::SetNextItemWidth(160.0f);
        // ⚑ NO RANGE, BECAUSE NOTHING DECLARES ONE. `glow_strength` is a gain on
        // an albedo and 2.2 is a sensible value for it; the next param this game
        // adds could be a metre or a count. A DragFloat with a speed and no clamp
        // says "this is a number the shader reads" without inventing a limit the
        // shader does not have - and a slider would have to invent one.
        if (ImGui::DragFloat(param.name.c_str(), &param.value, 0.01f, 0.0f, 0.0f, "%.3f")) {
            if (assets::DefKey* live = row.find("params"); live != nullptr) {
                changed = live->setInlineValue(param.name, assets::defNumber(param.value, kUnitDecimals)) ||
                          changed;
            }
        }
        noteActivation("set material param");
    }
    return changed;
}

DefEditor::MaterialEdit DefEditor::drawMaterialRows(const std::vector<std::string>& textureStems,
                                                    const std::vector<std::string>& vertexStems,
                                                    const std::vector<std::string>& fragmentStems,
                                                    const std::string& shownTexture,
                                                    std::string* outShowTexture)
{
    if (!m_loaded) {
        ImGui::TextDisabled("def rows unavailable: %s",
                            m_error.empty() ? "no data directory" : m_error.c_str());
        return MaterialEdit::None;
    }
    if (m_openModels.empty()) {
        // ⚑ The order matters, and the panel says it rather than leaving an
        // author to infer it: a material is what a model is drawn WITH, so there
        // is nothing to edit here until a row names this mesh.
        ImGui::TextDisabled("no [[model]] row names this mesh, so nothing here has a surface");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("the Mesh panel can make one; a material is how a model is drawn, so "
                            "the model row comes first");
        ImGui::PopTextWrapPos();
        return MaterialEdit::None;
    }

    const std::string modelId = m_openModels.front();
    if (m_openModels.size() > 1) {
        // ⚑ REPORTED RATHER THAN GUESSED AT. Several models can share one mesh
        // and wear different surfaces; the viewport draws one thing, so it draws
        // the first and this says which.
        ImGui::TextDisabled(
            "%zu [[model]] rows use this mesh - showing %s", m_openModels.size(), modelId.c_str());
    }

    if (m_openMaterial >= m_defs.materials().size()) {
        // ⚑⚑ THE CROSS-REFERENCE `mergeToml` DOES NOT CHECK, reachable now that
        // this editor can see both files. `validateMaterials` is a separate call
        // the GAME makes at boot, so before stage D a model naming a missing
        // material parsed clean in this tool and failed at load.
        const DefRow* model = m_docs[kModels].doc.find("model", modelId);
        const assets::DefKey* names = model != nullptr ? model->find("material") : nullptr;
        const std::string named = names != nullptr ? std::string(names->unquoted()) : std::string("?");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("model '%s' names material '%s', which no [[material]] row defines",
                           modelId.c_str(),
                           named.c_str());
        ImGui::PopStyleColor();
        return MaterialEdit::None;
    }

    const assets::MaterialDef& material = m_defs.materials()[m_openMaterial];

    // ⚑⚑⚑ THE COMMON CASE, AND THE ONE THIS PANEL EXISTS FOR. Five of the eight
    // shipped models name no material, so the database derives one and rebuilds
    // it from scratch after every merge - which means it has no file, no row and
    // nothing an author can put a slider on.
    if (material.synthesised) {
        ImGui::Text("%s", material.id.c_str());
        ImGui::TextDisabled("  derived from [[model]] %s - it has no row to edit", modelId.c_str());
        ImGui::TextDisabled("  texture %s, emissive %.3f%s",
                            material.texture.c_str(),
                            static_cast<double>(material.emissive),
                            material.translucent ? ", translucent" : "");
        ImGui::TextDisabled("  the stock lambert pair (mesh.vert, mesh.frag)");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("a [[material]] row is what lets this surface name its own shaders, "
                            "declare extra textures and carry tunable numbers. Making one moves the "
                            "surface keys off the model row, where they can no longer both live.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("make this a [[material]] row")) {
            if (DefRow* model = m_docs[kModels].doc.find("model", modelId); model != nullptr) {
                promoteMaterial(*model);
                return MaterialEdit::Structure;
            }
        }
        return MaterialEdit::None;
    }

    DefRow* row = materialRow(material.id);
    if (row == nullptr) {
        ImGui::TextDisabled("material '%s' is not defined in materials.toml", material.id.c_str());
        return MaterialEdit::None;
    }

    bool structure = false;
    bool params = false;
    ImGui::PushID(material.id.c_str());
    ImGui::Text("[[material]] %s", material.id.c_str());
    // ⚑ `id` is the match key - the model row points at it by name - so it is
    // shown and not edited, exactly as `asset` is on a sound row. Renaming it
    // here would break the reference in the same gesture that made it.
    ImGui::TextDisabled("  the surface of [[model]] %s", modelId.c_str());

    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("texture", material.texture.c_str())) {
        for (const std::string& stem : textureStems) {
            const bool selected = stem == material.texture;
            if (ImGui::Selectable(stem.c_str(), selected) && !selected) {
                beginEdit("set material texture");
                row->set("texture", assets::defString(stem));
                // ⚑ The picture follows the file. Without this the row would
                // change, the game would draw the new albedo, and the viewport
                // would go on showing the old one - which is the exact class of
                // "the tool disagrees with the game" this whole programme
                // exists to remove.
                if (outShowTexture != nullptr) {
                    *outShowTexture = stem;
                }
                structure = true;
            }
        }
        ImGui::EndCombo();
    }
    // ⚑ THE DEVIATION IS NAMED, NOT HIDDEN. Clicking a texture in the Texture
    // list still shades the mesh - that is how a UV gets checked against a
    // checker without touching a def - so the viewport can legitimately show
    // something this material does not name. What it must never do is show it
    // SILENTLY.
    if (!shownTexture.empty() && shownTexture != material.texture) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
        ImGui::TextWrapped("  the viewport is showing %s, which this material does not name",
                           shownTexture.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (ImGui::Button("show this material's texture")) {
            if (outShowTexture != nullptr) {
                *outShowTexture = material.texture;
            }
        }
    }

    // ⚑⚑ THE SHADER STEMS ARE COMBOS OVER WHAT IS ACTUALLY ON THE SEARCH PATH,
    // for the reason the texture combo is one: a stem that resolves to no `.spv`
    // costs the material its pipeline and every model wearing it its picture,
    // and that failure arrives as a log line rather than as anything the schema
    // could refuse. The lists are the compiled `.vert.spv` and `.frag.spv`
    // beside the tool, which is the same set the game looks in.
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("vertex_shader", material.vertexShader.c_str())) {
        for (const std::string& stem : vertexStems) {
            const bool selected = stem == material.vertexShader;
            if (ImGui::Selectable(stem.c_str(), selected) && !selected) {
                beginEdit("set vertex_shader");
                row->set("vertex_shader", assets::defString(stem));
                structure = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("fragment_shader", material.fragmentShader.c_str())) {
        for (const std::string& stem : fragmentStems) {
            const bool selected = stem == material.fragmentShader;
            if (ImGui::Selectable(stem.c_str(), selected) && !selected) {
                beginEdit("set fragment_shader");
                row->set("fragment_shader", assets::defString(stem));
                structure = true;
            }
        }
        ImGui::EndCombo();
    }

    float emissive = material.emissive;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat("emissive", &emissive, 0.005f, 0.0f, 4.0f, "%.3f")) {
        row->set("emissive", assets::defNumber(emissive, kUnitDecimals));
        // ⚑ Per-DRAW, not per-pipeline: emissive reaches the shader in the push
        // block, so moving it needs neither a rebuild nor a buffer write. The
        // viewport picks it up from the next frame's resolved `MaterialDef`.
        params = true;
    }
    noteActivation("set material emissive");

    bool translucent = material.translucent;
    if (ImGui::Checkbox("translucent", &translucent)) {
        beginEdit("toggle material translucent");
        row->set("translucent", assets::defBool(translucent));
        structure = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(seeds blend, depth_write and cull)");
    if (material.translucent) {
        float alpha = material.alpha;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("alpha", &alpha, 0.005f, 0.0f, 1.0f, "%.3f")) {
            row->set("alpha", assets::defNumber(alpha, kUnitDecimals));
            params = true;
        }
        noteActivation("set material alpha");
    }

    // ⚑ SHOWN AS RESOLVED, WRITTEN AS A KEY. `translucent` seeds these three, so
    // a row that says nothing still HAS values - and showing the raw key would
    // print "opaque" under a translucent row that never spelled `blend` out. A
    // key appears in the file the moment an author moves the control, and not
    // before, which is `floatOr`'s rule one type over.
    static const char* const kBlendNames[] = {"opaque", "alpha", "additive"};
    int blend = static_cast<int>(material.blend);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("blend", &blend, kBlendNames, 3)) {
        beginEdit("set blend");
        row->set("blend", assets::defString(kBlendNames[blend]));
        structure = true;
    }
    bool depthTest = material.depthTest;
    if (ImGui::Checkbox("depth_test", &depthTest)) {
        beginEdit("toggle depth_test");
        row->set("depth_test", assets::defBool(depthTest));
        structure = true;
    }
    ImGui::SameLine();
    bool depthWrite = material.depthWrite;
    if (ImGui::Checkbox("depth_write", &depthWrite)) {
        beginEdit("toggle depth_write");
        row->set("depth_write", assets::defBool(depthWrite));
        structure = true;
    }
    ImGui::SameLine();
    bool cull = material.cullBackFaces;
    if (ImGui::Checkbox("cull", &cull)) {
        beginEdit("toggle cull");
        row->set("cull", assets::defBool(cull));
        structure = true;
    }

    if (!material.slots.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("declared textures - a slot's POSITION is its binding");
        structure = drawSlotTable(*row, textureStems) || structure;
    }
    if (!material.params.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("declared params - matched into the shader by NAME");
        params = drawParamTable(*row) || params;
    }
    if (material.slots.empty() && material.params.empty()) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("this material declares no extra textures and no params, so it gets no "
                            "set 1 at all. Declaring one is a change to what the shader reads, so it "
                            "belongs beside the shader that wants it.");
        ImGui::PopTextWrapPos();
    }
    ImGui::PopID();

    if (structure || params) {
        m_docs[kMaterials].dirty = true;
        (void)revalidate();
    }
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }
    // ⚑ Structure wins when both moved: a rebuild repacks the params anyway, and
    // doing the cheap one as well would write into a buffer about to be freed.
    return structure ? MaterialEdit::Structure : (params ? MaterialEdit::Params : MaterialEdit::None);
}

bool DefEditor::drawSoundRows(const AssetEntry& entry, Audition& audition)
{
    audition = Audition{};
    if (!m_loaded) {
        ImGui::TextDisabled("def rows unavailable: %s",
                            m_error.empty() ? "no data directory" : m_error.c_str());
        return false;
    }

    bool changed = false;
    std::size_t shown = 0;
    for (DefRow& row : m_docs[kSounds].doc.rows) {
        if (row.type != "sound") {
            continue;
        }
        const assets::DefKey* asset = row.find("asset");
        if (asset == nullptr || asset->unquoted() != entry.stem) {
            continue;
        }
        ++shown;
        const std::string id(row.id());
        ImGui::PushID(id.c_str());
        ImGui::Text("[[sound]] %s", id.c_str());

        const float gain = floatOr(row, "gain", 1.0f);
        const float jitter = floatOr(row, "pitch_jitter", 0.0f);
        const auto cap = static_cast<int>(floatOr(row, "max_instances", 0.0f));

        // ⚑⚑ THE BUTTON THIS WHOLE STAGE IS FOR, AND IT PLAYS THE CUE RATHER
        // THAN THE FILE. The list above auditions what is on disk at gain 1;
        // this plays what the GAME would play - the row's gain, its jitter, its
        // instance cap - which is the only version of the sound anybody will
        // ever hear. Press it repeatedly and the cap and the jitter are both
        // audible, which is what makes those two numbers editable rather than
        // decorative.
        if (ImGui::Button("play cue")) {
            audition = Audition{.wanted = true,
                                .gain = gain,
                                .pitchJitter = jitter,
                                .maxInstances = static_cast<std::uint32_t>(cap < 0 ? 0 : cap)};
        }
        ImGui::SameLine();
        ImGui::TextDisabled("as the game fires it");

        float editedGain = gain;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragFloat("gain", &editedGain, 0.005f, 0.0f, 4.0f, "%.3f")) {
            row.set("gain", assets::defNumber(editedGain, kUnitDecimals));
            changed = true;
        }
        noteActivation("set gain");

        float editedJitter = jitter;
        ImGui::SetNextItemWidth(160.0f);
        // The schema's own ceiling: half is already a musical fifth either way.
        if (ImGui::DragFloat("pitch_jitter", &editedJitter, 0.002f, 0.0f, 0.5f, "%.3f")) {
            row.set("pitch_jitter", assets::defNumber(editedJitter, kUnitDecimals));
            changed = true;
        }
        noteActivation("set pitch_jitter");
        if (editedJitter == 0.0f) {
            ImGui::TextDisabled("  0 = every firing is the same recording");
        }

        // ⚑ SHOWN WITHOUT A CONTROL, because a 2D audition cannot demonstrate
        // it and a slider that does nothing you can hear is a lever reaching a
        // state this panel cannot show. Distance is the viewport's kind of
        // question and this tool has no world to put a listener in.
        ImGui::TextDisabled("rolloff %.0f m (positional only - the audition is 2D)",
                            static_cast<double>(floatOr(row, "rolloff", 500.0f)));

        int editedCap = cap;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::DragInt("max_instances", &editedCap, 0.1f, 0, 32)) {
            // ⚑⚑ `std::to_string` AND NOT `defNumber`, AND IT IS THE FIRST
            // INTEGER KEY THIS EDITOR HAS EVER WRITTEN. `defNumber` "always
            // carries a `.` so TOML reads a float rather than an integer" - its
            // own words - and `max_instances` is read with `optionalUint`,
            // which refuses anything that is not `isInteger()`. So the obvious
            // call writes `4.0` and the game's own schema throws the file out.
            // The panel would have SHOWN that refusal rather than shipping it,
            // which is `revalidate` earning its place; the point of writing it
            // down is that the next integer key will look equally obvious.
            row.set("max_instances", std::to_string(editedCap));
            changed = true;
        }
        noteActivation("set max_instances");
        if (editedCap == 0) {
            ImGui::TextDisabled("  0 = unlimited");
        }

        ImGui::PopID();
        ImGui::Separator();
    }

    if (shown == 0) {
        // ⚑ The sound half of the gap stage H closed for meshes: a cue cooked
        // and sitting in the output directory that no def names, and that
        // therefore nothing in the game can fire.
        ImGui::TextDisabled("no [[sound]] row names this asset");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("without one the cooked sound cannot be fired by the engine or by a "
                            "script, which is what a cue id is for");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("create [[sound]] row")) {
            beginEdit("create [[sound]] row");
            DefDoc& doc = m_docs[kSounds].doc;
            DefRow& row = doc.append("sound");
            // ⚑ Two keys and no more. `gain`, `pitch_jitter`, `rolloff` and
            // `max_instances` all have schema defaults that are already the
            // right answer for a new cue, and `writeForge`'s rule - "a
            // parameter the source never mentioned stays unmentioned" - is what
            // keeps a def file readable as a list of DECISIONS rather than a
            // dump of every key that exists. Each appears the moment it is
            // dragged above.
            row.set("id", assets::defString(uniqueId(doc, "sound", "sol." + entry.stem)));
            row.set("asset", assets::defString(entry.stem));
            changed = true;
        }
    }

    if (changed) {
        m_docs[kSounds].dirty = true;
        (void)revalidate();
    }
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }
    return changed;
}

bool DefEditor::drawContentRow(DefRow& row, const std::vector<std::string>& models)
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
    noteActivation("set name");

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
                beginEdit("set model");
                row.set("model", assets::defString(candidate));
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    // ⚑ Asked of the validated DATABASE rather than of the combo's list, so the
    // panel and `missingModelRefs` are one implementation - and `m_defs` is
    // rebuilt on every accepted edit, so a model row created this session and
    // not yet saved already counts as existing.
    if (m_defs.findModel(model.c_str()) == nullptr) {
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
        noteActivation("set scale");
        ImGui::TextDisabled("  the sim multiplies the model's radius by this");
    }
    ImGui::PopID();
    ImGui::Separator();
    return changed;
}

// ---------------------------------------------------------------------------
// The mount surface (engine plan Phase 31 stage D). No ImGui below this line:
// the gesture is `MountTool`'s and the document is this file's.
// ---------------------------------------------------------------------------

std::size_t DefEditor::hullRow(const DefDoc& doc, const std::string& hullId)
{
    return doc.indexOf("ship", hullId);
}

bool DefEditor::commitShips(DefDoc&& candidate, const char* label)
{
    if (!m_loaded) {
        m_error = "nothing loaded";
        return false;
    }
    // ⚑ THE WHOLE SET, not just the candidate. A mount can only be refused by
    // `ships.toml`'s own reading today, but `mergeToml` is layered - the
    // documents are merged in order into one database - and validating one file
    // alone would be a second, narrower schema. Running all five is what makes
    // "the tool cannot write a file the game rejects" true rather than likely.
    assets::DefDatabase probe;
    for (std::size_t i = 0; i < kDocumentCount; ++i) {
        const std::string text = assets::writeDefs(i == kShips ? candidate : m_docs[i].doc);
        std::string error;
        if (!probe.mergeToml(text.c_str(), text.size(), m_docs[i].file, &error)) {
            m_error = error;
            return false;
        }
    }
    // Accepted, so the edit is going to land and the undo entry is worth
    // pushing. ⚑ An empty label means the caller already pushed one for the
    // whole gesture - a viewport drag, which arrives here every frame.
    if (label != nullptr) {
        beginEdit(label);
    }
    m_docs[kShips].doc = std::move(candidate);
    m_docs[kShips].dirty = true;
    m_defs = std::move(probe);
    m_error.clear();
    return true;
}

std::vector<std::string> DefEditor::hullsOnOpenModel() const
{
    std::vector<std::string> hulls;
    if (!m_loaded || m_openModels.empty()) {
        return hulls;
    }
    for (const DefRow& row : m_docs[kShips].doc.rows) {
        if (row.type != "ship") {
            continue;
        }
        const std::string model = stringOr(row, "model", "ship");
        if (std::find(m_openModels.begin(), m_openModels.end(), model) == m_openModels.end()) {
            continue;
        }
        if (!row.id().empty()) {
            hulls.emplace_back(row.id());
        }
    }
    return hulls;
}

const assets::ShipDef* DefEditor::hull(const std::string& hullId) const
{
    return hullId.empty() ? nullptr : m_defs.findShip(hullId.c_str());
}

std::vector<DefEditor::Fitting> DefEditor::fittingsFor(assets::MountKind kind) const
{
    std::vector<Fitting> fittings;
    if (assets::mountTakesWeapon(kind)) {
        for (const assets::WeaponDef& weapon : m_defs.weapons()) {
            fittings.push_back({weapon.id, weapon.name, weapon.mount, weapon.size});
        }
        return fittings;
    }
    for (const assets::ComponentDef& component : m_defs.components()) {
        fittings.push_back({component.id, component.name, component.mount, component.size});
    }
    return fittings;
}

bool DefEditor::addMount(const std::string& hullId, const MountDraft& draft)
{
    DefDoc candidate = m_docs[kShips].doc;
    const std::size_t hull = hullRow(candidate, hullId);
    if (hull == DefDoc::kNoRow) {
        m_error = "no [[ship]] row named '" + hullId + "'";
        return false;
    }
    MountDraft placed = draft;
    // ⚑ The id is made unique HERE rather than trusted from the caller, so the
    // create button can never be the thing that produces a duplicate. A rename
    // the author types is a different gesture and IS allowed to be refused -
    // they can see what they typed and change it.
    placed.id = uniqueMountId(candidate, hull, draft.id);
    DefRow& row =
        candidate.insertAfter(mountInsertPoint(candidate, hull), kMountRowType, mountIndent(candidate, hull));
    writeMountDraft(row, placed, kMetreDecimals);
    return commitShips(std::move(candidate), "add mount");
}

bool DefEditor::removeMount(const std::string& hullId, const std::string& mountId)
{
    DefDoc candidate = m_docs[kShips].doc;
    const std::size_t hull = hullRow(candidate, hullId);
    if (hull == DefDoc::kNoRow) {
        m_error = "no [[ship]] row named '" + hullId + "'";
        return false;
    }
    const std::size_t row = findMountRow(candidate, hull, mountId);
    if (row == DefDoc::kNoRow) {
        m_error = "no mount '" + mountId + "' on " + hullId;
        return false;
    }
    candidate.eraseRow(row);
    return commitShips(std::move(candidate), "remove mount");
}

bool DefEditor::setMountAt(const std::string& hullId, const std::string& mountId, const float (&at)[3])
{
    DefDoc candidate = m_docs[kShips].doc;
    const std::size_t hull = hullRow(candidate, hullId);
    const std::size_t row = hull == DefDoc::kNoRow ? DefDoc::kNoRow : findMountRow(candidate, hull, mountId);
    if (row == DefDoc::kNoRow) {
        m_error = "no mount '" + mountId + "' on " + hullId;
        return false;
    }
    writeMountVector(candidate.rows[row], "at", at, kMetreDecimals);
    // ⚑ No label: this is the drag, and the entry was pushed when it started.
    return commitShips(std::move(candidate), nullptr);
}

bool DefEditor::setMountKey(const std::string& hullId,
                            const std::string& mountId,
                            const char* key,
                            std::string_view value)
{
    DefDoc candidate = m_docs[kShips].doc;
    const std::size_t hull = hullRow(candidate, hullId);
    const std::size_t row = hull == DefDoc::kNoRow ? DefDoc::kNoRow : findMountRow(candidate, hull, mountId);
    if (row == DefDoc::kNoRow) {
        m_error = "no mount '" + mountId + "' on " + hullId;
        return false;
    }
    candidate.rows[row].set(key, value);
    return commitShips(std::move(candidate), "set mount key");
}

bool DefEditor::clearMountKey(const std::string& hullId, const std::string& mountId, const char* key)
{
    DefDoc candidate = m_docs[kShips].doc;
    const std::size_t hull = hullRow(candidate, hullId);
    const std::size_t row = hull == DefDoc::kNoRow ? DefDoc::kNoRow : findMountRow(candidate, hull, mountId);
    if (row == DefDoc::kNoRow) {
        m_error = "no mount '" + mountId + "' on " + hullId;
        return false;
    }
    candidate.rows[row].remove(key);
    return commitShips(std::move(candidate), "clear mount key");
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
        const std::string model = stringOr(row, "model", row.type == "station" ? "station" : "ship");
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
            if (drawContentRow(row, models)) {
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
        beginEdit("create [[ship]] row");
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
