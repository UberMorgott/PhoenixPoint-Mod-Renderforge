using PhoenixPoint.Modding;

namespace DlssMod
{
    /// <summary>Passthrough = native CopyResource instead of NGX (pipeline proof); Depth/MotionVectors = the present
    /// camera shows depthRT/mvRT (stretched, raw values) instead of the DLSS output.</summary>
    public enum DebugView { None, Passthrough, Depth, MotionVectors }

    /// <summary>Public fields = the in-game mod settings UI + ModConfig.json (ModConfig.GetConfigFields).</summary>
    public class DlssConfig : ModConfig
    {
        public DlssMode Mode = DlssMode.Auto;
        public bool ShowInGraphicsOptions = true;
        public DebugView DebugView = DebugView.None;
    }
}
