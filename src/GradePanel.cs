using System;
using System.Globalization;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Eight live slider rows in the game's native Graphics panel - Exposure, Black point, White point,
    /// Brightness, Contrast, Clarity, Vibrance, Saturation (the chain order of the analytic post pass, Grade.h) -
    /// plus one "Reset image settings" button row. Each slider row is the LUT-strength recipe (a clone of the
    /// panel's ShadowDistance row); immediate apply + SaveConfig, the driver reads the values every frame, so no
    /// feature teardown is needed. The reset row is a TextureQualityPicker clone with the arrows hidden and the
    /// central button wired directly (the panel has no standalone button prefab).</summary>
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
            public Func<int, string> Fmt;   // readout; null = the raw integer
        }

        /// <summary>The ONE source of defaults = DlssConfig's field initializers; Active and ResetAll read it.</summary>
        private static readonly DlssConfig Defaults = new DlssConfig();

        // Row order in the panel = array order = shader order. Ranges mirror Dlss_SetGrade's clamps.
        private static readonly Knob[] Knobs =
        {
            new Knob { Name = "RenderforgeExposure", En = "Exposure", Ru = "Экспозиция",
                TipEn = "Exposure in tenths of a stop: 10 = +1 EV (twice the light), -10 = half. Applied first in the chain. Scene only; the HUD stays unchanged. Default: 0.",
                TipRu = "Экспозиция в десятых долях ступени: 10 = +1 EV (вдвое больше света), -10 = вдвое меньше. Применяется первой в цепочке. Только сцена, интерфейс не меняется. По умолчанию: 0.",
                Min = -40, Max = 40, Get = c => c.Exposure, Set = (c, v) => c.Exposure = v,
                Fmt = v => (v / 10f).ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture) },   // "+2.0" / "0.0" / "-0.5" EV
            new Knob { Name = "RenderforgeLevelsBlack", En = "Black point", Ru = "Точка чёрного",
                TipEn = "Pixels darker than this become black and the rest stretch — deeper shadows. 0 = off. Default: 0.",
                TipRu = "Пиксели темнее этого значения становятся чёрными, остальные растягиваются — тени глубже. 0 = выкл. По умолчанию: 0.",
                Min = 0, Max = 40, Get = c => c.LevelsBlack, Set = (c, v) => c.LevelsBlack = v },
            new Knob { Name = "RenderforgeLevelsWhite", En = "White point", Ru = "Точка белого",
                TipEn = "Pixels brighter than this become white — brighter highlights. 255 = off. Default: 255.",
                TipRu = "Пиксели ярче этого значения становятся белыми — светлые участки ярче. 255 = выкл. По умолчанию: 255.",
                Min = 215, Max = 255, Get = c => c.LevelsWhite, Set = (c, v) => c.LevelsWhite = v },
            new Knob { Name = "RenderforgeBrightness", En = "Brightness", Ru = "Яркость",
                TipEn = "Midtone brightness (gamma): black and white stay put, above 0 lifts the mids, below 0 sinks them. Scene only; the HUD stays unchanged. Default: 0.",
                TipRu = "Яркость средних тонов (гамма): чёрный и белый остаются на месте, выше 0 средние тона светлее, ниже 0 — темнее. Только сцена, интерфейс не меняется. По умолчанию: 0.",
                Min = -100, Max = 100, Get = c => c.Brightness, Set = (c, v) => c.Brightness = v },
            new Knob { Name = "RenderforgeContrast", En = "Contrast", Ru = "Контраст",
                TipEn = "Pivots at mid-grey: 100 = off, below flattens, above deepens (dark scenes get darker). Default: 100.",
                TipRu = "Опорная точка — средний серый: 100 = выкл, ниже — мягче, выше — контрастнее (тёмные сцены темнеют). По умолчанию: 100.",
                Min = 50, Max = 150, Get = c => c.Contrast, Set = (c, v) => c.Contrast = v },
            new Knob { Name = "RenderforgeClarity", En = "Clarity", Ru = "Чёткость",
                TipEn = "Local contrast on fine detail; 0 = off. Scene only; the HUD stays unchanged. Default: 0.",
                TipRu = "Локальный контраст мелких деталей; 0 = выкл. Только сцена, интерфейс не меняется. По умолчанию: 0.",
                Min = 0, Max = 100, Get = c => c.Clarity, Set = (c, v) => c.Clarity = v },
            new Knob { Name = "RenderforgeVibrance", En = "Vibrance", Ru = "Красочность",
                TipEn = "Saturation boost for muted colours only; vivid ones barely change. Below 0 mutes them. Scene only; the HUD stays unchanged. Default: 0.",
                TipRu = "Усиление насыщенности только приглушённых цветов; яркие почти не меняются. Ниже 0 — приглушает. Только сцена, интерфейс не меняется. По умолчанию: 0.",
                Min = -100, Max = 100, Get = c => c.Vibrance, Set = (c, v) => c.Vibrance = v },
            new Knob { Name = "RenderforgeSaturation", En = "Saturation", Ru = "Насыщенность",
                TipEn = "Colour intensity of everything: 0 = greyscale, 100 = as rendered, 200 = double. Scene only; the HUD stays unchanged. Default: 100.",
                TipRu = "Интенсивность всех цветов: 0 = чёрно-белое, 100 = как отрисовано, 200 = вдвое сильнее. Только сцена, интерфейс не меняется. По умолчанию: 100.",
                Min = 0, Max = 200, Get = c => c.Saturation, Set = (c, v) => c.Saturation = v },
        };
        private const string ResetName = "RenderforgeImageReset";
        private static ArrowPickerController reset;
        private static bool loggedError;

        /// <summary>The ONE activation gate, like ColorVisionPanel.Active: DlssDriver.Step (start the pipeline) and
        /// the per-frame Dlss_SetGrade both use it. Any knob off its default = on.</summary>
        internal static bool Active(DlssConfig cfg)
        {
            if (cfg == null) return false;
            foreach (var knob in Knobs) if (knob.Get(cfg) != knob.Get(Defaults)) return true;
            return false;
        }

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
                after = row;   // advanced before any skip, so a broken clone cannot make later rows stack in reverse
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
            }
            Sync();
            return after;
        }

        /// <summary>"Reset image settings" row, placed by GraphicsPanel AFTER the last Renderforge row: the panel ships
        /// no standalone button, so this is a TextureQualityPicker clone (the same prefab every Renderforge picker row
        /// uses) with both arrows hidden and CentralButton (a PhoenixGeneralButton) wired straight to OnReset.
        /// ArrowPickerController.Init is deliberately NOT called: it would bind the central button to NextOption
        /// (ArrowPickerController.cs:45-48).</summary>
        internal static Transform BuildReset(UIModuleGraphicsOptionsPanel panel, Transform after)
        {
            var src = panel.TextureQualityPicker;
            if (src == null || after == null) return after;
            var content = after.parent;
            var found = content.Find(ResetName);
            if (found != null) reset = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(src.gameObject, content);
                go.name = ResetName;
                reset = go.GetComponent<ArrowPickerController>();
            }
            GraphicsPanel.SetRaw(reset.Title, null,   // label left like every other row; on reuse too, so a re-enabled Localize cannot restore TextureQuality
                DlssConfig.Loc("Reset image settings", "Сбросить настройки изображения").ToUpperInvariant());
            reset.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
            reset.gameObject.SetActive(true);
            reset.PreviousArrow.gameObject.SetActive(false);
            reset.NextArrow.gameObject.SetActive(false);
            reset.CentralButton.SetEnabled(true);
            reset.CentralButton.PointerClicked = OnReset;   // assignment, not +=: a rebuilt panel must not stack handlers
            GraphicsPanel.SetRaw(reset.CurrentItem, reset.CurrentItemText,   // one short verb: the long label wrapped and clipped inside the button
                DlssConfig.Loc("Reset", "Сбросить").ToUpperInvariant());
            GraphicsPanel.Tip(reset.CentralButton.gameObject, DlssConfig.Loc(
                "Resets every Renderforge image setting to its default: Sharpness, LUT filter and LUT strength, Exposure, Black point, White point, Brightness, Contrast, Clarity, Vibrance, Saturation, Scene style with its strength and pixel block size, Vignette, Shadow resolution, Anisotropic filtering and LOD detail. Renderer, Upscaler, Quality, Frame generation and Colour vision stay as chosen.",
                "Сбрасывает все настройки изображения Renderforge к значениям по умолчанию: резкость, LUT-фильтр и силу LUT, экспозицию, точку чёрного, точку белого, яркость, контраст, чёткость, красочность, насыщенность, стиль сцены с его силой и размером пикселя, виньетку, разрешение теней, анизотропную фильтрацию и детализацию LOD. Рендерер, апскейлер, качество, генерация кадров и цветовое зрение не меняются."));
            return reset.transform;
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
            var resetRow = content.Find(ResetName);
            if (resetRow != null) resetRow.gameObject.SetActive(false);
        }

        internal static void Clear() { foreach (var knob in Knobs) { knob.Slider = null; knob.Value = null; } reset = null; }

        /// <summary>Every Renderforge Graphics-panel slider/picker back to its DlssConfig default: Sharpness, LUT filter +
        /// strength, the eight knobs here, scene style + strength + pixel size, and the QualityPanel rows. Hardware and
        /// accessibility choices stay: Renderer, Upscaler, Quality (mode), Frame generation, Colour vision. Applied the way
        /// each panel's own change handler applies it - the driver polls Sharpness / grade / scene style every frame,
        /// the LUT needs ApplyLutSettings (LutPanel.OnPreset), the quality rows need QualityKnobs.ApplyAll
        /// (QualityPanel.Change); one SaveConfig; every visible row re-synced.</summary>
        internal static void ResetAll()
        {
            var m = RenderforgeMod.Instance;
            if (m == null) return;
            var c = m.Cfg;
            var d = Defaults;
            c.Sharpness = d.Sharpness;
            c.Lut = d.Lut;
            c.LutStrength = d.LutStrength;
            foreach (var knob in Knobs) knob.Set(c, knob.Get(d));
            c.SceneStyle = d.SceneStyle;
            c.SceneStyleStrength = d.SceneStyleStrength;
            c.PixelSize = d.PixelSize;
            c.Vignette = d.Vignette;
            c.ShadowResolution = d.ShadowResolution;
            c.Anisotropic = d.Anisotropic;
            c.LodBias = d.LodBias;
            RenderforgeMod.SaveConfig();
            RenderforgeMod.ApplyLutSettings();
            QualityKnobs.ApplyAll();
            Sync();
            GraphicsPanel.SyncSharpness();
            LutPanel.Sync();
            SceneStylePanel.Sync();
            QualityPanel.Sync();
            m.Logger.LogInfo("Renderforge image settings reset to defaults: Sharpness, Lut, LutStrength, Exposure, LevelsBlack, "
                + "LevelsWhite, Brightness, Contrast, Clarity, Vibrance, Saturation, SceneStyle, SceneStyleStrength, PixelSize, "
                + "Vignette, ShadowResolution, Anisotropic, LodBias (Renderer, Upscaler, Mode, FrameGen, ColorVision kept)");
        }

        private static void OnReset()
        {
            try { ResetAll(); }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge image reset failed: " + ex);
                loggedError = true;
            }
        }

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
            if (knob.Value != null) GraphicsPanel.SetRaw(knob.Value.GetComponent<Localize>(), knob.Value.GetComponent<Text>(),
                knob.Fmt != null ? knob.Fmt(v) : v.ToString());
        }
    }
}
