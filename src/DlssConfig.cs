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
        // Never F4/F5/F9/F10 (quicksave/quickload/report) nor F12 (Steam screenshot): a mod key there fires both.
        public KeyCode ToggleHotkey = KeyCode.F11;      // DLSS on/off: Off <-> the last non-Off mode
        public KeyCode OverlayHotkey = KeyCode.F8;      // benchmark overlay show/hide
        public bool ShowOverlay = false;
        public OverlayCorner OverlayPosition = OverlayCorner.TopCenter;
        public DebugView DebugView = DebugView.None;
    }
}
