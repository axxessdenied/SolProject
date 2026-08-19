#include "part_editor.hpp"

#include "gltf.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <imgui.h>

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
                ForgePart part;
                part.primitive = primitive;
                part.id = m_doc.uniqueId(assets::forgePrimitiveName(primitive));
                // A new part lands beside the selection rather than at the root,
                // which is almost always where an author wants it next.
                if (m_selected >= 0) {
                    part.parent = m_doc.parts[static_cast<std::size_t>(m_selected)].parent;
                }
                m_doc.parts.push_back(part);
                m_selected = static_cast<int>(m_doc.parts.size()) - 1;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const bool hasSelection = m_selected >= 0 && m_selected < static_cast<int>(m_doc.parts.size());
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("duplicate") && hasSelection) {
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
    ImGui::EndDisabled();
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
    ImGui::TextDisabled("%s", m_dirty ? "* unsaved" : "saved");

    // ⚑ Said out loud because it is a trap rather than a footnote: the TOML
    // parser discards comments, so a save REWRITES the file from the document
    // and any header a person hand-wrote is gone. Saving over a hand-authored
    // asset is lossy in exactly the way an author will not expect.
    ImGui::TextDisabled("save rewrites the file: comments are not kept");

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
