using System.IO;

namespace Renderforge
{
    /// <summary>Which upscaler the player asked for. Auto resolves per renderer + GPU (see Resolve).</summary>
    public enum UpscalerKind { Off, Auto, DLSS, FSR, XeSS }

    /// <summary>Everything that depends on WHICH upscaler is active: the Auto order, the provider id handed to the
    /// shim, the quality labels and the display name. One place, so the pickers, the overlay and the driver agree.
    /// Switching provider needs a restart: the shim latches it at Dlss_Init, and tearing an NGX/ffx context down
    /// mid-frame to stand the other up is not worth the risk for a once-per-install setting.</summary>
    internal static class Upscalers
    {
        /// <summary>The upscaler the shim was actually initialised with this session; Off before init.</summary>
        internal static UpscalerKind Running = UpscalerKind.Off;

        internal static UpscalerKind Wanted
        {
            get
            {
                var cfg = RenderforgeMod.Instance != null ? RenderforgeMod.Instance.Cfg : null;
                return cfg == null ? UpscalerKind.Auto : cfg.Upscaler;
            }
        }

        /// <summary>Auto order (spec, D3D12): NVIDIA -> DLSS, else FSR (XeSS: Phase 4), else off. D3D11: DLSS or off.
        /// Decided from HARDWARE facts only (vendor, API, DLLs on disk) because it runs BEFORE Dlss_Init, when
        /// Availability.Reason still says "init failed"; once the shim is up, Running is the answer. A concrete choice
        /// is returned as-is even when unavailable — the picker greys it with the reason.</summary>
        internal static UpscalerKind Resolve(UpscalerKind want)
        {
            if (want != UpscalerKind.Auto) return want;
            if (Running != UpscalerKind.Off) return Running;
            if (Availability.IsD3D11) return Availability.IsNvidia ? UpscalerKind.DLSS : UpscalerKind.Off;
            if (!Availability.IsD3D12) return UpscalerKind.Off;
            if (Availability.IsNvidia) return UpscalerKind.DLSS;
            return FsrDllsPresent ? UpscalerKind.FSR : UpscalerKind.Off;
        }

        internal static int ProviderOf(UpscalerKind k)
        {
            return k == UpscalerKind.FSR ? Native.PROVIDER_FSR : k == UpscalerKind.XeSS ? Native.PROVIDER_XESS : Native.PROVIDER_DLSS;
        }

        internal static Feature FeatureOf(UpscalerKind k)
        {
            return k == UpscalerKind.FSR ? Feature.Fsr : k == UpscalerKind.XeSS ? Feature.Xess : Feature.Dlss;
        }

        /// <summary>The feature the quality row and the overlay should ask Availability about.</summary>
        internal static Feature ActiveFeature { get { return FeatureOf(Resolve(Wanted)); } }

        /// <summary>Quality-row labels. Index order is RenderforgeMode: Off, Auto, then the five ratios.
        /// FSR and XeSS call the 1.0x ratio "Native AA"; NVIDIA calls it "DLAA" (super-resolution-ml.md:59).</summary>
        internal static string[] QualityLabels
        {
            get
            {
                string native = Resolve(Wanted) == UpscalerKind.DLSS ? "DLAA" : "Native AA";
                return new[] { "Off", "Auto", native, "Quality", "Balanced", "Performance", "Ultra Performance" };
            }
        }

        /// <summary>Overlay/log name of the RUNNING provider, with its real version: "FSR 4.1.1", "FSR 3.1.5",
        /// "DLSS SR (nvngx 310.7.129.0)". Falls back to the bare name when the version is unknown.</summary>
        internal static string RunningName(string nvngxVersion)
        {
            switch (Running)
            {
                case UpscalerKind.FSR:
                {
                    string v = Native.ProviderVersion();
                    return v.Length > 0 ? "FSR " + v : "FSR";
                }
                case UpscalerKind.XeSS: return "XeSS";
                case UpscalerKind.DLSS: return "DLSS SR (nvngx " + nvngxVersion + ")";
                default: return "off";
            }
        }

        /// <summary>Both AMD DLLs present next to the mod? Cheap, cached — it is asked on every options repaint.</summary>
        internal static bool FsrDllsPresent
        {
            get
            {
                if (fsrDlls == 0)
                {
                    string dir = RenderforgeMod.ModDir ?? ".";
                    bool ok = File.Exists(Path.Combine(dir, "amd_fidelityfx_loader_dx12.dll"))
                           && File.Exists(Path.Combine(dir, "amd_fidelityfx_upscaler_dx12.dll"));
                    fsrDlls = ok ? 1 : -1;
                }
                return fsrDlls == 1;
            }
        }

        private static int fsrDlls;
    }
}
