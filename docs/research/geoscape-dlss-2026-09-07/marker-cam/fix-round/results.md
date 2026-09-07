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
