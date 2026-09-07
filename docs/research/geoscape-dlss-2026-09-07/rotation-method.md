# Geoscape globe rotation via PPCLI — verified 2026-09-07

## Mechanism

`GeoscapeCamera` (decompile `PhoenixPoint.Geoscape.Cameras/GeoscapeCamera.cs`) keeps a
`DampedVector3 _eulerRotation` (private). `.Target.y` = yaw, `.Target.x` = pitch (clamped
by `MaximumPitchDegrees`). Every frame `UpdateParams()` lerps `Current` toward `Target`
(`Current = Lerp(Current, Target, dt * DampFactor)`), then sets the Cinemachine virtual
camera transform via `CameraParams.SetToVirtualCamera`. No Cinemachine body/aim pipeline
fights it — the game writes `transform.position`/`rotation` directly on the vcam each frame
(`CameraBehavior.BehaviorUpdate` loop at `CameraBehavior.cs:156`).

## Discrete rotation — `focus_pos` console command

Built-in at `GeoscapeView.cs:2189`. Sends `CameraDirectorHint.GeoscapeFocus` with a
`GeoCamDirectorParams { TargetPosition, Unmanaged=true, InstantChase=false }`, which
reaches `GeoscapeCamera.HandleHint(ChaseGeoTarget, ...)` at `:359` and computes the
target euler angles from the world position. The globe sphere center is at the origin;
any point on or near the sphere works.

```
.\ppcli.ps1 connect console '{"command":"focus_pos","args":["5","0","0"]}'
```

Verified: three `focus_pos` calls (`5,0,0` / `0,0,5` / `-5,0,0`) produced three
completely different globe views (North America -> South America -> Africa -> Indian Ocean).

## Continuous rotation for DLSS ghosting

`DampedVector3.Update` is an exponential lerp: motion is fastest right after target is
set, then decays. Default `RotationDamping=5.0` converges in < 0.5 s at 240 fps.

**Slow the tween for sustained inter-frame motion:**

```
.\ppcli.ps1 connect multi '[
  {"id":"slow","verb":"call","args":{"op":"set","target":"<GeoscapeCamera handle>","member":"RotationDamping","value":1.0}},
  {"id":"rot", "verb":"console","args":{"command":"focus_pos","args":["5","0","0"]}},
  {"id":"s1",  "verb":"screenshot","args":{"path":"...frame1.png"}},
  {"id":"s2",  "verb":"screenshot","args":{"path":"...frame2.png"}}
]'
```

With `RotationDamping=1.0`, two sequential screenshots (~200-500 ms apart) showed the
globe sweeping from the Indian Ocean to central Africa — massive visible motion, ideal
for measuring DLSS ghosting artifacts between consecutive frames.

**Restore after test:** set `RotationDamping` back to `5.0`.

## Reaching the GeoscapeCamera instance

```
@view -> CameraDirector (h) -> Manager (h) -> CurrentBehavior (h) = GeoscapeCamera
```

```powershell
$dir = .\ppcli.ps1 connect call '{"op":"get","target":"@view","member":"CameraDirector"}'
$mgr = .\ppcli.ps1 connect call "{`"op`":`"get`",`"target`":`"$($dir.value.h)`",`"member`":`"Manager`"}"
$cam = .\ppcli.ps1 connect call "{`"op`":`"get`",`"target`":`"$($mgr.value.h)`",`"member`":`"CurrentBehavior`"}"
# $cam.value.h is the GeoscapeCamera handle
```

## Key decompile locations

- `GeoscapeCamera.cs:84` — `_eulerRotation` (DampedVector3, private)
- `GeoscapeCamera.cs:38` — `RotationDamping` (float, public, default 5.0)
- `GeoscapeCamera.cs:169` — `UpdateParams()` — applies lerp each frame
- `GeoscapeCamera.cs:359` — `HandleHint(ChaseGeoTarget)` — focus_pos path
- `GeoscapeView.cs:2189` — `FocusPosition` console command definition
- `DampedVector3.cs:17` — `Update(float dt)` — the lerp: `Current = Lerp(Current, Target, dt * DampFactor)`
- `CameraBehavior.cs:156` — `BehaviorUpdate` — per-frame loop writing to vcam
- `CameraParams.cs:30` — `SetToVirtualCamera` — writes transform directly

## Alternative: direct _eulerRotation.Target set via reflection

Also works but `_eulerRotation` is a struct; getting a handle returns a copy. The
`focus_pos` console command is simpler and achieves the same result. For fine-grained
yaw control (e.g. exact +90 degrees), use `focus_pos` with computed sphere coordinates.

## Input abstraction

The game uses a custom `InputController` (not Rewired/InControl). Keyboard rotation
reads `"Camera Up/Down/Left/Right"` held keys in `UpdateInput()` (:227). Joystick uses
`"Joystick Cursor Horizontal/Vertical"` axis events in `HandleInput()` (:254). PPCLI
cannot feed these (no synthetic input injection), but `focus_pos` bypasses input entirely.
