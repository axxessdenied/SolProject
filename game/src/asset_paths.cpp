#include "asset_paths.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <set>

namespace game {

using namespace sol;

namespace {

// '/' on every platform, and no trailing one. `platform::listFiles` and
// `platform::executableDirectory` both promise forward slashes already, so
// this is belt as well as braces - but it is cheap belt, and the one time this
// contract slipped it cost a mod layer named `C:` rather than a compile error.
[[nodiscard]] std::string normalizeDirectory(const std::string& path)
{
    std::string result = path;
    for (char& c : result) {
        if (c == '\\') {
            c = '/';
        }
    }
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

[[nodiscard]] std::string normalizePath(const std::string& path)
{
    std::string result = path;
    for (char& c : result) {
        if (c == '\\') {
            c = '/';
        }
    }
    return result;
}

} // namespace

std::vector<std::string> modLayerNames(const std::string& modsDirectory,
                                       std::span<const std::string> listing,
                                       std::vector<std::string>* outUnrelated)
{
    const std::string prefix = normalizeDirectory(modsDirectory) + "/";

    // Sorted and de-duplicated in one step: a mod with forty files must yield
    // one name, and the def layering's "sorted by name" is the order every
    // other part of this system already agrees on.
    std::set<std::string> names;
    for (const std::string& entry : listing) {
        const std::string path = normalizePath(entry);
        if (path.rfind(prefix, 0) != 0) {
            // ⚑ THE BRANCH THE `C:` BUG CAME THROUGH. The old code carried on
            // with the unmatched path and took its first segment, so an
            // absolute Windows path yielded the drive letter. There is no
            // sensible layer name to recover here - a path that is not under
            // the mods directory is not a mod - so it is refused and reported.
            if (outUnrelated != nullptr) {
                outUnrelated->push_back(entry);
            }
            continue;
        }
        const std::string relative = path.substr(prefix.size());
        const std::size_t slash = relative.find('/');
        if (slash == std::string::npos || slash == 0) {
            continue; // a file directly in mods/ (README.md) is not a layer
        }
        names.insert(relative.substr(0, slash));
    }
    return {names.begin(), names.end()};
}

std::vector<std::string> discoverModLayers(const std::string& modsDirectory)
{
    const std::string root = normalizeDirectory(modsDirectory);
    const std::vector<std::string> listing = platform::listFiles(modsDirectory.c_str());

    std::vector<std::string> unrelated;
    const std::vector<std::string> names = modLayerNames(root, listing, &unrelated);
    for (const std::string& path : unrelated) {
        SOL_LOG_WARN("mods: '%s' is not under %s and was skipped", path.c_str(), root.c_str());
    }

    std::vector<std::string> directories;
    directories.reserve(names.size());
    for (const std::string& name : names) {
        directories.push_back(root + "/" + name);
    }
    return directories;
}

std::vector<std::string> cookedSearchPath(const std::string& baseCookedDirectory,
                                          std::span<const std::string> modLayerDirectories)
{
    std::vector<std::string> searchPath;
    searchPath.reserve(modLayerDirectories.size() + 1);

    // Reverse layer order: the last-named mod wins, which is the same
    // precedence the def merge produces by overwriting in place.
    for (std::size_t i = modLayerDirectories.size(); i > 0; --i) {
        searchPath.push_back(normalizeDirectory(modLayerDirectories[i - 1]) + "/cooked/");
    }
    searchPath.push_back(normalizeDirectory(baseCookedDirectory) + "/");
    return searchPath;
}

std::vector<std::string> assetCandidates(std::span<const std::string> searchPath, const std::string& name)
{
    std::vector<std::string> candidates;
    candidates.reserve(searchPath.size());
    for (const std::string& directory : searchPath) {
        candidates.push_back(directory + name);
    }
    return candidates;
}

std::string resolveAsset(std::span<const std::string> searchPath, const std::string& name)
{
    for (const std::string& candidate : assetCandidates(searchPath, name)) {
        if (platform::fileModificationTime(candidate.c_str()) != 0) {
            return candidate;
        }
    }
    return {};
}

std::string describeSearchPath(std::span<const std::string> searchPath)
{
    std::string description;
    for (const std::string& directory : searchPath) {
        if (!description.empty()) {
            description += ", ";
        }
        description += directory;
    }
    return description;
}

} // namespace game
