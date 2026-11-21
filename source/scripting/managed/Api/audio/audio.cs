#pragma warning disable CS8981 // Public audio mirrors the native API and repository snake_case policy.
using System;
using System.Threading;
using deren.scripting;

namespace deren {
    /// <summary>An independently controlled playback instance. Use using/Dispose to detach it promptly.</summary>
    public sealed class audio : IDisposable {
        private ulong value;
        private readonly int epoch;
        private audio(ulong value) {
            this.value = value;
            epoch = audio_api.epoch;
        }
        private ulong handle => Volatile.Read(ref value);
        public bool is_valid => handle != 0 && audio_api.is_current(epoch);

        public static audio_buffer? load_audio_buffer_from_file(string path) =>
            audio_buffer.try_load_from_file(path, out var result) ? result : null;
        public static audio? create(audio_buffer buffer) => try_create(buffer, out var result) ? result : null;
        public static audio? create_from_file(string path) => try_create_from_file(path, out var result) ? result : null;
        public static bool try_create(audio_buffer buffer, out audio? result) => try_create(buffer, out result, out _);
        public static unsafe bool try_create(audio_buffer buffer, out audio? result, out script_status status) {
            result = null;
            if (!audio_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            if (buffer == null || !buffer.is_valid) {
                status = script_status.asset_invalid;
                return false;
            }
            ulong value = 0;
            status = audio_api.table.create(buffer.handle, &value);
            return accept(value, status, out result);
        }
        public static bool try_create_from_file(string path, out audio? result) => try_create_from_file(path, out result, out _);
        public static unsafe bool try_create_from_file(string path, out audio? result, out script_status status) {
            result = null;
            if (!audio_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            if (!audio_api.try_path(path, out var text, out status)) {
                return false;
            }
            using (text) {
                ulong value = 0;
                status = audio_api.table.create_from_file(text.view, &value);
                return accept(value, status, out result);
            }
        }
        public audio? clone(bool clone_state = false) => try_clone(out var result, clone_state) ? result : null;
        public bool try_clone(out audio? result, bool clone_state = false) => try_clone(out result, out _, clone_state);
        public unsafe bool try_clone(out audio? result, out script_status status, bool clone_state = false) {
            result = null;
            if (!available(out status)) {
                return false;
            }
            ulong value = 0;
            status = audio_api.table.clone(handle, clone_state ? (byte)1 : (byte)0, &value);
            return accept(value, status, out result);
        }
        private static bool accept(ulong value, script_status status, out audio? result) {
            result = status == script_status.success && value != 0 ? new audio(value) : null;
            return result != null;
        }
        private bool available(out script_status status) {
            status = !audio_api.is_ready() ? script_status.module_unavailable :
                !is_valid ? script_status.asset_invalid : script_status.success;
            return status == script_status.success;
        }
        private unsafe bool command(uint kind, double argument, out script_status status) {
            if (!available(out status)) {
                return false;
            }
            status = audio_api.table.command(handle, kind, argument);
            return status == script_status.success;
        }
        private unsafe bool try_query(uint kind, out double result, out script_status status) {
            result = 0;
            if (!available(out status)) {
                return false;
            }
            // The address of an `out` parameter cannot be taken (CS0212), so the ABI writes a local
            // cell and the value only reaches the caller when the status says the read happened.
            double cell = 0;
            status = audio_api.table.query(handle, kind, &cell);
            result = status == script_status.success ? cell : 0;
            if (status != script_status.success) {
                // The ABI writes nothing on failure, so whatever the cell held is not state.
                result = 0;
                return false;
            }
            return true;
        }
        public bool play() => play(out _);
        public bool play(out script_status status) => command(0, 0, out status);
        public bool pause() => pause(out _);
        public bool pause(out script_status status) => command(1, 0, out status);
        public bool stop() => stop(out _);
        public bool stop(out script_status status) => command(2, 0, out status);
        public bool seek(double seconds) => seek(seconds, out _);
        public bool seek(double seconds, out script_status status) => command(3, seconds, out status);
        public bool try_set_volume(float volume) => try_set_volume(volume, out _);
        public bool try_set_volume(float volume, out script_status status) => command(4, volume, out status);
        public bool try_set_pan(float pan) => try_set_pan(pan, out _);
        public bool try_set_pan(float pan, out script_status status) => command(5, pan, out status);
        public bool try_set_pitch(float pitch) => try_set_pitch(pitch, out _);
        public bool try_set_pitch(float pitch, out script_status status) => command(6, pitch, out status);
        public bool try_set_looping(bool enabled) => try_set_looping(enabled, out _);
        public bool try_set_looping(bool enabled, out script_status status) => command(7, enabled ? 1 : 0, out status);

        // THE READS CAN FAIL, and "unavailable" is not a value: a stale handle and a failed query both
        // answer zero, and a property has nowhere to say so. The try_get forms hand the status back;
        // the properties keep the old shapes for call sites that only want the number, and their
        // docs say which value a failed read fabricates.
        public bool try_get_position(out double value) => try_get_position(out value, out _);
        public bool try_get_position(out double value, out script_status status) => try_query(0, out value, out status);
        public bool try_get_duration(out double value) => try_get_duration(out value, out _);
        public bool try_get_duration(out double value, out script_status status) => try_query(1, out value, out status);
        public bool try_get_is_playing(out bool value) => try_get_is_playing(out value, out _);
        public bool try_get_is_playing(out bool value, out script_status status) {
            var ok = try_query(2, out var raw, out status);
            value = ok && raw != 0;
            return ok;
        }
        public bool try_get_at_end(out bool value) => try_get_at_end(out value, out _);
        public bool try_get_at_end(out bool value, out script_status status) {
            var ok = try_query(3, out var raw, out status);
            value = ok && raw != 0;
            return ok;
        }
        public bool try_get_volume(out float value) => try_get_volume(out value, out _);
        public bool try_get_volume(out float value, out script_status status) {
            var ok = try_query(4, out var raw, out status);
            value = ok ? (float)raw : 0.0f;
            return ok;
        }
        public bool try_get_pan(out float value) => try_get_pan(out value, out _);
        public bool try_get_pan(out float value, out script_status status) {
            var ok = try_query(5, out var raw, out status);
            value = ok ? (float)raw : 0.0f;
            return ok;
        }
        public bool try_get_pitch(out float value) => try_get_pitch(out value, out _);
        public bool try_get_pitch(out float value, out script_status status) {
            var ok = try_query(6, out var raw, out status);
            value = ok ? (float)raw : 0.0f;
            return ok;
        }
        public bool try_get_looping(out bool value) => try_get_looping(out value, out _);
        public bool try_get_looping(out bool value, out script_status status) {
            var ok = try_query(7, out var raw, out status);
            value = ok && raw != 0;
            return ok;
        }

        /// <summary>Seconds into the sound; 0 when the handle is stale or the query failed.</summary>
        public double position => try_get_position(out var value, out _) ? value : 0;
        /// <summary>The sound's length in seconds; 0 when the handle is stale or the query failed.</summary>
        public double duration => try_get_duration(out var value, out _) ? value : 0;
        /// <summary>Whether the clock advances; false when the handle is stale or the query failed.</summary>
        public bool is_playing => try_get_is_playing(out var value, out _) && value;
        /// <summary>Whether the cursor reached the end; false when the handle is stale or the query failed.</summary>
        public bool at_end => try_get_at_end(out var value, out _) && value;
        /// <summary>The playback gain; 0 when the handle is stale or the query failed.</summary>
        public float volume => try_get_volume(out var value, out _) ? value : 0.0f;
        /// <summary>The stereo pan; 0 when the handle is stale or the query failed.</summary>
        public float pan => try_get_pan(out var value, out _) ? value : 0.0f;
        /// <summary>The pitch scale; 0 when the handle is stale or the query failed.</summary>
        public float pitch => try_get_pitch(out var value, out _) ? value : 0.0f;
        /// <summary>Whether playback loops; false when the handle is stale or the query failed.</summary>
        public bool looping => try_get_looping(out var value, out _) && value;
        public bool release(out script_status status) {
            if (handle == 0 || !audio_api.is_current(epoch)) {
                Interlocked.Exchange(ref value, 0);
                status = script_status.success;
                return true;
            }
            if (!available(out status)) {
                return false;
            }
            status = release_native(handle);
            if (status == script_status.success || status == script_status.asset_invalid) {
                Interlocked.Exchange(ref value, 0);
            }
            return status == script_status.success;
        }
        private static unsafe script_status release_native(ulong value) => audio_api.table.release(value);
        public void Dispose() {
            release(out _);
        }
    }
}
