#pragma once

// The texture editor (engine plan Phase 9 stage G) - what turns the Forge from
// something that shows a texture into something that authors one.
//
// It owns the open `.tex` document and every edit made to it, and reports "the
// document changed" so the caller rebuilds the image and re-uploads it. Same
// arrangement as `PartEditor` and for the same reason: there is one path from a
// document to the thing the GPU holds, and the editor does not own a second.
//
// ⚑ Every value here is an INTEGER, which is the one way this panel is simpler
// than the part editor's. A texture document has no lengths, no angles and no
// round-off: a pixel coordinate and a colour channel are exact, so none of the
// double-vs-float care `part_editor.hpp` documents applies. What does carry over
// is the discipline behind it - write back only when a widget reports an edit,
// so redrawing a panel never dirties a file nobody touched.

#include "edit_history.hpp"

#include "sol/assets/texture_doc.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace forge {

class TextureEditor
{
public:
    // Starts a new document with one fill in it, for the same reason the part
    // editor starts with a box: a tool that opens on nothing gives an author
    // nothing to grab.
    void openNew(const std::string& directory);

    [[nodiscard]] bool openFile(const std::string& path, std::string& status);
    [[nodiscard]] bool save(std::string& status);
    void close();

    // Draws the panel. Returns true when the document changed and the caller
    // must rebuild and re-upload.
    [[nodiscard]] bool draw();

    // Draws the flat preview as an EDIT SURFACE (stage I): click a panel to
    // select it, drag it to move it. `image` is the caller's ImGui handle for
    // the texture this document built - the editor never uploads anything, so
    // there is still exactly one path from a document to what the GPU holds.
    // Returns true when the document changed.
    //
    // ⚑ The gesture belongs to an `InvisibleButton` rather than to the viewport,
    // which is what keeps G's undo rule working unchanged: `ImGui::Image` is not
    // an activatable item and has no `IsItemActivated` to hang a gesture on.
    [[nodiscard]] bool drawPreview(void* image, float availableWidth);

    // Undo is a copy of the document, per the E1 precedent: `TextureDoc` is a
    // plain value and the three committed sources are 0.6-3 KB, so a command
    // pattern would be a second description of every edit for no gain.
    // Stage Q: `label` is what the status line says the undo took back, and
    // the step functions are driven by `EditHistory` rather than by this
    // editor's own button. See `edit_history.hpp`.
    void beginEdit(std::string label);
    [[nodiscard]] bool undoStep();
    [[nodiscard]] bool redoStep();
    void clearRedo() { m_redo.clear(); }
    void setHistory(EditHistory* history) { m_history = history; }
    [[nodiscard]] std::size_t undoDepth() const { return m_undo.size(); }
    [[nodiscard]] std::size_t redoDepth() const { return m_redo.size(); }

    // ⚑ Pushes undo on the frame the LAST-SUBMITTED widget became active, which
    // is the whole difference between one undo entry per gesture and one per
    // frame. A DragInt reports an edit on every frame the mouse moves, so
    // pushing from the write-back path turns a 24-frame drag into 24 entries -
    // one press of `undo` then steps back a single frame and the other 23 evict
    // real history. `PartEditor` has always known this (its `movePoint` pushes
    // nothing and the caller opens the gesture once); this is the same rule
    // where the gesture belongs to ImGui rather than to the viewport.
    void noteActivation(const char* label);

    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] const sol::assets::TextureDoc& doc() const { return m_doc; }
    [[nodiscard]] bool dirty() const { return m_dirty; }
    [[nodiscard]] const std::string& path() const { return m_path; }
    void setBuildError(const std::string& error) { m_buildError = error; }

private:
    [[nodiscard]] bool drawOpList();
    [[nodiscard]] bool drawSelectedOp();
    [[nodiscard]] bool drawParams(sol::assets::TextureLayer& layer);
    // What both directions must do once the document has been replaced.
    void afterStep();
    // Throws this editor's snapshots away AND tells the history.
    void forgetHistory();

    static constexpr std::size_t kUndoDepth = 64;

    // Recomputed lazily, on the press that needs it rather than on every edit:
    // a drag does not re-pick, so the map is stale exactly while nobody is
    // asking it anything.
    void refreshHitMap();
    // Clears everything the preview gesture holds about a document.
    void resetPick();

    // Highlights the row the preview picked and scrolls it into view once.
    // Returns whether a style was pushed, which `endPickedRow` must pop.
    [[nodiscard]] bool beginPickedRow(std::size_t ordinal);
    void endPickedRow(bool picked);

    sol::assets::TextureDoc m_doc;
    std::vector<sol::assets::TextureDoc> m_undo;
    std::vector<sol::assets::TextureDoc> m_redo;
    EditHistory* m_history = nullptr;
    std::string m_path;
    std::string m_buildError;
    std::string m_saveName;
    int m_selected = -1;
    // Which row of the selected op the preview picked, so the list can scroll
    // to it and the picture can outline it. -1 when the op has no rows.
    int m_selectedRow = -1;
    bool m_scrollToSelectedRow = false;
    bool m_open = false;
    bool m_dirty = false;

    // --- the preview gesture (stage I) ---
    std::vector<sol::assets::TextureHit> m_hitMap;
    bool m_hitMapDirty = true;
    sol::assets::TextureHit m_dragHit;
    bool m_dragging = false;
    // ⚑ The value and the cursor position the gesture STARTED at. Everything is
    // computed from these rather than from the previous frame, because rounding
    // a per-frame delta does not compose to rounding the total.
    float m_dragStartCursor[2] = {0.0f, 0.0f};
    int m_dragStartPosition[2] = {0, 0};
    // ⚑ Set on the first frame the row actually MOVES, not on the press. A
    // click that selects without moving must leave the document alone (E1's
    // zero-delta rule) AND leave no undo entry behind it (G's one-gesture
    // rule); this one flag is what satisfies both, and it is also what lets a
    // drag wander back to where it started and still be applied.
    bool m_dragMoved = false;
};

} // namespace forge
