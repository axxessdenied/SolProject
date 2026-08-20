#pragma once

// The `.forge` source document (engine plan Phase 9 stage D): a tree of parts,
// each a primitive with named parameters and a placement, which rebuilds into a
// mesh through stage B. It is what an asset IS, as opposed to what an asset
// came out as - a `.gltf` is triangles and cannot be edited back into the shape
// that produced them.
//
// ⚑ The format is TOML, parsed by the parser this repo already has. That costs
// no new dependency (AGENTS 5), reuses the validation idiom `data_defs.cpp`
// established, and - the reason that actually decided it - keeps an authored
// asset DIFFABLE. Every mesh in this game up to now has been base64 embedded in
// a glTF, where a moved vertex is an unreviewable wall of changed characters.
//
// ⚑ The tree is a FLAT `[[part]]` list with `parent` naming another part by id,
// not nested TOML headers. Nesting is expressible and it is the wrong choice
// twice: unreadable at the 10-17 parts this game's assets actually have, and
// re-parenting rewrites an indentation block instead of one line - which throws
// away the only reason to have chosen a text format.
//
// ⚑ A part's parameters are the MeshBuilder call's parameters, one for one, and
// the placement is additive on top. A box therefore keeps its own `center` even
// though the transform could express it. That redundancy is deliberate: the
// alternative - stripping each primitive down to "shape only" and pushing
// position into the transform - makes the mapping from this file to stage B a
// translation rather than a transcription, and a transcription is the thing
// that can be proved exact against the meshes already shipping.

#include "sol/assets/asset_loader.hpp"
#include "sol/assets/mesh_build.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sol::assets {

enum class ForgePrimitive
{
    // Carries no geometry: a named frame that exists so its children can be
    // moved together. Without one, "the solar panel assembly" is not a thing a
    // file can say.
    Group,
    Box,
    Beam,
    Torus,
    FlatTriangle,
    Revolve,
    Extrude,
    // ⚑ Literal geometry: vertices and indices written out, with no recipe
    // behind them. This is THE BAKE, and the rule it implements was decided at
    // the D checkpoint - a hand edit bakes the parts it touched and leaves every
    // other part parametric, rather than freezing the whole document or keeping
    // an edit list keyed to vertex indices that a parameter change invalidates.
    //
    // It is also what carries geometry the parametric vocabulary cannot honestly
    // describe. The asteroid is the first: it is a noise-displaced icosphere
    // whose noise is keyed on PowerShell's uint32 multiply promoting to double
    // (see the port in the geometry suite), and a primitive that reproduced that
    // would put a bug-compatibility shim in the engine's permanent vocabulary
    // for every future asset to inherit.
    Mesh,
};

[[nodiscard]] const char* forgePrimitiveName(ForgePrimitive primitive);
[[nodiscard]] bool forgePrimitiveFromName(const char* name, ForgePrimitive& out);
// Every primitive, in the order an editor should offer them.
[[nodiscard]] std::span<const ForgePrimitive> forgePrimitives();

enum class ForgeParamKind
{
    Scalar,
    Integer,
    Boolean,
    Vec3,
    Uv,
    // A list of 2D points: a revolve's (radius, height) profile or an
    // extrude's outline.
    Profile,
    // A baked part's geometry: one array of eight numbers per vertex, position
    // then normal then uv. Eight-per-line rather than three parallel arrays so
    // that MOVING A VERTEX IS ONE CHANGED LINE, which is the only reason this
    // format is text.
    VertexList,
    // A baked part's triangles, as offsets into its own VertexList.
    IndexList,
};

// One corner of a baked part, in authoring precision.
struct ForgeVertex
{
    BuildPoint position{0, 0, 0};
    BuildPoint normal{0, 1, 0};
    BuildUv uv{};
};

// One parameter value. Which member is live is decided by the schema, never by
// the value - a parameter that has been read as the wrong kind is a parse
// error, not a silently defaulted zero.
struct ForgeValue
{
    double scalar = 0.0;
    BuildPoint vec{0, 0, 0};
    BuildUv uv{};
    std::vector<BuildProfilePoint> profile;
    std::vector<ForgeVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct ForgeParamSpec
{
    const char* name = "";
    ForgeParamKind kind = ForgeParamKind::Scalar;
    ForgeValue defaultValue;
};

// ⚑ The single source of truth for what a primitive takes. The parser
// validates against it, the writer omits anything still at its default, and the
// tool's editor panel is generated from it - so adding a primitive parameter is
// one table entry rather than three edits that can disagree.
[[nodiscard]] std::span<const ForgeParamSpec> forgeParams(ForgePrimitive primitive);

struct ForgePart
{
    std::string id;
    std::string parent; // empty: a root part
    ForgePrimitive primitive = ForgePrimitive::Group;

    // Placement in the parent's frame.
    //
    // ⚑ Rotation is stored in DEGREES, which is a deliberate exception to the
    // engine's radians-everywhere convention (math/scalar.hpp) and was forced
    // by measurement rather than taste: degrees -> radians -> degrees is lossy,
    // and 30 comes back as 29.999999999999996. Converting on the way in would
    // mean every open-and-save rewrote angles nobody touched, which breaks the
    // one discipline this repo's asset generation has always had - regenerating
    // must leave `git status` clean. The conversion happens at localTransform(),
    // once, at the point of use.
    BuildPoint position{0, 0, 0};
    BuildPoint rotationDegrees{0, 0, 0};
    BuildPoint scale{1, 1, 1};

    std::vector<std::pair<std::string, ForgeValue>> params;

    // ⚑ The comments and blank lines that stood above this part's `[[part]]`
    // line, kept VERBATIM and written back out in front of it. Empty on a part
    // the tool created, where the writer supplies the blank separator itself.
    //
    // It is trivia rather than "the comment block" because `cockpit.forge` puts
    // a blank line between its `# --- canopy frame ---` divider and the part it
    // introduces: storing only the comments would reproduce the words and lose
    // the spacing, and a writer that silently reformats a committed file is one
    // nobody will let near a committed file. Whatever was there comes back.
    std::string leading;

    [[nodiscard]] const ForgeValue* find(const char* name) const;
    // The authored value if present, the schema default otherwise.
    [[nodiscard]] ForgeValue value(const char* name) const;
    void set(const char* name, const ForgeValue& value);
    [[nodiscard]] BuildTransform localTransform() const;
};

// What happens to the assembled soup before it becomes a mesh. All default to
// off, and that matters: a document with no `[build]` table emits exactly what
// MeshBuilder produced, which is the only setting under which a `.forge`
// transcription can be proved byte-identical to an already-shipped mesh.
struct ForgeBuildOptions
{
    // Merge corners that agree on position, normal and uv. Purely a size win -
    // by construction it cannot change what is drawn.
    bool weld = false;
    // Reorder for the vertex cache. Implies the weld, since both need the
    // indexed representation.
    bool optimize = false;
    // Above zero, re-derive normals and let faces meeting at a shallower angle
    // share one. Below zero the primitives' own normals are kept.
    double smoothAngleDegrees = -1.0;
};

struct ForgeDoc
{
    std::string name;
    ForgeBuildOptions build;
    std::vector<ForgePart> parts;

    // ⚑ What stood above the first plain key, verbatim - which for every asset
    // in this game is the file's header comment, and those headers are where
    // most of what this project knows about its own meshes is written down. A
    // writer that regenerates a file from its values alone deletes them on the
    // first save: survivable while the tool only opens files, fatal once stage
    // E is saving on every accepted edit.
    //
    // Trivia above a `[[part]]` belongs to the PART, even when that part is the
    // first thing in the file, so a document that opens straight into a part
    // has no header. Splitting it the other way would make the writer emit both
    // this and its own separator, and the file would gain a blank line every
    // time it was saved.
    std::string header;
    // The same, above a `[build]` table. Separate because `[build]` sits
    // between the name and the parts and can carry its own note.
    std::string buildLeading;
    // Trailing comments and blank lines after the last part, which belong to no
    // element and would otherwise be the one kind of trivia still dropped.
    std::string trailer;

    // ⚑ True when the source carried a comment this model cannot place: one
    // after a value on the same line, or one inside a multi-line array's
    // brackets. Neither exists in this repo's six assets, and neither can be
    // attached without a per-key slot the document does not have - so the
    // parser flags it and the tool says so, because a known gap a person is
    // told about is a different thing from a silent loss.
    bool hasUnplaceableComments = false;

    [[nodiscard]] const ForgePart* find(const std::string& id) const;
    [[nodiscard]] std::size_t indexOf(const std::string& id) const; // npos when absent
    // A part id not already taken, derived from `base`.
    [[nodiscard]] std::string uniqueId(const std::string& base) const;
};

// Parses and validates: ids are present and unique, every `parent` resolves,
// the parent graph is acyclic, the primitive is known, and every parameter is
// a name the primitive's schema carries. On failure returns false and sets
// `error` to a message naming the source.
[[nodiscard]] bool parseForge(const char* text, std::size_t length, const char* sourceName,
                              ForgeDoc& out, std::string* error = nullptr);

// Serialises back to TOML. Round-trip stable: parsing the output gives an equal
// document. A parameter the source never mentioned stays unmentioned, so a file
// says only what it actually says - but one that was written down is written
// back even where it equals the schema default, because a person who typed the
// segment count is naming the knob and a tool that answers by deleting the line
// is not one they will trust with the file.
//
// ⚑ Stronger than that for a file that came from `parseForge`: the six assets
// in `assets/meshes/` come back BYTE FOR BYTE, comments and blank lines
// included, and `geometry.unit` asserts it against the committed files. That is
// the property stage E needs - a modeller saves on every accepted edit, so a
// writer that reformatted anything would rewrite the whole asset on the first
// nudge of a vertex and bury the change it actually made.
[[nodiscard]] std::string writeForge(const ForgeDoc& doc);

// Where one part's vertices landed in the mesh buildForge produced (engine plan
// Phase 9 stage E). Parts emit in file order into one builder, so a part's
// vertices are a contiguous run and the range is a bracket around the emission
// rather than a second traversal - which matters, because a second traversal
// that disagreed with the first is exactly the two-implementations trap this
// programme has now paid for twice.
struct ForgePartRange
{
    std::size_t part = 0; // into ForgeDoc::parts
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
};

// Evaluates the tree into a mesh. When `ranges` is given it is filled with one
// entry per geometry-carrying part, in emission order.
//
// ⚑ Parts are emitted in FILE order, not tree order, and that is load-bearing
// rather than incidental: emission order fixes vertex numbering, so any other
// order would produce a mesh that draws identically and hashes differently -
// and stage B's whole regression net is hashes.
//
// ⚑ `ranges` comes back EMPTY when the `[build]` post-pass ran, and that is
// honesty rather than an oversight: weld, optimize and a smoothing angle all
// merge and renumber vertices, after which a built index no longer names the
// part that emitted it. Ask forgeHasVertexAttribution first.
[[nodiscard]] bool buildForge(const ForgeDoc& doc, MeshData& out, std::string* error = nullptr,
                              std::vector<ForgePartRange>* ranges = nullptr);

// True when a built vertex index names the part that emitted it - i.e. the
// `[build]` post-pass is off. Not one of the six committed assets sets it, which
// is the same fact that lets them be proved byte for byte.
[[nodiscard]] bool forgeHasVertexAttribution(const ForgeDoc& doc);

// How a dragged point reaches the authored numbers behind it. The kind decides
// the arithmetic; `param` names what the write lands in, so the tool can say it
// out loud and an author can go and read that line.
//
// ⚑ The three classes stage E measured over all 61 parts in `assets/meshes/`:
// the first two kinds are class (1), where the parameter IS the vertex; the
// last two are class (2), where it is not and has to be solved for. Class (3)
// - a torus ring, which is a function of two segment indices - has no kind here
// at all, because there is nothing to write until the part is baked.
enum class ForgeWriteKind
{
    // A `flat_triangle`'s p0/p1/p2, which `addFlatTriangle` emits one for one.
    // The delta goes straight onto the named Vec3.
    Vertex,
    // One entry of a baked part's vertex list, addressed by `element`.
    BakedVertex,
    // ⚑ A box corner, which is not a parameter and SOLVES BACK to two: `center`
    // moves by half the delta and `size` grows by it, which is exactly what
    // pins the opposite corner. `element` is the corner's sign code - bit 0 is
    // +x, bit 1 is +y, bit 2 is +z - so the pinned corner is `element ^ 7`.
    //
    // The code names which combination of half-extents the corner is, not where
    // it sits, so it stays right for a box authored with a negative size.
    BoxCorner,
    // ⚑ A beam corner, which solves back to the END it stands at: `from` for
    // element 0, `to` for element 1. A beam's cross-section is derived from its
    // axis (`addBeam` builds `side` and `up` from the run), so NOT ONE of its
    // eight corners is a number in the file and the only answer to a dragged
    // corner is to move the end. A drag is therefore a re-aim: the other three
    // corners at that end come with it, and the four at the pinned end swing as
    // the cross-section is re-derived - by at most the section's half-diagonal,
    // which is 0.05 m on every beam in `cockpit.forge`.
    BeamEnd,
};

// One authored value a dragged point writes back through.
struct ForgePointWrite
{
    std::size_t part = 0; // into ForgeDoc::parts
    ForgeWriteKind kind = ForgeWriteKind::Vertex;
    // "p0"/"p1"/"p2", "vertices", "center+size" or "from"/"to". For display and
    // for the class (1) kinds, which look the parameter up by name.
    std::string param;
    // Into a VertexList for BakedVertex, a corner sign code for BoxCorner, an
    // end index for BeamEnd, and 0 for a plain Vec3.
    std::uint32_t element = 0;
};

// A distinct point of the built mesh, with every authored value standing at it.
//
// ⚑ This is the stage E mechanism, and the reason it is a SET rather than a
// value: `ship.forge`'s front corner is carried by five parts under three
// different parameter names, because a corner is shared across two quadrants.
// Writing one of them and not the others opens a seam in the hull. One drag,
// every write, or the tool is worse than the text editor it replaces.
struct ForgePoint
{
    // ⚑ Where the point is DRAWN, which is what a pick has to match - and that
    // means it is float precision promoted to double, because it was read out
    // of a MeshData. An authored `-2.6` comes back here as -2.5999999046325684.
    // Nothing may write this number back into the document; see forgeMovePoint.
    BuildPoint position{0, 0, 0}; // in the frame the mesh is built in
    // ⚑ DEDUPLICATED, which is why it is not simply one entry per corner. A box
    // puts THREE render vertices at each of its eight corners and all three
    // name the same `center`+`size` pair, so writing one per corner would apply
    // the drag three times and send the corner three times as far as the hand.
    std::vector<ForgePointWrite> writes;
    std::uint32_t corners = 0;  // render vertices standing here
    std::uint32_t resolved = 0; // ...of which this many found an authored answer

    // ⚑ False when some corner at this point came out of a primitive with no
    // parametric answer - a torus ring vertex is a function of two segment
    // indices and there is nothing to write. Such a point is not movable until
    // its part is baked, which is the D checkpoint's rule meeting the two parts
    // in this repo that actually need it.
    //
    // ⚑ It asks whether every corner was ANSWERED, not whether every corner
    // produced a line. Those were the same question while a corner was always a
    // parameter of its own; a box makes them different, and reading it the old
    // way reports every box in the repo as unmovable.
    [[nodiscard]] bool movable() const { return corners > 0 && resolved == corners; }
};

// Every distinct point of the mesh `doc` builds, with its writes resolved.
//
// ⚑ `tolerance` defaults to `WeldOptions::positionTolerance`, deliberately the
// same number rather than a second one of this function's own: the tool's idea
// of "one point" and the engine's have to agree, or the panel's welded-point
// count and the point a drag moves describe different meshes.
[[nodiscard]] bool forgePoints(const ForgeDoc& doc, std::vector<ForgePoint>& out,
                               std::string* error = nullptr, double tolerance = 1e-5);

// Moves one point by `delta`, writing every parameter standing at it. The delta
// is in the frame the mesh is built in and is rotated into each part's own
// frame through the inverse of that part's world transform, so a part under a
// rotated group gets the number it would have been authored with.
//
// ⚑⚑ A DELTA, NOT A DESTINATION, AND THE DIFFERENCE IS THE WHOLE DISCIPLINE.
// `ForgePoint::position` is float - it was read out of the built mesh - so a
// destination-based move would rebuild every authored double from a rounded
// number, turning `p1 = [-2.6, -1.3, -1.0]` into `[-2.5999999046325684, ...]`
// on a drag of zero distance. Every file would grow noise on the first click.
// Adding a delta to the value already in the document means an unmoved point
// writes back exactly what it read, which is the same fixed-point discipline
// the comment-preserving writer exists to keep.
//
// ⚑ A CLASS (2) WRITE IS A RESIZE OR A RE-AIM, NOT A MOVE, and that is the
// honest answer rather than a compromise: a box corner and a beam corner are
// not numbers in the file, so the only thing that CAN be written is the pair
// the corner is computed from. Dragging a box corner therefore moves the seven
// corners that share a coordinate with it and pins the eighth. Dragging a beam
// corner moves the whole end it stands at, and swings the corners at the far
// end as the cross-section is re-derived from the new axis - bounded by the
// section's half-diagonal, 0.05 m on every beam in `cockpit.forge`.
//
// ⚑ A write whose local delta is exactly zero is SKIPPED rather than applied,
// which keeps the fixed point above true for a part that authors nothing.
// `cube.forge` is the whole asset that does: every box parameter of it is at
// the schema default, and `set` adds a key that is not there, so a write of
// `value + 0` would materialise `center` and `size` on a click-and-release.
//
// Refuses a point that is not `movable()`, and refuses one whose part has a
// singular world transform, rather than writing a number derived from a
// division by zero. Refuses a drag that would push a box's `size` through zero
// (it would light inside out) or collapse a beam onto its other end (it would
// silently stop drawing). Nothing is written unless every write can be
// resolved: a half-applied move is the seam this function exists to prevent.
[[nodiscard]] bool forgeMovePoint(ForgeDoc& doc, const ForgePoint& point, BuildPoint delta,
                                  std::string* error = nullptr);

// The world transform of one part: its own placement composed up the parent
// chain. Exposed because the editor needs it to draw a part's own axes.
[[nodiscard]] BuildTransform forgeWorldTransform(const ForgeDoc& doc, std::size_t partIndex);

// Geometry as a literal `mesh` part - the bake. The part carries no placement,
// because the geometry handed in is already wherever it is.
//
// ⚑ Baked numbers are written at FLOAT precision, not double, and that is a
// decision rather than a shortcut: a baked part's numbers came out of a mesh,
// and a mesh is float, so the seventeen digits a double writer emits would be
// fifteen digits of noise per coordinate in a file whose whole purpose is that
// a person can read it.
[[nodiscard]] ForgePart forgeBakePart(const std::string& id, const MeshData& mesh);

} // namespace sol::assets
