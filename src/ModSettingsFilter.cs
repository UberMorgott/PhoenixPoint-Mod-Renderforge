using System;
using HarmonyLib;
using PhoenixPoint.Home.View.ViewModules;

namespace Renderforge
{
    /// <summary>The game uses ModConfig.GetConfigFields both to build Mods -> settings and to serialize
    /// ModConfig.json. Filtering in DlssConfig unconditionally therefore deletes hidden values on every save.
    /// Mark only the private UI construction call; serialization runs outside this scope and sees every field.</summary>
    [HarmonyPatch(typeof(UIModuleModManager), "SelectModSettingsSection")]
    internal static class ModSettingsFilter
    {
        [ThreadStatic]
        private static int depth;

        internal static bool Building { get { return depth > 0; } }

        [HarmonyPrefix]
        private static void Prefix()
        {
            depth++;
        }

        [HarmonyFinalizer]
        private static Exception Finalizer(Exception __exception)
        {
            if (depth > 0) depth--;
            return __exception;
        }
    }
}
