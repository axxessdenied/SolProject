#include "part_editor.hpp"

#include "gltf.hpp"
#include "history_buttons.hpp"
#include "list_layout_style.hpp"
#include "part_pick.hpp"

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
    forgetHistory();
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
    forgetHistory();
    status = "opened " + m_saveName + " (" + std::to_string(m_doc.parts.size()) + " parts)";
    return true;
}

// ⚑⚑ THE THIRD OF THE FOUR WAYS THE ORDER AND THE SNAPSHOTS COME APART, and
// the only one whose consequence is destructive rather than merely wrong: with
// the order left standing, a `Ctrl+Z` after opening a second asset would
// replace this document's contents with the previous file's. Throwing the
// snapshots away without telling the history is exactly the bug - the history
// would still offer the step.
void PartEditor::forgetHistory()
{
    m_undo.clear();
    m_redo.clear();
    if (m_history != nullptr) {
        m_history->forget(EditHistory::Editor::Mesh);
    }
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

void PartEditor::beginEdit(std::string label)
{
    if (!m_open) {
        return;
    }
    m_undo.push_back(m_doc);
    // ⚑ A new edit ends the forward branch. This clears only THIS editor's
    // snapshots; the history drops the ORDER for all three and raises the flag
    // that makes the caller sweep the others, because an edit here can
    // invalidate a redo entry belonging to the texture editor.
    m_redo.clear();
    if (m_history != nullptr) {
        m_history->note(EditHistory::Editor::Mesh, std::move(label));
    }
    if (m_undo.size() > kUndoDepth) {
        m_undo.erase(m_undo.begin());
        // ⚑⚑ THE CAPS DO NOT EVICT TOGETHER even though both are 64: the
        // history counts edits from all three editors and this one counts
        // only its own. Telling it is what stops it promising a step whose
        // snapshot has already gone.
        if (m_history != nullptr) {
            m_history->evicted(EditHistory::Editor::Mesh);
        }
    }
}

bool PartEditor::undoStep()
{
    if (m_undo.empty()) {
        return false;
    }
    m_redo.push_back(std::move(m_doc));
    m_doc = std::move(m_undo.back());
    m_undo.pop_back();
    afterStep();
    return true;
}

bool PartEditor::redoStep()
{
    if (m_redo.empty()) {
        return false;
    }
    m_undo.push_back(std::move(m_doc));
    m_doc = std::move(m_redo.back());
    m_redo.pop_back();
    afterStep();
    return true;
}

// ⚑ SHARED BY BOTH DIRECTIONS ON PURPOSE. Everything here is a consequence of
// "the document was replaced by a snapshot", which is equally true stepping
// back and stepping forward - and a rule applied in two places becomes a defect
// in the one nobody looked at, which this tool has now recorded five times.
void PartEditor::afterStep()
{
    // ⚑ Still dirty after a step, and deliberately. Landing on the state on
    // disk is indistinguishable here from landing on any other, and claiming
    // "saved" when the file has not been written since is the one lie that
    // costs an author their work.
    m_dirty = true;
    if (m_selected >= static_cast<int>(m_doc.parts.size())) {
        m_selected = m_doc.parts.empty() ? -1 : static_cast<int>(m_doc.parts.size()) - 1;
    }
    m_buildError.clear();
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
    beginEdit("split edge");
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
    beginEdit("extrude");
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

void PartEditor::selectPart(std::size_t part)
{
    if (part >= m_doc.parts.size()) {
        return;
    }
    m_selected = static_cast<int>(part);
    m_scrollToSelected = true;
}

std::size_t PartEditor::selectedPart() const
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_doc.parts.size())) {
        return kNoPart;
    }
    return static_cast<std::size_t>(m_selected);
}

std::size_t PartEditor::takeHoveredPart()
{
    const std::size_t hovered = m_hoveredPart;
    m_hoveredPart = kNoPart;
    return hovered;
}

bool PartEditor::drawPartList()
{
    bool changed = false;

    // ⚑ Stage M's filter, and it is offered only on a document long enough to
    // need finding rather than reading. It filters the DRAW and never the
    // indexing - `m_selected` stays an index into `m_doc.parts`, exactly as K1
    // kept `openIndex` flat when it grouped the mesh list - so nothing else in
    // this file has to know the filter exists.
    const bool offerFilter = m_doc.parts.size() >= kFilterFromRows;
    if (offerFilter) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##partfilter", "filter parts", m_partFilter,
                                 sizeof(m_partFilter));
    } else {
        // A short document cannot leave a stale filter hiding its own parts.
        m_partFilter[0] = '\0';
    }
    const std::string_view needle{m_partFilter};

    // ⚑⚑ THE SELECTED ROW IS ALWAYS DRAWN, MATCH OR NO MATCH (stage N), AND
    // THIS IS THE TRAP STAGE M LEFT FOR STAGE N. The filter draws only matching
    // rows while `m_selected` indexes the whole of `m_doc.parts`, so a viewport
    // pick on a part the filter excludes selects a row THAT IS NOT ON SCREEN -
    // no highlight anywhere, which reads exactly like the click having done
    // nothing while the parameter fields below quietly change to another part.
    //
    // ⚑⚑ AND THE REASON IT IS THIS RULE RATHER THAN "CLEAR THE FILTER", WHICH
    // WAS THE FIRST ANSWER AND IS THE OBVIOUS ONE: AN ACTIVE `InputText` OWNS
    // ITS BUFFER. ImGui keeps the edited text in its own state while the field
    // is active and writes that copy back into the caller's buffer, so clearing
    // `m_partFilter` from outside the widget is silently undone in the SAME
    // frame - measured, not guessed: `selectPart` logged "clearing filter
    // 'floor'" and `drawPartList` went on seeing 'floor' with no intervening
    // empty. Fighting that needs `ClearActiveID` out of `imgui_internal.h`.
    // ⚑ Showing the row instead is better on its own terms anyway: it keeps the
    // filter the author was part-way through typing, and it also covers the
    // reverse order - select a row, then type a filter that excludes it.
    const auto rowVisible = [&](std::size_t index) {
        return listMatchesFilter(m_doc.parts[index].id, needle) ||
               static_cast<int>(index) == m_selected;
    };

    std::size_t matches = 0;      // rows the list will draw, for its height
    std::size_t needleHits = 0;   // rows the FILTER matched, for the message
    for (std::size_t i = 0; i < m_doc.parts.size(); ++i) {
        needleHits += listMatchesFilter(m_doc.parts[i].id, needle) ? 1u : 0u;
        matches += rowVisible(i) ? 1u : 0u;
    }

    // Stage M: as tall as the tree wants, up to 45% of the panel. The document
    // being edited is what this window is FOR, so it gets the larger of the two
    // shares here; the mesh list above it is navigation.
    const float outerHeight = ImGui::GetWindowHeight();
    const float partsHeight =
        listHeight(textRowMetrics(), matches, kMinListRows, 0.45f, outerHeight);
    if (ImGui::BeginChild("##parts", {0.0f, partsHeight}, ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(m_doc.parts.size()); ++i) {
            const ForgePart& part = m_doc.parts[static_cast<std::size_t>(i)];
            if (!rowVisible(static_cast<std::size_t>(i))) {
                continue;
            }
            ImGui::PushID(i);
            const int depth = depthOf(m_doc, static_cast<std::size_t>(i));
            char label[192];
            std::snprintf(label, sizeof(label), "%*s%s  (%s)", depth * 2, "", part.id.c_str(),
                          assets::forgePrimitiveName(part.primitive));
            if (ImGui::Selectable(label, i == m_selected)) {
                m_selected = i;
            }
            // ⚑⚑ STAGE O: THE ROW UNDER THE CURSOR LIGHTS ITS PART UP IN THE
            // VIEWPORT, WHICH IS THE ONE PAIRING STAGE N LEFT UNWIRED. N drew a
            // box for a *committed* selection from either surface and for an
            // *uncommitted* hover from the viewport only - so answering "which
            // lump is that" from the list meant clicking, i.e. committing to a
            // selection in order to ask a question. This is a read: it does not
            // touch `m_selected`, does not arm the scroll, and cannot dirty the
            // document. ⚑ After the widget, like the scroll below and for the
            // same reason - `IsItemHovered` reports on the item just submitted.
            if (ImGui::IsItemHovered()) {
                m_hoveredPart = static_cast<std::size_t>(i);
            }
            // Stage N: bring a selection made in the VIEWPORT into view here.
            // ⚑ After the widget, because `SetScrollHereY` scrolls to the item
            // just submitted - the same ordering trap stage G met with
            // `IsItemActivated`, where placing the call before the widget reads
            // the previous one.
            if (m_scrollToSelected && i == m_selected) {
                ImGui::SetScrollHereY(0.5f);
            }
            ImGui::PopID();
        }
        // ⚑ On the NEEDLE's hits, not on the drawn rows: with a selection
        // showing through, `matches` is never zero and the message would never
        // appear even when the filter genuinely found nothing.
        if (needleHits == 0) {
            ImGui::TextDisabled("no part matches \"%s\"", m_partFilter);
        }
    }
    ImGui::EndChild();
    // ⚑ Consumed whether or not a row was found to scroll to, or a selection
    // the filter still hides would re-arm it every frame and pin the list.
    m_scrollToSelected = false;

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
        beginEdit("duplicate part");
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
        beginEdit("delete part");
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
            beginEdit("bake part");
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
                    beginEdit("merge parts");
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

// ⚑⚑⚑ STAGE Q's WHOLE POINT, IN ONE FUNCTION CALL PER WIDGET. Before it, every
// parameter here was UNMEASURABLY absent from the history: `drawParams`
// contained no `beginEdit` at all, so dragging `position` moved a part three
// metres across twelve logged frames with the undo depth sitting at zero and
// the document dirty. There was nothing to press `Ctrl+Z` on.
//
// ⚑⚑ IT MUST BE `IsItemActivated`, NOT THE WRITE-BACK PATH, and the reason is
// the one the `undo drag` button's old name was apologising for: a drag reports
// an edit on every frame the mouse moves, so pushing a snapshot from the edit
// would bury the state before the gesture under sixty identical copies and one
// press of undo would step back a single frame. Activation fires once, on the
// frame the widget takes the mouse. `TextureEditor::noteActivation` has done
// exactly this since stage G, with a comment observing that `PartEditor` "has
// always known this" - it had, and it had never applied it here.
//
// ⚑ IT WORKS OVER A `DragScalarN` TOO, which is not obvious: three sub-widgets
// are submitted inside one group, and `imgui.cpp:11828` forwards the group's
// `ActiveId` to `LastItemData` precisely so `IsItemActive`/`IsItemActivated`
// are functional over the whole thing. Checked in the vendored source, not
// assumed.
void PartEditor::noteActivation(const char* label)
{
    if (ImGui::IsItemActivated()) {
        beginEdit(label);
    }
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
            noteActivation(spec.name);
            break;
        case ForgeParamKind::Integer: {
            int shown = static_cast<int>(value.scalar);
            if (ImGui::DragInt(spec.name, &shown, 0.25f, 3, 512)) {
                value.scalar = shown;
                edited = true;
            }
            noteActivation(spec.name);
            break;
        }
        case ForgeParamKind::Boolean: {
            bool shown = value.scalar != 0.0;
            if (ImGui::Checkbox(spec.name, &shown)) {
                value.scalar = shown ? 1.0 : 0.0;
                edited = true;
            }
            // ⚑ A checkbox is a press rather than a drag, so activation and
            // edit are the same frame - but it goes through the same call so
            // there is one rule here, not one rule and an exception.
            noteActivation(spec.name);
            break;
        }
        case ForgeParamKind::Vec3:
            edited = dragDoubleN(spec.name, &value.vec.x, 3, 0.05);
            noteActivation(spec.name);
            break;
        case ForgeParamKind::Uv:
            edited = dragDoubleN(spec.name, &value.uv.u, 2, 0.01);
            noteActivation(spec.name);
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
                noteActivation("move profile point");
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    beginEdit("remove profile point");
                    value.profile.erase(value.profile.begin() +
                                        static_cast<std::ptrdiff_t>(i));
                    edited = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("add point")) {
                beginEdit("add profile point");
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
    const bool idEdited = ImGui::InputText("id", idBuffer, sizeof(idBuffer));
    // ⚑⚑ A TEXT FIELD CANNOT USE `IsItemActivated` THE WAY A DRAG DOES.
    // Activation is the CLICK INTO the field, which is not an edit - clicking
    // in and straight back out would leave a no-op in the history for an author
    // to press through, which is the thing the `bake` button goes out of its
    // way to avoid. So the snapshot is taken on the first REPORTED CHANGE while
    // the field is active, and once per session rather than once per keystroke:
    // typing "Fuselage" is one rename, not eight.
    //
    // ⚑ It is still taken before the document is touched, because `InputText`
    // writes into the caller's char buffer and `part.id` is assigned below.
    if (!ImGui::IsItemActive()) {
        m_idEditOpen = false;
    }
    if (idEdited) {
        const std::string wanted = idBuffer;
        // A rename has to carry the children with it, and an empty or taken id
        // is refused rather than written - both would produce a file that no
        // longer parses.
        if (!wanted.empty() && m_doc.indexOf(wanted) == std::string::npos) {
            if (!m_idEditOpen) {
                m_idEditOpen = true;
                beginEdit("rename part");
            }
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
                beginEdit("change type");
                part.primitive = primitive;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    const char* parentLabel = part.parent.empty() ? "(root)" : part.parent.c_str();
    if (ImGui::BeginCombo("parent", parentLabel)) {
        if (ImGui::Selectable("(root)", part.parent.empty())) {
            beginEdit("reparent");
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
                beginEdit("reparent");
                part.parent = m_doc.parts[i].id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    // ⚑ THESE THREE ARE THE MOST-USED WIDGETS IN THE TOOL and were the least
    // recoverable: the measurement that opened this stage was a `position` drag
    // moving a part three metres with nothing on the undo stack at all.
    if (dragDoubleN("position", &part.position.x, 3, 0.05)) {
        changed = true;
    }
    noteActivation("move part");
    if (dragDoubleN("rotation", &part.rotationDegrees.x, 3, 1.0)) {
        changed = true;
    }
    noteActivation("rotate part");
    if (dragDoubleN("scale", &part.scale.x, 3, 0.01)) {
        changed = true;
    }
    noteActivation("scale part");
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
    beginEdit(std::string("add ") + assets::forgePrimitiveName(primitive));
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
    // ⚑⚑ THE DIRTY MARK BELONGS HERE, AND LEAVING IT TO THE CALLER WAS A REAL
    // DEFECT SHIPPED BY K5 AND FOUND BY THE USER AT STAGE L. `draw()` sets it
    // from its own `changed` flag at the tail, which covered the `add part`
    // combo because the combo lives inside `draw()` - but the TOOLBAR calls this
    // method from `main.cpp`, outside any of that, so a toolbar-added part left
    // the document marked CLEAN. K5's own comment above warned that a rule
    // written twice diverges in the copy nobody looked at; it moved the
    // PLACEMENT rule in here and left this one behind.
    //
    // What it cost: stage L's import refuses to merge over an unsaved document
    // and asks `dirty()`, so a part added from the toolbar was invisible to that
    // guard - the import merged into the file on disk, which had never seen it,
    // and the author's part was gone on the next reload.
    m_dirty = true;
    return true;
}

void PartEditor::adoptDoc(ForgeDoc doc)
{
    // ⚑ A Blender re-send is an edit like any other and is recorded like one -
    // which is what the import path already assumed when it chose to adopt the
    // merged document rather than re-open the file, so as not to "make the
    // import the one edit in this tool a person cannot take back".
    beginEdit("import from Blender");
    m_doc = std::move(doc);
    m_dirty = true;
    // The selection is an index into a part list that has just been rebuilt, so
    // it can point past the end or at something else entirely. Clamped rather
    // than cleared: an import usually replaces parts in place, so the row the
    // author was on is very often still the row they want.
    if (m_selected >= static_cast<int>(m_doc.parts.size())) {
        m_selected = m_doc.parts.empty() ? -1 : static_cast<int>(m_doc.parts.size()) - 1;
    }
    m_buildError.clear();
    m_editError.clear();
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
    // ⚑⚑ IT GETS ITS PLAIN NAME BACK AT STAGE Q. It was called `undo drag` for
    // eight stages, with a comment naming its own gap - "that is its reach in
    // this slice: a point drag and the three part-list buttons" - because the
    // parameter widgets fired continuously while held and an entry per frame
    // would have buried the state before the edit under sixty identical copies.
    // The reasoning was right; the conclusion was a placeholder. `IsItemActivated`
    // is what closes it, so the button can say what it does.
    //
    // ⚑ AND IT NOW MEANS WHAT `Ctrl+Z` MEANS. It does not undo "this panel" -
    // there is one history in this tool and this is a second way to reach it,
    // so pressing it while the last edit was a texture edit undoes that.
    if (m_history != nullptr) {
        drawHistoryButtons(*m_history);
    }
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
