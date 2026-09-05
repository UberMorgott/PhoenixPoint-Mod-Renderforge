using System;
using Base.Core;
using Base.Lighting;
using HarmonyLib;
using PhoenixPoint.Common.Core;
using UnityEngine;
using UnityEngine.Rendering.PostProcessing;

namespace Renderforge
{
    /// <summary>Graphics knobs the game never exposes: vignette off, very-high shadow maps, forced 16x anisotropic
    /// filtering, LOD bias. "Vanilla" always means "write the captured baseline back", never "skip the write" -
    /// otherwise turning a knob off would leave the mod's value in place until the next preset change.
    ///
    /// Vanilla writes QualitySettings inside OptionsManager.UsePreset (OptionsManager.cs:384), which nests
    /// ChangeGraphicsQuality (:391) -> OnGraphicsSettingsChangedEvent (:406) -> LightingManager.ApplyPostProcessOptions
    /// (LightingManager.cs:53-55). So the whole nesting runs under a guard: nothing of ours is written while vanilla is
    /// mid-apply, and the baseline is taken the moment the nesting unwinds.</summary>
    public static class QualityKnobs
    {
        // Depth, not a bool: UsePreset is not documented as non-reentrant, and a nested call must not clear the guard early.
        [ThreadStatic] private static int usePresetDepth;
        internal static bool InUsePreset { get { return usePresetDepth > 0; } }

        private static bool haveSnapshot;
        private static AnisotropicFiltering baseAniso;
        private static float baseLodBias;
        private static ShadowResolution baseShadowRes;
        private static bool loggedError;

        /// <summary>What vanilla left behind. Taken when UsePreset unwinds, and - as a fallback - immediately before the
        /// mod's first write, for the case where a preset was applied before Harmony was installed.</summary>
        internal static void Snapshot()
        {
            baseAniso = QualitySettings.anisotropicFiltering;
            baseLodBias = QualitySettings.lodBias;
            baseShadowRes = QualitySettings.shadowResolution;
            haveSnapshot = true;
        }

        internal static void EnterUsePreset() { usePresetDepth++; }

        /// <summary>Guard down, take the fresh baseline, put our values back on top. Called from a HarmonyFinalizer, so a
        /// throw inside UsePreset cannot leave the guard latched and the knobs frozen.
        ///
        /// <paramref name="succeeded"/> is false when UsePreset threw. That path must NOT snapshot: UsePreset's first
        /// statement is DefinedPresetIndexToQualityIndex (OptionsManager.cs:377), which indexes DefinedPresets unguarded
        /// (:447), so an invalid preset index throws BEFORE QualitySettings.SetQualityLevel (:384) — vanilla wrote
        /// nothing, and the live QualitySettings are still OUR overrides. Snapshotting there would record the mod's own
        /// values as "Vanilla" and the baseline would be lost for good. Unwind the guard always, capture only on success.</summary>
        internal static void LeaveUsePreset(bool succeeded)
        {
            if (usePresetDepth > 0) usePresetDepth--;
            if (usePresetDepth > 0) return;
            if (succeeded) Snapshot();
            ApplyAll();   // re-assert our values either way: a partial apply may have clobbered them
        }

        /// <summary>Every seam that needs the full set: OnLevelStart, OnConfigChanged, the UI rows, the UsePreset unwind.</summary>
        public static void ApplyAll()
        {
            ApplyScalars();
            ApplyVignette(null);
        }

        /// <summary>QualitySettings writes. Suppressed while vanilla is applying a preset - the UsePreset finalizer
        /// re-runs this the moment the nesting unwinds.</summary>
        internal static void ApplyScalars()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null || InUsePreset) return;
            try
            {
                if (!haveSnapshot) Snapshot();     // never restore an uninitialised snapshot
                if (cfg.Anisotropic == AnisotropicMode.Force16)
                {
                    QualitySettings.anisotropicFiltering = AnisotropicFiltering.ForceEnable;
                    Texture.SetGlobalAnisotropicFilteringLimits(16, 16);
                }
                else
                {
                    QualitySettings.anisotropicFiltering = baseAniso;
                    // (-1, -1) is the engine default (no forced limits). Coexistence with another mod's limits is not attempted.
                    Texture.SetGlobalAnisotropicFilteringLimits(-1, -1);
                }
                QualitySettings.lodBias = cfg.LodBias > 0f ? Mathf.Clamp(cfg.LodBias, 1f, 4f) : baseLodBias;
                QualitySettings.shadowResolution = cfg.ShadowResolution == ShadowResolutionMode.VeryHigh
                    ? ShadowResolution.VeryHigh
                    : baseShadowRes;
            }
            catch (Exception ex) { Log("scalar apply failed", ex); }
        }

        /// <summary>Filled in by Task 4.</summary>
        internal static void ApplyVignette(LightingManager known) { }

        /// <summary>PPCLI readback: {"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"Status"}.
        /// Config value, live QualitySettings value and the captured baseline, side by side.</summary>
        public static string Status()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return "mod not enabled";
            return "cfg vignette=" + cfg.Vignette + " shadowRes=" + cfg.ShadowResolution
                 + " aniso=" + cfg.Anisotropic + " lodBias=" + cfg.LodBias.ToString("R")
                 + " | live aniso=" + QualitySettings.anisotropicFiltering
                 + " lodBias=" + QualitySettings.lodBias.ToString("R")
                 + " shadowRes=" + QualitySettings.shadowResolution
                 + " | base have=" + haveSnapshot + " aniso=" + baseAniso
                 + " lodBias=" + baseLodBias.ToString("R") + " shadowRes=" + baseShadowRes
                 + " | inUsePreset=" + InUsePreset;
        }

        /// <summary>PPCLI setter: {"member":"SetQuality","args":["Off","VeryHigh","Force16",2.0]}. Applies live and saves.</summary>
        public static string SetQuality(string vignette, string shadowRes, string aniso, float lodBias)
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return "mod not enabled";
            VignetteMode v; ShadowResolutionMode s; AnisotropicMode a;
            if (!Enum.TryParse(vignette, true, out v)) return "bad vignette '" + vignette + "' (Vanilla / Off)";
            if (!Enum.TryParse(shadowRes, true, out s)) return "bad shadow resolution '" + shadowRes + "' (Vanilla / VeryHigh)";
            if (!Enum.TryParse(aniso, true, out a)) return "bad anisotropic '" + aniso + "' (Vanilla / Force16)";
            cfg.Vignette = v; cfg.ShadowResolution = s; cfg.Anisotropic = a;
            cfg.LodBias = lodBias > 0f ? Mathf.Clamp(lodBias, 1f, 4f) : 0f;
            ApplyAll();
            RenderforgeMod.SaveConfig();
            // QualityPanel.Sync();  // Task 5
            return Status();
        }

        /// <summary>PPCLI test lever for the acceptance run: applies a graphics preset by index the way the options screen
        /// does (OptionsManager.cs:375), so the UsePreset seam can be exercised without driving the UI.
        /// {"member":"UsePreset","args":[3]}</summary>
        public static string UsePreset(int index)
        {
            var options = GameUtl.GameComponent<OptionsManager>();
            if (options == null) return "no OptionsManager";
            options.UsePreset(index, false);
            return "preset=" + index + " | " + Status();
        }

        private static void Log(string what, Exception ex)
        {
            if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge quality knobs " + what + ": " + ex);
            loggedError = true;
        }
    }
}
