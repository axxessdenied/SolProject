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

    // ⚑ WHERE THIS PART CAME FROM, when it came from somewhere: the Blender
    // object's uid, carried across in the glTF's node `extras` (Phase 9 stage
    // P). Empty on every part the Forge itself made, and on every committed
    // asset - the writer omits it, so nothing gains a line.
    //
    // It exists because an `id` is a LABEL and a re-import needs an IDENTITY.
    // Matching on the name makes a rename indistinguishable from a delete plus
    // an add, which leaves the author two copies of one object with the live
    // geometry in the one carrying none of their placement.
    std::string origin;

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

// One edge of the built mesh (engine plan Phase 9 stage E4): a pair of points
// with at least one triangle standing on it.
//
// ⚑⚑ `a` and `b` index the ForgePoint vector, NOT an EditMesh, and that is the
// whole reason this exists instead of `MeshAdjacency`. `buildAdjacency` already
// produces a sorted unique edge list with its faces - but over
// `EditMesh::positions`, which `toEditMesh` numbers with a spatial-hash welder
// and `removeUnused` then RENUMBERS, while `forgePoints` numbers by its own
// linear scan over every built vertex, referenced or not. Two independent welds
// at the same tolerance are two numberings that agree only by accident, and a
// tool that picked an edge in one and wrote through the other would move the
// wrong point on the first asset where they diverged. This repo has shipped
// that bug once already, one index meaning two things.
struct ForgeEdge
{
    std::uint32_t a = 0; // a < b, both into the ForgePoint vector
    std::uint32_t b = 0;
    std::uint32_t faceCount = 0; // triangles standing on it; 1 means a border
};

// Every distinct point of the mesh `doc` builds, and every edge joining them,
// from ONE build and ONE dedup pass.
//
// ⚑ Both halves come out of the same traversal on purpose. Deriving the edges
// from a second pass - or from a second weld - is the two-implementations trap
// this programme has already paid for twice (the mesh that was a `.gltf` and a
// `.forge`, the `box` that the bake and the build each described). The points
// this returns are exactly `forgePoints`', so a caller may use both.
//
// ⚑ A degenerate edge is not an adjacency: a face whose two corners weld to one
// point contributes nothing, which is how `gate_membrane.forge` - a revolve
// that touches its own axis and fans to a point, 32 degenerate faces, one per
// segment - is described honestly rather than with 32 edges of no length.
// One triangle of the built mesh, in the same numbering as the points and the
// edges (engine plan Phase 9 stage E4d).
//
// ⚑ A face whose corners do not weld to THREE distinct points is not here, for
// the same reason a degenerate edge is not: it has no area, so no ray can enter
// it and no drag can move it. `gate_membrane.forge` contributes 32 of them, one
// per segment, where its revolve touches its own axis and fans to a point.
struct ForgeFace
{
    std::uint32_t a = 0; // all three into the ForgePoint vector
    std::uint32_t b = 0;
    std::uint32_t c = 0;

    // ⚑⚑ WHICH PART EMITTED THIS TRIANGLE, AND WHICH OF ITS OWN TRIANGLES IT IS
    // (engine plan Phase 9 stage E5). E4 never needed either: a face drag moves
    // POINTS, and a point already carries every part standing at it. A topology
    // change is the opposite - it rewrites one part's index list, so it has to
    // know whose, and where.
    //
    // ⚑ `triangle` counts the part's OWN triangles including the degenerate ones
    // that never reach this vector, because a baked part's `indices` carries them
    // too and the number has to address that list. It is stable across the bake:
    // `forgeBakeDocumentPart` emits the part through the same `emitPart` the
    // whole-document build does, so the k-th triangle stays the k-th triangle -
    // which is the property E3's byte-for-byte assertion already pins.
    std::size_t part = 0;
    std::uint32_t triangle = 0;
};

[[nodiscard]] bool forgeTopology(const ForgeDoc& doc, std::vector<ForgePoint>& points,
                                 std::vector<ForgeEdge>& edges, std::vector<ForgeFace>& faces,
                                 std::string* error = nullptr, double tolerance = 1e-5);

// The nearest face a ray enters, by Möller-Trumbore.
//
// ⚑⚑ THIS IS THE ONE PIECE OF E4's ESTIMATE THAT STOOD: nothing in this repo
// matched `Trumbore`, `rayTriangle` or `intersectTriangle`, and face picking is
// the first thing that has ever needed one. Edge picking did NOT - an edge is a
// segment on screen once its ends are projected, so a two-dimensional
// point-to-segment distance is the whole test. A face has an interior, and the
// projection of a triangle is a triangle, so the honest test is the ray.
//
// ⚑ It is here, over `ForgePoint` indices, rather than in a general geometry
// header over an `EditMesh`, for the reason `forgeTopology` exists at all: the
// thing a click resolves to has to be numbered in the vector the tool WRITES
// through, or the pick and the write describe different meshes.
//
// `direction` need not be normalised; `distance` comes back in units of it.
// Back faces are hit as readily as front ones - the tool draws both sides and an
// author may be looking at either.
[[nodiscard]] bool forgePickFace(std::span<const ForgePoint> points,
                                 std::span<const ForgeFace> faces, BuildPoint origin,
                                 BuildPoint direction, std::size_t& face, double& distance);

// Every triangle coplanar with and edge-connected to `seed` - the QUAD behind a
// picked half of one.
//
// ⚑⚑ THIS EXISTS BECAUSE A BOX FACE IS TWO TRIANGLES AND THE ENGINE REFUSES
// THREE CORNERS OUT OF FOUR. `forgeMovePoints` can express a whole box face
// exactly and nothing less than one, so a tool that handed it the picked
// TRIANGLE would be refused on every box in the repo - the commonest primitive
// there is. The widening happens at PICK time, which is the only place it can:
// by the time the write set is built, "three corners" and "an author who meant
// four" are indistinguishable.
//
// ⚑ Coplanarity is measured against the SEED's normal, never against each
// neighbour's own. Comparing step by step lets a chain of small angles walk the
// group right around a cylinder, one nearly-flat hop at a time, and arrive at a
// selection nobody could have meant. Against the seed, a curved surface stops
// at the first bend by construction.
//
// Returns the seed alone when nothing is coplanar with it, which is the answer
// for a `flat_triangle` and for every face of a faceted hull.
void forgeFaceGroup(std::span<const ForgePoint> points, std::span<const ForgeFace> faces,
                    std::size_t seed, std::vector<std::uint32_t>& out);

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

// Moves a SET of points by one delta - an edge's two, a face's three or four -
// applying every authored value standing at any of them exactly ONCE.
//
// ⚑⚑ THIS IS NOT `forgeMovePoint` IN A LOOP, AND THE DIFFERENCE IS THE STAGE.
// The point move deduplicates the writes standing at one point; a set has to
// deduplicate ACROSS points, because a box puts the same `center`+`size` pair
// behind all eight of its corners and a beam puts the same `from` behind the
// four at one end. Called in a loop, an edge of a box travels TWICE as far as
// the hand that dragged it, and the file records the doubled number.
//
// ⚑⚑ A BOX CANNOT MOVE AN EDGE, AND THAT IS ALGEBRA RATHER THAN A LIMITATION
// ANYONE CHOSE. A corner sits at `center + s*size/2`, so on each axis a change
// of the pair moves a corner by one of exactly TWO amounts, one per sign -
// there is no shear to write. Three selections come out of that and a box can
// only do two of them: ONE CORNER is E2's resize, unchanged, with the opposite
// corner pinned; a WHOLE FACE is exact, with the opposite face pinned; and
// ANYTHING ELSE - an edge, or part of a face - is REFUSED, with the error
// naming the part to bake. The nearest thing the arithmetic could offer instead
// is to widen the face the edge lies on, which measured out as "grabbed 2,
// moved 4" and is indistinguishable from a bug. Baked, the same drag moves
// exactly the two corners that were grabbed.
//
// ⚑ AND A FACE IS EXACT ONLY ALONG ITS OWN NORMAL. On an axis the face's
// corners straddle, the only expressible move is `ds = 0, dc = delta`, which
// slides the WHOLE box - the same "moved more than I grabbed" surprise wearing
// different clothes - so an off-normal component is discarded and `dropped` is
// set, letting the tool say so rather than leaving it to be discovered. It is a
// FLAG and not the surviving vector because the drop is decided in each part's
// own frame and `gate.forge` turns seven of its parts: a composed world vector
// would be right only while every box stood at the identity.
//
// Refuses on the same terms as `forgeMovePoint`, and for the same reason:
// nothing is written unless every write resolves, because a half-applied move
// is the seam this whole mechanism exists to prevent.
[[nodiscard]] bool forgeMovePoints(ForgeDoc& doc, std::span<const ForgePoint> points,
                                   BuildPoint delta, bool* dropped = nullptr,
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

// Converts one part of a document into a literal `mesh` part in place (engine
// plan Phase 9 stage E3) - the D checkpoint's rule, which is that a hand edit
// bakes only the parts it touches and leaves every other part parametric.
//
// ⚑⚑ THE GEOMETRY IS STORED IN THE PARENT'S FRAME, NOT THE PART'S OWN AND NOT
// THE WORLD'S, AND THAT IS THE ONE DECISION THIS FUNCTION MAKES. A part's world
// transform is `P * L` - the parent chain times its own placement. Folding `L`
// into the geometry and leaving the placement at identity means the rebuild
// computes `P * (L * p)`, so the part STAYS HANGING WHERE THE AUTHOR PUT IT,
// and it is bit-exact whenever `P` is the identity.
//
// The two alternatives are both worse and the reasons are measurable. Storing
// the part's OWN local geometry and keeping its placement rounds in the MIDDLE:
// the build path is `round(W * p)`, rounding once at `build()` per stage B's
// rule, and the local form is `round(W * round(p))`, which differs whenever the
// intermediate rounding tips a coordinate across a float boundary - and
// `gate.forge`'s ring and six of its arms are turned by 90, 180 and 270
// degrees, none of which is exact in radians. Folding the WHOLE world transform
// in is bit-exact too, but it has to re-hang the part at the root, so baking
// one strut detaches it from the assembly it belongs to.
//
// ⚑ `id`, `parent` and the part's `leading` trivia survive; the primitive, its
// parameters and its placement are what the bake consumes. Keeping the comment
// is not tidiness: in this repo a `[[part]]` comment block is where the
// knowledge about the asset lives, and deleting one is the loss the
// comment-preserving writer exists to prevent, arriving one stage later.
//
// Refuses a group (it carries no geometry), an index past the end, and any part
// whose primitive will not build. A part that is already a `mesh` is left
// exactly as it is rather than round-tripped, so baking twice is baking once.
[[nodiscard]] bool forgeBakeDocumentPart(ForgeDoc& doc, std::size_t partIndex,
                                         std::string* error = nullptr);

// ⚑⚑ THE THREE TOPOLOGY OPERATIONS (engine plan Phase 9 stage E5), AND THE ONE
// THING THEY ALL SHARE: THEY EDIT A BAKED PART'S OWN LISTS IN PLACE.
//
// The stage E spec named `mesh_edit`'s `append`, `weld` and
// `removeDegenerateFaces` as the plumbing. None of them is called here, and the
// measurement that settles it is one line: `toEditMesh` followed by
// `toMeshData` changes the vertex count on three of the seven committed assets
// (asteroid 960 -> 954, cockpit 426 -> 424, ship 48 -> 42). `EditMesh` is a
// GLOBAL representation - it welds every position and merges every identical
// corner across the whole mesh - while a baked part writes one vertex per line
// precisely so an edit is a small diff. A one-face extrude routed through it
// would rewrite all 99,115 bytes of `asteroid.forge` and shift every
// `BakedVertex` element index the point tool writes through. `mesh_edit` is
// stage F's layer, where a whole-mesh weld and decimate is the intention.
//
// ⚑ Every one of these bakes the parts it touches, because a topology change
// has no parametric answer by construction - a box with a face pulled out of it
// is not a box. The bake is performed rather than demanded, unlike the point
// tool's refusals, because unlike a drag there is no version of the operation
// that leaves the part parametric. What the CALLER owes an author is the
// warning, since the cost varies by 750x across the primitives: baking one
// `flat_triangle` costs `ship.forge` two lines and baking `station.forge`'s
// `habitat_ring` costs it 1,492.

// Merges two parts into one. The EARLIER of the two survives - keeping its id,
// its parent and its `leading` comment block - and the later one's geometry is
// appended to it and the part erased.
//
// ⚑⚑ EARLIER RATHER THAN "TARGET" IS WHAT KEEPS IT BIT-EXACT. Parts emit in
// FILE order, so a merge that put the survivor anywhere but the earlier slot
// would renumber every vertex after it. Merging two ADJACENT parts leaves
// `buildForge`'s output identical by memcmp; merging two distant ones is the
// same geometry renumbered, which is honest but is not the same file.
//
// ⚑ Both parts must share a `parent`, and that is E3's bake frame talking
// rather than a convenience: a baked part's geometry is stored in its PARENT's
// frame, so concatenating across two different parents would need one side
// re-expressed through the world and back - exact only while both parents are
// the identity. Nothing in `assets/meshes/` is affected: only `gate.forge` sets
// a parent at all, and all eight of those name one bare group.
//
// Refuses two indices that are the same, either being a group, either failing
// to bake, and a later part that other parts hang off - re-parenting the
// orphans silently is the kind of edit nobody would find again.
[[nodiscard]] bool forgeMergeParts(ForgeDoc& doc, std::size_t partA, std::size_t partB,
                                   std::string* error = nullptr);

// Splits the edge joining points `a` and `b` at its midpoint: every face
// standing on it becomes two, and the parts that emitted them are baked.
//
// ⚑⚑ A SPLIT COMPOSES ACROSS PARTS AND THAT IS WHY IT IS THE CHEAP ONE. Each
// face answers for itself - insert the midpoint, replace the triangle with two -
// so a split that lands in two different parts is just two independent splits,
// and the new point welds to its twin next door because both are the midpoint of
// the same pair under an affine transform. Nothing is created that belongs to
// nobody. That matters more than it sounds: 24 of `ship.forge`'s 24 edges have
// their two faces in DIFFERENT parts, so on the asset stage E exists for, every
// single edge is the cross-part case.
//
// ⚑ The midpoint is taken between the two PART-LOCAL vertices, not between the
// two `ForgePoint` positions, so no frame conversion is involved and the answer
// is right for a part hanging under a turned parent.
//
// Refuses a pair with no non-degenerate face standing on it, and refuses the
// whole edit if any part on the edge will not bake.
[[nodiscard]] bool forgeSplitEdge(ForgeDoc& doc, std::span<const ForgePoint> points,
                                  std::span<const ForgeFace> faces, std::uint32_t a,
                                  std::uint32_t b, std::string* error = nullptr);

// Raises a coplanar face group along its own normal and walls in the gap it
// leaves - the extrude. `group` is `forgeFaceGroup`'s output.
//
// ⚑⚑ IT REFUSES A GROUP THAT SPANS TWO PARTS, AND NAMES `merge`. A split is per
// face; an extrude raises WALLS around the group's border, and a wall straddling
// the seam between two parts belongs to neither. This is E4c's rule one level up
// - refuse a wrong result you could have declined, and route to the escape hatch
// - and the escape hatch is `forgeMergeParts` above, which is why merge is built
// before extrude rather than beside it. Measured: 4 of the 1,273 face groups in
// `assets/meshes/` span two parts (`ship.forge`'s `hull_0a`+`hull_0b`,
// `hull_2a`+`hull_2b` and `rear_lower`+`rear_upper`, and `cockpit.forge`'s
// `cowl_back_lower`+`cowl_back_upper`), all four are ADJACENT pairs, and merging
// them is what an author wanted anyway: two triangles that are one quad.
//
// ⚑⚑ THE OFFSET CANNOT BE ZERO, WHICH IS THE ONE PLACE THE STANDARD IDIOM DOES
// NOT TRANSFER. The modeller's gesture is extrude-then-move: the geometry is
// created flush and the existing drag positions it. Here a point's identity is
// POSITIONAL - `forgePoints` welds at `WeldOptions::positionTolerance` - so a
// face duplicated in place welds straight back into the face it came from and
// the drag moves both. Nor can it be a fixed number of metres: this tool's orbit
// camera covers four orders of magnitude and an offset that reads on the
// station's 102 m ring is wider than the whole `cockpit.forge` dash. So it is a
// FRACTION OF THE GROUP'S OWN LONGEST BORDER EDGE, quantized to the same 0.1 mm
// write grid a drag lands on and floored at one step of it. `offset` reports
// what was used, in metres, so the tool can say it.
//
// ⚑ A vertex the group shares with a triangle OUTSIDE it is duplicated rather
// than moved, so the neighbour keeps its corner. A vertex used only by the group
// is moved in place, which is what keeps the diff to the lines that changed.
//
// Refuses an empty group, a group with no area, a group spanning parts, and a
// part that will not bake. Nothing is written unless the whole operation
// resolves.
// ⚑ It takes no `ForgePoint` span, unlike everything else in stage E, and that
// is the finding rather than an omission: an extrude is entirely a PART-LOCAL
// operation. The geometry, the normal it raises along and the offset are all in
// the frame the part's own numbers are written in, so there is no inverse
// transform in it anywhere - where a drag arrives in the built frame and has to
// be brought back.
[[nodiscard]] bool forgeExtrudeFaces(ForgeDoc& doc, std::span<const ForgeFace> faces,
                                     std::span<const std::uint32_t> group, double* offset = nullptr,
                                     std::string* error = nullptr);

} // namespace sol::assets
