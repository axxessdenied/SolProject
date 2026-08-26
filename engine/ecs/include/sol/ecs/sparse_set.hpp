#pragma once

// Sparse-set component storage, per docs/decisions/001-ecs-storage-model.md:
// one pool per component type holding a dense value array, a parallel dense
// array of owning entity indices, and an entity-indexed sparse redirection
// table. O(1) add, swap-and-pop remove, linear iteration over dense arrays.

#include "sol/core/assert.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace sol::ecs {

inline constexpr std::uint32_t kInvalidDenseIndex = 0xffff'ffffu;

// Type-erased base carrying the entity bookkeeping, so the registry can walk
// every pool on entity destroy and views can probe pools without knowing the
// component type.
class PoolBase
{
public:
    PoolBase() = default;
    PoolBase(const PoolBase&) = delete;
    PoolBase& operator=(const PoolBase&) = delete;
    PoolBase(PoolBase&&) = default;
    PoolBase& operator=(PoolBase&&) = default;
    virtual ~PoolBase() = default;

    [[nodiscard]] bool contains(std::uint32_t entityIndex) const
    {
        return entityIndex < m_sparse.size() && m_sparse[entityIndex] != kInvalidDenseIndex;
    }

    // Caller guarantees contains(entityIndex).
    [[nodiscard]] std::uint32_t denseIndexOf(std::uint32_t entityIndex) const
    {
        SOL_ASSERT(contains(entityIndex));
        return m_sparse[entityIndex];
    }

    [[nodiscard]] std::size_t size() const { return m_dense.size(); }

    [[nodiscard]] bool empty() const { return m_dense.empty(); }

    // Owning entity indices, parallel to the derived pool's value array.
    [[nodiscard]] const std::vector<std::uint32_t>& entityIndices() const { return m_dense; }

    void remove(std::uint32_t entityIndex)
    {
        SOL_ASSERT(contains(entityIndex));
        const std::uint32_t dense = m_sparse[entityIndex];
        const std::uint32_t last = static_cast<std::uint32_t>(m_dense.size()) - 1;
        popValueInto(last, dense);
        if (dense != last) {
            m_dense[dense] = m_dense[last];
            m_sparse[m_dense[dense]] = dense;
        }
        m_dense.pop_back();
        m_sparse[entityIndex] = kInvalidDenseIndex;
    }

protected:
    void insertIndex(std::uint32_t entityIndex)
    {
        SOL_ASSERT(!contains(entityIndex));
        if (entityIndex >= m_sparse.size()) {
            m_sparse.resize(entityIndex + 1, kInvalidDenseIndex);
        }
        m_sparse[entityIndex] = static_cast<std::uint32_t>(m_dense.size());
        m_dense.push_back(entityIndex);
    }

    // Move the value at dense slot `fromLast` into slot `to` (no-op when they
    // are the same slot) and shrink the value array by one.
    virtual void popValueInto(std::uint32_t fromLast, std::uint32_t to) = 0;

private:
    std::vector<std::uint32_t> m_sparse;
    std::vector<std::uint32_t> m_dense;
};

template <typename T>
class Pool final : public PoolBase
{
public:
    template <typename... Args>
    T& emplace(std::uint32_t entityIndex, Args&&... args)
    {
        insertIndex(entityIndex);
        return m_values.emplace_back(std::forward<Args>(args)...);
    }

    [[nodiscard]] T& get(std::uint32_t entityIndex) { return m_values[denseIndexOf(entityIndex)]; }

    [[nodiscard]] const T& get(std::uint32_t entityIndex) const
    {
        return m_values[denseIndexOf(entityIndex)];
    }

    [[nodiscard]] T* tryGet(std::uint32_t entityIndex)
    {
        return contains(entityIndex) ? &m_values[denseIndexOf(entityIndex)] : nullptr;
    }

    [[nodiscard]] const T* tryGet(std::uint32_t entityIndex) const
    {
        return contains(entityIndex) ? &m_values[denseIndexOf(entityIndex)] : nullptr;
    }

    // Dense value array, parallel to entityIndices().
    [[nodiscard]] std::vector<T>& values() { return m_values; }

    [[nodiscard]] const std::vector<T>& values() const { return m_values; }

private:
    void popValueInto(std::uint32_t fromLast, std::uint32_t to) override
    {
        if (fromLast != to) {
            m_values[to] = std::move(m_values[fromLast]);
        }
        m_values.pop_back();
    }

    std::vector<T> m_values;
};

} // namespace sol::ecs
