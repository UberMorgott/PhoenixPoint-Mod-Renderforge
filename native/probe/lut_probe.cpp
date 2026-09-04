// Exercises the production grade shader without Unity, NGX, a window, or a LUT asset.
#include "Sharpen.h"
#include "RenderforgeNative.h"
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
static void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }

struct GradeProbe {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    GradeProbe() {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, &device, nullptr, &context));
        int kind = 0;
        ComPtr<ID3DBlob> blob; blob.Attach(CompileSharpenBlob(&kind, false, true));
        Require(blob.Get() != nullptr, "Production grade HLSL did not compile");
        Check(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader));
    }
    std::vector<Pixel> Run(const std::vector<Pixel>& pixels, unsigned width, unsigned height, int preset, float strength) {
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
        // Pack a valid grade first, then exercise the shader's strength-zero endpoint explicitly.
        FillSharpenConstants(constants, DLSS_SHARPEN_RCAS, 0, width, height, preset, 1);
        reinterpret_cast<float*>(constants)[1] = strength;
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

int main(int argc, char** argv) {
    try {
        GradeProbe probe;
        if (argc == 6) {
            unsigned w = unsigned(std::stoul(argv[2])), h = unsigned(std::stoul(argv[3]));
            Require(w > 0 && h > 0 && w <= 4096 && h <= 4096, "Preview dimensions out of bounds");
            std::vector<Pixel> input(size_t(w) * h);
            std::ifstream file(argv[1], std::ios::binary);
            Require(bool(file.read(reinterpret_cast<char*>(input.data()), input.size() * sizeof(Pixel))), "Cannot read preview input");
            auto output = probe.Run(input, w, h, std::stoi(argv[4]), 1);
            std::ofstream result(argv[5], std::ios::binary);
            Require(bool(result.write(reinterpret_cast<const char*>(output.data()), output.size() * sizeof(Pixel))), "Cannot write preview output");
            return 0;
        }
        Require(argc == 1, "Usage: lut_probe [input.rgba32f width height preset output.rgba32f]");
        std::vector<Pixel> cube;
        for (int b = 0; b < 17; ++b) for (int g = 0; g < 17; ++g) for (int r = 0; r < 17; ++r)
            cube.push_back({r / 16.f, g / 16.f, b / 16.f, (r + g + b) / 48.f});
        std::vector<Pixel> ramp;
        for (int i = 0; i <= 1024; ++i) ramp.push_back({i / 1024.f, i / 1024.f, i / 1024.f, 0.37f});
        Require(!ColorGradeEnabled(0, 1) && !ColorGradeEnabled(10, 1), "Invalid preset accepted");
        for (int preset = 1; preset <= 9; ++preset) {
            Require(ColorGradeEnabled(preset, 1) && !ColorGradeEnabled(preset, 0), "Grade enable boundary failed");
            const auto full = probe.Run(cube, 17, 289, preset, 1);
            const auto half = probe.Run(cube, 17, 289, preset, 0.5f);
            const auto zero = probe.Run(cube, 17, 289, preset, 0);
            for (size_t i = 0; i < cube.size(); ++i) {
                Require(full[i][3] == cube[i][3], "Alpha changed");
                for (int c = 0; c < 3; ++c) {
                    Require(std::isfinite(full[i][c]) && full[i][c] >= 0 && full[i][c] <= 2, "Nonfinite/out-of-range grade");
                    Require(std::abs(zero[i][c] - cube[i][c]) < 1e-6f, "Zero strength changed input");
                    Require(std::abs(half[i][c] - (cube[i][c] + full[i][c]) * 0.5f) < 1e-6f, "Blend not linear");
                }
                if (preset == 5 || preset == 6) Require(std::abs(full[i][0] - full[i][1]) < 1e-6f && std::abs(full[i][1] - full[i][2]) < 1e-6f, "B&W chroma leakage");
            }
            auto gradedRamp = probe.Run(ramp, 1025, 1, preset, 1);
            for (size_t i = 1; i < ramp.size(); ++i) for (int c = 0; c < 3; ++c)
                Require(gradedRamp[i][c] + 1e-6f >= gradedRamp[i-1][c], "Neutral ramp inversion");
            auto bright = probe.Run({Pixel{4, 4, 4, 1}}, 1, 1, preset, 1);
            Require(bright[0][0] > 1 && bright[0][1] > 1 && bright[0][2] > 1, "FP overbright clipped");
        }
        printf("PASS: production HLSL on D3D11 WARP; 9 presets; 4913-color cube; alpha, finite range, blend endpoints, B&W equality, 1025-step monotonic ramps, FP overbrights.\n");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
