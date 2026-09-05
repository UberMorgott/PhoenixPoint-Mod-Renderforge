// Production HLSL readback on WARP. No window, game, neural model or vendor runtime.
#include "Sharpen.h"
#include "RenderforgeNative.h"
#include "SceneStyle.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D11 operation failed"); }
static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

struct StyleProbe
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    StyleProbe()
    {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, &device, nullptr, &context));
        int kind = 0;
        ComPtr<ID3DBlob> blob; blob.Attach(CompileSharpenBlob(&kind, false, true));
        Require(blob.Get() != nullptr, "Production style HLSL did not compile");
        Check(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader));
    }

    std::vector<Pixel> Run(const std::vector<Pixel>& pixels, unsigned width, unsigned height,
        SceneStyleParams style, bool linear = false, float sharpness = 0)
    {
        Require(pixels.size() == size_t(width) * height, "Input dimensions mismatch");
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial = {pixels.data(), width * sizeof(Pixel), 0};
        ComPtr<ID3D11Texture2D> src, dst, staging;
        Check(device->CreateTexture2D(&td, &initial, &src));
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        Check(device->CreateTexture2D(&td, nullptr, &dst));
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&td, nullptr, &staging));
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateShaderResourceView(src.Get(), nullptr, &srv));
        Check(device->CreateUnorderedAccessView(dst.Get(), nullptr, &uav));
        alignas(16) unsigned char constants[256];
        // Force the combined layout, then disable LUT. This also exercises Off/zero inside the HLSL itself.
        FillSharpenConstants(constants, DLSS_SHARPEN_RCAS, sharpness, width, height, 1, 1, linear, style);
        reinterpret_cast<float*>(constants)[1] = 0;
        reinterpret_cast<unsigned*>(constants)[4] = 0;
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(constants); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data = {constants, 0, 0};
        ComPtr<ID3D11Buffer> cb;
        Check(device->CreateBuffer(&bd, &data, &cb));
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        context->CSSetShaderResources(0, 1, srv.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        ID3D11UnorderedAccessView* empty = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
        context->CopyResource(staging.Get(), dst.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<Pixel> output(pixels.size());
        for (unsigned y = 0; y < height; ++y)
            memcpy(output.data() + y * width, static_cast<char*>(mapped.pData) + y * mapped.RowPitch, width * sizeof(Pixel));
        context->Unmap(staging.Get(), 0);
        context->ClearState();
        return output;
    }
};

int main(int argc, char** argv)
{
    try
    {
        StyleProbe probe;
        if (argc == 7)
        {
            unsigned w = unsigned(std::stoul(argv[2])), h = unsigned(std::stoul(argv[3]));
            Require(w > 0 && h > 0 && w <= 4096 && h <= 4096, "Preview dimensions out of bounds");
            std::vector<Pixel> input(size_t(w) * h);
            std::ifstream file(argv[1], std::ios::binary);
            Require(bool(file.read(reinterpret_cast<char*>(input.data()), input.size() * sizeof(Pixel))), "Cannot read input");
            SceneStyleParams style = {unsigned(std::stoul(argv[4])), 1.0f, unsigned(std::stoul(argv[5]))};
            Require(SceneStyleEnabled(style) && style.pixelSize >= 2 && style.pixelSize <= 16, "Invalid style");
            const auto output = probe.Run(input, w, h, style);
            std::ofstream result(argv[6], std::ios::binary);
            Require(bool(result.write(reinterpret_cast<const char*>(output.data()), output.size() * sizeof(Pixel))), "Cannot write output");
            return 0;
        }
        Require(argc == 1, "Usage: scene_style_probe [input.rgba32f width height mode pixelSize output.rgba32f]");
        Require(!SceneStyleEnabled({0, 1, 6}) && !SceneStyleEnabled({3, 1, 6})
            && !SceneStyleEnabled({1, 0, 6}) && !SceneStyleEnabled({1, NAN, 6}), "Style enable boundary failed");
        unsigned char before[256], after[256];
        for (int kind : {DLSS_SHARPEN_NIS, DLSS_SHARPEN_RCAS}) {
            FillSharpenConstants(before, kind, 0.4f, 37, 23);
            FillSharpenConstants(after, kind, 0.4f, 37, 23, 0, 0, false, {});
            Require(memcmp(before, after, 256) == 0, "Off changed original constant packing");
        }
        const unsigned w = 37, h = 23;
        std::vector<Pixel> input;
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x)
            input.push_back({x / float(w-1), y / float(h-1), ((x*13+y*7)%37)/36.f, (x+y)/float(w+h)});
        for (bool linear : {false, true}) for (unsigned mode : {1u, 2u})
        {
            auto full = probe.Run(input, w, h, {mode, 1, 6}, linear);
            auto half = probe.Run(input, w, h, {mode, 0.5f, 6}, linear);
            auto zero = probe.Run(input, w, h, {mode, 0, 6}, linear);
            auto off = probe.Run(input, w, h, {}, linear);
            unsigned changed = 0;
            for (size_t i = 0; i < input.size(); ++i) {
                Require(full[i][3] == input[i][3], "Alpha changed");
                for (int c = 0; c < 3; ++c) {
                    Require(std::isfinite(full[i][c]) && full[i][c] >= 0, "Invalid output");
                    Require(zero[i][c] == input[i][c] && off[i][c] == input[i][c], "Off/zero changed a pixel");
                    Require(std::abs(half[i][c] - (input[i][c] + full[i][c]) * 0.5f) < 2e-6f, "Blend endpoint mismatch");
                    if (std::abs(full[i][c] - input[i][c]) > 0.01f) ++changed;
                }
            }
            Require(changed > input.size(), "Style does not visibly change the fixture");
            auto tiny = probe.Run({Pixel{0.3f, 0.5f, 0.7f, 0.4f}}, 1, 1, {mode, 1, 16}, linear);
            Require(tiny[0][3] == 0.4f && std::isfinite(tiny[0][0]), "Tiny texture boundary failed");
        }
        for (unsigned block = 2; block <= 16; ++block)
        {
            auto pixel = probe.Run(input, w, h, {2, 1, block}, false, 1);
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
                const auto value = pixel[y*w+x][c];
                Require(value == pixel[(y/block*block)*w + x/block*block][c], "Pixel grid is not block-uniform");
                Require(std::abs(value*31-std::round(value*31)) < 4e-6f, "Pixel palette is not quantized");
                if (block == 2) {
                    const unsigned sx = (x / 2 * 2 + 1 < w) ? x / 2 * 2 + 1 : w - 1;
                    const unsigned sy = (y / 2 * 2 + 1 < h) ? y / 2 * 2 + 1 : h - 1;
                    Require(std::abs(value - input[sy*w+sx][c]) <= 0.5f / 31 + 1e-6f,
                        "Default pixel palette lost too much sampled color detail");
                }
            }
        }
        printf("PASS: production HLSL on D3D11 WARP; 2 styles x gamma/linear; exact Off/zero, alpha, finite output, blend, tiny/odd sizes; 15 pixel grids, 32-level palettes, default color error <=1/62; original Off packing.\n");
        return 0;
    }
    catch (const std::exception& e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
