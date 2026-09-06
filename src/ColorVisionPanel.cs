using System;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Live colour-vision (daltonization) picker in the game's native Graphics panel. Immediate apply +
    /// SaveConfig; the driver reads the mode every frame, so no feature teardown is needed.</summary>
    internal static class ColorVisionPanel
    {
        private const string PickerName = "RenderforgeColorVision";
        private static ArrowPickerController picker;
        private static bool loggedError;

        // Index == ColorVisionMode (declared None, Deuteranopia, Protanopia, Tritanopia).
        private static string[] Labels => new[] {
            DlssConfig.Loc("None", "Нет"),
            DlssConfig.Loc("Deuteranopia", "Дейтеранопия"),
            DlssConfig.Loc("Protanopia", "Протанопия"),
            DlssConfig.Loc("Tritanopia", "Тританопия") };

        /// <summary>The ONE activation gate. Both DlssDriver.Step (start the pipeline) and the per-frame
        /// submission (Dlss_SetColorVision) call this, so they cannot drift apart and leave an uncorrected
        /// passthrough pass running on the geoscape. Tactical-only, mirroring lutPreset in DlssDriver — the post
        /// pass exists on the tactical camera only. The UI row does not use this: the picker stays visible and
        /// settable everywhere.</summary>
        internal static bool Active(DlssConfig cfg) => RenderforgeMod.TacticalActive && cfg != null
            && cfg.ColorVision >= ColorVisionMode.Deuteranopia && cfg.ColorVision <= ColorVisionMode.Tritanopia;

        internal static Transform Build(UIModuleGraphicsOptionsPanel panel, Transform after, DlssConfig cfg)
        {
            var src = panel.TextureQualityPicker;
            if (src == null || after == null) return after;
            var content = after.parent;
            var found = content.Find(PickerName);
            if (found != null) picker = found.GetComponent<ArrowPickerController>();
            else
            {
                var go = UnityEngine.Object.Instantiate(src.gameObject, content);
                go.name = PickerName;
                picker = go.GetComponent<ArrowPickerController>();
                GraphicsPanel.SetRaw(picker.Title, null, DlssConfig.Loc("Colour vision", "Цветовое зрение").ToUpperInvariant());
            }
            picker.transform.SetSiblingIndex(after.GetSiblingIndex() + 1);
            picker.gameObject.SetActive(true);
            picker.Init(Labels.Length, Mathf.Clamp((int)cfg.ColorVision, 0, Labels.Length - 1), OnMode);
            Sync();
            return picker.transform;
        }

        /// <summary>Label AND CurrentIndex (Pickers.ReInit), so a PPCLI/console change cannot leave a stale index.</summary>
        internal static void Sync()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg == null || picker == null) return;
            int index = Mathf.Clamp((int)cfg.ColorVision, 0, Labels.Length - 1);
            Pickers.ReInit(picker, Labels.Length, index, OnMode);
            GraphicsPanel.SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[index]);
            GraphicsPanel.Grey(picker.CurrentItem.gameObject, false);
            GraphicsPanel.Tip(picker.CentralButton.gameObject, DlssConfig.Loc(
                "Redistributes the colours the eye cannot separate onto the channels it can, at full strength. Tactical missions only; scene only, the interface is drawn after this pass and is not corrected.",
                "Перераспределяет неразличимые глазом цвета на различимые каналы, в полную силу. Только тактические миссии; только сцена, интерфейс рисуется после этого прохода и не корректируется."));
        }

        internal static void Hide(Transform content)
        {
            if (content == null) return;
            var row = content.Find(PickerName);
            if (row != null) row.gameObject.SetActive(false);
        }

        internal static void Clear() { picker = null; }

        private static void OnMode(int index)
        {
            try
            {
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg == null) return;
                cfg.ColorVision = (ColorVisionMode)Mathf.Clamp(index, 0, Labels.Length - 1);
                RenderforgeMod.SaveConfig();   // the driver polls this every frame, like the scene style
                Sync();
            }
            catch (Exception ex)
            {
                if (!loggedError) RenderforgeMod.Instance?.Logger.LogError("Renderforge colour vision picker change failed: " + ex);
                loggedError = true;
            }
        }
    }
}
