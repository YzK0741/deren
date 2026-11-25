using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace deren.editor {
    /// <summary>Reads complete UTF-8 lines from the Play child's log without rereading old output.</summary>
    internal sealed class play_log_reader {
        private readonly byte[] buffer = new byte[64 * 1024];
        private long offset;
        private long last_read;
        private bool native_error;
        private bool discard_long_line;

        internal void read(string path, bool finished, Action<Guid, string, bool> receive) {
            long now = Stopwatch.GetTimestamp();
            if (!finished && Stopwatch.GetElapsedTime(last_read, now).TotalMilliseconds < 200) { return; }
            last_read = now;
            try {
                using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                if (file.Length < offset) { offset = 0; }
                file.Position = offset;
                for (int batch = 0; batch < (finished ? 256 : 8); ++batch) {
                    int count = file.Read(buffer);
                    if (count == 0) { break; }
                    int consumed = 0;
                    for (int index = 0; index < count; ++index) {
                        if (buffer[index] != (byte)'\n') { continue; }
                        if (!discard_long_line) {
                            parse(Encoding.UTF8.GetString(buffer, consumed, index - consumed).TrimEnd('\r'), receive);
                        }
                        consumed = index + 1;
                        discard_long_line = false;
                    }
                    if (consumed == 0 && count == buffer.Length) {
                        discard_long_line = true;
                        consumed = count;
                    }
                    offset += consumed;
                    if (consumed == 0 || count != buffer.Length) { break; }
                    file.Position = offset;
                }
            } catch (IOException) {
                // The renderer may still be starting, or rotating its log between frames.
            } catch (UnauthorizedAccessException) { }
        }

        private void parse(string line, Action<Guid, string, bool> receive) {
            if (line.StartsWith('[')) {
                native_error = line.Contains("[ERROR]", StringComparison.Ordinal) ||
                    line.Contains("[WARNING]", StringComparison.Ordinal) || line.Contains("[FATAL]", StringComparison.Ordinal);
                return;
            }
            const string scripts = "scripts: ", target = "Editor play node ";
            if (line.StartsWith(scripts, StringComparison.Ordinal)) {
                string message = line[scripts.Length..];
                if (message.StartsWith(target, StringComparison.Ordinal) && message.Length >= target.Length + 34 &&
                    Guid.TryParseExact(message.AsSpan(target.Length, 32), "N", out var node)) {
                    receive(node, message[(target.Length + 34)..], true);
                } else {
                    receive(Guid.Empty, message, native_error);
                }
            } else if (native_error && line.Length != 0) {
                receive(Guid.Empty, line, true);
            }
            native_error = false;
        }
    }

    internal sealed class output_log_panel {
        private readonly record struct log_line(int id, Guid node, string text, bool error);
        private readonly Queue<log_line> lines = new();
        private int next_id;
        private bool dirty = true;

        internal void add(Guid node, string text, bool error) {
            if (lines.Count == 200) { lines.Dequeue(); }
            lines.Enqueue(new(next_id++, node, text.Length > 1600 ? text[..1600] : text, error));
            dirty = true;
        }

        /// <summary>The number of lines kept; the oldest is index 0 and the queue drops past 200.</summary>
        internal int count => lines.Count;

        internal string text_at(int index) {
            int walked = 0;
            foreach (var line in lines) {
                if (walked++ == index) { return line.text; }
            }
            return string.Empty;
        }

        internal bool error_at(int index) {
            int walked = 0;
            foreach (var line in lines) {
                if (walked++ == index) { return line.error; }
            }
            return false;
        }

        internal Guid node_at(int index) {
            int walked = 0;
            foreach (var line in lines) {
                if (walked++ == index) { return line.node; }
            }
            return Guid.Empty;
        }

        internal void clear() {
            lines.Clear();
            dirty = true;
        }

        internal Guid node_for(string id) {
            foreach (var line in lines) {
                if (id == $"output_{line.id}") { return line.node; }
            }
            return Guid.Empty;
        }

        internal void refresh(rml_document ui) {
            if (!dirty) { return; }
            dirty = false;
            var markup = new StringBuilder();
            var snapshot = lines.ToArray();
            for (int index = snapshot.Length - 1; index >= 0; --index) {
                var line = snapshot[index];
                string style = line.node != Guid.Empty ? "output_message output_link" :
                    line.error ? "output_message output_error" : "output_message";
                markup.Append($"""<div id="output_{line.id}" class="{style}"></div>""");
            }
            if (ui.try_set_markup("output_lines", markup.ToString())) {
                foreach (var line in snapshot) { ui.set_text($"output_{line.id}", line.text); }
            }
            ui.set_visible("output_empty", snapshot.Length == 0);
        }
    }
}
