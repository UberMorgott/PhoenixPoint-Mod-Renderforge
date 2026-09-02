using UnityEngine;
using UnityEngine.Rendering;

namespace Renderforge
{
    /// <summary>Everything the pickers, the tooltips and the overlay can be greyed for.</summary>
    internal enum Feature { Dlss, Fsr, Xess, FrameGen }

    /// <summary>The single "can this run right now, and if not why" oracle. Reason(f) == null means available.
    /// DLSS is live on D3D11 and D3D12 (Phase 2); FSR/XeSS/FG arrive in Phases 3-5 and add their
    /// "DLL missing" / "SDK init failed" reasons here and nowhere else.</summary>
    internal static class Availability
    {
        private const int VendorNvidia = 0x10DE;   // PCI vendor id, SystemInfo.graphicsDeviceVendorID

        internal static GraphicsDeviceType Api { get { return SystemInfo.graphicsDeviceType; } }
        internal static bool IsD3D11 { get { return Api == GraphicsDeviceType.Direct3D11; } }
        internal static bool IsD3D12 { get { return Api == GraphicsDeviceType.Direct3D12; } }
        internal static bool IsNvidia { get { return SystemInfo.graphicsDeviceVendorID == VendorNvidia; } }

        /// <summary>Short name for the overlay: "D3D11" / "D3D12" / whatever Unity reports otherwise.</summary>
        internal static string ApiName
        {
            get { return IsD3D11 ? "D3D11" : IsD3D12 ? "D3D12" : Api.ToString(); }
        }

        /// <summary>D3D12 but Unity never called UnityPluginLoad (bit0 of Dlss_UnityIface): the shim was loaded from the mod
        /// folder, not from Plugins\x86_64 — Native.EnsureStaged has copied it there and a restart picks it up.</summary>
        internal static bool NeedsRestart { get { return IsD3D12 && Native.Handle != System.IntPtr.Zero && Native.UnityIface() == 0; } }

        private static string RestartReason
        {
            get { return DlssConfig.Loc("Native plugin staged — restart the game", "Плагин установлен — перезапустите игру"); }
        }

        internal static string Reason(Feature feature)
        {
            switch (feature)
            {
                case Feature.Dlss:
                    if (!IsD3D11 && !IsD3D12)
                        return DlssConfig.Loc("Requires DirectX 11 or DirectX 12", "Требуется DirectX 11 или DirectX 12");
                    if (!IsNvidia)
                        return DlssConfig.Loc("Requires an NVIDIA RTX GPU", "Требуется видеокарта NVIDIA RTX");
                    if (RenderforgeMod.Available) return null;
                    if (NeedsRestart) return RestartReason;
                    return RenderforgeMod.InitCode == Native.DLSS_ERR_NOT_AVAILABLE   // NVIDIA without tensor cores (GTX)
                        ? DlssConfig.Loc("Requires an NVIDIA RTX GPU", "Требуется NVIDIA RTX")
                        : DlssConfig.Loc("DLSS init failed — see the log", "Не удалось инициализировать DLSS — смотрите лог");
                case Feature.Fsr:
                case Feature.Xess:
                case Feature.FrameGen:
                    return NeedsRestart ? RestartReason
                        : IsD3D12
                        ? DlssConfig.Loc("Not implemented yet", "Пока не реализовано")
                        : DlssConfig.Loc("Requires DirectX 12 — switch Renderer", "Требуется DirectX 12 — переключите рендерер");
                default:
                    return null;
            }
        }
    }
}
