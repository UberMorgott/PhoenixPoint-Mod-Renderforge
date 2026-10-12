namespace Renderforge
{
    /// <summary>Runtime-only development levers. They deliberately are not ModConfig fields: every game
    /// launch starts from the single production-tested path, while PPCLI can still change them temporarily
    /// during controlled diagnostics.</summary>
    internal static class Diagnostics
    {
        internal static DebugView View { get; set; }
        internal static bool MvJittered { get; set; }
        internal static bool D3D12SrgbViews { get; set; }
        internal static bool D3D12ColorDesc { get; set; }
        internal static bool D3D12HalfColor { get; set; }
        internal static int JitterReportSignX { get; set; }
        internal static int JitterReportSignY { get; set; }
        internal static float JitterScale { get; set; }
        internal static bool JitterReportSwapXY { get; set; }
        internal static bool JitterConstEnabled { get; set; }
        internal static float JitterConstX { get; set; }
        internal static float JitterConstY { get; set; }
        internal static bool ForceReset { get; set; }
        /// <summary>Dlss_SetBiasMaskMode: 0 = no bias-current-colour mask bound, 1 = all-zero, 2 = all-one (D3D11 NGX only).</summary>
        internal static int BiasMaskMode { get; set; }
        /// <summary>GeoMarkerOverlay on/off (A-B lever for the geoscape marker ghosting; production = on).</summary>
        internal static bool MarkerOverlay { get; set; }
        /// <summary>GeoMarkerOverlay takes the aircraft too (A-B lever; production = on).</summary>
        internal static bool MarkerCraft { get; set; }
        /// <summary>GeoMarkerOverlay camera pose: -1 = by API (production: the previous frame's pose on D3D12, where the
        /// upscaled picture is one frame behind the scene), 0 = this frame's, 1 = the previous frame's.</summary>
        internal static int MarkerPoseDelay { get; set; }
        /// <summary>GeoMarkerOverlay lighting of the marker layer (A-B lever, bits; production = 7): 1 = the lights of Default
        /// light it, 2 = the directional ones per pixel while the present camera renders, 4 = with their shadows.</summary>
        internal static int MarkerLights { get; set; }
        /// <summary>MotionVectorFilter on/off (A-B lever for the geoscape softness; production = on).</summary>
        internal static bool MvFilter { get; set; }
        /// <summary>Depth copy source (A-B lever): 0 = by rendering path (production), 1 = force BuiltinRenderTextureType.Depth,
        /// 2 = force ResolvedDepth.</summary>
        internal static int DepthSource { get; set; }
        /// <summary>Report the jitter of the previous (1) or the next (-1) frame instead of this one's (frame-pairing A-B lever).</summary>
        internal static int JitterReportDelay { get; set; }

        // Frame-time sampler (PPCLI FrameStats): the driver's Update records Time.unscaledDeltaTime while Sampling.
        internal static bool Sampling;
        private static readonly float[] frameTimes = new float[16384];
        private static int frameTimeCount;
        private static float sampleStart;

        internal static void SampleFrame(float dt) { if (frameTimeCount < frameTimes.Length) frameTimes[frameTimeCount++] = dt; }

        /// <summary>start = true: clear and record; false: stop and report base-frame count, fps and frame-time percentiles (ms).</summary>
        internal static string FrameStats(bool start)
        {
            if (start) { frameTimeCount = 0; sampleStart = UnityEngine.Time.realtimeSinceStartup; Sampling = true; return "sampling"; }
            Sampling = false;
            int n = frameTimeCount;
            if (n == 0) return "frames=0";
            float span = UnityEngine.Time.realtimeSinceStartup - sampleStart;
            var s = new float[n];
            System.Array.Copy(frameTimes, s, n);
            System.Array.Sort(s);
            System.Func<float, string> ms = v => (v * 1000f).ToString("F2", System.Globalization.CultureInfo.InvariantCulture);
            return "frames=" + n + " fps=" + (n / UnityEngine.Mathf.Max(span, 1e-3f)).ToString("F1", System.Globalization.CultureInfo.InvariantCulture)
                 + " p50=" + ms(s[n / 2]) + " p99=" + ms(s[System.Math.Min(n - 1, n * 99 / 100)]) + " max=" + ms(s[n - 1]);
        }

        internal static void Reset()
        {
            Sampling = false;
            MvFilter = true;
            DepthSource = 0;
            JitterReportDelay = 0;
            BiasMaskMode = 0;
            MarkerOverlay = true;
            MarkerCraft = true;
            MarkerPoseDelay = -1;
            MarkerLights = 7;
            View = DebugView.None;
            MvJittered = false;
            D3D12SrgbViews = false;
            D3D12ColorDesc = false;
            D3D12HalfColor = true;
            JitterReportSignX = 1;
            JitterReportSignY = 1;
            JitterScale = 1f;
            JitterReportSwapXY = false;
            JitterConstEnabled = false;
            JitterConstX = 0f;
            JitterConstY = 0f;
            ForceReset = false;
        }
    }
}
