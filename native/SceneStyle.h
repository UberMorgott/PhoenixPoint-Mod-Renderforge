// Code-only scene stylization. Ordinals cross the managed/native ABI; append only.
#pragma once

struct SceneStyleParams
{
    unsigned mode;       // 0 = Off, 1 = Cartoon, 2 = PixelArt
    float strength;      // 0..1 blend; zero bypasses the style
    unsigned pixelSize;  // output pixels per block, 2..16
};

inline bool SceneStyleEnabled(const SceneStyleParams& style)
{
    return style.mode >= 1 && style.mode <= 2 && style.strength > 0.0f;
}

// Included as a string fragment after the shared shader's resources, cbuffer and L() helper.
// Edges are image contrast, not depth/normal silhouettes: this is a cartoon post filter, not material cel shading.
#define RF_SCENE_STYLE_HLSL R"hlsl(
float3 StyleDisplay(float3 c) { return styleLinear != 0 ? pow(max(c, 0.0), 1.0 / 2.2) : max(c, 0.0); }
float3 StyleLinear(float3 c) { return styleLinear != 0 ? pow(max(c, 0.0), 2.2) : c; }
float StyleLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 StyleSample(int2 p) { return StyleDisplay(L(p)); }
float3 Stylize(int2 p, float3 original) {
    if (styleMode == 0 || styleStrength <= 0.0) return original;
    float3 c;
    if (styleMode == 2) {
        int block = int(max(pixelSize, 2u));
        int2 q = (p / block) * block + block / 2;
        c = floor(StyleSample(q) * 7.0 + 0.5) / 7.0;
    } else {
        // A small cross smooths fine texture before banding. Ink uses the original local contrast.
        int radius = max(1, int(H / 720));
        float3 e = StyleSample(p);
        float3 b = StyleSample(p + int2(0, -radius)), d = StyleSample(p + int2(-radius, 0));
        float3 f = StyleSample(p + int2(radius, 0)), h = StyleSample(p + int2(0, radius));
        c = (e * 4.0 + b + d + f + h) / 8.0;
        float y = StyleLuma(c);
        float band = (floor(saturate(y) * 5.0) + 0.5) / 5.0;
        c *= lerp(1.0, band / max(y, 0.04), 0.75);
        float edge = max(abs(StyleLuma(b) - StyleLuma(h)), abs(StyleLuma(d) - StyleLuma(f)));
        c *= 1.0 - 0.85 * smoothstep(0.08, 0.24, edge);
    }
    float3 styled = StyleLinear(c);
    // Avoid cancellation against a different original pixel at full strength: every pixel in a block must match.
    return styleStrength >= 1.0 ? styled : lerp(original, styled, styleStrength);
}
)hlsl"
