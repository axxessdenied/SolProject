#include <sol/test/test.hpp>
#include <sol/ui/layout.hpp>

using sol::ui::Column;
using sol::ui::inset;
using sol::ui::Rect;
using sol::ui::Row;

namespace {

constexpr Rect kBox = {{100.0f, 200.0f}, {500.0f, 600.0f}};

bool nearlyEqual(float a, float b)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < 1.0e-3f;
}

} // namespace

SOL_TEST(layout_column_stacks_rows_with_padding_and_spacing)
{
    Column column(kBox, 10.0f, 4.0f);

    const Rect first = column.row(30.0f);
    SOL_CHECK(nearlyEqual(first.min.x, 110.0f)); // inset by padding
    SOL_CHECK(nearlyEqual(first.max.x, 490.0f));
    SOL_CHECK(nearlyEqual(first.min.y, 210.0f));
    SOL_CHECK(nearlyEqual(first.max.y, 240.0f));

    const Rect second = column.row(20.0f);
    SOL_CHECK(nearlyEqual(second.min.y, 244.0f)); // previous bottom + spacing
    SOL_CHECK(nearlyEqual(second.max.y, 264.0f));

    // Consumed height excludes the trailing gap, so a panel can size to its
    // contents without a stray margin.
    SOL_CHECK(nearlyEqual(column.consumed(), 54.0f));
    SOL_CHECK(nearlyEqual(column.width(), 380.0f));
}

SOL_TEST(layout_column_skip_and_remaining)
{
    Column column(kBox, 0.0f, 0.0f);
    (void)column.row(100.0f);
    column.skip(50.0f);

    const Rect rest = column.remaining();
    SOL_CHECK(nearlyEqual(rest.min.y, 350.0f));
    SOL_CHECK(nearlyEqual(rest.max.y, 600.0f));
    SOL_CHECK(nearlyEqual(rest.min.x, 100.0f));

    // Overflowing rows still return a usable rect; clipping is the caller's
    // job, so list code never has to branch at the bottom edge.
    Column tight({{0.0f, 0.0f}, {100.0f, 20.0f}});
    const Rect overflow = tight.row(40.0f);
    SOL_CHECK(nearlyEqual(overflow.min.y, 0.0f));
    SOL_CHECK(nearlyEqual(overflow.max.y, 40.0f));
}

SOL_TEST(layout_row_hands_out_cells_from_both_ends)
{
    Row row({{0.0f, 0.0f}, {300.0f, 20.0f}}, 10.0f);

    const Rect left = row.cell(50.0f);
    SOL_CHECK(nearlyEqual(left.min.x, 0.0f));
    SOL_CHECK(nearlyEqual(left.max.x, 50.0f));

    const Rect right = row.cellFromRight(40.0f);
    SOL_CHECK(nearlyEqual(right.min.x, 260.0f));
    SOL_CHECK(nearlyEqual(right.max.x, 300.0f));

    // What is left is the gap between the two cursors, spacing removed.
    const Rect middle = row.remaining();
    SOL_CHECK(nearlyEqual(middle.min.x, 60.0f));
    SOL_CHECK(nearlyEqual(middle.max.x, 250.0f));
}

SOL_TEST(layout_row_never_returns_an_inverted_remainder)
{
    // Over-allocating must collapse the remainder to empty rather than
    // producing a backwards rect that would become a garbage scissor.
    Row row({{0.0f, 0.0f}, {100.0f, 20.0f}}, 0.0f);
    (void)row.cell(80.0f);
    (void)row.cellFromRight(80.0f);

    const Rect rest = row.remaining();
    SOL_CHECK(rest.max.x >= rest.min.x);
    SOL_CHECK(rest.empty());
}

SOL_TEST(layout_row_fractions_tile_without_gaps_or_overlap)
{
    Row row({{0.0f, 0.0f}, {300.0f, 20.0f}}, 10.0f);

    const Rect a = row.fraction(0, 3);
    const Rect b = row.fraction(1, 3);
    const Rect c = row.fraction(2, 3);

    // Three equal cells with two gaps: (300 - 20) / 3.
    SOL_CHECK(nearlyEqual(a.width(), 280.0f / 3.0f));
    SOL_CHECK(nearlyEqual(b.width(), a.width()));
    SOL_CHECK(nearlyEqual(c.width(), a.width()));

    SOL_CHECK(nearlyEqual(a.min.x, 0.0f));
    SOL_CHECK(nearlyEqual(b.min.x - a.max.x, 10.0f)); // exactly one gap apart
    SOL_CHECK(nearlyEqual(c.min.x - b.max.x, 10.0f));
    SOL_CHECK(nearlyEqual(c.max.x, 300.0f)); // and the last one reaches the edge

    // A single cell fills the row; a zero count degrades to the whole row.
    SOL_CHECK(nearlyEqual(row.fraction(0, 1).width(), 300.0f));
    SOL_CHECK(nearlyEqual(row.fraction(0, 0).width(), 300.0f));
}

SOL_TEST(layout_inset_shrinks_symmetrically)
{
    const Rect small = inset(kBox, 25.0f);
    SOL_CHECK(nearlyEqual(small.min.x, 125.0f));
    SOL_CHECK(nearlyEqual(small.max.x, 475.0f));
    SOL_CHECK(nearlyEqual(small.width(), kBox.width() - 50.0f));
    SOL_CHECK(nearlyEqual(small.height(), kBox.height() - 50.0f));
}
