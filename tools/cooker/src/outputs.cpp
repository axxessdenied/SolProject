#include "outputs.hpp"

#include "mesh.hpp"

#include <algorithm>
#include <cctype>

namespace sol::cooker {

namespace {

[[nodiscard]] std::string fileName(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] std::string lowerExtension(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string extension = path.substr(dot);
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

// ⚑ The four the cooker writes, and the list is exhaustive on purpose: adding a
// cooked format without adding it here means its outputs are never swept, which
// fails quietly. Adding it here without the cooker producing it is far worse -
// every file of that kind becomes a stray on the next build.
[[nodiscard]] bool isCookedOutput(const std::string& name)
{
    const std::string extension = lowerExtension(name);
    return extension == ".smesh" || extension == ".stex" || extension == ".saud" || extension == ".sfont";
}

} // namespace

std::vector<std::string> expectedOutputNames(const std::vector<std::string>& jobOutputs)
{
    std::vector<std::string> expected;
    expected.reserve(jobOutputs.size() * 2);
    for (const std::string& output : jobOutputs) {
        expected.push_back(fileName(output));
        if (lowerExtension(output) != ".smesh") {
            continue;
        }
        // ⚑ Named through `meshLevelPath` rather than by rebuilding the
        // `.lodN.` pattern here. Two spellings of where a level lives is
        // exactly the kind of pair that drifts, and the one that drifts is the
        // one that decides what gets DELETED.
        for (std::uint32_t level = 1; level <= kMaxMeshLevels; ++level) {
            expected.push_back(fileName(meshLevelPath(output, level)));
        }
    }
    return expected;
}

std::vector<std::string> strayOutputNames(const std::vector<std::string>& expected,
                                          const std::vector<std::string>& present)
{
    std::vector<std::string> strays;
    for (const std::string& path : present) {
        const std::string name = fileName(path);
        if (!isCookedOutput(name)) {
            continue;
        }
        if (std::find(expected.begin(), expected.end(), name) != expected.end()) {
            continue;
        }
        strays.push_back(path);
    }
    return strays;
}

} // namespace sol::cooker
