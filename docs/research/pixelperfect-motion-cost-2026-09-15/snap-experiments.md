# Harmony snap without Canvas.pixelPerfect: why the first postfix did nothing, and the fix (2026-09-15)

Follows `verify.md` test 3 (FAIL: `prod` On = 118/115/0, 118/0/0 = the "mip0 alone" row). Rig: D:\PP-Instance2,
2560x1440 borderless, D3D11, RTX 5070 Ti; IconBleed diag (`icon-bleed-2026-09-08\IconBleed.cs.txt`) on a
3840x2160-reference overlay canvas, `canvas.scaleFactor` 0.6667; DLSS Off during bleed runs so the DLSS bias is 0.
Working-tree build + `src\IconBleed.cs`, Release DLL swapped by hand with the game stopped; `-logFile` per run.

## Phase 1 - evidence

Temporary public counters in `PixelPerfectUi` (`Calls / NoCanvas / Changed / LastRect`) + a diag postfix on
`Image.OnPopulateMesh` recording the four mesh vertices of `atlas_0.66` (local -> `TransformPoint` ->
`RectTransformUtility.WorldToScreenPoint(null, w)`), read through PPCLI `call invoke Renderforge.PixelPerfectUi.Diag`.

| probe | result |
|---|---|
| postfix reached? | `active=True calls=3130 noCanvas=0` - yes, for every Graphic incl. the diag images |
| rect changed? | **`changed=0`**: `RectTransformUtility.PixelAdjustRect(rt, canvas)` returned exactly `rectTransform.rect` (0,0 64x64) |
| mesh verts on screen | `[0,0->173.660,260.660] ... [64,64->216.327,303.327]` - fractional, identical to the unsnapped rect |
| native with the flag OFF (live `call` on `RectTransformUtility`) | `PixelAdjustPoint((0,0), rt, canvas)` = **(0, 0)**; `PixelAdjustRect` = (0,0 64x64) |
| same natives after `canvas.pixelPerfect = true` (set + read + reset via `call`) | `PixelAdjustPoint` = **(-0.4949, -0.4950)**; `PixelAdjustRect` = **(-0.4949, -0.4950, 64.5 x 64.5)** |

Root cause: the natives `RectTransformUtility.PixelAdjustRect/Point` read `canvas.pixelPerfect` THEMSELVES and are the
identity while it is off. The managed gate in `Graphic.GetPixelAdjustedRect` is a fast path in front of a second,
native gate - so a postfix that calls the natives on a flag-off canvas changes nothing. Nothing else was hidden:
world position of `atlas_0.33` = 173.33 px, i.e. an overlay root canvas already lives in screen pixels in world
space, and the native snap = round each corner to a whole pixel (173.33 -> 173, 216.0 -> 216, width 43 px = 64.5 units).

## Phase 2 - experiments (`run.ps1 -Modes prod`, worst edge, magenta 0..255)

| experiment | atlas .00/.33/.66 | fullrect .00/.33/.66 | vanilla |
|---|---|---|---|
| E1 (verify.md): postfix -> natives | 118 / 115 / 0 | 118 / 0 / 0 | 104 |
| E2 as briefed (Text-style shift via `PixelAdjustPoint`) | not run - Phase 1 proves the native returns the input while the flag is off, so the shift is (0,0) by construction |
| **E2' managed snap: corner world xy -> `Mathf.Round` -> back to local, in both postfixes** | **0 / 0 / 0** | **0 / 0 / 0** | 104 flat |
| E2' control, option Off (`cfg.PixelPerfectUi=false` + `OnConfigChanged`) | 138 / 136 / 0 | 89 / 25 / 0 | 104 flat |
| E3 (vertex snap in `Image.OnPopulateMesh`) | not needed |

E2' diag readout: `changed=166`, `atlas_0.66 before=(0,0 64x64)scr[173.660,260.660-216.327,303.327]
after=(0.510,0.510 63x63)scr[174.000,261.000-216.000,303.000]`, mesh verts `174.000,261.000 ... 216.000,303.000`.
Crops: `crop-bleed-prod-on.png` / `crop-bleed-prod-off.png` (the fullrect texture's own magenta bottom row + left
column are part of the sprite; the bleed is measured on its TOP and RIGHT edges).

## Final approach (`src\PixelPerfectUi.cs`)

- `SnapPoint(Transform t, Vector2 local)`: `w = t.TransformPoint(local)`, round `w.x`/`w.y`, `l = t.InverseTransformPoint(w)`,
  accept only if `t.TransformPoint(l)` lands back on `w` within 0.01 px (a singular matrix - the `localScale.z = 0`
  prefabs - fails and keeps the unsnapped value). `SnapRect` snaps `rect.min` and `rect.max` and requires width/height > 0.
- `Graphic.GetPixelAdjustedRect` postfix -> `SnapRect`; `Graphic.PixelAdjustPoint` postfix -> `SnapPoint`. Same
  `SnapCanvas` guard as before (option on, canvas without its own `pixelPerfect`, `scaleFactor != 0`, root canvas
  ScreenSpaceOverlay). No native `RectTransformUtility.PixelAdjust*` calls remain.

## Phase 3 - sanity

| check | result |
|---|---|
| overlay FPS, geoscape idle, DLSS Quality (`screenshot -Window`, TopCenter overlay) | On **390** (2.6 ms) / Off **392** (2.6 ms) / On repeat **392** (2.5 ms) - `crop-fps-*-geo.png` from verify.md stay the reference layout |
| zero-Z prefabs, tactical `start-mission.json` | vehicle class icon, TAB/SPC/F/R badges, AP pips ОД 4/4, portraits all drawn On (`crop-squadbar-on.png` re-taken with the final code) |
| live toggle log | geoscape `on: 87402` -> `off: 87405` -> `on: 87408`; tactical `off: 32836` -> `on: 32839` -> `off: 32842` graphics rebuilt; Renderforge exceptions 0 |
| cleanup | PID 29624 stopped, 0 Instance2 processes; `ppcli-enabled` deleted; `src\IconBleed.cs` + all counters removed; clean Release DLL (242688 B) deployed to Instance2 (hash match); profile 592 `ModConfig.json` byte-identical to the pre-run copy (`PixelPerfectUi:true`, `Mode:3`) |

## Commands

```powershell
# handles (die on scene change - re-resolve)
.\ppcli.ps1 connect call '{"op":"get","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"Instance"}'
.\ppcli.ps1 connect call '{"op":"get","target":"<mod>","member":"Cfg"}'
# state: DLSS Off for bleed runs, Quality for fps; option toggle
.\ppcli.ps1 connect call '{"op":"set","target":"<cfg>","member":"Mode","value":"Off|Quality"}'
.\ppcli.ps1 connect call '{"op":"set","target":"<cfg>","member":"PixelPerfectUi","value":true|false}'
.\ppcli.ps1 connect call '{"op":"invoke","target":"<mod>","member":"OnConfigChanged","args":[]}'
# Phase 1 probes (rt/canvas handles from IconBleed.images[1].rectTransform / .canvas)
.\ppcli.ps1 connect call '{"op":"invoke","type":"UnityEngine.RectTransformUtility","assembly":"UnityEngine.UIModule","member":"PixelAdjustPoint","args":[{"$v2":[0,0]},{"$h":"<rt>"},{"$h":"<canvas>"}]}'
.\ppcli.ps1 connect call '{"op":"set","target":"<canvas>","member":"pixelPerfect","value":true}'
.\ppcli.ps1 connect inspect '{"h":"<rect handle>","values":true}'
# bleed: IconBleed Setup, then icon-bleed-2026-09-08\run.ps1 -Tag <tag> -Modes prod (dir = session scratchpad bleed2\)
# fps: ShowWindow(SW_RESTORE)+SetForegroundWindow, then .\ppcli.ps1 connect screenshot '{"path":"<abs>"}' -Window
```

PPCLI defects: none. (`screenshot -Window` replies top-level `{ok,mode:"window",...}` with no `result` wrapper -
documented in AGENTS.md, only my first wrapper script assumed otherwise.)
