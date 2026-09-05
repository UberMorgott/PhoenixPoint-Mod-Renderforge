# NR-free production acceptance — 2026-09-05

- Production sources: managed `624d47c`, native `9e3ba24`; historical findings are retained in [the retirement dossier](2026-09-05-dlss5-retirement-dossier.md).
- Final process: **8232**, `PhoenixPointWin64.exe -mods -force-d3d12`, `GeoscapeLevel(Clone)` / `Playing`, Unity 2019.4.31f1, Direct3D12, RTX 5070 Ti. This is the installed game's normal mod path. Actual mod discovery, Renderforge initialization, native evaluation and injected settings rows were observed; the command-line flag alone was not used as loading proof.
- Evidence root: `D:\Renderforge-work\nr-free-live`. Diagnostic observer/helper assemblies and captures remain outside the distributed mod. No game executable, other mod or campaign data was patched.

## Binary identity and retirement

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| Renderforge.dll | 121344 | `0818B4365134F54BD5779EC7DAB5B71972CEE456FECC6DBDB89219F1966F2B78` |
| RenderforgeNative.dll | 228352 | `338A92743CEE702BFDF573F2B3BCFEE576F8B1B2D042CF66A18193A0F84FEDBA` |

- `final-deployed-hashes.json` verifies managed canonical build/installed copies and both native installed copies. Loaded managed MVID is `e510673f-b66d-4f4d-b850-812ca3f0d357`, matching the previously tested managed artifact. `production-identity.json` reports no retired managed types or NR API methods.
- `deploy.ps1 -PPRoot 'D:\Steam\steamapps\common\Phoenix Point' -SkipNative` built with zero warnings/errors while the game was exited. Its rebuilt managed DLL had a different hash (`C59CDB14426B58103C8A27C9671C15F41662925CD37C244CE8E62D7C8507D15F`); the cause is **unproven**. That DLL was preserved as `deploy-rebuilt-C59CDB14.dll`; the known-tested `0818B436...` artifact was deployed before launch and verified afterward.
- Current module inventory contains ordinary `nvngx_dlss.dll`, `nvngx_dlssg.dll`, driver `_nvngx.dll` and Streamline. Neither the private NR bridge nor `nvngx_dlssnr.dll` is loaded. `current-native-modules.json` records exact paths.
- The installed `Mods\Renderforge\RenderforgeNR` directory was separately checked for exact contents, no reparse points and no loaded modules, then removed. Only its private `nvngx.dll` (10752 B) and `nvngx_dlssnr.dll` (165840496 B) were deleted; hashes are in `installed-nr-cleanup-before.json`. Ordinary NGX/FG runtimes remain. This operation did not retry the separately rejected cleanup of old repository `build/**` artifacts.
- Normal `ModManager.SaveModConfig()` serialization removed all seven legacy `Neural*` fields; `saved-config-final.json` retains the 19 current settings. The Graphics menu has no NR row; Screen contains **Чёткие шрифты**. `graphics-menu.png` and `screen-menu.png` record the actual production controls.

## Checkpoint preservation and correct loading path

- Before graceful exit, a new slot was created only after confirming it did not exist: `Renderforge-checkpoint-a00e487f986c4c05bbb4f636311184a7`.
- Save size is **278327 B**, SHA256 `DC10FC390881D715F79107A6B5492C45D46CE13C6CB54E78AD5C7B2381ECD3F0`; final verification found the same hash. No existing slot was overwritten, and neither `22222` nor an autosave was loaded. The failed intermediate world was never saved.
- The first automation load used PPCLI `restore` → console `load_game`. Installed `SerializationCommands.LoadGameCrt` directly calls `PhoenixGame.FinishLevelAndLoadGame(meta)`, bypassing `PhoenixSaveManager.PrepareLoadGame`. Consequently `LatestLoad` stayed null. TFTV's first exception at **04:54:38** dereferenced `saveManager.LatestLoad.DifficultyDef` in `CorrrectPhoenixSaveManagerDifficulty`; Geoscape startup was interrupted. This is a grounded automation-path defect, not evidence of save corruption or font/NR causation.
- One corrective graceful `RendererSwitch.Restart(true)` was performed. The exact same slot then loaded through the UI-equivalent coroutine: obtain metadata with `PhoenixSaveManager.GetSaveGame(name, ByRef<SavegameMetaData>)`, verify the exact slot and non-null `DifficultyDef`, then `Timing.Current.Call(saveManager.LoadGame(meta))`. **Do not substitute console `load_game` for this path.** `CheckpointFlow.cs` records the complete installed-API helper and `CheckpointFlow.Status()` its result.
- Corrected load: `LatestLoad` is the exact new slot, both difficulty values are `Easy_GameDifficultyLevelDef`, and TFTV logged **Geoscape start finished**. `TFTV-corrected-load.log`, `corrected-load-completed.json`, `corrected-load.png` and `Player-final.log` contain the evidence. The corrected run has no logged exception/error modal. A pre-existing broken-geoscape-event warning was logged as corrected by the game; it is not silently omitted from the retained log.

## Bounded live checks

- Font observation used the **loaded production core/lifecycle**, with a scoped post-mesh observer only (`Acceptance.cs`). Across 30 paired visible HUD labels, **17 were corrected and changed UVs; 13 fell back**. Every position, triangle array, preferred size and rectangle matched Off/On. Colors matched on 29 labels; the fallback clock caption `ПАУЗА` retained RGB 204 but alpha changed 21→252 between captures, so this asynchronous comparison does not claim identical color for that animated label. `font-comparison-summary.json`, `font-comparison.json` and `clean-fonts-{off,on}.{json,png}` retain results.
- The actual Screen toggle and Apply handler were also exercised. Off reported `active=false`, `cached=0`; On reported `active=true`. The observer was explicitly stopped, its Harmony owner/watchdog removed, and crisp fonts left enabled. Both production font samples contain no captured errors. Diagnostic fixtures do not ship.
- The actual LUT selector callbacks accepted **BlackAndWhiteCinema (5), Noir (6), VintageSepia (9)**; status retained successful DLSS evaluation. LUTs intentionally bypass Geoscape, so this is **selector/integration proof, not live tactical color-image proof**. Previous production-HLSL WARP validation remains the rendering evidence for all nine grades.
- Actual scene callbacks enabled **Cartoon** and **PixelArt**, strength 100, PixelArt **2 output pixels**. `style-1.png` and `style-2.png` show distinct world rendering behind readable HUD/pause controls. Both report successful native evaluation and zero native errors; the generation reset counter remained 3. The test restored LUT `RealisticDesaturated/100`, style `Off/100`, pixel size 2 afterward.
- DLAA and DLSS FG X2 remained configured. Final read-only status reported `fg=live enabled=1 multiplier=2 focus=game fg=1 lastError=0 presentHr=0 fps=235`; this confirms reported active generation, **not a measured doubling of delivered frames**. No benchmark sweep was performed.

## Handoff state and limits

- After test restoration, independent concurrent settings changes were observed. They were preserved rather than overwritten; their origin was not attributed. Final observed settings: **DLAA, DLSS, DirectX12, FG X2, Sharpness40, RealisticDesaturated100, Cartoon86, PixelSize2, CrispFonts=true, ShowOverlay=true, OverlayScale5, limit240**. Hotkeys U/O, TopCenter and ShowInGraphicsOptions remained unchanged. `production-identity.json` / `saved-config-final.json` record all values.
- Gameplay was left loaded and usable in PID8232; no further game mutations followed the ownership handoff. Screenshots taken before the concurrent changes show the acceptance states, not necessarily the user's later view.
- Remaining limits: no new tactical LUT mission run, no delivered-frame benchmark, no claim that all unsupported text pipelines are corrected. Earlier NR/head experiments are retired, not represented as successful features. No push or shutdown was performed by this validation task.
