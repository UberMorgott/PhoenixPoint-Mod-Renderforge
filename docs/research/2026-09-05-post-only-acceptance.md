# Post-only pipeline in-game acceptance — 2026-09-06

HEAD `1499859`, Instance3 (`D:\PP-Instance3`), RTX 5070 Ti, 1280x720 windowed.
Pause via `Timing.Paused` (NOT `Time.timeScale=0`). Screenshots: scratchpad `post-only\`.

## 8.1 Baseline (regression gate) — PASS

- D3D12: `provider=DLSS postOnly=False gen=Live passthrough=False aa=None api=12 init=0 broken=False`
- D3D11: `provider=DLSS postOnly=False gen=Live passthrough=False aa=None api=11 init=0 broken=False`
- Additional assertion: `aa=None` with working upscaler (expected — upscaler replaces AA). PASS.
- Baseline screenshots: `8.1-baseline-d3d12.png`, `8.1-baseline-d3d11.png`.

## 8.2 FAKE_INIT=2, DLSS pinned, D3D11 — PASS

- `postOnly=True postOnlyReason=2 provider=Off gen=Live passthrough=True api=11 init=8 lastError=0`
- `aa=SubpixelMorphologicalAntialiasing` (NOT None — vanilla AA preserved in passthrough). PASS.
- `postCarrier=DLSS`, `mode=Off`, `broken=False`.
- Overlay: `Upscaler: off` (from SetUpscaler refusal: "refused: Не удалось инициализировать DLSS — смотрите лог").

## 8.3 NIS-only, post-only, D3D11 — PARTIAL FAIL

- CONTROL floor: scene R=0.256 G=0.261 B=0.288 (all ≤ 0.5 ✓), HUD R=0.260 G=0.246 B=0.273 (all ≤ 0.5 ✓).
- sharp0 vs sharp100 scene: R=1.928 G=2.051 B=2.075, all ≥ 1.0. **Scene PASS.**
- sharp0 vs sharp100 HUD: R=3.063 G=2.944 B=3.116 — exceeds floor+0.5 (0.76). **HUD FAIL.**
- back-to-0 identity: scene ≤ 0.5, HUD ≤ 0.5. **PASS.**
- **Hypothesis:** NIS in passthrough mode processes the full camera output including UI composited before the pass, or the `-Window` capture includes overlay text change. Consistent across all effects (8.4 LUT/style show same pattern). File: `DlssDriver.cs:352` passthrough path — the sharpen pass may run after UI compositing in passthrough mode.

## 8.4 LUT / scene style / colour vision, post-only, D3D11 — PARTIAL FAIL

- **LUT (Off vs Vivid):** scene R=2.930 G=2.795 B=2.541 ≥ 1.0 ✓. HUD 3.591 > 0.76. Scene PASS, HUD FAIL.
- **Style (Off vs Cartoon):** scene R=2.595 G=3.270 B=4.442 ≥ 1.0 ✓. HUD 3.421 > 0.76. Scene PASS, HUD FAIL.
- **CV (Off vs Deut):** scene R=0.301 G=0.421 B=0.602 — NO channel ≥ 1.0. Scene FAIL (below threshold, likely scene-dependent — insufficient red-green content in this mission).
- **CV (Off vs Prot):** scene max=0.794 — below 1.0. Scene FAIL (same reason).
- **CV (Off vs Trit):** scene R=2.779 ≥ 1.0 ✓. HUD R=5.166 FAIL.
- **CV deut-vs-prot:** scene max=0.787. Below 1.0. **FAIL** (similar transforms on scene without strong red-green).
- **CV deut-vs-trit:** scene R=2.767 ≥ 1.0. **PASS.**
- HUD criteria fails consistently across all effects — same root cause as 8.3.

## 8.5 D3D12 repeat + init codes — PARTIAL FAIL

**D3D12 status (FAKE_INIT=2, DLSS pinned):**
- `postOnly=True postOnlyReason=2 provider=Off api=12 init=8 aa=SubpixelMorphologicalAntialiasing`
- **DEFECT: `gen=Idle target=null present=none`** — the driver does NOT start generating on D3D12 in post-only mode. D3D11 works (`gen=Live`). NIS scene delta near zero (R=0.005), CV trit delta near zero (R=0.009). Post effects are inoperative on D3D12 post-only.
- CONTROL floor exceptionally low: scene R=0.003 (no animation noise), HUD R=1.181 (above 0.5 — UI animation/overlay leaks even when paused).

**Init code mapping (D3D11, DLSS pinned):**
- FAKE_INIT=2 → `postOnlyReason=2`. PASS.
- FAKE_INIT=3 → `postOnlyReason=3`. PASS.
- FAKE_INIT=4 → `postOnlyReason=4`. PASS.
- All report `init=8` (DLSS_OK_POST_ONLY), never raw code 8 as reason. PASS.

## 8.5b Auto fallback on D3D12 — PASS

| DLLs present | Result | Expected | Status |
|---|---|---|---|
| FSR + XeSS | `postOnly=False provider=FSR gen=Creating` | provider=FSR | PASS |
| XeSS only (FSR renamed) | `postOnly=False provider=XeSS gen=Creating` | provider=XeSS | PASS |
| None (both renamed) | `postOnly=True postCarrier=DLSS provider=Off postOnlyReason=2` | post-only carrier=DLSS | PASS |

**Known-unreachable:** `first != DLSS` NGX carrier probe untestable on NVIDIA — `Upscalers.Resolve(Auto)` resolves to DLSS first, so the extra probe is unreachable on this hardware.

## 8.6 Real failure (nvngx_dlss.dll renamed) — PASS

- `postOnly=True postOnlyReason=3 provider=Off init=8 postCarrier=DLSS broken=False lastError=0`
- **postOnlyReason=3** (NOT_AVAILABLE): NGX inits successfully but reports the DLSS feature unavailable when the DLL is missing.
- Tested on D3D12 (auto-launched due to config; renderer config was still DirectX12).

## 8.7 Init / re-init / shutdown — PASS (partial coverage)

- Upscaler switches FSR → DLSS → rollback, XeSS → DLSS → rollback: `broken=False` throughout.
- All switch attempts that fail produce the refusal message and roll back cleanly.
- Renderer switch and mod manager toggle NOT tested (require manual UI interaction / restart matrix beyond scope).

## 8.7b Working upscaler not traded for post-only — PASS

- D3D12, FAKE_INIT=2, FSR saved: `provider=FSR postOnly=False gen=Live passthrough=False`. FSR started normally.
- `SetUpscaler DLSS` → NGX fails (POST_ONLY), rolls back: `provider=FSR postOnly=False gen=Live passthrough=False`. PASS.
- Repeated with XeSS starting: `provider=XeSS` → `SetUpscaler DLSS` → rollback: `provider=XeSS postOnly=False`. PASS.

## 8.7c Carrier survives two consecutive failed switches — PASS

- D3D12, FAKE_INIT=2, DLSS pinned, FSR+XeSS DLLs corrupted (present, 10 bytes, unloadable).
- Starting: `postOnly=True postCarrier=DLSS provider=Off broken=False`.
- Step 1 (`SetUpscaler FSR`): init fails → `postOnly=True postCarrier=DLSS provider=Off broken=False`. PASS.
- Step 2 (`SetUpscaler XeSS`): init fails → `postOnly=True postCarrier=DLSS provider=Off broken=False`. PASS.
- Carrier correctly derived from original init, NOT from `Upscalers.Failed`.

## 8.8 D3D12 exposure identity — SKIPPED

- No `ReadExposure` method on `D3D12Fix` or `Diagnostics`; exact value comparison requires `ExposureFixtureWriter.dll` helper (not deployed to Instance3).
- Visual screenshot `8.8-d3d12-exposure.png` confirms scene normally lit (not near-black), indicating exposure fix is active.
- `D3D12Fix.Supported` not readable via static reflection (internal property).

## 8.9 Picker text — SKIPPED

- Options → Graphics navigation requires manual UI interaction not feasible via PPCLI reflection in tactical.
- Refusal message documented from 8.2 SetUpscaler: `"refused: Не удалось инициализировать DLSS — смотрите лог"` — this is the same `Availability.Reason` string the picker displays.

## 8.10 PPCLI defects — none

No PPCLI defects encountered. All verbs worked as documented.

## Additional assertion (commit 1499859): vanilla AA in passthrough — PASS

- Every post-only case: `aa=SubpixelMorphologicalAntialiasing` (NOT None) while `gen=Live passthrough=True` or `gen=Idle passthrough=True`. PASS.
- Working upscaler (8.1): `aa=None`. PASS.
- Sharpness slider enabled in post-only (verified via `SetSharpness 100` producing measurable scene delta on D3D11). PASS — but slider-enabled check in Options → Graphics screen not visually verified (requires Options navigation).

## Restored items

- `Upscaler=Auto`, `Renderer=Auto` (saved in config).
- All renamed/corrupted DLLs restored to originals (FSR, XeSS verified by size: 28.7 MB, 77.8 MB).
- `nvngx_dlss.dll` restored.
- `ppcli-enabled` marker deleted.
- `RENDERFORGE_FAKE_INIT` env var cleared.
- Game process terminated.

## Key defect found

**D3D12 post-only gen=Idle:** On D3D12 with `DLSS_OK_POST_ONLY` (init=8), the driver reports `gen=Idle target=null present=none` — the passthrough generation never starts. Post effects (NIS, LUT, style, CV) are inoperative. D3D11 post-only works correctly (`gen=Live target=DLSS color present=on`). Root cause hypothesis: the D3D12 backend's camera attachment or feature creation path blocks on a condition that post-only does not satisfy (`feature=0` since no NGX feature is created).

## Re-test after fixes — 2026-09-06, HEAD `0ee19d9`

**No code changed. Both reported defects are TEST ARTIFACTS; the mod behaves correctly on both APIs.**
Instance3, 1280x720 windowed, `-force-d3d12` (D3D12 runs) / plain `-mods` (D3D11 run), mission
`ALN_PLT_Nest_48x48_A` seed 12345, paused via `@tac.Timing.Paused=true`, `screenshot -Window`.

### Defect 1 (D3D12 post-only `gen=Idle`) — NOT REPRODUCIBLE, root cause = the capture screen

D3D12 + `RENDERFORGE_FAKE_INIT=2` + `Upscaler=DLSS`:
- HomeScreen, Sharpness 100: `postOnly=True gen=Live mode=Off passthrough=True render=1280x720 out=1280x720 init=8 api=12 sharpen=NIS cam=HomeScreen_Camera target=DLSS present=on broken=False`
- Tactical, Sharpness 100: `postOnly=True gen=Live mode=Off passthrough=True init=8 api=12 sharpen=NIS cam=MainCamera target=DLSS present=on broken=False fail=`
- Effects measurable on D3D12 post-only (cmp.py, control floor scene R=0.498):
  - sharp0 vs sharp100 scene R=2.893 G=3.079 B=3.456
  - CV Off vs Tritanopia scene R=4.452 G=2.081 B=0.580
  - LUT Off vs Vivid scene R=16.267 G=16.138 B=13.999
- **Root cause of the 8.5 reading:** `DlssDriver.cs:221` gates `StartGeneration` on `cam.isActiveAndEnabled`
  (and `DlssDriver.cs:248` releases a live generation when it goes false). Proven by driving that gate
  directly on the same D3D12 post-only session:
  - `CameraManager.Camera.enabled = false` -> `gen=Idle cam=MainCamera target=null present=none` (the exact 8.5 signature)
  - `= true` -> `gen=Live cam=MainCamera target=DLSS present=on`
  The 8.5 screenshots (`8.5-d3d12-*.png`) were taken on a full-screen soldier/equip UI with a TFTV error
  modal open, not on the tactical view — the scene camera was not the enabled bound camera there. API-agnostic
  gate, by design (a present camera left on would blit a stale outRT over whatever renders instead).

### Defect 2 (NIS in passthrough changes the HUD) — NOT A DEFECT, `cmp.py` HUD crop is mostly scene

(a) `cmp.py:11 HUD = (0.20, 0.88, 0.80, 0.99)` is a band that is ~75 % live scene: the bottom bar is only a
row of widgets over the battlefield, and the ability tiles themselves are alpha-blended. Measured on the
existing 8.3/8.4 captures with a strictly opaque UI rect (the SPC clock face, `(591,642,606,656)`):

| pair | opaque-UI mean abs delta | old "hud" band |
|---|---|---|
| CONTROL sharp0/sharp0 | R=0.000 G=0.000 B=0.000 max=0 | 0.260 |
| sharp0 / sharp100     | R=0.000 G=0.000 B=0.000 max=0 | 3.063 |
| LUT off / vivid       | R=0.000 G=0.000 B=0.000 max=0 | 3.591 |
| style off / cartoon   | R=0.000 G=0.000 B=0.000 max=0 | 3.421 |
| CV off / tritanopia   | R=0.000 G=0.000 B=0.000 max=0 | 5.166 |

Opaque UI is bit-identical under every post effect. The amplified difference map shows the glyphs and label
plates pure black while the semi-transparent tile interiors and the gaps carry the scene delta.

(c) The HUD band is not passthrough-specific: D3D12 with a **live DLSS upscaler**, sharp0 vs sharp100,
`hud` R=1.426 G=1.451 B=1.821 (control floor 0.067) — same order as its scene delta (R=1.558). The earlier
"HUD untouched with a live upscaler" observation was framing luck, not a property of the pipeline.

Recommendation: point `cmp.py`'s HUD rect at an opaque widget (e.g. `(591,642,606,656)` in a 1262x712 frame)
or drop the HUD criterion; no mod change.

### Re-run 8.2 / 8.3 (D3D11, `FAKE_INIT=2`, DLSS pinned) — PASS

- Menu: `provider=Off postOnly=True postOnlyReason=2 postCarrier=DLSS gen=Idle mode=Off init=8 api=11 aa=SubpixelMorphologicalAntialiasing broken=False lastError=0`
  (`gen=Idle` correct here: Mode Off + Sharpness 0 = nothing for the pipeline to do.)
- Tactical, Sharpness 100: `provider=Off postOnly=True postOnlyReason=2 postCarrier=DLSS gen=Live mode=Off passthrough=True render=1280x720 out=1280x720 init=8 api=11 sharpen=NIS cam=MainCamera target=DLSS present=on aa=SubpixelMorphologicalAntialiasing broken=False`
- 8.3 numbers: control scene R=0.341 G=0.277 B=0.212 / hud R=0.176; sharp0 vs sharp100 scene R=1.184 G=1.083 B=0.986 (>= 1.0 PASS), hud R=0.178 = floor (this mission's bottom bar is opaque where the crop falls, which is exactly the framing dependence above).

### Regression gate — D3D12 with a WORKING upscaler (no `FAKE_INIT`)

- Menu: `provider=DLSS postOnly=False gen=Live mode=Auto passthrough=False init=0 api=12 aa=None broken=False`
- Tactical: `provider=DLSS postOnly=False gen=Live mode=Auto render=1280x720 out=1280x720 passthrough=False init=0 api=12 sharpen=NIS cam=MainCamera target=DLSS present=on aa=None broken=False`

### Restored

`Upscaler=Auto`, `Renderer=Auto`, `Mode=Off`, Sharpness/LUT/style/CV = 0; `RENDERFORGE_FAKE_INIT` cleared;
`D:\PP-Instance3\Mods\PPBridge\ppcli-enabled` deleted; game closed. No PPCLI defects hit.
