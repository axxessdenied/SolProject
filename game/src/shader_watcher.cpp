#include "shader_watcher.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

namespace game {

using namespace sol;

namespace {

std::string fileName(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool hasShaderExtension(const std::string& path)
{
    return path.size() > 5 &&
           (path.rfind(".vert") == path.size() - 5 || path.rfind(".frag") == path.size() - 5);
}

} // namespace

ShaderWatcher::ShaderWatcher(std::string sourceDirectory, std::string compilerPath,
                             std::string outputDirectory)
    : m_compilerPath(std::move(compilerPath))
{
    if (m_compilerPath.empty() || sourceDirectory.empty()) {
        SOL_LOG_INFO("shader hot-reload unavailable (no compiler/source path)");
        return;
    }

    for (const std::string& file : platform::listFiles(sourceDirectory.c_str())) {
        if (!hasShaderExtension(file)) {
            continue;
        }
        WatchedShader shader;
        shader.sourcePath = file;
        shader.outputPath = outputDirectory + fileName(file) + ".spv";
        shader.modificationTime = platform::fileModificationTime(file.c_str());
        m_shaders.push_back(std::move(shader));
    }

    m_available = !m_shaders.empty();
    if (m_available) {
        SOL_LOG_INFO("shader hot-reload watching %zu files (F5 forces reload)", m_shaders.size());
    }
}

bool ShaderWatcher::poll(double nowSeconds, bool force)
{
    if (!m_available) {
        return false;
    }
    if (!force && nowSeconds - m_lastPollTime < kPollIntervalSeconds) {
        return false;
    }
    m_lastPollTime = nowSeconds;

    bool anyRecompiled = false;
    for (WatchedShader& shader : m_shaders) {
        const std::uint64_t currentTime = platform::fileModificationTime(shader.sourcePath.c_str());
        if (!force && (currentTime == 0 || currentTime == shader.modificationTime)) {
            continue;
        }
        shader.modificationTime = currentTime;

        const std::string command = "\"" + m_compilerPath + "\" --target-env=vulkan1.3 \"" +
                                    shader.sourcePath + "\" -o \"" + shader.outputPath + "\"";
        const int exitCode = platform::runProcess(command.c_str());
        if (exitCode == 0) {
            SOL_LOG_INFO("recompiled %s", fileName(shader.sourcePath).c_str());
            anyRecompiled = true;
        } else {
            SOL_LOG_ERROR("shader compile failed (%d): %s - keeping previous pipeline", exitCode,
                          fileName(shader.sourcePath).c_str());
        }
    }
    return anyRecompiled;
}

} // namespace game
