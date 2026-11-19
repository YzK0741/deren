using System;
using System.IO;
using System.Runtime.InteropServices;
using deren.scripting;

namespace deren {
    internal static unsafe class audio_selftest {
        private static audio? original;
        private static audio? copy;
        private static bool stopped;
        private static bool missing;
        internal static string? path;
        internal static script_status initialize(managed_api* scripts) {
            missing = path == "missing";
            stopped = false;
            scripts->update = &update;
            scripts->stop = &stop;
            try {
                if (missing) {
                    return !audio.try_create_from_file("no-file.opus", out _, out var absent) &&
                        absent == script_status.module_unavailable ? script_status.success : script_status.script_failure;
                }
                if (path == null) {
                    return script_status.invalid_argument;
                }
                var unicode = Path.Combine(Environment.CurrentDirectory, "deren_audio_" + Guid.NewGuid().ToString("N") + "_\u97f3\u4e50.opus");
                try {
                    File.Copy(path, unicode);
                    if (!audio_buffer.try_load_from_file(unicode, out var buffer) || buffer == null) {
                        return script_status.script_failure;
                    }
                    using (buffer) {
                        if (buffer.channels != 2 || buffer.sample_rate != 48000 || buffer.frame_count == 0) {
                            return script_status.script_failure;
                        }
                        original = audio.create(buffer);
                    }
                    if (original == null || !original.seek(0.025) || !original.try_set_volume(0.4f) ||
                        !original.try_set_looping(true) || !original.try_set_pitch(1.1f) || !original.try_set_pan(-0.3f)) {
                        return script_status.script_failure;
                    }
                    using var fresh = original.clone();
                    copy = original.clone(true);
                    if (fresh == null || copy == null || fresh.position != 0 || fresh.is_playing ||
                        fresh.looping || Math.Abs(copy.position - 0.025) > 0.0001 || copy.volume != 0.4f ||
                        !copy.looping || copy.is_playing || copy.pitch != 1.1f || copy.pan != -0.3f) {
                        return script_status.script_failure;
                    }
                    if (original.seek(-1) || original.try_set_volume(float.NaN)) {
                        return script_status.script_failure;
                    }
                    fresh.Dispose();
                    fresh.Dispose();
                    if (fresh.play(out var disposed) || disposed != script_status.asset_invalid) {
                        return script_status.script_failure;
                    }
                    using var from_file = audio.create_from_file(unicode);
                    return from_file != null && original.play() ? script_status.success : script_status.script_failure;
                } finally {
                    File.Delete(unicode);
                }
            } catch (Exception error) {
                managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes($"Audio self-test: {error}"));
                return script_status.script_failure;
            }
        }
        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (missing) {
                return script_status.success;
            }
            if (original == null || copy == null) {
                return script_status.script_failure;
            }
            if (stopped) {
                return !original.is_valid && !original.play(out var status) && status == script_status.module_unavailable
                    ? script_status.success : script_status.script_failure;
            }
            return original.position > 0.025 && Math.Abs(copy.position - 0.025) < 0.0001
                ? script_status.success : script_status.script_failure;
        }
        [UnmanagedCallersOnly]
        private static script_status stop() {
            // Leave two live managed wrappers to prove shutdown invalidates them and native cleanup owns the ids.
            stopped = true;
            return script_status.success;
        }
    }
}
