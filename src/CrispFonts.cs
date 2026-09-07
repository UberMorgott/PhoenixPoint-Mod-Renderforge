using System;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    internal static class CrispFonts
    {
        private const string Owner = "com.morgott.Renderforge.crisp-fonts";
        private static readonly MethodInfo Populate = AccessTools.Method(typeof(Text), "OnPopulateMesh", new[] { typeof(VertexHelper) });
        private static readonly MethodInfo Disable = AccessTools.Method(typeof(Text), "OnDisable");
        private static readonly MethodInfo ChangeFont = AccessTools.PropertySetter(typeof(Text), "font");
        private static Harmony harmony;
        private static GameObject host;
        private static bool active, logged, cleanupPending;
        private static Text[] restore = new Text[0];
        private static int checkedFrame = -1;
        internal static bool Active => active;

        internal static bool Supported(Text text) => text && text.GetType() == typeof(Text) && text.isActiveAndEnabled
            && text.font && text.font.dynamic && text.material == text.defaultMaterial && text.canvas && text.canvas.rootCanvas
            && text.canvas.rootCanvas.renderMode == RenderMode.ScreenSpaceOverlay;

        internal static void Apply(bool enabled)
        {
            if (cleanupPending) { Tick(); if (cleanupPending) return; }
            if (enabled == active) return;
            if (!enabled) { Dispose(); return; }
            try
            {
                if (HasForeignOwner())
                { Log("Other Text.OnPopulateMesh patch owner; original fonts retained."); return; }
                harmony = new Harmony(Owner);
                harmony.Patch(Populate, postfix: new HarmonyMethod(typeof(CrispFonts), nameof(AfterPopulate)));
                harmony.Patch(Disable, postfix: new HarmonyMethod(typeof(CrispFonts), nameof(Release)));
                harmony.Patch(ChangeFont, postfix: new HarmonyMethod(typeof(CrispFonts), nameof(AfterFontChanged)));
                Font.textureRebuilt += FontRasterCorrection.AtlasChanged;
                host = new GameObject("Renderforge crisp fonts") { hideFlags = HideFlags.HideAndDontSave };
                UnityEngine.Object.DontDestroyOnLoad(host);
                host.AddComponent<CrispFontsRefresh>();
                active = true;
                checkedFrame = -1;
                DirtySupported();
            }
            catch (Exception ex) { Dispose(); Log("Font correction unavailable: " + ex.Message); }
        }

        private static void AfterPopulate(Text __instance, VertexHelper toFill)
        {
            if (!active || !Supported(__instance)) return;
            // Patch metadata once per dirty frame, not once per glyph or label.
            if (checkedFrame != Time.frameCount)
            {
                checkedFrame = Time.frameCount;
                if (HasForeignOwner())
                { Dispose(); Log("A late Text.OnPopulateMesh patch owner appeared; original fonts restored."); return; }
            }
            try { FontRasterCorrection.Apply(__instance, toFill, true); }
            catch (Exception ex) { Log("Font correction fallback failed: " + ex.Message); }
        }
        private static void Release(Text __instance) => FontRasterCorrection.Release(__instance);
        private static void AfterFontChanged(Text __instance) => FontRasterCorrection.FontChanged(__instance);
        private static bool HasForeignOwner()
        {
            var patches = Harmony.GetPatchInfo(Populate);
            return patches != null && patches.Owners.Any(o => o != Owner);
        }
        private static void DirtySupported()
        {
            foreach (Text text in UnityEngine.Object.FindObjectsOfType<Text>())
                if (Supported(text)) text.SetVerticesDirty();
        }
        internal static void Dispose()
        {
            active = false;
            // Snapshot ownership before clearing caches; a formerly supported Text may now use a custom material.
            restore = restore.Concat(FontRasterCorrection.AffectedTexts()).Where(t => t).Distinct().ToArray();
            cleanupPending = true;
            Tick();
        }

        internal static void Tick()
        {
            // Unity rejects SetVerticesDirty registration during a graphics rebuild. Retain the host until drained.
            if (FontRasterCorrection.Processing || CanvasUpdateRegistry.IsRebuildingGraphics() || CanvasUpdateRegistry.IsRebuildingLayout()) return;
            if (!cleanupPending) { FontRasterCorrection.RefreshPending(); return; }
            Font.textureRebuilt -= FontRasterCorrection.AtlasChanged;
            if (harmony != null)
            {
                harmony.Unpatch(Populate, HarmonyPatchType.All, Owner);
                harmony.Unpatch(Disable, HarmonyPatchType.All, Owner);
                harmony.Unpatch(ChangeFont, HarmonyPatchType.All, Owner);
                harmony = null;
            }
            if (host) UnityEngine.Object.Destroy(host);
            host = null;
            FontRasterCorrection.Dispose();
            foreach (Text text in restore) if (text && text.isActiveAndEnabled) text.SetVerticesDirty();
            restore = new Text[0]; cleanupPending = false;
        }
        private static void Log(string message)
        {
            if (!logged) RenderforgeMod.Instance?.Logger.LogWarning("Crisp fonts: " + message);
            logged = true;
        }
    }

    internal sealed class CrispFontsRefresh : MonoBehaviour
    {
        private void LateUpdate() => CrispFonts.Tick();
    }
}
