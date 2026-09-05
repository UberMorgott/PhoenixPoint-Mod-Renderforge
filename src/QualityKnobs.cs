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
    /// (LightingManager.cs:53-55). So the whole nesting runs under a guard: the outgoing level gets the baseline back
    /// before vanilla switches, nothing of ours is written while vanilla is mid-apply, and the baseline is taken the
    /// moment the nesting unwinds.</summary>
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

        private static PostProcessVolume volume;
        private static bool haveVignetteBase;
        private static bool baseVignette;

        /// <summary>What vanilla left behind. Taken when UsePreset unwinds, and - as a fallback - immediately before the
        /// mod's first write, for the case where a preset was applied before Harmony was installed.</summary>
        internal static void Snapshot()
        {
            baseAniso = QualitySettings.anisotropicFiltering;
            baseLodBias = QualitySettings.lodBias;
            baseShadowRes = QualitySettings.shadowResolution;
            haveSnapshot = true;
        }

        /// <summary>Guard up. On the OUTERMOST entry the baseline goes back into the outgoing level first: Unity quality
        /// levels are runtime-mutable - every QualitySettings setter writes into the ACTIVE level's stored values, and
        /// SetQualityLevel only selects a level, it never reloads the asset defaults. Without this, reselecting the same
        /// preset (or switching away and back) makes the unwind Snapshot() read OUR overrides as "Vanilla".</summary>
        internal static void EnterUsePreset()
        {
            if (usePresetDepth == 0) RestoreBaseline();
            usePresetDepth++;
        }

        /// <summary>Vanilla aniso = captured mode + (-1, -1), the engine default (no forced limits). Coexistence with
        /// another mod's limits is not attempted.</summary>
        private static void RestoreAniso()
        {
            QualitySettings.anisotropicFiltering = baseAniso;
            Texture.SetGlobalAnisotropicFilteringLimits(-1, -1);
        }

        /// <summary>Write the captured baseline back. No-op without a snapshot - never restore uninitialised values.
        /// If UsePreset then throws, the Finalizer's ApplyAll puts the overrides back on top.</summary>
        private static void RestoreBaseline()
        {
            if (!haveSnapshot) return;
            try
            {
                RestoreAniso();
                QualitySettings.lodBias = baseLodBias;
                QualitySettings.shadowResolution = baseShadowRes;
            }
            catch (Exception ex) { Log("baseline restore failed", ex); }
        }

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
                else RestoreAniso();
                QualitySettings.lodBias = cfg.LodBias > 0f ? Mathf.Clamp(cfg.LodBias, 1f, 4f) : baseLodBias;
                QualitySettings.shadowResolution = cfg.ShadowResolution == ShadowResolutionMode.VeryHigh
                    ? ShadowResolution.VeryHigh
                    : baseShadowRes;
            }
            catch (Exception ex) { Log("scalar apply failed", ex); }
        }

        /// <summary>The volume vanilla itself drives: GetComponentInChildren&lt;PostProcessVolume&gt;() under the
        /// LightingManager's protected _currentLightsRoot (LightingManager.cs:168). Reacquired on every call, never cached
        /// across levels - LightingSettingsDef.ApplyTo destroys and re-instantiates the lights prefab per level
        /// (LightingSettingsDef.cs:21-22), so a cached reference points at a dead clone.</summary>
        private static PostProcessVolume CurrentVolume(LightingManager known)
        {
            LightingManager lm = known ?? GameUtl.GameComponent<LightingManager>();
            if (lm == null) return null;
            Transform root = Traverse.Create(lm).Field("_currentLightsRoot").GetValue<Transform>();
            return root == null ? null : root.GetComponentInChildren<PostProcessVolume>();
        }

        /// <summary>Vanilla = write the captured baseline back; Off = enabled.value = false. Writes `profile` (the runtime
        /// clone), which is what vanilla writes at LightingManager.cs:172-178 - `sharedProfile` is the shared asset and
        /// editing it would leak across levels and into the player's install.</summary>
        internal static void ApplyVignette(LightingManager known)
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return;
            try
            {
                PostProcessVolume found = CurrentVolume(known);
                if (found != volume) { volume = found; haveVignetteBase = false; }   // new clone = new baseline
                if (volume == null || volume.profile == null) return;
                Vignette vignette;
                if (!volume.profile.TryGetSettings(out vignette) || vignette == null) return;
                if (!haveVignetteBase) { baseVignette = vignette.enabled.value; haveVignetteBase = true; }
                vignette.enabled.value = cfg.Vignette == VignetteMode.Off ? false : baseVignette;
                // The exact invalidation vanilla performs at LightingManager.cs:167 - a public static field on PPv2's
                // PostProcessManager (PostProcessManager.cs:25), read back by UpdateSettings (:239).
                PostProcessManager.NeedUpdateSettings = true;
            }
            catch (Exception ex) { Log("vignette apply failed", ex); }
        }

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
                 + " | inUsePreset=" + InUsePreset
                 + " | vignette base=" + (haveVignetteBase ? baseVignette.ToString() : "?")
                 + " live=" + LiveVignette();
        }

        private static string LiveVignette()
        {
            Vignette vignette;
            if (volume == null || volume.profile == null || !volume.profile.TryGetSettings(out vignette) || vignette == null) return "?";
            return vignette.enabled.value.ToString();
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

    /// <summary>OptionsManager.UsePreset (public void UsePreset(int, bool), OptionsManager.cs:375) is where vanilla writes
    /// the quality level and, through ChangeGraphicsQuality (:391), nests the LightingManager callback. Prefix raises the
    /// guard so every nested seam writes nothing of ours; the Finalizer lowers it — ALWAYS, so a throw cannot latch the
    /// guard and freeze the knobs (same shape as ModSettingsFilter.cs:18-29) — but takes the baseline only when the
    /// original method actually completed.
    ///
    /// __exception is Harmony's "the original threw" channel: non-null = it threw. Returning it UNCHANGED rethrows the
    /// original exception with its own type and message; returning null would swallow it, which would hide a real
    /// OptionsManager failure from the game. So: always unwind, snapshot only on __exception == null, return as-is.</summary>
    [HarmonyPatch(typeof(OptionsManager), "UsePreset")]
    internal static class OptionsManager_UsePreset_Patch
    {
        [HarmonyPrefix]
        private static void Prefix() => QualityKnobs.EnterUsePreset();

        [HarmonyFinalizer]
        private static Exception Finalizer(Exception __exception)
        {
            QualityKnobs.LeaveUsePreset(__exception == null);
            return __exception;   // preserve/rethrow the original; never swallow
        }
    }

    /// <summary>LightingManager.ApplyPostProcessOptions (LightingManager.cs:163) runs on every lighting change and on
    /// every level, and LightingSettingsDef.ApplyTo re-instantiates the lights prefab each time
    /// (LightingSettingsDef.cs:21-22), so the PostProcessVolume here is a NEW object with a fresh profile. Vignette only:
    /// the scalars are QualitySettings and are already suppressed while inUsePreset, which is exactly when this fires
    /// as a nested callback.</summary>
    [HarmonyPatch(typeof(LightingManager), "ApplyPostProcessOptions")]
    internal static class LightingManager_ApplyPostProcessOptions_QualityPatch
    {
        static void Postfix(LightingManager __instance) => QualityKnobs.ApplyVignette(__instance);
    }
}
