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
        private static bool active, logged;
        internal static bool Active => active;

        internal static bool Supported(Text text) => text && text.GetType() == typeof(Text) && text.isActiveAndEnabled
            && text.font && text.font.dynamic && text.material == text.defaultMaterial && text.canvas && text.canvas.rootCanvas
            && text.canvas.rootCanvas.renderMode == RenderMode.ScreenSpaceOverlay;

        internal static void Apply(bool enabled)
        {
            if (enabled == active) return;
            if (!enabled) { Dispose(); return; }
            try
            {
                var patches = Harmony.GetPatchInfo(Populate);
                if (patches != null && patches.Owners.Any(o => o != Owner))
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
                DirtySupported();
            }
            catch (Exception ex) { Dispose(); Log("Font correction unavailable: " + ex.Message); }
        }

        private static void AfterPopulate(Text __instance, VertexHelper toFill)
        {
            if (!active || !Supported(__instance)) return;
            try { FontRasterCorrection.Apply(__instance, toFill, true); }
            catch (Exception ex) { Log("Font correction fallback failed: " + ex.Message); }
        }
        private static void Release(Text __instance) => FontRasterCorrection.Release(__instance);
        private static void AfterFontChanged(Text __instance) => FontRasterCorrection.FontChanged(__instance);
        private static void DirtySupported()
        {
            foreach (Text text in UnityEngine.Object.FindObjectsOfType<Text>())
                if (Supported(text)) text.SetVerticesDirty();
        }
        internal static void Dispose()
        {
            bool wasActive = active;
            active = false;
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
            if (wasActive) DirtySupported();
        }
        private static void Log(string message)
        {
            if (!logged) RenderforgeMod.Instance?.Logger.LogWarning("Crisp fonts: " + message);
            logged = true;
        }
    }

    internal sealed class CrispFontsRefresh : MonoBehaviour
    {
        private void LateUpdate() => FontRasterCorrection.RefreshPending();
    }
}
