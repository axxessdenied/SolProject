#include "gltf.hpp"

#include "sol/core/json.hpp"
#include "sol/core/log.hpp"
#include "sol/core/math/math.hpp"
#include "sol/platform/file_io.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sol::cooker {

using core::JsonValue;
using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

// glTF componentType constants
constexpr int kComponentU16 = 5123;
constexpr int kComponentU32 = 5125;
constexpr int kComponentF32 = 5126;

struct BufferSet
{
    std::vector<std::vector<std::uint8_t>> buffers;
};

struct AccessorView
{
    const std::uint8_t* data = nullptr;
    std::size_t count = 0;
    std::size_t stride = 0; // bytes between elements
    int componentType = 0;
    int componentsPerElement = 0;
};

int componentCountForType(const std::string& type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

std::size_t componentByteSize(int componentType)
{
    switch (componentType) {
    case kComponentU16: return 2;
    case kComponentU32:
    case kComponentF32: return 4;
    case 5120:
    case 5121: return 1; // i8/u8
    case 5122: return 2; // i16
    default: return 0;
    }
}

bool resolveAccessor(const JsonValue& document, const BufferSet& buffers, std::size_t accessorIndex,
                     AccessorView& out)
{
    const JsonValue* accessors = document.find("accessors");
    const JsonValue* bufferViews = document.find("bufferViews");
    if (accessors == nullptr || bufferViews == nullptr || accessorIndex >= accessors->size()) {
        return false;
    }
    const JsonValue& accessor = (*accessors)[accessorIndex];

    const JsonValue* viewIndexValue = accessor.find("bufferView");
    const JsonValue* countValue = accessor.find("count");
    const JsonValue* typeValue = accessor.find("type");
    const JsonValue* componentValue = accessor.find("componentType");
    if (viewIndexValue == nullptr || countValue == nullptr || typeValue == nullptr ||
        componentValue == nullptr) {
        return false; // sparse accessors unsupported in v0
    }

    const std::size_t viewIndex = static_cast<std::size_t>(viewIndexValue->asNumber());
    if (viewIndex >= bufferViews->size()) {
        return false;
    }
    const JsonValue& view = (*bufferViews)[viewIndex];

    const std::size_t bufferIndex =
        static_cast<std::size_t>(view.find("buffer") != nullptr ? view.find("buffer")->asNumber() : 0);
    if (bufferIndex >= buffers.buffers.size()) {
        return false;
    }
    const std::vector<std::uint8_t>& buffer = buffers.buffers[bufferIndex];

    const std::size_t viewOffset =
        view.find("byteOffset") != nullptr ? static_cast<std::size_t>(view.find("byteOffset")->asNumber())
                                           : 0;
    const std::size_t accessorOffset =
        accessor.find("byteOffset") != nullptr
            ? static_cast<std::size_t>(accessor.find("byteOffset")->asNumber())
            : 0;

    out.componentType = static_cast<int>(componentValue->asNumber());
    out.componentsPerElement = componentCountForType(typeValue->asString());
    out.count = static_cast<std::size_t>(countValue->asNumber());

    const std::size_t elementSize =
        componentByteSize(out.componentType) * static_cast<std::size_t>(out.componentsPerElement);
    if (elementSize == 0 || out.componentsPerElement == 0) {
        return false;
    }
    out.stride = view.find("byteStride") != nullptr
                     ? static_cast<std::size_t>(view.find("byteStride")->asNumber())
                     : elementSize;

    const std::size_t start = viewOffset + accessorOffset;
    const std::size_t end = start + (out.count > 0 ? (out.count - 1) * out.stride + elementSize : 0);
    if (end > buffer.size()) {
        return false;
    }
    out.data = buffer.data() + start;
    return true;
}

Mat4 nodeLocalTransform(const JsonValue& node)
{
    if (const JsonValue* matrix = node.find("matrix"); matrix != nullptr && matrix->size() == 16) {
        Mat4 m;
        for (std::size_t i = 0; i < 16; ++i) {
            m.m[i] = static_cast<float>((*matrix)[i].asNumber());
        }
        return m;
    }

    Vec3 translationVec = {};
    Quat rotation = Quat::identity();
    Vec3 scaleVec = {1.0f, 1.0f, 1.0f};

    if (const JsonValue* t = node.find("translation"); t != nullptr && t->size() == 3) {
        translationVec = {static_cast<float>((*t)[0].asNumber()), static_cast<float>((*t)[1].asNumber()),
                          static_cast<float>((*t)[2].asNumber())};
    }
    if (const JsonValue* r = node.find("rotation"); r != nullptr && r->size() == 4) {
        rotation = {static_cast<float>((*r)[0].asNumber()), static_cast<float>((*r)[1].asNumber()),
                    static_cast<float>((*r)[2].asNumber()), static_cast<float>((*r)[3].asNumber())};
    }
    if (const JsonValue* s = node.find("scale"); s != nullptr && s->size() == 3) {
        scaleVec = {static_cast<float>((*s)[0].asNumber()), static_cast<float>((*s)[1].asNumber()),
                    static_cast<float>((*s)[2].asNumber())};
    }
    return core::translation(translationVec) * toMat4(rotation) * core::scale(scaleVec);
}

bool appendPrimitive(const JsonValue& document, const BufferSet& buffers, const JsonValue& primitive,
                     const Mat4& transform, assets::MeshData& out)
{
    if (const JsonValue* mode = primitive.find("mode");
        mode != nullptr && static_cast<int>(mode->asNumber()) != 4) {
        SOL_LOG_WARN("gltf: skipping non-triangle primitive");
        return true;
    }

    const JsonValue* attributes = primitive.find("attributes");
    if (attributes == nullptr) {
        return false;
    }
    const JsonValue* positionIndex = attributes->find("POSITION");
    if (positionIndex == nullptr) {
        return false;
    }

    AccessorView positions;
    if (!resolveAccessor(document, buffers, static_cast<std::size_t>(positionIndex->asNumber()),
                         positions) ||
        positions.componentType != kComponentF32 || positions.componentsPerElement != 3) {
        return false;
    }

    AccessorView normals = {};
    bool hasNormals = false;
    if (const JsonValue* normalIndex = attributes->find("NORMAL"); normalIndex != nullptr) {
        hasNormals = resolveAccessor(document, buffers,
                                     static_cast<std::size_t>(normalIndex->asNumber()), normals) &&
                     normals.componentType == kComponentF32 && normals.componentsPerElement == 3 &&
                     normals.count == positions.count;
    }

    AccessorView uvs = {};
    bool hasUvs = false;
    if (const JsonValue* uvIndex = attributes->find("TEXCOORD_0"); uvIndex != nullptr) {
        hasUvs = resolveAccessor(document, buffers, static_cast<std::size_t>(uvIndex->asNumber()), uvs) &&
                 uvs.componentType == kComponentF32 && uvs.componentsPerElement == 2 &&
                 uvs.count == positions.count;
    }

    const std::uint32_t baseVertex = static_cast<std::uint32_t>(out.vertices.size());

    for (std::size_t i = 0; i < positions.count; ++i) {
        float position[3];
        std::memcpy(position, positions.data + i * positions.stride, sizeof(position));
        float normal[3] = {0.0f, 0.0f, 1.0f};
        if (hasNormals) {
            std::memcpy(normal, normals.data + i * normals.stride, sizeof(normal));
        }
        float uv[2] = {0.0f, 0.0f};
        if (hasUvs) {
            std::memcpy(uv, uvs.data + i * uvs.stride, sizeof(uv));
        }

        const Vec3 worldPosition = transformPoint(transform, {position[0], position[1], position[2]});
        // Rotation part only; assumes no non-uniform scale on normals (v0).
        const Vec3 worldNormal =
            normalize(transformDirection(transform, {normal[0], normal[1], normal[2]}));

        assets::MeshVertex vertex = {};
        vertex.position[0] = worldPosition.x;
        vertex.position[1] = worldPosition.y;
        vertex.position[2] = worldPosition.z;
        vertex.normal[0] = worldNormal.x;
        vertex.normal[1] = worldNormal.y;
        vertex.normal[2] = worldNormal.z;
        vertex.uv[0] = uv[0];
        vertex.uv[1] = uv[1];
        out.vertices.push_back(vertex);
    }

    if (const JsonValue* indicesIndex = primitive.find("indices"); indicesIndex != nullptr) {
        AccessorView indices;
        if (!resolveAccessor(document, buffers, static_cast<std::size_t>(indicesIndex->asNumber()),
                             indices) ||
            indices.componentsPerElement != 1) {
            return false;
        }
        for (std::size_t i = 0; i < indices.count; ++i) {
            std::uint32_t index = 0;
            if (indices.componentType == kComponentU16) {
                std::uint16_t value = 0;
                std::memcpy(&value, indices.data + i * indices.stride, sizeof(value));
                index = value;
            } else if (indices.componentType == kComponentU32) {
                std::memcpy(&index, indices.data + i * indices.stride, sizeof(index));
            } else {
                return false;
            }
            out.indices.push_back(baseVertex + index);
        }
    } else {
        for (std::size_t i = 0; i < positions.count; ++i) {
            out.indices.push_back(baseVertex + static_cast<std::uint32_t>(i));
        }
    }
    return true;
}

bool traverseNode(const JsonValue& document, const BufferSet& buffers, std::size_t nodeIndex,
                  const Mat4& parentTransform, assets::MeshData& out)
{
    const JsonValue* nodes = document.find("nodes");
    if (nodes == nullptr || nodeIndex >= nodes->size()) {
        return false;
    }
    const JsonValue& node = (*nodes)[nodeIndex];
    const Mat4 transform = parentTransform * nodeLocalTransform(node);

    if (const JsonValue* meshIndex = node.find("mesh"); meshIndex != nullptr) {
        const JsonValue* meshes = document.find("meshes");
        const std::size_t index = static_cast<std::size_t>(meshIndex->asNumber());
        if (meshes == nullptr || index >= meshes->size()) {
            return false;
        }
        const JsonValue* primitives = (*meshes)[index].find("primitives");
        if (primitives != nullptr) {
            for (std::size_t p = 0; p < primitives->size(); ++p) {
                if (!appendPrimitive(document, buffers, (*primitives)[p], transform, out)) {
                    return false;
                }
            }
        }
    }

    if (const JsonValue* children = node.find("children"); children != nullptr) {
        for (std::size_t c = 0; c < children->size(); ++c) {
            if (!traverseNode(document, buffers,
                              static_cast<std::size_t>((*children)[c].asNumber()), transform, out)) {
                return false;
            }
        }
    }
    return true;
}

std::string directoryOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool loadBuffers(const JsonValue& document, const std::string& gltfDirectory,
                 std::vector<std::uint8_t> glbBinary, BufferSet& out)
{
    const JsonValue* buffers = document.find("buffers");
    if (buffers == nullptr) {
        return true; // meshes without buffers will fail accessor resolution later
    }
    for (std::size_t i = 0; i < buffers->size(); ++i) {
        const JsonValue* uri = (*buffers)[i].find("uri");
        if (uri == nullptr) {
            // GLB binary chunk
            if (glbBinary.empty()) {
                SOL_LOG_ERROR("gltf: buffer %zu has no uri and no GLB chunk", i);
                return false;
            }
            out.buffers.push_back(std::move(glbBinary));
            continue;
        }
        const std::string& uriText = uri->asString();
        constexpr const char* kDataPrefix = "data:";
        if (uriText.rfind(kDataPrefix, 0) == 0) {
            const std::size_t comma = uriText.find(',');
            if (comma == std::string::npos ||
                uriText.find(";base64", 0) == std::string::npos) {
                SOL_LOG_ERROR("gltf: unsupported data uri in buffer %zu", i);
                return false;
            }
            std::vector<std::uint8_t> decoded;
            if (!decodeBase64(uriText.c_str() + comma + 1, uriText.size() - comma - 1, decoded)) {
                SOL_LOG_ERROR("gltf: base64 decode failed for buffer %zu", i);
                return false;
            }
            out.buffers.push_back(std::move(decoded));
        } else {
            std::vector<std::uint8_t> fileData;
            const std::string fullPath = gltfDirectory + uriText;
            if (!platform::readFileBytes(fullPath.c_str(), fileData)) {
                SOL_LOG_ERROR("gltf: cannot read buffer file %s", fullPath.c_str());
                return false;
            }
            out.buffers.push_back(std::move(fileData));
        }
    }
    return true;
}

} // namespace

bool decodeBase64(const char* text, std::size_t length, std::vector<std::uint8_t>& out)
{
    auto decodeChar = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    out.clear();
    int accumulator = 0;
    int bits = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
        const int value = decodeChar(c);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

bool importGltf(const char* path, assets::MeshData& out)
{
    std::vector<std::uint8_t> fileData;
    if (!platform::readFileBytes(path, fileData)) {
        SOL_LOG_ERROR("gltf: cannot read %s", path);
        return false;
    }

    std::vector<std::uint8_t> glbBinary;
    const char* jsonText = nullptr;
    std::size_t jsonLength = 0;

    if (fileData.size() >= 12 && std::memcmp(fileData.data(), "glTF", 4) == 0) {
        // GLB container: 12-byte header, then chunks (JSON first, optional BIN).
        std::size_t offset = 12;
        while (offset + 8 <= fileData.size()) {
            std::uint32_t chunkLength = 0;
            std::uint32_t chunkType = 0;
            std::memcpy(&chunkLength, fileData.data() + offset, 4);
            std::memcpy(&chunkType, fileData.data() + offset + 4, 4);
            offset += 8;
            if (offset + chunkLength > fileData.size()) {
                SOL_LOG_ERROR("gltf: truncated GLB chunk");
                return false;
            }
            if (chunkType == 0x4E4F534Au) { // "JSON"
                jsonText = reinterpret_cast<const char*>(fileData.data() + offset);
                jsonLength = chunkLength;
            } else if (chunkType == 0x004E4942u) { // "BIN\0"
                glbBinary.assign(fileData.begin() + static_cast<std::ptrdiff_t>(offset),
                                 fileData.begin() + static_cast<std::ptrdiff_t>(offset + chunkLength));
            }
            offset += chunkLength;
        }
    } else {
        jsonText = reinterpret_cast<const char*>(fileData.data());
        jsonLength = fileData.size();
    }

    if (jsonText == nullptr) {
        SOL_LOG_ERROR("gltf: no JSON chunk in %s", path);
        return false;
    }

    JsonValue document;
    std::string error;
    if (!JsonValue::parse(jsonText, jsonLength, document, &error)) {
        SOL_LOG_ERROR("gltf: JSON error in %s: %s", path, error.c_str());
        return false;
    }

    BufferSet buffers;
    if (!loadBuffers(document, directoryOf(path), std::move(glbBinary), buffers)) {
        return false;
    }

    out.vertices.clear();
    out.indices.clear();

    // Default scene, or scene 0.
    std::size_t sceneIndex = 0;
    if (const JsonValue* scene = document.find("scene"); scene != nullptr) {
        sceneIndex = static_cast<std::size_t>(scene->asNumber());
    }
    const JsonValue* scenes = document.find("scenes");
    if (scenes == nullptr || sceneIndex >= scenes->size()) {
        SOL_LOG_ERROR("gltf: no scenes in %s", path);
        return false;
    }
    const JsonValue* rootNodes = (*scenes)[sceneIndex].find("nodes");
    if (rootNodes == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < rootNodes->size(); ++i) {
        if (!traverseNode(document, buffers, static_cast<std::size_t>((*rootNodes)[i].asNumber()),
                          Mat4::identity(), out)) {
            SOL_LOG_ERROR("gltf: import failed for %s", path);
            return false;
        }
    }

    if (out.vertices.empty() || out.indices.empty()) {
        SOL_LOG_ERROR("gltf: no triangle data in %s", path);
        return false;
    }
    return true;
}

std::string encodeBase64(const std::uint8_t* data, std::size_t length)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= length; i += 3) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8) | static_cast<std::uint32_t>(data[i + 2]);
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += kAlphabet[(triple >> 6) & 0x3F];
        out += kAlphabet[triple & 0x3F];
    }
    if (i < length) {
        const std::size_t remaining = length - i;
        std::uint32_t triple = static_cast<std::uint32_t>(data[i]) << 16;
        if (remaining == 2) {
            triple |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += remaining == 2 ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

std::string exportGltf(const assets::MeshData& mesh, const char* name)
{
    const auto vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto indexCount = static_cast<std::uint32_t>(mesh.indices.size());

    // Planar layout - positions, normals, uvs, indices - matching the assets
    // already in this repo, so a diff between an exported mesh and a shipped
    // one compares like with like.
    const std::size_t positionBytes = std::size_t{vertexCount} * 12;
    const std::size_t normalBytes = std::size_t{vertexCount} * 12;
    const std::size_t uvBytes = std::size_t{vertexCount} * 8;
    const std::size_t indexBytes = std::size_t{indexCount} * 4;

    std::vector<std::uint8_t> buffer(positionBytes + normalBytes + uvBytes + indexBytes);
    std::uint8_t* cursor = buffer.data();
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        std::memcpy(cursor, vertex.position, 12);
        cursor += 12;
    }
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        std::memcpy(cursor, vertex.normal, 12);
        cursor += 12;
    }
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        std::memcpy(cursor, vertex.uv, 8);
        cursor += 8;
    }
    if (indexBytes != 0) {
        std::memcpy(cursor, mesh.indices.data(), indexBytes);
    }

    // POSITION's min/max are required by the glTF specification, not optional
    // metadata: a viewer that culls or frames on bounds reads them and nothing
    // else in the file carries the information.
    float minimum[3] = {0, 0, 0};
    float maximum[3] = {0, 0, 0};
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            const float value = mesh.vertices[i].position[axis];
            if (i == 0 || value < minimum[axis]) {
                minimum[axis] = value;
            }
            if (i == 0 || value > maximum[axis]) {
                maximum[axis] = value;
            }
        }
    }

    const auto number = [](double value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.9g", value);
        return std::string(text);
    };
    const auto vec3 = [&number](const float (&v)[3]) {
        return "[" + number(v[0]) + "," + number(v[1]) + "," + number(v[2]) + "]";
    };

    std::string json;
    json += "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Sol Forge\"},";
    json += "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
    json += "\"nodes\":[{\"mesh\":0,\"name\":\"" + std::string(name) + "\"}],";
    json += "\"meshes\":[{\"name\":\"" + std::string(name) +
            "\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
            "\"indices\":3}]}],";
    json += "\"accessors\":[";
    json += "{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(vertexCount) +
            ",\"type\":\"VEC3\",\"min\":" + vec3(minimum) + ",\"max\":" + vec3(maximum) + "},";
    json += "{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(vertexCount) +
            ",\"type\":\"VEC3\"},";
    json += "{\"bufferView\":2,\"componentType\":5126,\"count\":" + std::to_string(vertexCount) +
            ",\"type\":\"VEC2\"},";
    json += "{\"bufferView\":3,\"componentType\":5125,\"count\":" + std::to_string(indexCount) +
            ",\"type\":\"SCALAR\"}],";
    json += "\"bufferViews\":[";
    json += "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(positionBytes) + "},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionBytes) +
            ",\"byteLength\":" + std::to_string(normalBytes) + "},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionBytes + normalBytes) +
            ",\"byteLength\":" + std::to_string(uvBytes) + "},";
    json += "{\"buffer\":0,\"byteOffset\":" +
            std::to_string(positionBytes + normalBytes + uvBytes) +
            ",\"byteLength\":" + std::to_string(indexBytes) + "}],";
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
            ",\"uri\":\"data:application/octet-stream;base64," +
            encodeBase64(buffer.data(), buffer.size()) + "\"}]}";
    return json;
}

} // namespace sol::cooker
