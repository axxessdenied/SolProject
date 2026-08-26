#pragma once

// The Forge's one account of what the author has done (engine plan Phase 9
// stage Q). `Ctrl+Z` takes back the last thing you did, whichever of the three
// editors you did it in.
//
// ⚑⚑ IT OWNS THE ORDER AND NOTHING ELSE. The document snapshots stay in the
// editors, which already held them - this is not a second description of every
// edit, which is the thing `PartEditor`'s own undo comment warns about. What
// lives here is the one fact none of the editors can know on its own: which of
// them moved most recently. Only this class may push or pop that order, so
// there is exactly one account of it.
//
// ⚑⚑ WHY NOT ROUTE BY WHICH PANEL HAS FOCUS, WHICH IS WHAT EVERY OTHER TOOL
// DOES: because the 3D viewport is not an ImGui window. Measured across six
// gestures before this was written - clicking in the Mesh panel reports
// `mesh[submitted 1 focused 1]`, and clicking the VIEWPORT reports
// `mesh[submitted 1 focused 0]` with nothing else focused either, and it stays
// that way through an orbit. Focus-routing would therefore leave undo with
// nobody to route to after every camera move, point drag and part pick, which
// is most of the tool. The same trace showed the other half: a panel in an
// unselected tab reports `submitted 0`, so a router can never ASK a panel
// anything. The panel has to tell it, which is what `note` is.
//
// ⚑ The honest cost of the global model, stated rather than discovered: an
// undo can act on a document that is not on screen. It is always the edit that
// was just made, which is what "take that back" means, and the status line
// names it.

#include <cstddef>
#include <deque>
#include <string>
#include <utility>

namespace forge {

class EditHistory
{
public:
    // Which editor made an edit. `Def` covers both def documents, because
    // `DefEditor` owns the pair and its own undo entry already names which.
    enum class Editor
    {
        Mesh,
        Texture,
        Def,
    };

    struct Entry
    {
        Editor editor{Editor::Mesh};
        // What the status line says: "undo: move point". Short and in the
        // author's terms - the gesture, not the function that ran.
        std::string label;
    };

    // ⚑⚑⚑ THERE IS DELIBERATELY NO CAP HERE, AND THE FIRST DRAFT HAD ONE. It
    // capped itself at 64 - the same number as each editor's snapshot cap - on
    // the reasoning that "the order can never outlive the snapshots it names".
    // That reasoning is wrong with three editors: the history is the UNION of
    // what they hold and can legitimately be three times deeper. Worse, the two
    // caps then trimmed the same eviction TWICE - one entry dropped by this
    // cap and a second by `evicted` - silently losing an undo step whose
    // snapshot the editor was still holding. **Two bounds on one quantity is a
    // defect**, and this one was caught by the test written for `evicted`, one
    // press after the stacks would have come apart in a real session.
    //
    // The length is bounded by the editors: nothing appends here that did not
    // also push a snapshot, and each editor trims at its own cap and reports.

    // An edit is about to be made. Called from the editor's own `beginEdit`, so
    // there is one call site per editor rather than one per gesture.
    //
    // ⚑⚑ A NEW EDIT DISCARDS THE REDO BRANCH, which is the standard rule and
    // the reason `redoDiscarded` exists: the entries dropped here can name
    // OTHER editors, whose snapshots are then unreachable but still held. The
    // caller must sweep them - see `takeRedoDiscarded`.
    void note(Editor editor, std::string label)
    {
        if (!m_redo.empty()) {
            m_redo.clear();
            m_redoDiscarded = true;
        }
        m_undo.push_back(Entry{editor, std::move(label)});
    }

    // The editor's own cap has thrown away its OLDEST snapshot, so the oldest
    // entry naming it can no longer be undone.
    //
    // ⚑⚑ THIS IS THE OUT-OF-STEP CASE THAT IS EASIEST TO MISS, because both
    // caps are 64 and it looks as though they must evict together. They do
    // not: the history counts edits from ALL THREE editors while each editor
    // counts only its own, so the mesh editor fills its stack long before the
    // history fills its own if the author is also editing a texture.
    void evicted(Editor editor)
    {
        for (auto it = m_undo.begin(); it != m_undo.end(); ++it) {
            if (it->editor == editor) {
                m_undo.erase(it);
                return;
            }
        }
    }

    // An editor has thrown ALL its snapshots away - a document was opened,
    // created or closed - so nothing naming it can be undone or redone.
    void forget(Editor editor)
    {
        eraseAll(m_undo, editor);
        eraseAll(m_redo, editor);
    }

    // Undoes the most recent edit. `apply(Editor)` performs the step and
    // returns whether it happened.
    //
    // ⚑ THE CALLBACK RATHER THAN A TWO-CALL PROTOCOL is what keeps the order
    // and the snapshots in step: there is no window in which this class has
    // moved an entry and the editor has not moved its document. An `apply`
    // that returns false leaves the history untouched.
    template <typename Apply>
    [[nodiscard]] bool undo(Apply&& apply, std::string* label = nullptr)
    {
        return step(m_undo, m_redo, std::forward<Apply>(apply), label);
    }

    template <typename Apply>
    [[nodiscard]] bool redo(Apply&& apply, std::string* label = nullptr)
    {
        return step(m_redo, m_undo, std::forward<Apply>(apply), label);
    }

    // ⚑ CONSUME-ONCE, the stage O idiom: the caller drops every editor's redo
    // snapshots when this returns true, and asking again in the same frame
    // must not make it do so twice. One place notices, whichever of the many
    // `beginEdit` call sites raised it.
    [[nodiscard]] bool takeRedoDiscarded()
    {
        const bool discarded = m_redoDiscarded;
        m_redoDiscarded = false;
        return discarded;
    }

    // ⚑ A PANEL'S `undo` BUTTON MEANS EXACTLY WHAT `Ctrl+Z` MEANS, and the
    // buttons funnel through here rather than each editor undoing itself,
    // because "undo" having one meaning in the tool is the whole point of the
    // stage. Before Q there were three buttons doing three different things,
    // one of them named `undo drag` to admit it. Consume-once, read in the
    // same place as the keyboard.
    void requestUndo() { m_undoRequested = true; }
    void requestRedo() { m_redoRequested = true; }
    [[nodiscard]] bool takeUndoRequest()
    {
        const bool requested = m_undoRequested;
        m_undoRequested = false;
        return requested;
    }
    [[nodiscard]] bool takeRedoRequest()
    {
        const bool requested = m_redoRequested;
        m_redoRequested = false;
        return requested;
    }

    [[nodiscard]] bool canUndo() const { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const { return !m_redo.empty(); }
    // What the next `Ctrl+Z` would take back, for a button's tooltip or a
    // disabled state. Empty when there is nothing.
    [[nodiscard]] const std::string& undoLabel() const
    {
        return m_undo.empty() ? emptyLabel() : m_undo.back().label;
    }
    [[nodiscard]] const std::string& redoLabel() const
    {
        return m_redo.empty() ? emptyLabel() : m_redo.back().label;
    }
    [[nodiscard]] std::size_t undoDepth() const { return m_undo.size(); }
    [[nodiscard]] std::size_t redoDepth() const { return m_redo.size(); }

private:
    static const std::string& emptyLabel()
    {
        static const std::string empty;
        return empty;
    }

    static void eraseAll(std::deque<Entry>& entries, Editor editor)
    {
        for (auto it = entries.begin(); it != entries.end();) {
            it = it->editor == editor ? entries.erase(it) : it + 1;
        }
    }

    template <typename Apply>
    [[nodiscard]] static bool step(std::deque<Entry>& from, std::deque<Entry>& to, Apply&& apply,
                                   std::string* label)
    {
        if (from.empty()) {
            return false;
        }
        Entry entry = from.back();
        if (!apply(entry.editor)) {
            // The order named an editor that could not take the step, which
            // means the two have come apart. Leave the history alone rather
            // than compounding it, and let the caller say so.
            return false;
        }
        if (label != nullptr) {
            *label = entry.label;
        }
        from.pop_back();
        to.push_back(std::move(entry));
        return true;
    }

    std::deque<Entry> m_undo;
    std::deque<Entry> m_redo;
    bool m_redoDiscarded = false;
    bool m_undoRequested = false;
    bool m_redoRequested = false;
};

} // namespace forge
