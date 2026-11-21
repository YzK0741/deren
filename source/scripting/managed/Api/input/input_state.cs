using System;
using System.Collections.Generic;

namespace deren {
    public static unsafe class input_state {
        // Match action_flag in the native input module.
        private const byte down = 1;
        private const byte pressed = 2;
        private const byte released = 4;

        [ThreadStatic]
        private static IReadOnlyDictionary<string, int>? actions;
        [ThreadStatic]
        private static byte* frame;
        // Bytes usable behind `frame`. The two-argument begin_frame() only knows the action count;
        // begin_frame(..., length) takes the real extent from frame_info.input_count.
        [ThreadStatic]
        private static int frame_length;

        public static bool is_down(string action) {
            return has_flag(action, down);
        }

        public static bool was_pressed(string action) {
            return has_flag(action, pressed);
        }

        public static bool was_released(string action) {
            return has_flag(action, released);
        }

        internal static void begin_frame(IReadOnlyDictionary<string, int> names, byte* values) {
            // One byte per registered action is the most this overload can promise; a game that knows
            // the native length should call the overload below instead.
            begin_frame(names, values, names.Count);
        }

        internal static void begin_frame(IReadOnlyDictionary<string, int> names, byte* values, int length) {
            actions = names;
            frame = values;
            frame_length = length < 0 ? 0 : length;
        }

        internal static void end_frame() {
            actions = null;
            frame = null;
            frame_length = 0;
        }

        private static bool has_flag(string action, byte flag) {
            if (actions == null || frame == null) {
                throw new InvalidOperationException("Input actions are available during on_update.");
            }
            if (!actions.TryGetValue(action, out var index)) {
                throw new ArgumentException($"Unknown input action '{action}'.", nameof(action));
            }
            if (index < 0 || index >= frame_length) {
                // Reading that slot would take whatever follows the borrow; the project's map and the
                // native buffer disagree, which is a project bug and is reported, not silently read.
                throw new InvalidOperationException(
                    $"The '{action}' action (slot {index}) is outside this frame's {frame_length}-byte input buffer.");
            }
            return (frame[index] & flag) != 0;
        }
    }
}
