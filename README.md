# Zelnat

**Zelnat** is an Intel-only fork of [Lichii](https://github.com/zortos293/Lichii) (by Zortos; itself based on Eglantine and Eden) — an experimental Nintendo Switch 2 emulator for **Windows**, tuned for Intel GPUs, including low-end iGPUs such as the UHD 620.

> [!CAUTION]
> Zelnat has no website. Download it **only** from this repository's releases page. (The same warning applies upstream: Lichii's only site is its compatibility page, and lookalike download sites such as lichii.org are not affiliated and may contain malware.)

This repository replaces the earlier Fritchii-based Zelnat: the fork was rebased onto Lichii, which already ships the AV1 software video decoding (libdav1d) and the compute fallback for task/mesh draws on GPUs without `VK_EXT_mesh_shader` that the Fritchii base lacked.

## What is different

- Intel GPUs are detected (`Device::IsIntel()`) and preferred at startup.
- An **Ocarina of Time — Intel low** preset is offered when the game boots: 0.5x resolution, low GPU accuracy, FSR scaling, conservative VRAM usage, handheld mode — aimed at making the game playable (10 FPS minimum target) on an i5-8250U + UHD 620 with 12 GB RAM. It applies only to the running session; saved settings are not changed.
- Branding, executables (`zelnat.exe` / `zelnat-cli.exe`), data folder (`%APPDATA%\zelnat`) and the built-in update checker all belong to this fork and its own releases (`rafarosev/ZELNAT-Emu`).

## Builds

Windows builds (clang-cl, Standard and PGO with a profile trained on Ocarina of Time) are produced by the `release-binaries` GitHub Actions workflow and published as pre-releases tagged `build-N`, with assets `zelnat-windows-x86_64.zip` and `zelnat-windows-x86_64-pgo.zip`.

## Credits

Zelnat stands on the work of the Lichii, Eglantine and Eden teams, and the wider yuzu/Sudachi lineage. This fork only adds the Intel-focused changes described in `INTEL_FORK_PLAN.md`. For upstream documentation and setup notes, see the [Lichii repository](https://github.com/zortos293/Lichii).

This software should not be used to play games you have not legally obtained. "Nintendo Switch" is a trademark of Nintendo. Zelnat is not affiliated with Nintendo in any way.
