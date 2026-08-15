#include "sol/core/toml.hpp"

#include "sol/test/test.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace {

using sol::core::TomlType;
using sol::core::TomlValue;

bool parseToml(const char* text, TomlValue& out, std::string* error = nullptr)
{
    return TomlValue::parse(text, std::strlen(text), out, error);
}

} // namespace

SOL_TEST(toml_scalarsAndComments)
{
    TomlValue root;
    SOL_CHECK(parseToml("# ship definition\n"
                        "name = \"Vulture\" # trailing comment\n"
                        "mass = 12_500\n"
                        "drag = 0.25\n"
                        "military = false\n"
                        "hardpoints = 4\n",
                        root));
    SOL_CHECK(root.find("name")->asString() == "Vulture");
    SOL_CHECK(root.find("mass")->asInteger() == 12500);
    SOL_CHECK(root.find("drag")->asFloat() == 0.25);
    SOL_CHECK(!root.find("military")->asBool(true));
    SOL_CHECK(root.find("hardpoints")->isInteger());
    SOL_CHECK(root.find("hardpoints")->asFloat() == 4.0); // integer coerces to float
    SOL_CHECK(root.find("missing") == nullptr);
}

SOL_TEST(toml_integerFormats)
{
    TomlValue root;
    SOL_CHECK(parseToml("dec = -42\n"
                        "hex = 0xDEAD\n"
                        "oct = 0o755\n"
                        "bin = 0b1011\n"
                        "big = 9223372036854775807\n"
                        "small = -9223372036854775808\n",
                        root));
    SOL_CHECK(root.find("dec")->asInteger() == -42);
    SOL_CHECK(root.find("hex")->asInteger() == 0xDEAD);
    SOL_CHECK(root.find("oct")->asInteger() == 0755);
    SOL_CHECK(root.find("bin")->asInteger() == 11);
    SOL_CHECK(root.find("big")->asInteger() == 9223372036854775807ll);
    SOL_CHECK(root.find("small")->asInteger() == (-9223372036854775807ll - 1));
}

SOL_TEST(toml_floatFormats)
{
    TomlValue root;
    SOL_CHECK(parseToml("plain = 3.14\n"
                        "exponent = 1e6\n"
                        "combined = -2.5e-3\n"
                        "positiveInf = +inf\n"
                        "negativeInf = -inf\n"
                        "notANumber = nan\n",
                        root));
    SOL_CHECK(root.find("plain")->asFloat() == 3.14);
    SOL_CHECK(root.find("exponent")->asFloat() == 1e6);
    SOL_CHECK(root.find("combined")->asFloat() == -2.5e-3);
    SOL_CHECK(std::isinf(root.find("positiveInf")->asFloat()));
    SOL_CHECK(root.find("negativeInf")->asFloat() < 0.0);
    SOL_CHECK(std::isnan(root.find("notANumber")->asFloat()));
}

SOL_TEST(toml_stringsAndEscapes)
{
    TomlValue root;
    SOL_CHECK(parseToml("basic = \"line\\none\\ttab \\\"quoted\\\" back\\\\slash\"\n"
                        "unicode = \"\\u00E9\\U0001F680\"\n"
                        "literal = 'C:\\Users\\no\\escape'\n"
                        "empty = \"\"\n",
                        root));
    SOL_CHECK(root.find("basic")->asString() == "line\none\ttab \"quoted\" back\\slash");
    SOL_CHECK(root.find("unicode")->asString() == "\xC3\xA9\xF0\x9F\x9A\x80");
    SOL_CHECK(root.find("literal")->asString() == "C:\\Users\\no\\escape");
    SOL_CHECK(root.find("empty")->asString().empty());
}

SOL_TEST(toml_arraysIncludingMultiline)
{
    TomlValue root;
    SOL_CHECK(parseToml("sizes = [1, 2, 3]\n"
                        "mixed = [\"a\", 5, 1.5, true]\n"
                        "nested = [[1, 2], [3]]\n"
                        "spread = [\n"
                        "  10, # first\n"
                        "  20,\n"
                        "  30, # trailing comma next\n"
                        "]\n",
                        root));
    const TomlValue& sizes = *root.find("sizes");
    SOL_CHECK(sizes.size() == 3 && sizes[2].asInteger() == 3);
    const TomlValue& mixed = *root.find("mixed");
    SOL_CHECK(mixed[0].asString() == "a" && mixed[1].asInteger() == 5 &&
              mixed[2].asFloat() == 1.5 && mixed[3].asBool());
    SOL_CHECK((*root.find("nested"))[0].size() == 2);
    const TomlValue& spread = *root.find("spread");
    SOL_CHECK(spread.size() == 3 && spread[1].asInteger() == 20);
}

SOL_TEST(toml_tablesDottedKeysAndInlineTables)
{
    TomlValue root;
    SOL_CHECK(parseToml("title = \"root\"\n"
                        "physics.gravity = 9.8\n"
                        "physics.substeps = 4\n"
                        "[ship]\n"
                        "name = \"Asp\"\n"
                        "[ship.hull]\n"
                        "mass = 280\n"
                        "point = { x = 1, y = -2 }\n",
                        root));
    SOL_CHECK(root.findPath("physics.gravity")->asFloat() == 9.8);
    SOL_CHECK(root.findPath("ship.name")->asString() == "Asp");
    SOL_CHECK(root.findPath("ship.hull.mass")->asInteger() == 280);
    SOL_CHECK(root.findPath("ship.hull.point.x")->asInteger() == 1);
    SOL_CHECK(root.findPath("ship.hull.point.y")->asInteger() == -2);
    SOL_CHECK(root.findPath("ship.missing.deep") == nullptr);
}

SOL_TEST(toml_arrayOfTables)
{
    TomlValue root;
    SOL_CHECK(parseToml("[[weapon]]\n"
                        "id = \"pulse\"\n"
                        "damage = 12\n"
                        "[[weapon]]\n"
                        "id = \"beam\"\n"
                        "damage = 30\n"
                        "[weapon.spread]\n"
                        "arc = 5\n",
                        root));
    const TomlValue& weapons = *root.find("weapon");
    SOL_CHECK(weapons.isArray() && weapons.size() == 2);
    SOL_CHECK(weapons[0].find("id")->asString() == "pulse");
    SOL_CHECK(weapons[1].find("damage")->asInteger() == 30);
    // [weapon.spread] attaches to the most recent [[weapon]] element.
    SOL_CHECK(weapons[1].findPath("spread.arc")->asInteger() == 5);
    SOL_CHECK(weapons[0].find("spread") == nullptr);
}

SOL_TEST(toml_quotedKeys)
{
    TomlValue root;
    SOL_CHECK(parseToml("\"key with space\" = 1\n"
                        "'literal.key' = 2\n",
                        root));
    SOL_CHECK(root.find("key with space")->asInteger() == 1);
    SOL_CHECK(root.find("literal.key")->asInteger() == 2); // not a dotted path
}

SOL_TEST(toml_errorsCarryLineNumbers)
{
    TomlValue root;
    std::string error;

    SOL_CHECK(!parseToml("ok = 1\ndup = 2\ndup = 3\n", root, &error));
    SOL_CHECK(error.find("line 3") != std::string::npos);
    SOL_CHECK(error.find("duplicate") != std::string::npos);

    SOL_CHECK(!parseToml("bad = \"unterminated\n", root, &error));
    SOL_CHECK(error.find("line 1") != std::string::npos);

    SOL_CHECK(!parseToml("date = 1979-05-27\n", root, &error));
    SOL_CHECK(error.find("not supported") != std::string::npos);

    SOL_CHECK(!parseToml("time = 07:32:00\n", root, &error));
    SOL_CHECK(error.find("not supported") != std::string::npos);

    SOL_CHECK(!parseToml("multi = \"\"\"x\"\"\"\n", root, &error));
    SOL_CHECK(error.find("multi-line") != std::string::npos);

    SOL_CHECK(!parseToml("x = 1 y = 2\n", root, &error));
    SOL_CHECK(!parseToml("x = 1__0\n", root, &error));
    SOL_CHECK(!parseToml("x = _10\n", root, &error));
    SOL_CHECK(!parseToml("[table\nx = 1\n", root, &error));
    SOL_CHECK(!parseToml("x = 99999999999999999999\n", root, &error)); // > int64
}

SOL_TEST(toml_conflictingDefinitionsFail)
{
    TomlValue root;
    std::string error;
    SOL_CHECK(!parseToml("x = 1\n[x]\ny = 2\n", root, &error));
    SOL_CHECK(!parseToml("x = 1\n[[x]]\ny = 2\n", root, &error));
    SOL_CHECK(!parseToml("x.y = 1\nx.y.z = 2\n", root, &error));
}
