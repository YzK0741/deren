using System;
using System.Globalization;
using System.IO;
using deren;
using deren.scripting;

namespace game {
    internal sealed class demo_audio_panel : IDisposable {
        private readonly rml_document document;
        private audio? player;
        private string path = string.Empty;
        private float volume = 0.7f;
        private double refresh_time;
        private string last_time = string.Empty;
        private string last_position = string.Empty;

        private demo_audio_panel(rml_document document) { this.document = document; }
        internal static demo_audio_panel? create(string? initial_path) {
            if (!rml_document.is_available) { return null; }
            var root = Path.Combine(managed_bootstrap.project_directory, "ui");
            if (!rml_document.try_open(Path.Combine(root, "audio.rml"), Path.Combine(root, "LatoLatin-Regular.ttf"),
                                       out var document, out var status) || document == null) {
                host_logger.write($"Audio panel: could not open document ({status}).");
                return null;
            }
            var result = new demo_audio_panel(document);
            if (!string.IsNullOrEmpty(initial_path)) {
                result.path = initial_path;
                document.set_value("path", initial_path);
                result.load();
            }
            return result;
        }
        private void load() {
            if (!audio.try_create_from_file(path, out var loaded, out var status) || loaded == null) {
                document.set_text("status", $"Cannot load file: {status}");
                return;
            }
            player?.Dispose();
            player = loaded;
            player.try_set_volume(volume);
            document.set_text("status", "Ready. Press Play.");
            last_time = string.Empty;
            last_position = string.Empty;
        }
        internal void update(float delta_time) {
            // A bound keeps one busy document from monopolizing a game update.
            for (var count = 0; count < 128 && document.try_poll_event(out var input); ++count) {
                if (input.type == rml_event_type.change) {
                    if (input.id == "path") {
                        path = input.value;
                    } else if (double.TryParse(input.value, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) &&
                               double.IsFinite(number)) {
                        if (input.id == "volume") {
                            volume = (float)Math.Clamp(number / 100, 0, 1);
                            player?.try_set_volume(volume);
                        } else if (input.id == "position" && player != null) {
                            player.seek(Math.Clamp(number / 1000, 0, 1) * player.duration);
                        }
                    }
                } else if (input.type == rml_event_type.click) {
                    switch (input.id) {
                        case "load": load(); break;
                        case "play":
                            if (player != null) { report(player.play(out var status), status, "Playing"); }
                            break;
                        case "pause":
                            if (player != null) { report(player.pause(out var status), status, "Paused"); }
                            break;
                        case "stop":
                            if (player != null) { report(player.stop(out var status), status, "Stopped"); }
                            break;
                    }
                }
            }
            refresh_time += delta_time;
            if (refresh_time < 0.1 || player == null) { return; }
            refresh_time = 0;
            var position = player.position;
            var duration = player.duration;
            var text = $"{format_time(position)} / {format_time(duration)}";
            if (text != last_time) {
                document.set_text("time", text);
                last_time = text;
            }
            var progress = (duration > 0 ? Math.Clamp(position / duration, 0, 1) * 1000 : 0)
                .ToString("F0", CultureInfo.InvariantCulture);
            if (progress != last_position) {
                document.set_value("position", progress);
                last_position = progress;
            }
            if (player.at_end) { document.set_text("status", "Finished"); }
        }
        private void report(bool success, script_status status, string text) =>
            document.set_text("status", success ? text : $"Audio: {status}");
        private static string format_time(double seconds) {
            var whole = (long)Math.Max(0, seconds);
            return $"{whole / 60}:{whole % 60:00}";
        }
        public void Dispose() {
            player?.Dispose();
            player = null;
            document.Dispose();
        }
    }
}
