using System;
using System.Globalization;
using Base.UI;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>The four dimensionless numeric controls exposed by the private DLSSNR 310.8 parameter ABI.</summary>
    internal static class NeuralRenderingPanel
    {
        private static readonly string[] Names =
        {
            "RenderforgeNrIntensity", "RenderforgeNrLocalTone", "RenderforgeNrLocalStructure", "RenderforgeNrSkinStructure"
        };
        private static readonly Slider[] Sliders = new Slider[4];
        private static readonly Transform[] Values = new Transform[4];
        private static readonly Transform[] Rows = new Transform[4];
        private static bool loggedError;

        internal static Transform Build(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            if (!NeuralRenderingSupport.Available) { Hide(after.parent); return after; }
            var source = panel.ShadowDistanceSlider;
            if (source == null) return after;
            string[] labels =
            {
                DlssConfig.Loc("NR intensity", "Интенсивность NR"),
                DlssConfig.Loc("NR local tone", "Локальный тон NR"),
                DlssConfig.Loc("NR local structure", "Локальная структура NR"),
                DlssConfig.Loc("NR skin structure", "Структура кожи NR")
            };
            float[] current = { cfg.NeuralIntensity, cfg.NeuralLocalTone, cfg.NeuralLocalStructure, cfg.NeuralSkinStructure };
            var content = after.parent;
            for (int i = 0; i < Names.Length; ++i)
            {
                var row = content.Find(Names[i]);
                if (row == null)
                {
                    var go = UnityEngine.Object.Instantiate(source.transform.parent.gameObject, content);
                    go.name = Names[i];
                    row = go.transform;
                }
                Rows[i] = row;
                row.SetSiblingIndex(after.GetSiblingIndex() + i + 1);
                row.gameObject.SetActive(true);
                Sliders[i] = row.GetComponentInChildren<Slider>(true);
                Values[i] = row.Find("UITextGeneric_Medium");
                var label = row.Find("UITextGeneric_Medium (1)");
                if (label != null) GraphicsPanel.SetRaw(label.GetComponent<Localize>(), label.GetComponent<Text>(), labels[i].ToUpperInvariant());
                if (Values[i] != null) Values[i].gameObject.SetActive(true);
                var slider = Sliders[i];
                slider.gameObject.SetActive(true);
                slider.wholeNumbers = false;
                slider.minValue = i == 3 ? -1f : 0f;
                slider.maxValue = i == 3 ? 1f : 2f;
                slider.SetValueWithoutNotify(Mathf.Clamp(current[i], slider.minValue, slider.maxValue));
                var formatter = row.GetComponentInChildren<SliderTextFormatter>(true);
                if (formatter != null)
                {
                    formatter.Slider = slider;
                    formatter.LerpValue = false;
                    formatter.TextFormat = "{0:0.00}";
                    if (Values[i] != null) formatter.Text = Values[i].GetComponent<Text>();
                }
                int slot = i;
                slider.onValueChanged.RemoveAllListeners();
                slider.onValueChanged.AddListener(v => OnValue(slot, v));
                GraphicsPanel.Tip(slider.gameObject, i == 3
                    ? DlssConfig.Loc("Runtime range: -1.00 to 1.00", "Диапазон runtime: от -1,00 до 1,00")
                    : DlssConfig.Loc("Runtime range: 0.00 to 2.00", "Диапазон runtime: от 0,00 до 2,00"));
                Show(i, slider.value);
            }
            Sync();
            return Rows[Rows.Length - 1];
        }

        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            bool on = NeuralRenderingSupport.Available && cfg != null && cfg.NeuralRendering == NeuralRenderingMode.Auto;
            for (int i = 0; i < Sliders.Length; ++i)
            {
                if (Sliders[i] == null) continue;
                Sliders[i].interactable = on;
                foreach (var go in new[] { Sliders[i].gameObject, Values[i] != null ? Values[i].gameObject : null })
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
            foreach (string name in Names)
            {
                var row = content.Find(name);
                if (row != null) row.gameObject.SetActive(false);
            }
        }

        internal static void Clear()
        {
            for (int i = 0; i < Sliders.Length; ++i) { Sliders[i] = null; Values[i] = null; Rows[i] = null; }
        }

        private static void OnValue(int slot, float raw)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null || cfg.NeuralRendering != NeuralRenderingMode.Auto) return;
                float value = Mathf.Round(raw * 100f) / 100f;
                Sliders[slot].SetValueWithoutNotify(value);
                switch (slot)
                {
                    case 0: cfg.NeuralIntensity = value; break;
                    case 1: cfg.NeuralLocalTone = value; break;
                    case 2: cfg.NeuralLocalStructure = value; break;
                    case 3: cfg.NeuralSkinStructure = value; break;
                }
                Show(slot, value);
                RenderforgeMod.ApplyNeuralRenderingSettings();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge NR slider change failed: " + ex);
                loggedError = true;
            }
        }

        private static void Show(int slot, float value)
        {
            if (Values[slot] != null)
                GraphicsPanel.SetRaw(Values[slot].GetComponent<Localize>(), Values[slot].GetComponent<Text>(),
                    value.ToString("0.00", CultureInfo.CurrentCulture));
        }
    }
}
