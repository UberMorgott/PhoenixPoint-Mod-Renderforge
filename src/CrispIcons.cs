using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Trilinear + anisotropic sampling for UI sprites drawn smaller than their source. The UI canvases scale
    /// with a 3840x2160 reference (scaleFactor 0.6667 at 1440p), so every sprite is minified and the main atlas
    /// (13 mips, filterMode Bilinear) snaps between mip levels. Textures without mips are left alone - creating
    /// mipmapped copies needs readable textures, and Image cannot draw an RT-backed sprite.
    /// Eligibility mirrors CrispFonts.Supported: root ScreenSpaceOverlay canvas only. The write is per texture, so a
    /// shared atlas also changes on world/camera canvases that use it - sampler state, not pixels.</summary>
    internal static class CrispIcons
    {
        private struct Original { public FilterMode Filter; public int Aniso; }
        private const int SweepEveryFrames = 180;   // ponytail: ~3 s at 60 fps; a panel opened in between is caught by the next sweep
        private static readonly Dictionary<Texture, Original> originals = new Dictionary<Texture, Original>();
        private static readonly HashSet<Texture> noMips = new HashSet<Texture>();
        private static bool active, logged;
        private static int lastSweepFrame = -1;
        internal static bool Active => active;

        internal static bool Supported(Graphic graphic) => graphic && graphic.isActiveAndEnabled && graphic.canvas
            && graphic.canvas.rootCanvas && graphic.canvas.rootCanvas.renderMode == RenderMode.ScreenSpaceOverlay;

        internal static void Apply(bool enabled)
        {
            if (enabled == active) return;
            active = enabled;
            if (enabled) { Canvas.willRenderCanvases += Tick; logged = false; Sweep(); }
            else { Canvas.willRenderCanvases -= Tick; Restore(); }
        }

        internal static void Dispose() => Apply(false);

        /// <summary>OnLevelStart + the throttled Tick. Active Image/RawImage only - inactive panels are swept when shown.</summary>
        internal static void Sweep()
        {
            if (!active) return;
            lastSweepFrame = Time.frameCount;
            try
            {
                foreach (Image image in UnityEngine.Object.FindObjectsOfType<Image>()) Visit(image);
                foreach (RawImage raw in UnityEngine.Object.FindObjectsOfType<RawImage>()) Visit(raw);
            }
            catch (Exception ex) { RenderforgeMod.Instance?.Logger.LogWarning("Crisp icons sweep failed: " + ex.Message); }
            if (!logged && originals.Count > 0)
            {
                logged = true;
                RenderforgeMod.Instance?.Logger.LogInfo("crisp icons: " + originals.Count + " textures trilinear (" + noMips.Count + " skipped: no mips)");
            }
        }

        private static void Tick()
        {
            if (Time.frameCount - lastSweepFrame >= SweepEveryFrames) Sweep();
        }

        private static void Visit(Graphic graphic)
        {
            if (!Supported(graphic)) return;
            Texture texture = graphic.mainTexture;
            if (texture == null || originals.ContainsKey(texture) || noMips.Contains(texture)) return;
            if (texture.mipmapCount <= 1) { noMips.Add(texture); return; }
            originals[texture] = new Original { Filter = texture.filterMode, Aniso = texture.anisoLevel };
            texture.filterMode = FilterMode.Trilinear;
            texture.anisoLevel = 8;
        }

        /// <summary>Originals back on every recorded texture that is still alive; no-op when nothing was recorded.</summary>
        private static void Restore()
        {
            foreach (KeyValuePair<Texture, Original> pair in originals)
            {
                if (!pair.Key) continue;
                pair.Key.filterMode = pair.Value.Filter;
                pair.Key.anisoLevel = pair.Value.Aniso;
            }
            originals.Clear();
            noMips.Clear();
        }

        /// <summary>PPCLI readback: {"op":"invoke","type":"Renderforge.CrispIcons","assembly":"Renderforge","member":"Status"}.</summary>
        public static string Status() => "crisp icons active=" + active + " trilinear=" + originals.Count + " noMips=" + noMips.Count;
    }
}
