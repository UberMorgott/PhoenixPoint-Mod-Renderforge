# Mist rectangles diagnostic — 2026-09-07

## User report

"Dark rectangular patches with hard edges inside the mist on the ground" in a
tactical mission with Pandoran mist + TFTV Oneiric Delirium tint. Older
Renderforge version, D3D11, 1080p.

## Mist rendering facts (live reflection, Instance3, Renderforge 1.5.1)

- **Blob type:** `VoxelBlob(Clone)` GameObject with `MeshRenderer` — NOT a
  `ParticleSystem`. Confirmed via `GetComponentInChildren<ParticleSystem>` = null.
- **Shader:** `_PX_FX/MistDecoShader` (game-shipped custom FX shader).
- **Mesh:** mesh-based quads/planes (MeshFilter present). Each voxel is 1×0.6×1 m
  (`Scale` from the voxel).
- **Motion vectors:** NONE — custom FX shaders in BiRP D3D11 do not output MV
  unless they explicitly write to the motion vector pass. `MistDecoShader` does not.
- **Mip maps:** mainTexture returned null via reflection — the shader likely uses
  a custom property name (e.g. `_NoiseTex`). Texture mip state could not be read
  due to handle expiration during the session.
- **Voxel data confirmed:** `TacticalVoxelMatrix._soundedVoxels` count 10–23,
  `_voxelType=Mist`, `_blobComponent` present, `activeSelf=true`,
  `RelatedRenderersCache` 3 entries per voxel.
- **Visual quality:** controlled by `OptionsManager.GraphicLevel.ParticleQuality`
  via `TacticalVoxel.SetVisualQuality`.

## Renderforge status (Player.log)

```
mode=Quality render=1707x960 out=2560x1440 q=1 phases=18
colorRT=R8G8B8A8_SRGB outRT=R8G8B8A8_SRGB
MipBias: bias=-0.585 applied to 1987 textures (skipped 750) in 14 ms
api=11 sharpen=NIS
```

## Screenshot matrix (outdoor NJ map, mist spawned via `spawn_voxel` + `propagate_mist`)

Camera drifted between Off→Quality mode switches (Cinemachine recomputes framing
when `camera.targetTexture` resolution changes). Same-mode captures are stable.

| Cell | Mode | Render | Sharpness | MipBias | File |
|------|------|--------|-----------|---------|------|
| a | Off | 2560×1440 | 0 | n/a | matrix-a-off-s0.png |
| b | Off | 2560×1440 | 40 | n/a | matrix-b-off-s40.png |
| c | Quality | 1707×960 | 0 | -0.585 | matrix-c-quality-s0.png |
| d | Quality | 1707×960 | 40 | -0.585 | matrix-d-quality-s40.png |
| e | DLAA | 2560×1440 | 40 | 0 | matrix-e-dlaa-s40.png |
| f | Quality | 1707×960 | 40 | off | matrix-f-quality-nomip.png |
| t1–t3 | Quality | 1707×960 | 40 | -0.585 | t1/t2/t3-quality-s40.png |

Mist was clearly visible in one Off-mode capture (`a-off-s0.png`, earlier take)
as white/grey cloud blobs around the squad. DLSS Quality captures (c, d) show
the same area but the camera framing shifted; mist blob boundaries are NOT
visibly darker or more rectangular than in Off mode in the captured frames.

## Reproduction status

**Not reproduced on 1.5.1 at 2560×1440.** The dark rectangular patches were not
observed in any cell of the matrix at this resolution. Possible reasons:

1. The user's report was at 1080p where DLSS Quality renders at ~720p — lower
   input res makes the MeshRenderer quad edges proportionally larger and the
   temporal accumulation noisier (fewer input samples per output pixel).
2. The "older version" may have had different mip bias, sharpening or reset
   behaviour (the LUT was `BlackAndWhiteCinema` on Instance3's config, which
   would alter contrast in the mist region).
3. The TFTV "Oneiric Delirium" tint (a PPv2 volume colour grade) was not active
   in this test (no campaign, no Delirium event) — it could interact with the
   mist shader's alpha blend.
4. Camera angle/distance: the user's view may have been closer to the ground
   where quad edges are pixel-aligned.

## Root-cause hypothesis

The mist is `MeshRenderer` quads with `_PX_FX/MistDecoShader` — a custom FX
shader that does NOT write motion vectors. Under DLSS SR, objects without
motion vectors produce temporal ghosting and disocclusion artefacts (the
temporal history sees the mesh quad as static while the camera moves, producing
hard-edged rectangular remnants of previous frames). NIS sharpening on top of
these artefacts would accentuate the edges further.

Mip bias is unlikely to be the cause: the mist shader's texture (if mipmapped
at all) is a noise/gradient texture where LOD bias produces blur, not
rectangles. The quad geometry itself is the rectangle source.

## Recommended next steps

1. Reproduce at 1080p with DLSS Quality (render ~720p) on a campaign save that
   has natural mist + Oneiric Delirium.
2. If confirmed: file as a known limitation — "mist mesh quads ghost under DLSS
   because MistDecoShader has no motion vector pass". Mitigation options:
   a. InReset=1 on every frame while mist is in the frustum (heavy, wastes
      temporal history).
   b. Harmony patch MistDecoShader's render queue to force it through the MV
      pass (complex, may not be possible with a compiled shader).
   c. Document "turn off DLSS SR on maps with heavy mist" as a workaround.

## Cleanup

- ppcli-enabled deleted ✓
- No Instance3 process ✓
- D:\PP-Instance2 untouched ✓
- D:\Steam\... untouched ✓
