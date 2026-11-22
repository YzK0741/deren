using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.Json;
using deren;
using deren.scripting;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private enum project_panel { none, scenes, settings }
        private static bool requested_project_selftest;
        private project_panel open_project_panel;
        private string[] project_scenes = [];
        private string? selected_project_scene;
        private string? startup_project_scene;
        private readonly Dictionary<string, string> look_fields = new(StringComparer.Ordinal);
        private string? look_path;
        private string? look_source;
        private render_settings? preview_render;
        private lighting_settings? preview_lighting;
        private project_look saved_look;
        private bool look_previewed;

        private string project_manifest_path => Path.Combine(managed_bootstrap.project_directory, "deren.toml");

        private void show_project_scenes() {
            var root = managed_bootstrap.project_directory;
            try {
                var scenes = Path.Combine(root, "scenes");
                project_scenes = Directory.Exists(scenes)
                    ? Directory.EnumerateFiles(scenes, "*.scene.toml", new EnumerationOptions {
                          RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint
                      })
                      .Select(path => Path.GetRelativePath(root, path).Replace('\\', '/'))
                      .Order(StringComparer.OrdinalIgnoreCase).ToArray()
                    : [];
                var manifest = toml_table.parse(File.ReadAllText(project_manifest_path), out _);
                startup_project_scene = manifest?.table("game")?.text("scene")?.Replace('\\', '/');
                var current = document?.path is { } path ? Path.GetRelativePath(root, path).Replace('\\', '/') : null;
                selected_project_scene = Array.Find(project_scenes, item => item == current) ??
                    Array.Find(project_scenes, item => item == startup_project_scene) ?? project_scenes.FirstOrDefault();
                render_project_scenes();
                ui?.set_text("project_scene_message", project_scenes.Length == 0 ? "Save a scene in scenes/ to list it here." : string.Empty);
                open_project_panel = project_panel.scenes;
                ui?.try_set_element_style("project_scenes_prompt", "display", "flex");
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or ArgumentException) {
                notice = $"Could not list project scenes: {failure.Message}";
            }
        }

        private void render_project_scenes() {
            var markup = new StringBuilder();
            for (int index = 0; index < project_scenes.Length; ++index) {
                var path = project_scenes[index];
                var selected = path == selected_project_scene ? " project_scene_selected" : string.Empty;
                var startup = path == startup_project_scene ? " · Startup" : "";
                markup.Append($"""<button id="project_scene_{index}" class="project_scene_row{selected}">{WebUtility.HtmlEncode(path + startup)}</button>""");
            }
            ui?.try_set_markup("project_scenes_list", markup.ToString());
        }

        private void handle_project_scenes(rml_event input) {
            if (input.type != rml_event_type.click) { return; }
            const string row = "project_scene_";
            if (input.id.StartsWith(row, StringComparison.Ordinal) &&
                int.TryParse(input.id.AsSpan(row.Length), out int index) && index >= 0 && index < project_scenes.Length) {
                selected_project_scene = project_scenes[index];
                render_project_scenes();
                ui?.set_text("project_scene_message", string.Empty);
                return;
            }
            if (input.id == "project_scene_close") { close_project_panel(); return; }
            if (selected_project_scene is not { } path) { return; }
            var full_path = Path.Combine(managed_bootstrap.project_directory, path);
            if (input.id is not "project_scene_open" and not "project_scene_startup") { return; }
            if (!document_io.try_load(full_path, out var loaded, out var error) || loaded == null) {
                ui?.set_text("project_scene_message", error ?? "Could not read the scene.");
                return;
            }
            if (input.id == "project_scene_open") {
                close_project_panel();
                request_open_scene(loaded);
            } else if (set_startup_scene(path, out var message)) {
                startup_project_scene = path;
                render_project_scenes();
                ui?.set_text("project_scene_message", "Startup scene saved.");
            } else {
                ui?.set_text("project_scene_message", message);
            }
        }

        private bool set_startup_scene(string path, out string error) {
            error = string.Empty;
            try {
                var filename = project_manifest_path;
                var original = File.ReadAllText(filename);
                return project_settings_file.try_write(filename, original,
                    new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal) {
                        ["game"] = new(StringComparer.Ordinal) { ["scene"] = JsonSerializer.Serialize(path) }
                    }, out error);
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                error = failure.Message;
                return false;
            }
        }

        private void show_project_settings() {
            try {
                var root = managed_bootstrap.project_directory;
                var manifest = toml_table.parse(File.ReadAllText(project_manifest_path), out _);
                var configured = manifest?.table("project")?.text("main_scene");
                if (string.IsNullOrWhiteSpace(configured)) {
                    notice = "The project has no renderer settings file.";
                    return;
                }
                look_path = Path.GetFullPath(configured, root);
                look_source = File.ReadAllText(look_path);
                var parsed = toml_table.parse(look_source, out _) ?? throw new IOException("Cannot read renderer settings.");
                preview_render = render_settings.from_document(parsed);
                preview_lighting = lighting_settings.from_document(parsed, Path.GetDirectoryName(look_path)!);
                saved_look = project_look.read(preview_render, preview_lighting);
                look_previewed = false;
                set_look_field("look_exposure", project_look.number(saved_look.exposure));
                set_look_field("look_sun", project_look.number(saved_look.sun_intensity));
                set_look_field("look_sun_dir", project_look.vector(saved_look.sun_direction));
                set_look_field("look_background", saved_look.background.ToString(CultureInfo.InvariantCulture));
                set_look_field("look_color", project_look.vector(saved_look.background_color));
                set_look_field("look_fxaa", saved_look.fxaa ? "on" : "off");
                set_look_field("look_taa", saved_look.taa ? "on" : "off");
                ui?.set_text("project_settings_file", Path.GetRelativePath(root, look_path));
                ui?.set_text("project_settings_message", string.Empty);
                open_project_panel = project_panel.settings;
                ui?.try_set_element_style("project_settings_prompt", "display", "flex");
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or ArgumentException) {
                notice = $"Could not open project settings: {failure.Message}";
            }
        }

        private void set_look_field(string id, string value) {
            look_fields[id] = value;
            ui?.set_value(id, value);
        }

        private bool try_read_look(out project_look look, out string error) {
            look = default;
            error = string.Empty;
            if (!project_look.try_number(look_fields["look_exposure"], 0.05f, 20, out var exposure)) {
                error = "Exposure must be between 0.05 and 20.";
                return false;
            }
            if (!project_look.try_number(look_fields["look_sun"], 0, 3, out var intensity)) {
                error = "Sun intensity must be between 0 and 3.";
                return false;
            }
            if (!project_look.try_vector(look_fields["look_sun_dir"], -1000, 1000, out var direction) ||
                direction.x * direction.x + direction.y * direction.y + direction.z * direction.z <= 1e-12f) {
                error = "Enter a non-zero sun direction as X, Y, Z.";
                return false;
            }
            if (!int.TryParse(look_fields["look_background"], out var background) || background is < 0 or > 3 ||
                !project_look.try_vector(look_fields["look_color"], 0, 1, out var color)) {
                error = "Enter a background color as R, G, B, with each channel from 0 to 1.";
                return false;
            }
            look = new project_look(exposure, intensity, direction, background, color,
                                   look_fields["look_fxaa"] == "on", look_fields["look_taa"] == "on");
            return true;
        }

        private void handle_project_settings(rml_event input) {
            if (input.type == rml_event_type.change && look_fields.ContainsKey(input.id)) {
                look_fields[input.id] = input.value;
                return;
            }
            if (input.type != rml_event_type.click) { return; }
            if (input.id == "project_settings_cancel") {
                if (look_previewed && preview_render != null && preview_lighting != null) {
                    saved_look.apply(preview_render, preview_lighting);
                }
                close_project_panel();
                return;
            }
            if (input.id is not "project_settings_preview" and not "project_settings_save" ||
                preview_render == null || preview_lighting == null || look_path == null || look_source == null) { return; }
            if (!try_read_look(out var look, out var error)) {
                ui?.set_text("project_settings_message", error);
                return;
            }

            look_previewed = true;
            if (!look.apply(preview_render, preview_lighting)) {
                ui?.set_text("project_settings_message", "The renderer could not apply these settings.");
                return;
            }
            if (input.id == "project_settings_preview") {
                ui?.set_text("project_settings_message", "Preview applied.");
                return;
            }
            if (!project_settings_file.try_write(look_path, look_source, look.changes_from(saved_look), out error)) {
                ui?.set_text("project_settings_message", error);
                return;
            }
            close_project_panel();
            notice = "Project settings saved";
        }

        private void close_project_panel() {
            ui?.try_set_element_style("project_scenes_prompt", "display", "none");
            ui?.try_set_element_style("project_settings_prompt", "display", "none");
            open_project_panel = project_panel.none;
        }
    }
}
