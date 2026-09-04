using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Newtonsoft.Json.Linq;
using PhoenixPoint.Common.Entities.Addons;
using PhoenixPoint.Common.View.ViewModules;
using UnityEngine;
using UnityEngine.Rendering;

namespace Renderforge
{
    /// <summary>Opt-in one-frame depth-contract experiment, not an enhancement input.</summary>
    public sealed partial class CharacterMaskDiagnostic : MonoBehaviour
    {
        private Camera camera;
        private RenderTexture initialTarget;
        private int initialWidth, initialHeight;
        private CommandBuffer commands, outputCommands;
        private readonly List<RenderTexture> targets = new List<RenderTexture>();
        private readonly List<Material> materials = new List<Material>();
        private Dictionary<Renderer, Material[]> originals;
        private Renderer[] selected;
        private SkinnedMeshRenderer face;
        private string directory, expectedPrefab, fingerprint;
        private int startedFrame;
        private bool closeUp, cameraMoved;
        private Vector3 savedPosition;
        private Quaternion savedRotation;
        private float savedFov;
        private Texture2D cutoutTexture;
        private UIModuleActorCycle actorCycle;
        private object actor;
        private string faceTag;
        private GameObject occluder;
        private Mesh occluderMesh;
        private bool finished;
        private JObject evidence;

        public static string Capture(int cameraId, string prefab, string outputDirectory, int occluderMode, bool closeUp)
        {
            if (FindObjectsOfType<MonoBehaviour>().Any(c => c.GetType().FullName == typeof(CharacterMaskDiagnostic).FullName))
                throw new InvalidOperationException("A character mask diagnostic is already running.");
            var cam = Camera.allCameras.Single(c => c.GetInstanceID() == cameraId);
            if (!Path.IsPathRooted(outputDirectory)) throw new ArgumentException("Absolute output directory required.");
            if (cam.targetTexture == null || cam.targetTexture.antiAliasing != 1)
                throw new InvalidOperationException("Diagnostic requires a camera render target with MSAA 1.");
            var go = new GameObject("Renderforge mask diagnostic");
            var probe = go.AddComponent<CharacterMaskDiagnostic>();
            try
            {
                probe.camera = cam;
                probe.initialTarget = cam.targetTexture;
                probe.initialWidth = cam.targetTexture.width;
                probe.initialHeight = cam.targetTexture.height;
                probe.directory = outputDirectory;
                probe.expectedPrefab = prefab;
                if (occluderMode < 0 || occluderMode > 2) throw new ArgumentOutOfRangeException(nameof(occluderMode));
                probe.closeUp = closeUp;
                probe.SelectCurrent();
                Directory.CreateDirectory(outputDirectory);
                probe.evidence = new JObject { ["status"] = "pending", ["expectedPrefab"] = prefab,
                    ["camera"] = cam.name, ["cameraId"] = cameraId, ["depthSource"] = "CameraTarget", ["faceTag"] = probe.faceTag,
                    ["cameraTargetDepthBits"] = cam.targetTexture.depth, ["reversedZ"] = SystemInfo.usesReversedZBuffer,
                    ["width"] = cam.targetTexture.width, ["height"] = cam.targetTexture.height,
                    ["faceRenderer"] = probe.face.name, ["faceRendererId"] = probe.face.GetInstanceID(),
                    ["occluderMode"] = occluderMode, ["closeUp"] = closeUp };
                if (closeUp) probe.MoveCamera();
                if (occluderMode != 0) probe.CreateOccluder(occluderMode == 2);
                probe.RestoreCamera();
                probe.startedFrame = Time.frameCount;
                Camera.onPreCull += probe.PrepareCamera;
                Camera.onPreRender += probe.BeforeCamera;
                Camera.onPostRender += probe.AfterCamera;
                probe.WriteEvidence();
                return "scheduled=" + outputDirectory;
            }
            catch { probe.Cleanup(); Destroy(go); throw; }
        }

        private void SelectCurrent()
        {
            actorCycle = FindObjectsOfType<UIModuleActorCycle>().Single(c => c.isActiveAndEnabled && c.CurrentCharacter != null);
            actor = actorCycle.CurrentCharacter;
            faceTag = actorCycle.CurrentCharacter.Identity.FaceTag.Guid.ToString();
            string expectedFaceTag = expectedPrefab == "Head - Tutorial - Female_Ready" ? "276b5f5f-1d18-aa64-5aef-6298b23430d6" : "b522b546-b9d3-d564-3902-d78541d04c44";
            if (faceTag != expectedFaceTag) throw new InvalidOperationException("Selected actor identity does not match the verified head variant.");
            var candidates = new HashSet<AddonsManager>();
            foreach (var component in FindObjectsOfType<MonoBehaviour>())
            {
                var manager = (component as IAddonsManagerProvider)?.AddonsManager;
                if (manager?.RootAddon != null) candidates.Add(manager);
            }
            if (candidates.Count != 1) throw new InvalidOperationException("Diagnostic requires the verified single-character roster scene.");
            var matches = candidates.Where(m => m.RootAddon.Any(a =>
                a.VisualRoot != null && a.VisualsSourcePrefab != null && a.VisualsSourcePrefab.name == expectedPrefab)).ToArray();
            if (matches.Length != 1) throw new InvalidOperationException("Expected exactly one CURRENT head variant: " + expectedPrefab);
            var current = matches[0].RootAddon.Where(a => a.VisualRoot != null && a.IsVisible).ToArray();
            selected = current.SelectMany(a => a.VisualRoot.GetComponentsInChildren<Renderer>(false))
                .Where(r => r.enabled && r.gameObject.activeInHierarchy && (camera.cullingMask & (1 << r.gameObject.layer)) != 0)
                .Distinct().ToArray();
            string meshName;
            switch (expectedPrefab)
            {
                case "Head - Tutorial - Female_Ready": meshName = "Tutorial_Head_Female"; break;
                case "Head_Hispanic1_F_V01_Ready": meshName = "Head_Hispanic1_F_V01"; break;
                default: throw new InvalidOperationException("Unverified face variant; fail closed.");
            }
            face = selected.OfType<SkinnedMeshRenderer>().Single(r => r.sharedMesh != null && r.sharedMesh.name == meshName);
            if (face.sharedMesh.subMeshCount != 1) throw new InvalidOperationException("Unverified face submesh layout.");
            originals = selected.ToDictionary(r => r, r => r.sharedMaterials);
            fingerprint = CurrentFingerprint();
        }

        private string CurrentFingerprint()
        {
            if (camera == null || camera.targetTexture != initialTarget || initialTarget == null
                || initialTarget.width != initialWidth || initialTarget.height != initialHeight)
                return "camera target changed";
            if (actorCycle == null || actorCycle.CurrentCharacter != actor || actorCycle.CurrentCharacter.Identity.FaceTag.Guid.ToString() != faceTag)
                return "identity changed";
            var parts = new List<string>();
            var seen = new HashSet<AddonsManager>();
            foreach (var component in FindObjectsOfType<MonoBehaviour>())
            {
                var manager = (component as IAddonsManagerProvider)?.AddonsManager;
                if (manager?.RootAddon == null || !seen.Add(manager)) continue;
                foreach (var a in manager.RootAddon)
                    parts.Add(a.AddonDef?.Guid + ":" + (a.VisualRoot != null ? a.VisualRoot.GetInstanceID().ToString() : "null")
                        + ":" + (a.VisualRoot != null && a.VisualsSourcePrefab != null ? a.VisualsSourcePrefab.name : "null"));
            }
            return string.Join("|", parts.OrderBy(p => p, StringComparer.Ordinal));
        }

        private Material MaskMaterial(CompareFunction comparison)
        {
            var shader = Shader.Find("Hidden/Internal-Colored");
            if (shader == null || !shader.isSupported) throw new InvalidOperationException("Internal-Colored unavailable.");
            var mat = new Material(shader);
            materials.Add(mat);
            foreach (var property in new[] { "_Color", "_ZTest", "_ZWrite", "_Cull", "_SrcBlend", "_DstBlend" })
                if (!mat.HasProperty(property)) throw new InvalidOperationException("Missing shader property: " + property);
            mat.SetColor("_Color", Color.white);
            mat.SetInt("_ZTest", (int)comparison);
            mat.SetInt("_ZWrite", 0);
            mat.SetInt("_Cull", (int)CullMode.Back);
            mat.SetInt("_SrcBlend", (int)BlendMode.One);
            mat.SetInt("_DstBlend", (int)BlendMode.Zero);
            return mat;
        }

        private RenderTexture Target(string name)
        {
            var target = new RenderTexture(camera.targetTexture.width, camera.targetTexture.height, 0, RenderTextureFormat.ARGB32);
            target.name = "Mask diagnostic " + name;
            target.filterMode = FilterMode.Point;
            target.Create();
            targets.Add(target);
            return target;
        }

        private void BeforeCamera(Camera current)
        {
            if (current != camera || finished || commands != null || Time.frameCount - startedFrame < Math.Max(prototypeWarmupFrames, closeUp ? 32 : 2)) return;
            try
            {
                if (fingerprint != CurrentFingerprint()) throw new InvalidOperationException("Visual generation changed; capture cancelled.");
                commands = new CommandBuffer { name = "Renderforge diagnostic masks" };
                var equal = MaskMaterial(CompareFunction.Equal);
                var always = MaskMaterial(CompareFunction.Always);
                DrawMask(Target("character"), selected, equal);
                DrawMask(Target("face"), new Renderer[] { face }, equal);
                DrawMask(Target("face-unoccluded"), new Renderer[] { face }, always);
                commands.Blit(BuiltinRenderTextureType.CameraTarget, Target("source"));
                commands.SetRenderTarget(BuiltinRenderTextureType.CameraTarget);
                camera.AddCommandBuffer(CameraEvent.BeforeImageEffects, commands);
                var driver = Resources.FindObjectsOfTypeAll<MonoBehaviour>().Single(c => c.GetType().FullName == "Renderforge.DlssDriver");
                var output = (RenderTexture)driver.GetType().GetField("outRT", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(driver);
                if (output == null || output.width != camera.targetTexture.width || output.height != camera.targetTexture.height)
                    throw new InvalidOperationException("Diagnostic requires DLAA output at camera resolution.");
                outputCommands = new CommandBuffer { name = "Renderforge diagnostic reconstructed output" };
                outputCommands.Blit(output, Target("output"));
                camera.AddCommandBuffer(CameraEvent.AfterEverything, outputCommands);
                evidence["renderPosition"] = camera.transform.position.ToString("R");
                evidence["renderRotation"] = camera.transform.rotation.ToString("R");
                evidence["renderFov"] = camera.fieldOfView;
                evidence["warmupFrames"] = Math.Max(prototypeWarmupFrames, closeUp ? 32 : 2);
                evidence["renderFrame"] = Time.frameCount;
                evidence["renderers"] = new JArray(selected.Select(r => new JObject {
                    ["id"] = r.GetInstanceID(), ["name"] = r.name, ["materials"] = new JArray(r.sharedMaterials.Select(m => m.GetInstanceID())) }));
                evidence["generation"] = fingerprint;
            }
            catch (Exception exception) { Fail(exception); }
        }

        private void DrawMask(RenderTexture target, IEnumerable<Renderer> renderers, Material material)
        {
            commands.SetRenderTarget(target, BuiltinRenderTextureType.CameraTarget);
            commands.ClearRenderTarget(false, true, Color.black);
            foreach (var renderer in renderers)
            {
                var mesh = (renderer as SkinnedMeshRenderer)?.sharedMesh ?? renderer.GetComponent<MeshFilter>()?.sharedMesh;
                if (mesh == null) continue;
                for (int slot = 0; slot < mesh.subMeshCount; slot++) commands.DrawRenderer(renderer, material, slot, 0);
            }
        }

        private void AfterCamera(Camera current)
        {
            if (current != camera || finished) return;
            RestoreCamera();
            if (commands == null) return;
            Camera.onPostRender -= AfterCamera;
            RestoreCamera();
            StartCoroutine(Readback());
        }

        private IEnumerator Readback()
        {
            yield return new WaitForEndOfFrame();
            try
            {
                if (fingerprint != CurrentFingerprint()) throw new InvalidOperationException("Visual generation changed before readback.");
                if (originals.Any(p => p.Key == null || !p.Value.SequenceEqual(p.Key.sharedMaterials)))
                    throw new InvalidOperationException("Original shared materials changed during diagnostic.");
                camera.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, commands);
                var counts = new JObject();
                string[] names = { "character", "face", "face-unoccluded", "source", "output" };
                for (int i = 0; i < targets.Count; i++) counts[names[i]] = Save(targets[i], names[i]);
                evidence["nonBlackPixels"] = counts;
                if ((int)counts["face-unoccluded"] == 0 || (int)counts["source"] == 0 || (int)counts["output"] == 0)
                    throw new InvalidOperationException("Empty control or source image; mask contract was not established.");
                evidence["sharedMaterialsUnchanged"] = authoredFaceMaterials == null;
                evidence["captureMaterialsStable"] = true;
                evidence["status"] = "captured";
                Finish();
            }
            catch (Exception exception) { Fail(exception); }
        }

        private int Save(RenderTexture target, string name)
        {
            var previous = RenderTexture.active;
            var texture = new Texture2D(target.width, target.height, TextureFormat.RGB24, false);
            try
            {
                RenderTexture.active = target;
                texture.ReadPixels(new Rect(0, 0, target.width, target.height), 0, 0);
                texture.Apply();
                File.WriteAllBytes(Path.Combine(directory, name + ".png"), texture.EncodeToPNG());
                return texture.GetPixels32().Count(p => p.r > 0 || p.g > 0 || p.b > 0);
            }
            finally { RenderTexture.active = previous; Destroy(texture); }
        }

        private void PrepareCamera(Camera current)
        {
            if (current != camera || finished || Time.frameCount <= startedFrame) return;
            try
            {
                if (fingerprint != CurrentFingerprint()) throw new InvalidOperationException("Visual generation changed; capture cancelled.");
                if (closeUp) MoveCamera();
            }
            catch (Exception exception) { Fail(exception); }
        }

        private void MoveCamera()
        {
            if (cameraMoved) return;
            savedPosition = camera.transform.position;
            savedRotation = camera.transform.rotation;
            savedFov = camera.fieldOfView;
            cameraMoved = true;
            camera.transform.rotation = Quaternion.AngleAxis(prototypeYaw, Vector3.up) * savedRotation;
            camera.transform.position = face.bounds.center - camera.transform.forward * 1.15f;
            camera.fieldOfView = 26f;
        }

        private void RestoreCamera()
        {
            if (!cameraMoved || camera == null) return;
            camera.transform.position = savedPosition;
            camera.transform.rotation = savedRotation;
            camera.fieldOfView = savedFov;
            cameraMoved = false;
        }

        private void CreateOccluder(bool cutout)
        {
            var bounds = face.bounds;
            var center = bounds.center;
            var distance = Vector3.Dot(center - camera.transform.position, camera.transform.forward);
            var position = center - camera.transform.forward * Mathf.Max(0.15f, bounds.extents.z * 2f);
            float size = Mathf.Max(bounds.extents.x, bounds.extents.y);
            occluder = new GameObject("Renderforge temporary depth occluder");
            occluder.layer = face.gameObject.layer;
            occluder.transform.position = position - camera.transform.right * size * 0.4f;
            occluder.transform.rotation = camera.transform.rotation;
            occluderMesh = new Mesh { name = "Temporary diagnostic occluder quad" };
            occluderMesh.vertices = new[] { new Vector3(-size * .5f, -size, 0), new Vector3(-size * .5f, size, 0),
                new Vector3(size * .5f, size, 0), new Vector3(size * .5f, -size, 0) };
            occluderMesh.triangles = new[] { 0, 1, 2, 0, 2, 3, 2, 1, 0, 3, 2, 0 };
            occluderMesh.RecalculateBounds();
            occluder.AddComponent<MeshFilter>().sharedMesh = occluderMesh;
            var shader = Shader.Find(cutout ? "Unlit/Colored Cutout" : "Unlit/Color");
            if (shader == null || !shader.isSupported) throw new InvalidOperationException("Occluder shader unavailable.");
            var mat = new Material(shader);
            materials.Add(mat);
            foreach (var property in cutout ? new[] { "_Color", "_MainTex", "_Cutoff" } : new[] { "_Color" })
                if (!mat.HasProperty(property)) throw new InvalidOperationException("Missing occluder shader property: " + property);
            mat.SetColor("_Color", Color.magenta);
            if (cutout)
            {
                cutoutTexture = new Texture2D(2, 2, TextureFormat.RGBA32, false);
                cutoutTexture.filterMode = FilterMode.Point;
                cutoutTexture.wrapMode = TextureWrapMode.Clamp;
                cutoutTexture.SetPixels(new[] { Color.white, Color.clear, Color.clear, Color.white });
                cutoutTexture.Apply();
                mat.SetTexture("_MainTex", cutoutTexture);
                mat.SetFloat("_Cutoff", 0.5f);
                occluderMesh.uv = new[] { new Vector2(0, 0), new Vector2(0, 1), new Vector2(1, 1), new Vector2(1, 0) };
            }
            occluder.AddComponent<MeshRenderer>().sharedMaterial = mat;
            evidence["faceBoundsCenter"] = center.ToString("R");
            evidence["faceBoundsSize"] = bounds.size.ToString("R");
            evidence["faceCameraDistance"] = distance;
        }

        private void Update()
        {
            if (finished) return;
            if (camera == null) { Fail(new InvalidOperationException("Camera destroyed during capture.")); return; }
            if (Time.frameCount - startedFrame > 180) Fail(new TimeoutException("Camera did not produce diagnostic frame."));
        }
        private void Fail(Exception exception) { if (evidence != null) { evidence["status"] = "failed"; evidence["error"] = exception.ToString(); } Finish(); }
        private void WriteEvidence() { File.WriteAllText(Path.Combine(directory, "evidence.json"), evidence.ToString()); }
        private void Finish()
        {
            finished = true;
            try
            {
                Cleanup();
                if (evidence != null)
                {
                    evidence["temporaryResourcesReleased"] = true;
                    try { WriteEvidence(); }
                    catch (Exception exception) { Debug.LogWarning("Renderforge diagnostic evidence could not be saved: " + exception.Message); }
                }
            }
            finally { Destroy(gameObject); }
        }
        private void OnDestroy() { Cleanup(); }
        private void Cleanup()
        {
            RestorePrototype();
            RestoreCamera();
            Camera.onPreCull -= PrepareCamera;
            Camera.onPreRender -= BeforeCamera;
            Camera.onPostRender -= AfterCamera;
            if (commands != null) { if (camera != null) camera.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, commands); commands.Release(); commands = null; }
            if (outputCommands != null) { if (camera != null) camera.RemoveCommandBuffer(CameraEvent.AfterEverything, outputCommands); outputCommands.Release(); outputCommands = null; }
            foreach (var target in targets) { target.Release(); Destroy(target); } targets.Clear();
            foreach (var material in materials) Destroy(material); materials.Clear();
            if (occluder != null) Destroy(occluder);
            if (occluderMesh != null) Destroy(occluderMesh);
            if (cutoutTexture != null) Destroy(cutoutTexture);
        }
    }
}
