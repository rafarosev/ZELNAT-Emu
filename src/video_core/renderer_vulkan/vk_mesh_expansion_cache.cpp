// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>

#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_mesh_expansion_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

MeshExpansionCache::MeshExpansionCache(MemoryAllocator& memory_allocator_, Scheduler& scheduler_)
    : memory_allocator{memory_allocator_}, scheduler{scheduler_},
      enabled{Settings::IsMeshCacheEnabled()} {}

MeshExpansionCache::~MeshExpansionCache() = default;

MeshExpansionEntry* MeshExpansionCache::Lookup(const MeshExpansionKey& key) {
    const auto it = entries.find(key);
    if (it == entries.end()) {
        return nullptr;
    }
    MeshExpansionEntry& entry = it->second;
    if (!scheduler.IsFree(entry.ready_tick)) {
        return nullptr;
    }
    entry.last_use_tick = scheduler.CurrentTick();
    lru.splice(lru.end(), lru, entry.lru);
    return &entry;
}

bool MeshExpansionCache::AddPending(MeshExpansionRegistration registration) {
    if (pendings.size() >= MaxPendings) {
        return false;
    }
    MeshExpansionPending pending;
    pending.key = std::move(registration.key);
    pending.has_task = registration.has_task;
    pending.compact = registration.compact;
    pending.compact_entries = registration.compact_entries;
    pending.block_words = registration.block_words;
    pending.tick = registration.tick;
    pending.output_buffer = registration.output_buffer;
    pending.output_offset = registration.output_offset;
    pending.output_size = registration.output_size;
    pending.args_buffer = registration.args_buffer;
    pending.args_offset = registration.args_offset;
    pending.counts_buffer = registration.counts_buffer;
    pending.counts_offset = registration.counts_offset;
    pending.counts_size = registration.counts_size;
    pending.commands_buffer = registration.commands_buffer;
    pending.commands_offset = registration.commands_offset;
    pending.commands_size = registration.commands_size;
    pendings.push_back(std::move(pending));
    return true;
}

bool MeshExpansionCache::Materialize(MeshExpansionPending& pending) {
    u64 output_offset = pending.output_offset;
    u64 output_size = pending.output_size;
    if (pending.has_task) {
        // The output region is sized by the distribution pass's own words: how many mesh
        // workgroups it dispatched, starting at which arena word.
        if (pending.args_mapped.size() <
            Shader::Backend::SPIRV::MESH_EMULATION_ARGS_WORDS * sizeof(u32)) {
            return false;
        }
        std::array<u32, Shader::Backend::SPIRV::MESH_EMULATION_ARGS_WORDS> words{};
        std::memcpy(words.data(), pending.args_mapped.data(), sizeof(words));
        const u32 dispatched = words[0];
        if (dispatched == 0) {
            return false;
        }
        output_offset = u64{words[Shader::Backend::SPIRV::MESH_EMULATION_ARGS_BASE]} * 4;
        output_size = u64{dispatched} * pending.block_words * 4;
        if (output_offset + output_size > pending.output_size) {
            return false;
        }
    }
    const u64 args_size = pending.has_task
                              ? Shader::Backend::SPIRV::MESH_EMULATION_ARGS_WORDS * sizeof(u32)
                              : 0;
    const u64 total = output_size + args_size + pending.counts_size + pending.commands_size;
    EvictFor(total);

    const auto create = [&](u64 size) {
        return memory_allocator.TryCreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = (std::max)(size, u64{4}),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::DeviceLocal);
    };
    MeshExpansionEntry entry;
    entry.output = create(output_size);
    if (!*entry.output) {
        return false;
    }
    entry.output_size = output_size;
    if (pending.has_task) {
        entry.args = create(args_size);
        if (!*entry.args) {
            return false;
        }
    }
    if (pending.counts_size != 0) {
        entry.counts = create(pending.counts_size);
        if (!*entry.counts) {
            return false;
        }
    }
    if (pending.commands_size != 0) {
        entry.commands = create(pending.commands_size);
        if (!*entry.commands) {
            return false;
        }
        entry.commands_size = pending.commands_size;
    }
    entry.bytes = total;

    const VkBuffer output_buffer = *entry.output;
    const VkBuffer args_buffer = pending.has_task ? *entry.args : VkBuffer{};
    const VkBuffer counts_buffer = pending.counts_size != 0 ? *entry.counts : VkBuffer{};
    const VkBuffer commands_buffer = pending.commands_size != 0 ? *entry.commands : VkBuffer{};
    const VkBuffer src_output = pending.output_buffer;
    const VkBuffer src_args = pending.args_buffer;
    const VkDeviceSize src_args_offset = pending.args_offset;
    const VkBuffer src_counts = pending.counts_buffer;
    const VkDeviceSize src_counts_offset = pending.counts_offset;
    const VkDeviceSize counts_size = pending.counts_size;
    const VkBuffer src_commands = pending.commands_buffer;
    const VkDeviceSize src_commands_offset = pending.commands_offset;
    const VkDeviceSize commands_size = pending.commands_size;
    const VkBuffer zero_buffer = pending.zero_buffer;
    const VkDeviceSize zero_offset = pending.zero_offset;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const auto barrier_for = [&](VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                                     VkAccessFlags src_access, VkAccessFlags dst_access,
                                     VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
            const VkBufferMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = src_access,
                .dstAccessMask = dst_access,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = buffer,
                .offset = offset,
                .size = size,
            };
            cmdbuf.PipelineBarrier(src_stage, dst_stage, 0, barrier);
        };
        // The sources were written by the draw's compute dispatches in an earlier submission.
        barrier_for(src_output, output_offset, output_size, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (args_buffer) {
            barrier_for(src_args, src_args_offset, args_size, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        if (counts_buffer) {
            barrier_for(src_counts, src_counts_offset, counts_size, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        if (commands_buffer) {
            barrier_for(src_commands, src_commands_offset, commands_size,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        const auto copy = [&](VkBuffer src, VkDeviceSize src_off, VkBuffer dst, VkDeviceSize size) {
            const VkBufferCopy region{.srcOffset = src_off, .dstOffset = 0, .size = size};
            cmdbuf.CopyBuffer(src, dst, vk::Span<VkBufferCopy>{region});
        };
        copy(src_output, output_offset, output_buffer, output_size);
        if (args_buffer) {
            copy(src_args, src_args_offset, args_buffer, args_size);
            // The cached output starts at its own word 0: rebase the arena-base word the
            // mesh dispatch and the draw read their blocks from, with a one-word copy.
            const VkBufferMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = args_buffer,
                .offset = Shader::Backend::SPIRV::MESH_EMULATION_ARGS_BASE * sizeof(u32),
                .size = sizeof(u32),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, barrier);
            const VkBufferCopy rebase{.srcOffset = zero_offset,
                                      .dstOffset =
                                          Shader::Backend::SPIRV::MESH_EMULATION_ARGS_BASE *
                                          sizeof(u32),
                                      .size = sizeof(u32)};
            cmdbuf.CopyBuffer(zero_buffer, args_buffer, vk::Span<VkBufferCopy>{rebase});
        }
        if (counts_buffer) {
            copy(src_counts, src_counts_offset, counts_buffer, counts_size);
        }
        if (commands_buffer) {
            copy(src_commands, src_commands_offset, commands_buffer, commands_size);
        }
        // From here the entry is read-only: order the copies before the vertex shader reads
        // and the draw's indirect reads, exactly as a miss orders its dispatches.
        barrier_for(output_buffer, 0, output_size, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
        if (args_buffer) {
            barrier_for(args_buffer, 0, args_size, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        }
        if (commands_buffer) {
            barrier_for(commands_buffer, 0, commands_size, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        }
    });
    entry.ready_tick = scheduler.CurrentTick();
    entry.last_use_tick = entry.ready_tick;

    const auto existing = entries.find(pending.key);
    if (existing != entries.end()) {
        total_bytes -= existing->second.bytes;
        lru.erase(existing->second.lru);
        entries.erase(existing);
    }
    lru.push_back(pending.key);
    auto it = entries.emplace(pending.key, std::move(entry)).first;
    it->second.lru = std::prev(lru.end());
    total_bytes += it->second.bytes;
    return true;
}

void MeshExpansionCache::EvictFor(u64 extra_bytes) {
    while (total_bytes + extra_bytes > MaxBytes && !lru.empty()) {
        const auto it = entries.find(lru.front());
        if (it == entries.end()) {
            lru.pop_front();
            continue;
        }
        if (!scheduler.IsFree(it->second.last_use_tick)) {
            break;
        }
        total_bytes -= it->second.bytes;
        entries.erase(it);
        lru.pop_front();
    }
}

void MeshExpansionCache::NoteDraw(bool hit, bool has_task, bool compact,
                                  const MeshExpansionKey& key) {
    ++stat_draws;
    if (hit) {
        ++stat_hits;
        stat_skipped += (has_task ? 3 : 1) + (compact ? 1 : 0);
        stat_hit_keys.insert(key);
    } else {
        ++stat_misses;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - stat_window >= std::chrono::seconds{5}) {
        LogStats();
        stat_draws = stat_hits = stat_misses = stat_skipped = 0;
        stat_hit_keys.clear();
        stat_window = now;
    }
}

void MeshExpansionCache::LogStats() {
    LOG_INFO(Render_Vulkan,
             "Mesh expansion cache: {} mesh draws, {} hits ({} distinct draws), {} misses, {} "
             "dispatches skipped; {} entries, {} MiB cached",
             stat_draws, stat_hits, stat_hit_keys.size(), stat_misses, stat_skipped,
             entries.size(), total_bytes >> 20);
}

} // namespace Vulkan
