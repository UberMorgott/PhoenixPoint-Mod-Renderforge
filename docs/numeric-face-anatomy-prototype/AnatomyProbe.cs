using System;
using System.IO;
using System.Linq;
using Newtonsoft.Json.Linq;
using UnityEngine;

namespace Renderforge
{
    public sealed partial class CharacterMaskDiagnostic
    {
        public static string InspectAnatomy(int cameraId, string prefab, string outputDirectory)
        {
            Capture(cameraId, prefab, outputDirectory, 0, false);
            var probe = FindObjectsOfType<CharacterMaskDiagnostic>().Single();
            try
            {
                var mesh = new Mesh();
                probe.occluderMesh = mesh;
                probe.face.BakeMesh(mesh);
                var vertices = mesh.vertices;
                var weights = probe.face.sharedMesh.boneWeights;
                var bones = probe.face.bones;
                var head = bones.Single(b => b.name == "#Head_Addon => Human_Head_BodyPartDef");
                var rows = new JArray();
                for (int b = 0; b < bones.Length; ++b)
                {
                    float sum = 0; int count = 0;
                    Vector3 centroid = Vector3.zero, localCentroid = Vector3.zero, predicted = Vector3.zero;
                    for (int v = 0; v < vertices.Length; ++v)
                    {
                        var w = weights[v];
                        float weight = (w.boneIndex0 == b ? w.weight0 : 0) + (w.boneIndex1 == b ? w.weight1 : 0)
                            + (w.boneIndex2 == b ? w.weight2 : 0) + (w.boneIndex3 == b ? w.weight3 : 0);
                        if (weight <= .05f) continue;
                        ++count; sum += weight;
                        var world = probe.face.transform.TransformPoint(vertices[v]);
                        var local = bones[b].InverseTransformPoint(world);
                        centroid += head.InverseTransformPoint(world) * weight;
                        localCentroid += local * weight;
                        predicted += head.InverseTransformVector(bones[b].TransformPoint(Quaternion.AngleAxis(-3f, Vector3.right) * local) - world) * weight;
                    }
                    rows.Add(new JObject { ["name"] = bones[b].name, ["index"] = b, ["verticesOver005"] = count,
                        ["weightSum"] = sum, ["weightedCentroidHeadLocal"] = V(sum > 0 ? centroid / sum : Vector3.zero),
                        ["weightedCentroidBoneLocal"] = V(sum > 0 ? localCentroid / sum : Vector3.zero),
                        ["predictedMinus3DegreesDeltaHeadLocal"] = V(sum > 0 ? predicted / sum : Vector3.zero),
                        ["boneXAxisHeadLocal"] = V(head.InverseTransformVector(bones[b].right)),
                        ["position"] = V(bones[b].localPosition), ["rotation"] = Q(bones[b].localRotation) });
                }
                var renderers = new JArray(probe.selected.Select(r => new JObject { ["name"] = r.name,
                    ["type"] = r.GetType().Name, ["parent"] = r.transform.parent != null ? r.transform.parent.name : null,
                    ["materials"] = new JArray(r.sharedMaterials.Select(m => m.name)),
                    ["bones"] = new JArray((r as SkinnedMeshRenderer)?.bones.Select(b => b != null ? b.name : null) ?? Enumerable.Empty<string>()) }));
                File.WriteAllText(Path.Combine(outputDirectory, "anatomy.json"), new JObject {
                    ["prefab"] = prefab, ["faceTag"] = probe.faceTag, ["frame"] = Time.frameCount,
                    ["headMeshBounds"] = V(mesh.bounds.size), ["bones"] = rows, ["renderers"] = renderers }.ToString());
                probe.evidence["status"] = "inventoried";
                return "bones=" + rows.Count + " renderers=" + renderers.Count;
            }
            finally { probe.Finish(); }
        }
        private static JArray V(Vector3 v) => new JArray(v.x, v.y, v.z);
        private static JArray Q(Quaternion q) => new JArray(q.x, q.y, q.z, q.w);

        public static string CaptureAnatomy(int cameraId, string prefab, string outputDirectory, float yaw, bool corrected)
        {
            if (Time.timeScale != 0f) throw new InvalidOperationException("Freeze simulation before this one-shot anatomy probe.");
            var result = CaptureHead(cameraId, prefab, outputDirectory, "", yaw);
            var probe = FindObjectsOfType<CharacterMaskDiagnostic>().Single();
            AnatomyRestore guard = null;
            try
            {
                guard = probe.gameObject.AddComponent<AnatomyRestore>();
                guard.Initialize(probe.face, probe.selected, cameraId, outputDirectory, corrected);
                probe.evidence["anatomyCorrected"] = corrected;
                probe.evidence["anatomyUpperLidFractionOfInterEyeDistance"] = corrected ? .025f : 0f;
                probe.WriteEvidence();
                return result;
            }
            catch (Exception exception) { if (guard != null) guard.Restore(); probe.Fail(exception); throw; }
        }
    }

    public sealed class AnatomyRestore : MonoBehaviour
    {
        private Transform[] bones;
        private Vector3[] positions, scales;
        private Quaternion[] rotations;
        private Transform[] changed;
        private Vector3[] expected;
        private Renderer[] renderers;
        private Material[][] materials;
        private SkinnedMeshRenderer face;
        private Mesh authoredMesh;
        private string directory;
        private int cameraId;
        private JObject evidence;
        private bool restored;
        private int observations;
        private float maxObservedDrift;

        public void Initialize(SkinnedMeshRenderer skin, Renderer[] visible, int camera, string output, bool corrected)
        {
            directory = output; cameraId = camera; face = skin; authoredMesh = skin.sharedMesh;
            bones = skin.bones;
            positions = bones.Select(b => b.localPosition).ToArray();
            rotations = bones.Select(b => b.localRotation).ToArray();
            scales = bones.Select(b => b.localScale).ToArray();
            renderers = visible;
            materials = visible.Select(r => r.sharedMaterials).ToArray();
            var right = bones.Single(b => b.name == "Eye_UpEyelid_R");
            var left = bones.Single(b => b.name == "Eye_UpEyelid_L");
            var head = bones.Single(b => b.name == "#Head_Addon => Human_Head_BodyPartDef");
            if (right.parent != head || left.parent != head) throw new InvalidOperationException("Unverified eyelid parent space.");
            float spacing = Vector3.Distance(right.localPosition, left.localPosition);
            if (spacing < .03f || spacing > .08f) throw new InvalidOperationException("Unexpected inter-eye bone spacing.");
            changed = corrected ? new[] { right, left } : new Transform[0];
            expected = changed.Select(b => b.localPosition + Vector3.up * spacing * .025f).ToArray();
            var beforeMesh = new Mesh();
            var afterMesh = new Mesh();
            try
            {
                skin.BakeMesh(beforeMesh);
                var before = beforeMesh.vertices;
                for (int i = 0; i < changed.Length; ++i) changed[i].localPosition = expected[i];
                skin.BakeMesh(afterMesh);
                var after = afterMesh.vertices;
                if (before.Length != after.Length) throw new InvalidOperationException("Head topology changed.");
                float maximum = 0; int count = 0;
                for (int i = 0; i < before.Length; ++i)
                {
                    float distance = Vector3.Distance(before[i], after[i]);
                    maximum = Math.Max(maximum, distance);
                    if (distance > .000001f) ++count;
                }
                evidence = new JObject { ["corrected"] = corrected, ["frame"] = Time.frameCount,
                    ["interEyeDistanceLocal"] = spacing, ["localYOffset"] = corrected ? spacing * .025f : 0f,
                    ["changedBones"] = new JArray(changed.Select(b => b.name)),
                    ["changedSkinVertices"] = count, ["maxBakedVertexDelta"] = maximum,
                    ["originalMeshId"] = authoredMesh.GetInstanceID(),
                    ["originalBones"] = new JArray(bones.Select((b, i) => new JObject { ["name"] = b.name,
                        ["position"] = V(positions[i]), ["rotation"] = Q(rotations[i]), ["scale"] = V(scales[i]) })) };
                File.WriteAllText(Path.Combine(directory, "anatomy.json"), evidence.ToString());
            }
            finally { Destroy(beforeMesh); Destroy(afterMesh); }
            Camera.onPreRender += Observe;
        }

        private void Observe(Camera camera)
        {
            if (camera.GetInstanceID() != cameraId || restored) return;
            ++observations;
            for (int i = 0; i < changed.Length; ++i)
                if (changed[i] != null) maxObservedDrift = Math.Max(maxObservedDrift, Vector3.Distance(changed[i].localPosition, expected[i]));
        }

        public void Restore()
        {
            if (restored) return;
            restored = true;
            Camera.onPreRender -= Observe;
            int destroyed = 0;
            if (bones != null && positions != null)
                for (int i = 0; i < bones.Length; ++i)
                {
                    if (bones[i] == null) { ++destroyed; continue; }
                    if (changed != null && changed.Contains(bones[i]))
                    { bones[i].localPosition = positions[i]; bones[i].localRotation = rotations[i]; bones[i].localScale = scales[i]; }
                }
            float delta = 0;
            if (bones != null)
                for (int i = 0; i < bones.Length; ++i)
                {
                    if (bones[i] == null) continue;
                    delta = Math.Max(delta, Vector3.Distance(bones[i].localPosition, positions[i]));
                    delta = Math.Max(delta, Vector3.Distance(bones[i].localScale, scales[i]));
                    var q = bones[i].localRotation; var r = rotations[i];
                    delta = Math.Max(delta, Math.Max(Math.Max(Math.Abs(q.x-r.x), Math.Abs(q.y-r.y)), Math.Max(Math.Abs(q.z-r.z), Math.Abs(q.w-r.w))));
                }
            if (directory != null)
            {
                var result = new JObject { ["maxOriginalTransformDelta"] = delta, ["destroyedBones"] = destroyed,
                    ["renderObservations"] = observations, ["maxAppliedPositionDrift"] = maxObservedDrift,
                    ["meshRestored"] = face != null && face.sharedMesh == authoredMesh,
                    ["materialsUntouched"] = renderers != null && renderers.Select((r,i) => r != null && r.sharedMaterials.SequenceEqual(materials[i])).All(v => v) };
                try { File.WriteAllText(Path.Combine(directory, "restoration.json"), result.ToString()); }
                catch (Exception e) { Debug.LogWarning("Anatomy restoration evidence write failed: " + e.Message); }
            }
        }
        private void OnDestroy() { Restore(); }
        private static JArray V(Vector3 v) => new JArray(v.x, v.y, v.z);
        private static JArray Q(Quaternion q) => new JArray(q.x, q.y, q.z, q.w);
    }
}
