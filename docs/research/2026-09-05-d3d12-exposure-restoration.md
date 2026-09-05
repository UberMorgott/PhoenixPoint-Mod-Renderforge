# D3D12 auto-exposure restoration — 2026-09-05

## Root cause

- PPv2 shipped ComputeShaders `AutoExposure` and `ExposureHistogram` target renderer 2 (D3D11 only).
- On D3D12 (renderer 18): zero kernels found -> `KEyeHistogramClear` not found, 4137 exceptions before guard `6a8b37c`.
- Guard removed exceptions but left `autoExposureTexture` = Unity's White Texture (1.0) vs the D3D11 measured value of 22.6273518 (+4.5 EV).
- Result: tactical scenes near-black under D3D12.

## Fix: `rf-exposure-d3d12.bundle`

- Asset bundle `assets/rf-exposure-d3d12.bundle`, 20992 bytes.
- SHA256: `5E29D585222FAA5D1982ED04249B5FD03D29FC9D96DAEAA6B6FCB14E9E404D8D`.
- Contains the same two shaders with identical DXBC bytecode and bindings; only `targetRenderer` changed 2 -> 18.
- Built offline from the game's own assets with AssetsTools.NET in `D:\RenderforgeWork\d3d12-exposure-compat` (README there).
- ContentTool is NOT a build or runtime dependency. Game files unchanged.

## Loader: `src/D3D12Fix.cs`

- `FixExposure(PostProcessResources.ComputeShaders)` called from `Apply()` on D3D12 only.
- Loads bundle once from `RenderforgeMod.ModDir`, `LoadAsset<ComputeShader>` for:
  - `assets/renderforge-exposure-compat/exposurehistogram.compute`
  - `assets/renderforge-exposure-compat/autoexposure.compute`
- Assigns into `res.computeShaders.exposureHistogram` / `autoExposure` only when:
  - The stock shaders lack kernels (checked per kernel).
  - The bundle shaders report `HasKernel` for all 4 kernels.
- Bundle kept loaded for process lifetime.
- Failure path: one WARN line; existing `HasKernel` guard keeps the dark but exception-free path.
- Profiles/adaptation settings untouched.
- `Renderforge.csproj` adds `UnityEngine.AssetBundleModule` reference.
- `deploy.ps1` and `build/release.ps1` (Core pack) ship the bundle.
- Probe `docs/probes/histogram-support` stubs extended, PASS 2554, build 0/0.

## Paired fixture proof

- Harness: `ExposureCompatProbe.DispatchFixture`, 64x64 uniform inputs [0.01, 0.0625, 1, 4], EV [-4.5, 4], filtering 50..95%, 8 progressive steps.
- Baseline: `baseline-d3d11.json` (D3D11). Candidate: `candidate-d3d12.json` (D3D12).
- Location: `D:\RenderforgeWork\d3d12-exposure-compat`.
- `compare_fixture.py` PASS: exact 4x128 histogram bins (sum 45708 each), 36 exposure values (abs/rel tolerance 1e-4).
- Fixed exposure for input 0.01 = 22.6274166 on both renderers.
- Control: original renderer-2 bundle on D3D12 -> `HasKernel=False` for all 4 kernels.
- PPCLI clips returned strings at 2000 chars; a tiny helper `writer/ExposureFixtureWriter.dll` wrote the JSON in-process.

## Live D3D12 verification

- Main install, mission Fort Liberty / `HavenDefAlienNJ_Civ_CustomMissionTypeDef`, seed -1247052369, plot `NJR_PLT_RES_EliteLiving_48x48_C`.
- New save `Renderforge-tactical-progress-b03d16f1b0324e419f17b42b08db61ea` loaded via `PhoenixSaveManager.LoadGame(metadata)` + `PhoenixGame.Timing.Start` (never console `load_game`).
- Before fix (guard-only pair): `autoExposure=null`, texture White 1.0, world near-black (`D:\RenderforgeWork\exposure-live\before-d3d12.png`).
- After fix: `autoExposure` non-null, exposure texture 22.6121941 at load, 22.6273823 later, world normally lit (`after-d3d12.png`).
- Log line: `Renderforge D3D12: exposure histogram + auto-exposure kernels supported via rf-exposure-d3d12.bundle`.
- Zero exceptions in Player log.

## Final installed pair

- Managed `Renderforge.dll` 126464 B, SHA256 `833B715EBDD385EC037043FDA943C956513176DF4B587D0FB0A2B56ED185EB1C`.
- Native `RenderforgeNative.dll` 229376 B, SHA256 `4ECAFD82D56E2AB57D15CB31104745C394FD55E64B07E08AF5B956F862507E66` (includes retirement lifetime fix `ca89121`).
- Both verified loaded in-process.

## Bounded live acceptance

- PID 50136, D3D12, process alive + responding after each step, zero exceptions.
- Sequence: FG X2 on (DLSS-G live, multiplier 2, 239 fps) -> LUT Vivid 100 -> style Cartoon -> style PixelArt -> style Off -> LUT Off -> FG Off (teardown) -> FG X2 again -> FG Off.
- Exposure still 22.627 afterwards; state Tactical/Playing; config restored byte-equal.
- Screenshots: `fg-x2.png`, `fg-x2-lut-pixel.png`, `final-all-off.png` in `D:\RenderforgeWork\exposure-live`.
- This is bounded acceptance, NOT a proof that the earlier UnityPlayer access-violation crash is fixed (its cause remains unattributed).

## Saves

- New slot created before any restart; old reference save untouched.
- Old save hash: `C31C52AF6750CE8A583DBAD9E9FB23A61047D1373D618FA84ED6BF0E7D308C47`.
- Game was quit gracefully via `Application.Quit`.

## Limits

- Crash attribution remains unproven; exception onset and later crash are distinct findings.
- The fixture proof is numerical parity, not a rendering quality guarantee across all scenes.
- The bundle is game-version-locked: if PP ships new exposure shaders, the bundle may need rebuilding.
