#pragma once

// What the Forge can open, and what it says about it (engine plan Phase 9
// stage C). The numbers here are the ones a `[[model]]` row and a collision
// sphere are built from, which is why the viewer reports them beside the
// picture rather than leaving them to be guessed from it.

#include "sol/assets/asset_loader.hpp"
#include "sol/assets/data_defs.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/assets/texture_doc.hpp"
#include "sol/core/math/vec.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sol::assets {
struct ForgeDoc;
}

namespace forge {

// One openable file on disk. `label` is what the list shows; the extension is
// what decides which loader runs; `stem` is what a `[[model]]` row names, which
// is how a mesh on disk is tied to the content that uses it.
//
// ⚑⚑ `group` WAS A SUFFIX ON `label` UNTIL SESSION 14 TURNED IT INTO A FIELD,
// AND THE POINT IS THAT THE GROUPING ALREADY EXISTED - it was just spelled in a
// way only a human reading the row could use. `listMeshes` has always emitted
// its categories in contiguous runs, so a list can draw a header per run
// without sorting, indexing or a second pass. Promoting the suffix costs
// nothing because it was never the thing distinguishing two rows: the file
// EXTENSION already does that (`checker.tex` against `checker.stex`), which is
// why the tag can leave the label without any caller losing information.
struct AssetEntry
{
    std::string label;
    std::string path;
    std::string stem;
    // Display name of the run this entry belongs to. Rows are CONTIGUOUS by
    // group, which is the property the list draw depends on.
    std::string group;
    // Build output: openable so it can be looked at, never editable, and
    // collapsed by default because it is the majority of the list and the
    // minority of the interest. ⚑ It is also the half that grows on its own -
    // stage F emits a `.lodN.smesh` sibling per level, so a chained model adds
    // up to two rows here with no author action at all.
    bool cooked = false;
};

// Authored `.gltf` under the source tree first, then cooked `.smesh` beside
// the executable - in that order because the authored file is the one an
// author has just changed, and the cooked one is a build behind it.
[[nodiscard]] std::vector<AssetEntry> listMeshes(const std::string& sourceDirectory,
                                                 const std::string& cookedDirectory);

// Authored `.tex` under the source tree first, then cooked `.stex` beside the
// executable - the same order as listMeshes and for the same reason. A mesh does
// not name its texture (that is the `[[model]]` row's job, and stage H's), so
// the viewer lets one be picked.
[[nodiscard]] std::vector<AssetEntry> listTextures(const std::string& sourceDirectory,
                                                   const std::string& cookedDirectory);

// Authored `.wav`/`.ogg` under the source tree first, then cooked `.saud`
// beside the executable - the third listing of one shape, and the last asset
// kind this tool could not see at all (Phase 24 stage U1).
//
// ⚑ THE THIRD KIND IS THE ONE THAT MAKES THE PATTERN A RULE RATHER THAN A
// COINCIDENCE, so this deliberately adds no new idea: same argument order, same
// group names, same contiguity, and therefore the same `drawAssetList` with no
// widget written for it.
[[nodiscard]] std::vector<AssetEntry> listSounds(const std::string& sourceDirectory,
                                                 const std::string& cookedDirectory);

// A `.forge` part tree: the only kind of MESH this tool can edit rather than
// just open.
[[nodiscard]] bool isPartSource(const AssetEntry& entry);

// A `.tex` document: likewise, the only kind of texture it can edit.
[[nodiscard]] bool isTextureSource(const AssetEntry& entry);

// ⚑ A `.wav`/`.ogg`, i.e. the half of the sound list this tool did not build.
// There is no sound EDITOR and this stage does not invent one - the predicate
// exists so the panel can say which of two audible things you are hearing.
[[nodiscard]] bool isSoundSource(const AssetEntry& entry);

// Dispatches on extension: `.tex` is parsed, evaluated and encoded exactly as
// the cooker would, `.stex` goes through the runtime loader.
//
// ⚑ The encode matters and is not an optimisation: an author looking at a
// texture in this tool is looking at BC1 with a mip chain, which is what the
// game uploads. Showing the RGBA the document built would make the tool prettier
// than the game and hide every artefact the compression introduces - the same
// mistake as previewing a mesh at a distance no player ever sees it from.
[[nodiscard]] bool
loadTexture(const AssetEntry& entry, sol::assets::TextureData& out, std::string* error = nullptr);

// The same evaluation and encode from a document already in memory: what the
// editor calls after every accepted edit, so the hull in the viewport changes
// while the panel is still open.
[[nodiscard]] bool buildTextureData(const sol::assets::TextureDoc& doc,
                                    sol::assets::TextureData& out,
                                    std::string* error = nullptr);

// Dispatches on extension: `.forge` is parsed and evaluated, `.gltf`/`.glb` go
// through the cooker's importer, `.smesh` through the runtime loader. All three
// give the same buffer the game would draw, which is the point.
[[nodiscard]] bool loadMesh(const AssetEntry& entry, sol::assets::MeshData& out);

// --- sounds (stage U1) -------------------------------------------------------

// Dispatches on extension: `.wav` and `.ogg` go through the cooker's own
// importers, `.saud` through the runtime loader.
//
// ⚑⚑ AND THE PREVIEW IS THE COOKED PAYLOAD RATHER THAN A SECOND READING OF THE
// FILE, WHICH IS `loadTexture`'s BC1 ARGUMENT ARRIVING AT A SECOND FORMAT - but
// here it costs nothing to be exact instead of merely close. `SoundData` IS what
// a `.saud` holds: `encodeSound` serialises this struct and `assets::loadSound`
// reads it back, so importing the wav gives sample-for-sample what the cook
// would have written. An author auditioning a `.wav` before it is cooked is
// hearing the cooked cue, and the pair in the list can be played against each
// other to say so out loud.
[[nodiscard]] bool
loadSound(const AssetEntry& entry, sol::assets::SoundData& out, std::string* error = nullptr);

// Everything the panel prints about a sound.
//
// ⚑ `peak` is here because it is the number `gain` is a number ABOUT. A cue
// authored at 0.3 and one clipping at 1.0 want opposite edits, and the row
// beside them cannot say which is which - the same reason the mesh report
// prints a measured radius next to the authored one.
struct SoundReport
{
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint32_t frames = 0;
    float seconds = 0.0f;
    float peak = 0.0f; // 0..1, the loudest sample as a fraction of full scale
};

[[nodiscard]] SoundReport reportSound(const sol::assets::SoundData& data);

// --- the Blender bridge (stage L) -------------------------------------------
//
// ⚑⚑ WHY A glTF BECOMES A `.forge` RATHER THAN STAYING A glTF. The Forge can
// already OPEN a `.gltf` and the cooker can already COOK one, so a Blender
// export is a shippable asset today with no code at all - but `isPartSource`
// admits only `.forge`, so it arrives read-only, and every tool this programme
// built for stages E through I is unavailable on it. Converting is what makes
// Blender a front end to this tool rather than a second pipeline beside it.
//
// ⚑⚑ AND WHY THE glTF MUST NOT LIVE UNDER `assets/`. The cooker walks the
// source tree with a RECURSIVE `listFiles` into ONE FLAT output directory and
// keys outputs on the file STEM, so `ship.gltf` and `ship.forge` both cook to
// `ship.smesh` - and the collision guard does not skip that pair, it aborts the
// entire cook ("nothing cooked", exit 1). An inbox anywhere under `assets/`
// would therefore break the whole build the moment its first import succeeded.
// The drop directory is outside it, and the glTF is TRANSPORT: once converted
// the `.forge` is the source, which is what `gltf.hpp` already argued when it
// called export an interop action rather than a pipeline step.

// A part id derived from a Blender object name: everything outside
// [A-Za-z0-9_] becomes an underscore, runs collapse, and an empty result
// becomes "part". `Hull.001` and `Wing L` are the cases that matter.
[[nodiscard]] std::string forgePartIdFromName(const std::string& name);

// What an import did to a part tree, so the tool can say it rather than leaving
// the author to diff the file.
struct ImportOutcome
{
    std::vector<std::string> added;
    std::vector<std::string> replaced;
    // Parts whose object was RENAMED in Blender: {was, is}. They are neither
    // added nor replaced - the geometry was replaced like any other match, but
    // the part answers to a different name afterwards, and an author who is not
    // told that will read it as one part vanishing and another appearing.
    std::vector<std::pair<std::string, std::string>> renamed;
    // Parts already in the document that this glTF did not name. ⚑ LEFT ALONE,
    // deliberately: an author who added a part in the Forge should not lose it
    // because they re-exported from Blender, and deleting someone's work is a
    // worse failure than leaving a part they must remove by hand.
    std::vector<std::string> kept;
};

// Brings every mesh-bearing node of `gltfPath` into `doc` as a literal `mesh`
// part - the same representation the `bake` button produces, which is why every
// stage E-I tool works on the result without knowing where it came from.
//
// ⚑ PARTS ARE MATCHED BY ORIGIN FIRST AND BY ID ONLY AS A FALLBACK (stage P),
// and a match is replaced WHOLE: geometry and placement both, keeping only the
// `parent` and the comment trivia above it. Blender is authoritative for the
// shape and the position of a part that came from Blender; a placement kept
// across a re-import would be applied on top of a transform already baked into
// the vertices, which is a silent double transform rather than a preserved
// intent. The NAME is Blender's on the same argument, so a matched part is
// renamed to follow its object and every child naming it as `parent` follows.
//
// ⚑⚑ THE FALLBACK IS THE MIGRATION PATH AND IT IS DELIBERATELY NARROW: a name
// matches only a part carrying NO origin. Once a part knows which object it
// came from, that outranks the name in both directions - it finds the part
// whose object was renamed, and it refuses to hand an object a part that
// belongs to a different one, which is what renaming ONTO another object's old
// name would otherwise do.
[[nodiscard]] bool importGltfIntoDoc(const std::string& gltfPath,
                                     sol::assets::ForgeDoc& doc,
                                     ImportOutcome& outcome,
                                     std::string* error = nullptr);

// --- the inbox lifecycle (stage R) -------------------------------------------
//
// ⚑⚑⚑ THE WHOLE STAGE IS ONE MISSING FACT: WHICH DROPS ARE DONE. Before this,
// a drop that had been imported was byte-for-byte indistinguishable from one
// that had not, and the only record was an in-memory list that died with the
// process. So the tool guessed, and guessed differently in two places: at
// launch it assumed everything present was already done (a drop made while the
// Forge was shut never imported, ever), and `Import now` assumed nothing was
// (one click re-imported every stale drop the directory had accumulated).
// Filing an imported drop under `imported/` makes "done" a fact on disk, and
// the two guesses collapse into one rule: what is in the inbox is pending.
//
// ⚑⚑⚑ AND THE TRAP THIS MUST NOT WALK INTO IS THE ONE THAT PUT THE INBOX
// OUTSIDE `assets/` IN THE FIRST PLACE: `platform::listFiles` IS RECURSIVE. It
// returns `blender-inbox/imported/Hull.glb` for a listing of `blender-inbox`,
// so an archive inside the watched directory re-imports on the next poll and
// re-files itself forever - a loop rather than a duplicate. That is what
// `forgeIsPendingDrop` exists to prevent, and why it is tested rather than
// commented.

// Where an imported drop is filed: a subdirectory of the inbox, so the whole
// lifecycle is one folder an author can open in a file manager. No trailing
// separator.
[[nodiscard]] std::string forgeInboxArchive(const std::string& inboxDirectory);

// Whether a path from `listFiles(inboxDirectory)` is a drop still waiting to be
// imported: a `.gltf`/`.glb` that is not already filed under the archive.
[[nodiscard]] bool forgeIsPendingDrop(const std::string& path, const std::string& inboxDirectory);

// Every pending drop in a `listFiles` result, in a stable order so a drain of
// several reports the same way twice.
[[nodiscard]] std::vector<std::string> forgePendingDrops(std::vector<std::string> listed,
                                                         const std::string& inboxDirectory);

// Where `dropPath` goes once it has been imported. Keeps the file name, so a
// re-send of the same object replaces its own previous archive rather than
// accumulating copies.
[[nodiscard]] std::string forgeArchivedDropPath(const std::string& dropPath,
                                                const std::string& inboxDirectory);

// Everything the viewer prints about a mesh.
//
// ⚑ `renderVertices` and `positions` are different numbers and the gap between
// them is information, not noise: a corner carries a normal and a uv, so a
// hard edge splits one point into several corners. A cube is 8 positions under
// 24 corners; a smooth revolve is nearly 1:1.
struct MeshReport
{
    std::uint32_t renderVertices = 0;
    std::uint32_t positions = 0;
    std::uint32_t triangles = 0;
    sol::core::Vec3 boundsMin{};
    sol::core::Vec3 boundsMax{};
    // The radius a `[[model]]` row would carry: the furthest point from the
    // origin the mesh is authored around.
    float boundingRadius = 0.0f;
    double surfaceArea = 0.0;
    double signedVolume = 0.0;
    bool manifold = false;
    bool closed = false;
    std::uint32_t borderEdges = 0;
    float cacheMissRatio = 0.0f;
};

[[nodiscard]] MeshReport reportMesh(const sol::assets::MeshData& data);

// A `[[model]]` row that names the open mesh, with the authored numbers set
// beside the measured ones.
//
// ⚑ This exists because stage C had to find two of these by EYE. The asteroid's
// mesh measures 1.1584 m against an authored 1.0 and the station's 102.0 against
// 100.0, so both collision spheres sit inside their own hulls - true since those
// assets were written, recorded nowhere, and noticed only when a human finally
// looked at the numbers side by side. A viewer that can measure a radius and
// cannot read the authored one is asking to have that happen again.
struct ModelMatch
{
    std::string id;
    std::string texture;
    float authoredRadius = 0.0f;
    // 0 in the row means "the same as radius"; resolved here so the panel does
    // not have to know that rule.
    float authoredAvoidRadius = 0.0f;
    float emissive = 0.0f;
    bool solid = true;
    // measured - authored, in metres. POSITIVE means the sphere the sim builds
    // is smaller than the hull that is drawn: ships pass through the picture.
    float radiusDelta = 0.0f;

    // ⚑ Agreement is RELATIVE, not absolute, and the gate is why: it authors
    // 106.7 against a measured 106.701, which is somebody rounding a number
    // they read off this very panel. A millimetre on a 107 m ring is not a
    // finding; the same millimetre on a 0.6 m part might be. One tenth of one
    // percent separates the four real mismatches in this game (2%, 12%, 13%,
    // 16%) from that one rounding by three orders of magnitude.
    [[nodiscard]] bool radiusAgrees() const;
    // Signed, as a percentage of the authored radius.
    [[nodiscard]] float radiusDeltaPercent() const;
};

// Reads the `[[model]]` rows out of the game's data directory.
//
// ⚑ The tool reads the game's DATA and never its CODE, which is the line
// AGENTS.md 4 draws; `sol::assets::DefDatabase` is engine, and models.toml is a
// file. A missing directory is not an error - an installed tool without the
// source tree beside it simply reports no authored rows.
[[nodiscard]] bool
loadModelCatalog(const std::string& dataDirectory, sol::assets::DefDatabase& out, std::string* error);

// Every row whose `mesh` stem is this asset's, which is usually one and is
// deliberately allowed to be several: six models already share five meshes.
[[nodiscard]] std::vector<ModelMatch>
matchModels(const sol::assets::DefDatabase& defs, const AssetEntry& entry, const MeshReport& report);

// A `[[ship]]` or `[[station]]` row naming a `[[model]]` that does not exist.
//
// ⚑ THE CHECK THE STRICT SCHEMA DOES NOT DO, and stage H is what makes it
// matter. `parseShip` reads `model` with `optionalString` and never resolves it,
// so a typo LOADS CLEANLY and surfaces at spawn as a log warning behind a
// fallback that draws something plausible - a freighter wearing a shuttle. It
// is a cross-def question, which is the layer `validateFactions` already
// occupies, and it belongs here rather than in `data_defs.cpp` for the reason
// that file states: a mod layer may remove a def another layer still names.
//
// Headless on purpose. The tool's panel prevents the mistake by offering a
// combo, but a rule enforced only by a widget is a rule with no test.
struct MissingModelRef
{
    std::string defType; // "ship" | "station"
    std::string defId;
    std::string model; // the name that resolves to nothing
};

[[nodiscard]] std::vector<MissingModelRef> missingModelRefs(const sol::assets::DefDatabase& defs);

// --- the texture preview's geometry (stage I) --------------------------------
//
// ⚑ Here rather than in `texture_editor.cpp` for the reason stage H finally
// applied BEFORE promising a test rather than after: that file pulls in ImGui
// and cannot be in a suite. A rule enforced only by a widget is a rule with no
// test, and both rules below are exactly the kind that fail by one.

// Screen pixels per texture pixel, always a whole number and never less than 1.
//
// ⚑⚑ THE INTEGER IS THE WHOLE POINT AND IT IS WHAT MADE THIS STAGE POSSIBLE AT
// ALL. The preview shipped at 200 px for a 256 px document, which puts 1.28
// texture pixels under every screen pixel - so a drag could only produce offsets
// of round(n * 1.28), and 56 of the 257 possible offsets could not be produced
// AT ALL. The first one missing is 2: a drag went 0, 1, 3. Every value in a
// texture document is an exact integer, and a fractional scale is precisely what
// makes that untrue.
[[nodiscard]] int texturePreviewScale(int textureWidth, float availableWidth);

// The texture pixel under a cursor, given the preview's top-left corner. False
// when the cursor is outside the image.
[[nodiscard]] bool texturePixelAt(
    sol::core::Vec2 cursor, sol::core::Vec2 origin, int scale, int width, int height, int& x, int& y);

// How far a gesture has travelled, in whole texture pixels.
//
// ⚑⚑ ABSOLUTE BY CONSTRUCTION: round(total), NEVER a sum of rounded per-frame
// deltas. `PointTool` drags on a per-frame `cursorDelta` and that is right for a
// mesh authored in double; here every write is an integer, and the two are not
// the same arithmetic. Three frames of 0.4 px round to zero apiece while the
// hand moved 1.2, so an accumulating drag sticks and then jumps a whole pixel
// at once. The caller keeps the cursor position the gesture STARTED at, which
// is what makes the wrong version inexpressible rather than merely discouraged.
[[nodiscard]] int textureDragOffset(float startCursor, float cursor, int scale);

} // namespace forge
