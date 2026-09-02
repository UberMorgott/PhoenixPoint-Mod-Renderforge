using System;
using HarmonyLib;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;

namespace Renderforge
{
    /// <summary>The Renderforge rows in Options -> Graphics, all clones of the panel's own TextureQualityPicker
    /// (UIModuleGraphicsOptionsPanel.cs:25) placed directly under it, in this order:
    /// RENDERER, UPSCALER, FRAME GENERATION. GraphicsPanel then places its DLSS quality picker + sharpness
    /// slider after the row this returns, so the order is decided in ONE place.
    /// RENDERER is deferred like the panel's own settings (HasChanges lights Apply, Apply commits + asks);
    /// UPSCALER applies immediately when it is available; an unavailable entry snaps back to the applied value
    /// and leaves the reason as the row's tooltip, writing nothing.</summary>
    internal static class Pickers
    {
        internal const string RendererName = "RenderforgeRenderer";
        internal const string UpscalerName = "RenderforgeUpscaler";
        internal const string FrameGenName = "RenderforgeFrameGen";

        private static bool loggedError;
        private static ArrowPickerController renderer, upscaler, frameGen;
        private static RendererMode pendingRenderer;
        private static bool rendererTouched;   // the user moved the RENDERER row since the panel opened
        private static int pendingUpscaler, pendingFrameGen;
        private static Action onChanged;

        private static string[] RendererLabels
        {
            get { return new[] { "DirectX 11", DlssConfig.Loc("DirectX 12 (experimental)", "DirectX 12 (экспериментально)") }; }
        }

        private static string[] UpscalerLabels
        {
            get { return new[] { DlssConfig.Loc("Off", "Выкл"), "DLSS", "FSR", "XeSS" }; }
        }

        private static string[] FrameGenLabels
        {
            get { return new[] { DlssConfig.Loc("Off", "Выкл"), "2x", "3x", "4x" }; }
        }

        private static Feature UpscalerFeature(int index)
        {
            return index == 2 ? Feature.Fsr : index == 3 ? Feature.Xess : Feature.Dlss;
        }

        /// <summary>Builds/re-syncs the three rows and returns the LAST one, for the caller to place after.</summary>
        internal static Transform Build(UIModuleGraphicsOptionsPanel panel)
        {
            var src = panel.TextureQualityPicker;
            var cfg = RenderforgeMod.Instance.Cfg;
            onChanged = Traverse.Create(panel).Field("_onChanged").GetValue<Action>();

            Reset();
            renderer = Row(src, RendererName, DlssConfig.Loc("Renderer", "Рендерер"), src.transform.GetSiblingIndex() + 1);
            renderer.Init(RendererLabels.Length, pendingRenderer == RendererMode.DirectX12 ? 1 : 0, OnRenderer);
            ShowRenderer();

            pendingUpscaler = cfg.Mode == RenderforgeMode.Off ? 0 : 1;
            upscaler = Row(src, UpscalerName, DlssConfig.Loc("Upscaler", "Апскейлер"), renderer.transform.GetSiblingIndex() + 1);
            upscaler.Init(UpscalerLabels.Length, pendingUpscaler, OnUpscaler);
            ShowUpscaler();

            pendingFrameGen = 0;
            frameGen = Row(src, FrameGenName, DlssConfig.Loc("Frame generation", "Генерация кадров"), upscaler.transform.GetSiblingIndex() + 1);
            frameGen.Init(FrameGenLabels.Length, pendingFrameGen, OnFrameGen);
            ShowFrameGen();

            return frameGen.transform;
        }

        /// <summary>Forget an uncommitted RENDERER choice: called on panel Init AND Deinit (UIModuleGraphicsOptionsPanel.cs:86,:107)
        /// so a change the user backed out of can never be committed by a later Apply.</summary>
        internal static void Reset()
        {
            var cfg = RenderforgeMod.Instance != null ? RenderforgeMod.Instance.Cfg : null;
            pendingRenderer = cfg != null ? RendererSwitch.Effective(cfg.Renderer) : RendererSwitch.Running;
            rendererTouched = false;
        }

        internal static void Hide(Transform content)
        {
            foreach (string n in new[] { RendererName, UpscalerName, FrameGenName })
            {
                var t = content.Find(n);
                if (t != null) t.gameObject.SetActive(false);
            }
        }

        private static ArrowPickerController Row(ArrowPickerController src, string name, string title, int siblingIndex)
        {
            var found = src.transform.parent.Find(name);
            ArrowPickerController p;
            if (found != null) p = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(src.gameObject, src.transform.parent);
                go.name = name;
                p = go.GetComponent<ArrowPickerController>();
                GraphicsPanel.SetRaw(p.Title, null, title.ToUpperInvariant());
            }
            p.transform.SetSiblingIndex(siblingIndex);
            p.gameObject.SetActive(true);
            return p;
        }

        private static void ShowRenderer()
        {
            string label = RendererLabels[pendingRenderer == RendererMode.DirectX12 ? 1 : 0];
            if (pendingRenderer != RendererSwitch.Running)
                label += DlssConfig.Loc(" (restart pending)", " (нужен перезапуск)");
            GraphicsPanel.SetRaw(renderer.CurrentItem, renderer.CurrentItemText, label);
            GraphicsPanel.Grey(renderer.CurrentItem.gameObject, false);
            GraphicsPanel.Tip(renderer.CentralButton.gameObject,
                DlssConfig.Loc("DirectX 12 unlocks FSR, XeSS and frame generation. Changing it restarts the game.",
                               "DirectX 12 открывает FSR, XeSS и генерацию кадров. Смена требует перезапуска игры."));
        }

        private static void ShowUpscaler()
        {
            string reason = pendingUpscaler == 0 ? null : Availability.Reason(UpscalerFeature(pendingUpscaler));
            GraphicsPanel.SetRaw(upscaler.CurrentItem, upscaler.CurrentItemText, UpscalerLabels[pendingUpscaler]);
            GraphicsPanel.Grey(upscaler.CurrentItem.gameObject, reason != null);
            GraphicsPanel.Tip(upscaler.CentralButton.gameObject, reason);
        }

        private static void ShowFrameGen()
        {
            string reason = pendingFrameGen == 0 ? null : Availability.Reason(Feature.FrameGen);
            GraphicsPanel.SetRaw(frameGen.CurrentItem, frameGen.CurrentItemText, FrameGenLabels[pendingFrameGen]);
            GraphicsPanel.Grey(frameGen.CurrentItem.gameObject, reason != null);
            GraphicsPanel.Tip(frameGen.CentralButton.gameObject, reason);
        }

        private static void OnRenderer(int index)
        {
            try
            {
                pendingRenderer = index == 1 ? RendererMode.DirectX12 : RendererMode.DirectX11;
                rendererTouched = true;
                ShowRenderer();
                if (onChanged != null) onChanged();   // lights the panel's Apply button
            }
            catch (Exception ex) { Log("renderer picker change failed", ex); }
        }

        private static void OnUpscaler(int index)
        {
            try
            {
                var mod = RenderforgeMod.Instance;
                if (mod == null) return;
                string reason = index == 0 ? null : Availability.Reason(UpscalerFeature(index));
                if (reason != null)
                {
                    // Refused: snap the row back to the applied value (CurrentIndex is private-set, Init is the
                    // only way to move it) and leave the reason as the row's tooltip.
                    pendingUpscaler = mod.Cfg.Mode == RenderforgeMode.Off ? 0 : 1;
                    upscaler.Init(UpscalerLabels.Length, pendingUpscaler, OnUpscaler);
                    ShowUpscaler();
                    GraphicsPanel.Tip(upscaler.CentralButton.gameObject, reason);
                    return;
                }
                pendingUpscaler = index;
                ShowUpscaler();
                if (index == 0)
                {
                    RenderforgeMod.SetMode(RenderforgeMode.Off.ToString(), mod.Cfg.DebugView.ToString());
                    RenderforgeMod.SaveConfig();
                }
                else if (index == 1 && mod.Cfg.Mode == RenderforgeMode.Off)
                {
                    RenderforgeMod.SetMode(RenderforgeMode.Auto.ToString(), mod.Cfg.DebugView.ToString());
                    RenderforgeMod.SaveConfig();
                }
                GraphicsPanel.SyncQuality();   // keep the DLSS quality row's label/grey in step
            }
            catch (Exception ex) { Log("upscaler picker change failed", ex); }
        }

        private static void OnFrameGen(int index)
        {
            try
            {
                pendingFrameGen = index;
                ShowFrameGen();
            }
            catch (Exception ex) { Log("frame-generation picker change failed", ex); }
        }

        /// <summary>Picker differs from the config (must be written), OR the user moved it and it differs from the
        /// RUNNING API (config may already say so after a "No" to the restart dialog - Apply must still light
        /// so the dialog can be asked again).</summary>
        internal static bool RendererChanged
        {
            get
            {
                var cfg = RenderforgeMod.Instance != null ? RenderforgeMod.Instance.Cfg : null;
                return cfg != null && renderer != null
                    && (pendingRenderer != RendererSwitch.Effective(cfg.Renderer)
                        || (rendererTouched && pendingRenderer != RendererSwitch.Running));
            }
        }

        internal static void ApplyRenderer()
        {
            try
            {
                var cfg = RenderforgeMod.Instance != null ? RenderforgeMod.Instance.Cfg : null;
                if (cfg == null || renderer == null || !RendererChanged) return;
                bool ask = rendererTouched && pendingRenderer != RendererSwitch.Running;
                cfg.Renderer = pendingRenderer;
                rendererTouched = false;
                RenderforgeMod.SaveConfig();
                ShowRenderer();
                if (ask) RendererSwitch.Confirm(pendingRenderer == RendererMode.DirectX12, ShowRenderer);
            }
            catch (Exception ex) { Log("renderer apply failed", ex); }
        }

        private static void Log(string what, Exception ex)
        {
            if (!loggedError && RenderforgeMod.Instance != null)
                RenderforgeMod.Instance.Logger.LogError("Renderforge " + what + ": " + ex);
            loggedError = true;
        }
    }

    /// <summary>RENDERER is a deferred setting: the panel's own Apply button commits it
    /// (UIModuleGraphicsOptionsPanel.cs:124 HasChanges, :137 Apply) - same shape as VideoPanel.</summary>
    [HarmonyPatch(typeof(UIModuleGraphicsOptionsPanel))]
    internal static class GraphicsPanelApply
    {
        [HarmonyPostfix, HarmonyPatch("HasChanges")]
        static void HasChanges(ref bool __result)
        {
            __result |= Pickers.RendererChanged;
        }

        [HarmonyPostfix, HarmonyPatch("Apply")]
        static void Apply()
        {
            Pickers.ApplyRenderer();
        }

        [HarmonyPostfix, HarmonyPatch("Deinit")]
        static void Deinit()
        {
            Pickers.Reset();
        }
    }
}
