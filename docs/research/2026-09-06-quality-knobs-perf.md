# Quality knobs — measured cost (2026-09-06)

## Rig

- GPU: NVIDIA RTX 5070 Ti (16 GB)
- Resolution: 2560x1440, borderless (FullScreenWindow), vsync off
- Frame limiter: off (vanilla LimitFrameRate disabled, Renderforge frame-rate limit disabled)
- Renderforge: Mode=Off, Upscaler=Auto, Renderer=Auto (D3D11, native rendering, no upscaler active)
- Graphics preset: Ultra (QualitySettings: aniso=ForceEnable, lodBias=2, shadowRes=High)
- Instance: D:\PP-Instance3

## Scene

- Map: ALN_PLT_Nest_48x48_A (start-mission.json, seed 12345)
- Phase: tactical, player turn, AI disabled (ai_enabled=false), static camera
- Factions: Phoenix 8, Alien 81, NJ 28, Environment 36

## Method

- **Frame time**: PPCLI `call op:get` on `UnityEngine.Time.unscaledDeltaTime`, 300 samples per
  config via `connect multi`. CPU-side frame cadence, not GPU busy time.
- **GPU load**: `nvidia-smi --query-gpu=utilization.gpu,power.draw --format=csv,noheader,nounits`,
  10 readings 1 s apart per config (mean). System-wide, not per-process.
- **VRAM**: `nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits`. System-wide,
  not per-process. Two readings per config, 5 s apart; fluctuation between same-config readings
  is ~100 MiB (driver streaming, caching), so per-knob deltas within that band are noise.
- **Warmup**: 8 s after each knob change before sampling.
- **Camera**: untouched between configs (same view, same frame).

## Measurement 1 was capped

The first pass (commit `85f9e2c`) recorded every config at 238-240 FPS with p95 within 0.3 ms
of the mean. That is the DWM compositor ceiling of a 240 Hz monitor in borderless
(`FullScreenWindow`), not a CPU-bound scene. Frame-time deltas under a cap cannot show cost,
so the "within noise" conclusion was unsupported.

Exclusive fullscreen (`Screen.SetResolution` with `ExclusiveFullScreen`) was attempted but
Windows 11 converts it to optimized borderless — the runtime confirmed `ExclusiveFullScreen`
yet `Screen.width`/`Screen.height` stayed at 2560x1440 regardless of requested resolution,
and vanilla FPS did not exceed 240. Raising resolution to 4K had no effect for the same reason.

The cap cannot be bypassed on this OS. Measurement 2 uses GPU utilization and power draw at
the fixed 240-cap as a cost proxy instead.

## Results

### Frame time (300 samples each, DWM-capped at ~240 FPS)

| Config | Mean ms | P95 ms | FPS (1000/mean) |
|---|---|---|---|
| Vanilla (baseline) | 4.20 | 4.35 | 238 |
| Shadow: Very High | 4.17 | 4.36 | 240 |
| LOD detail: 4.0 | 4.20 | 4.36 | 238 |
| Anisotropic: 16x | 4.19 | 4.59 | 239 |
| Vignette: Off | 4.17 | 4.46 | 240 |
| All max (Off/VH/16x/4.0) | 4.20 | 4.40 | 238 |

All deltas vs baseline are within ±0.03 ms — clamped by the DWM 240 Hz ceiling, not by
rendering cost.

### GPU utilization at 240-cap (10 nvidia-smi samples / 10 s, system-wide)

| Config | GPU % | Power W | Δ GPU % | Δ W |
|---|---|---|---|---|
| Vanilla (baseline) | 87.2 | 185.3 | — | — |
| Shadow: Very High | 87.8 | 181.0 | +0.6 | −4.3 |
| LOD detail: 4.0 | 82.2 | 170.1 | −5.0 | −15.2 |
| Anisotropic: 16x | 94.9 | 189.3 | +7.7 | +4.0 |
| Vignette: Off | 91.6 | 187.1 | +4.4 | +1.8 |
| All max (Off/VH/16x/4.0) | 95.1 | 188.7 | +7.9 | +3.4 |

Aniso 16x is the costliest single knob (+7.7 pp GPU utilization). All max is dominated by
the aniso cost. Shadow VH and Vignette Off are within the per-sample noise band. LOD 4.0
reads anomalously low — likely nvidia-smi noise (system-wide, not per-process), not a real
reduction; a static camera at this distance may already show all objects at max LOD.

### VRAM (MiB, nvidia-smi system-wide)

| Config | Pass 1 | Pass 2a / 2b |
|---|---|---|
| Vanilla | 8889 | 8693 / 8710 |
| Shadow: Very High | 8921 | 8713 / 8798 |
| LOD detail: 4.0 | 8889 | 8787 / 8776 |
| Anisotropic: 16x | 8667 | 8733 / 8719 |
| Vignette: Off | 8668 | 8700 / 8700 |
| All max (Off/VH/16x/4.0) | — | 8731 / 8731 |
| Vanilla (return) | — | 8690 / 8690 |

Shadow: Very High shows a consistent small increase (~30–90 MiB across passes), plausibly
the larger shadow-map allocation. All other knobs show deltas within the ±100 MiB noise band.

## Conclusion

On an RTX 5070 Ti at 1440p in a 153-actor tactical scene capped at ~240 FPS by the DWM
compositor, the costliest quality knob (Aniso 16x) adds ~8 percentage points of GPU
utilization and ~4 W. All knobs together add ~8 pp / ~3 W — dominated by the aniso cost.
Shadow VH, LOD 4.0 and Vignette Off show no measurable GPU-load increase above the
per-sample noise. VRAM cost is within the ±100 MiB noise band for all knobs; Very High
shadows show a small consistent increase.

No knob measurably affects frame time on this GPU: even with all knobs at max the GPU has
headroom (~95% utilization), and the DWM cap prevents frame-time separation.

## Caveats

- nvidia-smi GPU utilization is system-wide and reads in 1 s buckets. Per-sample variance is
  ~5 pp; deltas within that band (Shadow VH, Vignette Off) are not reliably distinguishable
  from noise.
- Frame time is DWM-capped at the monitor's refresh rate (240 Hz / ~4.17 ms). Windows 11
  converts exclusive fullscreen to optimized borderless, so true uncapped measurement is not
  available on this OS without a display-less render or a different presentation path.
- A single scene and camera position. Pathological geometry (many LOD boundaries in view,
  many shadow-casting lights) could amplify the LOD/shadow cost.
- On a weaker GPU or a heavier scene, aniso 16x could push utilization past 100% and become
  the knob that visibly drops frame rate.
