using System;
using System.IO;
using System.Runtime.InteropServices;

namespace DlssMod
{
    /// <summary>P/Invoke surface of native\DlssNative.h. Call <see cref="Load"/> first: it pins the DLL from the
    /// mod folder so "DlssNative" resolves against the already-loaded module, not the game's Plugins folder.</summary>
    public static class Native
    {
        public const int DLSS_OK = 0, DLSS_ERR_NO_DEVICE = 1, DLSS_ERR_INIT_FAILED = 2, DLSS_ERR_NOT_AVAILABLE = 3, DLSS_ERR_NEEDS_DRIVER = 4;
        public const int DLSS_Q_DLAA = 0, DLSS_Q_QUALITY = 1, DLSS_Q_BALANCED = 2, DLSS_Q_PERFORMANCE = 3, DLSS_Q_ULTRA_PERFORMANCE = 4;
        public const int DLSS_F_HDR = 1, DLSS_F_DEPTH_INVERTED = 2, DLSS_F_MV_LOW_RES = 4, DLSS_F_MV_JITTERED = 8, DLSS_F_AUTO_EXPOSURE = 16;
        public const int DLSS_EV_CREATE = 1, DLSS_EV_EVALUATE = 2, DLSS_EV_RELEASE = 3;

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        public static IntPtr Handle { get; private set; }

        public static bool Load(string modDir)
        {
            if (Handle != IntPtr.Zero) return true;
            Handle = LoadLibraryW(Path.Combine(modDir, "DlssNative.dll"));
            return Handle != IntPtr.Zero;
        }

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int Dlss_Init(IntPtr anyD3D11Resource, [MarshalAs(UnmanagedType.LPWStr)] string dllDir, [MarshalAs(UnmanagedType.LPWStr)] string logDir);

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_GetOptimal(uint outW, uint outH, int quality,
            out uint renderW, out uint renderH, out uint minW, out uint minH, out uint maxW, out uint maxH);

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetCreateParams(uint w, uint h, uint outW, uint outH, int quality, int flags);

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_SetFrame(IntPtr color, IntPtr depth, IntPtr mv, IntPtr output,
            float jitterX, float jitterY, float mvScaleX, float mvScaleY,
            int reset, float dtMs, uint renderW, uint renderH,
            float preExposure, float sharpness);

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr Dlss_GetRenderEventFunc();

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr Dlss_GetRenderEventAndDataFunc();

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Dlss_Status(out int lastCreateResult, out int lastEvalResult, out int featureAlive);

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl, EntryPoint = "Dlss_ResultString")]
        private static extern IntPtr Dlss_ResultStringPtr(int ngxResult);

        public static string Dlss_ResultString(int ngxResult) => Marshal.PtrToStringAnsi(Dlss_ResultStringPtr(ngxResult)) ?? "";

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_ReleaseNow();

        [DllImport("DlssNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Dlss_Shutdown();

        /// <summary>Dlss_Init behind a try: a missing/unloadable DLL is a code, not an exception in the game's frame.</summary>
        public static int Init(IntPtr anyTex, string dllDir, string logDir)
        {
            try { return Dlss_Init(anyTex, dllDir, logDir); }
            catch (Exception) { return DLSS_ERR_INIT_FAILED; }
        }
    }
}
