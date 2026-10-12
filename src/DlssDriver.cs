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
        private float prevJx, prevJy;   // Diagnostics.JitterReportDelay
        private bool copyDeferred;      // the rendering path cbCopy's depth source was built for (BuildCopy)
        private IntPtr colorPtr, depthPtr, mvPtr, outPtr;
        private CommandBuffer cbCopy, cbEval, cbPresent;
        private Camera present;
        private IntPtr evFn, evDataFn;

        private RenderforgeMode wantMode = RenderforgeMode.Off;
        private DebugView wantView = DebugView.None;
        private Gen gen = Gen.Idle;
        private int genFrames;
        private bool releaseQueued, shutdownRequested;
        private static bool quitting;
        // Only needed if another component destroys our persistent owner unexpectedly. One successor drains it.
        private static DlssDriver orphan;
        private RenderforgeMode liveMode;
        private DebugView liveView;
        private bool passthrough;
        private int renderW, renderH, outW, outH, quality;
        private bool wantsFeature;          // false in Passthrough: no NGX feature is created at all
        private bool liveMvJittered;        // DLSS_F_MV_JITTERED the feature was created with (runtime diagnostic)
        private int livePreset;             // NGX render preset the feature was created with (Cfg.DlssModel; 0 = per-mode defaults)
        private bool liveSrgbViews;         // DLSS_F_SRGB_VIEWS the generation was created with (D3D12 runtime diagnostic)
        private bool liveColorDesc;         // colorRT created from an explicit R8G8B8A8_SRGB descriptor (D3D12 runtime diagnostic)
        private bool liveHalfColor;         // colorRT + outRT linear ARGBHalf, DLSS_F_HDR (fixed-on production path)

        private int jitterIndex, phaseCount;
        private int cullFrame = -1;          // Time.frameCount of the last AfterPostProcessPreCull of the bound layer
        private float jx, jy;
        private float reportJx, reportJy;   // what the last Dlss_SetFrame actually got (after the diagnostic sign/swap knobs)
        private bool resetNext;
        private Vector3 lastPos;
        private float lastFov;
        private const float FovCutFraction = 0.05f;   // history reset on a per-frame FOV change above 5 % (zoom snap / cut)
        private bool suspended;             // level end: no generation until Resume (OnLevelStart / OnModEnabled)
        private bool copyAttached;          // cbCopy on the camera + Depth|MotionVectors forced (see NeedsMvDepth)

        private bool depthModeSaved;
        private DepthTextureMode savedDepthMode;
        private bool aaSaved;
        private PostProcessLayer.Antialiasing savedAA;

        private float mipReapplyAt;         // second MipBias sweep 2 s after Live: level content still streaming in; also 0.5 s after a resolution change
        private int lastScreenW = Screen.width, lastScreenH = Screen.height;
        private long frames, resets;
        private string lastFail = "";
        private bool broken;                // threw once inside a Unity callback -> self-disabled

        public bool IsLive => gen == Gen.Live;
        internal bool Retiring => gen == Gen.Releasing;
        public bool Passthrough => passthrough;
        public Camera SceneCamera => cam;
        public PostProcessLayer Layer => layer;
        public RenderforgeMode LiveMode => liveMode;
        public int Quality => quality;
        public int RenderW => renderW;
        public int RenderH => renderH;
        public int OutW => outW;
        public int OutH => outH;
        /// <summary>GeoMarkerOverlay borrows the present camera while Live: it moves cbPresent to BeforeForwardOpaque and lets
        /// the camera draw the geoscape marker layer over the blit. A separate marker camera does not work here (see there).</summary>
        internal Camera PresentCamera => present;
        internal CommandBuffer PresentBuffer => cbPresent;

        public static DlssDriver Create()
        {
            if (Instance != null)
            {
                if (Instance.shutdownRequested) Instance.broken = false;
                Instance.shutdownRequested = false;
                return Instance;
            }
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

        private void OnApplicationQuit() { quitting = true; }

        internal void RequestShutdown()
        {
            shutdownRequested = true;
            wantMode = RenderforgeMode.Off;
            switchTo = UpscalerKind.Off;
            if (gen != Gen.Releasing) BeginRelease();
        }

        private void OnDestroy()
        {
            Camera.onPostRender -= OnCameraPostRender;
            if (Instance == this) Instance = null;
            if (colorRT == null && cbEval == null) return;
            Detach();
            // Application exit: retain the last owner until process/device teardown. Never free live GPU inputs.
            orphan = this;
            if (!quitting) Create();
        }

        /// <summary>Bind to the scene camera (CameraManager.Camera). A different camera than the live one = Off first.</summary>
        public void Attach(Camera sceneCamera)
        {
            if (sceneCamera == null || cam == sceneCamera) return;
            if (gen == Gen.Live || gen == Gen.Creating) BeginRelease();   // old camera detached now; Idle re-creates on the new one
            cam = sceneCamera;
            layer = cam.GetComponent<PostProcessLayer>();
        }

        /// <summary>Live provider switch: detach now, retain the generation until render/GPU retirement is
        /// acknowledged, then Idle re-inits the shim on `kind` and the normal path re-creates on it.</summary>
        public void SwitchProvider(UpscalerKind kind)
        {
            switchTo = kind;
            if (gen == Gen.Live || gen == Gen.Creating) BeginRelease();
            else if (gen == Gen.Idle) genFrames = 0;
        }

        private UpscalerKind switchTo = UpscalerKind.Off;   // Off = no switch pending

        /// <summary>Level end: the level's camera is about to die. Release now even when a passthrough generation (upscaler
        /// Off, sharpen/LUT/grade on) would otherwise stay wanted - with wantMode == liveMode == Off, Apply(Off) alone kept
        /// it Live on the dying camera. Lifted ONLY by Resume (OnLevelStart / OnModEnabled): an Apply in between (config
        /// change, provider-switch Idle branch, hotkey) must not bring generation back on the dying camera.</summary>
        public void Suspend()
        {
            suspended = true;
            wantMode = RenderforgeMode.Off;
            if (gen == Gen.Live || gen == Gen.Creating) BeginRelease();
        }

        public void Resume() { suspended = false; }

        public void Apply(RenderforgeMode mode, DebugView view)
        {
            // Post-only: there is no upscaler to ask for a mode. Every generation is a passthrough one.
            wantMode = RenderforgeMod.PostOnly ? RenderforgeMode.Off : mode;
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
                     + " q=" + quality + " preset=" + livePreset + "/" + Native.DlssPreset() + " passthrough=" + passthrough + " liveMvJittered=" + liveMvJittered + " liveSrgbViews=" + liveSrgbViews + " frames=" + frames + " resets=" + resets + " fov=" + (cam ? cam.fieldOfView.ToString("F3") : "-") + " jitter=" + jx.ToString("F3") + "," + jy.ToString("F3")
                     + " init=" + init + " api=" + Native.Api() + " unityIface=" + Native.UnityIface()
                     + " create=0x" + c.ToString("X") + "(" + Native.Dlss_ResultString(c) + ") eval=0x" + e.ToString("X") + "(" + Native.Dlss_ResultString(e) + ")"
                     + " feature=" + alive + " lastError=" + Native.Dlss_LastError() + " sharpen=" + Native.SharpenerName(Native.Dlss_Sharpener())
                     + " cam=" + (cam ? cam.name : "null") + " target=" + (cam && cam.targetTexture ? cam.targetTexture.name : "null")
                     + " depthMode=" + (cam ? cam.depthTextureMode.ToString() : "-") + " aa=" + (layer ? layer.antialiasingMode.ToString() : "-")
                     + " colorSpace=" + QualitySettings.activeColorSpace + " reversedZ=" + SystemInfo.usesReversedZBuffer
                     + " path=" + (cam ? cam.actualRenderingPath.ToString() : "-")
                     + " present=" + (present ? (present.enabled ? "on" : "off") : "none") + " mvCopy=" + copyAttached + " broken=" + broken + " fail=" + lastFail
                     + " " + Native.Timings();
            }
        }

        // ---------------------------------------------------------------- generation state machine (main thread)

        private void Update()
        {
            if (Diagnostics.Sampling) Diagnostics.SampleFrame(Time.unscaledDeltaTime);
            var cfg = RenderforgeMod.Instance?.Cfg;
            if (cfg != null && (Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl))
                            && (Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt)))
            {
                if (Input.GetKeyDown(cfg.ToggleHotkey)) RenderforgeMod.Toggle();
                if (Input.GetKeyDown(cfg.OverlayHotkey)) RenderforgeMod.ToggleOverlay();
            }
            // The UI pin is log2(canvas scale), which follows the resolution: re-sweep shortly after any change, in every
            // generation state (with the upscaler Off nothing else would, and the pin would stay stale until the next level).
            if (Screen.width != lastScreenW || Screen.height != lastScreenH) { lastScreenW = Screen.width; lastScreenH = Screen.height; mipReapplyAt = Time.unscaledTime + 0.5f; }
            if (mipReapplyAt > 0f && Time.unscaledTime >= mipReapplyAt) { mipReapplyAt = 0f; MipBias.Reapply(); }
            if (broken && gen != Gen.Releasing && !shutdownRequested && ReferenceEquals(orphan, null)) return;
            try
            {
                FrameGen.Pump();
                if (!ReferenceEquals(orphan, null))
                {
                    if (orphan.gen != Gen.Releasing) orphan.BeginRelease();
                    if (!orphan.TryRetire()) return;
                    orphan = null;
                }
                Step();
            }
            catch (Exception ex) { Fail("Update threw: " + ex); }
        }

        private void Step()
        {
            var cfg = RenderforgeMod.Instance?.Cfg;
            bool lutActive = cfg != null && cfg.Lut != LutPreset.Off && cfg.LutStrength > 0;
            // Sharpness is its own reason to run: with the upscaler Off the NIS pass is the only thing on the frame.
            bool needsPipeline = !suspended && (wantMode != RenderforgeMode.Off || lutActive || SceneStylePanel.Active(cfg)
                || ColorVisionPanel.Active(cfg) || GradePanel.Active(cfg) || (cfg != null && cfg.Sharpness > 0));
            switch (gen)
            {
                case Gen.Idle:
                    if (shutdownRequested)
                    {
                        if (Native.Dlss_Shutdown() != 0) Destroy(gameObject);
                        break;
                    }
                    if (switchTo != UpscalerKind.Off)
                    {
                        // Retirement was acknowledged by the render thread. A refused vendor shutdown still
                        // keeps this provider selected; a frame count cannot authorize freeing its resources.
                        if (!FrameGen.Release() || Native.Dlss_Shutdown() == 0) break;
                        UpscalerKind k = switchTo; switchTo = UpscalerKind.Off;
                        RenderforgeMod.ReinitNative(k);
                        // ReinitNative may have just left post-only (or entered it): Apply is the single funnel that
                        // maps Cfg.Mode through RenderforgeMod.PostOnly, so re-run it before StartGeneration.
                        Apply(RenderforgeMod.Instance?.Cfg?.Mode ?? RenderforgeMode.Off, wantView);
                        break;   // needsPipeline was computed above with the stale mode; re-enter Idle next frame
                    }
                    if (needsPipeline && cam != null && cam.isActiveAndEnabled && RenderforgeMod.Available) StartGeneration();
                    break;
                case Gen.Creating:
                    if (++genFrames < 2) break;
                    if (wantsFeature)
                    {
                        int c, e, alive; _ = Native.Dlss_Status(out c, out e, out alive);   // returns the init code; only the out values matter here
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
                    if (!cam.isActiveAndEnabled || !needsPipeline || wantMode != liveMode || !SameSizeClass(liveView, wantView)
                        || Screen.width != outW || Screen.height != outH || liveMvJittered != WantMvJittered || liveSrgbViews != WantSrgbViews || liveColorDesc != WantColorDesc || liveHalfColor != WantHalfColor
                        || (wantsFeature && livePreset != WantPreset))   // NGX reads the preset only at creation: re-create
                    {
                        BeginRelease();
                        break;
                    }
                    if (wantsFeature && frames > 2)
                    {
                        int c, e, alive; _ = Native.Dlss_Status(out c, out e, out alive);   // returns the init code; only the out values matter here
                        if ((e & 0xFFF00000) == 0xBAD00000) { Fail("NGX evaluate failed: 0x" + e.ToString("X") + " " + Native.Dlss_ResultString(e) + " lastError=" + Native.Dlss_LastError()); break; }
                    }
                    KeepCameraState();
                    GeoMarkerOverlay.Tick(cam, passthrough);   // gated (geoscape, curtain up, D3D11 or D3D12, no colour vision); never throws
                    FrameGen.Retry();
                    break;
                case Gen.Releasing:
                    if (!TryRetire()) break;
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
            // A LUT or scene style with the upscaler Off uses this full-resolution copy path.
            passthrough = liveView == DebugView.Passthrough || liveMode == RenderforgeMode.Off;
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
                    sRGB = true,
                    depthBufferBits = 0,
                    msaaSamples = 1,
                    enableRandomWrite = false,
                    useMipMap = false,
                    autoGenerateMips = false,
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
            BuildCopy();
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

            _ = Native.Dlss_Passthrough(passthrough ? 1 : 0);   // returns the previous flag, not an error
            if (wantsFeature)
            {
                liveMvJittered = WantMvJittered;
                int flags = Native.DLSS_F_MV_LOW_RES | (SystemInfo.usesReversedZBuffer ? Native.DLSS_F_DEPTH_INVERTED : 0)
                          | (liveMvJittered ? Native.DLSS_F_MV_JITTERED : 0)
                          | (liveSrgbViews ? Native.DLSS_F_SRGB_VIEWS : 0)
                          | (liveHalfColor ? Native.DLSS_F_HDR : 0);
                livePreset = WantPreset;
                Native.SetDlssPreset(livePreset);   // latched by the SetCreateParams below
                Native.Dlss_SetCreateParams((uint)renderW, (uint)renderH, (uint)outW, (uint)outH, quality, flags);
                GL.IssuePluginEvent(evFn, Native.DLSS_EV_CREATE);
            }
            gen = Gen.Creating; genFrames = 0; frames = 0;
            RenderforgeMod.Instance?.Logger.LogInfo("DLSS generation: mode=" + liveMode + " view=" + liveView + " render=" + renderW + "x" + renderH + " out=" + outW + "x" + outH + " q=" + quality + " phases=" + phaseCount + " colorRT=" + colorRT.graphicsFormat + " sRGB=" + colorRT.sRGB + " colorDesc=" + liveColorDesc + " halfColor=" + liveHalfColor + " outRT=" + outRT.graphicsFormat + " preset=" + (wantsFeature ? livePreset : 0));
        }

        private static RenderTexture Make(string name, int w, int h, RenderTextureFormat fmt, bool uav, RenderTextureReadWrite rw = RenderTextureReadWrite.Default)
        {
            var rt = new RenderTexture(w, h, 0, fmt, rw) { name = name, enableRandomWrite = uav, filterMode = FilterMode.Point, useMipMap = false, autoGenerateMips = false };
            rt.Create();
            return rt;
        }

        /// <summary>The deferred paths have no camera depth texture of their own: there the scene depth is the resolved
        /// G-buffer depth. Measured 2026-10-12 on D3D11 and D3D12 (DepthStats, geoscape + tactical): under DeferredShading
        /// BuiltinRenderTextureType.Depth is not bound ("built-in render texture type 3 not found while executing DLSS copy
        /// depth+mv (Blit source)" in Player.log) and the material-less Blit then copies whatever _MainTex still holds - the
        /// colour buffer, correlation 1.0000 with colorRT.r - while ResolvedDepth reads the real reversed-Z device depth
        /// (centre 0.01741 against 0.01737 predicted by a physics ray). Under Forward it is exactly the other way round.</summary>
        private static bool IsDeferred(Camera c) => c.actualRenderingPath == RenderingPath.DeferredShading || c.actualRenderingPath == RenderingPath.DeferredLighting;

        /// <summary>MV + depth copies into the RTs the SDKs read. depthRT (RFloat, render resolution, same row order as
        /// colorRT / mvRT) holds raw device depth: reversed-Z on D3D, which is what DLSS_F_DEPTH_INVERTED declares.</summary>
        private void BuildCopy()
        {
            if (cbCopy == null) return;
            copyDeferred = cam != null && IsDeferred(cam);
            bool resolved = Diagnostics.DepthSource == 0 ? copyDeferred : Diagnostics.DepthSource == 2;
            cbCopy.Clear();
            cbCopy.CopyTexture(BuiltinRenderTextureType.MotionVectors, mvRT);
            cbCopy.Blit(resolved ? BuiltinRenderTextureType.ResolvedDepth : BuiltinRenderTextureType.Depth, depthRT);
        }

        /// <summary>Diagnostic: switch the depth copy's source on a live generation (Diagnostics.DepthSource).</summary>
        public void RebuildCopy() => BuildCopy();

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
            cam.AddCommandBuffer(CameraEvent.AfterEverything, cbEval);
            KeepCameraState();   // also adds cbCopy when MV/depth are consumed
            present.enabled = true;
            lastPos = cam.transform.position; lastFov = cam.fieldOfView;
        }

        /// <summary>Motion vectors + depth feed only a real upscaler feature, frame generation and the Depth / MotionVectors
        /// debug views. A passthrough generation (upscaler Off, sharpen / LUT / grade on; the shim's passthrough Evaluate
        /// never reads them, Device11.cpp / Device12.cpp) must not force Unity's MotionVectors pass, which re-draws every
        /// skinned / moving renderer, nor copy the two textures every frame. Frame generation is NOT a reason: FG reads the
        /// shim's owned twins, and the passthrough Evaluate never copies depth/MV into them (Device12.cpp Declare/RunPassthrough),
        /// so forcing the pass fed FG unwritten twins. Without cbCopy the FG_PREPARE gate (copyAttached) skips the frame
        /// and the FG host idles (Unity presents its own frames) until a real upscaler generation feeds it.</summary>
        private bool NeedsMvDepth => !passthrough || liveView == DebugView.Depth || liveView == DebugView.MotionVectors;

        /// <summary>Re-asserted every live frame: PPv2 or the game may touch these between frames.</summary>
        private void KeepCameraState()
        {
            cam.targetTexture = colorRT;
            // Follows NeedsMvDepth live: a Depth / MotionVectors debug view can be switched in on a live generation.
            if (NeedsMvDepth)
            {
                // The depth source depends on the rendering path; a path switch mid-generation would feed colour as depth.
                if (IsDeferred(cam) != copyDeferred) { BuildCopy(); resetNext = true; }
                cam.depthTextureMode |= DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
                // Attached mid-generation: the MV/depth history so far is missing, so the first frame fed is a reset.
                if (!copyAttached) { cam.AddCommandBuffer(CameraEvent.BeforeImageEffects, cbCopy); copyAttached = true; resetNext = true; }
            }
            else if (copyAttached)
            {
                cam.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, cbCopy); copyAttached = false;
                if (depthModeSaved) cam.depthTextureMode = savedDepthMode;
            }
            MotionVectorFilter.Set(copyAttached);   // spun props + decal volumes write no object motion while the pass is consumed
            // SMAA off only under a real upscaler (it replaces the AA); a passthrough generation keeps vanilla AA.
            if (!passthrough && layer != null && layer.antialiasingMode != PostProcessLayer.Antialiasing.None) layer.antialiasingMode = PostProcessLayer.Antialiasing.None;
            present.depth = cam.depth + 1;
        }

        private void Detach()
        {
            GeoMarkerOverlay.Restore();
            MotionVectorFilter.Restore();
            if (cam != null)
            {
                if (cbCopy != null && copyAttached) cam.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, cbCopy);
                if (cbEval != null) cam.RemoveCommandBuffer(CameraEvent.AfterEverything, cbEval);
                if (cam.targetTexture == colorRT) cam.targetTexture = null;
                if (depthModeSaved) { cam.depthTextureMode = savedDepthMode; depthModeSaved = false; }
                cam.ResetProjectionMatrix();
            }
            copyAttached = false;
            if (layer != null && aaSaved) { layer.antialiasingMode = savedAA; aaSaved = false; }
            if (present != null) present.enabled = false;
        }

        private void BeginRelease()
        {
            MipBias.Reset();
            Detach(); // Stop all future evaluate/FG submissions before requesting retirement.
            FrameGen.Release();
            _ = Native.Dlss_Passthrough(0);   // returns the previous flag, not an error
            releaseQueued = false;
            gen = Gen.Releasing; genFrames = 0;
        }

        private bool TryRetire()
        {
            if (!FrameGen.Release()) return false;
            if (releaseQueued && Native.Dlss_ReleaseStatus() == 1)
            {
                TeardownResources();
                return true;
            }
            // One event at a time. A failed fence poll is acknowledged as -1; 0 is still queued.
            if (!releaseQueued || Native.Dlss_ReleaseStatus() < 0)
            {
                Native.Dlss_BeginRelease();
                GL.IssuePluginEvent(evFn, Native.DLSS_EV_RELEASE);
                releaseQueued = true;
            }
            return false;
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
            bool again = cullFrame == Time.frameCount;   // the bound layer culled a second time inside one frame (a manual Camera.Render)
            cullFrame = Time.frameCount;
            if (again) FrameGen.DupPreCull++;
            try
            {
                Halton(jitterIndex, out jx, out jy);
                jitterIndex = (jitterIndex + 1) % phaseCount;
                if (passthrough) { jx = 0f; jy = 0f; }
                if (Diagnostics.JitterConstEnabled && !passthrough) { jx = Diagnostics.JitterConstX; jy = Diagnostics.JitterConstY; }   // JitterConst: rendered AND reported
                float jscale = Diagnostics.JitterScale;
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

                int reset = resetNext || Diagnostics.ForceReset ? 1 : 0;   // ForceReset: NGX InReset / FSR reset / XeSS resetHistory every frame
                Vector3 pos = cam.transform.position;
                // Camera cut = position jump or a relative FOV jump above FovCutFraction. Cinemachine blends and
                // FrameModifier nudge the FOV every frame; resetting on each (Mathf.Approximately) shimmered the whole blend.
                float fov = cam.fieldOfView;
                if ((pos - lastPos).sqrMagnitude > 50f * 50f || Mathf.Abs(fov - lastFov) > FovCutFraction * Mathf.Max(lastFov, 1f)) reset = 1;
                lastPos = pos; lastFov = fov; resetNext = false;
                if (reset != 0) resets++;

                // Signs (live-verified 2026-09-01, DLAA 1280x720): NGX gets (-jx, -jy). Unity's view space is right-handed
                // (w_clip = -z_view), so "+2jx/w" on proj[0,2] shifts the image by -jx pixels; the same holds for y.
                // Verified by an 8x crop: (+jx,+jy) doubles thin edges, (-jx,-jy) resolves them. Same as HDRP's DLSSPass.
                // MV: Unity's texture is (current - previous) in UV space (PPv2 TAA fetches history at uv - mv); DLSS wants
                // current -> previous in pixels, hence InMVScale = (-renderW, -renderH).
                // Sharpness = our RCAS pass in the shim (NGX InSharpness is deprecated in SDK 310), read live: slider/100.
                // Zero only for the debug Passthrough VIEW (an untouched reference frame). An Off-mode generation
                // is a passthrough too, and there the NIS pass is exactly what the player asked for.
                float sharp = liveView == DebugView.Passthrough ? 0f : Mathf.Clamp01((RenderforgeMod.Instance?.Cfg?.Sharpness ?? 0) / 100f);
                var cfg = RenderforgeMod.Instance?.Cfg;
                int lutPreset = cfg != null ? Mathf.Clamp((int)cfg.Lut, 0, (int)LutPreset.VintageSepia) : 0;
                float lutStrength = lutPreset == 0 ? 0f : Mathf.Clamp01((cfg?.LutStrength ?? 0) / 100f);
                // FSR needs the camera frustum (cameraNear/Far/FovAngleVertical); NGX ignores it. Cached in the
                // shim and copied into the frame slot, so the ABI of Dlss_SetFrame stays untouched.
                Native.SetCamera(cam.nearClipPlane, cam.farClipPlane, cam.fieldOfView * Mathf.Deg2Rad);
                // Diagnostic (D3D12 detail-loss hunt): sign/swap knobs touch ONLY the offset reported to the SDK, not the projection.
                float sjx = jx, sjy = jy;
                if (Diagnostics.JitterReportDelay > 0) { sjx = prevJx; sjy = prevJy; }
                else if (Diagnostics.JitterReportDelay < 0) { Halton(jitterIndex, out sjx, out sjy); sjx *= jscale; sjy *= jscale; }
                prevJx = jx; prevJy = jy;
                float rjx = -sjx * Diagnostics.JitterReportSignX, rjy = -sjy * Diagnostics.JitterReportSignY;
                if (Diagnostics.JitterReportSwapXY) { float t = rjx; rjx = rjy; rjy = t; }
                reportJx = rjx; reportJy = rjy;
                IntPtr slot = Native.Dlss_GetFrameSlot();
                Native.Dlss_SetFrame(slot, colorPtr, depthPtr, mvPtr, outPtr, rjx, rjy, -renderW, -renderH,
                    reset, Time.unscaledDeltaTime * 1000f, (uint)renderW, (uint)renderH, 1f, sharp, lutPreset, lutStrength);
                if (SceneStylePanel.Active(cfg))
                    Native.Dlss_SetSceneStyle(slot, (int)cfg.SceneStyle,
                        Mathf.Clamp01(cfg.SceneStyleStrength / 100f), Mathf.Clamp(cfg.PixelSize, 2, 16));
                if (ColorVisionPanel.Active(cfg))
                    Native.Dlss_SetColorVision(slot, (int)cfg.ColorVision);
                if (GradePanel.Active(cfg))
                    Native.Dlss_SetGrade(slot, Mathf.Clamp(cfg.LevelsBlack, 0, 40) / 255f, Mathf.Clamp(cfg.LevelsWhite, 215, 255) / 255f,
                        Mathf.Clamp(cfg.Contrast, 50, 150) / 100f, Mathf.Clamp(cfg.Clarity, 0, 100) / 100f,
                        Mathf.Clamp(cfg.Exposure, -40, 40) / 10f, Mathf.Clamp(cfg.Brightness, -100, 100) / 100f,
                        Mathf.Clamp(cfg.Saturation, 0, 200) / 100f, Mathf.Clamp(cfg.Vibrance, -100, 100) / 100f);
                cbEval.Clear();
                cbEval.IssuePluginEventAndData(evDataFn, Native.DLSS_EV_EVALUATE, slot);
                // One frame is fed ONCE: a second prepare event inside a frame cannot be presented (one Present per frame)
                // and made the shim throw the first one away after waiting 100 ms for a Present that could not come.
                if (again && FrameGen.Live && !FrameGen.HoldPrepare && copyAttached) FrameGen.DupFeeds++;
                else if (FrameGen.Live && !FrameGen.HoldPrepare && copyAttached)   // copyAttached: FG never gets MV/depth nobody rendered or copied (a passthrough generation)
                {
                    // The FG contract (native\Fg.h FgFrame, docs\DESIGN.md "FG input contract"): everything in Direct3D
                    // conventions for a TOP-DOWN image, because the generators compare these inputs with the swapchain's
                    // back buffer. View space left-handed (z = cam.transform.forward, the basis the camera vectors below
                    // span); clip space y up, reversed Z, w = +z: GL.GetGPUProjectionMatrix(p, false) is Unity's own
                    // GL -> D3D conversion without the render-texture flip. Unity's matrices are right-handed (w = -z),
                    // hence the z mirror on both, which cancels in clip -> previous clip.
                    Matrix4x4 v = cam.worldToCameraMatrix;
                    v.SetRow(2, -v.GetRow(2));
                    Matrix4x4 pr = GL.GetGPUProjectionMatrix(cam.nonJitteredProjectionMatrix, false);
                    pr.SetColumn(2, -pr.GetColumn(2));
                    RowVector(fgView, v); RowVector(fgProj, pr);
                    Vector3 cp = cam.transform.position, cu = cam.transform.up, cr = cam.transform.right, cf = cam.transform.forward;
                    fgCam[0] = cp.x; fgCam[1] = cp.y; fgCam[2] = cp.z; fgCam[3] = cu.x; fgCam[4] = cu.y; fgCam[5] = cu.z;
                    fgCam[6] = cr.x; fgCam[7] = cr.y; fgCam[8] = cr.z; fgCam[9] = cf.x; fgCam[10] = cf.y; fgCam[11] = cf.z;
                    // Jitter and MV scale: x as the upscalers get it (-jx, -renderW), y mirrored with the rows (the shim
                    // hands the generators row-flipped twins): +jy, +renderH.
                    IntPtr fgSlot = Native.Fg_SetFrame(outPtr, depthPtr, mvPtr, -jx, jy, -renderW, renderH,
                        cam.nearClipPlane, cam.farClipPlane, cam.fieldOfView * Mathf.Deg2Rad,
                        Time.unscaledDeltaTime * 1000f, reset,
                        (uint)renderW, (uint)renderH, (uint)outW, (uint)outH, (ulong)frames,
                        fgView, fgProj, fgCam);
                    // The block travels WITH the event: the render thread pairs it with the frame it really holds (Fg.h).
                    cbEval.IssuePluginEventAndData(evDataFn, Native.DLSS_EV_FG_PREPARE, fgSlot);
                    FrameGen.Fed();   // frames flow: the provider's pacer owns the next frame start (FrameGen pacing)
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
            savedAA = layer.antialiasingMode; aaSaved = true;   // re-saved even in passthrough: Detach restores the fresh preset
            if (!passthrough) layer.antialiasingMode = PostProcessLayer.Antialiasing.None;
        }

        // ---------------------------------------------------------------- helpers

        private static bool WantMvJittered => Diagnostics.MvJittered;
        // D3D12 only: on D3D11 the SDK views Unity's sRGB resource directly (Device11.cpp) and the knob is a no-op.
        private static bool WantSrgbViews => SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12 && Diagnostics.D3D12SrgbViews;
        private static bool WantColorDesc => SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12 && Diagnostics.D3D12ColorDesc;
        private static bool WantHalfColor => SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12 && Diagnostics.D3D12HalfColor;
        // FSR/XeSS have no NGX presets: 0 there, so a DLSS MODEL change never re-creates their features.
        private static int WantPreset => Upscalers.Running == UpscalerKind.DLSS
            ? DlssConfig.NgxPreset(RenderforgeMod.Instance?.Cfg?.DlssModel ?? DlssModel.Auto) : 0;

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

        /// <summary>Diagnostic: the whole mvRT as RGBA floats (r,g = Unity's UV-space motion), row 0 at the bottom; null when not live.</summary>
        internal float[] ReadMv() => ReadFloats(mvRT);

        /// <summary>Diagnostic: an RT of this generation as RGBA floats, row 0 at the bottom; null when not live.</summary>
        private float[] ReadFloats(RenderTexture rt)
        {
            if (gen != Gen.Live || rt == null) return null;
            int rw = rt.width, rh = rt.height;
            var tmp = RenderTexture.GetTemporary(rw, rh, 0, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear);
            var tex = new Texture2D(rw, rh, TextureFormat.RGBAFloat, false);
            var prev = RenderTexture.active;
            try
            {
                Graphics.Blit(rt, tmp);
                RenderTexture.active = tmp;
                tex.ReadPixels(new Rect(0, 0, rw, rh), 0, 0, false);
                return tex.GetRawTextureData<float>().ToArray();
            }
            finally
            {
                RenderTexture.active = prev;
                RenderTexture.ReleaseTemporary(tmp);
                Destroy(tex);
            }
        }

        /// <summary>Diagnostic: the motion field the SDK is fed, whole frame, in render-res pixels per frame: share of texels
        /// above 0.01 px, the largest vector and where, the bounding box of the moving texels, and a 16x9 map of each
        /// cell's largest magnitude in 1/100 px (rows top to bottom). A still camera over a still picture must read 0.
        /// A non-empty absPath also gets a PNG: |mv| in px x 64 in red (x) and green (y), blue = any motion.</summary>
        public string MvStats(string absPath)
        {
            float[] mv = ReadMv();
            if (mv == null) return "not live (gen=" + gen + ")";
            int w = renderW, h = renderH, moving = 0, minX = w, minY = h, maxX = -1, maxY = -1, atX = 0, atY = 0;
            const int gw = 16, gh = 9;
            var grid = new float[gw * gh];
            float best = 0f, bx = 0f, by = 0f;
            double sum = 0;
            Color32[] px = string.IsNullOrEmpty(absPath) ? null : new Color32[w * h];
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int i = (y * w + x) * 4;
                    float mx = mv[i] * w, my = mv[i + 1] * h;
                    float m = Mathf.Sqrt(mx * mx + my * my);
                    sum += m;
                    if (px != null) px[y * w + x] = new Color32((byte)Mathf.Min(255f, Mathf.Abs(mx) * 64f), (byte)Mathf.Min(255f, Mathf.Abs(my) * 64f), (byte)(m > 0.01f ? 255 : 0), 255);
                    if (m <= 0.01f) continue;
                    moving++;
                    if (x < minX) minX = x; if (x > maxX) maxX = x; if (y < minY) minY = y; if (y > maxY) maxY = y;
                    int g = (gh - 1 - y * gh / h) * gw + x * gw / w;
                    if (m > grid[g]) grid[g] = m;
                    if (m > best) { best = m; bx = mx; by = my; atX = x; atY = y; }
                }
            }
            if (px != null)
            {
                var tex = new Texture2D(w, h, TextureFormat.RGBA32, false);
                try { tex.SetPixels32(px); tex.Apply(false); System.IO.File.WriteAllBytes(absPath, tex.EncodeToPNG()); }
                finally { Destroy(tex); }
            }
            var sb = new System.Text.StringBuilder();
            sb.Append("render=").Append(w).Append('x').Append(h).Append(" moving=").Append((100.0 * moving / ((double)w * h)).ToString("F3")).Append('%')
              .Append(" meanpx=").Append((sum / ((double)w * h)).ToString("F4")).Append(" maxpx=").Append(best.ToString("F3"))
              .Append(" (").Append(bx.ToString("F3")).Append(',').Append(by.ToString("F3")).Append(") at ").Append(atX).Append(',').Append(atY)
              .Append(" bbox=").Append(maxX < 0 ? "none" : minX + "," + minY + "-" + maxX + "," + maxY).Append(" grid=");
            for (int r = 0; r < gh; r++)
            {
                if (r > 0) sb.Append('/');
                for (int c = 0; c < gw; c++) sb.Append(c > 0 ? "," : "").Append(Mathf.RoundToInt(grid[r * gw + c] * 100f));
            }
            return sb.ToString();
        }

        /// <summary>Diagnostic: is depthRT (the depth every SDK is fed) real device depth? Range, the centre texel with the
        /// eye distance it decodes to, the device depth a physics ray through the screen centre predicts, and the Pearson
        /// correlation with colorRT's red channel (1.0 = the blit copied colour, not depth).</summary>
        public string DepthStats()
        {
            float[] d = ReadFloats(depthRT), c = ReadFloats(colorRT);
            if (d == null || c == null) return "not live (gen=" + gen + ")";
            int w = renderW, h = renderH, n = 0, zero = 0;
            float min = float.MaxValue, max = float.MinValue;
            double sd = 0, sc = 0, sdd = 0, scc = 0, sdc = 0;
            for (int i = 0; i < w * h; i++)
            {
                float dv = d[i * 4], cv = c[i * 4];
                if (dv < min) min = dv; if (dv > max) max = dv; if (dv == 0f) zero++;
                sd += dv; sc += cv; sdd += (double)dv * dv; scc += (double)cv * cv; sdc += (double)dv * cv; n++;
            }
            double vd = sdd / n - sd / n * (sd / n), vc = scc / n - sc / n * (sc / n);
            double corr = vd > 0 && vc > 0 ? (sdc / n - sd / n * (sc / n)) / Math.Sqrt(vd * vc) : 0;
            float centre = d[((h / 2) * w + w / 2) * 4];
            Matrix4x4 p = GL.GetGPUProjectionMatrix(cam.nonJitteredProjectionMatrix, true);
            string eye = centre + p.m22 != 0f ? (p.m23 / (centre + p.m22)).ToString("F3") : "inf";
            string expect = "no hit";
            Ray ray = cam.ViewportPointToRay(new Vector3(0.5f, 0.5f, 0f));
            if (Physics.Raycast(ray, out RaycastHit hit, cam.farClipPlane))
            {
                float z = Vector3.Dot(hit.point - cam.transform.position, cam.transform.forward);
                expect = ((p.m23 - p.m22 * z) / z).ToString("F6") + " (eye " + z.ToString("F3") + " m, " + hit.collider.name + ")";
            }
            return "depthRT " + w + "x" + h + " src=" + Diagnostics.DepthSource + " min=" + min.ToString("F6") + " max=" + max.ToString("F6") + " mean=" + (sd / n).ToString("F6")
                 + " zero=" + (100.0 * zero / n).ToString("F1") + "% centre=" + centre.ToString("F6") + " (eye " + eye + " m) expect=" + expect
                 + " corrColorR=" + corr.ToString("F4") + " reversedZ=" + SystemInfo.usesReversedZBuffer + " path=" + cam.actualRenderingPath
                 + " near=" + cam.nearClipPlane + " far=" + cam.farClipPlane;
        }

        /// <summary>Diagnostic: MotionVectorFilter.Survey as a coroutine on this object; the file appears `frames` frames later.</summary>
        public string SurveyMovers(string absPath, int frames)
        {
            if (gen != Gen.Live) return "not live (gen=" + gen + ")";
            StartCoroutine(MotionVectorFilter.Survey(this, absPath, frames));
            return "survey started -> " + absPath;
        }

        /// <summary>Diagnostic: outRT (the SDK output BEFORE Unity's present Blit) to PNG. Works on D3D12 (Blit into an
        /// sRGB-less ARGB32 temp, ReadPixels from there). An ARGBHalf source (D3D12HalfColor) holds LINEAR values: it is
        /// read back as floats and sRGB-encoded on the CPU, so a D3D12 dump compares 1:1 with a D3D11 one (whose 8-bit
        /// bytes are already encoded); the string says enc=srgb(cpu) then. Returns the path + WxH.</summary>
        public string DumpOut(string absPath) => DumpRt(outRT, "outRT", absPath);

        /// <summary>Diagnostic: colorRT (the SDK colour input, render res) to PNG.</summary>
        public string DumpColorIn(string absPath) => DumpRt(colorRT, "colorRT", absPath);

        private string DumpRt(RenderTexture rt, string what, string absPath)
        {
            if (gen != Gen.Live || rt == null) return "not live (gen=" + gen + ")";
            int w = rt.width, h = rt.height;
            if (rt.format == RenderTextureFormat.ARGBHalf)
            {
                float[] f = ReadFloats(rt);
                var px = new Color32[w * h];
                for (int i = 0; i < px.Length; i++)
                    px[i] = new Color32(SrgbByte(f[i * 4]), SrgbByte(f[i * 4 + 1]), SrgbByte(f[i * 4 + 2]), 255);
                var enc = new Texture2D(w, h, TextureFormat.RGBA32, false);
                try { enc.SetPixels32(px); enc.Apply(false); System.IO.File.WriteAllBytes(absPath, enc.EncodeToPNG()); }
                finally { Destroy(enc); }
                return what + " -> " + absPath + " " + w + "x" + h + " fmt=" + rt.graphicsFormat + " enc=srgb(cpu)";
            }
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

        private static byte SrgbByte(float v)
        {
            v = Mathf.Clamp01(v);
            return (byte)Mathf.RoundToInt(255f * (v <= 0.0031308f ? v * 12.92f : 1.055f * Mathf.Pow(v, 1f / 2.4f) - 0.055f));
        }

        private static int QualityFor(RenderforgeMode mode, int outH)
        {
            switch (mode)
            {
                // Quality before fps: 4K (2160) stays Quality - Performance there (1080p -> 4K) read as blurry; only above 4K does Auto drop, and then to Balanced.
                case RenderforgeMode.Auto: return outH <= 1200 ? Native.DLSS_Q_DLAA : outH <= 2160 ? Native.DLSS_Q_QUALITY : Native.DLSS_Q_BALANCED;
                case RenderforgeMode.Quality: return Native.DLSS_Q_QUALITY;
                case RenderforgeMode.Balanced: return Native.DLSS_Q_BALANCED;
                case RenderforgeMode.Performance: return Native.DLSS_Q_PERFORMANCE;
                case RenderforgeMode.UltraPerformance: return Native.DLSS_Q_ULTRA_PERFORMANCE;
                case RenderforgeMode.UltraQuality: return Native.DLSS_Q_ULTRA_QUALITY;           // XeSS only; others run Quality
                case RenderforgeMode.UltraQualityPlus: return Native.DLSS_Q_ULTRA_QUALITY_PLUS;
                default: return Native.DLSS_Q_DLAA;
            }
        }

        // Fg_SetFrame inputs, reused every frame: the shim memcpy's them inside the call (RenderforgeNative.cpp Fg_SetFrame).
        private static readonly float[] fgView = new float[16], fgProj = new float[16], fgCam = new float[12];

        /// <summary>Unity's column-vector matrix (clip = P * v) as a Direct3D row-vector one (clip = v * M), row-major:
        /// the transpose. Streamline (sl_matrix_helpers.h:206-214) and XeSS-FG (developer guide "Frame Constants": an
        /// XMFLOAT4X4 memcpy) both take this form.</summary>
        private static void RowVector(float[] d, Matrix4x4 m)
        {
            d[0] = m.m00; d[1] = m.m10; d[2] = m.m20; d[3] = m.m30;
            d[4] = m.m01; d[5] = m.m11; d[6] = m.m21; d[7] = m.m31;
            d[8] = m.m02; d[9] = m.m12; d[10] = m.m22; d[11] = m.m32;
            d[12] = m.m03; d[13] = m.m13; d[14] = m.m23; d[15] = m.m33;
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
