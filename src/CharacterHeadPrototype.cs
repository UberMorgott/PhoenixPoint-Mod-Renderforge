using System;
using System.IO;
using System.Linq;
using Newtonsoft.Json.Linq;
using UnityEngine;
using UnityEngine.Rendering;

namespace Renderforge
{
    public sealed partial class CharacterMaskDiagnostic
    {
        private float prototypeYaw;
        private int prototypeWarmupFrames;
        private Material[] authoredFaceMaterials;
        private Material prototypeMaterial;
        private Texture2D prototypeTexture;

        // Explicit development utility. No renderer cache or automatic production hook.
        public static string ExportHead(int cameraId, string prefab, string outputDirectory)
        {
            Capture(cameraId, prefab, outputDirectory, 0, false);
            var probe = FindObjectsOfType<CharacterMaskDiagnostic>().Single();
            try
            {
                var mesh = new Mesh { name = probe.face.sharedMesh.name + " posed export" };
                probe.face.BakeMesh(mesh);
                probe.occluderMesh = mesh;
                if (mesh.vertexCount == 0 || mesh.uv.Length != mesh.vertexCount)
                    throw new InvalidOperationException("Readable posed head mesh and UVs are required.");
                var material = probe.face.sharedMaterials.Single();
                var textures = new JArray();
                foreach (var property in material.GetTexturePropertyNames())
                {
                    var texture = material.GetTexture(property);
                    if (texture == null) continue;
                    bool srgb = texture.graphicsFormat.ToString().EndsWith("_SRGB", StringComparison.Ordinal);
                    SaveTexture(texture, Path.Combine(outputDirectory, property + ".png"), srgb);
                    var scale = material.GetTextureScale(property);
                    var offset = material.GetTextureOffset(property);
                    textures.Add(new JObject { ["property"] = property, ["name"] = texture.name,
                        ["id"] = texture.GetInstanceID(), ["format"] = texture.graphicsFormat.ToString(),
                        ["srgb"] = srgb, ["width"] = texture.width, ["height"] = texture.height,
                        ["scale"] = new JArray(scale.x, scale.y), ["offset"] = new JArray(offset.x, offset.y),
                        ["wrap"] = texture.wrapMode.ToString(), ["filter"] = texture.filterMode.ToString() });
                }
                var properties = new JObject();
                var shader = material.shader;
                for (int i = 0; i < shader.GetPropertyCount(); i++)
                {
                    var name = shader.GetPropertyName(i);
                    var type = shader.GetPropertyType(i);
                    if (type == ShaderPropertyType.Float || type == ShaderPropertyType.Range) properties[name] = material.GetFloat(name);
                    else if (type == ShaderPropertyType.Color || type == ShaderPropertyType.Vector)
                    { var v = material.GetVector(name); properties[name] = new JArray(v.x, v.y, v.z, v.w); }
                }
                var document = new JObject { ["prefab"] = prefab, ["faceTag"] = probe.faceTag,
                    ["mesh"] = mesh.name, ["rendererId"] = probe.face.GetInstanceID(),
                    ["material"] = material.name, ["materialId"] = material.GetInstanceID(),
                    ["shader"] = shader.name, ["shaderId"] = shader.GetInstanceID(),
                    ["colorSpace"] = QualitySettings.activeColorSpace.ToString(), ["textures"] = textures,
                    ["properties"] = properties, ["keywords"] = new JArray(material.shaderKeywords),
                    ["vertices"] = new JArray(mesh.vertices.Select(v => new JArray(v.x, v.y, v.z))),
                    ["normals"] = new JArray(mesh.normals.Select(v => new JArray(v.x, v.y, v.z))),
                    ["tangents"] = new JArray(mesh.tangents.Select(v => new JArray(v.x, v.y, v.z, v.w))),
                    ["uv"] = new JArray(mesh.uv.Select(v => new JArray(v.x, v.y))),
                    ["indices"] = new JArray(mesh.GetTriangles(0)),
                    ["note"] = "Raw source game assets for local diagnostic use only. Geometry is not modified. Normal/gloss channels are exported in their original packed form." };
                File.WriteAllText(Path.Combine(outputDirectory, "head.json"), document.ToString());
                probe.evidence["status"] = "exported";
                return "exported=" + outputDirectory + " vertices=" + mesh.vertexCount;
            }
            catch (Exception exception)
            {
                probe.evidence["status"] = "failed";
                probe.evidence["error"] = exception.ToString();
                throw;
            }
            finally { probe.Finish(); }
        }

        private static void SaveTexture(Texture source, string path, bool srgb)
        {
            var previous = RenderTexture.active;
            bool previousSrgb = GL.sRGBWrite;
            var target = RenderTexture.GetTemporary(source.width, source.height, 0, RenderTextureFormat.ARGB32,
                srgb ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear);
            var texture = new Texture2D(source.width, source.height, TextureFormat.RGBA32, false, !srgb);
            try
            {
                GL.sRGBWrite = srgb;
                Graphics.Blit(source, target);
                RenderTexture.active = target;
                texture.ReadPixels(new Rect(0, 0, source.width, source.height), 0, 0);
                texture.Apply();
                File.WriteAllBytes(path, texture.EncodeToPNG());
            }
            finally
            {
                GL.sRGBWrite = previousSrgb;
                RenderTexture.active = previous;
                Destroy(texture);
                RenderTexture.ReleaseTemporary(target);
            }
        }

        // The candidate is visible only for this bounded capture; empty path is the matched original control.
        public static string CaptureHead(int cameraId, string prefab, string outputDirectory, string candidatePath, float yaw)
        {
            if (!float.IsNaN(yaw) && !float.IsInfinity(yaw) && Math.Abs(yaw) <= 90f)
            {
                Capture(cameraId, prefab, outputDirectory, 0, true);
                var probe = FindObjectsOfType<CharacterMaskDiagnostic>().Single();
                try
                {
                    probe.prototypeYaw = yaw;
                    probe.evidence["yaw"] = yaw;
                    probe.evidence["candidate"] = candidatePath;
                    if (!string.IsNullOrEmpty(candidatePath)) probe.ApplyPrototype(candidatePath);
                    probe.WriteEvidence();
                    return "scheduled=" + outputDirectory;
                }
                catch (Exception exception) { probe.Fail(exception); throw; }
            }
            throw new ArgumentOutOfRangeException(nameof(yaw));
        }

        public static string CaptureHeadMotion(int cameraId, string prefab, string outputDirectory, string candidatePath)
        {
            var result = CaptureHead(cameraId, prefab, outputDirectory, candidatePath, 0f);
            FindObjectsOfType<CharacterMaskDiagnostic>().Single().prototypeWarmupFrames = 120;
            return result;
        }

        public static string CaptureHeadCalibrated(int cameraId, string prefab, string outputDirectory, string candidatePath, float yaw)
        {
            var result = CaptureHead(cameraId, prefab, outputDirectory, candidatePath, yaw);
            var probe = FindObjectsOfType<CharacterMaskDiagnostic>().Single();
            try
            {
                if (probe.prototypeMaterial == null || !probe.prototypeMaterial.HasProperty("_GlossMapScale"))
                    throw new InvalidOperationException("Verified gloss scale is unavailable.");
                probe.evidence["originalGlossScale"] = probe.prototypeMaterial.GetFloat("_GlossMapScale");
                probe.prototypeMaterial.SetFloat("_GlossMapScale", 0.35f);
                probe.evidence["candidateGlossScale"] = 0.35f;
                return result;
            }
            catch (Exception exception) { probe.Fail(exception); throw; }
        }

        private void ApplyPrototype(string path)
        {
            if (!Path.IsPathRooted(path)) throw new ArgumentException("Absolute candidate path required.");
            var original = face.sharedMaterials.Single();
            var source = original.GetTexture("_MainTex");
            if (source == null || !source.graphicsFormat.ToString().EndsWith("_SRGB", StringComparison.Ordinal))
                throw new InvalidOperationException("Only verified sRGB albedo replacement is supported.");
            prototypeTexture = new Texture2D(2, 2, TextureFormat.RGBA32, true, false);
            if (!prototypeTexture.LoadImage(File.ReadAllBytes(path))) throw new InvalidOperationException("Candidate PNG decode failed.");
            if (prototypeTexture.width < Math.Min(1024, source.width) || prototypeTexture.height < Math.Min(1024, source.height)
                || (long)prototypeTexture.width * source.height != (long)prototypeTexture.height * source.width)
                throw new InvalidOperationException("Candidate must preserve atlas aspect ratio and at least 1024 pixels per side.");
            evidence["candidateWidth"] = prototypeTexture.width;
            evidence["candidateHeight"] = prototypeTexture.height;
            prototypeTexture.name = "Renderforge temporary generated albedo";
            prototypeTexture.wrapMode = source.wrapMode;
            prototypeTexture.filterMode = source.filterMode;
            prototypeTexture.anisoLevel = source.anisoLevel;
            prototypeMaterial = new Material(original);
            prototypeMaterial.name = "Renderforge temporary generated head";
            prototypeMaterial.SetTexture("_MainTex", prototypeTexture);
            authoredFaceMaterials = face.sharedMaterials;
            face.sharedMaterials = new[] { prototypeMaterial };
            originals[face] = face.sharedMaterials;
            evidence["authoredMaterialId"] = original.GetInstanceID();
            evidence["temporaryMaterialId"] = prototypeMaterial.GetInstanceID();
            evidence["geometryUnchanged"] = true;
        }

        public static string CancelHead()
        {
            foreach (var probe in FindObjectsOfType<CharacterMaskDiagnostic>())
            {
                probe.evidence["status"] = "cancelled";
                probe.Finish();
            }
            return "cancelled";
        }

        private void RestorePrototype()
        {
            if (authoredFaceMaterials != null && face != null)
            {
                // Never overwrite a material update made by the game while the diagnostic was active.
                if (face.sharedMaterials.Length == 1 && face.sharedMaterials[0] == prototypeMaterial)
                    face.sharedMaterials = authoredFaceMaterials;
                if (evidence != null) evidence["authoredMaterialsRestored"] = face.sharedMaterials.SequenceEqual(authoredFaceMaterials);
            }
            authoredFaceMaterials = null;
            if (prototypeMaterial != null) Destroy(prototypeMaterial);
            if (prototypeTexture != null) Destroy(prototypeTexture);
            prototypeMaterial = null;
            prototypeTexture = null;
        }
    }
}
