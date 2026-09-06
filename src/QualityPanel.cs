using System;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Vignette / shadow resolution / anisotropic filtering pickers and the LOD detail slider in the game's
    /// native Graphics panel. Immediate apply + SaveConfig, like every other Renderforge row.</summary>
    internal static class QualityPanel
    {
        private const string VignetteName = "RenderforgeVignette";
        private const string ShadowName = "RenderforgeShadowRes";
        private const string AnisoName = "RenderforgeAniso";
        private const string LodName = "RenderforgeLodBias";

        private static ArrowPickerController vignette, shadow, aniso;
        private static Slider lod;
        private static Transform lodValue;
        private static bool loggedError;

        private static string Vanilla { get { return DlssConfig.Loc("Vanilla", "Как в игре"); } }
        /// <summary>"Vanilla (High)": the Vanilla position names the value it currently stands for (QualityKnobs baseline).</summary>
        private static string VanillaWith(string live) { return live == null ? Vanilla : Vanilla + " (" + live + ")"; }
        private static string[] VignetteLabels { get { return new[] { VanillaWith(VignetteLive), DlssConfig.Loc("Off", "Выкл") }; } }
        private static string[] ShadowLabels { get { return new[] { VanillaWith(ShadowLive), DlssConfig.Loc("Very High", "Очень высокое") }; } }
        private static string[] AnisoLabels { get { return new[] { VanillaWith(AnisoLive), "16x" }; } }

        private static string VignetteLive
        {
            get
            {
                bool? on = QualityKnobs.VanillaVignette;
                return on == null ? null : on.Value ? DlssConfig.Loc("On", "Вкл") : DlssConfig.Loc("Off", "Выкл");
            }
        }

        private static string ShadowLive
        {
            get
            {
                switch (QualityKnobs.VanillaShadowRes)
                {
                    case ShadowResolution.Low: return DlssConfig.Loc("Low", "Низкое");
                    case ShadowResolution.Medium: return DlssConfig.Loc("Medium", "Среднее");
                    case ShadowResolution.High: return DlssConfig.Loc("High", "Высокое");
                    default: return DlssConfig.Loc("Very High", "Очень высокое");
                }
            }
        }

        private static string AnisoLive
        {
            get
            {
                switch (QualityKnobs.VanillaAniso)
                {
                    case AnisotropicFiltering.Disable: return DlssConfig.Loc("Off", "Выкл");
                    case AnisotropicFiltering.ForceEnable: return DlssConfig.Loc("forced", "принудительно");
                    default: return DlssConfig.Loc("per-texture", "по текстуре");
                }
            }
        }

        private static string LodLive { get { return QualityKnobs.VanillaLodBias.ToString("F1"); } }

        /// <summary>" Currently X." appended to a knob tooltip; nothing when the value is unknown (vignette outside a mission).</summary>
        private static string Currently(string live)
        {
            return live == null ? "" : " " + DlssConfig.Loc("Currently {0}.", "Сейчас: {0}.").Replace("{0}", live);
        }

        /// <summary>Adds the four rows after `after` and returns the last one, so the caller can keep chaining.</summary>
        internal static Transform Build(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            if (panel.TextureQualityPicker == null || after == null) return after;
            after = Picker(panel, after, VignetteName, DlssConfig.Loc("Vignette", "Виньетка"),
                VignetteLabels, (int)cfg.Vignette, OnVignette, out vignette);
            after = Picker(panel, after, ShadowName, DlssConfig.Loc("Shadow resolution", "Разрешение теней"),
                ShadowLabels, (int)cfg.ShadowResolution, OnShadow, out shadow);
            after = Picker(panel, after, AnisoName, DlssConfig.Loc("Anisotropic filtering", "Анизотропная фильтрация"),
                AnisoLabels, (int)cfg.Anisotropic, OnAniso, out aniso);
            after = BuildLodSlider(panel, after, cfg);
            Sync();
            return after;
        }

        /// <summary>One picker row cloned from TextureQualityPicker; existing clones are reused and re-indexed, because
        /// UIModuleGraphicsOptionsPanel.Init() runs on every options open (GraphicsPanel.cs:15).</summary>
        private static Transform Picker(UIModuleGraphicsOptionsPanel panel, Transform after, string name, string title,
                                        string[] labels, int index, Action<int> onChanged, out ArrowPickerController row)
        {
            var content = after.parent;
            var found = content.Find(name);
            if (found != null) row = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(panel.TextureQualityPicker.gameObject, content);
                go.name = name;
                row = go.GetComponent<ArrowPickerController>();
                GraphicsPanel.SetRaw(row.Title, null, title.ToUpperInvariant());
            }
            row.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
            row.gameObject.SetActive(true);
            row.Init(labels.Length, Mathf.Clamp(index, 0, labels.Length - 1), onChanged);
            return row.transform;
        }

        /// <summary>LOD detail slider. The spec's range is "0 = Vanilla, otherwise 1.0 … 4.0" (spec §A table row LodBias),
        /// so the valid values are NOT contiguous on a bias axis - but the slider positions must be, or keyboard/controller
        /// decrement gets trapped. Positions are therefore CONTIGUOUS and mapped, not scaled: 0 = Vanilla, 1..31 = 1.0,
        /// 1.1, … 4.0 (value = 1.0 + (pos - 1) * 0.1). Decrementing from position 1 (bias 1.0) lands on position 0 =
        /// Vanilla, and every arrow press moves exactly one step in both directions.</summary>
        private static Transform BuildLodSlider(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            var srcSlider = panel.ShadowDistanceSlider;
            if (srcSlider == null) return after;
            var content = after.parent;
            var row = content.Find(LodName);
            if (row == null)
            {
                var go = UnityEngine.Object.Instantiate(srcSlider.transform.parent.gameObject, content);
                go.name = LodName;
                row = go.transform;
            }
            row.SetSiblingIndex(after.GetSiblingIndex() + 1);
            row.gameObject.SetActive(true);
            lod = row.GetComponentInChildren<Slider>(true);
            lodValue = row.Find("UITextGeneric_Medium");
            var label = row.Find("UITextGeneric_Medium (1)");
            if (label != null) GraphicsPanel.SetRaw(label.GetComponent<Localize>(), label.GetComponent<Text>(),
                DlssConfig.Loc("LOD detail", "Детализация LOD").ToUpperInvariant());
            if (lodValue != null) lodValue.gameObject.SetActive(true);
            lod.gameObject.SetActive(true);
            lod.wholeNumbers = true;
            lod.minValue = 0;
            lod.maxValue = 31;                       // 0 = Vanilla, 1..31 = 1.0 .. 4.0 in 0.1 steps
            lod.SetValueWithoutNotify(PosFromBias(cfg.LodBias));
            ShowLod(cfg.LodBias);
            lod.onValueChanged.RemoveAllListeners();
            lod.onValueChanged.AddListener(OnLod);
            return row;
        }

        /// <summary>Position -> bias. 0 = Vanilla (0f), 1..31 = 1.0 + (pos - 1) * 0.1, rounded to one decimal so
        /// float accumulation cannot produce 3.9999997.</summary>
        private static float BiasFromPos(int pos)
        {
            if (pos <= 0) return 0f;
            return Mathf.Round((1f + (Mathf.Min(pos, 31) - 1) * 0.1f) * 10f) / 10f;
        }

        /// <summary>Bias -> position, the exact inverse of BiasFromPos.</summary>
        private static float PosFromBias(float bias)
        {
            if (bias <= 0f) return 0f;
            return Mathf.Clamp(Mathf.RoundToInt((Mathf.Clamp(bias, 1f, 4f) - 1f) * 10f) + 1, 1, 31);
        }

        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return;
            Show(vignette, VignetteLabels, (int)cfg.Vignette, OnVignette, DlssConfig.Loc(
                "Tactical missions only. Vanilla keeps the mission's own vignette; Off removes the darkened frame edges.",
                "Только тактические миссии. «Как в игре» сохраняет виньетку миссии; «Выкл» убирает затемнение по краям кадра.") + Currently(VignetteLive));
            Show(shadow, ShadowLabels, (int)cfg.ShadowResolution, OnShadow, DlssConfig.Loc(
                "Vanilla keeps the graphics preset's value; Very High raises the shadow map size.",
                "«Как в игре» — значение выбранного пресета; «Очень высокое» увеличивает размер карты теней.") + Currently(ShadowLive));
            Show(aniso, AnisoLabels, (int)cfg.Anisotropic, OnAniso, DlssConfig.Loc(
                "Vanilla leaves per-texture filtering alone; 16x forces 16 samples on every texture.",
                "«Как в игре» ничего не меняет; «16x» включает 16 выборок для всех текстур.") + Currently(AnisoLive));
            if (lod != null) lod.SetValueWithoutNotify(PosFromBias(cfg.LodBias));
            ShowLod(cfg.LodBias);
        }

        /// <summary>Label AND CurrentIndex (Pickers.ReInit), so a config change from PPCLI/console cannot leave a stale index.</summary>
        private static void Show(ArrowPickerController row, string[] labels, int index, Action<int> onChanged, string tip)
        {
            if (row == null) return;
            index = Mathf.Clamp(index, 0, labels.Length - 1);
            Pickers.ReInit(row, labels.Length, index, onChanged);
            GraphicsPanel.SetRaw(row.CurrentItem, row.CurrentItemText, labels[index]);
            GraphicsPanel.Grey(row.CurrentItem.gameObject, false);
            GraphicsPanel.Tip(row.CentralButton.gameObject, tip);
        }

        private static void ShowLod(float bias)
        {
            if (lodValue == null) return;
            GraphicsPanel.SetRaw(lodValue.GetComponent<Localize>(), lodValue.GetComponent<Text>(),
                bias > 0f ? bias.ToString("F1") : VanillaWith(LodLive));
            if (lod != null) GraphicsPanel.Tip(lod.gameObject, DlssConfig.Loc(
                "0 = the graphics preset's own value (currently {0}). Higher keeps detailed models further away; below the preset's value = less detail.",
                "0 = значение выбранного пресета (сейчас {0}). Выше — модели дольше остаются детальными вдали; ниже значения пресета — меньше деталей.").Replace("{0}", LodLive));
        }

        internal static void Hide(Transform content)
        {
            if (content == null) return;
            foreach (string name in new[] { VignetteName, ShadowName, AnisoName, LodName })
            {
                var row = content.Find(name);
                if (row != null) row.gameObject.SetActive(false);
            }
        }

        internal static void Clear() { vignette = shadow = aniso = null; lod = null; lodValue = null; }

        private static void OnVignette(int index) => Change(cfg => cfg.Vignette = (VignetteMode)Mathf.Clamp(index, 0, 1));
        private static void OnShadow(int index) => Change(cfg => cfg.ShadowResolution = (ShadowResolutionMode)Mathf.Clamp(index, 0, 1));
        private static void OnAniso(int index) => Change(cfg => cfg.Anisotropic = (AnisotropicMode)Mathf.Clamp(index, 0, 1));

        /// <summary>No snapping: every position is valid, so a decrement from position 1 (bias 1.0) reaches position 0 =
        /// Vanilla instead of being bounced back up.</summary>
        private static void OnLod(float raw)
        {
            float bias = BiasFromPos(Mathf.RoundToInt(raw));
            Change(cfg => cfg.LodBias = bias);
        }

        private static void Change(Action<DlssConfig> edit)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                edit(cfg);
                QualityKnobs.ApplyAll();
                RenderforgeMod.SaveConfig();
                Sync();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge quality row change failed: " + ex);
                loggedError = true;
            }
        }
    }
}
