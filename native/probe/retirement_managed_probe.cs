// Critical driver methods are copied verbatim by run_retirement_probe.ps1; Unity/vendor boundaries are fake.
using System;
using UnityEngine;
using UnityEngine.Rendering;
namespace UnityEngine {
 public class Object { public static int Destroyed; public static void Destroy(object o) { Destroyed++; } }
 public class MonoBehaviour : Object { public GameObject gameObject = new GameObject(); protected static void DontDestroyOnLoad(object o) {} }
 public enum HideFlags { HideAndDontSave }
 public class GameObject { public string name; public HideFlags hideFlags; public GameObject(string n="") {name=n;} public T AddComponent<T>() where T:new() {return new T();} }
 public class Camera { public static Action<Camera> onPostRender; public GameObject gameObject=new GameObject(); }
 public class RenderTexture { public int Releases; public void Release() {Releases++;} }
 public enum KeyCode { LeftControl,RightControl,LeftAlt,RightAlt }
 public static class Input { public static bool GetKey(KeyCode k)=>false; public static bool GetKeyDown(KeyCode k)=>false; }
 public static class Time { public static float unscaledTime=1; }
 public static class GL { public static int Queued; public static void IssuePluginEvent(IntPtr f,int id) {Queued++;} }
}
namespace UnityEngine.Rendering { public class CommandBuffer { public int Releases; public void Release() {Releases++;} } }
namespace Renderforge {
 enum FrameGenMode { Off, X2, X3, X4 } enum Feature { FrameGen }
 enum RenderforgeMode { Off } enum DebugView { None } enum UpscalerKind { Off }
 class DlssConfig { public FrameGenMode FrameGen=FrameGenMode.X2; public KeyCode ToggleHotkey,OverlayHotkey; }
 static class Availability { public static bool IsD3D12=true,IsNvidia=true,IsIntel=false; public static string Reason(Feature f)=>null; }
 static class Upscalers { public static bool SlDllsPresent=true,XessFgDllsPresent=true,FsrFgDllPresent=true; }
 static class Overlay { public static int FgFps; }
 class Log { public void LogInfo(string s){} public void LogWarning(string s){} }
 class RenderforgeMod { public static RenderforgeMod Instance=new RenderforgeMod(); public Log Logger=new Log(); public DlssConfig Cfg=new DlssConfig(); public static string ModDir=""; public static void ApplyFrameRate(){} public static void Toggle(){} public static void ToggleOverlay(){} }
 static class MipBias { public static void Reset(){} }
 static class Native {
 public const int FG_PROVIDER_NONE=0,FG_PROVIDER_DLSS=1,FG_PROVIDER_FSR=2,FG_PROVIDER_XESS=3,FG_ERR_NO_PROVIDER=-1,FG_ERR_PROVIDER_FAILED=-2,FG_OK=0,DLSS_EV_RELEASE=3;
 public static IntPtr Handle=(IntPtr)1; public static int ShutdownReady=1,Alive=0,InitCalls,Result=1;
 public static int Fg_Pump()=>ShutdownReady; public static int Fg_Shutdown(){if(ShutdownReady==1)Alive=0; return ShutdownReady;}
 public static int Fg_Alive()=>Alive; public static int Fg_Provider()=>FG_PROVIDER_DLSS; public static uint Fg_Caps()=>1;
 public static int Fg_Init(int p,uint m,string dir){InitCalls++;Alive=1;return 0;} public static void Fg_SetEnabled(int on){}
 public static string Fg_Status()=>"mock"; public static string Fg_Reason()=>"mock";
 public static void Dlss_Passthrough(int i){} public static void Dlss_BeginRelease(){Result=0;} public static int Dlss_ReleaseStatus()=>Result;
 }
 partial class DlssDriver : MonoBehaviour {
 public static DlssDriver Instance {get;private set;}
 private enum Gen { Idle,Creating,Live,Releasing }
 internal bool Retiring => gen == Gen.Releasing;
 private Gen gen; private bool shutdownRequested,releaseQueued,broken; private static bool quitting; private static DlssDriver orphan;
 private RenderforgeMode wantMode; private UpscalerKind switchTo; private int genFrames;
 private RenderTexture colorRT,depthRT,mvRT,outRT; private CommandBuffer cbCopy,cbEval,cbPresent; private Camera present;
 private IntPtr colorPtr,depthPtr,mvPtr,outPtr,evFn;
 private void Detach(){} private void OnCameraPostRender(Camera c){}
 private int stepCalls; private void Step(){stepCalls++;} private void Fail(string why){throw new Exception(why);}
 internal static void RunProbe() {
 var cfg=new DlssConfig(); FrameGen.Apply(cfg); Check(FrameGen.Live,"FG starts");
 Native.ShutdownReady=0; Check(!FrameGen.Release() && FrameGen.Retiring && !FrameGen.Live,"pending separate from live");
 int inits=Native.InitCalls; FrameGen.Apply(cfg); FrameGen.Retry(); Check(Native.InitCalls==inits,"no reinit while pending");
 var d=Create(); var rt=new RenderTexture(); var cb=new CommandBuffer(); d.colorRT=rt; d.cbEval=cb; d.gen=Gen.Live;
 d.broken=true; d.RequestShutdown(); Check(d.shutdownRequested,"disable owns retirement");
 for(int i=0;i<150;i++)Check(!d.TryRetire() && rt.Releases==0 && cb.Releases==0 && GL.Queued==0,"FG pending keeps Unity resources");
 var resumed=Create(); Check(ReferenceEquals(resumed,d) && !d.shutdownRequested && !d.broken,"re-enable reuses sole retirement owner");
 Native.ShutdownReady=1; Check(!d.TryRetire() && GL.Queued==1,"FG complete queues render ack");
 for(int i=0;i<150;i++)Check(!d.TryRetire() && GL.Queued==1 && rt.Releases==0,"queued event cannot duplicate or free early");
 Native.Result=-1; Check(!d.TryRetire() && GL.Queued==2 && rt.Releases==0,"GPU pending queues single retry");
 Native.Result=1; Check(d.TryRetire() && rt.Releases==1 && cb.Releases==1,"GPU completion releases Unity resources once");
 Check(d.TryRetire() && rt.Releases==1 && cb.Releases==1,"repeat retirement idempotent");
 d.gen=Gen.Idle; Time.unscaledTime+=1; FrameGen.Apply(cfg); Check(FrameGen.Live && Native.InitCalls==inits+1,"FG resumes after retirement");
 FrameGen.Stop(); Check(!FrameGen.Live && !FrameGen.Retiring,"normal stop");
 d.RequestShutdown(); d.gen=Gen.Idle; d.broken=true; d.Update(); Check(d.stepCalls==1,"broken idle still reaches shutdown step");
 d.colorRT=new RenderTexture(); Native.ShutdownReady=0; d.OnDestroy(); Check(ReferenceEquals(orphan,d) && Instance!=null && !ReferenceEquals(Instance,d),"unexpected destruction transfers one owner");
 var orphanRt=d.colorRT; Instance.Update(); Check(orphanRt.Releases==0 && ReferenceEquals(orphan,d),"successor retains pending orphan");
 Native.ShutdownReady=1; Instance.Update(); Native.Result=1; Instance.Update();
 Check(orphan==null && orphanRt.Releases==1,"successor drains orphan once");
 Console.WriteLine("managed retirement probe: PASS "+checks+" checks (source-linked FrameGen + verbatim driver retirement methods)");
 }
 private static int checks; private static void Check(bool v,string label){checks++;if(!v)throw new Exception(label);}
 }
 class Program { static void Main()=>DlssDriver.RunProbe(); }
}
