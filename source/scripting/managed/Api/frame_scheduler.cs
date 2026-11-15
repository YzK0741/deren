using System;
using System.Collections.Generic;
using System.Text;

namespace deren.scripting {
    /// <summary>
    /// Collects managed callables and runs them when the native render loop emits a frame signal.
    /// The native host never stores or invokes individual callables.
    /// </summary>
    public static class frame_scheduler {
        /// <summary>Signal value emitted by the host after a presented frame.</summary>
        public const uint after_frame = 1;

        /// <summary>
        /// Sets the frame rate cap: 0 runs uncapped (what a throughput measurement wants), a positive
        /// value paces the loop. The host rejects a negative or non-finite value instead of clamping.
        /// </summary>
        public static bool try_set_max_fps(double fps) {
            return try_set_max_fps(fps, out _);
        }

        public static bool try_set_max_fps(double fps, out script_status status) {
            status = scene_api.assign_max_fps(fps);
            write_log(status == script_status.success
                ? $"Game: max fps: {(fps > 0.0 ? fps.ToString("0.##") : "uncapped")}"
                : $"Game: max fps {fps} rejected ({status})");
            return status == script_status.success;
        }

        private static readonly object gate = new();
        private static List<Action> pending = [];

        /// <summary>
        /// Queues one callable for the next after-frame signal. A callable added while a signal is
        /// being dispatched runs on the following signal, never recursively.
        /// </summary>
        public static void do_after_frame(Action action) {
            ArgumentNullException.ThrowIfNull(action);
            lock (gate) {
                pending.Add(action);
            }
        }

        internal static void dispatch(uint signal) {
            if (signal != after_frame) {
                write_log($"Frame scheduler: unknown signal {signal}.");
                return;
            }

            List<Action> actions;
            lock (gate) {
                if (pending.Count == 0) {
                    return;
                }
                actions = pending;
                pending = [];
            }

            foreach (var action in actions) {
                try {
                    action();
                } catch (Exception error) {
                    write_log($"Frame scheduler: callable failed: {error.GetBaseException().Message}");
                }
            }
        }

        internal static void clear() {
            lock (gate) {
                pending.Clear();
            }
        }

        private static unsafe void write_log(string message) {
            if (managed_bootstrap.core.log == null) {
                return;
            }
            managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(message));
        }
    }
}
