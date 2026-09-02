# Renderforge Phase 1 — Renderer switch + D3D12 fix + availability infra — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the player choose DirectX 11 or DirectX 12 from the game's own Graphics options, restart the game into that renderer, make D3D12 render correctly (PPv2 fix), and show every not-yet-available upscaler/frame-gen entry greyed with the game's native tooltip explaining why.

**Architecture:** Managed-only phase — no C++ changes. Three new C# files (`Availability.cs`, `RendererSwitch.cs`, `Pickers.cs`, `D3D12Fix.cs`) plus small edits to `GraphicsPanel.cs`, `Overlay.cs`, `Patches.cs`, `RenderforgeMod.cs`, `DlssConfig.cs`. All UI is cloned from the game's own `ArrowPickerController` rows, the confirmation dialog is the game's `Base.UI.MessageBox.MessageBox`, and the tooltip is the game's `UITooltipText` component — no custom widgets. The D3D12 fix rides the Harmony postfix that already exists on `Base.Lighting.LightingManager.ApplyPostProcessOptions`.

**Tech Stack:** C# for Unity 2019.4.31f1 Mono, `net472`, `LangVersion=latest` but **stay on C# 7.3-compatible syntax** (no records, no switch expressions, no target-typed `new`); HarmonyLib 2 (`0Harmony.dll` from `<PPRoot>\ModSDK`); Unity PPv2 (`Unity.Postprocessing.Runtime.dll`, already referenced in `Renderforge.csproj:67-70`); PPCLI (`E:\DEV\PhoenixPoint\PPCLI\ppcli.ps1`) for every in-game test; build/deploy with `E:\DEV\PhoenixPoint\Renderforge\deploy.ps1` (defaults to `-PPRoot D:\PP-Instance2`, `-SkipNative` reuses `build\out\*.dll` — managed-only tasks always pass `-SkipNative`).

**Shell:** PowerShell only. Never touch `D:\Steam\steamapps\common\Phoenix Point`.

---

## Verified ground truth (do not re-derive; cite these in code comments)

| Thing | Where it was verified |
|---|---|
| Native confirm dialog | `Base.UI.MessageBox.MessageBox.ShowSimplePrompt(string, MessageBoxIcon, MessageBoxButtons, MessageBoxCallback, object sender = null, object userData = null)` — `decompiled\AssemblyCSharp\Assembly-CSharp\src\Base.UI.MessageBox\MessageBox.cs:77`. Obtained via `Base.Core.GameUtl.GetMessageBox()` (`GameUtl.cs:106`, returns `null` before the game object exists). Result in `MessageBoxCallbackResult.DialogResult` (`MessageBoxCallbackResult.cs:5`), enum `MessageBoxResult { None, OK, Cancel, Abort, Retry, Ignore, Yes, No }` (`MessageBoxResult.cs:3`), buttons `MessageBoxButtons.YesNo = 0x60` (`MessageBoxButtons.cs:19`), icon `MessageBoxIcon.Question` (used by `UIStateMainMenu.cs:102`). |
| Native tooltip | `UITooltipText` (global namespace, `decompiled\...\src\UITooltipText.cs:7`), a `MonoBehaviour` implementing `IPointerEnterHandler`/`IPointerExitHandler` (`:99`, `:104`). Fields: `TipText`, `MaxWidth`, `Position` (`UITooltip.Position`), `AppearTime`, `FadeInTime`, `FadeOutTime`, `Enabled`, `TextColor`. It clones the game's own prefab `Interface/UI_Prefabs/UI_Tooltip` (`:52`) and calls `UITooltip.Init(...)` (`UITooltip.cs:76`). Must be attached to a GameObject that receives pointer events — use the picker's `CentralButton` (a `PhoenixGeneralButton`, `ArrowPickerController.cs:20`). |
| PPv2 resources access path | `PostProcessLayer` holds `private PostProcessResources m_Resources` (`decompiled\Postprocessing\UnityEngine.Rendering.PostProcessing\PostProcessLayer.cs:55`), assigned by `Init(PostProcessResources)` (`:184`) and handed to the render context each frame (`:624 context.resources = m_Resources`). Reach it with `HarmonyLib.Traverse.Create(layer).Field("m_Resources").GetValue<PostProcessResources>()` on every `UnityEngine.Object.FindObjectsOfType<PostProcessLayer>()` — the same enumeration `LightingManager.cs:180` itself uses. `PostProcessResources.computeShaders.lut3DBaker` is a public field (`PostProcessResources.cs:71,:113`); nulling it makes `ColorGradingRenderer.cs:33` take the LDR 2D-LUT path instead of `KGenLut3D_AcesTonemap` (`:80`). |
| AO fix options | `AmbientOcclusion.mode` is an `AmbientOcclusionModeParameter` (`AmbientOcclusion.cs:10`); `AmbientOcclusionMode { ScalableAmbientObscurance, MultiScaleVolumetricObscurance }` (`AmbientOcclusionMode.cs:3`). SAO needs only the **pixel** shader `resources.shaders.scalableAO` (`AmbientOcclusion.cs:82-88`); MSVO needs the four compute shaders including `multiScaleAODownsample1` (`:94`), which has no D3D12 kernel. `ParameterOverride<T>.Override(T)` sets value + `overrideState` (`ParameterOverride.cs:61`). `enabled.value` is the on/off switch the game itself writes (`LightingManager.cs:195`). |
| Harmony seam for the fix | `Base.Lighting.LightingManager.ApplyPostProcessOptions(OptionsManager.GraphicsQualityPreset)` — `decompiled\...\Base.Lighting\LightingManager.cs:163-187`. **Already patched** by `Renderforge\src\Patches.cs:19-23`; extend that postfix, do not add a second patch on the same method. |
| Options-panel apply flow | `UIModuleGraphicsOptionsPanel.Init()` (`:86`) rebuilds rows on every open, `HasChanges()` (`:124`) lights the Apply button, `Apply()` (`:137`) commits. Private `Action _onChanged` (`:49`) must be invoked to light Apply — read it with `Traverse.Create(panel).Field("_onChanged").GetValue<Action>()`, exactly like `src\VideoPanel.cs:40`. |
| Picker API | `ArrowPickerController.Init(int valueRange, int currentValue, Action<int> onValueChanged)` (`ArrowPickerController.cs:32`), `CurrentIndex` (`:28`), `Title`/`CurrentItem` (`Localize`), `CurrentItemText` (`Text`), `PreviousArrow`/`NextArrow`/`CentralButton` (`PhoenixGeneralButton`), `SetEnabled(bool)` (`:62`). |
| Player.log | `%USERPROFILE%\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log` (shared by both installs — always read it right after the run you care about). |

---

## File structure

| File | Responsibility |
|---|---|
| `src\DlssConfig.cs` (modify) | Adds the `RendererMode` enum + persisted `Renderer` field with RU/EN labels. |
| `src\Availability.cs` (create) | The single source of "can this feature run right now, and if not, why" — one `Reason(Feature)` returning `null` or a localized string. Drives grey + tooltip + overlay. |
| `src\RendererSwitch.cs` (create) | Command-line rebuild, `Process.Start` restart, the native confirmation dialog, and the once-per-session startup prompt. |
| `src\Pickers.cs` (create) | The three new option rows (Renderer, Upscaler, Frame generation) cloned from `TextureQualityPicker`, plus the Harmony `HasChanges`/`Apply` postfixes that make Renderer a deferred (Apply-button) setting. |
| `src\GraphicsPanel.cs` (modify) | Keeps owning the DLSS quality picker + sharpness slider; gains the reusable `Grey()` / `Tip()` helpers and calls `Pickers.Build()` so row order is deterministic. |
| `src\D3D12Fix.cs` (create) | The PPv2 D3D12 repair (AO mode/off + `lut3DBaker = null`). |
| `src\Patches.cs` (modify) | One-line hook of `D3D12Fix` into the existing `LightingManager` postfix. |
| `src\RenderforgeMod.cs` (modify) | Gate change: the mod stays alive under D3D12 with the native DLSS init skipped; overlay applies even without a driver; arms the startup prompt. |
| `src\Overlay.cs` (modify) | `Renderer:` line + the `FgFps` seam for Phase 5. |
| `docs\DESIGN.md`, `README.md` (modify) | User- and dev-facing documentation of the switch. |

---

### Task 1: Config — `RendererMode`

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\DlssConfig.cs:9-11` (enum block), `:42` (field block), `:57` (RU table)

- [ ] **Step 1: Add the enum next to the existing config enums**

In `src\DlssConfig.cs`, replace this block (currently lines 9-11):

```csharp
    public enum DebugView { None, Passthrough, Depth, MotionVectors }

    public enum OverlayCorner { TopLeft, TopCenter, TopRight, BottomCenter }
```

with:

```csharp
    public enum DebugView { None, Passthrough, Depth, MotionVectors }

    public enum OverlayCorner { TopLeft, TopCenter, TopRight, BottomCenter }

    /// <summary>Which graphics API the game should be launched with. Auto == DirectX11 (the game's own default);
    /// DirectX12 needs "-force-d3d12" on the command line, i.e. a restart (RendererSwitch).</summary>
    public enum RendererMode { Auto, DirectX11, DirectX12 }
```

- [ ] **Step 2: Add the persisted field**

In the same file, immediately after the `DebugView` field (currently line 42, `public DebugView DebugView = DebugView.None;`), add:

```csharp
        [ConfigField("Renderer", "Auto = DirectX 11. DirectX 12 is experimental and needs a restart.")]
        public RendererMode Renderer = RendererMode.Auto;
```

- [ ] **Step 3: Add the Russian label**

In the `Ru` dictionary, after the `DebugView` entry (currently line 57), add:

```csharp
            { nameof(Renderer), new[] { "Рендерер", "Авто = DirectX 11. DirectX 12 — экспериментальный, требуется перезапуск." } },
```

- [ ] **Step 4: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 5: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\DlssConfig.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(config): RendererMode field (Auto/DirectX11/DirectX12) with RU labels"
```

---

### Task 2: `Availability` — one reason string per feature

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\src\Availability.cs`

- [ ] **Step 1: Write the file**

```csharp
using UnityEngine;
using UnityEngine.Rendering;

namespace Renderforge
{
    /// <summary>Everything the pickers, the tooltips and the overlay can be greyed for.</summary>
    internal enum Feature { Dlss, Fsr, Xess, FrameGen }

    /// <summary>The single "can this run right now, and if not why" oracle. Reason(f) == null means available.
    /// Phase 1 knows only the API + vendor gates; Phase 2-5 add "DLL missing" / "SDK init failed" here and
    /// nowhere else.</summary>
    internal static class Availability
    {
        private const int VendorNvidia = 0x10DE;   // PCI vendor id, SystemInfo.graphicsDeviceVendorID

        internal static GraphicsDeviceType Api { get { return SystemInfo.graphicsDeviceType; } }
        internal static bool IsD3D11 { get { return Api == GraphicsDeviceType.Direct3D11; } }
        internal static bool IsD3D12 { get { return Api == GraphicsDeviceType.Direct3D12; } }
        internal static bool IsNvidia { get { return SystemInfo.graphicsDeviceVendorID == VendorNvidia; } }

        /// <summary>Short name for the overlay: "D3D11" / "D3D12" / whatever Unity reports otherwise.</summary>
        internal static string ApiName
        {
            get { return IsD3D11 ? "D3D11" : IsD3D12 ? "D3D12" : Api.ToString(); }
        }

        internal static string Reason(Feature feature)
        {
            switch (feature)
            {
                case Feature.Dlss:
                    if (!IsD3D11 && !IsD3D12)
                        return DlssConfig.Loc("Requires DirectX 11 or DirectX 12", "Требуется DirectX 11 или DirectX 12");
                    if (!IsNvidia)
                        return DlssConfig.Loc("Requires an NVIDIA RTX GPU", "Требуется видеокарта NVIDIA RTX");
                    if (IsD3D12)
                        return DlssConfig.Loc("DLSS on D3D12 comes in Phase 2", "DLSS на D3D12 появится в фазе 2");
                    return RenderforgeMod.Available
                        ? null
                        : DlssConfig.Loc("DLSS init failed — see the log", "Не удалось инициализировать DLSS — смотрите лог");
                case Feature.Fsr:
                case Feature.Xess:
                case Feature.FrameGen:
                    return IsD3D12
                        ? DlssConfig.Loc("Not implemented yet", "Пока не реализовано")
                        : DlssConfig.Loc("Requires DirectX 12 — switch Renderer", "Требуется DirectX 12 — переключите рендерер");
                default:
                    return null;
            }
        }
    }
}
```

- [ ] **Step 2: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 3: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\Availability.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat: Availability oracle - one reason string per upscaler/FG feature"
```

---

### Task 3: `RendererSwitch` — command line, restart, native dialog, startup prompt

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\src\RendererSwitch.cs`

- [ ] **Step 1: Write the file**

```csharp
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using Base.Core;
using Base.UI.MessageBox;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Switching the graphics API is a process-level thing: Unity picks it from the command line at
    /// startup ("-force-d3d12"), so the only honest switch is relaunch. The confirmation uses the game's own
    /// MessageBox (MessageBox.cs:77 ShowSimplePrompt, GameUtl.cs:106) - never a custom dialog.</summary>
    internal static class RendererSwitch
    {
        internal const string Flag12 = "-force-d3d12";
        internal const string Flag11 = "-force-d3d11";

        internal static RendererMode Running
        {
            get { return Availability.IsD3D12 ? RendererMode.DirectX12 : RendererMode.DirectX11; }
        }

        /// <summary>Auto means DirectX 11 (the game's own default).</summary>
        internal static RendererMode Effective(RendererMode mode)
        {
            return mode == RendererMode.DirectX12 ? RendererMode.DirectX12 : RendererMode.DirectX11;
        }

        internal static bool Wants12(DlssConfig cfg)
        {
            return cfg != null && Effective(cfg.Renderer) == RendererMode.DirectX12;
        }

        /// <summary>argv[0] is the full exe path under Mono on Windows; Application.dataPath
        /// ("&lt;root&gt;\PhoenixPointWin64_Data") is the fallback.</summary>
        internal static string ExePath()
        {
            string[] argv = Environment.GetCommandLineArgs();
            if (argv.Length > 0 && !string.IsNullOrEmpty(argv[0]) && File.Exists(argv[0]))
                return Path.GetFullPath(argv[0]);
            string root = Directory.GetParent(Application.dataPath).FullName;
            return Path.Combine(root, "PhoenixPointWin64.exe");
        }

        /// <summary>The CURRENT command line (so "-mods" and everything else survives) minus any renderer
        /// flag, plus the one we want. DirectX 11 = no flag at all.</summary>
        internal static string Args(bool want12)
        {
            string[] argv = Environment.GetCommandLineArgs();
            List<string> kept = new List<string>();
            for (int i = 1; i < argv.Length; i++)
            {
                string a = argv[i];
                if (string.Equals(a, Flag11, StringComparison.OrdinalIgnoreCase)) continue;
                if (string.Equals(a, Flag12, StringComparison.OrdinalIgnoreCase)) continue;
                kept.Add(a.IndexOf(' ') >= 0 ? "\"" + a + "\"" : a);
            }
            if (want12) kept.Add(Flag12);
            return string.Join(" ", kept.ToArray());
        }

        /// <summary>PPCLI check without restarting anything:
        /// {"op":"invoke","type":"Renderforge.RendererSwitch","assembly":"Renderforge","member":"Preview","args":[true]}</summary>
        public static string Preview(bool want12)
        {
            return ExePath() + " " + Args(want12);
        }

        public static string Restart(bool want12)
        {
            string exe = ExePath();
            string args = Args(want12);
            if (RenderforgeMod.Instance != null)
                RenderforgeMod.Instance.Logger.LogInfo("Renderforge restart: " + exe + " " + args);
            ProcessStartInfo psi = new ProcessStartInfo(exe, args);
            psi.WorkingDirectory = Path.GetDirectoryName(exe);
            psi.UseShellExecute = true;
            Process.Start(psi);
            Application.Quit();
            return exe + " " + args;
        }

        /// <summary>Yes -> relaunch. No -> onNo() (the caller repaints the "(restart pending)" label).
        /// If the MessageBox is not up yet, treat it as No.</summary>
        internal static void Confirm(bool want12, Action onNo)
        {
            MessageBox box = GameUtl.GetMessageBox();
            if (box == null) { if (onNo != null) onNo(); return; }
            string text = DlssConfig.Loc("Restart required to switch renderer. Restart now?",
                                         "Для смены рендерера нужен перезапуск. Перезапустить сейчас?");
            box.ShowSimplePrompt(text, MessageBoxIcon.Question, MessageBoxButtons.YesNo, delegate(MessageBoxCallbackResult res)
            {
                if (res.DialogResult == MessageBoxResult.Yes) Restart(want12);
                else if (onNo != null) onNo();
            });
        }

        /// <summary>Config says DirectX 12 but the process runs D3D11 (a plain Steam launch): offer the restart
        /// ONCE, as soon as the MessageBox exists.</summary>
        internal static void ArmStartupPrompt()
        {
            if (StartupPrompt.Armed) return;
            StartupPrompt.Armed = true;
            GameObject go = new GameObject("RenderforgeStartupPrompt");
            go.hideFlags = HideFlags.HideAndDontSave;
            UnityEngine.Object.DontDestroyOnLoad(go);
            go.AddComponent<StartupPrompt>();
        }

        private class StartupPrompt : MonoBehaviour
        {
            internal static bool Armed;

            private void Update()
            {
                DlssConfig cfg = RenderforgeMod.Instance != null ? RenderforgeMod.Instance.Cfg : null;
                if (cfg == null || !Wants12(cfg) || Availability.IsD3D12) { Destroy(gameObject); return; }
                if (GameUtl.GetMessageBox() == null) return;   // UI not up yet, keep waiting
                Destroy(gameObject);                           // once per session
                Confirm(true, null);
            }
        }
    }
}
```

- [ ] **Step 2: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 3: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\RendererSwitch.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat: RendererSwitch - relaunch with -force-d3d12 behind the game's own MessageBox"
```

---

### Task 4: Grey + tooltip helpers

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\GraphicsPanel.cs:160` (after `SetRaw`)

- [ ] **Step 1: Add the two helpers**

In `src\GraphicsPanel.cs`, immediately after the closing brace of `SetRaw` (currently line 160), before the class's closing brace, add:

```csharp

        /// <summary>Same grey as SetSliderEnabled: interactable alone shows nothing on these prefabs, a CanvasGroup does.</summary>
        internal static void Grey(GameObject go, bool grey)
        {
            if (go == null) return;
            var cg = go.GetComponent<CanvasGroup>() ?? go.AddComponent<CanvasGroup>();
            cg.alpha = grey ? 0.35f : 1f;
        }

        /// <summary>The game's OWN tooltip (UITooltipText.cs:7, IPointerEnterHandler at :99) - it clones the
        /// game's "Interface/UI_Prefabs/UI_Tooltip" prefab (:52). Attach it to something that receives pointer
        /// events (a picker's CentralButton). tip == null/empty disables it.</summary>
        internal static void Tip(GameObject go, string tip)
        {
            if (go == null) return;
            var t = go.GetComponent<UITooltipText>();
            if (string.IsNullOrEmpty(tip))
            {
                if (t != null) t.Enabled = false;
                return;
            }
            if (t == null)
            {
                t = go.AddComponent<UITooltipText>();
                t.Position = UITooltip.Position.RightMiddle;
                t.MaxWidth = 300;
                t.AppearTime = 0.3f;
                t.FadeInTime = 8f;
                t.FadeOutTime = 8f;
            }
            t.Enabled = true;
            t.TipText = tip;
            t.UpdateText(tip);
        }
```

- [ ] **Step 2: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 3: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\GraphicsPanel.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(ui): reusable Grey()/Tip() helpers using the game's UITooltipText"
```

---

### Task 5: The three new option rows (`Pickers.cs`)

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\src\Pickers.cs`

- [ ] **Step 1: Write the file**

```csharp
using System;
using HarmonyLib;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;

namespace Renderforge
{
    /// <summary>The Renderforge rows in Options -> Graphics, all clones of the panel's own TextureQualityPicker
    /// (UIModuleGraphicsOptionsPanel.cs:25) placed directly under it, in this order:
    /// RENDERER, UPSCALER, FRAME GENERATION. GraphicsPanel then places its DLSS quality picker + sharpness
    /// slider after the row this returns, so the order is decided in ONE place.
    /// RENDERER is deferred like the panel's own settings (HasChanges lights Apply, Apply commits + asks);
    /// UPSCALER applies immediately when it is available, and an unavailable entry just stays greyed with the
    /// tooltip and writes nothing.</summary>
    internal static class Pickers
    {
        internal const string RendererName = "RenderforgeRenderer";
        internal const string UpscalerName = "RenderforgeUpscaler";
        internal const string FrameGenName = "RenderforgeFrameGen";

        private static bool loggedError;
        private static ArrowPickerController renderer, upscaler, frameGen;
        private static RendererMode pendingRenderer;
        private static int pendingUpscaler, pendingFrameGen;
        private static Action onChanged;

        private static string[] RendererLabels
        {
            get { return new[] { "DirectX 11", DlssConfig.Loc("DirectX 12 (experimental)", "DirectX 12 (экспериментально)") }; }
        }

        private static string[] UpscalerLabels
        {
            get { return new[] { DlssConfig.Loc("Off", "Выкл"), "DLSS", "FSR", "XeSS" }; }
        }

        private static string[] FrameGenLabels
        {
            get { return new[] { DlssConfig.Loc("Off", "Выкл"), "2x", "3x", "4x" }; }
        }

        private static Feature UpscalerFeature(int index)
        {
            return index == 2 ? Feature.Fsr : index == 3 ? Feature.Xess : Feature.Dlss;
        }

        /// <summary>Builds/re-syncs the three rows and returns the LAST one, for the caller to place after.</summary>
        internal static Transform Build(UIModuleGraphicsOptionsPanel panel)
        {
            var src = panel.TextureQualityPicker;
            var cfg = RenderforgeMod.Instance.Cfg;
            onChanged = Traverse.Create(panel).Field("_onChanged").GetValue<Action>();

            pendingRenderer = RendererSwitch.Effective(cfg.Renderer);
            renderer = Row(src, RendererName, DlssConfig.Loc("Renderer", "Рендерер"), src.transform.GetSiblingIndex() + 1);
            renderer.Init(RendererLabels.Length, pendingRenderer == RendererMode.DirectX12 ? 1 : 0, OnRenderer);
            ShowRenderer();

            pendingUpscaler = cfg.Mode == RenderforgeMode.Off ? 0 : 1;
            upscaler = Row(src, UpscalerName, DlssConfig.Loc("Upscaler", "Апскейлер"), renderer.transform.GetSiblingIndex() + 1);
            upscaler.Init(UpscalerLabels.Length, pendingUpscaler, OnUpscaler);
            ShowUpscaler();

            pendingFrameGen = 0;
            frameGen = Row(src, FrameGenName, DlssConfig.Loc("Frame generation", "Генерация кадров"), upscaler.transform.GetSiblingIndex() + 1);
            frameGen.Init(FrameGenLabels.Length, pendingFrameGen, OnFrameGen);
            ShowFrameGen();

            return frameGen.transform;
        }

        internal static void Hide(Transform content)
        {
            foreach (string n in new[] { RendererName, UpscalerName, FrameGenName })
            {
                var t = content.Find(n);
                if (t != null) t.gameObject.SetActive(false);
            }
        }

        private static ArrowPickerController Row(ArrowPickerController src, string name, string title, int siblingIndex)
        {
            var found = src.transform.parent.Find(name);
            ArrowPickerController p;
            if (found != null) p = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(src.gameObject, src.transform.parent);
                go.name = name;
                p = go.GetComponent<ArrowPickerController>();
                GraphicsPanel.SetRaw(p.Title, null, title.ToUpperInvariant());
            }
            p.transform.SetSiblingIndex(siblingIndex);
            p.gameObject.SetActive(true);
            return p;
        }

        private static void ShowRenderer()
        {
            string label = RendererLabels[pendingRenderer == RendererMode.DirectX12 ? 1 : 0];
            if (pendingRenderer != RendererSwitch.Running)
                label += DlssConfig.Loc(" (restart pending)", " (нужен перезапуск)");
            GraphicsPanel.SetRaw(renderer.CurrentItem, renderer.CurrentItemText, label);
            GraphicsPanel.Grey(renderer.CurrentItem.gameObject, false);
            GraphicsPanel.Tip(renderer.CentralButton.gameObject,
                DlssConfig.Loc("DirectX 12 unlocks FSR, XeSS and frame generation. Changing it restarts the game.",
                               "DirectX 12 открывает FSR, XeSS и генерацию кадров. Смена требует перезапуска игры."));
        }

        private static void ShowUpscaler()
        {
            string reason = pendingUpscaler == 0 ? null : Availability.Reason(UpscalerFeature(pendingUpscaler));
            GraphicsPanel.SetRaw(upscaler.CurrentItem, upscaler.CurrentItemText, UpscalerLabels[pendingUpscaler]);
            GraphicsPanel.Grey(upscaler.CurrentItem.gameObject, reason != null);
            GraphicsPanel.Tip(upscaler.CentralButton.gameObject, reason);
        }

        private static void ShowFrameGen()
        {
            string reason = pendingFrameGen == 0 ? null : Availability.Reason(Feature.FrameGen);
            GraphicsPanel.SetRaw(frameGen.CurrentItem, frameGen.CurrentItemText, FrameGenLabels[pendingFrameGen]);
            GraphicsPanel.Grey(frameGen.CurrentItem.gameObject, reason != null);
            GraphicsPanel.Tip(frameGen.CentralButton.gameObject, reason);
        }

        private static void OnRenderer(int index)
        {
            try
            {
                pendingRenderer = index == 1 ? RendererMode.DirectX12 : RendererMode.DirectX11;
                ShowRenderer();
                if (onChanged != null) onChanged();   // lights the panel's Apply button
            }
            catch (Exception ex) { Log("renderer picker change failed", ex); }
        }

        private static void OnUpscaler(int index)
        {
            try
            {
                pendingUpscaler = index;
                ShowUpscaler();
                var mod = RenderforgeMod.Instance;
                if (mod == null) return;
                if (index == 0)
                {
                    RenderforgeMod.SetMode(RenderforgeMode.Off.ToString(), mod.Cfg.DebugView.ToString());
                    RenderforgeMod.SaveConfig();
                }
                else if (index == 1 && Availability.Reason(Feature.Dlss) == null && mod.Cfg.Mode == RenderforgeMode.Off)
                {
                    RenderforgeMod.SetMode(RenderforgeMode.Auto.ToString(), mod.Cfg.DebugView.ToString());
                    RenderforgeMod.SaveConfig();
                }
                GraphicsPanel.SyncQuality();   // keep the DLSS quality row's label/grey in step
            }
            catch (Exception ex) { Log("upscaler picker change failed", ex); }
        }

        private static void OnFrameGen(int index)
        {
            try
            {
                pendingFrameGen = index;
                ShowFrameGen();
            }
            catch (Exception ex) { Log("frame-generation picker change failed", ex); }
        }

        internal static bool RendererChanged
        {
            get
            {
                var cfg = RenderforgeMod.Instance != null ? RenderforgeMod.Instance.Cfg : null;
                return cfg != null && renderer != null && pendingRenderer != RendererSwitch.Effective(cfg.Renderer);
            }
        }

        internal static void ApplyRenderer()
        {
            try
            {
                var cfg = RenderforgeMod.Instance != null ? RenderforgeMod.Instance.Cfg : null;
                if (cfg == null || renderer == null) return;
                if (pendingRenderer == RendererSwitch.Effective(cfg.Renderer)) return;
                cfg.Renderer = pendingRenderer;
                RenderforgeMod.SaveConfig();
                ShowRenderer();
                if (pendingRenderer != RendererSwitch.Running)
                    RendererSwitch.Confirm(pendingRenderer == RendererMode.DirectX12, ShowRenderer);
            }
            catch (Exception ex) { Log("renderer apply failed", ex); }
        }

        private static void Log(string what, Exception ex)
        {
            if (!loggedError && RenderforgeMod.Instance != null)
                RenderforgeMod.Instance.Logger.LogError("Renderforge " + what + ": " + ex);
            loggedError = true;
        }
    }

    /// <summary>RENDERER is a deferred setting: the panel's own Apply button commits it
    /// (UIModuleGraphicsOptionsPanel.cs:124 HasChanges, :137 Apply) - same shape as VideoPanel.</summary>
    [HarmonyPatch(typeof(UIModuleGraphicsOptionsPanel))]
    internal static class GraphicsPanelApply
    {
        [HarmonyPostfix, HarmonyPatch("HasChanges")]
        static void HasChanges(ref bool __result)
        {
            __result |= Pickers.RendererChanged;
        }

        [HarmonyPostfix, HarmonyPatch("Apply")]
        static void Apply()
        {
            Pickers.ApplyRenderer();
        }
    }
}
```

- [ ] **Step 2: Build (expected to FAIL — `SyncQuality` does not exist yet)**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `error CS0117: 'GraphicsPanel' does not contain a definition for 'SyncQuality'`. Task 6 adds it.

- [ ] **Step 3: Do NOT commit yet** — the tree does not build. Commit happens at the end of Task 6.

---

### Task 6: Wire `GraphicsPanel` to the new rows

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\GraphicsPanel.cs:25-62` (the `Postfix`), and add `SyncQuality`

- [ ] **Step 1: Replace the `Postfix` body**

In `src\GraphicsPanel.cs`, replace the whole `Postfix` method (currently lines 25-62) with:

```csharp
        static void Postfix(UIModuleGraphicsOptionsPanel __instance)
        {
            try
            {
                var mod = RenderforgeMod.Instance;
                var src = __instance.TextureQualityPicker;
                if (mod == null || src == null) return;
                var existing = src.transform.parent.Find(Name);
                if (!mod.Cfg.ShowInGraphicsOptions)
                {
                    Pickers.Hide(src.transform.parent);
                    if (existing != null) existing.gameObject.SetActive(false);
                    var hidden = src.transform.parent.Find(SliderName);
                    if (hidden != null) hidden.gameObject.SetActive(false);
                    return;
                }
                // RENDERER / UPSCALER / FRAME GENERATION first; our quality row goes after the last of them.
                Transform after = Pickers.Build(__instance);
                if (existing != null) picker = existing.GetComponent<ArrowPickerController>();
                else
                {
                    var go = UnityEngine.Object.Instantiate(src.gameObject, src.transform.parent);
                    go.name = Name;
                    picker = go.GetComponent<ArrowPickerController>();
                    SetRaw(picker.Title, null, DlssConfig.Loc("DLSS quality", "Качество DLSS").ToUpperInvariant());
                }
                picker.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
                picker.gameObject.SetActive(true);
                int idx = (int)mod.Cfg.Mode;
                if (idx < 0 || idx >= Labels.Length) idx = 0;
                picker.Init(Labels.Length, idx, i => OnChanged(picker, i));
                BuildSlider(__instance, picker.transform, mod.Cfg);
                SyncQuality();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge graphics-panel picker failed: " + ex);
                loggedError = true;
            }
        }

        /// <summary>Repaints the quality row's label + grey from the config and Availability. Called after our own
        /// changes and by Pickers when the UPSCALER row moves.</summary>
        internal static void SyncQuality()
        {
            var mod = RenderforgeMod.Instance;
            if (picker == null || mod == null) return;
            int idx = (int)mod.Cfg.Mode;
            if (idx < 0 || idx >= Labels.Length) idx = 0;
            string reason = Availability.Reason(Feature.Dlss);
            SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[idx]);
            Grey(picker.CurrentItem.gameObject, reason != null);
            Tip(picker.CentralButton.gameObject, reason);
            SetSliderEnabled(reason == null && idx != (int)RenderforgeMode.Off);
        }
```

- [ ] **Step 2: Add the `picker` field**

In the same file, in the field block (currently lines 19-23), replace:

```csharp
        private static bool loggedError;
        private static Slider sharp;
        private static Transform sharpValue;
```

with:

```csharp
        private static bool loggedError;
        private static ArrowPickerController picker;
        private static Slider sharp;
        private static Transform sharpValue;
```

- [ ] **Step 3: Make `OnChanged` reuse `SyncQuality`**

Replace the whole `OnChanged` method (currently lines 64-80) with:

```csharp
        private static void OnChanged(ArrowPickerController target, int i)
        {
            try
            {
                var mod = RenderforgeMod.Instance;
                if (mod == null) return;
                if (Availability.Reason(Feature.Dlss) != null)
                {
                    // Not usable on this API/GPU: show the choice greyed, write nothing.
                    SetRaw(target.CurrentItem, target.CurrentItemText, Labels[i]);
                    Grey(target.CurrentItem.gameObject, true);
                    return;
                }
                RenderforgeMod.SetMode(((RenderforgeMode)i).ToString(), mod.Cfg.DebugView.ToString());
                RenderforgeMod.SaveConfig();
                SyncQuality();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge picker change failed: " + ex);
                loggedError = true;
            }
        }
```

- [ ] **Step 4: Drop the now-unused `Available` gate reference**

Confirm there is no remaining `RenderforgeMod.Available` reference in `src\GraphicsPanel.cs`:

```powershell
Select-String -Path E:\DEV\PhoenixPoint\Renderforge\src\GraphicsPanel.cs -Pattern 'RenderforgeMod.Available'
```

Expected: no output (the availability decision now lives in `Availability.Reason`).

- [ ] **Step 5: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 6: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\Pickers.cs src\GraphicsPanel.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(ui): Renderer/Upscaler/Frame-generation pickers with greyed entries and native tooltips"
```

---

### Task 7: D3D12 PPv2 fix

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\src\D3D12Fix.cs`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Patches.cs:22`

- [ ] **Step 1: Write `D3D12Fix.cs`**

```csharp
using System;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

namespace Renderforge
{
    /// <summary>Unity 2019.4's D3D12 backend ships no kernels for two PPv2 compute shaders:
    /// MultiScaleVODownsample1 (AO in MSVO mode, AmbientOcclusion.cs:94) and KGenLut3D_AcesTonemap
    /// (HDR ColorGrading 3D-LUT baker, ColorGradingRenderer.cs:80). Either one throws every frame and
    /// PostProcessLayer aborts, which ALSO drops FogOfWarPostProcess and grading -> washed-out, fully lit scene.
    /// Fix: take AO off the compute path (SAO is a pixel shader, AmbientOcclusion.cs:82-88) or disable it, and
    /// null PostProcessResources.computeShaders.lut3DBaker so ColorGrading takes the LDR 2D-LUT branch
    /// (ColorGradingRenderer.cs:33). Resources are reached through PostProcessLayer's private m_Resources
    /// (PostProcessLayer.cs:55, handed to the context at :624).</summary>
    internal static class D3D12Fix
    {
        /// <summary>false = AO switched to ScalableAmbientObscurance (kept); true = AO disabled entirely.
        /// The in-game test in Task 11 decides which one ships.</summary>
        internal static bool DisableAo;

        private static bool loggedError;

        internal static bool Active
        {
            get { return SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12; }
        }

        internal static void Apply()
        {
            if (!Active) return;
            try
            {
                foreach (var layer in UnityEngine.Object.FindObjectsOfType<PostProcessLayer>())
                {
                    var res = Traverse.Create(layer).Field("m_Resources").GetValue<PostProcessResources>();
                    if (res != null && res.computeShaders != null) res.computeShaders.lut3DBaker = null;
                }
                foreach (var volume in UnityEngine.Object.FindObjectsOfType<PostProcessVolume>())
                    FixAo(volume.profile);
            }
            catch (Exception ex)
            {
                if (!loggedError && RenderforgeMod.Instance != null)
                    RenderforgeMod.Instance.Logger.LogError("Renderforge D3D12 fix failed: " + ex);
                loggedError = true;
            }
        }

        private static void FixAo(PostProcessProfile profile)
        {
            AmbientOcclusion ao;
            if (profile == null || !profile.TryGetSettings(out ao) || ao == null) return;
            if (DisableAo) ao.enabled.value = false;
            else ao.mode.Override(AmbientOcclusionMode.ScalableAmbientObscurance);
        }

        /// <summary>PPCLI switch for the SAO-vs-off test:
        /// {"op":"invoke","type":"Renderforge.D3D12Fix","assembly":"Renderforge","member":"SetAo","args":["off"]}</summary>
        public static string SetAo(string mode)
        {
            DisableAo = string.Equals(mode, "off", StringComparison.OrdinalIgnoreCase);
            Apply();
            return "d3d12=" + Active + " ao=" + (DisableAo ? "off" : "sao");
        }
    }
}
```

- [ ] **Step 2: Hook it into the existing `LightingManager` postfix**

In `src\Patches.cs`, replace line 22:

```csharp
        static void Postfix() => DlssDriver.Instance?.AfterApplyPostProcessOptions();
```

with:

```csharp
        static void Postfix()
        {
            DlssDriver.Instance?.AfterApplyPostProcessOptions();
            D3D12Fix.Apply();          // AO + lut3DBaker repair; no-op unless the process runs D3D12
        }
```

- [ ] **Step 3: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 4: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\D3D12Fix.cs src\Patches.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "fix(d3d12): PPv2 repair - AO off the compute path + lut3DBaker null (LDR LUT branch)"
```

---

### Task 8: Mod gate — stay alive under D3D12

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\RenderforgeMod.cs:26-59` (`OnModEnabled`), `:78` (`OnLevelStart`), `:140-150` (`AttachAndApply`)

- [ ] **Step 1: Replace `OnModEnabled`**

In `src\RenderforgeMod.cs`, replace the whole `OnModEnabled` method (currently lines 26-59) with:

```csharp
        public override void OnModEnabled()
        {
            Instance = this;
            ModDir = base.Instance?.Entry?.Directory ?? ".";
            Available = false;
            ApplyFrameRate();
            // D3D11: the NGX path as before. D3D12: the mod stays alive for the pickers, the overlay and the
            // PPv2 repair, but the native DLSS init is skipped (D3D12 upscaling is Phase 2).
            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Direct3D11)
            {
                try
                {
                    if (!Native.Load(ModDir))
                    {
                        InitCode = Native.DLSS_ERR_INIT_FAILED;
                        Logger.LogInfo("DLSS unavailable (code " + InitCode + "): RenderforgeNative.dll failed to load from " + ModDir);
                    }
                    else
                    {
                        probeTex = new Texture2D(1, 1, TextureFormat.RGBA32, false);
                        InitCode = Native.Init(probeTex.GetNativeTexturePtr(), ModDir, ModDir);
                        Available = InitCode == Native.DLSS_OK;
                    }
                }
                catch (Exception ex)
                {
                    InitCode = Native.DLSS_ERR_INIT_FAILED;
                    Logger.LogError("Renderforge init THREW " + ex.Message);
                }
                Logger.LogInfo(Available ? "DLSS available" : "DLSS unavailable (code " + InitCode + "): " + Reason(InitCode));
            }
            else
            {
                InitCode = Native.DLSS_ERR_NOT_AVAILABLE;
                Logger.LogInfo("Renderforge: " + SystemInfo.graphicsDeviceType + " - native DLSS init skipped ("
                               + Availability.Reason(Feature.Dlss) + ")");
            }
            try
            {
                if (Available) DlssDriver.Create();
                ((Harmony)HarmonyInstance).PatchAll(typeof(RenderforgeMod).Assembly);
                patched = true;
                if (RendererSwitch.Wants12(Cfg) && !Availability.IsD3D12) RendererSwitch.ArmStartupPrompt();
                AttachAndApply();
            }
            catch (Exception ex) { Logger.LogError("Renderforge enable THREW " + ex); }
        }
```

- [ ] **Step 2: Apply the D3D12 fix on every level start too**

Replace line 78:

```csharp
        public override void OnLevelStart(Level level) { AttachAndApply(); MipBias.Reapply(); }   // Reapply covers a level that starts with the generation still live
```

with:

```csharp
        public override void OnLevelStart(Level level) { AttachAndApply(); MipBias.Reapply(); D3D12Fix.Apply(); }   // Reapply covers a level that starts with the generation still live
```

- [ ] **Step 3: Show the overlay even without a driver**

Replace `AttachAndApply` (currently lines 140-150) with:

```csharp
        private void AttachAndApply()
        {
            Overlay.Apply(Cfg);               // before the driver check: under D3D12 there is no driver at all
            var d = DlssDriver.Instance;
            if (d == null) return;
            if (Cfg.Mode != RenderforgeMode.Off) lastOn = Cfg.Mode;
            var cam = GameUtl.GameComponent<CameraManager>()?.Camera;
            if (cam == null) return;          // main menu without CameraManager: wait for the next level
            d.Attach(cam);
            d.Apply(Cfg.Mode, Cfg.DebugView);
        }
```

- [ ] **Step 4: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 5: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\RenderforgeMod.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat: keep the mod active under D3D12 (pickers, overlay, PPv2 fix) with the NGX init skipped"
```

---

### Task 9: Overlay — renderer line and the frame-gen seam

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Overlay.cs:15-16` (statics), `:144-150` (text build)

- [ ] **Step 1: Add the `FgFps` field**

In `src\Overlay.cs`, replace lines 15-16:

```csharp
        private static Overlay inst;
        private static string upscaler;
```

with:

```csharp
        private static Overlay inst;
        private static string upscaler;

        /// <summary>Presented (frame-generated) fps. 0 = frame generation off, which is every Phase-1 build;
        /// Phase 5's FG provider writes it and the FPS line turns into "real / presented".</summary>
        public static int FgFps;
```

- [ ] **Step 2: Replace the text build**

Replace lines 144-150 (from `float avg = dtSum / dts.Count;` through the `box.sizeDelta` assignment) with:

```csharp
            float avg = dtSum / dts.Count;
            string dlssReason = Availability.Reason(Feature.Dlss);
            string fps = "FPS: " + Mathf.RoundToInt(1f / avg)
                       + (FgFps > 0 ? " / " + FgFps : "")
                       + " (" + (avg * 1000f).ToString("F1") + " ms)";
            text.text = "Renderer: " + Availability.ApiName
                      + "\nUpscaler: " + (live ? upscaler : "off" + (dlssReason != null ? " (" + dlssReason + ")" : ""))
                      + "\nMode: " + mode
                      + "\nAA: " + aa
                      + "\n" + fps;
            box.sizeDelta = new Vector2(text.preferredWidth + 2f * Pad, text.preferredHeight + 2f * Pad);
```

- [ ] **Step 3: Build**

Run:

```powershell
dotnet build E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 4: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add src\Overlay.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(overlay): Renderer line + FgFps seam for real/presented fps"
```

---

### Task 10: In-game test A — D3D11 baseline (overlay, rows, grey, tooltip)

**Files:**
- No source changes. Uses `deploy.ps1`, `PPCLI\ppcli.ps1`.

- [ ] **Step 1: Deploy the managed build to Instance2**

Run:

```powershell
E:\DEV\PhoenixPoint\Renderforge\deploy.ps1 -SkipNative
```

Expected: `Deployed Renderforge to D:\PP-Instance2\Mods\Renderforge` followed by the file table (`Renderforge.dll`, `RenderforgeNative.dll`, `nvngx_dlss.dll`, …). If it prints `REFUSED: ... has Phoenix Point running`, close that process first.

- [ ] **Step 2: Clear the log and launch D3D11**

```powershell
$log = "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log"
Remove-Item $log -ErrorAction SilentlyContinue
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods'
```

Expected: the game window appears. Wait for the main menu.

- [ ] **Step 3: Wait until the bridge answers, then turn the overlay on**

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"ToggleOverlay"}'
```

Expected: `state` returns a JSON object (do not send anything before it does); the invoke returns `overlay=True at TopCenter`.

- [ ] **Step 4: Screenshot the overlay**

```powershell
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-1-d3d11-overlay.png"}'
```

Expected: `{ok:true,path:...}`. Open the PNG and confirm the first line reads `Renderer: D3D11` and the FPS line has the form `FPS: <n> (<ms> ms)` with no `/`.

- [ ] **Step 5: Verify the restart command line WITHOUT restarting**

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RendererSwitch","assembly":"Renderforge","member":"Preview","args":[true]}'
```

Expected: a string ending in `PhoenixPointWin64.exe -mods -force-d3d12` (the `-mods` from the current launch is preserved, no duplicate renderer flag).

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RendererSwitch","assembly":"Renderforge","member":"Preview","args":[false]}'
```

Expected: the same line **without** any `-force-d3d1*` flag.

- [ ] **Step 6: Open Options → Graphics and screenshot the rows**

In the game window: main menu → OPTIONS → GRAPHICS. Then:

```powershell
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-2-d3d11-rows.png"}'
```

Expected in the PNG, directly under TEXTURE QUALITY, in this order: `RENDERER  DirectX 11`, `UPSCALER  Off`, `FRAME GENERATION  Off`, `DLSS QUALITY  <mode>`, `SHARPNESS <n>`.

- [ ] **Step 7: Select FSR in the UPSCALER row and confirm it renders greyed**

Click the UPSCALER row's right arrow twice (Off → DLSS → FSR), then:

```powershell
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-3-d3d11-fsr-greyed.png"}'
```

Expected: the value text `FSR` is visibly dimmed (alpha 0.35) while the row's title stays bright.

- [ ] **Step 8: Hover the greyed row and screenshot the native tooltip**

Move the OS cursor onto the UPSCALER row's value (read the pixel coordinates off the previous screenshot; the game reads the OS cursor through `InputController.GetCursorPosition`), wait out the 0.3 s appear delay, then shoot:

```powershell
Add-Type -Name W -Namespace Win -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);'
[Win.W]::SetCursorPos(<x>, <y>)      # coordinates read off p1-3-d3d11-fsr-greyed.png
Start-Sleep -Milliseconds 900
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-4-d3d11-tooltip.png"}'
```

Expected: the game's own tooltip box next to the row reading `Requires DirectX 12 — switch Renderer`.

- [ ] **Step 9: Same for FRAME GENERATION**

Click the FRAME GENERATION row's right arrow once (Off → 2x), hover it the same way and shoot:

```powershell
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-5-d3d11-fg-tooltip.png"}'
```

Expected: `2x` dimmed, tooltip `Requires DirectX 12 — switch Renderer`.

- [ ] **Step 10: Check the log is clean**

```powershell
Select-String -Path "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log" -Pattern 'Renderforge' | Select-Object -Last 10
```

Expected: `DLSS available` and no `Renderforge ... failed` / `THREW` lines.

- [ ] **Step 11: Commit the shots**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add docs\shots
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test(d3d11): overlay renderer line, three new rows, greyed FSR/FG entries with native tooltips"
```

---

### Task 11: In-game test B — switch to DirectX 12 through the picker

**Files:**
- No source changes.

- [ ] **Step 1: With the game still on the Graphics panel, set the UPSCALER row back to Off and the FRAME GENERATION row back to Off**

Click the left arrows until both read `Off`. (Neither wrote anything to the config; this only tidies the screen.)

- [ ] **Step 2: Set RENDERER to DirectX 12 and press APPLY**

Click the RENDERER row's right arrow once — the value must read `DirectX 12 (experimental) (restart pending)` and the panel's APPLY button must become active. Press APPLY.

Expected: the game's own dialog appears with `Restart required to switch renderer. Restart now?` and YES / NO buttons.

- [ ] **Step 3: Press NO and screenshot**

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-6-restart-dialog-no.png"}'
```

Expected: the dialog is gone and the RENDERER row still reads `DirectX 12 (experimental) (restart pending)`.

- [ ] **Step 4: Confirm the choice was persisted**

```powershell
Get-Content 'D:\PP-Instance2\Mods\Renderforge\ModConfig.json' -ErrorAction SilentlyContinue
Get-ChildItem 'D:\PP-Instance2' -Recurse -Filter 'ModConfig.json' | Select-Object FullName
```

Expected: a `ModConfig.json` containing `"Renderer": "DirectX12"` (note its path — later steps read the same file).

- [ ] **Step 5: Press APPLY again, then YES**

Reopen the Graphics panel if it closed, toggle RENDERER to DirectX 11 and back to DirectX 12 so APPLY lights up, press APPLY, then press YES in the dialog.

Expected: the running process exits and a new Phoenix Point process starts by itself.

- [ ] **Step 6: Verify the new process really runs D3D12**

```powershell
Get-CimInstance Win32_Process -Filter "Name='PhoenixPointWin64.exe'" | Select-Object ProcessId, CommandLine
Select-String -Path "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log" -Pattern 'Forcing GfxDevice|Direct3D 12' | Select-Object -First 3
```

Expected: the command line contains `-mods -force-d3d12`, and the log contains `Forcing GfxDevice: Direct3D 12` and `Version: Direct3D 12 [level 12.1]`.

- [ ] **Step 7: Confirm the overlay agrees**

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-7-d3d12-overlay.png"}'
```

Expected: the overlay's first line reads `Renderer: D3D12` and the second reads `Upscaler: off (DLSS on D3D12 comes in Phase 2)`.

- [ ] **Step 8: Commit the shots**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add docs\shots
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test(d3d12): picker -> native dialog -> relaunch with -force-d3d12 verified"
```

---

### Task 12: In-game test C — D3D12 mission, and the SAO-vs-off decision

**Files:**
- Possibly modify: `E:\DEV\PhoenixPoint\Renderforge\src\D3D12Fix.cs:22` (`DisableAo` default)

- [ ] **Step 1: Start a tactical mission on the running D3D12 process**

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
```

Expected: an `ok:true` JSON with a per-faction actor census, ~12 s.

- [ ] **Step 2: Screenshot with the default fix (AO = ScalableAmbientObscurance)**

```powershell
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-8-d3d12-sao.png"}'
```

Expected: the cave is dark with visible fog of war — the same look as the D3D11 reference, NOT washed out / fully lit.

- [ ] **Step 3: Count the shader errors with SAO**

```powershell
$log = "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log"
(Select-String -Path $log -Pattern 'Kernel').Count
(Select-String -Path $log -Pattern 'Kernel' | ForEach-Object { $_.Line } | Sort-Object -Unique)
```

Expected for a PASS: `0` and no lines.

- [ ] **Step 4: If step 2 or 3 failed, switch to AO off and re-measure**

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.D3D12Fix","assembly":"Renderforge","member":"SetAo","args":["off"]}'
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-9-d3d12-ao-off.png"}'
(Select-String -Path "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log" -Pattern 'Kernel').Count
```

Expected: dark cave + fog of war, and no NEW `Kernel` lines after the switch.

- [ ] **Step 5: Lock in whichever mode passed**

If SAO passed, leave `src\D3D12Fix.cs` as it is. If AO-off was needed, change the field declaration in `src\D3D12Fix.cs` from:

```csharp
        internal static bool DisableAo;
```

to:

```csharp
        // In-game 2026-09-02: ScalableAmbientObscurance still errored under D3D12, so AO is off by default.
        internal static bool DisableAo = true;
```

then rebuild and redeploy:

```powershell
E:\DEV\PhoenixPoint\Renderforge\deploy.ps1 -SkipNative -AllowRunning
```

Expected: `Deployed Renderforge to D:\PP-Instance2\Mods\Renderforge` (staged for the next launch).

- [ ] **Step 6: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add docs\shots src\D3D12Fix.cs
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test(d3d12): tactical mission renders correctly, zero Kernel errors after the PPv2 fix"
```

---

### Task 13: In-game test D — D3D12 stability gate

**Files:**
- No source changes.

- [ ] **Step 1: If Task 12 step 5 changed the code, relaunch into D3D12**

```powershell
Get-Process PhoenixPointWin64 -ErrorAction SilentlyContinue | Stop-Process
Remove-Item "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log" -ErrorAction SilentlyContinue
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods','-force-d3d12'
```

Expected: the game reaches the main menu. (Skip this step if the code did not change.)

- [ ] **Step 2: Load mission 1 and idle 15 minutes in tactical**

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
Start-Sleep -Seconds 900
Get-Process PhoenixPointWin64 -ErrorAction SilentlyContinue | Select-Object Id, StartTime
```

Expected: the process is still listed with the same `Id` and its original `StartTime`.

- [ ] **Step 3: Load two more missions**

```powershell
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":777}'
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":31337}'
Get-Process PhoenixPointWin64 -ErrorAction SilentlyContinue | Select-Object Id, StartTime
```

Expected: both plans return `ok:true`, and the process id is unchanged from step 2 — three mission loads total, no crash.

- [ ] **Step 4: Final screenshot + error sweep**

```powershell
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-10-d3d12-stability.png"}'
$log = "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log"
(Select-String -Path $log -Pattern 'Kernel').Count
Select-String -Path $log -Pattern 'Exception|error' | ForEach-Object { $_.Line } | Sort-Object -Unique | Select-Object -First 20
```

Expected: `Kernel` count `0`; the unique-line list may still contain the known `Mesh can not have more than 65000 vertices` warning — record the exact remaining lines in the commit message; anything per-frame is a FAIL and must be reported before the next task.

- [ ] **Step 5: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add docs\shots
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test(d3d12): stability gate - 15 min tactical idle + 3 mission loads, process alive"
```

---

### Task 14: In-game test E — switch back to DirectX 11 through the picker

**Files:**
- No source changes.

- [ ] **Step 1: From the running D3D12 process, open Options → Graphics**

Press ESC in the game window, open OPTIONS → GRAPHICS. The RENDERER row must read `DirectX 12 (experimental)` with no `(restart pending)` suffix (config and process agree).

- [ ] **Step 2: Select DirectX 11, press APPLY, press YES**

Expected: the dialog `Restart required to switch renderer. Restart now?` appears; YES exits the process and starts a new one.

- [ ] **Step 3: Verify the new process runs D3D11 with no renderer flag**

```powershell
Get-CimInstance Win32_Process -Filter "Name='PhoenixPointWin64.exe'" | Select-Object ProcessId, CommandLine
Select-String -Path "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log" -Pattern 'Forcing GfxDevice|Direct3D 11' | Select-Object -First 3
```

Expected: the command line contains `-mods` and **no** `-force-d3d12`; the log shows a Direct3D 11 device and no `Forcing GfxDevice: Direct3D 12`.

- [ ] **Step 4: Confirm the overlay and the config agree**

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-11-back-to-d3d11.png"}'
```

Expected: overlay line 1 reads `Renderer: D3D11`, line 2 reads `Upscaler: off` or the live DLSS string.

- [ ] **Step 5: Verify the startup prompt does NOT fire**

The config now says DirectX 11 and the process runs D3D11, so no dialog may appear at startup. Confirm nothing was logged:

```powershell
Select-String -Path "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log" -Pattern 'Renderforge restart'
```

Expected: no output.

- [ ] **Step 6: Verify the startup prompt DOES fire in the mismatch case**

```powershell
Get-Process PhoenixPointWin64 -ErrorAction SilentlyContinue | Stop-Process
# set the config to DirectX12 while launching WITHOUT the flag (this is the plain-Steam-launch case)
$cfg = (Get-ChildItem 'D:\PP-Instance2' -Recurse -Filter 'ModConfig.json' | Where-Object { (Get-Content $_.FullName -Raw) -match 'Renderer' } | Select-Object -First 1).FullName
(Get-Content $cfg -Raw) -replace '"Renderer":\s*"DirectX11"', '"Renderer": "DirectX12"' | Set-Content $cfg
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods'
```

Expected: once the main menu is up, the `Restart required to switch renderer. Restart now?` dialog appears by itself, exactly once. Press NO, then:

```powershell
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\p1-12-startup-prompt.png"}'
```

Expected: no second dialog appears for the rest of the session.

- [ ] **Step 7: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add docs\shots
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test: switch back to DirectX 11 via the picker; startup prompt fires once on mismatch"
```

---

### Task 15: Documentation

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md` (new section before `## Idea backlog`, currently line 316)
- Modify: `E:\DEV\PhoenixPoint\Renderforge\README.md` (new subsection under `## Settings`, currently line 70)

- [ ] **Step 1: Add the DESIGN.md section**

Insert immediately before the `## Idea backlog (user, not scheduled)` heading:

```markdown
## Renderer switch (Phase 1, 2026-09-02)

- Config `DlssConfig.Renderer` (`RendererMode { Auto, DirectX11, DirectX12 }`, `Auto == DirectX11`) is the
  desired API; `SystemInfo.graphicsDeviceType` is the running one. They differ only until the next launch.
- UI: three cloned `ArrowPickerController` rows under TEXTURE QUALITY, built by `src\Pickers.cs` —
  RENDERER, UPSCALER (Off/DLSS/FSR/XeSS), FRAME GENERATION (Off/2x/3x/4x). `GraphicsPanel` then places
  its DLSS QUALITY row and the SHARPNESS slider after them, so the order lives in one place.
- RENDERER is deferred like the panel's own settings: the Harmony postfixes on
  `UIModuleGraphicsOptionsPanel.HasChanges` (`:124`) and `Apply` (`:137`) light and commit it. On Apply,
  `RendererSwitch.Confirm` shows the GAME'S dialog (`GameUtl.GetMessageBox().ShowSimplePrompt`,
  `MessageBox.cs:77`, `MessageBoxButtons.YesNo`). Yes → `Process.Start(argv[0], current command line minus
  any -force-d3d1*, plus -force-d3d12 when DX12)` then `Application.Quit()`. No → the row keeps the value
  and shows "(restart pending)".
- A plain Steam launch cannot carry the flag, so when the config says DirectX 12 and the process runs
  D3D11, `RendererSwitch.ArmStartupPrompt` offers the same dialog ONCE per session, as soon as the
  MessageBox exists. The permanent alternative is Steam → Properties → Launch Options `-force-d3d12 -mods`.
- Availability: `src\Availability.cs` is the ONLY place that decides whether DLSS/FSR/XeSS/FG can run and
  why not. `Reason(f) == null` means available; anything else is shown as the greyed value's tooltip
  (`UITooltipText`, the game's own component — never a custom overlay) and, for DLSS, in the overlay.
- D3D12 PPv2 repair: `src\D3D12Fix.cs`, driven by the existing `LightingManager.ApplyPostProcessOptions`
  postfix and by `OnLevelStart`. It nulls `PostProcessResources.computeShaders.lut3DBaker` (reached through
  `PostProcessLayer`'s private `m_Resources`, `PostProcessLayer.cs:55`) so ColorGrading takes the LDR 2D-LUT
  branch, and takes AO off the compute path (`ScalableAmbientObscurance`, or `enabled=false` — see
  `D3D12Fix.DisableAo`). Cost: the ACES HDR grading path is unavailable under D3D12.
- Under D3D12 the mod stays fully active (pickers, overlay, PPv2 fix) but the NGX init is skipped; the
  overlay says `Upscaler: off (DLSS on D3D12 comes in Phase 2)`.
```

- [ ] **Step 2: Add the README paragraph**

Insert at the start of the `## Settings` section (immediately after the `## Settings` heading line):

```markdown
### Renderer

`Renderer` (Options → Graphics, or the mod's settings) picks the graphics API: `DirectX 11` (default) or
`DirectX 12 (experimental)`. DirectX 12 is what FSR, XeSS and frame generation will need — those entries
stay greyed on DirectX 11 and tell you so when you hover them.

Changing it needs a restart, because Unity picks the API from the command line. Press APPLY and answer
`Yes` and the game relaunches itself with `-force-d3d12` added to whatever it was started with. If you
launch from Steam instead, the mod offers the same restart once per session — or set it permanently in
Steam → Library → right-click Phoenix Point → Properties → Launch Options:

```
-force-d3d12 -mods
```

DirectX 12 is experimental: expect the ACES HDR colour grading to fall back to the LDR path, and report
any crash with your `Player.log`.
```

- [ ] **Step 3: Verify both files still render as intended**

```powershell
Select-String -Path E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md -Pattern '^## ' | ForEach-Object { "$($_.LineNumber): $($_.Line)" }
Select-String -Path E:\DEV\PhoenixPoint\Renderforge\README.md -Pattern '^#{2,3} ' | ForEach-Object { "$($_.LineNumber): $($_.Line)" }
```

Expected: `## Renderer switch (Phase 1, 2026-09-02)` appears before `## Idea backlog`, and `### Renderer` appears directly under `## Settings`.

- [ ] **Step 4: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add docs\DESIGN.md README.md
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "docs: renderer switch, availability model and the D3D12 PPv2 repair"
```

---

## Phase 1 exit criteria (all must be green before Phase 2)

- [ ] Overlay line 1 shows the running API under both renderers (screenshots `p1-1`, `p1-7`, `p1-11`).
- [ ] RENDERER / UPSCALER / FRAME GENERATION rows appear under TEXTURE QUALITY in that order.
- [ ] Unavailable entries render at alpha 0.35 and hovering them shows the game's own tooltip with the reason.
- [ ] Apply on a changed RENDERER shows the game's dialog; Yes relaunches into the chosen API, No leaves "(restart pending)".
- [ ] A D3D12 tactical mission looks like the D3D11 one (dark cave + fog of war) and `Player.log` has zero `Kernel` errors.
- [ ] 15-minute tactical idle + 3 mission loads under D3D12 with the process alive.
- [ ] `docs\DESIGN.md` and `README.md` describe the switch, including the Steam launch-option alternative.
