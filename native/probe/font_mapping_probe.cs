using System;
using Renderforge;
using UnityEngine;
using UnityEngine.UI;

// Links the actual production files. The executable verifies the pure mapping/attribute gate;
// LiveApply/Atlas/Refresh/Release/Dispose let an isolated Unity probe exercise this same core.
public static class FontMappingProbe
{
    public static string LiveApply(Text text, VertexHelper mesh, bool enabled) => FontRasterCorrection.Apply(text, mesh, enabled);
    public static void Atlas(Font font) => FontRasterCorrection.AtlasChanged(font);
    public static void Refresh() => FontRasterCorrection.RefreshPending();
    public static void Release(Text text) => FontRasterCorrection.Release(text);
    public static void Dispose() => FontRasterCorrection.Dispose();
    public static string Result(int id) => FontRasterCorrection.Result(id);
    private static int checks;
    private static void Require(bool value, string message)
    { checks++; if (!value) throw new Exception(message); }

    private static UIVertex[] Quad(float w, float h)
    {
        var result = new UIVertex[4];
        float[] x = { 0, w, w, 0 }, y = { 0, 0, h, h };
        for (int i = 0; i < 4; i++) result[i] = new UIVertex {
            position = new Vector3(x[i], y[i], 0), normal = new Vector3(0, 0, -1),
            tangent = new Vector4(1, 0, 0, -1), color = new Color32(72, 120, 180, 240),
            uv0 = new Vector2(i % 2, i / 2), uv1 = new Vector2(.3f, .7f)
        };
        return result;
    }
    private static string Check(UIVertex[] original, UIVertex[] high, float ppu = 1) =>
        FontGlyphMapping.Check(original, 6, high, ppu, 1, 1, 1, 1, new[] { 0 }, new[] { 0 }, 20, 40);
    public static int Main()
    {
        try
        {
            // Independent geometric oracle: doubling raster coverage should leave the display quad untouched.
            for (int density = 1; density <= 3; density++)
                for (int w = 3; w <= 21; w += 3)
                    for (int h = 4; h <= 28; h += 4)
                    {
                        float ppu = density / 2f;
                        var a = Quad(w, h); var high = Quad(w * ppu * 2, h * ppu * 2);
                        for (int i = 0; i < 4; i++) high[i].uv0 = new Vector2(.1f + i * .11f, .2f + i * .12f);
                        Require(Check(a, high, ppu) == null, "matched glyph refused");
                        var saved = (UIVertex[])a.Clone(); FontGlyphMapping.Transfer(a, high);
                        for (int i = 0; i < a.Length; i++)
                        {
                            var expected = saved[i]; expected.uv0 = high[i].uv0;
                            Require(a[i].Equals(expected), "non-UV vertex attribute changed");
                        }
                    }
            var normal = Quad(10, 20); var doubled = Quad(20, 40);
            Require(Check(normal, Quad(40, 40)) == "glyph box mismatch", "changed box accepted");
            Require(Check(Quad(0, 20), doubled) == "glyph box mismatch", "degenerate mapping accepted");
            var color = (UIVertex[])doubled.Clone(); color[0].color = new Color32(1, 2, 3, 4);
            Require(Check(normal, color) == "rich-text color mapping changed", "reordered rich text accepted");
            var reversed = new[] { doubled[1], doubled[0], doubled[3], doubled[2] };
            Require(Check(normal, reversed) == "quad orientation changed", "reversed quad accepted");
            var badUv = (UIVertex[])doubled.Clone(); badUv[0].uv0 = new Vector2(float.NaN, 0);
            Require(Check(normal, badUv) == "invalid glyph UV", "NaN UV accepted");
            Require(Check(normal, doubled, float.NaN) == "invalid pixel density", "NaN density accepted");
            Require(FontGlyphMapping.Check(normal, 3, doubled, 1, 1, 1, 1, 1, new[] { 0 }, new[] { 0 }, 20, 40) == "unsupported topology", "nonquad topology accepted");
            Require(FontGlyphMapping.Check(normal, 6, doubled, 1, 1, 2, 1, 1, new[] { 0 }, new[] { 0 }, 20, 40) != null, "character count mismatch accepted");
            Require(FontGlyphMapping.Check(normal, 6, doubled, 1, 1, 1, 1, 1, new[] { 0 }, new[] { 1 }, 20, 40) != null, "line remapping accepted");
            Require(FontGlyphMapping.Check(normal, 6, doubled, 1, 1, 1, 1, 1, new[] { 0 }, new[] { 0 }, 20, 39) == "best-fit/hinted size changed", "best fit mismatch accepted");
            Console.WriteLine("PASS " + checks + " production mapping/attribute checks; no Unity rendering or atlas lifecycle simulated.");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
