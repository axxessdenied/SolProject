#include "project_paths.hpp"

#include "sol/platform/file_io.hpp"

#include <cstring>

namespace forge {
namespace {

using namespace sol;

// '/' on every platform, exactly one at the end. The same normalisation
// `game/src/asset_paths.cpp` does and for the same recorded reason: the one
// time the separator contract slipped it produced a mod layer named `C:` in a
// shipping build rather than a compile error. Here the inputs genuinely differ
// - `executableDirectory()` supplies a trailing slash and a path typed on a
// command line supplies whatever the author typed, including a Windows one.
[[nodiscard]] std::string asDirectory(const std::string& path)
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
    return result + "/";
}

} // namespace

ProjectPaths projectPathsFor(const std::string& projectDirectory, const std::string& executableDirectory)
{
    const std::string project = asDirectory(projectDirectory);
    const std::string install = asDirectory(executableDirectory);

    ProjectPaths paths;
    paths.assets = project + "assets/";
    paths.cooked = project + "cooked/";
    // The project itself: a mod's def documents sit at its top level.
    paths.data = project;
    paths.inbox = project + "blender-inbox/";
    paths.shaderSearchPath = {project + "shaders/", install + "shaders/"};
    paths.isProject = true;
    return paths;
}

ProjectPaths devPathsFor(const DevPaths& dev, const std::string& executableDirectory)
{
    const std::string install = asDirectory(executableDirectory);

    ProjectPaths paths;
    paths.assets = asDirectory(dev.assets);
    // ⚑ Beside the BINARY and not under the source tree, because that is where
    // `sol_cook_assets` and `sol_shaders` generate them. The repo has no
    // `cooked/` and never has: a dev tree is three unrelated directories rather
    // than one project, which is precisely why it cannot be spelled as one.
    paths.cooked = install + "cooked/";
    paths.data = asDirectory(dev.data);
    paths.inbox = asDirectory(dev.inbox);
    // One entry, not two. There is no project to lay in front, and repeating
    // the install directory would make `resolveAsset` read the same file twice
    // before failing.
    paths.shaderSearchPath = {install + "shaders/"};
    paths.isProject = false;
    return paths;
}

ProjectPaths resolveProjectPaths(const std::string& explicitProject,
                                 const DevPaths& dev,
                                 const std::string& executableDirectory)
{
    if (!explicitProject.empty()) {
        return projectPathsFor(explicitProject, executableDirectory);
    }
    // ⚑ ALL THREE, not any one of them. A build either bakes the source tree or
    // it does not; a half-baked set would be a configuration nothing produces,
    // and treating it as dev would mean resolving one directory against a repo
    // that is not on this machine.
    if (!dev.assets.empty() && !dev.data.empty() && !dev.inbox.empty()) {
        return devPathsFor(dev, executableDirectory);
    }
    return projectPathsFor(executableDirectory, executableDirectory);
}

std::string parseProjectArgument(std::span<const std::string> arguments)
{
    // ⚑ The operands of the other two flags are skipped rather than merely not
    // matched. `forge --open ship` would otherwise read `ship` as a bare path
    // and open a project that does not exist, which is a worse outcome than any
    // typo: the tool would come up empty and blame the author's directory.
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "--project") {
            return i + 1 < arguments.size() ? arguments[i + 1] : std::string();
        }
        if (argument == "--open" || argument == "--frames") {
            ++i;
            continue;
        }
        if (!argument.empty() && argument[0] != '-') {
            return argument;
        }
    }
    return {};
}

bool createProjectDirectories(const ProjectPaths& paths)
{
    // ⚑ The three asset subdirectories are named because the tool's own "new
    // document" buttons write into them - `openNew(assets + "/meshes")` and
    // `openNew(assets + "/textures")` - and because the cooker walks `assets/`
    // recursively, so an author who never makes one still gets a project laid
    // out the way every example in `game/mods/README.md` is written.
    const std::string directories[] = {
        paths.assets,
        paths.assets + "meshes/",
        paths.assets + "textures/",
        paths.assets + "sounds/",
        paths.cooked,
        paths.inbox,
        paths.shaderSearchPath.front(),
    };
    bool ok = true;
    for (const std::string& directory : directories) {
        if (!platform::createDirectories(directory.c_str())) {
            ok = false;
        }
    }
    return ok;
}

} // namespace forge
