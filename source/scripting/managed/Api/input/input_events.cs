using System;
using System.Collections.Generic;
using deren.scripting;

namespace deren {
    /// <summary>
    /// The kind of window event a <see cref="input_event"/> carries. The names are the managed
    /// vocabulary; the numeric values are the native ABI's and never appear in game code.
    /// </summary>
    public enum input_type : uint {
        mouse_move = 1,
        mouse_button = 2,
        scroll = 3,
        key = 4,
        character = 5,
        focus_lost = 6
    }

    /// <summary>
    /// One window event, as the game sees it. The host reports everything the window produces and
    /// lets the game decide what any of it means - the engine holds no input state of its own.
    ///
    /// Fields that a kind does not define keep their previous meaning (zero for a fresh struct), so
    /// a handler switches on <see cref="type"/> first.
    /// </summary>
    public readonly struct input_event {
        public input_event(input_type type, int button, bool down, int key, float x, float y, float dx,
                           float dy, float wheel, uint codepoint) {
            this.type = type;
            this.button = button;
            this.down = down;
            this.key = key;
            this.x = x;
            this.y = y;
            this.dx = dx;
            this.dy = dy;
            this.wheel = wheel;
            this.codepoint = codepoint;
        }

        public input_type type { get; }
        /// <summary>The mouse button index (0 left, 1 right, 2 middle) for <see cref="input_type.mouse_button"/>.</summary>
        public int button { get; }
        /// <summary>Whether the button or key went down (false means it went up).</summary>
        public bool down { get; }
        /// <summary>The GLFW keycode for <see cref="input_type.key"/>; compare against <see cref="key_code"/>.</summary>
        public int key { get; }
        /// <summary>The cursor position in window pixels, for the pointer events.</summary>
        public float x { get; }
        public float y { get; }
        /// <summary>How far the cursor moved since the previous event, for <see cref="input_type.mouse_move"/>.</summary>
        public float dx { get; }
        public float dy { get; }
        /// <summary>The scroll amount for <see cref="input_type.scroll"/> (positive is up/away).</summary>
        public float wheel { get; }
        /// <summary>The Unicode codepoint typed, for <see cref="input_type.character"/>.</summary>
        public uint codepoint { get; }
    }

    /// <summary>
    /// Registers ONE handler that receives every window event, delivered on the render thread during
    /// the frame's update. This is the whole input contract: a game that wants a pointer-locked
    /// camera, a text field or a hotkey reads these events and keeps its own state.
    ///
    /// Passing null removes the handler and stops the forwarding. The call returns false only when
    /// the host exposes no input entry at all (an older host), never because the host has no window.
    /// </summary>
    public static unsafe class input_events {
        private static Action<input_event>? handler;

        public static bool try_set_callback(Action<input_event>? value) {
            return try_set_callback(value, out _);
        }

        public static bool try_set_callback(Action<input_event>? value, out script_status status) {
            var api = managed_bootstrap.core;
            if (api.set_input_event_callback == null) {
                status = script_status.module_unavailable;
                return false;
            }
            handler = value;
            api.set_input_event_callback(value == null ? null : &dispatch, null);
            status = script_status.success;
            return true;
        }

        public static bool active => handler != null;

        [System.Runtime.InteropServices.UnmanagedCallersOnly]
        private static void dispatch(void* context, scripting.native_input_event* value) {
            var callback = handler;
            if (callback == null || value == null) {
                return;
            }
            try {
                var record = *value;
                callback(new input_event((input_type)record.type, record.button, record.down != 0, record.key,
                                         record.x, record.y, record.dx, record.dy, record.wheel, record.codepoint));
            } catch (Exception error) {
                logger.write(
                    $"Game: input handler threw: {error.GetBaseException().Message}");
            }
        }
    }

    /// <summary>
    /// The GLFW keycodes the host forwards, named so a game does not hard-code numbers. Only the keys
    /// the managed side is likely to need are listed; any other code still arrives as its raw value.
    /// </summary>
    public static class key_code {
        public const int escape = 256;
        public const int enter = 257;
        public const int tab = 258;
        public const int backspace = 259;
        public const int space = 32;
        public const int left = 263;
        public const int right = 262;
        public const int up = 265;
        public const int down = 264;
        public const int a = 65;
        public const int d = 68;
        public const int e = 69;
        public const int f = 70;
        public const int q = 81;
        public const int r = 82;
        public const int s = 83;
        public const int w = 87;
        public const int left_shift = 340;
        public const int left_control = 341;
        public const int left_alt = 342;
    }
}
