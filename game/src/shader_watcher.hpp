#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// Dev-build shader hot-reload: polls source GLSL mtimes and recompiles
// changed files with glslc via process spawn. Inert when the compiler or
// source directory is unavailable (e.g. shipping layouts).
class ShaderWatcher
{
public:
    ShaderWatcher(std::string sourceDirectory, std::string compilerPath, std::string outputDirectory);

    // Returns true when at least one shader recompiled successfully (the
    // caller should idle the device and reload pipelines). force=true
    // recompiles everything regardless of timestamps.
    [[nodiscard]] bool poll(double nowSeconds, bool force = false);

    [[nodiscard]] bool available() const { return m_available; }

private:
    struct WatchedShader
    {
        std::string sourcePath;
        std::string outputPath;
        std::uint64_t modificationTime = 0;
    };

    static constexpr double kPollIntervalSeconds = 0.5;

    std::string m_compilerPath;
    std::vector<WatchedShader> m_shaders;
    double m_lastPollTime = -1.0;
    bool m_available = false;
};

} // namespace game
