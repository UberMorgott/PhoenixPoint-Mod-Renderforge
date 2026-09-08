using System.Collections.Generic;
using HarmonyLib;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>With Canvas.pixelPerfect on, Graphic.GetPixelAdjustedRect goes through RectTransformUtility.PixelAdjustRect,
    /// which returns an EMPTY rect for any element whose world matrix is singular: the game's ActorClassIcon, UIPCHotkey
    /// and AP-pip prefabs carry localScale.z = 0, so 18 icons across tactical + Geoscape vanished with the toggle on
    /// (docs\research\icon-bleed-2026-09-08.md, Round 3). A degenerate result falls back to the unsnapped rect.</summary>
    [HarmonyPatch(typeof(Graphic), nameof(Graphic.GetPixelAdjustedRect))]
    internal static class Graphic_GetPixelAdjustedRect_Patch
    {
        static void Postfix(Graphic __instance, ref Rect __result)
        {
            if (__result.width <= 0f || __result.height <= 0f) __result = __instance.rectTransform.rect;
        }
    }

    /// <summary>Canvas.pixelPerfect on every ROOT ScreenSpaceOverlay canvas (nested canvases inherit unless they set
    /// overridePixelPerfect; camera/world canvases are never touched). Measured at 1440p: UI.Text Sobel +2-7%, elements
    /// shift onto the pixel grid (docs\research\font-remeasure-2026-09-07\results.md) - animated panels step by whole
    /// pixels. Also pins UI textures to mip 0 (MipBias.UiPin): with it the 1 px edge bleed on runtime-created mod icons
    /// measures 0 at every fractional position (docs\research\icon-bleed-2026-09-08.md), hence on by default since 1.6.1.
    /// Re-applied from RenderforgeMod.OnLevelStart so the level's own canvases get it.</summary>
    internal static class PixelPerfectUi
    {
        // ponytail: keys of destroyed canvases stay until the next Restore (a handful per level); prune if it ever matters.
        private static readonly Dictionary<Canvas, bool> original = new Dictionary<Canvas, bool>();
        private static bool active;

        internal static void Apply(bool on)
        {
            // UI textures to mip 0 (log2(canvasScale)); MipBias.Reapply keeps it across level starts.
            if (MipBias.UiPin != on) { MipBias.UiPin = on; MipBias.Resweep(); }
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
