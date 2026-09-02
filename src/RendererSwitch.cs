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
            return string.Join(" ", BuildArgs(Environment.GetCommandLineArgs(), want12));
        }

        /// <summary>argv[1..] with every Unity graphics-API force flag dropped, quoted, plus "-force-d3d12" when
        /// wanted. In Unity 2019.4 every "-force-*" arg is a gfx flag (-force-d3d11, -force-d3d12,
        /// -force-d3d12-debug, -force-d3d11-singlethreaded, -force-vulkan, -force-glcore, -force-glcore32..46,
        /// -force-gles*, -force-opengl, -force-clamped, -force-low-power-device), so a prefix match is exact.
        /// "-mods", "-logFile &lt;path&gt;" and everything else pass through untouched.</summary>
        internal static string[] BuildArgs(string[] argv, bool d3d12)
        {
            List<string> kept = new List<string>();
            for (int i = 1; i < argv.Length; i++)
            {
                string a = argv[i];
                if (a.StartsWith("-force-", StringComparison.OrdinalIgnoreCase)) continue;
                kept.Add(Quote(a));
            }
            if (d3d12) kept.Add(Flag12);
            return kept.ToArray();
        }

        [Conditional("DEBUG")]
        internal static void SelfTest()
        {
            string[] argv = { "exe", "-mods", "-FORCE-D3D11", "-force-glcore45", "-logFile", "C:\\a b\\log.txt", "-force-gles31" };
            string a11 = string.Join(" ", BuildArgs(argv, false));
            string a12 = string.Join(" ", BuildArgs(argv, true));
            Check(a11 == "-mods -logFile \"C:\\a b\\log.txt\"", a11);
            Check(a12 == "-mods -logFile \"C:\\a b\\log.txt\" -force-d3d12", a12);
            Check(BuildArgs(new[] { "exe", "-force-d3d12" }, true).Length == 1, "dup flag");
        }

        private static void Check(bool ok, string what)
        {
            if (!ok) throw new InvalidOperationException("RendererSwitch.SelfTest failed: " + what);
        }

        /// <summary>MSVC argv rules: quote on whitespace/quotes, embedded " becomes \", a trailing backslash
        /// is doubled so it cannot escape the closing quote.</summary>
        private static string Quote(string a)
        {
            if (a.IndexOf(' ') < 0 && a.IndexOf('"') < 0) return a;
            string s = a.Replace("\"", "\\\"");
            if (s.EndsWith("\\")) s += "\\";
            return "\"" + s + "\"";
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
            // In-game 2026-09-02: the game is single-instance ("Another instance is already running" fatal
            // error), so a relaunch started while this process is still shutting down dies at once. A hidden
            // Windows PowerShell waits for THIS pid to exit, then starts the new game (verified: new process
            // appears ~1 s after Application.Quit, no console flash with CreateNoWindow).
            string dir = Path.GetDirectoryName(exe);
            string start = "Start-Process -FilePath '" + exe + "' -WorkingDirectory '" + dir + "'"
                         + (args.Length > 0 ? " -ArgumentList '" + args + "'" : "");
            string script = "Wait-Process -Id " + Process.GetCurrentProcess().Id + " -ErrorAction SilentlyContinue; " + start;
            ProcessStartInfo psi = new ProcessStartInfo("powershell.exe",
                "-NoProfile -NonInteractive -WindowStyle Hidden -Command \"" + script + "\"");
            psi.WorkingDirectory = dir;
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
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
