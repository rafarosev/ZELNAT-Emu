// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "common/game_presets.h"
#include "common/oot_intel_preset.h"
#include "common/settings.h"

TEST_CASE("Display filter defaults use ScaleForce and FXAA", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    CHECK(settings->scaling_filter.GetDefault() == Settings::ScalingFilter::ScaleForce);
    CHECK(settings->anti_aliasing.GetDefault() == Settings::AntiAliasing::Fxaa);
}

TEST_CASE("A game override keeps CPU inner invalidation on despite the skip hack",
          "[game_presets]") {
    auto& values = Settings::values;
    const bool skip = values.skip_cpu_inner_invalidation.GetValue();
    const bool keep = values.keep_cpu_inner_invalidation;
    values.skip_cpu_inner_invalidation.SetValue(true);
    values.keep_cpu_inner_invalidation = false;
    CHECK(Settings::SkipCpuInnerInvalidation());
    values.keep_cpu_inner_invalidation = true;
    CHECK_FALSE(Settings::SkipCpuInnerInvalidation());
    values.skip_cpu_inner_invalidation.SetValue(skip);
    values.keep_cpu_inner_invalidation = keep;
}

TEST_CASE("Ocarina of Time Intel-low preset applies only when accepted for OoT", "[game_presets]") {
    auto settings = std::make_unique<Settings::Values>();
    settings->resolution_setup.SetValue(Settings::ResolutionSetup::Res2X);
    CHECK_FALSE(Settings::ApplyOcarinaOfTimeIntelLowPreset(
        *settings, Settings::OcarinaOfTimeProgramId, false));
    CHECK_FALSE(Settings::ApplyOcarinaOfTimeIntelLowPreset(
        *settings, Settings::MarioKartWorldProgramId, true));
    CHECK(settings->resolution_setup.GetValue() == Settings::ResolutionSetup::Res2X);
    CHECK(Settings::ApplyOcarinaOfTimeIntelLowPreset(*settings,
                                                     Settings::OcarinaOfTimeProgramId, true));
    CHECK(settings->renderer_backend.GetValue() == Settings::RendererBackend::Vulkan);
    CHECK(settings->resolution_setup.GetValue() == Settings::ResolutionSetup::Res1_2X);
    CHECK(settings->gpu_accuracy.GetValue() == Settings::GpuAccuracy::Low);
    CHECK(settings->use_asynchronous_shaders.GetValue());
    CHECK(settings->astc_recompression.GetValue() == Settings::AstcRecompression::Bc3);
    CHECK(settings->vram_usage_mode.GetValue() == Settings::VramUsageMode::Conservative);
    CHECK_FALSE(settings->use_reactive_flushing.GetValue());
    CHECK(settings->vsync_mode.GetValue() == Settings::VSyncMode::FifoRelaxed);
    CHECK(settings->use_docked_mode.GetValue() == Settings::ConsoleMode::Handheld);
    CHECK(settings->scaling_filter.GetValue() == Settings::ScalingFilter::Fsr);
    CHECK(settings->anti_aliasing.GetValue() == Settings::AntiAliasing::None);
}
