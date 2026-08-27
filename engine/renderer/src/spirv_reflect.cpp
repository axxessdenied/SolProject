#include "sol/renderer/spirv_reflect.hpp"

#include <algorithm>

namespace sol::renderer {

namespace {

// SPIR-V is a flat stream of instructions over a 5-word header, and every
// instruction carries its own length - so this walks it once, remembers the
// handful of facts it needs, and resolves them at the end. The numbers below
// are from the SPIR-V specification's own tables; they are spelled out as
// named constants rather than inline so a reader can check them against it.
constexpr std::uint32_t kMagic = 0x07230203u;
constexpr std::uint32_t kMagicReversed = 0x03022307u;
constexpr std::size_t kHeaderWords = 5;

constexpr std::uint16_t kOpName = 5;
constexpr std::uint16_t kOpMemberName = 6;
constexpr std::uint16_t kOpTypeInt = 21;
constexpr std::uint16_t kOpTypeFloat = 22;
constexpr std::uint16_t kOpTypeVector = 23;
constexpr std::uint16_t kOpTypeMatrix = 24;
constexpr std::uint16_t kOpTypeImage = 25;
constexpr std::uint16_t kOpTypeSampler = 26;
constexpr std::uint16_t kOpTypeSampledImage = 27;
constexpr std::uint16_t kOpTypeArray = 28;
constexpr std::uint16_t kOpTypeRuntimeArray = 29;
constexpr std::uint16_t kOpTypeStruct = 30;
constexpr std::uint16_t kOpTypePointer = 32;
constexpr std::uint16_t kOpVariable = 59;
constexpr std::uint16_t kOpDecorate = 71;
constexpr std::uint16_t kOpMemberDecorate = 72;

constexpr std::uint32_t kDecorationBinding = 33;
constexpr std::uint32_t kDecorationDescriptorSet = 34;
constexpr std::uint32_t kDecorationOffset = 35;

constexpr std::uint32_t kStorageClassUniformConstant = 0;
constexpr std::uint32_t kStorageClassUniform = 2;
constexpr std::uint32_t kStorageClassStorageBuffer = 12;

// What one <id> turned out to be. SPIR-V ids are dense from 1 to the header's
// bound, so a vector indexed by id is both the simplest structure and the
// fastest one; there is no hashing to do.
struct IdInfo
{
    std::uint16_t op = 0;
    // OpTypePointer: the type pointed at. OpTypeArray: the element type.
    // OpVariable: its result type (always a pointer).
    std::uint32_t target = 0;
    std::uint32_t storageClass = 0;
    // OpTypeVector: component count. OpTypeFloat/OpTypeInt: 1.
    std::uint32_t componentCount = 0;
    std::uint32_t componentBytes = 0;
    std::string name;
    bool hasSet = false;
    bool hasBinding = false;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    // OpTypeStruct only, parallel to its members.
    std::vector<std::uint32_t> memberTypes;
    std::vector<std::string> memberNames;
    std::vector<std::uint32_t> memberOffsets;
};

// A SPIR-V literal string is NUL-terminated and padded to a word boundary,
// which is why this reads bytes rather than words.
std::string readString(std::span<const std::uint32_t> words, std::size_t first, std::size_t last)
{
    std::string text;
    for (std::size_t i = first; i < last; ++i) {
        const std::uint32_t word = words[i];
        for (int byte = 0; byte < 4; ++byte) {
            const char c = static_cast<char>((word >> (byte * 8)) & 0xFFu);
            if (c == '\0') {
                return text;
            }
            text.push_back(c);
        }
    }
    return text;
}

void ensureMembers(IdInfo& info, std::size_t count)
{
    if (info.memberNames.size() < count) {
        info.memberNames.resize(count);
        info.memberOffsets.resize(count, 0);
    }
}

bool failWith(std::string* outError, std::string message)
{
    if (outError != nullptr) {
        *outError = std::move(message);
    }
    return false;
}

} // namespace

bool reflectShader(std::span<const std::uint32_t> words, ShaderInterface& out, std::string* outError)
{
    out.bindings.clear();
    out.blocks.clear();

    if (words.size() < kHeaderWords) {
        return failWith(outError, "not SPIR-V: the file is too short to hold a module header");
    }
    if (words[0] == kMagicReversed) {
        // Worth its own message: a byte-swapped module is a real thing that
        // happens when a .spv is moved between hosts by something that thinks
        // it is text, and "bad magic number" would send the reader looking at
        // the compiler instead of at the copy.
        return failWith(outError, "not SPIR-V: the module is byte-swapped (wrong-endian host?)");
    }
    if (words[0] != kMagic) {
        return failWith(outError, "not SPIR-V: wrong magic number");
    }

    const std::uint32_t bound = words[3];
    // A bound is an id ceiling, so a malformed one is a memory question rather
    // than a parsing one and is checked before anything is allocated.
    if (bound == 0 || bound > 4u * 1024u * 1024u) {
        return failWith(outError, "not SPIR-V: implausible id bound in the module header");
    }
    std::vector<IdInfo> ids(bound);

    const auto validId = [&](std::uint32_t id) { return id != 0 && id < bound; };

    std::size_t cursor = kHeaderWords;
    while (cursor < words.size()) {
        const std::uint32_t header = words[cursor];
        const std::uint16_t op = static_cast<std::uint16_t>(header & 0xFFFFu);
        const std::size_t length = static_cast<std::size_t>(header >> 16);
        if (length == 0 || cursor + length > words.size()) {
            return failWith(outError, "malformed SPIR-V: an instruction runs past the end of the module");
        }
        const std::size_t first = cursor + 1;     // first operand
        const std::size_t last = cursor + length; // one past the last operand
        const std::size_t operands = length - 1;

        switch (op) {
        case kOpName:
            if (operands >= 2 && validId(words[first])) {
                ids[words[first]].name = readString(words, first + 1, last);
            }
            break;
        case kOpMemberName:
            if (operands >= 3 && validId(words[first])) {
                IdInfo& info = ids[words[first]];
                const std::uint32_t member = words[first + 1];
                ensureMembers(info, static_cast<std::size_t>(member) + 1);
                info.memberNames[member] = readString(words, first + 2, last);
            }
            break;
        case kOpDecorate:
            if (operands >= 3 && validId(words[first])) {
                IdInfo& info = ids[words[first]];
                if (words[first + 1] == kDecorationDescriptorSet) {
                    info.hasSet = true;
                    info.set = words[first + 2];
                } else if (words[first + 1] == kDecorationBinding) {
                    info.hasBinding = true;
                    info.binding = words[first + 2];
                }
            }
            break;
        case kOpMemberDecorate:
            if (operands >= 4 && validId(words[first]) && words[first + 2] == kDecorationOffset) {
                IdInfo& info = ids[words[first]];
                const std::uint32_t member = words[first + 1];
                ensureMembers(info, static_cast<std::size_t>(member) + 1);
                info.memberOffsets[member] = words[first + 3];
            }
            break;
        case kOpTypeInt:
        case kOpTypeFloat:
            if (operands >= 2 && validId(words[first])) {
                IdInfo& info = ids[words[first]];
                info.op = op;
                info.componentCount = 1;
                info.componentBytes = words[first + 1] / 8u;
            }
            break;
        case kOpTypeVector:
            if (operands >= 3 && validId(words[first])) {
                IdInfo& info = ids[words[first]];
                info.op = op;
                info.target = words[first + 1];
                info.componentCount = words[first + 2];
            }
            break;
        case kOpTypeMatrix:
        case kOpTypeImage:
        case kOpTypeSampler:
        case kOpTypeSampledImage:
        case kOpTypeRuntimeArray:
            if (operands >= 1 && validId(words[first])) {
                ids[words[first]].op = op;
                if (op == kOpTypeSampledImage && operands >= 2) {
                    ids[words[first]].target = words[first + 1];
                }
            }
            break;
        case kOpTypeArray:
            if (operands >= 2 && validId(words[first])) {
                IdInfo& info = ids[words[first]];
                info.op = op;
                info.target = words[first + 1];
            }
            break;
        case kOpTypeStruct:
            if (operands >= 1 && validId(words[first])) {
                IdInfo& info = ids[words[first]];
                info.op = op;
                for (std::size_t i = first + 1; i < last; ++i) {
                    info.memberTypes.push_back(words[i]);
                }
                ensureMembers(info, info.memberTypes.size());
            }
            break;
        case kOpTypePointer:
            if (operands >= 3 && validId(words[first])) {
                IdInfo& info = ids[words[first]];
                info.op = op;
                info.storageClass = words[first + 1];
                info.target = words[first + 2];
            }
            break;
        case kOpVariable:
            // ⚑ Result TYPE first, result id second - the opposite order from
            // every type instruction above, and the one place in this parser
            // where getting it backwards would still produce plausible output.
            if (operands >= 3 && validId(words[first + 1])) {
                IdInfo& info = ids[words[first + 1]];
                info.op = op;
                info.target = words[first];
                info.storageClass = words[first + 2];
            }
            break;
        default:
            break;
        }
        cursor = last;
    }

    // Resolution pass. A descriptor is an OpVariable carrying both a set and a
    // binding decoration; everything else - inputs, outputs, the push block,
    // function locals - has neither and drops out here without a special case.
    for (std::uint32_t id = 1; id < bound; ++id) {
        const IdInfo& variable = ids[id];
        if (variable.op != kOpVariable || !variable.hasSet || !variable.hasBinding) {
            continue;
        }
        // The variable's type is a pointer; the thing bound is what it points
        // at, one array level down if it is arrayed.
        std::uint32_t typeId = 0;
        if (validId(variable.target) && ids[variable.target].op == kOpTypePointer) {
            typeId = ids[variable.target].target;
        }
        bool arrayed = false;
        while (validId(typeId) && (ids[typeId].op == kOpTypeArray || ids[typeId].op == kOpTypeRuntimeArray)) {
            arrayed = true;
            typeId = ids[typeId].target;
        }

        ShaderBinding binding;
        binding.set = variable.set;
        binding.binding = variable.binding;
        binding.name = variable.name;

        const std::uint16_t typeOp = validId(typeId) ? ids[typeId].op : 0;
        const bool uniformStorage = variable.storageClass == kStorageClassUniform ||
                                    variable.storageClass == kStorageClassUniformConstant;
        if (typeOp == kOpTypeSampledImage && !arrayed) {
            binding.kind = ShaderResourceKind::SampledImage;
        } else if (typeOp == kOpTypeStruct && uniformStorage &&
                   variable.storageClass != kStorageClassStorageBuffer && !arrayed) {
            binding.kind = ShaderResourceKind::UniformBuffer;

            const IdInfo& block = ids[typeId];
            ShaderBlock reflected;
            reflected.set = variable.set;
            reflected.binding = variable.binding;
            for (std::size_t m = 0; m < block.memberTypes.size(); ++m) {
                ShaderBlockMember member;
                member.name = m < block.memberNames.size() ? block.memberNames[m] : std::string{};
                member.offset = m < block.memberOffsets.size() ? block.memberOffsets[m] : 0u;
                if (member.name.empty()) {
                    reflected.hasMemberNames = false;
                }

                // Only scalars and vectors of them are fillable from a
                // `[material.params]` row; a matrix or a nested struct reflects
                // as componentCount 0, which the caller turns into a refusal
                // naming the member rather than into a silently unwritten one.
                std::uint32_t memberType = block.memberTypes[m];
                std::uint32_t components = 0;
                std::uint32_t componentBytes = 0;
                if (validId(memberType)) {
                    const IdInfo& type = ids[memberType];
                    if (type.op == kOpTypeFloat || type.op == kOpTypeInt) {
                        components = 1;
                        componentBytes = type.componentBytes;
                    } else if (type.op == kOpTypeVector && validId(type.target)) {
                        const IdInfo& component = ids[type.target];
                        if (component.op == kOpTypeFloat || component.op == kOpTypeInt) {
                            components = type.componentCount;
                            componentBytes = component.componentBytes;
                        }
                    }
                }
                member.componentCount = components;
                const std::uint32_t bytes = components * (componentBytes == 0 ? 4u : componentBytes);
                reflected.size = std::max(reflected.size, member.offset + bytes);
                reflected.members.push_back(std::move(member));
            }
            out.blocks.push_back(std::move(reflected));
        } else {
            binding.kind = ShaderResourceKind::Other;
        }
        out.bindings.push_back(std::move(binding));
    }

    const auto byLocation = [](const ShaderBinding& a, const ShaderBinding& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    };
    std::sort(out.bindings.begin(), out.bindings.end(), byLocation);
    std::sort(out.blocks.begin(), out.blocks.end(), [](const ShaderBlock& a, const ShaderBlock& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    });
    return true;
}

const ShaderBlock* findBlock(const ShaderInterface& interface, std::uint32_t set, std::uint32_t binding)
{
    for (const ShaderBlock& block : interface.blocks) {
        if (block.set == set && block.binding == binding) {
            return &block;
        }
    }
    return nullptr;
}

} // namespace sol::renderer
