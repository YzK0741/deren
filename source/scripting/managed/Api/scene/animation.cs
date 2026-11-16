using System;
using System.Collections.Generic;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// The host's character animation player, seen from the game.
    /// </summary>
    /// <remarks>
    /// The PLAYER lives on the host: it samples the clip, writes the skin matrices and uploads the
    /// morph deltas between frames, so a pose costs the game nothing and cannot race the frame. The
    /// host builds it from the first bound asset whose manifest names a motion file (asset.toml's
    /// `motion` key); managed code decides WHICH clip plays, from when, and how fast. Until such an
    /// asset is bound, every entry here reports <see cref="script_status.module_unavailable"/> and
    /// <see cref="try_get_info"/> returns a zeroed, unavailable snapshot.
    ///
    /// Commands are queued for the next frame boundary rather than applied inline, so a successful
    /// call only means the request was accepted; the snapshot returned by <see cref="try_get_info"/>
    /// shows the applied result one frame later.
    /// </remarks>
    public static unsafe class animation {
        /// <summary>One frame of the player: what is playing, where its clock is, and how fast.</summary>
        public readonly struct info {
            /// <summary>The host has a player (an animated asset is bound).</summary>
            public bool available { get; init; }
            /// <summary>The clips the player holds.</summary>
            public int clip_count { get; init; }
            /// <summary>The index of the selected clip.</summary>
            public int current_clip { get; init; }
            /// <summary>A clip is selected.</summary>
            public bool has_active { get; init; }
            /// <summary>The clock advances on each frame.</summary>
            public bool playing { get; init; }
            /// <summary>Seconds into the clip.</summary>
            public float time { get; init; }
            /// <summary>The clip's looping length in seconds.</summary>
            public float duration { get; init; }
            /// <summary>Playback-rate multiplier; the host clamps it to 0..16.</summary>
            public float speed { get; init; }
        }

        /// <summary>The fastest playback multiplier the host keeps.</summary>
        public const float max_speed = 16.0F;

        /// <summary>The player as of the frame the host last finished.</summary>
        public static bool try_get_info(out info value) {
            return try_get_info(out value, out _);
        }

        public static bool try_get_info(out info value, out script_status status) {
            value = default;
            animation_state state;
            status = scene_api.query_animation_state(&state);
            if (status != script_status.success) {
                return false;
            }
            value = new info {
                available = state.available != 0,
                clip_count = checked((int)state.clip_count),
                current_clip = checked((int)state.current_clip),
                has_active = state.has_active != 0,
                playing = state.playing != 0,
                time = state.time,
                duration = state.duration,
                speed = state.speed
            };
            return state.available != 0;
        }

        /// <summary>The number of clips the player holds.</summary>
        public static bool try_get_clip_count(out int count) {
            uint value = 0;
            var status = scene_api.query_animation_playable_count(&value);
            count = status == script_status.success ? checked((int)value) : 0;
            return status == script_status.success;
        }

        /// <summary>Every clip's name, copied out of the player.</summary>
        public static string[] get_clip_names() {
            if (!try_get_clip_count(out var count)) {
                return Array.Empty<string>();
            }
            var names = new List<string>(count);
            for (var index = 0; index < count; ++index) {
                if (!try_get_clip_name(index, out var name)) {
                    return Array.Empty<string>();
                }
                names.Add(name!);
            }
            return names.ToArray();
        }

        /// <summary>One clip's name, copied out of the player.</summary>
        public static bool try_get_clip_name(int index, out string? name) {
            return try_get_clip_name(index, out name, out _);
        }

        public static bool try_get_clip_name(int index, out string? name, out script_status status) {
            name = null;
            if (index < 0) {
                status = script_status.invalid_argument;
                return false;
            }
            utf8_view view;
            status = scene_api.query_animation_playable_name((uint)index, &view);
            if (status != script_status.success) {
                return false;
            }
            if (view.data == null) {
                status = script_status.invalid_argument;
                return false;
            }
            name = Encoding.UTF8.GetString(view.data, checked((int)view.length));
            return true;
        }

        /// <summary>Plays the current clip (or the first one, when nothing is selected yet).</summary>
        public static bool try_play() {
            return try_set_playing(true);
        }

        /// <summary>Pauses the clock without changing the clip or its time.</summary>
        public static bool try_pause() {
            return try_set_playing(false);
        }

        public static bool try_set_playing(bool playing) {
            var status = scene_api.assign_animation_playing(playing ? (byte)1 : (byte)0);
            report(status, playing ? "play" : "pause");
            return status == script_status.success;
        }

        /// <summary>Selects a clip by index; the host opens it at time 0 and keeps the current speed.</summary>
        public static bool try_select(int index) {
            if (index < 0) {
                log.write($"Game: animation clip {index} is out of range");
                return false;
            }
            var status = scene_api.assign_animation_select((uint)index);
            report(status, $"select clip {index}");
            return status == script_status.success;
        }

        /// <summary>Sets the playback-rate multiplier (clamped by the host to <see cref="max_speed"/>).</summary>
        public static bool try_set_speed(float speed) {
            if (float.IsNaN(speed) || float.IsInfinity(speed) || speed < 0.0F) {
                log.write($"Game: animation speed {speed} is not a finite, non-negative number");
                return false;
            }
            var status = scene_api.assign_animation_speed(speed);
            report(status, $"speed {speed}");
            return status == script_status.success;
        }

        /// <summary>
        /// Scrubs to a time in seconds and pauses, so dragging a timeline does not fight the advancing
        /// clock; the host clamps the time into the clip.
        /// </summary>
        public static bool try_seek(float seconds) {
            if (float.IsNaN(seconds) || float.IsInfinity(seconds)) {
                log.write($"Game: animation time {seconds} is not a finite number");
                return false;
            }
            var status = scene_api.assign_animation_time(seconds);
            report(status, $"seek {seconds}");
            return status == script_status.success;
        }

        private static void report(script_status status, string what) {
            if (status != script_status.success) {
                log.write($"Game: animation {what} failed ({status})");
            }
        }
    }
}
