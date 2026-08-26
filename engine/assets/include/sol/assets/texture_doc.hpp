#pragma once

// The authored source of a texture: an ordered list of drawing ops in TOML,
// evaluated to RGBA8 (engine plan Phase 9 stage G).
//
// ⚑ This exists because the textures in this repo were ALREADY procedural - the
// recipe just lived in `tools/scripts/gen_assets.ps1`, drawn with System.Drawing
// and committed as PNG. What that arrangement cost was everything the `.forge`
// change bought for meshes: the source was opaque binary, a change was
// undiffable, the tool could not open it, and the generator only ran on Windows
// with .NET present. The ops here are the ops that script actually used, and no
// more than those - the vocabulary grows when an asset needs it.
//
// ⚑⚑ THE ROW LISTS ARE DATA AND NOT A SEED, WHICH IS THE ONE DECISION IN THIS
// FILE MOST LIKELY TO BE "TIDIED" LATER. `hull` and `cockpit` place their panels
// with `System.Random`, and .NET's subtractive generator has no C++ twin - so a
// file saying `seed = 1337, panels = 60` would re-roll every panel and change
// what the game draws. `ForgePrimitive::Mesh` already settled the general case
// for the asteroid's PowerShell noise: reproducing another runtime's arithmetic
// puts a bug-compatibility shim in this engine's permanent vocabulary. The
// second reason is the tool: a seed is a texture you can regenerate but cannot
// EDIT, and you cannot nudge one panel of it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sol::assets {

enum class TextureOp
{
    // Every pixel one colour. The background a panel scatter sits on.
    Fill,
    // Alternating cells of two colours, from the top-left.
    Checker,
    // Axis-aligned rectangles, all one colour.
    Rects,
    // Axis-aligned rectangles whose colour is a per-row grey `shade` plus the
    // op's `tint` offset. Separate from Rects because it is a different SOURCE
    // of colour, not a different shape: which member is live is decided by the
    // schema, never by the value.
    //
    // ⚑ It is also what makes the format editable rather than merely readable -
    // "make the panelling bluer" is one tint, not sixty rewritten rows.
    Panels,
    // Full-width and full-height lines of one colour: the seams between panels.
    Lines,
};

[[nodiscard]] const char* textureOpName(TextureOp op);
[[nodiscard]] bool textureOpFromName(const char* name, TextureOp& out);
// Every op, in the order an editor should offer them.
[[nodiscard]] std::span<const TextureOp> textureOps();

enum class TextureParamKind
{
    Integer,
    // Three channels, each 0-255.
    Color,
    // Three signed channel offsets, each -255..255, added to a row's shade.
    ColorOffset,
    // Rows of [x, y, w, h].
    RectList,
    // Rows of [x, y, w, h, shade].
    PanelList,
    // A flat list of coordinates.
    IntegerList,
};

struct TextureColor
{
    int r = 0;
    int g = 0;
    int b = 0;
};

struct TextureRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct TexturePanel
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    // Grey level before the op's tint is added.
    int shade = 0;
};

// One parameter value; the schema decides which member is live.
struct TextureValue
{
    std::int64_t integer = 0;
    TextureColor color;
    std::vector<TextureRect> rects;
    std::vector<TexturePanel> panels;
    std::vector<std::int64_t> integers;
};

struct TextureParamSpec
{
    const char* name = "";
    TextureParamKind kind = TextureParamKind::Integer;
    TextureValue defaultValue;
};

// ⚑ The single source of truth for what an op takes: the parser validates
// against it, the writer omits anything still at its default, and the tool's
// panel is generated from it - so adding a parameter is one table entry rather
// than three edits that can disagree. Same arrangement as `forgeParams`, and for
// the same reason.
[[nodiscard]] std::span<const TextureParamSpec> textureParams(TextureOp op);

struct TextureLayer
{
    TextureOp op = TextureOp::Fill;
    std::vector<std::pair<std::string, TextureValue>> params;

    // The comments and blank lines that stood above this op's `[[op]]` line,
    // kept verbatim and written back in front of it. Same trivia model as
    // `ForgePart::leading`, and load-bearing for the same reason: a tool that
    // saves on every edit must not reformat a committed file.
    std::string leading;

    [[nodiscard]] const TextureValue* find(const char* name) const;
    // The authored value if present, the schema default otherwise.
    [[nodiscard]] TextureValue value(const char* name) const;
    void set(const char* name, const TextureValue& value);
};

struct TextureDoc
{
    // Optional and purely a label. The identity of a texture is its file stem,
    // which is what a `[[model]]` row names.
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<TextureLayer> layers;

    std::string header;
    std::string trailer;
    // A comment this model cannot place: after a value on the same line, or
    // inside a multi-line array. The tool says so rather than dropping it.
    bool hasUnplaceableComments = false;
};

// Parses a `.tex` document. On failure returns false and sets `error`.
[[nodiscard]] bool parseTexture(const char* text,
                                std::size_t length,
                                const char* sourceName,
                                TextureDoc& out,
                                std::string* error = nullptr);

// Serialises back to TOML. A file that came from `parseTexture` comes back BYTE
// FOR BYTE, comments and blank lines included - the property stage G needs for
// the same reason stage E needed it of `writeForge`.
[[nodiscard]] std::string writeTexture(const TextureDoc& doc);

// RGBA8, row-major, no padding - the layout `cooker::ImageRgba` wants and the
// only thing the cook needs from this file.
struct TextureImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool empty() const { return pixels.empty(); }
};

// Evaluates the ops in file order, rows within an op in row order - so a later
// panel covers an earlier one and the strips sit on top of the seams, which is
// what the PowerShell drew and therefore what the committed pixels are.
//
// ⚑⚑ THE PIXEL RULES ARE MEASURED OFF THE COMMITTED PNGs, NOT ASSUMED, because
// nothing here calls GDI+ and "close enough" would be a texture that differs
// from the one the game ships. A rect covers [x, x+w) x [y, y+h). A line of
// width w centred on c covers [c - w/2, c + (w+1)/2 - 1]: for the width-2 pen
// every seam in this repo uses, that is [c-1, c], confirmed on both axes
// (cockpit's vertical seam at 222 lights columns 221-222, its horizontal seam at
// 178 lights rows 177-178). There is no antialiasing anywhere in this format.
//
// ⚑ Drawing is CLIPPED, not rejected: a panel hanging off the edge is a normal
// thing for an author to want, and Phase 16's rule is that an invariant must
// fail a BROKEN asset rather than a CHANGED one.
[[nodiscard]] bool buildTexture(const TextureDoc& doc, TextureImage& out, std::string* error = nullptr);

// How many pixels a layer actually writes, given the document's size. Zero means
// the op contributes nothing to the image - an empty row list, or geometry
// entirely off the canvas - which is the one thing about a texture source that
// is broken rather than merely unusual.
[[nodiscard]] std::size_t textureLayerCoverage(const TextureDoc& doc, std::size_t layerIndex);

// --- picking (stage I) ------------------------------------------------------
//
// ⚑⚑ THE ROW ORDINAL IS THE ORDER THE BUILDER DRAWS IN, NOT A SECOND NUMBERING
// BESIDE IT. `rects` and `panels` number their own list. `lines` numbers its
// `vertical` entries and then its `horizontal` ones, because that is the order
// they are painted. `fill` and `checker` have no list at all, so they report a
// layer with NO row - which the caller must be able to tell apart from "the
// first rect", or a click on bare hull moves something.

// How many rows of a layer can be addressed by ordinal. Zero for `fill` and
// `checker`, which paint without a row list.
[[nodiscard]] std::size_t textureRowCount(const TextureLayer& layer);

struct TextureHit
{
    // Index into `TextureDoc::layers`, or -1 for a pixel nothing painted.
    int layer = -1;
    // Row ordinal within that layer, or -1 when the op has no row list.
    int row = -1;

    [[nodiscard]] bool valid() const { return layer >= 0; }

    // True when there is a row with a position to move, which is the question
    // a drag actually asks.
    [[nodiscard]] bool movable() const { return layer >= 0 && row >= 0; }
};

// Which authored row is on TOP at every pixel, row-major, one entry per pixel.
//
// ⚑ Answered by evaluating the document and keeping the identity of whatever
// wrote each pixel last, so it agrees with the picture by construction. A
// geometric "is the cursor inside this rect" would be a second implementation
// of the half-open box, the clipping, the file order and the width-2 pen, and
// the first one to drift would select something the image does not show.
//
// ⚑ THE WHOLE MAP RATHER THAN ONE PIXEL, because the answer costs a full
// evaluation either way: a caller that asked per pixel would pay for one
// document build per query. An editor already rebuilds on every accepted edit,
// so this is computed exactly where the image is and cached beside it.
[[nodiscard]] bool textureAttribution(const TextureDoc& doc, std::vector<TextureHit>& out);

// One pixel of the above, for a caller holding no map. Costs a full evaluation,
// so do not call it in a loop over pixels.
[[nodiscard]] TextureHit textureHitTest(const TextureDoc& doc, int x, int y);

// Where a row currently sits. For a `lines` seam - which is ONE coordinate -
// the axis it does not have reads back as the value it will keep.
[[nodiscard]] bool textureRowPosition(const TextureDoc& doc, TextureHit hit, int& x, int& y);

// The box a row occupies, unclipped, so an editor can outline what is selected.
// A seam's box is the pen's span across the whole canvas, which is what it
// paints. False for a row with no geometry.
[[nodiscard]] bool textureRowBounds(const TextureDoc& doc, TextureHit hit, TextureRect& out);

// Places a row at an ABSOLUTE position, which is the whole shape of the drag
// gesture rather than an implementation detail of it.
//
// ⚑⚑ THERE IS DELIBERATELY NO `moveRow(dx, dy)`. Every value here is an
// integer, and rounding a per-frame delta does not compose to rounding the
// total: three frames of 0.4 px round to zero apiece while the hand moved 1.2,
// so an accumulating drag sticks and then jumps. The caller keeps the value the
// gesture started from and sets an absolute one, which makes the wrong
// arithmetic inexpressible instead of merely discouraged.
[[nodiscard]] bool textureSetRowPosition(TextureDoc& doc, TextureHit hit, int x, int y);

} // namespace sol::assets
