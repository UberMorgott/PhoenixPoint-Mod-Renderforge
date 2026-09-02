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
        public const int DLSS_Q_DLAA = 0, DLSS_Q_QUALITY = 1, DLSS_Q_BALANCED = 2, DLSS_Q_PERFORMANCE = 3, DLSS_Q_ULTRA_PERFORMANCE = 4;
        public const int DLSS_F_HDR = 1, DLSS_F_DEPTH_INVERTED = 2, DLSS_F_MV_LOW_RES = 4, DLSS_F_MV_JITTERED = 8, DLSS_F_AUTO_EXPOSURE = 16;
        public const int DLSS_EV_CREATE = 1, DLSS_EV_EVALUATE = 2, DLSS_EV_RELEASE = 3;
        public const int DLSS_ERR_PASSTHROUGH_SIZE = -1, DLSS_ERR_NO_CONTEXT = -2, DLSS_ERR_SHARPEN = -3;
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
        /// Workshop install, so the mod stages the file itself. A loaded (locked) target gets a `.new` marker beside it
        /// and is replaced on the next run, when the process is fresh and nothing holds it. Never throws.</summary>
        public static void EnsureStaged(string modDir, Action<string> log)
        {
            try
            {
                string src = Path.Combine(modDir, DllName), dst = StagedPath, pending = dst + ".new";
                if (!File.Exists(src)) return;
                if (SameFile(src, dst)) { if (File.Exists(pending)) File.Delete(pending); return; }
                Directory.CreateDirectory(PluginsDir);
                try
                {
                    File.Copy(src, dst, true);
                    if (File.Exists(pending)) File.Delete(pending);
                    log("[Renderforge] staged native shim into Plugins\\x86_64 (takes effect after restart)");
                }
                catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
                {
                    // ponytail: the .new file is a marker, not the replacement source — the next run copies from the mod folder again.
                    File.Copy(src, pending, true);
                    log("[Renderforge] Plugins\\x86_64\\" + DllName + " is in use; wrote " + DllName + ".new, replaced on the next start");
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

        /// <summary>sharpness 0..1 = the shim's RCAS pass on `output` after NGX (0 = skipped). ABI unchanged since 0.1.</summary>
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetFrame(IntPtr slot, IntPtr color, IntPtr depth, IntPtr mv, IntPtr output,
            float jitterX, float jitterY, float mvScaleX, float mvScaleY,
            int reset, float dtMs, uint renderW, uint renderH,
            float preExposure, float sharpness);

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
    }
}
