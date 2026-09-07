// Grade.h - ReShade-style Exposure / Levels / Brightness / Contrast / Clarity / Vibrance / Saturation stages of the
// analytic post pass (1.5.0, extended 1.6.0). Values cross the managed/native ABI through Dlss_SetGrade already
// normalised; Dlss_SetGrade encodes them so that ZERO IS OFF for every field: an all-zero struct, a memset, a
// zero-filled constant block (the NIS/RCAS layouts, a probe that never packs the grade) are all the bit-exact bypass
// by construction.
#pragma once

struct GradeParams
{
    float black           = 0.0f;   // levels black point, fraction of full scale (slider 0..40 / 255); 0 = off
    float whiteDrop       = 0.0f;   // 1 - levels white point (slider (255 - s) / 255, 0..40/255); 0 = off
    float contrastDelta   = 0.0f;   // contrast gain - 1 about mid-grey (slider / 100 - 1, -0.5..0.5); 0 = off
    float clarity         = 0.0f;   // local-contrast strength 0..1 (slider / 100); 0 = off
    float exposure        = 0.0f;   // EV, linear-light stops (slider / 10, -4..4); 0 = off
    float brightness      = 0.0f;   // midtone gamma exponent = exp2(-b) (slider / 100, -1..1); 0 = off
    float saturationDelta = 0.0f;   // saturation gain - 1 (slider / 100 - 1, -1..1); 0 = off
    float vibrance        = 0.0f;   // saturation gain on the least-saturated colours (slider / 100, -1..1); 0 = off
};

inline bool GradeEnabled(const GradeParams& g)
{
    return g.black != 0.0f || g.whiteDrop != 0.0f || g.contrastDelta != 0.0f || g.clarity != 0.0f
        || g.exposure != 0.0f || g.brightness != 0.0f || g.saturationDelta != 0.0f || g.vibrance != 0.0f;
}

// Included after RF_SCENE_STYLE_HLSL (uses L(), StyleDisplay/StyleLinear/StyleLuma/StyleBlockCentre). Every stage
// branches on a per-dispatch uniform, `!= 0` throughout, so an Off knob is a bit-exact bypass and costs nothing.
// Display-referred maths on both colour-space paths (the FP16-linear path goes through the same pow(2.2) pair as
// Stylize); only negatives are clipped, a UNORM UAV clamps the top on store and FP16 keeps overbrights.
// Order: Exposure -> Levels -> Brightness -> Contrast -> Clarity -> Vibrance -> Saturation.
#define RF_GRADE_HLSL R"hlsl(
static const float2 kClarityTaps[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696,  0.457), float2(-0.203,  0.621),
    float2( 0.962, -0.195), float2( 0.473, -0.480), float2( 0.519,  0.767), float2( 0.185, -0.893),
    float2( 0.507,  0.064), float2( 0.896,  0.412), float2(-0.322, -0.933), float2(-0.792, -0.598) };
// Display-referred luma of the input at p: luma of the linear value, THEN one scalar gamma pow - not the luma of the
// per-channel gamma-encoded colour (StyleLuma(StyleSample(p)) = three pows). The two differ (pow is not linear), but
// the mask only needs a monotonic brightness estimate, and 13 scalar pows beat 39 per pixel.
float ClarityLuma(int2 p) { float y = StyleLuma(max(L(p), 0.0)); return styleLinear != 0 ? pow(y, 1.0 / 2.2) : y; }
float3 Adjust(int2 p, float3 c) {
    if (exposure == 0.0 && levelsBlack == 0.0 && whiteDrop == 0.0 && brightness == 0.0 && contrastDelta == 0.0
        && clarity == 0.0 && vibrance == 0.0 && saturationDelta == 0.0) return c;
    float3 d = StyleDisplay(c);
    // 1 EV = x2 in LINEAR light; on the display-referred value that is x exp2(EV / 2.2) (pow(c * 2^EV, 1/2.2)).
    // ClarityLuma below reads the UNEXPOSED source texture, so its mask is relative and anchored pre-exposure.
    if (exposure != 0.0) d *= exp2(exposure / 2.2);
    if (levelsBlack != 0.0 || whiteDrop != 0.0) d = max((d - levelsBlack) / max(1.0 - whiteDrop - levelsBlack, 1e-4), 0.0);
    // Midtone gamma, not an offset: black and white stay put, +1 lifts the mids (exponent 0.5), -1 sinks them (2).
    if (brightness != 0.0) d = pow(max(d, 0.0), exp2(-brightness));
    if (contrastDelta != 0.0) d = max((d - 0.5) * (1.0 + contrastDelta) + 0.5, 0.0);
    if (clarity != 0.0) {
        // Luma unsharp mask: the source luma at p against a 13-tap Poisson-disc mean of the SAME input texture
        // (radius 10 px at 1080p, scaled by output height), applied as a bounded multiplicative gain.
        // Under PixelArt the colour is one block-centre sample for the whole block, so the mask is evaluated at
        // that same centre (StyleBlockCentre): every pixel of a block gets one factor and blocks stay uniform.
        float r = 10.0 * float(H) / 1080.0;
        int2 s = styleMode == 2 && styleStrength > 0.0 ? StyleBlockCentre(p) : p;
        float y = ClarityLuma(s), blur = y;
        [unroll] for (int i = 0; i < 12; ++i) blur += ClarityLuma(s + int2(round(kClarityTaps[i] * r)));
        blur /= 13.0;
        d *= 1.0 + 0.6 * clarity * clamp((y - blur) / max(y, 1e-3), -1.0, 1.0);
    }
    if (vibrance != 0.0) {
        // Saturation gain weighted by (1 - current saturation): muted colours move most, already-vivid ones barely.
        // Luma here (and in Saturation below) is StyleLuma of the DISPLAY-encoded value - consistent with the rest of
        // the chain, not luminance-preserving in linear light.
        float mx = max(d.r, max(d.g, d.b)), sat = (mx - min(d.r, min(d.g, d.b))) / max(mx, 1e-4);
        d = max(lerp(StyleLuma(d).xxx, d, 1.0 + vibrance * (1.0 - sat)), 0.0);
    }
    if (saturationDelta != 0.0) d = max(lerp(StyleLuma(d).xxx, d, 1.0 + saturationDelta), 0.0);
    return StyleLinear(d);
}
)hlsl"
