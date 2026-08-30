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
// It owns the def DOCUMENTS (`models.toml`, `ships.toml`, `stations.toml`, and
// since Phase 24 stage U1 `sounds.toml`) rather than a parsed database, because
// what an author needs preserved is the half a typed parse throws away - see
// `sol/assets/def_doc.hpp` for why values are text and what breaks when they
// are not.
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

#include "edit_history.hpp"
#include "mesh_library.hpp"
#include "mount_rows.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/assets/def_doc.hpp"

#include <cstddef>
#include <cstdint>
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

    // ⚑⚑⚑ WHICH MESH IS OPEN, SET ONCE A FRAME BY THE CALLER AND OWNED BY NO
    // PANEL. It decides the `[[model]]` rows the panels show and, through them,
    // the material the viewport draws with - and it must be true whether or not
    // any particular window is submitted. See the implementation for the defect
    // that made this a separate call: the Material panel docked in front of the
    // Report panel, ImGui stopped submitting Report, and a hidden window cannot
    // update anything.
    void setOpenMesh(const std::string& stem);

    // Draws the `[[model]]` rows naming this mesh, with the measured report
    // beside the authored numbers. Returns true when a document changed.
    [[nodiscard]] bool drawModelRows(const AssetEntry& entry,
                                     const MeshReport& report,
                                     const std::vector<std::string>& textureStems);

    // Draws the `[[sound]]` rows whose `asset` is this file's stem, and offers
    // to make one when nothing names it (Phase 24 stage U1) - the same shape
    // `drawModelRows` has for a mesh, because it is the same gap: an asset on
    // disk that no def gives a name to cannot be fired by anything.
    //
    // ⚑ `asset` IS THE MATCH KEY, so it is shown and not edited. Every other
    // panel in this tool offers a combo where a name could be mistyped; here
    // the combo would make the row being edited disappear from under the hand
    // the moment it was used, which is a worse failure than the typo. A cue
    // that should point somewhere else is a cue with the wrong file open.
    //
    // ⚑ `audition` is how the panel asks to be HEARD without this editor
    // knowing what audio is: it says what to play at what gain, and the caller
    // owns the device. Keeping `sol::audio` out of the def editor is the same
    // line `drawAssetList` keeps ImGui out of `mesh_library.cpp` for.
    struct Audition
    {
        bool wanted = false;
        float gain = 1.0f;
        float pitchJitter = 0.0f;
        std::uint32_t maxInstances = 0;
    };

    [[nodiscard]] bool drawSoundRows(const AssetEntry& entry, Audition& audition);

    // ⚑⚑⚑ PHASE 25 STAGE D: THE SURFACE OF WHATEVER IS OPEN, EDITED IN PLACE.
    // Draws the `[[material]]` row the open mesh's model row NAMES - not a row
    // the author picked off a list - so the viewport can never show a surface
    // the game would not. `drawModelRows` decides which one that is, so this
    // must be called after it.
    //
    // ⚑⚑ THE COMMON CASE IS A MATERIAL WITH NO ROW, AND SAYING SO IS HALF THE
    // PANEL. Five of the eight shipped models name no material, so the game
    // derives one - `sol.auto.<model id>`, rebuilt from scratch after every
    // merge - which exists only in memory and has no file to edit. That is
    // where the `[[material]]` row this stage writes comes from: the button
    // moves the four surface keys off the model row and onto a real material,
    // in ONE undo entry, because half of that move is a model naming a material
    // that does not exist.
    enum class MaterialEdit
    {
        None,
        // A declared number moved. The registry rewrites one mapped buffer and
        // touches no pipeline.
        Params,
        // Anything a pipeline or a descriptor set is BUILT from - a shader
        // stem, a state flag, a slot's texture, or a whole new row. Needs an
        // idle device, so the caller raises it rather than serving it here.
        Structure,
    };

    // ⚑⚑ `shownTexture` AND `outShowTexture` ARE HOW THIS PANEL AND THE TEXTURE
    // LIST STAY ONE ANSWER TO "WHICH SURFACE AM I LOOKING AT". Stage G made
    // selecting a texture shade the open mesh, and that is still what a list
    // click does - it is how an author checks a UV against a checker without
    // touching any def. But a material OWNS the texture the game will use, so
    // changing it here has to move the picture too, or the exit criterion of
    // this stage is false. So: the combo below reports the stem it wrote
    // through `outShowTexture`, the caller selects it, and whenever the two
    // disagree the panel SAYS SO rather than leaving a viewport that quietly
    // does not match the file.
    [[nodiscard]] MaterialEdit drawMaterialRows(const std::vector<std::string>& textureStems,
                                                const std::vector<std::string>& vertexStems,
                                                const std::vector<std::string>& fragmentStems,
                                                const std::string& shownTexture,
                                                std::string* outShowTexture);

    // The materials as the GAME resolves them: the authored rows plus the ones
    // derived from `[[model]]` rows that name none, in the order the renderer
    // indexes them. Empty until `load` succeeds.
    [[nodiscard]] const std::vector<sol::assets::MaterialDef>& materials() const
    {
        return m_defs.materials();
    }

    // Which of those the open mesh is drawn with, or `kNoMaterial`.
    static constexpr std::uint32_t kNoMaterial = 0xFFFFFFFFu;

    [[nodiscard]] std::uint32_t openMaterialIndex() const { return m_openMaterial; }

    // Draws the `[[ship]]` and `[[station]]` rows that name any of those models
    // - the content that actually puts the mesh in front of a player - and can
    // make a new one. ⚑ Only the keys that are about the ASSET: id, name, model,
    // scale. Flight tuning, defence, price and slots stay in a text editor,
    // where the strict schema already makes them safe; a generated 33-key panel
    // would answer "author a whole ship" literally and duplicate a surface that
    // works, with no complaint behind it.
    [[nodiscard]] bool drawContentRows();

    // ⚑⚑⚑ PHASE 31 STAGE D: THE MOUNT SURFACE. Everything above draws a panel;
    // these do not, and that is the point. A mount is placed by clicking a hull
    // in the VIEWPORT, so the gesture belongs to `MountTool` and only the
    // document mechanics belong here - which keeps the one thing that must not
    // be duplicated in one place: the write goes through the same `DefDoc`,
    // the same undo stack and the same `DefDatabase::mergeToml` validation as
    // every other edit this tool makes.
    //
    // ⚑⚑ AND THEY VALIDATE BEFORE THEY MUTATE, WHICH THE PANELS ABOVE DO NOT.
    // A panel edit that the schema refuses leaves the bad value on screen for
    // the author to fix and `save` refuses until they do - fine for a number in
    // a field they are looking at. A mount edit is STRUCTURAL: a duplicate id
    // or an `aim` on an internal mount would leave a document nothing can save
    // and no widget showing the offending text. So the candidate is built,
    // handed to the schema, and only kept if it is accepted.

    // The `[[ship]]` ids that fly the open mesh's model, in file order. Empty
    // until `drawModelRows` has said which models those are.
    [[nodiscard]] std::vector<std::string> hullsOnOpenModel() const;

    // One hull as the GAME reads it - the validated database, not the text - so
    // the tool draws the mounts the game would load and never a second parse of
    // the same file. Null when no such hull exists.
    [[nodiscard]] const sol::assets::ShipDef* hull(const std::string& hullId) const;

    // What a mount of this kind can be given, as (id, name) pairs in file
    // order: the weapon catalog for a gun mount, the component catalog for
    // everything else. `mountTakesWeapon` decides, which is the same function
    // the sim uses to look the id up.
    struct Fitting
    {
        std::string id;
        std::string name;
        sol::assets::MountKind kind = sol::assets::MountKind::Utility;
        sol::assets::MountSize size = sol::assets::MountSize::Small;
    };

    [[nodiscard]] std::vector<Fitting> fittingsFor(sol::assets::MountKind kind) const;

    // Adds a mount to a hull. One undo entry; false with `error()` set and the
    // document untouched when the schema refuses.
    [[nodiscard]] bool addMount(const std::string& hullId, const MountDraft& draft);

    // Removes one. ⚑ The comment above the row moves DOWN to the next mount
    // rather than being deleted with it - see `DefDoc::eraseRow`.
    [[nodiscard]] bool removeMount(const std::string& hullId, const std::string& mountId);

    // Moves a mount's `at`. ⚑ NO UNDO ENTRY OF ITS OWN, because this is what a
    // viewport DRAG calls sixty times a second - the caller pushes one entry
    // when the gesture begins, exactly as `noteActivation` does for a slider.
    [[nodiscard]] bool
    setMountAt(const std::string& hullId, const std::string& mountId, const float (&at)[3]);

    // Sets or clears one key on one mount, by its authored text. One undo entry.
    // Clearing `at` is what makes a mount internal, which is why removing a key
    // is an operation rather than an omission.
    [[nodiscard]] bool setMountKey(const std::string& hullId,
                                   const std::string& mountId,
                                   const char* key,
                                   std::string_view value);
    [[nodiscard]] bool clearMountKey(const std::string& hullId, const std::string& mountId, const char* key);

    // Writes every dirty document back. Validated first, so a refusal leaves
    // the files exactly as they were.
    [[nodiscard]] bool save(std::string& status);

    [[nodiscard]] bool loaded() const { return m_loaded; }

    [[nodiscard]] bool dirty() const;

    [[nodiscard]] const std::string& error() const { return m_error; }

    // Undo is a copy of the document, per E1's precedent and G's: a `DefDoc` is
    // a plain value and the largest def file in this game is 139 lines.
    void beginEdit(std::string label);
    [[nodiscard]] bool undoStep();
    [[nodiscard]] bool redoStep();

    void clearRedo() { m_redo.clear(); }

    void setHistory(EditHistory* history) { m_history = history; }

    [[nodiscard]] std::size_t undoDepth() const { return m_undo.size(); }

    [[nodiscard]] std::size_t redoDepth() const { return m_redo.size(); }

    // ⚑ G2d's rule, and the reason it is a separate call: a DragFloat reports an
    // edit on every frame the mouse moves, so pushing undo from the write-back
    // path turns one drag into twenty-four entries. This pushes on the frame the
    // LAST-SUBMITTED widget became active, so it must be called immediately
    // after the widget it is about.
    void noteActivation(const char* label);
    // Throws this editor's snapshots away AND tells the history.
    void forgetHistory();

private:
    // One def file, as text and as the game's reading of it.
    struct Document
    {
        const char* file = nullptr;
        sol::assets::DefDoc doc;
        bool dirty = false;
        bool ok = false;
    };

    // Indices into m_docs. Fixed, because a `[[model]]` row and the material it
    // names live in different files and one gesture can touch both.
    static constexpr std::size_t kModels = 0;
    static constexpr std::size_t kShips = 1;
    static constexpr std::size_t kStations = 2;
    // ⚑ Stage U1. `sounds.toml` joins the same set rather than getting an
    // editor of its own, which is what buys it the undo stack, the dirty
    // tracking, the write-only-what-changed rule and - the one that matters -
    // `revalidate`, so a bad gain is refused by the GAME'S schema and not by a
    // second one written here.
    static constexpr std::size_t kSounds = 3;
    // ⚑⚑ STAGE 25-D. `materials.toml` joins the set for the same reason
    // `sounds.toml` did, and one more: a material is the ONLY def in this game
    // that another def file points AT. A `[[model]]` naming a material that no
    // row defines passes `mergeToml` clean - `validateMaterials` is a separate
    // call the game makes and this editor did not - so until this document was
    // here, the tool could write that dangling reference and not know.
    static constexpr std::size_t kMaterials = 4;
    // ⚑⚑ STAGE 31-D JOINED THE TWO FITTING CATALOGS, AND FOR THE REASON
    // `drawContentRow` ALREADY GIVES FOR `model`: a `[[ship.mount]]`'s `fit`
    // key is read with `optionalString` and never resolved, so a typo parses
    // clean and turns up at spawn as one log warning behind a mount that simply
    // comes bare. This stage exists to WRITE that key, so it owes the check the
    // schema does not do - and a combo over the ids that exist is what makes the
    // mistake unspellable.
    //
    // ⚑ Which of the two a mount draws from is `mountTakesWeapon`, the same
    // function that decides which table the GAME looks an id up in. One rule,
    // one place; the tool cannot offer a component for a gun mount because the
    // sim would not find it there either.
    static constexpr std::size_t kWeapons = 5;
    static constexpr std::size_t kComponents = 6;
    static constexpr std::size_t kDocumentCount = 7;
    static constexpr std::size_t kUndoDepth = 64;

    // ⚑⚑⚑ STAGE 25-D MADE THIS A SNAPSHOT OF EVERY DOCUMENT RATHER THAN OF
    // ONE, AND THE REASON IS A SINGLE BUTTON. Promoting a synthesised material
    // into a real `[[material]]` row writes TWO files at once: the row is
    // appended to `materials.toml` and, in the same gesture, `models.toml` gains
    // a `material` key and loses the four surface keys it can no longer carry.
    // Recorded as two entries, one Ctrl+Z would leave a model naming a material
    // that does not exist yet - an invalid halfway state that the panel would
    // then report as an author's mistake.
    //
    // ⚑ The dirty flags travel with it, which also fixes something that was
    // already slightly wrong: undoing back to the start used to leave every
    // document it had touched marked dirty, so `save defs` rewrote files whose
    // contents were identical to what was on disk.
    //
    // ⚑ Five documents copied instead of one, per edit. The largest def file in
    // this game is 139 lines; the whole set is under 20 KB.
    struct UndoEntry
    {
        sol::assets::DefDoc docs[kDocumentCount];
        bool dirty[kDocumentCount] = {};
    };

    [[nodiscard]] UndoEntry snapshot() const;
    void restore(UndoEntry& entry);

    // Serialises every document, validates each through the game's schema, and
    // keeps the merged result. False with `m_error` set on the first refusal.
    [[nodiscard]] bool revalidate();
    // The `[[model]]` ids that exist, for the combos and the cross-check.
    [[nodiscard]] std::vector<std::string> modelIds() const;
    // The `[[material]]` row with this id, or null when the id names one the
    // database DERIVED rather than one somebody wrote.
    [[nodiscard]] sol::assets::DefRow* materialRow(const std::string& id);
    // Moves a model row's surface onto a new `[[material]]` row. One undo entry
    // covering two documents; see `UndoEntry`.
    void promoteMaterial(sol::assets::DefRow& model);
    // One inline table's entries, edited. Returns true when one changed.
    [[nodiscard]] bool drawSlotTable(sol::assets::DefRow& row, const std::vector<std::string>& textureStems);
    [[nodiscard]] bool drawParamTable(sol::assets::DefRow& row);
    // One `[[ship]]`/`[[station]]` row's asset keys. Returns true when changed.
    [[nodiscard]] bool drawContentRow(sol::assets::DefRow& row, const std::vector<std::string>& models);

    // ⚑ Stage D. Validates a candidate `ships.toml` against the game's schema
    // ALONGSIDE the other four documents, and keeps it only if every one of
    // them still loads. `label` empty means "no undo entry" - the drag case.
    [[nodiscard]] bool commitShips(sol::assets::DefDoc&& candidate, const char* label);
    // The hull row for an id, in a document the caller is about to edit.
    [[nodiscard]] static std::size_t hullRow(const sol::assets::DefDoc& doc, const std::string& hullId);

    Document m_docs[kDocumentCount];
    std::vector<UndoEntry> m_undo;
    std::vector<UndoEntry> m_redo;
    EditHistory* m_history = nullptr;
    sol::assets::DefDatabase m_defs;
    // The model ids on the mesh currently open, which is what decides whether a
    // content row is worth showing. Set by drawModelRows, read by drawContentRows.
    std::vector<std::string> m_openModels;
    // The material the open mesh draws with, decided in `drawModelRows` and read
    // by `drawMaterialRows` and by the viewport.
    std::uint32_t m_openMaterial = kNoMaterial;
    std::string m_dataDirectory;
    std::string m_error;
    bool m_loaded = false;
};

} // namespace forge
