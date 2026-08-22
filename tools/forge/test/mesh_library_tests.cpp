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

#include "mesh_library.hpp"

#include "sol/assets/forge_doc.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/test/test.hpp"

#include <cmath>
#include <cstdint>
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
