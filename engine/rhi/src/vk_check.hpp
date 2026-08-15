#pragma once

#include "sol/core/log.hpp"

#include <vulkan/vulkan.h>

// Init-path helper: any failure here is unrecoverable in Phase 1.
#define SOL_VK_CHECK(expression)                                                                             \
    do {                                                                                                     \
        const VkResult solVkResult_ = (expression);                                                          \
        if (solVkResult_ != VK_SUCCESS) {                                                                    \
            SOL_LOG_FATAL("Vulkan call failed (%d): %s (%s:%d)",                                             \
                          static_cast<int>(solVkResult_),                                                    \
                          #expression,                                                                       \
                          __FILE__,                                                                          \
                          __LINE__);                                                                         \
        }                                                                                                    \
    } while (0)
