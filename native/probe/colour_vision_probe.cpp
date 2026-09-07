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
        // Force the analytic layout even when every stage is off (mode 0, LUT off, style off), like
        // scene_style_probe: pack with a LUT on, then switch the LUT off in place. The mode-0 bypass check is then a
        // test of the shader's own Off branches (styleLinear, W/H and the zero grade all real), not of the
        // NIS/RCAS-layout fallback FillSharpenConstants would otherwise pack.
        const bool forced = !PostShaderEnabled(lutPreset, lutStrength, style, mode, GradeParams{});
        FillSharpenConstants(constants, DLSS_SHARPEN_RCAS, 0, width, height, forced ? DLSS_LUT_VIVID : lutPreset,
                             forced ? 1.0f : lutStrength, hdr, style, mode);
        if (forced) { reinterpret_cast<float*>(constants)[1] = 0; reinterpret_cast<unsigned*>(constants)[4] = 0; }
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
