using System;

namespace deren.scripting {
    /// <summary>
    /// The pacing policy and the older one-shot hook, kept as the ergonomic front door.
    /// Frame-boundary callables themselves live in <see cref="frame_event_processor"/>, which owns
    /// the queue and the completion notice the render loop waits for.
    /// </summary>
    public static class frame_scheduler {
        /// <summary>
        /// Sets the frame rate cap: 0 runs uncapped (what a throughput measurement wants), a positive
        /// value paces the loop. The host rejects a negative or non-finite value instead of clamping.
        /// </summary>
        public static bool try_set_max_fps(double fps) {
            return try_set_max_fps(fps, out _);
        }

        public static bool try_set_max_fps(double fps, out script_status status) {
            status = scene_api.assign_max_fps(fps);
            log.write(status == script_status.success
                ? $"Game: max fps: {(fps > 0.0 ? fps.ToString("0.##") : "uncapped")}"
                : $"Game: max fps {fps} rejected ({status})");
            return status == script_status.success;
        }

        /// <summary>
        /// Queues one callable for the next after-frame boundary. A callable added while a boundary
        /// is being dispatched runs on the following frame, never recursively.
        /// </summary>
        public static void do_after_frame(Action action) {
            frame_event_processor.do_after_frame_once(action);
        }

        internal static void clear() {
            frame_event_processor.clear_all();
        }
    }
}
