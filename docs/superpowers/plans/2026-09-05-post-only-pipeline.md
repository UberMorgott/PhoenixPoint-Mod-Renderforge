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
native\Device11.cpp             keep device on NGX failure; fake result INSIDE the real branches; PostAlive override
native\Device12.cpp             same, after ring/sharpen Attach
native\RenderforgeNative.cpp    S.providerCode, POST_ONLY mapping, idempotence guard, RfFakeInitCode, Dlss_PostOnlyReason
native\probe\dlss_probe.cpp     + --fake=N mode (RunPostOnly11, pattern upload + readback), code 8 = untested in the DLSS runs
build-native.ps1                + gate the --fake=2 probe run
src\Native.cs                   + DLSS_OK_POST_ONLY, + PostOnlyReason()
src\RenderforgeMod.cs           + PostOnly, InitNative NGX post carrier, ReinitNative carrier-preserving rollback, Reason(), GetStatus
src\DlssDriver.cs               wantMode forced Off, mode recomputed after ReinitNative, needsPipeline includes sharpness, sharpness zeroing by VIEW
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
// DEV ONLY. RENDERFORGE_FAKE_INIT injects an NGX failure with the device already acquired - the post-only path
// without a non-NVIDIA GPU. It never short-circuits the handler it is testing:
//   2 = substitute a failing NVSDK_NGX_Result for the *_Init_with_ProjectID result, so the PRODUCTION failure
//       branch (the one that must keep the device) runs unchanged. Lands on every GPU.
//   3 = force available = 0, 4 = force needsDriver = 1, i.e. the real capability branches. Both need NGX to have
//       come up, so they are NVIDIA-only; elsewhere the real handler answers 2 first.
// 0 = off. Read once, cached.
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

- [ ] **3.3 `native\Device11.cpp:172-174`** — inject INTO the production handler, never before it. A switch that
  returned early (`if (fake) return initCode = fake;` after line 163) would skip the branch at line 174 entirely, so
  reverting Task 1.1 would still pass every check. Replace

```cpp
        NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY, kEngineVersion,
                                                                 logDir ? logDir : L".", device, &common, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; return initCode = DLSS_ERR_INIT_FAILED; }
```

with

```cpp
        NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY, kEngineVersion,
                                                                 logDir ? logDir : L".", device, &common, NVSDK_NGX_Version_API);
        // DEV ONLY (RfFakeInitCode, Device.h): =2 substitutes a failing result so the REAL branch below runs, device
        // retention included. On a box where NGX did come up its context is then left to process teardown on purpose
        // - ngxInitialized stays 0, so Shutdown() must not call NVSDK_NGX_D3D11_Shutdown1 on it.
        const int fake = RfFakeInitCode();
        if (fake == 2) r = NVSDK_NGX_Result_FAIL_PlatformError;
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; return initCode = DLSS_ERR_INIT_FAILED; }
```

  and after the four `NVSDK_NGX_Parameter_Get*` reads (lines 181-184), immediately before `if (needsDriver)`:

```cpp
        if (fake == 4) needsDriver = 1;      // real branch: return initCode = DLSS_ERR_NEEDS_DRIVER  (line 186)
        else if (fake == 3) available = 0;   // real branch: return initCode = DLSS_ERR_NOT_AVAILABLE (line 187)
```

- [ ] **3.4 `native\Device12.cpp:95-97` and `:104-107`** — the identical pair, with
  `NVSDK_NGX_D3D12_Init_with_ProjectID` / `NVSDK_NGX_D3D12_Shutdown1` named in the comment. `ring`, `sharpen` and
  `RfDbg` are already attached at that point (`Device12.cpp:84-86`), so ownership matches the real post-only path:

```cpp
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY, kEngineVersion,
                                                                 logDir ? logDir : L".", device, &common, NVSDK_NGX_Version_API);
        const int fake = RfFakeInitCode();
        if (fake == 2) r = NVSDK_NGX_Result_FAIL_PlatformError;
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; return initCode = DLSS_ERR_INIT_FAILED; }
```

```cpp
        if (fake == 4) needsDriver = 1;      // real branch: return initCode = DLSS_ERR_NEEDS_DRIVER  (line 109)
        else if (fake == 3) available = 0;   // real branch: return initCode = DLSS_ERR_NOT_AVAILABLE (line 110)
```

- [ ] **3.5** FSR/XeSS are deliberately untouched — the switch exists to fake *NGX* failure. That is also what makes
  8.7 testable: a session launched with the switch on can still bring FSR/XeSS up normally.

Build: `.\build-native.ps1`
Check: with the switch unset nothing changes (probe runs still `code=0`) — `NVSDK_NGX_Result_FAIL_PlatformError` is
the same constant `Xess12.cpp:161` already maps to, so no new header is needed. Task 4 is the check that proves the
switch works.

Commit: `feat(native): add RENDERFORGE_FAKE_INIT dev switch`

---

## Task 4 — Probe: `--fake=N` covers the `Dlss_Init` result mapping

`dlss_probe` links `RenderforgeNative` directly (`native\CMakeLists.txt:94-97`) and creates a hardware D3D11 device
(`native\probe\dlss_probe.cpp:51`), so this check runs on **any** GPU — including the machines that cannot run NGX.

- [ ] **4.1 `native\probe\dlss_probe.cpp`** — add `#include <vector>` next to the existing includes (line 10) and,
  after `RunD3D11` (line 125), the pattern + readback helpers and the run itself. A status code alone proves
  nothing: `Report` only reads `NVSDK_NGX_Result`, and uninitialised textures make "the output changed" unprovable.
  The upload + `CopyResource` + `Map` shape is the one `GradeProbe::Run` already uses
  (`native\probe\lut_probe.cpp:39-70`, staging desc at `:43-44`, map/copy at `:64-70`) and `CvProbe`
  (`native\probe\colour_vision_probe.cpp:76`) repeats; those two link `Sharpen.cpp` directly and cannot be reused as
  code from here (`native\CMakeLists.txt:102-115` vs `:94-97`), so the shape is copied, not the file:

```cpp
// R8G8B8A8_UNORM is a first-class sharpen view format (SharpenViewFormat default arm, native\Sharpen.cpp:183) and
// is trivially comparable on the CPU, unlike the FP16 the D3D11 game path uses.
static ID3D11Texture2D* MakeTexInit(ID3D11Device* dev, unsigned w, unsigned h, const void* pixels, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA data = { pixels, w * 4u, 0 };
    ID3D11Texture2D* t = NULL;
    HRESULT hr = dev->CreateTexture2D(&d, &data, &t);
    if (FAILED(hr)) { printf("CreateTexture2D(init) %ux%u failed hr=0x%08X\n", w, h, (unsigned)hr); g_failed = 1; }
    return t;
}

// Deterministic and non-uniform: 8x8 checker tiles (NIS/RCAS only move pixels where neighbours differ) over a
// horizontal ramp (a grade has something to bend). A flat fill would pass even if no pass ever ran.
static void FillPattern(std::vector<unsigned>& px, unsigned w, unsigned h)
{
    px.resize(size_t(w) * h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            unsigned checker = (((x >> 3) ^ (y >> 3)) & 1u) ? 230u : 25u;
            unsigned ramp = 16u + (x * 200u) / (w - 1);
            px[size_t(y) * w + x] = 0xFF000000u | (checker << 16) | (ramp << 8) | (checker ^ 0xFFu);
        }
}

static bool ReadBack(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src,
                     unsigned w, unsigned h, std::vector<unsigned>& out)
{
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    d.BindFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = NULL;
    if (FAILED(dev->CreateTexture2D(&d, NULL, &stage))) { printf("readback staging texture failed\n"); return false; }
    ctx->CopyResource(stage, src);
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { printf("readback Map failed hr=0x%08X\n", (unsigned)hr); stage->Release(); return false; }
    out.resize(size_t(w) * h);
    for (unsigned y = 0; y < h; ++y)
        memcpy(out.data() + size_t(y) * w, (const char*)m.pData + size_t(y) * m.RowPitch, w * sizeof(unsigned));
    ctx->Unmap(stage, 0);
    stage->Release();
    return true;
}

// Pixels differing by more than one 8-bit step in any of R/G/B: a copy through the compute pass may round, an
// effect may not.
static size_t DiffCount(const std::vector<unsigned>& a, const std::vector<unsigned>& b)
{
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            int d = int((a[i] >> (c * 8)) & 0xFFu) - int((b[i] >> (c * 8)) & 0xFFu);
            if (d > 1 || d < -1) { ++n; break; }
        }
    return n;
}

// --fake=N: RENDERFORGE_FAKE_INIT injected an NGX failure while the device stayed alive. Dlss_Init must answer
// DLSS_OK_POST_ONLY, remember the retained code, and the analytic post pass must still produce real pixels.
static int RunPostOnly11(const wchar_t* dllDir, const wchar_t* cwd, int fake)
{
    ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed hr=0x%08X\n", (unsigned)hr); return 1; }

    ID3D11Texture2D* any = MakeTex(dev, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    int init = Dlss_Init(any, dllDir, cwd);
    int reason = Dlss_PostOnlyReason();
    // fake=2 substitutes the NGX init result, so it lands on every GPU. fake=3|4 reach the capability branches only
    // where NGX itself came up; elsewhere the real handler answers 2 first - still POST_ONLY, still the point.
    printf("Dlss_Init      fake=%d code=%d (expect %d) api=%d (expect 11) postOnlyReason=%d (expect %d%s)\n",
           fake, init, DLSS_OK_POST_ONLY, Dlss_Api(), reason, fake, fake == 2 ? "" : " or 2");
    if (init != DLSS_OK_POST_ONLY || Dlss_Api() != 11
        || !(reason == fake || (fake != 2 && reason == DLSS_ERR_INIT_FAILED))) g_failed = 1;

    // Idempotence: a retry must replay POST_ONLY, not re-enter Init() on the retained device.
    if (Dlss_Init(any, dllDir, cwd) != DLSS_OK_POST_ONLY) { printf("re-init did not replay POST_ONLY\n"); g_failed = 1; }
    if (g_failed) { Dlss_Shutdown(); any->Release(); ctx->Release(); dev->Release(); return 1; }

    const unsigned W = 256, H = 128;
    std::vector<unsigned> src, plain, sharpened, graded;
    FillPattern(src, W, H);
    ID3D11Texture2D* color = MakeTexInit(dev, W, H, src.data(), D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* out   = MakeTex(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (g_failed) { Dlss_Shutdown(); if (color) color->Release(); if (out) out->Release(); any->Release(); ctx->Release(); dev->Release(); return 1; }

    RenderEventAndDataFn evd = (RenderEventAndDataFn)Dlss_GetRenderEventAndDataFunc();
    Dlss_Passthrough(1);
    int c = 0, e = 0, alive = 0;

    // 1) Nothing enabled: the passthrough copy must reproduce the upload.
    void* slot = Dlss_GetFrameSlot();
    Dlss_SetFrame(slot, color, NULL, NULL, out, 0, 0, 0, 0, 1, 16.6f, W, H, 1.0f, 0.0f, DLSS_LUT_OFF, 0.0f);
    evd(DLSS_EV_EVALUATE, slot);
    Dlss_Status(&c, &e, &alive);
    Report("PostOnly copy", e);
    if (!ReadBack(dev, ctx, out, W, H, plain)) g_failed = 1;

    // 2) NIS sharpen alone must move the checker edges.
    slot = Dlss_GetFrameSlot();
    Dlss_SetFrame(slot, color, NULL, NULL, out, 0, 0, 0, 0, 0, 16.6f, W, H, 1.0f, 1.0f, DLSS_LUT_OFF, 0.0f);
    evd(DLSS_EV_EVALUATE, slot);
    Dlss_Status(&c, &e, &alive);
    Report("PostOnly sharp", e);
    if (!ReadBack(dev, ctx, out, W, H, sharpened)) g_failed = 1;

    // 3) LUT + scene style + colour vision with sharpness 0: the analytic effects on their own.
    slot = Dlss_GetFrameSlot();
    Dlss_SetFrame(slot, color, NULL, NULL, out, 0, 0, 0, 0, 0, 16.6f, W, H, 1.0f, 0.0f, DLSS_LUT_VIVID, 1.0f);
    Dlss_SetSceneStyle(slot, 1, 1.0f, 4);
    Dlss_SetColorVision(slot, DLSS_CV_DEUTERANOPIA);
    evd(DLSS_EV_EVALUATE, slot);
    Dlss_Status(&c, &e, &alive);
    Report("PostOnly grade", e);
    if (!ReadBack(dev, ctx, out, W, H, graded)) g_failed = 1;
    ctx->Flush();

    size_t copyDiff = DiffCount(plain, src), sharpDiff = DiffCount(sharpened, src);
    size_t gradeDiff = DiffCount(graded, src), crossDiff = DiffCount(sharpened, graded);
    printf("PostOnly pixels copy=%zu (expect 0) sharp=%zu (expect >0) grade=%zu (expect >0) sharp-vs-grade=%zu (expect >0)\n",
           copyDiff, sharpDiff, gradeDiff, crossDiff);
    if (copyDiff != 0 || sharpDiff == 0 || gradeDiff == 0 || crossDiff == 0) g_failed = 1;

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

- [ ] **4.2 `native\probe\dlss_probe.cpp:59-61` and `:275-276`** — the ORDINARY DLSS runs must stop treating code 8
  as a failure. On every non-RTX machine `Dlss_Init` now answers 8 where it used to answer 3/4, so the untouched
  gate would turn `build-native.ps1` red there. Code 8 means the same thing for these two runs — DLSS itself is
  untested here — and the retained device must be released before the early return, which the current 3/4 arm does
  not do. D3D11 (`:59-61`):

```cpp
    // NGX refuses on a non-RTX / non-NVIDIA GPU or an old driver: not a defect of ours (3, like --fsr / --xess).
    // Code 8 is the same refusal with the device retained - the post pass is gated separately by --fake=2 below.
    if (init == DLSS_OK_POST_ONLY || init == DLSS_ERR_NOT_AVAILABLE || init == DLSS_ERR_NEEDS_DRIVER) {
        printf("NGX unsupported here (code %d, retained reason %d): DLSS untested on this machine\n", init, Dlss_PostOnlyReason());
        Dlss_Shutdown(); any->Release(); ctx->Release(); dev->Release();
        return 3;
    }
    if (init != DLSS_OK) { printf("NGX init not ok, see nvngx.log in %ls\n", cwd); Dlss_Shutdown(); any->Release(); ctx->Release(); dev->Release(); return 1; }
```

  D3D12 (`:275-276`), releasing what `InitD3D12` and `MakeTex12` own, in the order `RunD3D12`'s own tail uses
  (`:368-373`):

```cpp
    if (init == DLSS_OK_POST_ONLY || init == DLSS_ERR_NOT_AVAILABLE || init == DLSS_ERR_NEEDS_DRIVER) {
        printf("NGX unsupported here (code %d, retained reason %d): DLSS D3D12 untested on this machine\n", init, Dlss_PostOnlyReason());
        Dlss_Shutdown(); any->Release(); g_fence->Release(); g_queue->Release(); g_dev12->Release();
        return 3;
    }
    if (init != DLSS_OK) { printf("NGX D3D12 init not ok, see nvngx.log in %ls\n", cwd); Dlss_Shutdown(); any->Release(); g_fence->Release(); g_queue->Release(); g_dev12->Release(); return 1; }
```

- [ ] **4.3 `native\probe\dlss_probe.cpp:646-659`** — parse the flag and route. In `wmain`, add `int fake = 0;`
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

- [ ] **4.4 `native\probe\dlss_probe.cpp:2`** — update the usage comment to list `[--fake=2|3|4]` and to say that
  exit 3 now also covers a POST_ONLY init in the ordinary DLSS runs.

- [ ] **4.5 `build-native.ps1`** — after the `--xess` run (line 88-89), inside the same `try` block:

```powershell
    & (Join-Path $outDir 'dlss_probe.exe') $outDir --fake=2
    $rcFake = $LASTEXITCODE
```

and after the XeSS gate (line 100):

```powershell
# Post-only must work on EVERY GPU, so exit 3 is not tolerated here: --fake=2 substitutes the NGX init result and
# is therefore deterministic on any device. (--fake=3|4 need a working NGX init, so they stay a manual run.)
if ($rcFake -ne 0) { throw "dlss_probe (--fake=2, post-only) failed ($rcFake)" }
```

Build + check (one command, the gate is the check):

```powershell
cd E:\DEV\PhoenixPoint\Renderforge; .\build-native.ps1
```

Expected new lines: `Dlss_Init fake=2 code=8 (expect 8) api=11 (expect 11) postOnlyReason=2 (expect 2)`,
`PostOnly copy 0x00000001`, `PostOnly sharp 0x00000001`, `PostOnly grade 0x00000001`,
`PostOnly pixels copy=0 (expect 0) sharp=<n>0 grade=<n>0 sharp-vs-grade=<n>0`, `build-native: OK`.
Also run `.\out\dlss_probe.exe .\out --fake=3` and `--fake=4` once by hand on this RTX box (postOnlyReason 3 / 4).

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
  preserved exactly as today; post-only is accepted only after the alternatives are exhausted, and only then is NGX
  probed as a last-resort post carrier (an Auto run that never met an NGX backend has no carrier to restore):

```csharp
        private static bool InitNative(UpscalerKind want)
        {
            var m = Instance;
            UpscalerKind first = Upscalers.Running = Upscalers.Resolve(want);
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
            // An Auto chain that started on FSR/XeSS can exhaust itself without ever meeting an NGX backend, and
            // those two release the device on every failure path (Fsr12.cpp:147,156; Xess12.cpp:202,212) - so no
            // carrier was recorded and the post pass would die with them. NGX is the only backend that keeps its
            // device, and NextFallback never returns DLSS (Upscaler.cs:52-59), so probe it once, here. Skipped when
            // Auto already started on DLSS: it was tried, and a retry answers the same code.
            if (InitCode != Native.DLSS_OK && InitCode != Native.DLSS_OK_POST_ONLY
                && postCarrier == UpscalerKind.Off && want == UpscalerKind.Auto && first != UpscalerKind.DLSS)
            {
                Native.Dlss_Shutdown();
                Upscalers.Running = UpscalerKind.DLSS;
                Native.SetProvider(Upscalers.ProviderOf(UpscalerKind.DLSS));
                InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
                if (InitCode == Native.DLSS_OK_POST_ONLY) postCarrier = UpscalerKind.DLSS;
                m.Logger.LogInfo("no upscaler available: NGX probed as the post carrier (code " + InitCode + ")");
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

- [ ] **6.4 `src\RenderforgeMod.cs:125-140`** — replace the whole of `ReinitNative`. Two things are wrong with the
  current body once post-only exists: a switch must not trade a WORKING upscaler for post-only, and the rollback
  restores `prev`, which in a post-only session is `Off` (6.3 parks the carrier in `Upscalers.Failed` and sets
  `Running = Off`) — so `Native.Dlss_Shutdown()` at `:130` would destroy the carrier and `InitNative(prev)` would
  never run, leaving the mod with no device and no post pass:

```csharp
        internal static void ReinitNative(UpscalerKind want)
        {
            var m = Instance;
            if (m == null || probeTex == null) return;
            UpscalerKind prev = Upscalers.Running;
            // In post-only Running is Off and the live carrier sits in Upscalers.Failed (see InitNative). Remember
            // it, or the rollback below has nothing to restore and the post pass dies with the failed switch.
            bool prevPostOnly = PostOnly;
            UpscalerKind prevCarrier = prevPostOnly ? Upscalers.Failed : UpscalerKind.Off;
            Native.Dlss_Shutdown();
            // Post-only is a good answer when nothing was running; it is a regression when it replaces a live
            // provider, so treat that exactly as a hard failure and roll back.
            if (InitNative(want) && !(PostOnly && prev != UpscalerKind.Off))
            {
                m.Logger.LogInfo("upscaler switched to " + Upscalers.Running + (PostOnly ? " (post pass only)" : " version=" + Native.ProviderVersion()));
                return;
            }
            m.Logger.LogWarning(Upscalers.Failed + " init failed (code " + InitCode + "): back to "
                                + (prev != UpscalerKind.Off ? prev.ToString() : prevCarrier + " (post pass only)"));
            UpscalerKind failed = Upscalers.Failed; int code = Upscalers.FailedCode;
            Native.Dlss_Shutdown();
            UpscalerKind back = prev != UpscalerKind.Off ? prev : prevCarrier;
            // Restoring the carrier lands on POST_ONLY again, which is a SUCCESS for this call: InitNative sets
            // PostOnly/Available itself, and the picker keeps the reason of the provider the user actually asked for.
            if (back != UpscalerKind.Off && InitNative(back)) { Upscalers.Failed = failed; Upscalers.FailedCode = code; }
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
Check: build succeeds, and no bare `DLSS_OK` comparison treats post-only as a failure. `DLSS_OK\b` does not match
`DLSS_OK_POST_ONLY` (`_` is a word character), so the grep still finds exactly the plain-success tests — but 6.3
legitimately introduces four of them, and `:150` loses its one (6.5). Allowlist those, fail on anything else:

```powershell
$allow = @(
  'while (InitCode != Native.DLSS_OK && want == UpscalerKind.Auto)',
  'if (InitCode != Native.DLSS_OK && InitCode != Native.DLSS_OK_POST_ONLY',
  '&& postCarrier == UpscalerKind.Off && want == UpscalerKind.Auto && first != UpscalerKind.DLSS)',
  'if (InitCode != Native.DLSS_OK && InitCode != Native.DLSS_OK_POST_ONLY && postCarrier != UpscalerKind.Off)',
  'if (InitCode == Native.DLSS_OK_POST_ONLY && postCarrier == UpscalerKind.Off) postCarrier = Upscalers.Running;',
  'if (InitCode == Native.DLSS_OK_POST_ONLY) postCarrier = UpscalerKind.DLSS;',
  'PostOnly = InitCode == Native.DLSS_OK_POST_ONLY;',
  'Available = InitCode == Native.DLSS_OK || PostOnly;'
)
$bad = Select-String -Path E:\DEV\PhoenixPoint\Renderforge\src\*.cs -Pattern 'DLSS_OK' |
       Where-Object { $_.Filename -ne 'Native.cs' -and $allow -notcontains $_.Line.Trim() }
if ($bad) { $bad; throw 'post-only success handled somewhere unreviewed' }
```

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

- [ ] **7.4 `src\DlssDriver.cs:207-214`** — 7.1 latches `wantMode = Off` at the moment `Apply` runs, but the
  provider switch is asynchronous: `SwitchProvider` only records `switchTo` (`:125-130`) and the shim is re-inited
  frames later, in the `Gen.Idle` arm. A PostOnly -> FSR/XeSS switch therefore succeeds while `wantMode` still says
  `Off` from the post-only era, and nothing recomputes it until the next `OnConfigChanged`/`OnLevelStart`. Recompute
  it right where `PostOnly` can have changed, before any generation is created:

```csharp
                        UpscalerKind k = switchTo; switchTo = UpscalerKind.Off;
                        RenderforgeMod.ReinitNative(k);
                        // ReinitNative may have just left post-only (or entered it): Apply is the single funnel that
                        // maps Cfg.Mode through RenderforgeMod.PostOnly, so re-run it before StartGeneration.
                        Apply(RenderforgeMod.Instance?.Cfg?.Mode ?? RenderforgeMode.Off, wantView);
                        break;   // needsPipeline was computed above with the stale mode; re-enter Idle next frame
```

- [ ] **7.5** Leave `src\DlssDriver.cs:235` (`MipBias.Apply(passthrough ? 0f : ...)`) alone — bias stays 0 with no
  upscaling, which is correct.

- [ ] **7.6** Leave `src\Overlay.cs:138` (`live = d != null && d.IsLive && !d.Passthrough`) alone: in post-only
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
by launching the game with `$env:RENDERFORGE_FAKE_INIT = '2'` set in the launching shell (the shim reads it once, on
the first `Dlss_Init`, so it CANNOT be turned on mid-session); clear it with `Remove-Item Env:\RENDERFORGE_FAKE_INIT`
between cases. `2` is the case that lands on every GPU; `3`/`4` exercise the capability branches and work on this
RTX box only.

**Pin the upscaler for every post-only case.** The Auto fallback is deliberately preserved (6.3), so on this box
under D3D12 a faked NGX failure just falls through to a WORKING FSR and `postOnly` stays False — correct behaviour,
useless as a post-only test. Set the pin before the case and confirm it in the same reply:

```powershell
& $Cli connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetUpscaler","args":["DLSS"]}' -PPRoot $PP
```

A concrete pin makes `want != UpscalerKind.Auto`, so `InitNative`'s fallback loop never runs and the NGX failure is
reported as post-only. Restore `Auto` after the post-only block.

- [ ] **8.1 Baseline, switch OFF (regression gate).** D3D11 and D3D12, DLSS/FSR/XeSS as today: `GetStatus` must
  show `postOnly=False`, `provider=` the real upscaler, `gen=Live passthrough=False`. Capture one tactical frame per
  API for the later identity comparisons.
- [ ] **8.2 `RENDERFORGE_FAKE_INIT=2`, upscaler pinned to DLSS, D3D11.** `GetStatus` → `postOnly=True postOnlyReason=2 provider=Off`,
  `gen=Live passthrough=True`, overlay `Upscaler: off (…)`. `Dlss_LastError` (in the driver `Status` string) = 0.
- [ ] **8.3 NIS-only, post-only, D3D11.** `SetMode Off None`, LUT Off, style Off, CV None, `SetSharpness 100` vs
  `SetSharpness 0`: `cmp.py` scene mean |Δ| ≥ 1.0 on ≥1 channel; HUD ≤ the control floor + 0.5 (the pass runs on the
  camera output, not the UI). Then `SetSharpness 0` twice = CONTROL, must be ≤ 0.5 on all channels.
- [ ] **8.4 LUT / scene style / colour vision, post-only, D3D11.** One case each, off-vs-on, same criteria as
  colour-vision plan `:1586-1590`. Colour vision additionally: deut-vs-prot and deut-vs-trit scene ≥ 1.0.
- [ ] **8.5 Repeat 8.2-8.4 under D3D12** (`SetRenderer D3D12` + restart), upscaler still pinned to DLSS,
  `RENDERFORGE_FAKE_INIT=2`. Then, D3D11 only and still pinned, once with `=3` and once with `=4` to prove all three
  codes map and `postOnlyReason` echoes each (2/3/4, never 8).
- [ ] **8.5b Auto fallback still wins on D3D12 (the counter-case to 8.5).** Same `RENDERFORGE_FAKE_INIT=2`, but
  `SetUpscaler Auto`, D3D12: NGX fails, `NextFallback` reaches FSR, and the run must end `postOnly=False`
  `provider=FSR` `gen=Live passthrough=False` — a faked NGX failure must NOT disable a working upscaler. Repeat with
  `amd_fidelityfx_*.dll` renamed aside so Auto continues to XeSS (`provider=XeSS`, `postOnly=False`); restore the
  files. Only with BOTH renamed aside does Auto land in post-only (`postOnly=True`, carrier DLSS — 6.3's NGX probe).
- [ ] **8.6 Real failure, no fake switch.** Upscaler pinned to DLSS (same reason as 8.5), rename `nvngx_dlss.dll` to
  `nvngx_dlss.dll.off` in the Instance3 mod folder, launch: `postOnly=True`, `postOnlyReason=` 2, 3 or 4 (record
  which — NGX may init and only report the feature unavailable), post effects still work on both APIs. Restore the
  file afterwards.
- [ ] **8.7 Init / re-init / shutdown.** With the switch on (`=2`): renderer switch D3D11↔D3D12; `SetUpscaler DLSS`
  → `FSR` → `XeSS` → `Auto` (each must leave the mod alive, no `broken=True`, no `lastError` != 0; `postOnly=True`
  wherever the target provider is NGX or unavailable, `postOnly=False` where FSR/XeSS came up); disable the mod in
  the mod manager and re-enable it. After a switch that leaves post-only for a working FSR/XeSS, `GetStatus` must
  show `gen=Live passthrough=False` **without** touching any other setting — that is 7.4's stale-`wantMode` check.
- [ ] **8.7b A working upscaler is never traded for post-only (Task 6.4).** The switch is read once per process, so
  it cannot be armed mid-session; arm it at LAUNCH and start on a provider it does not touch (3.5). D3D12, launch
  with `$env:RENDERFORGE_FAKE_INIT = '2'` and `Cfg.Upscaler = FSR` (`SetUpscaler FSR` in the previous session, so it
  is saved): FSR comes up normally (`postOnly=False provider=FSR gen=Live passthrough=False`). Then request the
  faked provider through the same seam the picker uses:

```powershell
& $Cli connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetUpscaler","args":["DLSS"]}' -PPRoot $PP
```

  `InitNative` answers POST_ONLY, 6.4 rejects it and rolls back. Expected afterwards: `provider=FSR postOnly=False`,
  `gen=Live passthrough=False`, log `DLSS init failed (code 8): back to FSR`, and the picker's UPSCALER row greyed
  with the DLSS reason (`Upscalers.Failed=DLSS`, `FailedCode=2`). Repeat with XeSS as the starting provider.
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
  (`- **Bold lead-in** -- prose`, ASCII double hyphen, commit sha in parens): the post pass now runs even when the
  upscaler cannot start. Use the SAME qualification as 9.3 — verified on NVIDIA under D3D11 and D3D12, with the
  failure both faked and forced; non-NVIDIA is expected to work but is not yet smoke-tested. No "any GPU" claim
  anywhere until that evidence exists.

Check: `git -C E:\DEV\PhoenixPoint\Renderforge diff --stat` shows only the three doc files.

Commit: `docs: record the post-only pipeline and its dev switch`

---

## Self-review

Before declaring the plan done, verify each of these against the working tree — with a command, not from memory:

1. **No device leak.** `Select-String -Path native\Device11.cpp,native\Device12.cpp -Pattern 'device->Release'` —
   every remaining release is either in `Shutdown()` or on a path that returns a code with `PostAlive() == false`.
2. **`ngxInitialized` still success-only.** It is assigned 1 only after `NVSDK_NGX_FAILED(r)` was false
   (`Device11.cpp:175`, `Device12.cpp:98`). The fake switch does not bypass that test — it feeds it a failing
   result (`fake == 2`) or bends the capability flags read after it (`3`/`4`), so every branch that runs is a
   production branch, and a revert of Task 1.1 fails the `--fake=2` gate.
3. **Idempotence.** `Dlss_Init` returns early for both `DLSS_OK` and `DLSS_OK_POST_ONLY`, and both backends'
   `Init` replay guards test `ngxInitialized || device`. The probe's second `Dlss_Init` call (4.1) proves it.
4. **Auto fallback preserved.** The loop in `InitNative` is byte-identical to the old one apart from the
   `postCarrier` line; post-only is only accepted after `NextFallback` returns `Off`, and the extra NGX carrier
   probe runs only when the whole chain failed and never started on DLSS. Proven live by 8.5b: a faked NGX failure
   must still end on a working FSR (then XeSS) before it may end in post-only.
5. **A working upscaler is never traded away, and the carrier survives a failed switch.** Task 6.4's guard plus
   its `prevCarrier` rollback, checked live by 8.7b (`provider=FSR` after a rejected DLSS request).
5b. **No stale mode after an async switch.** 7.4 re-runs `Apply` after `ReinitNative`; 8.7 checks
   `gen=Live passthrough=False` straight after a PostOnly -> FSR/XeSS switch, with nothing else touched.
5c. **Post output is proven by pixels, not status codes.** 4.1 uploads a known pattern and asserts
   copy == input, sharpen != input, grade != input, sharpen != grade.
6. **Failed provider still recorded.** In post-only `Upscalers.Failed`/`FailedCode` hold the carrier and the
   *retained* code (2/3/4), never 8 — so `src\Availability.cs:51-75` produces the same greying text as today (8.9).
7. **Debug passthrough unchanged.** `DebugView.Passthrough` still zeroes jitter (`src\DlssDriver.cs:465`) and now
   also still zeroes sharpness (7.3).
8. **Behaviour change is the intended one.** With `Sharpness` defaulting to 40 (`src\DlssConfig.cs:58`), mode Off
   now starts a passthrough generation for every player, on every GPU. That is the feature; call it out in the
   release notes and confirm the frame cost in 8.3.
9. **Everything green is committed.** One conventional commit per task, in the inner repo
   `E:\DEV\PhoenixPoint\Renderforge`, local only.
