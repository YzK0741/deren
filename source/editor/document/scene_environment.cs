using System;
using System.IO;

namespace deren.editor {
    internal static class scene_environment {
        internal static string resolve_path(string text, string project_root) {
            if (text.Length == 0 || Path.IsPathRooted(text)) {
                return text;
            }
            if (string.IsNullOrEmpty(project_root)) {
                return text;
            }
            try {
                var candidate = Path.GetFullPath(text, project_root);
                return File.Exists(candidate) ? candidate : text;
            } catch (Exception failure) when (failure is ArgumentException or NotSupportedException or PathTooLongException) {
                return text;
            }
        }
    }
}
