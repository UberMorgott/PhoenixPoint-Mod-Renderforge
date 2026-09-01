using System;
using System.Diagnostics;
using UnityEngine;

namespace DlssMod
{
    /// <summary>Texture LOD bias under upscaling: Unity 2019.4 has no global mip bias, so every mipmapped Texture2D
    /// gets `mipMapBias = log2(renderW/outW)` (DLSS programming guide) while a reduced-res generation is live, and 0 again on Off.
    /// Vanilla assets ship with 0 (sampled + logged before the first sweep), so "restore" = write 0 back.</summary>
    public static class MipBias
    {
        /// <summary>PPCLI switch: {"op":"invoke","type":"DlssMod.MipBias","assembly":"DLSS","member":"SetEnabled","args":[false]}.</summary>
        public static bool Enabled = true;
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

        /// <summary>Same bias again, for textures loaded after the last sweep (level start). No-op at 0.</summary>
        public static void Reapply() { if (current != 0f) Sweep(current); }

        public static void Reset() => Apply(0f);

        public static string SetEnabled(bool on) { Enabled = on; Apply(wanted); return "mipbias enabled=" + Enabled + " current=" + current.ToString("F3"); }

        private static void Sweep(float bias)
        {
            var log = DlssMod.Instance?.Logger;
            try
            {
                var sw = Stopwatch.StartNew();
                var all = Resources.FindObjectsOfTypeAll<Texture2D>();
                if (!sampled) { sampled = true; log?.LogInfo("MipBias: originals max|bias|=" + SampleMax(all).ToString("F3") + " over " + Math.Min(20, all.Length) + " sampled"); }
                int n = 0, skipped = 0;
                foreach (var t in all)
                {
                    if (t == null || t.mipmapCount <= 1 || Skip(t.name)) { skipped++; continue; }
                    t.mipMapBias = bias; n++;
                }
                current = bias;
                log?.LogInfo("MipBias: bias=" + bias.ToString("F3") + " applied to " + n + " textures (skipped " + skipped + ") in " + sw.ElapsedMilliseconds + " ms");
            }
            catch (Exception ex) { log?.LogError("MipBias sweep threw: " + ex.Message); }
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

        private static bool Skip(string name)
        {
            foreach (var s in skipNames) if (name.IndexOf(s, StringComparison.OrdinalIgnoreCase) >= 0) return true;
            return false;
        }
    }
}
