using Base.Cameras;
using Base.Lighting;
using HarmonyLib;
using PhoenixPoint.Common.Core;
using UnityEngine.Rendering.PostProcessing;

namespace Renderforge
{
    /// <summary>PPv2's own OnPreCull resets projectionMatrix and re-assigns nonJitteredProjectionMatrix every frame
    /// (PostProcessLayer.cs:326-328 decompiled), so the jitter must be applied AFTER it - a postfix, not a sibling message.</summary>
    [HarmonyPatch(typeof(PostProcessLayer), "OnPreCull")]
    internal static class PostProcessLayer_OnPreCull_Patch
    {
        static void Postfix(PostProcessLayer __instance) => DlssDriver.Instance?.AfterPostProcessPreCull(__instance);
    }

    /// <summary>LightingManager.ApplyPostProcessOptions (LightingManager.cs:180-185) sets SMAA on every layer from the
    /// preset; SMAA over DLSS = blur, so force None on the layer we drive while DLSS is live.</summary>
    [HarmonyPatch(typeof(LightingManager), "ApplyPostProcessOptions")]
    internal static class LightingManager_ApplyPostProcessOptions_Patch
    {
        static void Postfix()
        {
            DlssDriver.Instance?.AfterApplyPostProcessOptions();
            D3D12Fix.Apply();          // AO + lut3DBaker repair; no-op unless the process runs D3D12
        }
    }

    /// <summary>CameraManager.Camera SWAPS after OnLevelStart: the level's Loaded->Playing transition queues
    /// SetOverrideCameraCrt (CameraManager.cs:232, priority -2000), which makes the scene's own camera (GeoscapeCamera,
    /// the tactical camera) the override and deactivates the prefab MainCamera the mod had just bound. Left alone the
    /// driver either stays Idle on the inactive camera (DLSS silently off) or, if it went Live first, keeps presenting a
    /// stale outRT over the real camera (black screen with HUD until toggled). Rebind on every swap.</summary>
    [HarmonyPatch(typeof(CameraManager), "SetOverrideCamera")]
    internal static class CameraManager_SetOverrideCamera_Patch
    {
        static void Postfix(CameraManager __instance) => DlssDriver.Instance?.Attach(__instance.Camera);
    }

    /// <summary>OptionsManager.InitVideoOptions (OptionsManager.cs:505) is the game's ONLY SetFrameRateLimit(60) call
    /// (no platform subclass overrides Platform.SetFrameRateLimit, Platform.cs:376); re-apply the config value after it.</summary>
    [HarmonyPatch(typeof(OptionsManager), "InitVideoOptions")]
    internal static class OptionsManager_InitVideoOptions_Patch
    {
        static void Postfix() => RenderforgeMod.ApplyFrameRate();
    }
}
