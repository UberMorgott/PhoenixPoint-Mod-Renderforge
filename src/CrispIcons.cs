using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    /// <summary>Trilinear + anisotropic sampling for UI sprites drawn smaller than their source. The UI canvases scale
    /// with a 3840x2160 reference (scaleFactor 0.6667 at 1440p), so every sprite is minified and the main atlas
    /// (13 mips, filterMode Bilinear) snaps between mip levels. Textures without mips are left alone - creating
    /// mipmapped copies needs readable textures, and Image cannot draw an RT-backed sprite.
    /// Eligibility: Image/RawImage under a root ScreenSpaceOverlay canvas, inactive panels included. The write is per
    /// texture, so a shared atlas also changes on world/camera canvases that use it - sampler state, not pixels.
    /// No periodic scan: a global Image sweep costs a ~27 ms frame, so the pass runs on level start, once more
    /// <see cref="DelaySeconds"/> later for lazily built UI, and when the knob turns on.</summary>
    internal static class CrispIcons
    {
        private struct Original { public FilterMode Filter; public int Aniso; }
        private const float DelaySeconds = 3f;   // ponytail: one delayed pass; a panel built later than this keeps bilinear until the next level
        /// <summary>Strong refs only for the level's lifetime: LevelEnd restores + clears so UnloadUnusedAssets can reclaim them.</summary>
        private static readonly Dictionary<Texture, Original> originals = new Dictionary<Texture, Original>();
        private static readonly HashSet<int> noMips = new HashSet<int>();   // instance ids, per sweep - never a texture ref
        private static bool active, logged;
        private static CrispIconsHost host;
        internal static bool Active => active;

        internal static void Apply(bool enabled)
        {
            if (enabled == active) return;
            active = enabled;
            if (enabled)
            {
                logged = false;
                var go = new GameObject("Renderforge crisp icons") { hideFlags = HideFlags.HideAndDontSave };
                UnityEngine.Object.DontDestroyOnLoad(go);
                host = go.AddComponent<CrispIconsHost>();
                Sweep();
            }
            else
            {
                if (host) UnityEngine.Object.Destroy(host.gameObject);
                host = null;
                Restore();
            }
        }

        internal static void Dispose() => Apply(false);

        /// <summary>OnLevelEnd: cancel the pending delayed pass, originals back + forget every texture, so the level's
        /// atlases are not pinned across the transition; the next OnLevelStart sweep re-applies.</summary>
        internal static void LevelEnd()
        {
            if (!active) return;
            if (host) host.StopAllCoroutines();
            Restore();
        }

        /// <summary>OnLevelStart + knob on: one pass now, one more after <see cref="DelaySeconds"/>.</summary>
        internal static void Sweep()
        {
            if (!active) return;
            Pass();
            if (!host) return;
            host.StopAllCoroutines();
            host.StartCoroutine(Delayed());
        }

        private static IEnumerator Delayed()
        {
            yield return new WaitForSecondsRealtime(DelaySeconds);
            Pass();
        }

        /// <summary>Root overlay canvases only - a handful of objects, not every Image in the scene.</summary>
        private static void Pass()
        {
            noMips.Clear();
            try
            {
                foreach (Canvas canvas in UnityEngine.Object.FindObjectsOfType<Canvas>())
                {
                    if (!canvas.isRootCanvas || canvas.renderMode != RenderMode.ScreenSpaceOverlay) continue;
                    foreach (Image image in canvas.GetComponentsInChildren<Image>(true)) Visit(image);
                    foreach (RawImage raw in canvas.GetComponentsInChildren<RawImage>(true)) Visit(raw);
                }
            }
            catch (Exception ex) { RenderforgeMod.Instance?.Logger.LogWarning("Crisp icons sweep failed: " + ex.Message); }
            if (!logged && originals.Count > 0)
            {
                logged = true;
                RenderforgeMod.Instance?.Logger.LogInfo("crisp icons: " + originals.Count + " textures trilinear (" + noMips.Count + " skipped: no mips)");
            }
        }

        private static void Visit(Graphic graphic)
        {
            Texture texture = graphic.mainTexture;
            if (texture == null || originals.ContainsKey(texture) || noMips.Contains(texture.GetInstanceID())) return;
            if (texture.mipmapCount <= 1) { noMips.Add(texture.GetInstanceID()); return; }
            originals[texture] = new Original { Filter = texture.filterMode, Aniso = texture.anisoLevel };
            texture.filterMode = FilterMode.Trilinear;
            texture.anisoLevel = 8;
        }

        /// <summary>Originals back on every recorded texture that is still alive; no-op when nothing was recorded.</summary>
        private static void Restore()
        {
            foreach (KeyValuePair<Texture, Original> pair in originals)
            {
                if (!pair.Key) continue;
                pair.Key.filterMode = pair.Value.Filter;
                pair.Key.anisoLevel = pair.Value.Aniso;
            }
            originals.Clear();
            noMips.Clear();
        }

        /// <summary>PPCLI readback: {"op":"invoke","type":"Renderforge.CrispIcons","assembly":"Renderforge","member":"Status"}.</summary>
        public static string Status() => "crisp icons active=" + active + " trilinear=" + originals.Count + " noMips=" + noMips.Count;
    }

    /// <summary>Coroutine host for the delayed pass; created on Apply(true), destroyed on Apply(false).</summary>
    internal sealed class CrispIconsHost : MonoBehaviour { }
}
