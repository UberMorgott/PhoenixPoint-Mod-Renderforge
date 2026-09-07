# Geoscape markers after reconstruction — verification (2026-09-07)

Fix under test: `src\GeoMarkerOverlay.cs` (design in `docs\DESIGN.md` "Geoscape markers after reconstruction").
Rig: RTX 5070 Ti, Instance2 (`D:\PP-Instance2`, profile 592), 2560x1440 FullScreenWindow, vsync off, D3D11,
DLSS Quality (render 1707x960 -> 2560x1440, preset K). Fresh campaign per run (`start-campaign.json`,
difficulty 1); Phoenix base near the pole in the final run (`UI_GS_Site (197)`), the sweep anchor is therefore the
site marker nearest the screen centre (`DumpScreen` sidecar `site=`), not the base.

## What the markers really are

`RenderforgeMod.DumpHierarchy("UI_GS_Site (191)")`: the marker is `GS_Unknown(Clone)/Location_Information` under the
site's `GeoSiteVisualsController.VisualsContainer` — MeshRenderer quads (`Unlit/Colored, Animated Mask` q=3000,
`Unlit/Transparent` q=2990, gradients q=2450) and TextMesh (`GUI/3D Cull Back Text Shader`), `mv=Object`, no MV pass.
The WorldSpace `PopUp` canvas is empty (0x0, no Graphic). Only 4 root WorldSpace canvases are active on a fresh
geoscape; the "48 PopUp canvases" of the first diagnosis were inactive-site prefab instances.

## Checks

| | check | result |
|---|---|---|
| a | log line, mask, camera | `Geoscape markers: drawn after reconstruction by the DlssPresent camera, layer 15, 425 sites / 17425 objects, GeoscapeCamera mask 0xFFFFFFFF -> 0xFFFF7FFF`. `GetMarkerOverlayStatus`: `M=DlssPresent M.mask=0x8000 M.clear=Nothing M.cbs=2 M.path=Forward M.msaa=False M.hdr=False M.occlusion=False M.target=null M.pp=False M.brain=False M.pos==geo.pos M.rot==geo.rot M.fov=45.000=geo.fov M.rect=(0,0,1,1) M.pixel=2560x1440 screen=2560x1440 globeR=6.371` |
| b | markers out of outRT, in the frame | `outRT-on.png`: globe + ground rings only, no icons / labels; `still-on-vs-vanilla.png` left: icons, "1%", "1", "6", "?" present in the backbuffer |
| c | rotation sweep | 30 deg yaw, `RotationDamping=1`, frames a/b/c at +48/+206/+357 ms (~0.7 px/ms). `sweep-b-on-off-vanilla.png` (rows on / off / vanilla, site 116, 200x100 @3x): **on = single crisp copy, no trail**; off (markers inside DLSS) = doubled edges / smear; vanilla = SMAA-soft single copy. Numbers below. |
| d | far side | `still-on-vs-vanilla.png` (same pose, fix on vs Mode Off): identical marker set and positions; no far-side marker shows through. `onNear=219` of 425 sites at that pose. |
| e | click | `ProbeMarkerClick`: `site=UI_GS_Site Alien Base 0 screen=2124.3,987.3 hit=PickingCollider [20] of UI_GS_Site Alien Base 0 sameSite=True canvasCam=DlssPresent` — the pixel projected through the present camera raycasts (GeoscapeCamera) back onto the same site's PickingCollider; picking is physics on layer 20 and untouched. |
| f | Mode Off | `GetMarkerOverlayStatus` = `inactive layer=15`; present camera `cullingMask=0 commandBufferCount=1 renderingPath=UsePlayerSettings enabled=True`; driver `gen=Live mode=Off passthrough=True`; log `Geoscape markers: restored 425 sites / 17425 objects, GeoscapeCamera mask 0xFFFFFFFF`; `RotationDamping` back to 5. Player.log: 0 `Exception`. |

## Sweep metrics (`marker-metrics.json`)

200x100 crop around the anchor site, gray 0..1, 3x3 Laplacian variance / mean |Sobel| (/8 kernels). Not the same
implementation as `../bias-mask/marker-metrics.json`, so only rows within this table compare.

| frame (site) | on | off (markers in DLSS) | vanilla (Mode Off) |
|---|---|---|---|
| still (197, base) | 0.0474 / 0.0286 | 0.0347 / 0.0230 | 0.0643 / 0.0299 |
| a (197) | 0.0416 / 0.0236 | 0.0525 / 0.0245 | 0.0807 / 0.0338 |
| b (116) | 0.0313 / 0.0206 | 0.0460 / 0.0248 | 0.0843 / 0.0347 |
| c (116) | 0.0303 / 0.0191 | 0.0481 / 0.0243 | 0.0795 / 0.0334 |

The crop metric is dominated by the marker's own contrast, which changed with the fix (below), not by trails:
"on" b/c read lower than vanilla although the crops show one crisp copy against a doubled one. Read the crops, not
the row. The `>= no-history sharpness` target of the brief cannot be judged with this metric.

## Known look difference (unresolved, reported)

`still2-site116-on-vs-vanilla.png` (top on, bottom vanilla, at rest): the unknown-site ring is grey and the disk a
translucent grey instead of white ring / black disk; whites reach 255 (vanilla 246). The markers used to be composited
inside the scene camera's HDR buffer and then tonemapped / graded by PPv2; drawn after reconstruction they are raw.
`depthTextureMode=Depth` on the present camera changes nothing (tested), so it is the grade, not a depth read. The base
marker rows look the same; a parked aircraft now sits under the soldier row (markers draw last, cleared depth).

## Dead ends (measured)

1. Separate marker camera `RenderforgeMarkerCam` (child of GeoscapeCamera, depth geo+2, any clearFlags): once it
   renders, the DlssPresent blit no longer reaches the buffer the HUD and end-of-frame ReadPixels see — the level
   curtain art (or the camera's own last clear colour) stays for the rest of the session with marker copies piling up
   per frame; `enabled=false` on it brings the blit back at once. Cause not identified; the present camera drawing the
   markers itself is the design that works.
2. Arming under the level curtain: `GeoscapeCamera.cullingMask` is 0 there (`CullEverythingController`), the takeover
   recorded mask 0 and took no site. Gate = `LevelSwitchCurtainController.IsCurtainLifted`.
3. PPCLI `connect screenshot` composes the frame itself and showed the curtain art with markers on top while the real
   backbuffer was fine -> `RenderforgeMod.DumpScreen` (ReadPixels at `WaitForEndOfFrame`, raw RGB24). Logged in
   `PPCLI\ISSUES.md`.

## Cleanup

Mode back to Quality only for the session (no ModConfig write: `SetMode` does not save; Instance2 `ModConfig.json`
content identical to the start, Mode 0). Process stopped, `ppcli-enabled` deleted, 0 Instance2 processes.
