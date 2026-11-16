using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// The mirror of deren::scripting::animation_state (24 bytes): one frame of the host's character
    /// animation player. The host publishes it once per frame, so every query reads a consistent
    /// snapshot instead of reaching into the player while it advances.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct animation_state {
        /// <summary>The clips the player holds (0 until an animated asset is bound).</summary>
        public uint clip_count;
        /// <summary>The index of the selected clip.</summary>
        public uint current_clip;
        /// <summary>A clip is selected.</summary>
        public byte has_active;
        /// <summary>The clock advances on each frame.</summary>
        public byte playing;
        /// <summary>The host has a player at all.</summary>
        public byte available;
        public byte padding;
        /// <summary>Seconds into the clip.</summary>
        public float time;
        /// <summary>The clip's looping length in seconds.</summary>
        public float duration;
        /// <summary>Playback-rate multiplier (the host clamps it to 0..16).</summary>
        public float speed;
    }
}
