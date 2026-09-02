using System;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Managed side of the frame-generation chain. The native shim owns the Present hook and the
    /// shadow swapchain; this class only decides WHICH provider and WHEN, retries the init until the hook has
    /// seen a Present (it cannot build the chain before that), and exposes the status for PPCLI and the overlay.</summary>
    internal static class FrameGen
    {
        private static int wantProvider, wantMultiplier, builtMultiplier;
        private static bool live;
        private static float retryAt;
        private static int lastRc = -1;

        internal static bool Live => live;
        internal static int Provider => live ? Native.Fg_Provider() : Native.FG_PROVIDER_NONE;
        internal static uint Caps => live ? Native.Fg_Caps() : 0u;
        internal static int LastResult => lastRc;

        internal static string ProviderName(int id)
        {
            switch (id)
            {
                case Native.FG_PROVIDER_FSR: return "FSR";
                case Native.FG_PROVIDER_XESS: return "XeSS";
                case Native.FG_PROVIDER_DLSS: return "DLSS";
                default: return "none";
            }
        }

        private static int forced = -1;

        /// <summary>PPCLI/test override: -1 = auto, else an FG_PROVIDER_* id. Tears the live chain down so Apply rebuilds it.</summary>
        internal static void Force(int provider) { forced = provider; Stop(); }

        internal static int Multiplier(FrameGenMode m) => m == FrameGenMode.X4 ? 4 : m == FrameGenMode.X3 ? 3 : m == FrameGenMode.X2 ? 2 : 0;

        /// <summary>Which vendor drives FG on this GPU: NVIDIA -> DLSS-G, everything else -> FSR-FG (cross-vendor),
        /// with XeSS-FG as the second cross-vendor option. Mirrors the spec's Auto order for D3D12.</summary>
        internal static int AutoProvider()
        {
            if (forced >= 0) return forced;
            if (Availability.IsNvidia) return Native.FG_PROVIDER_DLSS;
            return Native.FG_PROVIDER_FSR;
        }

        /// <summary>Called from RenderforgeMod on config change and from the driver every frame while live.</summary>
        internal static void Apply(DlssConfig cfg)
        {
            int mult = Multiplier(cfg.FrameGen);
            if (!Availability.IsD3D12 || mult == 0 || Availability.Reason(Feature.FrameGen) != null)
            {
                Stop();
                return;
            }
            wantProvider = AutoProvider();
            wantMultiplier = mult;
            if (live && builtMultiplier != mult) Release();   // the multiplier is baked into the chain
            if (live) { Native.Fg_SetEnabled(1); return; }
            Retry();
        }

        /// <summary>The shim tears the chain down on its own (ResizeBuffers, shadow Present failure); mirror that.</summary>
        private static void SyncAlive()
        {
            if (!live || Native.Fg_Alive() != 0) return;
            live = false;
            lastRc = -1;
            Overlay.FgFps = 0;
            RenderforgeMod.Instance?.Logger.LogInfo("FG chain torn down by the shim: " + Native.Fg_Status());
        }

        /// <summary>The chain can only be built after the Present hook has seen a frame, so the first attempts
        /// legitimately return FG_ERR_NO_SWAPCHAIN. Retry four times a second, log once per distinct code.</summary>
        internal static void Retry()
        {
            SyncAlive();
            if (live || wantMultiplier == 0) return;
            if (Time.unscaledTime < retryAt) return;
            retryAt = Time.unscaledTime + 0.25f;
            int rc = Native.Fg_Init(wantProvider, (uint)wantMultiplier, RenderforgeMod.ModDir);
            if (rc != lastRc)
            {
                lastRc = rc;
                RenderforgeMod.Instance?.Logger.LogInfo("FG init " + ProviderName(wantProvider) + " " + wantMultiplier + "x -> " + rc);
            }
            if (rc != Native.FG_OK) return;
            live = true;
            builtMultiplier = wantMultiplier;
            Native.Fg_SetEnabled(1);
            RenderforgeMod.Instance?.Logger.LogInfo("FG live: " + Native.Fg_Status());
        }

        /// <summary>User/config off: forget the wish and tear the chain down.</summary>
        internal static void Stop()
        {
            wantMultiplier = 0;
            Release();
        }

        /// <summary>Driver teardown (the chain references outRT/depthRT/mvRT, which are about to die): tear the
        /// chain down but keep the wish, so the next live generation's Retry() rebuilds it.</summary>
        internal static void Release()
        {
            if (!live) return;
            Native.Fg_SetEnabled(0);
            Native.Fg_Shutdown();
            live = false;
            lastRc = -1;
            Overlay.FgFps = 0;
        }

        internal static string Status() => (live ? "live " : "off ") + Native.Fg_Status();
    }
}
