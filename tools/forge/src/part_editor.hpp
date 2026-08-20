#pragma once

// The part-tree editor (engine plan Phase 9 stage D) - the panel that turns the
// Forge from something that opens a mesh into something that authors one.
//
// It owns the open `.forge` document and every edit made to it. The viewer half
// of the tool stays as it was: this reports "the document changed" and the
// caller rebuilds the mesh through stage B and re-uploads it, so there is one
// path from a part tree to triangles and the editor does not own a second.
//
// ⚑ Every numeric widget here is DOUBLE (ImGuiDataType_Double), not float, and
// values are written back ONLY when a widget reports an edit. Both halves of
// that matter: the document is authored in double, and a float round trip would
// turn an authored 0.075 into 0.07500000298023224 the first time a panel was
// drawn - dirtying a file nobody touched.

#include "sol/assets/forge_doc.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace forge {

class PartEditor
{
public:
    // Starts an empty document with one box in it, because a tool that opens on
    // nothing gives an author nothing to grab.
    void openNew(const std::string& directory);

    [[nodiscard]] bool openFile(const std::string& path, std::string& status);
    [[nodiscard]] bool save(std::string& status);
    [[nodiscard]] bool exportGltf(std::string& status);

    // Draws the panel. Returns true when the document changed and the caller
    // must rebuild.
    [[nodiscard]] bool draw();

    // ⚑ Undo is a COPY OF THE DOCUMENT, not a command pattern, and the reason
    // is the document rather than laziness: `ForgeDoc` is a plain value and the
    // five hand-authored assets are 0.5-5 KB. Only the baked asteroid at 99 KB
    // even registers, which is what the depth cap is for. A command pattern
    // would be a second description of every edit, and the second description
    // is the one that gets out of step.
    void beginEdit();
    [[nodiscard]] bool undo();
    [[nodiscard]] std::size_t undoDepth() const { return m_undo.size(); }

    // Moves one point of the built mesh by `delta`, writing every authored
    // value standing at it. Does NOT push undo - a drag calls beginEdit() once
    // and this many times.
    [[nodiscard]] bool movePoint(const sol::assets::ForgePoint& point,
                                 sol::assets::BuildPoint delta, std::string& error);

    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] const sol::assets::ForgeDoc& doc() const { return m_doc; }
    [[nodiscard]] bool dirty() const { return m_dirty; }
    [[nodiscard]] const std::string& path() const { return m_path; }
    // Set by the caller when a rebuild fails, so the panel can say so beside
    // the part that caused it rather than only in the log.
    void setBuildError(const std::string& error) { m_buildError = error; }

private:
    [[nodiscard]] bool drawPartList();
    [[nodiscard]] bool drawSelectedPart();
    [[nodiscard]] bool drawParams(sol::assets::ForgePart& part);
    // True when `candidate` is `part` or sits below it, which is what a parent
    // combo must refuse or the tree becomes a loop.
    [[nodiscard]] bool isDescendant(std::size_t candidate, std::size_t part) const;

    // ⚑ Deep enough to cover a session of nudging and shallow enough that the
    // asteroid's 99 KB of baked vertices cannot quietly become nine megabytes
    // of history.
    static constexpr std::size_t kUndoDepth = 64;

    sol::assets::ForgeDoc m_doc;
    std::vector<sol::assets::ForgeDoc> m_undo;
    std::string m_path;
    std::string m_buildError;
    // Why the last bake was refused. Separate from m_buildError, which is the
    // caller's rebuild failure - a bake that will not run and a document that
    // will not build are different problems with different fixes.
    std::string m_bakeError;
    std::string m_saveName;
    int m_selected = -1;
    bool m_open = false;
    bool m_dirty = false;
};

} // namespace forge
