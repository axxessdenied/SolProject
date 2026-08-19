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

// Evaluates the tree into a mesh.
//
// ⚑ Parts are emitted in FILE order, not tree order, and that is load-bearing
// rather than incidental: emission order fixes vertex numbering, so any other
// order would produce a mesh that draws identically and hashes differently -
// and stage B's whole regression net is hashes.
[[nodiscard]] bool buildForge(const ForgeDoc& doc, MeshData& out, std::string* error = nullptr);

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
