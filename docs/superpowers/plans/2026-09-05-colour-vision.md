# Colour Vision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship section **B** of `docs\superpowers\specs\2026-09-05-quality-knobs-colour-vision-design.md` — a fixed-strength daltonization stage (`None / Deuteranopia / Protanopia / Tritanopia`) that runs **after** `Grade()` and the scene style inside the existing analytic post pass, composes with any LUT preset or style, and activates on its own with LUT=Off / style=Off / sharpness=0.

**Architecture:** One new HLSL stage at the tail of `kColorGradeHlsl` (`native\Sharpen.cpp:44-74`). The 3×3 correction `D = I + R·(I − S)` is precomputed on the CPU as a `constexpr` (`native\ColorVision.h`, new) and uploaded through the *existing* 256-byte constant block as three `float4` rows plus one `uint` mode. Because the stage lives in the analytic shader (not NIS/RCAS), the whole activation problem reduces to one predicate: every site that currently reads `ColorGradeEnabled(...) || SceneStyleEnabled(...)` becomes `PostShaderEnabled(..., colorVision)`. Managed side mirrors the `SceneStyle` shape exactly: config enum → `Dlss_SetColorVision` on the frame slot → `ColorVisionPanel` row cloned from `TextureQualityPicker`.

**Tech Stack:** C++17 + HLSL `cs_5_0` (`native\`, CMake / VS2022 x64), C# net472 Harmony mod (`src\`, `dotnet build`), Python 3 stdlib-only reference script, D3D11 WARP probe for offline verification, PPCLI on `D:\PP-Instance3` for live acceptance.

---

## Matrix provenance (settled before implementation — do not re-derive)

| Symbol | Value | Source |
|---|---|---|
| `S` deuteranopia 1.0 | `0.367322 0.860646 -0.227968 / 0.280085 0.672501 0.047413 / -0.011820 0.042940 0.968881` | Machado, Oliveira & Fernandes 2009, authors' own table — https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html — **verified exact match** |
| `S` protanopia 1.0 | `0.152286 1.052583 -0.204868 / 0.114503 0.786281 0.099216 / -0.003882 -0.048116 1.051998` | same page — **verified exact match** |
| `S` tritanopia 1.0 | `1.255528 -0.076749 -0.178779 / -0.078411 0.930809 0.147602 / 0.004733 0.691367 0.303900` | same page — **verified exact match** |
| `R` **protan** | `0 0 0 / 0.7 1 0 / 0.7 0 1` | Fidaner, Lin & Ozguven 2005, `err2mod` verbatim in their MATLAB — https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m : `err2mod = [0 0 0; .7 1 0; .7 0 1];`, applied as `ERR(i,j,:) = err2mod * err;` with `err = errorp(i,j,:)`. Report: http://acorn.stanford.edu/psych221/projects/2005/ofidaner/colorblindness_project.htm |
| `R` **deutan** | same matrix as protan | The MATLAB above corrects **only** the protan error (`errorp`); its deuteranopia output is the plain simulation, so it is *not* a citation for a deutan `R`. The citation that is: the maintained Python port of the same method, https://github.com/joergdietrich/daltonize — `daltonize/daltonize.py:125` defines the single `err2mod = np.array([[0, 0, 0], [0.7, 1, 0], [0.7, 0, 1]])` and applies it for **every** `color_deficit` value (`d`, `p`, `t`); the type argument only selects the simulation matrix passed to `simulate()` at `:120`. Reusing the protan `R` on deutan is therefore that implementation's published behaviour, not our invention. |
| `R` tritan | `1 0 0.7 / 0 1 0.7 / 0 0 0` | https://ixora.io/projects/colorblindness/color-blindness-simulation-research.html — the page's own daltonization-matrix table (protan `0 0 0 / 0.7 1 0 / 0.7 0 1`, deutan `1 0.7 0 / 0 0 0 / 0 0.7 1`, tritan as above). **Secondary source, coefficients NOT independently verified.** The page's author cites Simon-Liedtke & Farup (JVCI 2016) as the *justification* for using a per-deficiency matrix at all; the numbers themselves are not transcribed from that paper here and must not be presented as if they were. |

**Deutan `R` — the decision, stated once.** The spec (§B) says "`R` = the per-type error-redistribution matrix" but does not mandate that deut and prot share one. Two candidates existed: the shared `err2mod` (daltonize.py, above) and ixora's distinct deutan `1 0.7 0 / 0 0 0 / 0 0.7 1`. **Chosen: the shared `err2mod`**, because it is the matrix an actively maintained implementation of the cited method actually applies to `d`, while ixora's deutan variant rests on the same unverified secondary table as its tritan row. Recorded in `ColorVision.h` and in DESIGN so it is not silently re-litigated.

**Tritan status: SHIPPING, flagged.** The spec's fallback ("if no reference is found, defer tritan") does not fire — a citable matrix exists. Its citation is strictly weaker than the deut/prot one, so `ColorVision.h` and the DESIGN note both say so. Deliberate deviation from `daltonize.py`, which would give tritan the same `err2mod`: that matrix pushes the error onto G and **B**, and B is exactly the channel a tritan cannot discriminate — so the per-deficiency tritan matrix is used instead, at the cost of the weaker citation.

The colorspace R-package vignette (https://colorspace.r-forge.r-project.org/articles/color_vision_deficiency.html) cites Machado 2009 but prints no numeric table — it could not serve as the cross-check; the authors' own page did.

---

## File structure

```
native\ColorVision.h                    NEW  - S, R, constexpr D = I + R*(I - S)
native\probe\colour_vision_ref.py       NEW  - independent Python reference, prints D
native\probe\colour_vision_probe.cpp    NEW  - WARP probe (bypass / matrices / parity / gamut)
native\Sharpen.h                        EDIT - PostShaderEnabled(), ColorVisionEnabled(), FillSharpenConstants sig
native\Sharpen.cpp                      EDIT - HLSL cbuffer + ColorVision() stage + constant packing (:44-74, :132-151)
native\Device.h                         EDIT - FrameParams.colorVision (:33)
native\RenderforgeNative.h              EDIT - DLSS_CV_* enum + Dlss_SetColorVision (:51, :75)
native\RenderforgeNative.cpp            EDIT - Dlss_SetColorVision (:214)
native\Device11.cpp                     EDIT - Sharpen() sig + predicates (:87, :91, :254, :278)
native\D3D12Sharpen.h                   EDIT - Run/RunPassthrough sigs + predicate (:206, :217, :257, :261, :272)
native\Device12.cpp                     EDIT - predicates + Run call (:188, :189, :196, :198, :220)
native\Fsr12.cpp                        EDIT - predicates + Run call (:289, :290, :297, :298, :346)
native\Xess12.cpp                       EDIT - predicates + Run call (:324, :325, :332, :333, :364)
native\CMakeLists.txt                   EDIT - ColorVision.h in lib sources, colour_vision_probe target (:57, :109)
src\Native.cs                           EDIT - Dlss_SetColorVision DllImport (:111)
src\DlssConfig.cs                       EDIT - ColorVisionMode enum, field, RU labels, HiddenFromModSettings
src\DlssDriver.cs                       EDIT - needsPipeline predicate (:197), SetColorVision send (:504)
src\ColorVisionPanel.cs                 NEW  - picker row, cloned from LutPanel's recipe
src\GraphicsPanel.cs                    EDIT - Hide (:43) + Build (:62)
src\Pickers.cs                          EDIT - Clear (:90)
src\RenderforgeMod.cs                   EDIT - SetColorVision console setter (:338)
README.md, docs\DESIGN.md               EDIT - user-facing + design records
```

---

## Task 1 — Independent Python reference for the matrices

**Files:** `native\probe\colour_vision_ref.py` (new)

- [ ] Create `native\probe\colour_vision_ref.py` with exactly this content:

```python
"""Independent reference for the Renderforge colour-vision correction matrices.

D = I + R*(I - S), column-vector linear RGB (v' = D*v), printed row-major.

S = Machado, Oliveira & Fernandes 2009 simulation matrices at severity 1.0, transcribed from the
    authors' own table: https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html
R = error redistribution.
    protanopia: Fidaner, Lin & Ozguven 2005, "Analysis of Color Blindness" - `err2mod` verbatim in their
        MATLAB (https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m), which applies it to
        the protan error only.
    deuteranopia: the SAME err2mod. Citation: daltonize/daltonize.py:125 in the maintained Python port
        (https://github.com/joergdietrich/daltonize), where the one err2mod is applied for every
        color_deficit value; the type only selects the simulate() matrix (:120).
    tritanopia: per-deficiency redistribution from the matrix table at
        https://ixora.io/projects/colorblindness/color-blindness-simulation-research.html - a SECONDARY
        source whose coefficients are not independently verified (its Simon-Liedtke & Farup 2016 citation
        justifies the per-type approach, it is not the source of these numbers).

Deliberately stdlib-only and written from the published numbers, not from the C++ header, so that
colour_vision_probe.cpp compares two independent derivations rather than one value against itself.
"""

MODES = ("Deuteranopia", "Protanopia", "Tritanopia")

SIMULATE = {
    "Deuteranopia": (( 0.367322,  0.860646, -0.227968),
                     ( 0.280085,  0.672501,  0.047413),
                     (-0.011820,  0.042940,  0.968881)),
    "Protanopia":   (( 0.152286,  1.052583, -0.204868),
                     ( 0.114503,  0.786281,  0.099216),
                     (-0.003882, -0.048116,  1.051998)),
    "Tritanopia":   (( 1.255528, -0.076749, -0.178779),
                     (-0.078411,  0.930809,  0.147602),
                     ( 0.004733,  0.691367,  0.303900)),
}

REDISTRIBUTE = {
    "Deuteranopia": ((0.0, 0.0, 0.0), (0.7, 1.0, 0.0), (0.7, 0.0, 1.0)),
    "Protanopia":   ((0.0, 0.0, 0.0), (0.7, 1.0, 0.0), (0.7, 0.0, 1.0)),
    "Tritanopia":   ((1.0, 0.0, 0.7), (0.0, 1.0, 0.7), (0.0, 0.0, 0.0)),
}

IDENTITY = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def correction(mode):
    s, r = SIMULATE[mode], REDISTRIBUTE[mode]
    error = [[IDENTITY[i][j] - s[i][j] for j in range(3)] for i in range(3)]
    return [[IDENTITY[i][j] + sum(r[i][k] * error[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


if __name__ == "__main__":
    for mode in MODES:
        d = correction(mode)
        print(mode)
        for row in d:
            print("    " + ", ".join("%.9ff" % v for v in row) + ",")
        print("    row sums: " + ", ".join("%.9f" % sum(row) for row in d))
```

- [ ] Run it and confirm the output matches, character for character, the block below:

```
python E:\DEV\PhoenixPoint\Renderforge\native\probe\colour_vision_ref.py
```

Expected output:

```
Deuteranopia
    1.000000000f, 0.000000000f, 0.000000000f,
    0.162789600f, 0.725046800f, 0.112164600f,
    0.454694600f, -0.645392200f, 1.190696600f,
    row sums: 1.000000000, 1.000001000, 0.999999000
Protanopia
    1.000000000f, 0.000000000f, 0.000000000f,
    0.478896800f, 0.476910900f, 0.044191600f,
    0.597281800f, -0.688692100f, 1.091409600f,
    row sums: 1.000000000, 0.999999300, 0.999999300
Tritanopia
    0.741158900f, -0.407207900f, 0.666049000f,
    0.075097900f, 0.585234100f, 0.339668000f,
    0.000000000f, 0.000000000f, 1.000000000f,
    row sums: 1.000000000, 1.000000000, 1.000000000
```

- [ ] Note for later steps: every row sum is 1 to within 1e-6 (the residual is the rounding in the published `S` matrices), so neutral grey/white is preserved. Task 2's probe asserts this.
- [ ] Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add native/probe/colour_vision_ref.py
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test: add independent colour-vision matrix reference"
```

---

## Task 2 — Native matrices, HLSL stage and the probe (TDD: probe first, and it must fail)

**Files:** `native\ColorVision.h` (new), `native\probe\colour_vision_probe.cpp` (new), `native\Sharpen.h`, `native\Sharpen.cpp`, `native\CMakeLists.txt`

### 2a — compilable scaffolding, then the probe, and it must fail for the right reason

> **Ordering rule (do not "optimise" it away).** A probe that cannot *build* is not a red test — CMake
> aborts before a single assertion runs and nothing is proven. So this section first lands the smallest
> scaffolding that makes the probe **compile and run** with the correction **bypassed**: an identity
> `ColorVision.h`, the `Sharpen.h` predicate/signature, and the widened `FillSharpenConstants` — no
> matrices, no HLSL stage, no constant rows. The red is then an *assertion* failure: the run output does
> not match the independent reference. 2b makes the CPU half green, 2c/2d make the GPU half green.

- [ ] **Scaffolding 1/3** — create `native\ColorVision.h` as a stub that returns the identity for every
  mode. Everything except the body of `CvCorrection` is final; 2b fills in the body.

```cpp
// ColorVision.h - daltonization matrices for the analytic post pass. Mode ordinals cross the managed/native
// ABI (DLSS_CV_* in RenderforgeNative.h, ColorVisionMode in src\DlssConfig.cs); append only.
// Provenance and the full source discussion arrive with the real matrices in Task 2b.
#pragma once

// Mirrored by DLSS_CV_* (RenderforgeNative.h) and ColorVisionMode (src\DlssConfig.cs).
enum { RF_CV_NONE = 0, RF_CV_DEUTERANOPIA = 1, RF_CV_PROTANOPIA = 2, RF_CV_TRITANOPIA = 3 };

struct CvMatrix { float m[9]; };   // row-major: m[row * 3 + col]

// TASK 2b REPLACES THIS BODY. Identity for every mode, so the probe links, runs, and fails its
// reference comparison instead of failing to build.
constexpr CvMatrix CvCorrection(int mode)
{
    (void)mode;
    return CvMatrix{ { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f } };
}
```

- [ ] **Scaffolding 2/3** — `native\Sharpen.h`: add the include next to the existing one (`:7`, after
  `#include "SceneStyle.h"`):

```cpp
#include "ColorVision.h"
```

- [ ] **Scaffolding 2/3** — `native\Sharpen.h`: extend the `FillSharpenConstants` declaration (`:17-19`)
  with the new trailing parameter:

```cpp
void FillSharpenConstants(void* dst256, int kind, float sharpness, unsigned w, unsigned h,
                          int lutPreset = 0, float lutStrength = 0.0f, bool hdr = false,
                          const SceneStyleParams& style = SceneStyleParams{}, int colorVision = 0);
```

- [ ] **Scaffolding 2/3** — `native\Sharpen.h`: immediately after `ColorGradeEnabled` (`:21`), add:

```cpp
inline bool ColorVisionEnabled(int mode) { return mode >= RF_CV_DEUTERANOPIA && mode <= RF_CV_TRITANOPIA; }

// The analytic post shader (RCAS + grade + scene style + colour vision) is compiled INSTEAD of NIS/RCAS
// whenever any of its stages is active. Colour vision lives only in that shader, so every "is the post pass
// needed" test must go through here - otherwise LUT=Off + style=Off silently bypasses the correction.
inline bool PostShaderEnabled(int preset, float strength, const SceneStyleParams& style, int colorVision)
{
    return ColorGradeEnabled(preset, strength) || SceneStyleEnabled(style) || ColorVisionEnabled(colorVision);
}
```

- [ ] **Scaffolding 3/3** — `native\Sharpen.cpp`: widen the `FillSharpenConstants` **definition** only
  (`:132-136`) — the new parameter and the new predicate, and nothing else. The body keeps packing exactly
  what it packs today, which is what leaves the correction bypassed:

```cpp
void FillSharpenConstants(void* dst256, int kind, float sharpness, unsigned w, unsigned h,
                          int lutPreset, float lutStrength, bool hdr, const SceneStyleParams& style,
                          int colorVision)
{
    memset(dst256, 0, 256);
    if (PostShaderEnabled(lutPreset, lutStrength, style, colorVision)) {
```

- [ ] Create `native\probe\colour_vision_probe.cpp`:

```cpp
// Exercises the production colour-vision stage without Unity, NGX, a window or a LUT asset.
// The reference matrices below are the printed output of probe/colour_vision_ref.py, which derives
// them from the published Machado / Fidaner / ixora numbers independently of ColorVision.h.
#include "Sharpen.h"
#include "ColorVision.h"
#include "RenderforgeNative.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D11 operation failed"); }
static void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }

// colour_vision_ref.py output, transcribed. Indexed by mode - 1, row-major.
static const float kReference[3][9] = {
    { 1.000000000f,  0.000000000f, 0.000000000f,
      0.162789600f,  0.725046800f, 0.112164600f,
      0.454694600f, -0.645392200f, 1.190696600f },   // Deuteranopia
    { 1.000000000f,  0.000000000f, 0.000000000f,
      0.478896800f,  0.476910900f, 0.044191600f,
      0.597281800f, -0.688692100f, 1.091409600f },   // Protanopia
    { 0.741158900f, -0.407207900f, 0.666049000f,
      0.075097900f,  0.585234100f, 0.339668000f,
      0.000000000f,  0.000000000f, 1.000000000f },   // Tritanopia
};

static float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }
static float Clamp01(float c) { return c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c; }

// CPU model of the shader's ColorVision() stage, driven by kReference (the PYTHON numbers) rather than by
// ColorVision.h. This is the only check that can catch a transposed matrix, a swapped channel or a
// mis-indexed cvRow: every other assertion here is satisfied by any well-behaved 3x3.
// UNORM path: saturate, decode sRGB, apply D, clamp in linear, encode. FP16 path: max(0), apply D, max(0).
static Pixel ReferenceApply(const Pixel& in, int mode, bool hdr)
{
    if (mode < RF_CV_DEUTERANOPIA || mode > RF_CV_TRITANOPIA) return in;
    const float* d = kReference[mode - 1];
    float v[3];
    for (int c = 0; c < 3; ++c) v[c] = hdr ? (in[c] < 0.0f ? 0.0f : in[c]) : SrgbToLinear(Clamp01(in[c]));
    Pixel out = in;
    for (int r = 0; r < 3; ++r) {
        float x = d[r * 3 + 0] * v[0] + d[r * 3 + 1] * v[1] + d[r * 3 + 2] * v[2];
        out[r] = hdr ? (x < 0.0f ? 0.0f : x) : LinearToSrgb(Clamp01(x));
    }
    return out;
}

// GPU pow() vs CPU powf() on WARP differ by ~1e-6 relative; 1.5/255 is loose for that and still tight
// enough that any channel-order or transposition error (which moves cube samples by 0.1..0.6) is caught.
// The tight numeric gate is the encoded/linear parity check further down.
static const float kRefTol = 1.5f / 255.0f;

static void RequireMatchesReference(const std::vector<Pixel>& got, const std::vector<Pixel>& in,
                                    int mode, bool hdr, const char* what)
{
    for (size_t i = 0; i < in.size(); ++i) {
        Pixel want = ReferenceApply(in[i], mode, hdr);
        for (int c = 0; c < 3; ++c)
            if (!(std::abs(got[i][c] - want[c]) <= kRefTol)) {
                fprintf(stderr, "  %s: mode %d %s sample %zu ch %d: in %.6f got %.6f want %.6f\n",
                        what, mode, hdr ? "linear" : "encoded", i, c, in[i][c], got[i][c], want[c]);
                throw std::runtime_error("GPU output differs from the reference model");
            }
    }
}

struct CvProbe {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    CvProbe() {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, &device, nullptr, &context));
        int kind = 0;
        ComPtr<ID3DBlob> blob; blob.Attach(CompileSharpenBlob(&kind, false, true));
        Require(blob.Get() != nullptr, "Production post HLSL did not compile");
        Check(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader));
    }
    // Defaults = LUT off, style off, sharpness 0: only the colour-vision stage may touch the pixels.
    // The LUT/style arguments exist for the composition test, which needs the other stages switched ON.
    std::vector<Pixel> Run(const std::vector<Pixel>& pixels, unsigned width, unsigned height, int mode, bool hdr,
                           int lutPreset = DLSS_LUT_OFF, float lutStrength = 0.0f,
                           const SceneStyleParams& style = SceneStyleParams{}) {
        Require(pixels.size() == size_t(width) * height, "Input dimensions mismatch");
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = { pixels.data(), width * sizeof(Pixel), 0 };
        ComPtr<ID3D11Texture2D> src, dst, readback;
        Check(device->CreateTexture2D(&td, &data, &src));
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        Check(device->CreateTexture2D(&td, nullptr, &dst));
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&td, nullptr, &readback));
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateShaderResourceView(src.Get(), nullptr, &srv));
        Check(device->CreateUnorderedAccessView(dst.Get(), nullptr, &uav));
        alignas(16) unsigned char constants[256];
        FillSharpenConstants(constants, DLSS_SHARPEN_RCAS, 0, width, height, lutPreset, lutStrength, hdr,
                             style, mode);
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(constants); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial = { constants, 0, 0 };
        ComPtr<ID3D11Buffer> cb;
        Check(device->CreateBuffer(&bd, &initial, &cb));
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        context->CSSetShaderResources(0, 1, srv.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        ID3D11UnorderedAccessView* empty = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
        context->CopyResource(readback.Get(), dst.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<Pixel> result(pixels.size());
        for (unsigned y = 0; y < height; ++y)
            memcpy(result.data() + y * width, static_cast<char*>(mapped.pData) + y * mapped.RowPitch, width * sizeof(Pixel));
        context->Unmap(readback.Get(), 0);
        context->ClearState();
        return result;
    }
};

int main() {
    try {
        CvProbe probe;

        // 1. The CPU matrices equal the independent Python reference, and preserve neutral white.
        Require(!ColorVisionEnabled(0) && !ColorVisionEnabled(4) && !ColorVisionEnabled(-1), "Invalid mode accepted");
        for (int mode = 1; mode <= 3; ++mode) {
            Require(ColorVisionEnabled(mode), "Valid mode rejected");
            CvMatrix d = CvCorrection(mode);
            for (int i = 0; i < 9; ++i)
                Require(std::abs(d.m[i] - kReference[mode - 1][i]) < 1e-6f, "Matrix differs from the Python reference");
            for (int r = 0; r < 3; ++r)
                Require(std::abs(d.m[r * 3 + 0] + d.m[r * 3 + 1] + d.m[r * 3 + 2] - 1.0f) < 2e-6f, "White is not preserved");
        }
        Require(CvCorrection(0).m[0] == 1.0f && CvCorrection(0).m[1] == 0.0f && CvCorrection(0).m[4] == 1.0f,
                "Mode 0 is not the identity");

        // A 17^3 cube of encoded display values plus the six saturated primaries/secondaries.
        std::vector<Pixel> cube;
        for (int b = 0; b < 17; ++b) for (int g = 0; g < 17; ++g) for (int r = 0; r < 17; ++r)
            cube.push_back({r / 16.f, g / 16.f, b / 16.f, (r + g + b) / 48.f});
        const std::vector<Pixel> primaries = {
            {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1}, {0,1,1,1}, {1,0,1,1}, {1,1,1,1}, {0,0,0,1}};

        // Both sRGB transfer breakpoints, crossed so every channel sees both sides of both knees.
        // 0.04045 is the DECODE knee; it is also exactly 12.92 * 0.0031308, so on the encoded path it
        // decodes onto the ENCODE knee - one value exercises both branches. On the linear path the same
        // numbers are fed as linear light and straddle 0.0031308 directly.
        const float kKnees[] = { 0.0f, 0.0015f, 0.0031f, 0.0031308f, 0.00314f, 0.0045f, 0.012f,
                                 0.0402f, 0.04045f, 0.0406f, 0.06f, 0.5f, 0.94f, 1.0f };
        std::vector<Pixel> knees;
        for (float a : kKnees) for (float b : kKnees) knees.push_back({a, b, 0.5f * (a + b), 1.0f});
        const unsigned kneeW = (unsigned)knees.size();

        // 2. Mode 0 is a bit-exact bypass on both paths - not "close", identical.
        for (bool hdr : {false, true}) {
            const auto pass = probe.Run(cube, 17, 289, 0, hdr);
            for (size_t i = 0; i < cube.size(); ++i)
                for (int c = 0; c < 4; ++c)
                    Require(pass[i][c] == cube[i][c], "Mode 0 is not a bit-exact bypass");
        }

        for (int mode = 1; mode <= 3; ++mode) {
            const auto encoded = probe.Run(cube, 17, 289, mode, false);
            // 3. The encoded path stays inside the display gamut and never touches alpha.
            for (size_t i = 0; i < cube.size(); ++i) {
                Require(encoded[i][3] == cube[i][3], "Alpha changed");
                for (int c = 0; c < 3; ++c)
                    Require(std::isfinite(encoded[i][c]) && encoded[i][c] >= 0.0f && encoded[i][c] <= 1.0f,
                            "Encoded output left [0,1]");
            }
            // 4. THE assertion that catches a channel-order or transposition slip: every GPU sample equals
            // the CPU model built from the Python numbers. Encoded path: full cube, both sRGB knees, and
            // the gamut corners.
            const auto edge = probe.Run(primaries, 8, 1, mode, false);
            RequireMatchesReference(encoded, cube, mode, false, "cube");
            RequireMatchesReference(probe.Run(knees, kneeW, 1, mode, false), knees, mode, false, "sRGB knees");
            RequireMatchesReference(edge, primaries, mode, false, "gamut corners");

            // 5. Encoded and FP16-linear paths agree to better than one 8-bit step, and the FP16-linear
            // path independently matches the same reference (its own branch: max(0), no encode).
            std::vector<Pixel> linear(cube.size());
            for (size_t i = 0; i < cube.size(); ++i)
                linear[i] = {SrgbToLinear(cube[i][0]), SrgbToLinear(cube[i][1]), SrgbToLinear(cube[i][2]), cube[i][3]};
            const auto fromLinear = probe.Run(linear, 17, 289, mode, true);
            RequireMatchesReference(fromLinear, linear, mode, true, "cube");
            RequireMatchesReference(probe.Run(knees, kneeW, 1, mode, true), knees, mode, true, "sRGB knees");
            RequireMatchesReference(probe.Run(primaries, 8, 1, mode, true), primaries, mode, true, "gamut corners");
            for (size_t i = 0; i < cube.size(); ++i)
                for (int c = 0; c < 3; ++c) {
                    float reencoded = LinearToSrgb(fromLinear[i][c] < 0 ? 0 : fromLinear[i][c] > 1 ? 1 : fromLinear[i][c]);
                    Require(std::abs(reencoded - encoded[i][c]) <= 1.0f / 255.0f, "Encoded/linear parity exceeds 1/255");
                }
            // 6. Saturated primaries stay in gamut, white and black are fixed points.
            for (size_t i = 0; i < primaries.size(); ++i)
                for (int c = 0; c < 3; ++c)
                    Require(edge[i][c] >= 0.0f && edge[i][c] <= 1.0f, "Saturated primary left [0,1]");
            for (int c = 0; c < 3; ++c) {
                Require(std::abs(edge[6][c] - 1.0f) < 2e-3f, "White moved");
                Require(std::abs(edge[7][c]) < 2e-3f, "Black moved");
            }
            // 7. The stage is not a no-op: at least one cube colour actually moves.
            bool moved = false;
            for (size_t i = 0; i < cube.size() && !moved; ++i)
                for (int c = 0; c < 3; ++c)
                    if (std::abs(encoded[i][c] - cube[i][c]) > 1.0f / 255.0f) { moved = true; break; }
            Require(moved, "Colour-vision stage changed nothing");

            // 8. Composition: the correction is applied TO what the LUT and the style produced, not instead
            // of them. The cv=0 run of the same shader is the input to the CPU model, so this holds for any
            // preset/style without duplicating their maths here. Cartoon and PixelArt both, because
            // PixelArt samples a block neighbourhood and Cartoon does not.
            // BOTH colour-space paths (encoded hdr=false AND FP16-linear hdr=true): ColorVision() picks its
            // decode/encode branch off `styleLinear`, which is the SAME uniform the LUT and the style stages
            // branch on, so a composition bug that only bites when the shader runs linear (D3D12 half-colour,
            // the shipping D3D12 path) is invisible to an hdr=false-only test. `linear` is the sRGB-decoded
            // cube built for check 5 above, so hdr=true is fed linear light, exactly as the runtime feeds it.
            for (unsigned styleMode : { 1u, 2u }) {
                SceneStyleParams style = {}; style.mode = styleMode; style.strength = 0.8f; style.pixelSize = 4;
                for (bool hdr : {false, true}) {
                    const std::vector<Pixel>& input = hdr ? linear : cube;
                    const auto graded   = probe.Run(input, 17, 289, RF_CV_NONE, hdr, DLSS_LUT_VIVID, 0.85f, style);
                    const auto composed = probe.Run(input, 17, 289, mode,       hdr, DLSS_LUT_VIVID, 0.85f, style);
                    RequireMatchesReference(composed, graded, mode, hdr, "LUT+style composition");
                    bool differs = false;
                    for (size_t i = 0; i < graded.size() && !differs; ++i)
                        for (int c = 0; c < 3; ++c)
                            if (std::abs(composed[i][c] - graded[i][c]) > 1.0f / 255.0f) { differs = true; break; }
                    Require(differs, "Correction vanished once a LUT and a style were active");
                }
            }
        }

        printf("PASS: production HLSL on D3D11 WARP; 3 modes; matrices match colour_vision_ref.py; "
               "every GPU sample matches the reference model on BOTH colour-space paths (4913-colour cube, "
               "196 sRGB-knee samples, 8 gamut corners) and composes with LUT Vivid + Cartoon/PixelArt on BOTH paths; "
               "bit-exact mode-0 bypass on both paths; cube stays in gamut; encoded/linear parity <= 1/255; "
               "white and black are fixed points.\n");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
```

- [ ] Register it in `native\CMakeLists.txt` after the `scene_style_probe` block (currently ends at `:109`):

```cmake
add_executable(colour_vision_probe EXCLUDE_FROM_ALL probe/colour_vision_probe.cpp Sharpen.cpp)
target_include_directories(colour_vision_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(colour_vision_probe PRIVATE d3d11 d3dcompiler)
target_compile_definitions(colour_vision_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
```

- [ ] Add `ColorVision.h` to the `add_library(RenderforgeNative SHARED ...)` list in `native\CMakeLists.txt`, on the line after `Sharpen.h` (`:72`). The file exists by now (scaffolding 1/3), so CMake configures instead of aborting — that is the whole point of the ordering.
- [ ] **Red run.** The probe must BUILD and then FAIL an assertion:

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build E:\DEV\PhoenixPoint\Renderforge\build\native --config Release --target colour_vision_probe -- /verbosity:minimal
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\colour_vision_probe.exe
```

Expected: **0 errors** from cmake, then

```
FAIL: Matrix differs from the Python reference
```

with exit code 1 (`$LASTEXITCODE` = 1). A build error here means the scaffolding is incomplete — fix that, do not proceed. A `PASS` here means the stage already exists somewhere; stop and re-read the tree.

- [ ] Also re-run `lut_probe` and `scene_style_probe` now, before touching anything else — they share
`FillSharpenConstants`, and this is the cheapest moment to catch a broken signature change:

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build E:\DEV\PhoenixPoint\Renderforge\build\native --config Release --target lut_probe scene_style_probe -- /verbosity:minimal
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\lut_probe.exe
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\scene_style_probe.exe
```

Expected: both still print their existing `PASS:` line, exit 0.

### 2b — the matrices (half the red goes green)

- [ ] Replace the whole of `native\ColorVision.h` — the stub body and its `TODO` disappear:

```cpp
// ColorVision.h - daltonization matrices for the analytic post pass. Mode ordinals cross the managed/native
// ABI (DLSS_CV_* in RenderforgeNative.h, ColorVisionMode in src\DlssConfig.cs); append only.
//
// D = I + R * (I - S), applied to COLUMN-VECTOR linear RGB (v' = D * v), stored ROW-MAJOR.
//
// S = Machado, Oliveira & Fernandes 2009 simulation matrices at severity 1.0, transcribed from the authors'
//     own table: https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html
// R = error redistribution: how the colour a deficient eye cannot separate is pushed onto channels it can.
//     protanopia: Fidaner, Lin & Ozguven 2005, "Analysis of Color Blindness" - `err2mod` verbatim in their
//         MATLAB (https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m), which applies it to
//         the protan error (`errorp`) only; report at
//         http://acorn.stanford.edu/psych221/projects/2005/ofidaner/colorblindness_project.htm.
//     deuteranopia: the SAME matrix, and that is a deliberate, sourced choice - the MATLAB does not correct
//         the deutan error at all, but the maintained Python port of the same method applies the single
//         err2mod to every deficiency type (daltonize/daltonize.py:125 in
//         https://github.com/joergdietrich/daltonize; the type argument only picks the simulate() matrix,
//         :120). The alternative was ixora's distinct deutan R (1 0.7 0 / 0 0 0 / 0 0.7 1); it was rejected
//         because it rests on the same unverified secondary table as the tritan row below.
//     tritanopia: the matrix table at
//         https://ixora.io/projects/colorblindness/color-blindness-simulation-research.html, which
//         redistributes onto the channels a tritan can still discriminate. SECONDARY source, coefficients
//         NOT independently verified - the page cites Simon-Liedtke & Farup (JVCI 2016) as justification
//         for using a per-type matrix, it is not the published origin of these numbers. Weaker than the
//         citations above; used because err2mod would push the tritan error onto B, the very channel a
//         tritan cannot discriminate.
//
// Every row of every D sums to 1 within 1e-6 (residual = the rounding in the published S), so neutral
// grey and white are fixed points. probe/colour_vision_probe.cpp asserts that, and asserts D against
// probe/colour_vision_ref.py, which derives the same numbers from the same publications independently.
#pragma once

// Mirrored by DLSS_CV_* (RenderforgeNative.h) and ColorVisionMode (src\DlssConfig.cs).
enum { RF_CV_NONE = 0, RF_CV_DEUTERANOPIA = 1, RF_CV_PROTANOPIA = 2, RF_CV_TRITANOPIA = 3 };

struct CvMatrix { float m[9]; };   // row-major: m[row * 3 + col]

namespace CvDetail {

struct M3 { float m[9]; };

constexpr M3 kIdentity = { { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f } };

constexpr M3 kSimulate[3] = {
    { {  0.367322f,  0.860646f, -0.227968f,
         0.280085f,  0.672501f,  0.047413f,
        -0.011820f,  0.042940f,  0.968881f } },   // deuteranopia
    { {  0.152286f,  1.052583f, -0.204868f,
         0.114503f,  0.786281f,  0.099216f,
        -0.003882f, -0.048116f,  1.051998f } },   // protanopia
    { {  1.255528f, -0.076749f, -0.178779f,
        -0.078411f,  0.930809f,  0.147602f,
         0.004733f,  0.691367f,  0.303900f } },   // tritanopia
};

constexpr M3 kRedistribute[3] = {
    { { 0.0f, 0.0f, 0.0f,  0.7f, 1.0f, 0.0f,  0.7f, 0.0f, 1.0f } },   // deuteranopia (err2mod, daltonize.py:125)
    { { 0.0f, 0.0f, 0.0f,  0.7f, 1.0f, 0.0f,  0.7f, 0.0f, 1.0f } },   // protanopia   (err2mod, Fidaner conv_img.m)
    { { 1.0f, 0.0f, 0.7f,  0.0f, 1.0f, 0.7f,  0.0f, 0.0f, 0.0f } },   // tritanopia   (ixora.io, secondary)
};

constexpr float Dot(const M3& a, const M3& b, int r, int c)
{
    return a.m[r * 3 + 0] * b.m[0 * 3 + c] + a.m[r * 3 + 1] * b.m[1 * 3 + c] + a.m[r * 3 + 2] * b.m[2 * 3 + c];
}

}   // namespace CvDetail

// Correction matrix for `mode`. Anything outside 1..3 returns the identity, so a bad ordinal from the
// managed side is a visual no-op instead of garbage.
constexpr CvMatrix CvCorrection(int mode)
{
    CvMatrix d = { { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f } };
    if (mode < RF_CV_DEUTERANOPIA || mode > RF_CV_TRITANOPIA) return d;
    const CvDetail::M3& s = CvDetail::kSimulate[mode - 1];
    const CvDetail::M3& r = CvDetail::kRedistribute[mode - 1];
    CvDetail::M3 error = { {} };
    for (int i = 0; i < 9; ++i) error.m[i] = CvDetail::kIdentity.m[i] - s.m[i];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            d.m[row * 3 + col] = CvDetail::kIdentity.m[row * 3 + col] + CvDetail::Dot(r, error, row, col);
    return d;
}
```

- [ ] **Second red run** — the CPU half is now green and the GPU half is still bypassed, so the failure
must have MOVED to the reference comparison:

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build E:\DEV\PhoenixPoint\Renderforge\build\native --config Release --target colour_vision_probe -- /verbosity:minimal
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\colour_vision_probe.exe
```

Expected: exit 1, a `cube: mode 1 encoded sample … in … got … want …` line on stderr where `got` equals
`in` (the shader is still a passthrough), then

```
FAIL: GPU output differs from the reference model
```

If it still says `Matrix differs from the Python reference`, 2b is wrong — fix that first. If it PASSES,
the constants or the shader are not what this plan thinks they are; stop and re-read `Sharpen.cpp`.

### 2c — the HLSL stage

- [ ] In `native\Sharpen.cpp`, replace the cbuffer line of `kColorGradeHlsl` (`:47`) with:

```cpp
"cbuffer C : register(b0) { float sharpness; float strength; uint W; uint H; uint preset; float con; uint styleMode; uint pixelSize; float styleStrength; uint styleLinear; uint colorVision; float pad0; float4 cvRow0; float4 cvRow1; float4 cvRow2; };\n"
```

Packing note (why this is safe): the first ten scalars fill bytes 0..39; `colorVision` takes 40..43 and `pad0` 44..47, completing the third 16-byte register. The three `float4` rows then start at 48, 64 and 80 — all 16-byte aligned, so no member straddles a register boundary. Total 96 of the 256 bytes the block already reserves; nothing else moves.

- [ ] In `native\Sharpen.cpp`, insert the stage between `Grade()` and `main` (after `:64`, the closing `"}\n"` of `Grade`):

```cpp
// Daltonization, AFTER Grade() and Stylize() so it corrects whatever the player actually sees. The matrix is
// precomputed on the CPU (ColorVision.h) and arrives row-major in cvRow0..2; the shader only transforms.
// The exact piecewise sRGB curve is used, not pow(2.2): the error of the approximation is ~2/255 in the
// darks, which is the same order as the correction itself on near-neutral colours.
"float3 CvSrgbToLinear(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }\n"
"float3 CvLinearToSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }\n"
"float3 ColorVision(float3 c) {\n"
"  if (colorVision == 0) return c;\n"                      // uniform branch: mode 0 is a bit-exact bypass
"  float3 v = styleLinear != 0 ? max(c, 0.0) : CvSrgbToLinear(c);\n"
"  float3 d = float3(dot(cvRow0.xyz, v), dot(cvRow1.xyz, v), dot(cvRow2.xyz, v));\n"
// FP16 linear output keeps overbrights (only negatives are clipped, as everywhere else in this shader);
// the UNORM path clamps in LINEAR light before encoding, so the encode never sees an out-of-range value.
"  return styleLinear != 0 ? max(d, 0.0) : CvLinearToSrgb(d);\n"
"}\n"
```

- [ ] In `native\Sharpen.cpp`, change the store line of `main` (`:74`) to wrap the existing chain:

```cpp
"  dst[id.xy]=float4(ColorVision(Grade(Stylize(p,c))),src.Load(int3(p,0)).a); }\n";
```

### 2d — constant packing

- [ ] In `native\Sharpen.cpp`, replace `FillSharpenConstants` (`:132-151`, already widened in 2a) with:

```cpp
void FillSharpenConstants(void* dst256, int kind, float sharpness, unsigned w, unsigned h,
                          int lutPreset, float lutStrength, bool hdr, const SceneStyleParams& style,
                          int colorVision)
{
    memset(dst256, 0, 256);
    if (PostShaderEnabled(lutPreset, lutStrength, style, colorVision)) {
        float* fp = (float*)dst256; unsigned* up = (unsigned*)dst256;
        fp[0] = sharpness; fp[1] = lutStrength; up[2] = w; up[3] = h; up[4] = (unsigned)lutPreset;
        fp[5] = exp2f(-2.0f * (1.0f - sharpness));
        up[6] = style.mode; up[7] = style.pixelSize;
        fp[8] = style.strength; up[9] = hdr ? 1u : 0u;
        up[10] = ColorVisionEnabled(colorVision) ? (unsigned)colorVision : 0u;
        // fp[11] is the cbuffer's pad0. The three float4 rows start at float index 12 (byte 48); their w
        // components stay zero from the memset above, which is what the shader's .xyz swizzle expects.
        CvMatrix d = CvCorrection(colorVision);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                fp[12 + row * 4 + col] = d.m[row * 3 + col];
    } else if (kind == DLSS_SHARPEN_NIS) {
        NISConfig cfg = {};
        NVSharpenUpdateConfig(cfg, sharpness, 0, 0, w, h, w, h, 0, 0, hdr ? NISHDRMode::Linear : NISHDRMode::None);
        memcpy(dst256, &cfg, sizeof(cfg));
    } else {
        // RCAS mapping = the FSR1 sample's: attenuation stops = 2*(1-s), con = exp2(-stops), so s=1 -> con=1.
        float* fp = (float*)dst256; unsigned* up = (unsigned*)dst256;
        fp[0] = exp2f(-2.0f * (1.0f - sharpness)); fp[1] = 0.0f; up[2] = w; up[3] = h;
    }
}
```

- [ ] Confirm the pass predicate at `:136` still reads `if (PostShaderEnabled(lutPreset, lutStrength, style, colorVision)) {` — it was changed in 2a and this replacement must not have reverted it.

### 2e — green

- [ ] Build and run:

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build E:\DEV\PhoenixPoint\Renderforge\build\native --config Release --target lut_probe scene_style_probe colour_vision_probe -- /verbosity:minimal
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\colour_vision_probe.exe
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\lut_probe.exe
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\scene_style_probe.exe
```

Expected: the cmake build reports 0 errors, then

```
PASS: production HLSL on D3D11 WARP; 3 modes; matrices match colour_vision_ref.py; every GPU sample matches the reference model on BOTH colour-space paths (4913-colour cube, 196 sRGB-knee samples, 8 gamut corners) and composes with LUT Vivid + Cartoon/PixelArt on BOTH paths; bit-exact mode-0 bypass on both paths; cube stays in gamut; encoded/linear parity <= 1/255; white and black are fixed points.
PASS: production HLSL on D3D11 WARP; 9 presets; 4913-color cube; alpha, finite range, blend endpoints, B&W equality, 1025-step monotonic ramps, FP overbrights.
```

plus `scene_style_probe`'s own PASS line unchanged (the LUT and style probes must be re-run: they call the same shader and the same `FillSharpenConstants`, so they are the regression gate on the cbuffer change).

- [ ] Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add native/ColorVision.h native/Sharpen.h native/Sharpen.cpp native/CMakeLists.txt native/probe/colour_vision_probe.cpp
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(native): add colour-vision daltonization stage to the post shader"
```

---

## Task 3 — Native export and every pass predicate

**Files:** `native\RenderforgeNative.h`, `native\RenderforgeNative.cpp`, `native\Device.h`, `native\Device11.cpp`, `native\D3D12Sharpen.h`, `native\Device12.cpp`, `native\Fsr12.cpp`, `native\Xess12.cpp`

- [ ] `native\Device.h` — add the field to `FrameParams`, after `SceneStyleParams style;` (`:33`):

```cpp
    int colorVision;          // DLSS_CV_*; cleared by SetFrame, filled by SetColorVision before the event is queued
```

- [ ] `native\RenderforgeNative.h` — after the `DLSS_LUT_*` enum (`:51`), add:

```c
// Colour-vision correction (daltonization) applied after the grade and the scene style. Ordinals cross the ABI.
enum { DLSS_CV_OFF = 0, DLSS_CV_DEUTERANOPIA = 1, DLSS_CV_PROTANOPIA = 2, DLSS_CV_TRITANOPIA = 3 };
```

- [ ] `native\RenderforgeNative.h` — after the `Dlss_SetSceneStyle` declaration (`:75`), add:

```c
// Main thread, after SetFrame (which clears the slot) and before queuing its event - same contract as
// Dlss_SetSceneStyle. mode = DLSS_CV_*; out-of-range values disable the stage. Unlike the LUT and the style,
// this stage alone is enough to make the post pass run: LUT Off + style Off + sharpness 0 still corrects.
DLSS_API void __cdecl Dlss_SetColorVision(void* slot, int mode);
```

- [ ] `native\RenderforgeNative.cpp` — after `Dlss_SetSceneStyle` (`:214`), add:

```cpp
void __cdecl Dlss_SetColorVision(void* slot, int mode)
{
    FrameParams* p = (FrameParams*)slot;
    if (!p) return;
    p->colorVision = (mode >= DLSS_CV_DEUTERANOPIA && mode <= DLSS_CV_TRITANOPIA) ? mode : DLSS_CV_OFF;
}
```

- [ ] `native\Device11.cpp:87-92` — thread the mode into the D3D11 pass:

```cpp
    void Sharpen(ID3D11DeviceContext* ctx, ID3D11Resource* output, float sharpness,
                 int lutPreset, float lutStrength, const SceneStyleParams& style, int colorVision)
    {
        if (sharpenDead || !output || !device) return;
        bool grade = PostShaderEnabled(lutPreset, lutStrength, style, colorVision);
        if (!EnsureSharpenShader(grade)) return;
```

- [ ] `native\Device11.cpp` — find the `FillSharpenConstants(...)` call inside that same `Sharpen` body (below the view setup) and append `, colorVision` as the last argument.
- [ ] `native\Device11.cpp:254-255` — passthrough call site:

```cpp
                if (fp.sharpness > 0.0f || PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision))
                    Sharpen(ctx, output, fp.sharpness, fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
```

- [ ] `native\Device11.cpp:278-279` — DLSS call site:

```cpp
            else if (fp.sharpness > 0.0f || PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision))
                Sharpen(ctx, output, fp.sharpness, fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
```

- [ ] `native\D3D12Sharpen.h:206-207` — `Run` signature gains the mode:

```cpp
    void Run(ID3D12GraphicsCommandList* cl, ID3D12Resource* output, float sharpness,
             int lutPreset, float lutStrength, int slot, const SceneStyleParams& style, int colorVision)
```

- [ ] `native\D3D12Sharpen.h:217-218` — pass it to the packer:

```cpp
        FillSharpenConstants(cbCpu + 256 * (size_t)slot, owner->sharpener, sharpness, w, h,
                             lutPreset, lutStrength, psoHdr, style, colorVision);
```

- [ ] `native\D3D12Sharpen.h:257-263` — `RunPassthrough` signature, predicate and inner call:

```cpp
    bool RunPassthrough(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* output,
                        OwnedSet12& owned, D3D12Ring& ring, bool srgbViews,
                        float sharpness, int lutPreset, float lutStrength, int slot,
                        const SceneStyleParams& style, int colorVision)
    {
        bool grade = PostShaderEnabled(lutPreset, lutStrength, style, colorVision);
```

- [ ] `native\D3D12Sharpen.h:272` — the inner `Run` call inside `RunPassthrough`:

```cpp
        Run(cl, owned.out, sharpness, lutPreset, lutStrength, slot, style, colorVision);
```

- [ ] `native\Device12.cpp:188-190`:

```cpp
            bool wantPost = fp.sharpness > 0.0f || PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
            if (!wantPost || !sharpen.RunPassthrough(cl, color, output, owned, ring, srgbViews,
                                                      fp.sharpness, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style, fp.colorVision))
```

- [ ] `native\Device12.cpp:196`:

```cpp
            bool grade = PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
```

- [ ] `native\Device12.cpp:220`:

```cpp
            else if (doPost) sharpen.Run(cl, owned.out, fp.sharpness, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style, fp.colorVision);
```

- [ ] `native\Fsr12.cpp:289-291`:

```cpp
            bool wantPost = fp.sharpness > 0.0f || PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
            if (!wantPost || !post.RunPassthrough(cl, color, output, owned, ring, srgbViews,
                                                   fp.sharpness, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style, fp.colorVision))
```

- [ ] `native\Fsr12.cpp:297` — note FSR does its own RCAS, so `doPost` (`:298`) is driven by `grade` alone; extending `grade` is exactly what makes colour vision reach the FSR path:

```cpp
            bool grade = PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
```

- [ ] `native\Fsr12.cpp:346`:

```cpp
            else if (doPost) post.Run(cl, owned.out, 0.0f, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style, fp.colorVision);
```

- [ ] `native\Xess12.cpp:324-326`:

```cpp
            bool wantPost = fp.sharpness > 0.0f || PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
            if (!wantPost || !sharpen.RunPassthrough(cl, color, output, owned, ring, srgbViews,
                                                      fp.sharpness, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style, fp.colorVision))
```

- [ ] `native\Xess12.cpp:332`:

```cpp
            bool grade = PostShaderEnabled(fp.lutPreset, fp.lutStrength, fp.style, fp.colorVision);
```

- [ ] `native\Xess12.cpp:364`:

```cpp
            else if (doPost) sharpen.Run(cl, owned.out, fp.sharpness, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style, fp.colorVision);
```

- [ ] Prove no predicate was missed. Before this task the old two-term disjunction appears at **11** sites (`Sharpen.cpp:136`, `Device11.cpp:91/254/278`, `D3D12Sharpen.h:261`, `Device12.cpp:188/196`, `Fsr12.cpp:289/297`, `Xess12.cpp:324/332`). Afterwards exactly **one** occurrence may remain — the body of `PostShaderEnabled` in `Sharpen.h`, which is the whole point of the refactor. The naive regex matches that body too, so it needs a lookahead that permits the three-term form and rejects every two-term one:

```powershell
Select-String -Path E:\DEV\PhoenixPoint\Renderforge\native\*.cpp,E:\DEV\PhoenixPoint\Renderforge\native\*.h `
  -Pattern 'ColorGradeEnabled\([^)]*\)\s*\|\|\s*SceneStyleEnabled\([^)]*\)(?!\s*\|\|\s*ColorVisionEnabled)'
```

Expected: **no output at all** (`Select-String` prints nothing when nothing matches).

- [ ] Positive control, so "no output" cannot mean "the regex is broken" — the one permitted definition must be found exactly once:

```powershell
@(Select-String -Path E:\DEV\PhoenixPoint\Renderforge\native\Sharpen.h `
  -Pattern 'ColorGradeEnabled\([^)]*\)\s*\|\|\s*SceneStyleEnabled\([^)]*\)\s*\|\|\s*ColorVisionEnabled\([^)]*\)').Count
```

Expected output: `1`.

- [ ] Full native build (this is also the FG/Streamline regression gate):

```powershell
E:\DEV\PhoenixPoint\Renderforge\build-native.ps1
```

Expected: cmake configure + build succeed, `RenderforgeNative.dll` and `dlss_probe.exe` are copied to `build\out`, and the script's own probe run passes. Then re-run the three shader probes:

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build E:\DEV\PhoenixPoint\Renderforge\build\native --config Release --target lut_probe scene_style_probe colour_vision_probe -- /verbosity:minimal
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\colour_vision_probe.exe
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\lut_probe.exe
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\scene_style_probe.exe
```

Expected: three `PASS:` lines, exit 0 each.

- [ ] Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add native
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(native): export Dlss_SetColorVision and activate the pass on colour vision alone"
```

---

## Task 4 — Managed config field, P/Invoke and driver predicate

**Files:** `src\Native.cs`, `src\DlssConfig.cs`, `src\DlssDriver.cs`

- [ ] `src\Native.cs` — after the `Dlss_SetSceneStyle` import (`:110-111`), add:

```csharp
        // Same slot contract as Dlss_SetSceneStyle: fill after Dlss_SetFrame, before the event is queued.
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetColorVision(IntPtr slot, int mode);
```

- [ ] `src\DlssConfig.cs` — after the `LutPreset` enum (`:22`), add:

```csharp
    /// <summary>Colour-vision correction (daltonization). Ordinals cross the managed/native ABI; append only.</summary>
    public enum ColorVisionMode { None, Deuteranopia, Protanopia, Tritanopia }
```

> **Coexistence with the quality-knobs plan (`docs\superpowers\plans\2026-09-05-quality-knobs.md`).**
> That plan adds `Vignette` / `ShadowResolution` / `Anisotropic` / `LodBias` to the same three places in
> `DlssConfig.cs` and its own `QualityPanel` row block to `GraphicsPanel.cs`. The two plans are
> **additive, and order-independent — whichever lands second keeps everything the first one added.**
> So: locate every edit below by its **anchor name**, never by the line number (A's inserts shift them),
> read the current text first, and add to it. If a snippet below shows a full replacement line and the
> file already carries A's names, use the "quality knobs already landed" variant.

- [ ] `src\DlssConfig.cs` — add the field name to `HiddenFromModSettings` (anchor: the initializer containing `nameof(CrispFonts)`).

If the quality-knobs plan has **not** landed, the last line becomes:

```csharp
            nameof(SceneStyle), nameof(SceneStyleStrength), nameof(PixelSize), nameof(CrispFonts),
            nameof(ColorVision)
```

If it **has** landed (its four names are already there), keep them and append ours:

```csharp
            nameof(SceneStyle), nameof(SceneStyleStrength), nameof(PixelSize), nameof(CrispFonts),
            nameof(Vignette), nameof(ShadowResolution), nameof(Anisotropic), nameof(LodBias),
            nameof(ColorVision)
```

- [ ] `src\DlssConfig.cs` — add the field immediately after the `PixelSize` field (`:51` pre-A; anchor: `public int PixelSize`). Pure insert — it never collides with A's fields, which go in after `CrispFonts`:

```csharp
        [ConfigField("Colour vision", "Off, Deuteranopia, Protanopia or Tritanopia. Redistributes colours the eye cannot separate onto channels it can. Scene only; the interface is drawn after this pass. Also in Options → Graphics.")]
        public ColorVisionMode ColorVision = ColorVisionMode.None;
```

- [ ] `src\DlssConfig.cs` — add the Russian entry to `Ru`, after the `nameof(PixelSize)` row (`:89` pre-A). Pure insert; A's four RU rows go in after the `FrameGen` row and do not conflict:

```csharp
            { nameof(ColorVision), new[] { "Цветовое зрение", "Выкл, дейтеранопия, протанопия или тританопия. Перераспределяет неразличимые цвета на различимые каналы. Только сцена: интерфейс рисуется после этого прохода. Также в Настройки → Графика." } },
```

- [ ] `src\DlssDriver.cs:197` — the pipeline must start for colour vision alone (upscaler Off, LUT Off, style Off):

```csharp
            bool needsPipeline = wantMode != RenderforgeMode.Off || lutActive || SceneStylePanel.Active(cfg)
                || ColorVisionPanel.Active(cfg);
```

- [ ] **One gate, not two.** Activation (`:197`) and submission (`:504`, below) must ask the *same*
question, or the geoscape gets a post pass that starts for colour vision and then never receives the
mode — an uncorrected passthrough pass, pure cost and a real risk of a visual delta from a pass that
should not be running at all. `lutPreset` is already tactical-gated at `:492` and `Dlss_SetColorVision`
mirrors it, so the tactical test belongs **inside** `ColorVisionPanel.Active` where both callers pick it
up and cannot drift apart. In `src\ColorVisionPanel.cs` (Task 5) `Active` therefore reads:

```csharp
        // Tactical-only, exactly like lutPreset at DlssDriver.cs:492 - the post pass exists on the tactical
        // camera only. Both the activation predicate (DlssDriver.cs:197) and the per-frame submission
        // (DlssDriver.cs:504) call this, so the two can never disagree.
        internal static bool Active(DlssConfig cfg) => RenderforgeMod.TacticalActive && cfg != null
            && cfg.ColorVision >= ColorVisionMode.Deuteranopia && cfg.ColorVision <= ColorVisionMode.Tritanopia;
```

Known and deliberately untouched asymmetry: `SceneStylePanel.Active(cfg)` carries no tactical gate. That
is existing behaviour of a shipped feature; do not "fix" it inside this plan.

- [ ] `src\DlssDriver.cs:504-506` — send the mode on the same slot, right after the scene-style send. The tactical gate lives inside `ColorVisionPanel.Active`, so this call site and `:197` are literally the same predicate — do **not** re-spell it here:

```csharp
                if (SceneStylePanel.Active(cfg))
                    Native.Dlss_SetSceneStyle(slot, (int)cfg.SceneStyle,
                        Mathf.Clamp01(cfg.SceneStyleStrength / 100f), Mathf.Clamp(cfg.PixelSize, 2, 16));
                if (ColorVisionPanel.Active(cfg))
                    Native.Dlss_SetColorVision(slot, (int)cfg.ColorVision);
```

- [ ] This task does not build on its own — `ColorVisionPanel` arrives in Task 5. Do not run `dotnet build` here; go straight to Task 5 and commit both together at the end of it.

---

## Task 5 — UI row, labels and the console setter

**Files:** `src\ColorVisionPanel.cs` (new), `src\GraphicsPanel.cs`, `src\Pickers.cs`, `src\RenderforgeMod.cs`

- [ ] Create `src\ColorVisionPanel.cs` — the `LutPanel` picker recipe with the strength slider removed (the spec fixes the correction at full strength; there is no v1 slider):

```csharp
using System;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Live colour-vision (daltonization) picker in the game's native Graphics panel.</summary>
    internal static class ColorVisionPanel
    {
        private const string PickerName = "RenderforgeColorVision";
        private static ArrowPickerController picker;
        private static bool loggedError;

        private static string[] Labels => new[] {
            DlssConfig.Loc("None", "Нет"),
            DlssConfig.Loc("Deuteranopia", "Дейтеранопия"),
            DlssConfig.Loc("Protanopia", "Протанопия"),
            DlssConfig.Loc("Tritanopia", "Тританопия") };

        /// <summary>The ONE activation gate. Both DlssDriver.cs:197 (start the pipeline) and :504 (send the
        /// mode) call this, so they cannot drift apart and leave an uncorrected passthrough pass running on
        /// the geoscape. Tactical-only, mirroring lutPreset at DlssDriver.cs:492 — the post pass exists on
        /// the tactical camera only. Note the UI row itself does not use this: the picker stays visible and
        /// settable everywhere.</summary>
        internal static bool Active(DlssConfig cfg) => RenderforgeMod.TacticalActive && cfg != null
            && cfg.ColorVision >= ColorVisionMode.Deuteranopia && cfg.ColorVision <= ColorVisionMode.Tritanopia;

        internal static Transform Build(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            var src = panel.TextureQualityPicker;
            if (src == null || after == null) return after;
            var content = after.parent;
            var found = content.Find(PickerName);
            if (found != null) picker = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(src.gameObject, content);
                go.name = PickerName;
                picker = go.GetComponent<ArrowPickerController>();
                GraphicsPanel.SetRaw(picker.Title, null, DlssConfig.Loc("Colour vision", "Цветовое зрение").ToUpperInvariant());
            }
            picker.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
            picker.gameObject.SetActive(true);
            picker.Init(Labels.Length, Mathf.Clamp((int)cfg.ColorVision, 0, Labels.Length - 1), OnMode);
            Sync();
            return picker.transform;
        }

        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null || picker == null) return;
            int index = Mathf.Clamp((int)cfg.ColorVision, 0, Labels.Length - 1);
            GraphicsPanel.SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[index]);
            GraphicsPanel.Grey(picker.CurrentItem.gameObject, false);
            GraphicsPanel.Tip(picker.CentralButton.gameObject, DlssConfig.Loc(
                "Redistributes the colours the eye cannot separate onto the channels it can, at full strength. Applies to the scene only; the interface is drawn after this pass and is not corrected.",
                "Перераспределяет неразличимые глазом цвета на различимые каналы, в полную силу. Действует только на сцену: интерфейс рисуется после этого прохода и не корректируется."));
        }

        internal static void Hide(Transform content)
        {
            if (content == null) return;
            var row = content.Find(PickerName);
            if (row != null) row.gameObject.SetActive(false);
        }

        internal static void Clear() { picker = null; }

        private static void OnMode(int index)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                cfg.ColorVision = (ColorVisionMode)Mathf.Clamp(index, 0, Labels.Length - 1);
                RenderforgeMod.SaveConfig();
                // The driver polls this every frame, like the scene style; no feature teardown is needed.
                Sync();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge colour vision: " + ex);
                loggedError = true;
            }
        }
    }
}
```

> **Row order, stated once for both plans.** The combined Options → Graphics order is
> **LUT → Colour vision → Scene style → Quality knobs** (colour vision "next to the LUT row" per spec §B;
> the quality knobs stay last, as their own plan places them). Whichever of the two plans lands second
> **keeps the other's rows** — read `GraphicsPanel.cs` before editing and add a line, never replace the
> block wholesale. `QualityPanel.*` exists only after `2026-09-05-quality-knobs.md` has landed; omit
> those lines if it has not, and add them without touching ours if it lands later.

- [ ] `src\GraphicsPanel.cs:43` — hide it with the others when the rows are turned off (anchor: the `LutPanel.Hide` line inside the `ShowInGraphicsOptions == false` branch). Insert exactly one line; the `QualityPanel.Hide` line is A's and is present only if A landed:

```csharp
                    LutPanel.Hide(src.transform.parent);
                    ColorVisionPanel.Hide(src.transform.parent);          // <- this plan adds only this line
                    SceneStylePanel.Hide(src.transform.parent);
                    QualityPanel.Hide(src.transform.parent);              // quality-knobs plan; omit if not landed
```

- [ ] `src\GraphicsPanel.cs:61-62` — build it directly after the LUT rows. `SceneStylePanel.Build` returns the last row it made (`src\SceneStylePanel.cs:30`), so chaining `after =` through it is safe whether or not anything follows:

```csharp
                after = LutPanel.Build(__instance, sharp != null ? sharp.transform.parent : picker.transform, mod.Cfg);
                after = ColorVisionPanel.Build(__instance, after, mod.Cfg);   // <- this plan adds only this line
                after = SceneStylePanel.Build(__instance, after, mod.Cfg);
                QualityPanel.Build(__instance, after, mod.Cfg);               // quality-knobs plan; omit if not landed
```

If the quality-knobs plan has not landed, the last line is dropped and the `SceneStylePanel` line may keep
its original `SceneStylePanel.Build(__instance, after, mod.Cfg);` form — but writing `after =` now costs
nothing and is what A's Task 5 step 2 expects to find.

- [ ] `src\Pickers.cs:89-90` — drop the cached controller with the others. One line; leave any `QualityPanel.Clear()` that is already there:

```csharp
            LutPanel.Clear();
            ColorVisionPanel.Clear();
            SceneStylePanel.Clear();
```

- [ ] `src\RenderforgeMod.cs` — after `SetLut` (`:338`), add the PPCLI setter, same shape as `SetLut`/`SetSceneStyle`. No `AttachAndApply`: the driver reads the value every frame and no temporal feature is recreated.

```csharp
        /// <summary>PPCLI/live A-B surface: mode = None/Deuteranopia/Protanopia/Tritanopia.</summary>
        public static string SetColorVision(string mode)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            ColorVisionMode value;
            if (!Enum.TryParse(mode, true, out value) || !Enum.IsDefined(typeof(ColorVisionMode), value))
                return "bad colour vision mode '" + mode + "'";
            m.Cfg.ColorVision = value;
            SaveConfig();
            ColorVisionPanel.Sync();
            return "colorVision=" + value;
        }
```

- [ ] Build the managed side:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release '/p:PPRoot=D:\PP-Instance3' -v:q
```

Expected: `Build succeeded.` with **0 Warning(s), 0 Error(s)**.

- [ ] Commit Tasks 4 and 5 together (the managed side does not compile split across them):

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat: add colour vision setting, Graphics row and console setter"
```

---

## Task 6 — Deploy to Instance3 and prove activation live

Instance3 only. Never `D:\PP-Instance2`, never `D:\Steam\steamapps\common\Phoenix Point`.

- [ ] Deploy:

```powershell
E:\DEV\PhoenixPoint\Renderforge\deploy.ps1 -PPRoot 'D:\PP-Instance3'
```

Expected: no `REFUSED` (close the Instance3 game first if it is running), `dotnet build` succeeds, the DLLs land in `D:\PP-Instance3\Mods\Renderforge`, and `RenderforgeNative.dll` is staged into `D:\PP-Instance3\PhoenixPointWin64_Data\Plugins\x86_64`.

- [ ] Update PPCLI first — the `-Window` capture below only exists from commit `f5878b7`:

```powershell
git -C E:\DEV\PhoenixPoint\PPCLI pull
```

- [ ] Launch Instance3 into a tactical mission and wait until PPCLI actually answers before sending anything else. Every command in this task carries `-PPRoot 'D:\PP-Instance3'` so it can never reach Instance2 or the user's Steam install:

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state -PPRoot 'D:\PP-Instance3'
```

### 6a — read the ACTUAL renderer and FP16 state (never infer it from launch flags)

A launch flag is a request, not a result: `-force-d3d12` can be ignored, and the FP16-linear branch
additionally depends on a runtime knob. Read what the mod itself reports, once per launch, and record it
beside every number this task produces.

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"GetStatus","args":[]}' -PPRoot 'D:\PP-Instance3'
```

The returned string (`src\RenderforgeMod.cs:486`, which embeds `DlssDriver.Status`, `src\DlssDriver.cs:147-159`) carries everything needed:

| Read this token | Meaning | Source of truth |
|---|---|---|
| `api=` | the live graphics API — `D3D11` / `D3D12`. This is the renderer, not the flag. | `Native.Api()` via `DlssDriver.Status` |
| `d3d12HalfColor=` | the FP16 **request** (`Diagnostics.D3D12HalfColor`, default on) — a knob, NOT the allocation | `RenderforgeMod.cs:486` |
| `provider=` | which upscaler is actually running | `Upscalers.Running` |
| `passthrough=` / `mode=` / `render=`/`out=` | whether an upscaler is really scaling | `DlssDriver.Status` |
| `lastError=` | must stay `0` (in particular never `-3`, `DLSS_ERR_SHARPEN`) | `Native.Dlss_LastError()` |
| `sharpen=` | which post shader kind is compiled | `Native.SharpenerName(...)` |
| `fg=` | frame-gen state: `live`/`off` + the whole native `Fg_Status()` line | `FrameGen.Status()`, `RenderforgeMod.cs:490` |

**`d3d12HalfColor=` is the requested configuration, not the live allocation.** It is
`Diagnostics.D3D12HalfColor` printed straight back (`RenderforgeMod.cs:486`); the generation only *acts* on
it through `WantHalfColor => graphicsDeviceType == Direct3D12 && Diagnostics.D3D12HalfColor`
(`src\DlssDriver.cs:550`), latches the result into the private `liveHalfColor` (`:41`), and the colour RT is
allocated from *that* — `ARGBHalf` linear when it is set, `ARGB32` (or the sRGB descriptor path) when it is
not (`:303`, `:311-314`, `:329`). A knob flipped after the generation was built, or a generation created on
D3D11 and never rebuilt, leaves the two disagreeing. **`DlssDriver.Status` does not print `liveHalfColor` at
all**, so `GetStatus` alone can never prove which branch the shader took.

- [ ] Read the LIVE allocation off the render targets themselves. `Instance` is a public static property
(`src\DlssDriver.cs:13`); `colorRT` / `outRT` / `liveHalfColor` are private (`:19`, `:41`) and PPCLI's
reflection reaches private members (`PPCLI\docs\REFERENCE.md:505`). Each `get` on a non-scalar returns a
handle — feed it to the next call as `target`:

```powershell
# 1. the driver instance -> <DRV>
.\ppcli.ps1 connect call '{"op":"get","type":"Renderforge.DlssDriver","assembly":"Renderforge","member":"Instance"}' -PPRoot 'D:\PP-Instance3'
# 2. what the generation actually latched (bool, inline - no handle)
.\ppcli.ps1 connect call '{"op":"get","target":"<DRV>","member":"liveHalfColor"}' -PPRoot 'D:\PP-Instance3'
# 3. the colour RT -> <CRT>, then its REAL format
.\ppcli.ps1 connect call '{"op":"get","target":"<DRV>","member":"colorRT"}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"get","target":"<CRT>","member":"graphicsFormat"}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"get","target":"<CRT>","member":"format"}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"get","target":"<CRT>","member":"sRGB"}' -PPRoot 'D:\PP-Instance3'
# 4. the same three on outRT -> <ORT>
.\ppcli.ps1 connect call '{"op":"get","target":"<DRV>","member":"outRT"}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"get","target":"<ORT>","member":"graphicsFormat"}' -PPRoot 'D:\PP-Instance3'
```

**The FP16-linear branch (`styleLinear != 0`) is live iff the colour RT is really FP16**, i.e.
`liveHalfColor` = `True` **and** `colorRT.format` = `ARGBHalf` (`graphicsFormat` = `R16G16B16A16_SFloat`,
`sRGB` = `False`) **and** `outRT.format` = `ARGBHalf`. Anything else — `ARGB32` /
`R8G8B8A8_SRGB` / `R8G8B8A8_UNorm` — means the encoded branch ran and case 4 tested nothing new,
whatever `d3d12HalfColor=` said. Cross-check against the generation log line, which prints the same
three values at creation time (`src\DlssDriver.cs:363`: `colorRT=… sRGB=… colorDesc=… halfColor=… outRT=…`).

If a D3D12 pass reports `liveHalfColor=False`, turn the knob back on **and force a generation rebuild**
(the RTs are allocated once per generation, so flipping the knob alone changes nothing until the driver
re-creates them), then re-read steps 2–4 before capturing:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetD3D12HalfColor","args":[true]}' -PPRoot 'D:\PP-Instance3'
```

- [ ] **Frame generation: `lastError=0` proves nothing.** `Dlss_LastError` is the upscaler's error slot; it
stays `0` when FG never started, when the provider was rejected, and when the chain detached mid-run. The
FG state is the `fg=` token, which is `FrameGen.Status()` = `live `/`off ` + the native `Fg_Status()` line
(`src\FrameGen.cs:176`, `RenderforgeMod.cs:490`). That line is formatted at `native\FgHost.cpp:601-608`:

```
provider=<name|detached|-> enabled=<0|1> multiplier=<n> chain=<-|child> child=<ptr> childHr=0x… hit=<n>
focus=<none|game|child|other> fg=<0|1> shadow=<ptr> out=<w>x<h> flags=0x… caps=0x… lastError=<n>
presentHr=0x… presented=<n> fps=<n> frameId=<n> idle=<n>[ reason=<text>]
```

**`presented` going up proves nothing on its own.** `presented=` is `FgPresentCount()` = `g_total`
(`native\FgHook.cpp:536`), and `g_total` is bumped once per *present of any kind* by `CountPresent`
(`FgHook.cpp:180-190`). While FG is live the hook stops counting Unity's own present
(`FgHook.cpp:283,297`: `if (fg) FgHostAfterUnityPresent(hr); else CountPresent();`) and the host counts
`1 + generated` instead (`FgHost.cpp:536`), with the provider adding its interpolated frames through
`FgPresentedAdd` (`FgStreamline.cpp:455`, `FgXess.cpp:243`, `FgFsr.cpp:190`). So with **zero** generated
frames `presented` still advances 1 per rendered frame, and `fps=` (`FgPresentedFps`, the same counter over
a 0.5 s window) with it. There is no `generated=`/`interpolated=` token in `Fg_Status()` at all — the
providers only log their `generated` totals.

**The generated-frame count is therefore a difference, and must be measured as one:** over the same window,
`Δpresented − ΔTime.frameCount`. `Time.frameCount` advances once per rendered frame = the one real present
the host counts, so the remainder is exactly what the provider interpolated. Take **two readbacks ~2 s
apart**, each reading both numbers back to back:

```powershell
# t0
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"GetStatus","args":[]}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"get","type":"UnityEngine.Time","member":"frameCount"}'                      -PPRoot 'D:\PP-Instance3'
Start-Sleep -Seconds 2
# t1 — the same two calls again, verbatim
```

With the 6b helpers loaded this is one helper (`Rf-Call` returns the value, see 6b):

```powershell
# Each status read is BRACKETED by a frameCount read before and after: the two PPCLI round-trips are
# not simultaneous, so a single frame sample per readback lets a slow status call inflate or deflate
# dGen. The conservative generated count uses the WIDEST frame window that can contain both status
# reads: frames from FrameBefore of the first sample to FrameAfter of the second.
function Rf-FgCounters {
    $fb = Rf-Frame
    $s  = Rf-Call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"GetStatus","args":[]}'
    $fa = Rf-Frame
    if ("$s" -notmatch 'presented=(\d+)') { throw "no presented= in status: $s" }
    [pscustomobject]@{ Presented = [long]$Matches[1]; FrameBefore = $fb; FrameAfter = $fa; Status = "$s" }
}
function Rf-FgDelta($a, $b) {
    $dPres  = $b.Presented - $a.Presented
    $dFrame = $b.FrameAfter - $a.FrameBefore          # widest window -> conservative (lowest) dGen
    [pscustomobject]@{ dPres = $dPres; dFrame = $dFrame; dGen = $dPres - $dFrame }
}
$a = Rf-FgCounters; Start-Sleep -Seconds 2; $b = Rf-FgCounters
$d = Rf-FgDelta $a $b
$dPres = $d.dPres; $dFrame = $d.dFrame; $dGen = $d.dGen
"presented $($a.Presented) -> $($b.Presented) (d=$dPres); frameCount $($a.FrameBefore)/$($a.FrameAfter) -> $($b.FrameBefore)/$($b.FrameAfter) (widest d=$dFrame); generated>=$dGen"
if ($dFrame -le 0) { throw "the engine presented nothing in 2 s - this readback is not FG evidence" }
if ($dGen -le 0)   { throw "FG generated 0 frames in 2 s (presented advanced only by the real frames) - NOT active" }
```

**Pass condition, every FG case and after every transition leg (`fg1-x2`, `fg2-off`, `fg3-x2`):** `$dGen > 0`,
and at `multiplier=2` it should land near `$dFrame` — accept `$dGen -ge 0.5 * $dFrame`, report the actual pair.
Both thresholds apply to the CONSERVATIVE bound (`Δpresented − (FrameAfter₁ − FrameBefore₀)`), so a slow
status round-trip can only make the test stricter, never let it pass falsely.
On the `Off` leg the required result is the opposite: `$dGen` = 0 (± the one frame a leg boundary can straddle).
Record both `presented`/`frameCount` readbacks verbatim per leg — a single reading is not evidence.

*If `$dGen` is 0 while everything else looks live, check `focus=` / `fg=<0|1>` in the same status line first:*
DLSS-G interpolates ONLY while the game window has focus (`native\FgStreamline.cpp:35-39`), so an
unfocused window produces exactly this symptom and is a test-harness fault, not an FG failure.

**FG is ACTIVE iff** the `fg=` token starts with `live `, the embedded `provider=` names a real provider
(never `-`, never `detached`), `enabled=1`, `multiplier=2` for X2, and the `$dGen > 0` check above passes
while `fps` is non-zero. A `reason=` suffix is the rejection text and means it
is not running. Confirm with the two ints the host exposes directly (`FgHostAlive` / `FgHostProvider`,
`native\FgHost.cpp:581,583`; `Renderforge.Native` is a public static class, `src\Native.cs:10`):

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.Native","assembly":"Renderforge","member":"Fg_Alive","args":[]}'    -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.Native","assembly":"Renderforge","member":"Fg_Provider","args":[]}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.Native","assembly":"Renderforge","member":"Fg_Reason","args":[]}'   -PPRoot 'D:\PP-Instance3'
```

Expected with X2 running: `Fg_Alive` = `1`, `Fg_Provider` = `1` (FSR) / `2` (XeSS) / `3` (DLSS) matching the
case (ids from `native\RenderforgeNative.h:142` — `enum { FG_PROVIDER_NONE = 0, FG_PROVIDER_FSR = 1,
FG_PROVIDER_XESS = 2, FG_PROVIDER_DLSS = 3 };`), `Fg_Reason` = `""`. `Fg_Alive=0` on a case 7–9 capture means that capture is **not** FG evidence —
fix it or mark the case failed; do not report it as passing because `lastError` was `0`.

- [ ] Record, verbatim, for each of the two launches (6c pass 1 and pass 2): `api=` / `provider=` /
`d3d12HalfColor=` (requested) / `liveHalfColor` (latched) / `colorRT.graphicsFormat` / `outRT.graphicsFormat`,
and — for cases 7–9 — the `fg=` line plus `Fg_Alive` / `Fg_Provider`. A case whose readback does not match
its intended renderer, colour format or FG state is **not evidence** — relaunch or fix, do not report it.

### 6b — the capture protocol (defined once, used by every case below)

Three rules make a screenshot pair mean something:

1. **`-Window` capture, not the engine one.** `connect screenshot … -Window` grabs the FINAL composited
   frame after present (DWM `PrintWindow`), i.e. after upscale + our shim pass + UI — which is the only
   thing that can prove both "the scene changed" and "the HUD did not". It talks to the game only to learn
   which pid to capture, so it is also immune to the engine-capture edge cases. The window must **not be
   minimised**.
2. **Pause the game — and put the previous `timeScale` back, whatever it was.** `-Window` capture is not
   frame-synchronised, so animation, temporal accumulation and an idle-breathing soldier all leak into a
   whole-frame diff. Freeze time first. **Read `timeScale` into a variable before setting it to 0 and restore
   *that*** — hard-coding `1` on the way out silently rewrites the game's own speed if the mission was
   running at anything else (a slow-mo cinematic, another mod's time control), and the whole protocol then
   leaves the game in a state it was never in. The restore lives in a PowerShell `finally`, so an exception,
   a failed capture or Ctrl+C cannot leave the game frozen.

3. **A setter is not a frame.** `connect call` returns as soon as the main thread ran the setter; the new
   constant reaches the GPU on the *next* frame, and `-Window` grabs whatever DWM last composited. Capturing
   immediately after a setter can therefore photograph the previous mode. So after every state change, wait
   for **presented frames**, not a sleep: poll `UnityEngine.Time.frameCount` until it has advanced by at
   least 10.
   *`frameCount` at `timeScale = 0`:* Unity 2019.4 documents `Time.frameCount` as
   "The total number of frames since the start of the game (Read Only). This value starts at 0 and increases
   by 1 on each Update phase" — the Update phase of the player loop, which `timeScale` does not gate
   (`timeScale` scales `deltaTime` and the FixedUpdate budget, it does not stop the loop). The docs do not
   state this for the `timeScale = 0` case explicitly, so the helper below **does not assume it**: if
   `frameCount` has not moved within the timeout it throws instead of capturing a stale frame. There is no
   `Time.renderedFrameCount` in 2019.4 (the API page 404s), so `frameCount` is the only counter available.

4. **Always take an OFF/OFF control pair first, and re-take it until it is stable.** Two captures with
   *nothing changed between them* measure the residual noise floor (TAA/DLSS history, dithering, a cursor
   blink). Only a signal well above that floor counts, so an OFF/ON pair captured while the control floor is
   still above 0.5 is worthless — take the OFF/ON pair only after the control scene delta has settled to
   ≤ 0.5. Never compare a single OFF/ON pair on its own.

5. **Every capture filename is unique — per case AND per step.** A repeated name (`fg-X2-deut.png` written
   once before and once after the `Off` step) overwrites the earlier evidence and the two X2 legs become
   indistinguishable. Naming scheme, used everywhere below: **`cv-<case>-<step>-<mode>.png`**, where
   `<case>` is the 6c tag, `<step>` is a monotonically distinct step label (`control-a`, `control-b`, `set`,
   `fg1-x2`, `fg2-off`, `fg3-x2`), and `<mode>` is the colour-vision mode (`off`, `deut`, `prot`, `trit`).

Paste the helpers once per session, then run the protocol per case with the camera untouched throughout:

```powershell
$PP  = 'D:\PP-Instance3'
$Cli = 'E:\DEV\PhoenixPoint\PPCLI\ppcli.ps1'
$Out = 'C:\Temp\rf-cv'
New-Item -ItemType Directory -Force $Out | Out-Null

# PPCLI prints ONE object and it is an ENVELOPE: {status:"done",id,jobId,result:{ok,…}} - the verb DTO is
# `.result`, never the top level (PPCLI\AGENTS.md:60-61). So `(… | ConvertFrom-Json).value` is ALWAYS $null
# and every number read through it would be 0/empty. Inside `.result`:
#   op:"get"          -> {ok:true, value:<projected>}          (PPCLI\src\Reflect.cs:1064-1067, Value())
#   op:"get" +convertTo -> …plus {convertedTo, converted}      (Reflect.cs:554)
#   op:"set"          -> {ok:true, set:"<member>"}  - NO value key (Reflect.cs:572)
#   op:"invoke" void  -> {ok:true, void:true}                  (Reflect.cs:515); non-void -> {ok:true, value:…}
#   refusal           -> {ok:false, error, code} and NO value/items/output key (AGENTS.md:61); exits 1.
function Rf-Call([string]$json) {
    $raw = (& $Cli connect call $json -PPRoot $PP) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "ppcli exited $LASTEXITCODE for $json`n$raw" }
    $r = ($raw | ConvertFrom-Json).result
    if (-not $r -or $r.ok -ne $true) { throw "call refused (code=$($r.code)): $($r.error)`n  request: $json" }
    $r.value   # $null for set / void invoke, which is correct - those carry no value
}
function Rf-Frame { [int](Rf-Call '{"op":"get","type":"UnityEngine.Time","member":"frameCount"}') }

# timeScale is a float and its JSON must be built, not interpolated: `'…"value":' + $prevScale` emits
# `"value":` (invalid JSON) when the read failed, and a decimal comma under a non-invariant culture.
function Rf-SetTimeScale([double]$v) {
    Rf-Call (@{ op = 'set'; type = 'UnityEngine.Time'; member = 'timeScale'; value = $v } | ConvertTo-Json -Compress) | Out-Null
}
function Rf-GetTimeScale { [double](Rf-Call '{"op":"get","type":"UnityEngine.Time","member":"timeScale"}') }

# Block until the engine has presented at least $n more frames. Throws rather than capturing a stale frame:
# if frameCount does not advance while paused, EVERY number this task produces would be a ghost.
function Rf-WaitFrames([int]$n = 10, [int]$timeoutSec = 20) {
    $start = Rf-Frame
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Rf-Frame) -lt $start + $n) {
        if ((Get-Date) -gt $deadline) { throw "Time.frameCount stalled at $start - the player loop is not advancing at timeScale=0; captures would be meaningless" }
        Start-Sleep -Milliseconds 100
    }
}

# cv-<case>-<step>-<mode>.png, always after a frame wait.
function Rf-Shot([string]$case, [string]$step, [string]$mode) {
    Rf-WaitFrames 10
    $path = "$Out\cv-$case-$step-$mode.png"
    & $Cli connect screenshot ('{"path":"' + $path.Replace('\', '\\') + '"}') -Window -PPRoot $PP | Out-Null
    $path
}

function Rf-SetCv([string]$mode) { Rf-Call (@{ op = 'invoke'; type = 'Renderforge.RenderforgeMod'; member = 'SetColorVision'; args = @($mode) } | ConvertTo-Json -Compress) | Out-Null }

# Mean |d| on the SCENE row of cmp.py's output, as three doubles.
function Rf-SceneDelta([string]$a, [string]$b, [string]$label) {
    $lines = python C:\Temp\rf-cv\cmp.py $a $b $label
    $lines | Write-Host
    $scene = $lines | Where-Object { $_ -match '\bscene\b' }
    if ($scene -notmatch 'R=(-?[\d.]+) G=(-?[\d.]+) B=(-?[\d.]+)') { throw "cmp.py produced no scene row for $label" }
    @([double]$Matches[1], [double]$Matches[2], [double]$Matches[3])
}
```

```powershell
$case = 'd3d11-off'            # case tag, see the table in 6c

# Read the CURRENT timeScale first; restore THAT, not a hard-coded 1.
$prevScale = Rf-GetTimeScale
try {
    Rf-SetTimeScale 0
    Rf-SetCv 'None'

    # Control pair, re-taken until the noise floor is genuinely stable (<= 0.5 on every channel).
    # Only then is an OFF/ON delta interpretable.
    $a = $null; $b = $null
    for ($try = 1; $true; $try++) {
        $a = Rf-Shot $case "control-a" "off"
        $b = Rf-Shot $case "control-b" "off"
        $floor = Rf-SceneDelta $a $b "$case CONTROL try$try"
        if (($floor | Where-Object { $_ -gt 0.5 }).Count -eq 0) { break }
        if ($try -ge 5) { throw "$case : control scene floor never settled <= 0.5 - the pause did not take; fix that before reading anything else" }
    }

    foreach ($m in @(@('Deuteranopia','deut'), @('Protanopia','prot'), @('Tritanopia','trit'))) {
        Rf-SetCv $m[0]
        Rf-Shot $case "set" $m[1] | Out-Null      # Rf-Shot waits 10 presented frames before grabbing
    }
    Rf-SetCv 'None'
}
finally {
    # Runs on success, on a thrown assertion and on Ctrl+C: the game never stays frozen, and it goes back to
    # the speed it actually had.
    Rf-SetTimeScale $prevScale
}
```

Files this produces per case (list them in the record): `cv-<case>-control-a-off.png`,
`cv-<case>-control-b-off.png`, `cv-<case>-set-deut.png`, `cv-<case>-set-prot.png`, `cv-<case>-set-trit.png`.

- [ ] Write the comparison script **once**, to `C:\Temp\rf-cv\cmp.py`. It reports mean |Δ| per channel over
      two regions: a stable scene box and a HUD box that our pass must never touch.

```python
"""Region diff for the colour-vision live acceptance.
usage: python cmp.py A.png B.png [label]
Regions are fractions of the frame, so they survive a resolution change. Check them ONCE against the
first capture (open it, confirm SCENE holds only terrain/soldiers and HUD holds only the action bar)
and adjust the two tuples if this mission's framing differs - then leave them fixed for every case.
"""
import sys
from PIL import Image, ImageChops, ImageStat

SCENE = (0.30, 0.18, 0.70, 0.55)   # centre of the viewport: scene only, no HUD, no minimap
HUD   = (0.20, 0.88, 0.80, 0.99)   # bottom action bar: UI only, composited AFTER our pass

def crop(img, f):
    w, h = img.size
    return img.crop((int(f[0] * w), int(f[1] * h), int(f[2] * w), int(f[3] * h)))

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
if a.size != b.size:
    sys.exit("size mismatch: %s vs %s" % (a.size, b.size))
label = sys.argv[3] if len(sys.argv) > 3 else ""
for name, f in (("scene", SCENE), ("hud", HUD)):
    d = ImageChops.difference(crop(a, f), crop(b, f))
    st = ImageStat.Stat(d)
    mx = max(hi for _, hi in st.extrema)
    print("%-22s %-5s mean|d| R=%.3f G=%.3f B=%.3f  max=%d" %
          (label, name, st.mean[0], st.mean[1], st.mean[2], mx))
```

- [ ] Run it per case — control floor first, then each mode, then mode-vs-mode:

```powershell
$case = 'd3d11-off'
$C = "$Out\cv-$case"
python C:\Temp\rf-cv\cmp.py "$C-control-a-off.png" "$C-control-b-off.png" "$case CONTROL"
foreach ($m in 'deut','prot','trit') {
    python C:\Temp\rf-cv\cmp.py "$C-control-a-off.png" "$C-set-$m.png" "$case off-vs-$m"
}
python C:\Temp\rf-cv\cmp.py "$C-set-deut.png" "$C-set-prot.png" "$case deut-vs-prot"
python C:\Temp\rf-cv\cmp.py "$C-set-deut.png" "$C-set-trit.png" "$case deut-vs-trit"
```

**Acceptance per case** (all five, or the case fails):

- `CONTROL scene` mean |Δ| ≤ **0.5** on every channel — a paused frame captured twice must be near-identical. If it is not, the pause did not take; fix that before reading anything else.
- `CONTROL hud` mean |Δ| ≤ **0.5** on every channel — the HUD noise floor.
- `off-vs-<mode> scene` mean |Δ| ≥ **1.0** on at least one channel **and** ≥ 4× the control scene floor. This is the activation proof: LUT Off, style Off, sharpness 0, and the scene still changed.
- `off-vs-<mode> hud` mean |Δ| ≤ **control hud floor + 0.5** on every channel — the HUD is composited after the pass and must be untouched. A HUD delta anywhere near the scene delta means the correction is being applied to the wrong surface; that is a **failure**, not a cosmetic note.
- `deut-vs-prot` and `deut-vs-trit` scene mean |Δ| ≥ **1.0** — the three modes are genuinely different transforms, not one shared tint.

### 6c — the cases

Pass 1 launches Instance3 normally (D3D11), pass 2 with `-force-d3d12`; **confirm which one you actually
got with 6a's `api=` before trusting any case in that pass.** Set the state, run 6b, run the comparison,
record the numbers. Unless a row says otherwise the state is the hardest one — LUT Off, style Off,
sharpness 0 — so that nothing but colour vision can make the post pass run:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetLut","args":["Off",0]}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetSceneStyle","args":["Off",0,4]}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetSharpness","args":[0]}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetColorVision","args":["None"]}' -PPRoot 'D:\PP-Instance3'
```

Expected: each replies `ok:true`; the last returns `colorVision=None`.

| # | Tag | Pass | State to set first | What it proves |
|---|---|---|---|---|
| 1 | `d3d11-off` | 1 | `SetUpscaler Off` | **Upscaler OFF.** The correction runs with no upscaler at all — the `Device11.cpp:254` passthrough predicate, the case the old plan never covered |
| 2 | `d3d11-dlss` | 1 | `SetUpscaler DLSS` | the `Device11.cpp:278` DLSS call site |
| 3 | `d3d11-compose` | 1 | `SetUpscaler DLSS`, `SetLut VintageSepia 80`, `SetSceneStyle Cartoon 70 4` | **Composition live:** correction on top of a LUT preset *and* a style. Its control pair is captured with LUT+style already on, so the measured delta is the correction alone |
| 4 | `d3d12-dlss` | 2 | `SetUpscaler DLSS` | the **FP16-linear branch** (`styleLinear != 0`) D3D11 never takes — only valid if 6a reported `api=D3D12` **and** the live readback showed `liveHalfColor=True` with `colorRT`/`outRT` at `R16G16B16A16_SFloat`; `d3d12HalfColor=True` alone is only the request |
| 5 | `d3d12-fsr` | 2 | `SetUpscaler FSR` | `Fsr12.cpp:289/297` — FSR does its own RCAS, so `doPost` is driven by `grade` alone and this is the only proof the widened predicate reaches it |
| 6 | `d3d12-xess` | 2 | `SetUpscaler XeSS` | `Xess12.cpp:324/332` |
| 7 | `d3d12-fg-dlss` | 2 | `SetUpscaler DLSS`, `SetFgProvider Dlss`, `SetFrameGen X2` | **Frame generation on**, the case the provider probes never exercise: the correction must still be there in the presented frame |
| 8 | `d3d12-fg-fsr` | 2 | `SetUpscaler FSR`, `SetFgProvider Fsr`, `SetFrameGen X2` | FG through the FSR provider |
| 9 | `d3d12-fg-xess` | 2 | `SetUpscaler XeSS`, `SetFgProvider Xess`, `SetFrameGen X2` | FG through the XeSS provider |

Example of setting a case (case 3):

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetUpscaler","args":["DLSS"]}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetLut","args":["VintageSepia",80]}' -PPRoot 'D:\PP-Instance3'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetSceneStyle","args":["Cartoon",70,4]}' -PPRoot 'D:\PP-Instance3'
```

- [ ] **FG transition test** (cases 7–9, one extra step each). With colour vision left ON at `Deuteranopia`, walk frame generation `X2 → Off → X2` and capture after each step; the correction must be present in all three:

The three legs are `X2 → Off → X2`, so the two X2 legs would collide on one filename. The step label is
therefore part of the name (`fg1-x2`, `fg2-off`, `fg3-x2`), never the FG value alone.

```powershell
$case = 'd3d12-fg-dlss'        # or d3d12-fg-fsr / d3d12-fg-xess
$prevScale = Rf-GetTimeScale
try {
    Rf-SetTimeScale 0
    Rf-SetCv 'Deuteranopia'
    $step = 0
    foreach ($fg in 'X2','Off','X2') {
        $step++
        Rf-Call (@{ op = 'invoke'; type = 'Renderforge.RenderforgeMod'; member = 'SetFrameGen'; args = @($fg) } | ConvertTo-Json -Compress) | Out-Null
        Rf-WaitFrames 10
        # FG activation: two readbacks ~2 s apart of BOTH counters (Rf-FgCounters, 6a). `presented` alone
        # advances with zero generated frames - the generated count is d(presented) - d(frameCount).
        $c1 = Rf-FgCounters
        $alive = Rf-Call '{"op":"invoke","type":"Renderforge.Native","assembly":"Renderforge","member":"Fg_Alive","args":[]}'
        $prov  = Rf-Call '{"op":"invoke","type":"Renderforge.Native","assembly":"Renderforge","member":"Fg_Provider","args":[]}'
        Start-Sleep -Seconds 2
        $c2 = Rf-FgCounters
        $d = Rf-FgDelta $c1 $c2                      # bracketed, conservative bound (6a)
        $dPres = $d.dPres; $dFrame = $d.dFrame; $dGen = $d.dGen
        "$case fg$step-$fg alive=$alive provider=$prov dPresented=$dPres dFrame=$dFrame generated=$dGen`n  $($c1.Status)`n  $($c2.Status)" |
            Tee-Object -FilePath "$Out\cv-$case-fg.txt" -Append
        if ($dFrame -le 0) { throw "$case fg$step-$fg : frameCount did not advance in 2 s - this leg is not evidence" }
        if ($fg -eq 'X2' -and $dGen -lt (0.5 * $dFrame)) { throw "$case fg$step-$fg : generated=$dGen over $dFrame real frames - FG is NOT interpolating" }
        if ($fg -eq 'Off' -and $dGen -gt 1)              { throw "$case fg$step-$fg : generated=$dGen with FG off - the chain did not detach" }
        Rf-Shot $case ("fg{0}-{1}" -f $step, $fg.ToLowerInvariant()) "deut" | Out-Null
    }
    Rf-SetCv 'None'
    Rf-Call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetFrameGen","args":["Off"]}' | Out-Null
}
finally {
    Rf-SetTimeScale $prevScale
}
```

Produces `cv-<case>-fg1-x2-deut.png`, `cv-<case>-fg2-off-deut.png`, `cv-<case>-fg3-x2-deut.png` — three
distinct files — plus `cv-<case>-fg.txt` with the three status pairs.

Acceptance, per leg:

- Each of the three captures, compared against that case's `cv-<case>-control-a-off.png`, meets the same
  scene/HUD thresholds as 6b. A transition that clears the correction is a failure.
- **On the two X2 legs, FG must actually be running** — `Fg_Alive` = `1`, `Fg_Provider` matching the case
  (1 FSR / 2 XeSS / 3 DLSS — `native\RenderforgeNative.h:142`), and inside the `fg=` token: `live `, `provider=` naming a real provider,
  `enabled=1`, `multiplier=2`, no `reason=` suffix, and **`generated` = `dPresented - dFrame` > 0** across the
  two readbacks 2 s apart (at `multiplier=2`, ≈ `dFrame`; the loop throws below `0.5 * dFrame`) with `fps`
  non-zero. `presented` rising on its own is NOT that proof — it counts the real frames too
  (`FgHook.cpp:180-190,536`; see 6a). A leg with `generated` = 0 is a parked chain, not FG evidence.
- On the `Off` leg the opposite must hold: `Fg_Alive` = `0`, the `fg=` token starts with `off `, and
  `generated` = 0 (the loop allows 1 for a leg boundary straddling a readback).
- `lastError=0` in `GetStatus` is a necessary condition, **not** proof of anything about FG: it is the
  upscaler's error slot (`Native.Dlss_LastError()`) and stays `0` when FG never started at all. Judge FG by
  the tokens above, and `lastError` only as a "the upscale pass did not break" check.

- [ ] After every case, restore the state you changed. The `timeScale` is already back — the `finally` above
  restored the value read before the pause — so only the feature state is left:

```powershell
Rf-SetCv 'None'
Rf-Call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetFrameGen","args":["Off"]}' | Out-Null
Rf-Call '{"op":"get","type":"UnityEngine.Time","member":"timeScale"}'   # confirm it is back at $prevScale, not hard-coded 1
```

- [ ] Record, for every case: the full 6a readback — `api=` / `provider=` / `d3d12HalfColor=` (requested) /
`liveHalfColor` (latched) / `colorRT.graphicsFormat` / `outRT.graphicsFormat`, plus `fg=` + `Fg_Alive` /
`Fg_Provider` for cases 7–9 — the control floor, the three scene/HUD deltas, and **the list of capture
filenames** (`cv-<case>-<step>-<mode>.png`) the case produced. Those numbers go into the doc in Task 7. A
case reported without its readback, or with two captures sharing a filename, is not evidence.
- [ ] If PPCLI itself misbehaves at any point, append an entry to `E:\DEV\PhoenixPoint\PPCLI\ISSUES.md` (attempted → happened → expected → evidence → severity) and work around it. Do not edit PPCLI source.
- [ ] Nothing to commit in this task unless a fix was needed; if the live run forced a code change, re-run Task 2e's probes and commit the fix on its own.

---

## Task 7 — Documentation

**Files:** `README.md`, `docs\DESIGN.md`

- [ ] `README.md` — add a bullet to **What it does**, after the Scene styles bullet (`:16`):

```markdown
- **Colour vision correction** offers Deuteranopia, Protanopia and Tritanopia daltonization for tactical missions, at full fixed strength, composed on top of any LUT filter or scene style. It redistributes the colours the eye cannot separate onto the channels it can, using the Machado et al. (2009) simulation matrices. The correction applies to the **scene only** — the interface is composited after this pass and is not corrected.
```

- [ ] `README.md` — add a row to the settings table, after the Scene style row (`:82`):

```markdown
| Colour vision | Off | Deuteranopia, Protanopia or Tritanopia correction for tactical missions, at full strength. Scene only: the HUD and menus are drawn after this pass and stay uncorrected. |
```

- [ ] `docs\DESIGN.md` — add a `### Colour vision` subsection under `### Renderforge.dll (C#, ~700 LOC)`'s neighbourhood (place it before `### Data flow per frame`, `:361`), recording: the stage order (after `Grade()` and `Stylize()`); `D = I + R·(I − S)` with all three sources from the provenance table at the top of this plan — protan `R` from Fidaner's `conv_img.m` (which corrects `errorp` only), deutan `R` the same matrix on the authority of `daltonize/daltonize.py:125`, tritan `R` from the ixora matrix table at `https://ixora.io/projects/colorblindness/color-blindness-simulation-research.html`, flagged as an unverified secondary source whose Simon-Liedtke & Farup 2016 reference justifies the per-type approach but is not the origin of the numbers, and the note that ixora's differing deuteranopia `R` was rejected; the constant-buffer extension (`colorVision` at byte 40, `cvRow0..2` at 48/64/80 of the 256-byte block); the two colour-space branches keyed on `styleLinear`; `PostShaderEnabled` as the single activation seam replacing the old two-term disjunction at all 11 native call sites; and the measured live deltas from Task 6.
- [ ] Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add README.md docs/DESIGN.md
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "docs: record the colour-vision stage, its matrix sources and live acceptance"
```

---

## Self-review — spec coverage

Check each line before declaring the plan done.

| Spec requirement (§B) | Where it is satisfied |
|---|---|
| Separate stage AFTER `Grade()` and scene style | Task 2c — `ColorVision(Grade(Stylize(p,c)))` at `Sharpen.cpp:74` |
| Own int param `ColorVision` 0/1/2/3 | Task 2c cbuffer `uint colorVision`; Task 3 `DLSS_CV_*`, `FrameParams.colorVision`; Task 4 `ColorVisionMode` |
| Full, fixed correction, no slider in v1 | Task 5 — picker only, no `BuildSlider` call |
| Composes with any LUT preset / style | Task 2c — chained after both inside the one shader; probe check 8 runs LUT Vivid 0.85 + Cartoon **and** PixelArt and asserts the correction composes; live case 3 in Task 6 |
| `D = I + R·(I − S)`, column-vector linear RGB | Task 2b `CvCorrection`; Task 1 reference; Task 2a assertion |
| `S` = Machado 2009 severity 1.0, verified + cited | Provenance table; all three **exact match** against the authors' own page |
| `R` = Fidaner for deut/prot, sourced matrix for tritan | Provenance table — protan = Fidaner `conv_img.m` `err2mod` (which corrects `errorp` only); deutan = the same `err2mod`, cited to `daltonize/daltonize.py:125` where it is applied for every deficiency type, with the choice stated explicitly; tritan = the ixora matrix table, shipped flagged as an unverified secondary source |
| One 3×3 per type, precomputed on the CPU, row-major | Task 2b `constexpr CvCorrection`; Task 2d packs it row-major |
| Uploaded as constants, alignment checked | Task 2c packing note (40/44 fill register 2; rows at 48/64/80); Task 2d |
| UNORM: decode sRGB → linear, apply, `saturate`, encode — exact piecewise curve | Task 2c `CvSrgbToLinear` / `CvLinearToSrgb`, both piecewise |
| FP16 linear: apply, `max(0)`, no encode, keyed on `styleLinear` | Task 2c `styleLinear != 0` branches |
| Clamp always in linear before encoding | Task 2c — `CvLinearToSrgb` saturates its linear input before the transfer function |
| `ColorVision != 0` counts as "pass active" in EVERY predicate | Task 3 — `Sharpen.cpp:136`, `Device11.cpp:91/254/278`, `D3D12Sharpen.h:261`, `Device12.cpp:188/196`, `Fsr12.cpp:289/297`, `Xess12.cpp:324/332`, `DlssDriver.cs:197` (one gate shared with the `:504` submission, inside `ColorVisionPanel.Active`); the Task 3 grep gate uses a lookahead so the new `PostShaderEnabled` body is permitted and any remaining two-term site fails it, plus a positive control expecting exactly `1` |
| UI picker row next to the LUT row, en/ru labels | Task 5 — `GraphicsPanel.cs:62`, `Labels`, tooltip, title, all `DlssConfig.Loc(en, ru)` |
| Hidden from the Mods menu | Task 4 — `nameof(ColorVision)` in `HiddenFromModSettings` |
| Console/setter parity with `SetLut` | Task 5 — `RenderforgeMod.SetColorVision` |
| README: scene only, HUD not corrected | Task 7 — bullet and table row both say it |
| Probe: mode 0 bit-exact bypass | Task 2a check 2, both `hdr` values, `==` not a tolerance |
| Probe: matrices equal an independent reference | Task 2a check 1 (CPU matrices vs `colour_vision_ref.py` at 1e-6) **and** check 4/5 (every GPU sample vs the CPU reference model built from those same numbers, on BOTH colour-space paths — cube, sRGB knees, gamut corners) |
| Probe: encoded/linear parity ≤ 1/255 | Task 2a check 5 |
| Probe: saturated primaries stay in [0,1] | Task 2a checks 3 and 6, plus white/black fixed points and the 196-sample sRGB-knee set |
| Live: LUT=None/style=None/sharpen=0 + `ColorVision=1` differs from 0, with numbers | Task 6 — 9 cases (upscaler Off, D3D11 DLSS, LUT+style composition, D3D12 DLSS/FSR/XeSS, FG X2 on all three providers plus X2→Off→X2), each with a paused, frame-synchronised `-Window` OFF/OFF control pair (uniquely named `cv-<case>-<step>-<mode>.png`, `timeScale` saved and restored in a `finally`), scene- and HUD-region mean&#124;Δ&#124;, the real `api=`/`provider=` readback plus the LIVE `liveHalfColor` + `colorRT`/`outRT` `graphicsFormat`, and — for FG cases — `Fg_Alive`/`Fg_Provider` and a moving `presented` counter, never a launch flag and never `lastError=0` alone |
| Instance3 only | Task 6 — `-PPRoot 'D:\PP-Instance3'`, explicit prohibition on Instance2 and the Steam install |

Deliberately **not** built (say so if asked, do not add): a severity slider, a per-channel strength control, HUD/UI correction, a simulation ("show me what a deuteranope sees") preview mode, and any per-type `R` for deut/prot beyond the single Fidaner matrix the authors actually publish.
