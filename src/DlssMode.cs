namespace Renderforge
{
    /// <summary>Serialized by ORDINAL into ModConfig.json, so new values are APPENDED, never inserted.
    /// UltraQuality (1.5x) / UltraQualityPlus (1.3x) exist only on XeSS; the quality row offers them only while
    /// XeSS is the resolved upscaler, and DLSS/FSR run them as Quality.</summary>
    public enum RenderforgeMode { Off, Auto, DLAA, Quality, Balanced, Performance, UltraPerformance, UltraQuality, UltraQualityPlus }
}
