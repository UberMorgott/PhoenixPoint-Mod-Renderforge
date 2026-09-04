using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using PhoenixPoint.Common.Entities.Addons;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Explicit, read-only runtime evidence. This is not a face classifier or a mask.</summary>
    public static class CharacterInventory
    {
        // Call on Unity's main thread (for example through PPCLI). No persistent object cache:
        // each capture reads the current visual variant, including authored non-customizable heads.
        public static string Capture(string path)
        {
            var managers = new HashSet<AddonsManager>();
            var records = new JArray();
            foreach (var component in UnityEngine.Object.FindObjectsOfType<MonoBehaviour>())
            {
                var provider = component as IAddonsManagerProvider;
                var manager = provider?.AddonsManager;
                if (manager == null || !managers.Add(manager) || manager.RootAddon == null) continue;
                var addons = new JArray();
                foreach (var addon in manager.RootAddon)
                {
                    var root = addon.VisualRoot;
                    var renderers = new JArray();
                    if (root != null)
                        foreach (var renderer in root.GetComponentsInChildren<Renderer>(true))
                            renderers.Add(RendererRecord(renderer, root));
                    addons.Add(new JObject {
                        ["defName"] = addon.AddonDef?.name,
                        ["defGuid"] = addon.AddonDef?.Guid.ToString(),
                        // The game retains VisualsSourcePrefab after destroying VisualRoot.
                        // Keep that history separate so it cannot masquerade as a live variant.
                        ["lastSourcePrefab"] = addon.VisualsSourcePrefab != null ? addon.VisualsSourcePrefab.name : null,
                        ["liveSourcePrefab"] = root != null && addon.VisualsSourcePrefab != null ? addon.VisualsSourcePrefab.name : null,
                        ["visualRoot"] = root != null ? HierarchyPath(root, null) : null,
                        ["visible"] = addon.IsVisible,
                        ["renderers"] = renderers
                    });
                }
                records.Add(new JObject {
                    ["provider"] = component.GetType().FullName,
                    ["providerPath"] = HierarchyPath(component.transform, null),
                    ["managerDef"] = manager.AddonsManagerDef?.name,
                    ["managerGuid"] = manager.AddonsManagerDef?.Guid.ToString(),
                    ["addons"] = addons
                });
            }
            var shaders = new JArray();
            foreach (var shader in Resources.FindObjectsOfTypeAll<Shader>().OrderBy(s => s.name, StringComparer.Ordinal))
            {
                var properties = new JArray();
                for (int i = 0; i < shader.GetPropertyCount(); i++)
                    properties.Add(new JObject { ["name"] = shader.GetPropertyName(i), ["type"] = shader.GetPropertyType(i).ToString() });
                shaders.Add(new JObject { ["name"] = shader.name, ["supported"] = shader.isSupported,
                    ["passes"] = shader.passCount, ["properties"] = properties });
            }
            var document = new JObject {
                ["schema"] = 1, ["capturedUtc"] = DateTime.UtcNow.ToString("o"),
                ["unity"] = Application.unityVersion, ["graphicsApi"] = SystemInfo.graphicsDeviceType.ToString(),
                ["reversedZ"] = SystemInfo.usesReversedZBuffer,
                ["classification"] = "unclassified; no masks or material changes",
                ["characters"] = records, ["loadedShaders"] = shaders,
                ["cameras"] = new JArray(Camera.allCameras.Select(camera => {
                    var target = camera.targetTexture;
                    return new JObject {
                        ["name"] = camera.name, ["instanceId"] = camera.GetInstanceID(),
                        ["pixelWidth"] = camera.pixelWidth, ["pixelHeight"] = camera.pixelHeight,
                        ["depthMode"] = camera.depthTextureMode.ToString(), ["renderingPath"] = camera.actualRenderingPath.ToString(),
                        ["cullingMask"] = camera.cullingMask, ["fieldOfView"] = camera.fieldOfView,
                        ["near"] = camera.nearClipPlane, ["far"] = camera.farClipPlane,
                        ["allowMsaa"] = camera.allowMSAA,
                        ["target"] = target != null ? target.name : null,
                        ["targetWidth"] = target != null ? (int?)target.width : null,
                        ["targetHeight"] = target != null ? (int?)target.height : null,
                        ["targetDepthBits"] = target != null ? (int?)target.depth : null,
                        ["targetMsaa"] = target != null ? (int?)target.antiAliasing : null
                    };
                }))
            };
            File.WriteAllText(path, document.ToString(Formatting.Indented));
            return "inventory=" + Path.GetFullPath(path) + " managers=" + records.Count + " shaders=" + shaders.Count;
        }

        private static JObject RendererRecord(Renderer renderer, Transform root)
        {
            var skinned = renderer as SkinnedMeshRenderer;
            var filter = renderer.GetComponent<MeshFilter>();
            var mesh = skinned != null ? skinned.sharedMesh : filter != null ? filter.sharedMesh : null;
            // sharedMaterials getter returns an array copy without cloning/assigning materials.
            var originals = renderer.sharedMaterials;
            var materials = new JArray();
            for (int slot = 0; slot < originals.Length; slot++)
            {
                var material = originals[slot];
                if (material == null) { materials.Add(JValue.CreateNull()); continue; }
                var textures = new JArray();
                foreach (var property in material.GetTexturePropertyNames())
                {
                    var texture = material.GetTexture(property);
                    textures.Add(new JObject { ["property"] = property, ["name"] = texture != null ? texture.name : null,
                        ["instanceId"] = texture != null ? (int?)texture.GetInstanceID() : null,
                        ["width"] = texture != null ? (int?)texture.width : null, ["height"] = texture != null ? (int?)texture.height : null });
                }
                materials.Add(new JObject { ["slot"] = slot, ["name"] = material.name, ["instanceId"] = material.GetInstanceID(),
                    ["shader"] = material.shader != null ? material.shader.name : null, ["renderQueue"] = material.renderQueue,
                    ["keywords"] = new JArray(material.shaderKeywords),
                    ["passes"] = new JArray(Enumerable.Range(0, material.passCount).Select(material.GetPassName)), ["textures"] = textures });
            }
            bool unchanged = originals.SequenceEqual(renderer.sharedMaterials);
            if (!unchanged) throw new InvalidOperationException("sharedMaterials changed during capture: " + renderer.name);
            return new JObject { ["path"] = HierarchyPath(renderer.transform, root), ["type"] = renderer.GetType().Name,
                ["instanceId"] = renderer.GetInstanceID(), ["enabled"] = renderer.enabled,
                ["active"] = renderer.gameObject.activeInHierarchy, ["layer"] = renderer.gameObject.layer,
                ["mesh"] = mesh != null ? mesh.name : null, ["submeshes"] = mesh != null ? (int?)mesh.subMeshCount : null,
                ["sharedMaterialsUnchanged"] = unchanged, ["materials"] = materials };
        }

        private static string HierarchyPath(Transform node, Transform root)
        {
            var parts = new List<string>();
            while (node != null && node != root) { parts.Add(node.name + "[" + node.GetSiblingIndex() + "]"); node = node.parent; }
            parts.Reverse();
            return parts.Count == 0 ? "." : string.Join("/", parts);
        }
    }
}
