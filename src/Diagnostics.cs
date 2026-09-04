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

        internal static void Reset()
        {
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
