// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <deque>
#include <list>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class MemoryAllocator;
class Scheduler;

/// One emulated mesh draw's full input identity. Two draws with the same key run the same
/// task/mesh programs over the same input bytes, so the expansion the first one computed is
/// the expansion the second one would compute. Built by GraphicsPipeline::ConfigureDraw from
/// the bindings it resolved; see BuildMeshExpansionKey there for the exact recipe.
struct MeshExpansionKey {
    u64 program_id{};
    u64 task_hash{};
    u64 mesh_hash{};
    u32 groups{};
    u32 capacity_words{}; ///< task path: the distribution pass clamps to the arena capacity
    bool has_task{};
    bool compact{};
    u32 compact_entries{};
    /// The push constants the task/mesh programs read (rescaling words, down factor bits,
    /// render area words).
    std::vector<u32> push_words;
    /// Per task/mesh stage binding, in binding order: (device address, offset in the host
    /// buffer, size, newest write epoch of the guest range, host buffer write tick, format).
    /// Uniforms add a hash of the staged bytes the reuse machinery verified for this draw.
    std::vector<u64> bindings;
    /// Per task/mesh stage sampled image: (view id, modification tick, newest alias tick).
    std::vector<u64> images;

    bool operator==(const MeshExpansionKey&) const = default;
};

/// The miss a ConfigureDraw registered, before the rasterizer takes over its sources.
struct MeshExpansionRegistration {
    MeshExpansionKey key;
    bool has_task{};
    bool compact{};
    u32 compact_entries{};
    u32 block_words{};
    u64 tick{};
    VkBuffer output_buffer{};
    VkDeviceSize output_offset{};
    VkDeviceSize output_size{};
    VkBuffer args_buffer{};
    VkDeviceSize args_offset{};
    VkBuffer counts_buffer{};
    VkDeviceSize counts_offset{};
    VkDeviceSize counts_size{};
    VkBuffer commands_buffer{};
    VkDeviceSize commands_offset{};
    VkDeviceSize commands_size{};
};

struct MeshExpansionKeyHash {
    size_t operator()(const MeshExpansionKey& key) const noexcept {
        size_t hash{key.program_id * 0x9e3779b97f4a7c15ULL ^ key.task_hash};
        const auto mix = [&hash](u64 value) {
            hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        };
        mix(key.mesh_hash);
        mix(key.groups);
        mix(key.capacity_words);
        mix(u64{key.has_task} | (u64{key.compact} << 1) | (u64{key.compact_entries} << 2));
        for (const u64 word : key.push_words) {
            mix(word);
        }
        for (const u64 word : key.bindings) {
            mix(word);
        }
        for (const u64 word : key.images) {
            mix(word);
        }
        return hash;
    }
};

/// What a repeat draw reads instead of dispatching: the expanded output blocks, the task
/// distribution's arguments (rebased to arena word 0) and the compaction's draw commands.
/// Cache-owned device buffers, written once when the entry materializes and read-only after.
struct MeshExpansionEntry {
    vk::Buffer output;
    u64 output_size{};
    vk::Buffer args;
    vk::Buffer counts;
    vk::Buffer commands;
    u64 commands_size{};
    u64 bytes{};
    u64 ready_tick{}; ///< the materialize submission's tick; hits wait for it to be free
    u64 last_use_tick{};
    std::list<MeshExpansionKey>::iterator lru;
};

/// A miss whose expansion still has to be copied out of the draw's own buffers. The copies
/// can only be recorded once the draw's submission finished: a task draw's output region is
/// sized by words the distribution pass wrote on the GPU. The rasterizer holds the draw's
/// staging buffers (and pins its output arena) until then.
struct MeshExpansionPending {
    MeshExpansionKey key;
    bool has_task{};
    bool compact{};
    u32 compact_entries{};
    u32 block_words{};
    u64 tick{}; ///< the draw submission's tick
    // Sources, as (buffer, offset, size). The task path's output is the submission's arena:
    // its region starts at the word the materialize step reads back from the args copy.
    VkBuffer output_buffer{};
    VkDeviceSize output_offset{};
    VkDeviceSize output_size{};
    VkBuffer args_buffer{};
    VkDeviceSize args_offset{};
    VkBuffer counts_buffer{};
    VkDeviceSize counts_offset{};
    VkDeviceSize counts_size{};
    VkBuffer commands_buffer{};
    VkDeviceSize commands_offset{};
    VkDeviceSize commands_size{};
    /// Staging buffer holding one zero word: the materialize step copies it over the entry
    /// args' arena-base word, rebasing the cached expansion to its own word 0.
    VkBuffer zero_buffer{};
    VkDeviceSize zero_offset{};
    /// The draw's staging buffers, kept un-recycled until the copies are recorded.
    std::vector<StagingBufferRef> held;
    /// The arena buffer to unpin once the copies are recorded (task path).
    VkBuffer arena_buffer{};
    /// The args staging buffer's mapped bytes (Download usage), for the readback.
    std::span<const u8> args_mapped;
};

/// Cross-frame cache of emulated mesh expansions (the mesh_cache setting). Owned by the
/// pipeline cache; the rasterizer drives materialization, the graphics pipeline looks up.
class MeshExpansionCache {
public:
    static constexpr u64 MaxBytes = 256ULL << 20;
    static constexpr size_t MaxPendings = 64;

    explicit MeshExpansionCache(MemoryAllocator& memory_allocator_, Scheduler& scheduler_);
    ~MeshExpansionCache();

    MeshExpansionCache(const MeshExpansionCache&) = delete;
    MeshExpansionCache& operator=(const MeshExpansionCache&) = delete;

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// The entry a hit draws from, or null. Only entries whose materialize submission the
    /// GPU finished are served.
    [[nodiscard]] MeshExpansionEntry* Lookup(const MeshExpansionKey& key);

    /// Registers a miss for materialization once its submission finishes. False when the
    /// pending queue is full (the draw simply stays uncached). The rasterizer fills the
    /// pending's held sources in before the draw's buffers are released.
    bool AddPending(MeshExpansionRegistration registration);

    [[nodiscard]] std::deque<MeshExpansionPending>& Pendings() noexcept {
        return pendings;
    }
    void ErasePending(std::deque<MeshExpansionPending>::iterator it) {
        pendings.erase(it);
    }

    /// Allocates the entry's buffers and records the copies out of the pending's sources
    /// (the args copy's readback sizes the task path's output region). False when the
    /// readback shows nothing was dispatched, or an allocation failed.
    bool Materialize(MeshExpansionPending& pending);

    void NoteDraw(bool hit, bool has_task, bool compact, const MeshExpansionKey& key);

private:
    void EvictFor(u64 extra_bytes);
    void LogStats();

    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    const bool enabled;
    ::Common::unordered_map<MeshExpansionKey, MeshExpansionEntry, MeshExpansionKeyHash> entries;
    std::list<MeshExpansionKey> lru;
    std::deque<MeshExpansionPending> pendings;
    u64 total_bytes{};

    u64 stat_draws{};
    u64 stat_hits{};
    u64 stat_misses{};
    u64 stat_skipped{};
    std::unordered_set<MeshExpansionKey, MeshExpansionKeyHash> stat_hit_keys;
    std::chrono::steady_clock::time_point stat_window{std::chrono::steady_clock::now()};
};

} // namespace Vulkan
