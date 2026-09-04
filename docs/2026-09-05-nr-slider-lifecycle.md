# NR strength slider lifecycle

- Baseline: `3252a5f6d9a9fd08cb481ae487bc1b565fac8b83`.
- Symptom: dragging the four Neural Rendering strength sliders repeatedly tears down rendering; with FG enabled, its child window disappears and is recreated.
- Cause: `NeuralRenderingSupport.SettingsKey` included the four numeric strengths. `DlssDriver.Apply` treated each changed value as a structural change and called `BeginRelease`, releasing SR/NR and the FG child/swapchain.
- Fix: only NR mode, style and auto-mask remain in the rebuild key. For the same structural key, `Apply` submits current strengths to native code. Native `Configure` publishes a mutex-protected config; creation/evaluation use a local snapshot, with no NGX calls under that lock. Main-thread configuration no longer mutates render-thread `active_`.
- NR `UICorrection=0`, absence of the incorrect Backbuffer alias, and NR-only projection-jitter suppression are preserved. This remains an experimental/unofficial NR integration.

## Verification

- `.\build-native.ps1`: `build-native: OK`; five `PROBE OK` results (DLSS D3D11/D3D12, NR, FSR, XeSS), zero reported resource-state mismatches.
- `dotnet build .\Renderforge.csproj -c Release '/p:PPRoot=D:\Steam\steamapps\common\Phoenix Point'`: 0 warnings, 0 errors.
- The NR probe now changes all four strengths on the existing feature between evaluations. `dlss_probe.exe <build\out> --d3d12 --nr`: three successful NR evaluations with `featureAlive=1`; exactly five submissions (one create, four evaluates); `PROBE OK`.
- `git diff --check`: passed. Independent source review found no actionable findings in the five changed code files.
- Live controls: PPCLI opens the real HomeScreen options and Graphics panel, writes the four native `UnityEngine.UI.Slider.value` properties, and lets their existing `onValueChanged` listeners run. This is a drag-like event sequence, not physical pointer dragging.
- Per FG mode: 12 changes per slider with 16 ms waits, then 12 with 250 ms waits; four sliders, 96 samples. Each sample reads `RenderforgeMod.GetStatus`. Plan cleanup restores initial values.

| Normal launch control | FG | Samples | Not Live / present off | NR not alive | Child creates / shutdowns | Device errors |
|---|---|---:|---:|---:|---:|---:|
| Unmodified baseline | Off | 96 | 52 / 52 | 44 | 0 / 0 | 0 |
| Unmodified baseline | 2x | 96 | 49 / 49 | 33 | 64 / 65 | 0 |
| Patched | Off | 96 | 0 / 0 | 0 | 0 / 0 | 0 |
| Patched | 2x | 96 | 0 / 0 | 0 | 0 / 0 | 0 |

- Baseline was built from `git archive` of the exact commit in an isolated ignored directory; main was not reset. Both controls used `-mods -force-d3d12`, HomeScreen, DLAA, 2560x1440 borderless, NR Auto/Style0/auto-mask, 120 target FPS and identical saved strengths.
- Patched normal FG-on samples kept child `0000000000430F1C` and shadow `00000134B5C1F820` unchanged. All 192 patched normal samples stayed Live, present on, NR alive, with zero reported errors.
- A separate patched debug-log launch passed the same 192 samples. Each mode recorded 98 changed-strength submissions to the existing NR feature, including cleanup. These logs prove submitted parameter values, not the closed runtime's visual interpretation of them.

## Remaining acceptance limits

- The controlled slider sequences ran without generated frames (`mode=1`, `status=0x0`, `presented=1`, `generated=0`) in both baseline and patched launches. The later foreground control below confirms working generation in the patched build; a combined slider sequence with positive generated-frame counters has not been recorded.
- No composited desktop video was captured. Stable lifecycle/status counters and the native framebuffer screenshot do not prove absence of every visible flicker. Physical dragging and desktop-visible behavior still need observation with working frame generation.
- No claim is made about improved NR image/face quality. Structural setting changes still rebuild the generation by design.

## Deployment and local evidence

- Steam: `D:\Steam\steamapps\common\Phoenix Point`; patched deployment restored after the baseline control. Both native copies match the build output; managed copy matches Release output.
- Native SHA256: `0376F18E0B2C5C41BD7763BBD72C74BDB8FEB4C610C85A2BD79BA91D42893818`.
- Managed SHA256: `12C9A96B3945140AF3624FDEEDCFE99C74026617CFC57F88AA506BEAFB4298C1`.
- Baseline control SHA256: native `E4A55312B9B15D6149EC83609BFA34EBCE4A9F08FDECA5ED1E989EBB9565AAA0`; managed `D7DA97CB1B7FF53D05F4D2CA4D14E099A9426BDFCB55EDCE27D20BC6B939EE70`.
- Restored strengths: intensity `0.27`, local tone `0.55`, local structure `0.36`, skin structure `0.34`; FG `2x`, DLAA, target FPS `120`.
- Ignored local evidence directory: `build\fg-settings-diagnosis-20260905\`. Key files: `fix-controlled-summary.json`, `baseline-deploy-hashes.json`, `fix-final-deploy-hashes.json`, `fix-final-status.json`, `fix-final-config.json`, `fix-native-build.log`, `fix-managed-build.log`, `fix-nr-probe.log`, `fix-debug-full.log`, `fix-on-nr-delta.log`, `fix-off-nr-delta.log`.
- Reproduction scripts/results: `fix-open-menu.ps1`, `fix-slider-soak.ps1`, `baseline-control\` and `patched-control\` (plans, results and FG deltas). These local diagnostic artifacts are not shipped or committed. `docs/shots/` was not changed or staged.

## User visual confirmation

After deployment of c0e8ce2346f08249c4cc49b3f39c1ec104828988, the user confirmed that moving the neural-strength sliders no longer causes visible flicker. This closes the user-observed slider-flicker check.

## Foreground generation control

- Same patched Steam process, PID `3368`; fresh unsaved campaign, Geoscape, DLAA, FG 2x, NR strengths unchanged. No rendering setting changed during this control.
- `focus=game` reports thread input focus; native `fg=1` reports that the game's top-level window is the foreground window (`FgHost.cpp`). Streamline can return success without generating frames while in the background.
- After window activation, 14 PPCLI `RenderforgeMod.GetStatus` samples over 13 seconds all reported `fg=1`, `gen=Live`, `nrAlive=1`, `lastError=0`; render frames `5854 -> 6583` (+729). Foreground was sampled approximately once per second, not instrumented on every frame.
- The native log confirms sustained DLSS-G `presented=2`, `status=0x0`, `mode=1`: frame `7200` generated `2348`; frame `7500` generated `2648`; frame `9300` generated `4448`; frame `12300` generated `7448`.
- This closes the question whether the patched build can generate frames. It does not retroactively make the earlier background slider sequences a combined FG-generation/slider proof.
- Local evidence: `build/fg-foreground-start.json`, `build/fg-foreground-soak.json`, `build/fg-foreground-native-tail.log`. The process remained running.
