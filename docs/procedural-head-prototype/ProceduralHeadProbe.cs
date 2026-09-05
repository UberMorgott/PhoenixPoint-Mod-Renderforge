using System;
using System.IO;
using System.Linq;
using Newtonsoft.Json.Linq;
using UnityEngine;

namespace Renderforge
{
    public sealed partial class CharacterMaskDiagnostic
    {
        public static object CurrentCycle() => FindObjectsOfType<PhoenixPoint.Common.View.ViewModules.UIModuleActorCycle>().Single(c => c.isActiveAndEnabled && c.CurrentCharacter != null);
        public static string RosterFaces() => new JArray(((PhoenixPoint.Common.View.ViewModules.UIModuleActorCycle)CurrentCycle()).Characters.Select((c, i) => new JObject { ["index"] = i, ["faceTag"] = c.Identity.FaceTag.Guid.ToString() })).ToString();
        public static string InventoryMorphs(int cameraId, string prefab, string outputDirectory)
        {
            Capture(cameraId, prefab, outputDirectory, 0, false);
            var probe = FindObjectsOfType<CharacterMaskDiagnostic>().Single();
            try
            {
                var renderer = probe.face;
                var mesh = renderer.sharedMesh;
                var shapes = new JArray();
                for (int i = 0; i < mesh.blendShapeCount; ++i)
                    shapes.Add(new JObject { ["index"] = i, ["name"] = mesh.GetBlendShapeName(i),
                        ["weight"] = renderer.GetBlendShapeWeight(i), ["frames"] = mesh.GetBlendShapeFrameCount(i) });
                var doc = new JObject { ["prefab"] = prefab, ["faceTag"] = probe.faceTag,
                    ["mesh"] = mesh.name, ["meshId"] = mesh.GetInstanceID(), ["rendererId"] = renderer.GetInstanceID(),
                    ["vertices"] = mesh.vertexCount, ["isReadable"] = mesh.isReadable,
                    ["blendShapeCount"] = mesh.blendShapeCount, ["shapes"] = shapes,
                    ["rootBone"] = renderer.rootBone != null ? renderer.rootBone.name : null,
                    ["bones"] = new JArray(renderer.bones.Select(b => b == null ? null : new JObject {
                        ["name"] = b.name, ["id"] = b.GetInstanceID(), ["parent"] = b.parent != null ? b.parent.name : null,
                        ["path"] = BonePath(b), ["localPosition"] = new JArray(b.localPosition.x, b.localPosition.y, b.localPosition.z),
                        ["localRotation"] = new JArray(b.localRotation.x, b.localRotation.y, b.localRotation.z, b.localRotation.w),
                        ["localScale"] = new JArray(b.localScale.x, b.localScale.y, b.localScale.z) })),
                    ["frame"] = Time.frameCount, ["timeScale"] = Time.timeScale,
                    ["materialId"] = renderer.sharedMaterials.Single().GetInstanceID(),
                    ["shader"] = renderer.sharedMaterials.Single().shader.name,
                    ["glossScale"] = renderer.sharedMaterials.Single().GetFloat("_GlossMapScale"),
                    ["normalScale"] = renderer.sharedMaterials.Single().GetFloat("_NormalScale") };
                try { doc["bindposeCount"] = mesh.bindposes.Length;
                    doc["bindposesColumnMajor"] = new JArray(mesh.bindposes.Select(m => new JArray(Enumerable.Range(0, 16).Select(i => m[i]))));
                } catch (Exception e) { doc["bindposeError"] = e.Message; }
                try { doc["boneWeightCount"] = mesh.boneWeights.Length; } catch (Exception e) { doc["boneWeightError"] = e.Message; }
                File.WriteAllText(Path.Combine(outputDirectory, "morphs.json"), doc.ToString());
                probe.evidence["status"] = "inventoried";
                return "shapes=" + mesh.blendShapeCount + " bones=" + renderer.bones.Length;
            }
            finally { probe.Finish(); }
        }

        private static string BonePath(Transform bone)
        {
            var result = bone.name;
            for (var parent = bone.parent; parent != null; parent = parent.parent) result = parent.name + "/" + result;
            return result;
        }

        public static string CaptureNormalMotion(int cameraId, string prefab, string outputDirectory)
        {
            var result = CaptureNormalControl(cameraId, prefab, outputDirectory, 0f);
            FindObjectsOfType<CharacterMaskDiagnostic>().Single().prototypeWarmupFrames = 120;
            return result;
        }

        public static string CaptureNormalControl(int cameraId, string prefab, string outputDirectory, float yaw)
            => CaptureNormal(cameraId, prefab, outputDirectory, yaw, false);

        public static string CaptureMicroNormal(int cameraId, string prefab, string outputDirectory, float yaw)
            => CaptureNormal(cameraId, prefab, outputDirectory, yaw, true);

        public static string CaptureMicroMotion(int cameraId, string prefab, string outputDirectory)
        {
            var result = CaptureMicroNormal(cameraId, prefab, outputDirectory, 0f);
            FindObjectsOfType<CharacterMaskDiagnostic>().Single().prototypeWarmupFrames = 120;
            return result;
        }

        private static string CaptureNormal(int cameraId, string prefab, string outputDirectory, float yaw, bool micro)
        {
            var result = CaptureHead(cameraId, prefab, outputDirectory, "", yaw);
            var probe = FindObjectsOfType<CharacterMaskDiagnostic>().Single();
            try
            {
                var original = probe.face.sharedMaterials.Single();
                var source = original.GetTexture("_BumpMap") as Texture2D;
                if (source == null || source.graphicsFormat.ToString() != "RG_BC5_UNorm")
                    throw new InvalidOperationException("Only verified BC5 RG normal input supported: " + source?.graphicsFormat);
                probe.evidence["normalFormat"] = source.graphicsFormat.ToString();
                probe.evidence["normalMipCount"] = source.mipmapCount;
                probe.evidence["originalGlossScale"] = original.GetFloat("_GlossMapScale");
                var previous = RenderTexture.active;
                bool srgb = GL.sRGBWrite;
                var target = RenderTexture.GetTemporary(source.width, source.height, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
                try
                {
                    probe.prototypeTexture = new Texture2D(source.width, source.height, TextureFormat.RGBA32, source.mipmapCount > 1, true);
                    GL.sRGBWrite = false;
                    Graphics.Blit(source, target);
                    RenderTexture.active = target;
                    probe.prototypeTexture.ReadPixels(new Rect(0, 0, source.width, source.height), 0, 0);
                    probe.prototypeTexture.Apply(true, false);
                    var pixels = probe.prototypeTexture.GetPixels32();
                    probe.evidence["sampleChannelsMin"] = new JArray(pixels.Min(p => p.r), pixels.Min(p => p.g), pixels.Min(p => p.b), pixels.Min(p => p.a));
                    probe.evidence["sampleChannelsMax"] = new JArray(pixels.Max(p => p.r), pixels.Max(p => p.g), pixels.Max(p => p.b), pixels.Max(p => p.a));
                    if (micro)
                    {
                        var watch = System.Diagnostics.Stopwatch.StartNew();
                        int changed = AddMicrostructure(pixels, source.width, source.height, prefab);
                        probe.prototypeTexture.SetPixels32(pixels);
                        probe.prototypeTexture.Apply(true, false);
                        watch.Stop();
                        probe.evidence["microGenerationMs"] = watch.Elapsed.TotalMilliseconds;
                        probe.evidence["microChangedPixels"] = changed;
                        probe.evidence["microAmplitude"] = 0.025;
                        probe.evidence["microSeed"] = 17;
                        probe.evidence["microProfile"] = prefab;
                    }
                    probe.prototypeTexture.Apply(false, true);
                }
                finally { RenderTexture.active = previous; GL.sRGBWrite = srgb; RenderTexture.ReleaseTemporary(target); }
                probe.prototypeTexture.name = micro ? "Renderforge numeric micro-normal candidate" : "Renderforge unchanged normal control";
                probe.prototypeTexture.wrapMode = source.wrapMode;
                probe.prototypeTexture.filterMode = source.filterMode;
                probe.prototypeTexture.anisoLevel = source.anisoLevel;
                probe.prototypeMaterial = new Material(original);
                probe.prototypeMaterial.SetTexture("_BumpMap", probe.prototypeTexture);
                probe.authoredFaceMaterials = probe.face.sharedMaterials;
                probe.face.sharedMaterials = new[] { probe.prototypeMaterial };
                probe.originals[probe.face] = probe.face.sharedMaterials;
                probe.evidence["originalMeshId"] = probe.face.sharedMesh.GetInstanceID();
                probe.evidence["originalBoneIds"] = new JArray(probe.face.bones.Select(b => b == null ? 0 : b.GetInstanceID()));
                probe.WriteEvidence();
                return result;
            }
            catch (Exception exception) { probe.Fail(exception); throw; }
        }

        // Whitelisted forehead/cheek islands observed on the original UV albedos and wireframes.
        // Positive islands leave eyelids, brows, nose, lips, ears, neck and atlas seams untouched.
        private static int AddMicrostructure(Color32[] pixels, int width, int height, string prefab)
        {
            bool sofia = prefab == "Head - Tutorial - Female_Ready";
            if (!sofia && prefab != "Head_Hispanic1_F_V01_Ready") throw new InvalidOperationException("Unknown UV layout.");
            float cheekX = sofia ? 0.31f : 0.34f;
            float cheekY = sofia ? 0.48f : 0.49f;
            float cheekRadius = sofia ? 0.055f : 0.065f;
            int count = 0;
            for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                float u = (x + 0.5f) / width, v = (y + 0.5f) / height;
                float mask = Math.Max(Island(u, v, cheekX, cheekY, cheekRadius, 0.065f),
                    Math.Max(Island(u, v, 1f - cheekX, cheekY, cheekRadius, 0.065f), Island(u, v, .5f, .76f, .08f, .035f)));
                if (mask <= 0f) continue;
                var p = pixels[y * width + x];
                float phaseA = 2f * Mathf.PI * (91f * u + 57f * v) + 1.7f;
                float phaseB = 2f * Mathf.PI * (-63f * u + 113f * v) + 4.1f;
                float nx = p.r / 127.5f - 1f + .025f * mask * (Mathf.Cos(phaseA) * .7f - Mathf.Cos(phaseB) * .3f);
                float ny = p.g / 127.5f - 1f + .025f * mask * (Mathf.Cos(phaseA) * .43846f + Mathf.Cos(phaseB) * .5381f);
                float magnitude = nx * nx + ny * ny;
                if (magnitude > .999f) { float scale = Mathf.Sqrt(.999f / magnitude); nx *= scale; ny *= scale; }
                p.r = (byte)Mathf.Clamp(Mathf.RoundToInt((nx + 1f) * 127.5f), 0, 255);
                p.g = (byte)Mathf.Clamp(Mathf.RoundToInt((ny + 1f) * 127.5f), 0, 255);
                if (!p.Equals(pixels[y * width + x])) { pixels[y * width + x] = p; ++count; }
            }
            return count;
        }

        private static float Island(float u, float v, float x, float y, float rx, float ry)
        {
            float a = (u - x) / rx, b = (v - y) / ry;
            float d = Mathf.Clamp01(1f - a * a - b * b);
            return d * d * (3f - 2f * d);
        }
    }
}
