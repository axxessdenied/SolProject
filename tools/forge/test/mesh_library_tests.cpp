// The Forge's first test suite (engine plan Phase 9, the E1-E3 checkpoint's
// debt slice).
//
// ⚑ AGENTS section 7's rule is that the tool's UI is verified by RUNNING it -
// "there is no suite for a viewport, and pretending otherwise is how game/src
// ended up with 21,000 untested lines" - and that rule is kept here. Nothing
// below opens a window, a device or an ImGui context. What is tested is
// `mesh_library.cpp`, which is arithmetic over a mesh and a def row: it pulls
// in no ImGui and needs no GPU, and it carries a threshold the D checkpoint
// flagged as "a real decision sitting in untested code".

#include "edit_history.hpp"
#include "gltf.hpp"
#include "list_layout.hpp"
#include "mesh_library.hpp"
#include "part_pick.hpp"
#include "status_line.hpp"

#include "sol/assets/forge_doc.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sol;

namespace {

[[nodiscard]] forge::ModelMatch match(float authoredRadius, float measuredRadius)
{
    forge::ModelMatch out;
    out.authoredRadius = authoredRadius;
    out.radiusDelta = measuredRadius - authoredRadius;
    return out;
}

[[nodiscard]] bool buildCommittedMesh(const char* name, assets::MeshData& out)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path.c_str(), bytes)) {
        return false;
    }
    assets::ForgeDoc doc;
    if (!assets::parseForge(reinterpret_cast<const char*>(bytes.data()), bytes.size(), path.c_str(),
                            doc, nullptr)) {
        return false;
    }
    return assets::buildForge(doc, out, nullptr);
}

// Stage N. The document AND its topology, because a part box is a fact about
// the triangles a part emitted and the face list is where that is recorded.
[[nodiscard]] bool loadCommittedTopology(const char* name, assets::ForgeDoc& doc,
                                         std::vector<assets::ForgePoint>& points,
                                         std::vector<assets::ForgeFace>& faces)
{
    const std::string path = std::string(SOL_MESH_SOURCE_DIR) + "/" + name + ".forge";
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path.c_str(), bytes)) {
        return false;
    }
    if (!assets::parseForge(reinterpret_cast<const char*>(bytes.data()), bytes.size(), path.c_str(),
                            doc, nullptr)) {
        return false;
    }
    std::vector<assets::ForgeEdge> edges;
    return assets::forgeTopology(doc, points, edges, faces, nullptr);
}

// ⚑ Every hand-authored source in the repo, so the assertions below run against
// the real spread rather than against the two that are convenient: 28 parts down
// to 1, a flat disc, a baked 99 KB blob and a torus with no parametric points.
constexpr const char* kCommittedParts[] = {"freighter_cockpit", "cockpit",  "ship",
                                           "station",           "gate",     "asteroid",
                                           "cube",              "gate_membrane"};

} // namespace

// ⚑⚑ THE THRESHOLD THE D CHECKPOINT NAMED, PINNED AGAINST THE FIVE CASES THAT
// SET IT. Agreement is RELATIVE, and the gate is the whole reason: it authors
// 106.7 against a measured 106.7005, which is somebody rounding a number they
// read off the Forge's own panel. An absolute tolerance flagged it as a defect.
// One tenth of one percent separates that rounding from the four real
// mismatches in this game by three orders of magnitude - and this is the test
// that stops a future tweak collapsing that gap from either end.
SOL_TEST(theRadiusToleranceSeparatesARoundingFromTheFourRealMismatches)
{
    // The rounding: 0.0005 m on a 107 m ring, 4.7e-6 of the authored value.
    SOL_CHECK(match(106.7f, 106.7005f).radiusAgrees());

    // ⚑⚑ THE FOUR THE TOOL EXISTS TO REPORT, WITH THEIR REAL MEASURED VALUES -
    // AND THEY DO NOT ALL POINT THE SAME WAY, WHICH THE PLAN'S OWN LIST OF
    // "2%, 12%, 13%, 16%" DOES NOT SAY. Positive means the collision sphere is
    // SMALLER than the hull, so ships pass through the picture; negative means
    // it is LARGER, so you stop short of a thing you can still see space around.
    // Two of each, and they are opposite defects.
    SOL_CHECK(!match(100.0f, 102.0f).radiusAgrees());  // station, +2.00%
    SOL_CHECK(!match(1.0f, 1.1584f).radiusAgrees());   // asteroid, +15.84%
    SOL_CHECK(!match(8.0f, 7.0064f).radiusAgrees());   // ship, -12.42%
    SOL_CHECK(!match(1.0f, 0.8660f).radiusAgrees());   // cube, -13.40%

    // The boundary itself, from both sides: one tenth of one percent.
    SOL_CHECK(match(100.0f, 100.09f).radiusAgrees());
    SOL_CHECK(!match(100.0f, 100.2f).radiusAgrees());

    // ⚑ And the absolute floor, which is not decoration: without it a part
    // authored at a millimetre would have a tolerance of a micron, and every
    // float rounding in the build would read as a mismatch.
    SOL_CHECK(match(0.001f, 0.00105f).radiusAgrees());
    SOL_CHECK(!match(0.001f, 0.002f).radiusAgrees());

    // A row that authors no radius at all cannot be said to disagree with one.
    SOL_CHECK(match(0.0f, 0.0f).radiusAgrees());
}

// Signed, and the sign is the finding rather than the magnitude: POSITIVE means
// the sphere the sim builds is SMALLER than the hull that is drawn, so ships
// pass through the picture.
SOL_TEST(theRadiusDeltaPercentIsSignedAndSurvivesAZeroAuthoredRadius)
{
    SOL_CHECK(std::fabs(match(100.0f, 102.0f).radiusDeltaPercent() - 2.0f) < 1e-3f);
    SOL_CHECK(std::fabs(match(1.0f, 1.1584f).radiusDeltaPercent() - 15.84f) < 1e-2f);
    // The other direction: a collision sphere larger than the hull.
    SOL_CHECK(match(100.0f, 98.0f).radiusDeltaPercent() < 0.0f);
    // No division by zero, and no infinity printed into a panel.
    SOL_CHECK(match(0.0f, 5.0f).radiusDeltaPercent() == 0.0f);
}

// ⚑ Matching is by MESH STEM and is deliberately one-to-many: six `[[model]]`
// rows already share five meshes in this game, so a viewer that showed only the
// first would hide the row an author was actually looking for.
SOL_TEST(everyModelRowNamingTheOpenMeshIsMatchedAndNoOthers)
{
    const std::string toml = R"(
[[model]]
id = "a"
mesh = "cube"
texture = "hull"
radius = 1.0

[[model]]
id = "b"
mesh = "cube"
texture = "panel"
radius = 2.0
avoid_radius = 5.0

[[model]]
id = "c"
mesh = "station"
texture = "hull"
radius = 100.0
)";
    assets::DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeToml(toml.c_str(), toml.size(), "test.toml", &error));
    SOL_CHECK(error.empty());

    forge::AssetEntry entry;
    entry.stem = "cube";
    forge::MeshReport report;
    report.boundingRadius = 1.13f;

    const std::vector<forge::ModelMatch> matches = forge::matchModels(defs, entry, report);
    SOL_REQUIRE(matches.size() == 2);
    SOL_CHECK(matches[0].id == "a");
    SOL_CHECK(matches[1].id == "b");
    SOL_CHECK(matches[0].texture == "hull");

    // ⚑ `avoid_radius = 0` in the row means "the same as radius", and it is
    // resolved HERE so the panel does not have to know that rule - two places
    // knowing it is how they come to disagree.
    SOL_CHECK(matches[0].authoredAvoidRadius == 1.0f);
    SOL_CHECK(matches[1].authoredAvoidRadius == 5.0f);

    // The measured radius is set against each row's own authored one.
    SOL_CHECK(std::fabs(matches[0].radiusDelta - 0.13f) < 1e-5f);
    SOL_CHECK(std::fabs(matches[1].radiusDelta - -0.87f) < 1e-5f);
}

// ⚑⚑ AND THE SAME QUESTION ASKED OF THE REAL ASSETS RATHER THAN OF FIXTURES.
// The tolerance is only right if it reports what a human found by eye at stage
// C, so this measures the committed `.forge` sources against the committed
// `[[model]]` rows and pins the verdicts. It fails if somebody changes one of
// those radii - which is correct: those four gaps are known gameplay decisions
// about how close a ship may come, and moving one should be noticed.
//
// ⚑ Measured here, and the numbers are recorded because writing them down is
// what this test is for:
//   gate     106.7000 vs 106.7005   +0.00%   agrees
//   station  100.0000 vs 102.0000   +2.00%   sphere INSIDE the hull
//   asteroid   1.0000 vs   1.1584  +15.84%   sphere INSIDE the hull
//   ship       8.0000 vs   7.0064  -12.42%   sphere OUTSIDE the hull
//   cube       1.0000 vs   0.8660  -13.40%   sphere OUTSIDE the hull
SOL_TEST(theCommittedMeshesAndTheirModelRowsStillDisagreeExactlyWhereTheyDid)
{
    assets::DefDatabase defs;
    std::string error;
    SOL_REQUIRE(forge::loadModelCatalog(SOL_MODEL_DATA_DIR, defs, &error));
    SOL_CHECK(error.empty());
    SOL_REQUIRE(!defs.models().empty());

    struct Expected
    {
        const char* stem;
        bool agrees;
    };
    // The gate is the one that matches; the other four are stage C's findings,
    // recorded in the assets' own headers rather than quietly corrected.
    const Expected expected[] = {
        {"gate", true},   {"station", false}, {"asteroid", false},
        {"ship", false},  {"cube", false},
    };

    for (const Expected& want : expected) {
        assets::MeshData mesh;
        SOL_REQUIRE(buildCommittedMesh(want.stem, mesh));
        const forge::MeshReport report = forge::reportMesh(mesh);

        forge::AssetEntry entry;
        entry.stem = want.stem;
        const std::vector<forge::ModelMatch> matches = forge::matchModels(defs, entry, report);
        SOL_REQUIRE(!matches.empty());
        for (const forge::ModelMatch& found : matches) {
            SOL_CHECK(found.radiusAgrees() == want.agrees);
            // ⚑ The SIGN is pinned as well as the verdict, because the sign is
            // which defect it is and the plan's own list of four percentages
            // does not carry it.
            const bool sphereInsideHull = found.radiusDelta > 0.0f;
            const bool expectInside = std::string(want.stem) == "station" ||
                                      std::string(want.stem) == "asteroid" ||
                                      std::string(want.stem) == "gate";
            SOL_CHECK(sphereInsideHull == expectInside);
        }
    }
}

// ⚑ The headless half of stage G, and it is worth having because the encode is
// the part an author cannot see: the panel shows a colour, the viewport shows a
// lit hull, and neither says whether what reached the GPU is the BC1 chain the
// game will load or the raw RGBA the document built. A tool that is prettier
// than the game is worse than useless, so the property asserted is that the
// tool's own load produces the cooked form.
SOL_TEST(aTextureSourceLoadsAsTheCookedFormRatherThanRawPixels)
{
    const std::vector<forge::AssetEntry> entries =
        forge::listTextures(SOL_TEXTURE_SOURCE_DIR, SOL_TEXTURE_SOURCE_DIR);

    std::size_t sourcesSeen = 0;
    for (const forge::AssetEntry& entry : entries) {
        if (!forge::isTextureSource(entry)) {
            continue;
        }
        ++sourcesSeen;
        assets::TextureData data;
        std::string error;
        if (!forge::loadTexture(entry, data, &error)) {
            std::printf("  %s: %s\n", entry.label.c_str(), error.c_str());
        }
        SOL_REQUIRE(forge::loadTexture(entry, data, &error));
        SOL_CHECK(data.width == 256);
        SOL_CHECK(data.height == 256);
        SOL_CHECK(data.format == assets::TextureFormat::BC1);
        // 256 down to 1 is nine levels, and the chain running all the way is
        // what separates "encoded" from "handed over as one big image".
        SOL_CHECK(data.mips.size() == 9);
        SOL_REQUIRE(!data.mips.empty());
        SOL_CHECK(data.mips[0].size() == 256 / 4 * 256 / 4 * 8);
    }
    SOL_CHECK(sourcesSeen == 3); // never a vacuous pass
}

// ⚑⚑ THE CROSS-REFERENCE THE STRICT SCHEMA DOES NOT CHECK, asserted over the
// game's own committed def files (Phase 9 stage H). `parseShip` reads `model`
// with `optionalString` and never resolves it, so a `[[ship]]` naming a model
// that does not exist LOADS CLEANLY and only surfaces at spawn as a log warning
// behind a fallback that draws something plausible.
//
// This is Phase 16's shape one directory over: an invariant over the shipped
// data rather than a pin on its values. Adding a ship or a station is fine;
// pointing one at a model that is not there is not.
SOL_TEST(noShippedDefNamesAModelThatDoesNotExist)
{
    assets::DefDatabase defs;
    std::string error;
    SOL_REQUIRE(forge::loadModelCatalog(SOL_MODEL_DATA_DIR, defs, &error));
    SOL_REQUIRE(!defs.models().empty());
    // Never a vacuous pass: both def kinds have to be present to be checked.
    SOL_CHECK(defs.ships().size() >= 3);
    SOL_CHECK(defs.stations().size() >= 4);

    const std::vector<forge::MissingModelRef> missing = forge::missingModelRefs(defs);
    for (const forge::MissingModelRef& ref : missing) {
        std::printf("  [[%s]] %s names model '%s', which does not exist\n", ref.defType.c_str(),
                    ref.defId.c_str(), ref.model.c_str());
    }
    SOL_CHECK(missing.empty());
}

// The same check, made to fail - because an invariant that no committed file
// can trip is one nobody has seen work (E4d, and G's channel clamp).
SOL_TEST(aDefNamingAMissingModelIsReported)
{
    assets::DefDatabase db;
    std::string error;
    const std::string toml = R"(
[[model]]
id = "real"
mesh = "m"
texture = "t"

[[ship]]
id = "sol.good"
name = "Good"
model = "real"

[[ship]]
id = "sol.bad"
name = "Bad"
model = "typo"

[[station]]
id = "sol.bad_station"
name = "Bad Station"
model = "also_typo"
)";
    SOL_REQUIRE(db.mergeToml(toml.c_str(), toml.size(), "fixture.toml", &error));

    const std::vector<forge::MissingModelRef> missing = forge::missingModelRefs(db);
    SOL_REQUIRE(missing.size() == 2);
    SOL_CHECK(missing[0].defType == "ship");
    SOL_CHECK(missing[0].defId == "sol.bad");
    SOL_CHECK(missing[0].model == "typo");
    SOL_CHECK(missing[1].defType == "station");
    SOL_CHECK(missing[1].defId == "sol.bad_station");
    SOL_CHECK(missing[1].model == "also_typo");
}

// --- the texture preview's geometry (stage I) --------------------------------

SOL_TEST(thePreviewScaleIsAWholeNumberOfScreenPixelsPerTexturePixel)
{
    // ⚑⚑ THE MEASUREMENT THAT MADE THIS STAGE NECESSARY. The preview shipped at
    // 200 px for a 256 px document. That is 1.28 texture pixels per screen
    // pixel, so a drag could only produce offsets of round(n * 1.28) - and 56
    // of the 257 possible offsets could not be produced at all, starting with
    // 2. Every value in a texture document is an exact integer; a fractional
    // scale is what makes that untrue.
    SOL_CHECK(forge::texturePreviewScale(256, 350.0f) == 1);
    SOL_CHECK(forge::texturePreviewScale(256, 255.0f) == 1); // never below 1:1
    SOL_CHECK(forge::texturePreviewScale(256, 200.0f) == 1); // what shipped
    SOL_CHECK(forge::texturePreviewScale(256, 512.0f) == 2);
    SOL_CHECK(forge::texturePreviewScale(256, 767.0f) == 2); // 2.996 is not 3
    SOL_CHECK(forge::texturePreviewScale(256, 768.0f) == 3);
    // A degenerate document must not divide by zero or scale by zero.
    SOL_CHECK(forge::texturePreviewScale(0, 350.0f) == 1);
    SOL_CHECK(forge::texturePreviewScale(-8, 350.0f) == 1);
}

SOL_TEST(aCursorMapsToTheTexturePixelUnderIt)
{
    const core::Vec2 origin{100.0f, 40.0f};
    int x = -1;
    int y = -1;

    // At 1:1 the mapping is a translation, and the pixel is the one the cursor
    // is INSIDE - the whole of [100, 101) is column 0.
    SOL_REQUIRE(forge::texturePixelAt({100.0f, 40.0f}, origin, 1, 256, 256, x, y));
    SOL_CHECK(x == 0 && y == 0);
    SOL_REQUIRE(forge::texturePixelAt({100.9f, 40.9f}, origin, 1, 256, 256, x, y));
    SOL_CHECK(x == 0 && y == 0);
    SOL_REQUIRE(forge::texturePixelAt({101.0f, 41.0f}, origin, 1, 256, 256, x, y));
    SOL_CHECK(x == 1 && y == 1);

    // At 2x a texture pixel is two screen pixels wide, both of them its own.
    SOL_REQUIRE(forge::texturePixelAt({103.0f, 40.0f}, origin, 2, 256, 256, x, y));
    SOL_CHECK(x == 1 && y == 0);
    SOL_REQUIRE(forge::texturePixelAt({104.0f, 40.0f}, origin, 2, 256, 256, x, y));
    SOL_CHECK(x == 2 && y == 0);

    // ⚑ Outside is REFUSED rather than clamped. A cursor above or left of the
    // image would otherwise land on row 0 - a cast to int truncates toward
    // zero, so -0.5 becomes 0 - and a click just off the top edge would grab
    // whatever sits in the corner.
    SOL_CHECK(!forge::texturePixelAt({99.5f, 40.0f}, origin, 1, 256, 256, x, y));
    SOL_CHECK(!forge::texturePixelAt({100.0f, 39.5f}, origin, 1, 256, 256, x, y));
    SOL_CHECK(!forge::texturePixelAt({356.0f, 40.0f}, origin, 1, 256, 256, x, y));
    SOL_CHECK(!forge::texturePixelAt({100.0f, 296.0f}, origin, 1, 256, 256, x, y));
}

SOL_TEST(aDragOffsetIsRoundedOnceFromTheWholeGestureAndNeverAccumulated)
{
    // ⚑⚑ ASSERTION (4) OF STAGE I, AND THE INPUT HAD TO BE BUILT BECAUSE NO
    // REAL MOUSE PRODUCES IT RELIABLY. `PointTool` drags on a per-frame
    // cursorDelta, which is correct for a mesh authored in double. Here every
    // write is an integer, and the two are not the same arithmetic: a hand
    // moving slowly enough that each FRAME rounds to zero still moves.
    const float frames[] = {0.4f, 0.8f, 1.2f, 1.6f, 2.0f};
    int accumulated = 0;
    float previous = 0.0f;
    for (const float position : frames) {
        accumulated += forge::textureDragOffset(previous, position, 1);
        previous = position;
    }
    // Five frames of 0.4 px each round to zero on their own...
    SOL_CHECK(accumulated == 0);
    // ...while the gesture plainly travelled two pixels, which is what a caller
    // holding the START of the drag reads.
    SOL_CHECK(forge::textureDragOffset(0.0f, 2.0f, 1) == 2);

    // The rule itself, at 1:1 and scaled.
    SOL_CHECK(forge::textureDragOffset(10.0f, 10.0f, 1) == 0); // a click is not a drag
    SOL_CHECK(forge::textureDragOffset(10.0f, 13.0f, 1) == 3);
    SOL_CHECK(forge::textureDragOffset(10.0f, 7.0f, 1) == -3);
    SOL_CHECK(forge::textureDragOffset(10.0f, 16.0f, 2) == 3);
    // Rounding is symmetric about zero, or a drag left would travel further
    // than the same drag right.
    SOL_CHECK(forge::textureDragOffset(0.0f, 1.5f, 1) == 2);
    SOL_CHECK(forge::textureDragOffset(0.0f, -1.5f, 1) == -2);
    SOL_CHECK(forge::textureDragOffset(0.0f, 100.0f, 0) == 0);
}

// --- the Blender bridge (stage L) --------------------------------------------

namespace {

// Two nodes pointing at one triangle mesh, ten metres apart, named the way
// Blender names things: a duplicate suffixed `.001`, and a name with a space.
constexpr const char* kTwoNodeGltf = R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"mesh": 0, "name": "Hull.001"},
        {"mesh": 0, "name": "Wing L", "translation": [10, 0, 0]}
    ],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "buffers": [{"byteLength": 42, "uri":
        "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ]
})";

[[nodiscard]] std::string writeFixture(const char* name, const char* text)
{
    const std::string path = std::string(platform::executableDirectory()) + name;
    if (!platform::writeFileBytes(path.c_str(), text, std::strlen(text))) {
        return {};
    }
    return path;
}

[[nodiscard]] bool hasPart(const assets::ForgeDoc& doc, const char* id)
{
    return doc.indexOf(id) != std::string::npos;
}

} // namespace

// ⚑ A Blender object name is not a part id, and the two cases that matter both
// come from ordinary use: duplicating an object appends `.001`, and nothing
// stops a name carrying a space.
SOL_TEST(aBlenderObjectNameBecomesAUsablePartId)
{
    SOL_CHECK(forge::forgePartIdFromName("Hull") == "Hull");
    SOL_CHECK(forge::forgePartIdFromName("Hull.001") == "Hull_001");
    SOL_CHECK(forge::forgePartIdFromName("Wing L") == "Wing_L");
    // Runs collapse rather than stacking underscores, and edges are trimmed, so
    // a decorated name stays readable in the Parts panel.
    SOL_CHECK(forge::forgePartIdFromName("front -- nose") == "front_nose");
    SOL_CHECK(forge::forgePartIdFromName(".hidden.") == "hidden");
    // A name made entirely of separators still has to produce something legal:
    // an empty id fails parseForge, so the import would write a file it could
    // not read back.
    SOL_CHECK(forge::forgePartIdFromName("...") == "part");
    SOL_CHECK(forge::forgePartIdFromName("") == "part");
}

// ⚑⚑ THE STAGE'S EXIT CRITERION AS ONE ASSERTION: the mesh the GAME gets out of
// the imported `.forge` is the mesh the COOKER would have got out of the glTF
// directly. It goes the whole way round - import, bake to `mesh` parts, write
// TOML, parse it back, build - because every one of those steps is a place the
// geometry could quietly change, and the `.forge` is what ships from here on.
SOL_TEST(aGltfImportedAsPartsBuildsBackToTheMeshTheCookerWouldHaveCooked)
{
    const std::string path = writeFixture("test_bridge_two.gltf", kTwoNodeGltf);
    SOL_CHECK(!path.empty());

    assets::MeshData direct;
    SOL_CHECK(cooker::importGltf(path.c_str(), direct));

    assets::ForgeDoc doc;
    forge::ImportOutcome outcome;
    SOL_CHECK(forge::importGltfIntoDoc(path, doc, outcome, nullptr));
    SOL_CHECK(outcome.added.size() == 2);
    SOL_CHECK(outcome.replaced.empty());

    // Through the text, which is the part that actually ships.
    const std::string text = assets::writeForge(doc);
    assets::ForgeDoc reparsed;
    std::string error;
    SOL_CHECK(assets::parseForge(text.c_str(), text.size(), "bridge", reparsed, &error));
    assets::MeshData rebuilt;
    SOL_CHECK(assets::buildForge(reparsed, rebuilt, &error));

    SOL_CHECK(rebuilt.vertices.size() == direct.vertices.size());
    SOL_CHECK(rebuilt.indices.size() == direct.indices.size());
    for (std::size_t i = 0; i < direct.vertices.size(); ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            SOL_CHECK(rebuilt.vertices[i].position[axis] == direct.vertices[i].position[axis]);
            SOL_CHECK(rebuilt.vertices[i].normal[axis] == direct.vertices[i].normal[axis]);
        }
    }
    for (std::size_t i = 0; i < direct.indices.size(); ++i) {
        SOL_CHECK(rebuilt.indices[i] == direct.indices[i]);
    }

    std::remove(path.c_str());
}

// ⚑ ONE PART PER NODE, WITH THE NODE TRANSFORM BAKED IN. A merged import would
// pass the round-trip test above just as happily while handing the author one
// opaque part - so the count and the naming carry their own assertion.
SOL_TEST(eachBlenderObjectArrivesAsItsOwnNamedPart)
{
    const std::string path = writeFixture("test_bridge_named.gltf", kTwoNodeGltf);
    SOL_CHECK(!path.empty());

    assets::ForgeDoc doc;
    forge::ImportOutcome outcome;
    SOL_CHECK(forge::importGltfIntoDoc(path, doc, outcome, nullptr));

    SOL_CHECK(doc.parts.size() == 2);
    SOL_CHECK(hasPart(doc, "Hull_001"));
    SOL_CHECK(hasPart(doc, "Wing_L"));
    // The document names itself after the file, so a fresh import writes a
    // `.forge` that is complete rather than one missing its `name` key.
    SOL_CHECK(doc.name == "test_bridge_named");

    // Both are literal geometry, which is what makes every stage E-I tool work
    // on them: it is exactly what `bake` produces.
    for (const assets::ForgePart& part : doc.parts) {
        SOL_CHECK(part.primitive == assets::ForgePrimitive::Mesh);
        // ⚑ And no placement, because the node transform is already in the
        // vertices. A part carrying both would draw its translation twice.
        SOL_CHECK(part.position.x == 0.0 && part.position.y == 0.0 && part.position.z == 0.0);
        SOL_CHECK(part.scale.x == 1.0 && part.scale.y == 1.0 && part.scale.z == 1.0);
    }

    // The second node's +10 X translation reached the geometry rather than the
    // placement, so the two parts do not sit on top of each other.
    assets::MeshData built;
    SOL_CHECK(assets::buildForge(doc, built, nullptr));
    float maxX = 0.0f;
    for (const assets::MeshVertex& vertex : built.vertices) {
        maxX = std::fmax(maxX, vertex.position[0]);
    }
    SOL_CHECK(std::fabs(maxX - 11.0f) < 1e-5f);

    std::remove(path.c_str());
}

// ⚑⚑ THE RULE A SECOND EXPORT FROM BLENDER DEPENDS ON, AND THE ONE THAT MAKES
// THE BRIDGE USABLE MORE THAN ONCE. Re-importing replaces the parts the glTF
// names and LEAVES EVERYTHING ELSE - so an author can bring a hull over from
// Blender, add a `beam` in the Forge, re-export the hull, and still have their
// beam. Overwriting the file wholesale is the obvious implementation and it
// silently deletes their work.
SOL_TEST(aSecondImportReplacesItsOwnPartsAndKeepsTheAuthorsOwn)
{
    const std::string path = writeFixture("test_bridge_again.gltf", kTwoNodeGltf);
    SOL_CHECK(!path.empty());

    assets::ForgeDoc doc;
    forge::ImportOutcome first;
    SOL_CHECK(forge::importGltfIntoDoc(path, doc, first, nullptr));

    // What an author does next: add a part of their own, and comment one of
    // the imported ones.
    assets::ForgePart strut;
    strut.id = "strut";
    strut.primitive = assets::ForgePrimitive::Box;
    doc.parts.push_back(strut);
    const std::size_t hull = doc.indexOf("Hull_001");
    SOL_CHECK(hull != std::string::npos);
    doc.parts[hull].leading = "# the bit that came from Blender\n";
    // And nudges it, which the re-import must NOT preserve - the geometry
    // coming back already carries Blender's own placement.
    doc.parts[hull].position = {5.0, 0.0, 0.0};

    forge::ImportOutcome second;
    SOL_CHECK(forge::importGltfIntoDoc(path, doc, second, nullptr));

    SOL_CHECK(second.replaced.size() == 2);
    SOL_CHECK(second.added.empty());
    SOL_CHECK(second.kept.size() == 1 && second.kept[0] == "strut");

    // Three parts, not five: matched BY ID rather than appended.
    SOL_CHECK(doc.parts.size() == 3);
    SOL_CHECK(hasPart(doc, "strut"));

    const std::size_t rehulled = doc.indexOf("Hull_001");
    SOL_CHECK(rehulled != std::string::npos);
    // The comment above it survives - the writer is faithful and an import that
    // ate an author's note would be the one thing this format exists to prevent.
    SOL_CHECK(doc.parts[rehulled].leading == "# the bit that came from Blender\n");
    // The nudge does not, which is the deliberate half of the rule.
    SOL_CHECK(doc.parts[rehulled].position.x == 0.0);

    std::remove(path.c_str());
}

// --- identity that survives a rename (stage P) --------------------------------

namespace {

// The same two nodes as `kTwoNodeGltf`, now carrying the uid the addon stamps.
// `extras` is where a glTF exporter puts its own data, and it is the only field
// here that is orthogonal to the name.
constexpr const char* kIdentifiedGltf = R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"mesh": 0, "name": "Hull.001", "extras": {"sol_forge_uid": "uid-hull"}},
        {"mesh": 0, "name": "Wing L", "translation": [10, 0, 0],
         "extras": {"sol_forge_uid": "uid-wing"}}
    ],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "buffers": [{"byteLength": 42, "uri":
        "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ]
})";

// The author renamed `Hull.001` to `Fuselage` in Blender. Same uid, same
// everything else - which is exactly the information the name cannot carry.
constexpr const char* kRenamedGltf = R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"mesh": 0, "name": "Fuselage", "extras": {"sol_forge_uid": "uid-hull"}},
        {"mesh": 0, "name": "Wing L", "translation": [10, 0, 0],
         "extras": {"sol_forge_uid": "uid-wing"}}
    ],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "buffers": [{"byteLength": 42, "uri":
        "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ]
})";

// What `Shift+D` actually produces: a second object wearing the FIRST one's
// uid, because Blender copies custom properties on duplicate. Measured against
// Blender 5.2, not assumed.
constexpr const char* kDuplicatedUidGltf = R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"mesh": 0, "name": "Hull", "extras": {"sol_forge_uid": "uid-hull"}},
        {"mesh": 0, "name": "Hull.001", "translation": [10, 0, 0],
         "extras": {"sol_forge_uid": "uid-hull"}}
    ],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "buffers": [{"byteLength": 42, "uri":
        "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ]
})";

} // namespace

// ⚑⚑ THE ASSERTION THAT SEPARATES STAGE P FROM WHAT IT REPLACES. Before it, a
// rename was indistinguishable from a delete plus an add: the renamed object
// missed every part and was ADDED, and the part it used to be was never claimed
// so it survived in `kept` - two lumps in one place, with the live geometry in
// the copy carrying none of the author's placement. The part count is the
// cheapest way to say that, and `renamed` is the way the tool says it out loud.
SOL_TEST(aRenamedBlenderObjectUpdatesItsPartInsteadOfLeavingADuplicate)
{
    const std::string before = writeFixture("test_rename_before.gltf", kIdentifiedGltf);
    const std::string after = writeFixture("test_rename_after.gltf", kRenamedGltf);
    SOL_CHECK(!before.empty() && !after.empty());

    assets::ForgeDoc doc;
    forge::ImportOutcome first;
    SOL_CHECK(forge::importGltfIntoDoc(before, doc, first, nullptr));
    SOL_CHECK(doc.parts.size() == 2);
    SOL_CHECK(first.added.size() == 2);
    // The identity landed in the document, or nothing below can work.
    const std::size_t hull = doc.indexOf("Hull_001");
    SOL_CHECK(hull != std::string::npos && doc.parts[hull].origin == "uid-hull");

    forge::ImportOutcome second;
    SOL_CHECK(forge::importGltfIntoDoc(after, doc, second, nullptr));

    // Still two parts. Under the old rule this was three.
    SOL_CHECK(doc.parts.size() == 2);
    SOL_CHECK(second.added.empty());
    // And the part that "disappeared" is not sitting in `kept` waiting to be
    // deleted by hand - it is the same part, renamed.
    SOL_CHECK(second.kept.empty());
    // REQUIRE rather than CHECK: the two lines below index it, and a mutant
    // that empties this vector should report a failed check rather than take
    // the process out with a subscript assertion.
    SOL_REQUIRE(second.renamed.size() == 1);
    SOL_CHECK(second.renamed[0].first == "Hull_001");
    SOL_CHECK(second.renamed[0].second == "Fuselage");
    SOL_CHECK(second.replaced.size() == 1 && second.replaced[0] == "Wing_L");

    SOL_CHECK(hasPart(doc, "Fuselage"));
    SOL_CHECK(!hasPart(doc, "Hull_001"));

    std::remove(before.c_str());
    std::remove(after.c_str());
}

// ⚑ A PART IS NAMED BY ITS CHILDREN, so a rename that stops at the part leaves
// them parented to something that no longer exists - which `worldTransforms`
// reads as "no parent" and silently draws at the document origin. A part whose
// children quietly teleport is a worse outcome than the duplicate this stage
// exists to remove.
SOL_TEST(aRenamedPartIsStillNamedByTheChildrenItCarries)
{
    const std::string before = writeFixture("test_rename_kid_before.gltf", kIdentifiedGltf);
    const std::string after = writeFixture("test_rename_kid_after.gltf", kRenamedGltf);
    SOL_CHECK(!before.empty() && !after.empty());

    assets::ForgeDoc doc;
    forge::ImportOutcome first;
    SOL_CHECK(forge::importGltfIntoDoc(before, doc, first, nullptr));

    // What an author does: hang something of their own off the imported hull.
    assets::ForgePart antenna;
    antenna.id = "antenna";
    antenna.primitive = assets::ForgePrimitive::Box;
    antenna.parent = "Hull_001";
    doc.parts.push_back(antenna);

    forge::ImportOutcome second;
    SOL_CHECK(forge::importGltfIntoDoc(after, doc, second, nullptr));

    const std::size_t child = doc.indexOf("antenna");
    SOL_CHECK(child != std::string::npos);
    SOL_CHECK(doc.parts[child].parent == "Fuselage");
    // The author's own part is still theirs - it is kept, not renamed.
    SOL_CHECK(second.kept.size() == 1 && second.kept[0] == "antenna");
    // And it never acquires an origin: an origin means "this came from
    // Blender", and `kept` could not tell the two apart otherwise.
    SOL_CHECK(doc.parts[child].origin.empty());

    std::remove(before.c_str());
    std::remove(after.c_str());
}

// ⚑⚑ THE HAZARD THAT MAKES THE NAIVE VERSION OF THIS STAGE WORSE THAN WHAT IT
// REPLACES, ASSERTED RATHER THAN TRUSTED. Blender copies custom properties on
// `Shift+D` and `Alt+D` alike, so two objects can arrive wearing one uid. The
// addon re-stamps at send time, but the importer must not fall over if one
// slips through: matching an ALREADY-CLAIMED part would let the second node
// overwrite the first and silently drop a part.
SOL_TEST(twoObjectsWearingOneUidStillArriveAsTwoParts)
{
    const std::string path = writeFixture("test_dup_uid.gltf", kDuplicatedUidGltf);
    SOL_CHECK(!path.empty());

    assets::ForgeDoc doc;
    forge::ImportOutcome outcome;
    SOL_CHECK(forge::importGltfIntoDoc(path, doc, outcome, nullptr));

    SOL_CHECK(doc.parts.size() == 2);
    SOL_CHECK(outcome.added.size() == 2);
    SOL_CHECK(hasPart(doc, "Hull") && hasPart(doc, "Hull_001"));

    // Re-importing the same file is still two parts, not four: the first node
    // takes the first part and the second cannot take it again.
    forge::ImportOutcome again;
    SOL_CHECK(forge::importGltfIntoDoc(path, doc, again, nullptr));
    SOL_CHECK(doc.parts.size() == 2);
    SOL_CHECK(again.added.empty());

    std::remove(path.c_str());
}

// ⚑⚑ THE OTHER HALF OF THE FALLBACK RULE, AND THE DESTRUCTIVE ONE. A name may
// only match a part that has NEVER been identified. Without that clause a NEW
// object that happens to take an existing part's name walks straight into it
// and overwrites another object's work - the same destruction stage L already
// had to fix once, arriving by a different door.
//
// It is written from the doc side rather than through a second glTF because
// what is being pinned is the RULE, not the file: an origin, once present,
// outranks the name.
SOL_TEST(aNewObjectDoesNotCaptureThePartOfAnIdentifiedOneWithTheSameName)
{
    const std::string path = writeFixture("test_name_capture.gltf", kIdentifiedGltf);
    SOL_CHECK(!path.empty());

    // A document whose `Hull_001` already belongs to some OTHER Blender object.
    assets::ForgeDoc doc;
    assets::ForgePart squatter;
    squatter.id = "Hull_001";
    squatter.primitive = assets::ForgePrimitive::Box;
    squatter.origin = "uid-somebody-else";
    doc.parts.push_back(squatter);

    forge::ImportOutcome outcome;
    SOL_CHECK(forge::importGltfIntoDoc(path, doc, outcome, nullptr));

    // The incoming `Hull.001` carries `uid-hull`, which matches nothing here,
    // so it must arrive as a NEW part beside the squatter rather than eating it.
    SOL_CHECK(doc.parts.size() == 3);
    SOL_CHECK(outcome.replaced.empty() && outcome.renamed.empty());
    SOL_CHECK(outcome.added.size() == 2);
    SOL_CHECK(outcome.kept.size() == 1 && outcome.kept[0] == "Hull_001");

    // The squatter is untouched and still owned by whoever owned it.
    const std::size_t kept = doc.indexOf("Hull_001");
    SOL_CHECK(kept != std::string::npos);
    SOL_CHECK(doc.parts[kept].origin == "uid-somebody-else");
    SOL_CHECK(doc.parts[kept].primitive == assets::ForgePrimitive::Box);
    // And the newcomer took a suffixed id rather than the taken one.
    SOL_REQUIRE(hasPart(doc, "Hull_001_2"));
    SOL_CHECK(doc.parts[doc.indexOf("Hull_001_2")].origin == "uid-hull");

    std::remove(path.c_str());
}

// ⚑ THE MIGRATION PATH, WHICH IS THE ONLY REASON NAME MATCHING SURVIVES AT ALL.
// Every part in every committed `.forge` predates this stage and carries no
// origin, so the first send after it must still find its target by name - and
// must RECORD the origin, or the second send is in the same position as the
// first and a rename never becomes possible.
SOL_TEST(aPartWithNoOriginIsMatchedByNameOnceAndCarriesOneAfterwards)
{
    const std::string old = writeFixture("test_migrate_old.gltf", kTwoNodeGltf);
    const std::string identified = writeFixture("test_migrate_new.gltf", kIdentifiedGltf);
    const std::string renamed = writeFixture("test_migrate_renamed.gltf", kRenamedGltf);
    SOL_CHECK(!old.empty() && !identified.empty() && !renamed.empty());

    // A document as it exists today: imported before stage P, no identities.
    assets::ForgeDoc doc;
    forge::ImportOutcome first;
    SOL_CHECK(forge::importGltfIntoDoc(old, doc, first, nullptr));
    SOL_CHECK(doc.parts.size() == 2);
    for (const assets::ForgePart& part : doc.parts) {
        SOL_CHECK(part.origin.empty());
    }

    // The first send from the upgraded addon: matched by name, and identified.
    forge::ImportOutcome second;
    SOL_CHECK(forge::importGltfIntoDoc(identified, doc, second, nullptr));
    SOL_CHECK(doc.parts.size() == 2);
    SOL_CHECK(second.replaced.size() == 2 && second.added.empty() && second.renamed.empty());
    SOL_REQUIRE(hasPart(doc, "Hull_001") && hasPart(doc, "Wing_L"));
    SOL_CHECK(doc.parts[doc.indexOf("Hull_001")].origin == "uid-hull");
    SOL_CHECK(doc.parts[doc.indexOf("Wing_L")].origin == "uid-wing");

    // And now a rename works, which it could not have before the send above.
    forge::ImportOutcome third;
    SOL_CHECK(forge::importGltfIntoDoc(renamed, doc, third, nullptr));
    SOL_CHECK(doc.parts.size() == 2);
    SOL_CHECK(third.renamed.size() == 1 && third.added.empty());

    std::remove(old.c_str());
    std::remove(identified.c_str());
    std::remove(renamed.c_str());
}

// ⚑⚑ STAGE M. These pin the height rule to numbers MEASURED FROM THE RUNNING
// TOOL rather than derived on paper. Before the change, the parts list reported
// `scrollMaxY 318` at its shipped 170 px with 28 rows, and the panel list 1252
// at 140 px with 60 - so the height at which each would stop scrolling was
// 170 + 318 = 488 and 140 + 1252 = 1392. Those two numbers are the fixture: if
// the formula ever stops reproducing them it has stopped describing ImGui.
namespace {

// The Forge's own style at the default font: 13 px ProggyClean, ItemSpacing.y 4,
// WindowPadding.y 8. No font is loaded (imgui_host.cpp calls only
// StyleColorsDark), which is what makes these constants legitimate.
constexpr forge::ListMetrics kTextRows{17.0f, 4.0f, 8.0f};
constexpr forge::ListMetrics kFrameRows{23.0f, 4.0f, 8.0f};

} // namespace

SOL_TEST(theExactHeightReproducesWhatTheRunningToolMeasured)
{
    // 28 parts of freighter_cockpit.forge, at a text row's pitch.
    SOL_CHECK(forge::listHeightForRows(kTextRows, 28) == 488.0f);
    // 60 panel rows of hull.tex, which are DRAG WIDGETS and 6 px taller each.
    SOL_CHECK(forge::listHeightForRows(kFrameRows, 60) == 1392.0f);

    // ⚑ And the mixed case, which is the one that caught a wrong prediction: the
    // mesh list is 2 CollapsingHeaders (framed, 23 px) plus 8 Selectables (17).
    // Sized as ten uniform rows it came out 182 and still scrolled by 12; the
    // right answer is 194, and only a per-row pitch can express that.
    const float mixed = 2.0f * kFrameRows.rowPitch + 8.0f * kTextRows.rowPitch;
    SOL_CHECK(forge::listHeightForContent(kTextRows, mixed, 0, 1.0f, 10000.0f) == 194.0f);
    SOL_CHECK(forge::listHeightForRows(kTextRows, 10) == 182.0f);
}

SOL_TEST(aListShowsExactlyTheRowsItWasSizedForAndNotOneMore)
{
    // The two directions must agree, or the tool would size for n and show n-1.
    for (std::size_t rows = 1; rows <= 60; ++rows) {
        const float exact = forge::listHeightForRows(kTextRows, rows);
        SOL_CHECK(forge::listRowsForHeight(kTextRows, exact) == rows);
        // One pixel short and the last row no longer fits: the fit is tight
        // rather than accidentally generous.
        SOL_CHECK(forge::listRowsForHeight(kTextRows, exact - 1.0f) == rows - 1);
    }
}

SOL_TEST(theShareCapsAListAndTheFloorOutranksTheShare)
{
    // Measured: at an 817 px panel, 0.45 gives the parts list 367 px, which is
    // 20 of 28 rows where the shipped 170 px gave 9.
    const float capped = forge::listHeight(kTextRows, 28, forge::kMinListRows, 0.45f, 817.0f);
    SOL_CHECK(capped > 367.0f && capped < 368.0f);
    SOL_CHECK(forge::listRowsForHeight(kTextRows, capped) == 20);
    SOL_CHECK(forge::listRowsForHeight(kTextRows, 170.0f) == 9);

    // Measured: dragging the panel to 978 px takes the same list to 440 px and
    // 25 rows. This is the whole point of the stage - a fixed height cannot
    // satisfy it, so no regression to a constant can pass this assertion.
    const float taller = forge::listHeight(kTextRows, 28, forge::kMinListRows, 0.45f, 978.0f);
    SOL_CHECK(taller > capped);
    SOL_CHECK(forge::listRowsForHeight(kTextRows, taller) == 25);

    // A list that already fits is NOT grown to fill the share: the anti-waste
    // half, which is what reclaims 40 px from the four-row op list.
    const float fits = forge::listHeight(kTextRows, 4, forge::kMinListRows, 0.45f, 817.0f);
    SOL_CHECK(fits == forge::listHeightForRows(kTextRows, 4));

    // ⚑ THE FLOOR WINS A SHORT PANEL OUTRIGHT, AND THIS IS THE ASSERTION THAT
    // WOULD CATCH AN INVERTED CLAMP. Measured live at a 128 px panel: the cap
    // works out at 0.30 * 128 = 38.4, below the 4-row floor of 80, and the list
    // came back at exactly 80.0 rather than at 38 or at something negative.
    const float floored = forge::listHeight(kTextRows, 21, forge::kMinListRows, 0.30f, 128.0f);
    SOL_CHECK(floored == 80.0f);
    SOL_CHECK(floored == forge::listHeightForRows(kTextRows, forge::kMinListRows));

    // An empty document still gets the floor rather than a sliver.
    SOL_CHECK(forge::listHeight(kTextRows, 0, forge::kMinListRows, 0.45f, 817.0f) == 80.0f);
    // And a degenerate panel cannot produce a negative height.
    SOL_CHECK(forge::listHeight(kTextRows, 28, forge::kMinListRows, 0.45f, 0.0f) == 80.0f);
}

SOL_TEST(thePartFilterMatchesTheWayAnAuthorTypes)
{
    // Empty needle keeps everything, which is what an untouched filter box means.
    SOL_CHECK(forge::listMatchesFilter("hull_2a", ""));

    // Case-insensitive in both directions: Blender capitalises object names and
    // nobody types the capital.
    SOL_CHECK(forge::listMatchesFilter("Fin_001", "fin"));
    SOL_CHECK(forge::listMatchesFilter("fin_001", "FIN"));

    // A SUBSTRING, not a prefix - the 40-object case is `Fin_001`..`Fin_012`
    // alongside `left_fin`, and a prefix match would miss half of them.
    SOL_CHECK(forge::listMatchesFilter("left_fin", "fin"));
    SOL_CHECK(forge::listMatchesFilter("hull_2a", "l_2"));

    SOL_CHECK(!forge::listMatchesFilter("hull_2a", "wing"));
    // A needle longer than the label can never match, and must not read past it.
    SOL_CHECK(!forge::listMatchesFilter("fin", "fin_001"));
    SOL_CHECK(!forge::listMatchesFilter("", "fin"));
    SOL_CHECK(forge::listMatchesFilter("", ""));

    // ⚑ High-bit bytes must not be folded: a part id is a sanitised Blender
    // object name and arrives as UTF-8. This is the case std::tolower would
    // have made undefined behaviour.
    SOL_CHECK(forge::listMatchesFilter("caf\xC3\xA9_strut", "strut"));
    SOL_CHECK(!forge::listMatchesFilter("caf\xC3\xA9_strut", "cafe"));
}

// ⚑⚑ STAGE N's CENTRAL ASSERTION, AND IT IS TWO PROPERTIES RATHER THAN A
// RECOMPUTATION. Writing min/max a second time here would be a second
// implementation of the thing under test, agreeing with it by construction and
// catching nothing. Instead: CONTAINMENT - every corner of every triangle a part
// emitted lies inside that part's box - rules out a box that is too SMALL, and
// TIGHTNESS - each of the six planes is touched by at least one corner - rules
// out one that is too LARGE. Together they pin the box exactly.
//
// ⚑ Compared with `==` rather than a tolerance on purpose: the bounds are copied
// verbatim out of `ForgePoint::position`, so a touching plane is bitwise equal to
// the corner that set it. A tolerance would hide a systematic drift, which is
// the only interesting way this can be wrong.
SOL_TEST(everyPartsBoxHoldsItsOwnTrianglesAndNothingSpare)
{
    for (const char* name : kCommittedParts) {
        assets::ForgeDoc doc;
        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(loadCommittedTopology(name, doc, points, faces));

        std::vector<forge::PartBounds> bounds;
        forge::forgePartBounds(points, faces, doc.parts.size(), bounds);
        SOL_REQUIRE(bounds.size() == doc.parts.size());

        for (const assets::ForgeFace& face : faces) {
            if (face.part >= doc.parts.size()) {
                continue;
            }
            const forge::PartBounds& box = bounds[face.part];
            SOL_CHECK(box.any);
            const std::uint32_t corners[3] = {face.a, face.b, face.c};
            for (const std::uint32_t corner : corners) {
                const assets::BuildPoint& p = points[corner].position;
                SOL_CHECK(p.x >= box.min.x && p.x <= box.max.x);
                SOL_CHECK(p.y >= box.min.y && p.y <= box.max.y);
                SOL_CHECK(p.z >= box.min.z && p.z <= box.max.z);
            }
        }

        std::vector<int> touched(doc.parts.size() * 6, 0);
        for (const assets::ForgeFace& face : faces) {
            if (face.part >= doc.parts.size()) {
                continue;
            }
            const forge::PartBounds& box = bounds[face.part];
            const std::uint32_t corners[3] = {face.a, face.b, face.c};
            for (const std::uint32_t corner : corners) {
                const assets::BuildPoint& p = points[corner].position;
                int* hit = touched.data() + face.part * 6;
                hit[0] += p.x == box.min.x ? 1 : 0;
                hit[1] += p.y == box.min.y ? 1 : 0;
                hit[2] += p.z == box.min.z ? 1 : 0;
                hit[3] += p.x == box.max.x ? 1 : 0;
                hit[4] += p.y == box.max.y ? 1 : 0;
                hit[5] += p.z == box.max.z ? 1 : 0;
            }
        }
        for (std::size_t part = 0; part < doc.parts.size(); ++part) {
            if (!bounds[part].any) {
                continue;
            }
            for (std::size_t plane = 0; plane < 6; ++plane) {
                SOL_CHECK(touched[(part * 6) + plane] > 0);
            }
        }
    }
}

// ⚑⚑ THE BOX VECTOR IS INDEXED BY THE PANEL's OWN PART INDEX, AND THAT IS WHAT A
// "SKIP THE EMPTY ONES" OPTIMISATION WOULD SILENTLY BREAK. The viewport hands
// `PartEditor::selectPart` a raw index into `doc.parts`; if this vector were
// packed to only the parts that emitted geometry, every part after the first
// empty one would light up the wrong box - and on the committed assets, where
// nothing is empty, the defect would be invisible.
SOL_TEST(thereIsOneBoxPerDocumentPartWhetherOrNotItHasGeometry)
{
    for (const char* name : kCommittedParts) {
        assets::ForgeDoc doc;
        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(loadCommittedTopology(name, doc, points, faces));

        std::vector<forge::PartBounds> bounds;
        forge::forgePartBounds(points, faces, doc.parts.size(), bounds);
        SOL_REQUIRE(bounds.size() == doc.parts.size());

        for (std::size_t part = 0; part < doc.parts.size(); ++part) {
            bool emitted = false;
            for (const assets::ForgeFace& face : faces) {
                emitted = emitted || face.part == part;
            }
            SOL_CHECK(bounds[part].any == emitted);
        }
    }

    // The headline case, pinned: 28 parts is the largest thing in the repo, and
    // stage L's one-part-per-Blender-object rule is what takes it past forty.
    assets::ForgeDoc cockpit;
    std::vector<assets::ForgePoint> points;
    std::vector<assets::ForgeFace> faces;
    SOL_REQUIRE(loadCommittedTopology("freighter_cockpit", cockpit, points, faces));
    SOL_CHECK(cockpit.parts.size() == 28);
    std::vector<forge::PartBounds> bounds;
    forge::forgePartBounds(points, faces, cockpit.parts.size(), bounds);
    for (const forge::PartBounds& box : bounds) {
        SOL_CHECK(box.any); // every one of the 28 is clickable
    }
}

// ⚑⚑ THE NO-OWNER SENTINEL IS ONE PAST THE END AND IT IS NOT HYPOTHETICAL:
// `collectPoints` does `ownerOf.assign(mesh.vertices.size(), doc.parts.size())`
// and only overwrites it inside a part's own vertex range, so a vertex produced
// by a `[build]` post-pass belongs to no part. Indexing on it is an
// out-of-range read a release build does not catch.
//
// ⚑ Built by hand rather than found in the repo, because no committed asset
// produces one - E4d's rule again: a mutation no asset can see still needs a
// can-fail test.
SOL_TEST(aTriangleThatBelongsToNoPartSelectsNothing)
{
    std::vector<assets::ForgePoint> points(3);
    points[0].position = {0.0, 0.0, 0.0};
    points[1].position = {1.0, 0.0, 0.0};
    points[2].position = {0.0, 1.0, 0.0};

    constexpr std::size_t kPartCount = 2;
    std::vector<assets::ForgeFace> faces;
    faces.push_back({0, 1, 2, kPartCount, 0}); // the sentinel

    SOL_CHECK(forge::forgePartOfFace(faces, 0, kPartCount) == forge::kNoPart);
    // And past the end of the face list, which is what a missed ray produces one
    // call site up.
    SOL_CHECK(forge::forgePartOfFace(faces, 1, kPartCount) == forge::kNoPart);

    std::vector<forge::PartBounds> bounds;
    forge::forgePartBounds(points, faces, kPartCount, bounds);
    SOL_REQUIRE(bounds.size() == kPartCount);
    SOL_CHECK(!bounds[0].any); // and emphatically NOT charged to part zero
    SOL_CHECK(!bounds[1].any);

    // The same triangle owned by a real part does land, so the guard is
    // rejecting the sentinel rather than rejecting everything.
    faces[0].part = 1;
    SOL_CHECK(forge::forgePartOfFace(faces, 0, kPartCount) == 1);
    forge::forgePartBounds(points, faces, kPartCount, bounds);
    SOL_CHECK(!bounds[0].any);
    SOL_CHECK(bounds[1].any);
    SOL_CHECK(bounds[1].min.x == 0.0 && bounds[1].max.x == 1.0);
    SOL_CHECK(bounds[1].max.y == 1.0);
    SOL_CHECK(bounds[1].min.z == 0.0 && bounds[1].max.z == 0.0); // flat is correct
}

// ⚑ The guard must not reject a LEGITIMATE part, which is the failure a `<=` or
// a `>` in `forgePartOfFace` produces and which the sentinel test above cannot
// see on its own: every real triangle in the repo has to resolve.
SOL_TEST(everyCommittedTriangleResolvesToThePartThatEmittedIt)
{
    for (const char* name : kCommittedParts) {
        assets::ForgeDoc doc;
        std::vector<assets::ForgePoint> points;
        std::vector<assets::ForgeFace> faces;
        SOL_REQUIRE(loadCommittedTopology(name, doc, points, faces));
        SOL_REQUIRE(!faces.empty());
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const std::size_t part = forge::forgePartOfFace(faces, i, doc.parts.size());
            SOL_CHECK(part != forge::kNoPart);
            SOL_CHECK(part == faces[i].part);
        }
    }
}

// Stage O. Two surfaces can point at a part and there is one box.
//
// ⚑ The viewport wins by rule rather than by write order. `PointTool::update`
// clears the viewport hover the moment ImGui takes the mouse, so the two are
// meant to be mutually exclusive - but "they cannot both be set" is an
// invariant held in another file, and stage O exists because stage N left a
// hover uncleared in exactly that way.
SOL_TEST(theViewportHoverOutranksTheRowHoverAndEitherCanDrawTheBox)
{
    constexpr std::size_t kNone = forge::kNoPart;

    // One surface at a time - the ordinary case, once per surface.
    SOL_CHECK(forge::forgeHoverBox(2, kNone, kNone) == 2);
    SOL_CHECK(forge::forgeHoverBox(kNone, 5, kNone) == 5);

    // Both at once: the viewport is the surface the ray was cast for. This is
    // reachable for exactly one frame, when the cursor crosses off the list
    // into the viewport before the panel has run again.
    SOL_CHECK(forge::forgeHoverBox(2, 5, kNone) == 2);

    // Neither, which is every frame the cursor is over furniture.
    SOL_CHECK(forge::forgeHoverBox(kNone, kNone, kNone) == kNone);
    // ⚑ And with a live selection, because a selection is not a hover: the
    // green box is drawn from the editor and must not gain an amber twin just
    // because nothing is being pointed at.
    SOL_CHECK(forge::forgeHoverBox(kNone, kNone, 3) == kNone);
}

// ⚑ The clause that carries the meaning, and it has to hold for BOTH surfaces
// or the list hover arrives without the rule the viewport hover already had.
SOL_TEST(theSelectedPartNeverGetsASecondBox)
{
    constexpr std::size_t kNone = forge::kNoPart;

    SOL_CHECK(forge::forgeHoverBox(3, kNone, 3) == kNone); // stage N's path
    SOL_CHECK(forge::forgeHoverBox(kNone, 3, 3) == kNone); // stage O's path

    // A neighbour of the selection still draws - the suppression is about the
    // one part, not about there being a selection at all.
    SOL_CHECK(forge::forgeHoverBox(kNone, 4, 3) == 4);
    SOL_CHECK(forge::forgeHoverBox(4, kNone, 3) == 4);

    // ⚑ Part zero is an ordinary part and the sentinel is SIZE_MAX, so a rule
    // that discriminates on a magnitude (`hover > 0`, `!hover`) rather than on
    // a sentinel compare has to fail somewhere. Measured rather than assumed:
    // the `!= 0` mutant fails these AND four others, because `kNoPart != 0` is
    // itself true and the mutant therefore swallows every no-ray-hover case -
    // so this pair is a second net over that class, not the only one.
    SOL_CHECK(forge::forgeHoverBox(kNone, 0, 3) == 0);
    SOL_CHECK(forge::forgeHoverBox(0, kNone, kNone) == 0);
}

// Stage O2, and the user found the need for it: "the orange box skips from part
// to part". While the camera orbits, the model turns under a stationary cursor
// and a per-frame hover walks through everything that passes beneath it -
// measured at 100 suppressed changes across 3 parts in one 24-step drag.
SOL_TEST(theHoverHoldsItsAnswerWhileTheCameraIsBeingDragged)
{
    // Held from an earlier frame: a drag. Both buttons that move the camera.
    SOL_CHECK(forge::forgeCameraHoldsMouse(/*leftDown=*/true, /*leftPressed=*/false,
                                           /*middleDown=*/false));
    SOL_CHECK(forge::forgeCameraHoldsMouse(false, false, true));
    SOL_CHECK(forge::forgeCameraHoldsMouse(true, false, true));

    // ⚑ THE CLAUSE THAT KEEPS THE TOOL USABLE: the frame the button goes DOWN is
    // a click, not a drag. Freezing it would mean the pick never runs on the one
    // frame that matters and NO PART COULD EVER BE SELECTED BY CLICKING - a
    // total loss of stage N, from a rule meant only to steady a highlight.
    SOL_CHECK(!forge::forgeCameraHoldsMouse(/*leftDown=*/true, /*leftPressed=*/true,
                                            /*middleDown=*/false));

    // Nothing held: the ordinary case, every frame the cursor is just moving.
    SOL_CHECK(!forge::forgeCameraHoldsMouse(false, false, false));

    // ⚑ A middle drag freezes even on its first frame, and that is correct
    // rather than sloppy: MMB is pan-only, it never selects, so there is no
    // press frame to protect.
    SOL_CHECK(forge::forgeCameraHoldsMouse(false, true, true));
}

// ---------------------------------------------------------------------------
// Stage Q - the edit history.
//
// ⚑ `EditHistory` is deliberately free of ImGui so it can be tested here: the
// ORDERING is the part with all the rules and the drawing is the part with
// none. What CANNOT be reached from a suite is `noteActivation`, which is one
// `ImGui::IsItemActivated()` call - so "one drag is one undo entry", the single
// most valuable behaviour in the stage, has to be proved by driving the tool.
// It is named here so the gap is not mistaken for coverage.

namespace {

using Editor = forge::EditHistory::Editor;

// A stand-in for one editor's snapshot stacks, with the same LIFO discipline
// the three real ones have. It records what it was ASKED to do, so a test can
// assert on the routing - which is the thing the history decides.
struct FakeEditor
{
    int state = 0;
    std::vector<int> undo;
    std::vector<int> redo;
    int undoSteps = 0;
    int redoSteps = 0;

    void edit(forge::EditHistory& history, Editor which, const char* label, int next)
    {
        undo.push_back(state);
        redo.clear();
        history.note(which, label);
        state = next;
    }

    [[nodiscard]] bool undoStep()
    {
        if (undo.empty()) {
            return false;
        }
        ++undoSteps;
        redo.push_back(state);
        state = undo.back();
        undo.pop_back();
        return true;
    }

    [[nodiscard]] bool redoStep()
    {
        if (redo.empty()) {
            return false;
        }
        ++redoSteps;
        undo.push_back(state);
        state = redo.back();
        redo.pop_back();
        return true;
    }
};

} // namespace

// The stage's headline behaviour: two editors, edits interleaved, and undo
// walks back through them in the order they were MADE - not in the order of
// whichever panel happens to be on screen, which is what the tool did before
// and what deleted a part from an invisible document.
SOL_TEST(undoWalksBackThroughBothEditorsInTheOrderTheEditsWereMade)
{
    forge::EditHistory history;
    FakeEditor mesh;
    FakeEditor texture;

    mesh.edit(history, Editor::Mesh, "move part", 1);
    texture.edit(history, Editor::Texture, "recolour", 10);
    mesh.edit(history, Editor::Mesh, "add box", 2);

    const auto apply = [&](Editor which) {
        return which == Editor::Mesh ? mesh.undoStep() : texture.undoStep();
    };

    // Most recent first, and the label names the gesture rather than the panel.
    std::string label;
    SOL_REQUIRE(history.undo(apply, &label));
    SOL_CHECK(label == "add box");
    SOL_CHECK(mesh.state == 1);
    SOL_CHECK(texture.state == 10);

    SOL_REQUIRE(history.undo(apply, &label));
    SOL_CHECK(label == "recolour");
    SOL_CHECK(texture.state == 0);
    SOL_CHECK(mesh.state == 1);

    SOL_REQUIRE(history.undo(apply, &label));
    SOL_CHECK(label == "move part");
    SOL_CHECK(mesh.state == 0);

    SOL_CHECK(!history.canUndo());
    SOL_CHECK(!history.undo(apply, &label));
    // Three undos, and each editor was asked exactly as often as it had edits.
    SOL_CHECK(mesh.undoSteps == 2);
    SOL_CHECK(texture.undoSteps == 1);
}

SOL_TEST(redoReturnsEveryDocumentToWhereItWas)
{
    forge::EditHistory history;
    FakeEditor mesh;
    FakeEditor texture;

    mesh.edit(history, Editor::Mesh, "move part", 1);
    texture.edit(history, Editor::Texture, "recolour", 10);
    mesh.edit(history, Editor::Mesh, "add box", 2);

    const auto undoApply = [&](Editor which) {
        return which == Editor::Mesh ? mesh.undoStep() : texture.undoStep();
    };
    const auto redoApply = [&](Editor which) {
        return which == Editor::Mesh ? mesh.redoStep() : texture.redoStep();
    };

    while (history.canUndo()) {
        SOL_REQUIRE(history.undo(undoApply));
    }
    SOL_CHECK(mesh.state == 0);
    SOL_CHECK(texture.state == 0);

    // Forward again, in the order they were made.
    std::string label;
    SOL_REQUIRE(history.redo(redoApply, &label));
    SOL_CHECK(label == "move part");
    SOL_CHECK(mesh.state == 1);
    SOL_REQUIRE(history.redo(redoApply, &label));
    SOL_CHECK(label == "recolour");
    SOL_CHECK(texture.state == 10);
    SOL_REQUIRE(history.redo(redoApply, &label));
    SOL_CHECK(label == "add box");
    SOL_CHECK(mesh.state == 2);

    SOL_CHECK(!history.canRedo());
    SOL_CHECK(!history.redo(redoApply));
}

// M1. A new edit ends the forward branch - the standard rule, and without it a
// redo would step into a future the author has already replaced.
SOL_TEST(aNewEditDiscardsTheForwardBranch)
{
    forge::EditHistory history;
    FakeEditor mesh;
    FakeEditor texture;

    mesh.edit(history, Editor::Mesh, "move part", 1);
    const auto undoApply = [&](Editor) { return mesh.undoStep(); };
    SOL_REQUIRE(history.undo(undoApply));
    SOL_CHECK(history.canRedo());

    // The new edit is in a DIFFERENT editor, which is the case the sweep exists
    // for: the mesh editor cannot know its own forward snapshot has just been
    // orphaned, because nothing asked it anything.
    texture.edit(history, Editor::Texture, "recolour", 10);
    SOL_CHECK(!history.canRedo());
    // ⚑ And the flag that makes the caller drop the orphan. Consume-once: a
    // second read in the same frame must not sweep twice.
    SOL_CHECK(history.takeRedoDiscarded());
    SOL_CHECK(!history.takeRedoDiscarded());
}

// M3, and it is the subtle one: BOTH CAPS ARE 64 AND THEY DO NOT EVICT
// TOGETHER. The history counts edits from all three editors while each editor
// counts only its own, so a mesh editor fills its stack long before the history
// fills its own if a texture is being edited alongside. Without `evicted`, the
// history names a snapshot that has already gone - and because a failed `apply`
// deliberately leaves the entry in place rather than compounding the error,
// UNDO IS THEN WEDGED PERMANENTLY: every later press finds the same dead entry
// on top and does nothing, with real history sitting underneath it.
SOL_TEST(evictingASnapshotAlsoRetiresTheEntryThatNamedIt)
{
    // The editors' cap, which is the only one in the design. ⚑ This test's
    // first version gave the history a cap of its own as well, and that is how
    // it found the defect it was written for from the other end: two bounds on
    // one quantity trimmed each eviction TWICE, dropping an undo step whose
    // snapshot the editor was still holding. See `edit_history.hpp`.
    constexpr std::size_t kEditorDepth = 64;

    forge::EditHistory history;
    FakeEditor mesh;
    FakeEditor texture;

    // Fill the mesh editor exactly to its cap, with a texture edit interleaved
    // so the two stacks are demonstrably different lengths - which is the
    // reason the caps cannot be assumed to evict together.
    texture.edit(history, Editor::Texture, "recolour", 10);
    for (std::size_t i = 0; i < kEditorDepth + 1; ++i) {
        mesh.undo.push_back(mesh.state);
        history.note(Editor::Mesh, "move part");
        // The editor's own cap, and the report that goes with it.
        if (mesh.undo.size() > kEditorDepth) {
            mesh.undo.erase(mesh.undo.begin());
            history.evicted(Editor::Mesh);
        }
        mesh.state = static_cast<int>(i) + 1;
    }

    SOL_REQUIRE(mesh.undo.size() == kEditorDepth);
    // ⚑ The history holds the UNION: the mesh's 64 plus the texture's one.
    SOL_CHECK(history.undoDepth() == kEditorDepth + 1);

    // Every entry the history offers can actually be taken - which is the whole
    // claim, and what a missing `evicted` breaks on the very first press,
    // wedging undo for the rest of the session.
    const auto apply = [&](Editor which) {
        return which == Editor::Mesh ? mesh.undoStep() : texture.undoStep();
    };
    std::size_t taken = 0;
    while (history.canUndo()) {
        SOL_REQUIRE(history.undo(apply));
        ++taken;
    }
    SOL_CHECK(taken == kEditorDepth + 1);
    SOL_CHECK(mesh.undoSteps == static_cast<int>(kEditorDepth));
    SOL_CHECK(texture.undoSteps == 1);
}

// M4, and the only one whose consequence is destructive: opening a second
// document throws the first's snapshots away, and an order left standing would
// let one press overwrite the newly opened file with the previous asset's
// contents.
SOL_TEST(openingAnotherDocumentLeavesNothingToUndoIntoTheOldOne)
{
    forge::EditHistory history;
    FakeEditor mesh;
    FakeEditor texture;

    mesh.edit(history, Editor::Mesh, "move part", 1);
    texture.edit(history, Editor::Texture, "recolour", 10);
    mesh.edit(history, Editor::Mesh, "add box", 2);
    SOL_CHECK(history.undoDepth() == 3);

    // The mesh editor opens a different file: its snapshots go, and so must
    // every entry naming it, in both directions.
    mesh.undo.clear();
    mesh.redo.clear();
    history.forget(Editor::Mesh);

    // What is left is the texture's edit, and only that.
    SOL_REQUIRE(history.undoDepth() == 1);
    SOL_CHECK(history.undoLabel() == "recolour");
    const auto apply = [&](Editor which) {
        return which == Editor::Mesh ? mesh.undoStep() : texture.undoStep();
    };
    SOL_REQUIRE(history.undo(apply));
    SOL_CHECK(texture.state == 0);
    // ⚑ THE ASSERTION THAT MATTERS: the mesh editor was never asked to step, so
    // the freshly opened document was not touched.
    SOL_CHECK(mesh.undoSteps == 0);
    SOL_CHECK(!history.canUndo());
}

// The safety net under M3 and M4. If the order and the snapshots ever do come
// apart, the history must not compound it by moving an entry whose step did not
// happen - a redo that then "returns" to a state nothing ever produced is worse
// than a press that does nothing.
SOL_TEST(anApplyThatRefusesLeavesTheHistoryExactlyAsItWas)
{
    forge::EditHistory history;
    history.note(Editor::Mesh, "move part");

    const auto refuse = [](Editor) { return false; };
    SOL_CHECK(!history.undo(refuse));
    SOL_CHECK(history.undoDepth() == 1);
    SOL_CHECK(history.redoDepth() == 0);
    SOL_CHECK(history.undoLabel() == "move part");
}

// M8. The routing IS the stage: an entry has to name the editor that made it or
// undo acts on the wrong document, which is the defect this was built to fix.
// Asserted by which editor gets ASKED rather than by comparing states, because
// two editors can hold equal values by coincidence.
SOL_TEST(eachStepIsRoutedToTheEditorThatMadeThatEdit)
{
    forge::EditHistory history;
    FakeEditor mesh;
    FakeEditor texture;
    FakeEditor def;

    mesh.edit(history, Editor::Mesh, "move part", 1);
    def.edit(history, Editor::Def, "set radius", 7);
    texture.edit(history, Editor::Texture, "recolour", 10);

    std::vector<Editor> asked;
    const auto apply = [&](Editor which) {
        asked.push_back(which);
        switch (which) {
        case Editor::Mesh:
            return mesh.undoStep();
        case Editor::Texture:
            return texture.undoStep();
        case Editor::Def:
            return def.undoStep();
        }
        return false;
    };
    while (history.canUndo()) {
        SOL_REQUIRE(history.undo(apply));
    }

    SOL_REQUIRE(asked.size() == 3);
    SOL_CHECK(asked[0] == Editor::Texture);
    SOL_CHECK(asked[1] == Editor::Def);
    SOL_CHECK(asked[2] == Editor::Mesh);
    SOL_CHECK(mesh.undoSteps == 1);
    SOL_CHECK(texture.undoSteps == 1);
    SOL_CHECK(def.undoSteps == 1);
}

// The buttons in all three panels mean what Ctrl+Z means, and the read that
// services them is consume-once - a press held across two frames must step
// once, exactly as the keyboard chord does.
SOL_TEST(aPanelButtonRequestIsDeliveredOnceAndOnlyOnce)
{
    forge::EditHistory history;
    SOL_CHECK(!history.takeUndoRequest());
    SOL_CHECK(!history.takeRedoRequest());

    history.requestUndo();
    SOL_CHECK(history.takeUndoRequest());
    SOL_CHECK(!history.takeUndoRequest());

    history.requestRedo();
    SOL_CHECK(history.takeRedoRequest());
    SOL_CHECK(!history.takeRedoRequest());
}

// ---------------------------------------------------------------------------
// Stage Q4: the status line as an event rather than a state.
//
// ⚑ These are here for the same reason the stage Q history tests are: the
// defect is a UI one, but the LOGIC under it is arithmetic over a string and a
// float, and that half does not need a window. What still cannot be tested
// here is whether 1.2 s reads as a flash or as a flicker - that is a loudness
// judgement and it wants a person, which is exactly why the numbers are named
// constants in the header instead of literals at the draw site.
// ---------------------------------------------------------------------------

SOL_TEST(theSameMessageTwiceIsCountedRatherThanDrawnIdentically)
{
    forge::StatusLine status;
    status = std::string("undo: cell");
    SOL_CHECK(status.repeat() == 1);
    // No suffix on the first arrival: the count is an admission that something
    // repeated, and "(x1)" would be noise on every ordinary message.
    SOL_CHECK(status.display() == "undo: cell");

    status = std::string("undo: cell");
    SOL_CHECK(status.repeat() == 2);
    SOL_CHECK(status.display() == "undo: cell  (x2)");
    SOL_CHECK(status.text() == "undo: cell");

    status = std::string("undo: cell");
    SOL_CHECK(status.repeat() == 3);
    SOL_CHECK(status.display() == "undo: cell  (x3)");
}

// ⚑⚑ THE LOAD-BEARING ONE. The `(xN)` suffix is the half of the repeat fix you
// can read after the fact; the serial is the half you SEE, because the draw
// site restamps its clock and re-fires the flash whenever this moves. A serial
// that ticked only on a CHANGE of text would leave the second identical undo
// looking exactly as ignored as it did before the stage - the whole defect,
// intact, behind a counter that happened to be right.
SOL_TEST(theSerialMovesOnEveryWriteIncludingAnIdenticalOne)
{
    forge::StatusLine status;
    const unsigned long long start = status.serial();

    status = std::string("undo: cell");
    const unsigned long long first = status.serial();
    SOL_CHECK(first != start);

    status = std::string("undo: cell");
    SOL_CHECK(status.serial() != first);

    const unsigned long long second = status.serial();
    status = std::string("undo: move part");
    SOL_CHECK(status.serial() != second);
}

SOL_TEST(aDifferentMessageStartsTheCountAgain)
{
    forge::StatusLine status;
    status = std::string("undo: cell");
    status = std::string("undo: cell");
    SOL_CHECK(status.repeat() == 2);

    status = std::string("undo: move part");
    SOL_CHECK(status.repeat() == 1);
    SOL_CHECK(status.display() == "undo: move part");

    // ...and going back to the first message does not resume its old count.
    status = std::string("undo: cell");
    SOL_CHECK(status.repeat() == 1);
    SOL_CHECK(status.display() == "undo: cell");
}

// An empty write is how a caller says "nothing to report", and it must not be
// counted as a repeat of itself - otherwise a path that clears the line twice
// would leave `(x2)` attached to a message with no text.
SOL_TEST(clearingTheLineIsNotARepeatOfNothing)
{
    forge::StatusLine status;
    SOL_CHECK(status.empty());
    SOL_CHECK(status.repeat() == 0);

    status = std::string();
    status = std::string();
    SOL_CHECK(status.empty());
    SOL_CHECK(status.repeat() == 0);
    SOL_CHECK(status.display().empty());

    status = std::string("3 drop(s), none changed");
    SOL_CHECK(status.repeat() == 1);
    status = std::string();
    SOL_CHECK(status.empty());
    SOL_CHECK(status.repeat() == 0);
}

// ⚑ Defect 1: a message that never expires reads as freshly at four minutes as
// at one second. The bar is cleaner empty than lying.
SOL_TEST(aMessageStopsBeingDrawnOnceItIsPastItsLifetime)
{
    SOL_CHECK(forge::statusAppearance(0.0f).visible);
    SOL_CHECK(forge::statusAppearance(forge::kStatusLifetimeSeconds - 0.1f).visible);
    // The boundary itself is out, not in: `>=` rather than `>`.
    SOL_CHECK(!forge::statusAppearance(forge::kStatusLifetimeSeconds).visible);
    SOL_CHECK(!forge::statusAppearance(forge::kStatusLifetimeSeconds + 60.0f).visible);
    SOL_CHECK(!forge::statusAppearance(4.0f * 60.0f).visible);

    // An expired message is never also lit, which would be the worst of both.
    SOL_CHECK(forge::statusAppearance(forge::kStatusLifetimeSeconds).highlight == 0.0f);
}

// ⚑ Defect 2's visible half: the message arrives lit and settles, so "it
// happened again" is legible without reading the text at all.
SOL_TEST(aFreshMessageArrivesLitAndSettlesIntoChrome)
{
    const forge::StatusAppearance atArrival = forge::statusAppearance(0.0f);
    SOL_CHECK(atArrival.visible);
    SOL_CHECK(atArrival.highlight == 1.0f);

    // Halfway through the flash it is halfway back to chrome.
    const forge::StatusAppearance midway =
        forge::statusAppearance(forge::kStatusFlashSeconds * 0.5f);
    SOL_CHECK(midway.visible);
    SOL_CHECK(std::fabs(midway.highlight - 0.5f) < 1e-5f);

    // Settled, but still worth reading - the two durations are separate
    // numbers because they answer separate questions.
    const forge::StatusAppearance settled =
        forge::statusAppearance(forge::kStatusFlashSeconds);
    SOL_CHECK(settled.visible);
    SOL_CHECK(settled.highlight == 0.0f);
    SOL_CHECK(forge::statusAppearance(forge::kStatusFlashSeconds + 5.0f).visible);
    SOL_CHECK(forge::statusAppearance(forge::kStatusFlashSeconds + 5.0f).highlight == 0.0f);

    // Never brightens as it ages, at any sample.
    float previous = 2.0f;
    for (int i = 0; i <= 24; ++i) {
        const float age = static_cast<float>(i) * (forge::kStatusFlashSeconds / 24.0f);
        const float highlight = forge::statusAppearance(age).highlight;
        SOL_CHECK(highlight <= previous + 1e-6f);
        SOL_CHECK(highlight >= 0.0f && highlight <= 1.0f);
        previous = highlight;
    }

    // A clock that ran backwards must not produce a highlight above full.
    SOL_CHECK(forge::statusAppearance(-1.0f).highlight == 1.0f);
    SOL_CHECK(forge::statusAppearance(-1.0f).visible);
}
