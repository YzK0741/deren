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
        /// Sets how long the host waits for one frame boundary before it treats the managed host as
        /// dead. 0 waits indefinitely, which a debugger breakpoint or a deliberately long blocking
        /// task needs; the host caps the value and reports invalid_argument above the cap. The
        /// default is 5000 ms, and a host without the entry (ABI 34 or older) reports
        /// module_unavailable.
        /// </summary>
        public static bool set_boundary_timeout_ms(uint milliseconds) {
            return set_boundary_timeout_ms(milliseconds, out _);
        }

        public static unsafe bool set_boundary_timeout_ms(uint milliseconds, out script_status status) {
            if (managed_bootstrap.core.set_frame_boundary_timeout == null) {
                status = script_status.module_unavailable;
                return false;
            }
            status = managed_bootstrap.core.set_frame_boundary_timeout(milliseconds);
            log.write(status == script_status.success
                ? $"Game: frame boundary timeout: {(milliseconds == 0 ? "unlimited" : $"{milliseconds} ms")}"
                : $"Game: frame boundary timeout {milliseconds} ms rejected ({status})");
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
