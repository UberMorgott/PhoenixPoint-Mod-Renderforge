using System;
using System.Collections.Generic;
using System.Diagnostics;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Texture LOD bias under upscaling: Unity 2019.4 has no global mip bias, so every mipmapped Texture2D
    /// gets `mipMapBias = log2(renderW/outW)` (DLSS programming guide) while a reduced-res generation is live, and 0 again on Off.
    /// Vanilla assets ship with 0 (sampled + logged before the first sweep), so "restore" = write 0 back.
    /// UI pin (PixelPerfectUi): textures behind any Sprite get `min(bias, log2(canvasScale))` instead, so the minified
    /// UI (scale 0.667 at 1440p under a 4K reference) samples mip 0 - a runtime atlas without SpriteAtlas padding
    /// otherwise bleeds its neighbour through mip 1 as a 1 px edge line (docs\research\icon-bleed-2026-09-08.md).</summary>
    public static class MipBias
    {
        /// <summary>PPCLI switch: {"op":"invoke","type":"Renderforge.MipBias","assembly":"Renderforge","member":"SetEnabled","args":[false]}.</summary>
        public static bool Enabled = true;
        /// <summary>Set by PixelPerfectUi.Apply. Sprite_Create_Patch pins textures created after the last sweep.</summary>
        public static bool UiPin;
        /// <summary>Bias written to UI textures by the last sweep (min of the DLSS bias and the canvas-scale pin).</summary>
        public static float CurrentUiBias { get; private set; }
        private static float wanted, current;
        private static bool sampled;
        private static readonly string[] skipNames = { "lut", "noise", "dither", "ramp", "gradient" };

        /// <summary>Idempotent: sweeps only when the effective bias changes.</summary>
        public static void Apply(float bias)
        {
            wanted = bias;
            float eff = Enabled ? wanted : 0f;
            if (Mathf.Approximately(eff, current)) return;
            Sweep(eff);
        }

        /// <summary>Same bias again, for textures loaded after the last sweep (level start). No-op at 0 unless the UI pin is on.</summary>
        public static void Reapply() { if (current != 0f || UiPin) Sweep(current); }

        /// <summary>Unconditional sweep: the UI pin toggled while the DLSS bias did not.</summary>
        public static void Resweep() => Sweep(Enabled ? wanted : 0f);

        public static void Reset() => Apply(0f);

        public static string SetEnabled(bool on) { Enabled = on; Apply(wanted); return "mipbias enabled=" + Enabled + " current=" + current.ToString("F3"); }

        private static void Sweep(float bias)
        {
            var log = RenderforgeMod.Instance?.Logger;
            try
            {
                var sw = Stopwatch.StartNew();
                var all = Resources.FindObjectsOfTypeAll<Texture2D>();
                if (!sampled) { sampled = true; log?.LogInfo("MipBias: originals max|bias|=" + SampleMax(all).ToString("F3") + " over " + Math.Min(20, all.Length) + " sampled"); }
                var ui = UiTextures();
                float uiBias = UiPin ? Mathf.Min(0f, Mathf.Log(UiScale(), 2f)) : 0f;
                CurrentUiBias = Mathf.Min(bias, uiBias);
                int n = 0, nUi = 0, skipped = 0;
                foreach (var t in all)
                {
                    if (t == null || t.mipmapCount <= 1 || Skip(t.name)) { skipped++; continue; }
                    bool isUi = ui.Contains(t);
                    t.mipMapBias = isUi ? CurrentUiBias : bias; n++;
                    if (isUi) nUi++;
                }
                current = bias;
                log?.LogInfo("MipBias: bias=" + bias.ToString("F3") + " applied to " + n + " textures (ui=" + CurrentUiBias.ToString("F3") + " on " + nUi + " sprite textures, skipped " + skipped + ") in " + sw.ElapsedMilliseconds + " ms");
            }
            catch (Exception ex) { log?.LogError("MipBias sweep threw: " + ex.Message); }
        }

        private static HashSet<Texture2D> UiTextures()
        {
            var set = new HashSet<Texture2D>();
            foreach (var s in Resources.FindObjectsOfTypeAll<Sprite>())
            {
                if (s == null) continue;
                var t = s.texture;
                if (t != null) set.Add(t);
            }
            return set;
        }

        /// <summary>Smallest scaleFactor over root ScreenSpaceOverlay canvases = the game's own 4K-reference UI
        /// (a mod canvas with a 1080p reference scales >1 and pins nothing). 1 when none exists yet.</summary>
        private static float UiScale()
        {
            float min = float.MaxValue;
            foreach (var c in Resources.FindObjectsOfTypeAll<Canvas>())
            {
                if (c == null || !c.gameObject.scene.IsValid() || !c.isRootCanvas || c.renderMode != RenderMode.ScreenSpaceOverlay) continue;
                float s = c.scaleFactor;
                if (s > 0f && s < min) min = s;
            }
            return min == float.MaxValue ? 1f : min;
        }

        private static float SampleMax(Texture2D[] all)
        {
            float max = 0f; int seen = 0;
            foreach (var t in all)
            {
                if (t == null || t.mipmapCount <= 1) continue;
                max = Mathf.Max(max, Mathf.Abs(t.mipMapBias));
                if (++seen >= 20) break;
            }
            return max;
        }

        /// <summary>Shared with Sprite_Create_Patch so a late-created texture the sweep would skip is never pinned either
        /// (otherwise turning the pin off could not restore it).</summary>
        internal static bool Skip(string name)
        {
            foreach (var s in skipNames) if (name.IndexOf(s, StringComparison.OrdinalIgnoreCase) >= 0) return true;
            return false;
        }
    }
}
