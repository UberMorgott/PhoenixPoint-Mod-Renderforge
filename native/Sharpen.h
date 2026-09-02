// Sharpen.h - the post-DLSS sharpen compute shader, shared by the D3D11 and D3D12 backends.
// NIS sharpen-only is preferred; AMD RCAS is the fallback when NIS fails to compile.
#pragma once

#include <d3dcommon.h>
#include <dxgiformat.h>

// Compiles the sharpen shader. Returns the DXBC blob (caller Release()s it) and writes
// DLSS_SHARPEN_NIS or DLSS_SHARPEN_RCAS to *outKind. Returns NULL on failure (*outKind untouched).
ID3DBlob* CompileSharpenBlob(int* outKind);

// Fills a 256-byte constant block for the compiled shader `kind`. w/h = output texture size.
// sharpness is 0..1 (0 means the caller should skip the pass entirely).
void FillSharpenConstants(void* dst256, int kind, float sharpness, unsigned w, unsigned h);

// Typeless render-target formats have no valid SRV/UAV format; map them to the concrete one.
DXGI_FORMAT SharpenViewFormat(DXGI_FORMAT fmt);

// Thread-group footprint of the compiled shader: NIS is 32x32 per group, RCAS is 8x8.
inline unsigned SharpenGroupSize(int kind) { return kind == 1 /* DLSS_SHARPEN_NIS */ ? 32u : 8u; }
