#include "sol/assets/forge_doc.hpp"

#include "sol/assets/mesh_edit.hpp"
#include "sol/core/math/scalar.hpp"
#include "sol/core/toml.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>

namespace sol::assets {
namespace {

using core::TomlValue;

constexpr const char* kPrimitiveNames[] = {"group",         "box",     "beam",    "torus",
                                           "flat_triangle", "revolve", "extrude", "mesh"};
constexpr ForgePrimitive kPrimitives[] = {
    ForgePrimitive::Group,        ForgePrimitive::Box,     ForgePrimitive::Beam,
    ForgePrimitive::Torus,        ForgePrimitive::FlatTriangle, ForgePrimitive::Revolve,
    ForgePrimitive::Extrude,      ForgePrimitive::Mesh,
};

// ⚑ Where every comment and blank line in the source sat, so the writer can put
// it back. It has to be a second pass over the same text because the TOML parser
// hands back values and nothing else: by the time a document exists, every
// comment that was in the file is already gone.
//
// The rule is that trivia attaches to whatever comes BELOW it, which is how a
// person reads a heading. What is stored is the raw lines - comments and blank
// lines together, verbatim - rather than a list of comments, because
// `cockpit.forge` puts a blank line between its `# --- canopy frame ---` divider
// and the part that divider introduces. Keeping only the words would reproduce
// them and quietly close the gap.
struct SourceTrivia
{
    std::string header;
    std::string buildLeading;
    std::vector<std::string> partLeading; // one entry per [[part]], in file order
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

// Advances the bracket depth across one line that carries content, and reports
// a `#` found after a value on it.
//
// ⚑ Quoted spans are skipped, so a `#` or a `[` inside an id is read as text
// rather than as syntax. Without that, one part called "a[b" would put the
// scanner permanently inside an array and every comment below it would be
// dropped - a failure that would be invisible until somebody's header vanished.
void scanContentLine(std::string_view line, int& depth, bool& sawComment)
{
    char quote = '\0';
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != '\0') {
            if (c == '\\' && quote == '"') {
                ++i; // an escape consumes the next character, quote included
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

// ⚑ Trivia is only collected at bracket depth ZERO. A baked part is a
// multi-line array of vertices, and a `#` line between two of them belongs to
// that value, not to the next part: attaching it would move the comment
// somewhere else in the file on the next save, which is worse than admitting it
// cannot be placed. `unplaceable` is how the tool gets to say so.
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
                pending += '\n'; // a last line with no newline still needs one back
            }
            cursor = next;
            continue;
        }

        if (depth == 0) {
            const std::string_view body = trimLeft(line);
            // ⚑ A part's own trivia goes to the PART even when it is the first
            // thing in the file, and the file header is only what stands above
            // the first plain key. Splitting it the other way - "everything
            // before the first element is the header" - double-counts, because
            // the writer would then emit the header AND its own separator in
            // front of part one, and the file would grow a blank line on every
            // save. A file that saves differently from how it loaded is exactly
            // what this change exists to stop.
            if (body.starts_with("[[part]]")) {
                trivia.partLeading.push_back(std::move(pending));
            } else if (body.starts_with("[build]")) {
                trivia.buildLeading = std::move(pending);
            } else if (!sawElement) {
                trivia.header = std::move(pending);
            } else if (!pending.empty()) {
                // A comment above a plain key, which has no slot: the document
                // stores parts and a header, not a note per field.
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

// What goes in front of a table header: whatever the source had there, or a
// blank line for an element the tool created.
//
// ⚑ The blank line is suppressed when nothing has been written yet, because a
// file that opens straight into `[[part]]` must save that way. Emitting one
// unconditionally would push a blank line onto the front of such a file, and
// then another on the next save.
[[nodiscard]] std::string separatorFor(const std::string& leading, const std::string& soFar)
{
    if (!leading.empty()) {
        return leading;
    }
    return soFar.empty() ? std::string() : std::string("\n");
}

[[nodiscard]] ForgeValue scalarValue(double v)
{
    ForgeValue out;
    out.scalar = v;
    return out;
}

[[nodiscard]] ForgeValue vecValue(BuildPoint v)
{
    ForgeValue out;
    out.vec = v;
    return out;
}

[[nodiscard]] ForgeValue uvValue(double u, double v)
{
    ForgeValue out;
    out.uv = {u, v};
    return out;
}

// The shortest text that reads back as exactly this double: an authored `47`
// must not come back as `47.000000`, and a computed 0.1 must not come back as
// something that reparses to a different number.
//
// ⚑ Exponent forms are skipped while a plain one still round-trips, and that is
// not cosmetic. The shortest round-tripping form of 90 is `9e+01`, and of 100
// is `1e+02` - so the round numbers an author is MOST likely to type are
// exactly the ones a naive shortest-form writer mangles. This file is read and
// edited by hand; that is the whole reason it is text.
void appendNumber(std::string& out, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    for (int precision = 1; precision <= 17; ++precision) {
        char candidate[64];
        std::snprintf(candidate, sizeof(candidate), "%.*g", precision, value);
        if (std::strpbrk(candidate, "eE") != nullptr) {
            continue; // a very large or very small number keeps its exponent
        }
        if (std::strtod(candidate, nullptr) == value) {
            std::memcpy(buffer, candidate, sizeof(buffer));
            break;
        }
    }
    out += buffer;
    // TOML tells integers and floats apart by the dot, and every number here is
    // a float to the reader, so keep one.
    if (std::strpbrk(buffer, ".eEni") == nullptr) {
        out += ".0";
    }
}

// As appendNumber, but the value only has to survive a round trip through
// FLOAT. Baked geometry came out of a MeshData, whose vertices are float, so
// the extra digits a double writer emits are not precision - they are the
// decimal expansion of a binary value that carries no more information. At 960
// vertices and eight numbers each, the difference is a file a person can scan
// against one they cannot.
void appendMeshNumber(std::string& out, double value)
{
    const auto target = static_cast<float>(value);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    for (int precision = 1; precision <= 9; ++precision) {
        char candidate[64];
        std::snprintf(candidate, sizeof(candidate), "%.*g", precision, value);
        if (std::strpbrk(candidate, "eE") != nullptr) {
            continue;
        }
        if (static_cast<float>(std::strtod(candidate, nullptr)) == target) {
            std::memcpy(buffer, candidate, sizeof(buffer));
            break;
        }
    }
    out += buffer;
    if (std::strpbrk(buffer, ".eEni") == nullptr) {
        out += ".0";
    }
}

void appendVec(std::string& out, BuildPoint v)
{
    out += "[";
    appendNumber(out, v.x);
    out += ", ";
    appendNumber(out, v.y);
    out += ", ";
    appendNumber(out, v.z);
    out += "]";
}

struct Reader
{
    std::string context;
    std::string* outError = nullptr;
    bool failed = false;

    void fail(const std::string& message)
    {
        if (!failed && outError != nullptr) {
            *outError = context + ": " + message;
        }
        failed = true;
    }

    [[nodiscard]] bool readVec(const TomlValue& value, const char* key, BuildPoint& out)
    {
        if (!value.isArray() || value.size() != 3) {
            fail(std::string("'") + key + "' must be an array of three numbers");
            return false;
        }
        double parts[3] = {0, 0, 0};
        for (std::size_t i = 0; i < 3; ++i) {
            const TomlValue& element = value[i];
            if (!element.isFloat() && !element.isInteger()) {
                fail(std::string("'") + key + "' must be an array of three numbers");
                return false;
            }
            parts[i] = element.asFloat();
        }
        out = {parts[0], parts[1], parts[2]};
        return true;
    }

    [[nodiscard]] bool readValue(const TomlValue& value, const ForgeParamSpec& spec, ForgeValue& out)
    {
        switch (spec.kind) {
        case ForgeParamKind::Scalar:
        case ForgeParamKind::Integer:
            if (!value.isFloat() && !value.isInteger()) {
                fail(std::string("'") + spec.name + "' must be a number");
                return false;
            }
            out.scalar = value.asFloat();
            return true;
        case ForgeParamKind::Boolean:
            if (!value.isBool()) {
                fail(std::string("'") + spec.name + "' must be true or false");
                return false;
            }
            out.scalar = value.asBool() ? 1.0 : 0.0;
            return true;
        case ForgeParamKind::Vec3:
            return readVec(value, spec.name, out.vec);
        case ForgeParamKind::Uv: {
            if (!value.isArray() || value.size() != 2) {
                fail(std::string("'") + spec.name + "' must be an array of two numbers");
                return false;
            }
            double parts[2] = {0, 0};
            for (std::size_t i = 0; i < 2; ++i) {
                const TomlValue& element = value[i];
                if (!element.isFloat() && !element.isInteger()) {
                    fail(std::string("'") + spec.name + "' must be an array of two numbers");
                    return false;
                }
                parts[i] = element.asFloat();
            }
            out.uv = {parts[0], parts[1]};
            return true;
        }
        case ForgeParamKind::Profile: {
            if (!value.isArray()) {
                fail(std::string("'") + spec.name + "' must be an array of [x, y] pairs");
                return false;
            }
            out.profile.clear();
            for (std::size_t i = 0; i < value.size(); ++i) {
                const TomlValue& pair = value[i];
                if (!pair.isArray() || pair.size() != 2 ||
                    (!pair[0].isFloat() && !pair[0].isInteger()) ||
                    (!pair[1].isFloat() && !pair[1].isInteger())) {
                    fail(std::string("'") + spec.name + "' must be an array of [x, y] pairs");
                    return false;
                }
                out.profile.push_back({pair[0].asFloat(), pair[1].asFloat()});
            }
            return true;
        }
        case ForgeParamKind::VertexList: {
            if (!value.isArray()) {
                fail(std::string("'") + spec.name +
                     "' must be an array of [px, py, pz, nx, ny, nz, u, v] rows");
                return false;
            }
            out.vertices.clear();
            out.vertices.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i) {
                const TomlValue& row = value[i];
                double parts[8] = {};
                if (!row.isArray() || row.size() != 8) {
                    fail(std::string("'") + spec.name + "' row " + std::to_string(i) +
                         " must be eight numbers: position, normal, uv");
                    return false;
                }
                for (std::size_t c = 0; c < 8; ++c) {
                    if (!row[c].isFloat() && !row[c].isInteger()) {
                        fail(std::string("'") + spec.name + "' row " + std::to_string(i) +
                             " must be eight numbers: position, normal, uv");
                        return false;
                    }
                    parts[c] = row[c].asFloat();
                }
                out.vertices.push_back({{parts[0], parts[1], parts[2]},
                                        {parts[3], parts[4], parts[5]},
                                        {parts[6], parts[7]}});
            }
            return true;
        }
        case ForgeParamKind::IndexList: {
            if (!value.isArray()) {
                fail(std::string("'") + spec.name + "' must be an array of whole numbers");
                return false;
            }
            out.indices.clear();
            out.indices.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i) {
                const TomlValue& element = value[i];
                if (!element.isInteger() || element.asInteger() < 0) {
                    fail(std::string("'") + spec.name +
                         "' must be an array of whole numbers, none negative");
                    return false;
                }
                out.indices.push_back(static_cast<std::uint32_t>(element.asInteger()));
            }
            return true;
        }
        }
        return false;
    }
};

// Every part's world transform, resolved in one pass. Parents may appear after
// their children in the file, so this walks each chain rather than assuming an
// order the format does not require.
[[nodiscard]] std::vector<BuildTransform> worldTransforms(const ForgeDoc& doc)
{
    std::vector<BuildTransform> world(doc.parts.size());
    std::vector<bool> resolved(doc.parts.size(), false);
    std::vector<std::size_t> chain;

    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        chain.clear();
        std::size_t cursor = i;
        while (cursor != std::string::npos && !resolved[cursor]) {
            chain.push_back(cursor);
            const std::string& parent = doc.parts[cursor].parent;
            cursor = parent.empty() ? std::string::npos : doc.indexOf(parent);
        }
        // Deepest ancestor first, so each step composes onto a resolved parent.
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const ForgePart& part = doc.parts[*it];
            const std::size_t parent = part.parent.empty() ? std::string::npos
                                                           : doc.indexOf(part.parent);
            world[*it] = parent == std::string::npos
                             ? part.localTransform()
                             : world[parent] * part.localTransform();
            resolved[*it] = true;
        }
    }
    return world;
}

} // namespace

const char* forgePrimitiveName(ForgePrimitive primitive)
{
    return kPrimitiveNames[static_cast<std::size_t>(primitive)];
}

bool forgePrimitiveFromName(const char* name, ForgePrimitive& out)
{
    for (std::size_t i = 0; i < std::size(kPrimitiveNames); ++i) {
        if (std::strcmp(kPrimitiveNames[i], name) == 0) {
            out = static_cast<ForgePrimitive>(i);
            return true;
        }
    }
    return false;
}

std::span<const ForgePrimitive> forgePrimitives()
{
    return {kPrimitives, std::size(kPrimitives)};
}

std::span<const ForgeParamSpec> forgeParams(ForgePrimitive primitive)
{
    // Defaults are the shape a newly added part takes in the editor, so they
    // are chosen to be visible at once rather than to be neutral: a box of zero
    // size is a correct default and a useless one.
    static const std::vector<ForgeParamSpec> kGroup;
    static const std::vector<ForgeParamSpec> kBox = {
        {"center", ForgeParamKind::Vec3, vecValue({0, 0, 0})},
        {"size", ForgeParamKind::Vec3, vecValue({1, 1, 1})},
        {"tile", ForgeParamKind::Scalar, scalarValue(0.0)},
    };
    static const std::vector<ForgeParamSpec> kBeam = {
        {"from", ForgeParamKind::Vec3, vecValue({0, 0, 0})},
        {"to", ForgeParamKind::Vec3, vecValue({0, 1, 0})},
        {"width", ForgeParamKind::Scalar, scalarValue(0.1)},
        {"height", ForgeParamKind::Scalar, scalarValue(0.1)},
        {"tile", ForgeParamKind::Scalar, scalarValue(0.0)},
    };
    static const std::vector<ForgeParamSpec> kTorus = {
        {"major_radius", ForgeParamKind::Scalar, scalarValue(1.0)},
        {"tube_radius", ForgeParamKind::Scalar, scalarValue(0.25)},
        {"segments_u", ForgeParamKind::Integer, scalarValue(32)},
        {"segments_v", ForgeParamKind::Integer, scalarValue(12)},
        {"u_tiles", ForgeParamKind::Scalar, scalarValue(1.0)},
    };
    static const std::vector<ForgeParamSpec> kFlatTriangle = {
        {"p0", ForgeParamKind::Vec3, vecValue({0, 0, 0})},
        {"p1", ForgeParamKind::Vec3, vecValue({1, 0, 0})},
        {"p2", ForgeParamKind::Vec3, vecValue({0, 1, 0})},
        {"uv0", ForgeParamKind::Uv, uvValue(0, 0)},
        {"uv1", ForgeParamKind::Uv, uvValue(1, 0)},
        {"uv2", ForgeParamKind::Uv, uvValue(0, 1)},
    };
    static const std::vector<ForgeParamSpec> kRevolve = {
        {"profile", ForgeParamKind::Profile, ForgeValue{}},
        {"segments", ForgeParamKind::Integer, scalarValue(24)},
        {"u_tiles", ForgeParamKind::Scalar, scalarValue(1.0)},
        {"cap_ends", ForgeParamKind::Boolean, scalarValue(0.0)},
    };
    // A baked part defaults to EMPTY rather than to some placeholder solid: a
    // bake is only ever created from geometry that already exists, so a default
    // shape here would be a shape nobody asked for.
    static const std::vector<ForgeParamSpec> kMesh = {
        {"vertices", ForgeParamKind::VertexList, ForgeValue{}},
        {"indices", ForgeParamKind::IndexList, ForgeValue{}},
    };
    static const std::vector<ForgeParamSpec> kExtrude = {
        {"outline", ForgeParamKind::Profile, ForgeValue{}},
        {"from", ForgeParamKind::Vec3, vecValue({0, 0, 0})},
        {"to", ForgeParamKind::Vec3, vecValue({0, 0, 1})},
        {"tile", ForgeParamKind::Scalar, scalarValue(0.0)},
        {"cap_ends", ForgeParamKind::Boolean, scalarValue(1.0)},
    };

    switch (primitive) {
    case ForgePrimitive::Group:
        return kGroup;
    case ForgePrimitive::Box:
        return kBox;
    case ForgePrimitive::Beam:
        return kBeam;
    case ForgePrimitive::Torus:
        return kTorus;
    case ForgePrimitive::FlatTriangle:
        return kFlatTriangle;
    case ForgePrimitive::Mesh:
        return kMesh;
    case ForgePrimitive::Revolve:
        return kRevolve;
    case ForgePrimitive::Extrude:
        return kExtrude;
    }
    return kGroup;
}

const ForgeValue* ForgePart::find(const char* name) const
{
    for (const auto& [key, value] : params) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

ForgeValue ForgePart::value(const char* name) const
{
    if (const ForgeValue* found = find(name); found != nullptr) {
        return *found;
    }
    for (const ForgeParamSpec& spec : forgeParams(primitive)) {
        if (std::strcmp(spec.name, name) == 0) {
            return spec.defaultValue;
        }
    }
    return {};
}

void ForgePart::set(const char* name, const ForgeValue& newValue)
{
    for (auto& [key, existing] : params) {
        if (key == name) {
            existing = newValue;
            return;
        }
    }
    params.emplace_back(name, newValue);
}

BuildTransform ForgePart::localTransform() const
{
    return BuildTransform::fromTrs(position,
                                   {rotationDegrees.x * (core::kPiD / 180.0),
                                    rotationDegrees.y * (core::kPiD / 180.0),
                                    rotationDegrees.z * (core::kPiD / 180.0)},
                                   scale);
}

const ForgePart* ForgeDoc::find(const std::string& id) const
{
    const std::size_t index = indexOf(id);
    return index == std::string::npos ? nullptr : &parts[index];
}

std::size_t ForgeDoc::indexOf(const std::string& id) const
{
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].id == id) {
            return i;
        }
    }
    return std::string::npos;
}

std::string ForgeDoc::uniqueId(const std::string& base) const
{
    if (indexOf(base) == std::string::npos) {
        return base;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (indexOf(candidate) == std::string::npos) {
            return candidate;
        }
    }
    return base;
}

bool parseForge(const char* text, std::size_t length, const char* sourceName, ForgeDoc& out,
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

    Reader reader{sourceName, error, false};
    ForgeDoc doc;

    if (const TomlValue* name = root.find("name"); name != nullptr) {
        if (!name->isString()) {
            reader.fail("'name' must be a string");
            return false;
        }
        doc.name = name->asString();
    }

    if (const TomlValue* build = root.find("build"); build != nullptr) {
        if (!build->isTable()) {
            reader.fail("'build' must be a table");
            return false;
        }
        if (const TomlValue* weld = build->find("weld"); weld != nullptr) {
            if (!weld->isBool()) {
                reader.fail("'build.weld' must be true or false");
                return false;
            }
            doc.build.weld = weld->asBool();
        }
        if (const TomlValue* optimize = build->find("optimize"); optimize != nullptr) {
            if (!optimize->isBool()) {
                reader.fail("'build.optimize' must be true or false");
                return false;
            }
            doc.build.optimize = optimize->asBool();
        }
        if (const TomlValue* smooth = build->find("smooth_angle"); smooth != nullptr) {
            if (!smooth->isFloat() && !smooth->isInteger()) {
                reader.fail("'build.smooth_angle' must be a number");
                return false;
            }
            doc.build.smoothAngleDegrees = smooth->asFloat();
        }
    }

    const TomlValue* parts = root.find("part");
    if (parts != nullptr) {
        if (!parts->isArray()) {
            reader.fail("'part' must be a [[part]] array");
            return false;
        }
        for (std::size_t i = 0; i < parts->size(); ++i) {
            const TomlValue& table = (*parts)[i];
            if (!table.isTable()) {
                reader.fail("'part' must be a [[part]] array");
                return false;
            }

            ForgePart part;
            const TomlValue* id = table.find("id");
            if (id == nullptr || !id->isString() || id->asString().empty()) {
                reader.context = std::string(sourceName) + ": part " + std::to_string(i);
                reader.fail("missing key 'id'");
                return false;
            }
            part.id = id->asString();
            reader.context = std::string(sourceName) + ": part '" + part.id + "'";

            if (doc.indexOf(part.id) != std::string::npos) {
                reader.fail("duplicate part id");
                return false;
            }

            const TomlValue* type = table.find("type");
            if (type == nullptr || !type->isString()) {
                reader.fail("missing key 'type'");
                return false;
            }
            if (!forgePrimitiveFromName(type->asString().c_str(), part.primitive)) {
                reader.fail("unknown primitive '" + type->asString() + "'");
                return false;
            }

            if (const TomlValue* parent = table.find("parent"); parent != nullptr) {
                if (!parent->isString()) {
                    reader.fail("'parent' must be a string");
                    return false;
                }
                part.parent = parent->asString();
                if (part.parent == part.id) {
                    reader.fail("part is its own parent");
                    return false;
                }
            }

            if (const TomlValue* position = table.find("position"); position != nullptr) {
                if (!reader.readVec(*position, "position", part.position)) {
                    return false;
                }
            }
            if (const TomlValue* rotation = table.find("rotation"); rotation != nullptr) {
                if (!reader.readVec(*rotation, "rotation", part.rotationDegrees)) {
                    return false;
                }
            }
            if (const TomlValue* scale = table.find("scale"); scale != nullptr) {
                if (!reader.readVec(*scale, "scale", part.scale)) {
                    return false;
                }
            }

            // ⚑ Strict: an unrecognised key is an error rather than a shrug.
            // A misspelt `major_radius` that parsed silently would leave the
            // author staring at a torus that ignored the number they typed.
            const std::span<const ForgeParamSpec> schema = forgeParams(part.primitive);
            for (const auto& [key, value] : table.members()) {
                if (key == "id" || key == "type" || key == "parent" || key == "position" ||
                    key == "rotation" || key == "scale") {
                    continue;
                }
                const ForgeParamSpec* spec = nullptr;
                for (const ForgeParamSpec& candidate : schema) {
                    if (key == candidate.name) {
                        spec = &candidate;
                        break;
                    }
                }
                if (spec == nullptr) {
                    reader.fail("'" + key + "' is not a parameter of " +
                                forgePrimitiveName(part.primitive));
                    return false;
                }
                ForgeValue parsed;
                if (!reader.readValue(value, *spec, parsed)) {
                    return false;
                }
                part.set(spec->name, parsed);
            }

            doc.parts.push_back(std::move(part));
        }
    }

    // Parents resolve, and the graph is acyclic. Both are checked after every
    // part is in, because a file is free to name a parent declared below it.
    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        const ForgePart& part = doc.parts[i];
        if (part.parent.empty()) {
            continue;
        }
        reader.context = std::string(sourceName) + ": part '" + part.id + "'";
        if (doc.indexOf(part.parent) == std::string::npos) {
            reader.fail("unknown parent '" + part.parent + "'");
            return false;
        }
        std::size_t cursor = i;
        for (std::size_t step = 0; step <= doc.parts.size(); ++step) {
            const std::string& parent = doc.parts[cursor].parent;
            if (parent.empty()) {
                break;
            }
            cursor = doc.indexOf(parent);
            if (cursor == i) {
                reader.fail("parent chain is a cycle");
                return false;
            }
        }
    }

    // The comments, last, because attaching them needs the parts counted. The
    // scanner finds `[[part]]` lines and the TOML parser finds part tables; a
    // file can express one without the other (an inline `part = [{...}]` array
    // has no header lines at all), so a disagreement means this document is one
    // whose trivia cannot be placed rather than one to guess at.
    SourceTrivia trivia = scanTrivia(text, length);
    doc.header = std::move(trivia.header);
    doc.buildLeading = std::move(trivia.buildLeading);
    doc.trailer = std::move(trivia.trailer);
    doc.hasUnplaceableComments = trivia.unplaceable;
    if (trivia.partLeading.size() == doc.parts.size()) {
        for (std::size_t i = 0; i < doc.parts.size(); ++i) {
            doc.parts[i].leading = std::move(trivia.partLeading[i]);
        }
    } else if (!doc.parts.empty()) {
        doc.hasUnplaceableComments = true;
    }

    out = std::move(doc);
    return true;
}

std::string writeForge(const ForgeDoc& doc)
{
    // ⚑ Nothing is invented here, including a header for a document that has
    // none. The writer is faithful or it is nothing: the moment it adds a line
    // of its own, `writeForge(parseForge(f)) == f` stops holding, and that
    // equality is the whole guarantee stage E rests on. Seeding a new file with
    // a header is the tool's business, not this function's.
    std::string out;
    out += doc.header;
    if (!doc.name.empty()) {
        out += "name = \"" + doc.name + "\"\n";
    }

    const ForgeBuildOptions defaults;
    if (doc.build.weld != defaults.weld || doc.build.optimize != defaults.optimize ||
        doc.build.smoothAngleDegrees != defaults.smoothAngleDegrees) {
        out += separatorFor(doc.buildLeading, out);
        out += "[build]\n";
        if (doc.build.weld != defaults.weld) {
            out += std::string("weld = ") + (doc.build.weld ? "true" : "false") + "\n";
        }
        if (doc.build.optimize != defaults.optimize) {
            out += std::string("optimize = ") + (doc.build.optimize ? "true" : "false") + "\n";
        }
        if (doc.build.smoothAngleDegrees != defaults.smoothAngleDegrees) {
            out += "smooth_angle = ";
            appendNumber(out, doc.build.smoothAngleDegrees);
            out += "\n";
        }
    }

    for (const ForgePart& part : doc.parts) {
        out += separatorFor(part.leading, out);
        out += "[[part]]\n";
        out += "id = \"" + part.id + "\"\n";
        out += std::string("type = \"") + forgePrimitiveName(part.primitive) + "\"\n";
        if (!part.parent.empty()) {
            out += "parent = \"" + part.parent + "\"\n";
        }
        if (part.position.x != 0.0 || part.position.y != 0.0 || part.position.z != 0.0) {
            out += "position = ";
            appendVec(out, part.position);
            out += "\n";
        }
        if (part.rotationDegrees.x != 0.0 || part.rotationDegrees.y != 0.0 ||
            part.rotationDegrees.z != 0.0) {
            out += "rotation = ";
            appendVec(out, part.rotationDegrees);
            out += "\n";
        }
        if (part.scale.x != 1.0 || part.scale.y != 1.0 || part.scale.z != 1.0) {
            out += "scale = ";
            appendVec(out, part.scale);
            out += "\n";
        }

        // ⚑ What was AUTHORED is written, including a value that happens to
        // equal the schema default. `params` holds exactly the keys the file
        // carried - the editor reads through `value()` and only calls `set()`
        // on a real edit - so "present" means a person typed it.
        //
        // This used to skip anything equal to its default, which deleted four
        // real lines across the six shipped assets on any save: gate's
        // `segments_u = 32`, station's `segments_v = 12`, ship's `uv2` and
        // cockpit's `width = 0.1`. It is the same lossy rewrite the comments
        // were, one field further in - an author who writes the segment count
        // down is saying "this is the knob", and a tool that answers by
        // deleting the line is not one they will trust with the file.
        for (const ForgeParamSpec& spec : forgeParams(part.primitive)) {
            const ForgeValue* authored = part.find(spec.name);
            if (authored == nullptr) {
                continue;
            }
            out += std::string(spec.name) + " = ";
            switch (spec.kind) {
            case ForgeParamKind::Scalar:
                appendNumber(out, authored->scalar);
                break;
            case ForgeParamKind::Integer:
                out += std::to_string(static_cast<std::int64_t>(authored->scalar));
                break;
            case ForgeParamKind::Boolean:
                out += authored->scalar != 0.0 ? "true" : "false";
                break;
            case ForgeParamKind::Vec3:
                appendVec(out, authored->vec);
                break;
            case ForgeParamKind::Uv:
                out += "[";
                appendNumber(out, authored->uv.u);
                out += ", ";
                appendNumber(out, authored->uv.v);
                out += "]";
                break;
            case ForgeParamKind::Profile:
                out += "[";
                for (std::size_t i = 0; i < authored->profile.size(); ++i) {
                    if (i != 0) {
                        out += ", ";
                    }
                    out += "[";
                    appendNumber(out, authored->profile[i].x);
                    out += ", ";
                    appendNumber(out, authored->profile[i].y);
                    out += "]";
                }
                out += "]";
                break;
            // ⚑ These two are the only parameters written across several lines,
            // and the reason is the diff. A baked part is hundreds of vertices;
            // on one line, moving one of them rewrites the single longest line
            // in the file and a reviewer sees a wall. One vertex per line makes
            // that edit one line, which is the entire argument for a text
            // format over the base64 glTF this replaces.
            case ForgeParamKind::VertexList:
                out += "[\n";
                for (const ForgeVertex& vertex : authored->vertices) {
                    out += "  [";
                    const double numbers[8] = {vertex.position.x, vertex.position.y,
                                               vertex.position.z, vertex.normal.x,
                                               vertex.normal.y,   vertex.normal.z,
                                               vertex.uv.u,       vertex.uv.v};
                    for (std::size_t i = 0; i < std::size(numbers); ++i) {
                        if (i != 0) {
                            out += ", ";
                        }
                        appendMeshNumber(out, numbers[i]);
                    }
                    out += "],\n";
                }
                out += "]";
                break;
            case ForgeParamKind::IndexList:
                out += "[\n";
                for (std::size_t i = 0; i < authored->indices.size(); ++i) {
                    // One triangle per line, for the same reason.
                    out += (i % 3 == 0) ? "  " : ", ";
                    out += std::to_string(authored->indices[i]);
                    if (i % 3 == 2) {
                        out += ",\n";
                    }
                }
                if (authored->indices.size() % 3 != 0) {
                    out += "\n";
                }
                out += "]";
                break;
            }
            out += "\n";
        }
    }
    out += doc.trailer;
    return out;
}

BuildTransform forgeWorldTransform(const ForgeDoc& doc, std::size_t partIndex)
{
    if (partIndex >= doc.parts.size()) {
        return {};
    }
    return worldTransforms(doc)[partIndex];
}

ForgePart forgeBakePart(const std::string& id, const MeshData& mesh)
{
    ForgePart part;
    part.id = id;
    part.primitive = ForgePrimitive::Mesh;

    ForgeValue vertices;
    vertices.vertices.reserve(mesh.vertices.size());
    for (const MeshVertex& vertex : mesh.vertices) {
        vertices.vertices.push_back(
            {{vertex.position[0], vertex.position[1], vertex.position[2]},
             {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
             {vertex.uv[0], vertex.uv[1]}});
    }
    part.set("vertices", vertices);

    ForgeValue indices;
    indices.indices = mesh.indices;
    part.set("indices", indices);
    return part;
}

bool buildForge(const ForgeDoc& doc, MeshData& out, std::string* error,
                std::vector<ForgePartRange>* ranges)
{
    const std::vector<BuildTransform> world = worldTransforms(doc);
    if (ranges != nullptr) {
        ranges->clear();
    }

    MeshBuilder builder;
    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        const ForgePart& part = doc.parts[i];
        if (part.primitive == ForgePrimitive::Group) {
            continue;
        }
        const std::uint32_t firstVertex = builder.vertexCount();
        builder.setTransform(world[i]);

        switch (part.primitive) {
        case ForgePrimitive::Group:
            break;
        case ForgePrimitive::Box:
            builder.addBox(part.value("center").vec, part.value("size").vec,
                           part.value("tile").scalar);
            break;
        case ForgePrimitive::Beam:
            builder.addBeam(part.value("from").vec, part.value("to").vec,
                            part.value("width").scalar, part.value("height").scalar,
                            part.value("tile").scalar);
            break;
        case ForgePrimitive::Torus: {
            const auto segU = static_cast<std::uint32_t>(part.value("segments_u").scalar);
            const auto segV = static_cast<std::uint32_t>(part.value("segments_v").scalar);
            if (segU < 3 || segV < 3) {
                if (error != nullptr) {
                    *error = "part '" + part.id + "': a torus needs at least 3 segments each way";
                }
                return false;
            }
            builder.addTorus(part.value("major_radius").scalar, part.value("tube_radius").scalar,
                             segU, segV, part.value("u_tiles").scalar);
            break;
        }
        case ForgePrimitive::FlatTriangle:
            builder.addFlatTriangle(part.value("p0").vec, part.value("p1").vec,
                                    part.value("p2").vec, part.value("uv0").uv,
                                    part.value("uv1").uv, part.value("uv2").uv);
            break;
        case ForgePrimitive::Revolve: {
            const ForgeValue profile = part.value("profile");
            const auto segments = static_cast<std::uint32_t>(part.value("segments").scalar);
            if (profile.profile.size() < 2 || segments < 3) {
                if (error != nullptr) {
                    *error = "part '" + part.id +
                             "': a revolve needs at least 2 profile points and 3 segments";
                }
                return false;
            }
            builder.addRevolve(profile.profile, segments, part.value("u_tiles").scalar,
                               part.value("cap_ends").scalar != 0.0);
            break;
        }
        case ForgePrimitive::Extrude: {
            const ForgeValue outline = part.value("outline");
            if (outline.profile.size() < 3) {
                if (error != nullptr) {
                    *error = "part '" + part.id + "': an extrude needs at least 3 outline points";
                }
                return false;
            }
            builder.addExtrude(outline.profile, part.value("from").vec, part.value("to").vec,
                               part.value("tile").scalar, part.value("cap_ends").scalar != 0.0);
            break;
        }
        case ForgePrimitive::Mesh: {
            // find() rather than value(): a baked part's parameter is hundreds
            // of vertices and value() returns a copy of it.
            const ForgeValue* vertices = part.find("vertices");
            const ForgeValue* indices = part.find("indices");
            if (vertices == nullptr || indices == nullptr || vertices->vertices.empty() ||
                indices->indices.empty()) {
                if (error != nullptr) {
                    *error = "part '" + part.id + "': a mesh part needs vertices and indices";
                }
                return false;
            }
            if (indices->indices.size() % 3 != 0) {
                if (error != nullptr) {
                    *error = "part '" + part.id + "': indices must come in threes";
                }
                return false;
            }
            const auto count = static_cast<std::uint32_t>(vertices->vertices.size());
            for (const std::uint32_t index : indices->indices) {
                if (index >= count) {
                    if (error != nullptr) {
                        *error = "part '" + part.id + "': index " + std::to_string(index) +
                                 " is past the part's " + std::to_string(count) + " vertices";
                    }
                    return false;
                }
            }
            // ⚑ Indices are part-local and the builder's are document-wide, so
            // they are rebased. Anything else would make a baked part's meaning
            // depend on what happened to be emitted before it.
            const std::uint32_t base = builder.vertexCount();
            for (const ForgeVertex& vertex : vertices->vertices) {
                builder.addVertex(vertex.position, vertex.normal, vertex.uv);
            }
            for (std::size_t t = 0; t + 2 < indices->indices.size(); t += 3) {
                builder.addTriangle(base + indices->indices[t], base + indices->indices[t + 1],
                                    base + indices->indices[t + 2]);
            }
            break;
        }
        }

        if (ranges != nullptr) {
            ranges->push_back({i, firstVertex, builder.vertexCount() - firstVertex});
        }
    }

    MeshData data = builder.build();
    const bool needsEdit = doc.build.weld || doc.build.optimize || doc.build.smoothAngleDegrees > 0.0;
    if (needsEdit) {
        EditMesh mesh = toEditMesh(data);
        if (doc.build.smoothAngleDegrees > 0.0) {
            recomputeNormals(mesh, static_cast<float>(doc.build.smoothAngleDegrees));
        }
        if (doc.build.optimize) {
            optimizeIndices(mesh);
        }
        data = toMeshData(mesh);
        // ⚑ The post-pass merged and renumbered vertices, so the ranges just
        // collected describe a mesh that no longer exists. Dropping them is the
        // honest answer; handing them back would be a mapping that is wrong in
        // exactly the case a caller cannot check.
        if (ranges != nullptr) {
            ranges->clear();
        }
    }

    out = std::move(data);
    return true;
}

bool forgeHasVertexAttribution(const ForgeDoc& doc)
{
    return !doc.build.weld && !doc.build.optimize && doc.build.smoothAngleDegrees <= 0.0;
}

bool forgePoints(const ForgeDoc& doc, std::vector<ForgePoint>& out, std::string* error,
                 double tolerance)
{
    out.clear();
    if (!forgeHasVertexAttribution(doc)) {
        if (error != nullptr) {
            *error = "this document's [build] table welds or reorders vertices, so a built vertex "
                     "no longer names the part that emitted it";
        }
        return false;
    }

    MeshData mesh;
    std::vector<ForgePartRange> ranges;
    if (!buildForge(doc, mesh, error, &ranges)) {
        return false;
    }

    // Which part each built vertex came from. One pass over the ranges rather
    // than a search per vertex: the asteroid is 1291 vertices and this runs on
    // every rebuild.
    std::vector<std::size_t> ownerOf(mesh.vertices.size(), doc.parts.size());
    std::vector<std::uint32_t> localOf(mesh.vertices.size(), 0);
    for (const ForgePartRange& range : ranges) {
        for (std::uint32_t i = 0; i < range.vertexCount; ++i) {
            const std::size_t vertex = range.firstVertex + i;
            if (vertex < ownerOf.size()) {
                ownerOf[vertex] = range.part;
                localOf[vertex] = i;
            }
        }
    }

    const double toleranceSquared = tolerance * tolerance;
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        const MeshVertex& vertex = mesh.vertices[v];
        const BuildPoint position{vertex.position[0], vertex.position[1], vertex.position[2]};

        // Linear over the points found so far. The meshes this tool authors are
        // 16-1291 vertices and this is not the hot path; a grid would be the
        // answer if a future asset made it one.
        ForgePoint* found = nullptr;
        for (ForgePoint& candidate : out) {
            const double dx = candidate.position.x - position.x;
            const double dy = candidate.position.y - position.y;
            const double dz = candidate.position.z - position.z;
            if ((dx * dx) + (dy * dy) + (dz * dz) <= toleranceSquared) {
                found = &candidate;
                break;
            }
        }
        if (found == nullptr) {
            out.push_back({position, {}, 0});
            found = &out.back();
        }
        ++found->corners;

        const std::size_t owner = ownerOf[v];
        if (owner >= doc.parts.size()) {
            continue; // no range covers this vertex: leaves the point unmovable
        }
        const ForgePart& part = doc.parts[owner];
        // ⚑ The three classes stage E's spec measured, and only the first two
        // lines of this switch are the whole of class (1). A flat triangle emits
        // p0, p1, p2 in that order and nothing else; a baked part emits its
        // vertex list in order. Every other primitive computes its corners from
        // parameters that are not corners, so there is no name to write and the
        // point stays unmovable until the part is baked.
        switch (part.primitive) {
        case ForgePrimitive::FlatTriangle: {
            static constexpr const char* kCorners[3] = {"p0", "p1", "p2"};
            const std::uint32_t local = localOf[v];
            if (local < 3) {
                found->writes.push_back({owner, kCorners[local], 0});
            }
            break;
        }
        case ForgePrimitive::Mesh:
            found->writes.push_back({owner, "vertices", localOf[v]});
            break;
        case ForgePrimitive::Group:
        case ForgePrimitive::Box:
        case ForgePrimitive::Beam:
        case ForgePrimitive::Torus:
        case ForgePrimitive::Revolve:
        case ForgePrimitive::Extrude:
            break;
        }
    }
    return true;
}

bool forgeMovePoint(ForgeDoc& doc, const ForgePoint& point, BuildPoint delta, std::string* error)
{
    if (!point.movable()) {
        if (error != nullptr) {
            *error = "this point has a corner with no parametric answer - bake its part first";
        }
        return false;
    }

    // ⚑ Every inverse is resolved BEFORE anything is written. A part with a
    // singular world transform has to abort the whole move, not half of it: a
    // hull with three of its five corners updated is a seam, which is the exact
    // defect this function exists to prevent.
    const std::vector<BuildTransform> world = worldTransforms(doc);
    std::vector<BuildPoint> localDelta(point.writes.size());
    for (std::size_t i = 0; i < point.writes.size(); ++i) {
        const std::size_t part = point.writes[i].part;
        if (part >= doc.parts.size()) {
            if (error != nullptr) {
                *error = "a write names a part this document no longer has";
            }
            return false;
        }
        BuildTransform inverse;
        if (!world[part].inverse(inverse)) {
            if (error != nullptr) {
                *error = "part '" + doc.parts[part].id +
                         "' is scaled flat, so there is no frame to write this point back into";
            }
            return false;
        }
        // ⚑ transformDirection, not transformPoint: a delta is a displacement
        // and must not pick up the part's translation. Using the point form
        // here would throw a child part across the frame by its parent's
        // offset on the first drag - and a zero drag would not be a no-op.
        localDelta[i] = inverse.transformDirection(delta);
    }

    for (std::size_t i = 0; i < point.writes.size(); ++i) {
        const ForgePointWrite& write = point.writes[i];
        ForgePart& part = doc.parts[write.part];
        if (write.param == "vertices") {
            ForgeValue vertices = part.value("vertices");
            if (write.element >= vertices.vertices.size()) {
                if (error != nullptr) {
                    *error = "part '" + part.id + "' no longer has that baked vertex";
                }
                return false;
            }
            BuildPoint& position = vertices.vertices[write.element].position;
            position = {position.x + localDelta[i].x, position.y + localDelta[i].y,
                        position.z + localDelta[i].z};
            part.set("vertices", vertices);
        } else {
            // ⚑ The value ALREADY IN THE DOCUMENT is what the delta is added
            // to, so an unmoved point writes back the author's own number
            // rather than a float round trip of it.
            ForgeValue value = part.value(write.param.c_str());
            value.vec = {value.vec.x + localDelta[i].x, value.vec.y + localDelta[i].y,
                         value.vec.z + localDelta[i].z};
            part.set(write.param.c_str(), value);
        }
    }
    return true;
}

} // namespace sol::assets
