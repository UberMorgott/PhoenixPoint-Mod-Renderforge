using System;
using Base.Cameras;
using Base.Core;
using Base.Levels;
using HarmonyLib;
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
        private bool patched;

        public override void OnModEnabled()
        {
            Instance = this;
            ModDir = base.Instance?.Entry?.Directory ?? ".";
            Available = false;
            ApplyFrameRate();
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
                Available = InitCode == Native.DLSS_OK && SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Direct3D11;
            }
            catch (Exception ex)
            {
                InitCode = Native.DLSS_ERR_INIT_FAILED;
                Logger.LogError("DLSS init THREW " + ex.Message);
            }
            Logger.LogInfo(Available ? "DLSS available" : "DLSS unavailable (code " + InitCode + "): " + Reason(InitCode));
            if (!Available) return;
            try
            {
                DlssDriver.Create();
                ((Harmony)HarmonyInstance).PatchAll(typeof(DlssMod).Assembly);
                patched = true;
                AttachAndApply();
            }
            catch (Exception ex) { Logger.LogError("DLSS enable THREW " + ex); }
        }

        public override void OnModDisabled()
        {
            try
            {
                DlssDriver.Instance?.Apply(DlssMode.Off, DebugView.None);
                if (DlssDriver.Instance != null) UnityEngine.Object.Destroy(DlssDriver.Instance.gameObject);
                Overlay.Destroy();
                if (patched) { ((Harmony)HarmonyInstance).UnpatchAll(((Harmony)HarmonyInstance).Id); patched = false; }
                if (InitCode == Native.DLSS_OK) Native.Dlss_Shutdown();
            }
            catch (Exception ex) { Logger.LogError("DLSS disable THREW " + ex); }
            Application.targetFrameRate = 60;   // the game's own value (OptionsManager.cs:505)
            Available = false;
            InitCode = -1;
            Instance = null;
        }

        public override void OnLevelStart(Level level) => AttachAndApply();

        /// <summary>Release before the level's camera goes away; the next OnLevelStart re-attaches.</summary>
        public override void OnLevelEnd(Level level) => DlssDriver.Instance?.Apply(DlssMode.Off, Cfg.DebugView);

        public override void OnConfigChanged()
        {
            Logger.LogInfo("DLSS mode = " + Cfg.Mode + " view = " + Cfg.DebugView);
            ApplyFrameRate();
            AttachAndApply();
        }

        /// <summary>Also the InitVideoOptions postfix (Patches.cs), which runs on the game's own SetFrameRateLimit(60).</summary>
        public static void ApplyFrameRate()
        {
            var cfg = Instance?.Cfg;
            if (cfg == null) return;
            Application.targetFrameRate = cfg.LimitFrameRate ? Mathf.Clamp(cfg.FrameRateLimit, 30, 300) : -1;
        }

        /// <summary>ModManager.SaveModConfig (ModManager.cs:120): the same path the mod-manager screen uses (UIStateModManagment.cs:137).</summary>
        public static void SaveConfig()
        {
            try { ModManager.GetInstance().SaveModConfig(); }
            catch (Exception ex) { Instance?.Logger.LogError("DLSS config save failed: " + ex.Message); }
        }

        // ---- hotkey handlers (also the PPCLI keypress substitute: {"op":"invoke","type":"DlssMod.DlssMod","assembly":"DLSS","member":"Toggle"})
        private static DlssMode lastOn = DlssMode.Auto;   // ponytail: not persisted; after a restart in Off, F11 restores Auto

        public static string Toggle()
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            if (m.Cfg.Mode == DlssMode.Off) m.Cfg.Mode = lastOn;
            else { lastOn = m.Cfg.Mode; m.Cfg.Mode = DlssMode.Off; }
            m.Logger.LogInfo("DLSS hotkey: mode = " + m.Cfg.Mode);
            m.AttachAndApply();
            SaveConfig();
            return GetStatus();
        }

        public static string ToggleOverlay()
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.ShowOverlay = !m.Cfg.ShowOverlay;
            Overlay.Apply(m.Cfg);
            SaveConfig();
            return "overlay=" + m.Cfg.ShowOverlay + " at " + m.Cfg.OverlayPosition;
        }

        public static string SetOverlay(string corner)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.OverlayPosition = (OverlayCorner)Enum.Parse(typeof(OverlayCorner), corner, true);
            m.Cfg.ShowOverlay = true;
            Overlay.Apply(m.Cfg);
            return "overlay=" + m.Cfg.ShowOverlay + " at " + m.Cfg.OverlayPosition;
        }

        private void AttachAndApply()
        {
            var d = DlssDriver.Instance;
            if (d == null) return;
            if (Cfg.Mode != DlssMode.Off) lastOn = Cfg.Mode;
            Overlay.Apply(Cfg);
            var cam = GameUtl.GameComponent<CameraManager>()?.Camera;
            if (cam == null) return;          // main menu without CameraManager: wait for the next level
            d.Attach(cam);
            d.Apply(Cfg.Mode, Cfg.DebugView);
        }

        // ---- PPCLI `connect call` surface: {"op":"invoke","type":"DlssMod.DlssMod","assembly":"DLSS","member":"SetMode","args":["DLAA","None"]}
        public static string SetMode(string mode, string view)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.Mode = (DlssMode)Enum.Parse(typeof(DlssMode), mode, true);
            m.Cfg.DebugView = (DebugView)Enum.Parse(typeof(DebugView), view, true);
            m.AttachAndApply();
            return GetStatus();
        }

        /// <summary>PPCLI substitute for the slider: {"member":"SetSharpness","args":[100]}. Live next frame + saved.</summary>
        public static string SetSharpness(int value)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.Sharpness = Mathf.Clamp(value, 0, 100);
            SaveConfig();
            return "sharpness=" + m.Cfg.Sharpness;
        }

        public static string GetStatus() => DlssDriver.Instance?.Status ?? ("no driver; available=" + Available + " init=" + InitCode);

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
