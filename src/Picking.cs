using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using Base.Cameras;
using HarmonyLib;
using PhoenixPoint.Tactical.View.ViewStates;
using SlimUI.ConsoleCursors;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Screen-space seams. While DLSS is live the scene camera renders into colorRT, so its pixelWidth/Height are
    /// the RENDER res while Input.mousePosition, Screen.* and the overlay HUD stay at SCREEN res. Unity's dynamic
    /// resolution would keep pixelWidth screen-sized, but it is DX12-only on Windows (ScalableBufferManager.ResizeBuffers
    /// is a no-op on D3D11, verified live 2026-09-02). So every screen&lt;-&gt;world conversion on that camera is rescaled here
    /// and the game keeps seeing a screen-sized camera. Only the innermost managed wrappers (the ones calling the
    /// *_Injected icalls) are patched; the 1-arg overloads route through them.</summary>
    internal static class Seams
    {
        /// <summary>render / screen scale for the live scene camera; false (1,1) for any other camera, DLAA or Off.</summary>
        public static bool Scaled(Camera c, out float sx, out float sy)
        {
            var d = DlssDriver.Instance;
            if (d == null || c == null || !d.IsLive || c != d.SceneCamera || (d.RenderW == d.OutW && d.RenderH == d.OutH)) { sx = sy = 1f; return false; }
            sx = (float)d.RenderW / d.OutW;
            sy = (float)d.RenderH / d.OutH;
            return true;
        }

        public static int PixelWidth(Camera c) => Scaled(c, out _, out _) ? DlssDriver.Instance.OutW : c.pixelWidth;
        public static int PixelHeight(Camera c) => Scaled(c, out _, out _) ? DlssDriver.Instance.OutH : c.pixelHeight;

        public static void ScreenToRender(Camera c, ref Vector3 p)
        {
            if (Scaled(c, out float sx, out float sy)) { p.x *= sx; p.y *= sy; }
        }

        public static void RenderToScreen(Camera c, ref Vector3 p)
        {
            if (Scaled(c, out float sx, out float sy)) { p.x /= sx; p.y /= sy; }
        }
    }

    [HarmonyPatch(typeof(Camera), nameof(Camera.ScreenPointToRay), typeof(Vector3), typeof(Camera.MonoOrStereoscopicEye))]
    internal static class Camera_ScreenPointToRay_Patch { static void Prefix(Camera __instance, ref Vector3 __0) => Seams.ScreenToRender(__instance, ref __0); }

    [HarmonyPatch(typeof(Camera), nameof(Camera.ScreenToWorldPoint), typeof(Vector3), typeof(Camera.MonoOrStereoscopicEye))]
    internal static class Camera_ScreenToWorldPoint_Patch { static void Prefix(Camera __instance, ref Vector3 __0) => Seams.ScreenToRender(__instance, ref __0); }

    [HarmonyPatch(typeof(Camera), nameof(Camera.ScreenToViewportPoint), typeof(Vector3))]
    internal static class Camera_ScreenToViewportPoint_Patch { static void Prefix(Camera __instance, ref Vector3 __0) => Seams.ScreenToRender(__instance, ref __0); }

    [HarmonyPatch(typeof(Camera), nameof(Camera.WorldToScreenPoint), typeof(Vector3), typeof(Camera.MonoOrStereoscopicEye))]
    internal static class Camera_WorldToScreenPoint_Patch { static void Postfix(Camera __instance, ref Vector3 __result) => Seams.RenderToScreen(__instance, ref __result); }

    [HarmonyPatch(typeof(Camera), nameof(Camera.ViewportToScreenPoint), typeof(Vector3))]
    internal static class Camera_ViewportToScreenPoint_Patch { static void Postfix(Camera __instance, ref Vector3 __result) => Seams.RenderToScreen(__instance, ref __result); }

    /// <summary>Game code that reads Camera.pixelWidth/pixelHeight as "the screen" (icall getters cannot be patched, so
    /// the readers are transpiled): CameraBehavior.CenterScreenPos (:108), PlanarScrollCamera edge scroll (:1072),
    /// FirstPersonCamera mouse-look sensitivity (:388-592), UIStateFreeCam centre rays (:164,:324,:497),
    /// FreeCursorController clamp (:396).</summary>
    [HarmonyPatch]
    internal static class PixelSize_Transpiler
    {
        static IEnumerable<MethodBase> TargetMethods()
        {
            yield return AccessTools.PropertyGetter(typeof(CameraBehavior), "CenterScreenPos");
            yield return AccessTools.Method(typeof(PlanarScrollCamera), "GetEdgeScrollOffset");
            yield return AccessTools.Method(typeof(FirstPersonCamera), "UpdateInput");
            yield return AccessTools.Method(typeof(FirstPersonCamera), "HandleInput");
            yield return AccessTools.Method(typeof(FirstPersonCamera), "GetMouseOffset");
            yield return AccessTools.Method(typeof(UIStateFreeCam), "GetTargetPos");
            yield return AccessTools.Method(typeof(UIStateFreeCam), "GetCameraPosByCameraTarget");
            yield return AccessTools.Method(typeof(UIStateFreeCam), "GetDefaultTarget");
            yield return AccessTools.Method(typeof(FreeCursorController), "SetFreeCursorActive");
        }

        static IEnumerable<CodeInstruction> Transpiler(IEnumerable<CodeInstruction> il)
        {
            var w = AccessTools.PropertyGetter(typeof(Camera), nameof(Camera.pixelWidth));
            var h = AccessTools.PropertyGetter(typeof(Camera), nameof(Camera.pixelHeight));
            var rw = AccessTools.Method(typeof(Seams), nameof(Seams.PixelWidth));
            var rh = AccessTools.Method(typeof(Seams), nameof(Seams.PixelHeight));
            foreach (var c in il)
            {
                if (c.Calls(w)) { c.opcode = OpCodes.Call; c.operand = rw; }
                else if (c.Calls(h)) { c.opcode = OpCodes.Call; c.operand = rh; }
                yield return c;
            }
        }
    }
}
