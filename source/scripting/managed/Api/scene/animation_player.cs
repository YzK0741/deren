using System;
using System.Collections.Generic;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// One placement's character animation player, seen from the game.
    /// </summary>
    /// <remarks>
    /// The PLAYER lives on the host: it samples the clip, writes the skin matrices and uploads the
    /// morph deltas between frames, so a pose costs the game nothing and cannot race the frame. The
    /// host builds one from every bound asset whose manifest names a motion file (asset.toml's
    /// `motion` key) - one per PLACEMENT, so two characters of the same asset animate independently.
    /// Managed code decides WHICH placement's player is addressed, and then which clip plays, from
    /// when, and how fast. Until such an asset is bound, every entry here reports
    /// <see cref="script_status.scene_not_found"/> and <see cref="try_get_info"/> returns a zeroed,
    /// unavailable snapshot.
    ///
    /// A <see cref="node_id"/> names a placement. A default (invalid) handle is a convenience for a
    /// game that has no handle to pass - it addresses the scene's first player, which is what a
    /// single-character game needs.
    ///
    /// Commands are queued for the next frame boundary rather than applied inline, so a successful
    /// call only means the request was accepted; the snapshot returned by <see cref="try_get_info"/>
    /// shows the applied result one frame later.
    /// </remarks>
    public static unsafe class animation_player {
        /// <summary>One frame of a player: what is playing, where its clock is, and how fast.</summary>
        public readonly struct animation_info {
            /// <summary>The placement has a player (an animated asset is bound at it).</summary>
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

        /// <summary>The scene's first player, as of the frame the host last finished.</summary>
        public static bool try_get_info(out animation_info value) {
            return try_get_info(default, out value, out _);
        }

        /// <summary>A placement's player, as of the frame the host last finished.</summary>
        public static bool try_get_info(node_id node, out animation_info value) {
            return try_get_info(node, out value, out _);
        }

        public static bool try_get_info(out animation_info value, out script_status status) {
            return try_get_info(default, out value, out status);
        }

        public static bool try_get_info(node_id node, out animation_info value, out script_status status) {
            value = default;
            animation_state state;
            status = scene_api.query_animation_state(node.tree_handle, node.handle, &state);
            if (status != script_status.success) {
                return false;
            }
            if (state.available == 0) {
                // A SUCCESS WITHOUT A PLAYER: the host answered, but the placement has no animated
                // asset. Returning true here would contradict the snapshot (all zeros) and the
                // remarks, so this is the scene_not_found they promise.
                status = script_status.scene_not_found;
                return false;
            }
            value = new animation_info {
                available = true,
                clip_count = checked((int)state.clip_count),
                current_clip = checked((int)state.current_clip),
                has_active = state.has_active != 0,
                playing = state.playing != 0,
                time = state.time,
                duration = state.duration,
                speed = state.speed
            };
            return true;
        }

        /// <summary>The number of clips the scene's first player holds.</summary>
        public static bool try_get_clip_count(out int count) {
            return try_get_clip_count(default, out count);
        }

        /// <summary>The number of clips a placement's player holds.</summary>
        public static bool try_get_clip_count(node_id node, out int count) {
            uint value = 0;
            var status = scene_api.query_animation_playable_count(node.tree_handle, node.handle, &value);
            count = status == script_status.success ? checked((int)value) : 0;
            return status == script_status.success;
        }

        /// <summary>Every clip's name, copied out of the scene's first player.</summary>
        public static string[] get_clip_names() {
            return get_clip_names(default);
        }

        /// <summary>Every clip's name, copied out of a placement's player.</summary>
        public static string[] get_clip_names(node_id node) {
            if (!try_get_clip_count(node, out var count)) {
                return [];
            }
            var names = new List<string>(count);
            for (var index = 0; index < count; ++index) {
                if (!try_get_clip_name(node, index, out var name)) {
                    return [];
                }
                names.Add(name!);
            }
            return [.. names];
        }

        /// <summary>One clip's name, copied out of the scene's first player.</summary>
        public static bool try_get_clip_name(int index, out string? name) {
            return try_get_clip_name(default, index, out name, out _);
        }

        /// <summary>One clip's name, copied out of a placement's player.</summary>
        public static bool try_get_clip_name(node_id node, int index, out string? name) {
            return try_get_clip_name(node, index, out name, out _);
        }

        public static bool try_get_clip_name(int index, out string? name, out script_status status) {
            return try_get_clip_name(default, index, out name, out status);
        }

        public static bool try_get_clip_name(node_id node, int index, out string? name, out script_status status) {
            name = null;
            if (index < 0) {
                status = script_status.invalid_argument;
                return false;
            }
            utf8_view view;
            status = scene_api.query_animation_playable_name(node.tree_handle, node.handle, (uint)index, &view);
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

        /// <summary>Plays the first player's current clip (or its first one, when nothing is selected).</summary>
        public static bool try_play() {
            return try_set_playing(default, true, out _);
        }

        /// <summary>Plays a placement's current clip (or its first one, when nothing is selected).</summary>
        public static bool try_play(node_id node) {
            return try_set_playing(node, true, out _);
        }

        /// <summary>Pauses the scene's first player without changing its clip or its time.</summary>
        public static bool try_pause() {
            return try_set_playing(default, false, out _);
        }

        /// <summary>Pauses a placement's player without changing its clip or its time.</summary>
        public static bool try_pause(node_id node) {
            return try_set_playing(node, false, out _);
        }

        public static bool try_set_playing(bool playing) {
            return try_set_playing(default, playing, out _);
        }

        public static bool try_set_playing(bool playing, out script_status status) {
            return try_set_playing(default, playing, out status);
        }

        public static bool try_set_playing(node_id node, bool playing) {
            return try_set_playing(node, playing, out _);
        }

        public static bool try_set_playing(node_id node, bool playing, out script_status status) {
            status = scene_api.assign_animation_playing(node.tree_handle, node.handle, playing ? (byte)1 : (byte)0);
            report(status, playing ? "play" : "pause");
            return status == script_status.success;
        }

        /// <summary>Selects a clip on the scene's first player; the host opens it at time 0.</summary>
        public static bool try_select(int index) {
            return try_select(default, index, out _);
        }

        /// <summary>Selects a clip on a placement's player; the host opens it at time 0 and keeps the speed.</summary>
        public static bool try_select(node_id node, int index) {
            return try_select(node, index, out _);
        }

        public static bool try_select(int index, out script_status status) {
            return try_select(default, index, out status);
        }

        public static bool try_select(node_id node, int index, out script_status status) {
            if (index < 0) {
                host_logger.write($"Game: animation clip {index} is out of range");
                status = script_status.invalid_argument;
                return false;
            }
            status = scene_api.assign_animation_select(node.tree_handle, node.handle, (uint)index);
            report(status, $"select clip {index}");
            return status == script_status.success;
        }

        /// <summary>Sets the scene's first player's playback-rate multiplier (clamped to <see cref="max_speed"/>).</summary>
        public static bool try_set_speed(float speed) {
            return try_set_speed(default, speed, out _);
        }

        /// <summary>Sets a placement's playback-rate multiplier (clamped to <see cref="max_speed"/>).</summary>
        public static bool try_set_speed(node_id node, float speed) {
            return try_set_speed(node, speed, out _);
        }

        public static bool try_set_speed(float speed, out script_status status) {
            return try_set_speed(default, speed, out status);
        }

        public static bool try_set_speed(node_id node, float speed, out script_status status) {
            if (float.IsNaN(speed) || float.IsInfinity(speed) || speed < 0.0F) {
                host_logger.write($"Game: animation speed {speed} is not a finite, non-negative number");
                status = script_status.invalid_argument;
                return false;
            }
            status = scene_api.assign_animation_speed(node.tree_handle, node.handle, speed);
            report(status, $"speed {speed}");
            return status == script_status.success;
        }

        /// <summary>
        /// Scrubs the scene's first player to a time in seconds and pauses, so dragging a timeline
        /// does not fight the advancing clock; the host clamps the time into the clip.
        /// </summary>
        public static bool try_seek(float seconds) {
            return try_seek(default, seconds, out _);
        }

        /// <summary>Scrubs a placement's player to a time in seconds and pauses.</summary>
        public static bool try_seek(node_id node, float seconds) {
            return try_seek(node, seconds, out _);
        }

        public static bool try_seek(float seconds, out script_status status) {
            return try_seek(default, seconds, out status);
        }

        public static bool try_seek(node_id node, float seconds, out script_status status) {
            if (float.IsNaN(seconds) || float.IsInfinity(seconds)) {
                host_logger.write($"Game: animation time {seconds} is not a finite number");
                status = script_status.invalid_argument;
                return false;
            }
            status = scene_api.assign_animation_time(node.tree_handle, node.handle, seconds);
            report(status, $"seek {seconds}");
            return status == script_status.success;
        }

        private static void report(script_status status, string what) {
            if (status != script_status.success) {
                host_logger.write($"Game: animation {what} failed ({status})");
            }
        }
    }
}
