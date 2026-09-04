using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
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
        IntegrationContractUnavailable,
        ProbeFailed
    }

    /// <summary>
    /// Fail-closed capability probe for DLSS 5 3D-Guided Neural Rendering. It never loads a candidate DLL:
    /// the existing DLSS path remains untouched until NVIDIA exposes an authorized integration contract.
    /// </summary>
    internal static class NeuralRenderingSupport
    {
        private const string RuntimeName = "nvngx_dlssnr.dll";
        private const int MinimumDriverBuild = 16;
        private const int MinimumDriverPrivate = 1664; // NVIDIA 616.64
        private static readonly Regex Rtx50 = new Regex(@"\bRTX\s*50\d{2}\b", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        private static readonly Guid GenericVerifyV2 = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");

        internal static NeuralRenderingState State { get; private set; } = NeuralRenderingState.NotProbed;
        internal static string RuntimePath { get; private set; }
        internal static string Status { get; private set; } = "not probed";

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

            bool rejected = false;
            foreach (string path in CandidatePaths(modDir))
            {
                if (!File.Exists(path)) continue;
                string reason;
                if (!IsTrustedNvidiaBinary(path, out reason))
                {
                    rejected = true;
                    continue;
                }

                RuntimePath = path;
                Set(NeuralRenderingState.IntegrationContractUnavailable,
                    "disabled: signed NVIDIA runtime found, but no authorized integration contract is available");
                return;
            }

            Set(rejected ? NeuralRenderingState.RuntimeUntrusted : NeuralRenderingState.RuntimeMissing,
                rejected ? "disabled: NVIDIA Neural Rendering runtime failed signature validation"
                         : "disabled: signed NVIDIA Neural Rendering runtime not found");
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

            string company = FileVersionInfo.GetVersionInfo(path).CompanyName ?? string.Empty;
            if (company.IndexOf("NVIDIA", StringComparison.OrdinalIgnoreCase) < 0)
            {
                reason = "publisher metadata is not NVIDIA";
                return false;
            }

            try
            {
                using (var signer = new X509Certificate2(X509Certificate.CreateFromSignedFile(path)))
                {
                    string subject = signer.Subject ?? string.Empty;
                    bool nvidia = subject.IndexOf("NVIDIA Corporation", StringComparison.OrdinalIgnoreCase) >= 0;
                    bool whcp = subject.IndexOf("Microsoft Windows Hardware Compatibility Publisher", StringComparison.OrdinalIgnoreCase) >= 0;
                    if (!nvidia && !whcp)
                    {
                        reason = "unexpected signer: " + subject;
                        return false;
                    }
                }
            }
            catch
            {
                reason = "signer certificate unavailable";
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

        private static IEnumerable<string> CandidatePaths(string modDir)
        {
            var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            Add(paths, modDir);
            Add(paths, Application.dataPath == null ? null : Path.Combine(Application.dataPath, "Plugins", "x86_64"));
            Add(paths, Application.dataPath == null ? null : Directory.GetParent(Application.dataPath)?.FullName);

            string programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
            string ngx = Path.Combine(programData, "NVIDIA", "NGX");
            AddTree(paths, ngx);

            string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
            DirectoryInfo windows = Directory.GetParent(system);
            string store = windows == null ? null : Path.Combine(windows.FullName, "System32", "DriverStore", "FileRepository");
            if (!string.IsNullOrEmpty(store) && Directory.Exists(store))
            {
                try
                {
                    foreach (string package in Directory.GetDirectories(store, "nv_disp*.inf_amd64_*", SearchOption.TopDirectoryOnly))
                        Add(paths, package);
                }
                catch { }
            }

            return paths;
        }

        private static void Add(ISet<string> paths, string directory)
        {
            if (string.IsNullOrEmpty(directory)) return;
            try { paths.Add(Path.GetFullPath(Path.Combine(directory, RuntimeName))); }
            catch { }
        }

        private static void AddTree(ISet<string> paths, string root)
        {
            if (!Directory.Exists(root)) return;
            Add(paths, root);
            try
            {
                foreach (string directory in Directory.GetDirectories(root, "*", SearchOption.AllDirectories)) Add(paths, directory);
            }
            catch { }
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
