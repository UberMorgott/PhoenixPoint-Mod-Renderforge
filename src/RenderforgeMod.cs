using System;
using Base.Cameras;
using Base.Core;
using Base.Levels;
using HarmonyLib;
using PhoenixPoint.Modding;
using UnityEngine;

namespace Renderforge
{
    public class RenderforgeMod : ModMain
    {
        // `new`: hides ModMain.Instance (the loader's ModInstance), which we reach via base.Instance.
        public static new RenderforgeMod Instance { get; private set; }
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
            // D3D11: the NGX path as before. D3D12: the mod stays alive for the pickers, the overlay and the
            // PPv2 repair, but the native DLSS init is skipped (D3D12 upscaling is Phase 2).
            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Direct3D11)
            {
                try
                {
                    if (!Native.Load(ModDir))
                    {
                        InitCode = Native.DLSS_ERR_INIT_FAILED;
                        Logger.LogInfo("DLSS unavailable (code " + InitCode + "): RenderforgeNative.dll failed to load from " + ModDir);
                    }
                    else
                    {
                        probeTex = new Texture2D(1, 1, TextureFormat.RGBA32, false);
                        InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
                        Available = InitCode == Native.DLSS_OK;
                    }
                }
                catch (Exception ex)
                {
                    InitCode = Native.DLSS_ERR_INIT_FAILED;
                    Logger.LogError("Renderforge init THREW " + ex.Message);
                }
                Logger.LogInfo(Available ? "DLSS available" : "DLSS unavailable (code " + InitCode + "): " + Reason(InitCode));
            }
            else
            {
                InitCode = Native.DLSS_ERR_NOT_AVAILABLE;
                // Phase 2 spike: load the shim so Unity gets a chance to call UnityPluginLoad, then report what arrived.
                int iface = Native.Load(ModDir) ? Native.UnityIface() : -2;
                Logger.LogInfo("Renderforge: " + SystemInfo.graphicsDeviceType + " - native DLSS init skipped ("
                               + Availability.Reason(Feature.Dlss) + ") unityIface=" + iface);
            }
            try
            {
                if (Available) DlssDriver.Create();
                ((Harmony)HarmonyInstance).PatchAll(typeof(RenderforgeMod).Assembly);
                patched = true;
                if (RendererSwitch.Wants12(Cfg) && !Availability.IsD3D12) RendererSwitch.ArmStartupPrompt();
                AttachAndApply();
            }
            catch (Exception ex) { Logger.LogError("Renderforge enable THREW " + ex); }
        }

        public override void OnModDisabled()
        {
            try
            {
                DlssDriver.Instance?.Apply(RenderforgeMode.Off, DebugView.None);
                if (DlssDriver.Instance != null) UnityEngine.Object.Destroy(DlssDriver.Instance.gameObject);
                Overlay.Destroy();
                Pickers.Clear();
                if (patched) { ((Harmony)HarmonyInstance).UnpatchAll(((Harmony)HarmonyInstance).Id); patched = false; }
                if (InitCode == Native.DLSS_OK) Native.Dlss_Shutdown();
            }
            catch (Exception ex) { Logger.LogError("Renderforge disable THREW " + ex); }
            Application.targetFrameRate = 60;   // the game's own value (OptionsManager.cs:505)
            Available = false;
            InitCode = -1;
            Instance = null;
        }

        public override void OnLevelStart(Level level) { AttachAndApply(); MipBias.Reapply(); D3D12Fix.Apply(); }   // Reapply covers a level that starts with the generation still live

        /// <summary>Release before the level's camera goes away; the next OnLevelStart re-attaches.</summary>
        public override void OnLevelEnd(Level level) => DlssDriver.Instance?.Apply(RenderforgeMode.Off, Cfg.DebugView);

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
            catch (Exception ex) { Instance?.Logger.LogError("Renderforge config save failed: " + ex.Message); }
        }

        // ---- hotkey handlers (also the PPCLI keypress substitute: {"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"Toggle"})
        private static RenderforgeMode lastOn = RenderforgeMode.Auto;   // ponytail: not persisted; after a restart in Off, F11 restores Auto

        public static string Toggle()
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            if (m.Cfg.Mode == RenderforgeMode.Off) m.Cfg.Mode = lastOn;
            else { lastOn = m.Cfg.Mode; m.Cfg.Mode = RenderforgeMode.Off; }
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
            Overlay.Apply(Cfg);               // before the driver check: under D3D12 there is no driver at all
            var d = DlssDriver.Instance;
            if (d == null) return;
            if (Cfg.Mode != RenderforgeMode.Off) lastOn = Cfg.Mode;
            var cam = GameUtl.GameComponent<CameraManager>()?.Camera;
            if (cam == null) return;          // main menu without CameraManager: wait for the next level
            d.Attach(cam);
            d.Apply(Cfg.Mode, Cfg.DebugView);
        }

        // ---- PPCLI `connect call` surface: {"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["DLAA","None"]}
        public static string SetMode(string mode, string view)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.Mode = (RenderforgeMode)Enum.Parse(typeof(RenderforgeMode), mode, true);
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
