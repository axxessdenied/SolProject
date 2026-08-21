#include "sol/assets/texture_doc.hpp"

#include "sol/core/toml.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace sol::assets {
namespace {

using core::TomlValue;

struct OpName
{
    TextureOp op;
    const char* name;
};

constexpr std::array<OpName, 5> kOpNames = {{
    {TextureOp::Fill, "fill"},
    {TextureOp::Checker, "checker"},
    {TextureOp::Rects, "rects"},
    {TextureOp::Panels, "panels"},
    {TextureOp::Lines, "lines"},
}};

[[nodiscard]] TextureValue colorValue(int r, int g, int b)
{
    TextureValue out;
    out.color = {r, g, b};
    return out;
}

[[nodiscard]] TextureValue integerValue(std::int64_t v)
{
    TextureValue out;
    out.integer = v;
    return out;
}

// Collects an error with the source name and, once a layer is being read, which
// one - "hull.tex: op 3: ..." says more than "hull.tex: ..." about a file whose
// ops all look alike.
struct Reader
{
    const char* sourceName = "";
    std::string* error = nullptr;
    std::string context;

    void fail(const std::string& message) const
    {
        if (error != nullptr) {
            *error = (context.empty() ? std::string(sourceName) : context) + ": " + message;
        }
    }
};

// --- trivia -----------------------------------------------------------------
//
// The same model `forge_doc.cpp` proved and for the same reason: comments and
// blank lines attach VERBATIM to whatever stands below them, so a tool that
// saves on every edit gives a committed file back the way it found it. The only
// difference is which header a run of trivia can land on.

struct SourceTrivia
{
    std::string header;
    std::vector<std::string> layerLeading; // one entry per [[op]], in file order
    std::string trailer;
    bool unplaceable = false;
};

[[nodiscard]] bool isTriviaLine(std::string_view line)
{
    for (const char c : line) {
        if (c == ' ' || c == '\t' || c == '\r') {
            continue;
        }
        return c == '#';
    }
    return true; // blank
}

[[nodiscard]] std::string_view trimLeft(std::string_view line)
{
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return line.substr(i);
}

// ⚑ Quoted spans are skipped so a `#` or `[` inside a string reads as text
// rather than as syntax, and the bracket depth is what keeps a comment sitting
// between two panel rows attached to that value instead of to the next op.
void scanContentLine(std::string_view line, int& depth, bool& sawComment)
{
    char quote = '\0';
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != '\0') {
            if (c == '\\' && quote == '"') {
                ++i;
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            sawComment = true;
            return;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
        }
    }
}

[[nodiscard]] SourceTrivia scanTrivia(const char* text, std::size_t length)
{
    SourceTrivia trivia;
    std::string pending;
    bool sawElement = false;
    int depth = 0;

    std::size_t cursor = 0;
    while (cursor < length) {
        std::size_t end = cursor;
        while (end < length && text[end] != '\n') {
            ++end;
        }
        const std::string_view line(text + cursor, end - cursor);
        const std::size_t next = (end < length) ? end + 1 : end;

        if (depth == 0 && isTriviaLine(line)) {
            pending.append(text + cursor, next - cursor);
            if (next == end) {
                pending += '\n';
            }
            cursor = next;
            continue;
        }

        if (depth == 0) {
            const std::string_view body = trimLeft(line);
            if (body.starts_with("[[op]]")) {
                trivia.layerLeading.push_back(std::move(pending));
            } else if (!sawElement) {
                trivia.header = std::move(pending);
            } else if (!pending.empty()) {
                trivia.unplaceable = true;
            }
            sawElement = true;
            pending.clear();
        }

        bool sawComment = false;
        scanContentLine(line, depth, sawComment);
        if (sawComment) {
            trivia.unplaceable = true;
        }
        cursor = next;
    }

    trivia.trailer = std::move(pending);
    return trivia;
}

[[nodiscard]] std::string separatorFor(const std::string& leading, const std::string& soFar)
{
    if (!leading.empty()) {
        return leading;
    }
    return soFar.empty() ? std::string() : std::string("\n");
}

// --- reading values ---------------------------------------------------------

[[nodiscard]] bool readIntegerArray(const TomlValue& value, std::size_t expected,
                                    std::array<std::int64_t, 5>& out)
{
    if (!value.isArray() || value.size() != expected) {
        return false;
    }
    for (std::size_t i = 0; i < expected; ++i) {
        if (!value[i].isInteger()) {
            return false;
        }
        out[i] = value[i].asInteger();
    }
    return true;
}

[[nodiscard]] bool inRange(std::int64_t v, std::int64_t low, std::int64_t high)
{
    return v >= low && v <= high;
}

// --- drawing ----------------------------------------------------------------

[[nodiscard]] int clampChannel(int v)
{
    return std::clamp(v, 0, 255);
}

void plot(TextureImage& image, std::vector<std::uint8_t>* touched, int x, int y, TextureColor c)
{
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
        y >= static_cast<int>(image.height)) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x);
    image.pixels[index * 4 + 0] = static_cast<std::uint8_t>(clampChannel(c.r));
    image.pixels[index * 4 + 1] = static_cast<std::uint8_t>(clampChannel(c.g));
    image.pixels[index * 4 + 2] = static_cast<std::uint8_t>(clampChannel(c.b));
    image.pixels[index * 4 + 3] = 255;
    if (touched != nullptr) {
        (*touched)[index] = 1;
    }
}

// [x, x+w) x [y, y+h), clipped. Measured off the committed PNGs: this is what
// GDI+'s FillRectangle covered.
void fillRect(TextureImage& image, std::vector<std::uint8_t>* touched, int x, int y, int w, int h,
              TextureColor color)
{
    for (int row = y; row < y + h; ++row) {
        for (int column = x; column < x + w; ++column) {
            plot(image, touched, column, row, color);
        }
    }
}

// A pen of width `w` centred on `c` covers [c - w/2, c + (w+1)/2 - 1]. For the
// width-2 pen every seam in this repo uses that is [c-1, c], which is what the
// committed pixels show on both axes.
[[nodiscard]] int penStart(int centre, int width)
{
    return centre - width / 2;
}

void drawLayerInto(const TextureDoc& doc, const TextureLayer& layer, TextureImage& image,
                   std::vector<std::uint8_t>* touched)
{
    switch (layer.op) {
    case TextureOp::Fill: {
        const TextureColor color = layer.value("color").color;
        fillRect(image, touched, 0, 0, doc.width, doc.height, color);
        break;
    }
    case TextureOp::Checker: {
        const int cell = static_cast<int>(layer.value("cell").integer);
        if (cell <= 0) {
            break;
        }
        const TextureColor a = layer.value("color_a").color;
        const TextureColor b = layer.value("color_b").color;
        // ⚑ Walked per pixel rather than per whole cell so a size that is not a
        // multiple of the cell still paints to the edge. The PowerShell looped
        // whole cells and would have left a bare strip; nothing in this repo is
        // such a size, and leaving the gap in would be preserving an accident.
        for (int y = 0; y < doc.height; ++y) {
            for (int x = 0; x < doc.width; ++x) {
                const bool even = ((x / cell) + (y / cell)) % 2 == 0;
                plot(image, touched, x, y, even ? a : b);
            }
        }
        break;
    }
    case TextureOp::Rects: {
        const TextureColor color = layer.value("color").color;
        for (const TextureRect& rect : layer.value("rects").rects) {
            fillRect(image, touched, rect.x, rect.y, rect.w, rect.h, color);
        }
        break;
    }
    case TextureOp::Panels: {
        const TextureColor tint = layer.value("tint").color;
        for (const TexturePanel& panel : layer.value("panels").panels) {
            const TextureColor color{panel.shade + tint.r, panel.shade + tint.g,
                                     panel.shade + tint.b};
            fillRect(image, touched, panel.x, panel.y, panel.w, panel.h, color);
        }
        break;
    }
    case TextureOp::Lines: {
        const TextureColor color = layer.value("color").color;
        const int width = static_cast<int>(layer.value("width").integer);
        if (width <= 0) {
            break;
        }
        for (const std::int64_t c : layer.value("vertical").integers) {
            fillRect(image, touched, penStart(static_cast<int>(c), width), 0, width, doc.height,
                     color);
        }
        for (const std::int64_t c : layer.value("horizontal").integers) {
            fillRect(image, touched, 0, penStart(static_cast<int>(c), width), doc.width, width,
                     color);
        }
        break;
    }
    }
}

} // namespace

const char* textureOpName(TextureOp op)
{
    for (const OpName& row : kOpNames) {
        if (row.op == op) {
            return row.name;
        }
    }
    return "fill";
}

bool textureOpFromName(const char* name, TextureOp& out)
{
    if (name == nullptr) {
        return false;
    }
    for (const OpName& row : kOpNames) {
        if (std::string_view(name) == row.name) {
            out = row.op;
            return true;
        }
    }
    return false;
}

std::span<const TextureOp> textureOps()
{
    static const std::array<TextureOp, 5> kAll = {TextureOp::Fill, TextureOp::Checker,
                                                  TextureOp::Rects, TextureOp::Panels,
                                                  TextureOp::Lines};
    return kAll;
}

std::span<const TextureParamSpec> textureParams(TextureOp op)
{
    static const std::vector<TextureParamSpec> kFill = {
        {"color", TextureParamKind::Color, colorValue(0, 0, 0)},
    };
    static const std::vector<TextureParamSpec> kChecker = {
        {"cell", TextureParamKind::Integer, integerValue(32)},
        {"color_a", TextureParamKind::Color, colorValue(255, 255, 255)},
        {"color_b", TextureParamKind::Color, colorValue(0, 0, 0)},
    };
    static const std::vector<TextureParamSpec> kRects = {
        {"color", TextureParamKind::Color, colorValue(255, 255, 255)},
        {"rects", TextureParamKind::RectList, TextureValue{}},
    };
    static const std::vector<TextureParamSpec> kPanels = {
        {"tint", TextureParamKind::ColorOffset, colorValue(0, 0, 0)},
        {"panels", TextureParamKind::PanelList, TextureValue{}},
    };
    static const std::vector<TextureParamSpec> kLines = {
        {"color", TextureParamKind::Color, colorValue(0, 0, 0)},
        {"width", TextureParamKind::Integer, integerValue(1)},
        {"vertical", TextureParamKind::IntegerList, TextureValue{}},
        {"horizontal", TextureParamKind::IntegerList, TextureValue{}},
    };

    switch (op) {
    case TextureOp::Fill:
        return kFill;
    case TextureOp::Checker:
        return kChecker;
    case TextureOp::Rects:
        return kRects;
    case TextureOp::Panels:
        return kPanels;
    case TextureOp::Lines:
        return kLines;
    }
    return kFill;
}

const TextureValue* TextureLayer::find(const char* name) const
{
    for (const std::pair<std::string, TextureValue>& entry : params) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

TextureValue TextureLayer::value(const char* name) const
{
    if (const TextureValue* authored = find(name); authored != nullptr) {
        return *authored;
    }
    for (const TextureParamSpec& spec : textureParams(op)) {
        if (std::string_view(spec.name) == name) {
            return spec.defaultValue;
        }
    }
    return {};
}

void TextureLayer::set(const char* name, const TextureValue& value)
{
    for (std::pair<std::string, TextureValue>& entry : params) {
        if (entry.first == name) {
            entry.second = value;
            return;
        }
    }
    params.emplace_back(name, value);
}

bool parseTexture(const char* text, std::size_t length, const char* sourceName, TextureDoc& out,
                  std::string* error)
{
    TomlValue root;
    std::string tomlError;
    if (!TomlValue::parse(text, length, root, &tomlError)) {
        if (error != nullptr) {
            *error = std::string(sourceName) + ": " + tomlError;
        }
        return false;
    }

    Reader reader{sourceName, error, {}};
    TextureDoc doc;

    if (const TomlValue* name = root.find("name"); name != nullptr) {
        if (!name->isString()) {
            reader.fail("'name' must be a string");
            return false;
        }
        doc.name = name->asString();
    }

    const TomlValue* size = root.find("size");
    if (size == nullptr) {
        reader.fail("missing key 'size'");
        return false;
    }
    std::array<std::int64_t, 5> dimensions{};
    if (!readIntegerArray(*size, 2, dimensions)) {
        reader.fail("'size' must be [width, height]");
        return false;
    }
    // 8192 is well past anything this game draws and short of a number that
    // would allocate gigabytes from a typo.
    if (!inRange(dimensions[0], 1, 8192) || !inRange(dimensions[1], 1, 8192)) {
        reader.fail("'size' must be between 1 and 8192 in each dimension");
        return false;
    }
    doc.width = static_cast<int>(dimensions[0]);
    doc.height = static_cast<int>(dimensions[1]);

    const TomlValue* ops = root.find("op");
    if (ops != nullptr) {
        if (!ops->isArray()) {
            reader.fail("'op' must be an [[op]] array");
            return false;
        }
        for (std::size_t i = 0; i < ops->size(); ++i) {
            const TomlValue& table = (*ops)[i];
            reader.context = std::string(sourceName) + ": op " + std::to_string(i);
            if (!table.isTable()) {
                reader.fail("'op' must be an [[op]] array");
                return false;
            }

            const TomlValue* kind = table.find("kind");
            if (kind == nullptr || !kind->isString()) {
                reader.fail("missing key 'kind'");
                return false;
            }
            TextureLayer layer;
            if (!textureOpFromName(kind->asString().c_str(), layer.op)) {
                reader.fail("unknown op kind '" + kind->asString() + "'");
                return false;
            }

            const std::span<const TextureParamSpec> specs = textureParams(layer.op);
            for (const std::pair<std::string, TomlValue>& member : table.members()) {
                if (member.first == "kind") {
                    continue;
                }
                const TextureParamSpec* spec = nullptr;
                for (const TextureParamSpec& candidate : specs) {
                    if (member.first == candidate.name) {
                        spec = &candidate;
                        break;
                    }
                }
                if (spec == nullptr) {
                    reader.fail("'" + std::string(textureOpName(layer.op)) +
                                "' has no parameter '" + member.first + "'");
                    return false;
                }

                const TomlValue& value = member.second;
                TextureValue parsed;
                switch (spec->kind) {
                case TextureParamKind::Integer:
                    if (!value.isInteger()) {
                        reader.fail("'" + member.first + "' must be an integer");
                        return false;
                    }
                    parsed.integer = value.asInteger();
                    break;
                case TextureParamKind::Color:
                case TextureParamKind::ColorOffset: {
                    std::array<std::int64_t, 5> channels{};
                    if (!readIntegerArray(value, 3, channels)) {
                        reader.fail("'" + member.first + "' must be [r, g, b]");
                        return false;
                    }
                    const std::int64_t low =
                        spec->kind == TextureParamKind::ColorOffset ? -255 : 0;
                    for (std::size_t c = 0; c < 3; ++c) {
                        if (!inRange(channels[c], low, 255)) {
                            reader.fail("'" + member.first + "' channels must be " +
                                        std::to_string(low) + "..255");
                            return false;
                        }
                    }
                    parsed.color = {static_cast<int>(channels[0]), static_cast<int>(channels[1]),
                                    static_cast<int>(channels[2])};
                    break;
                }
                case TextureParamKind::RectList:
                case TextureParamKind::PanelList: {
                    const bool shaded = spec->kind == TextureParamKind::PanelList;
                    const std::size_t arity = shaded ? 5 : 4;
                    if (!value.isArray()) {
                        reader.fail("'" + member.first + "' must be a list of rows");
                        return false;
                    }
                    for (std::size_t row = 0; row < value.size(); ++row) {
                        std::array<std::int64_t, 5> numbers{};
                        if (!readIntegerArray(value[row], arity, numbers)) {
                            reader.fail("'" + member.first + "' row " + std::to_string(row) +
                                        " must be " + std::to_string(arity) + " integers");
                            return false;
                        }
                        if (numbers[2] < 0 || numbers[3] < 0) {
                            reader.fail("'" + member.first + "' row " + std::to_string(row) +
                                        " has a negative size");
                            return false;
                        }
                        if (shaded) {
                            if (!inRange(numbers[4], 0, 255)) {
                                reader.fail("'" + member.first + "' row " + std::to_string(row) +
                                            " shade must be 0..255");
                                return false;
                            }
                            parsed.panels.push_back({static_cast<int>(numbers[0]),
                                                     static_cast<int>(numbers[1]),
                                                     static_cast<int>(numbers[2]),
                                                     static_cast<int>(numbers[3]),
                                                     static_cast<int>(numbers[4])});
                        } else {
                            parsed.rects.push_back(
                                {static_cast<int>(numbers[0]), static_cast<int>(numbers[1]),
                                 static_cast<int>(numbers[2]), static_cast<int>(numbers[3])});
                        }
                    }
                    break;
                }
                case TextureParamKind::IntegerList:
                    if (!value.isArray()) {
                        reader.fail("'" + member.first + "' must be a list of integers");
                        return false;
                    }
                    for (std::size_t element = 0; element < value.size(); ++element) {
                        if (!value[element].isInteger()) {
                            reader.fail("'" + member.first + "' must be a list of integers");
                            return false;
                        }
                        parsed.integers.push_back(value[element].asInteger());
                    }
                    break;
                }
                layer.set(member.first.c_str(), parsed);
            }

            doc.layers.push_back(std::move(layer));
        }
        reader.context.clear();
    }

    const SourceTrivia trivia = scanTrivia(text, length);
    doc.header = trivia.header;
    doc.trailer = trivia.trailer;
    doc.hasUnplaceableComments = trivia.unplaceable;
    for (std::size_t i = 0; i < doc.layers.size() && i < trivia.layerLeading.size(); ++i) {
        doc.layers[i].leading = trivia.layerLeading[i];
    }

    out = std::move(doc);
    return true;
}

std::string writeTexture(const TextureDoc& doc)
{
    // Faithful or nothing, exactly as `writeForge`: no invented header, no
    // invented key. The byte-exact round trip is the whole guarantee.
    std::string out;
    out += doc.header;
    if (!doc.name.empty()) {
        out += "name = \"" + doc.name + "\"\n";
    }
    out += "size = [" + std::to_string(doc.width) + ", " + std::to_string(doc.height) + "]\n";

    for (const TextureLayer& layer : doc.layers) {
        out += separatorFor(layer.leading, out);
        out += "[[op]]\n";
        out += std::string("kind = \"") + textureOpName(layer.op) + "\"\n";

        for (const TextureParamSpec& spec : textureParams(layer.op)) {
            const TextureValue* authored = layer.find(spec.name);
            if (authored == nullptr) {
                continue;
            }
            out += std::string(spec.name) + " = ";
            switch (spec.kind) {
            case TextureParamKind::Integer:
                out += std::to_string(authored->integer);
                break;
            case TextureParamKind::Color:
            case TextureParamKind::ColorOffset:
                out += "[" + std::to_string(authored->color.r) + ", " +
                       std::to_string(authored->color.g) + ", " +
                       std::to_string(authored->color.b) + "]";
                break;
            // ⚑ One row per line, which is the same argument the baked vertex
            // list makes: moving one panel must be one changed line, or the
            // format has no advantage over the PNG it replaced.
            case TextureParamKind::RectList:
                out += "[\n";
                for (const TextureRect& rect : authored->rects) {
                    out += "  [" + std::to_string(rect.x) + ", " + std::to_string(rect.y) + ", " +
                           std::to_string(rect.w) + ", " + std::to_string(rect.h) + "],\n";
                }
                out += "]";
                break;
            case TextureParamKind::PanelList:
                out += "[\n";
                for (const TexturePanel& panel : authored->panels) {
                    out += "  [" + std::to_string(panel.x) + ", " + std::to_string(panel.y) +
                           ", " + std::to_string(panel.w) + ", " + std::to_string(panel.h) +
                           ", " + std::to_string(panel.shade) + "],\n";
                }
                out += "]";
                break;
            case TextureParamKind::IntegerList: {
                out += "[";
                for (std::size_t i = 0; i < authored->integers.size(); ++i) {
                    if (i != 0) {
                        out += ", ";
                    }
                    out += std::to_string(authored->integers[i]);
                }
                out += "]";
                break;
            }
            }
            out += "\n";
        }
    }

    out += doc.trailer;
    return out;
}

bool buildTexture(const TextureDoc& doc, TextureImage& out, std::string* error)
{
    if (doc.width <= 0 || doc.height <= 0) {
        if (error != nullptr) {
            *error = "texture has no size";
        }
        return false;
    }

    TextureImage image;
    image.width = static_cast<std::uint32_t>(doc.width);
    image.height = static_cast<std::uint32_t>(doc.height);
    // Opaque black, so a document that paints nothing still produces a valid
    // image rather than a buffer of uninitialised alpha.
    image.pixels.assign(static_cast<std::size_t>(image.width) * image.height * 4, 0);
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        image.pixels[i] = 255;
    }

    for (const TextureLayer& layer : doc.layers) {
        drawLayerInto(doc, layer, image, nullptr);
    }

    out = std::move(image);
    return true;
}

std::size_t textureLayerCoverage(const TextureDoc& doc, std::size_t layerIndex)
{
    if (layerIndex >= doc.layers.size() || doc.width <= 0 || doc.height <= 0) {
        return 0;
    }
    TextureImage image;
    image.width = static_cast<std::uint32_t>(doc.width);
    image.height = static_cast<std::uint32_t>(doc.height);
    image.pixels.assign(static_cast<std::size_t>(image.width) * image.height * 4, 0);

    std::vector<std::uint8_t> touched(static_cast<std::size_t>(doc.width) * doc.height, 0);
    drawLayerInto(doc, doc.layers[layerIndex], image, &touched);

    std::size_t count = 0;
    for (const std::uint8_t flag : touched) {
        count += flag;
    }
    return count;
}

} // namespace sol::assets
