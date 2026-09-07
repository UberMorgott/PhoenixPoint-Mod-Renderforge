using System;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Four live slider rows in the game's native Graphics panel - Black point, White point, Contrast,
    /// Clarity - the Levels / Contrast / Clarity stages of the analytic post pass. Each row is the LUT-strength
    /// recipe (a clone of the panel's ShadowDistance row); immediate apply + SaveConfig, the driver reads the
    /// values every frame, so no feature teardown is needed.</summary>
    internal static class GradePanel
    {
        private sealed class Knob
        {
            public string Name, En, Ru, TipEn, TipRu;
            public int Min, Max;
            public Func<DlssConfig, int> Get;
            public Action<DlssConfig, int> Set;
            public Slider Slider;
            public Transform Value;
        }

        // Row order in the panel = array order. Ranges mirror Dlss_SetGrade's clamps.
        private static readonly Knob[] Knobs =
        {
            new Knob { Name = "RenderforgeLevelsBlack", En = "Black point", Ru = "Точка чёрного",
                TipEn = "Pixels darker than this become black and the rest stretch — deeper shadows. 0 = off.",
                TipRu = "Пиксели темнее этого значения становятся чёрными, остальные растягиваются — тени глубже. 0 = выкл.",
                Min = 0, Max = 40, Get = c => c.LevelsBlack, Set = (c, v) => c.LevelsBlack = v },
            new Knob { Name = "RenderforgeLevelsWhite", En = "White point", Ru = "Точка белого",
                TipEn = "Pixels brighter than this become white — brighter highlights. 255 = off.",
                TipRu = "Пиксели ярче этого значения становятся белыми — светлые участки ярче. 255 = выкл.",
                Min = 215, Max = 255, Get = c => c.LevelsWhite, Set = (c, v) => c.LevelsWhite = v },
            new Knob { Name = "RenderforgeContrast", En = "Contrast", Ru = "Контраст",
                TipEn = "Pivots at mid-grey: 100 = off, below flattens, above deepens (dark scenes get darker).",
                TipRu = "Опорная точка — средний серый: 100 = выкл, ниже — мягче, выше — контрастнее (тёмные сцены темнеют).",
                Min = 50, Max = 150, Get = c => c.Contrast, Set = (c, v) => c.Contrast = v },
            new Knob { Name = "RenderforgeClarity", En = "Clarity", Ru = "Чёткость",
                TipEn = "Local contrast on fine detail; 0 = off. Scene only; the HUD stays unchanged.",
                TipRu = "Локальный контраст мелких деталей; 0 = выкл. Только сцена, интерфейс не меняется.",
                Min = 0, Max = 100, Get = c => c.Clarity, Set = (c, v) => c.Clarity = v },
        };
        private static bool loggedError;

        /// <summary>The ONE activation gate, like ColorVisionPanel.Active: DlssDriver.Step (start the pipeline) and
        /// the per-frame Dlss_SetGrade both use it. Any knob off its default = on.</summary>
        internal static bool Active(DlssConfig cfg) => cfg != null
            && (cfg.LevelsBlack > 0 || cfg.LevelsWhite < 255 || cfg.Contrast != 100 || cfg.Clarity > 0);

        internal static Transform Build(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            var srcSlider = panel.ShadowDistanceSlider;
            if (srcSlider == null || after == null) return after;
            var content = after.parent;
            foreach (var knob in Knobs)
            {
                var row = content.Find(knob.Name);
                if (row == null)
                {
                    var go = UnityEngine.Object.Instantiate(srcSlider.transform.parent.gameObject, content);
                    go.name = knob.Name;
                    row = go.transform;
                }
                row.SetSiblingIndex(after.GetSiblingIndex() + 1);
                row.gameObject.SetActive(true);
                knob.Slider = row.GetComponentInChildren<Slider>(true);
                if (knob.Slider == null) continue;   // a row without a slider is a broken clone; Sync/OnChanged skip it too
                knob.Value = row.Find("UITextGeneric_Medium");
                var label = row.Find("UITextGeneric_Medium (1)");
                if (label != null) GraphicsPanel.SetRaw(label.GetComponent<Localize>(), label.GetComponent<Text>(),
                    DlssConfig.Loc(knob.En, knob.Ru).ToUpperInvariant());
                if (knob.Value != null) knob.Value.gameObject.SetActive(true);
                knob.Slider.gameObject.SetActive(true);
                knob.Slider.wholeNumbers = true;
                knob.Slider.minValue = knob.Min;
                knob.Slider.maxValue = knob.Max;
                knob.Slider.onValueChanged.RemoveAllListeners();
                var captured = knob;
                knob.Slider.onValueChanged.AddListener(v => OnChanged(captured, v));
                GraphicsPanel.Tip(knob.Slider.gameObject, DlssConfig.Loc(knob.TipEn, knob.TipRu));
                after = row;
            }
            Sync();
            return after;
        }

        /// <summary>Slider positions + readouts from the config (a PPCLI/console change cannot leave them stale).</summary>
        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return;
            foreach (var knob in Knobs)
            {
                if (knob.Slider == null) continue;
                int v = Mathf.Clamp(knob.Get(cfg), knob.Min, knob.Max);
                knob.Slider.SetValueWithoutNotify(v);
                Show(knob, v);
            }
        }

        internal static void Hide(Transform content)
        {
            if (content == null) return;
            foreach (var knob in Knobs)
            {
                var row = content.Find(knob.Name);
                if (row != null) row.gameObject.SetActive(false);
            }
        }

        internal static void Clear() { foreach (var knob in Knobs) { knob.Slider = null; knob.Value = null; } }

        private static void OnChanged(Knob knob, float raw)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                int v = Mathf.Clamp((int)raw, knob.Min, knob.Max);
                knob.Set(cfg, v);
                Show(knob, v);
                RenderforgeMod.SaveConfig();   // the driver polls the config every frame, like colour vision
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge " + knob.En + " change failed: " + ex);
                loggedError = true;
            }
        }

        private static void Show(Knob knob, int v)
        {
            if (knob.Value != null) GraphicsPanel.SetRaw(knob.Value.GetComponent<Localize>(), knob.Value.GetComponent<Text>(), v.ToString());
        }
    }
}
