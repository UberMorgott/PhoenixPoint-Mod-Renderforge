using System;
using HarmonyLib;
using I2.Loc;
using PhoenixPoint.Common.View.ViewModules;
using PhoenixPoint.Geoscape.View.ViewControllers;
using PhoenixPoint.Modding;
using UnityEngine;
using UnityEngine.UI;

namespace DlssMod
{
    /// <summary>Native "DLSS" picker in the game's Graphics options panel: a clone of the TextureQuality
    /// ArrowPickerController (UIModuleGraphicsOptionsPanel.cs:25), rebuilt/re-synced by the panel's own
    /// parameterless Init() (:86, called by Init(bool, Action) :69 on every options open). Immediate apply
    /// (no Apply-button flow: the panel's _onChanged/HasChanges track only the graphics preset).</summary>
    [HarmonyPatch(typeof(UIModuleGraphicsOptionsPanel), "Init", new Type[0])]
    internal static class GraphicsPanel
    {
        private const string Name = "DlssPicker";
        private static readonly string[] Labels = { "Off", "Auto", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
        private static bool loggedError;

        static void Postfix(UIModuleGraphicsOptionsPanel __instance)
        {
            try
            {
                var mod = DlssMod.Instance;
                var src = __instance.TextureQualityPicker;
                if (mod == null || src == null) return;
                var existing = src.transform.parent.Find(Name);
                if (!DlssMod.Available || !mod.Cfg.ShowInGraphicsOptions)
                {
                    if (existing != null) existing.gameObject.SetActive(false);
                    return;
                }
                ArrowPickerController picker;
                if (existing != null) picker = existing.GetComponent<ArrowPickerController>();
                else
                {
                    var go = UnityEngine.Object.Instantiate(src.gameObject, src.transform.parent);
                    go.name = Name;
                    go.transform.SetSiblingIndex(src.transform.GetSiblingIndex() + 1);
                    picker = go.GetComponent<ArrowPickerController>();
                    SetRaw(picker.Title, null, "DLSS");
                }
                picker.gameObject.SetActive(true);
                int idx = (int)mod.Cfg.Mode;
                if (idx < 0 || idx >= Labels.Length) idx = 0;
                picker.Init(Labels.Length, idx, i => OnChanged(picker, i));
                SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[idx]);
            }
            catch (Exception ex)
            {
                if (!loggedError) DlssMod.Instance?.Logger.LogError("DLSS graphics-panel picker failed: " + ex);
                loggedError = true;
            }
        }

        private static void OnChanged(ArrowPickerController picker, int i)
        {
            try
            {
                SetRaw(picker.CurrentItem, picker.CurrentItemText, Labels[i]);
                var mod = DlssMod.Instance;
                if (mod == null) return;
                DlssMod.SetMode(((DlssMode)i).ToString(), mod.Cfg.DebugView.ToString());
                ModManager.GetInstance().SaveModConfig();   // ModManager.cs:120 - the same path UIStateModManagment uses (:137)
            }
            catch (Exception ex)
            {
                if (!loggedError) DlssMod.Instance?.Logger.LogError("DLSS picker change failed: " + ex);
                loggedError = true;
            }
        }

        /// <summary>I2 Localize (Localize.cs:122 OnEnable / :336 SetTerm) overwrites the Text with a term lookup, and a
        /// missing term leaves the old text (:154,:169); so disable the bind and write the raw string.</summary>
        private static void SetRaw(Localize bind, Text text, string value)
        {
            if (bind != null)
            {
                bind.enabled = false;
                if (text == null) text = bind.GetComponent<Text>() ?? bind.GetComponentInChildren<Text>(true);
            }
            if (text != null) text.text = value;
        }
    }
}
