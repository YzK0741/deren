using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// Renderer decisions owned by the project. The native host fills nothing here: it passes the
    /// structure to <see cref="game_entry.preinit"/>, the project writes the values, and the host
    /// consumes them before creating the window and the Vulkan runtime.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct startup_options {
        public int capture_frames;
        public int capture_windowed;
        public int camera_set;
        public float camera_yaw_degrees;
        public float camera_pitch_degrees;
        public float camera_distance;
        public int target_set;
        public vector_3 target;
        public float sweep_yaw_deg_per_frame;
        public float animation_seconds_per_frame;

        /// <summary>Editor frontend: 0 selects the classic RmlUi shell, 1 the editor's own UI layer.</summary>
        public int editor_ui;

        /// <summary>
        /// Editor UI scale, 1.0 being the authored size. Zero follows the window's display scale.
        /// </summary>
        public float ui_scale;
    }
}
