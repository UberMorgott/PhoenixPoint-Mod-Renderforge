# Renderforge

Upscaling, anti-aliasing, sharpening, a DirectX 12 renderer switch, frame-rate control and a
benchmark overlay for Phoenix Point.

## Features

- **NVIDIA DLSS Super Resolution and DLAA** (DLSS SDK 310.7, shipped runtime `nvngx_dlss.dll`
  310.7.129, NVIDIA-signed, transformer model) on DirectX 11 **and** DirectX 12.
- **AMD FidelityFX Super Resolution** — one signed AMD runtime that carries FSR 4.1.1 (ML, RDNA 3/4)
  and the FSR 3.1.5 fallback for every other GPU. DirectX 12 only.
- **Intel XeSS** 2.0.2 (XeSS SDK 3.0.2), cross-vendor. DirectX 12 only.
- **Renderer switch** between DirectX 11 and DirectX 12 from inside the options screen.
- Quality modes per provider: Off, Auto, DLAA / Native AA, Quality, Balanced, Performance,
  Ultra Performance.
- Auto picks by output height — 1200p or lower: DLAA; 1201p–1600p: Quality; above 1600p: Performance.
- Change modes while playing; only the **renderer** and the **upscaler vendor** need a restart.
- Native pickers in **Options → Graphics** (RENDERER, UPSCALER, FRAME GENERATION, QUALITY, SHARPNESS).
- Anything that cannot run right now is greyed out and says why when you hover it.
- Automatically disables SMAA while an upscaler is active.
- Automatic texture mip bias in upscaling modes: `log2(render resolution / output resolution)`.
- NVIDIA Image Scaling sharpen-only pass, 0–100.
- Removes Phoenix Point's fixed 60 FPS cap; optional 30–300 FPS limit, mutually exclusive with VSync.
- Benchmark overlay: renderer, upscaler, mode, render → output resolution, anti-aliasing, FPS, frame time.
- Stays dormant and hides its UI when nothing it offers can run.

Renderforge affects the main tactical and geoscape camera. The interface stays at native output resolution.

## Requirements

- Windows
- Phoenix Point for Steam; other stores are currently untested
- For DLSS: an NVIDIA GeForce RTX 20-series or newer, on a recent Game Ready driver
- For FSR and XeSS: DirectX 12 (`Renderer` → `DirectX 12`), any modern GPU
- FSR 4.1.1's ML path additionally needs a Radeon RX 7000/9000-class GPU; everything else falls back
  to FSR 3.1.5 automatically

## Install — which zip do I need?

Every zip extracts into `<Phoenix Point>\Mods\` and contains a single `Renderforge` folder, so you can
extract **Core plus any vendor packs** on top of each other in any order.

| Your GPU | Download |
|---|---|
| NVIDIA GeForce RTX | `Renderforge-Core-<v>.zip` + `Renderforge-NVIDIA-<v>.zip` |
| AMD Radeon | `Renderforge-Core-<v>.zip` + `Renderforge-AMD-<v>.zip` |
| Intel Arc | `Renderforge-Core-<v>.zip` + `Renderforge-Intel-<v>.zip` |
| Anything else, or you want them all | `Renderforge-Full-<v>.zip` (Core + all three vendors) |

`Renderforge-Core-<v>.zip` on its own still gives you the renderer switch, the FPS limit, the mip bias
and the overlay — it just has no upscaler to run. A vendor pack on its own does nothing without Core.
Checksums for every asset are in `SHA256SUMS.txt` on the release page, and each zip carries a
`manifest-<pack>.json` listing every DLL's version, SHA-256 and licence.

### Steam Workshop

1. [Subscribe on Steam Workshop](STEAM_WORKSHOP_URL) — the Workshop item is the **Full** bundle.
2. Start Phoenix Point.
3. Open **Main menu → Mods** and enable **Renderforge**.
4. Restart the game once.

### Manual installation

1. Download the zips from the [releases page](https://github.com/UberMorgott/PhoenixPoint-Mod-Renderforge/releases).
2. Extract each of them into:

   ```text
   <Phoenix Point>\Mods\
   ```

3. Confirm the result, e.g. for Core + NVIDIA:

   ```text
   Mods\Renderforge\
     Renderforge.dll
     RenderforgeNative.dll
     nvngx_dlss.dll
     meta.json
     README.md
     LICENSE
     LICENSE-NIS.txt
     LICENSE-NVIDIA.txt
     manifest-core.json
     manifest-nvidia.json
   ```

4. Start Phoenix Point.
5. Open **Main menu → Mods** and enable **Renderforge**.
6. Restart the game once.

Do not add an extra nested `Renderforge` directory. On first launch the mod copies
`RenderforgeNative.dll` into `PhoenixPointWin64_Data\Plugins\x86_64\` — Unity only wires its DirectX 12
interface for plugins loaded from there — which is why that one restart is needed.

## What runs where

| Feature | DirectX 11 | DirectX 12 | Pack |
|---|---|---|---|
| DLSS Super Resolution / DLAA | yes | yes | NVIDIA |
| FSR 4.1.1 (ML) / 3.1.5 | no | yes | AMD |
| XeSS 2.0.2 | no | yes | Intel |
| NVIDIA Image Scaling sharpening | yes | yes | Core |
| Texture mip bias | yes | yes | Core |
| Frame-rate limit, uncapped FPS, overlay | yes | yes | Core |
| DLSS Frame Generation / MFG (Streamline) | no | yes (2x RTX 40; 2x-4x RTX 50) | NVIDIA |
| FSR Frame Generation (FidelityFX) | no | yes (2x) | AMD |
| XeSS Frame Generation (+ XeLL) | no | yes (2x; more on Intel Arc) | Intel |
| Ray Reconstruction | no | no | — |

## Settings

### Renderer

`Renderer` (Options → Graphics, or Mods → Renderforge) picks the graphics API: `DirectX 11` (default)
or `DirectX 12 (experimental)`. FSR, XeSS and frame generation need DirectX 12 — on DirectX 11 those
entries stay greyed and say so when you hover them.

Changing it needs a restart, because Unity picks the API from the command line. Press APPLY and answer
`Yes`: the game closes and relaunches itself with `-force-d3d12` added to whatever it was started with.
Answer `No` and the row shows "(restart pending)" until you restart yourself. If you launch from Steam,
the mod offers the same restart once per session — or set it permanently in Steam → Library →
right-click Phoenix Point → Properties → Launch Options:

```
-force-d3d12 -mods
```

DirectX 12 is experimental. Ambient occlusion runs in SAO mode there and HDR colour grading uses a 2D
LUT (the game's 3D-LUT compute shader has no DirectX 12 build). To go back, set `Renderer` to
`DirectX 11` and restart (or remove `-force-d3d12` from your launch options). Report any crash with
your `Player.log`.

### Upscaler

`Upscaler` is `Auto` / `Off` / `DLSS` / `FSR` / `XeSS`. `Auto` resolves from your hardware: on
DirectX 11, DLSS on NVIDIA and nothing otherwise; on DirectX 12, DLSS on NVIDIA, else FSR when the AMD
pack is installed. The shim latches the vendor at start-up, so **changing the upscaler needs a restart**;
switching quality mode does not.

| Setting | Location | Default |
|---|---|---:|
| Renderer | Options → Graphics; Mods → Renderforge | Auto (= DirectX 11) |
| Upscaler | Options → Graphics; Mods → Renderforge | Auto |
| Quality mode | Options → Graphics; Mods → Renderforge | Auto |
| Frame generation | Options → Graphics; Mods → Renderforge | Off |
| Sharpness | Options → Graphics; Mods → Renderforge | 40 |
| Frame rate limit | Options → Screen; Mods → Renderforge | Off |
| Max FPS | Options → Screen; Mods → Renderforge | 60 |
| Show DLSS in Graphics options | Mods → Renderforge | On |
| DLSS on/off key | Mods → Renderforge | U |
| Overlay key | Mods → Renderforge | O |
| Show benchmark overlay | Mods → Renderforge | Off |
| Overlay position | Mods → Renderforge | Top Center |
| Overlay scale | Mods → Renderforge | 1.0 |
| Debug view | Mods → Renderforge | None |

### Frame generation (DirectX 12 only, experimental)

Options -> Graphics -> **FRAME GENERATION**: `Off / 2x / 3x / 4x`.

| Provider | Used on | Multipliers |
|---|---|---|
| DLSS-G / MFG (NVIDIA Streamline) | NVIDIA RTX | 2x on RTX 40, up to 4x on RTX 50 |
| FSR Frame Generation (AMD FidelityFX) | any DirectX 12 GPU | 2x |
| XeSS-FG (Intel, needs XeLL) | any DirectX 12 GPU | 2x (more on Intel Arc) |

The mod picks the provider automatically: DLSS-G on NVIDIA, FSR-FG elsewhere. Frame generation needs the
DirectX 12 renderer and an upscaler switched on; the row is greyed with the reason when it cannot run.
Multipliers above what the GPU supports are greyed individually. The benchmark overlay then shows
`FPS: 62 / 118` -- really rendered frames, then frames actually presented.

Frame generation adds latency and can show artefacts on fast camera moves. It is off by default.
Windowed and borderless only -- switching to exclusive fullscreen tears the FG chain down cleanly.

**NVIDIA focus note.** DLSS-G generates frames only while the game window is in the foreground. If you
Alt-Tab away, you see only real frames (no error, no crash); bring the window back and generation resumes.

**Vendor files.** The vendor zips carry redistributable, vendor-signed runtime DLLs:

- **AMD**: `amd_fidelityfx_framegeneration_dx12.dll` (in addition to the upscaler DLLs)
- **Intel**: `libxess_fg.dll`, `libxell.dll` (in addition to `libxess.dll`)
- **NVIDIA**: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`,
  `nvngx_dlssg.dll` 310.7.129 (in addition to `nvngx_dlss.dll`)

Install only the vendor pack for your GPU if you prefer; anything missing simply greys the corresponding
option.

The Sharpness control is disabled when the upscaler is Off. A value of 0 skips sharpening.

With **Frame rate limit** disabled the game is uncapped. Enabling the limit disables VSync; enabling
VSync disables the Renderforge limit. **Max FPS** accepts 30–300.

## Hotkeys

The modifiers are fixed; the letter can be changed under **Mods → Renderforge**.

| Action | Default hotkey |
|---|---|
| Toggle the upscaler Off/on | `Ctrl+Alt+U` |
| Show/hide benchmark overlay | `Ctrl+Alt+O` |

The toggle restores the last active mode. If the game starts Off with no previous mode remembered, it
restores Auto.

## Benchmarking tip

Press `Ctrl+Alt+O` for the overlay. It reports the renderer, the active upscaler and its version, the
mode, render → output resolution, the anti-aliasing method, average FPS and frame time. For useful
comparisons, stand in the same scene and camera position, let the reading settle, then capture Off,
DLAA and one upscaling mode at the same output resolution and graphics settings.

## Known limitations

- Bloom, depth of field, screen-space reflections and ambient occlusion run at the lower render
  resolution, before upscaling.
- Particles and shader-animated vegetation can ghost because they provide no reliable motion vectors.
- NVIDIA NGX writes several log files into `Mods\Renderforge`.
- DirectX 12 loses ACES 3D-LUT colour grading (2D LUT instead) and runs SAO instead of MSVO ambient
  occlusion.

## Troubleshooting

### "Native plugin staged — restart the game"

Expected once, after installing or updating. The mod copied `RenderforgeNative.dll` into
`PhoenixPointWin64_Data\Plugins\x86_64\`; Unity only loads it from there at start-up. Restart.

### A row is greyed and says "DLL missing: … — install the … pack"

You installed Core without that vendor's pack (or extracted the pack somewhere else). Download the
named pack from the releases page and extract it into `<Phoenix Point>\Mods\`, so its files land next
to `Renderforge.dll`.

### A row says "Requires DirectX 12 — switch Renderer"

FSR, XeSS and frame generation are DirectX 12 only. Set `Renderer` → `DirectX 12`, APPLY, restart.

### Renderforge is dormant

Confirm the mod is enabled, the game was restarted after installation, and that your GPU/driver can run
at least one of the packs you installed. For DLSS, check:

```text
<Phoenix Point>\Mods\Renderforge\nvsdk_ngx.log
```

Unsupported hardware or an outdated driver makes Renderforge log one availability message, hide its UI
and otherwise leave the game unchanged.

### Black screen after enabling an upscaler

Update to the latest Renderforge release and the latest GPU driver. You can also press `Ctrl+Alt+U` to
switch the upscaler off.

### FPS is still limited to 60

In **Options → Screen**, disable VSync and either disable **Frame rate limit** or set **Max FPS** above
60. External driver or overlay limiters may impose their own cap.

### The pickers are missing

Open **Mods → Renderforge** and enable **Show DLSS in Graphics options**. Also confirm the mod is
enabled and the game was restarted after installation. For a manual installation, verify that
`Renderforge.dll`, `RenderforgeNative.dll`, `meta.json` and at least one vendor DLL are directly inside
`Mods\Renderforge`.

### DirectX 12 crashes or looks wrong

DirectX 12 is experimental in Unity 2019.4. Switch `Renderer` back to `DirectX 11` and attach your
`Player.log` to a GitHub issue.

## Building from source

Requirements:

- Windows
- [.NET SDK 8](https://dotnet.microsoft.com/download/dotnet/8.0)
- Visual Studio 2022 Build Tools with the C++ x64 toolchain
- [CMake](https://cmake.org/)
- Phoenix Point installation
- Vendor SDKs beside the repo, in `..\refs\`: [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) →
  `refs\DLSS-sdk`, [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) →
  `refs\FidelityFX-SDK`, [Intel XeSS](https://github.com/intel/xess) → `refs\XeSS-sdk`,
  [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) → `refs\Streamline`

Vendor runtimes are not stored in this repository; the build copies them from `refs\` and verifies each
one's Authenticode signature first. NVIDIA Image Scaling headers are vendored under `native\nis\`.

```powershell
# managed component
dotnet build Renderforge.csproj -c Release /p:PPRoot="<Phoenix Point install>"

# native shim + its offline NGX/FSR probe
.\build-native.ps1

# build + deploy the complete bundle into <Phoenix Point install>\Mods\Renderforge
.\deploy.ps1 -PPRoot "<Phoenix Point install>"

# build the release zips into build\release\
.\build\release.ps1
```

`native\probe\dlss_probe.exe` is an offline self-test: it creates a D3D11 or D3D12 device and checks
NGX initialisation, DLSS feature creation, NIS sharpening and FSR context creation without starting
Phoenix Point.

Release procedure: `docs\RELEASING.md`.

Source repository: [UberMorgott/PhoenixPoint-Mod-Renderforge](https://github.com/UberMorgott/PhoenixPoint-Mod-Renderforge)

Mod ID: `com.morgott.Renderforge`

## Credits and licences

Renderforge is developed by Morgott.

- Renderforge code is licensed under the [MIT License](LICENSE).
- `nvngx_dlss.dll`, `nvngx_dlssg.dll`, the `sl.*.dll` Streamline runtimes and the NGX/Streamline headers
  are covered by the NVIDIA RTX SDKs licence in [LICENSE-NVIDIA.txt](LICENSE-NVIDIA.txt) and are
  redistributed unmodified as permitted. "This software contains source code provided by NVIDIA
  Corporation."
- NVIDIA Image Scaling is licensed under the MIT License in [LICENSE-NIS.txt](LICENSE-NIS.txt).
- `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` and
  `amd_fidelityfx_framegeneration_dx12.dll` are AMD FidelityFX SDK binaries, redistributed unmodified
  under [LICENSE-AMD.txt](LICENSE-AMD.txt).
- `libxess.dll`, `libxess_fg.dll` and `libxell.dll` are Intel XeSS SDK binaries, redistributed
  unmodified under [LICENSE-INTEL.txt](LICENSE-INTEL.txt).
- NVIDIA, DLSS, GeForce, RTX; AMD, Radeon, FidelityFX, FSR; Intel, Arc, XeSS — and their respective
  logos — are trademarks of their respective owners. Renderforge is not affiliated with, endorsed by or
  sponsored by NVIDIA, AMD, Intel or Snapshot Games.
