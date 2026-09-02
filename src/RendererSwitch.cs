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
