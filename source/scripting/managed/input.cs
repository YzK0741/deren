using System;
using System.Collections.Generic;

namespace deren {
#pragma warning disable CS8981
    public static unsafe class input {
#pragma warning restore CS8981
        // Match action_flag in the native input module.
        private const byte down = 1;
        private const byte pressed = 2;
        private const byte released = 4;

        [ThreadStatic]
        private static IReadOnlyDictionary<string, int>? actions;
        [ThreadStatic]
        private static byte* frame;

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
            actions = names;
            frame = values;
        }

        internal static void end_frame() {
            actions = null;
            frame = null;
        }

        private static bool has_flag(string action, byte flag) {
            if (actions == null) {
                throw new InvalidOperationException("Input actions are available during on_update.");
            }
            if (!actions.TryGetValue(action, out var index)) {
                throw new ArgumentException($"Unknown input action '{action}'.", nameof(action));
            }
            return (frame[index] & flag) != 0;
        }
    }
}
