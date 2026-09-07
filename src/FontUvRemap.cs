using System;

namespace Renderforge
{
    // Unity-free glyph UV remap shared by FontGlyphMapping and the test project.
    // A 2x-rastered glyph quad is not exactly twice the 1x quad (integer texel rounding of width and bearing),
    // so copying its uv0 rect verbatim onto the 1x quad stretches/shifts the glyph by up to ~0.5 px.
    // This samples the 2x atlas exactly where the 1x quad sits instead.
    internal static class FontUvRemap
    {
        // Max allowed |2x size - 2 * 1x size| and |2x min - 2 * 1x min| per axis, in 2x texels.
        internal const float Tolerance = 2f;

        // p1: 4 vertices x (x, y) of the 1x quad in 1x generator pixels; p2: the same quad from the 2x generator
        // in 2x pixels; uv2: its uv0 pairs. Writes 8 floats into outUv. False = keep the vanilla uv0 for this glyph.
        internal static bool Remap(float[] p1, float[] p2, float[] uv2, float[] outUv)
        {
            float minX1 = Min(p1, 0), minY1 = Min(p1, 1), w1 = Max(p1, 0) - minX1, h1 = Max(p1, 1) - minY1;
            float minX2 = Min(p2, 0), minY2 = Min(p2, 1), w2 = Max(p2, 0) - minX2, h2 = Max(p2, 1) - minY2;
            if (!(w2 > 0) || !(h2 > 0) || !(Math.Abs(w2 - 2 * w1) <= Tolerance) || !(Math.Abs(h2 - 2 * h1) <= Tolerance)
                || !(Math.Abs(minX2 - 2 * minX1) <= Tolerance) || !(Math.Abs(minY2 - 2 * minY1) <= Tolerance))
                return false;
            // Affine map position -> uv from three corners of the 2x quad; handles any vertex order and flipped/rotated atlas glyphs.
            int c00 = -1, c10 = -1, c01 = -1;
            float midX = minX2 + w2 / 2, midY = minY2 + h2 / 2;
            for (int i = 0; i < 4; i++)
            {
                bool right = p2[2 * i] > midX, top = p2[2 * i + 1] > midY;
                if (!right && !top) c00 = i; else if (right && !top) c10 = i; else if (!right && top) c01 = i;
            }
            if (c00 < 0 || c10 < 0 || c01 < 0) return false;
            for (int i = 0; i < 4; i++)
            {
                // ponytail: clamp keeps sampling inside the 2x glyph rect when it is narrower than 2x the 1x quad
                // (falls back to a <=1 texel stretch); drop the clamp if the dynamic atlas is proven to pad glyphs.
                float tx = Clamp01((2 * p1[2 * i] - minX2) / w2), ty = Clamp01((2 * p1[2 * i + 1] - minY2) / h2);
                if (float.IsNaN(tx) || float.IsNaN(ty)) return false;
                for (int a = 0; a < 2; a++)
                    outUv[2 * i + a] = uv2[2 * c00 + a] + tx * (uv2[2 * c10 + a] - uv2[2 * c00 + a]) + ty * (uv2[2 * c01 + a] - uv2[2 * c00 + a]);
            }
            return true;
        }

        private static float Clamp01(float t) => t < 0 ? 0 : t > 1 ? 1 : t;
        private static float Min(float[] p, int axis) => Math.Min(Math.Min(p[axis], p[2 + axis]), Math.Min(p[4 + axis], p[6 + axis]));
        private static float Max(float[] p, int axis) => Math.Max(Math.Max(p[axis], p[2 + axis]), Math.Max(p[4 + axis], p[6 + axis]));

#if DEBUG
        // Worked numbers: 1x quad x 10..17 (7 px), 2x quad x 20..35 (15 texels), u 0.20..0.35 -> the right edge samples
        // u = 0.20 + (34-20)/15 * 0.15 = 0.34 (14 texels for 7 px), not 0.35. Exact 2x (20..34) reduces to the verbatim copy.
        internal static void SelfTest()
        {
            float[] p1 = { 10, 0, 17, 0, 17, 9, 10, 9 }, uv = new float[8];
            float[] p2 = { 20, 0, 35, 0, 35, 18, 20, 18 }, uv2 = { .20f, .5f, .35f, .5f, .35f, .68f, .20f, .68f };
            Require(Remap(p1, p2, uv2, uv) && Near(uv[2], .34f) && Near(uv[3], .5f) && Near(uv[6], .20f) && Near(uv[7], .68f), "15 vs 7 remap");
            float[] exact = { 20, 0, 34, 0, 34, 18, 20, 18 };
            Require(Remap(p1, exact, uv2, uv) && Near(uv[2], .35f) && Near(uv[4], .35f) && Near(uv[5], .68f), "exact 2x is verbatim");
            float[] wide = { 20, 0, 37, 0, 37, 18, 20, 18 };
            Require(!Remap(p1, wide, uv2, uv), "3 texel mismatch must fall back");
        }
        private static bool Near(float a, float b) => Math.Abs(a - b) < 1e-4f;
        private static void Require(bool ok, string what) { if (!ok) throw new InvalidOperationException("FontUvRemap self-test failed: " + what); }
#endif
    }
}
