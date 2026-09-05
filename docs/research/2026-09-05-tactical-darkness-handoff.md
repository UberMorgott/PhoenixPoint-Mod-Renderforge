# Tactical darkness handoff — 2026-09-05

## Completed work and release boundary

- User-requested NR/DLSS5 removal: managed `624d47c`, native `9e3ba242`; standard DLSS/DLAA/FG, fonts, LUTs and styles remain. Historical findings: dossier `624ae639`, [retirement dossier](2026-09-05-dlss5-retirement-dossier.md).
- Cleanup `78c32c`: 2,290 files / 8,565,395,555 bytes; installed private NR folder also removed. See [cleanup inventory](2026-09-05-nr-artifact-cleanup-inventory.md). Separately rejected old build-file cleanup remains pending approval (`blocked by policy`); do not retry or work around that rejection.
- PixelArt default changed to 4 output pixels in `f956a1ef`. Earlier [production acceptance](2026-09-05-nr-free-production-acceptance.md) was bounded; it does not establish tactical stability.
- Push was STOPPED after the crash and was never issued. User cancelled PC shutdown; never shut down as part of completion.

## Observed failure and exposure evidence

- Exact mission: Fort Liberty / `HavenDefAlienNJ_Civ_CustomMissionTypeDef`, seed `-1247052369`, plot `NJR_PLT_RES_EliteLiving_48x48_C`.
- Original tactical run logged 4,137 `KEyeHistogramClear` exceptions before a native crash. Crash causation remains unproven; exception onset and later crash are distinct findings. [Local RCA](D:/Renderforge-work/black-screen-rca/RCA.md).
- Histogram guard `6a8b37c` stops unsupported histogram/exposure consumers, including the direct light-meter render path; subsequent tactical proof had no missing-kernel exceptions, but the world remained dark. [Guard source and checks](2026-09-05-d3d12-histogram-guard.md).
- D3D12 pre/post DLAA world-region mean luma: `0.0010625186 -> 0.0011088092`, ratio `1.0435669`. The pre-DLAA image was already dark. [D3D12 measurements](D:/Renderforge-work/dark-tactical-rca/result.md).
- D3D12 actual context: `autoExposure=null`, `autoExposureTexture=White Texture` (1.0). Authored exposure: Progressive, EV `[-4.5,4]`, key `1`, filtering `50..95%`, speedUp `2`, speedDown `1`.
- Supported D3D11 actual exposure texture measured `22.6273518` (approximately `2^4.5`, +4.5 EV). Missing adaptation is a concrete contributor to darkness; this does not justify a universal fixed brightness multiplier.
- D3D11 pre/post DLAA world-region mean luma: `0.0520953474 -> 0.0523480768`, ratio `1.0048513`. No large DLAA darkening was measured in either backend's samples.
- Cross-API camera positions were UNMATCHED and later moved during read-only work. D3D12 source RTs were `R16G16B16A16_SFloat`; D3D11 source RTs were `R8G8B8A8_SRGB`. Existing DumpRt converts to linear ARGB32 / 8-bit PNG; these are not raw HDR samples. No cross-API pixel-matched brightness ratio is claimed.
- [D3D11 reference report](D:/Renderforge-work/d3d11-exposure-reference/result.md) links the float readback, captures, camera snapshots and checkpoint proof. Tiny readback resources were cleaned in finally.

## Runtime ownership and save preservation

- Last known diagnostic game: PID `57832`, temporary **D3D11**, Tactical / Playing, FG Off, DLAA, LUT/style Off. Persisted renderer preference remains **D3D12**. Other instance PID `59916` was untouched.
- Installed managed SHA256: `F7052FB657A37B273CDCB3E3ACA80B095FBE6B93DB2AC5B2D7832E8B8D2731CF` — coherent isolated `f956a1ef` plus final histogram guard only.
- Installed native SHA256: `338A92743CEE702BFDF573F2B3BCFEE576F8B1B2D042CF66A18193A0F84FEDBA`. The new retirement pair is NOT installed.
- New verified tactical ManualSave: `Renderforge-tactical-reference-d85edb5727034c2aa5ac205c9e7d3795`, 1,046,943 bytes, SHA256 `C31C52AF6750CE8A583DBAD9E9FB23A61047D1373D618FA84ED6BF0E7D308C47`; original and D backup matched in the final offline check. [Preservation record](D:/Renderforge-work/d3d11-exposure-reference/checkpoint-preservation-final.json).
- Load used `PhoenixSaveManager.LoadGame(metadata)` through game Timing, not console `load_game`, which bypasses required metadata setup. No old save was overwritten.
- Camera movement may indicate user interaction. Ownership returned to lead; pending asynchronous restart-availability answer is required before a dependent quit. Do not force the camera, overwrite current progress, or quit while waiting. Preserve concurrent configuration changes.

## Prepared fixes and remaining proof

- Retirement fix `ca89121` retains managed/native resources until completion acknowledgments; reported focused native checks `330`, managed checks `315`. The full four-provider test log predates the final acknowledgment/comment changes; do not present it as a rerun of the exact final source.
- Coherent final pair was built but NOT DEPLOYED: native `4ECAFD82D56E2AB57D15CB31104745C394FD55E64B07E08AF5B956F862507E66` at `build/native/Release/RenderforgeNative.dll`; managed `227A2A3538E6122ADB8F4A39D1B0A6307DEDE395A5437005FC117E48A5E354C5` at `D:\RenderforgeWork\retirement-proof\managed\Renderforge.dll`. [Integrity receipt](D:/RenderforgeWork/retirement-proof/handoff-integrity.json). Live teardown / re-enable acceptance remains pending.
- Exposure candidate and paired harness: [prototype README](D:/RenderforgeWork/d3d12-exposure-compat/README.md), containing exact commands and provenance. Two approximately 20.5 KiB bundles contain the two existing ComputeShaders; baseline renderer `2`, candidate renderer `18`, unchanged shader bytecode/bindings.
- Four existing compute blobs created PSOs on D3D12 WARP. This proves bytecode acceptance only. Unity bundle loading, kernel availability, dispatch, numerical paired comparison and production integration have NOT run.
- Harness is isolated and source-grounded; its comparator requires the measured D3D11 histogram/exposure reference, including progressive adaptation. Final fixture review reported no blockers; user reply is pending. Offline allocation/comparator checks are not shader execution proof.

## Resolution 2026-09-05

- Root cause confirmed and fixed: `rf-exposure-d3d12.bundle` restores the two PPv2 compute shaders with `targetRenderer` 18 (D3D12). Full write-up: [D3D12 exposure restoration](2026-09-05-d3d12-exposure-restoration.md).
- Installed pair: managed `Renderforge.dll` SHA256 `833B715EBDD385EC037043FDA943C956513176DF4B587D0FB0A2B56ED185EB1C`; native `RenderforgeNative.dll` SHA256 `4ECAFD82D56E2AB57D15CB31104745C394FD55E64B07E08AF5B956F862507E66`.
- Paired fixture proof (D3D11 vs D3D12, 64x64 uniform inputs, 4x128 bins, 36 exposure values) PASS within 1e-4 tolerance.
- Live verification on the same Fort Liberty mission: exposure 22.627, world normally lit, zero exceptions.
- Bounded acceptance: FG X2 on/off, LUT Vivid, styles Cartoon/PixelArt/Off, all clean; config restored byte-equal.
- Remaining unverified: the earlier UnityPlayer access-violation crash is unattributed. Exception onset and crash are distinct findings.
