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
        private unsafe double query(uint kind) {
            if (!available(out _)) {
                return 0;
            }
            double result = 0;
            return audio_api.table.query(handle, kind, &result) == script_status.success ? result : 0;
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
        public double position => (double)query(0);
        public double duration => (double)query(1);
        public bool is_playing => query(2) != 0;
        public bool at_end => query(3) != 0;
        public float volume => (float)query(4);
        public float pan => (float)query(5);
        public float pitch => (float)query(6);
        public bool looping => query(7) != 0;
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
