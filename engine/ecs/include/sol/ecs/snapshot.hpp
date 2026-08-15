#pragma once

// World state serialization (the ECS is the save-game backbone, engine plan
// 2.5). The game registers each serializable component type with a stable id
// - the runtime componentTypeId depends on template instantiation order and
// must never reach the disk. Chunks for unregistered stable ids are skipped
// on load (forward compatibility); registered types absent from the blob
// simply stay empty (backward compatibility).

#include "sol/ecs/entity.hpp"
#include "sol/ecs/registry.hpp"

#include "sol/core/serialize.hpp"

#include <cstdint>
#include <type_traits>
#include <vector>

namespace sol::ecs {

class Snapshot
{
public:
    static constexpr std::uint32_t kMagic = 0x574C'4F53u; // "SOLW"
    static constexpr std::uint32_t kVersion = 1;

    template <typename T>
    void component(std::uint32_t stableId)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "snapshot components are stored by memcpy; non-POD components need a "
                      "custom serializer (not yet required)");
        SOL_ASSERT(findEntry(stableId) == nullptr);
        m_entries.push_back(Entry{
            .stableId = stableId,
            .valueSize = static_cast<std::uint32_t>(sizeof(T)),
            .save = &saveComponent<T>,
            .load = &loadComponent<T>,
        });
    }

    // Writes the full world state: entity slots (generations + free list)
    // and one chunk per registered component, in registration order, so
    // save -> load -> save is byte-identical.
    void save(Registry& registry, core::BinaryWriter& writer) const
    {
        writer.write(kMagic);
        writer.write(kVersion);

        writer.write(static_cast<std::uint32_t>(registry.m_generations.size()));
        writer.writeBytes(registry.m_generations.data(),
                          registry.m_generations.size() * sizeof(std::uint32_t));
        writer.write(static_cast<std::uint32_t>(registry.m_freeList.size()));
        writer.writeBytes(registry.m_freeList.data(),
                          registry.m_freeList.size() * sizeof(std::uint32_t));

        writer.write(static_cast<std::uint32_t>(m_entries.size()));
        for (const Entry& entry : m_entries) {
            entry.save(entry, registry, writer);
        }
    }

    // Restores into an empty registry. Returns false on malformed input
    // (bad magic/version, truncation, components on dead or out-of-range
    // entity slots); the registry contents are unspecified on failure.
    [[nodiscard]] bool load(Registry& registry, core::BinaryReader& reader) const
    {
        SOL_ASSERT(registry.m_generations.empty()); // load wants a fresh registry

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        if (!reader.read(magic) || !reader.read(version) || magic != kMagic ||
            version != kVersion) {
            return false;
        }

        std::uint32_t slotCount = 0;
        if (!reader.read(slotCount) || slotCount * sizeof(std::uint32_t) > reader.remaining()) {
            return false;
        }
        registry.m_generations.resize(slotCount);
        if (!reader.readBytes(registry.m_generations.data(),
                              slotCount * sizeof(std::uint32_t))) {
            return false;
        }

        std::uint32_t freeCount = 0;
        if (!reader.read(freeCount) || freeCount > slotCount ||
            freeCount * sizeof(std::uint32_t) > reader.remaining()) {
            return false;
        }
        registry.m_freeList.resize(freeCount);
        if (!reader.readBytes(registry.m_freeList.data(), freeCount * sizeof(std::uint32_t))) {
            return false;
        }

        std::vector<bool> alive(slotCount, true);
        for (const std::uint32_t index : registry.m_freeList) {
            if (index >= slotCount) {
                return false;
            }
            alive[index] = false;
        }

        std::uint32_t chunkCount = 0;
        if (!reader.read(chunkCount)) {
            return false;
        }
        for (std::uint32_t i = 0; i < chunkCount; ++i) {
            std::uint32_t stableId = 0;
            std::uint32_t entityCount = 0;
            std::uint32_t byteSize = 0;
            if (!reader.read(stableId) || !reader.read(entityCount) || !reader.read(byteSize)) {
                return false;
            }
            const Entry* entry = findEntry(stableId);
            if (entry == nullptr) {
                if (!reader.skip(byteSize)) {
                    return false;
                }
                continue;
            }
            if (byteSize !=
                entityCount * (sizeof(std::uint32_t) + static_cast<std::size_t>(entry->valueSize))) {
                return false;
            }
            if (!entry->load(registry, reader, entityCount, alive)) {
                return false;
            }
        }
        return !reader.failed();
    }

private:
    struct Entry;
    using SaveFn = void (*)(const Entry&, Registry&, core::BinaryWriter&);
    using LoadFn = bool (*)(Registry&, core::BinaryReader&, std::uint32_t,
                            const std::vector<bool>&);

    struct Entry
    {
        std::uint32_t stableId = 0;
        std::uint32_t valueSize = 0;
        SaveFn save = nullptr;
        LoadFn load = nullptr;
    };

    [[nodiscard]] const Entry* findEntry(std::uint32_t stableId) const
    {
        for (const Entry& entry : m_entries) {
            if (entry.stableId == stableId) {
                return &entry;
            }
        }
        return nullptr;
    }

    template <typename T>
    static void saveComponent(const Entry& entry, Registry& registry, core::BinaryWriter& writer)
    {
        Pool<T>& pool = registry.storage<T>();
        const std::uint32_t count = static_cast<std::uint32_t>(pool.size());
        writer.write(entry.stableId);
        writer.write(count);
        writer.write(static_cast<std::uint32_t>(count * (sizeof(std::uint32_t) + sizeof(T))));
        writer.writeBytes(pool.entityIndices().data(), count * sizeof(std::uint32_t));
        writer.writeBytes(pool.values().data(), count * sizeof(T));
    }

    template <typename T>
    static bool loadComponent(Registry& registry, core::BinaryReader& reader,
                              std::uint32_t entityCount, const std::vector<bool>& alive)
    {
        std::vector<std::uint32_t> indices(entityCount);
        if (!reader.readBytes(indices.data(), entityCount * sizeof(std::uint32_t))) {
            return false;
        }
        Pool<T>& pool = registry.storage<T>();
        for (const std::uint32_t index : indices) {
            if (index >= alive.size() || !alive[index] || pool.contains(index)) {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < entityCount; ++i) {
            T value;
            if (!reader.read(value)) {
                return false;
            }
            pool.emplace(indices[i], value);
        }
        return true;
    }

    std::vector<Entry> m_entries;
};

} // namespace sol::ecs
