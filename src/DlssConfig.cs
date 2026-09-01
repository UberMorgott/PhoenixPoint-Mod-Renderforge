using PhoenixPoint.Modding;
using UnityEngine;

namespace DlssMod
{
    /// <summary>Passthrough = native CopyResource instead of NGX (pipeline proof); Depth/MotionVectors = the present
    /// camera shows depthRT/mvRT (stretched, raw values) instead of the DLSS output.</summary>
    public enum DebugView { None, Passthrough, Depth, MotionVectors }

    public enum OverlayCorner { TopLeft, TopCenter, TopRight, BottomCenter }

    /// <summary>Public fields = the in-game mod settings UI + ModConfig.json (ModConfig.GetConfigFields).</summary>
    public class DlssConfig : ModConfig
    {
        public DlssMode Mode = DlssMode.Auto;
        public bool ShowInGraphicsOptions = true;
        // Pressed together with Ctrl+Alt (fixed chord, like ContentTool's fit bench Ctrl+Alt+B). No F-keys/Insert/End:
        // the user's keyboard has none, and F4/F5/F9/F10 are the game's quicksave/quickload/report keys anyway.
        // Letters free in the live PhoenixInput map (2026-09-02): b h j k l o p u. D is "Camera Right" - not usable.
        public KeyCode ToggleHotkey = KeyCode.U;        // Ctrl+Alt+U: DLSS (upscaler) on/off (Off <-> the last non-Off mode)
        public KeyCode OverlayHotkey = KeyCode.O;       // Ctrl+Alt+O: benchmark overlay show/hide
        public bool ShowOverlay = false;
        public OverlayCorner OverlayPosition = OverlayCorner.TopCenter;
        public float OverlayScale = 1.0f;               // Overlay text size multiplier, 0.5..3
        public int FrameRateLimit = 0;                  // 0 = uncapped; the game itself pins 60. VSync still caps at the monitor rate.
        public DebugView DebugView = DebugView.None;
    }
}
