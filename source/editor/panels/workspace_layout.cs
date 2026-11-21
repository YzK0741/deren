using System;
using System.IO;
using System.Text.Json;
using deren.scripting;

namespace deren.editor {
    internal sealed record workspace_layout(
        bool sidebar_left = false, bool dock_top = false, bool details_first = false,
        float sidebar_width = 350, float dock_height = 244, float outliner_height = 254) {

        internal static workspace_layout load(string path) {
            try {
                if (File.Exists(path)) {
                    var layout = JsonSerializer.Deserialize<workspace_layout>(File.ReadAllText(path));
                    if (layout != null) {
                        return layout with {
                            sidebar_width = Math.Clamp(layout.sidebar_width, 240, 900),
                            dock_height = Math.Clamp(layout.dock_height, 120, 700),
                            outliner_height = Math.Clamp(layout.outliner_height, 120, 700)
                        };
                    }
                }
            } catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) {
                host_logger.write($"Editor: could not read workspace layout ({error.Message})");
            }
            return new();
        }

        internal void save(string path) {
            try {
                File.WriteAllText(path, JsonSerializer.Serialize(this));
            } catch (Exception error) when (error is IOException or UnauthorizedAccessException) {
                host_logger.write($"Editor: could not save workspace layout ({error.Message})");
            }
        }
    }
}
