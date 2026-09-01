# Renderforge

DLSS Super Resolution, DLAA, sharpening, frame-rate control, and a benchmark overlay for Phoenix Point.

## Features

- NVIDIA DLSS Super Resolution and DLAA using DLSS SDK 310.7 with the transformer model.
- Modes: Off, Auto, DLAA, Quality, Balanced, Performance, and Ultra Performance.
- Auto selects by output height:
  - 1200p or lower: DLAA
  - 1201p–1600p: Quality
  - Above 1600p: Performance
- Change DLSS modes while playing; no restart is required.
- Native **DLSS** picker in **Options → Graphics**, between Texture quality and Shader quality.
- Automatically disables SMAA while DLSS is active.
- Automatic texture mip bias in upscaling modes: `log2(render resolution / output resolution)`.
- NVIDIA NIS sharpen-only pass after DLSS, adjustable from 0–100.
- Removes Phoenix Point's fixed 60 FPS cap.
- Optional 30–300 FPS limit, mutually exclusive with VSync.
- Benchmark overlay showing the upscaler, mode, render and output resolutions, anti-aliasing method, FPS, and frame time.
- Configurable overlay position, scale, and hotkey letters.
- Stays dormant and hides its UI on unsupported GPUs or drivers.

Renderforge affects the main tactical and geoscape camera. The interface remains at native output resolution.

## Requirements

- Windows
- Phoenix Point for Steam; other stores are currently untested
- DirectX 11, which is the game's default
- NVIDIA GeForce RTX 20-series or newer
- A recent NVIDIA Game Ready driver

## Install

Choose one installation method.

### Steam Workshop

1. [Subscribe on Steam Workshop](STEAM_WORKSHOP_URL).
2. Start Phoenix Point.
3. Open **Main menu → Mods** and enable **Renderforge**.
4. Restart the game once.

### Manual installation

1. Download [`Renderforge-1.0.0.zip`](https://github.com/UberMorgott/PhoenixPoint-Mod-Renderforge/releases/download/v1.0.0/Renderforge-1.0.0.zip).
2. Extract it into:

   ```text
   <Phoenix Point>\Mods\Renderforge\
   ```

3. Confirm the final layout is:

   ```text
   Mods\Renderforge\
     Renderforge.dll
     RenderforgeNative.dll
     nvngx_dlss.dll
     meta.json
   ```

4. Start Phoenix Point.
5. Open **Main menu → Mods** and enable **Renderforge**.
6. Restart the game once.

Do not place the files in an additional nested `Renderforge` directory.

## Settings

| Setting | Location | Default |
|---|---|---:|
| DLSS mode | Options → Graphics; Mods → Renderforge | Auto |
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

The Sharpness control is disabled when DLSS is Off. A value of 0 skips sharpening.

With **Frame rate limit** disabled, the game is uncapped. Enabling the limit disables VSync; enabling VSync disables the Renderforge limit. **Max FPS** accepts values from 30–300.

## Hotkeys

The modifiers are fixed; the letter can be changed under **Mods → Renderforge**.

| Action | Default hotkey |
|---|---|
| Toggle DLSS Off/on | `Ctrl+Alt+U` |
| Show/hide benchmark overlay | `Ctrl+Alt+O` |

The DLSS toggle restores the last active mode. If the game starts with DLSS Off and no previous mode is remembered, it restores Auto.

## Benchmarking tip

Press `Ctrl+Alt+O` to display the benchmark overlay. It reports:

- Active upscaler
- DLSS mode
- Render resolution → output resolution
- Anti-aliasing method
- Average FPS and frame time

For useful comparisons, stand in the same scene and camera position, let the reading settle, then capture screenshots in Off, DLAA, and one upscaling mode. Keep the same output resolution and graphics settings between captures.

## Known limitations

- Bloom, depth of field, screen-space reflections, and ambient occlusion run at the lower render resolution before upscaling.
- Particles and shader-animated vegetation can ghost because they do not provide reliable motion vectors.
- NVIDIA NGX writes several log files into `Mods\Renderforge`.

## Troubleshooting

### Renderforge is dormant

Confirm that the game is using DirectX 11, the system has a supported NVIDIA RTX GPU, and the NVIDIA driver is current. Check:

```text
<Phoenix Point>\Mods\Renderforge\nvsdk_ngx.log
```

Unsupported hardware or an outdated driver causes Renderforge to log one availability message, hide its UI, and otherwise leave the game unchanged.

### Black screen after enabling DLSS

Install the latest Renderforge release and update to the latest NVIDIA Game Ready driver. You can also press `Ctrl+Alt+U` to switch DLSS Off.

### FPS is still limited to 60

In **Options → Screen**, disable VSync and either disable **Frame rate limit** or set **Max FPS** above 60. External driver or overlay limiters may also impose their own cap.

### The DLSS picker is missing

Open **Mods → Renderforge** and enable **Show DLSS in Graphics options**. Also confirm that the mod is enabled, the game was restarted after installation, and the GPU and driver meet the requirements.

For a manual installation, verify that `Renderforge.dll`, `RenderforgeNative.dll`, `nvngx_dlss.dll`, and `meta.json` are directly inside `Mods\Renderforge`.

## Not included and roadmap

Renderforge 1.0.0 does not include:

- **Frame Generation:** DLSS Frame Generation requires DirectX 12; Phoenix Point runs on DirectX 11.
- **Ray Reconstruction:** the game has no supported ray-tracing pipeline for it.
- **DLSS 5 neural rendering:** it is not publicly released.
- **FSR or XeSS:** support is planned.
- **Color grading preset:** a natural-grey preset is planned for a later release.

## Building from source

Requirements:

- Windows
- [.NET SDK 8](https://dotnet.microsoft.com/download/dotnet/8.0)
- Visual Studio 2022 Build Tools with the C++ x64 toolchain
- [CMake](https://cmake.org/)
- Phoenix Point installation
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) cloned into `..\refs\DLSS-sdk`

The native shim links against `nvsdk_ngx_d.lib`. The runtime `nvngx_dlss.dll` is copied from the SDK during deployment and is not stored in this repository. NVIDIA Image Scaling headers are vendored under `native\nis\`.

Build the managed component:

```powershell
dotnet build Renderforge.csproj -c Release /p:PPRoot="<Phoenix Point install>"
```

Build the native shim and run its NGX test:

```powershell
.\build-native.ps1
```

The `native\probe\dlss_probe.exe` utility is an offline NGX self-test. It creates a D3D11 device and checks NGX initialization, DLSS feature creation, and NIS sharpening without starting Phoenix Point.

Build and deploy the complete bundle:

```powershell
.\deploy.ps1 -PPRoot "<Phoenix Point install>"
```

The deployment script builds both components and copies the resulting package into:

```text
<Phoenix Point install>\Mods\Renderforge
```

Source repository: [UberMorgott/PhoenixPoint-Mod-Renderforge](https://github.com/UberMorgott/PhoenixPoint-Mod-Renderforge)

Mod ID: `com.morgott.Renderforge`

## Credits and licenses

Renderforge is developed by Morgott.

- Renderforge code is licensed under the [MIT License](LICENSE).
- `nvngx_dlss.dll` and the NGX headers are covered by the NVIDIA RTX SDKs license in [LICENSE-NVIDIA.txt](LICENSE-NVIDIA.txt) and are redistributed as permitted. “This software contains source code provided by NVIDIA Corporation.”
- NVIDIA Image Scaling is licensed under the MIT License in [LICENSE-NIS.txt](LICENSE-NIS.txt).
- NVIDIA, DLSS, GeForce, and their respective logos are trademarks of NVIDIA Corporation.