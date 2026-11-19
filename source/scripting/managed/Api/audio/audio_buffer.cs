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
        public uint channels => (uint)query(0);
        public uint sample_rate => (uint)query(1);
        public ulong frame_count => (ulong)query(2);
        public double duration => query(3);

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
        private unsafe double query(uint field) {
            if (!is_valid || !audio_api.is_ready()) {
                return 0;
            }
            double result = 0;
            return audio_api.table.buffer_query(handle, field, &result) == script_status.success ? result : 0;
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
