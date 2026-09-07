# Geoscape DLSS diagnostic (2026-09-07)

RTX 5070 Ti, 2560x1440 borderless, D3D11, Instance2.

## User reports

1. "UI elements feel like they're smearing" with DLSS ON on the geoscape.
2. "Dark rectangular edge artifacts on the sides when you spin the globe."
Both absent with DLSS off.

## Mod status on the geoscape (live)

```
gen=Live mode=Auto(=Quality@1440p) render=1707x960 out=2560x1440
cam=GeoscapeCamera target="DLSS color" depthMode=Depth,MotionVectors
aa=None path=DeferredShading api=11 sharpen=RCAS
create=0x1(Success) eval=0x1(Success) feature=1
```

Camera: CinemachineBrain on GeoscapeCamera; virtual camera has no body/aim
pipeline (componentPipeline=0), transform set externally by the geoscape
controller. `RotateAround` on the transform is overridden within the same
frame -- no programmatic continuous rotation is possible via PPCLI.

## Findings

### (1) Smearing source: WorldSpace "PopUp" canvases

2581 Canvas objects total on the geoscape. At least 48 named "PopUp" with
`renderMode = WorldSpace` -- these are the globe markers (bases, sites,
missions, havens, "?" icons, "9%" labels, faction badges).

WorldSpace canvases render through the scene camera at render resolution
(1707x960) and are upscaled by DLSS together with the globe mesh.

**Root cause:** Unity's UI shader does NOT write to the motion vector buffer
(no `MotionVectorPass` in the default UI material). During camera rotation:

- The globe mesh gets correct camera-derived motion vectors.
- The WorldSpace PopUp markers get NO per-object motion vectors.
- DLSS uses the camera MVs to reproject marker pixels, but those MVs
  correspond to the globe surface behind the marker, not the marker's own
  screen motion (markers billboard / face the camera and move differently
  from the underlying geometry).
- DLSS applies incorrect temporal reprojection to the markers --> ghosting /
  smearing trails, worse at higher upscale ratios.

### (2) Edge artifacts: disoccluded pixels at screen edges

During camera rotation new pixels enter the screen edges. These pixels have
no temporal history. DLSS fills them from the current frame's low-resolution
input alone. At Quality ratio (1.5x) the upscaled edge band is visibly
darker / softer than the accumulated interior:

- Only 1 frame of data vs multi-frame accumulation in the interior.
- The effect is proportional to the upscale ratio (Quality > DLAA).
- This is a known DLSS limitation during rapid camera pans with upscaling.
- **DLAA (render=out=native) should reduce or eliminate the edge rectangles**
  because disoccluded pixels are already at native resolution. Confirmed:
  DLAA ran at 2560x1440 passthrough=False, but continuous rotation could not
  be automated (Cinemachine override).

### Motion vectors on the geoscape

MV probe at center and edges (static frame, DLSS Quality): all values
<3e-05 (floating-point noise). MV debug view: entirely black.

MV probe after RotateAround + 50 ms wait (multi-request batch): still <3e-05
-- the Cinemachine controller snaps the transform back within the same frame,
so the rotation never persists across a render boundary.

### Player.log (Renderforge lines on the geoscape)

- `upscaler available provider=DLSS api=11`
- `DLSS generation: mode=Auto render=1707x960 out=2560x1440 q=1 phases=18 colorRT=R8G8B8A8_SRGB`
- `MipBias: bias=-0.585 applied to 1034 textures`
- `CommandBuffer: built-in render texture type 3 not found while executing DLSS copy depth+mv (Blit source)` -- once, during level transition (type 3 = BuiltinRenderTextureType.Depth). Transient, not ongoing.
- No errors or warnings during steady-state geoscape rendering.

## Rotation method

Could NOT drive continuous globe rotation: Cinemachine overrides
transform.position within the same frame. RotateAround produces a one-frame
position change that is immediately reverted by the camera controller.
`geo-fast-forward` advances the clock (markers shift, terminator moves) but
does not rotate the camera.

## Diagnostic limitations

- Cannot reproduce continuous mouse-drag rotation via PPCLI.
- Static screenshots do not capture temporal/motion artifacts.
- Game launched headless (Start-Process); -Window screenshot mode requires a
  visible window.
- DLAA edge-rectangle comparison requires manual testing (confirmed DLAA
  ran, but could not rotate the globe under it).

## Screenshots (480x270 thumbnails, originals were 2560x1440)

- `dlss-quality-still.png` -- DLSS Quality, still frame, geoscape
- `baseline-off-still.png` -- Renderforge Off, same view
- `dlss-quality-mv-debug.png` -- MV debug view (black = zero MVs)
- `dlss-outRT.png` -- outRT dump (DLSS output, no HUD)
- `dlss-colorIn.png` -- colorRT dump (DLSS input, 1707x960)

## Cleanup

- ppcli-enabled deleted: yes
- Instance2 process: none
- No Instance2 files modified (ModConfig.json was never created; defaults)
