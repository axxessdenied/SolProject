#include <cmath>
#include <cstdint>
#include <vector>

#include <sol/test/synthetic_cooked_font.hpp>
#include <sol/test/test.hpp>
#include <sol/ui/draw_list.hpp>

using sol::assets::Font;
using sol::assets::FontStyleRecord;
using sol::ui::Color;
using sol::ui::DrawList;
using sol::ui::Rect;
using sol::ui::TextAlign;

namespace {

constexpr Color kWhite = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kRed = {1.0f, 0.0f, 0.0f, 1.0f};

bool nearlyEqual(float a, float b)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < 1.0e-3f;
}

// Total indices across every batch, which must always equal the index buffer.
std::uint32_t batchedIndexCount(const DrawList& list)
{
    std::uint32_t total = 0;
    for (const DrawList::Batch& batch : list.batches()) {
        total += batch.indexCount;
    }
    return total;
}

bool loadTestFont(Font& font)
{
    return font.loadFromMemory(sol::test::buildSyntheticCookedFont());
}

} // namespace

SOL_TEST(draw_list_emits_a_quad_per_rect)
{
    DrawList list;
    list.addRect({{10.0f, 20.0f}, {40.0f, 50.0f}}, kWhite);

    SOL_REQUIRE(list.vertices().size() == 4);
    SOL_REQUIRE(list.indices().size() == 6);
    SOL_REQUIRE(list.batches().size() == 1);
    SOL_CHECK(list.batches()[0].indexCount == 6);
    SOL_CHECK(list.batches()[0].firstIndex == 0);
    SOL_CHECK(list.batches()[0].texture == 0); // solid fills use the white pixel
    SOL_CHECK(!list.overflowed());

    // Corners in order: top-left, top-right, bottom-right, bottom-left.
    SOL_CHECK(nearlyEqual(list.vertices()[0].position.x, 10.0f));
    SOL_CHECK(nearlyEqual(list.vertices()[0].position.y, 20.0f));
    SOL_CHECK(nearlyEqual(list.vertices()[2].position.x, 40.0f));
    SOL_CHECK(nearlyEqual(list.vertices()[2].position.y, 50.0f));

    // Every index must address a vertex that exists.
    for (const std::uint16_t index : list.indices()) {
        SOL_REQUIRE(index < list.vertices().size());
    }
}

SOL_TEST(draw_list_reset_clears_everything)
{
    DrawList list;
    list.pushClip({{0.0f, 0.0f}, {10.0f, 10.0f}});
    list.addRect({{0.0f, 0.0f}, {5.0f, 5.0f}}, kWhite);
    SOL_REQUIRE(!list.vertices().empty());

    list.reset();
    SOL_CHECK(list.vertices().empty());
    SOL_CHECK(list.indices().empty());
    SOL_CHECK(list.batches().empty());
    SOL_CHECK(!list.overflowed());
    // The clip stack resets too, or the next frame inherits a stale scissor.
    SOL_CHECK(list.currentClip().empty());
}

SOL_TEST(draw_list_skips_degenerate_geometry)
{
    DrawList list;
    list.addRect({{10.0f, 10.0f}, {10.0f, 50.0f}}, kWhite);            // zero width
    list.addRect({{10.0f, 10.0f}, {50.0f, 10.0f}}, kWhite);            // zero height
    list.addRect({{50.0f, 50.0f}, {10.0f, 10.0f}}, kWhite);            // inverted
    list.addRect({{0.0f, 0.0f}, {10.0f, 10.0f}}, kWhite.withAlpha(0)); // invisible
    list.addLine({5.0f, 5.0f}, {5.0f, 5.0f}, kWhite);                  // zero length

    SOL_CHECK(list.vertices().empty());
    SOL_CHECK(list.indices().empty());
    SOL_CHECK(list.batches().empty());
}

SOL_TEST(draw_list_merges_compatible_draws_into_one_batch)
{
    DrawList list;
    list.addRect({{0.0f, 0.0f}, {10.0f, 10.0f}}, kWhite);
    list.addRect({{20.0f, 0.0f}, {30.0f, 10.0f}}, kRed);
    list.addLine({0.0f, 0.0f}, {10.0f, 10.0f}, kWhite, 2.0f);

    // Same texture, same clip: one draw call, not three.
    SOL_REQUIRE(list.batches().size() == 1);
    SOL_CHECK(list.batches()[0].indexCount == 18);
    SOL_CHECK(batchedIndexCount(list) == list.indices().size());
}

SOL_TEST(draw_list_breaks_batches_on_clip_change)
{
    DrawList list;
    list.addRect({{0.0f, 0.0f}, {10.0f, 10.0f}}, kWhite);

    list.pushClip({{0.0f, 0.0f}, {50.0f, 50.0f}});
    list.addRect({{0.0f, 0.0f}, {10.0f, 10.0f}}, kWhite);
    list.popClip();

    list.addRect({{0.0f, 0.0f}, {10.0f, 10.0f}}, kWhite);

    // A scissor change cannot be folded into an existing draw.
    SOL_REQUIRE(list.batches().size() == 3);
    SOL_CHECK(list.batches()[1].clipMax.x == 50.0f);
    SOL_CHECK(list.batches()[2].clipMax.x == 0.0f); // back to unclipped
    SOL_CHECK(batchedIndexCount(list) == list.indices().size());

    // Batches must tile the index buffer in order, with no gaps.
    std::uint32_t expected = 0;
    for (const DrawList::Batch& batch : list.batches()) {
        SOL_REQUIRE(batch.firstIndex == expected);
        expected += batch.indexCount;
    }
}

SOL_TEST(draw_list_nested_clips_intersect)
{
    DrawList list;
    list.pushClip({{10.0f, 10.0f}, {100.0f, 100.0f}});
    list.pushClip({{50.0f, 0.0f}, {200.0f, 60.0f}});

    // A child clip can only ever shrink its parent.
    const Rect clip = list.currentClip();
    SOL_CHECK(nearlyEqual(clip.min.x, 50.0f));
    SOL_CHECK(nearlyEqual(clip.min.y, 10.0f));
    SOL_CHECK(nearlyEqual(clip.max.x, 100.0f));
    SOL_CHECK(nearlyEqual(clip.max.y, 60.0f));

    list.popClip();
    SOL_CHECK(nearlyEqual(list.currentClip().max.x, 100.0f));
    list.popClip();
    SOL_CHECK(list.currentClip().empty());

    // Popping an empty stack must not underflow.
    list.popClip();
    SOL_CHECK(list.currentClip().empty());
}

SOL_TEST(draw_list_disjoint_clips_collapse_to_empty)
{
    DrawList list;
    list.pushClip({{0.0f, 0.0f}, {10.0f, 10.0f}});
    list.pushClip({{100.0f, 100.0f}, {200.0f, 200.0f}});

    // Non-overlapping clips must not produce an inverted rect, which would
    // become a garbage scissor.
    const Rect clip = list.currentClip();
    SOL_CHECK(clip.max.x >= clip.min.x);
    SOL_CHECK(clip.max.y >= clip.min.y);
    SOL_CHECK(clip.empty());
}

SOL_TEST(draw_list_text_needs_a_font)
{
    DrawList list;
    Font font;
    SOL_REQUIRE(loadTestFont(font));
    const FontStyleRecord* hud = font.style("hud");
    SOL_REQUIRE(hud != nullptr);

    // Without a font set, text is dropped rather than crashing.
    SOL_CHECK(nearlyEqual(list.addText(*hud, {0.0f, 0.0f}, "AB", kWhite), 0.0f));
    SOL_CHECK(list.vertices().empty());
}

SOL_TEST(draw_list_text_emits_quads_and_advances)
{
    Font font;
    SOL_REQUIRE(loadTestFont(font));
    const FontStyleRecord* hud = font.style("hud");
    SOL_REQUIRE(hud != nullptr);

    DrawList list;
    list.setFont(&font, 1);

    const float width = list.addText(*hud, {100.0f, 50.0f}, "AB", kWhite);
    SOL_CHECK(nearlyEqual(width, 2.0f * sol::test::kSyntheticAdvance));
    SOL_CHECK(nearlyEqual(width, font.measureWidth(*hud, "AB")));

    SOL_REQUIRE(list.vertices().size() == 8); // one quad per inked glyph
    SOL_REQUIRE(list.batches().size() == 1);
    SOL_CHECK(list.batches()[0].texture == 1); // the font atlas, not white

    // First glyph sits at the pen, offset by its bearings.
    SOL_CHECK(nearlyEqual(list.vertices()[0].position.x, 100.0f));
    SOL_CHECK(nearlyEqual(list.vertices()[0].position.y, 48.0f)); // baseline + bearingY
    // Second glyph starts one advance along.
    SOL_CHECK(nearlyEqual(list.vertices()[4].position.x, 105.0f));

    // UVs must land inside the atlas.
    for (const DrawList::Vertex& vertex : list.vertices()) {
        SOL_REQUIRE(vertex.uv.x >= 0.0f && vertex.uv.x <= 1.0f);
        SOL_REQUIRE(vertex.uv.y >= 0.0f && vertex.uv.y <= 1.0f);
    }
}

SOL_TEST(draw_list_text_skips_blank_glyphs_but_keeps_advance)
{
    Font font;
    SOL_REQUIRE(loadTestFont(font));
    const FontStyleRecord* hud = font.style("hud");
    SOL_REQUIRE(hud != nullptr);

    DrawList list;
    list.setFont(&font, 1);
    const float width = list.addText(*hud, {0.0f, 0.0f}, "A B", kWhite);

    // Space contributes an advance and no geometry.
    SOL_CHECK(list.vertices().size() == 8);
    SOL_CHECK(nearlyEqual(width, 2.0f * sol::test::kSyntheticAdvance + sol::test::kSyntheticSpaceAdvance));
}

SOL_TEST(draw_list_text_alignment_in_a_box)
{
    Font font;
    SOL_REQUIRE(loadTestFont(font));
    const FontStyleRecord* hud = font.style("hud");
    SOL_REQUIRE(hud != nullptr);

    const Rect box = {{0.0f, 0.0f}, {100.0f, 40.0f}};
    const float textWidth = font.measureWidth(*hud, "AB");

    const auto firstX = [&](TextAlign align) {
        DrawList list;
        list.setFont(&font, 1);
        list.addTextInBox(*hud, box, "AB", kWhite, align);
        return list.vertices().empty() ? -1.0f : list.vertices()[0].position.x;
    };

    SOL_CHECK(nearlyEqual(firstX(TextAlign::Left), 0.0f));
    SOL_CHECK(nearlyEqual(firstX(TextAlign::Center), std::round((100.0f - textWidth) * 0.5f)));
    SOL_CHECK(nearlyEqual(firstX(TextAlign::Right), std::round(100.0f - textWidth)));
}

SOL_TEST(draw_list_reports_overflow_instead_of_wrapping_indices)
{
    DrawList list;
    // Vertex indices are 16-bit; past the cap the list must refuse to emit
    // rather than wrap around and draw nonsense.
    for (int i = 0; i < 20000; ++i) {
        list.addRect({{0.0f, 0.0f}, {1.0f, 1.0f}}, kWhite);
    }

    SOL_CHECK(list.overflowed());
    SOL_REQUIRE(list.vertices().size() <= DrawList::kMaxVertices);
    for (const std::uint16_t index : list.indices()) {
        SOL_REQUIRE(index < list.vertices().size());
    }
    SOL_CHECK(batchedIndexCount(list) == list.indices().size());
}

SOL_TEST(draw_list_rounded_rect_stays_within_bounds)
{
    DrawList list;
    const Rect box = {{10.0f, 10.0f}, {60.0f, 40.0f}};
    list.addRoundedRect(box, 8.0f, kWhite);

    SOL_REQUIRE(!list.vertices().empty());
    for (const DrawList::Vertex& vertex : list.vertices()) {
        SOL_REQUIRE(vertex.position.x >= box.min.x - 0.01f);
        SOL_REQUIRE(vertex.position.x <= box.max.x + 0.01f);
        SOL_REQUIRE(vertex.position.y >= box.min.y - 0.01f);
        SOL_REQUIRE(vertex.position.y <= box.max.y + 0.01f);
    }
    for (const std::uint16_t index : list.indices()) {
        SOL_REQUIRE(index < list.vertices().size());
    }

    // An over-large radius clamps to a capsule instead of inverting.
    DrawList clamped;
    clamped.addRoundedRect(box, 1000.0f, kWhite);
    for (const DrawList::Vertex& vertex : clamped.vertices()) {
        SOL_REQUIRE(vertex.position.x >= box.min.x - 0.01f);
        SOL_REQUIRE(vertex.position.y >= box.min.y - 0.01f);
    }
}

SOL_TEST(draw_list_outline_draws_four_sides_inside_the_rect)
{
    DrawList list;
    const Rect box = {{0.0f, 0.0f}, {100.0f, 50.0f}};
    list.addRectOutline(box, kWhite, 2.0f);

    SOL_CHECK(list.vertices().size() == 16); // four bars
    for (const DrawList::Vertex& vertex : list.vertices()) {
        SOL_REQUIRE(vertex.position.x >= box.min.x);
        SOL_REQUIRE(vertex.position.x <= box.max.x);
        SOL_REQUIRE(vertex.position.y >= box.min.y);
        SOL_REQUIRE(vertex.position.y <= box.max.y);
    }
}

SOL_TEST(draw_list_circle_strokes_a_ring_of_the_asked_width)
{
    DrawList list;
    const sol::core::Vec2 center = {100.0f, 100.0f};
    list.addCircle(center, 20.0f, kWhite, 4.0f, 8);

    // One vertex pair per step plus the closing pair, two triangles a step.
    SOL_REQUIRE(list.vertices().size() == 18);
    SOL_REQUIRE(list.indices().size() == 48);
    SOL_CHECK(batchedIndexCount(list) == list.indices().size());
    SOL_CHECK(list.batches()[0].texture == 0); // solid, like every other fill

    bool sawInner = false;
    bool sawOuter = false;
    for (const DrawList::Vertex& vertex : list.vertices()) {
        const float dx = vertex.position.x - center.x;
        const float dy = vertex.position.y - center.y;
        const float radius = std::sqrt(dx * dx + dy * dy);
        SOL_REQUIRE(radius > 17.99f && radius < 22.01f);
        sawInner = sawInner || nearlyEqual(radius, 18.0f);
        sawOuter = sawOuter || nearlyEqual(radius, 22.0f);
    }
    SOL_CHECK(sawInner);
    SOL_CHECK(sawOuter);
}

SOL_TEST(draw_list_arc_stays_inside_its_own_sweep)
{
    DrawList list;
    const sol::core::Vec2 center = {200.0f, 200.0f};
    // A short arc centered straight up: -pi/2 is up, since screen y grows down.
    constexpr float kUp = -1.57079633f;
    list.addArc(center, 30.0f, kUp - 0.3f, kUp + 0.3f, kWhite, 2.0f, 6);

    SOL_REQUIRE(list.vertices().size() == 14);
    for (const DrawList::Vertex& vertex : list.vertices()) {
        SOL_REQUIRE(vertex.position.y < center.y);                   // above the center
        SOL_REQUIRE(std::abs(vertex.position.x - center.x) < 10.0f); // narrow sweep
    }
}

SOL_TEST(draw_list_arc_ignores_degenerate_input)
{
    DrawList list;
    list.addArc({50.0f, 50.0f}, 0.0f, 0.0f, 1.0f, kWhite, 2.0f, 8);  // no radius
    list.addArc({50.0f, 50.0f}, 10.0f, 1.0f, 1.0f, kWhite, 2.0f, 8); // no sweep
    list.addArc({50.0f, 50.0f}, 10.0f, 0.0f, 1.0f, kWhite, 0.0f, 8); // no width
    SOL_CHECK(list.vertices().empty());
    SOL_CHECK(list.indices().empty());

    // A stroke wider than the radius fills to the center instead of winding
    // the inner edge back through it.
    list.addCircle({50.0f, 50.0f}, 4.0f, kWhite, 20.0f, 4);
    SOL_REQUIRE(!list.vertices().empty());
    for (const DrawList::Vertex& vertex : list.vertices()) {
        const float dx = vertex.position.x - 50.0f;
        const float dy = vertex.position.y - 50.0f;
        SOL_REQUIRE(std::sqrt(dx * dx + dy * dy) < 14.01f);
    }
}

SOL_TEST(draw_list_drops_geometry_under_a_collapsed_clip)
{
    DrawList list;

    // A clip that misses its parent entirely collapses to zero area. Nothing
    // may be emitted under it: the renderer reads an empty rect as "no clip"
    // and would draw the geometry across the whole screen.
    list.pushClip({{100.0f, 100.0f}, {200.0f, 200.0f}});
    list.pushClip({{600.0f, 600.0f}, {700.0f, 700.0f}});
    list.addRect({{610.0f, 610.0f}, {650.0f, 650.0f}}, kWhite);
    list.addRoundedRect({{610.0f, 610.0f}, {650.0f, 650.0f}}, 4.0f, kWhite);
    list.addLine({610.0f, 610.0f}, {650.0f, 650.0f}, kWhite, 2.0f);
    list.addTriangle({610.0f, 610.0f}, {650.0f, 610.0f}, {630.0f, 650.0f}, kWhite);
    list.addCircle({630.0f, 630.0f}, 10.0f, kWhite, 2.0f, 8);
    SOL_CHECK(list.vertices().empty());
    SOL_CHECK(list.indices().empty());
    list.popClip();

    // Back inside the parent, drawing resumes.
    list.addRect({{110.0f, 110.0f}, {150.0f, 150.0f}}, kWhite);
    SOL_CHECK(!list.vertices().empty());
    list.popClip();
}
