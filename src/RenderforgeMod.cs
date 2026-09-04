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

        // Kept alive for the life of the mod: NGX holds the D3D11/D3D12 device it was taken from.
        private static Texture2D probeTex;
        private bool patched;

        public override void OnModEnabled()
        {
            Instance = this;
            ModDir = base.Instance?.Entry?.Directory ?? ".";
            Available = false;
            Diagnostics.Reset();
            RendererSwitch.SelfTest();   // [Conditional("DEBUG")]: compiled out of Release
            ApplyFrameRate();
            // Every API: harmless under D3D11, and the switch to D3D12 always goes through a restart, so the copy is there by then.
            Native.EnsureStaged(ModDir, s => Logger.LogInfo(s));
            // D3D11 and D3D12 both go through the native shim; it picks its backend from the resource we hand it.
            if (Availability.IsD3D11 || Availability.IsD3D12)
            {
                try
                {
                    if (!Native.Load(ModDir, s => Logger.LogWarning(s)))
                    {
                        InitCode = Native.DLSS_ERR_INIT_FAILED;
                        Logger.LogInfo("DLSS unavailable (code " + InitCode + "): RenderforgeNative.dll failed to load from " + ModDir);
                    }
                    else
                    {
                        probeTex = new Texture2D(1, 1, TextureFormat.RGBA32, false);
                        InitNative(Cfg.Upscaler);
                    }
                }
                catch (Exception ex)
                {
                    InitCode = Native.DLSS_ERR_INIT_FAILED;
                    Logger.LogError("Renderforge init THREW " + ex.Message);
                }
                if (!Available && Upscalers.Failed == UpscalerKind.Off)   // DLL never loaded / threw: the row still says why
                { Upscalers.Failed = Upscalers.Resolve(Cfg.Upscaler); Upscalers.FailedCode = InitCode; }
                Logger.LogInfo((Available ? "upscaler available" : "upscaler unavailable (code " + InitCode + "): " + Reason(InitCode))
                               + " provider=" + Upscalers.Running + " version=" + Native.ProviderVersion()
                               + " api=" + Native.Api() + " unityIface=" + Native.UnityIface() + " renderer=" + Availability.ApiName + " unity=" + Application.unityVersion);
            }
            else
            {
                InitCode = Native.DLSS_ERR_NOT_AVAILABLE;
                Logger.LogInfo("Renderforge: " + SystemInfo.graphicsDeviceType + " - native DLSS init skipped ("
                               + Availability.Reason(Feature.Dlss) + ")");
            }
            NeuralRenderingSupport.Probe(ModDir);
            Logger.LogInfo("DLSS 5 Neural Rendering: " + NeuralRenderingSupport.Status);
            try
            {
                if (Available) DlssDriver.Create();
                ((Harmony)HarmonyInstance).PatchAll(typeof(RenderforgeMod).Assembly);
                patched = true;
                if (RendererSwitch.Wants12(Cfg) && !Availability.IsD3D12) RendererSwitch.ArmStartupRestart();
                AttachAndApply();
            }
            catch (Exception ex) { Logger.LogError("Renderforge enable THREW " + ex); }
        }

        /// <summary>Stand the shim up on `want` (SetProvider + Dlss_Init on the probe texture). Auto under D3D12: a provider
        /// that fails Init (GTX without DLSS, old driver, no DP4a) falls through to the next one - FSR, then XeSS
        /// (Upscalers.NextFallback); Dlss_Shutdown clears the latch, so a second Init with another provider is allowed.
        /// Sets Available / InitCode / Upscalers.Running (+ Failed on a miss).</summary>
        private static bool InitNative(UpscalerKind want)
        {
            var m = Instance;
            Upscalers.Running = Upscalers.Resolve(want);
            Native.SetProvider(Upscalers.ProviderOf(Upscalers.Running));
            InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
            while (InitCode != Native.DLSS_OK && want == UpscalerKind.Auto)
            {
                UpscalerKind next = Upscalers.NextFallback(Upscalers.Running);
                if (next == UpscalerKind.Off) break;
                m.Logger.LogInfo(Upscalers.Running + " init failed (code " + InitCode + "): Auto falls back to " + next);
                Native.Dlss_Shutdown();
                Upscalers.Running = next;
                Native.SetProvider(Upscalers.ProviderOf(next));
                InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
            }
            Available = InitCode == Native.DLSS_OK;
            if (Available) Upscalers.Failed = UpscalerKind.Off;
            else { Upscalers.Failed = Upscalers.Running; Upscalers.FailedCode = InitCode; Upscalers.Running = UpscalerKind.Off; }
            return Available;
        }

        /// <summary>Live provider switch, called by DlssDriver once nothing is generating (or directly when there is no
        /// driver): tear the shim's backend down and stand it up again on `want`. A failed Init falls back to the
        /// provider that was running (Upscalers.Failed keeps the reason for the picker).</summary>
        internal static void ReinitNative(UpscalerKind want)
        {
            var m = Instance;
            if (m == null || probeTex == null) return;
            UpscalerKind prev = Upscalers.Running;
            Native.Dlss_Shutdown();
            if (InitNative(want))
            {
                m.Logger.LogInfo("upscaler switched to " + Upscalers.Running + " version=" + Native.ProviderVersion());
                return;
            }
            m.Logger.LogWarning(Upscalers.Failed + " init failed (code " + InitCode + "): back to " + prev);
            UpscalerKind failed = Upscalers.Failed; int code = Upscalers.FailedCode;
            Native.Dlss_Shutdown();
            if (prev != UpscalerKind.Off && InitNative(prev)) { Upscalers.Failed = failed; Upscalers.FailedCode = code; }
        }

        public override void OnModDisabled()
        {
            try
            {
                DlssDriver.Instance?.Apply(RenderforgeMode.Off, DebugView.None);
                if (DlssDriver.Instance != null) UnityEngine.Object.Destroy(DlssDriver.Instance.gameObject);
                FrameGen.Stop();
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
        public override void OnLevelEnd(Level level) => DlssDriver.Instance?.Apply(RenderforgeMode.Off, Diagnostics.View);

        public override void OnConfigChanged()
        {
            Logger.LogInfo("DLSS mode = " + Cfg.Mode + " view = " + Diagnostics.View + " upscaler = " + Cfg.Upscaler);
            ApplyFrameRate();
            ApplyUpscaler();
            AttachAndApply();
        }

        /// <summary>Config's Upscaler differs from the running provider and is usable right now -> switch live. Null =
        /// nothing to do or the switch is under way; otherwise the Availability reason the row is greyed with.</summary>
        private string ApplyUpscaler()
        {
            UpscalerKind kind = Upscalers.Resolve(Cfg.Upscaler);
            if (kind == UpscalerKind.Off || kind == Upscalers.Running) return null;
            string reason = Availability.Reason(Upscalers.FeatureOf(kind));
            if (reason != null) return reason;
            if (DlssDriver.Instance != null) DlssDriver.Instance.SwitchProvider(kind);
            else { ReinitNative(kind); if (Available) DlssDriver.Create(); }
            return null;
        }

        /// <summary>The UPSCALER row and PPCLI: {"member":"SetUpscaler","args":["FSR"]} - Off / Auto / DLSS / FSR / XeSS.
        /// Applies live (DlssDriver releases the generation, re-inits the shim, re-creates) and saves. Off means "no
        /// upscaling at all", so the quality mode follows it; leaving Off turns the mode back to Auto. An unavailable
        /// choice is saved (the next launch under the right renderer picks it up) but refused now, with the reason.</summary>
        public static string SetUpscaler(string name)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            UpscalerKind want;
            if (!Enum.TryParse(name, true, out want)) return "bad upscaler '" + name + "' (Off / Auto / DLSS / FSR / XeSS)";
            m.Cfg.Upscaler = want;
            if (want == UpscalerKind.Off) m.Cfg.Mode = RenderforgeMode.Off;
            else if (m.Cfg.Mode == RenderforgeMode.Off) m.Cfg.Mode = RenderforgeMode.Auto;
            string refused = m.ApplyUpscaler();
            m.AttachAndApply();
            SaveConfig();
            return (refused != null ? "refused: " + refused : "upscaler=" + want) + " | " + GetStatus();
        }

        /// <summary>The setting is the final presented-FPS ceiling. With a live 2x/3x/4x frame-generation
        /// chain, cap Unity's rendered frames to floor(ceiling / multiplier), so generated frames stay inside
        /// the requested total. If FG is unavailable or tears down, FrameGen immediately reapplies the full cap.
        /// Also the InitVideoOptions postfix (Patches.cs), after the game's own SetFrameRateLimit(60).</summary>
        public static void ApplyFrameRate()
        {
            var cfg = Instance?.Cfg;
            if (cfg == null) return;
            int presented = Mathf.Clamp(cfg.FrameRateLimit, 30, 300);
            Application.targetFrameRate = cfg.LimitFrameRate
                ? NativeFrameRateFor(presented, FrameGen.OutputMultiplier)
                : -1;
        }

        internal static int NativeFrameRateFor(int presentedLimit, int multiplier)
        {
            return Math.Max(1, presentedLimit / Math.Max(1, multiplier));
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
            FrameGen.Apply(Cfg);              // also reached from OnConfigChanged through this method
            var d = DlssDriver.Instance;
            if (d == null) return;
            if (Cfg.Mode != RenderforgeMode.Off) lastOn = Cfg.Mode;
            var cam = GameUtl.GameComponent<CameraManager>()?.Camera;
            if (cam == null) return;          // main menu without CameraManager: wait for the next level
            d.Attach(cam);
            d.Apply(Cfg.Mode, Diagnostics.View);
        }

        // ---- PPCLI `connect call` surface: {"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["DLAA","None"]}
        public static string SetMode(string mode, string view)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.Mode = (RenderforgeMode)Enum.Parse(typeof(RenderforgeMode), mode, true);
            Diagnostics.View = (DebugView)Enum.Parse(typeof(DebugView), view, true);
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

        /// <summary>PPCLI: {"member":"SetFrameGen","args":["X2"]} - Off / X2 / X3 / X4. Live next frame + saved.</summary>
        public static string SetFrameGen(string mode)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            FrameGenMode fg;
            if (!Enum.TryParse(mode, true, out fg)) return "bad mode '" + mode + "' (Off / X2 / X3 / X4)";
            m.Cfg.FrameGen = fg;
            FrameGen.Apply(m.Cfg);
            SaveConfig();
            return "frameGen=" + m.Cfg.FrameGen + " " + FrameGen.Status();
        }

        public static string SetNeuralRendering(string mode)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            NeuralRenderingMode nr;
            if (!Enum.TryParse(mode, true, out nr)) return "bad mode '" + mode + "' (Off / Auto)";
            m.Cfg.NeuralRendering = nr;
            m.AttachAndApply();
            SaveConfig();
            return "neuralRendering=" + nr + " " + Native.NrStatus();
        }

        /// <summary>PPCLI/live UI surface for the four numeric parameters supported by the private DLSSNR ABI.</summary>
        public static string SetNeuralParameters(float intensity, float localTone, float localStructure, float skinStructure)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.NeuralIntensity = Mathf.Clamp(intensity, 0f, 2f);
            m.Cfg.NeuralLocalTone = Mathf.Clamp(localTone, 0f, 2f);
            m.Cfg.NeuralLocalStructure = Mathf.Clamp(localStructure, 0f, 2f);
            m.Cfg.NeuralSkinStructure = Mathf.Clamp(skinStructure, -1f, 1f);
            ApplyNeuralRenderingSettings();
            return "nrParams=" + m.Cfg.NeuralIntensity.ToString("R") + "," + m.Cfg.NeuralLocalTone.ToString("R") + ","
                + m.Cfg.NeuralLocalStructure.ToString("R") + "," + m.Cfg.NeuralSkinStructure.ToString("R") + " " + Native.NrStatus();
        }

        internal static void ApplyNeuralRenderingSettings()
        {
            var m = Instance;
            if (m == null) return;
            m.AttachAndApply();
            SaveConfig();
        }

        /// <summary>PPCLI: {"member":"SetFgProvider","args":["Fsr"]} - Auto / None / Fsr / Xess / Dlss. Test lever:
        /// the RTX in this machine can run all three, and only a forced pick proves the cross-vendor paths.</summary>
        public static string SetFgProvider(string name)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            int id;
            switch (name.ToLowerInvariant())
            {
                case "none": id = Native.FG_PROVIDER_NONE; break;
                case "fsr": id = Native.FG_PROVIDER_FSR; break;
                case "xess": id = Native.FG_PROVIDER_XESS; break;
                case "dlss": id = Native.FG_PROVIDER_DLSS; break;
                default: id = -1; break;
            }
            FrameGen.Force(id);
            FrameGen.Apply(m.Cfg);
            return "fgProvider=" + name + " " + FrameGen.Status();
        }

        /// <summary>PPCLI test knob: {"member":"SetHoldPrepare","args":[true]} - the driver stops feeding FG frames while the chain stays live.</summary>
        public static string SetHoldPrepare(bool on) { FrameGen.HoldPrepare = on; return "holdPrepare=" + on + " " + FrameGen.Status(); }

        /// <summary>PPCLI diagnostic: {"member":"SetMvJittered","args":[true]} - DLSS_F_MV_JITTERED in the create flags
        /// (NGX MVJittered / FSR jitter cancellation / XeSS JITTERED_MV). The driver re-creates the feature next frame. Runtime-only.</summary>
        public static string SetMvJittered(bool on)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.MvJittered = on;
            return GetStatus();
        }

        /// <summary>PPCLI diagnostic: {"member":"SetD3D12SrgbViews","args":[true]} - DLSS_F_SRGB_VIEWS in the create flags
        /// (D3D12: sRGB colour-in view for the SDK + sRGB-tagged outRT, see DlssDriver.StartGeneration). Re-creates next frame. Runtime-only.</summary>
        public static string SetD3D12SrgbViews(bool on)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.D3D12SrgbViews = on;
            return GetStatus();
        }

        /// <summary>PPCLI diagnostic: {"member":"SetD3D12ColorDesc","args":[true]} - colorRT from an explicit R8G8B8A8_SRGB
        /// RenderTextureDescriptor (D3D12: sRGB RTV for PPv2's final encode). Re-creates next frame. Runtime-only.</summary>
        public static string SetD3D12ColorDesc(bool on)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.D3D12ColorDesc = on;
            return GetStatus();
        }

        /// <summary>PPCLI diagnostic: {"member":"SetD3D12HalfColor","args":[true]} - colorRT + outRT as linear ARGBHalf and
        /// DLSS_F_HDR in the create flags (D3D12: no 8-bit sRGB storage anywhere, see DlssDriver.StartGeneration). Re-creates next frame. Runtime-only.</summary>
        public static string SetD3D12HalfColor(bool on)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.D3D12HalfColor = on;
            return GetStatus();
        }

        /// <summary>PPCLI diagnostic: {"member":"SetPostProcessEnabled","args":[false]} - toggles the bound camera's
        /// PostProcessLayer.enabled ("is the final PP pass the writer" experiment). Not saved.</summary>
        public static string SetPostProcessEnabled(bool on)
        {
            var l = DlssDriver.Instance?.Layer;
            if (l == null) return "no PostProcessLayer";
            l.enabled = on;
            return "postProcessEnabled=" + l.enabled;
        }

        /// <summary>PPCLI diagnostic: {"member":"ProbeMv","args":[640,360]} - see DlssDriver.ProbeMv.</summary>
        public static string ProbeMv(int x, int y) => DlssDriver.Instance?.ProbeMv(x, y) ?? "no driver";

        // ---- jitter diagnostics (D3D12 detail-loss hunt). Applied next frame, no feature re-create, runtime-only.
        /// <summary>{"member":"SetJitterReportSign","args":[1,-1]} - ±1 each; multiplies ONLY the offset reported to the SDK.</summary>
        public static string SetJitterReportSign(int x, int y)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.JitterReportSignX = x < 0 ? -1 : 1;
            Diagnostics.JitterReportSignY = y < 0 ? -1 : 1;
            return GetStatus();
        }

        /// <summary>{"member":"SetJitterScale","args":[0.5]} - scales the rendered projection jitter AND the reported offset; 0 = no jitter.</summary>
        public static string SetJitterScale(float s)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.JitterScale = s;
            return GetStatus();
        }

        /// <summary>{"member":"SetJitterReportSwapXY","args":[true]} - swap x/y of the reported offset only.</summary>
        public static string SetJitterReportSwapXY(bool on)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.JitterReportSwapXY = on;
            return GetStatus();
        }

        /// <summary>{"member":"SetJitterConst","args":[true,0.25,-0.25]} - replace the Halton sample with a constant (render-res pixels) every frame; rendered AND reported.</summary>
        public static string SetJitterConst(bool on, float x, float y)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.JitterConstEnabled = on; Diagnostics.JitterConstX = x; Diagnostics.JitterConstY = y;
            return GetStatus();
        }

        /// <summary>{"member":"SetForceReset","args":[true]} - pass the history-reset flag every frame (NGX InReset / FSR reset / XeSS resetHistory).</summary>
        public static string SetForceReset(bool on)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            Diagnostics.ForceReset = on;
            return GetStatus();
        }

        /// <summary>{"member":"DumpOut","args":["C:\\Temp\\out.png"]} - outRT (SDK output, before the present Blit) to PNG.</summary>
        public static string DumpOut(string absPath) => DlssDriver.Instance?.DumpOut(absPath) ?? "no driver";

        /// <summary>{"member":"DumpColorIn","args":["C:\\Temp\\in.png"]} - colorRT (SDK colour input) to PNG.</summary>
        public static string DumpColorIn(string absPath) => DlssDriver.Instance?.DumpColorIn(absPath) ?? "no driver";

        private static string JitterKnobs() =>
            " jitterSign=" + Diagnostics.JitterReportSignX + "," + Diagnostics.JitterReportSignY + " jitterScale=" + Diagnostics.JitterScale.ToString("R") + " jitterSwapXY=" + Diagnostics.JitterReportSwapXY
            + " jitterConst=" + Diagnostics.JitterConstEnabled + "," + Diagnostics.JitterConstX.ToString("R") + "," + Diagnostics.JitterConstY.ToString("R") + " forceReset=" + Diagnostics.ForceReset;

        public static string GetStatus() => "provider=" + Upscalers.Running + " unity=" + Application.unityVersion + " mvJittered=" + Diagnostics.MvJittered + " d3d12SrgbViews=" + Diagnostics.D3D12SrgbViews + " d3d12ColorDesc=" + Diagnostics.D3D12ColorDesc + " d3d12HalfColor=" + Diagnostics.D3D12HalfColor + JitterKnobs() + " "
                                          + (DlssDriver.Instance?.Status ?? ("no driver; available=" + Available + " init=" + InitCode))
                                          + " | fg=" + FrameGen.Status();

        private static string Reason(int code)
        {
            switch (code)
            {
                case Native.DLSS_ERR_NO_DEVICE: return "no D3D11/D3D12 device behind the probe texture";
                case Native.DLSS_ERR_INIT_FAILED: return "NGX init failed (see nvsdk_ngx.log in the mod folder)";
                case Native.DLSS_ERR_NOT_AVAILABLE: return "DLSS not supported on this GPU";
                case Native.DLSS_ERR_NEEDS_DRIVER: return "NVIDIA driver too old for this DLSS";
                case Native.DLSS_ERR_NO_UNITY_IFACE: return "Unity never handed the plugin its D3D12 interface (UnityPluginLoad did not run)";
                case Native.DLSS_ERR_NO_PROVIDER_DLL: return "amd_fidelityfx_*_dx12.dll missing from the mod folder";
                case Native.DLSS_ERR_PROVIDER_UNSUPPORTED: return "this upscaler needs DirectX 12 (or is not implemented yet)";
                default: return "unknown";
            }
        }
    }
}
