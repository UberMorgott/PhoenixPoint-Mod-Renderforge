using System;
using HarmonyLib;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>"Frame rate limit" toggle + "Max FPS" slider in the game's Screen (video) options panel. Both rows are
    /// clones of the VSync row (prefab OptionsVSync_TextAndToggle: a row with a Toggle AND an inactive Slider child,
    /// so the slider row is the same prefab with the other child switched on). Rebuilt/re-synced by
    /// UIModuleVideoOptionsPanel.Init() (:76, called on every options open), found by name on reopen.
    /// Edits are PENDING like the panel's own: HasChanges (:103) lights the Apply button, Apply (:108) commits;
    /// closing without Apply discards them because Init re-reads the config.</summary>
    [HarmonyPatch(typeof(UIModuleVideoOptionsPanel))]
    internal static class VideoPanel
    {
        private const string ToggleName = "DlssFrameLimitToggle";
        private const string SliderName = "DlssFrameLimitSlider";
        private static bool loggedError;
        private static Toggle limit, vsync;
        private static Slider fps;
        private static Transform fpsValue;
        private static Action onChanged;
        private static bool pendingLimit;
        private static int pendingFps;

        [HarmonyPostfix, HarmonyPatch("Init", new Type[0])]
        static void Init(UIModuleVideoOptionsPanel __instance)
        {
            try
            {
                var mod = RenderforgeMod.Instance;
                vsync = __instance.VerticalSyncToggle;
                if (mod == null || vsync == null) return;
                var cfg = mod.Cfg;
                pendingLimit = cfg.LimitFrameRate;
                pendingFps = Mathf.Clamp(cfg.FrameRateLimit, 30, 300);
                onChanged = Traverse.Create(__instance).Field("_onChanged").GetValue<Action>();

                var row = vsync.transform.parent;          // OptionsVSync_TextAndToggle
                var content = row.parent;                  // VerticalLayoutGroup: clones just insert
                var rowA = content.Find(ToggleName) ?? Clone(row, ToggleName, row.GetSiblingIndex() + 1);
                var rowB = content.Find(SliderName) ?? Clone(row, SliderName, row.GetSiblingIndex() + 2);

                // Row prefab children (live dump 2026-09-02): "Slider" (inactive on the VSync row), "Keybinds" (inactive,
                // carries Localize texts of its own - never search by component), label "UITextGeneric_Medium (1)",
                // value readout "UITextGeneric_Medium" (inactive; the ShadowDistance row shows "150" in it),
                // toggle "OptionsVSync_UIButtonToggle".
                limit = rowA.GetComponentInChildren<Toggle>(true);
                fps = rowB.Find("Slider").GetComponent<Slider>();
                fpsValue = rowB.Find("UITextGeneric_Medium");
                fps.gameObject.SetActive(true);
                fpsValue.gameObject.SetActive(true);
                rowB.GetComponentInChildren<Toggle>(true).gameObject.SetActive(false);

                // The game's own row labels are upper-case strings, not a Text setting - match them.
                GraphicsPanel.SetRaw(rowA.Find("UITextGeneric_Medium (1)").GetComponent<Localize>(), null,
                    DlssConfig.Loc("Frame rate limit", "Ограничение частоты кадров").ToUpperInvariant());
                GraphicsPanel.SetRaw(rowB.Find("UITextGeneric_Medium (1)").GetComponent<Localize>(), null,
                    DlssConfig.Loc("Max FPS", "Макс. FPS").ToUpperInvariant());
                fps.wholeNumbers = true;
                fps.minValue = 30;
                fps.maxValue = 300;
                fps.SetValueWithoutNotify(pendingFps);
                SetSliderEnabled(pendingLimit);
                limit.SetIsOnWithoutNotify(pendingLimit);
                ShowFps();

                limit.onValueChanged.RemoveAllListeners();
                limit.onValueChanged.AddListener(OnLimit);
                fps.onValueChanged.RemoveAllListeners();
                fps.onValueChanged.AddListener(OnFps);
                vsync.onValueChanged.RemoveListener(OnVSync);   // ours only; the game's own listener stays
                vsync.onValueChanged.AddListener(OnVSync);
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge video-panel rows failed: " + ex);
                loggedError = true;
            }
        }

        [HarmonyPostfix, HarmonyPatch("HasChanges")]
        static void HasChanges(ref bool __result)
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg != null && limit != null) __result |= pendingLimit != cfg.LimitFrameRate || pendingFps != cfg.FrameRateLimit;
        }

        [HarmonyPostfix, HarmonyPatch("Apply")]
        static void Apply()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null || limit == null) return;
            if (pendingLimit == cfg.LimitFrameRate && pendingFps == cfg.FrameRateLimit) return;
            cfg.LimitFrameRate = pendingLimit;
            cfg.FrameRateLimit = pendingFps;
            RenderforgeMod.ApplyFrameRate();
            RenderforgeMod.SaveConfig();
        }

        private static Transform Clone(Transform row, string name, int index)
        {
            var go = UnityEngine.Object.Instantiate(row.gameObject, row.parent);
            go.name = name;
            go.transform.SetSiblingIndex(index);
            return go.transform;
        }

        // Limit on -> VSync off through the game's own listener (OnVSyncEnableToggle :119 updates _currentOptions + _onChanged).
        private static void OnLimit(bool on)
        {
            pendingLimit = on;
            SetSliderEnabled(on);
            if (on && vsync.isOn) vsync.isOn = false;
            onChanged?.Invoke();
        }

        // VSync on -> limit off (its listener greys the slider and reports the change).
        private static void OnVSync(bool on)
        {
            if (on && limit.isOn) limit.isOn = false;
        }

        private static void OnFps(float v)
        {
            pendingFps = (int)v;
            ShowFps();
            onChanged?.Invoke();
        }

        // Slider.interactable alone changes nothing visible here (the prefab's handle is inactive, so no tinted target
        // graphic); a CanvasGroup on the row's slider+readout is the grey.
        private static void SetSliderEnabled(bool on)
        {
            fps.interactable = on;
            foreach (var go in new[] { fps.gameObject, fpsValue.gameObject })
            {
                var cg = go.GetComponent<CanvasGroup>() ?? go.AddComponent<CanvasGroup>();
                cg.alpha = on ? 1f : 0.35f;
            }
        }

        private static void ShowFps() => GraphicsPanel.SetRaw(fpsValue.GetComponent<Localize>(), null, pendingFps.ToString());
    }
}
