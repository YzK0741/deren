using deren.scripting;

namespace deren {
    /// <summary>
    /// Read-only frame telemetry: the resolutions the frame actually rendered at, and the GPU timing
    /// table when the backend exposes timestamps.
    ///
    /// Everything here describes the frame that has already been submitted, so a value read during
    /// <c>after_frame</c> belongs to the frame that just finished and one read during <c>before_frame</c>
    /// belongs to the previous frame. Numbers are integers and costs are qualified (see
    /// <see cref="try_get_counters"/>) because both come straight from render code, not from a probe.
    /// </summary>
    public static unsafe class frame_diagnostics {
        /// <summary>
        /// One frame's counters. <see cref="render_width"/> x <see cref="render_height"/> is the extent
        /// the shading passes ran at; <see cref="output_width"/> x <see cref="output_height"/> is the
        /// extent that was presented (they differ under a render scale). <see cref="gpu_interval_count"/>
        /// counts the intervals in the GPU timing table - zero when the backend does not timestamp.
        /// </summary>
        public readonly struct frame_counters {
            public readonly uint render_width;
            public readonly uint render_height;
            public readonly uint output_width;
            public readonly uint output_height;
            public readonly uint gpu_interval_count;
            public readonly bool gpu_timings_available;

            internal frame_counters(scene_api.diagnostic_counters counters) {
                render_width = counters.render_width;
                render_height = counters.render_height;
                output_width = counters.output_width;
                output_height = counters.output_height;
                gpu_interval_count = counters.gpu_interval_count;
                gpu_timings_available = counters.gpu_timings_available != 0;
            }
        }

        /// <summary>
        /// The counters of the last frame. Returns false before the first frame has rendered, when no
        /// extent exists yet.
        /// </summary>
        public static bool try_get_counters(out frame_counters counters) {
            return try_get_counters(out counters, out _);
        }

        public static bool try_get_counters(out frame_counters counters, out script_status status) {
            counters = default;
            scene_api.diagnostic_counters native;
            status = scene_api.query_diagnostic_counters(&native);
            if (status != script_status.success) {
                return false;
            }
            counters = new frame_counters(native);
            return true;
        }

        /// <summary>
        /// The GPU timing table as text, one line per interval ("<c>gpu: shadow 0.11 ms</c>"), or null
        /// when no scene is rendering or the backend does not timestamp. The text is borrowed from the
        /// renderer only until the next frame, so it is copied into a managed string here.
        /// </summary>
        public static string? gpu_timing_text() {
            return gpu_timing_text(out _);
        }

        public static string? gpu_timing_text(out script_status status) {
            utf8_view view;
            status = scene_api.query_gpu_timing_text(&view);
            if (status != script_status.success) {
                return null;
            }
            return view.length == 0
                ? string.Empty
                : System.Text.Encoding.UTF8.GetString(view.data, checked((int)view.length));
        }
    }
}
