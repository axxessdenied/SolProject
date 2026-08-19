#include "sol/assets/forge_doc.hpp"

#include "sol/assets/mesh_edit.hpp"
#include "sol/core/math/scalar.hpp"
#include "sol/core/toml.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

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

// Doubles that came from the same literal compare equal, which is all the
// writer's "omit what is still at its default" needs. It is deliberately not a
// tolerance: a parameter a person typed should survive a round trip even when
// it is a hair off the default.
[[nodiscard]] bool sameValue(ForgeParamKind kind, const ForgeValue& a, const ForgeValue& b)
{
    switch (kind) {
    case ForgeParamKind::Scalar:
    case ForgeParamKind::Integer:
    case ForgeParamKind::Boolean:
        return a.scalar == b.scalar;
    case ForgeParamKind::Vec3:
        return a.vec.x == b.vec.x && a.vec.y == b.vec.y && a.vec.z == b.vec.z;
    case ForgeParamKind::Uv:
        return a.uv.u == b.uv.u && a.uv.v == b.uv.v;
    case ForgeParamKind::Profile:
        if (a.profile.size() != b.profile.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.profile.size(); ++i) {
            if (a.profile[i].x != b.profile[i].x || a.profile[i].y != b.profile[i].y) {
                return false;
            }
        }
        return true;
    case ForgeParamKind::VertexList:
        if (a.vertices.size() != b.vertices.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.vertices.size(); ++i) {
            const ForgeVertex& x = a.vertices[i];
            const ForgeVertex& y = b.vertices[i];
            if (x.position.x != y.position.x || x.position.y != y.position.y ||
                x.position.z != y.position.z || x.normal.x != y.normal.x ||
                x.normal.y != y.normal.y || x.normal.z != y.normal.z || x.uv.u != y.uv.u ||
                x.uv.v != y.uv.v) {
                return false;
            }
        }
        return true;
    case ForgeParamKind::IndexList:
        return a.indices == b.indices;
    }
    return false;
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

    out = std::move(doc);
    return true;
}

std::string writeForge(const ForgeDoc& doc)
{
    std::string out;
    out += "# Authored with the Forge (engine plan Phase 9). This file is the\n";
    out += "# SOURCE: the mesh beside it is built from these parts and can be\n";
    out += "# rebuilt from them, which a cooked buffer of triangles cannot.\n";
    if (!doc.name.empty()) {
        out += "name = \"" + doc.name + "\"\n";
    }

    const ForgeBuildOptions defaults;
    if (doc.build.weld != defaults.weld || doc.build.optimize != defaults.optimize ||
        doc.build.smoothAngleDegrees != defaults.smoothAngleDegrees) {
        out += "\n[build]\n";
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
        out += "\n[[part]]\n";
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

        for (const ForgeParamSpec& spec : forgeParams(part.primitive)) {
            const ForgeValue* authored = part.find(spec.name);
            if (authored == nullptr || sameValue(spec.kind, *authored, spec.defaultValue)) {
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

bool buildForge(const ForgeDoc& doc, MeshData& out, std::string* error)
{
    const std::vector<BuildTransform> world = worldTransforms(doc);

    MeshBuilder builder;
    for (std::size_t i = 0; i < doc.parts.size(); ++i) {
        const ForgePart& part = doc.parts[i];
        if (part.primitive == ForgePrimitive::Group) {
            continue;
        }
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
    }

    out = std::move(data);
    return true;
}

} // namespace sol::assets
