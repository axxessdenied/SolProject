#include "sol/core/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace sol::core {

namespace {

LogSink g_sink = nullptr;
void* g_sinkUserData = nullptr;

const char* levelPrefix(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:
        return "[trace] ";
    case LogLevel::Info:
        return "[info ] ";
    case LogLevel::Warn:
        return "[warn ] ";
    case LogLevel::Error:
        return "[error] ";
    case LogLevel::Fatal:
        return "[FATAL] ";
    }
    return "[?    ] ";
}

} // namespace

void setLogSink(LogSink sink, void* userData)
{
    g_sink = sink;
    g_sinkUserData = userData;
}

void logMessage(LogLevel level, const char* format, ...)
{
    char message[1024];
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    std::FILE* stream = (level >= LogLevel::Warn) ? stderr : stdout;
    std::fputs(levelPrefix(level), stream);
    std::fputs(message, stream);
    std::fputc('\n', stream);

    if (g_sink != nullptr) {
        g_sink(level, message, g_sinkUserData);
    }

    if (level == LogLevel::Fatal) {
        std::fflush(stdout);
        std::fflush(stderr);
        std::abort();
    }
}

} // namespace sol::core
