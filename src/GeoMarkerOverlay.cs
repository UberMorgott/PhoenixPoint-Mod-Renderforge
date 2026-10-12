using System;
using System.Collections.Generic;
using System.Linq;
using Base.Cameras;
using Base.Core;
using Base.Utils;
using HarmonyLib;
using MaterialDecals;
using PhoenixPoint.Common.Core;
using PhoenixPoint.Geoscape.Entities;
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
    /// Aircraft (the lit model with its selection circles) and the GlobeMarker ground rings ride the same layer ("aircraft
    /// and globe markers" below); the layer is lit like Default (LightLayer) and, on Direct3D 12, drawn one frame late
    /// like the picture under it (PoseLag). Frame generation keeps the overlay: the markers are in the back buffer and
    /// not in the hud-less colour the generators are handed, which is what every SDK documents for UI (Blocker).
    /// Everything is restored on release; an exception anywhere here disables the overlay until the next level start, never the upscaler.</summary>
    internal static class GeoMarkerOverlay
    {
        // New marker objects arrive through Harmony seams (GeoSiteVisuals_* patches below: a site enabled, a highlight /
        // mission / diplomatic / details object spawned under a known one) and are walked the same frame, one site each.
        // FallbackInterval is only the safety net for spawners without a seam: the geoscape hierarchyCount is read that
        // often, and a change re-walks the near sites amortised, RewalkPerFrame at a time - never one ~5 ms burst.
        private const float FallbackInterval = 2f;   // s wall time
        private const int RewalkPerFrame = 8;
        /// <summary>Set by the OnEnable seam's Prepare: false (method missing) = the fallback also runs the scene-wide site search.</summary>
        internal static bool Hooked;

        private static Camera geo, M;                 // M = DlssDriver's present camera while active
        private static CommandBuffer cbPresent;
        private static Sync sync;
        private static int layer = -1;
        private static Exception pendingFail;         // thrown inside a render callback; Fail/Restore run from the next Tick (Update), never mid-render
        private static bool disabledByFailure;        // a Fail this level; ResetFailure (level start) clears it. Diagnostics.MarkerOverlay stays the user's own choice
        private static int origMask;                  // GeoscapeCamera's mask before the takeover (Walk: "was this ever drawn by it")
        private static readonly Dictionary<Camera, int> camMasks = new Dictionary<Camera, int>();   // every camera that lost bit `layer`
        private static readonly Dictionary<Light, int> lightMasks = new Dictionary<Light, int>();                       // every light that gained bit `layer`
        private static readonly Dictionary<Light, LightRenderMode> lightModes = new Dictionary<Light, LightRenderMode>();   // directional lights forced per-pixel
        private static readonly Dictionary<Transform, int> origLayers = new Dictionary<Transform, int>();
        private static readonly Dictionary<Canvas, Camera> origWorldCam = new Dictionary<Canvas, Camera>();
        private static readonly Dictionary<GeoSiteVisualsController, bool> near = new Dictionary<GeoSiteVisualsController, bool>();
        private static readonly List<GeoSiteVisualsController> active = new List<GeoSiteVisualsController>();   // horizon-tested a quarter per frame
        private static int cursor;
        private static readonly HashSet<GeoSiteVisualsController> dirty = new HashSet<GeoSiteVisualsController>();   // seams, drained in OnScenePreCull
        private static readonly Queue<GeoSiteVisualsController> rewalk = new Queue<GeoSiteVisualsController>();       // fallback, RewalkPerFrame per frame
        private static bool sitesDied;                // a GeoSiteVisualsController.OnDestroy since the last drain
        /// <summary>Set by the seams' Prepare (GeoVehicle.Initialize, GlobeMarker.Awake). The fallback searches the scene either way.</summary>
        internal static bool CraftHooked, GlobeMarkerHooked;
        /// <summary>Marker objects that are not under a site's VisualsContainer: an aircraft (V set; Root = its MeshRoot,
        /// resolved once GeoVehicle.Initialize ran) or a GlobeMarker (Root = the marker, Pivot = its surface offset child).</summary>
        private sealed class Loose
        {
            public Object Owner; public GeoVehicle V; public Transform Root, Pivot; public bool Near; public int Kids = -1;
            // World pose at this and the previous GeoscapeCamera pre-cull (PoseFrame = Time.frameCount of Pos), and the
            // local pose Sync put aside while the present camera draws the previous one (Shifted).
            public Vector3 Pos, PrevPos, HeldPos; public Quaternion Rot, PrevRot, HeldRot; public int PoseFrame = -1; public bool Shifted;
        }
        private static readonly List<Loose> loose = new List<Loose>();                  // horizon-tested every frame (aircraft fly)
        private static readonly HashSet<Object> looseKnown = new HashSet<Object>();
        private static readonly List<Object> looseSpawned = new List<Object>();          // seams, drained in OnScenePreCull
        private static Transform hcRoot;              // the geoscape hierarchy the sites live in (hierarchyCount is per root)
        private static int walkedHc;
        private static float nextScan;
        private static int fullScans, eventWalks, fallbackPasses;   // PPCLI Status counters, reset per activation
        private static Camera nameCam;                // last scene camera whose name was read (Camera.name allocates a string)
        private static bool nameIsGeo;
        private static readonly Predicate<GeoSiteVisualsController> deadSite = v => !v;
        private static LevelSwitchCurtainController curtain;
        private static CameraManager camMgr;
        private static string lastGate;              // last logged "not armed" reason (log once per reason)

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
                if (pendingFail != null) { var ex = pendingFail; pendingFail = null; Fail(ex); return; }
                string why = Blocker(sceneCam, passthrough);
                if (M != null)
                {
                    if (why == null) { StripCameras(); return; }   // every frame: cameras that appeared or got enabled since (dialogs, cinematics) must not draw the marker layer either
                    Log("released - " + why);
                    Restore();
                    lastGate = why;
                    return;
                }
                if (why != null)
                {
                    if (why != lastGate && IsGeoCam(sceneCam)) { lastGate = why; Log("not armed - " + why); }
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
        /// The game's own pause is authoritative too: a geoscape cutscene (UIStateGeoCutscene.cs:62) sets
        /// CameraManager.PauseObjectRendering, whose CullEverythingController zeroes GeoscapeCamera's mask for the whole video;
        /// the overlay stays released until the pause lifts (re-arming on the mask alone flapped every other frame, 1.6.2).
        /// ponytail: no colour vision - the markers bypass the shim's post pass, so daltonisation would stop covering
        /// the colour-coded icons. D3D12 runs the same pass as D3D11 (measured 2026-10-12, DLSS / FSR / XeSS Performance:
        /// the present camera's backbuffer write encodes and blends like D3D11 - same texel colours, same label-gradient
        /// blend - and site picking is unchanged).
        /// Frame generation is NOT a gate (it was until 2026-10-12, on the guess that interpolated frames would lack the
        /// markers). The generators get the upscaler's `out` twin as hud-less colour and Unity's back buffer as the
        /// final frame (docs\DESIGN.md "FG input contract" / "HUD"); the markers are drawn after the outRT blit, so they
        /// are in the back buffer only - the same class as the HUD. Each SDK keeps that class out of the interpolation:
        /// DLSS-G "Hudless - the scene color before any UI/HUD elements are drawn" (Streamline ProgrammingGuideDLSS_G.md:247,
        /// :268), XeSS-FG BACKBUFFER_HUDLESS "interpolates on the HUD-less color texture and extracts the UI from the
        /// back buffer" (xess_fg_developer_guide_english.md:602-605), FSR-FG "the frame interpolation shader will detect
        /// UI areas in the frame and suppress distortion in those areas" (frame-interpolation-swap-chain.md:91-92).
        /// Measured with Fg_Capture under all three: tagged hud-less without markers / aircraft, back buffer with them.
        /// ponytail: composited UI updates at the rendered rate (XeSS guide :521-522 "waypoints or enemy markers may
        /// stutter"): under an orbit the markers step at the base rate over an interpolated globe. Markers inside the
        /// hud-less colour (composited natively into the `out` twin before the generators read it) would interpolate
        /// with the globe's motion vectors - the upgrade path if that stutter is ever reported.</summary>
        private static string Blocker(Camera sceneCam, bool passthrough)
        {
            if (!Diagnostics.MarkerOverlay) return "disabled";
            if (disabledByFailure) return "disabled after a failure (until the next level)";
            if (!IsGeoCam(sceneCam)) return "not the geoscape camera";
            if (passthrough) return "passthrough generation";
            if (!Availability.IsD3D11 && !Availability.IsD3D12) return "API " + Availability.ApiName + " (marker pass only verified on D3D11 and D3D12)";
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg != null && cfg.ColorVision != ColorVisionMode.None) return "colour vision " + cfg.ColorVision + " (markers would bypass the daltonisation pass)";
            if (camMgr == null) camMgr = GameUtl.GameComponent<CameraManager>();
            if (curtain == null) curtain = camMgr?.GetComponentInParent<LevelSwitchCurtainController>();
            if (curtain == null || !curtain.IsCurtainLifted) return "level curtain down";
            if (camMgr != null && camMgr.PauseObjectRendering) return "object rendering paused by the game (cutscene)";
            if (sceneCam.cullingMask == 0) return "GeoscapeCamera culls nothing";
            return null;
        }

        /// <summary>Name read once per camera object (every Live frame asked; Camera.name allocates). A destroyed camera is a
        /// different reference from its successor, so the cache never answers for a new camera.</summary>
        private static bool IsGeoCam(Camera c)
        {
            if (!ReferenceEquals(c, nameCam)) { nameCam = c; nameIsGeo = c != null && c.name == "GeoscapeCamera"; }
            return nameIsGeo && c != null;
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
            hcRoot = null; walkedHc = -1; nextScan = Time.unscaledTime + FallbackInterval; cursor = 0; lastGate = null;
            fullScans = eventWalks = fallbackPasses = 0;
            dirty.Clear(); rewalk.Clear(); sitesDied = false;
            LightLayer();
            int n = FullScan();
            ScanLoose();
            UpdateLoose();   // classified and layered before the first frame, like the sites
            walkedHc = hcRoot ? hcRoot.hierarchyCount : -1;
            Log("drawn after reconstruction by the DlssPresent camera, layer " + layer + ", " + n + " sites / " + loose.Count + " aircraft + globe markers / " + origLayers.Count + " objects, " + camMasks.Count + " cameras lost the bit, GeoscapeCamera mask 0x" + origMask.ToString("X") + " -> 0x" + geo.cullingMask.ToString("X"));
        }

        /// <summary>Bit `layer` off every enabled camera but M (it was free: nobody drew it, but a mask of ~0 includes it).</summary>
        private static Camera[] camBuf = new Camera[8];   // reused: Camera.allCameras allocates a new array per call, this runs every frame
        private static void StripCameras()
        {
            int bit = 1 << layer;
            int count = Camera.allCamerasCount;
            if (camBuf.Length < count) camBuf = new Camera[count * 2];
            count = Camera.GetAllCameras(camBuf);
            for (int i = 0; i < count; i++)
            {
                var c = camBuf[i];
                if (c == M || (c.cullingMask & bit) == 0) continue;
                if (!camMasks.ContainsKey(c)) camMasks[c] = c.cullingMask;
                c.cullingMask &= ~bit;
            }
        }

        /// <summary>The marker layer lit like the layer its lit objects came from (Default: the aircraft model and the
        /// Standard-shader marker frames). Two things differ between GeoscapeCamera's deferred pass and the present camera's
        /// forward one. A light's culling mask: `Geoscape_Lights` (directional, 1.2) lists its layers and the free layer is
        /// not among them, so every light that lights Default gains the bit. And QualitySettings.pixelLightCount is 0
        /// in this game: forward shades only the brightest directional light per pixel and folds the other three
        /// into spherical harmonics, which left the aircraft flat (no speculars); the directional lights go ForcePixel
        /// while armed. Activation and every fallback pass; both are put back on release.</summary>
        private static void LightLayer()
        {
            int bit = 1 << layer;
            foreach (var l in Object.FindObjectsOfType<Light>())
            {
                if ((l.cullingMask & 1) == 0) continue;
                if ((l.cullingMask & bit) == 0) { if (!lightMasks.ContainsKey(l)) lightMasks[l] = l.cullingMask; l.cullingMask |= bit; }
                if (l.type == LightType.Directional && l.renderMode == LightRenderMode.Auto) { lightModes[l] = LightRenderMode.Auto; l.renderMode = LightRenderMode.ForcePixel; }
            }
        }

        internal static void Restore()
        {
            if (M == null && geo == null) return;
            Camera.onPreCull -= OnScenePreCull;
            UnshiftCrafts();
            int objects = 0;
            foreach (var kv in origLayers) if (kv.Key) { kv.Key.gameObject.layer = kv.Value; objects++; }
            foreach (var kv in origWorldCam) if (kv.Key) kv.Key.worldCamera = kv.Value;
            // Only the bit we stripped, and only onto the mask we left behind: a camera the game zeroed meanwhile
            // (CullEverythingController.PauseRendering during a cutscene) or rewrote keeps its mask - writing our bit
            // onto 0 made the next Tick see a non-zero mask, re-arm, strip, release, every other frame.
            int bit = 1 << layer;
            foreach (var kv in camMasks) if (kv.Key && kv.Key.cullingMask != 0 && kv.Key.cullingMask == (kv.Value & ~bit)) kv.Key.cullingMask |= kv.Value & bit;
            foreach (var kv in lightMasks) if (kv.Key && kv.Key.cullingMask == (kv.Value | bit)) kv.Key.cullingMask = kv.Value;
            foreach (var kv in lightModes) if (kv.Key && kv.Key.renderMode == LightRenderMode.ForcePixel) kv.Key.renderMode = kv.Value;
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
            Log("restored " + near.Count + " sites / " + loose.Count + " aircraft + globe markers / " + objects + " objects / " + camMasks.Count + " cameras, GeoscapeCamera mask 0x" + (geo ? geo.cullingMask.ToString("X") : "-"));
            Forget();
        }

        /// <summary>Every map and handle back to the unarmed state (Restore's tail; Fail even when Restore threw). A render-callback
        /// exception still pending (Restore reached from Detach / SetMarkerOverlay before the next Tick) is logged and counts as a Fail.</summary>
        private static void Forget()
        {
            if (pendingFail != null) { Disable(pendingFail); pendingFail = null; }
            origLayers.Clear(); origWorldCam.Clear(); near.Clear(); camMasks.Clear(); lightMasks.Clear(); lightModes.Clear();
            active.Clear(); cursor = 0; dirty.Clear(); rewalk.Clear(); sitesDied = false;
            loose.Clear(); looseKnown.Clear(); looseSpawned.Clear();
            geo = null; M = null; curtain = null; camMgr = null; sync = null; cbPresent = null; hcRoot = null; layer = -1;
        }

        /// <summary>Any exception in the overlay: log, put everything back, switch the overlay off until the next level start
        /// (ResetFailure). The upscaler keeps running (DlssDriver.Fail never sees it).</summary>
        private static void Fail(Exception ex)
        {
            Disable(ex);
            try { Restore(); }
            catch (Exception ex2) { RenderforgeMod.Instance?.Logger.LogError("Geoscape markers: restore threw - " + ex2); }
            Forget();
        }

        private static void Disable(Exception ex)
        {
            RenderforgeMod.Instance?.Logger.LogError("Geoscape markers: disabled until the next level - " + ex);
            disabledByFailure = true;
        }

        /// <summary>RenderforgeMod.OnLevelStart and SetMarkerOverlay: a Fail disables the overlay for its own level only, and never
        /// overrides an explicit Diagnostics.MarkerOverlay choice.</summary>
        internal static void ResetFailure() => disabledByFailure = false;

        private static void Log(string s) => RenderforgeMod.Instance?.Logger.LogInfo("Geoscape markers: " + s);

        private static void PruneDead<K, V>(Dictionary<K, V> d) where K : Object
        {
            List<K> dead = null;
            foreach (var k in d.Keys) if (!k) { if (dead == null) dead = new List<K>(); dead.Add(k); }
            if (dead != null) foreach (var k in dead) d.Remove(k);
        }

        /// <summary>Scene-wide search for GeoSiteVisualsControllers: activation, and the fallback when the OnEnable seam is
        /// missing. Only sites never seen before are classified and walked; known ones are left to the seams / rewalk queue.</summary>
        private static int FullScan()
        {
            fullScans++;
            var found = Object.FindObjectsOfType<GeoSiteVisualsController>();
            if (!hcRoot)
                foreach (var v in found) if (v.VisualsContainer) { hcRoot = v.VisualsContainer.root; break; }
            foreach (var v in found) if (!near.ContainsKey(v)) Admit(v);
            return active.Count;
        }

        /// <summary>One site from a seam (or a scan): a new one is classified on sight - a far-side site never gets a frame
        /// through the globe - and walked if near; a known near one is re-walked (something spawned under it).</summary>
        private static void Admit(GeoSiteVisualsController v)
        {
            if (!v || !v.VisualsContainer) return;
            bool isNear;
            if (near.TryGetValue(v, out isNear)) { if (isNear) { Layer(v, true); eventWalks++; } return; }
            isNear = FacesCamera(v.VisualsContainer.position, geo.transform.position, GlobeUnits.GlobeCenter, GlobeUnits.GlobeRadius * GlobeUnits.GlobeRadius);
            near[v] = isNear;
            if (isNear) { Layer(v, true); eventWalks++; }
            active.Add(v);
        }

        /// <summary>Seam events since the last frame, applied before GeoscapeCamera culls: the spawning frame already draws
        /// the new object on the marker layer.</summary>
        private static void DrainEvents()
        {
            if (sitesDied) { sitesDied = false; active.RemoveAll(deadSite); PruneDead(near); cursor = 0; }
            if (dirty.Count == 0) return;
            foreach (var v in dirty) Admit(v);
            dirty.Clear();
        }

        /// <summary>Safety net for spawners without a seam, every FallbackInterval: when the geoscape hierarchyCount moved,
        /// queue every near site for an amortised re-walk (RewalkPerFrame per frame); when it shrank, drop destroyed keys
        /// from the maps (cheap managed liveness checks, but ~17k of them - not per frame). Without the OnEnable seam the
        /// scene-wide site search runs here too.</summary>
        private static void Fallback()
        {
            float now = Time.unscaledTime;
            if (now < nextScan) return;
            nextScan = now + FallbackInterval;
            int hc = hcRoot ? hcRoot.hierarchyCount : -1;
            if (hc >= 0 && hc == walkedHc) return;
            fallbackPasses++;
            if (hc < walkedHc)
            {
                PruneDead(near); PruneDead(origLayers); PruneDead(origWorldCam); PruneDead(camMasks); PruneDead(lightMasks); PruneDead(lightModes);
                active.RemoveAll(deadSite); cursor = 0;
            }
            if (!Hooked || hc < 0) { FullScan(); if (hc < 0 && hcRoot) hc = hcRoot.hierarchyCount; }
            LightLayer();
            ScanLoose();   // an aircraft or globe marker the seams did not see (instantiated inactive, seam missing)
            for (int i = 0; i < loose.Count; i++) loose[i].Kids = -1;   // something spawned somewhere: UpdateLoose re-walks the near ones this frame
            walkedHc = hc;
            rewalk.Clear();
            foreach (var kv in near) if (kv.Value && kv.Key) rewalk.Enqueue(kv.Key);
        }

        private static void Rewalk()
        {
            for (int k = 0; k < RewalkPerFrame && rewalk.Count > 0; k++)
            {
                var v = rewalk.Dequeue();
                bool isNear;
                if (v && v.VisualsContainer && near.TryGetValue(v, out isNear) && isNear) Layer(v, true);
            }
        }

        /// <summary>Harmony seams (GeoSiteVisuals_* patches): a site enabled / something spawned under it. Cheap no-op while unarmed.</summary>
        internal static void MarkDirty(GeoSiteVisualsController v)
        {
            if (M == null || ReferenceEquals(v, null)) return;
            dirty.Add(v);
        }

        internal static void SiteDestroyed() { if (M != null) sitesDied = true; }

        /// <summary>VisualsContainer subtree onto the marker layer (take = true) or back to the recorded originals
        /// (take = false); the SiteSpecialAddonContainer / SiteUniqueAddonContainer subtrees (3D haven-zone models,
        /// GeoSiteVisualsController.cs:50,52) are pruned and stay with the upscaler. The site's WorldSpace root canvas
        /// follows: worldCamera = M while taken, the recorded original while vanilla.</summary>
        private static void Layer(GeoSiteVisualsController v, bool take)
        {
            Transform root = v.VisualsContainer;
            Transform a = v.SiteSpecialAddonContainer ? v.SiteSpecialAddonContainer.transform : null;
            Transform b = v.SiteUniqueAddonContainer ? v.SiteUniqueAddonContainer.transform : null;
            if (a == root) a = null;   // never prune the root itself
            if (b == root) b = null;
            Walk(root, a, b, take);
            var cv = v.CanvasIcons;
            if (take && cv && cv.isRootCanvas && cv.renderMode == RenderMode.WorldSpace && !origWorldCam.ContainsKey(cv)) origWorldCam[cv] = cv.worldCamera;
            Camera orig;
            if (cv && origWorldCam.TryGetValue(cv, out orig)) cv.worldCamera = take ? M : orig;
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
        /// centre GlobeUnits.GlobeCenter, radius GlobeUnits.GlobeRadius) goes back to vanilla rendering, where the globe's depth
        /// hides it. Only the sites that changed side are re-walked.
        /// ponytail: the test is on the pivot, so a marker pops as a whole at the horizon instead of sinking; a quarter of
        /// the active sites per frame, so a side change lands up to 4 frames late. Per-renderer bounds / all sites per
        /// frame if either is ever visible.</summary>
        private static void UpdateHorizon()
        {
            int n = active.Count;
            if (n == 0) return;
            if (cursor >= n) cursor = 0;
            Vector3 c = geo.transform.position, g = GlobeUnits.GlobeCenter;
            float r = GlobeUnits.GlobeRadius, r2 = r * r;
            for (int k = Mathf.Max(1, n / 4); k > 0; k--)
            {
                var v = active[cursor];
                cursor = (cursor + 1) % n;
                if (!v || !v.VisualsContainer) continue;
                bool visible = FacesCamera(v.VisualsContainer.position, c, g, r2);
                bool was;
                if (!near.TryGetValue(v, out was) || visible == was) continue;
                near[v] = visible;
                Layer(v, visible);
            }
        }

        // ---------------------------------------------------------------- aircraft and globe markers
        // A GeoVehicle's picture (decompile GeoVehicle.cs) is `GlobeOffset` (VisualsRoot :316): `Visuals` (MeshRoot :317 -
        // the lit 3D model, its base disc, blob shadow, selection circles; disengage visuals are spawned under it :378),
        // `Trails` and the `RangeIndicator` decal. Only MeshRoot is taken: the model is opaque and writes depth, so the
        // site labels behind it are hidden again exactly as in the scene (left to the scene camera it was a
        // render-resolution model UNDER every label). A GlobeMarker (GlobeMarker.cs: the pulsing ground ring of a
        // destination / selected site, a sibling of the site's SphereOffset) is taken whole.
        // Left with the upscaler: range rings and travel paths (MaterialDecals - a projection box that needs the
        // deferred G-buffer and the globe's depth; WalkGuarded never enters one) and the aircraft Trails (a ribbon along
        // the flight path can reach behind the limb, and the cleared depth has no globe to hide it).

        /// <summary>Seams (GeoVehicle.Initialize, GlobeMarker.Awake). Cheap no-op while unarmed.</summary>
        internal static void LooseSpawned(Object o)
        {
            if (M == null || ReferenceEquals(o, null)) return;
            looseSpawned.Add(o);
        }

        private static void ScanLoose()
        {
            foreach (var v in Object.FindObjectsOfType<GeoVehicle>()) AddLoose(v);
            foreach (var m in Object.FindObjectsOfType<GlobeMarker>()) AddLoose(m);
        }

        private static void AddLoose(Object o)
        {
            if (!o || !looseKnown.Add(o)) return;
            var k = new Loose { Owner = o, V = o as GeoVehicle };
            var m = o as GlobeMarker;
            if (m != null) { k.Root = m.transform; k.Pivot = k.Root.childCount > 0 ? k.Root.GetChild(0) : k.Root; }
            loose.Add(k);
        }

        /// <summary>Every aircraft and globe marker, every frame (a few dozen): the same segment/sphere test as the sites,
        /// on the pivot (an aircraft's carries the flight altitude) - behind the globe the object goes back to its own
        /// layers and the globe's depth hides it. A near one is re-walked when something was spawned under its root.</summary>
        private static void UpdateLoose()
        {
            if (looseSpawned.Count > 0) { foreach (var o in looseSpawned) AddLoose(o); looseSpawned.Clear(); }
            Vector3 c = geo.transform.position, g = GlobeUnits.GlobeCenter;
            float r2 = GlobeUnits.GlobeRadius * GlobeUnits.GlobeRadius;
            for (int i = loose.Count - 1; i >= 0; i--)
            {
                var k = loose[i];
                if (!k.Owner) { looseKnown.Remove(k.Owner); loose.RemoveAt(i); continue; }   // its transforms died with it; the fallback prunes origLayers
                if (!k.Root)
                {
                    var mr = k.V ? k.V.MeshRoot : null;
                    if (!mr) continue;
                    k.Root = k.Pivot = mr.transform; k.Near = false; k.Kids = -1;
                }
                if (!k.Pivot) k.Pivot = k.Root;
                bool visible = FacesCamera(k.Pivot.position, c, g, r2) && (Diagnostics.MarkerCraft || k.V == null);
                int kids = k.Root.childCount;
                if (visible == k.Near && (!visible || kids == k.Kids)) continue;
                k.Near = visible; k.Kids = kids;
                WalkGuarded(k.Root, visible);
            }
            // The pose every near aircraft had one frame ago, for PoseLag: a flying one moves every frame.
            int f = Time.frameCount;
            for (int i = 0; i < loose.Count; i++)
            {
                var k = loose[i];
                if (k.V == null || !k.Near || !k.Root) continue;
                Vector3 p = k.Root.position; Quaternion q = k.Root.rotation;
                bool fresh = k.PoseFrame == f - 1;
                k.PrevPos = fresh ? k.Pos : p; k.PrevRot = fresh ? k.Rot : q;
                k.Pos = p; k.Rot = q; k.PoseFrame = f;
            }
        }

        /// <summary>Direct3D 12 presents the scene one frame late: the shim's list runs before Unity's own of the same frame
        /// (docs\DESIGN.md "D3D12 frame pairing"), so the outRT the present camera blits in frame N holds what GeoscapeCamera
        /// rendered in frame N-1. Drawn with frame N's camera the markers led the globe by one frame of camera motion
        /// (measured: 10 px at 30 fps under an orbit; 0 on D3D11) and a flying aircraft led its own trails and range ring.
        /// So on D3D12 the present camera draws frame N-1: the previous pose and projection (Sync) and, for the aircraft
        /// that moved, the previous world pose - put on the transform for the present camera's cull and render only.</summary>
        private static bool PoseLag => Diagnostics.MarkerPoseDelay < 0 ? Availability.IsD3D12 : Diagnostics.MarkerPoseDelay > 0;

        private static void ShiftCrafts()
        {
            int f = Time.frameCount;
            for (int i = 0; i < loose.Count; i++)
            {
                var k = loose[i];
                if (k.V == null || !k.Near || k.Shifted || k.PoseFrame != f || !k.Root || (k.PrevPos == k.Pos && k.PrevRot == k.Rot)) continue;
                k.HeldPos = k.Root.localPosition; k.HeldRot = k.Root.localRotation; k.Shifted = true;
                k.Root.SetPositionAndRotation(k.PrevPos, k.PrevRot);
            }
        }

        private static void UnshiftCrafts()
        {
            for (int i = 0; i < loose.Count; i++)
            {
                var k = loose[i];
                if (!k.Shifted) continue;
                k.Shifted = false;
                if (k.Root) { k.Root.localPosition = k.HeldPos; k.Root.localRotation = k.HeldRot; }
            }
        }

        /// <summary>Walk with two exclusions: a GameObject carrying a Collider keeps its layer (an aircraft's PickingCollider,
        /// layer 20 - picking is a physics raycast, and GeoscapeCamera's mask is everything, so the "ever drawn by it"
        /// test cannot tell) and a MaterialDecalComponent subtree is not entered (deferred-only projection volume).</summary>
        private static void WalkGuarded(Transform t, bool take)
        {
            if (t.GetComponent<MaterialDecalComponent>() != null) return;
            var go = t.gameObject;
            int orig;
            if (!take) { if (origLayers.TryGetValue(t, out orig)) go.layer = orig; }
            else if (go.layer != layer && t.GetComponent<Collider>() == null
                     && (origLayers.ContainsKey(t) || (origMask & (1 << go.layer)) != 0))
            {
                if (!origLayers.ContainsKey(t)) origLayers[t] = go.layer;
                go.layer = layer;
            }
            for (int i = 0, n = t.childCount; i < n; i++) WalkGuarded(t.GetChild(i), take);
        }

        /// <summary>False when the segment camera c -> p crosses the globe sphere (centre g, radius^2 = r2; GlobeUnits holds the
        /// GlobeCollider's transform position and its unscaled SphereCollider.radius, GeoLevelController.cs:444 - the globe is unscaled).</summary>
        private static bool FacesCamera(Vector3 p, Vector3 c, Vector3 g, float r2)
        {
            Vector3 d = p - c, cg = c - g;
            float dd = d.sqrMagnitude;
            if (dd <= 1e-6f) return true;
            float t = -Vector3.Dot(cg, d) / dd;
            return t <= 0f || t >= 1f || (cg + d * t).sqrMagnitude >= r2;
        }

        /// <summary>GeoscapeCamera's own pre-cull: every layer-ownership change (mask re-assert, rescan, horizon) lands here,
        /// BEFORE it culls, so it and M draw this frame with the same near/far split. Done from M's OnPreCull (after
        /// GeoscapeCamera rendered) a side change dropped a marker for a frame (near->far) or doubled it (far->near).</summary>
        private static void OnScenePreCull(Camera c)
        {
            if (c != geo || M == null || pendingFail != null) return;
            try
            {
                geo.cullingMask &= ~(1 << layer);       // re-asserted: the game may rewrite it
                DrainEvents();
                Fallback();
                Rewalk();
                UpdateHorizon();
                UpdateLoose();
            }
            catch (Exception ex) { pendingFail = ex; }   // never Restore inside a render callback: the next Tick does
        }

        /// <summary>On M: pose + projection copy only. GeoscapeCamera already rendered (depth order) and
        /// DlssDriver.OnCameraPostRender reset its jitter.</summary>
        private sealed class Sync : MonoBehaviour
        {
            private Vector3 pPos;
            private Quaternion pRot;
            private Matrix4x4 pProj;
            private float pFov;
            private bool have;

            private void OnPreCull()
            {
                if (geo == null || M == null || pendingFail != null) return;
                try
                {
                    Vector3 pos = geo.transform.position;
                    Quaternion rot = geo.transform.rotation;
                    Matrix4x4 proj = geo.projectionMatrix;
                    float fov = geo.fieldOfView;
                    M.rect = geo.rect;
                    M.orthographic = geo.orthographic;
                    M.orthographicSize = geo.orthographicSize;
                    M.nearClipPlane = geo.nearClipPlane;
                    M.farClipPlane = geo.farClipPlane;
                    bool lag = have && PoseLag;
                    if (lag) ShiftCrafts();
                    M.transform.SetPositionAndRotation(lag ? pPos : pos, lag ? pRot : rot);
                    M.fieldOfView = lag ? pFov : fov;
                    M.projectionMatrix = lag ? pProj : proj;
                    pPos = pos; pRot = rot; pProj = proj; pFov = fov; have = true;
                }
                catch (Exception ex) { pendingFail = ex; }
            }

            private void OnPostRender()
            {
                try { UnshiftCrafts(); }
                catch (Exception ex) { pendingFail = ex; }
            }
        }

        // ---------------------------------------------------------------- PPCLI diagnostics

        internal static string Status()
        {
            if (M == null) return "inactive layer=" + layer + " gate=" + (lastGate ?? "-");
            int onNear = near.Count(kv => kv.Key && kv.Value);
            string g = geo ? geo.name : "destroyed";
            return "active layer=" + layer + " sites=" + near.Count + " activeSites=" + active.Count + " onNear=" + onNear
                 + " aircraft=" + loose.Count(k => k.V != null) + " aircraftNear=" + loose.Count(k => k.V != null && k.Near) + " globeMarkers=" + loose.Count(k => k.V == null) + " globeMarkersNear=" + loose.Count(k => k.V == null && k.Near) + " craftHooked=" + CraftHooked + " globeMarkerHooked=" + GlobeMarkerHooked + " poseDelay=" + Diagnostics.MarkerPoseDelay + " lights=" + lightMasks.Count + "/" + lightModes.Count + " objects=" + origLayers.Count + " canvases=" + origWorldCam.Count + " cameras=" + camMasks.Count
                 + " hooked=" + Hooked + " fullScans=" + fullScans + " eventWalks=" + eventWalks + " fallbackPasses=" + fallbackPasses + " rewalkQueue=" + rewalk.Count
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

        /// <summary>The same check for aircraft: every shown near-side one is projected through M and picked back through
        /// GeoscapeCamera (its PickingCollider keeps layer 20, WalkGuarded).</summary>
        internal static string ProbeCraftClick()
        {
            if (M == null || !geo) return "inactive";
            var sb = new System.Text.StringBuilder();
            int n = 0, same = 0;
            foreach (var k in loose)
            {
                if (!k.V || !k.Root || !k.Near || !k.Root.gameObject.activeInHierarchy) continue;
                Vector3 sp = M.WorldToScreenPoint(k.Root.position);
                if (sp.z <= 0f || sp.x < 0 || sp.y < 0 || sp.x >= Screen.width || sp.y >= Screen.height) continue;
                RaycastHit hit;
                bool ok = Physics.Raycast(geo.ScreenPointToRay(sp), out hit, geo.farClipPlane);
                var v = ok ? hit.collider.GetComponentInParent<GeoVehicle>() : null;
                n++; if (v == k.V) same++;
                sb.Append(" | ").Append(k.V.name).Append(" screen=").Append(sp.x.ToString("F1")).Append(',').Append(sp.y.ToString("F1"))
                  .Append(" layer=").Append(k.Root.gameObject.layer).Append(" hit=").Append(ok ? hit.collider.name + " [" + hit.collider.gameObject.layer + "]" : "none")
                  .Append(" sameCraft=").Append(v == k.V);
            }
            return "aircraftOnScreen=" + n + " sameCraft=" + same + sb;
        }

        /// <summary>Diagnostic: the real backbuffer at the end of this frame (PPCLI's screenshot re-renders the cameras itself and
        /// loses the present blit once a second scene camera exists) as raw RGB24, plus `path.txt` with size, frame number
        /// and the screen position + name of the site marker nearest the screen centre (the sweep metrics crop around it).</summary>
        /// <summary>Diagnostic: when set, DumpScreen anchors `base` on the site with this GameObject name instead of the one nearest the screen centre.</summary>
        internal static string DumpAnchor;

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
                Vector3 c = cam.transform.position, g = GlobeUnits.GlobeCenter; float r2 = GlobeUnits.GlobeRadius * GlobeUnits.GlobeRadius;
                var centre = new Vector2(w / 2f, h / 2f);
                foreach (var v in Object.FindObjectsOfType<GeoSiteVisualsController>())
                {
                    if (!v.VisualsContainer || !v.LocationIconParent || !v.LocationIconParent.gameObject.activeInHierarchy) continue;
                    Vector3 p = v.VisualsContainer.position;
                    if (!FacesCamera(p, c, g, r2)) continue;
                    Vector3 sp = cam.WorldToScreenPoint(p);
                    if (sp.z <= 0f) continue;
                    float dd = ((Vector2)sp - centre).sqrMagnitude;
                    if (DumpAnchor != null) { if (v.name != DumpAnchor) continue; dd = 0f; }
                    if (dd < bestD) { bestD = dd; best = sp; site = v.name.Replace(' ', '_'); }
                }
            }
            // Every shown aircraft on the near side, for the aircraft crops (screen px, y up like `base`).
            var craft = new System.Text.StringBuilder();
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            if (cam)
            {
                Vector3 c = cam.transform.position, g = GlobeUnits.GlobeCenter; float r2 = GlobeUnits.GlobeRadius * GlobeUnits.GlobeRadius;
                foreach (var v in Object.FindObjectsOfType<GeoVehicle>())
                {
                    if (!v.MeshRoot || !v.MeshRoot.activeInHierarchy || !FacesCamera(v.MeshRoot.transform.position, c, g, r2)) continue;
                    Vector3 sp = cam.WorldToScreenPoint(v.MeshRoot.transform.position);
                    if (sp.z <= 0f || sp.x < 0 || sp.y < 0 || sp.x >= w || sp.y >= h) continue;
                    craft.Append(" craft=").Append(sp.x.ToString("F1", inv)).Append(';').Append(sp.y.ToString("F1", inv)).Append(';').Append(v.name.Replace(' ', '_')).Append(';').Append(v.MeshRoot.layer);
                }
            }
            System.IO.File.WriteAllText(path + ".txt", "w=" + w + " h=" + h + " frame=" + Time.frameCount + " t=" + Time.unscaledTime.ToString("F3") + " base=" + best.x.ToString("F1") + "," + best.y.ToString("F1") + " overlay=" + (M != null) + " site=" + site + craft + "\n");
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

    // ---------------------------------------------------------------- marker seams (decompile GeoSiteVisualsController.cs)
    // Every place that brings a marker object into a site's VisualsContainer tree: OnEnable :167 (site spawned by
    // GeoActorSpawner or re-activated), AddSiteDetailsVisuals :175 (GeoSite.cs:939), RefreshAnimations :404 (highlight
    // under LocationIconParent :410), RefreshMissionVisuals :595 (diplomatic :601 / mission :627 under VisualsContainer).
    // Haven-zone addons (:514/:519) go under the pruned SiteAddon containers; RefreshSiteAddons :542 has no caller.
    // Prepare = false (method or field renamed by a game update) skips that seam; the 2 s fallback still covers it.

    [HarmonyPatch(typeof(GeoSiteVisualsController), nameof(GeoSiteVisualsController.OnEnable))]
    internal static class GeoSiteVisuals_OnEnable_Patch
    {
        static bool Prepare() => GeoMarkerOverlay.Hooked = AccessTools.Method(typeof(GeoSiteVisualsController), nameof(GeoSiteVisualsController.OnEnable)) != null;
        static void Postfix(GeoSiteVisualsController __instance) => GeoMarkerOverlay.MarkDirty(__instance);
    }

    /// <summary>GeoVehicle.Initialize (GeoVehicle.cs:312) resolves MeshRoot: an aircraft built, bought or revealed mid-campaign.</summary>
    [HarmonyPatch(typeof(GeoVehicle), nameof(GeoVehicle.Initialize))]
    internal static class GeoVehicle_Initialize_Patch
    {
        static bool Prepare() => GeoMarkerOverlay.CraftHooked = AccessTools.Method(typeof(GeoVehicle), nameof(GeoVehicle.Initialize)) != null;
        static void Postfix(GeoVehicle __instance) => GeoMarkerOverlay.LooseSpawned(__instance);
    }

    /// <summary>GlobeMarker.Awake (GlobeMarker.cs:46): a ground marker instantiated under a site (destination, selection).</summary>
    [HarmonyPatch(typeof(GlobeMarker), "Awake")]
    internal static class GlobeMarker_Awake_Patch
    {
        static bool Prepare() => GeoMarkerOverlay.GlobeMarkerHooked = AccessTools.Method(typeof(GlobeMarker), "Awake") != null;
        static void Postfix(GlobeMarker __instance) => GeoMarkerOverlay.LooseSpawned(__instance);
    }

    [HarmonyPatch(typeof(GeoSiteVisualsController), "OnDestroy")]
    internal static class GeoSiteVisuals_OnDestroy_Patch
    {
        static bool Prepare() => AccessTools.Method(typeof(GeoSiteVisualsController), "OnDestroy") != null;
        static void Postfix() => GeoMarkerOverlay.SiteDestroyed();
    }

    [HarmonyPatch(typeof(GeoSiteVisualsController), nameof(GeoSiteVisualsController.AddSiteDetailsVisuals))]
    internal static class GeoSiteVisuals_AddSiteDetailsVisuals_Patch
    {
        static bool Prepare() => AccessTools.Method(typeof(GeoSiteVisualsController), nameof(GeoSiteVisualsController.AddSiteDetailsVisuals)) != null;
        static void Postfix(GeoSiteVisualsController __instance) => GeoMarkerOverlay.MarkDirty(__instance);
    }

    [HarmonyPatch(typeof(GeoSiteVisualsController), nameof(GeoSiteVisualsController.RefreshAnimations))]
    internal static class GeoSiteVisuals_RefreshAnimations_Patch
    {
        static bool Prepare() => AccessTools.Method(typeof(GeoSiteVisualsController), nameof(GeoSiteVisualsController.RefreshAnimations)) != null
                              && AccessTools.Field(typeof(GeoSiteVisualsController), "_highlight") != null;
        static void Prefix(GameObject ____highlight, out GameObject __state) => __state = ____highlight;
        static void Postfix(GeoSiteVisualsController __instance, GameObject ____highlight, GameObject __state)
        {
            if (!ReferenceEquals(____highlight, null) && !ReferenceEquals(____highlight, __state)) GeoMarkerOverlay.MarkDirty(__instance);
        }
    }

    /// <summary>Called on every site refresh: only a NEW mission / diplomatic visual marks the site (a refresh without a
    /// spawn must not re-walk it - a day tick refreshes every site at once).</summary>
    [HarmonyPatch(typeof(GeoSiteVisualsController), "RefreshMissionVisuals")]
    internal static class GeoSiteVisuals_RefreshMissionVisuals_Patch
    {
        internal struct Before { public Object Mission, Diplomatic; }

        static bool Prepare() => AccessTools.Method(typeof(GeoSiteVisualsController), "RefreshMissionVisuals") != null
                              && AccessTools.Field(typeof(GeoSiteVisualsController), "_missionVisualsController") != null
                              && AccessTools.Field(typeof(GeoSiteVisualsController), "_diplomaticObjectiveController") != null;

        static void Prefix(GeoUpdatedableMissionVisualsController ____missionVisualsController,
                           GeoDiplomaticObjectiveVisualsController ____diplomaticObjectiveController, out Before __state)
            => __state = new Before { Mission = ____missionVisualsController, Diplomatic = ____diplomaticObjectiveController };

        static void Postfix(GeoSiteVisualsController __instance, GeoUpdatedableMissionVisualsController ____missionVisualsController,
                            GeoDiplomaticObjectiveVisualsController ____diplomaticObjectiveController, Before __state)
        {
            if ((!ReferenceEquals(____missionVisualsController, null) && !ReferenceEquals(____missionVisualsController, __state.Mission))
                || (!ReferenceEquals(____diplomaticObjectiveController, null) && !ReferenceEquals(____diplomaticObjectiveController, __state.Diplomatic)))
                GeoMarkerOverlay.MarkDirty(__instance);
        }
    }
}
