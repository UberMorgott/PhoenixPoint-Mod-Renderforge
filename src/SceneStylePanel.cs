using System;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    public enum SceneStyle { Off, Cartoon, PixelArt }

    /// <summary>Code-only scene styles; the normal overlay UI is drawn after this post pass.</summary>
    internal static class SceneStylePanel
    {
        private const string PickerName = "RenderforgeSceneStyle";
        private const string StrengthName = "RenderforgeSceneStyleStrength";
        private const string PixelName = "RenderforgePixelSize";
        private static ArrowPickerController picker;
        private static Slider strength, pixelSize;
        private static Transform strengthValue, pixelValue;
        private static bool loggedError;

        private static string[] Labels => new[] {
            DlssConfig.Loc("Off", "Выкл"), DlssConfig.Loc("Cartoon", "Мультфильм"),
            DlssConfig.Loc("Pixel art", "Пиксель-арт") };

        internal static bool Active(DlssConfig cfg) => cfg != null && cfg.SceneStyle >= SceneStyle.Cartoon
            && cfg.SceneStyle <= SceneStyle.PixelArt && cfg.SceneStyleStrength > 0;

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
                GraphicsPanel.SetRaw(picker.Title, null, DlssConfig.Loc("Scene style", "Стиль сцены").ToUpperInvariant());
            }
            picker.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
            picker.gameObject.SetActive(true);
            picker.Init(Labels.Length, Mathf.Clamp((int)cfg.SceneStyle, 0, 2), OnStyle);
            var row = BuildSlider(panel, picker.transform, StrengthName,
                DlssConfig.Loc("Style strength", "Сила стилизации"), 0, 100, cfg.SceneStyleStrength,
                ref strength, ref strengthValue, OnStrength);
            row = BuildSlider(panel, row, PixelName, DlssConfig.Loc("Pixel block size", "Размер пикселя"),
                2, 16, cfg.PixelSize, ref pixelSize, ref pixelValue, OnPixelSize);
            Sync();
            return row;
        }

        private static Transform BuildSlider(UIModuleGraphicsOptionsPanel panel, Transform after, string name,
            string title, int min, int max, int current, ref Slider slider, ref Transform value,
            UnityEngine.Events.UnityAction<float> callback)
        {
            var src = panel.ShadowDistanceSlider;
            if (src == null) return after;
            var row = after.parent.Find(name);
            if (row == null)
            {
                var go = UnityEngine.Object.Instantiate(src.transform.parent.gameObject, after.parent);
                go.name = name;
                row = go.transform;
            }
            row.SetSiblingIndex(after.GetSiblingIndex() + 1);
            row.gameObject.SetActive(true);
            slider = row.GetComponentInChildren<Slider>(true);
            value = row.Find("UITextGeneric_Medium");
            var label = row.Find("UITextGeneric_Medium (1)");
            if (label != null) GraphicsPanel.SetRaw(label.GetComponent<Localize>(), label.GetComponent<Text>(), title.ToUpperInvariant());
            if (value != null) value.gameObject.SetActive(true);
            slider.gameObject.SetActive(true);
            slider.wholeNumbers = true;
            slider.minValue = min;
            slider.maxValue = max;
            slider.SetValueWithoutNotify(Mathf.Clamp(current, min, max));
            slider.onValueChanged.RemoveAllListeners();
            slider.onValueChanged.AddListener(callback);
            return row;
        }

        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return;
            int index = Mathf.Clamp((int)cfg.SceneStyle, 0, 2);
            if (picker != null)
            {
                GraphicsPanel.SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[index]);
                GraphicsPanel.Tip(picker.CentralButton.gameObject, DlssConfig.Loc(
                    "Cartoon: colour bands and contrast outlines. Pixel art: 4-pixel blocks by default, a gentle palette reduction, no extra outlines. Adjust block size from 2 to 16.",
                    "Мультфильм: ступени цвета и контрастные контуры. Пиксель-арт: блоки по 4 пикселя, мягкое сокращение палитры, без дополнительных контуров. Размер блока — от 2 до 16."));
            }
            SyncSlider(strength, strengthValue, index != 0, Mathf.Clamp(cfg.SceneStyleStrength, 0, 100), "%");
            SyncSlider(pixelSize, pixelValue, index == 2, Mathf.Clamp(cfg.PixelSize, 2, 16), " px");
        }

        private static void SyncSlider(Slider slider, Transform value, bool enabled, int current, string suffix)
        {
            if (slider == null) return;
            slider.interactable = enabled;
            slider.SetValueWithoutNotify(current);
            if (value != null) GraphicsPanel.SetRaw(value.GetComponent<Localize>(), value.GetComponent<Text>(), current + suffix);
            var cg = slider.transform.parent.GetComponent<CanvasGroup>() ?? slider.transform.parent.gameObject.AddComponent<CanvasGroup>();
            cg.alpha = enabled ? 1f : 0.35f;
        }

        internal static void Hide(Transform content)
        {
            if (content == null) return;
            foreach (string name in new[] { PickerName, StrengthName, PixelName })
            {
                var row = content.Find(name);
                if (row != null) row.gameObject.SetActive(false);
            }
        }

        internal static void Clear() { picker = null; strength = pixelSize = null; strengthValue = pixelValue = null; }

        private static void OnStyle(int index) => Change(cfg => cfg.SceneStyle = (SceneStyle)Mathf.Clamp(index, 0, 2));
        private static void OnStrength(float raw) => Change(cfg => cfg.SceneStyleStrength = Mathf.Clamp((int)raw, 0, 100));
        private static void OnPixelSize(float raw) => Change(cfg => cfg.PixelSize = Mathf.Clamp((int)raw, 2, 16));

        private static void Change(Action<DlssConfig> change)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                change(cfg);
                RenderforgeMod.SaveConfig();
                // The existing driver polls these values every frame, including the full-resolution Off path.
                // No AttachAndApply/feature teardown is needed for a style control.
                Sync();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge scene style: " + ex);
                loggedError = true;
            }
        }
    }
}
