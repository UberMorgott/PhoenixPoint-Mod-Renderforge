using System;
using System.Collections.Generic;
using UnityEngine;

namespace Renderforge
{
    // Pure mapping gate shared by the production correction and the source-linked proof harness.
    internal static class FontGlyphMapping
    {
        internal static string Check(UIVertex[] original, int indexCount, IList<UIVertex> high, float ppu,
            int normalCharacters, int highCharacters, int normalVisible, int highVisible,
            int[] normalLines, int[] highLines, int normalSize, int highSize)
        {
            if (original.Length == 0 || original.Length % 4 != 0 || indexCount != original.Length / 4 * 6)
                return "unsupported topology";
            if (!(ppu > 0) || float.IsInfinity(ppu)) return "invalid pixel density";
            if (original.Length != high.Count || normalCharacters != highCharacters || normalVisible != highVisible
                || normalLines.Length != highLines.Length) return "character/line/quad mapping changed";
            for (int i = 0; i < normalLines.Length; i++)
                if (normalLines[i] != highLines[i]) return "character/line/quad mapping changed";
            if (normalSize <= 0 || (long)normalSize * 2 != highSize) return "best-fit/hinted size changed";
            for (int i = 0; i < original.Length; i += 4)
            {
                float bw = Extent(original, i, true) * ppu, bh = Extent(original, i, false) * ppu;
                float hw = Extent(high, i, true) / 2, hh = Extent(high, i, false) / 2;
                if (!SameExtent(bw, hw) || !SameExtent(bh, hh)) return "glyph box mismatch";
                for (int j = 0; j < 4; j++)
                {
                    int k = i + j, next = i + (j + 1) % 4;
                    if (!original[k].color.Equals(high[k].color)) return "rich-text color mapping changed";
                    Vector3 a = original[next].position - original[k].position;
                    Vector3 b = high[next].position - high[k].position;
                    if (!Finite(a.x) || !Finite(a.y) || !Finite(b.x) || !Finite(b.y)
                        || Math.Sign(a.x) != Math.Sign(b.x) || Math.Sign(a.y) != Math.Sign(b.y))
                        return "quad orientation changed";
                    if (!Finite(high[k].uv0.x) || !Finite(high[k].uv0.y)) return "invalid glyph UV";
                }
            }
            return null;
        }

        internal static void Transfer(UIVertex[] original, IList<UIVertex> high)
        {
            for (int i = 0; i < original.Length; i++) original[i].uv0 = high[i].uv0;
        }

        // Per glyph: sample the 2x atlas exactly where the 1x quad sits (FontUvRemap); a glyph the remap refuses keeps
        // its vanilla uv0. `normal` = the 1x generator's vertices (generator pixel space, before Text's unit/rounding pass).
        internal static void Transfer(UIVertex[] original, IList<UIVertex> normal, IList<UIVertex> high)
        {
            if (normal == null || normal.Count != original.Length) { Transfer(original, high); return; }
            float[] p1 = new float[8], p2 = new float[8], uv2 = new float[8], uv = new float[8];
            for (int i = 0; i < original.Length; i += 4)
            {
                for (int j = 0; j < 4; j++)
                {
                    Vector3 a = normal[i + j].position, b = high[i + j].position; Vector2 t = high[i + j].uv0;
                    p1[2 * j] = a.x; p1[2 * j + 1] = a.y; p2[2 * j] = b.x; p2[2 * j + 1] = b.y; uv2[2 * j] = t.x; uv2[2 * j + 1] = t.y;
                }
                if (!FontUvRemap.Remap(p1, p2, uv2, uv)) continue;
                for (int j = 0; j < 4; j++) original[i + j].uv0 = new Vector2(uv[2 * j], uv[2 * j + 1]);
            }
        }

        private static bool Finite(float x) => !float.IsNaN(x) && !float.IsInfinity(x);
        private static bool SameExtent(float a, float b) => Finite(a) && Finite(b)
            && (a < .001f) == (b < .001f) && Math.Abs(a - b) <= 2
            && (a <= 2 || (b / a >= .75f && b / a <= 1.25f));
        private static float Extent(IList<UIVertex> vertices, int offset, bool x)
        {
            float min = float.PositiveInfinity, max = float.NegativeInfinity;
            for (int j = offset; j < offset + 4; j++)
            {
                float value = x ? vertices[j].position.x : vertices[j].position.y;
                if (!Finite(value)) return float.NaN;
                min = Math.Min(min, value); max = Math.Max(max, value);
            }
            return max - min;
        }
    }
}
