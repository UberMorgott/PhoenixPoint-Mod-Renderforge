using System;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Managed side of the frame-generation chain. The native shim owns the Present hook and the
    /// shadow swapchain; this class only decides WHICH provider and WHEN, retries the init until the hook has
    /// seen a Present (it cannot build the chain before that), pumps the shim's deferred teardown every frame
    /// (the shim's render/UI threads only detach a chain; the main thread destroys it), and exposes the status
    /// for PPCLI and the overlay.</summary>
    internal static class FrameGen
    {
        private static int wantProvider, wantMultiplier, builtMultiplier;
        private static bool live;
        private static float retryAt;
        private static int lastRc = -1;
        private static int failed;             // bitmask of FG_PROVIDER_* ids that failed terminally this session
        private static string lastReason = "";

        private static bool Usable => Availability.IsD3D12 && Native.Handle != IntPtr.Zero;

        internal static bool Live => live;
        /// <summary>Total frames presented per rendered frame. A configured mode is not enough: until the
        /// provider is actually live (or after it fails), the game must retain the full output-FPS limit.</summary>
        internal static int OutputMultiplier => live ? builtMultiplier : 1;
        /// <summary>Test knob (PPCLI `set`): true = the driver stops feeding frames (Fg_SetFrame / FG_PREPARE) while the chain
        /// stays live - the "menu screen without a rendering camera" state the shim must idle through.</summary>
        public static bool HoldPrepare = false;
        internal static int Provider => live ? Native.Fg_Provider() : Native.FG_PROVIDER_NONE;
        /// <summary>The shim keeps the last provider's caps after a refused multiplier too, so 3x/4x grey out without a live chain.</summary>
        internal static uint Caps => Usable ? Native.Fg_Caps() : 0u;
        internal static int LastResult => lastRc;
        /// <summary>Every provider Auto could try on this GPU failed this session (the picker greys FG with LastReason).</summary>
        internal static bool Exhausted => !live && AutoProvider() < 0;
        internal static string LastReason => lastReason;

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
        internal static void Force(int provider) { forced = provider; failed = 0; Stop(); }

        internal static int Multiplier(FrameGenMode m) => m == FrameGenMode.X4 ? 4 : m == FrameGenMode.X3 ? 3 : m == FrameGenMode.X2 ? 2 : 0;

        private static bool Has(int provider)
        {
            switch (provider)
            {
                case Native.FG_PROVIDER_DLSS: return Upscalers.SlDllsPresent;
                case Native.FG_PROVIDER_XESS: return Upscalers.XessFgDllsPresent;
                case Native.FG_PROVIDER_FSR: return Upscalers.FsrFgDllPresent;
                default: return true;
            }
        }

        /// <summary>Auto order (D3D12): NVIDIA -> DLSS-G (Streamline DLLs present), then FSR-FG, then XeSS-FG;
        /// Intel -> XeSS-FG, then FSR-FG; everything else -> FSR-FG, then XeSS-FG. Providers that failed terminally
        /// this session (FG_ERR_NO_PROVIDER / FG_ERR_PROVIDER_FAILED) and providers whose DLLs are absent are
        /// skipped; -1 = nothing left to try.</summary>
        internal static int AutoProvider()
        {
            if (forced >= 0) return forced;
            int[] order = Availability.IsNvidia ? new[] { Native.FG_PROVIDER_DLSS, Native.FG_PROVIDER_FSR, Native.FG_PROVIDER_XESS }
                        : Availability.IsIntel ? new[] { Native.FG_PROVIDER_XESS, Native.FG_PROVIDER_FSR }
                        : new[] { Native.FG_PROVIDER_FSR, Native.FG_PROVIDER_XESS };
            foreach (int p in order)
                if ((failed & (1 << p)) == 0 && Has(p)) return p;
            return -1;
        }

        /// <summary>Called from RenderforgeMod on config change and from the driver every frame while live.</summary>
        internal static void Apply(DlssConfig cfg)
        {
            int mult = Multiplier(cfg.FrameGen);
            if (!Usable || mult == 0 || Availability.Reason(Feature.FrameGen) != null)
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

        /// <summary>Every frame on the main thread: the shim's render/UI threads only DETACH a chain (resize, Present
        /// failure, window recreated); Fg_Pump destroys it here, and `live` follows.</summary>
        internal static void Pump()
        {
            if (!Usable) return;
            Native.Fg_Pump();
            SyncAlive();
        }

        /// <summary>The shim detached the chain on its own; mirror that.</summary>
        private static void SyncAlive()
        {
            if (!live || Native.Fg_Alive() != 0) return;
            live = false;
            lastRc = -1;
            Overlay.FgFps = 0;
            RenderforgeMod.ApplyFrameRate();
            RenderforgeMod.Instance?.Logger.LogInfo("FG chain torn down by the shim: " + Native.Fg_Status());
        }

        /// <summary>The chain can only be built after the Present hook has seen a frame, so the first attempts
        /// legitimately return FG_ERR_NO_SWAPCHAIN. Retry four times a second, log once per distinct code. A
        /// terminal code moves Auto on to the next provider.</summary>
        internal static void Retry()
        {
            Pump();
            if (live || wantMultiplier == 0 || wantProvider < 0) return;
            if (Time.unscaledTime < retryAt) return;
            retryAt = Time.unscaledTime + 0.25f;
            int rc = Native.Fg_Init(wantProvider, (uint)wantMultiplier, RenderforgeMod.ModDir);
            if (rc != lastRc)
            {
                lastRc = rc;
                RenderforgeMod.Instance?.Logger.LogInfo("FG init " + ProviderName(wantProvider) + " " + wantMultiplier + "x -> " + rc);
            }
            if (rc == Native.FG_ERR_NO_PROVIDER || rc == Native.FG_ERR_PROVIDER_FAILED)
            {
                failed |= 1 << wantProvider;
                lastReason = ProviderName(wantProvider) + ": " + Native.Fg_Reason();
                int next = AutoProvider();
                RenderforgeMod.Instance?.Logger.LogInfo("FG " + ProviderName(wantProvider) + " unavailable (" + lastReason + ") -> "
                    + (next >= 0 ? "trying " + ProviderName(next) : "no provider left"));
                wantProvider = next;
                lastRc = -1;
                return;
            }
            if (rc != Native.FG_OK) return;
            live = true;
            builtMultiplier = wantMultiplier;
            Native.Fg_SetEnabled(1);
            RenderforgeMod.ApplyFrameRate();
            RenderforgeMod.Instance?.Logger.LogInfo("FG live: " + Native.Fg_Status());
        }

        /// <summary>User/config off: forget the wish and tear the chain down.</summary>
        internal static void Stop()
        {
            wantMultiplier = 0;
            Release();
        }

        /// <summary>Driver teardown (the chain references outRT/depthRT/mvRT, which are about to die): tear the
        /// chain down but keep the wish, so the next live generation's Retry() rebuilds it. Fg_Shutdown returns
        /// only once the chain is destroyed.</summary>
        internal static void Release()
        {
            if (!live) return;
            Native.Fg_SetEnabled(0);
            if (Native.Fg_Shutdown() == 0)
                RenderforgeMod.Instance?.Logger.LogWarning("FG shutdown: the render thread never left the chain - parked, Fg_Pump retries: " + Native.Fg_Status());
            live = false;
            lastRc = -1;
            Overlay.FgFps = 0;
            RenderforgeMod.ApplyFrameRate();
        }

        internal static string Status() => (live ? "live " : "off ") + Native.Fg_Status();
    }
}
