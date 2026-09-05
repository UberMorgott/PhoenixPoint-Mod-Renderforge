# Overlay-mod compatibility under an active upscaler (2026-09-05)

Reported by the ContentTool session (A/B on Instance2, same process, toggled via
`Renderforge.RenderforgeMod.Toggle()`; evidence `AB-renderforge-ON.png` / `-OFF.png` +
`AB-*-values.json` in that session's scratchpad). DLSS SR Quality, 1707x960 → 2560x1440, nvngx 310.9.0.0.

## 1. Ghost streaks on immediate-mode GL geometry — expected, not a Renderforge defect

- ContentTool's bench floor grid (`GL.Lines`, static camera) shows horizontal smears with DLSS SR
  ON, crisp with it OFF.
- Cause: GL draws issued during the camera pass (`OnPostRender` / `OnRenderObject`) land in the
  colour buffer that the temporal upscaler consumes, but write no motion vectors and no depth the
  MV pass knows about → history accumulates under jitter → streaks. Same holds for FSR/XeSS.
- Correct fix is on the overlay's side: draw from `OnGUI` (after the camera, output resolution), or
  after Renderforge's upscale event. Renderforge has no per-draw "exclude" hook and SR has no
  reactive mask (that exists only for FG). README now says this under *Known issues / notes*.

## 2. Coordinate-base trap — documented

- With an upscaler active: `Camera.main.targetTexture` = RT "DLSS color" (render res),
  `cam.pixelWidth/Height` = render res (1707x960), `Screen.width/height` = 2560x1440, and
  `cam.WorldToScreenPoint` returns **backbuffer** pixels (identical ON vs OFF).
- Mixing bases (`GL.LoadPixelMatrix(0, cam.pixelWidth, 0, cam.pixelHeight)`) draws gizmos 1.5x off.
  ContentTool switched to `Screen.*`. Restoring `targetTexture`/`pixelRect` around `OnGUI` on our
  side was considered and NOT done: it would run every frame for every camera for a bug in other
  mods' code, and `Screen.*` is the documented Unity contract anyway.

## 3. Ops note

Renaming `Mods\Renderforge` to `Renderforge.off` while the id stays in `MOD_ACTIVATED` puts the
game into a self-restart loop (3 pids, `Options.jopt` rewritten). Toggle in-process instead.
