#include "Sol/Proto/Harness/AllocationCounter.h"

#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace sol::proto {
namespace {

std::atomic<std::uint64_t> gAllocationCount{0};
std::atomic<std::uint64_t> gDeallocationCount{0};
std::atomic<std::uint64_t> gTotalAllocatedBytes{0};
std::atomic<std::uint64_t> gLiveBytes{0};

void recordAllocation(void* pointer, std::size_t requestedBytes, std::size_t usableBytes) noexcept {
    if (pointer == nullptr) {
        return;
    }
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    gTotalAllocatedBytes.fetch_add(requestedBytes, std::memory_order_relaxed);
    gLiveBytes.fetch_add(usableBytes, std::memory_order_relaxed);
}

void recordDeallocation(std::size_t usableBytes) noexcept {
    gDeallocationCount.fetch_add(1, std::memory_order_relaxed);

    // Saturate rather than wrap: a block allocated before reset() and freed after it would
    // otherwise underflow into a nonsensical enormous value.
    std::uint64_t live = gLiveBytes.load(std::memory_order_relaxed);
    while (true) {
        const std::uint64_t next = (live >= usableBytes) ? live - usableBytes : 0;
        if (gLiveBytes.compare_exchange_weak(live, next, std::memory_order_relaxed)) {
            break;
        }
    }
}

/// Allocates through malloc and records the block.
///
/// Returns nullptr on failure; the calling operator decides between throwing and returning
/// null. A zero-byte request is rounded up to one byte so every allocation yields a
/// distinct pointer, as the standard requires.
void* allocateTracked(std::size_t bytes) noexcept {
    const std::size_t requestBytes = (bytes == 0) ? 1 : bytes;
    void* const pointer = std::malloc(requestBytes);
    if (pointer != nullptr) {
        recordAllocation(pointer, bytes, _msize(pointer));
    }
    return pointer;
}

void* allocateTrackedAligned(std::size_t bytes, std::size_t alignment) noexcept {
    const std::size_t requestBytes = (bytes == 0) ? 1 : bytes;
    void* const pointer = _aligned_malloc(requestBytes, alignment);
    if (pointer != nullptr) {
        recordAllocation(pointer, bytes, _aligned_msize(pointer, alignment, 0));
    }
    return pointer;
}

void deallocateTracked(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    recordDeallocation(_msize(pointer));
    std::free(pointer);
}

void deallocateTrackedAligned(void* pointer, std::size_t alignment) noexcept {
    if (pointer == nullptr) {
        return;
    }
    recordDeallocation(_aligned_msize(pointer, alignment, 0));
    _aligned_free(pointer);
}

} // namespace

namespace allocations {

AllocationCounts snapshot() noexcept {
    AllocationCounts counts;
    counts.allocationCount = gAllocationCount.load(std::memory_order_relaxed);
    counts.deallocationCount = gDeallocationCount.load(std::memory_order_relaxed);
    counts.totalAllocatedBytes = gTotalAllocatedBytes.load(std::memory_order_relaxed);
    counts.liveBytes = gLiveBytes.load(std::memory_order_relaxed);
    return counts;
}

void reset() noexcept {
    gAllocationCount.store(0, std::memory_order_relaxed);
    gDeallocationCount.store(0, std::memory_order_relaxed);
    gTotalAllocatedBytes.store(0, std::memory_order_relaxed);
    gLiveBytes.store(0, std::memory_order_relaxed);
}

} // namespace allocations
} // namespace sol::proto

// --------------------------------------------------------------------------------------
// Replacement global allocation functions.
//
// The full family is replaced. Replacing only the common overloads would let an aligned or
// sized path reach the default allocator while its partner reached ours, which corrupts the
// heap rather than merely miscounting.
// --------------------------------------------------------------------------------------

void* operator new(std::size_t bytes) {
    void* const pointer = sol::proto::allocateTracked(bytes);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new[](std::size_t bytes) {
    void* const pointer = sol::proto::allocateTracked(bytes);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    void* const pointer =
        sol::proto::allocateTrackedAligned(bytes, static_cast<std::size_t>(alignment));
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    void* const pointer =
        sol::proto::allocateTrackedAligned(bytes, static_cast<std::size_t>(alignment));
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return sol::proto::allocateTracked(bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return sol::proto::allocateTracked(bytes);
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return sol::proto::allocateTrackedAligned(bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return sol::proto::allocateTrackedAligned(bytes, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer) noexcept {
    sol::proto::deallocateTracked(pointer);
}

void operator delete[](void* pointer) noexcept {
    sol::proto::deallocateTracked(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    sol::proto::deallocateTracked(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    sol::proto::deallocateTracked(pointer);
}

void operator delete(void* pointer, std::align_val_t alignment) noexcept {
    sol::proto::deallocateTrackedAligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
    sol::proto::deallocateTrackedAligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    sol::proto::deallocateTrackedAligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    sol::proto::deallocateTrackedAligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    sol::proto::deallocateTracked(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    sol::proto::deallocateTracked(pointer);
}

void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    sol::proto::deallocateTrackedAligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete[](void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    sol::proto::deallocateTrackedAligned(pointer, static_cast<std::size_t>(alignment));
}
