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
#include <span>
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

    // Adds one primitive part and selects it. Pushes undo itself, so it is a
    // whole gesture rather than the middle of one.
    //
    // ⚑ THIS IS A METHOD RATHER THAN A LOOP BODY BECAUSE IT NOW HAS TWO
    // CALLERS: the `add part` combo in this panel, and the primitives toolbar
    // above the dockspace. The rule about where a new part LANDS (beside the
    // selection, not at the root) is the sort of thing that quietly diverges
    // when it is written twice - a rule applied in two places becomes a defect
    // in the one nobody looked at.
    [[nodiscard]] bool addPrimitive(sol::assets::ForgePrimitive primitive);

    // Replaces the whole document as ONE undoable edit, keeping the open file's
    // path and save name.
    //
    // ⚑⚑ STAGE L's IMPORT NEEDS THIS BECAUSE THE FILE ON DISK IS NOT THE
    // DOCUMENT. A re-send from Blender must merge into what the AUTHOR HAS ON
    // SCREEN, which may be ahead of the file by every edit since the last save -
    // merging into the file instead silently discards them, which is exactly
    // what a user hit: a part added here, a re-send from Blender, and the part
    // was gone. Asking `dirty()` and refusing was the first answer and it is the
    // weaker one: it depends on every edit path remembering to raise that flag
    // (one of them did not), and it makes the author alt-tab and save to accept
    // an import they just asked for.
    void adoptDoc(sol::assets::ForgeDoc doc);

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

    // Moves a SET of points by one delta - an edge's two - applying every
    // authored value standing at any of them exactly ONCE.
    //
    // ⚑ NOT `movePoint` in a loop, and the difference is the whole of E4c: a
    // box puts one `center`+`size` pair behind all eight of its corners, so a
    // loop applies the resize once per grabbed point and the edge travels twice
    // as far as the hand. `dropped` reports a component the box could not
    // express - see `forgeMovePoints`.
    [[nodiscard]] bool movePoints(std::span<const sol::assets::ForgePoint> points,
                                  sol::assets::BuildPoint delta, bool& dropped,
                                  std::string& error);

    // ⚑ THE TWO TOPOLOGY EDITS (stage E5), AND UNLIKE THE MOVES ABOVE THEY PUSH
    // UNDO THEMSELVES. A drag is one gesture spread over sixty frames, so it
    // calls beginEdit() once and movePoints() many times; a split and an extrude
    // are each one discrete press with one entry to undo. Both bake the parts
    // they touch, because a box with a face pulled out of it is not a box.
    [[nodiscard]] bool splitEdge(std::span<const sol::assets::ForgePoint> points,
                                 std::span<const sol::assets::ForgeFace> faces, std::uint32_t a,
                                 std::uint32_t b, std::string& error);
    [[nodiscard]] bool extrudeFaces(std::span<const sol::assets::ForgeFace> faces,
                                    std::span<const std::uint32_t> group, double& offset,
                                    std::string& error);

    // Stage N. Selects a part from OUTSIDE the panel - the viewport pick - and
    // arms the scroll that a click on the row would have got for free: at 20
    // visible rows of 40, a selection below the fold is a selection you cannot
    // see (stage I's precedent, where the texture op list scrolls to its row).
    //
    // ⚑ Selection alone never dirties the document, so this does not touch
    // `m_dirty`. ⚑ The other half of "the author can see what they picked" -
    // a filter that would hide the row - is handled in `drawPartList`, which
    // always draws the selected row; see the comment there for why it cannot be
    // done by clearing the filter from here.
    void selectPart(std::size_t part);
    // `kNoPart` when nothing is selected. `std::size_t` rather than the `int`
    // this holds internally, because the viewport side speaks in part indices
    // and one signed/unsigned seam is enough.
    [[nodiscard]] std::size_t selectedPart() const;

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
    // Why the last part-list edit - a bake, or since E5b a merge - was refused.
    // Separate from m_buildError, which is the caller's rebuild failure: an edit
    // that will not run and a document that will not build are different
    // problems with different fixes. It is NOT a second string per operation,
    // because "this edit was declined and here is why" is one thing however many
    // buttons can say it.
    std::string m_editError;
    std::string m_saveName;
    // Stage M. A fixed buffer rather than a std::string because that is what
    // ImGui::InputText writes into; 64 is generous for a part id, which is a
    // sanitised Blender object name at its longest.
    char m_partFilter[64] = {};
    int m_selected = -1;
    // Stage N. Armed by `selectPart` and consumed by the next `drawPartList`,
    // because the row that must be scrolled to does not exist until the list is
    // submitted - and the pick happens in the viewport, before any of it runs.
    bool m_scrollToSelected = false;
    bool m_open = false;
    bool m_dirty = false;
};

} // namespace forge
