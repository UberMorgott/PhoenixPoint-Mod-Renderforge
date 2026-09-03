using System.Collections.Generic;
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

    /// <summary>Public fields = the in-game mod settings UI + ModConfig.json (ModConfig.GetConfigFields).
    /// [ConfigField] = the English label; GetConfigFields swaps in Russian when the game runs in Russian
    /// (same shape as PerkOracle's OracleConfig.GetConfigFields, minus the CSV: two languages, inline).</summary>
    public class DlssConfig : ModConfig
    {
        [ConfigField("DLSS mode", "Off, Auto (by resolution), DLAA, Quality, Balanced, Performance, Ultra Performance")]
        public RenderforgeMode Mode = RenderforgeMode.Auto;
        [ConfigField("Sharpness", "0 = off … 100. RCAS pass after DLSS; also a slider in Options → Graphics.")]
        public int Sharpness = 40;                      // 0..100 -> RCAS 0..1, applied every frame, live
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
        [ConfigField("Frame rate limit", "Off = uncapped. Also in Options → Screen.")]
        public bool LimitFrameRate = false;             // off = uncapped (the game itself pins 60). VSync still caps at the monitor rate.
        [ConfigField("Max FPS", "30 … 300, used when the limit is on")]
        public int FrameRateLimit = 60;                 // used only when LimitFrameRate, clamped 30..300
        [ConfigField("Debug view", "Developer: Passthrough / Depth / Motion vectors")]
        public DebugView DebugView = DebugView.None;
        [ConfigField("Renderer", "Auto = DirectX 11. DirectX 12 is experimental and needs a restart.")]
        public RendererMode Renderer = RendererMode.Auto;
        [ConfigField("Upscaler", "Auto picks by GPU: NVIDIA → DLSS, Intel → XeSS, otherwise FSR (XeSS if the AMD DLLs are missing). FSR/XeSS need DirectX 12. Switches live.")]
        public UpscalerKind Upscaler = UpscalerKind.Auto;
        [ConfigField("Frame generation", "Off / 2x / 3x / 4x. DirectX 12 only. 3x and 4x need DLSS-G on an RTX 50 GPU.")]
        public FrameGenMode FrameGen = FrameGenMode.Off;
        [ConfigField("MV jittered (diagnostic)", "Tell the upscaler the motion vectors carry the jitter (NGX MVJittered / FSR jitter cancellation / XeSS JITTERED_MV). Re-creates the feature.")]
        public bool MvJittered = false;
        [ConfigField("D3D12 sRGB views (diagnostic)", "DirectX 12: hand the upscaler an sRGB view of the colour input (it decodes to linear) and tag the output sRGB so the present encodes. Re-creates the feature.")]
        public bool D3D12SrgbViews = false;
        [ConfigField("D3D12 sRGB colour descriptor (diagnostic)", "DirectX 12: create the colour input from an explicit R8G8B8A8_SRGB descriptor so the render-target view is sRGB. Re-creates the feature.")]
        public bool D3D12ColorDesc = false;
        // Diagnostic jitter knobs (D3D12 detail-loss hunt): the REPORTED offset (Dlss_SetFrame) vs the RENDERED one (projection).
        [ConfigField("Jitter report sign X (diagnostic)", "+1 / -1, multiplies only the x offset reported to the upscaler")]
        public int JitterReportSignX = 1;
        [ConfigField("Jitter report sign Y (diagnostic)", "+1 / -1, multiplies only the y offset reported to the upscaler")]
        public int JitterReportSignY = 1;
        [ConfigField("Jitter scale (diagnostic)", "Scales the rendered projection jitter AND the reported offset. 0 = no jitter.")]
        public float JitterScale = 1f;
        [ConfigField("Jitter report swap XY (diagnostic)", "Swap x/y of the reported offset only")]
        public bool JitterReportSwapXY = false;
        [ConfigField("Constant jitter (diagnostic)", "Replace the Halton sample with JitterConstX/Y (render-res pixels) every frame; JitterScale still applies.")]
        public bool JitterConstEnabled = false;
        [ConfigField("Constant jitter X (diagnostic)", "render-res pixels, used when Constant jitter is on")]
        public float JitterConstX = 0f;
        [ConfigField("Constant jitter Y (diagnostic)", "render-res pixels, used when Constant jitter is on")]
        public float JitterConstY = 0f;
        [ConfigField("Force reset (diagnostic)", "Pass the history-reset flag to the upscaler every frame (NGX InReset / FSR reset / XeSS resetHistory).")]
        public bool ForceReset = false;

        // field ID -> (RU label, RU description); English comes from the attribute above.
        private static readonly Dictionary<string, string[]> Ru = new Dictionary<string, string[]>
        {
            { nameof(Mode), new[] { "Режим DLSS", "Выкл, Авто (по разрешению), DLAA, Quality, Balanced, Performance, Ultra Performance" } },
            { nameof(Sharpness), new[] { "Резкость", "0 = выкл … 100. Проход RCAS после DLSS; также ползунок в Настройки → Графика." } },
            { nameof(ShowInGraphicsOptions), new[] { "Показывать DLSS в настройках графики", null } },
            { nameof(ToggleHotkey), new[] { "Клавиша DLSS вкл/выкл (с Ctrl+Alt)", "Нажимайте Ctrl+Alt+<клавиша>" } },
            { nameof(OverlayHotkey), new[] { "Клавиша оверлея (с Ctrl+Alt)", "Нажимайте Ctrl+Alt+<клавиша>" } },
            { nameof(ShowOverlay), new[] { "Показывать оверлей (бенчмарк)", null } },
            { nameof(OverlayPosition), new[] { "Положение оверлея", null } },
            { nameof(OverlayScale), new[] { "Масштаб оверлея", "0.5 … 3" } },
            { nameof(LimitFrameRate), new[] { "Ограничение частоты кадров", "Выкл = без ограничения. Также в Настройки → Экран." } },
            { nameof(FrameRateLimit), new[] { "Макс. FPS", "30 … 300, действует при включённом ограничении" } },
            { nameof(DebugView), new[] { "Отладочный вид", "Для разработчика: Passthrough / Depth / Motion vectors" } },
            { nameof(Renderer), new[] { "Рендерер", "Авто = DirectX 11. DirectX 12 — экспериментальный, требуется перезапуск." } },
            { nameof(Upscaler), new[] { "Апскейлер", "Авто выбирает по видеокарте: NVIDIA → DLSS, Intel → XeSS, иначе FSR (XeSS, если нет DLL AMD). FSR/XeSS требуют DirectX 12. Смена требует перезапуска." } },
            { nameof(FrameGen), new[] { "Генерация кадров", "Выкл / 2x / 3x / 4x. Только DirectX 12. 3x и 4x — DLSS-G на видеокарте RTX 50." } },
            { nameof(MvJittered), new[] { "MV с джиттером (диагностика)", "Сообщить апскейлеру, что векторы движения содержат джиттер (NGX MVJittered / FSR jitter cancellation / XeSS JITTERED_MV). Пересоздаёт feature." } },
            { nameof(D3D12SrgbViews), new[] { "D3D12 sRGB-виды (диагностика)", "DirectX 12: отдать апскейлеру sRGB-вид входного цвета (он декодирует в линейный) и пометить выход как sRGB, чтобы вывод закодировал. Пересоздаёт feature." } },
            { nameof(D3D12ColorDesc), new[] { "D3D12 sRGB-дескриптор цвета (диагностика)", "DirectX 12: создавать входной цвет из явного дескриптора R8G8B8A8_SRGB, чтобы render-target view был sRGB. Пересоздаёт feature." } },
            { nameof(JitterReportSignX), new[] { "Знак джиттера X (диагностика)", "+1 / -1, множитель только для x-смещения, сообщаемого апскейлеру" } },
            { nameof(JitterReportSignY), new[] { "Знак джиттера Y (диагностика)", "+1 / -1, множитель только для y-смещения, сообщаемого апскейлеру" } },
            { nameof(JitterScale), new[] { "Масштаб джиттера (диагностика)", "Масштабирует джиттер проекции И сообщаемое смещение. 0 = без джиттера." } },
            { nameof(JitterReportSwapXY), new[] { "Поменять X/Y джиттера (диагностика)", "Поменять местами x/y только сообщаемого смещения" } },
            { nameof(JitterConstEnabled), new[] { "Постоянный джиттер (диагностика)", "Заменять выборку Халтона на JitterConstX/Y (пиксели рендера) каждый кадр; JitterScale тоже применяется." } },
            { nameof(JitterConstX), new[] { "Постоянный джиттер X (диагностика)", "пиксели рендера, при включённом постоянном джиттере" } },
            { nameof(JitterConstY), new[] { "Постоянный джиттер Y (диагностика)", "пиксели рендера, при включённом постоянном джиттере" } },
            { nameof(ForceReset), new[] { "Принудительный сброс (диагностика)", "Передавать апскейлеру флаг сброса истории каждый кадр (NGX InReset / FSR reset / XeSS resetHistory)." } },
        };

        /// <summary>True while the game runs in Russian (I2 LocalizationManager.CurrentLanguage, "English"/"Russian"/…).</summary>
        internal static bool IsRussian
        {
            get { try { return I2.Loc.LocalizationManager.CurrentLanguage == "Russian"; } catch { return false; } }
        }

        internal static string Loc(string en, string ru) => IsRussian && ru != null ? ru : en;

        public override List<ModConfigField> GetConfigFields()
        {
            List<ModConfigField> fields = base.GetConfigFields();
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
