#pragma once

#include "sol/core/log.hpp"

#if defined(_MSC_VER)
    #define SOL_DEBUG_BREAK() __debugbreak()
#else
    #define SOL_DEBUG_BREAK() __builtin_trap()
#endif

// Always-on invariant check; failure is fatal in every build configuration.
#define SOL_VERIFY(expression)                                                                               \
    do {                                                                                                     \
        if (!(expression)) {                                                                                 \
            SOL_LOG_FATAL("verify failed: %s (%s:%d)", #expression, __FILE__, __LINE__);                     \
        }                                                                                                    \
    } while (0)

// Dev-build-only programmer-error check; compiled out under NDEBUG.
#if defined(NDEBUG)
    #define SOL_ASSERT(expression) ((void)0)
#else
    #define SOL_ASSERT(expression)                                                                           \
        do {                                                                                                 \
            if (!(expression)) {                                                                             \
                SOL_LOG_ERROR("assert failed: %s (%s:%d)", #expression, __FILE__, __LINE__);                 \
                SOL_DEBUG_BREAK();                                                                           \
            }                                                                                                \
        } while (0)
#endif
