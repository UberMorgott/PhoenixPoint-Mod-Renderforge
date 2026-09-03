# Handoff — Renderforge, start of next session (written 2026-09-03 ~01:00)

Read this first. Then `docs\superpowers\2026-09-02-handoff-multivendor.md` (phase status, tooling, rules).
Everything is committed on `main` of `E:\DEV\PhoenixPoint\Renderforge` (HEAD `a01afe5`, not pushed).
Steam install `D:\Steam\steamapps\common\Phoenix Point` has HEAD deployed (mod + shim in Plugins);
user profile `...591` ModConfig: `Renderer 2, LimitFrameRate false, FrameGen 0` (backup `ModConfig.json.bak-fps`).

## What landed after the multivendor handoff (2026-09-02 evening → 03 night)

| Commit | What |
|---|---|
| `20cd0e3` | FG: no provider drive without a prepared frame (menu had no camera → SL pacer wedged); idle path hides the child; TLS re-entry log |
| `915e341` | D3D12 2x-dark fixed: `outRT` created `RenderTextureReadWrite.Linear` on D3D12 (Unity's present Blit decodes sRGB read, skips encode). Menu luma 56.5 = D3D11 |
| `35f8549` | Live upscaler switch DLSS/FSR/XeSS without restart (`DlssDriver.SwitchProvider`, `RenderforgeMod.ReinitNative`); `IDevice::BeginDestroy/EndDestroy` fixes the `ffxDestroyContext` AV at quit |
| `a01afe5` | Present-hook recursion (user crash #2): immutable dxgi `Present`/`Present1` pointers, depth cap (≥3 → FG detached, no stack overflow), hook hidden during vendor `CreateSwapChain*`, log names the module that re-patched the slot (suspect: Steam `GameOverlayRenderer64.dll`, absent on Instance2/3) |

Earlier the same day: Phase 5 FG complete + 2 hardening rounds (see the multivendor handoff).

## Measured facts to carry

- **User rig**: RTX 5070 Ti, 2560x1440 @ 240 Hz, borderless `FullScreenWindow`, vsync off, NVCP cap removed.
  D3D11 tactical = 336 fps native / 320 Balanced / 310 UP (CPU-bound; DLSS cost ≈ output-res constant).
- **1440p no-vsync, Instance3** (docs shots `docs\shots\darkfps\`, untracked, 40 MB): D3D11 Off 114 tac; D3D12 shim-idle **207**;
  D3D12 DLAA **141** (−32%); D3D12 Quality 193; DLAA+FSR-FG X2 136/261; DLAA+DLSS-G X2 127/254.
  `DebugView.Passthrough` = 141 = DLAA → the DLSS evaluate is ~free; **the whole −32% is the RT redirect/copy plumbing**
  (Unity RT → owned twins → copy back). Sharpness 0 vs 40 = no difference.
- **Vanilla D3D12 without the mod does not run** (`MultiScaleVO` kernel not found every frame); `src\D3D12Fix.cs` is why it
  works at all → MSVO ambient occlusion is OFF under D3D12 (confound vs D3D11 numbers; visual difference for players).
- User crash #1 root cause: FG enabled in the MAIN MENU → no camera Prepare → SL pacer wedge. #2: hook recursion via a
  later vtable patch (Steam overlay). Both fixed; #2 fix NOT yet confirmed on the user's Steam process.
- Player.log is SHARED by all installs (LocalLow) — user evidence gets overwritten by agent runs. fg log
  `Mods\Renderforge\renderforge_fg.log` + `sl.log` are per-install and survived.
- Seen, not fixed: live resolution change under D3D12 → DLSS SR `eval=0xBAD00002 FAIL_PlatformError → DLSS off`,
  one Unity crash 20 s later (`Crash_2026-09-02_220020615`, no FG).

## Next steps, in order (say "продолжаем" → run these)

1. **USER TEST on Steam** (needs the user at the machine): D3D12, main menu → FG 2x (DLSS) → no crash, ratio 2.0 in the
   overlay (Ctrl+Alt+O, `FPS: real / presented`), then in a mission; Alt-Tab and back; upscaler switch live; brightness = D3D11.
   If it crashes: read `Mods\Renderforge\renderforge_fg.log` first — the new `hook: first re-entry … caller <module+off>`
   line names the re-patcher. Then fix accordingly (if it is the Steam overlay: consider installing our hook AFTER
   the overlay's, or detour-safe chaining).
2. **D3D12 copy-plumbing cost (−32% at DLAA)** — one fable coder, Instance3 at 1440p no-vsync: profile with GPU
   timestamps (`evalMs`, `copyInMs`, `copyOutMs`, `ringWaitMs` in `GetStatus`), then cut: feed Unity's colorRT/depthRT/mvRT
   directly to the SDKs using the measured deterministic pre-states (DESIGN.md owned-resource contract) instead of
   copying into twins, keep only the `out` twin; check the ring's wait on Unity's frame fence for CPU stalls. Gate:
   DLAA ≥ 190 fps tac (shim-idle 207), debug layer 0 on our lists, FSR/XeSS/DLSS all still correct.
3. **MSVO under D3D12** (`src\D3D12Fix.cs`): find why the compute kernel is missing on D3D12 (PPv2 `MultiScaleVO`
   compute shader variant not in the build for D3D12?) — if it is unfixable, document "AO off under DX12" in README
   and show it in the picker tooltip; if fixable (e.g. force the fallback SAO path), do it.
4. Resolution-change robustness under D3D12 (DLSS `FAIL_PlatformError` → regen instead of off).
5. Frame-pacing metric (PresentMon `MsBetweenDisplayChange` CoV) still unmeasured; resize under XeSS/DLSS-G not exercised.
6. Phase 6 Tasks 7-8 (version bump 1.2.0, `build\release.ps1 -WithFrameGen`, GitHub release, Workshop) — USER-GATED.
7. **BACKLOG — blurry UI** (Discord, d4reptile 2026-09-03): icons/text/sprites soft with visible bilinear interpolation
   at 1440p/4K. Hypothesis: Unity Canvas atlases + font authored for 1080p, `CanvasScaler` stretches them; UI composited
   after the scene so NIS/DLSS never touch it. Investigate: atlas texture filter/mip settings, `CanvasScaler` mode
   (`ScaleWithScreenSize` → reference res), font atlas size, a UI-only sharpen pass. Not promised to the user.
   FACTS (decompile 2026-09-03): text = legacy `UnityEngine.UI.Text` (643 fields / 221 files), zero `TextMeshProUGUI`
   → no SDF/vector swap. Legacy Text with a DYNAMIC font rasterizes at `fontSize × canvas.scaleFactor` = crisp; blur
   means one of: static bitmap font (fix: swap `Text.font` to dynamic TTF via Harmony), UI drawn into a fixed-size RT,
   or non-pixel-perfect canvas (`Canvas.pixelPerfect`). Match logic: `Base.UI.CanvasScalerController` (aspect → match).
   Step 1 = PPCLI diagnosis: `Font.dynamic` of used fonts, `Canvas.renderMode/scaleFactor/pixelPerfect`, UI camera RT.
   Icons: 1080p atlases, no cheap fix except a UI-only sharpen (CAS on the canvas layer).

## Rules that applied (keep)

- Coders `model:"fable"`, reviewers/planners/testers `opus`, Codex peer in every design discussion and review
  (`cx`, thread `01a062e8-cda9-79b3-b4f7-8aa9bf741017` for the FG architecture; start a fresh one if gone).
- Instances: Instance2 = profile `...592`, Instance3 = profile `...593`, run agents in parallel on both; the Steam
  install is deployed on the user's explicit ask only, never launched/killed by agents. Nothing downloaded to C:.
- fps measurements: 2560x1440 borderless, vsync 0, `LimitFrameRate false`, NO debug layer; luminance checks at any res.
- Greyed picker rows stay visible with the native tooltip reason. Commit on green with explicit `git add`.
- `docs\shots\darkfps\` is evidence, untracked (40 MB) — do not commit.
