using System;
using HarmonyLib;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Native "DLSS" picker in the game's Graphics options panel: a clone of the TextureQuality
    /// ArrowPickerController (UIModuleGraphicsOptionsPanel.cs:25), rebuilt/re-synced by the panel's own
    /// parameterless Init() (:86, called by Init(bool, Action) :69 on every options open). Immediate apply
    /// (no Apply-button flow: the panel's _onChanged/HasChanges track only the graphics preset).</summary>
    [HarmonyPatch(typeof(UIModuleGraphicsOptionsPanel), "Init", new Type[0])]
    internal static class GraphicsPanel
    {
        private const string Name = "DlssPicker";
        private const string SliderName = "DlssSharpness";
        private static readonly string[] Labels = { "Off", "Auto", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
        private static bool loggedError;
        private static ArrowPickerController picker;
        private static Slider sharp;
        private static Transform sharpValue;

        static void Postfix(UIModuleGraphicsOptionsPanel __instance)
        {
            try
            {
                var mod = RenderforgeMod.Instance;
                var src = __instance.TextureQualityPicker;
                if (mod == null || src == null) return;
                var existing = src.transform.parent.Find(Name);
                if (!mod.Cfg.ShowInGraphicsOptions)
                {
                    Pickers.Reset();
                    Pickers.Hide(src.transform.parent);
                    if (existing != null) existing.gameObject.SetActive(false);
                    var hidden = src.transform.parent.Find(SliderName);
                    if (hidden != null) hidden.gameObject.SetActive(false);
                    return;
                }
                // RENDERER / UPSCALER / FRAME GENERATION first; our quality row goes after the last of them.
                Transform after = Pickers.Build(__instance);
                if (existing != null) picker = existing.GetComponent<ArrowPickerController>();
                else
                {
                    var go = UnityEngine.Object.Instantiate(src.gameObject, src.transform.parent);
                    go.name = Name;
                    picker = go.GetComponent<ArrowPickerController>();
                    SetRaw(picker.Title, null, DlssConfig.Loc("DLSS quality", "Качество DLSS").ToUpperInvariant());
                }
                picker.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
                picker.gameObject.SetActive(true);
                int idx = (int)mod.Cfg.Mode;
                if (idx < 0 || idx >= Labels.Length) idx = 0;
                picker.Init(Labels.Length, idx, i => OnChanged(picker, i));
                BuildSlider(__instance, picker.transform, mod.Cfg);
                SyncQuality();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge graphics-panel picker failed: " + ex);
                loggedError = true;
            }
        }

        /// <summary>Repaints the quality row's label + grey from the config and Availability. Called after our own
        /// changes and by Pickers when the UPSCALER row moves.</summary>
        internal static void SyncQuality()
        {
            var mod = RenderforgeMod.Instance;
            if (picker == null || mod == null) return;
            int idx = (int)mod.Cfg.Mode;
            if (idx < 0 || idx >= Labels.Length) idx = 0;
            string reason = Availability.Reason(Feature.Dlss);
            SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[idx]);
            Grey(picker.CurrentItem.gameObject, reason != null);
            Tip(picker.CentralButton.gameObject, reason);
            SetSliderEnabled(reason == null && idx != (int)RenderforgeMode.Off);
        }

        private static void OnChanged(ArrowPickerController target, int i)
        {
            try
            {
                var mod = RenderforgeMod.Instance;
                if (mod == null) return;
                if (Availability.Reason(Feature.Dlss) != null)
                {
                    // Not usable on this API/GPU: show the choice greyed, write nothing.
                    SetRaw(target.CurrentItem, target.CurrentItemText, Labels[i]);
                    Grey(target.CurrentItem.gameObject, true);
                    return;
                }
                RenderforgeMod.SetMode(((RenderforgeMode)i).ToString(), mod.Cfg.DebugView.ToString());
                RenderforgeMod.SaveConfig();
                SyncQuality();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge picker change failed: " + ex);
                loggedError = true;
            }
        }

        /// <summary>"SHARPNESS" slider row right under the picker: a clone of the panel's own ShadowDistance row (same
        /// TextAndSlider prefab family as VideoPanel's rows: "Slider", label "UITextGeneric_Medium (1)", readout
        /// "UITextGeneric_Medium"). Immediate apply like the picker; greyed while the picker is Off.</summary>
        private static void BuildSlider(UIModuleGraphicsOptionsPanel panel, Transform picker, DlssConfig cfg)
        {
            var srcSlider = panel.ShadowDistanceSlider;
            if (srcSlider == null) return;
            var content = picker.parent;
            var row = content.Find(SliderName);
            if (row == null)
            {
                var go = UnityEngine.Object.Instantiate(srcSlider.transform.parent.gameObject, content);
                go.name = SliderName;
                row = go.transform;
            }
            row.SetSiblingIndex(picker.GetSiblingIndex() + 1);
            row.gameObject.SetActive(true);
            sharp = row.GetComponentInChildren<Slider>(true);
            sharpValue = row.Find("UITextGeneric_Medium");
            var label = row.Find("UITextGeneric_Medium (1)");
            if (label != null) SetRaw(label.GetComponent<Localize>(), label.GetComponent<Text>(), DlssConfig.Loc("Sharpness", "Резкость").ToUpperInvariant());
            if (sharpValue != null) sharpValue.gameObject.SetActive(true);
            sharp.gameObject.SetActive(true);
            sharp.wholeNumbers = true;
            sharp.minValue = 0;
            sharp.maxValue = 100;
            sharp.SetValueWithoutNotify(Mathf.Clamp(cfg.Sharpness, 0, 100));
            ShowSharp((int)sharp.value);
            sharp.onValueChanged.RemoveAllListeners();
            sharp.onValueChanged.AddListener(OnSharp);
            SetSliderEnabled(cfg.Mode != RenderforgeMode.Off);
        }

        private static void OnSharp(float v)
        {
            try
            {
                var mod = RenderforgeMod.Instance;
                if (mod == null) return;
                mod.Cfg.Sharpness = (int)v;      // the driver reads it every frame: live
                ShowSharp((int)v);
                RenderforgeMod.SaveConfig();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge sharpness change failed: " + ex);
                loggedError = true;
            }
        }

        private static void ShowSharp(int v)
        {
            if (sharpValue != null) SetRaw(sharpValue.GetComponent<Localize>(), sharpValue.GetComponent<Text>(), v.ToString());
        }

        // Same grey as VideoPanel.SetSliderEnabled: interactable alone shows nothing on this prefab, a CanvasGroup does.
        private static void SetSliderEnabled(bool on)
        {
            if (sharp == null) return;
            sharp.interactable = on;
            foreach (var go in new[] { sharp.gameObject, sharpValue != null ? sharpValue.gameObject : null })
            {
                if (go == null) continue;
                var cg = go.GetComponent<CanvasGroup>() ?? go.AddComponent<CanvasGroup>();
                cg.alpha = on ? 1f : 0.35f;
            }
        }

        /// <summary>I2 Localize (Localize.cs:122 OnEnable / :336 SetTerm) overwrites the Text with a term lookup, and a
        /// missing term leaves the old text (:154,:169); so disable the bind and write the raw string.</summary>
        internal static void SetRaw(Localize bind, Text text, string value)
        {
            if (bind != null)
            {
                bind.enabled = false;
                if (text == null) text = bind.GetComponent<Text>() ?? bind.GetComponentInChildren<Text>(true);
            }
            if (text != null) text.text = value;
        }

        /// <summary>Same grey as SetSliderEnabled: interactable alone shows nothing on these prefabs, a CanvasGroup does.</summary>
        internal static void Grey(GameObject go, bool grey)
        {
            if (go == null) return;
            var cg = go.GetComponent<CanvasGroup>() ?? go.AddComponent<CanvasGroup>();
            cg.alpha = grey ? 0.35f : 1f;
        }

        /// <summary>The game's OWN tooltip (UITooltipText.cs:7, IPointerEnterHandler at :99) - it clones the
        /// game's "Interface/UI_Prefabs/UI_Tooltip" prefab (:52). Attach it to something that receives pointer
        /// events (a picker's CentralButton). tip == null/empty disables it.</summary>
        internal static void Tip(GameObject go, string tip)
        {
            if (go == null) return;
            var t = go.GetComponent<UITooltipText>();
            if (string.IsNullOrEmpty(tip))
            {
                if (t != null) t.Enabled = false;
                return;
            }
            if (t == null)
            {
                t = go.AddComponent<UITooltipText>();
                t.Position = UITooltip.Position.RightMiddle;
                t.MaxWidth = 300;
                t.AppearTime = 0.3f;
                t.FadeInTime = 8f;
                t.FadeOutTime = 8f;
            }
            t.Enabled = true;
            t.TipText = tip;
            t.UpdateText(tip);
        }
    }
}
