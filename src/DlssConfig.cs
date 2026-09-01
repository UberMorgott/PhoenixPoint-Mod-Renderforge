using PhoenixPoint.Modding;

namespace DlssMod
{
    /// <summary>Passthrough = native CopyResource instead of NGX (pipeline proof); Depth/MotionVectors = the present
    /// camera shows depthRT/mvRT (stretched, raw values) instead of the DLSS output.</summary>
    public enum DebugView { None, Passthrough, Depth, MotionVectors }

    /// <summary>Public fields = the in-game mod settings UI + ModConfig.json (ModConfig.GetConfigFields).</summary>
    public class DlssConfig : ModConfig
    {
        // ponytail: DLAA default until phase 3 fixes mouse picking under a reduced render res; then Auto.
        public DlssMode Mode = DlssMode.DLAA;
        public bool ShowInGraphicsOptions = true;
        public DebugView DebugView = DebugView.None;
    }
}
