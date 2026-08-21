#pragma once

// The def editor (engine plan Phase 9 stage H) - what turns the Forge from
// something that REPORTS a `[[model]]` row into something that writes one, and
// then into something that can put an authored mesh in the game.
//
// ⚑ It is the last wall in the programme. Stage A made a model a def row rather
// than a C++ enum member, and stage C taught the tool to READ those rows - which
// is how the four radius mismatches in this game came to be printed in an orange
// warning that an author then had to go and fix in a text editor. Everything
// else the Forge does ends in a file it wrote; this one ended in a sentence.
//
// It owns the def DOCUMENTS (`models.toml`, `ships.toml`, `stations.toml`)
// rather than a parsed database, because what an author needs preserved is the
// half a typed parse throws away - see `sol/assets/def_doc.hpp` for why values
// are text and what breaks when they are not.
//
// ⚑ VALIDATION IS THE GAME'S OWN SCHEMA AND NOT A SECOND ONE. After every edit
// the document is serialised and handed to `DefDatabase::mergeToml`, which is
// the same call the game makes at boot: strict about unknown keys, typed, and
// range-checked. So the panel shows what the game WOULD load, a refusal is the
// schema's own message, and the tool cannot write a file the game rejects.
//
// ⚑ THE ONE THING THAT CHECK DOES NOT DO IS RESOLVE A CROSS-REFERENCE. A
// `[[ship]]` naming a model that does not exist parses CLEAN - `parseShip` reads
// `model` with `optionalString` and never looks it up - and the failure surfaces
// at spawn as a log warning behind a fallback that draws something plausible.
// Since creating that reference is this stage's entire purpose, the tool owes
// the check the schema does not do: `model` is offered as a COMBO over the rows
// that exist, so the mistake is not expressible, and a row already on disk that
// names a missing model is reported.

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
    // beside the authored numbers. Returns true when a document changed.
    [[nodiscard]] bool drawModelRows(const AssetEntry& entry, const MeshReport& report,
                                     const std::vector<std::string>& textureStems);

    // Draws the `[[ship]]` and `[[station]]` rows that name any of those models
    // - the content that actually puts the mesh in front of a player - and can
    // make a new one. ⚑ Only the keys that are about the ASSET: id, name, model,
    // scale. Flight tuning, defence, price and slots stay in a text editor,
    // where the strict schema already makes them safe; a generated 33-key panel
    // would answer "author a whole ship" literally and duplicate a surface that
    // works, with no complaint behind it.
    [[nodiscard]] bool drawContentRows();

    // Writes every dirty document back. Validated first, so a refusal leaves
    // the files exactly as they were.
    [[nodiscard]] bool save(std::string& status);

    [[nodiscard]] bool loaded() const { return m_loaded; }
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] const std::string& error() const { return m_error; }

    // Undo is a copy of the document, per E1's precedent and G's: a `DefDoc` is
    // a plain value and the largest def file in this game is 139 lines.
    void beginEdit(std::size_t document);
    [[nodiscard]] bool undo();
    // ⚑ G2d's rule, and the reason it is a separate call: a DragFloat reports an
    // edit on every frame the mouse moves, so pushing undo from the write-back
    // path turns one drag into twenty-four entries. This pushes on the frame the
    // LAST-SUBMITTED widget became active, so it must be called immediately
    // after the widget it is about.
    void noteActivation(std::size_t document);

private:
    // One def file, as text and as the game's reading of it.
    struct Document
    {
        const char* file = nullptr;
        sol::assets::DefDoc doc;
        bool dirty = false;
        bool ok = false;
    };

    struct UndoEntry
    {
        std::size_t document = 0;
        sol::assets::DefDoc doc;
    };

    // Indices into m_docs, fixed so the undo stack can name one.
    static constexpr std::size_t kModels = 0;
    static constexpr std::size_t kShips = 1;
    static constexpr std::size_t kStations = 2;
    static constexpr std::size_t kDocumentCount = 3;
    static constexpr std::size_t kUndoDepth = 64;

    // Serialises every document, validates each through the game's schema, and
    // keeps the merged result. False with `m_error` set on the first refusal.
    [[nodiscard]] bool revalidate();
    // The `[[model]]` ids that exist, for the combos and the cross-check.
    [[nodiscard]] std::vector<std::string> modelIds() const;
    // One `[[ship]]`/`[[station]]` row's asset keys. Returns true when changed.
    [[nodiscard]] bool drawContentRow(std::size_t document, sol::assets::DefRow& row,
                                      const std::vector<std::string>& models);

    Document m_docs[kDocumentCount];
    std::vector<UndoEntry> m_undo;
    sol::assets::DefDatabase m_defs;
    // The model ids on the mesh currently open, which is what decides whether a
    // content row is worth showing. Set by drawModelRows, read by drawContentRows.
    std::vector<std::string> m_openModels;
    std::string m_dataDirectory;
    std::string m_error;
    bool m_loaded = false;
};

} // namespace forge
