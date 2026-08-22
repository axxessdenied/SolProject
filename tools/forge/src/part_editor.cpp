#include "part_editor.hpp"

#include "gltf.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace forge {

using namespace sol;
using assets::ForgeDoc;
using assets::ForgeParamKind;
using assets::ForgeParamSpec;
using assets::ForgePart;
using assets::ForgePrimitive;
using assets::ForgeValue;

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

// Depth of a part in the tree, used only to indent the list. Bounded by the
// part count so a document that somehow held a cycle would still draw.
[[nodiscard]] int depthOf(const ForgeDoc& doc, std::size_t index)
{
    int depth = 0;
    std::size_t cursor = index;
    while (depth <= static_cast<int>(doc.parts.size())) {
        const std::string& parent = doc.parts[cursor].parent;
        if (parent.empty()) {
            break;
        }
        const std::size_t next = doc.indexOf(parent);
        if (next == std::string::npos) {
            break;
        }
        cursor = next;
        ++depth;
    }
    return depth;
}

// ⚑ Double, not float. ImGui's DragFloat would quantise every value it touched
// to 24 bits of mantissa, and this document is authored in double precisely so
// that a rebuilt mesh comes out bit-identical to the one it replaces.
[[nodiscard]] bool dragDouble(const char* label, double& value, double speed)
{
    return ImGui::DragScalar(label, ImGuiDataType_Double, &value, static_cast<float>(speed));
}

[[nodiscard]] bool dragDoubleN(const char* label, double* values, int count, double speed)
{
    return ImGui::DragScalarN(label, ImGuiDataType_Double, values, count,
                              static_cast<float>(speed));
}

} // namespace

void PartEditor::openNew(const std::string& directory)
{
    m_doc = ForgeDoc{};
    // The header a new file starts life with. It lives here rather than in
    // writeForge because the writer is faithful to the document and invents
    // nothing - a file that opens without a header must save without one, or
    // the round trip that lets the tool save safely stops holding.
    m_doc.header = "# Authored with the Forge (engine plan Phase 9). This file is the\n"
                   "# SOURCE: the mesh beside it is built from these parts and can be\n"
                   "# rebuilt from them, which a cooked buffer of triangles cannot.\n";
    m_doc.name = "untitled";
    ForgePart part;
    part.id = "part_1";
    part.primitive = ForgePrimitive::Box;
    m_doc.parts.push_back(part);

    m_path = directory.empty() ? std::string("untitled.forge") : directory + "/untitled.forge";
    m_saveName = "untitled.forge";
    m_selected = 0;
    m_open = true;
    m_dirty = true;
    m_buildError.clear();
    m_editError.clear();
    // A new document has no history, and keeping the last one's would let an
    // undo replace this file's contents with a different asset's.
    m_undo.clear();
}

bool PartEditor::openFile(const std::string& path, std::string& status)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path.c_str(), bytes)) {
        status = "cannot read " + path;
        return false;
    }
    ForgeDoc doc;
    std::string error;
    if (!assets::parseForge(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                            path.c_str(), doc, &error)) {
        status = error;
        SOL_LOG_ERROR("forge: %s", error.c_str());
        return false;
    }
    m_doc = std::move(doc);
    m_path = path;
    m_saveName = fileStem(path) + ".forge";
    m_selected = m_doc.parts.empty() ? -1 : 0;
    m_open = true;
    m_dirty = false;
    m_buildError.clear();
    m_editError.clear();
    m_undo.clear();
    status = "opened " + m_saveName + " (" + std::to_string(m_doc.parts.size()) + " parts)";
    return true;
}

bool PartEditor::save(std::string& status)
{
    if (!m_open) {
        return false;
    }
    const std::string directory = directoryOf(m_path);
    const std::string target = directory.empty() ? m_saveName : directory + "/" + m_saveName;
    const std::string text = assets::writeForge(m_doc);
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

bool PartEditor::exportGltf(std::string& status)
{
    if (!m_open) {
        return false;
    }
    assets::MeshData mesh;
    std::string error;
    if (!assets::buildForge(m_doc, mesh, &error)) {
        status = error;
        return false;
    }
    const std::string directory = directoryOf(m_path);
    const std::string stem = fileStem(m_saveName);
    const std::string target = (directory.empty() ? stem : directory + "/" + stem) + ".gltf";
    const std::string json = cooker::exportGltf(mesh, stem.c_str());
    if (!platform::writeFileBytes(target.c_str(), json.data(), json.size())) {
        status = "cannot write " + target;
        return false;
    }
    status = "exported " + target;
    SOL_LOG_INFO("forge: %s (%zu verts)", status.c_str(), mesh.vertices.size());
    return true;
}

void PartEditor::beginEdit()
{
    if (!m_open) {
        return;
    }
    m_undo.push_back(m_doc);
    if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
    }
}

bool PartEditor::undo()
{
    if (m_undo.empty()) {
        return false;
    }
    m_doc = std::move(m_undo.back());
    m_undo.pop_back();
    // ⚑ Still dirty after an undo, and deliberately. Undoing back to the state
    // on disk is indistinguishable here from undoing to some other state, and
    // claiming "saved" when the file has not been written since is the one lie
    // that costs an author their work.
    m_dirty = true;
    if (m_selected >= static_cast<int>(m_doc.parts.size())) {
        m_selected = m_doc.parts.empty() ? -1 : static_cast<int>(m_doc.parts.size()) - 1;
    }
    m_buildError.clear();
    return true;
}

bool PartEditor::movePoint(const assets::ForgePoint& point, assets::BuildPoint delta,
                           std::string& error)
{
    if (!m_open) {
        return false;
    }
    if (!assets::forgeMovePoint(m_doc, point, delta, &error)) {
        return false;
    }
    m_dirty = true;
    return true;
}

bool PartEditor::movePoints(std::span<const assets::ForgePoint> points, assets::BuildPoint delta,
                            bool& dropped, std::string& error)
{
    if (!m_open) {
        return false;
    }
    if (!assets::forgeMovePoints(m_doc, points, delta, &dropped, &error)) {
        return false;
    }
    m_dirty = true;
    return true;
}

// ⚑ Both of these work on a COPY and only push undo once the engine has
// accepted, which is the discipline E3's bake set: the cheap version - push,
// try, roll back - leaves a no-op in the history for an author to press
// through, and `undo()` marks the document dirty, so a refused button would
// claim unsaved work.
bool PartEditor::splitEdge(std::span<const assets::ForgePoint> points,
                           std::span<const assets::ForgeFace> faces, std::uint32_t a,
                           std::uint32_t b, std::string& error)
{
    if (!m_open) {
        return false;
    }
    ForgeDoc next = m_doc;
    if (!assets::forgeSplitEdge(next, points, faces, a, b, &error)) {
        return false;
    }
    beginEdit();
    m_doc = std::move(next);
    m_dirty = true;
    return true;
}

bool PartEditor::extrudeFaces(std::span<const assets::ForgeFace> faces,
                              std::span<const std::uint32_t> group, double& offset,
                              std::string& error)
{
    if (!m_open) {
        return false;
    }
    ForgeDoc next = m_doc;
    if (!assets::forgeExtrudeFaces(next, faces, group, &offset, &error)) {
        return false;
    }
    beginEdit();
    m_doc = std::move(next);
    m_dirty = true;
    return true;
}

bool PartEditor::isDescendant(std::size_t candidate, std::size_t part) const
{
    if (candidate == part) {
        return true;
    }
    std::size_t cursor = candidate;
    for (std::size_t step = 0; step <= m_doc.parts.size(); ++step) {
        const std::string& parent = m_doc.parts[cursor].parent;
        if (parent.empty()) {
            return false;
        }
        const std::size_t next = m_doc.indexOf(parent);
        if (next == std::string::npos) {
            return false;
        }
        if (next == part) {
            return true;
        }
        cursor = next;
    }
    return false;
}

bool PartEditor::drawPartList()
{
    bool changed = false;

    if (ImGui::BeginChild("##parts", {0.0f, 170.0f}, ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(m_doc.parts.size()); ++i) {
            const ForgePart& part = m_doc.parts[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const int depth = depthOf(m_doc, static_cast<std::size_t>(i));
            char label[192];
            std::snprintf(label, sizeof(label), "%*s%s  (%s)", depth * 2, "", part.id.c_str(),
                          assets::forgePrimitiveName(part.primitive));
            if (ImGui::Selectable(label, i == m_selected)) {
                m_selected = i;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (ImGui::BeginCombo("##add", "add part")) {
        for (const ForgePrimitive primitive : assets::forgePrimitives()) {
            if (ImGui::Selectable(assets::forgePrimitiveName(primitive))) {
                changed = addPrimitive(primitive) || changed;
            }
        }
        ImGui::EndCombo();
    }

    // ⚑ THE BUTTONS GET THEIR OWN ROW, AND THEY DID NOT USED TO. `add part` is a
    // combo wide enough that `delete` was already running off the panel's right
    // edge with only three widgets on the line - clipped, not scrollable, since
    // stage D. Adding `bake` beside it is what made it visible, which is the
    // SIXTH time this repo has met "a widget does not fit its box" and the first
    // time it was a control rather than a sentence. Three buttons fit; four
    // widgets never did.
    const bool hasSelection = m_selected >= 0 && m_selected < static_cast<int>(m_doc.parts.size());
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("duplicate") && hasSelection) {
        beginEdit();
        ForgePart copy = m_doc.parts[static_cast<std::size_t>(m_selected)];
        copy.id = m_doc.uniqueId(copy.id);
        m_doc.parts.push_back(std::move(copy));
        m_selected = static_cast<int>(m_doc.parts.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("delete") && hasSelection) {
        // ⚑ Deleting a parent would orphan its children into a document that
        // no longer parses, so the children are re-hung on the grandparent.
        // Silently dropping them, or leaving a dangling `parent`, are both ways
        // of writing a file the cooker will reject.
        beginEdit();
        const std::string removed = m_doc.parts[static_cast<std::size_t>(m_selected)].id;
        const std::string grandparent = m_doc.parts[static_cast<std::size_t>(m_selected)].parent;
        m_doc.parts.erase(m_doc.parts.begin() + m_selected);
        for (ForgePart& part : m_doc.parts) {
            if (part.parent == removed) {
                part.parent = grandparent;
            }
        }
        if (m_selected >= static_cast<int>(m_doc.parts.size())) {
            m_selected = static_cast<int>(m_doc.parts.size()) - 1;
        }
        changed = true;
    }

    // ⚑ THE D CHECKPOINT'S RULE, AS A BUTTON. A hand edit bakes only the parts
    // it touches; everything else stays parametric. This is where a torus stops
    // being "no parametric answer, come back later" and becomes editable - the
    // ring is a function of two segment indices and nothing else in the tool can
    // give it a number to write.
    const bool bakeable =
        hasSelection &&
        m_doc.parts[static_cast<std::size_t>(m_selected)].primitive != ForgePrimitive::Group &&
        m_doc.parts[static_cast<std::size_t>(m_selected)].primitive != ForgePrimitive::Mesh;
    ImGui::SameLine();
    ImGui::BeginDisabled(!bakeable);
    if (ImGui::Button("bake") && bakeable) {
        // ⚑ Baked into a COPY first, so a refusal touches neither the document
        // nor the undo history. Pushing the entry and rolling it back would
        // leave a no-op in the history for an author to press through, and
        // `undo()` deliberately marks the document dirty - so the cheap version
        // of this would claim unsaved work after a button that did nothing.
        ForgeDoc next = m_doc;
        std::string error;
        if (assets::forgeBakeDocumentPart(next, static_cast<std::size_t>(m_selected), &error)) {
            beginEdit();
            m_doc = std::move(next);
            m_editError.clear();
            changed = true;
        } else {
            m_editError = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // ⚑⚑ MERGE GETS ITS OWN ROW, AND IT IS A COMBO RATHER THAN A VIEWPORT
    // MULTI-SELECT. It needs TWO objects and every selection in this tool holds
    // one; a viewport multi-select is a selection model, an undo question and a
    // highlight scheme all at once, while a combo naming the other part is the
    // idiom this panel already uses for `parent`. And it is a row of its own
    // because the line above is already three buttons wide - the E3 finding was
    // that four widgets never fit, and repeating it here would be the seventh
    // sighting of the same defect.
    //
    // ⚑ It is built BEFORE the extrude for a reason: an extrude refuses a face
    // whose triangles come from two parts and tells the author to merge them,
    // and a refusal that names something unbuilt is a lie.
    const bool mergeable =
        hasSelection && m_doc.parts[static_cast<std::size_t>(m_selected)].primitive !=
                            ForgePrimitive::Group;
    ImGui::BeginDisabled(!mergeable);
    if (ImGui::BeginCombo("##merge", "merge with...")) {
        for (std::size_t i = 0; i < m_doc.parts.size(); ++i) {
            const ForgePart& candidate = m_doc.parts[i];
            // Only what the engine can actually take: not itself, not a group,
            // and under the same parent - a baked part's geometry is stored in
            // its parent's frame, so two parents would be two frames.
            if (!mergeable || static_cast<int>(i) == m_selected ||
                candidate.primitive == ForgePrimitive::Group ||
                candidate.parent != m_doc.parts[static_cast<std::size_t>(m_selected)].parent) {
                continue;
            }
            if (ImGui::Selectable(candidate.id.c_str())) {
                // Into a copy, like the bake: a refusal must leave the document
                // and the undo history exactly as it found them.
                ForgeDoc next = m_doc;
                std::string error;
                if (assets::forgeMergeParts(next, static_cast<std::size_t>(m_selected), i,
                                            &error)) {
                    beginEdit();
                    m_doc = std::move(next);
                    // The EARLIER part survives, so that is what stays selected.
                    m_selected = std::min(m_selected, static_cast<int>(i));
                    m_editError.clear();
                    changed = true;
                } else {
                    m_editError = error;
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (!m_editError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored({0.95f, 0.45f, 0.35f, 1.0f}, "%s", m_editError.c_str());
        ImGui::PopTextWrapPos();
    }
    return changed;
}

bool PartEditor::drawParams(ForgePart& part)
{
    bool changed = false;
    for (const ForgeParamSpec& spec : assets::forgeParams(part.primitive)) {
        ForgeValue value = part.value(spec.name);
        bool edited = false;
        ImGui::PushID(spec.name);
        switch (spec.kind) {
        case ForgeParamKind::Scalar:
            edited = dragDouble(spec.name, value.scalar, 0.05);
            break;
        case ForgeParamKind::Integer: {
            int shown = static_cast<int>(value.scalar);
            if (ImGui::DragInt(spec.name, &shown, 0.25f, 3, 512)) {
                value.scalar = shown;
                edited = true;
            }
            break;
        }
        case ForgeParamKind::Boolean: {
            bool shown = value.scalar != 0.0;
            if (ImGui::Checkbox(spec.name, &shown)) {
                value.scalar = shown ? 1.0 : 0.0;
                edited = true;
            }
            break;
        }
        case ForgeParamKind::Vec3:
            edited = dragDoubleN(spec.name, &value.vec.x, 3, 0.05);
            break;
        case ForgeParamKind::Uv:
            edited = dragDoubleN(spec.name, &value.uv.u, 2, 0.01);
            break;
        case ForgeParamKind::Profile: {
            ImGui::Text("%s  (%zu points)", spec.name, value.profile.size());
            for (std::size_t i = 0; i < value.profile.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                char label[32];
                std::snprintf(label, sizeof(label), "##p%zu", i);
                if (dragDoubleN(label, &value.profile[i].x, 2, 0.05)) {
                    edited = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    value.profile.erase(value.profile.begin() +
                                        static_cast<std::ptrdiff_t>(i));
                    edited = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("add point")) {
                value.profile.push_back(value.profile.empty()
                                            ? assets::BuildProfilePoint{1.0, 0.0}
                                            : value.profile.back());
                edited = true;
            }
            break;
        }
        }
        ImGui::PopID();
        // ⚑ Written back only on an actual edit. Assigning every frame would
        // round-trip untouched doubles through the widget and dirty a document
        // nobody changed.
        if (edited) {
            part.set(spec.name, value);
            changed = true;
        }
    }
    return changed;
}

bool PartEditor::drawSelectedPart()
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_doc.parts.size())) {
        ImGui::TextDisabled("no part selected");
        return false;
    }
    bool changed = false;
    ForgePart& part = m_doc.parts[static_cast<std::size_t>(m_selected)];

    char idBuffer[128];
    std::snprintf(idBuffer, sizeof(idBuffer), "%s", part.id.c_str());
    if (ImGui::InputText("id", idBuffer, sizeof(idBuffer))) {
        const std::string wanted = idBuffer;
        // A rename has to carry the children with it, and an empty or taken id
        // is refused rather than written - both would produce a file that no
        // longer parses.
        if (!wanted.empty() && m_doc.indexOf(wanted) == std::string::npos) {
            const std::string previous = part.id;
            part.id = wanted;
            for (ForgePart& other : m_doc.parts) {
                if (other.parent == previous) {
                    other.parent = wanted;
                }
            }
            changed = true;
        }
    }

    if (ImGui::BeginCombo("type", assets::forgePrimitiveName(part.primitive))) {
        for (const ForgePrimitive primitive : assets::forgePrimitives()) {
            if (ImGui::Selectable(assets::forgePrimitiveName(primitive),
                                  primitive == part.primitive)) {
                // Parameters are kept across a type change: the schema decides
                // which ones are read, so a box turned into a beam and back
                // still has the size it started with.
                part.primitive = primitive;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    const char* parentLabel = part.parent.empty() ? "(root)" : part.parent.c_str();
    if (ImGui::BeginCombo("parent", parentLabel)) {
        if (ImGui::Selectable("(root)", part.parent.empty())) {
            part.parent.clear();
            changed = true;
        }
        for (std::size_t i = 0; i < m_doc.parts.size(); ++i) {
            // Its own subtree is not offered, which is what keeps the parent
            // graph acyclic by construction rather than by validation.
            if (isDescendant(i, static_cast<std::size_t>(m_selected))) {
                continue;
            }
            if (ImGui::Selectable(m_doc.parts[i].id.c_str(), m_doc.parts[i].id == part.parent)) {
                part.parent = m_doc.parts[i].id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    if (dragDoubleN("position", &part.position.x, 3, 0.05)) {
        changed = true;
    }
    if (dragDoubleN("rotation", &part.rotationDegrees.x, 3, 1.0)) {
        changed = true;
    }
    if (dragDoubleN("scale", &part.scale.x, 3, 0.01)) {
        changed = true;
    }
    ImGui::TextDisabled("rotation is degrees, X then Y then Z");

    if (!assets::forgeParams(part.primitive).empty()) {
        ImGui::Separator();
        if (drawParams(part)) {
            changed = true;
        }
    }
    return changed;
}

bool PartEditor::addPrimitive(ForgePrimitive primitive)
{
    // ⚑ A toolbar button can be pressed with nothing open, and the panel's combo
    // never could - so the guard belongs here rather than at either call site.
    if (!m_open) {
        return false;
    }
    beginEdit();
    ForgePart part;
    part.primitive = primitive;
    part.id = m_doc.uniqueId(assets::forgePrimitiveName(primitive));
    // A new part lands beside the selection rather than at the root, which is
    // almost always where an author wants it next.
    if (m_selected >= 0 && m_selected < static_cast<int>(m_doc.parts.size())) {
        part.parent = m_doc.parts[static_cast<std::size_t>(m_selected)].parent;
    }
    m_doc.parts.push_back(part);
    m_selected = static_cast<int>(m_doc.parts.size()) - 1;
    return true;
}

bool PartEditor::draw()
{
    if (!m_open) {
        ImGui::TextDisabled("no part document open");
        ImGui::TextDisabled("open a .forge from the list, or start a new one");
        return false;
    }

    bool changed = false;

    char nameBuffer[128];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_saveName.c_str());
    if (ImGui::InputText("file", nameBuffer, sizeof(nameBuffer))) {
        m_saveName = nameBuffer;
    }
    ImGui::TextDisabled("%s", directoryOf(m_path).c_str());

    std::string status;
    if (ImGui::Button("save")) {
        if (!save(status)) {
            m_buildError = status;
        } else {
            m_buildError.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("export .gltf")) {
        if (!exportGltf(status)) {
            m_buildError = status;
        }
    }
    ImGui::SameLine();
    // ⚑ Named "undo drag" rather than "undo", because that is its reach in this
    // slice: a point drag and the three part-list buttons. The parameter
    // widgets below fire continuously while held, so an entry per frame would
    // bury the state before the edit under sixty identical copies - and the
    // asteroid's 99 KB is what makes that a real cost rather than a tidy one.
    // Naming the gap in the button is cheaper than a person discovering it.
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("undo drag")) {
        changed = undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_dirty ? "* unsaved" : "saved");

    // ⚑ A save used to rewrite the file from the document and drop every
    // comment in it. It no longer does: whole-line comments and the blank lines
    // around them come back exactly where they were, asserted byte for byte
    // against the six committed assets in geometry.unit. What still cannot be
    // placed is a comment sharing a line with a value, or one inside a
    // multi-line array's brackets - so the warning fires only for a file that
    // actually carries one, which is the difference between a warning and
    // wallpaper.
    if (m_doc.hasUnplaceableComments) {
        // Wrapped for the same reason the catalog warning is: the sentence is
        // the finding, and a sentence that stops at the panel edge is not one.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
        ImGui::TextUnformatted("this file has a comment the writer cannot place - one after a "
                               "value on the same line, or one inside an array's brackets - and "
                               "saving will drop it");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    if (!m_buildError.empty()) {
        ImGui::TextColored({0.95f, 0.45f, 0.35f, 1.0f}, "%s", m_buildError.c_str());
    }

    ImGui::Separator();
    if (drawPartList()) {
        changed = true;
    }
    ImGui::Separator();
    if (drawSelectedPart()) {
        changed = true;
    }

    if (changed) {
        m_dirty = true;
    }
    return changed;
}

} // namespace forge
