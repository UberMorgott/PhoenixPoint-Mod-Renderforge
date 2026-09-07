# Marker overlay fix round — verification (2026-09-07)

Under test: commit `b5255e3` (`src\GeoMarkerOverlay.cs` review fixes: layer ownership in GeoscapeCamera's own
pre-cull, far side classified on first sight, full present-camera snapshot/restore, gates D3D11 / no FG / ColorVision
None, own try/catch, every camera loses bit 15, rescan gated on hierarchyCount + 1 s, `cbClear` replaced by
`clearFlags = Depth`). Rig: RTX 5070 Ti, Instance2 (`D:\PP-Instance2`, profile 592), 2560x1440, vsync off, D3D11,
DLSS Quality (1707x960 -> 2560x1440), fresh campaign (`start-campaign.json`, difficulty 1; `ppcli-marker.zsav` does
not load on Instance2 - the known geoscape-save gap, deserialization exception, process restarted). Frames are
`RenderforgeMod.DumpScreen` (raw RGB24 of the real backbuffer at end of frame), converted offline; ~90 ms per dump
because of the 11 MB write, frame numbers stay consecutive.

## Checks

| | check | result |
|---|---|---|
| a | activation frame | `SetMarkerOverlay(false)` -> ref frame 22863 (`a0`), then `SetMarkerOverlay(true)` + dump in the SAME plan step run: frames 22871 (arm frame), 22872, 22873, then 22947 settled. `a-activation-a0-off-vs-a1-arm-frame.png`: the arm frame shows exactly the discovered cluster (base, 1%, 6, three "?"), **no far-side marker through the globe** in 22871/22872/22873; the pixel diff vs the settled frame is the pulsing icons + scan ring animation only. Status on arm: `sites=425 onNear=225 objects=9450` (was 17425 before the fix: far-side sites are no longer layered at all). |
| b | horizon crossing, consecutive frames | `RotationDamping=1`, `focus_pos 5 0 0` from the cluster pose, 14 consecutive frames 37689-37702 (dump every frame after a 420 ms lead). Sidecar: the cluster's anchor site is camera-facing through 37693 and gone from 37694 on. `b-horizon-crossing-10-consecutive-frames.png` (260x280 limb crops): frames c0-c4 each show one copy of every marker sliding to the limb, c5-c9 none. **Doubled: 0. Missing while still in front of the limb: 0.** (An earlier 100 ms-spaced sweep, frames 25216-25338, agreed: single copies in every frame.) |
| c | restore | Fresh present camera (created under the closed colour-vision gate, never touched): `clearFlags=Nothing cbs=1 cullingMask=0 depth=1 far=1000 fov=60 near=0.3 ortho=False orthoSize=5 path=UsePlayerSettings rect=(0,0,1,1) localPos=(0,0,0) proj m00=0.974278569 m11=1.73205078 m22=-1.0006001 m23=-0.60018003`. Armed: `Depth 0x8000 far=250 fov=45 near=0.1 Forward pos=(12,0,0)`. After `SetMarkerOverlay(false)`: every field back to the fresh values, projection matrix identical to 9 digits, rect identical. `Camera.allCameras` masks: fresh `DlssPresent:0x0 GeoscapeCamera:0xFFFFFFFF` -> armed `0x8000 / 0xFFFF7FFF` -> restored `0x0 / 0xFFFFFFFF` (2 enabled cameras on the geoscape; `cameras=1` lost the bit). Log: `restored 425 sites / 4998 objects / 1 cameras, GeoscapeCamera mask 0xFFFFFFFF`. |
| d | colour-vision gate | `SetColorVision Deuteranopia` while armed -> log `released - colour vision Deuteranopia (markers would bypass the daltonisation pass)`, status `inactive layer=15 gate=colour vision Deuteranopia ...`; a NEW generation under that config logs `not armed - colour vision Deuteranopia ...` once and stays inactive. `None` -> `drawn after reconstruction ... layer 15, 425 sites / 4998 objects, 1 cameras lost the bit`. `d-colour-vision-e1-vs-e2.png`: cluster at the same pose, e1 (Deuteranopia, markers on the vanilla DLSS path: white "?" ring / dark disk) vs e2 (None, overlay: the known grey-disk look). |
| e | log | Instance2 own log (`-logFile`): `Exception` 0, Renderforge errors 0. |
| 11 | sync arm / cbClear | `SetMarkerOverlay(true)` returns `markerOverlay=True active ...` in the same call (was `inactive` until the next frame). `cbClear` deleted: `M.clear=Depth M.cbs=1` and the arm frame has no stale content (row a). `ProbeMarkerClick` after the sync arm: `site=UI_GS_Site End Alien base ... hit=PickingCollider [20] of UI_GS_Site End Alien base sameSite=True canvasCam=DlssPresent`. |

## Notes

- Gate log lines seen this session, each once per reason: `not armed - passthrough generation` (mode Off),
  `not armed - disabled` (A/B lever off), `not armed - colour vision Deuteranopia (...)`, `released - colour vision ...`.
- `UnityEngine.PhysicsModule` stays referenced: the game's own site picking IS `Physics.Raycast` on
  `CameraDirector.Camera.ScreenPointToRay` (`GeoscapeView.cs:912,935,979,1013`); there is no other picking entry
  point for `ProbeClick` to reuse.
- Not covered here: D3D12 (gate refuses by design), frame generation (D3D12 only), a site that toggles active without
  any spawn (the rescan now waits for a `hierarchyCount` change - `ponytail:` comment in `Rescan`).

## Cleanup

Instance2 process stopped, `Mods\PPBridge\ppcli-enabled` deleted, 0 Instance2 processes. `SetColorVision` saved
`ColorVision=None` (its starting value) to profile 592's `ModConfig.json`; `SetMode` does not save (Mode stays 0).

## Round 2 — `a690f65` + `7c3f277` (strip cameras every tick, 0.25 s time-based rescan, GlobeUnits centre, clean Fail / re-arm)

Same rig and method (Instance2, profile 592, 2560x1440, D3D11, DLSS Quality 1707x960 -> 2560x1440, fresh
`start-campaign.json` difficulty 1, log `D:\PP-Instance2\rf-round2.log` with `frame (seconds)` stamps).

| | check | result |
|---|---|---|
| a | arm, globe values | Frame 3496 (29.069 s): `drawn after reconstruction by the DlssPresent camera, layer 15, 425 sites / 7772 objects, 1 cameras lost the bit, GeoscapeCamera mask 0xFFFFFFFF -> 0xFFFF7FFF`. `GlobeUnits.GlobeCenter` = (0,0,0), `GlobeRadius` = 6.371, `GlobeTransform` = `Globe` with `lossyScale` (1,1,1); `GeoSceneReferences.GlobeCollider`: `radius` 6.371, `center` (0,0,0), same `Globe` transform (instanceId 300340) - `SetGlobeData` (`GeoLevelController.cs:444`) passes the unscaled radius and the globe is unscaled, so the overlay's sphere now equals the game's. Near classification: `onNear=185` of 425 at arm (camera at (-0.8, 7.0, 9.7), 12.0 from the centre), 109-191 at the other poses this session - the visible cap plus the sites whose floating pivot clears the limb. |
| b | new visuals -> adoption | `reveal_sites` issued at frame 9968 (t=98.531, `DumpScreen` sidecar in the same plan step) -> `rescan: 425 sites / 7772 objects, hierarchy 21368, t=98.717 frame=9978`: **186 ms / 10 frames**. `reveal_sites_all` at frame 12043 (t=126.326) -> rescan frame 12044 (t=126.408): **82 ms / 1 frame**. Both under the 0.3 s target; the old 60-frame x 1 s gate would have been ~1 s at 240 Hz. `objects` did not move because the reveal only toggles children that already exist under the site's `VisualsContainer` (hierarchy +7 both times) and `Walk` layers inactive children too - `DumpHierarchy("UI_GS_Site (191)")`: 60 nodes, every node under the marker subtree on layer 15, the 16 off-layer nodes are the site root, `SphereOffset`/`PickingCollider` colliders, the pruned addon containers, the range decals and `UI_GS_TravelTo`. |
| c | horizon crossing, consecutive frames | `RotationDamping=1`, `focus_pos 5 0 0`, 420 ms lead, then `DumpScreen` + 1 ms wait x16: frames **16558-16573 consecutive** (t 178.061-178.950, ~60 ms apart because of the dump stall). `r2-c-right-limb-16-consecutive-frames.png` (360x600 right-limb crops at 1:1): every marker is one copy per frame while it slides to the limb, then gone; the polar "?" marker rides the limb across all 16 frames as a single copy. **Doubled: 0. Missing in front of the limb: 0.** Status after the sweep: `onNear=111 objects=11802 canvases=281` (the reveal made more sites near-side). |
| d | restore | Armed present camera: `clear=Depth mask=0x8000 cbs=1 path=Forward fov=45 near=0.1 far=250`. After `SetMarkerOverlay(false)` (+300 ms): `clear=Nothing mask=0 cbs=1 path=UsePlayerSettings fov=60 near=0.3 far=1000 depth=1 localPos=(0,0,0)`, `GeoscapeCamera.cullingMask=0xFFFFFFFF` - identical to the fresh values in row c above. `SetMarkerOverlay(true)` re-armed in the same call: `markerOverlay=True active layer=15 sites=425 onNear=109 objects=4578 canvases=109 cameras=1`. Log: `restored 425 sites / 11802 objects / 1 cameras`, `not armed - disabled`, `drawn ... 425 sites / 4578 objects`. |
| e | geoscape -> tactical -> geoscape | `start-mission.json` from the live geoscape (`cameFrom:geoscape`, 21.4 s) -> `restored 425 sites / 4578 objects / 1 cameras` at frame 31125, `not armed - passthrough generation` during the teardown; `start-campaign.json` from the tactical mission (18.3 s) -> `not armed - level curtain down` at 33374, `drawn after reconstruction ... 425 sites / 8022 objects` at 33467 (359.778 s), status `active ... onNear=191`. Log audit: `Exception` 3, all `[MP][tftv] TFTV REPORTED AN EXCEPTION` mirrored by the Multiplayer mod at frame 32324 (tactical -> menu teardown, TFTV's own NRE / InvalidOperation), **Renderforge errors 0**, no `Geoscape markers: disabled`. |

Not exercised: `Fail` -> `ResetFailure` (no way to force an exception from PPCLI) and the `pendingFail` deferral - both
are code-review changes only; a camera enabled mid-level picking up bit 15 (the per-tick `StripCameras`) was not
provoked either, the session had 2 enabled cameras throughout (`cameras=1` lost the bit).

Cleanup: `quit` console command, 0 Instance2 processes, `Mods\PPBridge\ppcli-enabled` deleted.
