#pragma once

namespace sol::core {

enum class LogLevel
{
    Trace,
    Info,
    Warn,
    Error,
    Fatal,
};

// Printf-style. Fatal flushes all output and aborts the process.
void logMessage(LogLevel level, const char* format, ...);

} // namespace sol::core

#define SOL_LOG_TRACE(...) ::sol::core::logMessage(::sol::core::LogLevel::Trace, __VA_ARGS__)
#define SOL_LOG_INFO(...) ::sol::core::logMessage(::sol::core::LogLevel::Info, __VA_ARGS__)
#define SOL_LOG_WARN(...) ::sol::core::logMessage(::sol::core::LogLevel::Warn, __VA_ARGS__)
#define SOL_LOG_ERROR(...) ::sol::core::logMessage(::sol::core::LogLevel::Error, __VA_ARGS__)
#define SOL_LOG_FATAL(...) ::sol::core::logMessage(::sol::core::LogLevel::Fatal, __VA_ARGS__)
