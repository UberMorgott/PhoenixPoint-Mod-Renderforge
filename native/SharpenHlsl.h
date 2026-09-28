// SharpenHlsl.h - HLSL sources of the post pass. Compiled at BUILD time by shadergen/rf_shadergen.cpp into
// sharpen_dxbc.h (DXBC cs_5_0, the same D3DCompile call the shim used to make at runtime), so the render thread
// never compiles a shader: toggling LUT/style/colour vision/grade only switches between resident variants.
#pragma once

#include "SceneStyle.h"
#include "Grade.h"

// Analytic color grading is deliberately code-only: no third-party LUT data or textures. The preset coefficients
// are original transforms in display-referred RGB. FP16 overbrights are preserved (only negatives
// are clipped); UNORM UAVs clamp naturally on store. RCAS (AMD FidelityFX FsrRcasF, MIT, written from the public
// formula, epsilon-guarded denominators, FSR_RCAS_DENOISE on) is folded into this shader so LUT+sharpen stays one pass.
static const char kColorGradeHlsl[] =
"Texture2D<float4> src : register(t0);\n"
"RWTexture2D<float4> dst : register(u0);\n"
"cbuffer C : register(b0) { float sharpness; float strength; uint W; uint H; uint preset; float con; uint styleMode; uint pixelSize; float styleStrength; uint styleLinear; uint colorVision; float pad0; float4 cvRow0; float4 cvRow1; float4 cvRow2; float levelsBlack; float whiteDrop; float contrastDelta; float clarity; float exposure; float brightness; float saturationDelta; float vibrance; };\n"
"float3 L(int2 p) { p=clamp(p,int2(0,0),int2(int(W)-1,int(H)-1)); return src.Load(int3(p,0)).rgb; }\n"
RF_SCENE_STYLE_HLSL
RF_GRADE_HLSL
"float3 Grade(float3 c) {\n"
"  float y=dot(c,float3(0.2126,0.7152,0.0722)); float3 g=c;\n"
"  if (preset==1) { g=lerp(y.xxx,c,0.72); g=(g-0.5)*0.96+0.5; g*=float3(0.98,1.0,1.02); }\n"
"  else if (preset==2) { g=lerp(y.xxx,c,0.96); g=(g-0.5)*1.03+0.5; g*=float3(1.01,1.0,0.99); }\n"
"  else if (preset==3) { g=lerp(y.xxx,c,0.42); g=(g-0.5)*1.18+0.5; g*=float3(0.98,1.0,1.04); g+=float3(0.015,0.005,-0.005); }\n"
"  else if (preset==4) { g=lerp(y.xxx,c,1.28); g=(g-0.5)*1.08+0.5; g*=float3(1.03,1.0,0.98); }\n"
// B&W curves keep black/white endpoints and a monotonic toe/shoulder; the saturated coordinate only
// shapes contrast within 0..1, leaving FP16 overbrights intact. Noir uses a red-filter luma mix.
"  else if (preset==5) { float z=saturate(y); float film=y+0.28*z*(1.0-z)*(2.0*z-1.0); g=film.xxx; }\n"
"  else if (preset==6) { float n=dot(c,float3(0.32,0.60,0.08)); float z=saturate(n); float film=n+0.88*z*(1.0-z)*(2.0*z-1.0); g=film.xxx; }\n"
"  else if (preset==7) { float z=saturate(y); g=lerp(y.xxx,c,0.82); g+=z*(1.0-z)*float3(0.22,0.025,-0.18); }\n"
"  else if (preset==8) { float z=saturate(y); g=lerp(y.xxx,c,0.68); g+=z*(1.0-z)*float3(-0.20,0.035,0.24); }\n"
"  else if (preset==9) { float z=saturate(y); float film=0.035+0.93*y; g=film.xxx+z*(1.0-z)*float3(0.24,0.025,-0.26); }\n"
"  return lerp(c,max(g,0.0),strength);\n"
"}\n"
// Daltonization, AFTER Grade(), Stylize() and Adjust() so it corrects whatever the player actually sees. The matrix is
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
"[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {\n"
"  if(id.x>=W||id.y>=H)return; int2 p=int2(id.xy); float3 e=L(p), c=e;\n"
"  if(sharpness>0.0){ float3 b=L(p+int2(0,-1)),d=L(p+int2(-1,0)),f=L(p+int2(1,0)),h=L(p+int2(0,1));\n"
"    float bL=b.b*.5+(b.r*.5+b.g),dL=d.b*.5+(d.r*.5+d.g),eL=e.b*.5+(e.r*.5+e.g);\n"
"    float fL=f.b*.5+(f.r*.5+f.g),hL=h.b*.5+(h.r*.5+h.g); float mx=max(max(max(bL,dL),max(eL,fL)),hL),mn=min(min(min(bL,dL),min(eL,fL)),hL);\n"
"    float nz=1.0-0.5*saturate(abs(.25*(bL+dL+fL+hL)-eL)/max(mx-mn,1e-5));\n"
"    float3 mn4=min(min(b,d),min(f,h)),mx4=max(max(b,d),max(f,h));\n"
"    float3 hitMin=mn4/max(4.0*mx4,1e-5),hitMax=(1.0-mx4)/min(4.0*mn4-4.0,-1e-5);\n"
"    float3 lr=max(-hitMin,hitMax); float l=max(-0.1875,min(max(max(lr.r,lr.g),lr.b),0.0))*con*nz; c=(l*(b+d+f+h)+e)/(4.0*l+1.0); }\n"
"  dst[id.xy]=float4(ColorVision(Adjust(p,Grade(Stylize(p,c)))),src.Load(int3(p,0)).a); }\n";

// NIS sharpen-only: the NIS_Main.hlsl example's bindings + NVSharpen entry. Block/group sizes = NISOptimizer(isUpscaling=false,
// NVIDIA_Generic) in NIS_Config.h (32 x 32, 128 threads). NIS_HDR_MODE 0: the DLSS output is display-referred LDR;
// NIS_HDR_MODE 1 (linear) when the output is linear FP16 (D3D12HalfColor) - the generator prepends the define.
// Full source = mode define + kNisPreamble + NIS_Scaler.h (nis/, NVIDIA Image Scaling SDK 1.0.3, MIT) + kNisMain.
static const char kNisPreamble[] =
"#define NIS_HLSL 1\n#define NIS_SCALER 0\n"
"#define NIS_BLOCK_WIDTH 32\n#define NIS_BLOCK_HEIGHT 32\n#define NIS_THREAD_GROUP_SIZE 128\n"
"cbuffer cb : register(b0) {\n"
" float kDetectRatio; float kDetectThres; float kMinContrastRatio; float kRatioNorm;\n"
" float kContrastBoost; float kEps; float kSharpStartY; float kSharpScaleY;\n"
" float kSharpStrengthMin; float kSharpStrengthScale; float kSharpLimitMin; float kSharpLimitScale;\n"
" float kScaleX; float kScaleY; float kDstNormX; float kDstNormY; float kSrcNormX; float kSrcNormY;\n"
" uint kInputViewportOriginX; uint kInputViewportOriginY; uint kInputViewportWidth; uint kInputViewportHeight;\n"
" uint kOutputViewportOriginX; uint kOutputViewportOriginY; uint kOutputViewportWidth; uint kOutputViewportHeight;\n"
" float reserved0; float reserved1; };\n"
"SamplerState samplerLinearClamp : register(s0);\n"
"Texture2D in_texture : register(t0);\n"
"RWTexture2D<float4> out_texture : register(u0);\n";
static const char kNisMain[] =
"\n[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]\n"
"void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID) { NVSharpen(blockIdx.xy, threadIdx.x); }\n";
