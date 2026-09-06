using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using PhoenixPoint.Modding;
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
            nameof(SceneStyle), nameof(SceneStyleStrength), nameof(PixelSize), nameof(CrispFonts),
            nameof(Vignette), nameof(ShadowResolution), nameof(Anisotropic), nameof(LodBias),
            nameof(ColorVision)
        };

        [ConfigField("DLSS mode", "Off, Auto (by resolution), DLAA, Quality, Balanced, Performance, Ultra Performance. Needs an active upscaler.")]
        public RenderforgeMode Mode = RenderforgeMode.Auto;
        [ConfigField("Sharpness", "0 = off … 100. RCAS pass after DLSS; also a slider in Options → Graphics.")]
        public int Sharpness = 40;                      // 0..100 -> RCAS 0..1, applied every frame, live
        [ConfigField("LUT filter", "Original tactical-mission colour grade applied after temporal reconstruction. Tactical missions only; scene only, the HUD stays unchanged. Also in Options → Graphics.")]
        public LutPreset Lut = LutPreset.Off;
        [ConfigField("LUT strength", "0 = original image … 100 = full grade. Applied live. Tactical missions only.")]
        public int LutStrength = 100;
        [ConfigField("Scene style", "Off, Cartoon or PixelArt. Code-only scene filtering after reconstruction. Scene only; the HUD stays unchanged.")]
        public SceneStyle SceneStyle = SceneStyle.Off;
        [ConfigField("Style strength", "0 = original image, 100 = full style. Applied live.")]
        public int SceneStyleStrength = 100;
        [ConfigField("Pixel block size", "PixelArt: default 4 actual output pixels. Adjust from 2 to 16 for finer or stronger pixelation.")]
        public int PixelSize = 4;
        [ConfigField("Colour vision", "Off, Deuteranopia, Protanopia or Tritanopia. Redistributes colours the eye cannot separate onto channels it can. Tactical missions only; scene only, the interface is drawn after this pass. Also in Options → Graphics.")]
        public ColorVisionMode ColorVision = ColorVisionMode.None;
        [ConfigField("Crisp fonts", "Sharper supported interface text with its original layout. Also in Options → Screen.")]
        public bool CrispFonts = true;
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
        [ConfigField("Vignette", "Tactical missions only. Vanilla keeps the mission's own vignette; Off removes the darkened frame edges. Also in Options → Graphics.")]
        public VignetteMode Vignette = VignetteMode.Vanilla;
        [ConfigField("Shadow resolution", "Vanilla keeps the graphics preset's value; Very High raises the shadow map size. Also in Options → Graphics.")]
        public ShadowResolutionMode ShadowResolution = ShadowResolutionMode.Vanilla;
        [ConfigField("Anisotropic filtering", "Vanilla leaves per-texture filtering alone; 16x forces 16 samples on every texture. Also in Options → Graphics.")]
        public AnisotropicMode Anisotropic = AnisotropicMode.Vanilla;
        [ConfigField("LOD detail", "0 = vanilla. 1.0 … 4.0 keeps higher-detail models at distance; costs GPU time and VRAM. Also in Options → Graphics.")]
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
            { nameof(CrispFonts), new[] { "Чёткие шрифты", "Повышает чёткость поддерживаемого текста интерфейса, сохраняя расположение букв. Также в Настройки → Экран." } },
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
            { nameof(LodBias), new[] { "Детализация LOD", "0 = как в игре. 1.0 … 4.0 — модели дольше остаются детальными вдали; расход GPU и видеопамяти растёт. Также в Настройки → Графика." } },
        };

        // ---- Language table: src\Localization\strings.csv, embedded as "Renderforge.strings.csv". Key = the English
        // string exactly as written at the call site; the other columns carry TFTV's language names. A missing file,
        // a bad header or an empty cell fall back to the inline EN/RU pair at the call site.
        private static readonly Dictionary<string, string> ColumnCodes = new Dictionary<string, string>
        {
            { "Russian", "ru" }, { "Chinese (Simplified)", "zh-CN" }, { "French", "fr" }, { "German", "de" },
            { "Italian", "it" }, { "Polish", "pl" }, { "Spanish", "es" }
        };
        private static Dictionary<string, int> columnByCode;      // I2 language code -> cell index
        private static Dictionary<string, string[]> strings;      // EN key -> row cells

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
            string[] row; int col;
            if (strings != null && en != null && strings.TryGetValue(en, out row)
                && ColumnOf(lang, out col) && col < row.Length && row[col].Length > 0)
                return row[col];
            return ru != null && (lang == "ru" || lang.StartsWith("ru-")) ? ru : en;
        }

        private static bool ColumnOf(string lang, out int col)
        {
            col = 0;
            if (columnByCode == null) return false;
            if (columnByCode.TryGetValue(lang, out col)) return true;
            int dash = lang.IndexOf('-');
            return dash > 0 && columnByCode.TryGetValue(lang.Substring(0, dash), out col);
        }

        /// <summary>Parse the embedded CSV once (OnModEnabled). Any problem: report it, keep the inline fallback.</summary>
        internal static void LoadStrings(Action<string> log)
        {
            strings = null; columnByCode = null;
            try
            {
                using (var stream = typeof(DlssConfig).Assembly.GetManifestResourceStream("Renderforge.strings.csv"))
                {
                    if (stream == null) { log("strings.csv missing from the assembly - EN/RU fallback"); return; }
                    List<string[]> rows;
                    using (var reader = new StreamReader(stream, Encoding.UTF8, true)) rows = ParseCsv(reader.ReadToEnd());   // BOM dropped
                    if (rows.Count == 0 || rows[0][0] != "Key") { log("strings.csv header must start with Key - EN/RU fallback"); return; }
                    var cols = new Dictionary<string, int>();
                    for (int i = 1; i < rows[0].Length; i++)
                    {
                        string code;
                        if (!ColumnCodes.TryGetValue(rows[0][i], out code)) { log("strings.csv unknown column '" + rows[0][i] + "' - EN/RU fallback"); return; }
                        cols[code] = i;
                    }
                    var table = new Dictionary<string, string[]>();
                    for (int r = 1; r < rows.Count; r++)
                        if (rows[r].Length > 1 && rows[r][0].Length > 0) table[rows[r][0]] = rows[r];
                    columnByCode = cols; strings = table;
                    log("strings.csv: " + table.Count + " keys, " + cols.Count + " languages");
                }
            }
            catch (Exception ex) { strings = null; columnByCode = null; log("strings.csv parse failed - EN/RU fallback: " + ex.Message); }
        }

        /// <summary>RFC 4180: a quoted field may hold commas, quotes ("" = one quote) and newlines. Blank lines skipped.</summary>
        internal static List<string[]> ParseCsv(string text)
        {
            var rows = new List<string[]>();
            var row = new List<string>();
            var cell = new StringBuilder();
            bool quoted = false;
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (quoted)
                {
                    if (c == '"' && i + 1 < text.Length && text[i + 1] == '"') { cell.Append('"'); i++; }
                    else if (c == '"') quoted = false;
                    else cell.Append(c);
                }
                else if (c == '"') quoted = true;
                else if (c == ',') { row.Add(cell.ToString()); cell.Length = 0; }
                else if (c == '\r') { }
                else if (c == '\n')
                {
                    row.Add(cell.ToString()); cell.Length = 0;
                    if (row.Count > 1 || row[0].Length > 0) rows.Add(row.ToArray());
                    row.Clear();
                }
                else cell.Append(c);
            }
            if (cell.Length > 0 || row.Count > 0) { row.Add(cell.ToString()); rows.Add(row.ToArray()); }
            return rows;
        }

        [Conditional("DEBUG")]
        internal static void SelfTest()
        {
            var rows = ParseCsv("Key,Russian\r\n\"a,b\",\"say \"\"hi\"\"\nthere\"\n\nplain,\n");
            if (rows.Count != 3 || rows[1][0] != "a,b" || rows[1][1] != "say \"hi\"\nthere" || rows[2][0] != "plain" || rows[2][1] != "")
                throw new InvalidOperationException("DlssConfig.SelfTest: CSV parser broke");
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
