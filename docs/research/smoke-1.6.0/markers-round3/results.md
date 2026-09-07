# Marker overlay smoke — round 3 (1.6.0 release zip, 2026-09-07)

Rig: RTX 5070 Ti, Instance2 D:\PP-Instance2 (profile 592), 2560x1440 FullScreenWindow vsync off,
D3D11, DLSS Quality 1707x960->2560x1440. Source: Renderforge-Full-1.6.0.zip
(sha256 2ADB8EFA449522CC129CD3E0091C0EAC7D9088E21693B9AEBBCED794CA70A98E), DLL 1.6.0.0.
Fresh campaign (start-campaign.json, difficulty 1).

## Results

| | check | result |
|---|---|---|
| a | overlay armed log | PASS. `active layer=15 sites=425 onNear=229 cameras=1 geoMask=0xFFFF7FFF origMask=0xFFFFFFFF M=DlssPresent M.mask=0x8000 M.clear=Depth M.path=Forward`. |
| b | rotation sweep | PASS. RotationDamping=1, focus_pos 5 0 0, 12 DumpScreen frames (3376-3409, every 3 frames). All 12 valid (11059200 bytes = 2560x1440x3), 0 duplicate hashes, globe visibly rotated Africa->Pacific->Americas. Doubled: 0. Missing: 0. See `b-limb-sweep.png`. |
| c | restore + re-arm | PASS. SetMarkerOverlay(false) -> `inactive layer=-1 gate=-`. SetMarkerOverlay(true) -> `active layer=15 sites=425 geoMask=0xFFFF7FFF origMask=0xFFFFFFFF cameras=1`. |
| d | ColorVision gate | PASS. Deuteranopia while armed -> `inactive gate=colour vision Deuteranopia (markers would bypass the daltonisation pass)`. None -> `active layer=15`. Log: `released - colour vision Deuteranopia`. |
| e | geoscape->tactical->geoscape | PASS. start-mission from geoscape (25.1 s, cameFrom:geoscape). Tactical DLSS: `provider=DLSS gen=Live mode=Quality render=1707x960 out=2560x1440 eval=0x1(NVSDK_NGX_Result_Success)`. start-campaign back (21.4 s, cameFrom:tactical). Overlay re-armed: `active layer=15 sites=425 onNear=228 cameras=1 geoMask=0xFFFF7FFF`. |
| f | Player.log | PASS. Renderforge exceptions: 0, errors: 0. `Resolution list:` line absent (not logged in 1.6.0). Init: `upscaler available provider=DLSS version= api=11 unityIface=1 renderer=D3D11 unity=2019.4.31f1`. |

## Cleanup

Instance2 killed (PID 46488), `ppcli-enabled` deleted, 0 Instance2 processes.
