using System;
using System.Collections.Generic;
using System.Linq;
using Base.Cameras;
using Base.Core;
using Base.Utils;
using PhoenixPoint.Common.Core;
using PhoenixPoint.Geoscape.View;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace Renderforge
{
    /// <summary>Geoscape site markers drawn after reconstruction. Every GeoSite carries a GeoSiteVisualsController whose
    /// VisualsContainer (`GS_*(Clone)`) holds the marker: unlit transparent quads + TextMesh (site icon, "?", frame,
    /// shadow, resource / recruit / scanner-progress rows, timers; queue 2450-3000, no motion-vector pass, FOV
    /// billboarding) plus an empty WorldSpace "PopUp" canvas. GeoscapeCamera renders them at render resolution and every
    /// temporal upscaler trails them when the globe turns (docs\research\geoscape-dlss-2026-09-07). While a real upscaler
    /// generation is Live on GeoscapeCamera: those subtrees (minus the 3D SiteAddon models, which stay in the upscaler)
    /// move to a free layer no other camera culls, and the driver's DlssPresent camera (M here) draws that layer
    /// at output resolution: its outRT blit moves from AfterEverything to BeforeForwardOpaque, the camera clears depth
    /// only, and - pose and projection copied from GeoscapeCamera every OnPreCull - renders the markers on top.
    /// A THIRD screen camera after DlssPresent does not work on this engine (measured 2026-09-07): once it exists the
    /// present blit never reaches the buffer the HUD and the end-of-frame ReadPixels see - stale frames with marker
    /// trails - whatever its clear flags or depth; disabling it brings the blit back. Vanilla hid far-side markers with
    /// the globe's depth buffer, which the cleared depth cannot, so a segment/sphere test hands far-side sites back to
    /// their original layers (the game names every layer but one, so a hidden layer was not available). Layer ownership
    /// changes only in GeoscapeCamera's own pre-cull (Camera.onPreCull), so both cameras draw one frame with one split.
    /// Everything is restored on release; an exception anywhere here disables the overlay for the session, never the upscaler.</summary>
    internal static class GeoMarkerOverlay
    {
        private const int RescanEvery = 60;          // frames: sites activate mid-level; highlight / addon / mission visuals spawn under known ones
        private const float RewalkMinInterval = 1f;  // s: a hierarchy change re-walks at most once a second

        private static Camera geo, M;                 // M = DlssDriver's present camera while active
        private static CommandBuffer cbPresent;
        private static Sync sync;
        private static int layer = -1;
        private static int origMask;                  // GeoscapeCamera's mask before the takeover (Walk: "was this ever drawn by it")
        private static readonly Dictionary<Camera, int> camMasks = new Dictionary<Camera, int>();   // every camera that lost bit `layer`
        private static readonly Dictionary<Transform, int> origLayers = new Dictionary<Transform, int>();
        private static readonly Dictionary<Canvas, Camera> origWorldCam = new Dictionary<Canvas, Camera>();
        private static readonly Dictionary<GeoSiteVisualsController, bool> near = new Dictionary<GeoSiteVisualsController, bool>();
        private static GeoSiteVisualsController[] active = new GeoSiteVisualsController[0];   // horizon-tested a quarter per frame
        private static int cursor, frame;
        private static Transform hcRoot;              // the geoscape hierarchy the sites live in (hierarchyCount is per root)
        private static int walkedHc;
        private static float walkedAt;
        private static LevelSwitchCurtainController curtain;
        private static string lastGate;               // last logged "not armed" reason (log once per reason)

        // Present-camera snapshot, taken before the first Sync, restored verbatim.
        private static Rect sRect;
        private static float sFov, sNear, sFar, sOrthoSize, sDepth;
        private static bool sOrtho;
        private static int sMask;
        private static RenderingPath sPath;
        private static CameraClearFlags sClear;
        private static Vector3 sPos;
        private static Quaternion sRot;

        internal static bool Active => M != null;

        /// <summary>Every Live frame from DlssDriver.Step. Arms when every gate in Blocker is open; while armed, a gate closing
        /// (level curtain dropping, colour vision switched on, ...) releases the same frame. Never throws: the upscaler
        /// must not die for a marker walk.</summary>
        internal static void Tick(Camera sceneCam, bool passthrough)
        {
            try
            {
                string why = Blocker(sceneCam, passthrough);
                if (M != null)
                {
                    if (why == null) return;
                    Log("released - " + why);
                    Restore();
                    lastGate = why;
                    return;
                }
                if (why != null)
                {
                    if (why != lastGate && sceneCam != null && sceneCam.name == "GeoscapeCamera") { lastGate = why; Log("not armed - " + why); }
                    return;
                }
                Activate(sceneCam);
            }
            catch (Exception ex) { Fail(ex); }
        }

        /// <summary>Null when the overlay may run, else the reason. M must NOT exist while the level curtain is down: created
        /// under the curtain (CameraManager.PauseObjectRendering, GeoscapeCamera.cullingMask 0, then the fade), the
        /// DlssPresent blit never reaches the backbuffer again for the life of that M - the loading art stays on
        /// screen with the HUD over it (observed 2026-09-07 on every level start; re-creating M mid-level is fine).
        /// So arm only once LevelSwitchCurtainController.IsCurtainLifted (set after the lift fade, LevelSwitchCurtainController.cs:113).
        /// ponytail: D3D11 only, no frame generation, no colour vision - the markers bypass the shim's post pass, so
        /// daltonisation would stop covering the colour-coded icons, FG's interpolated frames would lack them, and the
        /// D3D12 sRGB blend of this pass is untested. Every other combination keeps the vanilla path unchanged.</summary>
        private static string Blocker(Camera sceneCam, bool passthrough)
        {
            if (!Diagnostics.MarkerOverlay) return "disabled";
            if (sceneCam == null || sceneCam.name != "GeoscapeCamera") return "not the geoscape camera";
            if (passthrough) return "passthrough generation";
            if (!Availability.IsD3D11) return "API " + Availability.ApiName + " (marker pass only verified on D3D11)";
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (FrameGen.Live || (cfg != null && cfg.FrameGen != FrameGenMode.Off)) return "frame generation on (interpolated frames would lack the markers)";
            if (cfg != null && cfg.ColorVision != ColorVisionMode.None) return "colour vision " + cfg.ColorVision + " (markers would bypass the daltonisation pass)";
            if (curtain == null) curtain = GameUtl.GameComponent<CameraManager>()?.GetComponentInParent<LevelSwitchCurtainController>();
            if (curtain == null || !curtain.IsCurtainLifted) return "level curtain down";
            if (sceneCam.cullingMask == 0) return "GeoscapeCamera culls nothing";
            return null;
        }

        private static void Activate(Camera sceneCam)
        {
            var d = DlssDriver.Instance;
            if (d == null || d.PresentCamera == null || d.PresentBuffer == null) return;
            layer = -1;
            for (int i = 31; i >= 8 && layer < 0; i--) if (string.IsNullOrEmpty(LayerMask.LayerToName(i))) layer = i;
            if (layer < 0) { lastGate = "no free layer"; RenderforgeMod.Instance?.Logger.LogWarning("Geoscape markers: no free layer, markers stay in the upscaler"); Diagnostics.MarkerOverlay = false; return; }
            geo = sceneCam;
            M = d.PresentCamera; cbPresent = d.PresentBuffer;
            sRect = M.rect; sFov = M.fieldOfView; sNear = M.nearClipPlane; sFar = M.farClipPlane;
            sOrtho = M.orthographic; sOrthoSize = M.orthographicSize; sDepth = M.depth; sMask = M.cullingMask;
            sPath = M.renderingPath; sClear = M.clearFlags;
            sPos = M.transform.localPosition; sRot = M.transform.localRotation;
            origMask = geo.cullingMask;
            StripCameras();
            M.RemoveCommandBuffer(CameraEvent.AfterEverything, cbPresent);
            M.AddCommandBuffer(CameraEvent.BeforeForwardOpaque, cbPresent);   // blit first (colour only, ZWrite off) ...
            M.clearFlags = CameraClearFlags.Depth;                              // ... over a depth cleared before it: a clean depth for the markers
            M.renderingPath = RenderingPath.Forward;
            M.cullingMask = 1 << layer;
            sync = M.gameObject.AddComponent<Sync>();
            Camera.onPreCull += OnScenePreCull;
            hcRoot = null; walkedHc = -1; walkedAt = -1f; frame = 0; cursor = 0; lastGate = null;
            int n = Rescan(true);
            Log("drawn after reconstruction by the DlssPresent camera, layer " + layer + ", " + n + " sites / " + origLayers.Count + " objects, " + camMasks.Count + " cameras lost the bit, GeoscapeCamera mask 0x" + origMask.ToString("X") + " -> 0x" + geo.cullingMask.ToString("X"));
        }

        /// <summary>Bit `layer` off every enabled camera but M (it was free: nobody drew it, but a mask of ~0 includes it).</summary>
        private static void StripCameras()
        {
            int bit = 1 << layer;
            foreach (var c in Camera.allCameras)
            {
                if (c == M || (c.cullingMask & bit) == 0) continue;
                if (!camMasks.ContainsKey(c)) camMasks[c] = c.cullingMask;
                c.cullingMask &= ~bit;
            }
        }

        internal static void Restore()
        {
            if (M == null && geo == null) return;
            Camera.onPreCull -= OnScenePreCull;
            int objects = 0;
            foreach (var kv in origLayers) if (kv.Key) { kv.Key.gameObject.layer = kv.Value; objects++; }
            foreach (var kv in origWorldCam) if (kv.Key) kv.Key.worldCamera = kv.Value;
            foreach (var kv in camMasks) if (kv.Key) kv.Key.cullingMask |= kv.Value & (1 << layer);
            if (M)
            {
                if (sync) Object.Destroy(sync);
                if (cbPresent != null)
                {
                    M.RemoveCommandBuffer(CameraEvent.BeforeForwardOpaque, cbPresent);
                    M.AddCommandBuffer(CameraEvent.AfterEverything, cbPresent);
                }
                M.cullingMask = sMask; M.clearFlags = sClear; M.renderingPath = sPath; M.depth = sDepth;
                M.rect = sRect; M.orthographic = sOrtho; M.orthographicSize = sOrthoSize;
                M.fieldOfView = sFov; M.nearClipPlane = sNear; M.farClipPlane = sFar;
                M.ResetProjectionMatrix();   // the snapshot had no custom projection; Sync installed one
                M.transform.localPosition = sPos; M.transform.localRotation = sRot;
            }
            Log("restored " + near.Count + " sites / " + objects + " objects / " + camMasks.Count + " cameras, GeoscapeCamera mask 0x" + (geo ? geo.cullingMask.ToString("X") : "-"));
            origLayers.Clear(); origWorldCam.Clear(); near.Clear(); camMasks.Clear();
            active = new GeoSiteVisualsController[0]; cursor = 0;
            geo = null; M = null; curtain = null; sync = null; cbPresent = null; hcRoot = null;
        }

        /// <summary>Any exception in the overlay: log, put everything back, switch the overlay off for the session. The
        /// upscaler keeps running (DlssDriver.Fail never sees it).</summary>
        private static void Fail(Exception ex)
        {
            RenderforgeMod.Instance?.Logger.LogError("Geoscape markers: disabled for this session - " + ex);
            Diagnostics.MarkerOverlay = false;
            try { Restore(); }
            catch (Exception ex2) { RenderforgeMod.Instance?.Logger.LogError("Geoscape markers: restore threw - " + ex2); geo = null; M = null; }
        }

        private static void Log(string s) => RenderforgeMod.Instance?.Logger.LogInfo("Geoscape markers: " + s);

        private static void PruneDead<K, V>(Dictionary<K, V> d) where K : Object
        {
            List<K> dead = null;
            foreach (var k in d.Keys) if (!k) { if (dead == null) dead = new List<K>(); dead.Add(k); }
            if (dead != null) foreach (var k in dead) d.Remove(k);
        }

        /// <summary>Every active GeoSiteVisualsController. Runs every RescanEvery frames but does the work only when the
        /// geoscape hierarchy's transform count moved and the last walk is at least RewalkMinInterval old (force = activation).
        /// ponytail: hierarchyCount is one number for the whole Geoscape tree, so a change anywhere re-walks every near
        /// site (~5 ms, the ceiling of one rescan); a site that merely toggles active without any spawn is not seen until
        /// something else changes the count. Per-site dirty flags if that ever shows.</summary>
        private static int Rescan(bool force)
        {
            int hc = hcRoot ? hcRoot.hierarchyCount : walkedHc;
            float now = Time.unscaledTime;
            if (!force && (hc == walkedHc || now - walkedAt < RewalkMinInterval)) return active.Length;
            PruneDead(near); PruneDead(origLayers); PruneDead(origWorldCam); PruneDead(camMasks);
            StripCameras();   // cameras that appeared since (dialogs, cinematics) must not draw the marker layer either
            var found = Object.FindObjectsOfType<GeoSiteVisualsController>();
            var list = new List<GeoSiteVisualsController>(found.Length);
            if (hcRoot == null)
                foreach (var v in found) if (v.VisualsContainer) { hcRoot = v.VisualsContainer.root; hc = hcRoot.hierarchyCount; break; }
            bool rewalk = hc != walkedHc;
            walkedHc = hc; walkedAt = now;
            Vector3 c = geo.transform.position;
            float r = GlobeUnits.GlobeRadius, r2 = r * r;
            foreach (var v in found)
            {
                if (v.VisualsContainer == null) continue;
                bool isNear;
                if (near.TryGetValue(v, out isNear)) { if (isNear && rewalk) Layer(v, true); list.Add(v); continue; }
                var cv = v.CanvasIcons;
                if (cv && cv.isRootCanvas && cv.renderMode == RenderMode.WorldSpace && !origWorldCam.ContainsKey(cv)) { origWorldCam[cv] = cv.worldCamera; cv.worldCamera = M; }
                isNear = FacesCamera(v.VisualsContainer.position, c, r2);   // classified on sight: a far-side site never gets a frame through the globe
                near[v] = isNear;
                if (isNear) Layer(v, true);
                list.Add(v);
            }
            active = list.ToArray();
            cursor = 0;
            return active.Length;
        }

        /// <summary>VisualsContainer subtree onto the marker layer (take = true) or back to the recorded originals
        /// (take = false); the SiteSpecialAddonContainer / SiteUniqueAddonContainer subtrees (3D haven-zone models,
        /// GeoSiteVisualsController.cs:50,52) are pruned and stay with the upscaler.</summary>
        private static void Layer(GeoSiteVisualsController v, bool take)
        {
            Transform root = v.VisualsContainer;
            Transform a = v.SiteSpecialAddonContainer ? v.SiteSpecialAddonContainer.transform : null;
            Transform b = v.SiteUniqueAddonContainer ? v.SiteUniqueAddonContainer.transform : null;
            if (a == root) a = null;   // never prune the root itself
            if (b == root) b = null;
            Walk(root, a, b, take);
        }

        private static void Walk(Transform t, Transform pruneA, Transform pruneB, bool take)
        {
            if (t == pruneA || t == pruneB) return;
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
            for (int i = 0, n = t.childCount; i < n; i++) Walk(t.GetChild(i), pruneA, pruneB, take);
        }

        /// <summary>A site whose visuals pivot is behind the globe sphere (segment camera->pivot crosses the sphere;
        /// centre at the origin, radius GlobeUnits.GlobeRadius) goes back to vanilla rendering, where the globe's depth
        /// hides it. Only the sites that changed side are re-walked.
        /// ponytail: the test is on the pivot, so a marker pops as a whole at the horizon instead of sinking; a quarter of
        /// the active sites per frame, so a side change lands up to 4 frames late. Per-renderer bounds / all sites per
        /// frame if either is ever visible.</summary>
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
                bool was;
                if (!near.TryGetValue(v, out was) || visible == was) continue;
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

        /// <summary>GeoscapeCamera's own pre-cull: every layer-ownership change (mask re-assert, rescan, horizon) lands here,
        /// BEFORE it culls, so it and M draw this frame with the same near/far split. Done from M's OnPreCull (after
        /// GeoscapeCamera rendered) a side change dropped a marker for a frame (near->far) or doubled it (far->near).</summary>
        private static void OnScenePreCull(Camera c)
        {
            if (c != geo || M == null) return;
            try
            {
                geo.cullingMask &= ~(1 << layer);       // re-asserted: the game may rewrite it
                if (++frame % RescanEvery == 0) Rescan(false);
                UpdateHorizon();
            }
            catch (Exception ex) { Fail(ex); }
        }

        /// <summary>On M: pose + projection copy only. GeoscapeCamera already rendered (depth order) and
        /// DlssDriver.OnCameraPostRender reset its jitter.</summary>
        private sealed class Sync : MonoBehaviour
        {
            private void OnPreCull()
            {
                if (geo == null || M == null) return;
                try
                {
                    M.transform.SetPositionAndRotation(geo.transform.position, geo.transform.rotation);
                    M.rect = geo.rect;
                    M.orthographic = geo.orthographic;
                    M.orthographicSize = geo.orthographicSize;
                    M.fieldOfView = geo.fieldOfView;
                    M.nearClipPlane = geo.nearClipPlane;
                    M.farClipPlane = geo.farClipPlane;
                    M.projectionMatrix = geo.projectionMatrix;
                }
                catch (Exception ex) { Fail(ex); }
            }
        }

        // ---------------------------------------------------------------- PPCLI diagnostics

        internal static string Status()
        {
            if (M == null) return "inactive layer=" + layer + " gate=" + (lastGate ?? "-");
            int onNear = near.Count(kv => kv.Key && kv.Value);
            string g = geo ? geo.name : "destroyed";
            return "active layer=" + layer + " sites=" + near.Count + " activeSites=" + active.Length + " onNear=" + onNear + " objects=" + origLayers.Count + " canvases=" + origWorldCam.Count + " cameras=" + camMasks.Count
                 + " geo=" + g + " geoMask=0x" + (geo ? geo.cullingMask.ToString("X") : "-") + " origMask=0x" + origMask.ToString("X")
                 + " M=" + M.name + " M.enabled=" + M.enabled + " M.pos=" + M.transform.position + " geo.pos=" + (geo ? geo.transform.position.ToString() : "-") + " M.rot=" + M.transform.rotation.eulerAngles + " geo.rot=" + (geo ? geo.transform.rotation.eulerAngles.ToString() : "-")
                 + " M.depth=" + M.depth + " geo.depth=" + (geo ? geo.depth.ToString() : "-") + " M.mask=0x" + M.cullingMask.ToString("X") + " M.clear=" + M.clearFlags + " M.cbs=" + M.commandBufferCount
                 + " M.path=" + M.renderingPath + " M.msaa=" + M.allowMSAA + " M.hdr=" + M.allowHDR + " M.occlusion=" + M.useOcclusionCulling + " M.target=" + (M.targetTexture ? M.targetTexture.name : "null")
                 + " M.fov=" + M.fieldOfView.ToString("F3") + " geo.fov=" + (geo ? geo.fieldOfView.ToString("F3") : "-") + " M.rect=" + M.rect + " M.pixel=" + M.pixelWidth + "x" + M.pixelHeight
                 + " M.pp=" + (M.GetComponent<UnityEngine.Rendering.PostProcessing.PostProcessLayer>() != null) + " M.brain=" + (M.GetComponent<Cinemachine.CinemachineBrain>() != null)
                 + " screen=" + Screen.width + "x" + Screen.height + " globeR=" + GlobeUnits.GlobeRadius.ToString("F3");
        }

        /// <summary>Site picking is a physics raycast on the scene camera (GeoscapeView.cs:912, PickingCollider on its own
        /// layer - untouched; the game has no other picking entry point, so this does the same raycast). Project the first
        /// near site's pivot through M, raycast the same pixel through GeoscapeCamera, report what it hits.</summary>
        internal static string ProbeClick()
        {
            if (M == null || !geo) return "inactive";
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
            // ponytail: raw RGB24 bottom-up (~11 MB, ~10 ms) - EncodeToPNG would stall the frame ~150 ms mid-sweep; the
            // reader converts. A background PNG encode if the dumps ever leave the test bench.
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
