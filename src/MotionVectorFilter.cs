using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using HarmonyLib;
using MaterialDecals;
using MirzaBeig.ParticleSystems;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Renderforge
{
    /// <summary>Keeps object motion vectors that describe no visible surface motion out of Unity's MotionVectors texture
    /// while an upscaler / frame generation consumes it (docs\DESIGN.md "Motion vectors of spun props and decal volumes").
    /// Unity's object-MV pass re-draws every renderer in MotionVectorGenerationMode.Object whose transform changed, with
    /// the renderer's own mesh. Two vanilla families move every frame without the picture moving:
    ///  - MaterialDecals.MaterialDecalComponent: the mesh is the decal's projection BOX (unit cube, scale 5 on a geoscape
    ///    range ring), never the surface the decal lands on. The game's other decal system already knows this
    ///    (LlockhamIndustries.Decals.ProjectionRenderer.cs:381 sets Camera); MaterialDecals forgot.
    ///  - MirzaBeig.ParticleSystems.Rotator (Rotator.cs:44/:48): the FX pack's idle spinner, on bodies of revolution
    ///    (geoscape planet Core, range rings, atmosphere shells). The spin is invisible, the motion vector is not.
    /// A static camera then feeds the upscaler a screen-wide sub-pixel motion field and it smears its history: the
    /// geoscape was soft and shimmering under every upscaler, DLAA included. Both families go to Camera (camera motion
    /// only) while armed; the original mode of every touched renderer is saved and restored on release.
    /// No per-frame search: decals come from MaterialDecalComponent.SceneDecals plus the OnEnable seam, rotators from one
    /// FindObjectsOfTypeAll at arm time plus the Start seam.</summary>
    internal static class MotionVectorFilter
    {
        /// <summary>Set by the seams' Prepare: false = that method is gone, only the arm-time pass covers the family.</summary>
        internal static bool DecalHooked, RotatorHooked;

        private static readonly Dictionary<Renderer, MotionVectorGenerationMode> saved = new Dictionary<Renderer, MotionVectorGenerationMode>();
        private static readonly FieldInfo sceneDecals = AccessTools.Field(typeof(MaterialDecalComponent), "SceneDecals");
        private static readonly List<Renderer> scratch = new List<Renderer>();
        private static bool armed, failed;
        private static int pruneAt = 1024;
        private static int decals, rotators, late;   // PPCLI Status counters, reset per arm

        internal static bool Armed => armed;

        /// <summary>Every Live frame from DlssDriver.KeepCameraState: armed exactly while the MV/depth pass is consumed.</summary>
        internal static void Set(bool wanted)
        {
            wanted &= Diagnostics.MvFilter && !failed;
            if (wanted == armed) return;
            if (wanted) Arm(); else Restore();
        }

        private static void Arm()
        {
            try
            {
                armed = true; decals = rotators = late = 0;
                var set = sceneDecals?.GetValue(null) as HashSet<MaterialDecalComponent>;
                if (set != null)
                    foreach (var d in set)
                        if (d != null && Take(d.GetComponent<Renderer>())) decals++;
                // Loaded-but-inactive ones too: an object deactivated now already ran its Start, so the seam never sees it again.
                foreach (var r in Resources.FindObjectsOfTypeAll<Rotator>())
                    if (r != null && r.gameObject.scene.IsValid() && TakeAll(r)) rotators++;
                Log("armed: " + decals + " decal volumes, " + rotators + " rotators");
            }
            catch (Exception ex) { Fail(ex); }
        }

        /// <summary>DlssDriver.Detach, the A/B lever, or the pass no longer needed. Destroyed renderers are skipped.</summary>
        internal static void Restore()
        {
            if (!armed && saved.Count == 0) return;
            armed = false;
            int n = 0;
            foreach (var kv in saved)
                if (kv.Key != null) { kv.Key.motionVectorGenerationMode = kv.Value; n++; }
            saved.Clear();
            pruneAt = 1024;
            Log("restored " + n + " renderers");
        }

        internal static void DecalEnabled(MaterialDecalComponent d)
        {
            if (!armed || ReferenceEquals(d, null)) return;
            try { if (Take(d.GetComponent<Renderer>())) late++; }
            catch (Exception ex) { Fail(ex); }
        }

        internal static void RotatorStarted(Rotator r)
        {
            if (!armed || ReferenceEquals(r, null)) return;
            try { if (TakeAll(r)) late++; }
            catch (Exception ex) { Fail(ex); }
        }

        /// <summary>Only the renderers on the spinner's own GameObject: a child may be real geometry riding along.</summary>
        private static bool TakeAll(Rotator r)
        {
            r.GetComponents(scratch);
            bool any = false;
            for (int i = 0; i < scratch.Count; i++) any |= Take(scratch[i]);
            scratch.Clear();
            return any;
        }

        private static bool Take(Renderer r)
        {
            if (r == null) return false;
            if (!saved.ContainsKey(r))
            {
                if (saved.Count >= pruneAt) Prune();
                saved[r] = r.motionVectorGenerationMode;
            }
            // Re-asserted on every enable: nothing in the game writes the mode, but a re-enabled object costs one compare.
            if (r.motionVectorGenerationMode != MotionVectorGenerationMode.Camera) r.motionVectorGenerationMode = MotionVectorGenerationMode.Camera;
            return true;
        }

        /// <summary>Geoscape travel-path decals are spawned and destroyed all campaign long: drop their dead keys.</summary>
        private static void Prune()
        {
            var dead = saved.Keys.Where(k => k == null).ToList();
            foreach (var k in dead) saved.Remove(k);
            pruneAt = Mathf.Max(1024, saved.Count * 2);
        }

        private static void Fail(Exception ex)
        {
            failed = true;
            RenderforgeMod.Instance?.Logger.LogError("MotionVectorFilter off for this session: " + ex);
            try { Restore(); } catch (Exception) { armed = false; saved.Clear(); }
        }

        private static void Log(string s) => RenderforgeMod.Instance?.Logger.LogInfo("MotionVectorFilter " + s);

        // ---------------------------------------------------------------- PPCLI diagnostics

        internal static string Status()
        {
            int alive = 0, camera = 0;
            var orig = new Dictionary<MotionVectorGenerationMode, int>();
            foreach (var kv in saved)
            {
                if (kv.Key == null) continue;
                alive++;
                if (kv.Key.motionVectorGenerationMode == MotionVectorGenerationMode.Camera) camera++;
                orig.TryGetValue(kv.Value, out int c); orig[kv.Value] = c + 1;
            }
            return "mvFilter=" + Diagnostics.MvFilter + " armed=" + armed + " failed=" + failed + " decalHooked=" + DecalHooked + " rotatorHooked=" + RotatorHooked
                 + " saved=" + saved.Count + " alive=" + alive + " nowCamera=" + camera + " decals=" + decals + " rotators=" + rotators + " late=" + late
                 + " savedModes=" + string.Join(",", orig.Select(k => k.Key + ":" + k.Value));
        }

        /// <summary>Every loaded scene renderer either family owns, by current mode: after a release none may still read
        /// Camera unless the prefab shipped that way. One-shot scene search - a diagnostic, never on the frame path.</summary>
        internal static string Audit()
        {
            var modes = new Dictionary<string, int>();
            void Count(string family, Renderer r)
            {
                if (r == null) return;
                string k = family + "/" + r.motionVectorGenerationMode;
                modes.TryGetValue(k, out int c); modes[k] = c + 1;
            }
            foreach (var d in Resources.FindObjectsOfTypeAll<MaterialDecalComponent>())
                if (d != null && d.gameObject.scene.IsValid()) Count("decal", d.GetComponent<Renderer>());
            foreach (var r in Resources.FindObjectsOfTypeAll<Rotator>())
                if (r != null && r.gameObject.scene.IsValid()) foreach (var x in r.GetComponents<Renderer>()) Count("rotator", x);
            return "armed=" + armed + " saved=" + saved.Count + " " + string.Join(" ", modes.OrderBy(k => k.Key, StringComparer.Ordinal).Select(k => k.Key + "=" + k.Value));
        }

        /// <summary>Which renderers move on a still picture: every active renderer whose localToWorldMatrix changed across
        /// `frames` frames, grouped by name / shader / queue / mode / components, with the motion vector the SDK is fed at
        /// the renderer's bounds centre (render-res pixels). Written to absPath when the coroutine ends.</summary>
        internal static IEnumerator Survey(DlssDriver driver, string absPath, int frames)
        {
            var all = Object.FindObjectsOfType<Renderer>();
            var before = new Matrix4x4[all.Length];
            for (int i = 0; i < all.Length; i++) before[i] = all[i].localToWorldMatrix;
            for (int f = 0; f < Mathf.Max(1, frames); f++) yield return null;
            var sb = new StringBuilder();
            try
            {
                Camera cam = driver.SceneCamera;
                int w = driver.RenderW, h = driver.RenderH;
                float[] mv = driver.ReadMv();
                var groups = new Dictionary<string, int>();
                var sample = new Dictionary<string, string>();
                int movers = 0;
                for (int i = 0; i < all.Length; i++)
                {
                    Renderer r = all[i];
                    if (r == null || r.localToWorldMatrix == before[i]) continue;
                    movers++;
                    Material m = r.sharedMaterial;
                    string key = r.name + " | " + r.GetType().Name + " | " + (m ? m.shader.name : "nomat") + " q=" + (m ? m.renderQueue : 0)
                               + " | mv=" + r.motionVectorGenerationMode + " | visible=" + r.isVisible + " | "
                               + string.Join(",", r.GetComponents<MonoBehaviour>().Where(c => c != null).Select(c => c.GetType().Name))
                               + " | parent=" + (r.transform.parent ? r.transform.parent.name : "-");
                    groups.TryGetValue(key, out int c0); groups[key] = c0 + 1;
                    if (sample.ContainsKey(key) || cam == null || mv == null) continue;
                    Vector3 sp = cam.WorldToViewportPoint(r.bounds.center);
                    int x = Mathf.RoundToInt(sp.x * w), y = Mathf.RoundToInt(sp.y * h);
                    sample[key] = sp.z <= 0f || x < 0 || y < 0 || x >= w || y >= h ? "offscreen"
                        : "at " + x + "," + y + " mvpx=" + (mv[(y * w + x) * 4] * w).ToString("F3") + "," + (mv[(y * w + x) * 4 + 1] * h).ToString("F3");
                }
                sb.Append("renderers=").Append(all.Length).Append(" movers=").Append(movers).Append(" frames=").Append(frames)
                  .Append(" render=").Append(w).Append('x').Append(h).Append(' ').Append(Status()).Append('\n');
                foreach (var g in groups.OrderByDescending(k => k.Value))
                    sb.Append(g.Value).Append(" x ").Append(g.Key).Append(" | ").Append(sample.TryGetValue(g.Key, out string s) ? s : "-").Append('\n');
            }
            catch (Exception ex) { sb.Append("survey threw: ").Append(ex); }
            System.IO.File.WriteAllText(absPath, sb.ToString());
        }
    }

    // ---------------------------------------------------------------- seams
    // Prepare = false (method renamed by a game update) skips the seam; the arm-time pass still covers what exists then.

    /// <summary>MaterialDecalComponent.OnEnable (MaterialDecals\MaterialDecalComponent.cs:136) registers the decal in
    /// SceneDecals: every range ring / travel path / sun decal created or re-activated after the arm-time pass.</summary>
    [HarmonyPatch(typeof(MaterialDecalComponent), "OnEnable")]
    internal static class MaterialDecalComponent_OnEnable_Patch
    {
        static bool Prepare() => MotionVectorFilter.DecalHooked = AccessTools.Method(typeof(MaterialDecalComponent), "OnEnable") != null;
        static void Postfix(MaterialDecalComponent __instance) => MotionVectorFilter.DecalEnabled(__instance);
    }

    /// <summary>Rotator.Start (MirzaBeig.ParticleSystems\Rotator.cs:19, an empty Unity message): once per spinner, on its
    /// first active frame - the ones instantiated after the arm-time pass.</summary>
    [HarmonyPatch(typeof(Rotator), "Start")]
    internal static class Rotator_Start_Patch
    {
        static bool Prepare() => MotionVectorFilter.RotatorHooked = AccessTools.Method(typeof(Rotator), "Start") != null;
        static void Postfix(Rotator __instance) => MotionVectorFilter.RotatorStarted(__instance);
    }
}
