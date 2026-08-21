#pragma once

// The def editor (engine plan Phase 9 stage H) - what turns the Forge from
// something that REPORTS a `[[model]]` row into something that writes one.
//
// ⚑ It is the last wall in the programme. Stage A made a model a def row rather
// than a C++ enum member, and stage C taught the tool to READ those rows - which
// is how the four radius mismatches in this game came to be printed in an orange
// warning that an author then had to go and fix in a text editor. Everything
// else the Forge does ends in a file it wrote; this one ended in a sentence.
//
// It owns the def DOCUMENTS (`models.toml`, and at H3 `ships.toml`) rather than
// a parsed database, because what an author needs preserved is the half a typed
// parse throws away - see `sol/assets/def_doc.hpp` for why values are text.
//
// ⚑ VALIDATION IS THE GAME'S OWN SCHEMA AND NOT A SECOND ONE. After every edit
// the document is serialised and handed to `DefDatabase::mergeToml`, which is
// the same call the game makes at boot: strict about unknown keys, typed, and
// range-checked. So the panel shows what the game WOULD load, a refusal is the
// schema's own message, and the tool cannot write a file the game rejects. The
// one thing that check does not do is resolve a cross-reference - a `[[ship]]`
// naming a model that does not exist parses clean - which is H3's problem and
// is why the model combo offers names rather than a text box.

#include "mesh_library.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/assets/def_doc.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace forge {

class DefEditor
{
public:
    // Reads the def documents out of the game's data directory. A missing
    // directory is not an error - an installed tool without the source tree
    // beside it simply cannot edit, and says so - which is the same rule
    // `loadModelCatalog` already follows for reading.
    void load(const std::string& dataDirectory);

    // Draws the `[[model]]` rows naming this mesh, with the measured report
    // beside the authored numbers. Returns true when the document changed.
    [[nodiscard]] bool drawModelRows(const AssetEntry& entry, const MeshReport& report,
                                     const std::vector<std::string>& textureStems);

    // Writes every dirty document back. Validated first, so a refusal leaves
    // the files exactly as they were.
    [[nodiscard]] bool save(std::string& status);

    // The validated view of the documents as they stand, which is what the
    // panel measures against. Re-derived on every accepted edit, so it is the
    // game's reading of the text an author is looking at rather than a second
    // copy that can drift from it.
    [[nodiscard]] const sol::assets::DefDatabase& defs() const { return m_defs; }
    [[nodiscard]] bool loaded() const { return m_loaded; }
    [[nodiscard]] bool dirty() const { return m_modelsDirty; }
    [[nodiscard]] const std::string& error() const { return m_error; }

    // Undo is a copy of the document, per E1's precedent and G's: a `DefDoc` is
    // a plain value and the largest def file in this game is 139 lines.
    void beginEdit();
    [[nodiscard]] bool undo();
    // ⚑ G2d's rule, and the reason it is a separate call: a DragFloat reports an
    // edit on every frame the mouse moves, so pushing undo from the write-back
    // path turns one drag into twenty-four entries. This pushes on the frame the
    // LAST-SUBMITTED widget became active, so it must be called immediately
    // after the widget it is about.
    void noteActivation();

private:
    // Serialises, validates through the game's schema, and keeps the result.
    // Returns false with `m_error` set, leaving the last good database alone.
    [[nodiscard]] bool revalidate();

    static constexpr std::size_t kUndoDepth = 64;

    sol::assets::DefDoc m_models;
    std::vector<sol::assets::DefDoc> m_undo;
    sol::assets::DefDatabase m_defs;
    std::string m_dataDirectory;
    std::string m_error;
    bool m_loaded = false;
    bool m_modelsDirty = false;
};

} // namespace forge
