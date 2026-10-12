using System;
using UnityEngine;
using UnityEngine.LowLevel;
using UnityEngine.PlayerLoop;

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
        private static bool retiring;
        internal static bool Retiring => retiring;
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
        internal static bool Exhausted => !live && (pacingFailed || AutoProvider() < 0);
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

        // ---------------------------------------------------------------- pacing (docs\DESIGN.md "FG pacing")
        //
        // While a chain is live two systems sit in Unity's PlayerLoop (UnityEngine.LowLevel.PlayerLoop, Unity 2019.3+):
        //   FrameStart  first in Initialization - before PlayerUpdateTime samples the clock and long before
        //               EarlyUpdate.UpdateInputManager reads input: the latency SDK's sleep (Reflex / XeLL want it at
        //               the start of the frame, before input) + SIMULATION_START, through Fg_FrameBegin;
        //   SimEnd      last in PreLateUpdate, i.e. after ScriptRunBehaviourLateUpdate: SIMULATION_END.
        // The render-submit / present markers follow on the render thread with the id Fg_FrameBegin returned (the shim
        // stamps it into the block Fg_SetFrame stores). No chain = no systems: nothing of this runs with FG off.
        //
        // FLOWING = the driver fed the chain a frame recently (DlssDriver calls Fed with every FG_PREPARE it issues). Only
        // then does a provider generate and its pacer sleep; a live chain nothing is fed to (main menu, loading screen, a
        // camera-less screen, HoldPrepare) is paced by Unity's own limiter at the full ceiling (RenderforgeMod.ApplyFrameRate).
        // The feed is the managed side's word only. Whether the shim PREPARED those frames is checked against
        // Fg_PreparedCount: a chain that is fed and prepares nothing for StarveFrames frames and StarveSeconds (twins
        // that cannot be built, a refused option, a provider prepare that fails for good) would otherwise leave the
        // rendered frames capped at ceiling / multiplier with nothing generated - it is torn down and its provider marked
        // failed for the session, like a provider whose init failed (Auto moves on, the picker shows the reason).
        private struct FrameStartSystem { }
        private struct SimEndSystem { }
        private const int IdleAfter = 3;       // frames without a fed one before the provider's pacer hands back to Unity
        private const int StarveFrames = 120;
        private const float StarveSeconds = 2f;
        private static bool hooked, fedThisFrame, flowing, mainPacing = true;
        private static int unfed = IdleAfter, startFrame = -1;
        private static uint paceId, preparedSeen;
        private static int starved;
        private static float starvedSince;
        private static bool pacingFailed;      // FrameStart / SimEnd threw: FG stays off until the provider is forced again
        private static bool starvedOut;        // Starved() marked the provider failed: the next Pump releases the chain

        /// <summary>Frames are reaching the live chain: the provider generates and (XeSS-FG, DLSS-G) owns the frame cap
        /// once its SDK accepted it (Native.FG_PACER_HELD).</summary>
        internal static bool Flowing => live && flowing;
        private static int pacer;              // FG_PACER_* bits ApplyFrameRate last decided on (FrameStart re-decides on a change)
        internal static void NotePacer(int bits) { pacer = bits; }
        internal static void Fed() { fedThisFrame = true; }

        /// <summary>Diagnostics (FgCaps): PostProcessLayer.OnPreCull calls of the bound layer beyond the first inside one
        /// frame, and how many of those would have fed the chain a second time (DlssDriver feeds a frame once).</summary>
        internal static int DupPreCull, DupFeeds;

        /// <summary>Diagnostic A/B: false = the latency SDK sleeps on the render thread. Rebuilds the chain (latched at Create).</summary>
        internal static void SetMainPacing(bool on) { mainPacing = on; Stop(); Native.Fg_SetMainPacing(on ? 1 : 0); }

        private static void FrameStart()
        {
            try
            {
                startFrame = Time.frameCount;
                if (!live || pacingFailed) { paceId = 0; return; }
                if (!fedThisFrame) starved = 0;
                else if (Starved()) { paceId = 0; return; }
                unfed = fedThisFrame ? 0 : Math.Min(unfed + 1, IdleAfter);
                fedThisFrame = false;
                bool now = unfed < IdleAfter;
                // Who paces follows the SDK's ANSWER, which arrives on other threads (a quiescent point accepts or
                // refuses the cap, an option is refused): re-decide whenever what the provider holds changes.
                if (now != flowing || Native.Fg_PacerState() != pacer) { flowing = now; RenderforgeMod.ApplyFrameRate(); }
                paceId = Native.Fg_FrameBegin(flowing ? 1 : 0);
            }
            catch (Exception ex) { PacingFailed(ex); }
        }

        /// <summary>The previous frame was fed. True = the chain has prepared nothing for too long and was torn down.</summary>
        private static bool Starved()
        {
            uint n = Native.Fg_PreparedCount();
            if (n != preparedSeen) { preparedSeen = n; starved = 0; return false; }
            if (starved++ == 0) starvedSince = Time.unscaledTime;
            if (starved < StarveFrames || Time.unscaledTime - starvedSince < StarveSeconds) return false;
            int prov = Native.Fg_Provider();
            failed |= 1 << prov;
            lastReason = ProviderName(prov) + ": frames could not be prepared - see renderforge_fg.log";
            RenderforgeMod.Instance?.Logger.LogError("FG " + ProviderName(prov) + " takes frames and prepares none (" + starved + " consecutive fed frames, "
                + (Time.unscaledTime - starvedSince).ToString("F1") + " s) - tearing the chain down, provider off for this session: " + Native.Fg_Status());
            starved = 0; flowing = false;
            starvedOut = true;                  // Pump tears down (same frame): the PlayerLoop is not edited from inside one of its systems
            return true;
        }

        private static void SimEnd()
        {
            // Every frame FrameStart opened natively, paced or not (the shim knows which id is open; none = no-op).
            try { if (live && !pacingFailed) Native.Fg_SimEnd(); }
            catch (Exception ex) { PacingFailed(ex); }
        }

        /// <summary>A pacing system threw (it runs inside Unity's PlayerLoop, so nothing above can catch it). The chain
        /// cannot be paced any more: every later prepare would be refused for lack of a pace id and FG would sit there
        /// live, capped and generating nothing. Only flags here - the PlayerLoop is not edited from inside one of its own
        /// systems; the next Pump (same frame, driver Update) tears the chain down and Apply keeps FG off.</summary>
        private static void PacingFailed(Exception ex)
        {
            if (pacingFailed) return;
            pacingFailed = true;
            paceId = 0; flowing = false;
            lastReason = "frame pacing failed - see the log";
            RenderforgeMod.Instance?.Logger.LogError("FG pacing failed, frame generation off: " + ex);
        }

        /// <summary>Insert / remove the two systems. Entries are matched by type NAME, so a reloaded mod assembly
        /// removes what its previous incarnation left behind.</summary>
        private static void Hook(bool on)
        {
            if (on == hooked) return;
            hooked = on;
            startFrame = Time.frameCount;
            PlayerLoopSystem loop = PlayerLoop.GetCurrentPlayerLoop();
            PlayerLoopSystem[] top = loop.subSystemList;
            if (top == null) return;
            for (int i = 0; i < top.Length; i++)
            {
                if (top[i].type == typeof(Initialization)) top[i].subSystemList = Edit(top[i].subSystemList, typeof(FrameStartSystem), on, true, FrameStart);
                else if (top[i].type == typeof(PreLateUpdate)) top[i].subSystemList = Edit(top[i].subSystemList, typeof(SimEndSystem), on, false, SimEnd);
            }
            loop.subSystemList = top;
            PlayerLoop.SetPlayerLoop(loop);
        }

        private static PlayerLoopSystem[] Edit(PlayerLoopSystem[] list, Type marker, bool add, bool first, PlayerLoopSystem.UpdateFunction fn)
        {
            var l = new System.Collections.Generic.List<PlayerLoopSystem>(list ?? new PlayerLoopSystem[0]);
            l.RemoveAll(s => s.type != null && s.type.FullName == marker.FullName);
            if (add)
            {
                var sys = new PlayerLoopSystem { type = marker, updateDelegate = fn };
                if (first) l.Insert(0, sys); else l.Add(sys);
            }
            return l.ToArray();
        }

        /// <summary>The chain went live / away: (un)hook and start from "nothing fed yet".</summary>
        private static void Pacing(bool on)
        {
            flowing = false; fedThisFrame = false; unfed = IdleAfter; paceId = 0; starved = 0; preparedSeen = 0;
            try { Hook(on); }
            catch (Exception ex) { hooked = false; RenderforgeMod.Instance?.Logger.LogError("FG pacing hook failed: " + ex); }
        }

        private static int forced = -1;

        /// <summary>PPCLI/test override: -1 = auto, else an FG_PROVIDER_* id. Tears the live chain down so Apply rebuilds it.</summary>
        internal static void Force(int provider) { forced = provider; failed = 0; pacingFailed = false; Stop(); }

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
            if (forced >= 0) return (failed & (1 << forced)) == 0 ? forced : -1;
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
            if (!Usable || mult == 0 || pacingFailed || Availability.Reason(Feature.FrameGen) != null)
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
            if (Native.Fg_Pump() == 0) retiring = true;
            else if (retiring) retiring = Native.Fg_Shutdown() == 0;
            SyncAlive();
            if (pacingFailed) { if (live || hooked) { Release(); Pacing(false); RenderforgeMod.ApplyFrameRate(); } return; }
            if (starvedOut) { starvedOut = false; if (live) { Release(); wantProvider = AutoProvider(); } return; }
            // Somebody replaced the PlayerLoop (SetPlayerLoop from another mod) and our systems went with it. FrameStart is
            // the first system of Initialization, BEFORE PlayerUpdateTime advances Time.frameCount, so in frame N it
            // records N - 1 and a healthy frame reads a difference of exactly 1 here; 2 = this very frame was not opened.
            // Put the systems back and open the frame late rather than not at all: FrameStart forgets the stale pace id (a
            // prepare stamped with it would be refused as a duplicate and the frame presented the idle way: child hidden,
            // history reset) and keeps the fed / flowing state, so the provider goes on pacing without an idle gap.
            if (live && hooked && Time.frameCount - startFrame > 1)
            {
                hooked = false;
                try { Hook(true); }
                catch (Exception ex) { hooked = false; RenderforgeMod.Instance?.Logger.LogError("FG pacing hook failed: " + ex); return; }
                RenderforgeMod.Instance?.Logger.LogInfo("FG pacing systems were dropped from the PlayerLoop (frame " + Time.frameCount + ") - restored");
                FrameStart();
            }
        }

        /// <summary>The shim detached the chain on its own; mirror that.</summary>
        private static void SyncAlive()
        {
            if (!live || Native.Fg_Alive() != 0) return;
            live = false;
            lastRc = -1;
            Overlay.FgFps = 0;
            Pacing(false);
            RenderforgeMod.ApplyFrameRate();
            RenderforgeMod.Instance?.Logger.LogInfo("FG chain torn down by the shim: " + Native.Fg_Status());
        }

        /// <summary>The chain can only be built after the Present hook has seen a frame, so the first attempts
        /// legitimately return FG_ERR_NO_SWAPCHAIN. Retry four times a second, log once per distinct code. A
        /// terminal code moves Auto on to the next provider.</summary>
        internal static void Retry()
        {
            Pump();
            if (live || retiring || DlssDriver.Instance?.Retiring == true || wantMultiplier == 0 || wantProvider < 0) return;
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
            Pacing(true);
            RenderforgeMod.ApplyFrameRate();
            RenderforgeMod.Instance?.Logger.LogInfo("FG live: " + Native.Fg_Status());
        }

        /// <summary>User/config off: forget the wish and tear the chain down.</summary>
        internal static void Stop()
        {
            wantMultiplier = 0;
            Release();
        }

        /// <summary>Stop publishing frames, but retain ownership until native teardown confirms completion.
        /// The driver keeps its Unity textures while this returns false; Pump/Release retry without re-init.</summary>
        internal static bool Release()
        {
            if (!Usable) return true;
            bool wasLive = live;
            if (wasLive) Native.Fg_SetEnabled(0);
            retiring = Native.Fg_Shutdown() == 0;
            live = false;
            lastRc = -1;
            Overlay.FgFps = 0;
            if (wasLive) { Pacing(false); RenderforgeMod.ApplyFrameRate(); }
            return !retiring;
        }

        internal static string Status() => (live ? "live " : "off ") + Native.Fg_Status();
    }
}
