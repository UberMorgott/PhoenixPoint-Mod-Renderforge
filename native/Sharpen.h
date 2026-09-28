// Sharpen.h - the post-DLSS sharpen compute shader, shared by the D3D11 and D3D12 backends.
// NIS sharpen-only, or the analytic RCAS + grade pass whenever a grade/style/colour-vision stage is active.
#pragma once

#include <d3dcommon.h>
#include <dxgiformat.h>
#include "SceneStyle.h"
#include "ColorVision.h"
#include "Grade.h"

// The post shader's DXBC, compiled at BUILD time (SharpenHlsl.h -> rf_shadergen -> sharpen_dxbc.h); nothing is
// compiled at runtime. colorGrade false = NIS sharpen-only (DLSS_SHARPEN_NIS); true = one analytic RCAS + color-grade
// pass (DLSS_SHARPEN_RCAS, written to *outKind). hdr: the output is linear FP16 (D3D12HalfColor) -> the
// NIS_HDR_MODE_LINEAR variant; the grade variant takes hdr from the cbuffer instead. Static storage, never NULL.
const void* SharpenBytecode(bool hdr, bool colorGrade, size_t* size, int* outKind);
// The same bytecode copied into a fresh ID3DBlob (caller Release()s it) - the probes' interface. NULL on OOM.
ID3DBlob* CompileSharpenBlob(int* outKind, bool hdr = false, bool colorGrade = false);

// Fills a 256-byte constant block for the compiled shader `kind`. w/h = output texture size.
// sharpness is 0..1; zero still runs when a color grade is active. hdr must match the compiled blob.
void FillSharpenConstants(void* dst256, int kind, float sharpness, unsigned w, unsigned h,
                          int lutPreset = 0, float lutStrength = 0.0f, bool hdr = false,
                          const SceneStyleParams& style = SceneStyleParams{}, int colorVision = 0,
                          const GradeParams& grade = GradeParams{});

inline bool ColorGradeEnabled(int preset, float strength) { return preset >= 1 && preset <= 9 && strength > 0.0f; }
inline bool ColorVisionEnabled(int mode) { return mode >= RF_CV_DEUTERANOPIA && mode <= RF_CV_TRITANOPIA; }

// The analytic post shader (RCAS + grade + scene style + levels/contrast/clarity + colour vision) is compiled
// INSTEAD of NIS/RCAS whenever any of its stages is active. Colour vision and the grade knobs live only in that
// shader, so every "is the post pass needed" test must go through here - otherwise LUT=Off + style=Off silently
// bypasses them. No default on `grade`: a call site that forgets it must fail to compile, not skip the stage.
inline bool PostShaderEnabled(int preset, float strength, const SceneStyleParams& style, int colorVision,
                              const GradeParams& grade)
{
    return ColorGradeEnabled(preset, strength) || SceneStyleEnabled(style) || ColorVisionEnabled(colorVision)
        || GradeEnabled(grade);
}

// Typeless render-target formats have no valid SRV/UAV format; map them to the concrete one.
DXGI_FORMAT SharpenViewFormat(DXGI_FORMAT fmt);

// FP16 output = linear values (D3D12HalfColor): the sharpen shader needs its HDR-linear variant.
inline bool SharpenIsHdr(DXGI_FORMAT fmt) { return fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS || fmt == DXGI_FORMAT_R16G16B16A16_FLOAT; }

// Thread-group footprint of the compiled shader: NIS is 32x32 per group, RCAS is 8x8.
inline unsigned SharpenGroupSize(int kind) { return kind == 1 /* DLSS_SHARPEN_NIS */ ? 32u : 8u; }
