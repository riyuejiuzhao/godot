using Godot;
using System;
using GodotTools.ProjectEditor;

namespace GodotTools
{
    public static class CsProjOperations
    {
        public static string GenerateGameProject(string dir, string name, bool hasGdExtension)
        {
            try
            {
                return ProjectGenerator.GenAndSaveGameProject(dir, name, hasGdExtension);
            }
            catch (Exception e)
            {
                GD.PushError(e.ToString());
                return string.Empty;
            }
        }
    }
}
