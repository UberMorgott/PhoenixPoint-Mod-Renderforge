namespace Renderforge
{
    /// <summary>Colour-vision (daltonization) gate. The Options → Graphics picker row lands in the next task.</summary>
    internal static class ColorVisionPanel
    {
        /// <summary>The ONE activation gate. Both DlssDriver.Step (start the pipeline) and the per-frame
        /// submission (Dlss_SetColorVision) call this, so they cannot drift apart and leave an uncorrected
        /// passthrough pass running on the geoscape. Tactical-only, mirroring lutPreset in DlssDriver — the post
        /// pass exists on the tactical camera only.</summary>
        internal static bool Active(DlssConfig cfg) => RenderforgeMod.TacticalActive && cfg != null
            && cfg.ColorVision >= ColorVisionMode.Deuteranopia && cfg.ColorVision <= ColorVisionMode.Tritanopia;
    }
}
