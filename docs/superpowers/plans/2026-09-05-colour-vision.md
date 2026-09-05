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
| `R` deut + prot | `0 0 0 / 0.7 1 0 / 0.7 0 1` | Fidaner, Lin & Ozguven 2005, `err2mod` verbatim in their MATLAB — https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m ; report http://acorn.stanford.edu/psych221/projects/2005/ofidaner/colorblindness_project.htm . One matrix for **both** types (they do not publish a per-type pair). |
| `R` tritan | `1 0 0.7 / 0 1 0.7 / 0 0 0` | http://ixora.io/projects/colorblindness/daltonization.html — per-deficiency redistribution, error pushed onto the channels a tritan can still discriminate. **Secondary source** (blog, attributing the per-type principle to Simon-Liedtke & Farup, JVCI 2016), not a peer-reviewed table. |

**Tritan status: SHIPPING, flagged.** The spec's fallback ("if no reference is found, defer tritan") does not fire — a citable matrix exists. It is a weaker citation than the deut/prot pair, so the provenance comment in `ColorVision.h` and the DESIGN note both say so.
Note the deliberate deviation: ixora also publishes a *different* deuteranopia `R` (`1 0.7 0 / 0 0 0 / 0 0.7 1`). The spec mandates the Fidaner matrix for deut/prot; Fidaner wins, ixora is used only where Fidaner has nothing.

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
    deuteranopia / protanopia: Fidaner, Lin & Ozguven 2005, "Analysis of Color Blindness" (`err2mod`,
        https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m). One matrix for both types.
    tritanopia: per-deficiency redistribution from http://ixora.io/projects/colorblindness/daltonization.html
        (secondary source; no peer-reviewed table publishes one).

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

### 2a — the probe, written before the shader exists

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
    // LUT off, style off, sharpness 0: only the colour-vision stage may touch the pixels.
    std::vector<Pixel> Run(const std::vector<Pixel>& pixels, unsigned width, unsigned height, int mode, bool hdr) {
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
        FillSharpenConstants(constants, DLSS_SHARPEN_RCAS, 0, width, height, DLSS_LUT_OFF, 0, hdr,
                             SceneStyleParams{}, mode);
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
            // 4. Encoded and FP16-linear paths agree to better than one 8-bit step.
            std::vector<Pixel> linear(cube.size());
            for (size_t i = 0; i < cube.size(); ++i)
                linear[i] = {SrgbToLinear(cube[i][0]), SrgbToLinear(cube[i][1]), SrgbToLinear(cube[i][2]), cube[i][3]};
            const auto fromLinear = probe.Run(linear, 17, 289, mode, true);
            for (size_t i = 0; i < cube.size(); ++i)
                for (int c = 0; c < 3; ++c) {
                    float reencoded = LinearToSrgb(fromLinear[i][c] < 0 ? 0 : fromLinear[i][c] > 1 ? 1 : fromLinear[i][c]);
                    Require(std::abs(reencoded - encoded[i][c]) <= 1.0f / 255.0f, "Encoded/linear parity exceeds 1/255");
                }
            // 5. Saturated primaries stay in gamut, white and black are fixed points.
            const auto edge = probe.Run(primaries, 8, 1, mode, false);
            for (size_t i = 0; i < primaries.size(); ++i)
                for (int c = 0; c < 3; ++c)
                    Require(edge[i][c] >= 0.0f && edge[i][c] <= 1.0f, "Saturated primary left [0,1]");
            for (int c = 0; c < 3; ++c) {
                Require(std::abs(edge[6][c] - 1.0f) < 2e-3f, "White moved");
                Require(std::abs(edge[7][c]) < 2e-3f, "Black moved");
            }
            // 6. The stage is not a no-op: at least one cube colour actually moves.
            bool moved = false;
            for (size_t i = 0; i < cube.size() && !moved; ++i)
                for (int c = 0; c < 3; ++c)
                    if (std::abs(encoded[i][c] - cube[i][c]) > 1.0f / 255.0f) { moved = true; break; }
            Require(moved, "Colour-vision stage changed nothing");
        }

        printf("PASS: production HLSL on D3D11 WARP; 3 modes; matrices match colour_vision_ref.py; "
               "bit-exact mode-0 bypass on both paths; 4913-colour cube in gamut; encoded/linear parity <= 1/255; "
               "saturated primaries, white and black endpoints.\n");
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

- [ ] Add `ColorVision.h` to the `add_library(RenderforgeNative SHARED ...)` list in `native\CMakeLists.txt`, on the line after `Sharpen.h` (`:72`).
- [ ] Confirm the probe does **not** build yet (TDD red). `ColorVision.h`, `CvMatrix`, `CvCorrection`, `ColorVisionEnabled` and the 9-argument `FillSharpenConstants` do not exist:

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build E:\DEV\PhoenixPoint\Renderforge\build\native --config Release --target colour_vision_probe -- /verbosity:minimal
```

Expected: a non-zero exit with `C1083: Cannot open include file: 'ColorVision.h'`. If it builds, something already exists — stop and re-read the tree before continuing.

### 2b — the matrices

- [ ] Create `native\ColorVision.h`:

```cpp
// ColorVision.h - daltonization matrices for the analytic post pass. Mode ordinals cross the managed/native
// ABI (DLSS_CV_* in RenderforgeNative.h, ColorVisionMode in src\DlssConfig.cs); append only.
//
// D = I + R * (I - S), applied to COLUMN-VECTOR linear RGB (v' = D * v), stored ROW-MAJOR.
//
// S = Machado, Oliveira & Fernandes 2009 simulation matrices at severity 1.0, transcribed from the authors'
//     own table: https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html
// R = error redistribution: how the colour a deficient eye cannot separate is pushed onto channels it can.
//     deuteranopia / protanopia: Fidaner, Lin & Ozguven 2005, "Analysis of Color Blindness" - `err2mod`
//         verbatim in their MATLAB (https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m);
//         report at http://acorn.stanford.edu/psych221/projects/2005/ofidaner/colorblindness_project.htm.
//         They publish ONE matrix used for both types, not a per-type pair.
//     tritanopia: http://ixora.io/projects/colorblindness/daltonization.html, which redistributes onto the
//         channels a tritan can still discriminate. This is a SECONDARY source (a blog attributing the
//         per-deficiency principle to Simon-Liedtke & Farup, JVCI 2016), weaker than the Fidaner citation
//         above; it is used only because no peer-reviewed table publishes a tritan redistribution matrix.
//         ixora also gives a different deuteranopia R - the Fidaner one wins there, per the design spec.
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
    { { 0.0f, 0.0f, 0.0f,  0.7f, 1.0f, 0.0f,  0.7f, 0.0f, 1.0f } },   // deuteranopia (Fidaner)
    { { 0.0f, 0.0f, 0.0f,  0.7f, 1.0f, 0.0f,  0.7f, 0.0f, 1.0f } },   // protanopia   (Fidaner, same matrix)
    { { 1.0f, 0.0f, 0.7f,  0.0f, 1.0f, 0.7f,  0.0f, 0.0f, 0.0f } },   // tritanopia   (ixora.io)
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

### 2c — the predicate and the constant-block signature

- [ ] In `native\Sharpen.h`, add the include next to the existing one (`:7`, after `#include "SceneStyle.h"`):

```cpp
#include "ColorVision.h"
```

- [ ] In `native\Sharpen.h`, extend the `FillSharpenConstants` declaration (`:17-19`) with the new trailing parameter:

```cpp
void FillSharpenConstants(void* dst256, int kind, float sharpness, unsigned w, unsigned h,
                          int lutPreset = 0, float lutStrength = 0.0f, bool hdr = false,
                          const SceneStyleParams& style = SceneStyleParams{}, int colorVision = 0);
```

- [ ] In `native\Sharpen.h`, immediately after `ColorGradeEnabled` (`:21`), add:

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

### 2d — the HLSL stage

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

### 2e — constant packing

- [ ] In `native\Sharpen.cpp`, replace `FillSharpenConstants` (`:132-151`) with:

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

- [ ] In `native\Sharpen.cpp`, change the pass predicate at `:136` — it is the `if` line replaced above; confirm it now reads `if (PostShaderEnabled(lutPreset, lutStrength, style, colorVision)) {`.

### 2f — green

- [ ] Build and run:

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' --build E:\DEV\PhoenixPoint\Renderforge\build\native --config Release --target lut_probe scene_style_probe colour_vision_probe -- /verbosity:minimal
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\colour_vision_probe.exe
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\lut_probe.exe
E:\DEV\PhoenixPoint\Renderforge\build\native\Release\scene_style_probe.exe
```

Expected: the cmake build reports 0 errors, then

```
PASS: production HLSL on D3D11 WARP; 3 modes; matrices match colour_vision_ref.py; bit-exact mode-0 bypass on both paths; 4913-colour cube in gamut; encoded/linear parity <= 1/255; saturated primaries, white and black endpoints.
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

- [ ] Prove no predicate was missed. Before this task the pattern matches **11** sites (`Sharpen.cpp:136`, `Device11.cpp:91/254/278`, `D3D12Sharpen.h:261`, `Device12.cpp:188/196`, `Fsr12.cpp:289/297`, `Xess12.cpp:324/332`); after it, the command must return **zero** matches:

```powershell
Select-String -Path E:\DEV\PhoenixPoint\Renderforge\native\*.cpp,E:\DEV\PhoenixPoint\Renderforge\native\*.h -Pattern 'ColorGradeEnabled\(.*\)\s*\|\|\s*SceneStyleEnabled'
```

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

- [ ] `src\DlssConfig.cs` — add the field name to `HiddenFromModSettings` (`:31-36`), on the `SceneStyle` line:

```csharp
            nameof(SceneStyle), nameof(SceneStyleStrength), nameof(PixelSize), nameof(CrispFonts),
            nameof(ColorVision)
```

- [ ] `src\DlssConfig.cs` — add the field immediately after `PixelSize` (`:51`):

```csharp
        [ConfigField("Colour vision", "Off, Deuteranopia, Protanopia or Tritanopia. Redistributes colours the eye cannot separate onto channels it can. Scene only; the interface is drawn after this pass. Also in Options → Graphics.")]
        public ColorVisionMode ColorVision = ColorVisionMode.None;
```

- [ ] `src\DlssConfig.cs` — add the Russian entry to `Ru`, after the `PixelSize` line (`:89`):

```csharp
            { nameof(ColorVision), new[] { "Цветовое зрение", "Выкл, дейтеранопия, протанопия или тританопия. Перераспределяет неразличимые цвета на различимые каналы. Только сцена: интерфейс рисуется после этого прохода. Также в Настройки → Графика." } },
```

- [ ] `src\DlssDriver.cs:197` — the pipeline must start for colour vision alone (upscaler Off, LUT Off, style Off):

```csharp
            bool needsPipeline = wantMode != RenderforgeMode.Off || lutActive || SceneStylePanel.Active(cfg)
                || ColorVisionPanel.Active(cfg);
```

- [ ] `src\DlssDriver.cs:504-506` — send the mode on the same slot, right after the scene-style send. Gated on `TacticalActive` exactly like `lutPreset` at `:492`: the post pass only exists on the tactical camera, so the geoscape is out of reach either way, and mirroring the LUT keeps one rule for the whole shader:

```csharp
                if (SceneStylePanel.Active(cfg))
                    Native.Dlss_SetSceneStyle(slot, (int)cfg.SceneStyle,
                        Mathf.Clamp01(cfg.SceneStyleStrength / 100f), Mathf.Clamp(cfg.PixelSize, 2, 16));
                if (RenderforgeMod.TacticalActive && ColorVisionPanel.Active(cfg))
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

        internal static bool Active(DlssConfig cfg) => cfg != null
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

- [ ] `src\GraphicsPanel.cs:43` — hide it with the others when the rows are turned off:

```csharp
                    LutPanel.Hide(src.transform.parent);
                    ColorVisionPanel.Hide(src.transform.parent);
                    SceneStylePanel.Hide(src.transform.parent);
```

- [ ] `src\GraphicsPanel.cs:61-62` — build it directly after the LUT rows, as the spec requires ("next to the LUT row"):

```csharp
                after = LutPanel.Build(__instance, sharp != null ? sharp.transform.parent : picker.transform, mod.Cfg);
                after = ColorVisionPanel.Build(__instance, after, mod.Cfg);
                SceneStylePanel.Build(__instance, after, mod.Cfg);
```

- [ ] `src\Pickers.cs:89-90` — drop the cached controller with the others:

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

- [ ] Launch Instance3 into a tactical mission and wait until PPCLI actually answers before sending anything else:

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
```

- [ ] Put the pass in its hardest state — LUT Off, style Off, sharpness 0 — so nothing but colour vision can make the post pass run:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetLut","args":["Off",0]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetSceneStyle","args":["Off",0,4]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetSharpness","args":[0]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetColorVision","args":["None"]}'
```

Expected: each replies `ok:true`; the last returns `colorVision=None`.

- [ ] Baseline screenshot from a camera that is not moving (do not touch the camera between the two captures):

```powershell
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf-cv\\cv-off.png"}'
```

- [ ] Switch on deuteranopia correction and capture again:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","member":"SetColorVision","args":["Deuteranopia"]}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf-cv\\cv-deut.png"}'
```

- [ ] Repeat for the other two modes into `cv-prot.png` and `cv-trit.png`, then restore `None`.
- [ ] Measure the difference — a claim of "it works" needs numbers:

```powershell
python -c @'
from PIL import Image, ImageChops
import statistics, sys
base = Image.open(r"C:\Temp\rf-cv\cv-off.png").convert("RGB")
for name in ("deut", "prot", "trit"):
    img = Image.open(rf"C:\Temp\rf-cv\cv-{name}.png").convert("RGB")
    if img.size != base.size: sys.exit("size mismatch: " + name)
    diff = ImageChops.difference(base, img)
    px = list(diff.getdata())
    flat = [c for p in px for c in p]
    changed = sum(1 for p in px if max(p) > 2) / len(px)
    print("%s: mean|delta|=%.3f max=%d changed>2/255=%.1f%%" %
          (name, statistics.fmean(flat), max(flat), changed * 100))
'@
```

Acceptance, all four must hold:
- each of the three modes reports `mean|delta|` clearly above 1.0 and `changed>2/255` above 20% — the pass is running with LUT and style Off, which is the activation proof the spec asks for;
- the three modes differ from **each other**, not just from the baseline (compare `cv-deut.png` against `cv-prot.png` the same way — non-zero);
- the HUD is visibly **un**corrected in the screenshots (expected: the spec says the UI is composited after the pass);
- `.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.Native","member":"Dlss_LastError","args":[]}'` returns 0 — in particular not `-3` (`DLSS_ERR_SHARPEN`).

- [ ] Repeat the `None` → `Deuteranopia` capture pair once under `-force-d3d12` on Instance3, to cover the FP16-linear branch (`styleLinear != 0`) that D3D11 never takes. Same acceptance.
- [ ] Record the measured numbers; they go into the doc in Task 7.
- [ ] If PPCLI itself misbehaves at any point, append an entry to `E:\DEV\PhoenixPoint\PPCLI\ISSUES.md` (attempted → happened → expected → evidence → severity) and work around it. Do not edit PPCLI source.
- [ ] Nothing to commit in this task unless a fix was needed; if the live run forced a code change, re-run Task 2f's probes and commit the fix on its own.

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

- [ ] `docs\DESIGN.md` — add a `### Colour vision` subsection under `### Renderforge.dll (C#, ~700 LOC)`'s neighbourhood (place it before `### Data flow per frame`, `:361`), recording: the stage order (after `Grade()` and `Stylize()`); `D = I + R·(I − S)` with all three sources from the provenance table at the top of this plan, including the explicit note that the tritan `R` rests on a secondary source and that ixora's differing deuteranopia `R` was rejected in favour of Fidaner's; the constant-buffer extension (`colorVision` at byte 40, `cvRow0..2` at 48/64/80 of the 256-byte block); the two colour-space branches keyed on `styleLinear`; `PostShaderEnabled` as the single activation seam replacing the old two-term disjunction at all 11 native call sites; and the measured live deltas from Task 6.
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
| Separate stage AFTER `Grade()` and scene style | Task 2d — `ColorVision(Grade(Stylize(p,c)))` at `Sharpen.cpp:74` |
| Own int param `ColorVision` 0/1/2/3 | Task 2d cbuffer `uint colorVision`; Task 3 `DLSS_CV_*`, `FrameParams.colorVision`; Task 4 `ColorVisionMode` |
| Full, fixed correction, no slider in v1 | Task 5 — picker only, no `BuildSlider` call |
| Composes with any LUT preset / style | Task 2d — chained after both inside the one shader; Task 2f probes run with LUT Off, live run in Task 6 composes them |
| `D = I + R·(I − S)`, column-vector linear RGB | Task 2b `CvCorrection`; Task 1 reference; Task 2a assertion |
| `S` = Machado 2009 severity 1.0, verified + cited | Provenance table; all three **exact match** against the authors' own page |
| `R` = Fidaner for deut/prot, sourced matrix for tritan | Provenance table; tritan sourced to ixora.io, shipped with the weaker-citation caveat recorded in code + DESIGN |
| One 3×3 per type, precomputed on the CPU, row-major | Task 2b `constexpr CvCorrection`; Task 2e packs it row-major |
| Uploaded as constants, alignment checked | Task 2d packing note (40/44 fill register 2; rows at 48/64/80); Task 2e |
| UNORM: decode sRGB → linear, apply, `saturate`, encode — exact piecewise curve | Task 2d `CvSrgbToLinear` / `CvLinearToSrgb`, both piecewise |
| FP16 linear: apply, `max(0)`, no encode, keyed on `styleLinear` | Task 2d `styleLinear != 0` branches |
| Clamp always in linear before encoding | Task 2d — `CvLinearToSrgb` saturates its linear input before the transfer function |
| `ColorVision != 0` counts as "pass active" in EVERY predicate | Task 3 — `Sharpen.cpp:136`, `Device11.cpp:91/254/278`, `D3D12Sharpen.h:261`, `Device12.cpp:188/196`, `Fsr12.cpp:289/297`, `Xess12.cpp:324/332`, `DlssDriver.cs:197`; grep gate proves none remain |
| UI picker row next to the LUT row, en/ru labels | Task 5 — `GraphicsPanel.cs:62`, `Labels`, tooltip, title, all `DlssConfig.Loc(en, ru)` |
| Hidden from the Mods menu | Task 4 — `nameof(ColorVision)` in `HiddenFromModSettings` |
| Console/setter parity with `SetLut` | Task 5 — `RenderforgeMod.SetColorVision` |
| README: scene only, HUD not corrected | Task 7 — bullet and table row both say it |
| Probe: mode 0 bit-exact bypass | Task 2a check 2, both `hdr` values, `==` not a tolerance |
| Probe: matrices equal an independent reference | Task 2a check 1 against `colour_vision_ref.py` output at 1e-6 |
| Probe: encoded/linear parity ≤ 1/255 | Task 2a check 4 |
| Probe: saturated primaries stay in [0,1] | Task 2a checks 3 and 5, plus white/black fixed points |
| Live: LUT=None/style=None/sharpen=0 + `ColorVision=1` differs from 0, with numbers | Task 6 — the four `Set*` calls, the screenshot pair, the diff script, the stated thresholds |
| Instance3 only | Task 6 — `-PPRoot 'D:\PP-Instance3'`, explicit prohibition on Instance2 and the Steam install |

Deliberately **not** built (say so if asked, do not add): a severity slider, a per-channel strength control, HUD/UI correction, a simulation ("show me what a deuteranope sees") preview mode, and any per-type `R` for deut/prot beyond the single Fidaner matrix the authors actually publish.
