using System;
using System.Runtime.CompilerServices;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

namespace Renderforge
{
    /// <summary>Unity 2019.4's D3D12 backend ships no kernels for two PPv2 compute shaders:
    /// MultiScaleVODownsample1 (AO in MSVO mode, AmbientOcclusion.cs:94) and KGenLut3D_AcesTonemap
    /// (HDR ColorGrading 3D-LUT baker, ColorGradingRenderer.cs:80). Either one throws every frame and
    /// PostProcessLayer aborts, which ALSO drops FogOfWarPostProcess and grading -> washed-out, fully lit scene.
    /// Fix: take AO off the compute path (SAO is a pixel shader, AmbientOcclusion.cs:82-88) or disable it, and
    /// null PostProcessResources.computeShaders.lut3DBaker so HDR ColorGrading routes to RenderHDRPipeline2D
    /// (ColorGradingRenderer.cs:33 gate, :44) - still HDR grading, just baked into a 2D LUT by a pixel shader.
    /// Resources are reached through PostProcessLayer's private m_Resources (PostProcessLayer.cs:55, handed to
    /// the context at :624).
    /// ponytail: the profile/resources are mutated in place and NOT restored on OnModDisabled (a mod toggle
    /// mid-session keeps SAO + the 2D-LUT path until relaunch); upgrade = cache the originals here and put
    /// them back in OnModDisabled.</summary>
    internal static class D3D12Fix
    {
        /// <summary>false = AO switched to ScalableAmbientObscurance (kept); true = AO disabled entirely.
        /// The in-game test in Task 11 decides which one ships.</summary>
        internal static bool DisableAo;

        private static bool loggedError;

        // Unity's supportsComputeShaders and a non-null shader do not prove that its D3D12 kernels shipped.
        // Cache by managed shader identity without retaining unloaded Unity assets. HasKernel is the installed
        // non-throwing availability API; never use FindKernel as a per-frame support probe.
        private sealed class HistogramKernels
        {
            internal readonly bool Supported;
            internal HistogramKernels(ComputeShader shader)
            {
                Supported = HasKernel(shader, "KEyeHistogramClear") && HasKernel(shader, "KEyeHistogram");
                if (!Supported) RenderforgeMod.Instance?.Logger.LogWarning("Renderforge D3D12: exposure histogram kernels unavailable; auto exposure and light meter skipped.");
            }
        }

        private sealed class ExposureKernels
        {
            internal readonly bool Fixed, Progressive;
            internal ExposureKernels(ComputeShader shader)
            {
                Fixed = HasKernel(shader, "KAutoExposureAvgLuminance_fixed");
                Progressive = HasKernel(shader, "KAutoExposureAvgLuminance_progressive");
                if (!Fixed || !Progressive) RenderforgeMod.Instance?.Logger.LogWarning("Renderforge D3D12: auto-exposure kernels fixed=" + Fixed + " progressive=" + Progressive + "; unsupported adaptation skipped.");
            }
        }

        private static readonly ConditionalWeakTable<ComputeShader, HistogramKernels> histogramKernels = new ConditionalWeakTable<ComputeShader, HistogramKernels>();
        private static readonly ConditionalWeakTable<ComputeShader, ExposureKernels> exposureKernels = new ConditionalWeakTable<ComputeShader, ExposureKernels>();

        private static bool HasKernel(ComputeShader shader, string name)
        {
            try { return shader.HasKernel(name); }
            catch { return false; } // A disposed/broken asset is unsupported; the cached result prevents log/exception spam.
        }

        internal static bool HistogramSupported(PostProcessRenderContext context)
        {
            var shader = context?.resources?.computeShaders?.exposureHistogram;
            return shader && histogramKernels.GetValue(shader, s => new HistogramKernels(s)).Supported;
        }

        internal static bool ExposureSupported(PostProcessRenderContext context, EyeAdaptation adaptation)
        {
            if (!HistogramSupported(context)) return false;
            var shader = context?.resources?.computeShaders?.autoExposure;
            if (!shader) return false;
            var kernels = exposureKernels.GetValue(shader, s => new ExposureKernels(s));
            // AutoExposureRenderer always uses fixed on history reset; progressive is only needed by that mode.
            return kernels.Fixed && (adaptation == EyeAdaptation.Fixed || kernels.Progressive);
        }

        internal static bool Active
        {
            get { return SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12; }
        }

        internal static void Apply()
        {
            if (!Active) return;
            try
            {
                foreach (var layer in UnityEngine.Object.FindObjectsOfType<PostProcessLayer>())
                {
                    var res = Traverse.Create(layer).Field("m_Resources").GetValue<PostProcessResources>();
                    if (res != null && res.computeShaders != null)
                    {
                        res.computeShaders.lut3DBaker = null;
                        // ScreenSpaceReflections.IsEnabledAndSupported returns this shader's truthiness after the same
                        // false-positive supportsComputeShaders gate; null = SSR skipped instead of dispatching an absent kernel.
                        res.computeShaders.gaussianDownsample = null;
                    }
                }
                foreach (var volume in UnityEngine.Object.FindObjectsOfType<PostProcessVolume>())
                    FixAo(volume.profile);
            }
            catch (Exception ex)
            {
                if (!loggedError && RenderforgeMod.Instance != null)
                    RenderforgeMod.Instance.Logger.LogError("Renderforge D3D12 fix failed: " + ex);
                loggedError = true;
            }
        }

        private static void FixAo(PostProcessProfile profile)
        {
            AmbientOcclusion ao;
            if (profile == null || !profile.TryGetSettings(out ao) || ao == null) return;
            if (DisableAo) ao.enabled.value = false;
            else ao.mode.Override(AmbientOcclusionMode.ScalableAmbientObscurance);
        }

        /// <summary>PPCLI switch for the SAO-vs-off test:
        /// {"op":"invoke","type":"Renderforge.D3D12Fix","assembly":"Renderforge","member":"SetAo","args":["off"]}</summary>
        public static string SetAo(string mode)
        {
            DisableAo = string.Equals(mode, "off", StringComparison.OrdinalIgnoreCase);
            Apply();
            return "d3d12=" + Active + " ao=" + (DisableAo ? "off" : "sao");
        }
    }

    [HarmonyPatch(typeof(AutoExposure), nameof(AutoExposure.IsEnabledAndSupported))]
    internal static class AutoExposure_D3D12Support_Patch
    {
        static void Postfix(AutoExposure __instance, PostProcessRenderContext context, ref bool __result)
        {
            if (__result && D3D12Fix.Active)
                __result = D3D12Fix.ExposureSupported(context, __instance.eyeAdaptation.value);
            // RenderBuiltins already initializes autoExposureTexture to white: skipping this effect retains
            // ordinary fixed exposure, tone mapping, fog of war and every other supported post effect.
        }
    }

    [HarmonyPatch(typeof(Monitor), nameof(Monitor.IsRequestedAndSupported))]
    internal static class LightMeter_D3D12Support_Patch
    {
        static void Postfix(Monitor __instance, PostProcessRenderContext context, ref bool __result)
        {
            // Block the histogram request and the debug layer's aggregate support test.
            if (__result && __instance is LightMeterMonitor && D3D12Fix.Active)
                __result = D3D12Fix.HistogramSupported(context);
        }
    }

    [HarmonyPatch(typeof(LightMeterMonitor), "Render")]
    internal static class LightMeter_D3D12Render_Patch
    {
        static bool Prefix(PostProcessRenderContext context)
        {
            // RenderMonitors checks support only when deciding whether ANY monitor can run, then renders
            // every requested monitor. A supported second monitor must not bypass this consumer guard.
            return !D3D12Fix.Active || D3D12Fix.HistogramSupported(context);
        }
    }
}
