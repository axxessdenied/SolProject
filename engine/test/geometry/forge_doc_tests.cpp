#include "shipped_meshes.hpp"

#include "sol/assets/forge_doc.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/test/test.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace sol;
using assets::ForgeDoc;
using assets::ForgePrimitive;
using assets::ForgeValue;

namespace {

[[nodiscard]] bool parses(const std::string& text, ForgeDoc& out)
{
    return assets::parseForge(text.c_str(), text.size(), "test.forge", out, nullptr);
}

[[nodiscard]] std::string rejects(const std::string& text)
{
    ForgeDoc doc;
    std::string error;
    if (assets::parseForge(text.c_str(), text.size(), "test.forge", doc, &error)) {
        return {};
    }
    return error.empty() ? "rejected" : error;
}

[[nodiscard]] std::string readWholeFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("  cannot open %s\n", path.c_str());
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// "the file differs" is a poor thing to learn about a 1300-line asset, so say
// which line and show both.
void reportFirstDifferingLine(const char* name, const std::string& expected, const std::string& actual)
{
    std::size_t line = 1;
    std::size_t lineStart = 0;
    const std::size_t shared = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < shared; ++i) {
        if (expected[i] != actual[i]) {
            const std::size_t expectedEnd = expected.find('\n', lineStart);
            const std::size_t actualEnd = actual.find('\n', lineStart);
            std::printf("  %s.forge line %zu\n    want: %s\n    got:  %s\n",
                        name,
                        line,
                        expected.substr(lineStart, expectedEnd - lineStart).c_str(),
                        actual.substr(lineStart, actualEnd - lineStart).c_str());
            return;
        }
        if (expected[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    std::printf("  %s.forge: same for %zu bytes, then lengths differ (%zu vs %zu)\n",
                name,
                shared,
                expected.size(),
                actual.size());
}

[[nodiscard]] bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

// The assertion stage B's regression net is built on: same counts, same bytes,
// in the same order.
[[nodiscard]] bool sameMesh(const assets::MeshData& a, const assets::MeshData& b)
{
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size()) {
        return false;
    }
    if (!a.vertices.empty() &&
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(assets::MeshVertex)) !=
            0) {
        return false;
    }
    return a.indices == b.indices;
}

// Leaves the mesh empty on failure, which fails every comparison it feeds -
// and prints the reason, because "the meshes differ" is a poor way to learn
// that the build refused to run at all.
[[nodiscard]] assets::MeshData built(const ForgeDoc& doc)
{
    assets::MeshData mesh;
    std::string error;
    if (!assets::buildForge(doc, mesh, &error)) {
        std::printf("buildForge failed: %s\n", error.c_str());
    }
    return mesh;
}

} // namespace

SOL_TEST(forgeParsesAPartTreeAndItsBuildOptions)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
name = "widget"

[build]
weld = true
optimize = true
smooth_angle = 35.0

[[part]]
id = "hull"
type = "box"
size = [2.0, 4.0, 6.0]

[[part]]
id = "fin"
type = "box"
parent = "hull"
position = [0.0, 3.0, 0.0]
size = [1.0, 2.0, 1.0]
)",
                       doc));

    SOL_CHECK(doc.name == "widget");
    SOL_CHECK(doc.build.weld);
    SOL_CHECK(doc.build.optimize);
    SOL_CHECK(near(doc.build.smoothAngleDegrees, 35.0, 1e-9));
    SOL_REQUIRE(doc.parts.size() == 2);
    SOL_CHECK(doc.parts[0].id == "hull");
    SOL_CHECK(doc.parts[0].primitive == ForgePrimitive::Box);
    SOL_CHECK(doc.parts[1].parent == "hull");
    SOL_CHECK(near(doc.parts[1].value("size").vec.y, 2.0, 1e-9));
}

// ⚑ Angles are stored exactly as authored and converted at the point of use.
// Round-tripping them through radians is LOSSY - 30 degrees comes back as
// 29.999999999999996 - so a document that converted on the way in would rewrite
// angles nobody touched on every save. 30 and 7.5 are here rather than 90
// because 90 survives the lossy path by luck and would have proved nothing.
SOL_TEST(forgeKeepsAuthoredAnglesExactlyAsWritten)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "a"
type = "box"
rotation = [30.0, 7.5, 120.0]
)",
                       doc));
    SOL_CHECK(doc.parts[0].rotationDegrees.x == 30.0);
    SOL_CHECK(doc.parts[0].rotationDegrees.y == 7.5);
    SOL_CHECK(doc.parts[0].rotationDegrees.z == 120.0);

    const std::string text = assets::writeForge(doc);
    SOL_CHECK(text.find("rotation = [30.0, 7.5, 120.0]") != std::string::npos);

    // And the rotation still means what it says once it reaches the builder.
    ForgeDoc quarter;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "a"
type = "group"
rotation = [0.0, 90.0, 0.0]
)",
                       quarter));
    const assets::BuildTransform turned = assets::forgeWorldTransform(quarter, 0);
    const assets::BuildPoint forward = turned.transformDirection({0, 0, 1});
    SOL_CHECK(near(forward.x, 1.0, 1e-12));
    SOL_CHECK(near(forward.z, 0.0, 1e-12));
}

// ⚑ A round number's SHORTEST round-tripping form is an exponent - 90 is
// `9e+01` - so a writer that only asked "does this reparse?" would turn the
// numbers an author is most likely to have typed into the ones they are least
// likely to recognise. The file is text so that a person can read it.
SOL_TEST(forgeWritesNumbersAPersonWouldRecognise)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "a"
type = "box"
center = [90.0, 100.0, 1000.0]
size = [0.075, 47.0, 1.5]
)",
                       doc));

    const std::string text = assets::writeForge(doc);
    SOL_CHECK(text.find("center = [90.0, 100.0, 1000.0]") != std::string::npos);
    SOL_CHECK(text.find("size = [0.075, 47.0, 1.5]") != std::string::npos);
    SOL_CHECK(text.find("e+") == std::string::npos);
}

SOL_TEST(forgeRoundTripsThroughItsOwnWriter)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
name = "rig"

[build]
weld = true

[[part]]
id = "frame"
type = "group"
position = [1.0, 2.0, 3.0]

[[part]]
id = "spar"
type = "beam"
parent = "frame"
from = [0.0, 0.0, 0.0]
to = [0.0, 5.0, 0.0]
width = 0.25
height = 0.4
scale = [1.0, 2.0, 1.0]

[[part]]
id = "ring"
type = "torus"
major_radius = 9.0
tube_radius = 1.5
segments_u = 40
segments_v = 12
u_tiles = 8.0

[[part]]
id = "cone"
type = "revolve"
profile = [[0.0, 0.0], [2.0, 0.0], [0.0, 4.0]]
segments = 16
cap_ends = true
)",
                       doc));

    const std::string text = assets::writeForge(doc);
    ForgeDoc reparsed;
    SOL_REQUIRE(parses(text, reparsed));

    SOL_REQUIRE(reparsed.parts.size() == doc.parts.size());
    SOL_CHECK(reparsed.name == doc.name);
    SOL_CHECK(reparsed.build.weld == doc.build.weld);
    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        SOL_CHECK(reparsed.parts[i].id == doc.parts[i].id);
        SOL_CHECK(reparsed.parts[i].parent == doc.parts[i].parent);
        SOL_CHECK(reparsed.parts[i].primitive == doc.parts[i].primitive);
    }
    // The mesh is the real round-trip assertion: the text can be reformatted,
    // but the geometry it describes cannot move.
    SOL_CHECK(sameMesh(built(doc), built(reparsed)));

    // Writing the reparsed document again is byte-identical, so a save that
    // changed nothing leaves `git status` clean.
    SOL_CHECK(assets::writeForge(reparsed) == text);
}

// ⚑ STAGE P's `origin` - where a part came from, when it came from somewhere.
// It has to survive the file or it is not an identity, and it has to be ABSENT
// from a part that has none or every committed asset gains a line on its next
// save (which `everyCommittedForgeSourceRoundTripsByteForByte` then catches
// from the other end).
SOL_TEST(aPartsOriginSurvivesTheFileAndAPartWithoutOneGainsNoLine)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
name = "rig"

[[part]]
id = "hull"
type = "box"
origin = "3f2b91c4e7a04d1e"

[[part]]
id = "strut"
type = "box"
position = [1.0, 0.0, 0.0]
)",
                       doc));

    SOL_REQUIRE(doc.parts.size() == 2);
    SOL_CHECK(doc.parts[0].origin == "3f2b91c4e7a04d1e");
    // A part the Forge made itself, which must stay anonymous: `kept` tells an
    // author's part from Blender's by exactly this emptiness.
    SOL_CHECK(doc.parts[1].origin.empty());

    const std::string text = assets::writeForge(doc);
    SOL_CHECK(text.find("origin = \"3f2b91c4e7a04d1e\"") != std::string::npos);
    // Once, not twice - the anonymous part contributes no key at all.
    SOL_CHECK(text.find("origin") == text.rfind("origin"));

    ForgeDoc reparsed;
    SOL_REQUIRE(parses(text, reparsed));
    SOL_REQUIRE(reparsed.parts.size() == 2);
    SOL_CHECK(reparsed.parts[0].origin == doc.parts[0].origin);
    SOL_CHECK(reparsed.parts[1].origin.empty());
    SOL_CHECK(assets::writeForge(reparsed) == text);
}

// ⚑ The parser is strict by design - an unrecognised key is an error rather
// than a shrug - and adding a key must not weaken that. A non-string `origin`
// is the shape a hand-edit gets wrong.
SOL_TEST(anOriginThatIsNotAStringIsRefusedRatherThanCoerced)
{
    ForgeDoc doc;
    SOL_CHECK(!parses("[[part]]\nid = \"a\"\ntype = \"box\"\norigin = 7\n", doc));
}

SOL_TEST(forgeComposesTransformsDownTheParentChain)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "root"
type = "group"
position = [10.0, 0.0, 0.0]

[[part]]
id = "mid"
type = "group"
parent = "root"
rotation = [0.0, 90.0, 0.0]

[[part]]
id = "leaf"
type = "box"
parent = "mid"
position = [0.0, 0.0, 4.0]
size = [1.0, 1.0, 1.0]
)",
                       doc));

    // +Z rotated 90 degrees about +Y lands on +X, and the root shifts it out to
    // 14. A child of a rotated parent moving along the parent's axes rather
    // than the world's is the whole point of a tree.
    const assets::BuildTransform leaf = assets::forgeWorldTransform(doc, 2);
    SOL_CHECK(near(leaf.translation.x, 14.0, 1e-9));
    SOL_CHECK(near(leaf.translation.y, 0.0, 1e-9));
    SOL_CHECK(near(leaf.translation.z, 0.0, 1e-9));
}

// A parent declared below its child is legal - the format is a list, and
// ordering it topologically would be a rule with no reason behind it.
SOL_TEST(forgeResolvesAParentDeclaredAfterItsChild)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "leaf"
type = "box"
parent = "root"
position = [0.0, 1.0, 0.0]
size = [1.0, 1.0, 1.0]

[[part]]
id = "root"
type = "group"
position = [0.0, 5.0, 0.0]
)",
                       doc));
    SOL_CHECK(near(assets::forgeWorldTransform(doc, 0).translation.y, 6.0, 1e-9));
}

SOL_TEST(forgeRejectsMalformedDocuments)
{
    SOL_CHECK(!rejects(R"(
[[part]]
type = "box"
)")
                   .empty()); // no id

    SOL_CHECK(!rejects(R"(
[[part]]
id = "a"
type = "sphere"
)")
                   .empty()); // unknown primitive

    SOL_CHECK(!rejects(R"(
[[part]]
id = "a"
type = "box"

[[part]]
id = "a"
type = "box"
)")
                   .empty()); // duplicate id

    SOL_CHECK(!rejects(R"(
[[part]]
id = "a"
type = "box"
parent = "ghost"
)")
                   .empty()); // dangling parent

    SOL_CHECK(!rejects(R"(
[[part]]
id = "a"
type = "box"
parent = "b"

[[part]]
id = "b"
type = "box"
parent = "a"
)")
                   .empty()); // cycle

    // ⚑ Strict schema: a misspelt parameter is an error, not a silently
    // ignored key. A `major_radius` typed as `major_radus` that parsed quietly
    // would leave an author staring at a torus that ignored the number.
    SOL_CHECK(!rejects(R"(
[[part]]
id = "a"
type = "torus"
major_radus = 4.0
)")
                   .empty());

    // A parameter of the wrong primitive is the same mistake wearing a
    // correctly spelled name.
    SOL_CHECK(!rejects(R"(
[[part]]
id = "a"
type = "box"
major_radius = 4.0
)")
                   .empty());

    SOL_CHECK(!rejects(R"(
[[part]]
id = "a"
type = "box"
size = [1.0, 2.0]
)")
                   .empty()); // vec3 of the wrong length
}

// ⚑ THE LOAD-BEARING TEST OF THE WHOLE FORMAT. Round-trip and validation prove
// a file is well formed; only this proves the format can express what this game
// ALREADY SHIPS. The station is the hardest of the five - a 533-vertex torus
// and nine boxes - and it has to come out byte for byte, because stage B's net
// is hashes and a mesh that draws the same while hashing differently would
// break it silently.
SOL_TEST(forgeReproducesTheShippedStationExactly)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
name = "station"

[[part]]
id = "ring"
type = "torus"
major_radius = 90.0
tube_radius = 12.0
segments_u = 40
segments_v = 12
u_tiles = 8.0

[[part]]
id = "hub"
type = "box"
size = [44.0, 60.0, 44.0]

[[part]]
id = "spoke_east"
type = "box"
center = [47.0, 0.0, 0.0]
size = [86.0, 6.0, 6.0]

[[part]]
id = "spoke_west"
type = "box"
center = [-47.0, 0.0, 0.0]
size = [86.0, 6.0, 6.0]

[[part]]
id = "spoke_south"
type = "box"
center = [0.0, 0.0, 47.0]
size = [6.0, 6.0, 86.0]

[[part]]
id = "spoke_north"
type = "box"
center = [0.0, 0.0, -47.0]
size = [6.0, 6.0, 86.0]

[[part]]
id = "mast_top"
type = "box"
center = [0.0, 44.0, 0.0]
size = [3.0, 28.0, 3.0]

[[part]]
id = "mast_bottom"
type = "box"
center = [0.0, -44.0, 0.0]
size = [3.0, 28.0, 3.0]

[[part]]
id = "panel_top"
type = "box"
center = [0.0, 62.0, 0.0]
size = [76.0, 1.5, 26.0]

[[part]]
id = "panel_bottom"
type = "box"
center = [0.0, -62.0, 0.0]
size = [76.0, 1.5, 26.0]
)",
                       doc));

    SOL_CHECK(sameMesh(built(doc), shipped::buildStation()));
}

// The cube is the other end of the range: one primitive, no parameters past
// its size, and the mesh five of the game's six models are drawn from.
SOL_TEST(forgeReproducesTheShippedCubeExactly)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "cube"
type = "box"
size = [1.0, 1.0, 1.0]
)",
                       doc));
    SOL_CHECK(sameMesh(built(doc), shipped::buildCube()));
}

// ⚑ And the placement is proved by building the same shape twice: once with a
// box's own centre, once with an identity-shaped box carried there by the part
// transform. If those disagreed, every parented part in every asset would be in
// the wrong place, and nothing else in this suite would have noticed.
SOL_TEST(forgePlacementAgreesWithAPrimitivesOwnParameters)
{
    ForgeDoc byCenter;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "a"
type = "box"
center = [3.0, -2.0, 5.0]
size = [2.0, 4.0, 6.0]
)",
                       byCenter));

    ForgeDoc byTransform;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "a"
type = "box"
position = [3.0, -2.0, 5.0]
size = [2.0, 4.0, 6.0]
)",
                       byTransform));

    SOL_CHECK(sameMesh(built(byCenter), built(byTransform)));
}

// A mirroring scale reverses orientation, so both the winding and the shading
// normals have to be put back or the part lights as though it faced inward.
// Volume carries the winding (an inside-out solid measures negative) and the
// normal check carries the shading; neither alone would catch both.
SOL_TEST(forgeMirroredPartStaysOutsideOut)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "a"
type = "box"
position = [4.0, 0.0, 0.0]
scale = [-1.0, 1.0, 1.0]
size = [2.0, 2.0, 2.0]
)",
                       doc));

    const assets::EditMesh mesh = assets::toEditMesh(built(doc));
    SOL_CHECK(near(assets::signedVolume(mesh), 8.0, 1e-4));

    // Every normal must point away from the box's own centre.
    for (const assets::EditVertex& vertex : mesh.vertices) {
        const core::Vec3 point = mesh.positions[vertex.position];
        const core::Vec3 outward{point.x - 4.0f, point.y, point.z};
        const float alignment =
            (outward.x * vertex.normal.x) + (outward.y * vertex.normal.y) + (outward.z * vertex.normal.z);
        SOL_CHECK(alignment > 0.0f);
    }
}

SOL_TEST(forgeBuildOptionsWeldAndOptimize)
{
    const std::string body = R"(
[[part]]
id = "a"
type = "box"
size = [1.0, 1.0, 1.0]
)";

    ForgeDoc raw;
    SOL_REQUIRE(parses(body, raw));
    const assets::MeshData soup = built(raw);
    SOL_CHECK(soup.vertices.size() == 24);

    ForgeDoc welded;
    SOL_REQUIRE(parses("[build]\nweld = true\n" + body, welded));
    const assets::MeshData merged = built(welded);
    // A cube's corners disagree on their normals, so welding merges nothing
    // here - which is the point: welding is not allowed to change what is
    // drawn, and a faceted box has 24 genuinely distinct corners.
    SOL_CHECK(merged.vertices.size() == 24);
    SOL_CHECK(merged.indices.size() == soup.indices.size());

    ForgeDoc optimized;
    SOL_REQUIRE(parses("[build]\noptimize = true\n" + body, optimized));
    const assets::MeshData reordered = built(optimized);
    SOL_CHECK(reordered.indices.size() == soup.indices.size());
    // Reordering must move nothing: same solid, same volume, same winding.
    SOL_CHECK(near(assets::signedVolume(assets::toEditMesh(reordered)), 1.0, 1e-6));

    // Smoothing re-derives normals; it must not move a single point.
    ForgeDoc smoothed;
    SOL_REQUIRE(parses("[build]\nsmooth_angle = 100.0\n" + body, smoothed));
    const assets::MeshData rounded = built(smoothed);
    SOL_CHECK(rounded.indices.size() == soup.indices.size());
    SOL_CHECK(near(assets::signedVolume(assets::toEditMesh(rounded)), 1.0, 1e-6));

    // Faceted: one axis carries the whole normal.
    const double faceted = std::abs(soup.vertices[0].normal[0]) + std::abs(soup.vertices[0].normal[1]) +
                           std::abs(soup.vertices[0].normal[2]);
    SOL_CHECK(near(faceted, 1.0, 1e-5));

    // ⚑ Smoothed, no normal is axis-aligned any more - but they are NOT the
    // equal-weight corner diagonal either, and that surprise is worth pinning
    // down. Measured, the first is (-1, -1, 2)/sqrt(6): FOUR triangle normals
    // with +Z counted twice. recomputeNormals averages per TRIANGLE, and a quad
    // is two of them, so a corner touched by both halves of a quad gets that
    // face's normal weighted double. It is standard unweighted averaging and it
    // means a smoothed box is subtly asymmetric - which is a reason to smooth
    // sweeps and leave boxes faceted, not a defect.
    for (const assets::MeshVertex& vertex : rounded.vertices) {
        const double length =
            std::sqrt((vertex.normal[0] * vertex.normal[0]) + (vertex.normal[1] * vertex.normal[1]) +
                      (vertex.normal[2] * vertex.normal[2]));
        SOL_CHECK(near(length, 1.0, 1e-5));
        const double dominant =
            std::max({std::abs(vertex.normal[0]), std::abs(vertex.normal[1]), std::abs(vertex.normal[2])});
        SOL_CHECK(dominant < 0.99); // nothing is a face normal any more
    }

    // Corners that agreed on all three attributes after smoothing merged, so
    // the count falls below the faceted 24 without reaching the 8 bare points -
    // box-mapped uvs keep the rest apart.
    SOL_CHECK(rounded.vertices.size() < soup.vertices.size());
    SOL_CHECK(rounded.vertices.size() > 8);
}

SOL_TEST(forgeRefusesADegenerateParametricPart)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(
[[part]]
id = "a"
type = "torus"
segments_u = 2
)",
                       doc));

    assets::MeshData mesh;
    std::string error;
    SOL_CHECK(!assets::buildForge(doc, mesh, &error));
    SOL_CHECK(!error.empty());
}

// ⚑ The load-bearing test of the comment-preserving writer, and the reason it
// can exist at all is the D debt slice: the six `.forge` files are committed and
// this suite already reads them, so the net is the real assets rather than a
// fixture that agrees with the code by construction.
//
// Byte for byte, not "equivalent". Stage E saves on every accepted edit, so a
// writer that reformatted anything at all would rewrite the whole asset the
// first time somebody nudged a vertex and bury the one line that changed.
SOL_TEST(everyCommittedForgeSourceRoundTripsByteForByte)
{
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid", "gate_membrane"};
    for (const char* name : names) {
        const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
        const std::string source = readWholeFile(path);
        SOL_CHECK(!source.empty());
        if (source.empty()) {
            continue;
        }

        ForgeDoc doc;
        std::string error;
        SOL_CHECK(assets::parseForge(source.data(), source.size(), path.c_str(), doc, &error));
        if (!error.empty()) {
            std::printf("  %s\n", error.c_str());
            continue;
        }

        // None of the six carries a comment after a value or inside an array,
        // which is what makes the byte-exact claim reachable at all.
        SOL_CHECK(!doc.hasUnplaceableComments);

        const std::string written = assets::writeForge(doc);
        SOL_CHECK(written == source);
        if (written != source) {
            reportFirstDifferingLine(name, source, written);
        }
    }
}

// The header is the file's, not the writer's. A document that arrives without
// one leaves without one - otherwise the tool would inject three lines of its
// own into a hand-written file the first time it was saved, which is the same
// class of unasked-for edit as dropping the comments was.
SOL_TEST(forgeWriterInventsNoHeaderOfItsOwn)
{
    const std::string source = "[[part]]\nid = \"a\"\ntype = \"box\"\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(doc.header.empty());
    SOL_CHECK(doc.parts.size() == 1 && doc.parts[0].leading.empty());
    SOL_CHECK(assets::writeForge(doc) == source);
}

// ⚑ The case cockpit.forge decided: a divider, a blank line, then the part it
// introduces. Storing the comments alone would reproduce the words and close
// the gap, and the author would find their spacing quietly edited.
SOL_TEST(forgeKeepsTheBlankLineBetweenACommentAndItsPart)
{
    const std::string source = "# the file, explained\n"
                               "name = \"demo\"\n"
                               "\n"
                               "# --- the hull ------------------------------\n"
                               "# and a second line about it\n"
                               "\n"
                               "[[part]]\n"
                               "id = \"hull\"\n"
                               "type = \"box\"\n"
                               "\n"
                               "# a trailing note nothing owns\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(doc.header == "# the file, explained\n");
    SOL_REQUIRE(doc.parts.size() == 1);
    SOL_CHECK(doc.parts[0].leading == "\n# --- the hull ------------------------------\n"
                                      "# and a second line about it\n\n");
    SOL_CHECK(doc.trailer == "\n# a trailing note nothing owns\n");
    SOL_CHECK(!doc.hasUnplaceableComments);
    SOL_CHECK(assets::writeForge(doc) == source);
}

// A part the tool created has no trivia, so the writer supplies the blank line
// that separates it from the part above - and a save of a document that mixes
// the two must not run them together.
SOL_TEST(forgeSeparatesAnAuthoredPartFromOneTheToolAdded)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses("name = \"demo\"\n\n# the first\n[[part]]\nid = \"a\"\ntype = \"box\"\n", doc));
    assets::ForgePart added;
    added.id = "b";
    added.primitive = ForgePrimitive::Box;
    doc.parts.push_back(added);

    const std::string written = assets::writeForge(doc);
    SOL_CHECK(written == "name = \"demo\"\n\n# the first\n[[part]]\nid = \"a\"\ntype = \"box\"\n"
                         "\n[[part]]\nid = \"b\"\ntype = \"box\"\n");

    // And it settles: writing what came back changes nothing further.
    ForgeDoc reparsed;
    SOL_REQUIRE(parses(written, reparsed));
    SOL_CHECK(assets::writeForge(reparsed) == written);
}

// Reordering carries a part's own comments with it, which is the behaviour a
// divider like "--- canopy frame ---" needs: the heading belongs to the part it
// introduces, not to the position in the file it happened to occupy.
SOL_TEST(forgeCommentsFollowTheirPartWhenItMoves)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses("name = \"demo\"\n"
                       "\n# about a\n[[part]]\nid = \"a\"\ntype = \"box\"\n"
                       "\n# about b\n[[part]]\nid = \"b\"\ntype = \"box\"\n",
                       doc));
    SOL_REQUIRE(doc.parts.size() == 2);
    std::swap(doc.parts[0], doc.parts[1]);

    const std::string written = assets::writeForge(doc);
    SOL_CHECK(written == "name = \"demo\"\n"
                         "\n# about b\n[[part]]\nid = \"b\"\ntype = \"box\"\n"
                         "\n# about a\n[[part]]\nid = \"a\"\ntype = \"box\"\n");
}

// ⚑ The two comments this model cannot place, flagged rather than dropped in
// silence. A `#` after a value would need a slot per key, and one inside an
// array's brackets belongs to the value - attaching either to the next part
// would MOVE it, which is worse than admitting the limit.
SOL_TEST(forgeFlagsACommentItCannotPlace)
{
    ForgeDoc trailing;
    SOL_REQUIRE(parses("[[part]]\nid = \"a\" # the hull\ntype = \"box\"\n", trailing));
    SOL_CHECK(trailing.hasUnplaceableComments);

    ForgeDoc inArray;
    SOL_REQUIRE(parses("[[part]]\nid = \"a\"\ntype = \"revolve\"\n"
                       "profile = [\n  [0.0, 0.0],\n  # the shoulder\n  [1.0, 2.0],\n]\n",
                       inArray));
    SOL_CHECK(inArray.hasUnplaceableComments);

    ForgeDoc clean;
    SOL_REQUIRE(parses("# a header\nname = \"demo\"\n\n[[part]]\nid = \"a\"\ntype = \"box\"\n", clean));
    SOL_CHECK(!clean.hasUnplaceableComments);
}

// ⚑ A `#` or a `[` inside a quoted id is text, not syntax. Without the scanner
// skipping quoted spans, one part called "a[b" would leave it permanently
// inside an array and every comment below that part would silently vanish -
// a failure nobody would see until their header was gone.
SOL_TEST(forgeTriviaScannerReadsBracketsInsideStringsAsText)
{
    const std::string source = "name = \"demo\"\n"
                               "\n[[part]]\nid = \"a[b\"\ntype = \"box\"\n"
                               "\n# still attached\n[[part]]\nid = \"c\"\ntype = \"box\"\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_REQUIRE(doc.parts.size() == 2);
    SOL_CHECK(doc.parts[1].leading == "\n# still attached\n");
    SOL_CHECK(!doc.hasUnplaceableComments);
    SOL_CHECK(assets::writeForge(doc) == source);
}

// --- stage E: the point a drag moves, and every parameter standing at it -----

namespace {

// The index of the point at `p`, or the count when there is none. Points come
// back in first-emission order, which is stable but not meaningful, so every
// test below names the point it wants by position.
//
// ⚑ The default tolerance is FLOAT-scale on purpose. A ForgePoint's position
// was read out of a MeshData, so the authored `-2.6` arrives here as
// -2.5999999046325684 - about 9.5e-8 away. A double-scale tolerance finds
// nothing, which is how these tests first found that a move must be a delta
// rather than a destination.
[[nodiscard]] std::size_t
pointAt(const std::vector<assets::ForgePoint>& points, assets::BuildPoint p, double tolerance = 1e-6)
{
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (std::abs(points[i].position.x - p.x) < tolerance &&
            std::abs(points[i].position.y - p.y) < tolerance &&
            std::abs(points[i].position.z - p.z) < tolerance) {
            return i;
        }
    }
    return points.size();
}

} // namespace

// Emission is in file order into one builder, so a part's vertices are a
// contiguous run. Nothing in the tool can attribute a picked vertex without
// this, and a second traversal that disagreed with the emission would be the
// two-implementations trap this programme has already paid for twice.
SOL_TEST(forgePartRangesPartitionTheBuiltMeshExactly)
{
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid", "gate_membrane"};
    for (const char* name : names) {
        const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
        const std::string source = readWholeFile(path);
        SOL_CHECK(!source.empty());
        if (source.empty()) {
            continue;
        }
        ForgeDoc doc;
        SOL_REQUIRE(parses(source, doc));

        assets::MeshData mesh;
        std::vector<assets::ForgePartRange> ranges;
        SOL_REQUIRE(assets::buildForge(doc, mesh, nullptr, &ranges));

        // One range per geometry-carrying part, and a group carries none.
        std::size_t geometryParts = 0;
        for (const assets::ForgePart& part : doc.parts) {
            if (part.primitive != ForgePrimitive::Group) {
                ++geometryParts;
            }
        }
        SOL_CHECK(ranges.size() == geometryParts);

        // Contiguous from zero, covering every vertex exactly once.
        std::uint32_t cursor = 0;
        for (const assets::ForgePartRange& range : ranges) {
            SOL_CHECK(range.firstVertex == cursor);
            SOL_CHECK(range.vertexCount > 0);
            SOL_CHECK(doc.parts[range.part].primitive != ForgePrimitive::Group);
            cursor += range.vertexCount;
        }
        SOL_CHECK(cursor == mesh.vertices.size());
    }
}

// ⚑ The post-pass merges and renumbers, after which a built index no longer
// names a part. Handing the ranges back anyway would be a mapping that is wrong
// in exactly the case the caller cannot check, so it comes back empty.
SOL_TEST(forgeVertexAttributionIsRefusedWhenTheBuildPostPassRuns)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"a\"\ntype = \"box\"\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(assets::forgeHasVertexAttribution(doc));

    std::vector<assets::ForgePoint> points;
    SOL_CHECK(assets::forgePoints(doc, points));
    SOL_CHECK(!points.empty());

    doc.build.optimize = true;
    SOL_CHECK(!assets::forgeHasVertexAttribution(doc));

    assets::MeshData mesh;
    std::vector<assets::ForgePartRange> ranges;
    SOL_REQUIRE(assets::buildForge(doc, mesh, nullptr, &ranges));
    SOL_CHECK(ranges.empty());

    std::string error;
    SOL_CHECK(!assets::forgePoints(doc, points, &error));
    SOL_CHECK(!error.empty());
    SOL_CHECK(points.empty());
}

// ⚑ THE MOTIVATING CASE, AND THE NUMBERS ARE THE FILE'S RATHER THAN THE PLAN'S.
// `ship.forge`'s own header says moving a front corner means editing "four of
// them". It is five, under three different parameter names, because a front
// corner is shared across two quadrants - and the header's ring tables document
// nine points where the file has twelve.
SOL_TEST(shipForgeFrontCornerIsCarriedByFivePartsUnderThreeParameterNames)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/ship.forge";
    const std::string source = readWholeFile(path);
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    std::string error;
    SOL_REQUIRE(assets::forgePoints(doc, points, &error));
    SOL_CHECK(error.empty());

    // 48 corners over 12 distinct points.
    SOL_CHECK(points.size() == 12);
    std::uint32_t corners = 0;
    for (const assets::ForgePoint& point : points) {
        corners += point.corners;
        // Every part of this asset is a flat triangle, so every point is
        // movable and every corner resolves to a write.
        SOL_CHECK(point.movable());
    }
    SOL_CHECK(corners == 48);

    const std::size_t index = pointAt(points, {-2.6, -1.3, -1.0});
    SOL_REQUIRE(index < points.size());
    SOL_CHECK(points[index].writes.size() == 5);
    SOL_CHECK(points[index].corners == 5);

    // nose_0.p2, hull_0a.p0, hull_0b.p0, nose_3.p1, hull_3a.p1 - three names.
    std::vector<std::string> named;
    for (const assets::ForgePointWrite& write : points[index].writes) {
        named.push_back(doc.parts[write.part].id + "." + write.param);
    }
    std::sort(named.begin(), named.end());
    SOL_REQUIRE(named.size() == 5);
    SOL_CHECK(named[0] == "hull_0a.p0");
    SOL_CHECK(named[1] == "hull_0b.p0");
    SOL_CHECK(named[2] == "hull_3a.p1");
    SOL_CHECK(named[3] == "nose_0.p2");
    SOL_CHECK(named[4] == "nose_3.p1");
}

// One drag, every write. A move that reached four of the five parts would open
// a seam in the hull, which is the defect the whole mechanism exists to prevent
// - so the assertion is that the other eleven points did not move at all.
SOL_TEST(movingOnePointWritesEveryPartStandingAtItAndMovesNothingElse)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/ship.forge";
    const std::string source = readWholeFile(path);
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> before;
    SOL_REQUIRE(assets::forgePoints(doc, before));
    SOL_REQUIRE(before.size() == 12);
    const std::size_t moved = pointAt(before, {-2.6, -1.3, -1.0});
    SOL_REQUIRE(moved < before.size());

    const assets::BuildPoint delta{-0.3, -0.15, 0.25};
    std::string error;
    SOL_REQUIRE(assets::forgeMovePoint(doc, before[moved], delta, &error));
    SOL_CHECK(error.empty());

    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    // Still twelve points: a write that missed a part would have split one
    // point into two.
    SOL_CHECK(after.size() == 12);

    const std::size_t landed = pointAt(after, {-2.9, -1.45, -0.75});
    SOL_REQUIRE(landed < after.size());
    SOL_CHECK(after[landed].corners == 5);

    // Every other point is exactly where it was.
    std::size_t matched = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (i == moved) {
            continue;
        }
        if (pointAt(after, before[i].position) < after.size()) {
            ++matched;
        }
    }
    SOL_CHECK(matched == 11);
}

// ⚑⚑ THE FIXED POINT, AND THE TESTS ABOVE ARE WHAT FOUND IT. A ForgePoint's
// position is float, because it was read out of the built mesh. A move that
// took a DESTINATION would rebuild every authored double from that rounded
// number, so clicking a vertex and letting go - a drag of zero distance -
// would rewrite `[-2.6, -1.3, -1.0]` as `[-2.5999999046325684, ...]` in five
// parts at once, and a modeller saving on every accepted edit would fill the
// file with noise nobody typed. A delta added to the value already in the
// document cannot do that, and this is the assertion that keeps it true.
// ⚑ E2 WIDENED IT TO ALL SIX, AND `cube.forge` IS WHY. E1 asserted this over
// `ship` and `asteroid`, both of which write down every number they use, so the
// only way to fail was arithmetic. The cube authors NOTHING - every parameter of
// its one box is at the schema default, which is the property that asset exists
// to demonstrate - and `ForgePart::set` adds a key that is not there. A write of
// `value + 0` would therefore MATERIALISE `center` and `size` into a four-line
// file on a click-and-release, so a write of no distance is not performed.
SOL_TEST(aMoveOfZeroDistanceLeavesTheFileByteForByteIdentical)
{
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid", "gate_membrane"};
    for (const char* name : names) {
        const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
        const std::string source = readWholeFile(path);
        SOL_CHECK(!source.empty());
        if (source.empty()) {
            continue;
        }
        ForgeDoc doc;
        SOL_REQUIRE(parses(source, doc));

        std::vector<assets::ForgePoint> points;
        SOL_REQUIRE(assets::forgePoints(doc, points));
        SOL_REQUIRE(!points.empty());

        // Every movable point, nudged by nothing at all.
        for (const assets::ForgePoint& point : points) {
            if (point.movable()) {
                SOL_REQUIRE(assets::forgeMovePoint(doc, point, {0.0, 0.0, 0.0}));
            }
        }
        SOL_CHECK(assets::writeForge(doc) == source);
    }
}

// ⚑ A torus ring vertex is a function of two segment indices and there is
// nothing authored to write, so the point is not movable and says so instead of
// silently doing nothing. This is the D checkpoint's bake rule meeting the two
// parts in this repo that actually need it - two out of sixty.
//
// ⚑ E2 IS WHAT MAKES THIS TEST WORTH KEEPING RATHER THAN A TAUTOLOGY. At E1 the
// station had NOTHING movable, so "the torus refuses" was indistinguishable
// from "everything refuses". Its nine boxes are class (2) now, so the file
// answers 72 of its 552 points and still refuses the other 480 - which is the
// bake rule holding a line rather than a whole asset sitting behind it.
SOL_TEST(aTorusPointHasNoParametricAnswerAndRefusesTheMoveUntilItIsBaked)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/station.forge";
    const std::string source = readWholeFile(path);
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(!points.empty());

    // 40 x 12 ring positions under 41 x 13 emitted vertices, plus nine boxes at
    // eight corners each.
    SOL_CHECK(points.size() == 552);
    std::size_t movable = 0;
    for (const assets::ForgePoint& point : points) {
        if (point.movable()) {
            ++movable;
        }
    }
    SOL_CHECK(movable == 72);

    // The torus is the first part in the file, so it emitted the first vertex.
    SOL_REQUIRE(doc.parts[0].primitive == ForgePrimitive::Torus);
    SOL_CHECK(!points[0].movable());
    SOL_CHECK(points[0].resolved == 0);
    std::string error;
    SOL_CHECK(!assets::forgeMovePoint(doc, points[0], {0.5, 0.0, 0.0}, &error));
    SOL_CHECK(!error.empty());

    // And a hub corner, which is class (2), moves - the same document, the same
    // call, a different answer because the primitive has one.
    const std::size_t hub = pointAt(points, {22.0, 30.0, 22.0});
    SOL_REQUIRE(hub < points.size());
    SOL_CHECK(points[hub].movable());
    SOL_CHECK(assets::forgeMovePoint(doc, points[hub], {1.0, 0.0, 0.0}, &error));
}

// ⚑⚑ THE MEMBRANE'S RIM IS THE APERTURE THE GAME TESTS, AND THIS IS WHAT PINS
// THEM TOGETHER. space_world.cpp's crossing test uses kGateRadiusMeters = 70 and
// gate.forge's ring is authored so its inner radius is exactly that (78 - 8).
// Phase 12 adds a THIRD thing that has to agree: a membrane of any other radius
// would draw a second aperture disagreeing with the one the mechanic uses, which
// is precisely the defect stage D closed when it found the old slab drawing
// +/-35 m against a test that accepted 70. The number is asserted rather than
// commented, because a comment does not fail.
SOL_TEST(theGateMembraneFillsExactlyTheApertureTheCrossingTestUses)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/gate_membrane.forge";
    const std::string source = readWholeFile(path);
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    assets::MeshData mesh;
    SOL_REQUIRE(assets::buildForge(doc, mesh, nullptr, nullptr));
    SOL_REQUIRE(!mesh.vertices.empty());

    // The disc is turned to stand in the lane, so its extent is in X and Y and
    // it is flat on Z - the same plane the ring occupies.
    double maxRadius = 0.0;
    double maxAbsZ = 0.0;
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        const double x = static_cast<double>(vertex.position[0]);
        const double y = static_cast<double>(vertex.position[1]);
        const double z = static_cast<double>(vertex.position[2]);
        maxRadius = std::max(maxRadius, std::sqrt((x * x) + (y * y)));
        maxAbsZ = std::max(maxAbsZ, std::abs(z));
    }
    SOL_CHECK(std::abs(maxRadius - 70.0) < 1e-3);
    SOL_CHECK(maxAbsZ < 1e-3);

    // ⚑⚑ EVERY NORMAL IS -Z, AND THE SIGN IS THE WHOLE LIGHTING STORY.
    //
    // A gate is placed with facingRotation(outward radial from the hub), which
    // takes the model's +Z onto that axis - so a -Z normal points back INWARD,
    // at the hub, where the star is. Lambert is max(dot(n, toSun), 0) and
    // toSun is that same inward direction, so the dot is +1.
    //
    // ⚑ The disc meets the sunlight dead-on rather than at an angle, and the
    // fragment shader does NOT flip the normal for back faces - so lambert is
    // all-or-nothing across BOTH faces together, not lit on one and black on
    // the other. Wind this profile the other way and the membrane goes black
    // from every angle at once, lit only by ambient and its emissive. That is
    // what the emissive floor is insurance against, and it is why the sign is
    // asserted here instead of being left to whoever next opens the asset.
    SOL_REQUIRE(mesh.vertices.size() > 1);
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        SOL_CHECK(std::abs(vertex.normal[0]) < 1e-3f);
        SOL_CHECK(std::abs(vertex.normal[1]) < 1e-3f);
        SOL_CHECK(std::abs(vertex.normal[2] + 1.0f) < 1e-3f);
    }
}

// A baked part's vertices ARE its authored numbers, so the asteroid - one
// `mesh` part of literal geometry - is fully movable without any further bake.
SOL_TEST(aBakedPartIsFullyMovableBecauseItsVerticesAreItsParameters)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/asteroid.forge";
    const std::string source = readWholeFile(path);
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_REQUIRE(doc.parts.size() == 1);
    SOL_REQUIRE(doc.parts[0].primitive == ForgePrimitive::Mesh);

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(points.size() > 100);
    for (const assets::ForgePoint& point : points) {
        SOL_CHECK(point.movable());
    }

    const assets::BuildPoint start = points[0].position;
    const assets::BuildPoint delta{0.25, -0.5, 0.125};
    SOL_REQUIRE(assets::forgeMovePoint(doc, points[0], delta));

    std::vector<assets::ForgePoint> after;
    // ⚑ To within the GRID, not exactly: Phase 14 snaps a dragged point to
    // 0.1 mm, and an asteroid vertex starts at a float value that is nowhere
    // near a grid line. Landing on the grid is the point of that phase, so the
    // cursor and the corner agree only to the grid step.
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(pointAt(after, {start.x + delta.x, start.y + delta.y, start.z + delta.z}, 1e-4) < after.size());
}

// ⚑ A part under a rotated, scaled parent must receive the number it would have
// been AUTHORED with, not the world number - otherwise the first drag on a
// child throws it across the frame by whatever its parent's transform was.
// Deliberately NOT a 90 degree rotation, which round-trips by luck.
SOL_TEST(aPointUnderATransformedParentIsWrittenBackInThePartsOwnFrame)
{
    const std::string source = "name = \"demo\"\n"
                               "\n[[part]]\nid = \"frame\"\ntype = \"group\"\n"
                               "position = [10.0, 0.0, -4.0]\n"
                               "rotation = [0.0, 30.0, 0.0]\n"
                               "scale = [2.0, 1.0, 0.5]\n"
                               "\n[[part]]\nid = \"tri\"\ntype = \"flat_triangle\"\n"
                               "parent = \"frame\"\n"
                               "p0 = [0.0, 0.0, 0.0]\np1 = [1.0, 0.0, 0.0]\np2 = [0.0, 1.0, 0.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(points.size() == 3);

    // Drag the p1 corner to a chosen point in the frame the mesh is built in.
    std::size_t target = points.size();
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (const assets::ForgePointWrite& write : points[i].writes) {
            if (write.param == "p1") {
                target = i;
            }
        }
    }
    SOL_REQUIRE(target < points.size());

    // Drag two metres along world +X. The parent is scaled 2x on X and turned
    // 30 degrees, so the authored number must move by neither two nor one.
    const assets::BuildPoint start = points[target].position;
    const assets::BuildPoint delta{2.0, 0.0, 0.0};
    SOL_REQUIRE(assets::forgeMovePoint(doc, points[target], delta));

    const assets::ForgeValue written = doc.parts[1].value("p1");
    SOL_CHECK(std::abs(written.vec.x - (1.0 + delta.x)) > 0.1);

    std::vector<assets::ForgePoint> after;
    // ⚑ The grid is a property of the AUTHORED number, not of the viewport, and
    // this is the test where the difference shows. Phase 14 snaps in the part's
    // OWN frame, because the number a person reads in the file is the local one
    // - so under a parent scaled 2x on X the world landing is off the cursor by
    // up to a grid step times that scale. Snapping in world space instead would
    // defeat the whole phase here: the inverse transform of a round world
    // number is an irrational local one.
    //
    // The bound is worked out rather than guessed: half a grid step is 5e-5 in
    // the part's frame, and the parent scales that by at most 2, so no world
    // component can be off by more than ~1.03e-4. Measured here: 4.4e-5.
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(pointAt(after, {start.x + delta.x, start.y + delta.y, start.z + delta.z}, 2e-4) < after.size());
}

// The whole stage rides on the writer staying a fixed point: a modeller saves
// on every accepted edit, so an edited document must write a file that differs
// only in the lines the edit touched - not the header, not the dividers, not
// the blank lines.
SOL_TEST(anEditedDocumentStillWritesEveryOtherLineUnchanged)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/ship.forge";
    const std::string source = readWholeFile(path);
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    const std::size_t moved = pointAt(points, {-2.6, -1.3, -1.0});
    SOL_REQUIRE(moved < points.size());
    SOL_REQUIRE(assets::forgeMovePoint(doc, points[moved], {-0.3, -0.15, 0.25}));

    const std::string written = assets::writeForge(doc);
    std::size_t differing = 0;
    std::istringstream before(source);
    std::istringstream after(written);
    std::string a;
    std::string b;
    while (std::getline(before, a) && std::getline(after, b)) {
        if (a != b) {
            ++differing;
        }
    }
    SOL_CHECK(differing == 5);
    SOL_CHECK(std::count(source.begin(), source.end(), '\n') ==
              std::count(written.begin(), written.end(), '\n'));
}

// ---------------------------------------------------------------------------
// Stage E2: class (2), where the corner is not a parameter and has to be
// solved for. 35 of the 60 geometry parts in `assets/meshes/`.
// ---------------------------------------------------------------------------

// ⚑⚑ THE SIGN TABLE PINNED AGAINST REAL GEOMETRY, NOT AGAINST ITSELF. A box
// corner is identified by WHICH of `addBox`'s 24 emitted vertices it is, which
// is exact and free - but it is only right while that face order is, and a
// table that agreed only with the code that wrote it would notice nothing if
// `mesh_build.cpp` were reordered. So the assertion is that the sign bits and
// the built position say the same thing about every one of the eight corners.
SOL_TEST(theBoxCornerSignCodeAgreesWithWhereAddBoxActuallyPutTheVertex)
{
    // Deliberately off the origin and unequal on every axis: a cube at the
    // origin would let a transposed table pass.
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"box\"\n"
                               "center = [3.0, -2.0, 7.0]\nsize = [2.0, 6.0, 10.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(points.size() == 8);

    std::uint32_t seen = 0;
    for (const assets::ForgePoint& point : points) {
        // ⚑ Three render vertices per corner and ONE write: the dedup that
        // class (1) never needed. Applying the drag once per corner would send
        // the point three times as far as the hand that moved it.
        SOL_CHECK(point.corners == 3);
        SOL_CHECK(point.resolved == 3);
        SOL_REQUIRE(point.writes.size() == 1);
        SOL_CHECK(point.writes[0].kind == assets::ForgeWriteKind::BoxCorner);
        SOL_CHECK(point.writes[0].param == "center+size");
        SOL_CHECK(point.movable());

        const std::uint32_t code = point.writes[0].element;
        SOL_REQUIRE(code < 8);
        seen |= 1u << code;
        const double x = 3.0 + ((code & 1u) != 0 ? 1.0 : -1.0);
        const double y = -2.0 + ((code & 2u) != 0 ? 3.0 : -3.0);
        const double z = 7.0 + ((code & 4u) != 0 ? 5.0 : -5.0);
        SOL_CHECK(std::abs(point.position.x - x) < 1e-5);
        SOL_CHECK(std::abs(point.position.y - y) < 1e-5);
        SOL_CHECK(std::abs(point.position.z - z) < 1e-5);
    }
    SOL_CHECK(seen == 0xFF); // all eight codes, each exactly once
}

// The same discipline for the beam: which END each of the 24 emitted vertices
// stands at, checked against where the vertex actually came out.
SOL_TEST(theBeamCornerEndAgreesWithWhichEndAddBeamActuallyPutTheVertexAt)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"beam\"\n"
                               "from = [0.0, 0.0, 0.0]\nto = [10.0, 0.0, 0.0]\n"
                               "width = 4.0\nheight = 4.0\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(points.size() == 8);

    std::size_t atFrom = 0;
    for (const assets::ForgePoint& point : points) {
        SOL_CHECK(point.corners == 3);
        SOL_REQUIRE(point.writes.size() == 1);
        SOL_CHECK(point.writes[0].kind == assets::ForgeWriteKind::BeamEnd);
        const bool nearFrom = point.position.x < 5.0;
        SOL_CHECK(point.writes[0].element == (nearFrom ? 0u : 1u));
        SOL_CHECK(point.writes[0].param == (nearFrom ? "from" : "to"));
        if (nearFrom) {
            ++atFrom;
        }
    }
    SOL_CHECK(atFrom == 4); // four corners ring each end
}

// ⚑⚑ THE SOLVE ITSELF, AND THE PIN IS NOT A RULE APPLIED ON TOP OF IT. Half the
// delta goes into `center` and the whole of it into `size`; at the far corner,
// which sits at `center - s*size/2`, those two halves cancel exactly. So the
// opposite corner staying still is what the arithmetic MEANS, and a resize is
// what an author dragging a box corner already expects to get.
SOL_TEST(aBoxCornerDragResizesTheBoxAndPinsTheOppositeCorner)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"box\"\n"
                               "center = [3.0, -2.0, 7.0]\nsize = [2.0, 6.0, 10.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> before;
    SOL_REQUIRE(assets::forgePoints(doc, before));
    const std::size_t corner = pointAt(before, {4.0, 1.0, 12.0});
    SOL_REQUIRE(corner < before.size());

    const assets::BuildPoint delta{0.5, 0.25, -0.125};
    std::string error;
    SOL_REQUIRE(assets::forgeMovePoint(doc, before[corner], delta, &error));
    SOL_CHECK(error.empty());

    const assets::BuildPoint center = doc.parts[0].value("center").vec;
    const assets::BuildPoint size = doc.parts[0].value("size").vec;
    SOL_CHECK(std::abs(center.x - 3.25) < 1e-12);
    SOL_CHECK(std::abs(center.y - -1.875) < 1e-12);
    SOL_CHECK(std::abs(center.z - 6.9375) < 1e-12);
    SOL_CHECK(std::abs(size.x - 2.5) < 1e-12);
    SOL_CHECK(std::abs(size.y - 6.25) < 1e-12);
    SOL_CHECK(std::abs(size.z - 9.875) < 1e-12);

    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(after.size() == 8); // still a box, not a box and a stray corner
    // The dragged corner is under the cursor...
    SOL_CHECK(pointAt(after, {4.5, 1.25, 11.875}) < after.size());
    // ...and the corner diagonally opposite has not moved at all.
    SOL_CHECK(pointAt(after, {2.0, -5.0, 2.0}) < after.size());
}

// ⚑⚑ PHASE 14'S OWN NOTE, AS A TEST, WITH THE NUMBERS THE PLAYTEST PRODUCED.
// A human dragged the shared apex of the cockpit's four cowl triangles, saved,
// and an authored `p2 = [0.0, 0.26, -6.95]` came back as
// `[0.009169150493107736, 0.3069186387490481, -7.117375393866678]`. The drag
// was a mouse ray cast against float geometry, so those digits were never
// precision - and `.forge` is text precisely so a person can read it.
//
// ⚑ The assertion is on the EMITTED TEXT as well as on the stored double,
// because the text is what the note was actually about.
SOL_TEST(aDraggedVertexLandsOnTheGridAndWritesANumberAPersonCanRead)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"cowl\"\n"
                               "type = \"flat_triangle\"\n"
                               "p0 = [-0.34, 0.34, -6.0]\np1 = [0.34, 0.34, -6.0]\n"
                               "p2 = [0.0, 0.26, -6.95]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    const std::size_t apex = pointAt(points, {0.0, 0.26, -6.95});
    SOL_REQUIRE(apex < points.size());

    // Exactly the displacement the playtest's saved file implies.
    const assets::BuildPoint delta{0.009169150493107736, 0.0469186387490481, -0.167375393866678};
    SOL_REQUIRE(assets::forgeMovePoint(doc, points[apex], delta));

    const assets::BuildPoint written = doc.parts[0].value("p2").vec;
    SOL_CHECK(std::abs(written.x - 0.0092) < 1e-12);
    SOL_CHECK(std::abs(written.y - 0.3069) < 1e-12);
    SOL_CHECK(std::abs(written.z - -7.1174) < 1e-12);

    const std::string text = assets::writeForge(doc);
    SOL_CHECK(text.find("p2 = [0.0092, 0.3069, -7.1174]") != std::string::npos);
    // And the untouched corners are still the author's own numbers.
    SOL_CHECK(text.find("p0 = [-0.34, 0.34, -6.0]") != std::string::npos);
}

// ⚑⚑ THE BOX'S THREE-WAY TRADE, ASSERTED FROM BOTH ENDS AT ONCE. `center` and
// `size` encode eight corners between them, so rounding the two independently
// would drag the corner the author is NOT holding. Phase 14 solves against the
// pinned corner instead, which makes this test the one that would catch a
// regression to the easy implementation: the dragged corner lands on the grid
// AND the opposite corner has not moved, at the same strength the pin was
// asserted at before the grid existed.
SOL_TEST(aQuantizedBoxCornerDragStillLeavesTheOppositeCornerExactlyWhereItWas)
{
    // Deliberately off-grid extents, so the solve has something to round.
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"box\"\n"
                               "center = [0.0, 0.0, 0.0]\nsize = [1.0, 1.0, 1.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> before;
    SOL_REQUIRE(assets::forgePoints(doc, before));
    const std::size_t corner = pointAt(before, {0.5, 0.5, 0.5});
    SOL_REQUIRE(corner < before.size());

    // An awkward drag of the kind a hand on a mouse actually produces.
    const assets::BuildPoint delta{0.318309886183791, -0.271828182845905, 0.141421356237309};
    SOL_REQUIRE(assets::forgeMovePoint(doc, before[corner], delta));

    // The dragged corner is on the grid...
    const assets::BuildPoint want{0.8183, 0.2282, 0.6414};
    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(pointAt(after, want, 1e-4) < after.size());

    // ...and the corner diagonally opposite has not moved AT ALL, at the same
    // 1e-6 the pin was asserted at before this phase.
    SOL_CHECK(pointAt(after, {-0.5, -0.5, -0.5}) < after.size());

    const assets::BuildPoint size = doc.parts[0].value("size").vec;
    SOL_CHECK(std::abs(size.x - 1.3183) < 1e-12);
    SOL_CHECK(std::abs(size.y - 0.7282) < 1e-12);
    SOL_CHECK(std::abs(size.z - 1.1414) < 1e-12);

    // ⚑⚑ AND `center` TOO, WHICH IS THE HALF A LIVE DRIVE CAUGHT AND THE UNIT
    // tests had missed. Solving `center` back from the rounded corner gave the
    // RIGHT value in an unreadable form - `0.07840000000000003` beside a clean
    // `size` - because `(dragged + pinned) / 2` is not the double nearest the
    // decimal. Rounding the STEP instead leaves it one exact halving from a
    // number the author wrote. Asserting the TEXT is what makes this bite.
    const assets::BuildPoint center = doc.parts[0].value("center").vec;
    SOL_CHECK(std::abs(center.x - 0.15915) < 1e-12);
    SOL_CHECK(std::abs(center.y - -0.1359) < 1e-12);
    SOL_CHECK(std::abs(center.z - 0.0707) < 1e-12);

    const std::string text = assets::writeForge(doc);
    SOL_CHECK(text.find("center = [0.15915, -0.1359, 0.0707]") != std::string::npos);
    SOL_CHECK(text.find("size = [1.3183, 0.7282, 1.1414]") != std::string::npos);
}

// ⚑⚑ THE EXACT CASE A LIVE DRIVE FOUND TWICE AND THE UNIT TESTS MISSED, PINNED
// HERE SO IT CANNOT COME BACK. Dragging `cube.forge`'s one box - the asset that
// authors NOTHING, so a save writes precisely `center` and `size` - produced
// `size = [1.1567999999999996, ...]` beside a clean `center`, and on the
// previous attempt a clean `size` beside `center = 0.07840000000000003`.
//
// ⚑ Both are the same fact: 1.0 + 0.1568 is NOT the double nearest 1.1568.
// Adding a small number to a larger one shifts the exponent, so the sum lands a
// ULP off the decimal and `appendNumber` honestly spells all seventeen digits.
// Two clean decimals do not add to a clean decimal in binary - which is why the
// step is rounded going in AND the result is rounded coming out.
//
// ⚑ The numbers below are the ones the Forge actually wrote, not invented ones.
SOL_TEST(aBoxDragWritesCleanDecimalsEvenWhereTheAdditionIsNot)
{
    const std::string source = "name = \"cube\"\n\n[[part]]\nid = \"box\"\ntype = \"box\"\n"
                               "center = [0.0, 0.0, 0.0]\nsize = [1.0, 1.0, 1.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    const std::size_t corner = pointAt(points, {0.5, 0.5, 0.5});
    SOL_REQUIRE(corner < points.size());

    // The drag the drive performed, to the digit.
    SOL_REQUIRE(assets::forgeMovePoint(
        doc, points[corner], {0.15680000000000005, -0.11239999999999994, -0.07820000000000004}));

    const std::string text = assets::writeForge(doc);
    SOL_CHECK(text.find("center = [0.0784, -0.0562, -0.0391]") != std::string::npos);
    SOL_CHECK(text.find("size = [1.1568, 0.8876, 0.9218]") != std::string::npos);

    // The pin still holds, which is what the rounding must not have cost.
    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(pointAt(after, {-0.5, -0.5, -0.5}) < after.size());
}

// ⚑ The zero-delta rule of E2, extended from EXACTLY ZERO to BELOW THE GRID. A
// hand on a mouse does not hold still, so a click that "did not move" can carry
// a few microns - and a tool that saves after every edit would write that into
// a file a person maintains. The document must come back byte-identical.
SOL_TEST(aDragFinerThanTheGridWritesNothingAtAll)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"t\"\n"
                               "type = \"flat_triangle\"\n"
                               "p0 = [0.0, 0.0, 0.0]\np1 = [1.0, 0.0, 0.0]\n"
                               "p2 = [0.0, 1.0, 0.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));
    const std::string before = assets::writeForge(doc);

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    const std::size_t corner = pointAt(points, {1.0, 0.0, 0.0});
    SOL_REQUIRE(corner < points.size());

    // Two hundredths of a millimetre, well inside one grid step.
    const assets::BuildPoint delta{2e-5, -1e-5, 3e-5};
    SOL_REQUIRE(assets::forgeMovePoint(doc, points[corner], delta));

    SOL_CHECK(assets::writeForge(doc) == before);
}

// The same guarantee on the other two kinds whose parameter IS the point, so
// neither is left behind: a beam end, and a baked vertex.
SOL_TEST(aDraggedBeamEndAndABakedVertexBothLandOnTheGrid)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"beam\"\n"
                               "from = [0.0, 0.0, 0.0]\nto = [0.0, 2.0, 0.0]\n"
                               "width = 0.05\nheight = 0.05\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(!points.empty());

    // Any corner at the `to` cap re-aims that end.
    std::size_t corner = points.size();
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (const assets::ForgePointWrite& write : points[i].writes) {
            if (write.kind == assets::ForgeWriteKind::BeamEnd && write.element == 1) {
                corner = i;
            }
        }
    }
    SOL_REQUIRE(corner < points.size());
    SOL_REQUIRE(assets::forgeMovePoint(doc, points[corner], {0.123456789, 0.0, 0.098765432}));

    const assets::BuildPoint end = doc.parts[0].value("to").vec;
    SOL_CHECK(std::abs(end.x - 0.1235) < 1e-12);
    SOL_CHECK(std::abs(end.z - 0.0988) < 1e-12);
    SOL_CHECK(assets::writeForge(doc).find("0.1235") != std::string::npos);
}

// ⚑⚑ THE CONSEQUENCE THAT HAS TO BE SAID OUT LOUD RATHER THAN DISCOVERED. A
// beam has NO authored corners - `side` and `up` are derived from its axis - so
// the only answer to a dragged corner is to move the end it stands at. Two
// things follow, and both look like bugs to anyone who has not been told: the
// other three corners at that end come with it, and the four at the PINNED end
// swing as the cross-section is re-derived from the new axis. This test uses a
// deliberately fat section so the swing is large enough to assert.
SOL_TEST(aBeamCornerDragReAimsTheEndItStandsAtAndSwingsTheFarEndWithIt)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"beam\"\n"
                               "from = [0.0, 0.0, 0.0]\nto = [10.0, 0.0, 0.0]\n"
                               "width = 4.0\nheight = 4.0\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> before;
    SOL_REQUIRE(assets::forgePoints(doc, before));
    const std::size_t corner = pointAt(before, {0.0, -2.0, -2.0});
    SOL_REQUIRE(corner < before.size());

    const assets::BuildPoint delta{0.0, 5.0, 0.0};
    SOL_REQUIRE(assets::forgeMovePoint(doc, before[corner], delta));

    // `from` took the whole delta and `to` was not touched.
    const assets::BuildPoint from = doc.parts[0].value("from").vec;
    const assets::BuildPoint to = doc.parts[0].value("to").vec;
    SOL_CHECK(std::abs(from.x - 0.0) < 1e-12);
    SOL_CHECK(std::abs(from.y - 5.0) < 1e-12);
    SOL_CHECK(std::abs(from.z - 0.0) < 1e-12);
    SOL_CHECK(std::abs(to.x - 10.0) < 1e-12);
    SOL_CHECK(std::abs(to.y - 0.0) < 1e-12);
    SOL_CHECK(std::abs(to.z - 0.0) < 1e-12);

    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(after.size() == 8);
    // ⚑ The dragged corner does NOT land under the cursor, and that is the
    // documented price rather than a defect: it is offset by the rotation of a
    // cross-section it does not own, bounded by the section's half-diagonal
    // (2.83 m here, 0.05 m on every beam in `cockpit.forge`). Asserted so that
    // nobody later "fixes" it by writing a corner that has nowhere to live.
    SOL_CHECK(pointAt(after, {0.0, 3.0, -2.0}, 1e-3) == after.size());
    // And the far end's corners moved too, though `to` never changed.
    SOL_CHECK(pointAt(after, {10.0, 2.0, 2.0}, 1e-3) == after.size());
}

// ⚑ A guard for a state the primitive answers in SILENCE. Push a box's size
// through zero and `addBox` still builds it - inside out, because its six face
// normals are authored constants, so every face is lit as though it faced into
// the solid. A build that succeeds cannot report this, so the move refuses.
SOL_TEST(aDragThroughABoxesOwnOppositeCornerIsRefusedRatherThanTurningItInsideOut)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"box\"\n"
                               "center = [3.0, -2.0, 7.0]\nsize = [2.0, 6.0, 10.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    const std::size_t corner = pointAt(points, {4.0, 1.0, 12.0});
    SOL_REQUIRE(corner < points.size());

    // The box is 2 m on X; pulling that corner 3 m in -X puts it a metre past
    // the face it was pinned against.
    std::string error;
    SOL_CHECK(!assets::forgeMovePoint(doc, points[corner], {-3.0, 0.0, 0.0}, &error));
    SOL_CHECK(!error.empty());
    // Refused means UNTOUCHED - not a partial write, and not a document that
    // has to be undone to get back to where it was.
    SOL_CHECK(doc.parts[0].value("size").vec.x == 2.0);
    SOL_CHECK(doc.parts[0].value("center").vec.x == 3.0);

    // A metre and a half is fine: it leaves half a metre of box.
    SOL_CHECK(assets::forgeMovePoint(doc, points[corner], {-1.5, 0.0, 0.0}, &error));
    SOL_CHECK(std::abs(doc.parts[0].value("size").vec.x - 0.5) < 1e-12);
}

// The same for the beam, and its silence is worse: `addBeam` RETURNS WITHOUT
// EMITTING ANYTHING at zero length, so the part would leave the mesh with no
// error raised anywhere and the file would still parse, build and save.
SOL_TEST(aDragThatCollapsesABeamOntoItsOtherEndIsRefused)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"beam\"\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(points.size() == 8);
    const std::size_t corner = pointAt(points, {-0.05, 0.0, -0.05});
    SOL_REQUIRE(corner < points.size());

    // The default beam runs one metre up +Y.
    std::string error;
    SOL_CHECK(!assets::forgeMovePoint(doc, points[corner], {0.0, 1.0, 0.0}, &error));
    SOL_CHECK(!error.empty());
    SOL_CHECK(doc.parts[0].find("from") == nullptr); // still authoring nothing
}

// ⚑ A box already flat on an axis has two corners standing in the same place,
// so there is no opposite corner to pin - it has no honest class (2) answer and
// routes to the bake, exactly as a torus ring does. The threshold is the
// caller's own point tolerance rather than a second number of this function's.
SOL_TEST(aBoxFlatOnAnAxisHasNoOppositeCornerToPinAndGoesToTheBake)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"b\"\ntype = \"box\"\n"
                               "center = [0.0, 0.0, 0.0]\nsize = [2.0, 0.0, 10.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    SOL_REQUIRE(points.size() == 4); // eight corners collapsed onto four places
    for (const assets::ForgePoint& point : points) {
        SOL_CHECK(point.corners == 6);
        SOL_CHECK(point.resolved == 0);
        SOL_CHECK(point.writes.empty());
        SOL_CHECK(!point.movable());
    }

    std::string error;
    SOL_CHECK(!assets::forgeMovePoint(doc, points[0], {0.0, 1.0, 0.0}, &error));
    SOL_CHECK(!error.empty());
}

// ⚑ One point, two boxes, and both are resized by one drag. This is the same
// property class (1) has on `ship.forge`'s shared corner - one drag, every
// write - reaching the class where the corner is not a parameter at all. Two
// boxes butted together are how a seam gets opened by a tool that writes one
// of them, and the point count is the assertion: a partial write splits one
// point into two.
SOL_TEST(aCornerSharedByTwoBoxesResizesBothInOneDrag)
{
    const std::string source = "name = \"demo\"\n\n[[part]]\nid = \"left\"\ntype = \"box\"\n"
                               "center = [-1.0, 0.0, 0.0]\nsize = [2.0, 2.0, 2.0]\n"
                               "\n[[part]]\nid = \"right\"\ntype = \"box\"\n"
                               "center = [1.0, 0.0, 0.0]\nsize = [2.0, 2.0, 2.0]\n";
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> before;
    SOL_REQUIRE(assets::forgePoints(doc, before));
    // Twelve: eight each, less the four they share on the x = 0 plane.
    SOL_REQUIRE(before.size() == 12);

    const std::size_t shared = pointAt(before, {0.0, 1.0, 1.0});
    SOL_REQUIRE(shared < before.size());
    SOL_CHECK(before[shared].corners == 6);
    SOL_CHECK(before[shared].resolved == 6);
    SOL_REQUIRE(before[shared].writes.size() == 2); // one per box, not one per corner

    SOL_REQUIRE(assets::forgeMovePoint(doc, before[shared], {0.0, 0.5, 0.0}));

    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(after.size() == 12); // a write that reached one box would make 13
    SOL_CHECK(pointAt(after, {0.0, 1.5, 1.0}) < after.size());
    // Both boxes grew on Y, and each pinned its own far corner.
    SOL_CHECK(std::abs(doc.parts[0].value("size").vec.y - 2.5) < 1e-12);
    SOL_CHECK(std::abs(doc.parts[1].value("size").vec.y - 2.5) < 1e-12);
    SOL_CHECK(pointAt(after, {-2.0, -1.0, -1.0}) < after.size());
    SOL_CHECK(pointAt(after, {2.0, -1.0, -1.0}) < after.size());
}

// ⚑⚑ THE CENSUS, MEASURED OVER THE COMMITTED ASSETS RATHER THAN OVER FIXTURES.
// E2's claim is that class (2) reaches 35 of the 60 geometry parts and that
// what is left refusing is exactly the two toruses. These are the numbers that
// claim comes to, and `cockpit.forge` is the one that matters: ten beams, seven
// boxes and six triangles, with nothing left over - the asset the stage was
// picked to make editable.
SOL_TEST(everyCommittedAssetButTheTwoTorusesIsNowFullyMovable)
{
    struct Expected
    {
        const char* name;
        std::size_t points;
        std::size_t movable;
    };

    const Expected expected[] = {
        {"cube", 8, 8},         // one box at its defaults
        {"gate", 320, 64},      // a 32x8 ring plus eight boxes
        {"ship", 12, 12},       // sixteen flat triangles: class (1), from E1
        {"station", 552, 72},   // a 40x12 ring plus nine boxes
        {"cockpit", 141, 141},  // ten beams, seven boxes, six triangles
        {"asteroid", 162, 162}, // baked: its vertices ARE its parameters
    };
    for (const Expected& want : expected) {
        const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + want.name + ".forge";
        const std::string source = readWholeFile(path);
        SOL_CHECK(!source.empty());
        if (source.empty()) {
            continue;
        }
        ForgeDoc doc;
        SOL_REQUIRE(parses(source, doc));

        std::vector<assets::ForgePoint> points;
        SOL_REQUIRE(assets::forgePoints(doc, points));
        std::size_t movable = 0;
        for (const assets::ForgePoint& point : points) {
            if (point.movable()) {
                ++movable;
            }
            // Whatever the class, a resolved corner count never exceeds the
            // corners standing there - the invariant `movable()` now rests on.
            SOL_CHECK(point.resolved <= point.corners);
        }
        SOL_CHECK(points.size() == want.points);
        SOL_CHECK(movable == want.movable);
    }
}

// ---------------------------------------------------------------------------
// Stage E3: class (3), which has no parametric answer at all and gets one by
// being baked - the D checkpoint's rule, per part.
// ---------------------------------------------------------------------------

// ⚑⚑ THE ASSERTION THAT DECIDED THE DESIGN, AND IT WAS WRITTEN BEFORE THE CODE.
// Stage E left one question open: bake in the part's own LOCAL frame and keep
// the tree, or fold the WORLD transform in and re-hang at the root. The answer
// is a third option - fold the part's OWN placement and keep the parent - and
// this is what makes it more than a preference. A bake that changed one bit of
// one asset would mean the tool silently re-authors a shipped mesh the first
// time anyone touches a vertex on it.
SOL_TEST(bakingAnyPartOfAnyCommittedAssetLeavesTheBuiltMeshUnchanged)
{
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid", "gate_membrane"};
    for (const char* name : names) {
        const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
        const std::string source = readWholeFile(path);
        SOL_CHECK(!source.empty());
        if (source.empty()) {
            continue;
        }
        ForgeDoc original;
        SOL_REQUIRE(parses(source, original));

        assets::MeshData before;
        SOL_REQUIRE(assets::buildForge(original, before));

        for (std::size_t i = 0; i < original.parts.size(); ++i) {
            if (original.parts[i].primitive == ForgePrimitive::Group) {
                continue;
            }
            ForgeDoc doc = original;
            std::string error;
            SOL_REQUIRE(assets::forgeBakeDocumentPart(doc, i, &error));
            SOL_CHECK(error.empty());
            SOL_CHECK(doc.parts[i].primitive == ForgePrimitive::Mesh);

            assets::MeshData after;
            SOL_REQUIRE(assets::buildForge(doc, after));
            SOL_REQUIRE(after.vertices.size() == before.vertices.size());
            SOL_REQUIRE(after.indices.size() == before.indices.size());
            // Byte for byte, not nearly: positions, normals and uvs all.
            SOL_CHECK(std::memcmp(after.vertices.data(),
                                  before.vertices.data(),
                                  before.vertices.size() * sizeof(assets::MeshVertex)) == 0);
            SOL_CHECK(std::memcmp(after.indices.data(),
                                  before.indices.data(),
                                  before.indices.size() * sizeof(std::uint32_t)) == 0);
        }
    }
}

// ⚑ `gate.forge` is the whole reason the frame question was real: it is the
// only asset with non-identity placements - a ring at 90 degrees about X and
// six arms and housings at 90, 180 and 270 about Z - and none of those angles
// is exact in radians. The test above covers it, and this one says out loud
// which case it is, so a future reader does not have to rediscover that the
// other five assets could not have distinguished the options.
SOL_TEST(theGateIsTheOnlyAssetWhosePlacementsCouldTellTheBakeFramesApart)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/gate.forge");
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::size_t turned = 0;
    std::size_t groups = 0;
    for (const assets::ForgePart& part : doc.parts) {
        if (part.rotationDegrees.x != 0.0 || part.rotationDegrees.y != 0.0 || part.rotationDegrees.z != 0.0) {
            ++turned;
        }
        if (part.primitive == ForgePrimitive::Group) {
            ++groups;
            // ⚑ The fact the whole decision rests on: the one group in this
            // repo is at the identity, so folding the part's own placement is
            // bit-exact rather than merely close.
            SOL_CHECK(part.localTransform().isIdentity());
        }
    }
    SOL_CHECK(turned == 7); // the ring plus six of the eight arms and housings
    SOL_CHECK(groups == 1);
}

// ⚑ A bake that dropped the comment above the part would be the comment-
// preserving writer's defect one function over - and in this repo a `[[part]]`
// comment is where the knowledge lives (`gate.forge` explains its quarter turn
// in one, `station.forge` its four spokes).
//
// ⚑ IT TAKES TWO PARTS TO ASSERT, BECAUSE NOT ONE PART IN THIS REPO CARRIES
// BOTH A PARENT AND A COMMENT. A comment block introduces a GROUP of parts and
// therefore sits above the first of them - which, for `gate.forge`'s armature,
// is the group itself. So `ring` carries the comment and `arm_east` carries the
// parent, and the bake has to keep whichever it is handed.
SOL_TEST(bakingAPartKeepsItsIdItsParentAndTheCommentAboveIt)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/gate.forge");
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    const std::size_t ring = doc.indexOf("ring");
    const std::size_t arm = doc.indexOf("arm_east");
    SOL_REQUIRE(ring < doc.parts.size());
    SOL_REQUIRE(arm < doc.parts.size());
    const std::string ringLeading = doc.parts[ring].leading;
    const std::string armLeading = doc.parts[arm].leading;
    const std::string parent = doc.parts[arm].parent;
    SOL_REQUIRE(ringLeading.find('#') != std::string::npos); // it really does carry one
    SOL_REQUIRE(!parent.empty());
    // The ring is turned a quarter turn; the arm hangs off a group.
    SOL_REQUIRE(!doc.parts[ring].localTransform().isIdentity());

    SOL_REQUIRE(assets::forgeBakeDocumentPart(doc, ring));
    SOL_REQUIRE(assets::forgeBakeDocumentPart(doc, arm));

    SOL_CHECK(doc.parts[ring].id == "ring");
    SOL_CHECK(doc.parts[ring].leading == ringLeading);
    SOL_CHECK(doc.parts[arm].id == "arm_east");
    SOL_CHECK(doc.parts[arm].parent == parent);
    SOL_CHECK(doc.parts[arm].leading == armLeading);
    // The placement was consumed into the geometry, so it is the identity now.
    SOL_CHECK(doc.parts[ring].localTransform().isIdentity());
    SOL_CHECK(doc.parts[arm].localTransform().isIdentity());

    // And it still writes and re-reads as the same document.
    const std::string written = assets::writeForge(doc);
    ForgeDoc reread;
    SOL_REQUIRE(parses(written, reread));
    SOL_CHECK(reread.parts[ring].leading == ringLeading);
    SOL_CHECK(reread.parts[arm].leading == armLeading);
    SOL_CHECK(reread.parts[arm].parent == parent);
}

// ⚑⚑ THE PAYOFF, AND IT IS THE D CHECKPOINT'S RULE CLOSING ITS OWN LOOP. A
// torus ring vertex is a function of two segment indices, so E1 and E2 both
// refused it and both said "bake its part first". Baking the ring is what makes
// that sentence true rather than a deferral: station.forge goes from 72 movable
// points to all 552, on the same document, through the same call.
SOL_TEST(bakingTheTorusIsWhatMakesTheRestOfTheStationMovable)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/station.forge");
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_REQUIRE(doc.parts[0].primitive == ForgePrimitive::Torus);

    std::vector<assets::ForgePoint> before;
    SOL_REQUIRE(assets::forgePoints(doc, before));
    SOL_REQUIRE(before.size() == 552);
    std::size_t movable = 0;
    for (const assets::ForgePoint& point : before) {
        if (point.movable()) {
            ++movable;
        }
    }
    SOL_CHECK(movable == 72);
    // The refusal E1 shipped, still refusing.
    std::string error;
    SOL_CHECK(!assets::forgeMovePoint(doc, before[0], {1.0, 0.0, 0.0}, &error));
    SOL_CHECK(!error.empty());

    error.clear();
    SOL_REQUIRE(assets::forgeBakeDocumentPart(doc, 0, &error));
    SOL_CHECK(error.empty());

    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    // ⚑ The same 552 points: the bake changed what can be WRITTEN, not what is
    // drawn, which is the whole claim of the assertion above.
    SOL_REQUIRE(after.size() == 552);
    for (const assets::ForgePoint& point : after) {
        SOL_CHECK(point.movable());
    }

    // And the ring point that refused a moment ago now moves.
    const std::size_t ring = pointAt(after, before[0].position);
    SOL_REQUIRE(ring < after.size());
    SOL_REQUIRE(assets::forgeMovePoint(doc, after[ring], {1.0, 0.0, 0.0}, &error));
    std::vector<assets::ForgePoint> moved;
    SOL_REQUIRE(assets::forgePoints(doc, moved));
    SOL_CHECK(pointAt(moved, {before[0].position.x + 1.0, before[0].position.y, before[0].position.z}) <
              moved.size());
}

// Baking twice is baking once: a `mesh` part is already literal, and a round
// trip through the builder could only lose bits it has no reason to spend.
SOL_TEST(bakingAnAlreadyBakedPartIsANoOpRatherThanARoundTrip)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/gate.forge");
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    const std::size_t ring = doc.indexOf("ring");
    SOL_REQUIRE(ring < doc.parts.size());
    SOL_REQUIRE(assets::forgeBakeDocumentPart(doc, ring));
    const std::string once = assets::writeForge(doc);

    SOL_REQUIRE(assets::forgeBakeDocumentPart(doc, ring));
    SOL_CHECK(assets::writeForge(doc) == once);
}

// A group carries no geometry, and an index past the end names nothing. Both
// say so rather than baking an empty part into the document.
SOL_TEST(bakingRefusesAGroupAndAPartThatIsNotThere)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/gate.forge");
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    const std::size_t armature = doc.indexOf("armature");
    SOL_REQUIRE(armature < doc.parts.size());
    SOL_REQUIRE(doc.parts[armature].primitive == ForgePrimitive::Group);

    std::string error;
    SOL_CHECK(!assets::forgeBakeDocumentPart(doc, armature, &error));
    SOL_CHECK(!error.empty());
    SOL_CHECK(doc.parts[armature].primitive == ForgePrimitive::Group);

    error.clear();
    SOL_CHECK(!assets::forgeBakeDocumentPart(doc, doc.parts.size(), &error));
    SOL_CHECK(!error.empty());
}

// ⚑⚑ STAGE E4. The edge list, in the numbering the tool already writes through.
//
// The stage's own one-line estimate said edge picking was "a projection over
// data that exists", meaning `MeshAdjacency::edges`. The data exists and it is
// indexed in a DIFFERENT numbering: `toEditMesh` welds with a spatial hash and
// `removeUnused` renumbers, while `forgePoints` welds with its own linear scan
// over every built vertex. This asserts the two agree about the TOPOLOGY - the
// same edges between the same places - which is what makes `forgeTopology` a
// re-expression rather than a second opinion, without ever assuming the two
// index spaces are the same integers.
SOL_TEST(everyCommittedAssetsEdgesAgreeWithAdjacencyAboutTheTopology)
{
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid", "gate_membrane"};
    for (const char* name : names) {
        const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
        const std::string source = readWholeFile(path);
        SOL_REQUIRE(!source.empty());
        ForgeDoc doc;
        SOL_REQUIRE(parses(source, doc));

        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeEdge> edges;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

        assets::MeshData mesh;
        SOL_REQUIRE(assets::buildForge(doc, mesh));
        const assets::EditMesh edit = assets::toEditMesh(mesh);
        const assets::MeshAdjacency adjacency = assets::buildAdjacency(edit);

        // The same number of edges, over the same number of points, however
        // each side numbered them.
        SOL_CHECK(points.size() == edit.positions.size());
        SOL_CHECK(edges.size() == adjacency.edges.size());

        // And every edge stands between the same two PLACES, compared as
        // geometry rather than as indices. Position pairs, ordered within the
        // pair, then sorted - so the two lists are equal as sets or they are not.
        const auto keyOf = [](assets::BuildPoint p, assets::BuildPoint q) {
            std::array<double, 6> key{p.x, p.y, p.z, q.x, q.y, q.z};
            if (std::tuple{q.x, q.y, q.z} < std::tuple{p.x, p.y, p.z}) {
                key = {q.x, q.y, q.z, p.x, p.y, p.z};
            }
            return key;
        };
        std::vector<std::array<double, 6>> mine;
        mine.reserve(edges.size());
        for (const assets::ForgeEdge& edge : edges) {
            SOL_REQUIRE(edge.a < points.size() && edge.b < points.size());
            SOL_CHECK(edge.a < edge.b);
            SOL_CHECK(edge.faceCount >= 1);
            mine.push_back(keyOf(points[edge.a].position, points[edge.b].position));
        }
        std::vector<std::array<double, 6>> theirs;
        theirs.reserve(adjacency.edges.size());
        for (const assets::MeshAdjacency::Edge& edge : adjacency.edges) {
            const core::Vec3 a = edit.positions[edge.a];
            const core::Vec3 b = edit.positions[edge.b];
            theirs.push_back(keyOf({a.x, a.y, a.z}, {b.x, b.y, b.z}));
        }
        std::sort(mine.begin(), mine.end());
        std::sort(theirs.begin(), theirs.end());
        SOL_CHECK(mine == theirs);
        if (mine != theirs) {
            std::printf("  %s: %zu edges vs %zu\n", name, mine.size(), theirs.size());
        }
    }
}

// The numbers, on the asset whose numbers a person can check by hand. A cube is
// 8 points, 12 triangles and 18 edges, and every edge of a closed solid carries
// exactly two faces - which is also Euler: 8 - 18 + 12 = 2.
SOL_TEST(theCubesEdgesAreEighteenAndEveryOneCarriesTwoFaces)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/cube.forge");
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    SOL_CHECK(points.size() == 8);
    SOL_CHECK(edges.size() == 18);
    for (const assets::ForgeEdge& edge : edges) {
        SOL_CHECK(edge.faceCount == 2);
    }

    // Sorted and unique, which is what lets a pick binary-search it later and
    // what proves the run-length pass collapsed the duplicates rather than
    // emitting one entry per face.
    for (std::size_t i = 1; i < edges.size(); ++i) {
        SOL_CHECK(std::tuple{edges[i - 1].a, edges[i - 1].b} < std::tuple{edges[i].a, edges[i].b});
    }
}

// ⚑ `gate_membrane.forge` is a revolve that touches its own axis, so it fans to
// a point and carries exactly one degenerate face per segment - 32 of them,
// which Phase 16 measured and pinned. A degenerate face's collapsed side is not
// an edge: it joins a point to itself, has no length, and could never be picked
// or dragged. It contributes nothing here rather than 32 zero-length entries.
SOL_TEST(theMembranesDegenerateFacesContributeNoEdges)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/gate_membrane.forge";
    const std::string source = readWholeFile(path);
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    for (const assets::ForgeEdge& edge : edges) {
        SOL_CHECK(edge.a != edge.b);
    }

    // The film is open, so it has a border - and a border edge carries one face
    // where a closed solid's carries two. That it has any at all is the honest
    // description of a surface with an outline, and it is why the closed-solid
    // invariants exclude this asset by name.
    std::uint32_t border = 0;
    for (const assets::ForgeEdge& edge : edges) {
        border += static_cast<std::uint32_t>(edge.faceCount == 1);
    }
    SOL_CHECK(border > 0);
}

// The same refusal `forgePoints` gives, for the same reason: a `[build]`
// post-pass renumbers vertices, so a built index no longer names a part and
// neither the points nor the edges over them mean anything.
SOL_TEST(topologyRefusesADocumentWhoseBuildPassRenumbersVertices)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/cube.forge");
    SOL_REQUIRE(!source.empty());
    ForgeDoc doc;
    SOL_REQUIRE(parses(source, doc));

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));
    SOL_CHECK(!edges.empty());
    SOL_CHECK(!faces.empty());

    doc.build.optimize = true;
    std::string error;
    SOL_CHECK(!assets::forgeTopology(doc, points, edges, faces, &error));
    SOL_CHECK(!error.empty());
    SOL_CHECK(points.empty());
    SOL_CHECK(edges.empty());
    SOL_CHECK(faces.empty());
}

// --- E4d: faces, the ray, and the widening a box makes compulsory ------------

namespace {

ForgeDoc openTopologyAsset(const char* name)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge");
    ForgeDoc doc;
    if (source.empty() || !parses(source, doc)) {
        return {};
    }
    return doc;
}

// The distinct points a face group stands on - the set a face drag hands to
// `forgeMovePoints`. Deduplicated by INDEX, because two triangles of a quad
// name their shared pair twice and a set with a repeat is a doubled write.
std::vector<assets::ForgePoint> pointsOfGroup(const std::vector<assets::ForgePoint>& points,
                                              const std::vector<assets::ForgeFace>& faces,
                                              const std::vector<std::uint32_t>& group)
{
    std::vector<std::uint32_t> corners;
    for (const std::uint32_t index : group) {
        const std::uint32_t three[3] = {faces[index].a, faces[index].b, faces[index].c};
        for (const std::uint32_t corner : three) {
            if (std::find(corners.begin(), corners.end(), corner) == corners.end()) {
                corners.push_back(corner);
            }
        }
    }
    std::vector<assets::ForgePoint> set;
    set.reserve(corners.size());
    for (const std::uint32_t corner : corners) {
        set.push_back(points[corner]);
    }
    return set;
}

} // namespace

// The cube's face count is the arithmetic the whole stage rests on: 6 quads, 12
// triangles, and Euler holding at 8 - 18 + 12 = 2.
SOL_TEST(theCubesFacesAreItsTwelveTrianglesInThePointNumbering)
{
    ForgeDoc doc = openTopologyAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    SOL_CHECK(points.size() == 8);
    SOL_CHECK(edges.size() == 18);
    SOL_CHECK(faces.size() == 12);
    // V - E + F = 2 for any closed surface of genus zero. It is the cheapest
    // cross-check there is that the three lists describe ONE mesh.
    SOL_CHECK((static_cast<int>(points.size()) - static_cast<int>(edges.size()) +
               static_cast<int>(faces.size())) == 2);
    for (const assets::ForgeFace& face : faces) {
        SOL_CHECK(face.a < points.size());
        SOL_CHECK(face.b < points.size());
        SOL_CHECK(face.c < points.size());
        SOL_CHECK(face.a != face.b);
        SOL_CHECK(face.b != face.c);
        SOL_CHECK(face.a != face.c);
    }
}

// ⚑ The same exclusion the edges get, measured from the other side. Phase 16
// pinned `gate_membrane` at exactly 32 degenerate faces; they are the ones whose
// corners do not weld to three distinct points, so the face list is exactly 32
// short of the triangle count.
SOL_TEST(theMembranesThirtyTwoDegenerateFacesAreNotInTheFaceList)
{
    ForgeDoc doc = openTopologyAsset("gate_membrane");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    assets::MeshData mesh;
    SOL_REQUIRE(assets::buildForge(doc, mesh));
    const std::size_t triangles = mesh.indices.size() / 3;
    SOL_CHECK(triangles == faces.size() + 32);
}

// ⚑ The ray's own round trip against `screenPoint` is asserted in `ui.unit`,
// beside `rayDirectionCamera` - `geometry.unit` links no `engine/ui`, and
// promising a test in a suite that cannot see the code is exactly the linkage
// mistake Phase 15 recorded twice.

// Möller-Trumbore against the cube, from six directions with a known answer:
// a ray down each axis at the origin must enter the near face first.
SOL_TEST(aRayIntoTheCubeEntersTheFaceItPointsAt)
{
    ForgeDoc doc = openTopologyAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    // The cube is the unit box at the origin, so a ray from 3 m out along an
    // axis enters at 2.5 m - and that number is what proves it took the NEAR
    // face rather than the far one, which a plane test with no ordering would.
    const assets::BuildPoint origins[] = {
        {3, 0, 0}, {-3, 0, 0}, {0, 3, 0}, {0, -3, 0}, {0, 0, 3}, {0, 0, -3}};
    for (const assets::BuildPoint& origin : origins) {
        const assets::BuildPoint direction{-origin.x / 3.0, -origin.y / 3.0, -origin.z / 3.0};
        std::size_t face = 0;
        double distance = 0.0;
        SOL_REQUIRE(assets::forgePickFace(points, faces, origin, direction, face, distance));
        SOL_CHECK(std::abs(distance - 2.5) < 1e-6);
        SOL_CHECK(face < faces.size());
    }

    // A ray that misses entirely finds nothing, and leaves its outputs alone.
    std::size_t face = 99;
    double distance = -1.0;
    SOL_CHECK(!assets::forgePickFace(points, faces, {3, 3, 3}, {1, 0, 0}, face, distance));
    SOL_CHECK(face == 99);
    SOL_CHECK(distance == -1.0);

    // ⚑ And a ray pointing AWAY from the cube finds nothing either, which is
    // the `along <= 0` clause. Without it every pick would answer with whatever
    // is behind the author's head.
    SOL_CHECK(!assets::forgePickFace(points, faces, {3, 0, 0}, {1, 0, 0}, face, distance));
}

// ⚑⚑ THE WIDENING, AND IT IS THE REASON E4d NEEDED A GROUP AT ALL. A box face is
// TWO triangles, `forgeMovePoints` can express a whole face and nothing less, so
// handing it the picked triangle would be refused on every box in the repo.
SOL_TEST(aPickedTriangleOfABoxWidensToTheWholeQuad)
{
    ForgeDoc doc = openTopologyAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));
    SOL_REQUIRE(faces.size() == 12);

    // Every one of the twelve, so this cannot pass by finding one lucky quad.
    for (std::size_t seed = 0; seed < faces.size(); ++seed) {
        std::vector<std::uint32_t> group;
        assets::forgeFaceGroup(points, faces, seed, group);
        SOL_CHECK(group.size() == 2);

        // Two triangles, four distinct points, all sharing one coordinate -
        // which is what makes them a FACE rather than merely two coplanar
        // triangles that happen to touch.
        const std::vector<assets::ForgePoint> corners = pointsOfGroup(points, faces, group);
        SOL_CHECK(corners.size() == 4);

        int agreeing = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const auto at = [&](const assets::ForgePoint& corner) {
                const assets::BuildPoint& p = corner.position;
                return axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
            };
            bool same = true;
            for (const assets::ForgePoint& corner : corners) {
                same = same && std::abs(at(corner) - at(corners[0])) < 1e-6;
            }
            agreeing += static_cast<int>(same);
        }
        SOL_CHECK(agreeing == 1); // exactly one axis: it is a face, not an edge
    }
}

namespace {

// The unit normal of a face, in the test's own arithmetic rather than the
// implementation's - a group's contract has to be checkable without borrowing
// the expression it is a contract on.
assets::BuildPoint normalOf(const std::vector<assets::ForgePoint>& points, const assets::ForgeFace& face)
{
    const assets::BuildPoint u = points[face.b].position - points[face.a].position;
    const assets::BuildPoint v = points[face.c].position - points[face.a].position;
    return core::normalize(core::cross(u, v));
}

// cos(0.5 degrees) - `forgeFaceGroup`'s own threshold, restated here because a
// test that read it out of the implementation would agree with any value.
constexpr double kCoplanarCheck = 0.9999619;

} // namespace

// ⚑⚑ THE MEMBRANE IS A FLAT DISC, AND THIS IS THE ASSERTION THAT SAYS SO. Its
// profile is `[[0.0, 0.0], [70.0, 0.0]]` - two points at the SAME height - so
// the revolve sweeps a flat fan and all 32 of its non-degenerate triangles lie
// in one plane. The whole film is therefore ONE face, which is the honest answer
// rather than a failure of the flood: an author dragging any part of it means
// all of it.
//
// ⚑ The first version of this test assumed a revolve must bend and pinned the
// group under eight. It failed, and the asset was right.
SOL_TEST(theMembranesFlatDiscIsOneFaceOfThirtyTwoTriangles)
{
    ForgeDoc doc = openTopologyAsset("gate_membrane");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));
    SOL_REQUIRE(faces.size() == 32);

    for (std::size_t seed = 0; seed < faces.size(); ++seed) {
        std::vector<std::uint32_t> group;
        assets::forgeFaceGroup(points, faces, seed, group);
        SOL_CHECK(group.size() == 32);
        SOL_CHECK(group[0] == seed); // the seed is always its own group's first
    }
}

// The contract over the committed assets: no group ever contains a face that
// disagrees with its seed's plane.
//
// ⚑⚑ THIS ONE CANNOT TELL A STEPWISE IMPLEMENTATION FROM A SEED-WISE ONE, AND
// SAYING SO IS THE POINT. No asset in this repo bends slowly enough for the two
// to differ: the finest curve here is a 32-segment torus at 11.25 degrees per
// segment, which BOTH rules reject in a single step. Comparing against the
// current face instead of the seed leaves all 117 tests green - measured, not
// assumed. The test below is the one that separates them, and it had to be
// built rather than found.
SOL_TEST(everyFaceInAGroupIsCoplanarWithTheGroupsSeed)
{
    const char* const names[] = {"gate", "station", "asteroid", "cockpit", "ship"};
    for (const char* const name : names) {
        ForgeDoc doc = openTopologyAsset(name);
        SOL_REQUIRE(!doc.parts.empty());

        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeEdge> edges;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));
        SOL_REQUIRE(!faces.empty());

        for (std::size_t seed = 0; seed < faces.size(); ++seed) {
            std::vector<std::uint32_t> group;
            assets::forgeFaceGroup(points, faces, seed, group);
            SOL_REQUIRE(!group.empty());
            SOL_REQUIRE(group[0] == seed);
            const assets::BuildPoint seedNormal = normalOf(points, faces[seed]);
            for (const std::uint32_t index : group) {
                SOL_CHECK(core::dot(seedNormal, normalOf(points, faces[index])) >= kCoplanarCheck);
            }
        }
    }
}

// ⚑⚑ THE TEST THAT ACTUALLY SEPARATES THE TWO RULES, AND IT IS SYNTHETIC
// BECAUSE NO ASSET IN THIS REPO CAN. A strip of twenty quads, each hinged 0.2
// degrees from the last: every neighbour looks flat against the one before it
// (0.2 < the 0.5-degree threshold), so a STEPWISE flood walks the whole strip
// and returns all forty triangles - a selection curving four degrees through
// space that nobody could have meant. Measured against the SEED it stops where
// the accumulated turn passes the threshold, which is two quads either side.
//
// ⚑ VALIDATED BY BREAKING IT: comparing against the current face instead of the
// seed makes this read 40 where it must read 10. That is the whole mutation, and
// it is invisible to every other test in this file.
SOL_TEST(aSlowlyBendingStripStopsAtTheSeedsPlaneRatherThanCreepingAlongIt)
{
    // The strip runs along the polyline in xy and is extruded one metre in z,
    // so quad `i`'s normal is perpendicular to its own segment and the turn
    // between consecutive quads is exactly the hinge angle.
    constexpr int kQuads = 20;
    constexpr double kHinge = 0.2 * 3.14159265358979323846 / 180.0;

    std::vector<assets::ForgePoint> points;
    double x = 0.0;
    double y = 0.0;
    for (int i = 0; i <= kQuads; ++i) {
        assets::ForgePoint low;
        low.position = {x, y, 0.0};
        assets::ForgePoint high;
        high.position = {x, y, 1.0};
        points.push_back(low);
        points.push_back(high);
        const double angle = static_cast<double>(i) * kHinge;
        x += std::cos(angle);
        y += std::sin(angle);
    }

    std::vector<assets::ForgeFace> faces;
    for (std::uint32_t i = 0; i < kQuads; ++i) {
        const std::uint32_t a = 2 * i;
        faces.push_back({a, a + 2, a + 1});
        faces.push_back({a + 1, a + 2, a + 3});
    }
    SOL_REQUIRE(faces.size() == 40);

    // Both triangles of a quad are exactly coplanar, so a group is always an
    // even number of them - and seeded in the middle it reaches two quads each
    // way before the accumulated turn (0.6 degrees) passes the threshold.
    std::vector<std::uint32_t> group;
    assets::forgeFaceGroup(points, faces, /*seed=*/20, group);
    SOL_CHECK(group.size() == 10);

    // The far end of the strip is four degrees off the seed and must not be in
    // it - the assertion a stepwise flood fails outright.
    SOL_CHECK(std::find(group.begin(), group.end(), 0u) == group.end());
    SOL_CHECK(std::find(group.begin(), group.end(), 39u) == group.end());
}

// ⚑⚑ AND THE PAYOFF, WHICH IS THE CASE E4c COULD ONLY ASSERT BY HAND: a face
// group fed to `forgeMovePoints` is ACCEPTED where an edge is refused, moves the
// face by exactly the delta, and leaves the opposite face where it was.
SOL_TEST(aWidenedBoxFaceMovesByTheDeltaAndPinsTheFaceOpposite)
{
    ForgeDoc doc = openTopologyAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    // The +x face: pick the triangle whose three corners are all at x = +0.5.
    std::size_t seed = faces.size();
    for (std::size_t i = 0; i < faces.size() && seed == faces.size(); ++i) {
        const std::uint32_t three[3] = {faces[i].a, faces[i].b, faces[i].c};
        bool allPositiveX = true;
        for (const std::uint32_t corner : three) {
            allPositiveX = allPositiveX && std::abs(points[corner].position.x - 0.5) < 1e-6;
        }
        seed = allPositiveX ? i : seed;
    }
    SOL_REQUIRE(seed < faces.size());

    std::vector<std::uint32_t> group;
    assets::forgeFaceGroup(points, faces, seed, group);
    SOL_REQUIRE(group.size() == 2);

    const std::vector<assets::ForgePoint> set = pointsOfGroup(points, faces, group);
    SOL_REQUIRE(set.size() == 4);

    // The picked TRIANGLE alone is three of four corners, and that is refused -
    // which is what the widening exists to avoid, asserted rather than assumed.
    ForgeDoc refused = doc;
    const std::vector<assets::ForgePoint> triangle = {
        points[faces[seed].a], points[faces[seed].b], points[faces[seed].c]};
    std::string error;
    SOL_CHECK(!assets::forgeMovePoints(refused, triangle, {0.2, 0.0, 0.0}, nullptr, &error));
    SOL_CHECK(!error.empty());

    // The widened FACE is accepted, and it is exact along its own normal.
    bool dropped = true;
    SOL_REQUIRE(assets::forgeMovePoints(doc, set, {0.2, 0.0, 0.0}, &dropped, &error));
    SOL_CHECK(!dropped);

    std::vector<assets::ForgePoint> after;
    std::vector<assets::ForgeEdge> afterEdges;
    std::vector<assets::ForgeFace> afterFaces;
    SOL_REQUIRE(assets::forgeTopology(doc, after, afterEdges, afterFaces));
    SOL_REQUIRE(after.size() == 8);

    double maxX = -10.0;
    double minX = 10.0;
    for (const assets::ForgePoint& point : after) {
        maxX = point.position.x > maxX ? point.position.x : maxX;
        minX = point.position.x < minX ? point.position.x : minX;
    }
    SOL_CHECK(std::abs(maxX - 0.7) < 1e-4); // moved by exactly the delta
    SOL_CHECK(std::abs(minX + 0.5) < 1e-4); // and the far face did not move
}

// ⚑ Off the normal there is no answer, and the flag says so. On an axis a face's
// corners straddle, the only expressible move slides the WHOLE box - so the
// component is discarded rather than silently applied to eight corners.
SOL_TEST(aBoxFacePulledOffItsNormalDropsThatComponentAndSaysSo)
{
    ForgeDoc doc = openTopologyAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    std::vector<assets::ForgePoint> set;
    for (const assets::ForgePoint& point : points) {
        if (std::abs(point.position.x - 0.5) < 1e-6) {
            set.push_back(point);
        }
    }
    SOL_REQUIRE(set.size() == 4);

    bool dropped = false;
    std::string error;
    SOL_REQUIRE(assets::forgeMovePoints(doc, set, {0.2, 0.3, 0.0}, &dropped, &error));
    SOL_CHECK(dropped);

    std::vector<assets::ForgePoint> after;
    std::vector<assets::ForgeEdge> afterEdges;
    std::vector<assets::ForgeFace> afterFaces;
    SOL_REQUIRE(assets::forgeTopology(doc, after, afterEdges, afterFaces));
    double maxX = -10.0;
    double maxY = -10.0;
    for (const assets::ForgePoint& point : after) {
        maxX = point.position.x > maxX ? point.position.x : maxX;
        maxY = point.position.y > maxY ? point.position.y : maxY;
    }
    SOL_CHECK(std::abs(maxX - 0.7) < 1e-4); // the normal component landed
    SOL_CHECK(std::abs(maxY - 0.5) < 1e-4); // the off-normal one did not
}

// ⚑ E1's fixed point, met by a FACE instead of a point. A drag of zero distance
// over every committed asset must leave every one of them byte-identical - the
// rule that caught `cube.forge` materialising `center` and `size` on a click.
SOL_TEST(aZeroDistanceFaceDragLeavesEveryCommittedFileByteIdentical)
{
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid", "gate_membrane"};
    for (const char* const name : names) {
        ForgeDoc doc = openTopologyAsset(name);
        SOL_REQUIRE(!doc.parts.empty());
        const std::string before = assets::writeForge(doc);

        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeEdge> edges;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));
        SOL_REQUIRE(!faces.empty());

        // The first face whose group is entirely movable - which on the gate and
        // the station is a box, and on the asteroid is a baked triangle.
        for (std::size_t seed = 0; seed < faces.size(); ++seed) {
            std::vector<std::uint32_t> group;
            assets::forgeFaceGroup(points, faces, seed, group);
            const std::vector<assets::ForgePoint> set = pointsOfGroup(points, faces, group);
            bool movable = true;
            for (const assets::ForgePoint& point : set) {
                movable = movable && point.movable();
            }
            if (!movable) {
                continue;
            }
            std::string error;
            // A refusal is fine - a box edge-shaped group is refused by design.
            // What is NOT fine is an accepted zero move that writes anything.
            (void)assets::forgeMovePoints(doc, set, {0.0, 0.0, 0.0}, nullptr, &error);
            break;
        }
        SOL_CHECK(assets::writeForge(doc) == before);
    }
}

namespace {

ForgeDoc openAsset(const char* name)
{
    const std::string source = readWholeFile(std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge");
    ForgeDoc doc;
    if (!source.empty()) {
        (void)parses(source, doc);
    }
    return doc;
}

} // namespace

// ⚑⚑ THE DEFECT THE STAGE EXISTS TO PREVENT, PINNED FROM BOTH SIDES. Moving a
// face's four points is NOT calling the point move four times: a box puts ONE
// `center`+`size` pair behind all eight of its corners, so the loop applies the
// resize once per grabbed point and the face travels four times as far as the
// hand. This asserts the right answer AND the wrong one, so a future
// simplification back to a loop fails here rather than silently multiplying
// every set drag in the tool.
SOL_TEST(movingABoxFaceAsASetMovesItOnceAndAsALoopMovesItFourTimes)
{
    const assets::BuildPoint drag{0.2, 0.0, 0.0};

    ForgeDoc set = openAsset("cube");
    SOL_REQUIRE(!set.parts.empty());
    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(set, points));

    std::vector<assets::ForgePoint> face;
    for (const assets::ForgePoint& point : points) {
        if (std::abs(point.position.x - 0.5) < 1e-4) {
            face.push_back(point);
        }
    }
    SOL_REQUIRE(face.size() == 4);

    bool dropped = true;
    SOL_REQUIRE(assets::forgeMovePoints(set, face, drag, &dropped));
    SOL_CHECK(!dropped); // the pull is along the face's own normal

    const std::size_t box = set.indexOf("box");
    SOL_REQUIRE(box < set.parts.size());
    SOL_CHECK(std::abs(set.parts[box].value("center").vec.x - 0.1) < 1e-9);
    SOL_CHECK(std::abs(set.parts[box].value("size").vec.x - 1.2) < 1e-9);

    // And the same drag through the point move, once per corner, which is the
    // defect: four applications of a pair only one of them should have written.
    ForgeDoc loop = openAsset("cube");
    std::vector<assets::ForgePoint> loopPoints;
    SOL_REQUIRE(assets::forgePoints(loop, loopPoints));
    for (const assets::ForgePoint& point : loopPoints) {
        if (std::abs(point.position.x - 0.5) < 1e-4) {
            SOL_REQUIRE(assets::forgeMovePoint(loop, point, drag));
        }
    }
    const std::size_t loopBox = loop.indexOf("box");
    SOL_REQUIRE(loopBox < loop.parts.size());
    SOL_CHECK(std::abs(loop.parts[loopBox].value("size").vec.x - 1.8) < 1e-9); // 4x the drag
}

// A whole face is the case a box answers exactly: four corners agreeing about
// one axis move by the drag, and the opposite face stays where it was.
SOL_TEST(draggingABoxFaceMovesItByTheDragAndPinsTheOppositeFace)
{
    ForgeDoc doc = openAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());
    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));

    std::vector<assets::ForgePoint> face;
    for (const assets::ForgePoint& point : points) {
        if (std::abs(point.position.x - 0.5) < 1e-4) {
            face.push_back(point);
        }
    }
    SOL_REQUIRE(face.size() == 4);

    bool dropped = true;
    SOL_REQUIRE(assets::forgeMovePoints(doc, face, {0.25, 0.0, 0.0}, &dropped));
    SOL_CHECK(!dropped);

    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    double minimum = 1e9;
    double maximum = -1e9;
    for (const assets::ForgePoint& point : after) {
        minimum = std::min(minimum, point.position.x);
        maximum = std::max(maximum, point.position.x);
    }
    SOL_CHECK(std::abs(minimum + 0.5) < 1e-6);  // the -x face is pinned
    SOL_CHECK(std::abs(maximum - 0.75) < 1e-6); // the +x face moved by the drag
}

// ⚑⚑ AN EDGE OF A BOX IS REFUSED, AND THE MESSAGE NAMES THE BAKE. A box has no
// shear, so there is no `center`/`size` change that moves two corners of an edge
// and leaves the other two of their face standing. The nearest thing the
// arithmetic can do is widen the whole face - measured, "grabbed 2, moved 4",
// with the two nobody touched coming along - which is indistinguishable from a
// bug. So the tool declines and points at the D checkpoint's rule instead.
SOL_TEST(draggingABoxEdgeIsRefusedAndTheMessageNamesTheBake)
{
    ForgeDoc doc = openAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());
    const std::string before = assets::writeForge(doc);

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    const std::size_t lo = pointAt(points, {0.5, 0.5, -0.5});
    const std::size_t hi = pointAt(points, {0.5, 0.5, 0.5});
    SOL_REQUIRE(lo < points.size() && hi < points.size());
    const std::vector<assets::ForgePoint> selection{points[lo], points[hi]};

    // Refused whichever way it is pulled: across its run, and along it.
    for (const assets::BuildPoint drag :
         {assets::BuildPoint{0.2, 0.0, 0.0}, assets::BuildPoint{0.0, 0.0, 0.3}}) {
        std::string error;
        SOL_CHECK(!assets::forgeMovePoints(doc, selection, drag, nullptr, &error));
        SOL_CHECK(error.find("bake") != std::string::npos);
        SOL_CHECK(error.find("box") != std::string::npos); // the part, by id
    }
    SOL_CHECK(assets::writeForge(doc) == before); // refused means untouched
}

// ⚑ AND THE PAYOFF, WHICH IS WHY THE REFUSAL IS NOT A DEAD END. Baked, the same
// part answers the same drag exactly: the two corners grabbed move and the other
// two of that face do not. This is the D checkpoint's rule doing the job it was
// written for, and it is what the refusal above is pointing at.
SOL_TEST(bakingTheBoxFirstLetsItsEdgeMoveExactly)
{
    ForgeDoc doc = openAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());
    const std::size_t box = doc.indexOf("box");
    SOL_REQUIRE(box < doc.parts.size());
    SOL_REQUIRE(assets::forgeBakeDocumentPart(doc, box));

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    const std::size_t lo = pointAt(points, {0.5, 0.5, -0.5});
    const std::size_t hi = pointAt(points, {0.5, 0.5, 0.5});
    SOL_REQUIRE(lo < points.size() && hi < points.size());

    const std::vector<assets::ForgePoint> selection{points[lo], points[hi]};
    bool dropped = true;
    SOL_REQUIRE(assets::forgeMovePoints(doc, selection, {0.2, 0.0, 0.0}, &dropped));
    SOL_CHECK(!dropped); // a baked vertex is its own number: nothing to discard

    std::vector<assets::ForgePoint> after;
    SOL_REQUIRE(assets::forgePoints(doc, after));
    SOL_CHECK(after.size() == points.size()); // no seam opened
    // The grabbed edge moved...
    SOL_CHECK(pointAt(after, {0.7, 0.5, -0.5}) < after.size());
    SOL_CHECK(pointAt(after, {0.7, 0.5, 0.5}) < after.size());
    // ...and the rest of that face did NOT, which is the whole difference.
    SOL_CHECK(pointAt(after, {0.5, -0.5, -0.5}) < after.size());
    SOL_CHECK(pointAt(after, {0.5, -0.5, 0.5}) < after.size());
}

// ⚑ A FACE MOVES ALONG ITS OWN NORMAL. On an axis its corners straddle, the only
// expressible move slides the WHOLE box - all eight corners - which is the same
// "moved more than you grabbed" surprise in different clothes. The off-normal
// pull is discarded and `dropped` is how the tool says so.
SOL_TEST(anOffNormalPullOnABoxFaceIsDroppedRatherThanSlidingTheWholeBox)
{
    ForgeDoc doc = openAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());
    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));

    std::vector<assets::ForgePoint> face;
    for (const assets::ForgePoint& point : points) {
        if (std::abs(point.position.x - 0.5) < 1e-4) {
            face.push_back(point);
        }
    }
    SOL_REQUIRE(face.size() == 4);

    bool dropped = false;
    SOL_REQUIRE(assets::forgeMovePoints(doc, face, {0.2, 0.35, 0.0}, &dropped));
    SOL_CHECK(dropped);

    const std::size_t box = doc.indexOf("box");
    SOL_REQUIRE(box < doc.parts.size());
    // The normal component landed; the sideways one did not move the box at all.
    SOL_CHECK(std::abs(doc.parts[box].value("size").vec.x - 1.2) < 1e-9);
    SOL_CHECK(std::abs(doc.parts[box].value("center").vec.y - 0.0) < 1e-9);
    SOL_CHECK(std::abs(doc.parts[box].value("size").vec.y - 1.0) < 1e-9);
}

// ⚑ A beam's four `from` corners are four distinct POINTS sharing ONE write, so
// the collapse matters there without an edge being involved at all. Two of them
// dragged together move the end once.
SOL_TEST(movingTwoCornersOfABeamsCapMovesThatEndOnlyOnce)
{
    ForgeDoc doc = openAsset("cockpit");
    SOL_REQUIRE(!doc.parts.empty());

    std::size_t beam = doc.parts.size();
    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        if (doc.parts[i].primitive == ForgePrimitive::Beam) {
            beam = i;
            break;
        }
    }
    SOL_REQUIRE(beam < doc.parts.size());
    const assets::BuildPoint from = doc.parts[beam].value("from").vec;

    std::vector<assets::ForgePoint> points;
    SOL_REQUIRE(assets::forgePoints(doc, points));
    std::vector<assets::ForgePoint> capped;
    for (const assets::ForgePoint& point : points) {
        for (const assets::ForgePointWrite& write : point.writes) {
            if (write.part == beam && write.kind == assets::ForgeWriteKind::BeamEnd && write.element == 0) {
                capped.push_back(point);
                break;
            }
        }
    }
    SOL_REQUIRE(capped.size() >= 2);

    const std::vector<assets::ForgePoint> selection{capped[0], capped[1]};
    SOL_REQUIRE(assets::forgeMovePoints(doc, selection, {0.1, 0.0, 0.0}));

    // Once, not twice: 0.1 moved, not 0.2.
    SOL_CHECK(std::abs(doc.parts[beam].value("from").vec.x - (from.x + 0.1)) < 1e-9);
}

// ⚑ THE FIXED POINT SURVIVES THE STAGE. E1 proved a zero-distance move writes
// nothing, E2 widened it to the asset that authors nothing, Phase 14 kept it
// through the quantizer - and a SET move is the fourth door onto the same rule.
// A modeller saves on every accepted edit, so a click that grabs an edge and
// releases it must leave the file exactly as it found it.
SOL_TEST(aZeroDistanceSetMoveLeavesEveryCommittedFileByteIdentical)
{
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid", "gate_membrane"};
    for (const char* name : names) {
        ForgeDoc doc = openAsset(name);
        SOL_REQUIRE(!doc.parts.empty());
        const std::string before = assets::writeForge(doc);

        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeEdge> edges;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

        // ⚑ Accepted or refused, the file must come back unchanged - and BOTH
        // outcomes occur here, because a box edge is refused while a triangle's
        // and a baked part's are taken. The property is the same either way: a
        // click that grabs an edge and releases it costs the author nothing.
        std::size_t taken = 0;
        std::size_t refused = 0;
        for (const assets::ForgeEdge& edge : edges) {
            if (!points[edge.a].movable() || !points[edge.b].movable()) {
                continue;
            }
            const std::vector<assets::ForgePoint> selection{points[edge.a], points[edge.b]};
            (assets::forgeMovePoints(doc, selection, {0.0, 0.0, 0.0}) ? taken : refused) += 1;
        }
        const std::size_t moved = taken;
        SOL_CHECK(assets::writeForge(doc) == before);
        if (assets::writeForge(doc) != before) {
            std::printf("  %s changed after %zu zero-distance edge move(s)\n", name, moved);
        }
    }
}

// ---------------------------------------------------------------------------
// Stage E5a: the three topology operations, headless.
// ---------------------------------------------------------------------------

namespace {

const char* const kAllAssets[] = {"asteroid", "cockpit", "cube", "gate", "gate_membrane", "ship", "station"};

struct Solid
{
    bool built = false;
    bool manifold = false;
    std::uint32_t borderEdges = 0;
    std::uint32_t degenerates = 0;
    double volume = 0.0;
    std::size_t triangles = 0;
};

// Phase 16's asset invariants, aimed at a mesh the TOOL made rather than at a
// committed file. That is the whole point of reusing them here: a side wall
// wound the wrong way is invisible to every count and obvious to the volume.
[[nodiscard]] Solid inspect(const ForgeDoc& doc)
{
    Solid out;
    assets::MeshData mesh;
    if (!assets::buildForge(doc, mesh)) {
        return out;
    }
    out.built = true;
    out.triangles = mesh.indices.size() / 3;
    assets::EditMesh edit = assets::toEditMesh(mesh);
    for (std::uint32_t face = 0; face < edit.triangleCount(); ++face) {
        const std::uint32_t a = edit.facePosition(face, 0);
        const std::uint32_t b = edit.facePosition(face, 1);
        const std::uint32_t c = edit.facePosition(face, 2);
        out.degenerates += static_cast<std::uint32_t>(a == b || b == c || a == c);
    }
    const assets::MeshAdjacency adjacency = assets::buildAdjacency(edit);
    out.manifold = adjacency.isManifold();
    out.borderEdges = adjacency.borderEdgeCount();
    out.volume = assets::signedVolume(edit);
    return out;
}

// The group of the first face whose three corners all sit at `value` on `axis` -
// which on a box is one of its six faces, addressed by where it is rather than
// by a triangle number nobody can read.
[[nodiscard]] std::vector<std::uint32_t> faceGroupAt(const std::vector<assets::ForgePoint>& points,
                                                     const std::vector<assets::ForgeFace>& faces,
                                                     int axis,
                                                     double value)
{
    for (std::size_t seed = 0; seed < faces.size(); ++seed) {
        const std::uint32_t three[3] = {faces[seed].a, faces[seed].b, faces[seed].c};
        bool onIt = true;
        for (const std::uint32_t corner : three) {
            const assets::BuildPoint& p = points[corner].position;
            const double coordinate = axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
            onIt = onIt && std::abs(coordinate - value) < 1e-6;
        }
        if (onIt) {
            std::vector<std::uint32_t> group;
            assets::forgeFaceGroup(points, faces, seed, group);
            return group;
        }
    }
    return {};
}

// Every vertex of a part referenced by at least one of its triangles. An extrude
// that duplicated where it should have moved leaves orphans, and an orphan is a
// point the tool draws and no triangle owns.
[[nodiscard]] bool noOrphanVertices(const assets::ForgePart& part)
{
    const ForgeValue* const vertices = part.find("vertices");
    const ForgeValue* const indices = part.find("indices");
    if (vertices == nullptr || indices == nullptr) {
        return false;
    }
    std::vector<bool> used(vertices->vertices.size(), false);
    for (const std::uint32_t index : indices->indices) {
        if (index < used.size()) {
            used[index] = true;
        }
    }
    return std::find(used.begin(), used.end(), false) == used.end();
}

} // namespace

// ⚑⚑ E5's FIRST FINDING, AND IT WAS SITTING IN E4's OUTPUT ALL ALONG:
// `ForgeEdge::faceCount` COUNTED DEGENERATE FACES, SO ON THE ONE OPEN SURFACE IN
// THIS REPO NO EDGE READ TWO. `gate_membrane.forge` is a flat disc fanned to a
// point - 32 real triangles and 32 with no area - and a face whose two corners
// weld to one point still has a third side, pushed twice. Its 32 axis edges read
// FOUR and its 32 rim edges read one. E4 only ever asked whether a triangle was
// there; a split asks WHICH, and would have tried to split two with no area.
//
// ⚑ The edge SET must not move, only the tally - which is what separates a fix
// from a change of meaning.
SOL_TEST(aFilmsAxisEdgesCountTwoRealFacesRatherThanFourWithTheDegenerateOnes)
{
    ForgeDoc doc = openAsset("gate_membrane");
    SOL_REQUIRE(!doc.parts.empty());

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    SOL_CHECK(points.size() == 33); // 32 around the rim and one on the axis
    SOL_CHECK(edges.size() == 64);
    SOL_CHECK(faces.size() == 32);

    std::size_t border = 0;
    std::size_t interior = 0;
    std::size_t overcounted = 0;
    for (const assets::ForgeEdge& edge : edges) {
        border += static_cast<std::size_t>(edge.faceCount == 1);
        interior += static_cast<std::size_t>(edge.faceCount == 2);
        overcounted += static_cast<std::size_t>(edge.faceCount > 2);
    }
    SOL_CHECK(border == 32);   // the rim, correctly open
    SOL_CHECK(interior == 32); // the axis, which used to read four
    SOL_CHECK(overcounted == 0);
}

// The edge SET is unchanged by that fix, asserted over every committed asset so
// nobody has to take the reasoning on trust.
SOL_TEST(theEdgeCountsOfEveryCommittedAssetAreUnchangedByTheDegenerateFix)
{
    struct Expected
    {
        const char* name;
        std::size_t points;
        std::size_t edges;
        std::size_t faces;
    };

    constexpr Expected kExpected[] = {
        {"asteroid", 162, 480, 320},
        {"cockpit", 141, 315, 210},
        {"cube", 8, 18, 12},
        {"gate", 320, 912, 608},
        {"gate_membrane", 33, 64, 32},
        {"ship", 12, 24, 16},
        {"station", 552, 1602, 1068},
    };
    for (const Expected& want : kExpected) {
        ForgeDoc doc = openAsset(want.name);
        SOL_REQUIRE(!doc.parts.empty());
        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeEdge> edges;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));
        SOL_CHECK(points.size() == want.points);
        SOL_CHECK(edges.size() == want.edges);
        SOL_CHECK(faces.size() == want.faces);
        if (edges.size() != want.edges) {
            std::printf("  %s: %zu edges, expected %zu\n", want.name, edges.size(), want.edges);
        }
    }
}

// ⚑ A face now names the part that emitted it and which of that part's own
// triangles it is. E4 needed neither - a face drag moves POINTS, and a point
// already carries every part standing at it - and a topology change needs both,
// because it rewrites one part's index list.
SOL_TEST(everyFaceNamesThePartThatEmittedItAndWhichOfItsTrianglesItIs)
{
    for (const char* const name : kAllAssets) {
        ForgeDoc doc = openAsset(name);
        SOL_REQUIRE(!doc.parts.empty());
        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeEdge> edges;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

        // The triangle numbers of one part are distinct, and none of them is
        // past the triangle count that part's own baked index list would have.
        std::vector<std::uint32_t> highest(doc.parts.size(), 0);
        std::vector<std::size_t> seen(doc.parts.size(), 0);
        for (const assets::ForgeFace& face : faces) {
            SOL_REQUIRE(face.part < doc.parts.size());
            highest[face.part] = std::max(highest[face.part], face.triangle);
            ++seen[face.part];
        }
        for (std::size_t part = 0; part < doc.parts.size(); ++part) {
            if (seen[part] == 0) {
                continue;
            }
            ForgeDoc baked = openAsset(name);
            SOL_REQUIRE(assets::forgeBakeDocumentPart(baked, part));
            const ForgeValue* const indices = baked.parts[part].find("indices");
            SOL_REQUIRE(indices != nullptr);
            SOL_CHECK(static_cast<std::size_t>(highest[part]) * 3 + 2 < indices->indices.size());
        }
    }
}

// ⚑⚑ THE MEASUREMENT THAT REORDERED THE STAGE: `ship.forge`'s HULL QUADS ARE
// TWO PARTS EACH. E5's one-line estimate said the operations "operate on baked
// parts", which assumes one. A coplanar face group floods over POINTS, and a
// point is shared across parts - that is E1's whole mechanism - so three of this
// asset's thirteen groups span two `flat_triangle` parts, and all 24 of its
// edges have their two faces in different parts.
SOL_TEST(theShipsHullQuadsAreEachTwoPartsAndAllItsEdgesAreCrossPart)
{
    ForgeDoc doc = openAsset("ship");
    SOL_REQUIRE(!doc.parts.empty());
    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));
    SOL_REQUIRE(faces.size() == 16);

    std::vector<bool> seen(faces.size(), false);
    std::size_t crossPart = 0;
    for (std::size_t seed = 0; seed < faces.size(); ++seed) {
        if (seen[seed]) {
            continue;
        }
        std::vector<std::uint32_t> group;
        assets::forgeFaceGroup(points, faces, seed, group);
        std::vector<std::size_t> parts;
        for (const std::uint32_t index : group) {
            seen[index] = true;
            if (std::find(parts.begin(), parts.end(), faces[index].part) == parts.end()) {
                parts.push_back(faces[index].part);
            }
        }
        crossPart += static_cast<std::size_t>(parts.size() > 1);
    }
    SOL_CHECK(crossPart == 3); // hull_0a+0b, hull_2a+2b, rear_lower+rear_upper

    std::size_t crossPartEdges = 0;
    for (const assets::ForgeEdge& edge : edges) {
        std::vector<std::size_t> parts;
        for (const assets::ForgeFace& face : faces) {
            const std::uint32_t three[3] = {face.a, face.b, face.c};
            const bool hasA = std::find(std::begin(three), std::end(three), edge.a) != std::end(three);
            const bool hasB = std::find(std::begin(three), std::end(three), edge.b) != std::end(three);
            if (hasA && hasB && std::find(parts.begin(), parts.end(), face.part) == parts.end()) {
                parts.push_back(face.part);
            }
        }
        crossPartEdges += static_cast<std::size_t>(parts.size() > 1);
    }
    SOL_CHECK(crossPartEdges == 24); // every single one
}

// ⚑⚑ MERGING TWO ADJACENT PARTS IS BIT-EXACT, WHICH IS E3's ASSERTION OVER A
// SECOND OPERATION. Parts emit in FILE order, so the survivor has to be the
// EARLIER of the two or every vertex after it renumbers. This is what makes the
// extrude's cross-part refusal a rule rather than an obstacle: the fix it names
// costs the built mesh nothing.
SOL_TEST(mergingTwoAdjacentPartsLeavesTheBuiltMeshIdentical)
{
    ForgeDoc doc = openAsset("ship");
    SOL_REQUIRE(!doc.parts.empty());
    assets::MeshData before;
    SOL_REQUIRE(assets::buildForge(doc, before));

    const std::size_t a = doc.indexOf("hull_0a");
    const std::size_t b = doc.indexOf("hull_0b");
    SOL_REQUIRE(a < doc.parts.size() && b < doc.parts.size());
    SOL_REQUIRE(b == a + 1); // adjacent, which is what makes it exact

    std::string error;
    SOL_REQUIRE(assets::forgeMergeParts(doc, b, a, &error)); // either order
    SOL_CHECK(doc.parts.size() == 15);
    SOL_CHECK(doc.indexOf("hull_0a") == a); // the earlier id survives
    SOL_CHECK(doc.indexOf("hull_0b") == std::string::npos);
    SOL_CHECK(doc.parts[a].primitive == ForgePrimitive::Mesh);

    assets::MeshData after;
    SOL_REQUIRE(assets::buildForge(doc, after));
    SOL_REQUIRE(before.vertices.size() == after.vertices.size());
    SOL_REQUIRE(before.indices.size() == after.indices.size());
    SOL_CHECK(std::memcmp(before.vertices.data(),
                          after.vertices.data(),
                          before.vertices.size() * sizeof(assets::MeshVertex)) == 0);
    SOL_CHECK(std::memcmp(before.indices.data(),
                          after.indices.data(),
                          before.indices.size() * sizeof(std::uint32_t)) == 0);
}

// The merge's three refusals, and each one leaves the document exactly as it
// found it. The parent rule is E3's bake frame talking: a baked part's geometry
// is stored in its PARENT's frame, so two parents means two frames.
SOL_TEST(mergingRefusesItselfAGroupAndTwoDifferentParents)
{
    ForgeDoc doc;
    SOL_REQUIRE(parses(R"(name = "merge"

[[part]]
id = "frame"
type = "group"

[[part]]
id = "loose"
type = "box"

[[part]]
id = "hung"
type = "box"
parent = "frame"
)",
                       doc));
    const std::string before = assets::writeForge(doc);

    std::string error;
    SOL_CHECK(!assets::forgeMergeParts(doc, 1, 1, &error));
    SOL_CHECK(error.find("itself") != std::string::npos);
    SOL_CHECK(!assets::forgeMergeParts(doc, 0, 1, &error));
    SOL_CHECK(error.find("group") != std::string::npos);
    SOL_CHECK(!assets::forgeMergeParts(doc, 1, 2, &error));
    SOL_CHECK(error.find("parent") != std::string::npos);
    SOL_CHECK(assets::writeForge(doc) == before);

    // And a part that other parts hang off is not deleted out from under them.
    ForgeDoc tree;
    SOL_REQUIRE(parses(R"(name = "tree"

[[part]]
id = "a"
type = "box"

[[part]]
id = "b"
type = "group"

[[part]]
id = "c"
type = "box"
parent = "b"
)",
                       tree));
    SOL_CHECK(!assets::forgeMergeParts(tree, 0, 1, &error));
}

// ⚑ A split composes across parts and that is why it is the cheap one: each face
// answers for itself, so a split landing in two parts is two independent splits,
// and the midpoints weld because a midpoint commutes with an affine transform.
// On `ship.forge` that is not an edge case - it is EVERY edge.
SOL_TEST(splittingAShipEdgeSplitsBothPartsStandingOnIt)
{
    ForgeDoc doc = openAsset("ship");
    SOL_REQUIRE(!doc.parts.empty());
    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    // The shared side of the bottom hull quad: two parts, one edge.
    const std::size_t lo = pointAt(points, {-2.6, -1.3, -1.0});
    const std::size_t hi = pointAt(points, {3.5, -1.7, 5.0});
    SOL_REQUIRE(lo < points.size() && hi < points.size());

    const Solid before = inspect(doc);
    SOL_REQUIRE(before.built);

    std::string error;
    SOL_REQUIRE(assets::forgeSplitEdge(
        doc, points, faces, static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi), &error));

    // Both parts baked, and only those two.
    SOL_CHECK(doc.parts[doc.indexOf("hull_0a")].primitive == ForgePrimitive::Mesh);
    SOL_CHECK(doc.parts[doc.indexOf("hull_0b")].primitive == ForgePrimitive::Mesh);
    SOL_CHECK(doc.parts[doc.indexOf("nose_0")].primitive == ForgePrimitive::FlatTriangle);
    SOL_CHECK(doc.parts.size() == 16); // a split adds no parts

    const Solid after = inspect(doc);
    SOL_CHECK(after.built);
    SOL_CHECK(after.triangles == before.triangles + 2); // one new triangle each
    SOL_CHECK(after.manifold);
    SOL_CHECK(after.borderEdges == before.borderEdges);
    SOL_CHECK(after.degenerates == 0);
    // ⚑ THE ASSERTION THAT MATTERS: a midpoint lies ON the surface, so the solid
    // it bounds is the same solid. A split that changed the volume moved
    // something, and a split that failed to weld across the seam would have torn
    // the hull open and changed it too.
    SOL_CHECK(std::abs(after.volume - before.volume) < 1e-4);

    std::vector<assets::ForgePoint> afterPoints;
    SOL_REQUIRE(assets::forgePoints(doc, afterPoints));
    SOL_CHECK(afterPoints.size() == points.size() + 1); // ONE new point, not two
    SOL_CHECK(pointAt(afterPoints, {0.45, -1.5, 2.0}) < afterPoints.size());
}

// The cube's edge split, where the volume assertion is exact rather than nearly:
// nothing about a closed unit box changes when one of its diagonals gains a
// point in the middle.
SOL_TEST(splittingACubeEdgeKeepsItClosedAndKeepsItsVolume)
{
    ForgeDoc doc = openAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());
    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    std::string error;
    SOL_REQUIRE(assets::forgeSplitEdge(doc, points, faces, edges[0].a, edges[0].b, &error));
    SOL_CHECK(doc.parts[0].primitive == ForgePrimitive::Mesh); // the box baked

    const Solid after = inspect(doc);
    SOL_CHECK(after.built);
    SOL_CHECK(after.manifold);
    SOL_CHECK(after.borderEdges == 0);
    SOL_CHECK(after.degenerates == 0);
    SOL_CHECK(std::abs(after.volume - 1.0) < 1e-5);
    SOL_CHECK(after.triangles == 14); // 12, and the two faces on that edge split

    // A pair that is not an edge, and one past the end, are both refused.
    ForgeDoc untouched = openAsset("cube");
    const std::string before = assets::writeForge(untouched);
    std::vector<assets::ForgePoint> p2;
    std::vector<assets::ForgeEdge> e2;
    std::vector<assets::ForgeFace> f2;
    SOL_REQUIRE(assets::forgeTopology(untouched, p2, e2, f2));
    SOL_CHECK(!assets::forgeSplitEdge(untouched, p2, f2, 0, 0, &error));
    SOL_CHECK(!assets::forgeSplitEdge(untouched, p2, f2, 0, 99, &error));
    SOL_CHECK(assets::writeForge(untouched) == before);
}

// ⚑⚑ THE ASSERTION THAT DECIDED THE EXTRUDE'S DESIGN, WRITTEN BEFORE IT: Phase
// 16's asset invariants aimed at a mesh the TOOL made. A wall wound the wrong
// way is invisible to every count and obvious to the signed volume, and a wall
// that missed a border edge leaves the solid open.
SOL_TEST(anExtrudedCubeFaceIsStillAClosedSolidWoundOutwards)
{
    ForgeDoc doc = openAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());
    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    const std::vector<std::uint32_t> group = faceGroupAt(points, faces, 2, 0.5);
    SOL_REQUIRE(group.size() == 2); // the +z face, both its triangles

    double offset = 0.0;
    std::string error;
    SOL_REQUIRE(assets::forgeExtrudeFaces(doc, faces, group, &offset, &error));
    // ⚑ A tenth of the group's longest border edge, on the 0.1 mm write grid.
    // Not zero, because `forgePoints` decides identity by POSITION and a face
    // duplicated in place welds straight back into the one it came from.
    SOL_CHECK(std::abs(offset - 0.1) < 1e-12);

    const Solid after = inspect(doc);
    SOL_CHECK(after.built);
    SOL_CHECK(after.manifold);
    SOL_CHECK(after.borderEdges == 0);
    SOL_CHECK(after.degenerates == 0);
    SOL_CHECK(after.triangles == 20); // 12, plus four walls of two
    SOL_CHECK(after.volume > 0.0);    // wound outwards
    // A 1 x 1 x 0.1 slab on top of a unit cube, and nothing else moved.
    SOL_CHECK(std::abs(after.volume - 1.1) < 1e-5);
    SOL_CHECK(noOrphanVertices(doc.parts[0]));

    std::vector<assets::ForgePoint> afterPoints;
    std::vector<assets::ForgeEdge> afterEdges;
    std::vector<assets::ForgeFace> afterFaces;
    SOL_REQUIRE(assets::forgeTopology(doc, afterPoints, afterEdges, afterFaces));
    SOL_CHECK(afterPoints.size() == 12);
    SOL_CHECK(afterEdges.size() == 30);
    SOL_CHECK(afterFaces.size() == 20);
    // V - E + F = 2, still one closed surface of genus zero.
    SOL_CHECK((static_cast<int>(afterPoints.size()) - static_cast<int>(afterEdges.size()) +
               static_cast<int>(afterFaces.size())) == 2);
    SOL_CHECK(pointAt(afterPoints, {0.5, 0.5, 0.6}) < afterPoints.size()); // raised
    SOL_CHECK(pointAt(afterPoints, {0.5, 0.5, 0.5}) < afterPoints.size()); // and the rim stayed
}

// ⚑⚑ THE REFUSAL THAT SET THE SLICE ORDER, AND ITS PAYOFF IN THE SAME TEST. A
// wall straddling the seam between two parts belongs to neither, so a cross-part
// extrude is declined with `merge` named - and merging first makes the identical
// call work. This is E4c's rule one level up, with the escape hatch built.
SOL_TEST(aCrossPartFaceIsRefusedNamingMergeAndMergingFirstMakesItWork)
{
    ForgeDoc doc = openAsset("ship");
    SOL_REQUIRE(!doc.parts.empty());
    const std::string before = assets::writeForge(doc);

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    // The bottom hull quad: `hull_0a` and `hull_0b`, coplanar, one face to look
    // at and two parts to write through.
    std::vector<std::uint32_t> group;
    for (std::size_t seed = 0; seed < faces.size() && group.empty(); ++seed) {
        std::vector<std::uint32_t> candidate;
        assets::forgeFaceGroup(points, faces, seed, candidate);
        if (candidate.size() == 2 && faces[candidate[0]].part != faces[candidate[1]].part) {
            group = candidate;
        }
    }
    SOL_REQUIRE(group.size() == 2);

    std::string error;
    SOL_CHECK(!assets::forgeExtrudeFaces(doc, faces, group, nullptr, &error));
    SOL_CHECK(error.find("merge") != std::string::npos);
    SOL_CHECK(assets::writeForge(doc) == before); // refused means untouched

    // Merge the two parts the message named, re-read the topology, and the same
    // face goes through.
    const std::size_t partA = faces[group[0]].part;
    const std::size_t partB = faces[group[1]].part;
    SOL_REQUIRE(assets::forgeMergeParts(doc, partA, partB, &error));

    std::vector<assets::ForgePoint> merged;
    std::vector<assets::ForgeEdge> mergedEdges;
    std::vector<assets::ForgeFace> mergedFaces;
    SOL_REQUIRE(assets::forgeTopology(doc, merged, mergedEdges, mergedFaces));
    std::vector<std::uint32_t> mergedGroup;
    for (std::size_t seed = 0; seed < mergedFaces.size() && mergedGroup.empty(); ++seed) {
        std::vector<std::uint32_t> candidate;
        assets::forgeFaceGroup(merged, mergedFaces, seed, candidate);
        if (candidate.size() == 2 && mergedFaces[candidate[0]].part == std::min(partA, partB)) {
            mergedGroup = candidate;
        }
    }
    SOL_REQUIRE(mergedGroup.size() == 2);

    const Solid was = inspect(doc);
    double offset = 0.0;
    SOL_REQUIRE(assets::forgeExtrudeFaces(doc, mergedFaces, mergedGroup, &offset, &error));
    SOL_CHECK(offset > 0.0);

    const Solid now = inspect(doc);
    SOL_CHECK(now.built);
    SOL_CHECK(now.manifold);
    SOL_CHECK(now.borderEdges == 0);
    SOL_CHECK(now.degenerates == 0);
    SOL_CHECK(now.triangles == was.triangles + 8); // four border sides, two each
    SOL_CHECK(now.volume > was.volume);            // raised OUTWARDS, not inwards
    SOL_CHECK(noOrphanVertices(doc.parts[std::min(partA, partB)]));
}

// ⚑ A vertex the group shares with a triangle OUTSIDE it is duplicated rather
// than moved, and `gate_membrane.forge` is the case that exercises it: its 32
// zero-area triangles stand on the same axis vertices the film's real ones do,
// and they are not in the group. Moving those would drag geometry nobody
// selected; duplicating them leaves no orphan behind either.
SOL_TEST(extrudingAFilmDuplicatesTheVerticesItSharesWithTrianglesOutsideTheGroup)
{
    ForgeDoc doc = openAsset("gate_membrane");
    SOL_REQUIRE(!doc.parts.empty());
    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    std::vector<std::uint32_t> group;
    assets::forgeFaceGroup(points, faces, 0, group);
    SOL_REQUIRE(group.size() == 32); // a flat disc really is one face

    double offset = 0.0;
    std::string error;
    SOL_REQUIRE(assets::forgeExtrudeFaces(doc, faces, group, &offset, &error));
    SOL_CHECK(offset > 0.0);
    SOL_CHECK(doc.parts[0].primitive == ForgePrimitive::Mesh);
    SOL_CHECK(noOrphanVertices(doc.parts[0]));

    const Solid after = inspect(doc);
    SOL_CHECK(after.built);
    // 32 raised triangles, 32 degenerate ones still where they were, and a
    // 32-sided wall of two triangles each.
    SOL_CHECK(after.triangles == 64 + 64);
    SOL_CHECK(after.borderEdges == 32); // it was an open film and it still is
}

// The extrude's refusals, each leaving the document byte-identical - the fixed
// point E1 established, met by an operation that changes topology.
SOL_TEST(aRefusedExtrudeLeavesTheDocumentByteIdentical)
{
    ForgeDoc doc = openAsset("cube");
    SOL_REQUIRE(!doc.parts.empty());
    const std::string before = assets::writeForge(doc);

    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeEdge> edges;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(assets::forgeTopology(doc, points, edges, faces));

    std::string error;
    double offset = 1.0;
    SOL_CHECK(!assets::forgeExtrudeFaces(doc, faces, {}, &offset, &error));
    SOL_CHECK(offset == 0.0); // reported as nothing done, not left stale
    const std::uint32_t past[] = {99u};
    SOL_CHECK(!assets::forgeExtrudeFaces(doc, faces, past, &offset, &error));
    SOL_CHECK(assets::writeForge(doc) == before);
}
