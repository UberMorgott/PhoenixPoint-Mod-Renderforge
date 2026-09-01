using PhoenixPoint.Modding;

namespace DlssMod
{
    /// <summary>Public fields = the in-game mod settings UI + ModConfig.json (ModConfig.GetConfigFields).</summary>
    public class DlssConfig : ModConfig
    {
        public DlssMode Mode = DlssMode.Auto;
        public bool ShowInGraphicsOptions = true;
    }
}
