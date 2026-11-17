# CS2 DMA Radar

Read-only overlay for Counter-Strike 2 that reads game state over FPGA DMA (PCILeech/MemProcFS) and draws a radar with ImGui + DirectX 11.

> **For offline modding and educational / research purposes only.** Demonstrates hardware memory analysis over DMA, Source 2 struct resolution, and ImGui/DX11 rendering. Running against online matches may violate a game's Terms of Service — you are solely responsible for what you do with it.

<img width="2042" height="1305" alt="screenshot" src="https://github.com/user-attachments/assets/ac6f3429-5919-43c7-9649-2d81fef98252" />

## Build

Open `CS2_DMA_RADAR.sln` in **Visual Studio 2022 (v143)**, select **Release x64**, build. Output lands in `bin/Release/`.

> Debug builds use the release CRT (`/MD`) to match vendor libraries. Don't re-add `_DEBUG`.

## Runtime

Everything is bootstrapped on first launch — the only manual prerequisite is the [Visual C++ 2015-2022 x64 redist](https://aka.ms/vs/17/release/vc_redist.x64.exe). On startup the exe pulls `vmm.dll` + `leechcore.dll` from the latest [MemProcFS](https://github.com/ufrisk/MemProcFS/releases) release and the `textures/` folder from this repo.

## Shareable web radar (optional)

ControlPanel → **Share → Enable** spins up a local HTTP + WebSocket server (default `:8787`) and streams an anonymized, spectator-POV radar to a browser at `http://<host>:8787/`. **Public tunnel** additionally spawns `cloudflared.exe` (drop it next to the exe, from [cloudflared releases](https://github.com/cloudflare/cloudflared/releases)) for a random `*.trycloudflare.com` link. The stream carries no "this is the sharer" marker; dots are standard T/CT yellow/blue.

Expect the public URL to be live evidence of a DMA session — treat it accordingly.

## Tracy (optional)

Launch with `--tracy [seconds]` for a 30s (or N-second) profiling capture. `tracy-capture.exe` is auto-downloaded; on exit the trace is converted to `profile_report.txt`. Zero overhead when no profiler is attached (`TRACY_ON_DEMAND`).

## Credits

- Original project by **GrimApostles** (Discord `@grimapostles`). This repo is the independent continuation after the original was deleted — not a fork.
- DMA timing model: [CyNickal's writeup](https://nicholascalcaterra.com/blog/dma-cheats-common-mistakes/).
- Struct offsets: [cs2-dumper](https://github.com/a2x/cs2-dumper).
