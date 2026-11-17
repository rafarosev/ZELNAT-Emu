# Zelnat — Intel-only fork of Lichii

## Target

- PC: Windows, Intel i5-8250U, 12 GB RAM, **Intel UHD 620** (no `VK_EXT_mesh_shader`,
  no AV1 hardware decode, shared VRAM).
- Goal: **Ocarina of Time (Switch 2, program id `0400486024DEC000`) playable at a
  minimum of 10 FPS** in-game.
- Base: **Lichii by Zortos** (`zortos293/Lichii`, branch `nx2`, VERSION `0.1.0-rc1`),
  cloned shallow at commit `f0bf0cf`. This branch (`intel-only`, local) will
  **replace the whole content of `rafarosev/ZELNAT-Emu`** — the previous
  Fritchii-based Zelnat is discarded. Rafael creates no new repo.
- Previous round's reference implementation: `~/workspace/fritchii-intel`
  (commits v0.1–v0.4.1 on its `intel-only` branch).

## Audit — what Lichii already has (checked 2026-11-17, source only, no build)

### a. AV1 software decode (libdav1d) — ✅ upstream, nothing to port

- `src/video_core/host1x/ffmpeg.cpp` (`Decoder::CreateCodec`): for
  `AV_CODEC_ID_AV1` it selects `avcodec_find_decoder_by_name("libdav1d")`,
  with the comment "FFmpeg's own av1 decoder only decodes through a GPU hwaccel".
- `externals/ffmpeg/CMakeLists.txt`: the bundled FFmpeg for Windows (MSVC and
  MinGW) and Linux x86_64 is built with libdav1d "for Switch 2".
- This is exactly the fix the Fritchii round had to port *from* Lichii
  (fritchii-intel v0.4). On the UHD 620 the OoT intro cinematic decodes on CPU.

### b. Compute fallback for task/mesh draws — ✅ upstream, nothing to port

Lichii has a full **mesh emulation**: when a pipeline has task/mesh stages and
the device lacks `VK_EXT_mesh_shader`, the mesh program is rebuilt as a compute
dispatch whose output buffer is then drawn as plain triangles.

- `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` (~line 2722): the
  emulation path is taken when `!device.IsExtMeshShaderSupported()`;
  `MeshEmulationSetup` is filled in ~line 2915 (with a resource-limit check
  that logs "Mesh emulation skipped" if the compute stage limits are exceeded).
- `src/video_core/renderer_vulkan/vk_graphics_pipeline.h`:
  `MeshEmulationSetup`, `GraphicsPipeline::IsEmulatedMesh()`.
- `src/video_core/renderer_vulkan/vk_rasterizer.cpp`: emulated draws are
  dispatched; only non-emulated mesh draws on a device without the extension
  are skipped (the Fritchii behaviour for *all* mesh draws).
- `src/video_core/vulkan_common/vulkan_device.cpp`: a **Mesh Shaders** setting
  (`Settings::MeshShaderMode` Native/Compute) and
  `EDEN_NX2_FORCE_MESH_FALLBACK=1` can force the fallback even on capable GPUs;
  RDNA2 is forced onto it automatically. Fritchii had lost this fallback when
  it forked off — here it is upstream, maintained, and used by other GPUs too.

### c. Intel GPU handling — ⚠️ partial: workarounds yes, preference no

Present upstream (driver-id based, scattered):

- `src/video_core/vulkan_common/vulkan_device.cpp`: `is_intel_windows`
  suitability handling; a block for old Intel Windows drivers (~line 1787);
  `HasBrokenCompute` for Intel proprietary 0.405.x in `vulkan_device.h`.
- `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` (~line 691): Intel
  Windows/Mesa ANV get the driver pipeline cache treatment and
  `has_broken_spirv_clamp` on Intel Windows.
- `src/video_core/renderer_vulkan/vk_query_cache.cpp`: Intel Windows is in the
  low-end query-cache list. Release notes for this base also list Intel-specific
  work (faster OoT mesh-shader compilation on Intel, Intel Windows pipeline
  cache) — consistent with the code above.

Missing upstream (ported in v0.1):

- No `Device::IsIntel()` helper.
- GPU preference order in `SortPhysicalDevices`
  (`src/video_core/vulkan_common/vulkan_wrapper.cpp`) is still
  NVIDIA > AMD > Intel — on a hybrid laptop the Intel iGPU is *last*.

### d. Xbyak/dynarmic "offset is too big" — ⚠️ not settled by source comparison

- `src/core/arm/dynarmic/arm_dynarmic_64.cpp`: `code_cache_size` logic is the
  **same** as Fritchii's (512 MiB on x64, 128 MiB, 8 MiB tiers).
- The code cache is still one `VirtualAlloc(nullptr, size, MEM_RESERVE, …)`
  reservation in dynarmic's `block_of_code.cpp` — no near-the-executable
  placement in either tree.
- Lichii's in-tree dynarmic is much newer, though: it adds a **far-code**
  region (`BlockOfCode::EnableFarCode`, enabled from
  `arm_dynarmic_64.cpp` via `config.far_code = true`) that segregates
  fault/abort paths and exit stubs at the end of the cache, plus a fast
  dispatch table, RSB, compact block exits and host call/return. That changes
  JIT emission a lot, but it is a density/performance feature, **not** a fix
  for a >2 GiB displacement between the cache and the host code it calls.
- Verdict: if Fritchii's failure was the cache landing too far from the host
  executable under ASLR, Lichii can hit it too; if it came from a specific
  emission path of the older dynarmic, Lichii may be fine. **Only a run on
  Rafael's PC settles it.** If it recurs, the v0.5 idea from the Fritchii
  round applies: reserve the code cache near the executable (or shrink
  `code_cache_size`) in `block_of_code.cpp`.

### e. CI / release builds — ✅ Lichii's own workflow, adapted in v0.3

- Lichii ships `.github/workflows/release-binaries.yml` (GitHub Actions;
  `.forgejo/workflows/` only has lint/translation jobs). Triggers:
  `workflow_dispatch` (with `publish_release`) and pushes to
  `nx2-experimental`. The Fritchii-round workflow is **not** ported.
- Windows builds are **clang-cl + ThinLTO** (no MSVC/MinGW split like
  Fritchii's): `windows-x86_64` and `windows-x86_64-pgo`, the latter using
  `lichii-windows.profdata`, a profile trained on Ocarina of Time — the
  opposite of the Fritchii round's MinGW PGO with a Switch 1 profile.
- In a fork, the workflow runs as-is after enabling Actions; releases are
  created in the fork's repo, which is what the renamed update checker reads.
- v0.3 trimmed the matrix to the two Windows jobs and renamed the package
  step, artifact names (`zelnat-windows-x86_64[-pgo].zip`) and release notes.
  The Linux/macOS steps remain in the file but are inert (no matrix entries).

## Ported changes (local commits on `intel-only`)

- **v0.1** (`d981365`) — `Device::IsIntel()` in `vulkan_device.h`;
  Intel-first vendor order in `SortPhysicalDevices`; an Intel startup log
  line in `RendererVulkan::Report()` (reports mesh_shader support and the
  `EDEN_NX2_NO_MESH=1` escape hatch). Same three files as the Fritchii round.
- **v0.2** (`4fdf718`) — Ocarina of Time Intel-low preset, same values as the
  Fritchii round: Vulkan, 0.5x resolution, GPU accuracy Low ("GPU Fast"),
  asynchronous shaders, ASTC recompression BC3, conservative VRAM, reactive
  flushing off, FIFO Relaxed VSync, handheld mode, FSR, no anti-aliasing.
  Lichii restructured this area, so the port is shaped differently:
  `OcarinaOfTimeProgramId` joins the other program ids in
  `src/common/game_presets.h` (constants only — core code includes it), the
  apply-functions live in a new `src/common/oot_intel_preset.h`, and the
  once-per-session offer sits in `MainWindow::BootGame`
  (`src/lichii/main_window.cpp`, after per-game config load, session-only via
  `SetGlobal(false)`; saved settings untouched). Lichii's own global
  "Apply Recommended Settings" menu action is a different feature and is
  left alone. A unit test was added to `src/tests/common/game_presets.cpp`.
- **v0.3** (`be7de55`) — full rename Lichii → Zelnat on Lichii's own naming
  layer (see below).

**Not ported, deliberately:**

- Fritchii v0.4 (AV1/libdav1d): already upstream (audit a).
- Fritchii v0.4.1 (workflow packaging name fix): moot — Lichii's package
  step checked `lichii.exe`/`eden-cli.exe`; v0.3 renamed those checks to
  `zelnat.exe`/`zelnat-cli.exe` in Lichii's own workflow.
- Fritchii v0.2's "mesh compute-fallback port": already upstream (audit b).

## Rename (v0.3) — scope and boundary

Renamed to Zelnat: GUI/CLI/room/updater output names (`zelnat.exe`,
`zelnat-cli.exe`, `zelnat-room`, `zelnat-updater.exe`, incl. the updater
lookup in `update_dialog.cpp` and the version resources), data folder
(`EDEN_DIR "zelnat"` → `%APPDATA%\zelnat`), Qt application name, window
titles, About dialog and UI strings, Discord presence text, NSIS installer,
desktop/metainfo contents, icon assets (`dist/zelnat.ico/.icns/.png/
wordmark*`, referenced from the `.rc` files, `lichii.qrc` and CMake),
README, and the update checker (`CMakeModules/GenerateSCMRev.cmake` →
`rafarosev/ZELNAT-Emu`; `src/common/net/net.cpp` expects
`zelnat-<platform>[-pgo]` assets and sends `User-Agent: Zelnat`;
`src/tests/common/update_checker.cpp` updated to match).

Kept unchanged, for compatibility (same precedent as the Fritchii round):
log file name `eden_log.txt`, `EDEN_NX2_*` environment variables, internal
CMake target names (`lichii`, `lichii-cmd`, `lichii-updater`) and source
layout (`src/lichii/…` include paths, the `Lichii`→`Zelnat` C++ namespace
rename stayed inside the frontend), `lichii-windows.profdata`, the
compatibility site `lichii.zortos.me` and its reporter protocol
(`lichii_version` keys), and internal format tags (`lichii-profile-import-v1`
magic, `lichii-bug-*` bundle names, `lichii-ncz` temp prefix). The theme's
internal resource id `"lichii"` (folder paired with the "Zelnat Dark"
display name) also stays.

## Pending before the first build

1. Push: wipe `rafarosev/ZELNAT-Emu` and push this `intel-only` branch as
   its content (Rafael's decision; nothing has been uploaded yet).
2. Enable Actions on the repo if needed, then dispatch
   `release-binaries` with `publish_release=true`.
3. First run on the target PC — watch the log for:
   - the v0.1 Intel line (`mesh_shader=false` expected on UHD 620),
   - "Mesh emulation skipped" warnings (compute limits on the UHD 620 would
     silently drop mesh geometry),
   - any recurrence of "offset is too big" at game boot (audit d),
   - the OoT preset offer, and FPS past the intro cinematic (the AV1 fix
     area) into gameplay.

## Risks

- **JIT offset error may persist** (audit d) — the single biggest unknown;
  it killed both Fritchii builds on the target PC.
- **Nothing here is compiled yet.** The clone is shallow with submodules
  uninitialised; the port was done by source inspection only. First CI run
  is the first real compile check (the new test file and the preset header
  are the newest code).
- **Mesh emulation cost/limits on UHD 620**: the fallback is compute-heavy
  and bounded by per-stage descriptor limits; OoT at 0.5x with the preset
  is the mitigation, but the 10 FPS floor is not guaranteed.
- **12 GB RAM**: Lichii's own boot flow already warns on low RAM
  (`ConfirmLowRamBoot`); the 512 MiB JIT cache plus shader compilation on a
  12 GB machine with shared VRAM will be tight.
