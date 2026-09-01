using System.Collections.Generic;
using PhoenixPoint.Modding;
using UnityEngine;

namespace DlssMod
{
    /// <summary>Passthrough = native CopyResource instead of NGX (pipeline proof); Depth/MotionVectors = the present
    /// camera shows depthRT/mvRT (stretched, raw values) instead of the DLSS output.</summary>
    public enum DebugView { None, Passthrough, Depth, MotionVectors }

    public enum OverlayCorner { TopLeft, TopCenter, TopRight, BottomCenter }

    /// <summary>Public fields = the in-game mod settings UI + ModConfig.json (ModConfig.GetConfigFields).
    /// [ConfigField] = the English label; GetConfigFields swaps in Russian when the game runs in Russian
    /// (same shape as PerkOracle's OracleConfig.GetConfigFields, minus the CSV: two languages, inline).</summary>
    public class DlssConfig : ModConfig
    {
        [ConfigField("DLSS mode", "Off, Auto (by resolution), DLAA, Quality, Balanced, Performance, Ultra Performance")]
        public DlssMode Mode = DlssMode.Auto;
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

        // field ID -> (RU label, RU description); English comes from the attribute above.
        private static readonly Dictionary<string, string[]> Ru = new Dictionary<string, string[]>
        {
            { nameof(Mode), new[] { "Режим DLSS", "Выкл, Авто (по разрешению), DLAA, Quality, Balanced, Performance, Ultra Performance" } },
            { nameof(ShowInGraphicsOptions), new[] { "Показывать DLSS в настройках графики", null } },
            { nameof(ToggleHotkey), new[] { "Клавиша DLSS вкл/выкл (с Ctrl+Alt)", "Нажимайте Ctrl+Alt+<клавиша>" } },
            { nameof(OverlayHotkey), new[] { "Клавиша оверлея (с Ctrl+Alt)", "Нажимайте Ctrl+Alt+<клавиша>" } },
            { nameof(ShowOverlay), new[] { "Показывать оверлей (бенчмарк)", null } },
            { nameof(OverlayPosition), new[] { "Положение оверлея", null } },
            { nameof(OverlayScale), new[] { "Масштаб оверлея", "0.5 … 3" } },
            { nameof(LimitFrameRate), new[] { "Ограничение частоты кадров", "Выкл = без ограничения. Также в Настройки → Экран." } },
            { nameof(FrameRateLimit), new[] { "Макс. FPS", "30 … 300, действует при включённом ограничении" } },
            { nameof(DebugView), new[] { "Отладочный вид", "Для разработчика: Passthrough / Depth / Motion vectors" } },
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
