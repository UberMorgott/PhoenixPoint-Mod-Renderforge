using System;
using PhoenixPoint.Modding;
using UnityEngine;

namespace DlssMod
{
    public class DlssMod : ModMain
    {
        // `new`: hides ModMain.Instance (the loader's ModInstance), which we reach via base.Instance.
        public static new DlssMod Instance { get; private set; }
        public static bool Available { get; private set; }
        public static int InitCode { get; private set; } = -1;
        /// <summary>ModEntry.Directory (ModEntry.cs:13). The loader uses Assembly.Load(byte[]) (ModSDKContext.cs:63),
        /// so Assembly.Location is empty and this is the only source of the mod folder.</summary>
        public static string ModDir { get; private set; }
        public DlssConfig Cfg => (DlssConfig)Config;

        // Kept alive for the life of the mod: NGX holds the D3D11 device it was taken from.
        private static Texture2D probeTex;

        public override void OnModEnabled()
        {
            Instance = this;
            ModDir = base.Instance?.Entry?.Directory ?? ".";
            Available = false;
            try
            {
                if (!Native.Load(ModDir))
                {
                    InitCode = Native.DLSS_ERR_INIT_FAILED;
                    Logger.LogInfo("DLSS unavailable (code " + InitCode + "): DlssNative.dll failed to load from " + ModDir);
                    return;
                }
                probeTex = new Texture2D(1, 1, TextureFormat.RGBA32, false);
                InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
                Available = InitCode == Native.DLSS_OK;
            }
            catch (Exception ex)
            {
                InitCode = Native.DLSS_ERR_INIT_FAILED;
                Logger.LogError("DLSS init THREW " + ex.Message);
            }
            Logger.LogInfo(Available ? "DLSS available" : "DLSS unavailable (code " + InitCode + "): " + Reason(InitCode));
        }

        public override void OnModDisabled()
        {
            if (InitCode == Native.DLSS_OK) { try { Native.Dlss_Shutdown(); } catch (Exception) { } }
            Available = false;
            InitCode = -1;
            Instance = null;
        }

        public override void OnConfigChanged()
        {
            Logger.LogInfo("DLSS mode = " + Cfg.Mode);
            // phase 2b: DlssDriver.Apply(Cfg)
        }

        private static string Reason(int code)
        {
            switch (code)
            {
                case Native.DLSS_ERR_NO_DEVICE: return "no D3D11 device behind the probe texture";
                case Native.DLSS_ERR_INIT_FAILED: return "NGX init failed (see nvsdk_ngx.log in the mod folder)";
                case Native.DLSS_ERR_NOT_AVAILABLE: return "DLSS not supported on this GPU";
                case Native.DLSS_ERR_NEEDS_DRIVER: return "NVIDIA driver too old for this DLSS";
                default: return "unknown";
            }
        }
    }
}
