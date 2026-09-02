# Renderforge — D3D12 mode, multi-vendor upscaling, frame generation (design)

Date 2026-09-02. Status: approved by user in chat ("давай делай, тестируй"), phased.

## Goal

One graphics-options block where the player picks renderer, upscaler, quality and frame
generation from every vendor, and everything that cannot work on the current renderer is
greyed out with a hover tooltip saying why. Overlay shows the active API and real / generated
fps. Full set: DLSS SR + DLSS-FG/MFG, FSR 4 (+3.1 fallback) + FSR-FG, XeSS 3 SR + XeSS-FG.

## Hard facts this design rests on (all live-verified 2026-09-02, see DESIGN.md)

- Game = Unity 2019.4.31f1, BiRP, PPv2, D3D11 by default. `-force-d3d12` works (menu + tactical
  render). `-force-vulkan` is dead: no SPIR-V in the build (`InitializeEngineGraphics failed`).
- D3D12 washed-out look = PPv2 `PostProcessLayer` aborts every frame because two compute
  shaders have no D3D12 kernels: `MultiScaleVODownsample1` (AO, MSVO mode) and
  `KGenLut3D_AcesTonemap` (HDR ColorGrading 3D-LUT baker). Layer abort kills
  `FogOfWarPostProcess` + grading. Verified fix: AO off + `PostProcessResources.computeShaders.lut3DBaker=null`
  (→ LDR 2D-LUT path) restores fog of war and the dark cave. Seam:
  `Base.Lighting.LightingManager.ApplyPostProcessOptions` (`LightingManager.cs:163-187`).
- Residual under D3D12: `Mesh can not have more than 65000 vertices` (8x), and the process
  crashed 3x while idling during the RCA session (not yet known whether the exception spam or
  D3D12 itself caused it — Phase 1 must measure stability AFTER the fix).
- Vendor SDK matrix (current releases): DLSS SR works on D3D11+D3D12 (`nvngx_dlss.dll`
  310.7.129, shipped). DLSS-FG/MFG = Streamline 2.12.0, D3D12 only. FSR = FidelityFX SDK 2.3
  (`amd_fidelityfx_upscaler_dx12.dll` 27 MB = FSR 4.1.1 ML + 3.1.5 fallback in one DLL;
  `amd_fidelityfx_framegeneration_dx12.dll` 38 MB), D3D12 only, no official D3D11 backend.
  XeSS = SDK 3.0.1 (`libxess.dll` 74 MB cross-vendor D3D12; `libxess_fg.dll` 22 MB D3D12 only;
  `libxess_dx11.dll` is Intel-Arc-only → not used). Licences (AMD binary licence, Intel
  Simplified Software Licence, NVIDIA NGX/Streamline MIT) all allow unmodified binary
  redistribution with notices.
- GitHub: 100 MB is the per-file limit for git objects only; release assets ≤ 2 GB each.
  Binaries stay gitignored (already the case), ship as release zips. No LFS, no downloader.

## Scope

IN: renderer picker + restart flow, D3D12 PPv2 fix, greyed entries + tooltips, overlay API +
real/FG fps, D3D12 path in the native shim, DLSS on D3D12, FSR 4/3.1 SR, XeSS 3 SR,
FSR-FG, XeSS-FG, DLSS-FG/MFG, per-vendor release zips.
OUT: Vulkan (dead), XeSS D3D11 (Arc-only), community FSR-DX11 forks, driver-level FG (AFMF,
Smooth Motion), FSR 1/2, DirectSR.

## Architecture

```
Mods\Renderforge\
  Renderforge.dll            managed: Harmony patches, pickers, overlay, restart, config
  RenderforgeNative.dll      shim: ONE exported ABI, two device backends (D3D11, D3D12),
                             N upscaler providers, M frame-gen providers
  nvngx_dlss.dll             NVIDIA zip
  sl.*.dll, nvngx_dlssg.dll  NVIDIA zip (Streamline, FG)
  amd_fidelityfx_*_dx12.dll  AMD zip
  libxess.dll, libxess_fg.dll, libxell.dll   Intel zip
```

### Managed side

- `RendererMode` config + picker `Renderer: DirectX 11 / DirectX 12 (experimental)`. Selecting
  a different value than `SystemInfo.graphicsDeviceType` shows the game's native confirmation
  dialog ("restart required — restart now?"). Yes → `Process.Start(same exe, "-force-d3d12"
  + original args)` then `Application.Quit()`. No → stays selected, picker shows "(restart
  pending)". Under D3D12 the mod ALSO writes the flag into a marker (`ModConfig`) so that a
  plain Steam launch (D3D11) offers the restart again once; the user can instead paste
  `-force-d3d12` into Steam launch options (README).
- D3D12 PPv2 fix: Harmony postfix on `LightingManager.ApplyPostProcessOptions`: when
  `graphicsDeviceType == Direct3D12` → AO `enabled=false` (or SAO mode if it renders — test),
  `lut3DBaker = null`. Also swallow the `Mesh > 65000 vertices` source if it is ours to fix.
- Greyed entries: picker labels render with `CanvasGroup.alpha=0.35` exactly like the existing
  slider grey; hover tooltip uses the game's own tooltip widget (RESEARCHER finds it in the
  decompile — `UITooltip`/`TooltipTrigger`-like component the options panel already uses;
  never a custom overlay). Tooltip text: "Requires DirectX 12 — switch Renderer" /
  "Requires an NVIDIA RTX GPU" / "Not supported by this GPU" / "DLL missing: <name>".
- Availability model (one function, drives grey + tooltip + Auto): per provider
  `{api ok?, gpu ok?, dll present?, sdk init ok?}` → reason string or null.
- Overlay lines: `Renderer: D3D12` · `Upscaler: FSR 4.1.1 (Quality 1920x1080→2560x1440)` ·
  `FG: FSR 2x` · `FPS: 62 / 118 (16.1 ms)` = real / presented. Without FG: `FPS: 62 (16.1 ms)`.
- Picker set: `Renderer`, `Upscaler: Auto/Off/DLSS/FSR/XeSS`, `Quality` (list from the chosen
  provider: DLAA/Native AA, Ultra Quality (XeSS), Quality, Balanced, Performance, Ultra
  Performance), `Frame generation: Off/2x/3x/4x` (3x/4x only where the provider+GPU allows),
  `Sharpness` slider (NIS/RCAS post pass, provider-independent).
- Auto order D3D12: NVIDIA RTX→DLSS, AMD RDNA3/4→FSR4, Intel Arc→XeSS, else FSR 3.1 fallback
  (inside the same AMD DLL) → XeSS cross-vendor → off. Auto order D3D11: DLSS → off.

### Native shim

- Keep the exported ABI shape (init / optimal / create-params / set-frame / render-event /
  release / status), add `provider` + `api` fields to the init and status structs and a second
  export family for FG (`Fg_Init / Fg_SetResources / Fg_Enable / Fg_Status / Fg_Release`) —
  FG is swapchain/pacing-shaped, not upscaler-shaped.
- Device backend: `Dlss_Init` receives `anyNativeResource`; QI decides `ID3D11Resource` vs
  `ID3D12Resource` → `IDevice11` / `IDevice12` implementing the resource-state, UAV and
  command-list plumbing once; providers only call into the vendor SDK.
- Providers: `DlssProvider` (NGX D3D11 + D3D12), `FsrProvider` (ffx-api, D3D12),
  `XessProvider` (D3D12). FG providers: `FsrFgProvider`, `XessFgProvider`,
  `DlssFgProvider` (Streamline: needs `sl.interposer.dll` hooking DXGI factory/swapchain
  creation; because Unity already created the device, use Streamline's manual-hooking mode
  (`slSetD3DDevice` + `slUpgradeInterface` on the swapchain) — validate in Phase 5).
- Shared inputs every provider needs from Unity: colour (pre-UI), depth, motion vectors
  (current→previous, pixels, Y convention normalised per provider), jitter, exposure, reset
  flag, HUD-less colour + UI texture for FG.

### Packaging

- `build\release.ps1` produces `Renderforge-Core-<v>.zip` (managed + shim + NIS), and
  `Renderforge-NVIDIA-<v>.zip`, `-AMD-<v>.zip`, `-Intel-<v>.zip` (vendor DLLs + licence),
  `Renderforge-Full-<v>.zip` (all), `SHA256SUMS.txt`. Authenticode check for every signed
  vendor DLL at build time (NVIDIA already; AMD/Intel binaries are signed too — assert).
- Workshop upload = Full zip content. Nexus = same assets.

## Phases (each = own plan, tested in-game on Instance2 before the next)

1. **Renderer switch + D3D12 fix + tooltip infra + overlay API line.** Exit: D3D12 tactical
   screenshot matches D3D11 (dark cave, FoW), Player.log has no per-frame exceptions, 15-min
   idle + 3 mission loads without a crash; picker/restart flow works; greyed entries show the
   tooltip.
2. **DLSS on D3D12** (shim `IDevice12`, NGX D3D12). Exit: DLAA/Quality screenshots under D3D12
   equal D3D11 quality, overlay shows `Renderer: D3D12`.
3. **FSR 4/3.1 SR** (FidelityFX SDK 2.3). Exit: FSR Quality screenshot sane on the RTX 5070 Ti
   (3.1.5 fallback path) — FSR4 ML path is untestable here (no RDNA GPU), mark as such.
4. **XeSS 3 SR** (cross-vendor D3D12 path). Exit: same as 3 on the RTX.
5. **Frame generation**: FSR-FG first (cross-vendor, testable), then XeSS-FG (cross-vendor
   1x generated frame), then DLSS-FG/MFG (Streamline). Exit: overlay `FPS: real / presented`,
   HUD not smeared, input latency acceptable.
6. **Packaging + docs + release** (per-vendor zips, README matrix, licences).

SDK downloads (FidelityFX SDK 2.3, XeSS 3.0.1, Streamline 2.12.0) happen at the start of
Phase 3/4/5 respectively and need the user's explicit OK per download (source URL + size).

## Testing

- Every phase: PPCLI `start-mission.json` on Instance2 under both renderers, `connect
  screenshot`, Player.log unique-error diff vs the D3D11 baseline, overlay screenshot.
- Stability gate for D3D12: 15-minute idle + 3 mission loads, zero crashes.
- No FPS claims without the overlay screenshot showing the number.

## Risks

- Unity 2019.4 D3D12 stability (3 crashes seen during RCA). If the fixed build still crashes,
  D3D12 stays labelled experimental and the README says so; FG then remains experimental too.
- ACES HDR grading lost under D3D12 (LDR 2D-LUT path). Upgrade path: build an AssetBundle
  with the two PPv2 compute shaders compiled for D3D12 (needs Unity 2019.4.31 editor) and swap
  them into `PostProcessResources` at runtime — separate decision after Phase 1.
- Streamline expects to own DXGI creation; manual-hooking mode may not support a foreign
  swapchain → DLSS-FG could end up FSR-FG/XeSS-FG only. Decided by Phase 5's spike.
