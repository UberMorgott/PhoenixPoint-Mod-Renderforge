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
  works at all → MSVO replaced by SAO under D3D12 (AO stays on; minor confound vs D3D11 numbers).
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
2. ~~D3D12 copy-plumbing cost~~ MEASURED 2026-09-03 (`cdc2087` timings, Instance3 1440p, Nest_48x48 seed 12345):
   idle 184 / DLAA 159 / Quality 226; GPU `copyIn 0.08 eval 0.90 copyOut 0.02 ringWait 0.00` ms → **the DLAA cost is
   the DLSS evaluate itself (0.9 ms), copies are noise.** The earlier "−32% = plumbing" was scene-dependent/wrong.
   Direct SDK inputs gave +0 fps and crashed `ReinitNative` on provider switch → REVERTED (kept `Dlss_Timings` +
   overlay `GPU:` line). Next lever if wanted: DLSS preset (transformer J/K vs CNN E/F) exposed in config — eval
   0.9 ms is transformer-class; CNN ≈ half. Open: one crash DLSS→FSR switch AFTER FG teardown with copy path —
   re-test after the revert (scout).
3. ~~MSVO under D3D12~~ DONE (Opus research 2026-09-03): player built with D3D11 only → ComputeShader kernels absent
   for D3D12 (`ComputeShader.FindKernel` throws `Kernel 'MultiScaleVODownsample1' not found`, MultiScaleVO.cs:232;
   `AmbientOcclusion.IsEnabledAndSupported` only checks asset non-null). `D3D12Fix.FixAo` already forces SAO (pixel
   shader) → **AO is ON under DX12** (SAO, matches D3D11 per DESIGN.md:482). Real MSVO = rebuild 4 compute assets in
   Unity editor, not worth it. TODO 5 min: null `res.computeShaders.gaussianDownsample` in `D3D12Fix.Apply` (same
   false-positive gate in `ScreenSpaceReflections.IsEnabledAndSupported`, latent under Deferred).
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
   WEB (Codex 2026-09-03, full report `C:\Temp\cx\19b1e9395f984e3ea54a77d2bdcfafaa.out.md`): no existing PP mod fixes
   UI blur (only "Geoscape Objectives Font Size", nexusmods.com/phoenixpoint/mods/6). UI font = **Purista** (Suitcase
   Type Foundry, commercial, $39/style, no free/Google version → cannot bundle TTF; `Font.CreateDynamicFontFromOSFont`
   only if the user has it installed → fallback to a metric-similar free font, opt-in). Engine = Unity 2018.4.15f1
   (PCGamingWiki). Legacy-Text pattern reference: newman55/unity-mod-manager `UnityModManager/UI.cs`. UI-only CAS =
   UI camera → full-res transparent RT → CAS blit → composite (no ready package).
   RUNTIME FACTS (scout, Instance2, 2026-09-03; shots `docs\shots\uiblur\`, untracked): canvases `ScreenSpaceOverlay`,
   `CanvasScaler` ScaleWithScreenSize ref **3840x2160**, match 0 → scaleFactor 0.667 at 1440p; ALL 18 fonts dynamic
   (Purista Semibold, SourceHanSans…); no UI RT. Main atlas 4096² RGBA32 bilinear 13 mips (0.667 minified); standalone
   icons 64-150 px BC7 **1 mip**. Text effective px fractional (fontSize 35 → 23.33). A/B at 1440p: `pixelPerfect=true`
   = SAME; trilinear+aniso = SAME. → Not a one-liner. Remaining options: (1) UI-only CAS pass (Renderforge-native),
   (2) Harmony: snap `Text.fontSize` so fontSize×scaleFactor is integer (small layout drift), (3) icons drawn larger
   than their 64-150 px source = true upscale blur → only fixable by a replacement texture pack (offline 2x upscale).
   Check (3) first next time: on-screen px of those icons vs texture px.

## OPEN BUG (user report 2026-09-03): D3D12 output loses detail below DLAA

- User: D3D12 + any DLSS mode < DLAA = "large blurry fragments"; D3D11 same settings = native-perfect; D3D12 DLAA ok.
- Repro (scout, Instance2, shots `docs\shots\d3d12quality\`, untracked): PNG entropy D3D12 Quality 2553 KB vs D3D11
  4337 KB (−40%); **D3D12 DLAA also −6% vs D3D11 DLAA** → ratio-independent component. FSR/XeSS on D3D12 lose the
  same → input plumbing, not one SDK. NGX eval Success, jitter cycles, params byte-identical to D3D11.
- REJECTED with evidence: mv/depth twins sized from colorRT (all input RTs = RenderW×RenderH: color ARGB32, depth RFloat,
  mv RGHalf; out ARGB32 at screen); MV jitter residual (ProbeMv: mv < 1.2e-7 px on static geometry, both APIs); jitter
  latency (mv ≠ previous jitter either). MV debug-view "outlines" on D3D12 = something else (debug shader?), not data.
- CLUE: `MvJittered=true` (SDK subtracts reported jitter from MVs) makes D3D12 +7.6% sharper although MVs are zero →
  the jitter REPORTED to the SDK ≠ the jitter RENDERED on D3D12 (sign / y-flip / scale), matches on D3D11.
- Tooling landed: `b3acfbd` (`MvJittered` cfg + `RenderforgeMod.SetMvJittered/ProbeMv`), next commit: jitter report
  sign/scale/swap knobs + `DumpOut`/`DumpColorIn`. Plan: sweep report-sign × MvJittered on D3D12 Perf with a Laplacian
  sharpness metric, same sweep on D3D11; the optimum that differs between APIs is the root cause. Also diff `outRT`
  dumps (SDK output before Unity's present Blit) D3D11 vs D3D12 to split SDK-side loss from the present path
  (`outRT` Linear on D3D12, `915e341`; `D3D12Owned.h:82` maps sRGB→UNORM views — candidate for the ratio-independent 6%).

## Rules that applied (keep)

- Model routing (user 2026-09-03): code `model:"fable"`; research/RCA/design `opus` (Opus 5); web search Codex
  `cx -Search` + `web-scout` (Opus 4.6); review Codex `cx -Review` + `opus` reviewer — NEVER Fable for review/research
  (`cx`, thread `01a062e8-cda9-79b3-b4f7-8aa9bf741017` for the FG architecture; start a fresh one if gone).
  In-game test/measure runs = `scout` (Opus 4.6); a scout logs PPCLI defects to `E:\DEV\PhoenixPoint\PPCLI\ISSUES.md`.
- Instances: Instance2 = profile `...592`, Instance3 = profile `...593`, run agents in parallel on both; the Steam
  install is deployed on the user's explicit ask only, never launched/killed by agents. Nothing downloaded to C:.
- fps measurements: 2560x1440 borderless, vsync 0, `LimitFrameRate false`, NO debug layer; luminance checks at any res.
- Greyed picker rows stay visible with the native tooltip reason. Commit on green with explicit `git add`.
- `docs\shots\darkfps\` is evidence, untracked (40 MB) — do not commit.
