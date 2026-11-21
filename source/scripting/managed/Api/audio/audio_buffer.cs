using System;
using System.Threading;
using deren.scripting;

namespace deren {
    /// <summary>One managed load reference. Existing audio objects retain their own shared PCM owner.</summary>
    public sealed class audio_buffer : IDisposable {
        private ulong value;
        private readonly int epoch;
        internal audio_buffer(ulong value) {
            this.value = value;
            epoch = audio_api.epoch;
        }
        internal ulong handle => Volatile.Read(ref value);
        public bool is_valid => handle != 0 && audio_api.is_current(epoch);
        private unsafe bool try_query(uint field, out double result, out script_status status) {
            result = 0;
            status = !audio_api.is_ready() ? script_status.module_unavailable :
                !is_valid ? script_status.asset_invalid : script_status.success;
            if (status != script_status.success) {
                return false;
            }
            // The address of an `out` parameter cannot be taken (CS0212), so the ABI writes a local
            // cell and only a successful status lets it reach the caller.
            double cell = 0;
            status = audio_api.table.buffer_query(handle, field, &cell);
            result = status == script_status.success ? cell : 0;
            if (status != script_status.success) {
                // The ABI writes nothing on failure, so the cell is a guess, not a decoded value.
                result = 0;
                return false;
            }
            return true;
        }

        // Same split as deren.audio: the try_get forms carry the status, the properties keep the old
        // shapes and say which value a failed read fabricates.
        public bool try_get_channels(out uint value) => try_get_channels(out value, out _);
        public bool try_get_channels(out uint value, out script_status status) {
            var ok = try_query(0, out var raw, out status);
            value = ok ? (uint)raw : 0;
            return ok;
        }
        public bool try_get_sample_rate(out uint value) => try_get_sample_rate(out value, out _);
        public bool try_get_sample_rate(out uint value, out script_status status) {
            var ok = try_query(1, out var raw, out status);
            value = ok ? (uint)raw : 0;
            return ok;
        }
        public bool try_get_frame_count(out ulong value) => try_get_frame_count(out value, out _);
        public bool try_get_frame_count(out ulong value, out script_status status) {
            var ok = try_query(2, out var raw, out status);
            value = ok ? (ulong)raw : 0;
            return ok;
        }
        public bool try_get_duration(out double value) => try_get_duration(out value, out _);
        public bool try_get_duration(out double value, out script_status status) => try_query(3, out value, out status);

        /// <summary>The channel count; 0 when the handle is stale or the query failed.</summary>
        public uint channels => try_get_channels(out var value, out _) ? value : 0;
        /// <summary>The sample rate in Hz; 0 when the handle is stale or the query failed.</summary>
        public uint sample_rate => try_get_sample_rate(out var value, out _) ? value : 0;
        /// <summary>The number of frames; 0 when the handle is stale or the query failed.</summary>
        public ulong frame_count => try_get_frame_count(out var value, out _) ? value : 0;
        /// <summary>The decoded length in seconds; 0 when the handle is stale or the query failed.</summary>
        public double duration => try_get_duration(out var value, out _) ? value : 0;

        public static bool try_load_from_file(string path, out audio_buffer? buffer) =>
            try_load_from_file(path, out buffer, out _);
        public static unsafe bool try_load_from_file(string path, out audio_buffer? buffer, out script_status status) {
            buffer = null;
            if (!audio_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            if (!audio_api.try_path(path, out var text, out status)) {
                return false;
            }
            using (text) {
                ulong value = 0;
                status = audio_api.table.load_buffer(text.view, &value);
                if (status != script_status.success || value == 0) {
                    if (status == script_status.success) {
                        status = script_status.asset_invalid;
                    }
                    return false;
                }
                buffer = new audio_buffer(value);
                return true;
            }
        }
        public bool release(out script_status status) {
            if (handle == 0 || !audio_api.is_current(epoch)) {
                Interlocked.Exchange(ref value, 0);
                status = script_status.success;
                return true;
            }
            if (!audio_api.is_ready()) {
                status = script_status.module_unavailable;
                return false;
            }
            status = release_native(handle);
            if (status == script_status.success || status == script_status.asset_invalid) {
                Interlocked.Exchange(ref value, 0);
            }
            return status == script_status.success;
        }
        private static unsafe script_status release_native(ulong value) => audio_api.table.release_buffer(value);
        public void Dispose() {
            release(out _);
        }
    }
}
