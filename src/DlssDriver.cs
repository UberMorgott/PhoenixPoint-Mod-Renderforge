using System;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

namespace Renderforge
{
    /// <summary>Rendering driver (docs/DESIGN.md "Renderforge.dll"). Lives on a persistent GameObject and drives the scene camera
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

        private RenderforgeMode wantMode = RenderforgeMode.Off;
        private DebugView wantView = DebugView.None;
        private Gen gen = Gen.Idle;
        private int genFrames;
        private RenderforgeMode liveMode;
        private DebugView liveView;
        private bool passthrough;
        private int renderW, renderH, outW, outH, quality;
        private bool wantsFeature;          // false in Passthrough: no NGX feature is created at all
        private bool liveMvJittered;        // DLSS_F_MV_JITTERED the feature was created with (Cfg.MvJittered, diagnostic)
        private bool liveSrgbViews;         // DLSS_F_SRGB_VIEWS the generation was created with (Cfg.D3D12SrgbViews, D3D12 only)
        private bool liveColorDesc;         // colorRT created from an explicit R8G8B8A8_SRGB descriptor (Cfg.D3D12ColorDesc, D3D12 only)
        private bool liveHalfColor;         // colorRT + outRT linear ARGBHalf, DLSS_F_HDR (Cfg.D3D12HalfColor, D3D12 only)

        private int jitterIndex, phaseCount;
        private float jx, jy;
        private float reportJx, reportJy;   // what the last Dlss_SetFrame actually got (after the diagnostic sign/swap knobs)
        private bool resetNext;
        private Vector3 lastPos;
        private float lastFov;

        private bool depthModeSaved;
        private DepthTextureMode savedDepthMode;
        private bool aaSaved;
        private PostProcessLayer.Antialiasing savedAA;

        private float mipReapplyAt;         // second MipBias sweep 2 s after Live: level content still streaming in
        private long frames, resets;
        private string lastFail = "";
        private bool broken;                // threw once inside a Unity callback -> self-disabled

        public bool IsLive => gen == Gen.Live;
        public bool Passthrough => passthrough;
        public Camera SceneCamera => cam;
        public PostProcessLayer Layer => layer;
        public RenderforgeMode LiveMode => liveMode;
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
            // Fg_Shutdown is synchronous: the chain that references outRT/depthRT/mvRT is gone when Release returns.
            try { FrameGen.Release(); Detach(); TeardownResources(); } catch (Exception) { }
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

        /// <summary>Live provider switch: release the generation now (FG chain, feature, mip bias, two frames for
        /// in-flight events), then Idle re-inits the shim on `kind` and the normal path re-creates on it.</summary>
        public void SwitchProvider(UpscalerKind kind)
        {
            switchTo = kind;
            if (gen == Gen.Live || gen == Gen.Creating) BeginRelease();
            else if (gen == Gen.Idle) genFrames = 0;
        }

        private UpscalerKind switchTo = UpscalerKind.Off;   // Off = no switch pending

        public void Apply(RenderforgeMode mode, DebugView view)
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
                     + " q=" + quality + " passthrough=" + passthrough + " liveMvJittered=" + liveMvJittered + " liveSrgbViews=" + liveSrgbViews + " frames=" + frames + " resets=" + resets + " fov=" + (cam ? cam.fieldOfView.ToString("F3") : "-") + " jitter=" + jx.ToString("F3") + "," + jy.ToString("F3")
                     + " init=" + init + " api=" + Native.Api() + " unityIface=" + Native.UnityIface()
                     + " create=0x" + c.ToString("X") + "(" + Native.Dlss_ResultString(c) + ") eval=0x" + e.ToString("X") + "(" + Native.Dlss_ResultString(e) + ")"
                     + " feature=" + alive + " lastError=" + Native.Dlss_LastError() + " sharpen=" + Native.SharpenerName(Native.Dlss_Sharpener())
                     + " cam=" + (cam ? cam.name : "null") + " target=" + (cam && cam.targetTexture ? cam.targetTexture.name : "null")
                     + " depthMode=" + (cam ? cam.depthTextureMode.ToString() : "-") + " aa=" + (layer ? layer.antialiasingMode.ToString() : "-")
                     + " colorSpace=" + QualitySettings.activeColorSpace + " reversedZ=" + SystemInfo.usesReversedZBuffer
                     + " path=" + (cam ? cam.actualRenderingPath.ToString() : "-")
                     + " present=" + (present ? (present.enabled ? "on" : "off") : "none") + " broken=" + broken + " fail=" + lastFail
                     + " " + Native.Timings();
            }
        }

        // ---------------------------------------------------------------- generation state machine (main thread)

        private void Update()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg != null && (Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl))
                            && (Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt)))
            {
                if (Input.GetKeyDown(cfg.ToggleHotkey)) RenderforgeMod.Toggle();
                if (Input.GetKeyDown(cfg.OverlayHotkey)) RenderforgeMod.ToggleOverlay();
            }
            if (broken) return;
            try { FrameGen.Pump(); Step(); }
            catch (Exception ex) { Fail("Update threw: " + ex); }
        }

        private void Step()
        {
            switch (gen)
            {
                case Gen.Idle:
                    if (switchTo != UpscalerKind.Off)
                    {
                        // The RELEASE event destroys the feature/context on the render thread; Shutdown must not race
                        // it from here (double ffxDestroyContext crashed 2026-09-03), so wait until it reports dead.
                        int c, e, alive; Native.Dlss_Status(out c, out e, out alive);
                        if (alive != 0 && ++genFrames < 120) break;
                        UpscalerKind k = switchTo; switchTo = UpscalerKind.Off;
                        RenderforgeMod.ReinitNative(k);
                    }
                    if (wantMode != RenderforgeMode.Off && cam != null && cam.isActiveAndEnabled && RenderforgeMod.Available) StartGeneration();
                    break;
                case Gen.Creating:
                    if (++genFrames < 2) break;
                    if (wantsFeature)
                    {
                        int c, e, alive; Native.Dlss_Status(out c, out e, out alive);
                        if (alive == 0)
                        {
                            // Every provider reports NGX codes (FSR/XeSS map theirs): only 0xBAD0xxxx is a failure.
                            // 0 / a stale Success means the create event has not landed yet (FSR/XeSS keep the
                            // previous generation's Success across ReleaseFeature) - keep waiting, bounded.
                            bool failed = (c & 0xFFF00000) == 0xBAD00000;
                            if (failed || genFrames >= 120) { Fail("create failed: 0x" + c.ToString("X") + " " + Native.Dlss_ResultString(c) + (failed ? "" : " (feature never came alive)")); }
                            break;
                        }
                    }
                    gen = Gen.Live;
                    resetNext = true;
                    AttachCamera();
                    MipBias.Apply(passthrough ? 0f : Mathf.Log((float)renderW / outW, 2f));
                    mipReapplyAt = Time.unscaledTime + 2f;
                    break;
                case Gen.Live:
                    if (cam == null) { Fail("scene camera destroyed while live"); break; }
                    // Bound camera deactivated (CameraManager swapped to another one): a present camera left on would
                    // blit a stale outRT over whatever renders now. Release; Idle re-creates on the rebound camera.
                    if (!cam.isActiveAndEnabled || wantMode == RenderforgeMode.Off || wantMode != liveMode || !SameSizeClass(liveView, wantView)
                        || Screen.width != outW || Screen.height != outH || liveMvJittered != WantMvJittered || liveSrgbViews != WantSrgbViews || liveColorDesc != WantColorDesc || liveHalfColor != WantHalfColor)
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
                    FrameGen.Retry();
                    if (mipReapplyAt > 0f && Time.unscaledTime >= mipReapplyAt) { mipReapplyAt = 0f; MipBias.Reapply(); }
                    break;
                case Gen.Releasing:
                    if (++genFrames < 2) break;       // two frames after event 3: no in-flight event touches a dead RT
                    TeardownResources();
                    gen = Gen.Idle; genFrames = 0;
                    break;
            }
        }

        private void Fail(string why)
        {
            lastFail = why;
            RenderforgeMod.Instance?.Logger.LogError("DLSS off: " + why);
            wantMode = RenderforgeMode.Off;
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

            // D3D12ColorDesc (diagnostic): on D3D12 the ARGB32/Default RTV is not *_UNORM_SRGB, so PPv2's final Uber blit
            // (the only sRGB encode in the chain) writes linear bytes into an sRGB-tagged RT -> crushed darks. An explicit
            // R8G8B8A8_SRGB descriptor asks for the sRGB RTV outright.
            // D3D12HalfColor: measured 2026-09-03 that on D3D12 an 8-bit sRGB RT used as camera target is NOT sRGB-encoded
            // on write (DumpColorIn luma 6.14 vs 13.77 on D3D11; the explicit R8G8B8A8_SRGB descriptor changed nothing),
            // so linear values land in 8 bits and the darks crush. FP16 linear colour + FP16 linear out avoid 8-bit sRGB
            // storage entirely; DLSS_F_HDR is passed as information only - every SDK stays LDR (NGX IsHDR=0, FSR without
            // HIGH_DYNAMIC_RANGE, XeSS with LDR_INPUT_COLOR: values are display-referred 0..1, no exposure source for HDR mode)
            // and the shim's NIS pass runs its linear variant on the FP16 out twin. Wins over ColorDesc/SrgbViews.
            liveHalfColor = WantHalfColor;
            liveColorDesc = WantColorDesc && !liveHalfColor;
            if (liveHalfColor) colorRT = Make("DLSS color", renderW, renderH, RenderTextureFormat.ARGBHalf, false, RenderTextureReadWrite.Linear);
            else if (liveColorDesc)
            {
                var desc = new RenderTextureDescriptor(renderW, renderH)
                {
                    graphicsFormat = UnityEngine.Experimental.Rendering.GraphicsFormat.R8G8B8A8_SRGB,
                    sRGB = true, depthBufferBits = 0, msaaSamples = 1, enableRandomWrite = false, useMipMap = false, autoGenerateMips = false,
                };
                colorRT = new RenderTexture(desc) { name = "DLSS color", filterMode = FilterMode.Point };
                colorRT.Create();
            }
            else colorRT = Make("DLSS color", renderW, renderH, RenderTextureFormat.ARGB32, false);
            depthRT = Make("DLSS depth", renderW, renderH, RenderTextureFormat.RFloat, false);
            mvRT = Make("DLSS mv", renderW, renderH, RenderTextureFormat.RGHalf, false);
            // D3D12: Unity's present Blit(outRT -> CameraTarget) decodes the sRGB SRV read but does NOT sRGB-encode the
            // backbuffer write the way its D3D11 backend does - one net decode, the frame came out ~2x darker (measured
            // 2026-09-03: menu luma 28 vs 56; Passthrough was dark too, so the shim's bit-exact copies/twins were never
            // at fault). A Linear outRT passes the already-encoded bytes through untouched. D3D11 keeps Default:
            // Linear there double-encodes. colorRT's flag changes nothing (its bytes are the same either way).
            // D3D12SrgbViews (diagnostic) selects the other combination: the shim views the colour input as
            // *_UNORM_SRGB (the SDK decodes to linear - D3D12Owned.h Typed), the SDK writes LINEAR values into a UNORM
            // UAV (no sRGB UAV in D3D12), and outRT stays sRGB-tagged (Default) so Unity's present Blit encodes it.
            liveSrgbViews = WantSrgbViews && !liveHalfColor;
            var outRW = SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12 && !liveSrgbViews ? RenderTextureReadWrite.Linear : RenderTextureReadWrite.Default;
            // HalfColor: the present Blit reads linear FP16; whether Unity's D3D12 backbuffer write encodes it is for the
            // in-game check (if the frame comes out dark, a LinearToSRGB blit belongs here - not added blind).
            outRT = Make("DLSS out", outW, outH, liveHalfColor ? RenderTextureFormat.ARGBHalf : RenderTextureFormat.ARGB32, true, outRW);
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
                liveMvJittered = WantMvJittered;
                int flags = Native.DLSS_F_MV_LOW_RES | (SystemInfo.usesReversedZBuffer ? Native.DLSS_F_DEPTH_INVERTED : 0)
                          | (liveMvJittered ? Native.DLSS_F_MV_JITTERED : 0)
                          | (liveSrgbViews ? Native.DLSS_F_SRGB_VIEWS : 0)
                          | (liveHalfColor ? Native.DLSS_F_HDR : 0);
                Native.Dlss_SetCreateParams((uint)renderW, (uint)renderH, (uint)outW, (uint)outH, quality, flags);
                GL.IssuePluginEvent(evFn, Native.DLSS_EV_CREATE);
            }
            gen = Gen.Creating; genFrames = 0; frames = 0;
            RenderforgeMod.Instance?.Logger.LogInfo("DLSS generation: mode=" + liveMode + " view=" + liveView + " render=" + renderW + "x" + renderH + " out=" + outW + "x" + outH + " q=" + quality + " phases=" + phaseCount + " colorRT=" + colorRT.graphicsFormat + " sRGB=" + colorRT.sRGB + " colorDesc=" + liveColorDesc + " halfColor=" + liveHalfColor + " outRT=" + outRT.graphicsFormat);
        }

        private static RenderTexture Make(string name, int w, int h, RenderTextureFormat fmt, bool uav, RenderTextureReadWrite rw = RenderTextureReadWrite.Default)
        {
            var rt = new RenderTexture(w, h, 0, fmt, rw) { name = name, enableRandomWrite = uav, filterMode = FilterMode.Point, useMipMap = false, autoGenerateMips = false };
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
            FrameGen.Release();    // the FG chain references outRT/depthRT/mvRT and must die before they do
            MipBias.Reset();
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
                var cfg = RenderforgeMod.Instance?.Cfg;
                if (cfg != null && cfg.JitterConstEnabled && !passthrough) { jx = cfg.JitterConstX; jy = cfg.JitterConstY; }   // JitterConst: rendered AND reported
                float jscale = cfg?.JitterScale ?? 1f;
                jx *= jscale; jy *= jscale;          // JitterScale: rendered AND reported jitter (0 = none)
                var p = cam.projectionMatrix;
                cam.nonJitteredProjectionMatrix = p;
                // PPv2 sign convention (RuntimeUtilities.GetJitteredPerspectiveProjectionMatrix, decompiled): proj[0,2] += 2*jx/w.
                p[0, 2] += 2f * jx / renderW;
                p[1, 2] += 2f * jy / renderH;
                cam.projectionMatrix = p;
                // TRUE, unlike PPv2 TAA: DLSS needs every surface jittered; transparents rendered with the
                // clean matrix keep hard aliased edges (seen live on the tactical path lines).
                cam.useJitteredProjectionMatrixForTransparentRendering = true;

                int reset = resetNext || (cfg != null && cfg.ForceReset) ? 1 : 0;   // ForceReset: NGX InReset / FSR reset / XeSS resetHistory every frame
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
                float sharp = passthrough ? 0f : Mathf.Clamp01((RenderforgeMod.Instance?.Cfg?.Sharpness ?? 0) / 100f);
                // FSR needs the camera frustum (cameraNear/Far/FovAngleVertical); NGX ignores it. Cached in the
                // shim and copied into the frame slot, so the ABI of Dlss_SetFrame stays untouched.
                Native.SetCamera(cam.nearClipPlane, cam.farClipPlane, cam.fieldOfView * Mathf.Deg2Rad);
                // Diagnostic (D3D12 detail-loss hunt): sign/swap knobs touch ONLY the offset reported to the SDK, not the projection.
                float rjx = -jx * (cfg?.JitterReportSignX ?? 1), rjy = -jy * (cfg?.JitterReportSignY ?? 1);
                if (cfg != null && cfg.JitterReportSwapXY) { float t = rjx; rjx = rjy; rjy = t; }
                reportJx = rjx; reportJy = rjy;
                IntPtr slot = Native.Dlss_GetFrameSlot();
                Native.Dlss_SetFrame(slot, colorPtr, depthPtr, mvPtr, outPtr, rjx, rjy, -renderW, -renderH,
                    reset, Time.unscaledDeltaTime * 1000f, (uint)renderW, (uint)renderH, 1f, sharp);
                cbEval.Clear();
                cbEval.IssuePluginEventAndData(evDataFn, Native.DLSS_EV_EVALUATE, slot);
                if (FrameGen.Live && !FrameGen.HoldPrepare)
                {
                    var v = cam.worldToCameraMatrix;
                    var pr = cam.nonJitteredProjectionMatrix;
                    float[] view = { v.m00, v.m01, v.m02, v.m03, v.m10, v.m11, v.m12, v.m13, v.m20, v.m21, v.m22, v.m23, v.m30, v.m31, v.m32, v.m33 };
                    float[] proj = { pr.m00, pr.m01, pr.m02, pr.m03, pr.m10, pr.m11, pr.m12, pr.m13, pr.m20, pr.m21, pr.m22, pr.m23, pr.m30, pr.m31, pr.m32, pr.m33 };
                    Vector3 cp = cam.transform.position, cu = cam.transform.up, cr = cam.transform.right, cf = cam.transform.forward;
                    float[] camv = { cp.x, cp.y, cp.z, cu.x, cu.y, cu.z, cr.x, cr.y, cr.z, cf.x, cf.y, cf.z };
                    Native.Fg_SetFrame(outPtr, depthPtr, mvPtr, -jx, -jy, -renderW, -renderH,
                        cam.nearClipPlane, cam.farClipPlane, cam.fieldOfView * Mathf.Deg2Rad,
                        Time.unscaledDeltaTime * 1000f, reset,
                        (uint)renderW, (uint)renderH, (uint)outW, (uint)outH, (ulong)frames,
                        view, proj, camv);
                    cbEval.IssuePluginEvent(evFn, Native.DLSS_EV_FG_PREPARE);
                }
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

        private static bool WantMvJittered => RenderforgeMod.Instance?.Cfg?.MvJittered ?? false;
        // D3D12 only: on D3D11 the SDK views Unity's sRGB resource directly (Device11.cpp) and the knob is a no-op.
        private static bool WantSrgbViews => SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12 && (RenderforgeMod.Instance?.Cfg?.D3D12SrgbViews ?? false);
        private static bool WantColorDesc => SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12 && (RenderforgeMod.Instance?.Cfg?.D3D12ColorDesc ?? false);
        private static bool WantHalfColor => SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12 && (RenderforgeMod.Instance?.Cfg?.D3D12HalfColor ?? false);

        /// <summary>Diagnostic: one texel of mvRT (the BuiltinRenderTextureType.MotionVectors copy the SDK is fed), render-res
        /// coordinates, y from the bottom (ReadPixels). RGHalf is not ReadPixels-readable, so Blit into an ARGBFloat temp first.
        /// Jitter = what this frame's Dlss_SetFrame got (pixels, render res; the driver passes -jx,-jy).</summary>
        public string ProbeMv(int x, int y)
        {
            if (gen != Gen.Live || mvRT == null) return "not live (gen=" + gen + ")";
            if (x < 0 || y < 0 || x >= renderW || y >= renderH) return "out of range: render=" + renderW + "x" + renderH;
            var tmp = RenderTexture.GetTemporary(renderW, renderH, 0, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear);
            var tex = new Texture2D(1, 1, TextureFormat.RGBAFloat, false);
            var prev = RenderTexture.active;
            try
            {
                Graphics.Blit(mvRT, tmp);
                RenderTexture.active = tmp;
                tex.ReadPixels(new Rect(x, y, 1, 1), 0, 0, false);
                tex.Apply(false);
                Color c = tex.GetPixel(0, 0);
                return "x=" + x + ",y=" + y + " mvx=" + c.r.ToString("R") + " mvy=" + c.g.ToString("R")
                     + " jitterx=" + reportJx.ToString("R") + " jittery=" + reportJy.ToString("R") + " render=" + renderW + "x" + renderH + " fmt=" + mvRT.format;
            }
            finally
            {
                RenderTexture.active = prev;
                RenderTexture.ReleaseTemporary(tmp);
                Destroy(tex);
            }
        }

        /// <summary>Diagnostic: outRT (the SDK output BEFORE Unity's present Blit) to PNG. Works on D3D12 (Blit into an
        /// sRGB-less ARGB32 temp, ReadPixels from there; an ARGBHalf source (D3D12HalfColor) stays linear in the PNG -
        /// the string's fmt= says which). Returns the path + WxH.</summary>
        public string DumpOut(string absPath) => DumpRt(outRT, "outRT", absPath);

        /// <summary>Diagnostic: colorRT (the SDK colour input, render res) to PNG.</summary>
        public string DumpColorIn(string absPath) => DumpRt(colorRT, "colorRT", absPath);

        private string DumpRt(RenderTexture rt, string what, string absPath)
        {
            if (gen != Gen.Live || rt == null) return "not live (gen=" + gen + ")";
            int w = rt.width, h = rt.height;
            var tmp = RenderTexture.GetTemporary(w, h, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
            var tex = new Texture2D(w, h, TextureFormat.RGBA32, false);
            var prev = RenderTexture.active;
            try
            {
                Graphics.Blit(rt, tmp);
                RenderTexture.active = tmp;
                tex.ReadPixels(new Rect(0, 0, w, h), 0, 0, false);
                tex.Apply(false);
                System.IO.File.WriteAllBytes(absPath, tex.EncodeToPNG());
                return what + " -> " + absPath + " " + w + "x" + h + " fmt=" + rt.graphicsFormat;
            }
            finally
            {
                RenderTexture.active = prev;
                RenderTexture.ReleaseTemporary(tmp);
                Destroy(tex);
            }
        }

        private static int QualityFor(RenderforgeMode mode, int outH)
        {
            switch (mode)
            {
                case RenderforgeMode.Auto: return outH <= 1200 ? Native.DLSS_Q_DLAA : outH <= 1600 ? Native.DLSS_Q_QUALITY : Native.DLSS_Q_PERFORMANCE;
                case RenderforgeMode.Quality: return Native.DLSS_Q_QUALITY;
                case RenderforgeMode.Balanced: return Native.DLSS_Q_BALANCED;
                case RenderforgeMode.Performance: return Native.DLSS_Q_PERFORMANCE;
                case RenderforgeMode.UltraPerformance: return Native.DLSS_Q_ULTRA_PERFORMANCE;
                case RenderforgeMode.UltraQuality: return Native.DLSS_Q_ULTRA_QUALITY;           // XeSS only; others run Quality
                case RenderforgeMode.UltraQualityPlus: return Native.DLSS_Q_ULTRA_QUALITY_PLUS;
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
