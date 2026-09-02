using System;
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
                    if (res != null && res.computeShaders != null) res.computeShaders.lut3DBaker = null;
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
}
