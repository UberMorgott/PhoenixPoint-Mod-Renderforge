# Quality Knobs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship section **A. Quality knobs** of `docs\superpowers\specs\2026-09-05-quality-knobs-colour-vision-design.md` — four graphics knobs the vanilla game never exposes (vignette off, very-high shadow maps, forced 16x anisotropic filtering, LOD bias), each with a true `Vanilla` position that writes the game's own captured baseline back, surfaced as native rows in Options → Graphics and hidden from the Mods menu.

**Architecture:** One new state/patch file `src\QualityKnobs.cs` + one new UI file `src\QualityPanel.cs` (mirrors `LutPanel.cs` / `SceneStylePanel.cs`, the repo's established row-builder shape). Vanilla writes `QualitySettings` inside `OptionsManager.UsePreset`, which nests `ChangeGraphicsQuality` → `OnGraphicsSettingsChangedEvent` → `LightingManager.ApplyPostProcessOptions` (`OptionsManager.cs:384,391,406`; `LightingManager.cs:53-55`). So the mod brackets `UsePreset` with a prefix/`HarmonyFinalizer` guard: every nested seam is suppressed while the guard is up, and when it comes down the mod re-applies its own values on top — snapshotting what vanilla left behind **only if `UsePreset` completed without throwing**, since it can throw before it writes anything (`OptionsManager.cs:377,447`). Three seams keep the knobs alive: the `UsePreset` finalizer (scalars + vignette), a postfix on `LightingManager.ApplyPostProcessOptions` (vignette only — the volume is a per-level runtime clone), and `RenderforgeMod.OnLevelStart` (everything).

**Tech Stack:** C# `net472`, Harmony (`0Harmony.dll`), Unity 2019.4 (`UnityEngine.CoreModule`), Unity PostProcessing v2 (`Unity.Postprocessing.Runtime` — already referenced at `Renderforge.csproj:75-78`, so no csproj change is needed). Build `dotnet build`, deploy `deploy.ps1`, acceptance via PPCLI against `D:\PP-Instance3`.

---

## Grounded facts this plan rests on

Read before writing code; every line below was verified in source for this plan.

| Fact | Source |
|---|---|
| `public void UsePreset(int presetIndex, bool hasOverrides)` — public instance method, patchable by name; calls `QualitySettings.SetQualityLevel(num, applyExpensiveChanges: true)` then, when `!hasOverrides`, `ChangeGraphicsQuality(...)` | `OptionsManager.cs:375-393` |
| **`UsePreset` can throw before it writes anything:** its first statement is `DefinedPresetIndexToQualityIndex(presetIndex)` (`:377`), which indexes `OptionsManagerDef.DefinedPresets[definedQualityIndex]` unguarded (`:447`) → an out-of-range index throws before `SetQualityLevel` (`:384`). So the finalizer MUST NOT snapshot on the exception path — vanilla left nothing behind, and the live values are still the mod's own overrides | `OptionsManager.cs:377,384,447` |
| `ChangeGraphicsQuality` fires `OnGraphicsSettingsChangedEvent` **before** assigning `_currentPreset` | `OptionsManager.cs:406-407` |
| That event is bound to `ApplyPostProcessOptions(newPreset)` in `LightingManager.Initialize` | `LightingManager.cs:51-57` |
| The PPv2 invalidation vanilla does — **exact call**: `PostProcessManager.NeedUpdateSettings = true;` | `LightingManager.cs:167` |
| `NeedUpdateSettings` is a **public static field** (`public static bool NeedUpdateSettings = true;`), not a property | `decompiled\Postprocessing\UnityEngine.Rendering.PostProcessing\PostProcessManager.cs:25` |
| The volume vanilla drives: `_currentLightsRoot.GetComponentInChildren<PostProcessVolume>()`, and it writes `componentInChildren.profile` (the runtime clone), reading `sharedProfile` only as the asset default | `LightingManager.cs:168-179` |
| `_currentLightsRoot` is `protected Transform` on `LightingManager` → reachable with `Traverse.Create(lm).Field("_currentLightsRoot")` (same pattern already used at `D3D12Fix.cs:99`) | `LightingManager.cs:20` |
| `LightingSettingsDef.ApplyTo` calls `CleanLightsRoot(lightsRoot)` then `Object.Instantiate(LightsPrefab, lightsRoot)` → **the volume is a NEW object each level**; never cache it across levels | `LightingSettingsDef.cs:16-23` |
| `ApplyPostProcessOptions` is `private void ApplyPostProcessOptions(OptionsManager.GraphicsQualityPreset)` — patchable by name, `__instance` gives the `LightingManager` | `LightingManager.cs:163` |
| `LightingManager` is a sibling component of `CameraManager` (`GetComponent<CameraManager>()`), and `CameraManager` is reached with `GameUtl.GameComponent<CameraManager>()` → `GameUtl.GameComponent<LightingManager>()` is valid | `LightingManager.cs:40`, `RenderforgeMod.cs:276` |
| `.enabled.value` is how this repo already flips a PPv2 effect | `D3D12Fix.cs:158` |
| Row-cloning recipe: picker = `Instantiate(panel.TextureQualityPicker.gameObject, content)` + `picker.Init(count, index, cb)` + `SetSiblingIndex(after.GetSiblingIndex() + 1)`; slider = `Instantiate(panel.ShadowDistanceSlider.transform.parent.gameObject, content)`, label `"UITextGeneric_Medium (1)"`, readout `"UITextGeneric_Medium"` | `LutPanel.cs:47-93`, `GraphicsPanel.cs:122-150` |
| Hidden-from-Mods-menu mechanism: add the field name to `DlssConfig.HiddenFromModSettings` | `DlssConfig.cs:31-36`, `ModSettingsFilter.cs:10-30` |
| Immediate apply + persist: `RenderforgeMod.SaveConfig()` (`ModManager.SaveModConfig`) | `RenderforgeMod.cs:228-232` |
| `finally`-safe Harmony pattern already in the repo: `[HarmonyPrefix]` increments, `[HarmonyFinalizer]` decrements | `ModSettingsFilter.cs:18-29` |
| No `TreatWarningsAsErrors` in the csproj → warning-free is enforced by reading `0 Warning(s)`, not by the compiler | `Renderforge.csproj:3-20` |
| Perf facility that already exists: the benchmark `Overlay` — CPU frame time in ms on the FPS line (0.5 s window over `Time.unscaledDeltaTime`), and a GPU line that is **D3D12 + live-upscaler only** (`Dlss_Timings`) | `Overlay.cs:155-179` |

**One item verified only at compile/run time:** nothing in this repo currently references `PostProcessManager.NeedUpdateSettings`. The decompiled PPv2 (above) says it is a public static field, and `Unity.Postprocessing.Runtime` is already referenced, so a successful build is the compile-time proof and Task 6 step 4 is the runtime proof.

---

## File structure

| File | Create/Modify | Responsibility |
|---|---|---|
| `src\DlssConfig.cs` | modify | 3 new enums (`VignetteMode`, `ShadowResolutionMode`, `AnisotropicMode`), 4 new `[ConfigField]` fields, 4 RU label rows, 4 names added to `HiddenFromModSettings` |
| `src\QualityKnobs.cs` | **create** | Baseline snapshot, `Vanilla`-writes-baseline apply/restore, vignette baseline per volume, `inUsePreset` guard, the 2 new `[HarmonyPatch]` classes, PPCLI `Status()` / `SetQuality()` / `UsePreset()` levers |
| `src\QualityPanel.cs` | **create** | The 3 picker rows + 1 slider row in Options → Graphics; immediate apply + `SaveConfig` |
| `src\GraphicsPanel.cs` | modify | Wire `QualityPanel.Build` after `SceneStylePanel.Build`; `QualityPanel.Hide` in the hidden branch |
| `src\RenderforgeMod.cs` | modify | `OnLevelStart` + `OnConfigChanged` call `QualityKnobs.ApplyAll()` |
| `README.md` | modify | User-facing rows for the four knobs + the measured cost sentence |
| `docs\DESIGN.md` | modify | Design rows: seams, baseline rule, why no "Extreme" shadow tier |

No test project exists in this repo (`Renderforge.csproj:14,34` — `EnableDefaultCompileItems=false`, `Compile Include="src\**\*.cs"` only) and every knob reads or writes a Unity static (`QualitySettings`, `Texture`, `PostProcessManager`). **There is no pure-C# logic worth a test harness here** — the only branch that is not a direct Unity write is the one-line LOD-bias clamp. Correctness is established by the PPCLI acceptance in Task 6, which is exact and reproducible. Do not add a test project.

---

## Task 1 — Config fields, enums and labels

**Files:** `src\DlssConfig.cs` (enums after line 22; `HiddenFromModSettings` at :31-36; fields after :78; `Ru` table before :102)

- [x] 1. Add the three enums immediately after the `LutPreset` enum (`src\DlssConfig.cs:22`):

```csharp
    /// <summary>Tactical vignette. Vanilla = whatever the level's volume shipped with (captured per volume);
    /// Off = the mod writes enabled.value = false on the runtime profile.</summary>
    public enum VignetteMode { Vanilla, Off }

    /// <summary>QualitySettings.shadowResolution. Vanilla = the captured preset value (High at Ultra);
    /// VeryHigh is Unity's top tier - the engine still caps the map at 4096 dir / 2048 spot / 1024 point.</summary>
    public enum ShadowResolutionMode { Vanilla, VeryHigh }

    /// <summary>Vanilla never writes anisotropic filtering at all. Force16 = ForceEnable +
    /// Texture.SetGlobalAnisotropicFilteringLimits(16, 16); the restore is the snapshot + limits (-1, -1).</summary>
    public enum AnisotropicMode { Vanilla, Force16 }
```

- [x] 2. Add the four config fields directly after the `FrameGen` field (`src\DlssConfig.cs:78`):

```csharp
        [ConfigField("Vignette", "Vanilla keeps the mission's own vignette; Off removes the darkened frame edges. Also in Options → Graphics.")]
        public VignetteMode Vignette = VignetteMode.Vanilla;
        [ConfigField("Shadow resolution", "Vanilla keeps the graphics preset's value; Very High raises the shadow map size. Also in Options → Graphics.")]
        public ShadowResolutionMode ShadowResolution = ShadowResolutionMode.Vanilla;
        [ConfigField("Anisotropic filtering", "Vanilla leaves per-texture filtering alone; 16x forces 16 samples on every texture. Also in Options → Graphics.")]
        public AnisotropicMode Anisotropic = AnisotropicMode.Vanilla;
        [ConfigField("LOD detail", "0 = vanilla. 1.0 … 4.0 keeps higher-detail models at distance; costs GPU time and VRAM. Also in Options → Graphics.")]
        public float LodBias = 0f;             // 0 = vanilla (write the captured baseline back); otherwise clamped to 1..4
```

- [x] 3. Add the four names to `HiddenFromModSettings` (`src\DlssConfig.cs:31-36`) — replace the closing line of the initializer so it reads:

```csharp
            nameof(Mode), nameof(Sharpness), nameof(Renderer), nameof(Upscaler), nameof(FrameGen),
            nameof(LimitFrameRate), nameof(FrameRateLimit), nameof(Lut), nameof(LutStrength),
            nameof(SceneStyle), nameof(SceneStyleStrength), nameof(PixelSize), nameof(CrispFonts),
            nameof(Vignette), nameof(ShadowResolution), nameof(Anisotropic), nameof(LodBias)
```

- [x] 4. Add the four RU rows to the `Ru` dictionary, immediately after the `FrameGen` row (`src\DlssConfig.cs:101`):

```csharp
            { nameof(Vignette), new[] { "Виньетка", "«Как в игре» сохраняет виньетку миссии; «Выкл» убирает затемнение по краям кадра. Также в Настройки → Графика." } },
            { nameof(ShadowResolution), new[] { "Разрешение теней", "«Как в игре» — значение выбранного пресета; «Очень высокое» увеличивает размер карты теней. Также в Настройки → Графика." } },
            { nameof(Anisotropic), new[] { "Анизотропная фильтрация", "«Как в игре» ничего не меняет; «16x» включает 16 выборок для всех текстур. Также в Настройки → Графика." } },
            { nameof(LodBias), new[] { "Детализация LOD", "0 = как в игре. 1.0 … 4.0 — модели дольше остаются детальными вдали; расход GPU и видеопамяти растёт. Также в Настройки → Графика." } },
```

- [x] 5. Build from the repo root `E:\DEV\PhoenixPoint\Renderforge`:

```powershell
dotnet build Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"
```

Expected tail: `Build succeeded.` … `0 Warning(s)` … `0 Error(s)`. Any warning must be fixed before moving on.

- [x] 6. Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(config): add vignette, shadow resolution, anisotropic and LOD bias fields"
```

---

## Task 2 — QualityKnobs core (snapshot / apply / restore, no patches yet)

**Files:** `src\QualityKnobs.cs` (create, ~130 lines)

- [x] 1. Create `src\QualityKnobs.cs` with the state, the guard, and the scalar path (patches come in Task 3, vignette in Task 4):

```csharp
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

        private static void Log(string what, Exception ex)
        {
            if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge quality knobs " + what + ": " + ex);
            loggedError = true;
        }
    }
}
```

- [x] 2. Add the PPCLI readback and setter levers at the end of the class, before `Log` — this is the acceptance surface Task 6 uses, and it matches the existing `RenderforgeMod.SetMode` / `D3D12Fix.SetAo` lever shape:

```csharp
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
            QualityPanel.Sync();
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
```

- [x] 3. `QualityPanel.Sync()` does not exist yet (Task 5). Temporarily comment that one call out with `// QualityPanel.Sync();  // Task 5` so this task builds standalone, and remember to restore it in Task 5 step 4.

- [x] 4. Build from `E:\DEV\PhoenixPoint\Renderforge`:

```powershell
dotnet build Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"
```

Expected: `Build succeeded.` `0 Warning(s)` `0 Error(s)`.

- [x] 5. Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(quality): add QualityKnobs baseline snapshot and scalar apply"
```

---

## Task 3 — Harmony seams

**Files:** `src\QualityKnobs.cs` (append the patch classes after the `QualityKnobs` class, inside the namespace), `src\RenderforgeMod.cs:162,167-174`

- [x] 1. Append the two patch classes to `src\QualityKnobs.cs`, after the closing brace of `QualityKnobs` and inside `namespace Renderforge`:

```csharp
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
```

- [x] 2. Wire `OnLevelStart` — replace `src\RenderforgeMod.cs:162` with:

```csharp
        public override void OnLevelStart(Level level) { AttachAndApply(); MipBias.Reapply(); D3D12Fix.Apply(); QualityKnobs.ApplyAll(); }   // Reapply covers a level that starts with the generation still live
```

- [x] 3. Wire `OnConfigChanged` — a ModConfig.json edit still routes here even though the rows are hidden from the Mods menu. Insert one line into `src\RenderforgeMod.cs:167-174`, after `ApplyFrameRate();`:

```csharp
            QualityKnobs.ApplyAll();
```

- [x] 4. Build from `E:\DEV\PhoenixPoint\Renderforge`:

```powershell
dotnet build Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"
```

Expected: `Build succeeded.` `0 Warning(s)` `0 Error(s)`. Two Harmony patches now target `LightingManager.ApplyPostProcessOptions` (the existing `Patches.cs:19` one and this one) — that is supported and intentional; keep them separate so the DLSS SMAA fix and the vignette knob stay independently removable.

- [x] 5. Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(quality): patch UsePreset and ApplyPostProcessOptions seams"
```

---

## Task 4 — Vignette

**Files:** `src\QualityKnobs.cs` (replace the Task 2 stub `ApplyVignette`, add the volume fields next to the other state)

- [x] 1. Add the vignette state next to the scalar state in `QualityKnobs` (right after the `loggedError` field):

```csharp
        private static PostProcessVolume volume;
        private static bool haveVignetteBase;
        private static bool baseVignette;
```

- [x] 2. Replace the stub `internal static void ApplyVignette(LightingManager known) { }` with the real implementation:

```csharp
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
```

- [x] 3. Extend `Status()` so the acceptance run can read the vignette back — append to the returned string, before the closing `;`:

```csharp
                 + " | vignette base=" + (haveVignetteBase ? baseVignette.ToString() : "?")
                 + " live=" + LiveVignette()
```

and add the helper right below `Status()`:

```csharp
        private static string LiveVignette()
        {
            Vignette vignette;
            if (volume == null || volume.profile == null || !volume.profile.TryGetSettings(out vignette) || vignette == null) return "?";
            return vignette.enabled.value.ToString();
        }
```

- [x] 4. Build from `E:\DEV\PhoenixPoint\Renderforge`:

```powershell
dotnet build Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"
```

Expected: `Build succeeded.` `0 Warning(s)` `0 Error(s)`. A build failure on `PostProcessManager.NeedUpdateSettings` would mean the shipped `Unity.Postprocessing.Runtime.dll` differs from the decompile — stop and report rather than working around it.

- [x] 5. Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(quality): add vignette override with per-volume baseline"
```

---

## Task 5 — Options → Graphics rows

**Files:** `src\QualityPanel.cs` (create), `src\GraphicsPanel.cs:42-43,61-63`

- [x] 1. Create `src\QualityPanel.cs` — three picker rows cloned from `TextureQualityPicker` and one slider row cloned from `ShadowDistanceSlider`, exactly the recipe in `LutPanel.cs:47-93`:

```csharp
using System;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Vignette / shadow resolution / anisotropic filtering pickers and the LOD detail slider in the game's
    /// native Graphics panel. Immediate apply + SaveConfig, like every other Renderforge row.</summary>
    internal static class QualityPanel
    {
        private const string VignetteName = "RenderforgeVignette";
        private const string ShadowName = "RenderforgeShadowRes";
        private const string AnisoName = "RenderforgeAniso";
        private const string LodName = "RenderforgeLodBias";

        private static ArrowPickerController vignette, shadow, aniso;
        private static Slider lod;
        private static Transform lodValue;
        private static bool loggedError;

        private static string Vanilla { get { return DlssConfig.Loc("Vanilla", "Как в игре"); } }
        private static string[] VignetteLabels { get { return new[] { Vanilla, DlssConfig.Loc("Off", "Выкл") }; } }
        private static string[] ShadowLabels { get { return new[] { Vanilla, DlssConfig.Loc("Very High", "Очень высокое") }; } }
        private static string[] AnisoLabels { get { return new[] { Vanilla, "16x" }; } }

        /// <summary>Adds the four rows after `after` and returns the last one, so the caller can keep chaining.</summary>
        internal static Transform Build(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            if (panel.TextureQualityPicker == null || after == null) return after;
            after = Picker(panel, after, VignetteName, DlssConfig.Loc("Vignette", "Виньетка"),
                VignetteLabels, (int)cfg.Vignette, OnVignette, out vignette);
            after = Picker(panel, after, ShadowName, DlssConfig.Loc("Shadow resolution", "Разрешение теней"),
                ShadowLabels, (int)cfg.ShadowResolution, OnShadow, out shadow);
            after = Picker(panel, after, AnisoName, DlssConfig.Loc("Anisotropic filtering", "Анизотропная фильтрация"),
                AnisoLabels, (int)cfg.Anisotropic, OnAniso, out aniso);
            after = BuildLodSlider(panel, after, cfg);
            Sync();
            return after;
        }

        /// <summary>One picker row cloned from TextureQualityPicker; existing clones are reused and re-indexed, because
        /// UIModuleGraphicsOptionsPanel.Init() runs on every options open (GraphicsPanel.cs:15).</summary>
        private static Transform Picker(UIModuleGraphicsOptionsPanel panel, Transform after, string name, string title,
                                        string[] labels, int index, Action<int> onChanged, out ArrowPickerController row)
        {
            var content = after.parent;
            var found = content.Find(name);
            if (found != null) row = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(panel.TextureQualityPicker.gameObject, content);
                go.name = name;
                row = go.GetComponent<ArrowPickerController>();
                GraphicsPanel.SetRaw(row.Title, null, title.ToUpperInvariant());
            }
            row.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
            row.gameObject.SetActive(true);
            row.Init(labels.Length, Mathf.Clamp(index, 0, labels.Length - 1), onChanged);
            return row.transform;
        }

        /// <summary>LOD detail slider. The spec's range is "0 = Vanilla, otherwise 1.0 … 4.0" (spec §A table row LodBias),
        /// so the valid values are NOT contiguous on a bias axis - but the slider positions must be, or keyboard/controller
        /// decrement gets trapped. Positions are therefore CONTIGUOUS and mapped, not scaled: 0 = Vanilla, 1..31 = 1.0,
        /// 1.1, … 4.0 (value = 1.0 + (pos - 1) * 0.1). Decrementing from position 1 (bias 1.0) lands on position 0 =
        /// Vanilla, and every arrow press moves exactly one step in both directions.</summary>
        private static Transform BuildLodSlider(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            var srcSlider = panel.ShadowDistanceSlider;
            if (srcSlider == null) return after;
            var content = after.parent;
            var row = content.Find(LodName);
            if (row == null)
            {
                var go = UnityEngine.Object.Instantiate(srcSlider.transform.parent.gameObject, content);
                go.name = LodName;
                row = go.transform;
            }
            row.SetSiblingIndex(after.GetSiblingIndex() + 1);
            row.gameObject.SetActive(true);
            lod = row.GetComponentInChildren<Slider>(true);
            lodValue = row.Find("UITextGeneric_Medium");
            var label = row.Find("UITextGeneric_Medium (1)");
            if (label != null) GraphicsPanel.SetRaw(label.GetComponent<Localize>(), label.GetComponent<Text>(),
                DlssConfig.Loc("LOD detail", "Детализация LOD").ToUpperInvariant());
            if (lodValue != null) lodValue.gameObject.SetActive(true);
            lod.gameObject.SetActive(true);
            lod.wholeNumbers = true;
            lod.minValue = 0;
            lod.maxValue = 31;                       // 0 = Vanilla, 1..31 = 1.0 .. 4.0 in 0.1 steps
            lod.SetValueWithoutNotify(PosFromBias(cfg.LodBias));
            ShowLod(cfg.LodBias);
            lod.onValueChanged.RemoveAllListeners();
            lod.onValueChanged.AddListener(OnLod);
            return row;
        }

        /// <summary>Position -> bias. 0 = Vanilla (0f), 1..31 = 1.0 + (pos - 1) * 0.1, rounded to one decimal so
        /// float accumulation cannot produce 3.9999997.</summary>
        private static float BiasFromPos(int pos)
        {
            if (pos <= 0) return 0f;
            return Mathf.Round((1f + (Mathf.Min(pos, 31) - 1) * 0.1f) * 10f) / 10f;
        }

        /// <summary>Bias -> position, the exact inverse of BiasFromPos.</summary>
        private static float PosFromBias(float bias)
        {
            if (bias <= 0f) return 0f;
            return Mathf.Clamp(Mathf.RoundToInt((Mathf.Clamp(bias, 1f, 4f) - 1f) * 10f) + 1, 1, 31);
        }

        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null) return;
            Show(vignette, VignetteLabels, (int)cfg.Vignette);
            Show(shadow, ShadowLabels, (int)cfg.ShadowResolution);
            Show(aniso, AnisoLabels, (int)cfg.Anisotropic);
            if (lod != null) lod.SetValueWithoutNotify(PosFromBias(cfg.LodBias));
            ShowLod(cfg.LodBias);
        }

        private static void Show(ArrowPickerController row, string[] labels, int index)
        {
            if (row == null) return;
            GraphicsPanel.SetRaw(row.CurrentItem, row.CurrentItemText, labels[Mathf.Clamp(index, 0, labels.Length - 1)]);
            GraphicsPanel.Grey(row.CurrentItem.gameObject, false);
        }

        private static void ShowLod(float bias)
        {
            if (lodValue == null) return;
            GraphicsPanel.SetRaw(lodValue.GetComponent<Localize>(), lodValue.GetComponent<Text>(),
                bias > 0f ? bias.ToString("F1") : Vanilla);
        }

        internal static void Hide(Transform content)
        {
            if (content == null) return;
            foreach (string name in new[] { VignetteName, ShadowName, AnisoName, LodName })
            {
                var row = content.Find(name);
                if (row != null) row.gameObject.SetActive(false);
            }
        }

        internal static void Clear() { vignette = shadow = aniso = null; lod = null; lodValue = null; }

        private static void OnVignette(int index) => Change(cfg => cfg.Vignette = (VignetteMode)Mathf.Clamp(index, 0, 1));
        private static void OnShadow(int index) => Change(cfg => cfg.ShadowResolution = (ShadowResolutionMode)Mathf.Clamp(index, 0, 1));
        private static void OnAniso(int index) => Change(cfg => cfg.Anisotropic = (AnisotropicMode)Mathf.Clamp(index, 0, 1));

        /// <summary>No snapping: every position is valid, so a decrement from position 1 (bias 1.0) reaches position 0 =
        /// Vanilla instead of being bounced back up.</summary>
        private static void OnLod(float raw)
        {
            float bias = BiasFromPos(Mathf.RoundToInt(raw));
            Change(cfg => cfg.LodBias = bias);
        }

        private static void Change(Action<DlssConfig> edit)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                edit(cfg);
                QualityKnobs.ApplyAll();
                RenderforgeMod.SaveConfig();
                Sync();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge quality row change failed: " + ex);
                loggedError = true;
            }
        }
    }
}
```

- [x] 2. Wire the rows into the panel — replace `src\GraphicsPanel.cs:61-63` with:

```csharp
                after = LutPanel.Build(__instance, sharp != null ? sharp.transform.parent : picker.transform, mod.Cfg);
                after = SceneStylePanel.Build(__instance, after, mod.Cfg);
                QualityPanel.Build(__instance, after, mod.Cfg);
                SyncQuality();
```

**Row order, and coexistence with the sibling plan.** The intended order in Options → Graphics is
**LUT → Colour vision → Scene style → Quality rows** (vignette, shadow resolution, anisotropic, LOD detail); the sibling
plan `docs\superpowers\plans\2026-09-05-colour-vision.md` Task 5 inserts its `ColorVisionPanel.Build(...)` between
`LutPanel.Build` and `SceneStylePanel.Build` in the very same `GraphicsPanel.cs:61-63` block and adds its own
`ColorVisionPanel.Hide` / `ColorVisionPanel.Clear` lines and its own `DlssConfig` field, so **whichever plan lands
second must keep the other's rows and config fields instead of replacing the block wholesale** — chain onto the existing
`after` (`after = ColorVisionPanel.Build(__instance, after, mod.Cfg);` stays, `QualityPanel.Build` goes last) and append,
never overwrite, in `HiddenFromModSettings`, the `Ru` table, `GraphicsPanel.Hide` and `Pickers.Clear`.

- [x] 3. Hide them with the rest — add one line to the `ShowInGraphicsOptions == false` branch, after `SceneStylePanel.Hide(src.transform.parent);` (`src\GraphicsPanel.cs:43`):

```csharp
                    QualityPanel.Hide(src.transform.parent);
```

- [x] 4. Restore the call commented out in Task 2 step 3: in `QualityKnobs.SetQuality`, change `// QualityPanel.Sync();  // Task 5` back to `QualityPanel.Sync();`.

- [x] 5. Build from `E:\DEV\PhoenixPoint\Renderforge`:

```powershell
dotnet build Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance3"
```

Expected: `Build succeeded.` `0 Warning(s)` `0 Error(s)`.

- [x] 6. Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(ui): add quality knob rows to Options -> Graphics"
```

---

## Task 6 — Deploy to Instance3 and run the PPCLI acceptance

**Files:** none changed unless a check fails. Target install `D:\PP-Instance3` — **not** Instance2, **never** the Steam install.

- [x] 1. Deploy the mod (reuse the already-built native DLLs; nothing native changed in this plan):

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
.\deploy.ps1 -PPRoot 'D:\PP-Instance3' -SkipNative
```

Expected tail: `Deployed Renderforge to D:\PP-Instance3\Mods\Renderforge` and a file list containing `Renderforge.dll`. If `build\out\*.dll` is missing, drop `-SkipNative` and let `build-native.ps1` run.

- [x] 2. Deploy the PPCLI bridge to the same install and arm it (the bridge is opt-in; a bare `deploy` would target the install named in `PPCLI\ppcli-install.txt`):

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 deploy -PPRoot 'D:\PP-Instance3' -Force
New-Item -ItemType File 'D:\PP-Instance3\Mods\PPBridge\ppcli-enabled'
```

Both `com.morgott.Renderforge` and `com.morgott.PPBridge` must be in that profile's `MOD_ACTIVATED`; if the client refuses to launch, tick them once in the in-game mod manager and quit.

- [x] 3. Launch `D:\PP-Instance3`, then **wait for the bridge to answer before sending anything** (querying a still-initialising game hangs for minutes and looks like an engine bug):

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
```

Expected: a JSON object with `ok:true`. Do not proceed until it answers.

- [x] 4. Baseline readback plus the one member this plan could not verify statically:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"Status"}'
.\ppcli.ps1 connect call '{"op":"get","type":"UnityEngine.Rendering.PostProcessing.PostProcessManager","assembly":"Unity.Postprocessing.Runtime","member":"NeedUpdateSettings"}'
```

Expected: `Status` returns `cfg vignette=Vanilla shadowRes=Vanilla aniso=Vanilla lodBias=0 | live aniso=… lodBias=… shadowRes=… | base have=True … | inUsePreset=False`, with `live` equal to `base` on every scalar. The second call returns a boolean — its success is the proof that the field exists at runtime.

- [x] 5. **All 6 presets.** For each index 0..5, apply the preset and read back. Run in one shell:

```powershell
0..5 | ForEach-Object {
  .\ppcli.ps1 connect call ('{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"UsePreset","args":[' + $_ + ']}')
}
```

Expected per preset: `inUsePreset=False` in every reply (the guard came down), `base have=True`, and `live` scalars equal to `base` scalars (all knobs still Vanilla). Record each preset's `base aniso / lodBias / shadowRes` triple — the Ultra row should read `aniso=ForceEnable lodBias=2 shadowRes=High`, matching the spec's verified facts.

- [x] 6. **Knobs on, then all 6 presets again** — this is the real seam test: a preset re-applies vanilla's values, and the finalizer must put ours back.

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["Off","VeryHigh","Force16",4.0]}'
0..5 | ForEach-Object {
  .\ppcli.ps1 connect call ('{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"UsePreset","args":[' + $_ + ']}')
}
```

Expected after every preset: `live aniso=ForceEnable lodBias=4 shadowRes=VeryHigh` and `vignette live=False`, while `base` shows that preset's own values (which differ per preset) — proving the snapshot refreshed and the knobs won.

- [x] 7. **Same-preset reapply** (the `ChangeGraphicsQuality` early-out at `OptionsManager.cs:397-400` means the nested lighting callback may not fire at all; the knobs must survive that too):

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"UsePreset","args":[3]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"UsePreset","args":[3]}'
```

Expected: identical `live` values in both replies, `inUsePreset=False` in both.

- [x] 8. **Failure path — an invalid preset index must not poison the baseline.** `UsePreset` throws at
`OptionsManager.cs:377/447` before it writes anything, so the finalizer must unwind the guard, skip the snapshot and
rethrow. Turn the knobs on first, so a wrong snapshot would be obvious (it would record `ForceEnable / 4 / VeryHigh`
as "Vanilla"):

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["Off","VeryHigh","Force16",4.0]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"Status"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"UsePreset","args":[99]}'
$LASTEXITCODE
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"Status"}'
```

Expected: the `args:[99]` call FAILS (`ok:false`, `code`/`error` naming an index/range exception, exit code `1`) — that
failure is the point, not a defect. The `Status` before and after must be **identical in the `base …` triple** (still the
current preset's own values, never `aniso=ForceEnable lodBias=4 shadowRes=VeryHigh`), `base have=True`, and
`inUsePreset=False` afterwards — proving the guard unwound on the throw path and no snapshot was taken. Then re-run a
valid `UsePreset` (`args:[3]`) and confirm the knobs still win, i.e. the guard is not latched.

- [x] 9. **Vanilla round-trip** — every knob back to Vanilla must restore the snapshot exactly:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["Vanilla","Vanilla","Vanilla",0]}'
.\ppcli.ps1 connect call '{"op":"get","type":"UnityEngine.QualitySettings","assembly":"UnityEngine.CoreModule","member":"lodBias"}'
.\ppcli.ps1 connect call '{"op":"get","type":"UnityEngine.QualitySettings","assembly":"UnityEngine.CoreModule","member":"shadowResolution"}'
.\ppcli.ps1 connect call '{"op":"get","type":"UnityEngine.QualitySettings","assembly":"UnityEngine.CoreModule","member":"anisotropicFiltering"}'
```

Expected: the three raw `QualitySettings` reads equal the `base …` triple from step 5 for the currently applied preset, and `vignette live` equals `vignette base`.

- [x] 10. **Survives `OnLevelStart`** — tactical → geoscape → tactical. Set the knobs on, then run a mission cold-start plan and read back inside the mission:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["Off","VeryHigh","Force16",2.0]}'
.\ppcli.ps1 plan .\plans\start-mission.json
.\ppcli.ps1 connect wait '{"ready":true,"timeoutMs":120000}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"Status"}'
```

Expected inside the mission: `live aniso=ForceEnable lodBias=2 shadowRes=VeryHigh`, `vignette live=False`, and `vignette base=` a value (not `?`) — a fresh baseline for the level's new volume clone. Repeat the transition once more and confirm the values are unchanged.

- [x] 11. **Vignette screenshot crop.** With the camera untouched between the two captures:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["Vanilla","Vanilla","Vanilla",0]}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\claude\\vig-on.png"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["Off","Vanilla","Vanilla",0]}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\claude\\vig-off.png"}'
```

Then compare a corner block against a centre block:

```powershell
Add-Type -AssemblyName System.Drawing
function Mean($path, $x, $y, $s) {
  $b = [System.Drawing.Bitmap]::FromFile($path); $t = 0.0
  for ($i = 0; $i -lt $s; $i++) { for ($j = 0; $j -lt $s; $j++) { $t += $b.GetPixel($x + $i, $y + $j).GetBrightness() } }
  $b.Dispose(); [math]::Round($t / ($s * $s), 4)
}
"corner on=$(Mean 'C:\Temp\claude\vig-on.png' 8 8 32) off=$(Mean 'C:\Temp\claude\vig-off.png' 8 8 32)"
"centre on=$(Mean 'C:\Temp\claude\vig-on.png' 1264 704 32) off=$(Mean 'C:\Temp\claude\vig-off.png' 1264 704 32)"
```

Expected: the **corner** means differ measurably (off is brighter — the darkened frame edge is gone); the **centre** means are equal to within rounding. Adjust the centre coordinates to the actual capture size if it is not 2560x1440.

- [x] 12. **LOD slider navigation — decrement from 1.0 must reach Vanilla** (the contiguous-position mapping of Task 5
step 1; a scaled 0..40 slider would trap the selection at 1.0). Set the bias, open the panel, and step down at the
keyboard/controller — PPCLI has no key-injection verb, so the arrow presses are made at the machine and PPCLI is used
for the readback:

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["Vanilla","Vanilla","Vanilla",1.0]}'
# In game: Esc -> Options -> Graphics, focus the "LOD DETAIL" row, press Left once.
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\claude\\lod-vanilla.png"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"Status"}'
# Press Right once (back to 1.0), then Right twice more.
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"Status"}'
```

Expected: one Left press from `1.0` shows the readout `Vanilla` / `Как в игре` in `lod-vanilla.png` and `Status` reports
`cfg … lodBias=0` with `live lodBias` equal to `base lodBias`; the following Right presses read `1.0`, `1.1`, `1.2` —
one step per press, no jump to `4.0` and no bounce back to `1.0`.

- [x] 13. Log any PPCLI defect hit during this run as an entry in `E:\DEV\PhoenixPoint\PPCLI\ISSUES.md` (attempted → happened → expected → evidence → severity). Do not fix PPCLI.

- [x] 14. Record the readback table (preset index → base triple, knobs-on live triple, invalid-index base-unchanged check, vignette corner/centre means) in the commit body. Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test(quality): verify knobs across all presets and level transitions on Instance3"
```

If nothing changed in the working tree, skip the commit and carry the numbers into Task 7 instead.

---

## Task 7 — Performance numbers

BLOCKED 2026-09-05: PresentMon saw 0 frames from the game; GPU contended — rerun when idle.

**Files:** none. Measurement only, on the user's rig (RTX 5070 Ti, 1440p 240 Hz borderless, vsync off) against `D:\PP-Instance3`.

**The overlay cannot answer this question.** `Overlay`'s FPS line is a CPU-side frame-time average over a 0.5 s window of
`Time.unscaledDeltaTime` (`Overlay.cs:155-159`) — presentation cadence, not GPU time — and its `GPU:` line is
`Dlss_Timings`, which covers only the native upscale commands (copy-in / evaluate / copy-out / wait,
`Overlay.cs:170,174-179`) and appears only under D3D12 with a live upscaler. Shadow-map rendering and LOD geometry work
are **not** in either number, and those are exactly the two knobs being measured. So the whole-frame GPU time is measured
**externally with PresentMon**, and the overlay figures are kept only as a secondary column for cross-checking.

There is **no VRAM readout anywhere in the mod** — use `nvidia-smi`, which ships with the driver. Do not add a VRAM or a
GPU-timing facility to the mod for this measurement.

- [ ] 1. Turn the overlay on and park the camera on a fixed tactical view (do not move it between the runs):

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetOverlay","args":["TopCenter"]}'
```

Expected: `overlay=True at TopCenter`.

- [ ] 2. **Get PresentMon.** It is not installed on this machine (checked: `Get-Command presentmon*` returns nothing).
Download the official Intel GameTechDev CLI into the scratchpad — the standalone `.exe`, not the `.msi` installer:

```powershell
$pm = 'C:\Temp\claude\PresentMon-2.5.1-x64.exe'
New-Item -ItemType Directory -Force 'C:\Temp\claude' | Out-Null
Invoke-WebRequest -Uri 'https://github.com/GameTechDev/PresentMon/releases/download/v2.5.1/PresentMon-2.5.1-x64.exe' -OutFile $pm
& $pm --help          # confirm the flags below exist in THIS build before trusting any number
```

Notes that decide whether the numbers are real:
- PresentMon captures ETW events and **must run elevated** — start that shell with "Run as administrator", or every
  capture comes back empty.
- **Flags, taken from the v2.5.1 README** (`README-ConsoleApplication.md` at tag `v2.5.1`):
  - `--no_console_stats` — *"Do not display active swap chains and frame statistics in the console."* This is the
    console-suppression flag this build has.
  - `--v2_metrics` — *"Output a CSV using PresentMon 2.x metrics."*
  - **`--no_top` does not exist in 2.5.1 and the tool rejects it** — the flag table lists no such option. Passing it
    aborts the capture, and a `Measure-Cfg` that ignores the exit code then produces an empty or absent CSV that the
    reducer would happily average to `0`. Do not carry it over from an older recipe.
- **Column names — `--v2_metrics` changes them, and that is the whole point of passing it.** The v2 header row is
  emitted by `WriteCsvHeader<FrameMetrics>` in `PresentMon/CsvOutput.cpp` at tag `v2.5.1`
  ([source, ~L426](https://github.com/GameTechDev/PresentMon/blob/v2.5.1/PresentMon/CsvOutput.cpp#L426)); the columns
  it writes, in order, are `CPUStartTime` (or `CPUStartQPC` / `CPUStartQPCTime` / `CPUStartDateTime`, per `--qpc_time`
  etc.), then `FrameTime`, `CPUBusy`, `CPUWait`, `GPULatency`, `GPUTime`, `GPUBusy`, `GPUWait`, `VideoBusy`,
  `DisplayLatency`, `DisplayedTime`, `AnimationError`, `AnimationTime`, `MsFlipDelay`, `AllInputToPhotonLatency`,
  `ClickToPhotonLatency`, `InstrumentedLatency` (the GPU / video / display / input groups only when that tracking is on).
  The `Ms`-prefixed spellings (`MsBetweenPresents`, `MsGPUBusy`, …) are the **v1** column set — the one you get
  *without* `--v2_metrics` — and they do not appear in a v2 CSV at all:
  - `FrameTime` — time from this frame's CPU start to the next one, in ms → frame time (the v1 `MsBetweenPresents`).
  - `GPUBusy` — how long the GPU was actively working on this frame, in ms → **the whole-frame GPU time this task
    exists to produce** (the v1 `MsGPUBusy`).
  - `GPUTime` — total GPU time attributed to the frame (recorded as a cross-check, not the headline figure).
  - `CPUBusy` — CPU work on this frame before it was presented.
  - Both spellings are wrong to *guess* at: the reducer in step 6 **fails with the actual header list** if a build
    disagrees; it never falls back to `0`.

- [ ] 3. **Start the VRAM sampler with timestamps** in a second shell and leave it running through all five
measurements, so each run can be aligned to its own window by wall clock:

```powershell
nvidia-smi --query-gpu=memory.used,timestamp --format=csv -l 1 | Tee-Object -FilePath C:\Temp\claude\vram.csv
```

- [ ] 4. **One helper, used identically for all five configurations.** Paste it into the elevated shell once:

```powershell
$pm = 'C:\Temp\claude\PresentMon-2.5.1-x64.exe'
function Measure-Cfg {
  param([string]$Tag, [string]$Vig, [string]$Shadow, [string]$Aniso, [double]$Lod)
  Push-Location E:\DEV\PhoenixPoint\PPCLI
  .\ppcli.ps1 connect call ('{"op":"invoke","type":"Renderforge.QualityKnobs","assembly":"Renderforge","member":"SetQuality","args":["' + $Vig + '","' + $Shadow + '","' + $Aniso + '",' + $Lod + ']}')
  Start-Sleep 30                                   # settle: streaming, shadow maps and LODs must stop churning
  $t0 = Get-Date
  $csv = "C:\Temp\claude\pm-$Tag.csv"
  Remove-Item $csv -ErrorAction SilentlyContinue      # a stale CSV from a failed run must not be re-reduced
  & $pm --process_name PhoenixPointWin64.exe --output_file $csv `
        --timed 30 --terminate_after_timed --stop_existing_session --no_console_stats --v2_metrics
  if ($LASTEXITCODE -ne 0) { throw "$Tag : PresentMon exited $LASTEXITCODE - flag rejected or not elevated. Do NOT reduce this run." }
  if (-not (Test-Path $csv)) { throw "$Tag : PresentMon wrote no CSV - the capture caught nothing." }
  $t1 = Get-Date
  .\ppcli.ps1 connect screenshot ('{"path":"C:\\Temp\\claude\\perf-' + $Tag + '.png"}')
  Pop-Location
  "$Tag window $($t0.ToString('HH:mm:ss')) .. $($t1.ToString('HH:mm:ss'))" |
    Tee-Object -FilePath C:\Temp\claude\pm-windows.txt -Append
}
```

- [ ] 5. Run the five configurations back to back, **camera untouched between them** (same tactical view, same turn,
nothing selected — a moved camera invalidates the comparison):

```powershell
Measure-Cfg -Tag vanilla -Vig Vanilla -Shadow Vanilla  -Aniso Vanilla -Lod 0
Measure-Cfg -Tag shadow  -Vig Vanilla -Shadow VeryHigh -Aniso Vanilla -Lod 0
Measure-Cfg -Tag lod     -Vig Vanilla -Shadow Vanilla  -Aniso Vanilla -Lod 4.0
Measure-Cfg -Tag aniso   -Vig Vanilla -Shadow Vanilla  -Aniso Force16 -Lod 0
Measure-Cfg -Tag vigoff  -Vig Off     -Shadow Vanilla  -Aniso Vanilla -Lod 0
```

(Anisotropic and Vignette get their own runs because Task 8 must state their cost from a measurement instead of
asserting they are free.)

- [ ] 6. Stop the `nvidia-smi` sampler (Ctrl+C). Reduce every capture to **medians** — a median is the figure to report;
a mean is skewed by the loading/streaming outliers a 30 s window always contains:

The reducer goes in a **script file**, not pasted into the shell, so a missing column can `exit` with a non-zero code
instead of silently reporting `0` (an `exit` typed into the elevated interactive shell would just close it). Write
`C:\Temp\claude\pm-stat.ps1`:

```powershell
# Reduce the PresentMon 2.5.1 captures to medians. Exits non-zero, loudly, rather than reporting a 0 it did not measure.
#   2 = no CSV / no data rows      3 = expected column absent      4 = column present but empty
$ErrorActionPreference = 'Stop'

# Exact v2.5.1 --v2_metrics headers (PresentMon/CsvOutput.cpp:426, tag v2.5.1). NOT 'MsBetweenPresents'/'MsGPUBusy' -
# those are the v1 column set, i.e. what a capture WITHOUT --v2_metrics writes; the capture above passes --v2_metrics.
$Cols = [ordered]@{ FrameMs = 'FrameTime'; GpuBusyMs = 'GPUBusy'; GpuTimeMs = 'GPUTime'; CpuBusyMs = 'CPUBusy' }

function Median([object[]]$rows, [string]$tag, [string]$name) {
    $v = @($rows.$name | Where-Object { $_ -ne $null -and "$_".Trim() -ne '' -and "$_" -ne 'NA' } |
           ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) {
        Write-Error "$tag : column '$name' exists but holds no numeric values (all blank or NA) - nothing was measured."
        exit 4
    }
    $s = @($v | Sort-Object)
    [math]::Round($s[[int]($s.Count / 2)], 3)
}

function Stat([string]$tag) {
    $csv = "C:\Temp\claude\pm-$tag.csv"
    if (-not (Test-Path $csv)) { Write-Error "$tag : $csv does not exist - PresentMon captured nothing (elevated shell? flag rejected?)."; exit 2 }
    $rows = @(Import-Csv $csv)
    if ($rows.Count -eq 0) { Write-Error "$tag : $csv has a header but no data rows - the 30 s window caught no frames."; exit 2 }
    $have = @($rows[0].PSObject.Properties.Name)
    foreach ($c in $Cols.Values) {
        if ($have -notcontains $c) {
            Write-Error ("$tag : expected column '$c' is not in $csv. Actual headers: " + ($have -join ', ') +
                         ". This build's v2 column names differ from PresentMon 2.5.1 - re-check '& `$pm --help' and fix `$Cols. Do NOT report 0.")
            exit 3
        }
    }
    $o = [ordered]@{ Config = $tag; Frames = $rows.Count }
    foreach ($k in $Cols.Keys) { $o[$k] = Median $rows $tag $Cols[$k] }
    [pscustomobject]$o
}

@('vanilla','shadow','lod','aniso','vigoff') | ForEach-Object { Stat $_ } | Format-Table -AutoSize
Get-Content C:\Temp\claude\pm-windows.txt
```

```powershell
& C:\Temp\claude\pm-stat.ps1
$LASTEXITCODE          # MUST be 0. Any other value = the numbers are not real; fix the capture, do not report the table.
```

`GpuBusyMs` (`GPUBusy`) is the headline whole-frame GPU figure; `GpuTimeMs` (`GPUTime`) and `CpuBusyMs`
(`CPUBusy`) are recorded alongside it as cross-checks. Expected output: one row per config with the columns
`Config Frames FrameMs GpuBusyMs GpuTimeMs CpuBusyMs`, all four medians non-zero. A run whose CSV came from a
capture that dropped `--v2_metrics` exits `3` printing the actual (v1, `Ms`-prefixed) header list — re-capture,
never re-map `$Cols` to make the error go away.

For VRAM, take the **peak** `memory.used` from `C:\Temp\claude\vram.csv` inside each run's timestamp window printed by
`pm-windows.txt` (the nvidia-smi `timestamp` column is what makes that alignment possible).

- [ ] 7. Build the report table: per configuration — **median GPU busy ms** (primary), median frame time ms, peak VRAM
MiB, each with its delta vs Vanilla, plus a **secondary column** with the overlay's own CPU frame-time ms read off
`perf-<tag>.png`. Under D3D12 with a live upscaler also note the overlay `GPU:` line (`in / eval / out / wait` ms) and
label it explicitly as *upscale commands only, not whole-frame*; under D3D11 that line is absent by design — say so
rather than leaving a blank. **No threshold gate** — the numbers are reported, not enforced — but they are the source for
the README cost table in Task 8 step 1.

- [ ] 8. Commit the numbers as a research note only if a doc changed; otherwise carry them straight into Task 8.

---

## Task 8 — Documentation

**Files:** `README.md`, `docs\DESIGN.md`

- [x] 1. Add a user-facing block to `README.md` beside the other Options → Graphics rows. **Every number below is a
placeholder to be filled from the Task 7 step 7 report table** — median GPU busy ms delta vs Vanilla, and peak VRAM MiB
delta vs Vanilla, for the `shadow`, `lod`, `aniso` and `vigoff` runs. No cost may be described in words ("free",
"negligible", "no impact") — if a knob measured at or below the run-to-run noise, write the measured delta and say
`within measurement noise`. Do not ship the block with a `<…>` left in it.

```markdown
### Quality knobs

Four settings the game itself never exposes, in **Options → Graphics**. Every one has a **Vanilla**
position that restores the value the game's own graphics preset had set — switching a knob off does
not leave the mod's value behind, and changing the graphics preset does not lose your choice.

| Row | Values | What it does |
|---|---|---|
| Vignette | Vanilla / Off | Removes the darkened frame edges of tactical missions. |
| Shadow resolution | Vanilla / Very High | Raises the shadow map size above the preset's own tier. |
| Anisotropic filtering | Vanilla / 16x | Forces 16 samples on every texture — sharper ground and walls at grazing angles. |
| LOD detail | 0 (Vanilla) / 1.0 … 4.0 | Keeps higher-detail models at distance. |

Measured cost on an RTX 5070 Ti at 1440p, in a tactical mission with a fixed camera — median whole-frame GPU time over
30 s (PresentMon) and peak VRAM, against Vanilla:

| Setting | GPU time | VRAM |
|---|---|---|
| Shadow resolution: Very High | `<+X.X ms>` | `<+YYY MiB>` |
| LOD detail: 4.0 | `<+X.X ms>` | `<+YYY MiB>` |
| Anisotropic filtering: 16x | `<+X.X ms>` | `<+YYY MiB>` |
| Vignette: Off | `<+X.X ms>` | `<+YYY MiB>` |
```

Fill the four rows from **Task 7 step 7** (`shadow`, `lod`, `aniso`, `vigoff` — the `GpuBusyMs` (`GPUBusy`) delta vs the `vanilla` row,
and the peak-VRAM delta from `vram.csv` inside each run's window). Your own rig is not the reference: if the measurement
was made anywhere other than the RTX 5070 Ti / 1440p rig, change the sentence to name the hardware actually used.

- [x] 2. Add the design rows to `docs\DESIGN.md` (place them with the other feature sections):

```markdown
## Quality knobs (1.4.0)

`src\QualityKnobs.cs` (state + patches), `src\QualityPanel.cs` (rows). Config: `Vignette`,
`ShadowResolution`, `Anisotropic`, `LodBias` on `DlssConfig`, hidden from the Mods menu.

- **Vanilla = write the captured baseline back, never "skip the write."** A skipped write would leave
  the mod's value in place until the next preset change.
- **Baseline capture.** Vanilla writes `QualitySettings` inside `OptionsManager.UsePreset`
  (`OptionsManager.cs:384`), which nests `ChangeGraphicsQuality` (`:391`) → `OnGraphicsSettingsChangedEvent`
  (`:406`) → `LightingManager.ApplyPostProcessOptions` (`LightingManager.cs:53-55`). A Harmony prefix
  raises `inUsePreset`, a `HarmonyFinalizer` lowers it — always — and re-applies the knobs; it snapshots
  `{anisotropicFiltering, lodBias, shadowResolution}` **only when the original method did not throw**
  (`__exception == null`), and returns `__exception` unchanged so the failure is rethrown, not swallowed. Finalizer, not
  postfix: a throw must not latch the guard. Success-gated snapshot, because `UsePreset` can throw at
  `DefinedPresetIndexToQualityIndex` (`OptionsManager.cs:377,447`) *before* `SetQualityLevel` (`:384`) — at that point the
  live `QualitySettings` are still the mod's own overrides, and snapshotting them would record them as "Vanilla".
- **Fallback capture.** If no snapshot exists at the first mod write (preset applied before Harmony was
  installed), the current values are captured right before that write. An uninitialised snapshot is never restored.
- **Vignette** is captured per volume, before the first override, and written on `profile` — the runtime
  clone vanilla writes (`LightingManager.cs:172-178`) — never `sharedProfile`. The write is followed by the
  same invalidation vanilla performs at `LightingManager.cs:167`:
  `PostProcessManager.NeedUpdateSettings = true` (`PostProcessManager.cs:25`). The volume is reacquired on
  every seam because `LightingSettingsDef.ApplyTo` re-instantiates the lights prefab each level
  (`LightingSettingsDef.cs:21-22`).
- **Anisotropic restore** = the snapshot value plus `Texture.SetGlobalAnisotropicFilteringLimits(-1, -1)`,
  the engine default. Coexistence with another mod's limits is not attempted.
- **Seams:** `OptionsManager.UsePreset` finalizer (scalars + vignette), `LightingManager.ApplyPostProcessOptions`
  postfix (vignette only — scalars are suppressed while `inUsePreset`), `RenderforgeMod.OnLevelStart` (everything).
- **LOD slider positions are contiguous, values are not.** Valid biases are `0` (Vanilla) and `1.0 … 4.0`, so the slider
  runs `0..31` and maps position → value (`1.0 + (pos - 1) * 0.1`) instead of scaling. A scaled `0..40` slider would snap
  positions 1–9 back to 10 and trap keyboard/controller decrement at `1.0`.
- **Row order in Options → Graphics:** LUT → Colour vision → Scene style → Quality rows.
- **No "Extreme" shadow tier.** Unity 2019.4 caps custom shadow maps at 4096 dir / 2048 spot / 1024 point,
  so a tier above `VeryHigh` would be meaningless. 11 of 71 lights cast shadows, all `FromQualitySettings`.
```

- [x] 3. Commit:

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "docs: document the quality knobs and their measured cost"
```

---

## Self-review — spec coverage

Check each row before declaring the plan implemented.

| Spec requirement (§A) | Where it is satisfied |
|---|---|
| New `src\QualityKnobs.cs` with its own `[HarmonyPatch]` classes | Task 2 step 1, Task 3 step 1 |
| `Vignette` Vanilla/Off → `profile.Vignette.enabled.value = false` | Task 4 step 2 |
| `ShadowResolution` Vanilla/VeryHigh → `QualitySettings.shadowResolution` | Task 2 step 1 (`ApplyScalars`) |
| `Anisotropic` Vanilla/Force16 → `ForceEnable` + `SetGlobalAnisotropicFilteringLimits(16,16)` | Task 2 step 1 |
| `LodBias` 0 = Vanilla, else 1.0–4.0 → `QualitySettings.lodBias` | Task 2 step 1 (clamp), Task 5 step 1 (contiguous slider positions 0..31, `BiasFromPos`/`PosFromBias`) |
| **Vanilla means "write the captured baseline back", never "skip"** | Task 2 step 1 — every branch writes; `else` writes `base*` |
| Snapshot after vanilla finished applying a preset — and **only** if it finished (`__exception == null`) | Task 2 step 1 `LeaveUsePreset(bool succeeded)`, Task 3 step 1 finalizer; verified Task 6 step 8 |
| Prefix sets `inUsePreset`, suppressing all scalar writes from nested seams | Task 2 (`ApplyScalars` early-returns on `InUsePreset`), Task 3 step 1 |
| `finally`-safe pattern (Harmony `Finalizer`) | Task 3 step 1 `[HarmonyFinalizer]` |
| Snapshot refreshed on every preset change | Task 3 step 1 (the finalizer runs per `UsePreset`); verified Task 6 step 6 |
| Fallback capture before the first write; never restore an uninitialised snapshot | Task 2 step 1 (`if (!haveSnapshot) Snapshot();` before any write) |
| Vignette baseline captured once **per volume** before the first override | Task 4 step 2 (`if (found != volume) haveVignetteBase = false;`) |
| Aniso restore = snapshot + `(-1,-1)`; no coexistence attempt | Task 2 step 1, documented Task 8 step 2 |
| Seam: postfix `OptionsManager.UsePreset` (scalars + vignette) | Task 3 step 1 (as a Finalizer, which also covers the throw path) |
| Seam: `ApplyPostProcessOptions` — reacquire volume each call, vignette only, scalars suppressed | Task 3 step 1 + Task 4 step 2 (`CurrentVolume` called every time) |
| Seam: `RenderforgeMod.OnLevelStart` (everything) | Task 3 step 2 |
| Direct UI vignette write performs the same PPv2 invalidation as `LightingManager.cs:167` | Task 4 step 2 — `PostProcessManager.NeedUpdateSettings = true` (read from source, not guessed) |
| Rows in Options → Graphics via `GraphicsPanel`, pickers cloned from `TextureQualityPicker`, slider from `ShadowDistanceSlider` | Task 5 step 1-3 |
| Immediate apply + `SaveConfig` | Task 5 step 1 (`Change` → `ApplyAll` + `SaveConfig` + `Sync`) |
| Hidden from the Mods menu | Task 1 step 3 |
| en + ru labels | Task 1 step 4 (config), Task 5 step 1 (rows) |
| Acceptance: readback per preset (all 6) | Task 6 steps 5-6 |
| Acceptance: same-preset reapply | Task 6 step 7 |
| Acceptance: failed `UsePreset` (invalid index) leaves the baseline untouched and the guard down | Task 6 step 8 |
| Acceptance: Vanilla round-trip restores the snapshot | Task 6 step 9 |
| Acceptance: survives `OnLevelStart` (tactical → geoscape → tactical) | Task 6 step 10 |
| Acceptance: vignette fixed-camera screenshot crop, edges differ / centre identical | Task 6 step 11 |
| Acceptance: LOD slider decrement from 1.0 reaches Vanilla, one step per press | Task 6 step 12 |
| Perf: whole-frame GPU time (PresentMon median) + peak VRAM, VeryHigh / LOD 4.0 / 16x / Vignette Off vs Vanilla, no threshold gate, README table filled from the measurement | Task 7 steps 2-7, Task 8 step 1 |
| Tests on Instance3, not Instance2, not Steam | Task 6 step 1-2 (`-PPRoot 'D:\PP-Instance3'` everywhere) |

**Out of scope, do not implement:** section B (Colour vision) and section C (Reflex standalone). Also excluded by the spec's own "Out of scope" list: crisp UI work, a UI scale slider, motion blur / grain toggles, a shadow "Extreme" tier, HUD colour-blind palettes.
