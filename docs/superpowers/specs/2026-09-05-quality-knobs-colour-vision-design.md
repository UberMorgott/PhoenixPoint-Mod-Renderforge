# Quality knobs + Colour vision — design (2026-09-05, release 1.4.0)

Reviewed in two rounds with the Codex peer (thread `01a07235-0aee-7631-baff-b9fa194164c7`,
outputs `C:\Temp\cx\08f66ef3…out.md`, `C:\Temp\cx\b22cb28c…out.md`). Reflex standalone was
reviewed and moved to backlog (see §C).

## Verified facts this design rests on

- PPv2 tactical volume profile (runtime clone) holds AO(active), AutoExposure, Bloom, CA,
  ColorGrading(active), DoF, Grain, LensDistortion, MotionBlur, SSR, **Vignette(active)**.
  Grain/MotionBlur exist but are inactive → no knob for them.
- Vanilla Options → Graphics already exposes AO/Bloom/DoF/LensDistortion/SSR/CA/shadow distance
  via `LightingManager.ApplyPostProcessOptions` (`Base.Lighting\LightingManager.cs:163-197`).
- `QualitySettings` at Ultra: aniso ForceEnable, lodBias 2.0, shadowResolution **High**,
  cascades 4. Vanilla never writes aniso/lodBias/shadowResolution; `OptionsManager.UsePreset`
  (`OptionsManager.cs:384`) calls `SetQualityLevel(num, true)` and re-applies the quality asset.
  `UsePreset → ChangeGraphicsQuality → LightingManager` callback runs nested (`:391,:406`,
  `LightingManager.cs:53-55`).
- 11 of 71 lights cast shadows, all `FromQualitySettings`. Unity 2019.4 caps custom shadow maps
  (4096 dir / 2048 spot / 1024 point) → an "8192" tier is meaningless; dropped.
- Renderforge seams: `GraphicsPanel.cs:15` (row cloning, immediate apply + `SaveConfig`),
  `Patches.cs:19` postfix on `ApplyPostProcessOptions` → `DlssDriver.cs:536-541` (gated on
  upscaler active — NOT reused), `RenderforgeMod.cs:162 OnLevelStart`, `MipBias.cs` sweep
  (skips unchanged — NOT reused for aniso).
- LUT/style: analytic HLSL `native\Sharpen.cpp:44-64 Grade()`, post-tonemap post-upscale pre-UI;
  UNORM path is sRGB-encoded, D3D12 FP16 path is linear (`styleLinear`, `Sharpen.cpp:141,144`).
  Pass predicates that decide whether the sharpen/grade pass runs at all: `DlssDriver.cs:197`,
  `Device11.cpp:91`, `D3D12Sharpen.h:261`, `Sharpen.cpp:136` (+ FSR/XeSS paths).

## A. Quality knobs

New file `src\QualityKnobs.cs`, own Harmony patches. Config fields on `DlssConfig`
(`[ConfigField]` en/ru, hidden from the Mods menu like the other Graphics rows):

| Field | Values | Default | Effect |
|---|---|---|---|
| `Vignette` | Vanilla / Off | Vanilla | `profile.Vignette.enabled.value = false` |
| `ShadowResolution` | Vanilla / VeryHigh | Vanilla | `QualitySettings.shadowResolution` |
| `Anisotropic` | Vanilla / Force16 | Vanilla | `anisotropicFiltering = ForceEnable` + `Texture.SetGlobalAnisotropicFilteringLimits(16,16)` |
| `LodBias` | 0 = Vanilla, else 1.0–4.0 | 0 | `QualitySettings.lodBias` |

**Vanilla means "write the captured baseline back", never "skip".**

Baseline capture:
- Snapshot `{anisotropicFiltering, lodBias, shadowResolution}` after vanilla finished applying a
  preset. `UsePreset` nests the lighting callback, so: prefix sets `inUsePreset = true`
  (suppresses all scalar writes from nested seams), postfix (in `finally`) clears it, takes the
  snapshot, then applies the knobs. Snapshot refreshed on every preset change.
- Fallback: if no snapshot exists at the first mod write (preset applied before Harmony was
  installed), capture current values right before that first write. Never restore an
  uninitialised snapshot.
- Vignette baseline = `enabled.value` captured once per volume before the first override.
- Aniso restore = snapshot value + `SetGlobalAnisotropicFilteringLimits(-1, -1)` (engine default;
  coexistence with another mod's limits is not attempted).

Seams: postfix `OptionsManager.UsePreset` (scalars + vignette), postfix
`LightingManager.ApplyPostProcessOptions` (reacquire the current volume under
`_currentLightsRoot` — `LightingSettingsDef.ApplyTo` recreates the prefab each level; vignette
only, scalars suppressed while `inUsePreset`), `RenderforgeMod.OnLevelStart` (everything).
A direct UI vignette write performs the same PPv2 invalidation vanilla does at
`LightingManager.cs:167` (read that call during implementation; do not guess).

Rows: Options → Graphics via `GraphicsPanel` (picker rows cloned from `TextureQualityPicker`,
slider row cloned from `ShadowDistanceSlider`), immediate apply + `SaveConfig`.

Acceptance (PPCLI on Instance3, not Instance2):
- Readback per preset (all 6) + same-preset reapply + Vanilla round-trip restores the snapshot.
- Survives `OnLevelStart` (tactical → geoscape → tactical).
- Vignette: fixed-camera screenshot crop on/off differs at frame edges, identical centre.
- Perf: GPU frame time + peak VRAM for VeryHigh and LOD 4.0 vs Vanilla on the user's rig
  (RTX 5070 Ti, 1440p) — report numbers, no threshold gate, but README states cost.

## B. Colour vision

Separate stage AFTER `Grade()` and scene style in `Sharpen.cpp`; own int param
`ColorVision` (0 none / 1 deuteranopia / 2 protanopia / 3 tritanopia); full, fixed correction
(no slider in v1); composes with any LUT preset / style.

Maths (column-vector RGB, linear light): `D = I + R·(I − S)` where `S` = Machado 2009
severity-1.0 simulation matrix for the type and `R` = the per-type error-redistribution
matrix. One 3×3 per type, precomputed on the CPU (row-major float3x3, same packing on both
paths), uploaded as constants. Tritanopia requires its own sourced `R`; if no reference for a
tritan redistribution matrix is found during implementation, ship deut/prot only and defer
tritan — do not invent one.

Colour space: UNORM path decode sRGB → linear, apply `D`, `saturate`, encode; FP16 linear path
apply `D`, `max(0)`, no encode (`styleLinear`). Clamp is always in linear before encoding.

Activation: `ColorVision != 0` must count as "pass active" in every predicate listed above
(managed + D3D11 + D3D12 + FSR/XeSS), otherwise LUT=None/style=None/sharpen=0 bypasses it.

UI: picker row "Colour vision" next to the LUT row in Options → Graphics; en/ru labels.
Scene only — HUD/UI is drawn after the pass and is NOT corrected; README says so.

Tests (`lut_probe`): mode 0 is a bit-exact bypass; each matrix equals an independently computed
reference (Python in `native\probe\preview_luts.py` or a new `colour_vision_ref.py`);
encoded/linear parity within 1/255; saturated primaries and gamut boundaries stay in [0,1];
pass activates with LUT=None/style=None/sharpen=0 (live PPCLI readback + screenshot).

## C. Reflex standalone — backlog, not 1.4.0

Conditions before it may ship: a real simulation marker from Unity `Update` plus an input
boundary (not the synthetic SIM pair at `FgStreamline.cpp:375`), measured input-to-photon
improvement on the target rig, `lowLatencyAvailable` gate (Reflex is GeForce 900+, not "RTX
only"), one shared Streamline lifecycle with FG (init is process-once, device-pinned,
`FgStreamline.cpp:117`). Recorded in `DESIGN.md` idea backlog.

## Out of scope (decided)

Crisp UI (measured, no blur to fix — `docs\research\2026-09-05-crisp-ui-measurement.md`),
UI scale slider, motion blur / grain toggles, shadow "Extreme", HUD colour-blind palettes.
