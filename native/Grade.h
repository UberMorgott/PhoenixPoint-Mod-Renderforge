// Grade.h - ReShade-style Levels / Contrast / Clarity stages of the analytic post pass (1.5.0). Values cross the
// managed/native ABI through Dlss_SetGrade already normalised; the defaults below are the bit-exact bypass.
#pragma once

struct GradeParams
{
    float black    = 0.0f;   // levels black point, fraction of full scale (slider 0..40 / 255); 0 = off
    float white    = 1.0f;   // levels white point, fraction of full scale (slider 215..255 / 255); 1 = off
    float contrast = 1.0f;   // gain about mid-grey, 0.5..1.5 (slider 50..150 / 100); 1 = off
    float clarity  = 0.0f;   // local-contrast strength 0..1 (slider / 100); 0 = off
};

inline bool GradeEnabled(const GradeParams& g)
{
    return g.black > 0.0f || g.white < 1.0f || g.contrast != 1.0f || g.clarity > 0.0f;
}

// Included after RF_SCENE_STYLE_HLSL (uses L(), StyleDisplay/StyleLinear/StyleLuma/StyleSample). Every stage
// branches on a per-dispatch uniform, so an Off knob is a bit-exact bypass and costs nothing. Display-referred
// maths on both colour-space paths (the FP16-linear path goes through the same pow(2.2) pair as Stylize); only
// negatives are clipped, a UNORM UAV clamps the top on store and FP16 keeps overbrights.
#define RF_GRADE_HLSL R"hlsl(
static const float2 kClarityTaps[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696,  0.457), float2(-0.203,  0.621),
    float2( 0.962, -0.195), float2( 0.473, -0.480), float2( 0.519,  0.767), float2( 0.185, -0.893),
    float2( 0.507,  0.064), float2( 0.896,  0.412), float2(-0.322, -0.933), float2(-0.792, -0.598) };
float3 Adjust(int2 p, float3 c) {
    if (levelsBlack <= 0.0 && levelsWhite >= 1.0 && contrastK == 1.0 && clarity <= 0.0) return c;
    float3 d = StyleDisplay(c);
    if (levelsBlack > 0.0 || levelsWhite < 1.0) d = max((d - levelsBlack) / max(levelsWhite - levelsBlack, 1e-4), 0.0);
    if (contrastK != 1.0) d = max((d - 0.5) * contrastK + 0.5, 0.0);
    if (clarity > 0.0) {
        // Luma unsharp mask: the source luma at p against a 13-tap Poisson-disc mean of the SAME input texture
        // (radius 10 px at 1080p, scaled by output height), applied as a bounded multiplicative gain.
        float r = 10.0 * float(H) / 1080.0;
        float y = StyleLuma(StyleSample(p)), blur = y;
        [unroll] for (int i = 0; i < 12; ++i) blur += StyleLuma(StyleSample(p + int2(round(kClarityTaps[i] * r))));
        blur /= 13.0;
        d *= 1.0 + 0.6 * clarity * clamp((y - blur) / max(y, 1e-3), -1.0, 1.0);
    }
    return StyleLinear(d);
}
)hlsl"
