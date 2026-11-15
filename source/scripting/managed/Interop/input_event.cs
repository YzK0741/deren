using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>What an <see cref="input_event"/> describes; the numbers are the ABI's.</summary>
    internal enum input_event_type : uint {
        mouse_move = 1,
        mouse_button = 2,
        scroll = 3,
        key = 4,
        character = 5,
        focus_lost = 6
    }

    /// <summary>
    /// One window event across the ABI: the flat mirror of deren::scripting::input_event (40 bytes).
    ///
    /// One struct carries every kind because the host reports on the render thread and a union would
    /// buy nothing but a second layout to keep in step. A reader switches on <see cref="type"/> and
    /// takes the fields that kind defines: `x`/`y` are the cursor, `dx`/`dy` the cursor delta,
    /// `wheel` the scroll amount, `key` the GLFW keycode, `codepoint` the typed character.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct native_input_event {
        public input_event_type type;
        public int button;
        public byte down;
        public fixed byte padding[3];
        public int key;
        public float x;
        public float y;
        public float dx;
        public float dy;
        public float wheel;
        public uint codepoint;
    }
}
