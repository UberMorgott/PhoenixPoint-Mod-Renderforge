# Post-Only Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to execute this plan. Work one task at a time, in order, and stop at each checkpoint.

## Goal

The analytic post pass (LUT grades, scene styles, colour-vision correction, NIS sharpen) plus mip bias must run
**without an upscaler**. Today they die with NGX:

- `Device11::Init` (`native\Device11.cpp:174`) and `Device12::Init` (`native\Device12.cpp:97`) do
  `lastCreate = r; device->Release(); device = NULL; return DLSS_ERR_INIT_FAILED;` when NGX init fails — the D3D
  device the post pass needs is gone. `Device11::Evaluate` (`native\Device11.cpp:244`) and `Device12::Evaluate`
  (`native\Device12.cpp:168`) then bail on `!device`.
- Managed: `InitNative` sets `Available = InitCode == Native.DLSS_OK` (`src\RenderforgeMod.cs:116`), so
  `DlssDriver.Create()` is never called (`src\RenderforgeMod.cs:86`) and `Step()` never reaches
  `StartGeneration()` (`src\DlssDriver.cs:215`, guarded on `RenderforgeMod.Available`).

Nothing in the post pass needs NGX: `native\Sharpen.cpp` compiles its own HLSL, and the no-upscaler path already
exists — `Device11.cpp:251-256` and `Device12.cpp:187-192` (`sharpen.RunPassthrough`), driven by
`Dlss_Passthrough` (`src\DlssDriver.cs:352`) with `passthrough = liveView == DebugView.Passthrough || liveMode ==
RenderforgeMode.Off` (`src\DlssDriver.cs:278`).

After this plan: an NGX init failure with a live device yields a new init result `DLSS_OK_POST_ONLY`, the mod
reports `Available` with `PostOnly`, the driver runs a permanent passthrough generation, and every post effect
works on any GPU under D3D11 and D3D12.

## Architecture

Init outcomes after this change (native `Dlss_Init`, `native\RenderforgeNative.cpp:129-152`):

| provider `Init()` returns | device alive (`PostAlive()`) | `Dlss_Init` returns | managed |
|---|---|---|---|
| `DLSS_OK` (0) | yes | `DLSS_OK` | `Available`, upscaler runs |
| `DLSS_ERR_INIT_FAILED` (2) / `NOT_AVAILABLE` (3) / `NEEDS_DRIVER` (4) | **yes** | **`DLSS_OK_POST_ONLY` (8)** | `Available` + `PostOnly`, mode forced Off, post pass runs |
| same codes | no | that code, unchanged | unavailable, as today |
| `NO_DEVICE` (1) / `NO_UNITY_IFACE` (5) / `NO_PROVIDER_DLL` (6) / `PROVIDER_UNSUPPORTED` (7) | no (device released at `Device12.cpp:83`, `Fsr12.cpp:147/156`, `Xess12.cpp:202/212`) | that code, unchanged | unavailable, as today |

Only the NGX backends (`Device11`, `Device12`) can reach post-only: FSR/XeSS release the device on every failure
path they have (`native\Fsr12.cpp:147,156`; `native\Xess12.cpp:202,212`) — `Fsr12::Init` can return
`DLSS_ERR_NOT_AVAILABLE` at `Fsr12.cpp:168` with the device still alive, but its `PostAlive()` default is `false`
(no override), so it stays a plain failure. That is deliberate: the Auto loop reaches D3D11/D3D12 NGX anyway on the
machines that matter, and a second post-only carrier buys nothing.

Invariants this must not break:

- `ngxInitialized` stays **success-only** (`Device11.cpp:175`, `Device12.cpp:98`), so the conditional NGX shutdown
  (`Device11.cpp:300`, `Device12.cpp:256`) never calls `NVSDK_NGX_*_Shutdown1` on a device that never initialised NGX.
- `Shutdown()` releases the device **unconditionally** — verified for both backends: `Device11.cpp:301`,
  `Device12.cpp:257`. So a post-only device is still freed on shutdown/provider switch.
- D3D12 attaches `ring` and `sharpen` **before** NGX (`Device12.cpp:84-85`); the post-only return must happen after
  that, and `Device12::Shutdown` releases both (`Device12.cpp:252-254`).
- Mip bias stays 0 in passthrough (`src\DlssDriver.cs:235`).

## Tech Stack

- C++17 shim, MSVC x64, CMake (`native\CMakeLists.txt`), built by `build-native.ps1`.
- C# mod (net472, Harmony, PPModdingLib), built by `dotnet build Renderforge.csproj`.
- Offline check: `native\probe\dlss_probe.cpp` → `dlss_probe.exe`, already linked against `RenderforgeNative`
  (`native\CMakeLists.txt:94-99`) and already run + gated by `build-native.ps1:80-100`.
- In-game acceptance: PPCLI against `D:\PP-Instance3`.

## File structure

```
native\RenderforgeNative.h      + DLSS_OK_POST_ONLY, + Dlss_PostOnlyReason declaration
native\Device.h                 + IDevice::PostAlive(), + RfFakeInitCode() declaration
native\Device11.cpp             keep device on NGX failure; fake-init hook; PostAlive override
native\Device12.cpp             same, after ring/sharpen Attach
native\RenderforgeNative.cpp    S.providerCode, POST_ONLY mapping, idempotence guard, RfFakeInitCode, Dlss_PostOnlyReason
native\probe\dlss_probe.cpp     + --fake=N mode (RunPostOnly11)
build-native.ps1                + gate the --fake=3 probe run
src\Native.cs                   + DLSS_OK_POST_ONLY, + PostOnlyReason()
src\RenderforgeMod.cs           + PostOnly, InitNative/ReinitNative post-only acceptance, Reason(), GetStatus
src\DlssDriver.cs               wantMode forced Off, needsPipeline includes sharpness, sharpness zeroing by VIEW
docs\DESIGN.md                  init state machine + dev switch
README.md                       honest claim
docs\release-notes-1.4.0.md     1.4.0 entry
```

---

## Task 1 — Native: keep the D3D device alive when NGX init fails

- [ ] **1.1 `native\Device11.cpp:174`** — replace

```cpp
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; device->Release(); device = NULL; return DLSS_ERR_INIT_FAILED; }
```

with

```cpp
        // The device is the post pass's only dependency (Sharpen.cpp needs no NGX): keep it so Dlss_Init can
        // answer DLSS_OK_POST_ONLY. ngxInitialized stays 0, so Shutdown() skips NVSDK_NGX_D3D11_Shutdown1
        // and still releases this reference at Device11.cpp Shutdown().
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; return initCode = DLSS_ERR_INIT_FAILED; }
```

- [ ] **1.2 `native\Device11.cpp:157`** — the replay guard must also cover a post-only device, or a second
  `Dlss_Init` without a `Shutdown` would `GetDevice` again and leak an AddRef:

```cpp
        if (ngxInitialized || device) return initCode;   // replay the real outcome, not a blanket OK
```

- [ ] **1.3 `native\Device12.cpp:97`** — same edit, same comment (mentioning `NVSDK_NGX_D3D12_Shutdown1`):

```cpp
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; return initCode = DLSS_ERR_INIT_FAILED; }
```

- [ ] **1.4 `native\Device12.cpp:75`** — `if (ngxInitialized || device) return initCode;`

- [ ] **1.5** Do **not** touch `Device12.cpp:83` (`!g_unityD3D12` → release + `DLSS_ERR_NO_UNITY_IFACE`):
  without the Unity D3D12 interface `Evaluate` cannot run at all (`Device12.cpp:168`), so post-only there would be a
  lie.

Build: `cd E:\DEV\PhoenixPoint\Renderforge; .\build-native.ps1`
Check: the script's existing `dlss_probe` runs must still print `Dlss_Init code=0` on this RTX box and exit 0
(`build-native: OK`).

Commit: `fix(native): keep the D3D device alive when NGX init fails`

---

## Task 2 — Native: `DLSS_OK_POST_ONLY` init result

- [ ] **2.1 `native\RenderforgeNative.h:15-16`** — extend the init-code enum:

```cpp
// Dlss_Init return codes. DLSS_OK_POST_ONLY: the upscaler is NOT available, but the D3D device survived the
// failure, so the analytic post pass (Sharpen.cpp: NIS/RCAS, LUT grades, scene styles, colour vision) still runs
// through the passthrough path. Dlss_PostOnlyReason() carries the original DLSS_ERR_* for diagnostics.
enum { DLSS_OK = 0, DLSS_ERR_NO_DEVICE = 1, DLSS_ERR_INIT_FAILED = 2, DLSS_ERR_NOT_AVAILABLE = 3, DLSS_ERR_NEEDS_DRIVER = 4,
       DLSS_ERR_NO_UNITY_IFACE = 5, DLSS_ERR_NO_PROVIDER_DLL = 6, DLSS_ERR_PROVIDER_UNSUPPORTED = 7,
       DLSS_OK_POST_ONLY = 8 };
```

- [ ] **2.2 `native\RenderforgeNative.h`** — declare the accessor next to `Dlss_Status` (after line 94):

```cpp
// The DLSS_ERR_* the provider actually returned when Dlss_Init answered DLSS_OK_POST_ONLY; 0 otherwise.
// The picker greys the upscaler row with THIS code, not with DLSS_OK_POST_ONLY.
DLSS_API int __cdecl Dlss_PostOnlyReason(void);
```

- [ ] **2.3 `native\Device.h`** — add the liveness probe to `IDevice` (after `FeatureAlive`, line 78):

```cpp
    // Can this backend still run the post pass after a failed Init? True only when the D3D device survived.
    // NGX backends override it; FSR/XeSS release the device on every failure path they have, so they never do.
    virtual bool PostAlive() const { return false; }
```

- [ ] **2.4 `native\Device11.cpp`** — override next to `FeatureAlive` (search `bool FeatureAlive() const override`):

```cpp
    bool PostAlive() const override { return device != NULL; }
```

- [ ] **2.5 `native\Device12.cpp:58`** — same override next to `bool FeatureAlive() const override`:

```cpp
    bool PostAlive() const override { return device != NULL && g_unityD3D12 != NULL; }
```

- [ ] **2.6 `native\RenderforgeNative.cpp:82-94`** — carry the retained code in the shared state:

```cpp
static struct {
    int initCode;
    IDevice* dev;
    CreateParams create;
    // Ring of per-frame blocks: the main thread fills slot N while the render thread may still read N-1..N-3.
    FrameParams slots[4];
    unsigned slotIdx;
    FrameParams* lastSlot;
    int passthrough;
    int provider;                       // DLSS_PROVIDER_*, latched by Dlss_Init
    int wantProvider;                   // what Dlss_SetProvider asked for
    int providerCode;                   // what the backend's Init() really returned (retained under POST_ONLY)
    float nearZ, farZ, fovY;            // Dlss_SetCamera cache, copied into every slot
} S = { DLSS_ERR_NO_DEVICE, NULL, {}, {}, 0u, NULL, 0, DLSS_PROVIDER_DLSS, DLSS_PROVIDER_DLSS, 0, 0.1f, 1000.0f, 1.0471976f };
```

- [ ] **2.7 `native\RenderforgeNative.cpp:96-104`** — clear it on teardown:

```cpp
    S.initCode = DLSS_ERR_NO_DEVICE;
    S.providerCode = 0;
    return true;
```

- [ ] **2.8 `native\RenderforgeNative.cpp:131`** — POST_ONLY joins the idempotence guard, so a retry never
  re-enters `Init()` on a retained device:

```cpp
    if (S.dev && (S.initCode == DLSS_OK || S.initCode == DLSS_OK_POST_ONLY)) return S.initCode;
```

- [ ] **2.9 `native\RenderforgeNative.cpp:150-151`** — map the result:

```cpp
    S.dev = d;                       // kept even on failure so Dlss_Api()/Dlss_Status() still answer
    S.providerCode = d->Init(anyNativeResource, dllDir, logDir);
    // A dead upscaler with a live device is still a working post pass (Sharpen.cpp needs no NGX): report
    // POST_ONLY and keep the real code for the picker's reason text.
    if ((S.providerCode == DLSS_ERR_INIT_FAILED || S.providerCode == DLSS_ERR_NOT_AVAILABLE
         || S.providerCode == DLSS_ERR_NEEDS_DRIVER) && d->PostAlive())
        return S.initCode = DLSS_OK_POST_ONLY;
    return S.initCode = S.providerCode;
```

- [ ] **2.10 `native\RenderforgeNative.cpp`** — the accessor, next to `Dlss_Status` (after line 296):

```cpp
int __cdecl Dlss_PostOnlyReason(void) { return S.initCode == DLSS_OK_POST_ONLY ? S.providerCode : 0; }
```

- [ ] **2.11** `Dlss_ResultString` (`native\RenderforgeNative.cpp:307-315`) is **not** touched: it maps
  `NVSDK_NGX_Result` values through `GetNGXResultAsString`, so it cannot render our `DLSS_ERR_*` ordinals.
  `Dlss_PostOnlyReason()` is the exposure; `Dlss_Status` keeps returning `S.initCode` (now possibly 8).

Build: `.\build-native.ps1`
Check: build succeeds; the D3D11/D3D12 probe runs still print `Dlss_Init code=0` (no behaviour change on a
working NGX box).

Commit: `feat(native): add DLSS_OK_POST_ONLY init result`

---

## Task 3 — Native: `RENDERFORGE_FAKE_INIT` dev switch

Same shape as the existing env knobs (`RENDERFORGE_D3D12_DEBUG` at `native\D3D12Debug.cpp:26`,
`RENDERFORGE_FG_BOUNCE` at `native\FgHook.cpp:449`): read once, cached, `GetEnvironmentVariableA`.

- [ ] **3.1 `native\Device.h`** — declare next to `ToNgxQuality` (line 98):

```cpp
// DEV ONLY. RENDERFORGE_FAKE_INIT=2|3|4 makes the NGX backends return that DLSS_ERR_* INSTEAD of calling NGX,
// with the device already acquired - the post-only path without a non-NVIDIA GPU. 0 = off. Read once, cached.
int RfFakeInitCode();
```

- [ ] **3.2 `native\RenderforgeNative.cpp`** — define it next to `ToNgxQuality` (after line 115):

```cpp
int RfFakeInitCode()
{
    static int cached = -1;
    if (cached < 0) {
        char v[8] = {};
        DWORD n = GetEnvironmentVariableA("RENDERFORGE_FAKE_INIT", v, sizeof(v));
        cached = (n == 1 && v[0] >= '2' && v[0] <= '4') ? v[0] - '0' : 0;
    }
    return cached;
}
```

- [ ] **3.3 `native\Device11.cpp`** — insert after `if (!device) return DLSS_ERR_NO_DEVICE;` (line 163), i.e.
  before anything NGX is touched, so a successful init can never be overwritten:

```cpp
        int fake = RfFakeInitCode();
        if (fake) return initCode = fake;   // dev switch: NGX is never called, the device stays alive
```

- [ ] **3.4 `native\Device12.cpp`** — insert after `RfDbg::Attach(device);` (line 86), so `ring` and `sharpen`
  are already attached and ownership matches the real post-only path:

```cpp
        int fake = RfFakeInitCode();
        if (fake) return initCode = fake;   // dev switch: NGX is never called, the device/ring/sharpen stay alive
```

- [ ] **3.5** FSR/XeSS are deliberately untouched — the switch exists to fake *NGX* failure.

Build: `.\build-native.ps1`
Check: with the switch unset nothing changes (probe runs still `code=0`). Task 4 is the check that proves the
switch works.

Commit: `feat(native): add RENDERFORGE_FAKE_INIT dev switch`

---

## Task 4 — Probe: `--fake=N` covers the `Dlss_Init` result mapping

`dlss_probe` links `RenderforgeNative` directly (`native\CMakeLists.txt:94-97`) and creates a hardware D3D11 device
(`native\probe\dlss_probe.cpp:51`), so this check runs on **any** GPU — including the machines that cannot run NGX.

- [ ] **4.1 `native\probe\dlss_probe.cpp`** — add after `RunD3D11` (line 125):

```cpp
// --fake=N: RENDERFORGE_FAKE_INIT made the NGX backend fail with code N while keeping the device. Dlss_Init must
// answer DLSS_OK_POST_ONLY, remember N, and the analytic post pass must still run through the passthrough path.
static int RunPostOnly11(const wchar_t* dllDir, const wchar_t* cwd, int fake)
{
    ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed hr=0x%08X\n", (unsigned)hr); return 1; }

    ID3D11Texture2D* any = MakeTex(dev, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    int init = Dlss_Init(any, dllDir, cwd);
    printf("Dlss_Init      fake=%d code=%d (expect %d) api=%d (expect 11) postOnlyReason=%d (expect %d)\n",
           fake, init, DLSS_OK_POST_ONLY, Dlss_Api(), Dlss_PostOnlyReason(), fake);
    if (init != DLSS_OK_POST_ONLY || Dlss_Api() != 11 || Dlss_PostOnlyReason() != fake) g_failed = 1;

    // Idempotence: a retry must replay POST_ONLY, not re-enter Init() on the retained device.
    if (Dlss_Init(any, dllDir, cwd) != DLSS_OK_POST_ONLY) { printf("re-init did not replay POST_ONLY\n"); g_failed = 1; }

    const unsigned W = 1920, H = 1080;
    ID3D11Texture2D* color = MakeTex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    ID3D11Texture2D* out   = MakeTex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (g_failed) return 1;

    RenderEventAndDataFn evd = (RenderEventAndDataFn)Dlss_GetRenderEventAndDataFunc();
    Dlss_Passthrough(1);
    int c = 0, e = 0, alive = 0;
    // Sharpen only, then LUT+style+colour vision with sharpness 0: both must reach the shader without NGX.
    void* slot = Dlss_GetFrameSlot();
    Dlss_SetFrame(slot, color, NULL, NULL, out, 0, 0, 0, 0, 1, 16.6f, W, H, 1.0f, 0.5f, DLSS_LUT_OFF, 0.0f);
    evd(DLSS_EV_EVALUATE, slot);
    Dlss_Status(&c, &e, &alive);
    Report("PostOnly sharp", e);

    slot = Dlss_GetFrameSlot();
    Dlss_SetFrame(slot, color, NULL, NULL, out, 0, 0, 0, 0, 0, 16.6f, W, H, 1.0f, 0.0f, DLSS_LUT_VIVID, 1.0f);
    Dlss_SetSceneStyle(slot, 1, 1.0f, 4);
    Dlss_SetColorVision(slot, DLSS_CV_DEUTERANOPIA);
    evd(DLSS_EV_EVALUATE, slot);
    Dlss_Status(&c, &e, &alive);
    Report("PostOnly grade", e);
    ctx->Flush();

    printf("Sharpen        shader=%d (1=NIS 2=RCAS) lastError=%d (expect 0) featureAlive=%d (expect 0)\n",
           Dlss_Sharpener(), Dlss_LastError(), alive);
    if (Dlss_Sharpener() == DLSS_SHARPEN_FAILED || Dlss_Sharpener() == DLSS_SHARPEN_NONE
        || Dlss_LastError() != 0 || alive != 0) g_failed = 1;

    Dlss_Passthrough(0);
    Dlss_Shutdown();
    color->Release(); out->Release(); any->Release();
    ctx->Release(); dev->Release();
    return g_failed ? 1 : 0;
}
```

- [ ] **4.2 `native\probe\dlss_probe.cpp:646-659`** — parse the flag and route. In `wmain`, add `int fake = 0;`
  next to `want12`, extend the option loop:

```cpp
        else if (wcsncmp(argv[i], L"--fake=", 7) == 0 && argv[i][7] >= L'2' && argv[i][7] <= L'4' && argv[i][8] == 0) {
            char v[2] = { (char)argv[i][7], 0 };
            _putenv_s("RENDERFORGE_FAKE_INIT", v);      // read once inside the DLL on the first Dlss_Init
            fake = v[0] - '0';
        }
```

and the dispatch line becomes:

```cpp
    int rc = fake ? RunPostOnly11(argv[1], cwd, fake)
           : wantXess ? RunXess(argv[1], cwd) : wantFsr ? RunFsr(argv[1], cwd) : want12 ? RunD3D12(argv[1], cwd) : RunD3D11(argv[1], cwd);
```

- [ ] **4.3 `native\probe\dlss_probe.cpp:2`** — update the usage comment to list `[--fake=2|3|4]`.

- [ ] **4.4 `build-native.ps1`** — after the `--xess` run (line 88-89), inside the same `try` block:

```powershell
    & (Join-Path $outDir 'dlss_probe.exe') $outDir --fake=3
    $rcFake = $LASTEXITCODE
```

and after the XeSS gate (line 100):

```powershell
# Post-only must work on EVERY GPU: exit 3 is not tolerated here, the fake failure never calls NGX.
if ($rcFake -ne 0) { throw "dlss_probe (--fake=3, post-only) failed ($rcFake)" }
```

Build + check (one command, the gate is the check):

```powershell
cd E:\DEV\PhoenixPoint\Renderforge; .\build-native.ps1
```

Expected new lines: `Dlss_Init fake=3 code=8 (expect 8) api=11 (expect 11) postOnlyReason=3 (expect 3)`,
`PostOnly sharp 0x00000001`, `PostOnly grade 0x00000001`, `build-native: OK`.

Commit: `test(native): probe the post-only init mapping with --fake`

---

## Task 5 — Managed: P/Invoke surface

- [ ] **5.1 `src\Native.cs:12-13`** — mirror the native enum:

```csharp
        public const int DLSS_OK = 0, DLSS_ERR_NO_DEVICE = 1, DLSS_ERR_INIT_FAILED = 2, DLSS_ERR_NOT_AVAILABLE = 3, DLSS_ERR_NEEDS_DRIVER = 4, DLSS_ERR_NO_UNITY_IFACE = 5;
        public const int DLSS_ERR_NO_PROVIDER_DLL = 6, DLSS_ERR_PROVIDER_UNSUPPORTED = 7;
        /// <summary>Upscaler dead, D3D device alive: the analytic post pass still runs (RenderforgeNative.h).</summary>
        public const int DLSS_OK_POST_ONLY = 8;
```

- [ ] **5.2 `src\Native.cs`** — the import + wrapper, next to `Api()` (after line 237), following the
  everything-behind-a-try convention of `Api`/`Init`:

```csharp
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        private static extern int Dlss_PostOnlyReason();

        /// <summary>The real DLSS_ERR_* behind a DLSS_OK_POST_ONLY init; 0 otherwise. Behind a try: an older
        /// shim without the export must not take the mod down.</summary>
        public static int PostOnlyReason()
        {
            try { return Dlss_PostOnlyReason(); }
            catch (Exception) { return 0; }
        }
```

Build: `dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"`
Check: build succeeds (0 errors).

Commit: `feat(mod): expose the post-only init result over P/Invoke`

---

## Task 6 — Managed: accept post-only in `InitNative` / `ReinitNative`

- [ ] **6.1 `src\RenderforgeMod.cs:16`** — add the flag next to `Available`:

```csharp
        public static bool Available { get; private set; }
        /// <summary>The shim came up WITHOUT an upscaler: Dlss_Init answered DLSS_OK_POST_ONLY. Available is true,
        /// the mode is forced Off and every generation is a passthrough one carrying the analytic post pass.</summary>
        public static bool PostOnly { get; private set; }
```

- [ ] **6.2 `src\RenderforgeMod.cs:45`** — reset it beside `Available = false;`: `PostOnly = false;`
  (and the same at `:157`, in `OnModDisabled`).

- [ ] **6.3 `src\RenderforgeMod.cs:100-120`** — replace the whole `InitNative` body. The Auto fallback loop is
  preserved exactly as today; post-only is accepted only after the alternatives are exhausted:

```csharp
        private static bool InitNative(UpscalerKind want)
        {
            var m = Instance;
            Upscalers.Running = Upscalers.Resolve(want);
            Native.SetProvider(Upscalers.ProviderOf(Upscalers.Running));
            InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
            UpscalerKind postCarrier = UpscalerKind.Off;      // first provider whose device survived its failure
            while (InitCode != Native.DLSS_OK && want == UpscalerKind.Auto)
            {
                if (InitCode == Native.DLSS_OK_POST_ONLY && postCarrier == UpscalerKind.Off) postCarrier = Upscalers.Running;
                UpscalerKind next = Upscalers.NextFallback(Upscalers.Running);
                if (next == UpscalerKind.Off) break;
                m.Logger.LogInfo(Upscalers.Running + " init failed (code " + InitCode + "): Auto falls back to " + next);
                Native.Dlss_Shutdown();
                Upscalers.Running = next;
                Native.SetProvider(Upscalers.ProviderOf(next));
                InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
            }
            // Every alternative upscaler is gone too: stand the post-only carrier back up so the LUT, the scene
            // styles, the colour-vision correction and NIS sharpen still have a live device to run on.
            if (InitCode != Native.DLSS_OK && InitCode != Native.DLSS_OK_POST_ONLY && postCarrier != UpscalerKind.Off)
            {
                Native.Dlss_Shutdown();
                Upscalers.Running = postCarrier;
                Native.SetProvider(Upscalers.ProviderOf(postCarrier));
                InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
                m.Logger.LogInfo("no upscaler available: post pass only, on " + postCarrier + " (code " + InitCode + ")");
            }
            PostOnly = InitCode == Native.DLSS_OK_POST_ONLY;
            Available = InitCode == Native.DLSS_OK || PostOnly;
            // In post-only the failed provider STAYS recorded with its real code, so the picker greys the row with
            // the true reason (Availability.Reason reads Upscalers.Failed/FailedCode) instead of claiming success.
            if (PostOnly) { Upscalers.Failed = Upscalers.Running; Upscalers.FailedCode = Native.PostOnlyReason(); Upscalers.Running = UpscalerKind.Off; }
            else if (Available) Upscalers.Failed = UpscalerKind.Off;
            else { Upscalers.Failed = Upscalers.Running; Upscalers.FailedCode = InitCode; Upscalers.Running = UpscalerKind.Off; }
            return Available;
        }
```

- [ ] **6.4 `src\RenderforgeMod.cs:131`** — a live switch must not trade a WORKING upscaler for post-only:

```csharp
            // Post-only is a good answer when nothing was running; it is a regression when it replaces a live
            // provider, so fall back to `prev` exactly as a hard failure would.
            if (InitNative(want) && !(PostOnly && prev != UpscalerKind.Off))
            {
                m.Logger.LogInfo("upscaler switched to " + Upscalers.Running + " version=" + Native.ProviderVersion());
                return;
            }
```

- [ ] **6.5 `src\RenderforgeMod.cs:150`** — `else if (InitCode == Native.DLSS_OK)` becomes `else if (Available)`
  so a post-only session still gets its shutdown owner.

- [ ] **6.6 `src\RenderforgeMod.cs:74`** — the startup log line already prints `Available`/`InitCode`; append the
  post-only marker:

```csharp
                Logger.LogInfo((Available ? (PostOnly ? "post pass only (no upscaler, code " + Native.PostOnlyReason() + ")" : "upscaler available")
                                          : "upscaler unavailable (code " + InitCode + "): " + Reason(InitCode))
```

- [ ] **6.7 `src\RenderforgeMod.cs:508-521`** — `Reason` gains the new code:

```csharp
                case Native.DLSS_OK_POST_ONLY: return "no upscaler on this GPU; post pass only";
```

- [ ] **6.8 `src\RenderforgeMod.cs:502`** — `GetStatus` is the acceptance readback, so it must carry the flag.
  Insert after `"provider=" + Upscalers.Running`:

```csharp
        public static string GetStatus() => "provider=" + Upscalers.Running + " postOnly=" + PostOnly + " postOnlyReason=" + Native.PostOnlyReason() + " lut=" + ...
```

Build: `dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"`
Check: build succeeds; no other reference to `InitCode == Native.DLSS_OK` remains —
`Select-String -Path E:\DEV\PhoenixPoint\Renderforge\src\*.cs -Pattern 'DLSS_OK\b'` must show only `Native.cs`,
`RenderforgeMod.cs:116` (the new line) and `:150`'s replacement.

Commit: `feat(mod): accept a post-only shim as available`

---

## Task 7 — Managed: the driver runs the post pass with the upscaler off

- [ ] **7.1 `src\DlssDriver.cs:134-136`** — `Apply` is the single funnel for `wantMode` (the only other writers
  are the `Off` resets at `:98` and `:268`), so force it there:

```csharp
        public void Apply(RenderforgeMode mode, DebugView view)
        {
            // Post-only: there is no upscaler to ask for a mode. Every generation is a passthrough one.
            wantMode = RenderforgeMod.PostOnly ? RenderforgeMode.Off : mode;
            wantView = view;
```

- [ ] **7.2 `src\DlssDriver.cs:196-198`** — sharpness alone must be able to start a generation. Today the pass
  exists but nothing asks for it when the mode is Off:

```csharp
            var cfg = RenderforgeMod.Instance?.Cfg;
            bool lutActive = RenderforgeMod.TacticalActive && cfg != null && cfg.Lut != LutPreset.Off && cfg.LutStrength > 0;
            // Sharpness is its own reason to run: with the upscaler Off the NIS pass is the only thing on the frame.
            bool needsPipeline = wantMode != RenderforgeMode.Off || lutActive || SceneStylePanel.Active(cfg)
                || ColorVisionPanel.Active(cfg) || (cfg != null && cfg.Sharpness > 0);
```

- [ ] **7.3 `src\DlssDriver.cs:491`** — stop zeroing sharpness for an ordinary Off+post generation; keep it zero
  for the DEBUG passthrough view, whose whole point is an untouched frame:

```csharp
                // Zero only for the debug Passthrough VIEW (an untouched reference frame). An Off-mode generation
                // is a passthrough too, and there the NIS pass is exactly what the player asked for.
                float sharp = liveView == DebugView.Passthrough ? 0f : Mathf.Clamp01((RenderforgeMod.Instance?.Cfg?.Sharpness ?? 0) / 100f);
```

- [ ] **7.4** Leave `src\DlssDriver.cs:235` (`MipBias.Apply(passthrough ? 0f : ...)`) alone — bias stays 0 with no
  upscaling, which is correct.

- [ ] **7.5** Leave `src\Overlay.cs:138` (`live = d != null && d.IsLive && !d.Passthrough`) alone: in post-only
  `live` is false, so the overlay prints `Upscaler: off (<reason>)` via `Upscalers.RunningName` / `dlssReason`
  (`src\Overlay.cs:161-163`) — exactly the required "off". Consequence to accept and record: the D3D12 `GPU:`
  timings line (`src\Overlay.cs:170`, gated on `live`) stays hidden in post-only.

Build: `dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"`
Check: build succeeds.

Commit: `feat(mod): run the post pass without an upscaler`

---

## Task 8 — In-game acceptance on `D:\PP-Instance3`

Read `E:\DEV\PhoenixPoint\PPCLI\PLAYBOOK.md` first. Protocol reused from
`docs\superpowers\plans\2026-09-05-colour-vision.md:1397-1591` (Task 6b): pause with `Time.timeScale`, gate on
`UnityEngine.Time.frameCount`, capture with `-Window`, compare with `cmp.py`.

Deploy for every case:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
.\build-native.ps1
dotnet build .\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"
```

Fixtures (`$Cli = 'E:\DEV\PhoenixPoint\PPCLI\ppcli.ps1'`, `$PP = 'D:\PP-Instance3'`):

```powershell
& $Cli connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"GetStatus","args":[]}' -PPRoot $PP
& $Cli connect screenshot ('{"path":"' + $path.Replace('\', '\\') + '"}') -Window -PPRoot $PP | Out-Null
python C:\Temp\claude\E--DEV-PhoenixPoint-Renderforge\eb2d5821-11bf-459d-9e97-c19a29418a64\scratchpad\cv\cmp.py $a $b $label
```

`cmp.py A.png B.png [label]` prints two lines (`scene`, `hud`) of mean |Δ| per channel + max. Fake failure is armed
by launching the game with `$env:RENDERFORGE_FAKE_INIT = '3'` set in the launching shell (the shim reads it once, on
the first `Dlss_Init`); clear it with `Remove-Item Env:\RENDERFORGE_FAKE_INIT` between cases.

- [ ] **8.1 Baseline, switch OFF (regression gate).** D3D11 and D3D12, DLSS/FSR/XeSS as today: `GetStatus` must
  show `postOnly=False`, `provider=` the real upscaler, `gen=Live passthrough=False`. Capture one tactical frame per
  API for the later identity comparisons.
- [ ] **8.2 `RENDERFORGE_FAKE_INIT=3`, D3D11.** `GetStatus` → `postOnly=True postOnlyReason=3 provider=Off`,
  `gen=Live passthrough=True`, overlay `Upscaler: off (…)`. `Dlss_LastError` (in the driver `Status` string) = 0.
- [ ] **8.3 NIS-only, post-only, D3D11.** `SetMode Off None`, LUT Off, style Off, CV None, `SetSharpness 100` vs
  `SetSharpness 0`: `cmp.py` scene mean |Δ| ≥ 1.0 on ≥1 channel; HUD ≤ the control floor + 0.5 (the pass runs on the
  camera output, not the UI). Then `SetSharpness 0` twice = CONTROL, must be ≤ 0.5 on all channels.
- [ ] **8.4 LUT / scene style / colour vision, post-only, D3D11.** One case each, off-vs-on, same criteria as
  colour-vision plan `:1586-1590`. Colour vision additionally: deut-vs-prot and deut-vs-trit scene ≥ 1.0.
- [ ] **8.5 Repeat 8.2-8.4 under D3D12** (`SetRenderer D3D12` + restart), with `RENDERFORGE_FAKE_INIT=3`, then
  once with `=2` and once with `=4` (D3D11 only, to prove all three codes map and `postOnlyReason` echoes each).
- [ ] **8.6 Real failure, no fake switch.** Rename `nvngx_dlss.dll` to `nvngx_dlss.dll.off` in the Instance3 mod
  folder, launch: `postOnly=True`, `postOnlyReason=` 3 or 4 (record which), post effects still work on both APIs.
  Restore the file afterwards.
- [ ] **8.7 Init / re-init / shutdown.** With the switch on: renderer switch D3D11↔D3D12; `SetUpscaler DLSS` →
  `FSR` → `XeSS` → `Auto` (each must leave the mod alive, `postOnly=True`, no `broken=True`, no `lastError` != 0);
  disable the mod in the mod manager and re-enable it. With the switch OFF, on this RTX box: `SetUpscaler DLSS` from
  a live FSR/XeSS session must NOT land in post-only (Task 6.4).
- [ ] **8.8 D3D12 exposure identity.** All effects Off (LUT Off, style Off, CV None, Sharpness 0), switch OFF,
  D3D12: measure exposure exactly as `docs\research\2026-09-05-d3d12-exposure-restoration.md` did — read that note's
  method before running — and confirm the 1.3.0 reference value **22.627** is unchanged.
- [ ] **8.9 Picker text.** Options → Graphics: the UPSCALER row greys with the same reason string as before this
  change (`Availability.Reason`, `src\Availability.cs:51-75`, reading `Upscalers.Failed` / `FailedCode`). Screenshot
  before/after.
- [ ] **8.10** Log every PPCLI defect hit on the way into `E:\DEV\PhoenixPoint\PPCLI\ISSUES.md` (attempted →
  happened → expected → evidence → severity). Do not fix PPCLI here.

Commit (evidence only, after the run): `test(renderforge): verify the post-only pipeline in-game`

---

## Task 9 — Docs

- [ ] **9.1 `docs\DESIGN.md`, under `## Failure handling` (line 420)** — add the init state machine: the table
  from this plan's Architecture section, the `ngxInitialized` success-only invariant, the unconditional device
  release in both `Shutdown()`s, and the rule that FSR/XeSS never reach post-only.
- [ ] **9.2 `docs\DESIGN.md`, same section** — document `RENDERFORGE_FAKE_INIT=2|3|4` as **dev-only**, next to the
  existing env knobs listed at `docs\DESIGN.md:633-634`; state that it never overwrites a successful init because
  it returns before NGX is called (`native\Device11.cpp` / `Device12.cpp` Init), and that
  `dlss_probe.exe <dir> --fake=3` is its regression gate in `build-native.ps1`.
- [ ] **9.3 `README.md:40`** (the post-pass row of `## Requirements`, line 26) and `README.md:120` — the honest
  claim: the post pass "runs without an upscaler", **verified on NVIDIA** (D3D11 and D3D12, with the failure both
  faked and forced by removing `nvngx_dlss.dll`); non-NVIDIA smoke evidence is still pending, so no universal
  claim. Reconcile with the existing 1.4.0 sentence at `README.md:120`.
- [ ] **9.4 `docs\release-notes-1.4.0.md`** — one bullet under `## Highlights`, in the file's shape
  (`- **Bold lead-in** -- prose`, ASCII double hyphen, commit sha in parens): the post pass now runs on any GPU
  where the upscaler cannot start.

Check: `git -C E:\DEV\PhoenixPoint\Renderforge diff --stat` shows only the three doc files.

Commit: `docs: record the post-only pipeline and its dev switch`

---

## Self-review

Before declaring the plan done, verify each of these against the working tree — with a command, not from memory:

1. **No device leak.** `Select-String -Path native\Device11.cpp,native\Device12.cpp -Pattern 'device->Release'` —
   every remaining release is either in `Shutdown()` or on a path that returns a code with `PostAlive() == false`.
2. **`ngxInitialized` still success-only.** It is assigned 1 only after `NVSDK_NGX_FAILED(r)` was false
   (`Device11.cpp:175`, `Device12.cpp:98`); the fake switch returns before that point.
3. **Idempotence.** `Dlss_Init` returns early for both `DLSS_OK` and `DLSS_OK_POST_ONLY`, and both backends'
   `Init` replay guards test `ngxInitialized || device`. The probe's second `Dlss_Init` call (4.1) proves it.
4. **Auto fallback preserved.** The loop in `InitNative` is byte-identical to the old one apart from the
   `postCarrier` line; post-only is only accepted after `NextFallback` returns `Off`.
5. **A working upscaler is never traded away.** Task 6.4's guard, checked live by 8.7.
6. **Failed provider still recorded.** In post-only `Upscalers.Failed`/`FailedCode` hold the carrier and the
   *retained* code (2/3/4), never 8 — so `src\Availability.cs:51-75` produces the same greying text as today (8.9).
7. **Debug passthrough unchanged.** `DebugView.Passthrough` still zeroes jitter (`src\DlssDriver.cs:465`) and now
   also still zeroes sharpness (7.3).
8. **Behaviour change is the intended one.** With `Sharpness` defaulting to 40 (`src\DlssConfig.cs:58`), mode Off
   now starts a passthrough generation for every player, on every GPU. That is the feature; call it out in the
   release notes and confirm the frame cost in 8.3.
9. **Everything green is committed.** One conventional commit per task, in the inner repo
   `E:\DEV\PhoenixPoint\Renderforge`, local only.
