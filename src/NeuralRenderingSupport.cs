using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using UnityEngine;

namespace Renderforge
{
    internal enum NeuralRenderingState
    {
        NotProbed,
        RendererUnsupported,
        GpuUnsupported,
        DriverTooOld,
        RuntimeMissing,
        RuntimeUntrusted,
        Available,
        ProbeFailed
    }

    /// <summary>
    /// Capability and trust gate for the experimental/unofficial RTX 50-only DLSS 5 Neural Rendering path.
    /// Only the private RenderforgeNR runtime is accepted; native failures stay isolated from DLSS SR/DLAA.
    /// </summary>
    internal static class NeuralRenderingSupport
    {
        private const string RuntimeName = "nvngx_dlssnr.dll";
        private const int MinimumDriverBuild = 16;
        private const int MinimumDriverPrivate = 1664; // NVIDIA 616.64
        private const string TrustedRuntimeSha256 = "E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E";
        private static readonly Regex Rtx50 = new Regex(@"\bRTX\s*50\d{2}\b", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        private static readonly Guid GenericVerifyV2 = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");

        internal static NeuralRenderingState State { get; private set; } = NeuralRenderingState.NotProbed;
        internal static string RuntimePath { get; private set; }
        internal static string Status { get; private set; } = "not probed";
        internal static bool Available { get { return State == NeuralRenderingState.Available; } }

        internal static void Probe(string modDir)
        {
            RuntimePath = null;
            try { ProbeCore(modDir); }
            catch (Exception ex)
            {
                Set(NeuralRenderingState.ProbeFailed, "disabled: capability probe failed (" + ex.GetType().Name + ")");
            }
        }

        private static void ProbeCore(string modDir)
        {
            if (!Availability.IsD3D12)
            {
                Set(NeuralRenderingState.RendererUnsupported, "disabled: DirectX 12 required");
                return;
            }

            string gpu = SystemInfo.graphicsDeviceName ?? string.Empty;
            if (!Availability.IsNvidia || !Rtx50.IsMatch(gpu))
            {
                Set(NeuralRenderingState.GpuUnsupported, "disabled: NVIDIA RTX 50 Series required");
                return;
            }

            string driver;
            if (!HasMinimumDriver(out driver))
            {
                Set(NeuralRenderingState.DriverTooOld, "disabled: NVIDIA 616.64 or newer required (found " + driver + ")");
                return;
            }

            string privateDir = Path.Combine(modDir, "RenderforgeNR");
            string bridge = Path.Combine(privateDir, "nvngx.dll");
            string path = Path.Combine(privateDir, RuntimeName);
            if (!File.Exists(bridge) || !File.Exists(path))
            {
                Set(NeuralRenderingState.RuntimeMissing, "disabled: private RenderforgeNR runtime or bridge missing");
                return;
            }
            string reason;
            if (!IsTrustedNvidiaBinary(path, out reason))
            {
                Set(NeuralRenderingState.RuntimeUntrusted, "disabled: NVIDIA Neural Rendering runtime failed signature validation (" + reason + ")");
                return;
            }
            RuntimePath = path;
            Set(NeuralRenderingState.Available, "available: experimental/unofficial RTX 50 Neural Rendering");
        }

        internal static bool ShouldEnable(DlssConfig cfg)
        {
            return cfg != null && Available && cfg.NeuralRendering == NeuralRenderingMode.Auto
                && Upscalers.Running == UpscalerKind.DLSS;
        }

        internal static void ConfigureNative(DlssConfig cfg)
        {
            if (cfg == null) { Native.ConfigureNr(0, 0, 1f, 1f, 1f, -1f, 1); return; }
            bool enabled = ShouldEnable(cfg);
            Native.ConfigureNr(enabled ? 1 : 0, (int)cfg.NeuralStyle,
                Mathf.Clamp(cfg.NeuralIntensity, 0f, 2f), Mathf.Clamp(cfg.NeuralLocalTone, 0f, 2f),
                Mathf.Clamp(cfg.NeuralLocalStructure, 0f, 2f), Mathf.Clamp(cfg.NeuralSkinStructure, -1f, 1f),
                cfg.NeuralAutoMask ? 1 : 0);
        }

        internal static string SettingsKey(DlssConfig cfg)
        {
            if (cfg == null) return "";
            return cfg.NeuralRendering + "|" + cfg.NeuralStyle + "|"
                + cfg.NeuralIntensity.ToString("R", CultureInfo.InvariantCulture) + "|"
                + cfg.NeuralLocalTone.ToString("R", CultureInfo.InvariantCulture) + "|"
                + cfg.NeuralLocalStructure.ToString("R", CultureInfo.InvariantCulture) + "|"
                + cfg.NeuralSkinStructure.ToString("R", CultureInfo.InvariantCulture) + "|" + cfg.NeuralAutoMask;
        }

        internal static bool IsTrustedNvidiaBinary(string path, out string reason)
        {
            reason = null;
            if (string.IsNullOrEmpty(path) || !File.Exists(path))
            {
                reason = "file missing";
                return false;
            }

            if (!string.Equals(Path.GetFileName(path), RuntimeName, StringComparison.OrdinalIgnoreCase))
            {
                reason = "unexpected file name";
                return false;
            }

            int trust = VerifyFileSignature(path);
            if (trust != 0)
            {
                reason = "WinVerifyTrust 0x" + trust.ToString("X8");
                return false;
            }

            try
            {
                using (var stream = File.OpenRead(path))
                using (var sha = SHA256.Create())
                {
                    string hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", string.Empty);
                    if (!string.Equals(hash, TrustedRuntimeSha256, StringComparison.OrdinalIgnoreCase))
                    {
                        reason = "runtime hash is not the trusted 310.8.0 build";
                        return false;
                    }
                }
            }
            catch
            {
                reason = "runtime hash unavailable";
                return false;
            }

            return true;
        }

        private static void Set(NeuralRenderingState state, string status)
        {
            State = state;
            Status = status;
        }

        private static bool HasMinimumDriver(out string version)
        {
            try
            {
                string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "nvapi64.dll");
                FileVersionInfo info = FileVersionInfo.GetVersionInfo(path);
                version = info.FileVersion ?? "unknown";
                return info.FileBuildPart > MinimumDriverBuild
                    || (info.FileBuildPart == MinimumDriverBuild && info.FilePrivatePart >= MinimumDriverPrivate);
            }
            catch
            {
                version = "unknown";
                return false;
            }
        }

        private static int VerifyFileSignature(string path)
        {
            IntPtr pathPtr = IntPtr.Zero;
            IntPtr filePtr = IntPtr.Zero;
            IntPtr dataPtr = IntPtr.Zero;
            try
            {
                pathPtr = Marshal.StringToCoTaskMemUni(path);
                var file = new WinTrustFileInfo
                {
                    cbStruct = (uint)Marshal.SizeOf(typeof(WinTrustFileInfo)),
                    pcwszFilePath = pathPtr
                };
                filePtr = Marshal.AllocCoTaskMem(Marshal.SizeOf(typeof(WinTrustFileInfo)));
                Marshal.StructureToPtr(file, filePtr, false);

                var data = new WinTrustData
                {
                    cbStruct = (uint)Marshal.SizeOf(typeof(WinTrustData)),
                    dwUIChoice = 2,              // WTD_UI_NONE
                    fdwRevocationChecks = 0,     // WTD_REVOKE_NONE
                    dwUnionChoice = 1,           // WTD_CHOICE_FILE
                    pFile = filePtr,
                    dwStateAction = 0,           // WTD_STATEACTION_IGNORE
                    dwProvFlags = 0x00001000      // WTD_CACHE_ONLY_URL_RETRIEVAL
                };
                dataPtr = Marshal.AllocCoTaskMem(Marshal.SizeOf(typeof(WinTrustData)));
                Marshal.StructureToPtr(data, dataPtr, false);

                Guid action = GenericVerifyV2;
                return WinVerifyTrust(new IntPtr(-1), ref action, dataPtr);
            }
            catch
            {
                return unchecked((int)0x800B0100); // TRUST_E_NOSIGNATURE
            }
            finally
            {
                if (dataPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(dataPtr);
                if (filePtr != IntPtr.Zero) Marshal.FreeCoTaskMem(filePtr);
                if (pathPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(pathPtr);
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct WinTrustFileInfo
        {
            internal uint cbStruct;
            internal IntPtr pcwszFilePath;
            internal IntPtr hFile;
            internal IntPtr pgKnownSubject;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct WinTrustData
        {
            internal uint cbStruct;
            internal IntPtr pPolicyCallbackData;
            internal IntPtr pSIPClientData;
            internal uint dwUIChoice;
            internal uint fdwRevocationChecks;
            internal uint dwUnionChoice;
            internal IntPtr pFile;
            internal uint dwStateAction;
            internal IntPtr hWVTStateData;
            internal IntPtr pwszURLReference;
            internal uint dwProvFlags;
            internal uint dwUIContext;
        }

        [DllImport("wintrust.dll", ExactSpelling = true, SetLastError = false)]
        private static extern int WinVerifyTrust(IntPtr hwnd, [In] ref Guid pgActionId, IntPtr pWinTrustData);
    }
}
