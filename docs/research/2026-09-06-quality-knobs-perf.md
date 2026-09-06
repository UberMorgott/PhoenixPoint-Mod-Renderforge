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
- Factions: Phoenix 8, Alien 74, NJ 29, Environment 32

## Method

- **Frame time**: PPCLI `call op:get` on `UnityEngine.Time.unscaledDeltaTime`, 300 samples per
  config via `connect multi`. This is CPU-side frame cadence (same as the Renderforge overlay's
  FPS line), not GPU busy time. PresentMon 2.5.1 saw 0 presents from Unity on this build
  (reported 2026-09-05), so CPU-side cadence is the only available whole-frame metric.
- **VRAM**: `nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits`. System-wide,
  not per-process. Two readings per config, 5 s apart; fluctuation between same-config readings
  is ~100 MiB (driver streaming, caching), so per-knob deltas within that band are noise.
- **Warmup**: 8 s after each knob change before sampling.
- **Camera**: untouched between configs (same view, same frame).

## Results

### Frame time (300 samples each)

| Config | Mean ms | P95 ms | FPS (1000/mean) |
|---|---|---|---|
| Vanilla (baseline) | 4.20 | 4.42 | 238 |
| Shadow: Very High | 4.17 | 4.53 | 240 |
| LOD detail: 4.0 | 4.19 | 4.50 | 239 |
| Anisotropic: 16x | 4.19 | 4.53 | 239 |
| Vignette: Off | 4.19 | 4.52 | 239 |

All deltas vs baseline are within ±0.03 ms — run-to-run noise at this frame rate.

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

On an RTX 5070 Ti at 1440p in a 143-actor tactical scene at ~238 FPS, every quality knob's
frame-time cost is within measurement noise (±0.03 ms). VRAM cost is within the system-wide
nvidia-smi noise band (~±100 MiB) for all knobs; Very High shadows show the most consistent
(but small) increase.

## Caveats

- CPU-side frame cadence, not GPU busy time. At ~4.2 ms/frame the GPU is unlikely to be the
  bottleneck on this GPU, so a GPU-time-specific cost could exist but be hidden by the frame
  time being CPU-bound. On a weaker GPU or a heavier scene the cost may become visible.
- nvidia-smi is system-wide. Other GPU consumers (desktop compositor, other apps) contribute
  to the reading and its noise.
- A single scene and camera position. Pathological geometry (many LOD boundaries in view,
  many shadow-casting lights) could amplify the LOD/shadow cost.
