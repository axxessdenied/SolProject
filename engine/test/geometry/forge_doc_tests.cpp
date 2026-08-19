#include "sol/assets/forge_doc.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/test/test.hpp"

#include "shipped_meshes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

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
void reportFirstDifferingLine(const char* name, const std::string& expected,
                              const std::string& actual)
{
    std::size_t line = 1;
    std::size_t lineStart = 0;
    const std::size_t shared = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < shared; ++i) {
        if (expected[i] != actual[i]) {
            const std::size_t expectedEnd = expected.find('\n', lineStart);
            const std::size_t actualEnd = actual.find('\n', lineStart);
            std::printf("  %s.forge line %zu\n    want: %s\n    got:  %s\n", name, line,
                        expected.substr(lineStart, expectedEnd - lineStart).c_str(),
                        actual.substr(lineStart, actualEnd - lineStart).c_str());
            return;
        }
        if (expected[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    std::printf("  %s.forge: same for %zu bytes, then lengths differ (%zu vs %zu)\n", name, shared,
                expected.size(), actual.size());
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
        std::memcmp(a.vertices.data(), b.vertices.data(),
                    a.vertices.size() * sizeof(assets::MeshVertex)) != 0) {
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
        const float alignment = (outward.x * vertex.normal.x) + (outward.y * vertex.normal.y) +
                                (outward.z * vertex.normal.z);
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
    const double faceted = std::abs(soup.vertices[0].normal[0]) +
                           std::abs(soup.vertices[0].normal[1]) +
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
        const double length = std::sqrt((vertex.normal[0] * vertex.normal[0]) +
                                        (vertex.normal[1] * vertex.normal[1]) +
                                        (vertex.normal[2] * vertex.normal[2]));
        SOL_CHECK(near(length, 1.0, 1e-5));
        const double dominant = std::max({std::abs(vertex.normal[0]), std::abs(vertex.normal[1]),
                                          std::abs(vertex.normal[2])});
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
    const char* const names[] = {"cube", "gate", "ship", "station", "cockpit", "asteroid"};
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
    SOL_REQUIRE(parses("name = \"demo\"\n\n# the first\n[[part]]\nid = \"a\"\ntype = \"box\"\n",
                       doc));
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
    SOL_REQUIRE(parses("# a header\nname = \"demo\"\n\n[[part]]\nid = \"a\"\ntype = \"box\"\n",
                       clean));
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
