// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/game_presets.h"
#include "common/settings.h"

namespace Settings {

// Intel-only fork: low-end Intel iGPU preset (UHD 620 class, no mesh shaders,
// shared VRAM). Target: a playable floor in Ocarina of Time on 12 GB systems.
// Kept out of game_presets.h, which core code includes for the program ids
// alone and which must not pull in the full settings header.
inline void SetOcarinaOfTimeIntelLowRecommendedSettings(Values& settings, bool global) {
    const auto session = [global](auto& setting, auto value) {
        setting.SetGlobal(global);
        setting.SetValue(value);
    };
    session(settings.renderer_backend, RendererBackend::Vulkan);
    session(settings.cpu_accuracy, CpuAccuracy::Unsafe);
    session(settings.resolution_setup, ResolutionSetup::Res1_2X);
    session(settings.gpu_accuracy, GpuAccuracy::Low);
    session(settings.use_asynchronous_shaders, true);
    session(settings.astc_recompression, AstcRecompression::Bc3);
    session(settings.vram_usage_mode, VramUsageMode::Conservative);
    session(settings.use_reactive_flushing, false);
    session(settings.vsync_mode, VSyncMode::FifoRelaxed);
    session(settings.use_docked_mode, ConsoleMode::Handheld);
    session(settings.scaling_filter, ScalingFilter::Fsr);
    session(settings.anti_aliasing, AntiAliasing::None);
}

inline bool ApplyOcarinaOfTimeIntelLowPreset(Values& settings, u64 program_id, bool accepted) {
    if (!accepted || program_id != OcarinaOfTimeProgramId) {
        return false;
    }
    SetOcarinaOfTimeIntelLowRecommendedSettings(settings, false);
    return true;
}

} // namespace Settings
