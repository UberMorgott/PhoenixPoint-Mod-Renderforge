using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using PhoenixPoint.Modding;
using Renderforge.Localization;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Passthrough = native CopyResource instead of NGX (pipeline proof); Depth/MotionVectors = the present
    /// camera shows depthRT/mvRT (stretched, raw values) instead of the DLSS output.</summary>
    public enum DebugView { None, Passthrough, Depth, MotionVectors }

    public enum OverlayCorner { TopLeft, TopCenter, TopRight, BottomCenter }

    /// <summary>Which graphics API the game should be launched with. Auto == DirectX11 (the game's own default);
    /// DirectX12 needs "-force-d3d12" on the command line, i.e. a restart (RendererSwitch).</summary>
    public enum RendererMode { Auto, DirectX11, DirectX12 }

    /// <summary>Frame generation multiplier. Off = the game presents every rendered frame and nothing else.
    /// 3x/4x exist only on DLSS-G with an RTX 50 GPU; the picker greys what Fg_Caps does not report.</summary>
    public enum FrameGenMode { Off, X2, X3, X4 }

    /// <summary>Original analytic colour grades. Ordinals cross the managed/native ABI; append only.</summary>
    public enum LutPreset { Off, RealisticDesaturated, Neutral, CinematicBleach, Vivid, BlackAndWhiteCinema, Noir, AmberFilm, Arctic, VintageSepia }

    /// <summary>Colour-vision correction (daltonization). Ordinals cross the managed/native ABI; append only.</summary>
    public enum ColorVisionMode { None, Deuteranopia, Protanopia, Tritanopia }

    /// <summary>Tactical vignette. Vanilla = whatever the level's volume shipped with (captured per volume);
    /// Off = the mod writes enabled.value = false on the runtime profile.</summary>
    public enum VignetteMode { Vanilla, Off }

    /// <summary>QualitySettings.shadowResolution. Vanilla = the captured preset value (High at Ultra);
    /// VeryHigh is Unity's top tier - the engine still caps the map at 4096 dir / 2048 spot / 1024 point.</summary>
    public enum ShadowResolutionMode { Vanilla, VeryHigh }

    /// <summary>Vanilla never writes anisotropic filtering at all. Force16 = ForceEnable +
    /// Texture.SetGlobalAnisotropicFilteringLimits(16, 16); the restore is the snapshot + limits (-1, -1).</summary>
    public enum AnisotropicMode { Vanilla, Force16 }

    /// <summary>Public fields = the in-game mod settings UI + ModConfig.json (ModConfig.GetConfigFields).
    /// [ConfigField] = the English label; GetConfigFields routes label + description through Loc, so the embedded
    /// strings.csv translates them like every other string and the Ru dictionary stays the fallback.</summary>
    public class DlssConfig : ModConfig
    {
        // These values stay in GetConfigFields for the loader's serializer. ModSettingsFilter marks only the
        // call that is building Mods -> Renderforge, where duplicate normal Graphics/Screen rows are hidden.
        private static readonly HashSet<string> HiddenFromModSettings = new HashSet<string>
        {
            nameof(Mode), nameof(Sharpness), nameof(Renderer), nameof(Upscaler), nameof(FrameGen),
            nameof(LimitFrameRate), nameof(FrameRateLimit), nameof(Lut), nameof(LutStrength),
            nameof(SceneStyle), nameof(SceneStyleStrength), nameof(PixelSize), nameof(CrispFonts), nameof(CrispIcons),
            nameof(Vignette), nameof(ShadowResolution), nameof(Anisotropic), nameof(LodBias),
            nameof(ColorVision), nameof(LevelsBlack), nameof(LevelsWhite), nameof(Contrast), nameof(Clarity)
        };

        [ConfigField("DLSS mode", "Off, Auto (by resolution), DLAA, Quality, Balanced, Performance, Ultra Performance. Needs an active upscaler.")]
        public RenderforgeMode Mode = RenderforgeMode.Auto;
        [ConfigField("Sharpness", "0 = off … 100. RCAS pass after DLSS; also a slider in Options → Graphics.")]
        public int Sharpness = 40;                      // 0..100 -> RCAS 0..1, applied every frame, live
        [ConfigField("LUT filter", "Original colour grade applied after temporal reconstruction. Scene only; the HUD stays unchanged. Also in Options → Graphics.")]
        public LutPreset Lut = LutPreset.Off;
        [ConfigField("LUT strength", "0 = original image … 100 = full grade. Applied live.")]
        public int LutStrength = 100;
        [ConfigField("Scene style", "Off, Cartoon or PixelArt. Code-only scene filtering after reconstruction. Scene only; the HUD stays unchanged.")]
        public SceneStyle SceneStyle = SceneStyle.Off;
        [ConfigField("Style strength", "0 = original image, 100 = full style. Applied live.")]
        public int SceneStyleStrength = 100;
        [ConfigField("Pixel block size", "PixelArt: default 4 actual output pixels. Adjust from 2 to 16 for finer or stronger pixelation.")]
        public int PixelSize = 4;
        [ConfigField("Colour vision", "Off, Deuteranopia, Protanopia or Tritanopia. Redistributes colours the eye cannot separate onto channels it can. Scene only; the HUD stays unchanged. Also in Options → Graphics.")]
        public ColorVisionMode ColorVision = ColorVisionMode.None;
        // ReShade-style Levels / Contrast / Clarity in the same post pass (GradePanel sliders, live every frame).
        [ConfigField("Black point", "Pixels darker than this become black and the rest stretch — deeper shadows. 0 = off.")]
        public int LevelsBlack = 0;                     // 0..40
        [ConfigField("White point", "Pixels brighter than this become white — brighter highlights. 255 = off.")]
        public int LevelsWhite = 255;                   // 215..255
        [ConfigField("Contrast", "Pivots at mid-grey: 100 = off, below flattens, above deepens (dark scenes get darker).")]
        public int Contrast = 100;                      // 50..150
        [ConfigField("Clarity", "Local contrast on fine detail; 0 = off. Scene only; the HUD stays unchanged.")]
        public int Clarity = 0;                         // 0..100
        // Legacy (feature removed in 1.5.0): kept so an old ModConfig.json still round-trips; no UI, ignored at runtime
        // (OnModEnabled logs once when it is true).
        public bool CrispFonts = false;
        [ConfigField("Crisp icons", "Trilinear filtering with anisotropy for interface icons drawn smaller than their source (the UI is authored for 4K). Icons without mipmaps are unchanged.")]
        public bool CrispIcons = true;
        [ConfigField("Show DLSS in Graphics options")]
        public bool ShowInGraphicsOptions = true;
        // Pressed together with Ctrl+Alt (fixed chord, like ContentTool's fit bench Ctrl+Alt+B). No F-keys/Insert/End:
        // the user's keyboard has none, and F4/F5/F9/F10 are the game's quicksave/quickload/report keys anyway.
        // Letters free in the live PhoenixInput map (2026-09-02): b h j k l o p u. D is "Camera Right" - not usable.
        [ConfigField("DLSS on/off key (with Ctrl+Alt)", "Press Ctrl+Alt+<key>")]
        public KeyCode ToggleHotkey = KeyCode.U;        // Ctrl+Alt+U: DLSS (upscaler) on/off (Off <-> the last non-Off mode)
        [ConfigField("Overlay key (with Ctrl+Alt)", "Press Ctrl+Alt+<key>")]
        public KeyCode OverlayHotkey = KeyCode.O;       // Ctrl+Alt+O: benchmark overlay show/hide
        [ConfigField("Show benchmark overlay")]
        public bool ShowOverlay = false;
        [ConfigField("Overlay position")]
        public OverlayCorner OverlayPosition = OverlayCorner.TopCenter;
        [ConfigField("Overlay scale", "0.5 … 3")]
        public float OverlayScale = 1.0f;               // Overlay text size multiplier, 0.5..3
        [ConfigField("Frame rate limit", "Final presented FPS, including generated frames. Off = uncapped; turning it on disables VSync. Also in Options → Screen.")]
        public bool LimitFrameRate = false;             // off = uncapped (the game itself pins 60). VSync still caps at the monitor rate.
        [ConfigField("Max presented FPS", "30 … 300, including generated frames; used when the limit is on")]
        public int FrameRateLimit = 60;                 // final presented ceiling; native cap is floor(this / live FG multiplier)
        [ConfigField("Renderer", "Auto = DirectX 11. DirectX 12 is experimental. Changing it requires a restart.")]
        public RendererMode Renderer = RendererMode.Auto;
        [ConfigField("Upscaler", "Auto picks by GPU: NVIDIA → DLSS, Intel → XeSS, otherwise FSR (XeSS if the AMD DLLs are missing). DLSS needs an NVIDIA RTX GPU; FSR/XeSS need DirectX 12. Switches live.")]
        public UpscalerKind Upscaler = UpscalerKind.Auto;
        [ConfigField("Frame generation", "Off / 2x / 3x / 4x. DirectX 12 with an upscaler active; 3x and 4x need DLSS-G on an RTX 50 GPU.")]
        public FrameGenMode FrameGen = FrameGenMode.Off;
        [ConfigField("Vignette", "Vanilla keeps the game's own vignette; Off removes the darkened frame edges. Also in Options → Graphics.")]
        public VignetteMode Vignette = VignetteMode.Vanilla;
        [ConfigField("Shadow resolution", "Vanilla keeps the graphics preset's value; Very High raises the shadow map size. Also in Options → Graphics.")]
        public ShadowResolutionMode ShadowResolution = ShadowResolutionMode.Vanilla;
        [ConfigField("Anisotropic filtering", "Vanilla leaves per-texture filtering alone; 16x forces 16 samples on every texture. Also in Options → Graphics.")]
        public AnisotropicMode Anisotropic = AnisotropicMode.Vanilla;
        [ConfigField("LOD detail", "0 = the graphics preset's own value. 1.0 … 4.0 keeps higher-detail models at distance (below the preset's value = less detail); costs GPU time and VRAM. Also in Options → Graphics.")]
        public float LodBias = 0f;             // 0 = vanilla (write the captured baseline back); otherwise clamped to 1..4

        // field ID -> (RU label, RU description); English comes from the attribute above.
        private static readonly Dictionary<string, string[]> Ru = new Dictionary<string, string[]>
        {
            { nameof(Mode), new[] { "Режим DLSS", "Выкл, Авто (по разрешению), DLAA, Quality, Balanced, Performance, Ultra Performance. Нужен работающий апскейлер." } },
            { nameof(Sharpness), new[] { "Резкость", "0 = выкл … 100. Проход RCAS после DLSS; также ползунок в Настройки → Графика." } },
            { nameof(Lut), new[] { "LUT-фильтр", "Оригинальная цветокоррекция тактических миссий после темпоральной реконструкции. Только тактические миссии; только сцена, интерфейс не меняется. Также в Настройки → Графика." } },
            { nameof(LutStrength), new[] { "Сила LUT", "0 = оригинал … 100 = полный эффект. Применяется сразу. Только тактические миссии." } },
            { nameof(SceneStyle), new[] { "Стиль сцены", "Выкл, мультфильм или пиксель-арт. Стилизация кодом после реконструкции. Только сцена, интерфейс не меняется." } },
            { nameof(SceneStyleStrength), new[] { "Сила стилизации", "0 = оригинал, 100 = полный эффект. Применяется сразу." } },
            { nameof(PixelSize), new[] { "Размер пикселя", "По умолчанию 4 пикселя экрана. Диапазон 2–16: от мелкой до крупной пикселизации." } },
            { nameof(ColorVision), new[] { "Цветовое зрение", "Выкл, дейтеранопия, протанопия или тританопия. Перераспределяет неразличимые цвета на различимые каналы. Только тактические миссии; только сцена, интерфейс рисуется после этого прохода. Также в Настройки → Графика." } },
            { nameof(LevelsBlack), new[] { "Точка чёрного", "Пиксели темнее этого значения становятся чёрными, остальные растягиваются — тени глубже. 0 = выкл." } },
            { nameof(LevelsWhite), new[] { "Точка белого", "Пиксели ярче этого значения становятся белыми — светлые участки ярче. 255 = выкл." } },
            { nameof(Contrast), new[] { "Контраст", "Опорная точка — средний серый: 100 = выкл, ниже — мягче, выше — контрастнее (тёмные сцены темнеют)." } },
            { nameof(Clarity), new[] { "Чёткость", "Локальный контраст мелких деталей; 0 = выкл. Только сцена, интерфейс не меняется." } },
            { nameof(CrispIcons), new[] { "Чёткие значки", "Трилинейная фильтрация с анизотропией для значков интерфейса, отрисованных меньше исходного размера (интерфейс нарисован под 4K). Значки без мип-уровней не меняются." } },
            { nameof(ShowInGraphicsOptions), new[] { "Показывать DLSS в настройках графики", null } },
            { nameof(ToggleHotkey), new[] { "Клавиша DLSS вкл/выкл (с Ctrl+Alt)", "Нажимайте Ctrl+Alt+<клавиша>" } },
            { nameof(OverlayHotkey), new[] { "Клавиша оверлея (с Ctrl+Alt)", "Нажимайте Ctrl+Alt+<клавиша>" } },
            { nameof(ShowOverlay), new[] { "Показывать оверлей (бенчмарк)", null } },
            { nameof(OverlayPosition), new[] { "Положение оверлея", null } },
            { nameof(OverlayScale), new[] { "Масштаб оверлея", "0.5 … 3" } },
            { nameof(LimitFrameRate), new[] { "Ограничение частоты кадров", "Итоговые FPS с учётом сгенерированных кадров. Выкл = без ограничения; при включении VSync выключается. Также в Настройки → Экран." } },
            { nameof(FrameRateLimit), new[] { "Макс. итоговых FPS", "30 … 300 с учётом сгенерированных кадров; действует при включённом ограничении" } },
            { nameof(Renderer), new[] { "Рендерер", "Авто = DirectX 11. DirectX 12 — экспериментальный. Смена требует перезапуска." } },
            { nameof(Upscaler), new[] { "Апскейлер", "Авто выбирает по видеокарте: NVIDIA → DLSS, Intel → XeSS, иначе FSR (XeSS, если нет DLL AMD). DLSS требует видеокарту NVIDIA RTX; FSR/XeSS требуют DirectX 12. Переключается без перезапуска." } },
            { nameof(FrameGen), new[] { "Генерация кадров", "Выкл / 2x / 3x / 4x. Только DirectX 12 при включённом апскейлере; 3x и 4x — DLSS-G на видеокарте RTX 50." } },
            { nameof(Vignette), new[] { "Виньетка", "Только тактические миссии. «Как в игре» сохраняет виньетку миссии; «Выкл» убирает затемнение по краям кадра. Также в Настройки → Графика." } },
            { nameof(ShadowResolution), new[] { "Разрешение теней", "«Как в игре» — значение выбранного пресета; «Очень высокое» увеличивает размер карты теней. Также в Настройки → Графика." } },
            { nameof(Anisotropic), new[] { "Анизотропная фильтрация", "«Как в игре» ничего не меняет; «16x» включает 16 выборок для всех текстур. Также в Настройки → Графика." } },
            { nameof(LodBias), new[] { "Детализация LOD", "0 = значение выбранного пресета. 1.0 … 4.0 — модели дольше остаются детальными вдали (ниже значения пресета — меньше деталей); расход GPU и видеопамяти растёт. Также в Настройки → Графика." } },
        };

        // ---- Language table: src\Localization\strings.csv, embedded as "Renderforge.strings.csv". Key = the English
        // string exactly as written at the call site; the other columns carry TFTV's language names. A missing file,
        // a bad header or an empty cell fall back to the inline EN/RU pair at the call site.
        private static StringTable table;

        /// <summary>I2 language code: "en", "ru", "zh-CN", … (LocalizationManager.cs:82, a field read after
        /// InitializeIfNeeded). "en" before I2 is up or on any failure.</summary>
        internal static string Language
        {
            get { try { return I2.Loc.LocalizationManager.CurrentLanguageCode ?? "en"; } catch { return "en"; } }
        }

        /// <summary>The table's cell for the current language when it has one; else the inline RU while the game
        /// runs in Russian; else EN. One dictionary lookup per call - it runs from UI code.</summary>
        internal static string Loc(string en, string ru)
        {
            string lang = Language;
            string hit = table != null ? table.Lookup(en, lang) : null;
            if (hit != null) return hit;
            return ru != null && (lang == "ru" || lang.StartsWith("ru-")) ? ru : en;
        }

        /// <summary>Parse the embedded CSV once (OnModEnabled). Any problem: report it, keep the inline fallback.</summary>
        internal static void LoadStrings(Action<string> log)
        {
            table = null;
            try
            {
                using (var stream = typeof(DlssConfig).Assembly.GetManifestResourceStream("Renderforge.strings.csv"))
                {
                    if (stream == null) { log("strings.csv missing from the assembly - EN/RU fallback"); return; }
                    string text;
                    using (var reader = new StreamReader(stream, Encoding.UTF8, true)) text = reader.ReadToEnd();   // BOM dropped
                    string error;
                    var parsed = StringTable.Parse(text, out error);
                    if (parsed == null) { log("strings.csv " + error + " - EN/RU fallback"); return; }
                    table = parsed;
                    log("strings.csv: " + parsed.KeyCount + " keys, " + parsed.LanguageCount + " languages");
                }
            }
            catch (Exception ex) { table = null; log("strings.csv parse failed - EN/RU fallback: " + ex.Message); }
        }

        public override List<ModConfigField> GetConfigFields()
        {
            List<ModConfigField> fields = base.GetConfigFields();
            if (ModSettingsFilter.Building) fields.RemoveAll(field => HiddenFromModSettings.Contains(field.ID));
            foreach (ModConfigField field in fields)
            {
                if (!Ru.TryGetValue(field.ID, out var ru)) continue;
                var en = field.GetText; var enDesc = field.GetDescription;   // the [ConfigField] delegates
                field.GetText = () => Loc(en?.Invoke(), ru[0]);
                field.GetDescription = () => Loc(enDesc?.Invoke(), ru[1]);
            }
            return fields;
        }
    }
}
