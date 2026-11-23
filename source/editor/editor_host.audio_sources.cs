using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using deren;
using deren.scripting;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private readonly Dictionary<string, string> audio_fields = new(StringComparer.Ordinal);
        private Guid selected_audio_uid;
        private audio_source_component? selected_audio_source;
        private audio? audio_preview;

        private void refresh_audio_source_details(document_node? node) {
            var source = node?.audio_source;
            var uid = source.HasValue ? node!.uid : Guid.Empty;
            if (uid == selected_audio_uid && source == selected_audio_source) { return; }
            stop_audio_preview();
            selected_audio_uid = uid;
            selected_audio_source = source;
            audio_fields.Clear();
            ui?.try_set_element_style("audio_source_details", "display", source.HasValue ? "block" : "none");
            if (source is not { } value) { return; }
            audio_fields["audio_file"] = value.file;
            audio_fields["audio_volume"] = project_look.number(value.volume);
            audio_fields["audio_loop"] = value.loop ? "on" : "off";
            refresh_audio_files();
            ui?.set_value("audio_volume", audio_fields["audio_volume"]);
            ui?.set_value("audio_loop", audio_fields["audio_loop"]);
        }

        private void refresh_audio_files() {
            var project = managed_bootstrap.project_directory;
            var root = Path.Combine(project, "audio");
            var selected = audio_fields["audio_file"];
            var markup = new StringBuilder("""<option value="">(no clip)</option>""");
            bool listed = selected.Length == 0;
            try {
                if (Directory.Exists(root)) {
                    var entries = Directory.EnumerateFiles(root, "*", new EnumerationOptions {
                        RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint,
                    }).Select(file => Path.GetRelativePath(project, file).Replace(Path.DirectorySeparatorChar, '/'))
                      .Where(audio_source_component.valid_reference).Order(StringComparer.OrdinalIgnoreCase);
                    foreach (var file in entries) {
                        listed |= file == selected;
                        markup.Append($"""<option value="{WebUtility.HtmlEncode(file)}">{WebUtility.HtmlEncode(file["audio/".Length..])}</option>""");
                    }
                }
            } catch (Exception error) when (error is IOException or UnauthorizedAccessException) {
                host_logger.write($"Editor: could not list audio files: {error.Message}");
            }
            if (!listed) {
                var missing = WebUtility.HtmlEncode(selected);
                markup.Append($"""<option value="{missing}">{missing} (missing)</option>""");
            }
            ui?.try_set_markup("audio_file", markup.ToString());
            ui?.set_value("audio_file", selected);
        }

        private void handle_audio_source(string action) {
            if (action == "audio_stop") {
                stop_audio_preview();
                return;
            }
            if (document?.find(selection_state?.primary ?? Guid.Empty) is not { audio_source: { } before } node ||
                audio_fields.Count != 3) { return; }
            if (action == "audio_refresh") {
                refresh_audio_files();
                return;
            }
            string file = audio_fields["audio_file"];
            if (!project_look.try_number(audio_fields["audio_volume"], 0, 1, out var volume) ||
                (file.Length != 0 && !audio_source_component.valid_reference(file))) {
                notice = "Select a project audio clip and use a volume from 0 to 1";
                return;
            }
            var after = new audio_source_component(file, volume, audio_fields["audio_loop"] == "on");
            if (action == "audio_preview") {
                stop_audio_preview();
                if (file.Length == 0) {
                    notice = "Select an audio clip to preview";
                    return;
                }
                if (!after.try_create(managed_bootstrap.project_directory, out audio_preview, out var status) ||
                    audio_preview == null || !audio_preview.play(out status)) {
                    notice = $"Could not preview '{file}' ({status})";
                    stop_audio_preview();
                    return;
                }
                notice = $"Previewing {Path.GetFileName(file)}";
                return;
            }
            if (before == after) { return; }
            bool assign(audio_source_component value) {
                node.audio_source = value;
                mark_dirty_and_refresh();
                return true;
            }
            assign(after);
            record_undo("Edit Audio Source", () => assign(before), () => assign(after));
            update_history_buttons();
            notice = "Audio source updated";
        }

        private void stop_audio_preview() {
            audio_preview?.Dispose();
            audio_preview = null;
        }
    }
}
