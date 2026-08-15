#include "sol/core/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace sol::core {

namespace {

const char* levelPrefix(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return "[trace] ";
    case LogLevel::Info: return "[info ] ";
    case LogLevel::Warn: return "[warn ] ";
    case LogLevel::Error: return "[error] ";
    case LogLevel::Fatal: return "[FATAL] ";
    }
    return "[?    ] ";
}

} // namespace

void logMessage(LogLevel level, const char* format, ...)
{
    std::FILE* stream = (level >= LogLevel::Warn) ? stderr : stdout;

    std::fputs(levelPrefix(level), stream);

    std::va_list args;
    va_start(args, format);
    std::vfprintf(stream, format, args);
    va_end(args);

    std::fputc('\n', stream);

    if (level == LogLevel::Fatal) {
        std::fflush(stdout);
        std::fflush(stderr);
        std::abort();
    }
}

} // namespace sol::core
