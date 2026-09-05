using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using UnityEngine.UI;

namespace Renderforge
{
    // The tested density2 coverage correction. Camera/patch compatibility policy belongs to CrispFonts.
    internal static class FontRasterCorrection
    {
        private sealed class Entry
        {
            internal Text Text;
            internal Font Font;
            internal TextGenerator High = new TextGenerator();
            internal string Content, Outcome;
            internal bool Pending, Retried;
        }
        private static readonly Dictionary<int, Entry> Entries = new Dictionary<int, Entry>();
        private static readonly HashSet<int> Retire = new HashSet<int>();
        private static readonly MethodInfo Populate = AccessTools.Method(typeof(Text), "OnPopulateMesh", new[] { typeof(VertexHelper) });
        private static bool processing, refreshQueued;
        private static int atlasSerial;
        internal static int CachedCount => Entries.Count;
        internal static string Result(int id) => Entries.TryGetValue(id, out Entry entry) ? entry.Outcome : "original";

        internal static void AtlasChanged(Font font)
        {
            atlasSerial++;
            foreach (Entry entry in Entries.Values)
                if (entry.Font == font)
                {
                    entry.High.Invalidate();
                    if (!entry.Retried) { entry.Pending = true; refreshQueued = true; }
                }
        }

        // Called by the host only to drain event-driven work; no per-frame layout invalidation.
        internal static void RefreshPending()
        {
            if (processing || !refreshQueued) return;
            refreshQueued = false;
            foreach (var pair in Entries.ToArray())
            {
                Entry entry = pair.Value;
                if (!entry.Text || !entry.Text.isActiveAndEnabled || entry.Text.font != entry.Font)
                { Remove(pair.Key); continue; }
                if (!entry.Pending || entry.Retried) continue;
                entry.Pending = false; entry.Retried = true;
                entry.Text.SetVerticesDirty();
            }
        }

        internal static void Release(Text text)
        {
            if (ReferenceEquals(text, null)) return;
            int id = text.GetInstanceID();
            if (processing) Retire.Add(id); else Remove(id);
        }
        internal static void FontChanged(Text text)
        {
            if (text && Entries.TryGetValue(text.GetInstanceID(), out Entry entry) && entry.Font != text.font)
                Release(text);
        }
        private static void Remove(int id)
        {
            if (!Entries.TryGetValue(id, out Entry entry)) return;
            ((IDisposable)entry.High).Dispose(); Entries.Remove(id);
        }
        internal static void Dispose()
        {
            foreach (Entry entry in Entries.Values) ((IDisposable)entry.High).Dispose();
            Entries.Clear(); Retire.Clear(); processing = false; refreshQueued = false;
        }

        private static UIVertex[] Read(VertexHelper mesh)
        {
            var result = new UIVertex[mesh.currentVertCount];
            for (int i = 0; i < result.Length; i++) mesh.PopulateUIVertex(ref result[i], i);
            return result;
        }
        private static void Fill(VertexHelper mesh, UIVertex[] vertices)
        {
            mesh.Clear(); var quad = new UIVertex[4];
            for (int i = 0; i < vertices.Length; i += 4)
            { Array.Copy(vertices, i, quad, 0, 4); mesh.AddUIVertexQuad(quad); }
        }
        private static void Original(Text text, VertexHelper mesh)
        {
            // The atlas can repack during Populate; copied old UVs are not a safe fallback.
            // Reentry skips correction while the installed original generates fresh normal-density UVs.
            Populate.Invoke(text, new object[] { mesh });
        }

        internal static string Apply(Text text, VertexHelper mesh, bool enabled)
        {
            if (processing || !enabled || !text || !text.font || !text.font.dynamic) return "original";
            float ppu = text.pixelsPerUnit;
            if (!(ppu > 0) || float.IsInfinity(ppu) || mesh.currentVertCount == 0) return "original";
            int id = text.GetInstanceID();
            if (Entries.TryGetValue(id, out Entry entry) && (entry.Text != text || entry.Font != text.font))
            { Remove(id); entry = null; }
            if (entry == null) Entries[id] = entry = new Entry { Text = text, Font = text.font };
            if (entry.Content != text.text)
            { entry.Content = text.text; entry.Retried = false; entry.Pending = false; }
            processing = true;
            int serial = atlasSerial;
            try
            {
                UIVertex[] baseline = Read(mesh);
                int indexCount = mesh.currentIndexCount;
                if (baseline.Length % 4 != 0 || indexCount != baseline.Length / 4 * 6)
                    return entry.Outcome = "fallback: unsupported topology";
                TextGenerator normal = text.cachedTextGenerator;
                int normalCharacters = normal.characters.Count, normalVisible = normal.characterCountVisible;
                int[] normalLines = normal.lines.Select(l => l.startCharIdx).ToArray();
                int normalSize = normal.fontSizeUsedForBestFit;
                var settings = text.GetGenerationSettings(text.rectTransform.rect.size);
                settings.scaleFactor *= 2;
                bool populated = entry.High.PopulateWithErrors(text.text, settings, text.gameObject);
                IList<UIVertex> high = entry.High.verts;
                string reason = !populated ? "native generation failure" : atlasSerial != serial ? "atlas rebuilt during generation"
                    : FontGlyphMapping.Check(baseline, indexCount, high, ppu, normalCharacters, entry.High.characters.Count,
                        normalVisible, entry.High.characterCountVisible, normalLines, entry.High.lines.Select(l => l.startCharIdx).ToArray(),
                        normalSize, entry.High.fontSizeUsedForBestFit);
                if (reason != null)
                {
                    Original(text, mesh);
                    if (atlasSerial != serial && !entry.Retried) { entry.Pending = true; refreshQueued = true; }
                    return entry.Outcome = "fallback: " + reason;
                }
                FontGlyphMapping.Transfer(baseline, high);
                Fill(mesh, baseline);
                return entry.Outcome = "corrected";
            }
            catch
            {
                // Original failures are allowed to reach the lifecycle wrapper's one-time logger.
                Original(text, mesh);
                return entry.Outcome = "fallback: correction exception";
            }
            finally
            {
                processing = false;
                foreach (int retired in Retire) Remove(retired);
                Retire.Clear();
            }
        }
    }
}
