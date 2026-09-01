using System;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

namespace DlssMod
{
    /// <summary>Rendering driver (docs/DESIGN.md "DLSS.dll"). Lives on a persistent GameObject and drives the scene camera
    /// it was attached to: scene camera -> colorRT (render res) -> [CB BeforeImageEffects: copy MV + depth] -> PPv2 ->
    /// [CB AfterEverything: native event 2 -> outRT] -> present camera [CB: Blit outRT -> backbuffer] -> HUD.</summary>
    public class DlssDriver : MonoBehaviour
    {
        public static DlssDriver Instance { get; private set; }

        private enum Gen { Idle, Creating, Live, Releasing }

        private Camera cam;
        private PostProcessLayer layer;
        private RenderTexture colorRT, depthRT, mvRT, outRT;
        private IntPtr colorPtr, depthPtr, mvPtr, outPtr;
        private CommandBuffer cbCopy, cbEval, cbPresent;
        private Camera present;
        private IntPtr evFn, evDataFn;

        private DlssMode wantMode = DlssMode.Off;
        private DebugView wantView = DebugView.None;
        private Gen gen = Gen.Idle;
        private int genFrames;
        private DlssMode liveMode;
        private DebugView liveView;
        private bool passthrough;
        private int renderW, renderH, outW, outH, quality;
        private bool wantsFeature;          // false in Passthrough: no NGX feature is created at all

        private int jitterIndex, phaseCount;
        private float jx, jy;
        private bool resetNext;
        private Vector3 lastPos;
        private float lastFov;

        private bool depthModeSaved;
        private DepthTextureMode savedDepthMode;
        private bool aaSaved;
        private PostProcessLayer.Antialiasing savedAA;

        private long frames, resets;
        private string lastFail = "";
        private bool broken;                // threw once inside a Unity callback -> self-disabled

        public bool IsLive => gen == Gen.Live;
        public bool Passthrough => passthrough;
        public Camera SceneCamera => cam;
        public PostProcessLayer Layer => layer;
        public DlssMode LiveMode => liveMode;
        public int Quality => quality;
        public int RenderW => renderW;
        public int RenderH => renderH;
        public int OutW => outW;
        public int OutH => outH;

        public static DlssDriver Create()
        {
            if (Instance != null) return Instance;
            var go = new GameObject("DlssDriver") { hideFlags = HideFlags.HideAndDontSave };
            DontDestroyOnLoad(go);
            Instance = go.AddComponent<DlssDriver>();
            return Instance;
        }

        private void Awake()
        {
            evFn = Native.Dlss_GetRenderEventFunc();
            evDataFn = Native.Dlss_GetRenderEventAndDataFunc();
            Camera.onPostRender += OnCameraPostRender;
        }

        private void OnDestroy()
        {
            Camera.onPostRender -= OnCameraPostRender;
            try { Detach(); TeardownResources(); } catch (Exception) { }
            if (Instance == this) Instance = null;
        }

        /// <summary>Bind to the scene camera (CameraManager.Camera). A different camera than the live one = Off first.</summary>
        public void Attach(Camera sceneCamera)
        {
            if (sceneCamera == null || cam == sceneCamera) return;
            if (gen == Gen.Live || gen == Gen.Creating) BeginRelease();   // old camera detached now; Idle re-creates on the new one
            cam = sceneCamera;
            layer = cam.GetComponent<PostProcessLayer>();
        }

        public void Apply(DlssMode mode, DebugView view)
        {
            wantMode = mode;
            wantView = view;
            if (gen == Gen.Live && liveMode == wantMode && liveView != wantView && SameSizeClass(liveView, wantView))
            {
                liveView = wantView;      // only the present source changes
                BuildPresent();
            }
        }

        private static bool SameSizeClass(DebugView a, DebugView b) => (a == DebugView.Passthrough) == (b == DebugView.Passthrough);

        public string Status
        {
            get
            {
                int c, e, alive; int init = Native.Dlss_Status(out c, out e, out alive);
                return "gen=" + gen + " mode=" + liveMode + " view=" + liveView + " want=" + wantMode + "/" + wantView
                     + " render=" + renderW + "x" + renderH + " out=" + outW + "x" + outH + " screen=" + Screen.width + "x" + Screen.height
                     + " q=" + quality + " passthrough=" + passthrough + " frames=" + frames + " resets=" + resets + " fov=" + (cam ? cam.fieldOfView.ToString("F3") : "-") + " jitter=" + jx.ToString("F3") + "," + jy.ToString("F3")
                     + " init=" + init + " create=0x" + c.ToString("X") + "(" + Native.Dlss_ResultString(c) + ") eval=0x" + e.ToString("X") + "(" + Native.Dlss_ResultString(e) + ")"
                     + " feature=" + alive + " lastError=" + Native.Dlss_LastError()
                     + " cam=" + (cam ? cam.name : "null") + " target=" + (cam && cam.targetTexture ? cam.targetTexture.name : "null")
                     + " depthMode=" + (cam ? cam.depthTextureMode.ToString() : "-") + " aa=" + (layer ? layer.antialiasingMode.ToString() : "-")
                     + " colorSpace=" + QualitySettings.activeColorSpace + " reversedZ=" + SystemInfo.usesReversedZBuffer
                     + " path=" + (cam ? cam.actualRenderingPath.ToString() : "-")
                     + " present=" + (present ? (present.enabled ? "on" : "off") : "none") + " broken=" + broken + " fail=" + lastFail;
            }
        }

        // ---------------------------------------------------------------- generation state machine (main thread)

        private void Update()
        {
            var cfg = DlssMod.Instance?.Cfg;
            if (cfg != null && (Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl))
                            && (Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt)))
            {
                if (Input.GetKeyDown(cfg.ToggleHotkey)) DlssMod.Toggle();
                if (Input.GetKeyDown(cfg.OverlayHotkey)) DlssMod.ToggleOverlay();
            }
            if (broken) return;
            try { Step(); }
            catch (Exception ex) { Fail("Update threw: " + ex); }
        }

        private void Step()
        {
            switch (gen)
            {
                case Gen.Idle:
                    if (wantMode != DlssMode.Off && cam != null && cam.isActiveAndEnabled) StartGeneration();
                    break;
                case Gen.Creating:
                    if (++genFrames < 2) break;
                    if (wantsFeature)
                    {
                        int c, e, alive; Native.Dlss_Status(out c, out e, out alive);
                        if (alive == 0) { Fail("NGX create failed: 0x" + c.ToString("X") + " " + Native.Dlss_ResultString(c)); break; }
                    }
                    gen = Gen.Live;
                    resetNext = true;
                    AttachCamera();
                    break;
                case Gen.Live:
                    if (cam == null) { Fail("scene camera destroyed while live"); break; }
                    // Bound camera deactivated (CameraManager swapped to another one): a present camera left on would
                    // blit a stale outRT over whatever renders now. Release; Idle re-creates on the rebound camera.
                    if (!cam.isActiveAndEnabled || wantMode == DlssMode.Off || wantMode != liveMode || !SameSizeClass(liveView, wantView)
                        || Screen.width != outW || Screen.height != outH)
                    {
                        BeginRelease();
                        break;
                    }
                    if (wantsFeature && frames > 2)
                    {
                        int c, e, alive; Native.Dlss_Status(out c, out e, out alive);
                        if ((e & 0xFFF00000) == 0xBAD00000) { Fail("NGX evaluate failed: 0x" + e.ToString("X") + " " + Native.Dlss_ResultString(e) + " lastError=" + Native.Dlss_LastError()); break; }
                    }
                    KeepCameraState();
                    break;
                case Gen.Releasing:
                    if (++genFrames < 2) break;       // two frames after event 3: no in-flight event touches a dead RT
                    TeardownResources();
                    gen = Gen.Idle;
                    break;
            }
        }

        private void Fail(string why)
        {
            lastFail = why;
            DlssMod.Instance?.Logger.LogError("DLSS off: " + why);
            wantMode = DlssMode.Off;
            if (gen == Gen.Live || gen == Gen.Creating) BeginRelease();
            if (why.StartsWith("Update threw") || why.StartsWith("callback threw")) broken = true;
        }

        private void StartGeneration()
        {
            outW = Screen.width; outH = Screen.height;
            liveMode = wantMode; liveView = wantView;
            passthrough = liveView == DebugView.Passthrough;
            wantsFeature = !passthrough;
            quality = QualityFor(liveMode, outH);
            renderW = outW; renderH = outH;
            if (wantsFeature && quality != Native.DLSS_Q_DLAA)
            {
                uint rw, rh, a, b, c, d;
                int r = Native.Dlss_GetOptimal((uint)outW, (uint)outH, quality, out rw, out rh, out a, out b, out c, out d);
                if (r != Native.NGX_SUCCESS || rw == 0 || rh == 0) { Fail("GetOptimal failed 0x" + r.ToString("X")); return; }
                renderW = (int)rw; renderH = (int)rh;
            }
            float ratio = (float)outW / renderW;
            phaseCount = Mathf.Max(8, Mathf.RoundToInt(8f * ratio * ratio));
            jitterIndex = 0;

            colorRT = Make("DLSS color", renderW, renderH, RenderTextureFormat.ARGB32, false);
            depthRT = Make("DLSS depth", renderW, renderH, RenderTextureFormat.RFloat, false);
            mvRT = Make("DLSS mv", renderW, renderH, RenderTextureFormat.RGHalf, false);
            outRT = Make("DLSS out", outW, outH, RenderTextureFormat.ARGB32, true);
            colorPtr = colorRT.GetNativeTexturePtr(); depthPtr = depthRT.GetNativeTexturePtr();
            mvPtr = mvRT.GetNativeTexturePtr(); outPtr = outRT.GetNativeTexturePtr();

            cbCopy = new CommandBuffer { name = "DLSS copy depth+mv" };
            cbCopy.CopyTexture(BuiltinRenderTextureType.MotionVectors, mvRT);
            cbCopy.Blit(BuiltinRenderTextureType.Depth, depthRT);
            cbEval = new CommandBuffer { name = "DLSS evaluate" };

            var go = new GameObject("DlssPresent") { hideFlags = HideFlags.HideAndDontSave };
            go.transform.SetParent(transform, false);
            present = go.AddComponent<Camera>();
            present.enabled = false;
            present.cullingMask = 0;
            present.clearFlags = CameraClearFlags.Nothing;
            present.targetTexture = null;
            present.allowHDR = false; present.allowMSAA = false; present.useOcclusionCulling = false;
            present.depth = cam.depth + 1;
            cbPresent = new CommandBuffer { name = "DLSS present" };
            present.AddCommandBuffer(CameraEvent.AfterEverything, cbPresent);
            BuildPresent();

            Native.Dlss_Passthrough(passthrough ? 1 : 0);
            if (wantsFeature)
            {
                int flags = Native.DLSS_F_MV_LOW_RES | (SystemInfo.usesReversedZBuffer ? Native.DLSS_F_DEPTH_INVERTED : 0);
                Native.Dlss_SetCreateParams((uint)renderW, (uint)renderH, (uint)outW, (uint)outH, quality, flags);
                GL.IssuePluginEvent(evFn, Native.DLSS_EV_CREATE);
            }
            gen = Gen.Creating; genFrames = 0; frames = 0;
            DlssMod.Instance?.Logger.LogInfo("DLSS generation: mode=" + liveMode + " view=" + liveView + " render=" + renderW + "x" + renderH + " out=" + outW + "x" + outH + " q=" + quality + " phases=" + phaseCount);
        }

        private static RenderTexture Make(string name, int w, int h, RenderTextureFormat fmt, bool uav)
        {
            var rt = new RenderTexture(w, h, 0, fmt) { name = name, enableRandomWrite = uav, filterMode = FilterMode.Point, useMipMap = false, autoGenerateMips = false };
            rt.Create();
            return rt;
        }

        private void BuildPresent()
        {
            if (cbPresent == null) return;
            cbPresent.Clear();
            RenderTexture src = liveView == DebugView.Depth ? depthRT : liveView == DebugView.MotionVectors ? mvRT : outRT;
            cbPresent.Blit(src, BuiltinRenderTextureType.CameraTarget);
        }

        private void AttachCamera()
        {
            if (!depthModeSaved) { savedDepthMode = cam.depthTextureMode; depthModeSaved = true; }
            if (layer != null && !aaSaved) { savedAA = layer.antialiasingMode; aaSaved = true; }
            cam.AddCommandBuffer(CameraEvent.BeforeImageEffects, cbCopy);
            cam.AddCommandBuffer(CameraEvent.AfterEverything, cbEval);
            KeepCameraState();
            present.enabled = true;
            lastPos = cam.transform.position; lastFov = cam.fieldOfView;
        }

        /// <summary>Re-asserted every live frame: PPv2 or the game may touch these between frames.</summary>
        private void KeepCameraState()
        {
            cam.targetTexture = colorRT;
            cam.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
            if (layer != null && layer.antialiasingMode != PostProcessLayer.Antialiasing.None) layer.antialiasingMode = PostProcessLayer.Antialiasing.None;
            present.depth = cam.depth + 1;
        }

        private void Detach()
        {
            if (cam != null)
            {
                if (cbCopy != null) cam.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, cbCopy);
                if (cbEval != null) cam.RemoveCommandBuffer(CameraEvent.AfterEverything, cbEval);
                if (cam.targetTexture == colorRT) cam.targetTexture = null;
                if (depthModeSaved) { cam.depthTextureMode = savedDepthMode; depthModeSaved = false; }
                cam.ResetProjectionMatrix();
            }
            if (layer != null && aaSaved) { layer.antialiasingMode = savedAA; aaSaved = false; }
            if (present != null) present.enabled = false;
        }

        private void BeginRelease()
        {
            Detach();
            if (wantsFeature) GL.IssuePluginEvent(evFn, Native.DLSS_EV_RELEASE);
            Native.Dlss_Passthrough(0);
            gen = Gen.Releasing; genFrames = 0;
        }

        private void TeardownResources()
        {
            if (present != null) { Destroy(present.gameObject); present = null; }
            cbCopy?.Release(); cbEval?.Release(); cbPresent?.Release();
            cbCopy = cbEval = cbPresent = null;
            foreach (var rt in new[] { colorRT, depthRT, mvRT, outRT }) if (rt != null) { rt.Release(); Destroy(rt); }
            colorRT = depthRT = mvRT = outRT = null;
            colorPtr = depthPtr = mvPtr = outPtr = IntPtr.Zero;
        }

        // ---------------------------------------------------------------- per-frame hooks

        /// <summary>Harmony postfix on PostProcessLayer.OnPreCull: projectionMatrix is already reset and
        /// nonJitteredProjectionMatrix already assigned by PPv2, so add the jitter and queue this frame's evaluate.</summary>
        public void AfterPostProcessPreCull(PostProcessLayer l)
        {
            if (broken || gen != Gen.Live || l != layer || cam == null) return;
            try
            {
                Halton(jitterIndex, out jx, out jy);
                jitterIndex = (jitterIndex + 1) % phaseCount;
                if (passthrough) { jx = 0f; jy = 0f; }
                var p = cam.projectionMatrix;
                cam.nonJitteredProjectionMatrix = p;
                // PPv2 sign convention (RuntimeUtilities.GetJitteredPerspectiveProjectionMatrix, decompiled): proj[0,2] += 2*jx/w.
                p[0, 2] += 2f * jx / renderW;
                p[1, 2] += 2f * jy / renderH;
                cam.projectionMatrix = p;
                // TRUE, unlike PPv2 TAA: DLSS needs every surface jittered; transparents rendered with the
                // clean matrix keep hard aliased edges (seen live on the tactical path lines).
                cam.useJitteredProjectionMatrixForTransparentRendering = true;

                int reset = resetNext ? 1 : 0;
                Vector3 pos = cam.transform.position;
                if ((pos - lastPos).sqrMagnitude > 50f * 50f || !Mathf.Approximately(cam.fieldOfView, lastFov)) reset = 1;
                lastPos = pos; lastFov = cam.fieldOfView; resetNext = false;
                if (reset != 0) resets++;

                // Signs (live-verified 2026-09-01, DLAA 1280x720): NGX gets (-jx, -jy). Unity's view space is right-handed
                // (w_clip = -z_view), so "+2jx/w" on proj[0,2] shifts the image by -jx pixels; the same holds for y.
                // Verified by an 8x crop: (+jx,+jy) doubles thin edges, (-jx,-jy) resolves them. Same as HDRP's DLSSPass.
                // MV: Unity's texture is (current - previous) in UV space (PPv2 TAA fetches history at uv - mv); DLSS wants
                // current -> previous in pixels, hence InMVScale = (-renderW, -renderH).
                // Sharpness = our RCAS pass in the shim (NGX InSharpness is deprecated in SDK 310), read live: slider/100.
                float sharp = passthrough ? 0f : Mathf.Clamp01((DlssMod.Instance?.Cfg?.Sharpness ?? 0) / 100f);
                IntPtr slot = Native.Dlss_GetFrameSlot();
                Native.Dlss_SetFrame(slot, colorPtr, depthPtr, mvPtr, outPtr, -jx, -jy, -renderW, -renderH,
                    reset, Time.unscaledDeltaTime * 1000f, (uint)renderW, (uint)renderH, 1f, sharp);
                cbEval.Clear();
                cbEval.IssuePluginEventAndData(evDataFn, Native.DLSS_EV_EVALUATE, slot);
                frames++;
            }
            catch (Exception ex) { Fail("callback threw (OnPreCull): " + ex); }
        }

        private void OnCameraPostRender(Camera c)
        {
            if (gen != Gen.Live || c != cam) return;
            cam.ResetProjectionMatrix();     // picking / UI raycasts see the clean matrix
            GL.InvalidateState();            // Unity re-applies its D3D11 state after NGX touched the context
        }

        /// <summary>Harmony postfix on LightingManager.ApplyPostProcessOptions: it just wrote the preset's SMAA onto every layer.</summary>
        public void AfterApplyPostProcessOptions()
        {
            if (gen != Gen.Live || layer == null) return;
            savedAA = layer.antialiasingMode; aaSaved = true;
            layer.antialiasingMode = PostProcessLayer.Antialiasing.None;
        }

        // ---------------------------------------------------------------- helpers

        private static int QualityFor(DlssMode mode, int outH)
        {
            switch (mode)
            {
                case DlssMode.Auto: return outH <= 1200 ? Native.DLSS_Q_DLAA : outH <= 1600 ? Native.DLSS_Q_QUALITY : Native.DLSS_Q_PERFORMANCE;
                case DlssMode.Quality: return Native.DLSS_Q_QUALITY;
                case DlssMode.Balanced: return Native.DLSS_Q_BALANCED;
                case DlssMode.Performance: return Native.DLSS_Q_PERFORMANCE;
                case DlssMode.UltraPerformance: return Native.DLSS_Q_ULTRA_PERFORMANCE;
                default: return Native.DLSS_Q_DLAA;
            }
        }

        private static void Halton(int index, out float x, out float y)
        {
            x = Radical(index + 1, 2) - 0.5f;
            y = Radical(index + 1, 3) - 0.5f;
        }

        private static float Radical(int i, int b)
        {
            float r = 0f, f = 1f;
            while (i > 0) { f /= b; r += f * (i % b); i /= b; }
            return r;
        }
    }
}
