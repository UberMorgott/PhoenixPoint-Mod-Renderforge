using System;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Renderforge
{
    /// <summary>P/Invoke surface of native\RenderforgeNative.h. Call <see cref="Load"/> first: it pins the DLL from the
    /// mod folder so "RenderforgeNative" resolves against the already-loaded module, not the game's Plugins folder.</summary>
    public static class Native
    {
        public const int DLSS_OK = 0, DLSS_ERR_NO_DEVICE = 1, DLSS_ERR_INIT_FAILED = 2, DLSS_ERR_NOT_AVAILABLE = 3, DLSS_ERR_NEEDS_DRIVER = 4, DLSS_ERR_NO_UNITY_IFACE = 5;
        public const int DLSS_ERR_NO_PROVIDER_DLL = 6, DLSS_ERR_PROVIDER_UNSUPPORTED = 7;
        public const int PROVIDER_DLSS = 0, PROVIDER_FSR = 1, PROVIDER_XESS = 2;
        public const int DLSS_Q_DLAA = 0, DLSS_Q_QUALITY = 1, DLSS_Q_BALANCED = 2, DLSS_Q_PERFORMANCE = 3, DLSS_Q_ULTRA_PERFORMANCE = 4;
        /// <summary>XeSS-only presets (1.5x / 1.3x); DLSS and FSR treat them as Quality.</summary>
        public const int DLSS_Q_ULTRA_QUALITY = 5, DLSS_Q_ULTRA_QUALITY_PLUS = 6;
        public const int DLSS_F_HDR = 1, DLSS_F_DEPTH_INVERTED = 2, DLSS_F_MV_LOW_RES = 4, DLSS_F_MV_JITTERED = 8, DLSS_F_AUTO_EXPOSURE = 16, DLSS_F_SRGB_VIEWS = 32;
        public const int DLSS_EV_CREATE = 1, DLSS_EV_EVALUATE = 2, DLSS_EV_RELEASE = 3;
        public const int DLSS_ERR_PASSTHROUGH_SIZE = -1, DLSS_ERR_NO_CONTEXT = -2, DLSS_ERR_SHARPEN = -3, DLSS_ERR_FENCE_TIMEOUT = -4, DLSS_ERR_FFX = -5, DLSS_ERR_XESS = -6;
        public const int NGX_SUCCESS = 1;

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        public static IntPtr Handle { get; private set; }

        private const string DllName = "RenderforgeNative.dll";

        /// <summary><game>_Data\Plugins\x86_64 — Application.dataPath is `<install>\PhoenixPointWin64_Data` in a standalone player.</summary>
        public static string PluginsDir => Path.Combine(Path.Combine(Application.dataPath, "Plugins"), "x86_64");
        public static string StagedPath => Path.Combine(PluginsDir, DllName);

        /// <summary>Same build = same length and last-write time (File.Copy preserves the timestamp on Windows).</summary>
        private static bool SameFile(string a, string b)
        {
            var fa = new FileInfo(a); var fb = new FileInfo(b);
            return fa.Exists && fb.Exists && fa.Length == fb.Length && fa.LastWriteTimeUtc == fb.LastWriteTimeUtc;
        }

        /// <summary>Copies the mod's shim into Plugins\x86_64 (Unity calls UnityPluginLoad only for modules it resolves
        /// from there; the D3D12 backend needs the IUnityInterfaces that call delivers). Nobody runs deploy.ps1 on a
        /// Workshop install, so the mod stages the file itself. Unity loads the Plugins copy at startup, so it is locked in
        /// every process — a mapped DLL cannot be overwritten but CAN be renamed: move it aside to `.old`, copy the new
        /// one in, sweep the `.old*` leftovers on the next run. Never throws.</summary>
        public static void EnsureStaged(string modDir, Action<string> log)
        {
            try
            {
                string src = Path.Combine(modDir, DllName), dst = StagedPath;
                if (!File.Exists(src)) return;
                if (Directory.Exists(PluginsDir))
                    foreach (string old in Directory.GetFiles(PluginsDir, DllName + ".old*"))
                        try { File.Delete(old); } catch (Exception) { }   // still mapped by this process: next run
                if (SameFile(src, dst)) return;
                Directory.CreateDirectory(PluginsDir);
                try
                {
                    File.Copy(src, dst, true);
                    log("[Renderforge] staged native shim into Plugins\\x86_64 (takes effect after restart)");
                }
                catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
                {
                    // ponytail: `.old` then `.old<ticks>` — the sweep above deletes whatever is no longer mapped.
                    string aside = dst + ".old";
                    if (File.Exists(aside)) aside = dst + ".old" + DateTime.UtcNow.Ticks;
                    File.Move(dst, aside);
                    File.Copy(src, dst);
                    log("[Renderforge] updated staged native shim (takes effect after restart)");
                }
            }
            catch (Exception ex) { log("[Renderforge] native shim staging failed: " + ex.Message); }
        }

        public static bool Load(string modDir, Action<string> log)
        {
            if (Handle != IntPtr.Zero) return true;
            string mine = Path.Combine(modDir, DllName), staged = StagedPath;
            // Prefer the Plugins copy (UnityPluginLoad) — but only when it is this build; a stale one would mismatch the ABI.
            if (File.Exists(staged))
            {
                if (SameFile(mine, staged)) Handle = LoadLibraryW(staged);
                else log("[Renderforge] Plugins\\x86_64\\" + DllName + " is stale — loading the mod copy; restart to pick up the staged one");
            }
            if (Handle == IntPtr.Zero) Handle = LoadLibraryW(mine);
            return Handle != IntPtr.Zero;
        }

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int Dlss_Init(IntPtr anyD3D11Resource, [MarshalAs(UnmanagedType.LPWStr)] string dllDir, [MarshalAs(UnmanagedType.LPWStr)] string logDir);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_GetOptimal(uint outW, uint outH, int quality,
            out uint renderW, out uint renderH, out uint minW, out uint minH, out uint maxW, out uint maxH);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetCreateParams(uint w, uint h, uint outW, uint outH, int quality, int flags);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr Dlss_GetFrameSlot();

        /// <summary>Sharpness and original analytic LUT grading run on `output` after temporal reconstruction.</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetFrame(IntPtr slot, IntPtr color, IntPtr depth, IntPtr mv, IntPtr output,
            float jitterX, float jitterY, float mvScaleX, float mvScaleY,
            int reset, float dtMs, uint renderW, uint renderH,
            float preExposure, float sharpness, int lutPreset, float lutStrength);

        // Fill the same frame slot after Dlss_SetFrame clears it; values never enter the feature settings key.
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetSceneStyle(IntPtr slot, int mode, float strength, int pixelSize);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr Dlss_GetRenderEventFunc();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr Dlss_GetRenderEventAndDataFunc();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_Passthrough(int on);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_LastError();

        /// <summary>1 = NIS sharpen, 2 = RCAS fallback, -1 = failed, 0 = not compiled yet (first non-zero sharpness compiles it).</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_Sharpener();

        public static string SharpenerName(int s) => s == 1 ? "NIS" : s == 2 ? "RCAS" : s < 0 ? "failed" : "none";

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_Status(out int lastCreateResult, out int lastEvalResult, out int featureAlive);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, EntryPoint = "DlssNr_Configure")]
        private static extern void DlssNr_ConfigureNative(int enabled, int style, float intensity, float localTone,
            float localStructure, float skinStructure, int autoMask);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void DlssNr_Status(out int initResult, out int createResult, out int evalResult, out int featureAlive);

        /// <summary>Fail-open: a missing experimental NR export leaves the normal DLSS path untouched.</summary>
        public static void ConfigureNr(int enabled, int style, float intensity, float localTone,
            float localStructure, float skinStructure, int autoMask)
        {
            try { DlssNr_ConfigureNative(enabled, style, intensity, localTone, localStructure, skinStructure, autoMask); }
            catch (Exception) { }
        }

        public static string NrStatus()
        {
            try
            {
                int i, c, e, alive; DlssNr_Status(out i, out c, out e, out alive);
                return "nrInit=0x" + i.ToString("X") + " nrCreate=0x" + c.ToString("X")
                    + " nrEval=0x" + e.ToString("X") + " nrAlive=" + alive;
            }
            catch (Exception) { return "nrUnavailable"; }
        }

        /// <summary>D3D12 ~60-frame averages: GPU ms of copy-in / upscale / copy-out on the evaluate list, CPU ms waited for a ring slot.</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_Timings(out float copyInMs, out float evalMs, out float copyOutMs, out float ringWaitMs);

        public static string Timings()
        {
            if (Handle == IntPtr.Zero || Api() != 12) return "";
            float ci, ev, co, rw; Dlss_Timings(out ci, out ev, out co, out rw);
            return "copyInMs=" + ci.ToString("F2") + " evalMs=" + ev.ToString("F2") + " copyOutMs=" + co.ToString("F2") + " ringWaitMs=" + rw.ToString("F2");
        }

        public const int FG_OK = 0, FG_ERR_NOT_D3D12 = 1, FG_ERR_NO_HOOK = 2, FG_ERR_NO_SWAPCHAIN = 3,
                         FG_ERR_NO_PROVIDER = 4, FG_ERR_PROVIDER_FAILED = 5, FG_ERR_UNSUPPORTED_MULTIPLIER = 6;
        public const int FG_CAP_2X = 1, FG_CAP_3X = 2, FG_CAP_4X = 4;
        public const int FG_PROVIDER_NONE = 0, FG_PROVIDER_FSR = 1, FG_PROVIDER_XESS = 2, FG_PROVIDER_DLSS = 3;
        public const int DLSS_EV_FG_PREPARE = 4;

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Fg_PresentedFps();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int Fg_Init(int provider, uint multiplier, [MarshalAs(UnmanagedType.LPWStr)] string dllDir);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Fg_SetEnabled(int on);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Fg_SetFrame(IntPtr hudless, IntPtr depth, IntPtr mv,
            float jitterX, float jitterY, float mvScaleX, float mvScaleY,
            float cameraNear, float cameraFar, float cameraFovY,
            float dtMs, int reset,
            uint renderW, uint renderH, uint outW, uint outH,
            ulong frameId,
            float[] view, float[] proj, float[] cam);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern uint Fg_Caps();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Fg_Provider();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, EntryPoint = "Fg_Status")]
        private static extern IntPtr Fg_StatusPtr();

        public static string Fg_Status() => Marshal.PtrToStringAnsi(Fg_StatusPtr()) ?? "";

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, EntryPoint = "Fg_Reason")]
        private static extern IntPtr Fg_ReasonPtr();

        public static string Fg_Reason() => Marshal.PtrToStringAnsi(Fg_ReasonPtr()) ?? "";

        /// <summary>Synchronous: 1 = the chain is destroyed when this returns, 0 = still parked (the render thread never left it).</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Fg_Shutdown();

        /// <summary>Every frame on the main thread: destroys a chain the shim's render/UI threads detached. 1 = nothing pending.</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Fg_Pump();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Fg_Alive();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, EntryPoint = "Dlss_ResultString")]
        private static extern IntPtr Dlss_ResultStringPtr(int ngxResult);

        public static string Dlss_ResultString(int ngxResult) => Marshal.PtrToStringAnsi(Dlss_ResultStringPtr(ngxResult)) ?? "";

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_ReleaseNow();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_Shutdown();

        /// <summary>Graphics API the shim bound to: 0 = none, 11 = D3D11, 12 = D3D12.</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_Api();

        /// <summary>bit0 = UnityPluginLoad ran, bit1 = IUnityGraphicsD3D12v5 acquired.</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_UnityIface();

        /// <summary>Dlss_UnityIface behind a try, for the startup diagnostic line.</summary>
        public static int UnityIface()
        {
            try { return Dlss_UnityIface(); }
            catch (Exception) { return -1; }
        }

        /// <summary>Dlss_Api behind a try: 0 when the DLL is missing or the export is absent.</summary>
        public static int Api()
        {
            try { return Dlss_Api(); }
            catch (Exception) { return 0; }
        }

        /// <summary>Dlss_Init behind a try: a missing/unloadable DLL is a code, not an exception in the game's frame.</summary>
        public static int Init(IntPtr anyTex, string dllDir, string logDir)
        {
            try { return Dlss_Init(anyTex, dllDir, logDir); }
            catch (Exception) { return DLSS_ERR_INIT_FAILED; }
        }

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetProvider(int provider);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_Provider();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        private static extern int Dlss_ProviderVersion(System.Text.StringBuilder buf, int cap);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetCamera(float nearZ, float farZ, float fovYRadians);

        /// <summary>Dlss_SetProvider behind a try: a missing export must not take the mod down.</summary>
        public static void SetProvider(int provider)
        {
            try { Dlss_SetProvider(provider); }
            catch (Exception) { }
        }

        /// <summary>Provider actually running: PROVIDER_*, or -1 before init / on a missing export.</summary>
        public static int Provider()
        {
            try { return Dlss_Provider(); }
            catch (Exception) { return -1; }
        }

        /// <summary>Version string of the live provider ("4.1.1", "3.1.5"; XeSS "2.0.2 DP4a" / "2.0.2 XMX"), "" when unknown.</summary>
        public static string ProviderVersion()
        {
            try
            {
                var sb = new System.Text.StringBuilder(64);
                return Dlss_ProviderVersion(sb, sb.Capacity) > 0 ? sb.ToString() : "";
            }
            catch (Exception) { return ""; }
        }

        /// <summary>Dlss_SetCamera behind a try; called every frame, so it must never throw into the render loop.</summary>
        public static void SetCamera(float nearZ, float farZ, float fovYRadians)
        {
            try { Dlss_SetCamera(nearZ, farZ, fovYRadians); }
            catch (Exception) { }
        }
    }
}
