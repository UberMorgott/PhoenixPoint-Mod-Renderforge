// Runs the exact production support gates with instrumented engine boundaries, without loading Unity.
// The normal Release build separately verifies installed Unity/Harmony API compatibility.
using System;
using System.Collections.Generic;
using System.Reflection;
using Renderforge;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

static class Probe
{
    static int checks;
    static readonly string[] Names = { "KEyeHistogramClear", "KEyeHistogram", "KAutoExposureAvgLuminance_fixed", "KAutoExposureAvgLuminance_progressive" };
    static void Check(bool value, string message) { checks++; if (!value) throw new Exception(message); }
    static bool Gate(Type patch, object instance, PostProcessRenderContext context, bool original)
    {
        object[] args = { instance, context, original };
        patch.GetMethod("Postfix", BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, args);
        return (bool)args[2];
    }
    static PostProcessRenderContext Context(int mask)
    {
        var c = new PostProcessRenderContext();
        for (int i = 0; i < Names.Length; ++i)
            if ((mask & (1 << i)) != 0) (i < 2 ? c.resources.computeShaders.exposureHistogram : c.resources.computeShaders.autoExposure).kernels.Add(Names[i]);
        return c;
    }
    static int Calls(PostProcessRenderContext c) => c.resources.computeShaders.exposureHistogram.calls + c.resources.computeShaders.autoExposure.calls;
    static bool RenderLightMeter(PostProcessRenderContext c) => (bool)typeof(LightMeter_D3D12Render_Patch).GetMethod("Prefix", BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, new object[] { c });
    static void Main()
    {
        foreach (var api in new[] { GraphicsDeviceType.Direct3D11, GraphicsDeviceType.Direct3D12 })
        foreach (bool original in new[] { false, true })
        for (int mask = 0; mask < 16; ++mask)
        foreach (var mode in new[] { EyeAdaptation.Fixed, EyeAdaptation.Progressive })
        {
            SystemInfo.graphicsDeviceType = api;
            var c = Context(mask); var histogram = c.resources.computeShaders.exposureHistogram; var exposure = c.resources.computeShaders.autoExposure;
            var effect = new AutoExposure(); effect.eyeAdaptation.value = mode;
            bool guarded = api == GraphicsDeviceType.Direct3D12;
            bool expected = original && (!guarded || ((mask & 7) == 7 && (mode == EyeAdaptation.Fixed || (mask & 8) != 0)));
            Check(Gate(typeof(AutoExposure_D3D12Support_Patch), effect, c, original) == expected, "exposure capability matrix");
            int calls = Calls(c);
            for (int frame = 0; frame < 10; ++frame) Check(Gate(typeof(AutoExposure_D3D12Support_Patch), effect, c, original) == expected, "stable exposure result");
            Check(Calls(c) == calls, "support probes repeated per frame");
            if (!guarded || !original) Check(calls == 0, "inactive/D3D11 path queried kernels");
            Check(ReferenceEquals(histogram, c.resources.computeShaders.exposureHistogram) && ReferenceEquals(exposure, c.resources.computeShaders.autoExposure), "resources mutated");
            Check(effect.eyeAdaptation.value == mode, "profile mode mutated");
            foreach (Monitor monitor in new Monitor[] { new LightMeterMonitor(), new OtherMonitor() })
            {
                bool monitorExpected = original && (!guarded || !(monitor is LightMeterMonitor) || (mask & 3) == 3);
                Check(Gate(typeof(LightMeter_D3D12Support_Patch), monitor, c, original) == monitorExpected, "monitor request/generation/consumer gate");
            }
            Check(RenderLightMeter(c) == (!guarded || (mask & 3) == 3), "direct light-meter consumer gate");
            // Installed RenderMonitors first ORs supported requests, then dispatches ALL requested monitors.
            bool anySupported = Gate(typeof(LightMeter_D3D12Support_Patch), new LightMeterMonitor(), c, true)
                || Gate(typeof(LightMeter_D3D12Support_Patch), new OtherMonitor(), c, true);
            Check(anySupported, "supported second monitor must remain available");
            bool lightMeterDispatched = anySupported && RenderLightMeter(c);
            Check(lightMeterDispatched == (!guarded || (mask & 3) == 3), "mixed monitor request bypassed light-meter guard");
        }
        SystemInfo.graphicsDeviceType = GraphicsDeviceType.Direct3D12;
        var missing = Context(15); missing.resources.computeShaders.exposureHistogram = null;
        Check(!D3D12Fix.HistogramSupported(missing), "missing shader must be unsupported");
        Check(!D3D12Fix.HistogramSupported(null), "missing context must be unsupported");
        missing.resources.computeShaders.exposureHistogram = new ComputeShader { destroyed = true };
        Check(!D3D12Fix.HistogramSupported(missing), "destroyed shader must be unsupported");
        var broken = Context(15); broken.resources.computeShaders.exposureHistogram.throws = true;
        for (int i = 0; i < 20; ++i) Check(!D3D12Fix.HistogramSupported(broken), "throwing asset fallback");
        Check(broken.resources.computeShaders.exposureHistogram.calls == 1, "broken asset exception must be cached");
        var replaced = Context(0); Check(!D3D12Fix.HistogramSupported(replaced), "initial unsupported resource");
        replaced.resources.computeShaders.exposureHistogram = Context(15).resources.computeShaders.exposureHistogram;
        Check(D3D12Fix.HistogramSupported(replaced), "replacement resource must be checked independently");
        Console.WriteLine("PASS " + checks + " production support-gate checks; engine stubs, no live rendering claim");
    }
}

namespace HarmonyLib
{
    [AttributeUsage(AttributeTargets.Class)] public sealed class HarmonyPatch : Attribute { public HarmonyPatch(Type type, string method) { } }
    public sealed class Traverse { public static Traverse Create(object value) => new Traverse(); public Traverse Field(string name) => this; public T GetValue<T>() => default; }
}
namespace UnityEngine
{
    public class Object
    {
        public bool destroyed;
        public static implicit operator bool(Object value) => value != null && !value.destroyed;
        public static T[] FindObjectsOfType<T>() => Array.Empty<T>();
    }
    public sealed class ComputeShader : Object
    {
        public HashSet<string> kernels = new HashSet<string>(); public int calls; public bool throws;
        public bool HasKernel(string name) { calls++; if (throws) throw new InvalidOperationException("disposed asset"); return kernels.Contains(name); }
    }
    public static class SystemInfo { public static GraphicsDeviceType graphicsDeviceType; }
    public sealed class AssetBundle : Object { public static AssetBundle LoadFromFile(string path) => null; public T LoadAsset<T>(string name) where T : Object => null; }
}
namespace UnityEngine.Rendering { public enum GraphicsDeviceType { Direct3D11, Direct3D12 } }
namespace UnityEngine.Rendering.PostProcessing
{
    public enum EyeAdaptation { Fixed, Progressive }
    public class Parameter<T> { public T value; public void Override(T next) => value = next; }
    public class AutoExposure { public Parameter<EyeAdaptation> eyeAdaptation = new Parameter<EyeAdaptation>(); public bool IsEnabledAndSupported(PostProcessRenderContext c) => true; }
    public class Monitor { public bool IsRequestedAndSupported(PostProcessRenderContext c) => true; }
    public class LightMeterMonitor : Monitor { }
    public class OtherMonitor : Monitor { }
    public class PostProcessRenderContext { public PostProcessResources resources = new PostProcessResources(); }
    public class PostProcessResources
    {
        public ComputeShaders computeShaders = new ComputeShaders();
        public sealed class ComputeShaders { public ComputeShader exposureHistogram = new ComputeShader(), autoExposure = new ComputeShader(), lut3DBaker, gaussianDownsample; }
    }
    public class PostProcessLayer : UnityEngine.Object { }
    public class PostProcessVolume : UnityEngine.Object { public PostProcessProfile profile; }
    public class PostProcessProfile { public bool TryGetSettings(out AmbientOcclusion ao) { ao = null; return false; } }
    public enum AmbientOcclusionMode { ScalableAmbientObscurance }
    public class AmbientOcclusion { public Parameter<bool> enabled = new Parameter<bool>(); public Parameter<AmbientOcclusionMode> mode = new Parameter<AmbientOcclusionMode>(); }
}
namespace Renderforge
{
    public class RenderforgeMod { public static RenderforgeMod Instance; public static string ModDir; public Logger Logger = new Logger(); }
    public class Logger { public void LogError(string message) { } public void LogWarning(string message) { } public void LogInfo(string message) { } }
}
