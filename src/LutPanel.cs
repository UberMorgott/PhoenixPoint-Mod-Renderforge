using System;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Live colour-grade picker and strength row in the game's native Graphics panel.</summary>
    internal static class LutPanel
    {
        private const string PickerName = "RenderforgeLut";
        private const string SliderName = "RenderforgeLutStrength";
        private static ArrowPickerController picker;
        private static Slider strength;
        private static Transform value;
        private static bool loggedError;

        private static string[] Labels => new[]
        {
            DlssConfig.Loc("Off", "Выкл"),
            DlssConfig.Loc("Realistic Desaturated", "Реалистичный приглушённый"),
            DlssConfig.Loc("Neutral", "Нейтральный"),
            DlssConfig.Loc("Cinematic Bleach", "Кино: bleach bypass"),
            DlssConfig.Loc("Vivid", "Насыщенный")
        };

        internal static Transform Build(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            var src = panel.TextureQualityPicker;
            if (src == null || after == null) return after;
            var content = after.parent;
            var found = content.Find(PickerName);
            if (found != null) picker = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(src.gameObject, content);
                go.name = PickerName;
                picker = go.GetComponent<ArrowPickerController>();
                GraphicsPanel.SetRaw(picker.Title, null, DlssConfig.Loc("LUT filter", "LUT-фильтр").ToUpperInvariant());
            }
            picker.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
            picker.gameObject.SetActive(true);
            int index = Mathf.Clamp((int)cfg.Lut, 0, Labels.Length - 1);
            picker.Init(Labels.Length, index, OnPreset);

            var srcSlider = panel.ShadowDistanceSlider;
            if (srcSlider == null) { Sync(); return picker.transform; }
            var row = content.Find(SliderName);
            if (row == null)
            {
                var go = UnityEngine.Object.Instantiate(srcSlider.transform.parent.gameObject, content);
                go.name = SliderName;
                row = go.transform;
            }
            row.SetSiblingIndex(picker.transform.GetSiblingIndex() + 1);
            row.gameObject.SetActive(true);
            strength = row.GetComponentInChildren<Slider>(true);
            value = row.Find("UITextGeneric_Medium");
            var label = row.Find("UITextGeneric_Medium (1)");
            if (label != null) GraphicsPanel.SetRaw(label.GetComponent<Localize>(), label.GetComponent<Text>(),
                DlssConfig.Loc("LUT strength", "Сила LUT").ToUpperInvariant());
            if (value != null) value.gameObject.SetActive(true);
            strength.gameObject.SetActive(true);
            strength.wholeNumbers = true;
            strength.minValue = 0;
            strength.maxValue = 100;
            strength.SetValueWithoutNotify(Mathf.Clamp(cfg.LutStrength, 0, 100));
            strength.onValueChanged.RemoveAllListeners();
            strength.onValueChanged.AddListener(OnStrength);
            ShowStrength((int)strength.value);
            Sync();
            return row;
        }

        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return;
            int index = Mathf.Clamp((int)cfg.Lut, 0, Labels.Length - 1);
            if (picker != null)
            {
                GraphicsPanel.SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[index]);
                GraphicsPanel.Grey(picker.CurrentItem.gameObject, false);
                GraphicsPanel.Tip(picker.CentralButton.gameObject,
                    index == 1
                        ? DlssConfig.Loc("Neutral grey balance with lower saturation, soft highlights and preserved skin tones.",
                            "Нейтральный серый баланс, меньше насыщенности, мягкие света и сохранённые оттенки кожи.")
                        : DlssConfig.Loc("Original analytic colour grade; no third-party LUT asset is bundled.",
                            "Оригинальная аналитическая цветокоррекция; сторонние LUT-файлы не включены."));
            }
            bool on = index != 0;
            if (strength != null)
            {
                strength.interactable = on;
                foreach (var go in new[] { strength.gameObject, value != null ? value.gameObject : null })
                {
                    if (go == null) continue;
                    var cg = go.GetComponent<CanvasGroup>() ?? go.AddComponent<CanvasGroup>();
                    cg.alpha = on ? 1f : 0.35f;
                }
            }
        }

        internal static void Hide(Transform content)
        {
            if (content == null) return;
            foreach (string name in new[] { PickerName, SliderName })
            {
                var row = content.Find(name);
                if (row != null) row.gameObject.SetActive(false);
            }
        }

        internal static void Clear() { picker = null; strength = null; value = null; }

        private static void OnPreset(int index)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                cfg.Lut = (LutPreset)Mathf.Clamp(index, 0, Labels.Length - 1);
                RenderforgeMod.SaveConfig();
                RenderforgeMod.ApplyLutSettings();
                Sync();
            }
            catch (Exception ex) { Log("picker change failed", ex); }
        }

        private static void OnStrength(float raw)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                cfg.LutStrength = Mathf.Clamp((int)raw, 0, 100);
                ShowStrength(cfg.LutStrength);
                RenderforgeMod.SaveConfig();
                RenderforgeMod.ApplyLutSettings();
            }
            catch (Exception ex) { Log("strength change failed", ex); }
        }

        private static void ShowStrength(int current)
        {
            if (value != null) GraphicsPanel.SetRaw(value.GetComponent<Localize>(), value.GetComponent<Text>(), current.ToString());
        }

        private static void Log(string what, Exception ex)
        {
            if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge LUT " + what + ": " + ex);
            loggedError = true;
        }
    }
}
