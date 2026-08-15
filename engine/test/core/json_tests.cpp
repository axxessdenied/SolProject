#include "sol/core/json.hpp"

#include "sol/test/test.hpp"

#include <cstring>

using sol::core::JsonType;
using sol::core::JsonValue;

namespace {

bool parseOk(const char* text, JsonValue& out)
{
    return JsonValue::parse(text, std::strlen(text), out);
}

bool parseFails(const char* text)
{
    JsonValue value;
    return !JsonValue::parse(text, std::strlen(text), value);
}

} // namespace

SOL_TEST(jsonParsesScalars)
{
    JsonValue v;
    SOL_CHECK(parseOk("42", v) && v.isNumber() && v.asNumber() == 42.0);
    SOL_CHECK(parseOk("-1.5e2", v) && v.asNumber() == -150.0);
    SOL_CHECK(parseOk("true", v) && v.isBool() && v.asBool());
    SOL_CHECK(parseOk("false", v) && !v.asBool(true));
    SOL_CHECK(parseOk("null", v) && v.isNull());
    SOL_CHECK(parseOk("\"hi\"", v) && v.isString() && v.asString() == "hi");
}

SOL_TEST(jsonParsesNestedDocument)
{
    const char* doc = R"({
        "meshes": [ {"name": "cube", "primitives": [{"indices": 3, "attributes": {"POSITION": 0}}]} ],
        "asset": {"version": "2.0"},
        "count": 12,
        "flags": [true, false, null]
    })";
    JsonValue v;
    SOL_CHECK(parseOk(doc, v));
    SOL_CHECK(v.isObject() && v.size() == 4);

    const JsonValue* meshes = v.find("meshes");
    SOL_CHECK(meshes != nullptr && meshes->isArray() && meshes->size() == 1);
    const JsonValue* name = (*meshes)[0].find("name");
    SOL_CHECK(name != nullptr && name->asString() == "cube");

    const JsonValue* attributes = (*(*meshes)[0].find("primitives"))[0].find("attributes");
    SOL_CHECK(attributes != nullptr && attributes->find("POSITION")->asNumber() == 0.0);

    SOL_CHECK(v.find("count")->asNumber() == 12.0);
    SOL_CHECK(v.find("missing") == nullptr);
    SOL_CHECK((*v.find("flags"))[2].isNull());
}

SOL_TEST(jsonHandlesStringEscapes)
{
    JsonValue v;
    SOL_CHECK(parseOk(R"("a\"b\\c\nd\t")", v));
    SOL_CHECK(v.asString() == "a\"b\\c\nd\t");
    // é = e-acute (2-byte UTF-8), surrogate pair = U+1F600 (4-byte UTF-8)
    SOL_CHECK(parseOk(R"("é")", v));
    SOL_CHECK(v.asString() == "\xC3\xA9");
    SOL_CHECK(parseOk(R"("😀")", v));
    SOL_CHECK(v.asString() == "\xF0\x9F\x98\x80");
}

SOL_TEST(jsonRejectsMalformedInput)
{
    SOL_CHECK(parseFails(""));
    SOL_CHECK(parseFails("{"));
    SOL_CHECK(parseFails("[1,]"));
    SOL_CHECK(parseFails("{\"a\":}"));
    SOL_CHECK(parseFails("{\"a\":1,}"));
    SOL_CHECK(parseFails("tru"));
    SOL_CHECK(parseFails("1 2"));
    SOL_CHECK(parseFails("\"unterminated"));
    SOL_CHECK(parseFails("{\"a\" 1}"));
}
