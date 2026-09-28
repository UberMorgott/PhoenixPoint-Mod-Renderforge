using HarmonyLib;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Pixel snapping WITHOUT Canvas.pixelPerfect. UGUI 2019.4 consults canvas.pixelPerfect in exactly two
    /// managed methods - Graphic.GetPixelAdjustedRect (Image quads: simple, sliced, sprite-mesh) and
    /// Graphic.PixelAdjustPoint (UI.Text rounding offset). Everything else the flag switches on is native per-frame
    /// canvas work that measured 0.5-0.6 ms/frame even on an idle screen (docs\research\pixelperfect-motion-cost-
    /// 2026-09-15\results.md, the Discord "fps drops during UI motion" report). So the canvas flag stays off and these
    /// postfixes snap instead, paying only when a mesh is actually rebuilt.
    /// The snap is our own math: the natives RectTransformUtility.PixelAdjustRect/Point read canvas.pixelPerfect
    /// THEMSELVES and are identity while it is off (live-probed: PixelAdjustPoint(0,0) = (0,0) off, (-0.495,-0.495) on;
    /// snap-experiments.md next to results.md). A ScreenSpaceOverlay root canvas lives in pixel units in world space,
    /// so rounding the world xy of a corner IS the pixel snap; the native rounds each corner the same way (rect
    /// (-0.495,-0.495 64.5x64.5) for a 64-unit quad at .33 px, scale 0.667).
    /// A non-invertible transform (ActorClassIcon, UIPCHotkey, AP-pip prefabs carry localScale.z = 0,
    /// docs\research\icon-bleed-2026-09-08.md Round 3) fails the round trip and keeps the unsnapped value.</summary>
    [HarmonyPatch(typeof(Graphic), nameof(Graphic.GetPixelAdjustedRect))]
    internal static class Graphic_GetPixelAdjustedRect_Patch
    {
        static void Postfix(Graphic __instance, ref Rect __result)
        {
            if (PixelPerfectUi.SnapCanvas(__instance) == null) return;
            Rect snapped;
            if (PixelPerfectUi.SnapRect(__instance.rectTransform, __result, out snapped)) __result = snapped;
        }
    }

    [HarmonyPatch(typeof(Graphic), nameof(Graphic.PixelAdjustPoint))]
    internal static class Graphic_PixelAdjustPoint_Patch
    {
        static void Postfix(Graphic __instance, Vector2 point, ref Vector2 __result)
        {
            if (PixelPerfectUi.SnapCanvas(__instance) == null) return;
            Vector2 snapped;
            if (PixelPerfectUi.SnapPoint(__instance.transform, point, out snapped)) __result = snapped;
        }
    }

    /// <summary>Option "Pixel-perfect UI": snap (patches above, ScreenSpaceOverlay canvases only, same guards as
    /// UGUI's own) + UI textures pinned to mip 0 (MipBias.UiPin). Measured: UI.Text Sobel +2-7% at 1440p
    /// (docs\research\font-remeasure-2026-09-07\results.md); with the pin the 1 px edge bleed on runtime-created
    /// mod icons measures 0 at every fractional position (docs\research\icon-bleed-2026-09-08.md) - snap alone 61,
    /// pin alone 118, both 0 - hence on by default since 1.6.1. Graphics created later build with the snap on
    /// their first mesh, so nothing is re-applied per level.</summary>
    internal static class PixelPerfectUi
    {
        private static bool active;

        /// <summary>Local point -> nearest whole screen pixel -> local. False when the transform does not round-trip
        /// (singular matrix) - the caller keeps the unsnapped value.
        /// Both matrices are read once per call (two icalls instead of three Transform*Point per point); the round trip still
        /// runs through the forward matrix, so a singular inverse can only ever produce a verified snap or none.</summary>
        internal static bool SnapPoint(Transform t, Vector2 local, out Vector2 snapped)
        {
            Matrix4x4 l2w = t.localToWorldMatrix, w2l = t.worldToLocalMatrix;
            return SnapPoint(ref l2w, ref w2l, local, out snapped);
        }

        private static bool SnapPoint(ref Matrix4x4 l2w, ref Matrix4x4 w2l, Vector2 local, out Vector2 snapped)
        {
            var w = l2w.MultiplyPoint3x4(local);
            w.x = Mathf.Round(w.x); w.y = Mathf.Round(w.y);
            var l = w2l.MultiplyPoint3x4(w);
            var back = l2w.MultiplyPoint3x4(l);
            snapped = new Vector2(l.x, l.y);
            return Mathf.Abs(back.x - w.x) < 0.01f && Mathf.Abs(back.y - w.y) < 0.01f;
        }

        internal static bool SnapRect(Transform t, Rect r, out Rect snapped)
        {
            Vector2 a, b;
            snapped = r;
            Matrix4x4 l2w = t.localToWorldMatrix, w2l = t.worldToLocalMatrix;
            if (!SnapPoint(ref l2w, ref w2l, r.min, out a) || !SnapPoint(ref l2w, ref w2l, r.max, out b)) return false;
            snapped = Rect.MinMaxRect(Mathf.Min(a.x, b.x), Mathf.Min(a.y, b.y), Mathf.Max(a.x, b.x), Mathf.Max(a.y, b.y));
            return snapped.width > 0f && snapped.height > 0f;
        }

        // canvas -> "root canvas is ScreenSpaceOverlay", valid for one frame (rootCanvas walks the hierarchy natively; a
        // layout pass asks it for every graphic of the same canvas). pixelPerfect / scaleFactor stay live reads.
        private static readonly System.Collections.Generic.Dictionary<Canvas, bool> overlayRoot = new System.Collections.Generic.Dictionary<Canvas, bool>();
        private static int overlayFrame = -1;

        internal static Canvas SnapCanvas(Graphic g)
        {
            if (!active) return null;
            var canvas = g.canvas;
            if (!canvas || canvas.pixelPerfect || canvas.scaleFactor == 0f) return null;
            int f = Time.frameCount;
            if (f != overlayFrame) { overlayRoot.Clear(); overlayFrame = f; }
            bool overlay;
            if (!overlayRoot.TryGetValue(canvas, out overlay)) overlayRoot[canvas] = overlay = canvas.rootCanvas.renderMode == RenderMode.ScreenSpaceOverlay;
            return overlay ? canvas : null;
        }

        internal static void Apply(bool on)
        {
            // UI textures to mip 0 (log2(canvasScale)); MipBias.Reapply keeps it across level starts.
            if (MipBias.UiPin != on) { MipBias.UiPin = on; MipBias.Resweep(); }
            if (active == on) return;
            active = on;
            int rebuilt = 0;
            foreach (var g in Resources.FindObjectsOfTypeAll<Graphic>())
                if (g.gameObject.scene.IsValid()) { g.SetVerticesDirty(); rebuilt++; }
            RenderforgeMod.Instance?.Logger.LogInfo("Pixel-perfect UI " + (on ? "on" : "off") + ": " + rebuilt + " graphics rebuilt");
        }
    }
}
