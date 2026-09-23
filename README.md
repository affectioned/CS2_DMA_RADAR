CS2 DMA Radar
---
An educational overlay for Counter-Strike 2 built with ImGui and DirectX 11. Demonstrates hardware-accelerated memory analysis using PCILeech/MemProcFS with FPGA devices. **Read-only — performs no writes.**

**Maintained by affectioned** — GrimApostles no longer maintains this project and has deleted the original repository. This is now an independent continuation, not a fork.

**Questions or feedback? Discord: @grimapostles** (original author)

<img width="2042" height="1305" alt="image" src="https://github.com/user-attachments/assets/ac6f3429-5919-43c7-9649-2d81fef98252" />


---

> **Disclaimer**
>
> Provided strictly for educational and research purposes. Demonstrates hardware memory analysis via FPGA, DirectX 11 rendering with ImGui, and runtime data resolution techniques. Usage in online games may violate Terms of Service. **You are solely responsible for how you use this software.**

---

Building
---
Open `CS2_DMA_RADAR.sln` in **Visual Studio 2022 (v143)**, set **Release x64**, build. Output goes to `bin/Release/`.

> **Note:** Debug builds use the release CRT (`/MD`) to match vendor libraries. Do not add `_DEBUG` back to the Debug preprocessor defines.

Runtime Dependencies
---
All runtime dependencies are **automatically downloaded** on first launch — no manual setup required.

- **MemProcFS DLLs** (`vmm.dll`, `leechcore.dll`, FPGA drivers) — bootstrapped from the [latest MemProcFS release](https://github.com/ufrisk/MemProcFS/releases). The DLLs are delay-loaded so the exe starts cleanly without them present.
- **Textures** (`textures/maps/`, `textures/icons/`) — bootstrapped from the repository archive. No need to run the Python extractor manually.
- **Visual C++ Redistributable 2015-2022 (x64)** — the only manual prerequisite. [Download here](https://aka.ms/vs/17/release/vc_redist.x64.exe).

Tracy Profiling
---
Built-in performance profiling via [Tracy](https://github.com/wolfpld/tracy). Launch with the `--tracy` flag:

```
CS2_DMA_RADAR.exe --tracy        # 30-second capture (default)
CS2_DMA_RADAR.exe --tracy 60     # 60-second capture
```

This will:
1. Auto-download `tracy-capture.exe` and `tracy-csvexport.exe` from the latest Tracy release (one-time)
2. Start a headless capture in the background
3. On exit, export the trace to CSV and write `profile_report.txt` — an LLM-friendly report you can paste into a conversation for performance analysis

Tracy is compiled with `TRACY_ON_DEMAND` — zero overhead unless a profiler is connected. 16 zones are instrumented across the DMA loop, game context, entity parsing, and render pipeline.

> Tracy is enabled in Release builds only. Debug builds compile the macros as no-ops.

DMA Best Practices
---
The DMA layer follows [CyNickal's measured timing model](https://nicholascalcaterra.com/blog/dma-cheats-common-mistakes/):
- `-norefresh` on `VMMDLL_Initialize` — skips automatic cache refresh
- `VMMDLL_FLAG_NOCACHE` on live reads — ensures fresh data from hardware
- Scatter reads via `VMMDLL_Scatter` API — batched DMA operations
- `std::this_thread::yield()` in the hot loop — no artificial sleep
- Snapshot architecture — DMA thread writes snapshots, render thread reads them independently
