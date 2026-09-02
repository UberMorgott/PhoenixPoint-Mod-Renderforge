using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering.PostProcessing;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Benchmark overlay: one ScreenSpaceOverlay canvas (sortingOrder 30000, no raycaster, nothing is a raycast
    /// target) with a translucent box and four lines refreshed 4x/s. Font = the first HUD Text's font, else Arial.</summary>
    public class Overlay : MonoBehaviour
    {
        private const float Margin = 6f, Pad = 6f, Refresh = 0.25f, Window = 0.5f;
        private static Overlay inst;
        private static string upscaler;

        /// <summary>Presented (frame-generated) fps. 0 = frame generation off, which is every Phase-1 build;
        /// Phase 5's FG provider writes it and the FPS line turns into "real / presented".</summary>
        public static int FgFps;

        private RectTransform box;
        private Text text;
        private OverlayCorner corner;
        private float scale = 1f;
        private int placedH;
        private float sinceRefresh;
        private readonly Queue<float> dts = new Queue<float>();
        private float dtSum;

        public static void Apply(DlssConfig cfg)
        {
            if (!cfg.ShowOverlay) { if (inst != null) inst.gameObject.SetActive(false); return; }
            if (inst == null) Create();
            inst.corner = cfg.OverlayPosition;
            inst.scale = Mathf.Clamp(cfg.OverlayScale, 0.5f, 3f);
            inst.placedH = 0;                // re-place on next Update
            inst.gameObject.SetActive(true);
        }

        public static void Destroy()
        {
            if (inst != null) UnityEngine.Object.Destroy(inst.gameObject);
            inst = null;
        }

        private static void Create()
        {
            var go = new GameObject("DlssOverlay") { hideFlags = HideFlags.HideAndDontSave };
            DontDestroyOnLoad(go);
            var canvas = go.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = 30000;
            inst = go.AddComponent<Overlay>();

            var boxGo = new GameObject("Box", typeof(RectTransform), typeof(Image));
            boxGo.transform.SetParent(go.transform, false);
            var img = boxGo.GetComponent<Image>();
            img.color = new Color(0f, 0f, 0f, 0.55f);
            img.raycastTarget = false;
            inst.box = boxGo.GetComponent<RectTransform>();

            var textGo = new GameObject("Text", typeof(RectTransform), typeof(Text));
            textGo.transform.SetParent(boxGo.transform, false);
            var rt = textGo.GetComponent<RectTransform>();
            rt.anchorMin = Vector2.zero; rt.anchorMax = Vector2.one;
            rt.offsetMin = new Vector2(Pad, Pad); rt.offsetMax = new Vector2(-Pad, -Pad);
            inst.text = textGo.GetComponent<Text>();
            inst.text.raycastTarget = false;
            inst.text.color = Color.white;
            inst.text.alignment = TextAnchor.UpperLeft;
            inst.text.horizontalOverflow = HorizontalWrapMode.Overflow;
            inst.text.verticalOverflow = VerticalWrapMode.Overflow;
            inst.text.font = HudFont();

            if (upscaler == null) upscaler = "DLSS SR (nvngx " + NativeFileVersion(Path.Combine(RenderforgeMod.ModDir, "nvngx_dlss.dll")) + ")";
        }

        // Mono's FileVersionInfo returns an empty FileVersion for a native DLL (seen live), so read VERSIONINFO via version.dll.
        [DllImport("version.dll", CharSet = CharSet.Unicode)] static extern int GetFileVersionInfoSizeW(string file, out int handle);
        [DllImport("version.dll", CharSet = CharSet.Unicode)] static extern bool GetFileVersionInfoW(string file, int handle, int len, byte[] data);
        [DllImport("version.dll", CharSet = CharSet.Unicode)] static extern bool VerQueryValueW(byte[] data, string sub, out System.IntPtr value, out uint len);

        private static string NativeFileVersion(string path)
        {
            try
            {
                int size = GetFileVersionInfoSizeW(path, out _);
                var data = new byte[size];
                if (size == 0 || !GetFileVersionInfoW(path, 0, size, data) || !VerQueryValueW(data, "\\", out var p, out _)) return "?";
                // VS_FIXEDFILEINFO: dwFileVersionMS at +8, dwFileVersionLS at +12.
                uint ms = (uint)Marshal.ReadInt32(p, 8), ls = (uint)Marshal.ReadInt32(p, 12);
                return (ms >> 16) + "." + (ms & 0xFFFF) + "." + (ls >> 16) + "." + (ls & 0xFFFF);
            }
            catch { return "?"; }
        }

        private static Font HudFont()
        {
            foreach (var t in Resources.FindObjectsOfTypeAll<Text>())
                if (t.font != null) return t.font;
            return Resources.GetBuiltinResource<Font>("Arial.ttf");
        }

        private void Place()
        {
            placedH = Screen.height;
            text.fontSize = Mathf.RoundToInt(14f * scale * Screen.height / 1080f);
            Vector2 a;
            switch (corner)
            {
                case OverlayCorner.TopLeft: a = new Vector2(0f, 1f); break;
                case OverlayCorner.TopRight: a = new Vector2(1f, 1f); break;
                case OverlayCorner.BottomCenter: a = new Vector2(0.5f, 0f); break;
                default: a = new Vector2(0.5f, 1f); break;
            }
            box.anchorMin = box.anchorMax = box.pivot = a;
            box.anchoredPosition = new Vector2(a.x == 0f ? Margin : a.x == 1f ? -Margin : 0f, a.y == 1f ? -Margin : Margin);
        }

        private void Update()
        {
            float dt = Time.unscaledDeltaTime;
            dts.Enqueue(dt); dtSum += dt;
            while (dtSum - dts.Peek() >= Window && dts.Count > 1) dtSum -= dts.Dequeue();
            sinceRefresh += dt;
            if (placedH != Screen.height) Place();
            if (sinceRefresh < Refresh && text.text.Length > 0) return;
            sinceRefresh = 0f;

            var d = DlssDriver.Instance;
            bool live = d != null && d.IsLive && !d.Passthrough;
            string mode, aa;
            if (live)
            {
                bool sameRes = d.RenderW == d.OutW && d.RenderH == d.OutH;   // DLAA: no upscale, show one resolution
                mode = d.LiveMode + (d.LiveMode == RenderforgeMode.Auto ? "/" + QualityName(d.Quality) : "")
                     + " (" + (sameRes ? "" : d.RenderW + "x" + d.RenderH + " -> ") + d.OutW + "x" + d.OutH + ")";
                aa = "DLSS";
            }
            else
            {
                mode = "Off";
                var l = d != null ? d.Layer : null;
                aa = l == null || l.antialiasingMode == PostProcessLayer.Antialiasing.None ? "none"
                   : l.antialiasingMode == PostProcessLayer.Antialiasing.SubpixelMorphologicalAntialiasing ? "SMAA" : l.antialiasingMode.ToString();
            }
            float avg = dtSum / dts.Count;
            string dlssReason = Availability.Reason(Feature.Dlss);
            string fps = "FPS: " + Mathf.RoundToInt(1f / avg)
                       + (FgFps > 0 ? " / " + FgFps : "")
                       + " (" + (avg * 1000f).ToString("F1") + " ms)";
            text.text = "Renderer: " + Availability.ApiName
                      + "\nUpscaler: " + (live ? upscaler
                                          : Availability.NeedsRestart ? "off (restart required)"
                                          : "off" + (dlssReason != null ? " (" + dlssReason + ")" : ""))
                      + "\nMode: " + mode
                      + "\nAA: " + aa
                      + "\n" + fps;
            box.sizeDelta = new Vector2(text.preferredWidth + 2f * Pad, text.preferredHeight + 2f * Pad);
        }

        private static string QualityName(int q)
        {
            switch (q)
            {
                case Native.DLSS_Q_QUALITY: return "Quality";
                case Native.DLSS_Q_BALANCED: return "Balanced";
                case Native.DLSS_Q_PERFORMANCE: return "Performance";
                case Native.DLSS_Q_ULTRA_PERFORMANCE: return "UltraPerformance";
                default: return "DLAA";
            }
        }
    }
}
