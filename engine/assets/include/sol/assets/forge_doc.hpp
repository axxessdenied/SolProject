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
// document. Parameters still at their schema default are omitted, so a file
// stays as short as what it actually says.
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

} // namespace sol::assets
