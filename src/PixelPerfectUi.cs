using System.Collections.Generic;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Canvas.pixelPerfect on every ROOT ScreenSpaceOverlay canvas (nested canvases inherit unless they set
    /// overridePixelPerfect; camera/world canvases are never touched). Measured at 1440p: UI.Text Sobel +2-7%, elements
    /// shift onto the pixel grid (docs\research\font-remeasure-2026-09-07\results.md) - animated panels step by whole
    /// pixels, hence off by default. Re-applied from RenderforgeMod.OnLevelStart so the level's own canvases get it.</summary>
    internal static class PixelPerfectUi
    {
        // ponytail: keys of destroyed canvases stay until the next Restore (a handful per level); prune if it ever matters.
        private static readonly Dictionary<Canvas, bool> original = new Dictionary<Canvas, bool>();
        private static bool active;

        internal static void Apply(bool on)
        {
            if (!on) { Restore(); return; }
            active = true;
            int touched = 0;
            foreach (var canvas in Resources.FindObjectsOfTypeAll<Canvas>())
            {
                if (!canvas.gameObject.scene.IsValid() || !canvas.isRootCanvas || canvas.renderMode != RenderMode.ScreenSpaceOverlay) continue;
                if (!original.ContainsKey(canvas)) original[canvas] = canvas.pixelPerfect;
                canvas.pixelPerfect = true;
                touched++;
            }
            RenderforgeMod.Instance?.Logger.LogInfo("Pixel-perfect UI on: " + touched + " root overlay canvases");
        }

        /// <summary>OnLevelStart (the level's UI canvases exist only now) and Overlay.Create.</summary>
        // ponytail: a root canvas another mod creates mid-level waits for the next level start; rescan on a timer if reported.
        internal static void Reapply() { if (active) Apply(true); }

        private static void Restore()
        {
            if (!active) return;
            active = false;
            int restored = 0;
            foreach (var pair in original)
                if (pair.Key) { pair.Key.pixelPerfect = pair.Value; restored++; }
            original.Clear();
            RenderforgeMod.Instance?.Logger.LogInfo("Pixel-perfect UI off: " + restored + " canvases restored");
        }
    }
}
