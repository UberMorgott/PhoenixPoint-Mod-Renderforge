using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using HarmonyLib;
using Newtonsoft.Json;
using UnityEngine;
using UnityEngine.UI;
using Object = UnityEngine.Object;

// Only the diagnostic logger adapter differs; all lifecycle and correction source files are linked unchanged.
namespace Renderforge
{
    internal sealed class RenderforgeMod
    {
        internal static RenderforgeMod Instance => null;
        internal ProbeLogger Logger => null;
    }
    internal sealed class ProbeLogger { public void LogWarning(string message) { } }
}

namespace RenderforgeFontLifecycleDiagnostic
{
    public static class Probe
    {
        const string Owner = "renderforge.diagnostic.font-lifecycle", Foreign = Owner + ".foreign";
        const string Root = @"D:\RenderforgeWork\fonts-lifecycle-proof\";
        static readonly MethodInfo Populate = AccessTools.Method(typeof(Text), "OnPopulateMesh", new[] { typeof(VertexHelper) });
        static readonly MethodInfo Generate = AccessTools.Method(typeof(Graphic), "DoMeshGeneration");
        static readonly FieldInfo SharedMesh = AccessTools.Field(typeof(Graphic), "s_Mesh");
        static readonly FieldInfo RawMaterial = AccessTools.Field(typeof(Graphic), "m_Material");
        static readonly Dictionary<int, Text> Texts = new Dictionary<int, Text>();
        static readonly Dictionary<int, object> Meshes = new Dictionary<int, object>();
        static readonly Dictionary<int, int> Rebuilds = new Dictionary<int, int>();
        static readonly List<string> Errors = new List<string>();
        static Harmony observer, foreign;
        static GameObject watchdog;
        static Text edited;
        static Material originalMaterial, ownedMaterial;
        static int originalSize;
        static Vector2 originalRect;
        static bool running;
        static float deadline;
        static int atlasEvents;
        static string beforeConfig;
        static object V(Vector3 v) => new[] { v.x, v.y, v.z };
        static bool Visible(Text t) => Renderforge.CrispFonts.Supported(t) && !t.canvasRenderer.cull
            && t.canvasRenderer.GetAlpha() > 0 && t.canvasRenderer.GetInheritedAlpha() > 0 && !string.IsNullOrWhiteSpace(t.text);
        static string PathOf(Transform t) => t.parent ? PathOf(t.parent) + "/" + t.name : t.name;
        static string Save(string name, object value)
        {
            string path = System.IO.Path.Combine(Root, name + ".json");
            System.IO.File.WriteAllText(path, JsonConvert.SerializeObject(value, Formatting.Indented)); return path;
        }
        static void Need(bool value, string reason) { if (!value) throw new InvalidOperationException(reason); }
        static Type MainMod => Type.GetType("Renderforge.RenderforgeMod, Renderforge", true);
        static string Config()
        {
            object mod = MainMod.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly).GetValue(null, null);
            object cfg = MainMod.GetProperty("Cfg").GetValue(mod, null);
            return JsonConvert.SerializeObject(cfg.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly)
                .OrderBy(f => f.Name).ToDictionary(f => f.Name, f => f.GetValue(cfg)));
        }
        static object Status() => MainMod.GetMethod("GetStatus").Invoke(null, null);
        static void Observe(Graphic __instance)
        {
            var t = __instance as Text;
            if (!running || !t || !Texts.ContainsKey(t.GetInstanceID())) return;
            var m = (Mesh)SharedMesh.GetValue(null);
            if (!m) return;
            int id = t.GetInstanceID(); Rebuilds[id] = Rebuilds.TryGetValue(id, out int n) ? n + 1 : 1;
            Meshes[id] = new { frame = Time.frameCount, positions = m.vertices.Select(V).ToArray(),
                uv = m.uv.Select(v => new[] { v.x, v.y }).ToArray(), colors = m.colors32, triangles = m.triangles };
        }
        static void OnLog(string msg, string stack, LogType type)
        { if (type == LogType.Error || type == LogType.Exception || type == LogType.Assert) Errors.Add(msg); }
        static void OnAtlas(Font font) { atlasEvents++; }
        static void Dirty()
        { foreach (Text t in Texts.Values) if (t && t.isActiveAndEnabled) t.SetVerticesDirty(); Canvas.ForceUpdateCanvases(); }
        public static string Begin()
        {
            Need(!running, "Already running");
            Need(!Renderforge.CrispFonts.Active, "Production correction unexpectedly active in proof assembly");
            var patches = Harmony.GetPatchInfo(Populate);
            Need(patches == null || !patches.Owners.Any(), "Existing Text mesh patch owner; refusing overlap");
            beforeConfig = Config(); Texts.Clear(); Meshes.Clear(); Rebuilds.Clear(); Errors.Clear(); atlasEvents = 0;
            foreach (Text t in Object.FindObjectsOfType<Text>().Where(Visible))
                if (!PathOf(t.transform).StartsWith("ConsoleCanvas")) Texts[t.GetInstanceID()] = t;
            Need(Texts.Count >= 2, "Too few current visible supported labels");
            watchdog = new GameObject("Renderforge fonts lifecycle watchdog"); watchdog.AddComponent<Watchdog>();
            deadline = Time.realtimeSinceStartup + 180; running = true;
            try
            {
                observer = new Harmony(Owner); observer.Patch(Generate, postfix: new HarmonyMethod(typeof(Probe), nameof(Observe)));
                Application.logMessageReceived += OnLog; Font.textureRebuilt += OnAtlas;
                Dirty(); return Sample("A1");
            }
            catch { Stop(); throw; }
        }
        public static string Enable()
        { Need(running, "Not running"); Renderforge.CrispFonts.Apply(true); Need(Renderforge.CrispFonts.Active, "Enable refused"); Dirty(); return "enabled"; }
        public static string Disable()
        { Renderforge.CrispFonts.Dispose(); Renderforge.CrispFonts.Tick(); Canvas.ForceUpdateCanvases(); return "disabled"; }
        public static string Sample(string name)
        {
            Need(name.All(c => char.IsLetterOrDigit(c) || c == '-'), "Invalid sample name");
            return Save(name, new { config = Config(), nativeStatus = Status(), screen = new[] { Screen.width, Screen.height }, timeScale = Time.timeScale,
                active = Renderforge.CrispFonts.Active, cached = Renderforge.FontRasterCorrection.CachedCount, atlasEvents, errors = Errors.ToArray(),
                labels = Texts.Values.Where(t => t).Select(t => new { id = t.GetInstanceID(), path = PathOf(t.transform), text = t.text,
                    font = t.font.name, fontId = t.font.GetInstanceID(), t.fontSize, t.resizeTextForBestFit, ppu = t.pixelsPerUnit,
                    preferred = new[] { t.preferredWidth, t.preferredHeight }, rect = new[] { t.rectTransform.rect.width, t.rectTransform.rect.height },
                    material = RawMaterial.GetValue(t) is Material mat ? mat.GetInstanceID() : 0,
                    outcome = Renderforge.FontRasterCorrection.Result(t.GetInstanceID()),
                    rebuilds = Rebuilds.TryGetValue(t.GetInstanceID(), out int count) ? count : 0,
                    mesh = Meshes.TryGetValue(t.GetInstanceID(), out object mesh) ? mesh : null }).ToArray() });
        }
        static Text Choose()
        {
            return Texts.Values.First(t => t && Renderforge.FontRasterCorrection.Result(t.GetInstanceID()) == "corrected"
                && t.GetComponents<Component>().All(c => !(c is IMeshModifier)));
        }
        public static string MaterialThenDisable()
        {
            Need(running && Renderforge.CrispFonts.Active, "Enable first"); edited = Choose();
            originalMaterial = (Material)RawMaterial.GetValue(edited); originalSize = edited.fontSize; originalRect = edited.rectTransform.sizeDelta;
            ownedMaterial = new Material(edited.material); edited.material = ownedMaterial;
            Need(!Renderforge.CrispFonts.Supported(edited), "Custom material did not exclude label");
            int before = Rebuilds[edited.GetInstanceID()];
            Renderforge.CrispFonts.Dispose(); Renderforge.CrispFonts.Tick(); Canvas.ForceUpdateCanvases();
            Need(Rebuilds[edited.GetInstanceID()] > before, "Previously corrected custom-material label was not rebuilt");
            var actual = Newtonsoft.Json.Linq.JObject.FromObject(Meshes[edited.GetInstanceID()]);
            var expectedUV = new List<float[]>(); var expectedPos = new List<object>();
            using (var vh = new VertexHelper())
            {
                Populate.Invoke(edited, new object[] { vh });
                for (int i = 0; i < vh.currentVertCount; i++) { UIVertex v = default; vh.PopulateUIVertex(ref v, i); expectedUV.Add(new[] { v.uv0.x, v.uv0.y }); expectedPos.Add(V(v.position)); }
            }
            bool uvExact = Newtonsoft.Json.Linq.JToken.DeepEquals(actual["uv"], Newtonsoft.Json.Linq.JArray.FromObject(expectedUV));
            bool posExact = Newtonsoft.Json.Linq.JToken.DeepEquals(actual["positions"], Newtonsoft.Json.Linq.JArray.FromObject(expectedPos));
            Save("material-disable", new { id = edited.GetInstanceID(), uvExact, posExact, beforeRebuilds = before, afterRebuilds = Rebuilds[edited.GetInstanceID()], cached = Renderforge.FontRasterCorrection.CachedCount });
            edited.material = originalMaterial; Object.Destroy(ownedMaterial); ownedMaterial = null; edited.SetVerticesDirty(); Canvas.ForceUpdateCanvases(); edited = null;
            Need(uvExact && posExact, "Material transition did not restore current normal mesh exactly");
            return "custom material before disable: fresh original UV/positions exact";
        }
        static void Noop() { }
        public static string LateForeign()
        {
            Need(running && Renderforge.CrispFonts.Active, "Enable first");
            foreign = new Harmony(Foreign); foreign.Patch(Populate, postfix: new HarmonyMethod(typeof(Probe), nameof(Noop)));
            Dirty(); return "foreign noop inserted; wait at least one frame then CheckForeign";
        }
        public static string CheckForeign()
        {
            Dirty(); Renderforge.CrispFonts.Tick(); Canvas.ForceUpdateCanvases();
            var owners = Harmony.GetPatchInfo(Populate)?.Owners.ToArray() ?? new string[0];
            bool suspended = !Renderforge.CrispFonts.Active, preserved = owners.Contains(Foreign);
            Save("foreign-suspended", new { suspended, foreignOwnerPreserved = preserved, owners, cached = Renderforge.FontRasterCorrection.CachedCount });
            foreign.Unpatch(Populate, HarmonyPatchType.All, Foreign); foreign = null;
            Need(suspended && preserved, "Late owner was not preserved with correction suspended");
            return "suspended; foreign owner preserved and then explicitly removed by diagnostic";
        }
        static object Entry(Text t) => ((IDictionary)AccessTools.Field(typeof(Renderforge.FontRasterCorrection), "Entries").GetValue(null))[t.GetInstanceID()];
        static bool Retried(Text t) { object e = Entry(t); return (bool)AccessTools.Field(e.GetType(), "Retried").GetValue(e); }
        public static string RetryKeys()
        {
            Need(running && Renderforge.CrispFonts.Active, "Enable first"); edited = Choose();
            originalMaterial = (Material)RawMaterial.GetValue(edited); originalSize = edited.fontSize; originalRect = edited.rectTransform.sizeDelta;
            Renderforge.FontRasterCorrection.AtlasChanged(edited.font); Renderforge.FontRasterCorrection.RefreshPending(); Canvas.ForceUpdateCanvases();
            bool first = Retried(edited);
            Renderforge.FontRasterCorrection.AtlasChanged(edited.font); Renderforge.FontRasterCorrection.RefreshPending(); Canvas.ForceUpdateCanvases();
            bool same = Retried(edited);
            edited.fontSize = originalSize + 2; Canvas.ForceUpdateCanvases(); bool fontReset = !Retried(edited);
            Renderforge.FontRasterCorrection.AtlasChanged(edited.font); Renderforge.FontRasterCorrection.RefreshPending(); Canvas.ForceUpdateCanvases(); bool second = Retried(edited);
            edited.rectTransform.sizeDelta = originalRect + new Vector2(1, 0); Canvas.ForceUpdateCanvases(); bool rectReset = !Retried(edited);
            Save("retry-keys", new { first, sameKeyRemainsSpent = same, fontSizeReset = fontReset, second, rectReset, stimulus = "explicit core AtlasChanged callback; actual atlas events separately counted" });
            edited.fontSize = originalSize; edited.rectTransform.sizeDelta = originalRect; edited.SetVerticesDirty(); Canvas.ForceUpdateCanvases(); edited = null;
            Need(first && same && fontReset && second && rectReset, "Retry key boundary failed"); return "bounded retry reset on fontSize/rect; same key remains spent";
        }
        public static string Stop()
        {
            Renderforge.CrispFonts.Dispose(); Renderforge.CrispFonts.Tick();
            if (edited) { edited.material = originalMaterial; edited.fontSize = originalSize; edited.rectTransform.sizeDelta = originalRect; edited.SetVerticesDirty(); edited = null; }
            if (ownedMaterial) Object.Destroy(ownedMaterial); ownedMaterial = null;
            if (foreign != null) { foreign.Unpatch(Populate, HarmonyPatchType.All, Foreign); foreign = null; }
            Canvas.ForceUpdateCanvases();
            if (observer != null) { observer.Unpatch(Generate, HarmonyPatchType.All, Owner); observer = null; }
            Application.logMessageReceived -= OnLog; Font.textureRebuilt -= OnAtlas;
            running = false; if (watchdog) Object.Destroy(watchdog); watchdog = null;
            var owners = Harmony.GetPatchInfo(Populate)?.Owners.ToArray() ?? new string[0];
            bool configRestored = Config() == beforeConfig;
            Save("restore", new { configRestored, active = Renderforge.CrispFonts.Active, cached = Renderforge.FontRasterCorrection.CachedCount, owners, errors = Errors, nativeStatus = Status(), timeScale = Time.timeScale });
            Need(configRestored, "Config changed"); return "restored; config exact; caches=" + Renderforge.FontRasterCorrection.CachedCount;
        }
        internal static void Tick() { if (running && Time.realtimeSinceStartup > deadline) Stop(); }
    }
    public sealed class Watchdog : MonoBehaviour { void Update() => Probe.Tick(); }
}
