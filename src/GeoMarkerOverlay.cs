using System.Collections.Generic;
using System.Linq;
using Base.Cameras;
using Base.Core;
using Base.Utils;
using PhoenixPoint.Common.Core;
using PhoenixPoint.Geoscape.View;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Geoscape site markers drawn after reconstruction. Every GeoSite carries a GeoSiteVisualsController whose
    /// VisualsContainer (`GS_*(Clone)`) holds the marker: unlit transparent quads + TextMesh (site icon, "?", frame,
    /// shadow, resource / recruit / scanner-progress rows, timers; queue 2450-3000, no motion-vector pass, FOV
    /// billboarding) plus an empty WorldSpace "PopUp" canvas. GeoscapeCamera renders them at render resolution and every
    /// temporal upscaler trails them when the globe turns (docs\research\geoscape-dlss-2026-09-07). While a real upscaler
    /// generation is Live on GeoscapeCamera: those subtrees (minus the 3D SiteAddon models, which stay in the upscaler)
    /// move to a free layer GeoscapeCamera no longer culls, and the driver's DlssPresent camera (M here) draws that layer
    /// at output resolution: its outRT blit moves from AfterEverything to BeforeForwardOpaque, a depth clear follows it,
    /// and the camera - pose and projection copied from GeoscapeCamera every OnPreCull - renders the markers on top.
    /// A THIRD screen camera after DlssPresent does not work on this engine (measured 2026-09-07): once it exists the
    /// present blit never reaches the buffer the HUD and the end-of-frame ReadPixels see - stale frames with marker
    /// trails - whatever its clear flags or depth; disabling it brings the blit back. Vanilla hid far-side markers with
    /// the globe's depth buffer, which the cleared depth cannot, so a segment/sphere test hands far-side sites back to
    /// their original layers (the game names every layer but one, so a hidden layer was not available). Everything is
    /// restored on release.</summary>
    internal static class GeoMarkerOverlay
    {
        private const int RescanEvery = 60;   // frames: sites activate mid-level; highlight / addon / mission visuals spawn under known ones

        private static Camera geo, M;                 // M = DlssDriver's present camera while active
        private static UnityEngine.Rendering.CommandBuffer cbPresent, cbClear;
        private static RenderingPath origPath;
        private static Sync sync;
        private static int layer = -1;
        private static int origMask;
        private static readonly Dictionary<Transform, int> origLayers = new Dictionary<Transform, int>();
        private static readonly Dictionary<Canvas, Camera> origWorldCam = new Dictionary<Canvas, Camera>();
        private static readonly Dictionary<GeoSiteVisualsController, bool> near = new Dictionary<GeoSiteVisualsController, bool>();
        private static GeoSiteVisualsController[] active = new GeoSiteVisualsController[0];   // horizon-tested a quarter per frame
        private static int cursor, walkedHc;

        internal static bool Active => M != null;
        private static LevelSwitchCurtainController curtain;

        /// <summary>Every Live frame from DlssDriver.Step. M must NOT exist while the level curtain is down: created
        /// under the curtain (CameraManager.PauseObjectRendering, GeoscapeCamera.cullingMask 0, then the fade), the
        /// DlssPresent blit never reaches the backbuffer again for the life of that M - the loading art stays on
        /// screen with the HUD over it (observed 2026-09-07 on every level start; re-creating M mid-level is fine).
        /// So arm only once LevelSwitchCurtainController.IsCurtainLifted (set after the lift fade, LevelSwitchCurtainController.cs:113).</summary>
        internal static void Tick(Camera sceneCam, bool passthrough)
        {
            if (M != null || passthrough || !Diagnostics.MarkerOverlay || sceneCam == null || sceneCam.name != "GeoscapeCamera") return;
            if (curtain == null) curtain = GameUtl.GameComponent<CameraManager>()?.GetComponentInParent<LevelSwitchCurtainController>();
            if (curtain == null || !curtain.IsCurtainLifted || sceneCam.cullingMask == 0) return;
            Activate(sceneCam);
        }

        internal static void Activate(Camera sceneCam)
        {
            if (M != null || !Diagnostics.MarkerOverlay || sceneCam == null || sceneCam.name != "GeoscapeCamera" || sceneCam.cullingMask == 0) return;
            var d = DlssDriver.Instance;
            if (d == null || d.PresentCamera == null || d.PresentBuffer == null) return;
            layer = -1;
            for (int i = 31; i >= 8 && layer < 0; i--) if (string.IsNullOrEmpty(LayerMask.LayerToName(i))) layer = i;
            if (layer < 0) { RenderforgeMod.Instance?.Logger.LogWarning("Geoscape markers: no free layer, markers stay in the upscaler"); return; }
            geo = sceneCam;
            origMask = geo.cullingMask;
            geo.cullingMask &= ~(1 << layer);
            M = d.PresentCamera; cbPresent = d.PresentBuffer;
            M.RemoveCommandBuffer(UnityEngine.Rendering.CameraEvent.AfterEverything, cbPresent);
            M.AddCommandBuffer(UnityEngine.Rendering.CameraEvent.BeforeForwardOpaque, cbPresent);   // blit first ...
            cbClear = new UnityEngine.Rendering.CommandBuffer { name = "Renderforge marker depth clear" };
            cbClear.ClearRenderTarget(true, false, Color.clear);
            M.AddCommandBuffer(UnityEngine.Rendering.CameraEvent.BeforeForwardOpaque, cbClear);     // ... then a clean depth for the markers
            origPath = M.renderingPath;
            M.renderingPath = RenderingPath.Forward;
            M.cullingMask = 1 << layer;
            sync = M.gameObject.AddComponent<Sync>();
            walkedHc = -1;
            int n = Rescan();
            RenderforgeMod.Instance?.Logger.LogInfo("Geoscape markers: drawn after reconstruction by the DlssPresent camera, layer " + layer + ", " + n + " sites / " + origLayers.Count + " objects, GeoscapeCamera mask 0x" + origMask.ToString("X") + " -> 0x" + geo.cullingMask.ToString("X"));
        }

        internal static void Restore()
        {
            if (M == null && geo == null) return;
            int objects = 0;
            foreach (var kv in origLayers) if (kv.Key) { kv.Key.gameObject.layer = kv.Value; objects++; }
            foreach (var kv in origWorldCam) if (kv.Key) kv.Key.worldCamera = kv.Value;
            if (geo) geo.cullingMask |= origMask & (1 << layer);
            if (M)
            {
                if (sync) Object.Destroy(sync);
                M.cullingMask = 0;
                M.renderingPath = origPath;
                M.ResetProjectionMatrix();
                M.transform.localPosition = Vector3.zero; M.transform.localRotation = Quaternion.identity;
                if (cbClear != null) M.RemoveCommandBuffer(UnityEngine.Rendering.CameraEvent.BeforeForwardOpaque, cbClear);
                if (cbPresent != null)
                {
                    M.RemoveCommandBuffer(UnityEngine.Rendering.CameraEvent.BeforeForwardOpaque, cbPresent);
                    M.AddCommandBuffer(UnityEngine.Rendering.CameraEvent.AfterEverything, cbPresent);
                }
            }
            cbClear?.Release();
            RenderforgeMod.Instance?.Logger.LogInfo("Geoscape markers: restored " + near.Count + " sites / " + objects + " objects, GeoscapeCamera mask 0x" + (geo ? geo.cullingMask.ToString("X") : "-"));
            origLayers.Clear(); origWorldCam.Clear(); near.Clear();
            active = new GeoSiteVisualsController[0]; cursor = 0;
            geo = null; M = null; curtain = null; sync = null; cbPresent = null; cbClear = null;
        }

        /// <summary>Every active GeoSiteVisualsController. Known sites are re-walked only when the geoscape hierarchy's
        /// transform count moved (hierarchyCount is per root hierarchy: one number for all sites, so a change anywhere
        /// re-walks every near site - ~5 ms, rare). Inactive sites wait for the rescan after they show (<= 60 frames).</summary>
        private static int Rescan()
        {
            foreach (var dead in near.Keys.Where(v => !v).ToList()) near.Remove(dead);
            var found = Object.FindObjectsOfType<GeoSiteVisualsController>();
            var list = new List<GeoSiteVisualsController>(found.Length);
            int hc = found.Length > 0 && found[0].VisualsContainer ? found[0].VisualsContainer.hierarchyCount : walkedHc;
            bool rewalk = hc != walkedHc;
            walkedHc = hc;
            foreach (var v in found)
            {
                if (v.VisualsContainer == null) continue;
                bool isNear;
                if (near.TryGetValue(v, out isNear)) { if (isNear && rewalk) Layer(v, true); list.Add(v); continue; }
                var c = v.CanvasIcons;
                if (c && c.isRootCanvas && c.renderMode == RenderMode.WorldSpace && !origWorldCam.ContainsKey(c)) { origWorldCam[c] = c.worldCamera; c.worldCamera = M; }
                near[v] = true;
                Layer(v, true);
                list.Add(v);
            }
            active = list.ToArray();
            cursor = 0;
            return active.Length;
        }

        /// <summary>VisualsContainer subtree onto the marker layer (take = true) or back to the recorded originals
        /// (take = false); the SiteAddon subtree (3D haven-zone models) is pruned and stays with the upscaler.</summary>
        private static void Layer(GeoSiteVisualsController v, bool take)
        {
            Transform root = v.VisualsContainer;
            Transform addons = v.SiteSpecialAddonContainer ? v.SiteSpecialAddonContainer.transform.parent : null;
            if (addons == root) addons = v.SiteSpecialAddonContainer.transform;   // never prune the root itself
            Walk(root, addons, take);
        }

        private static void Walk(Transform t, Transform prune, bool take)
        {
            if (t == prune) return;
            var go = t.gameObject;
            if (take)
            {
                if (go.layer != layer)
                {
                    int orig;
                    if (origLayers.TryGetValue(t, out orig) || (origMask & (1 << go.layer)) != 0)   // untracked + never drawn by GeoscapeCamera: not ours
                    {
                        if (!origLayers.ContainsKey(t)) origLayers[t] = go.layer;
                        go.layer = layer;
                    }
                }
            }
            else
            {
                int orig;
                if (origLayers.TryGetValue(t, out orig)) go.layer = orig;
            }
            for (int i = 0, n = t.childCount; i < n; i++) Walk(t.GetChild(i), prune, take);
        }

        /// <summary>A site whose visuals pivot is behind the globe sphere (segment camera->pivot crosses the sphere;
        /// centre at the origin, radius GlobeUnits.GlobeRadius) goes back to vanilla rendering, where the globe's depth
        /// hides it. A quarter of the active sites per frame; only the sites that changed side are re-walked.</summary>
        private static void UpdateHorizon()
        {
            int n = active.Length;
            if (n == 0) return;
            Vector3 c = geo.transform.position;
            float r = GlobeUnits.GlobeRadius, r2 = r * r;
            for (int k = Mathf.Max(1, n / 4); k > 0; k--)
            {
                var v = active[cursor];
                cursor = (cursor + 1) % n;
                if (!v || !v.VisualsContainer) continue;
                bool visible = FacesCamera(v.VisualsContainer.position, c, r2);
                if (visible == near[v]) continue;
                near[v] = visible;
                Layer(v, visible);
            }
        }

        /// <summary>False when the segment camera -> p crosses the globe sphere (centre at the origin, radius^2 = r2).</summary>
        private static bool FacesCamera(Vector3 p, Vector3 c, float r2)
        {
            Vector3 d = p - c;
            float dd = d.sqrMagnitude;
            if (dd <= 1e-6f) return true;
            float t = -Vector3.Dot(c, d) / dd;
            return t <= 0f || t >= 1f || (c + d * t).sqrMagnitude >= r2;
        }

        private sealed class Sync : MonoBehaviour
        {
            private int frame;

            private void OnPreCull()
            {
                if (geo == null || M == null) return;
                // GeoscapeCamera already rendered (depth order) and DlssDriver.OnCameraPostRender reset its jitter.
                M.transform.SetPositionAndRotation(geo.transform.position, geo.transform.rotation);
                M.rect = geo.rect;
                M.orthographic = geo.orthographic;
                M.orthographicSize = geo.orthographicSize;
                M.fieldOfView = geo.fieldOfView;
                M.nearClipPlane = geo.nearClipPlane;
                M.farClipPlane = geo.farClipPlane;
                M.projectionMatrix = geo.projectionMatrix;
                geo.cullingMask &= ~(1 << layer);       // re-asserted: the game may rewrite it
                if (++frame % RescanEvery == 0) Rescan();
                UpdateHorizon();
            }
        }

        // ---------------------------------------------------------------- PPCLI diagnostics

        internal static string Status()
        {
            if (M == null) return "inactive layer=" + layer;
            int onNear = near.Count(kv => kv.Key && kv.Value);
            return "active layer=" + layer + " sites=" + near.Count + " activeSites=" + active.Length + " onNear=" + onNear + " objects=" + origLayers.Count + " canvases=" + origWorldCam.Count
                 + " geo=" + geo.name + " geoMask=0x" + geo.cullingMask.ToString("X") + " origMask=0x" + origMask.ToString("X")
                 + " M=" + M.name + " M.enabled=" + M.enabled + " M.pos=" + M.transform.position + " geo.pos=" + geo.transform.position + " M.rot=" + M.transform.rotation.eulerAngles + " geo.rot=" + geo.transform.rotation.eulerAngles
                 + " M.depth=" + M.depth + " geo.depth=" + geo.depth + " M.mask=0x" + M.cullingMask.ToString("X") + " M.clear=" + M.clearFlags + " M.cbs=" + M.commandBufferCount
                 + " M.path=" + M.renderingPath + " M.msaa=" + M.allowMSAA + " M.hdr=" + M.allowHDR + " M.occlusion=" + M.useOcclusionCulling + " M.target=" + (M.targetTexture ? M.targetTexture.name : "null")
                 + " M.fov=" + M.fieldOfView.ToString("F3") + " geo.fov=" + geo.fieldOfView.ToString("F3") + " M.rect=" + M.rect + " M.pixel=" + M.pixelWidth + "x" + M.pixelHeight
                 + " M.pp=" + (M.GetComponent<UnityEngine.Rendering.PostProcessing.PostProcessLayer>() != null) + " M.brain=" + (M.GetComponent<Cinemachine.CinemachineBrain>() != null)
                 + " screen=" + Screen.width + "x" + Screen.height + " globeR=" + GlobeUnits.GlobeRadius.ToString("F3");
        }

        /// <summary>Site picking is a physics raycast on the scene camera (PickingCollider, its own layer - untouched). Project
        /// the first near site's pivot through M, raycast the same pixel through GeoscapeCamera, report what it hits.</summary>
        internal static string ProbeClick()
        {
            if (M == null) return "inactive";
            foreach (var kv in near)
            {
                var v = kv.Key;
                if (!v || !kv.Value || !v.VisualsContainer || !v.gameObject.activeInHierarchy) continue;
                Vector3 sp = M.WorldToScreenPoint(v.VisualsContainer.position);
                if (sp.z <= 0f || sp.x < 0 || sp.y < 0 || sp.x >= Screen.width || sp.y >= Screen.height) continue;
                RaycastHit hit;
                bool ok = Physics.Raycast(geo.ScreenPointToRay(sp), out hit, geo.farClipPlane);
                var site = ok ? hit.collider.GetComponentInParent<GeoSiteVisualsController>() : null;
                return "site=" + v.name + " screen=" + sp.x.ToString("F1") + "," + sp.y.ToString("F1") + " hit=" + (ok ? hit.collider.name + " [" + hit.collider.gameObject.layer + "] of " + (site ? site.name : "-") : "none")
                     + " sameSite=" + (site == v) + " canvasCam=" + (v.CanvasIcons && v.CanvasIcons.worldCamera ? v.CanvasIcons.worldCamera.name : "null");
            }
            return "no near-side active site on screen";
        }

        /// <summary>Diagnostic: the real backbuffer at the end of this frame (PPCLI's screenshot re-renders the cameras itself and
        /// loses the present blit once a second scene camera exists) as raw RGB24, plus `path.txt` with size, frame number
        /// and the screen position + name of the site marker nearest the screen centre (the sweep metrics crop around it).</summary>
        internal static string DumpScreen(string path)
        {
            var host = DlssDriver.Instance;
            if (host == null) return "no driver";
            host.StartCoroutine(Capture(path));
            return "queued " + path + " frame=" + Time.frameCount;
        }

        private static System.Collections.IEnumerator Capture(string path)
        {
            yield return new WaitForEndOfFrame();
            RenderTexture.active = null;   // the screen, whatever the last camera left bound
            int w = Screen.width, h = Screen.height;
            var tex = new Texture2D(w, h, TextureFormat.RGB24, false);
            tex.ReadPixels(new Rect(0, 0, w, h), 0, 0, false);
            tex.Apply(false);
            // Raw RGB24 bottom-up (~11 MB, ~10 ms): EncodeToPNG would stall the frame ~150 ms mid-sweep.
            System.IO.File.WriteAllBytes(path, tex.GetRawTextureData());
            Object.Destroy(tex);
            // Crop anchor for the sweep metrics: the shown, camera-facing site marker nearest the screen centre (same
            // pick in every condition as long as the camera path is the same; the sidecar names it so that can be checked).
            var cam = DlssDriver.Instance ? DlssDriver.Instance.SceneCamera : null;
            Vector3 best = new Vector3(-1, -1, -1); float bestD = float.MaxValue; string site = "-";
            if (cam)
            {
                Vector3 c = cam.transform.position; float r2 = GlobeUnits.GlobeRadius * GlobeUnits.GlobeRadius;
                var centre = new Vector2(w / 2f, h / 2f);
                foreach (var v in Object.FindObjectsOfType<GeoSiteVisualsController>())
                {
                    if (!v.VisualsContainer || !v.LocationIconParent || !v.LocationIconParent.gameObject.activeInHierarchy) continue;
                    Vector3 p = v.VisualsContainer.position;
                    if (!FacesCamera(p, c, r2)) continue;
                    Vector3 sp = cam.WorldToScreenPoint(p);
                    if (sp.z <= 0f) continue;
                    float dd = ((Vector2)sp - centre).sqrMagnitude;
                    if (dd < bestD) { bestD = dd; best = sp; site = v.name.Replace(' ', '_'); }
                }
            }
            System.IO.File.WriteAllText(path + ".txt", "w=" + w + " h=" + h + " frame=" + Time.frameCount + " t=" + Time.unscaledTime.ToString("F3") + " base=" + best.x.ToString("F1") + "," + best.y.ToString("F1") + " overlay=" + (M != null) + " site=" + site + "\n");
        }

        /// <summary>Diagnostic: the hierarchy under the first active scene object called `rootName`, with renderers/canvases per node.</summary>
        internal static string DumpHierarchy(string rootName, string path)
        {
            var root = Resources.FindObjectsOfTypeAll<Transform>().FirstOrDefault(t => t.name == rootName && t.gameObject.scene.IsValid());
            if (root == null) return "no object named " + rootName;
            var sb = new System.Text.StringBuilder();
            Dump(root, 0, sb);
            System.IO.File.WriteAllText(path, sb.ToString());
            return root.name + " -> " + path;
        }

        private static void Dump(Transform t, int depth, System.Text.StringBuilder sb)
        {
            sb.Append(' ', depth * 2).Append(t.name).Append(t.gameObject.activeSelf ? "" : " (inactive)").Append(" [").Append(t.gameObject.layer).Append("]");
            foreach (var comp in t.GetComponents<Component>())
            {
                if (comp == null || comp is Transform) continue;
                string s = comp.GetType().Name;
                var r = comp as Renderer;
                if (r != null) s += "{" + (r.sharedMaterial ? r.sharedMaterial.shader.name : "nomat") + " q=" + (r.sharedMaterial ? r.sharedMaterial.renderQueue : 0) + " mv=" + r.motionVectorGenerationMode + "}";
                var cv = comp as Canvas;
                if (cv != null) s += "{" + cv.renderMode + (cv.isRootCanvas ? " root" : "") + "}";
                sb.Append(' ').Append(s);
            }
            sb.Append('\n');
            for (int i = 0; i < t.childCount; i++) Dump(t.GetChild(i), depth + 1, sb);
        }
    }
}
