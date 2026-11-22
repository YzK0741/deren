using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.Json;
using deren;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// The environment half of the editor: the scene-wide HDR kept in the project settings and the
    /// fifteen custom lanes the scene document stores.
    /// </summary>
    /// <remarks>
    /// The engine has one scene-wide IBL slot and fifteen custom ones; a material samples one of them
    /// through the material override's lane index. Lane 0 is the project-wide map, so it lives in the
    /// project's <c>[lighting] environment_hdr</c> where the game reads it too; lanes 1..15 belong to
    /// the scene and travel with the document. Paths are stored as authored and resolved against the
    /// project when they are relative, because a document has to stay portable.
    /// </remarks>
    internal sealed unsafe partial class editor_host {
        /// <summary>
        /// The scene-wide HDR the project settings name, or an empty string when they name none.
        /// </summary>
        private string read_project_environment_hdr() {
            try {
                if (!try_read_project_look(out _, out var source, out _)) {
                    return string.Empty;
                }
                var parsed = toml_table.parse(source, out _);
                return parsed?.table("lighting")?.text("environment_hdr") ?? string.Empty;
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or ArgumentException) {
                host_logger.write($"Editor: cannot read the project environment: {failure.Message}");
                return string.Empty;
            }
        }

        /// <summary>
        /// Writes the scene-wide HDR into the project settings file, keeping every other key as it was.
        /// </summary>
        private bool try_write_project_environment_hdr(string text, out string error) {
            error = string.Empty;
            try {
                if (!try_read_project_look(out var path, out var source, out error)) {
                    return false;
                }
                var edits = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal) {
                    ["lighting"] = new(StringComparer.Ordinal) {
                        ["environment_hdr"] = JsonSerializer.Serialize(text),
                    },
                };
                return project_settings_file.try_write(path, source, edits, out error);
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or ArgumentException) {
                error = failure.Message;
                return false;
            }
        }

        /// <summary>
        /// Finds the renderer settings file the project manifest points at and returns its text.
        /// </summary>
        private bool try_read_project_look(out string path, out string source, out string error) {
            path = string.Empty;
            source = string.Empty;
            error = string.Empty;
            if (!File.Exists(project_manifest_path)) {
                error = "the project has no deren.toml";
                return false;
            }
            var manifest = toml_table.parse(File.ReadAllText(project_manifest_path), out _);
            var configured = manifest?.table("project")?.text("main_scene");
            if (string.IsNullOrWhiteSpace(configured)) {
                error = "the project does not name a main_scene";
                return false;
            }
            path = Path.GetFullPath(configured, managed_bootstrap.project_directory);
            if (!File.Exists(path)) {
                error = $"the renderer settings file '{configured}' is missing";
                return false;
            }
            source = File.ReadAllText(path);
            return true;
        }

        /// <summary>
        /// Resolves an authored environment path: a relative one is looked up in the project first and
        /// handed over untouched when the project does not have it, which leaves the host resolving it
        /// against the executable directory the way a game run does.
        /// </summary>
        private static string resolve_environment_path(string text) {
            if (text.Length == 0 || Path.IsPathRooted(text)) {
                return text;
            }
            var project_root = managed_bootstrap.project_directory;
            if (string.IsNullOrEmpty(project_root)) {
                return text;
            }
            try {
                var candidate = Path.GetFullPath(text, project_root);
                return File.Exists(candidate) ? candidate : text;
            } catch (Exception failure) when (failure is ArgumentException or NotSupportedException or PathTooLongException) {
                return text;
            }
        }

        /// <summary>
        /// Applies one change event of the environment panel: lane 0 edits the project settings, lanes
        /// 1..15 edit the document and are undoable. The selected value is either a `@sky`/`@choose`
        /// sentinel or the map text the dropdown entry stands for.
        /// </summary>
        private void apply_environment_selection(int lane, string selection) {
            if (selection == environment_panel.choose_value) {
                if (!try_pick_environment(out var picked)) {
                    // Cancelled or unavailable: the dropdown has already moved to this entry, so put it
                    // back to what the lane actually holds.
                    environment_ui?.invalidate(lane);
                    refresh_panels(force_details: true);
                    return;
                }
                apply_environment_text(lane, picked);
                return;
            }
            if (selection == environment_panel.sky_value && lane != 0) {
                clear_environment_lane(lane);
                return;
            }
            apply_environment_text(lane, selection == environment_panel.sky_value ? string.Empty : selection);
        }

        /// <summary>
        /// Empties one lane: its map becomes the procedural sky again and every object that sampled it
        /// returns to the scene-wide map. Both halves are one undo command, because a node left pointing at
        /// an empty lane would sample an undefined descriptor and render unlit.
        /// </summary>
        private void clear_environment_lane(int lane) {
            if (document == null || lane < 1 || lane >= global_environment.slot_count) {
                return;
            }
            var before_lane = document.environment_slots[lane - 1];
            var before_nodes = new List<(document_node node, node_snapshot snapshot)>();
            foreach (var node in nodes_on_lane(lane)) {
                before_nodes.Add((node, new node_snapshot(node)));
            }
            if (before_lane == null && before_nodes.Count == 0) {
                environment_ui?.invalidate(lane);
                refresh_panels(force_details: true);
                return;
            }
            clear();
            record_undo($"Clear Environment Lane {lane}", restore, clear);
            environment_ui?.invalidate(lane);
            environment_ui?.invalidate_node();
            refresh_panels(force_details: true);
            return;

            bool clear() {
                document.environment_slots[lane - 1] = null;
                applied_environments[lane] = null;
                failed_environments[lane] = null;
                foreach (var (node, _) in before_nodes) {
                    node.environment = 0;
                    if (!push_runtime(node)) {
                        // The lane is being emptied, so putting the node back on it would leave it
                        // sampling a descriptor nobody sets. The document keeps lane 0 and the undo
                        // command restores the previous lane pointer.
                        host_logger.write($"Editor: cannot move '{node.name}' back to the scene map");
                    }
                }
                document.mark_dirty();
                refresh_panels(force_details: true);
                sync_environment_lanes();
                return true;
            }

            bool restore() {
                document.environment_slots[lane - 1] = before_lane;
                applied_environments[lane] = null;
                failed_environments[lane] = null;
                foreach (var (node, snapshot) in before_nodes) {
                    restore_node(node, snapshot, mark_dirty: false);
                }
                document.mark_dirty();
                refresh_panels(force_details: true);
                sync_environment_lanes();
                return true;
            }
        }

        /// <summary>Every node of the document that samples @p lane, in document order.</summary>
        private List<document_node> nodes_on_lane(int lane) {
            var nodes = new List<document_node>();
            if (document != null) {
                foreach (var root in document.roots) {
                    collect(root);
                }
            }
            return nodes;

            void collect(document_node node) {
                if (node.environment == lane) {
                    nodes.Add(node);
                }
                foreach (var child in node.children) {
                    collect(child);
                }
            }
        }

        /// <summary>
        /// Asks the platform for an environment map and returns it as the document should store it.
        /// </summary>
        private bool try_pick_environment(out string text) {
            text = string.Empty;
            if (!asset_authoring.try_open(out var authoring)) {
                notice = "Environment picker unavailable";
                return false;
            }
            if (!authoring.has_environment_picker) {
                host_logger.write("Editor: the host does not publish the environment picker (module 8)");
                notice = "This build cannot open the file picker";
                return false;
            }
            if (authoring.pick_environment() is not { Length: > 0 } picked) {
                return false;
            }
            text = authored_environment_path(picked);
            return true;
        }

        /// <summary>
        /// Turns an absolute path into the text a document stores: relative to the project when it can be,
        /// so the scene stays portable, and absolute otherwise.
        /// </summary>
        private static string authored_environment_path(string absolute) {
            var project_root = managed_bootstrap.project_directory;
            if (!string.IsNullOrEmpty(project_root)) {
                try {
                    var full = Path.GetFullPath(absolute);
                    var relative = Path.GetRelativePath(Path.GetFullPath(project_root), full);
                    if (relative.Length > 0 && !Path.IsPathRooted(relative)) {
                        return relative.Replace('\\', '/');
                    }
                } catch (Exception failure) when (failure is ArgumentException or NotSupportedException or PathTooLongException) {
                    // fall through to the absolute spelling
                }
            }
            return absolute;
        }

        /// <summary>
        /// The environment maps the dropdown offers: the asset library's `environments` folder, in name
        /// order. Cached because the panel refreshes every frame; <see cref="invalidate_environment_catalog"/>
        /// drops it when the library is rescanned.
        /// </summary>
        private List<environment_entry> environment_catalog() {
            if (environment_entries != null) {
                return environment_entries;
            }
            var entries = new List<environment_entry>();
            if (!asset_authoring.try_open(out var authoring) || authoring.library_root() is not { Length: > 0 } root) {
                environment_entries = entries;
                return entries;
            }
            try {
                var directory = Path.Combine(Path.GetFullPath(root), "environments");
                if (!Directory.Exists(directory)) {
                    environment_entries = entries;
                    return entries;
                }
                var files = Directory.GetFiles(directory, "*", SearchOption.TopDirectoryOnly);
                var maps = new List<string>();
                foreach (var file in files) {
                    if (string.Equals(Path.GetExtension(file), ".hdr", StringComparison.OrdinalIgnoreCase)) {
                        maps.Add(file);
                    }
                }
                maps.Sort(StringComparer.OrdinalIgnoreCase);
                foreach (var file in maps) {
                    entries.Add(new environment_entry(authored_environment_path(file), Path.GetFileName(file)));
                }
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or ArgumentException) {
                host_logger.write($"Editor: cannot list the environment maps: {failure.Message}");
            }
            environment_entries = entries;
            return entries;
        }

        private void invalidate_environment_catalog() {
            environment_entries = null;
        }

        /// <summary>
        /// Applies one change event of the environment panel: lane 0 edits the project settings, lanes
        /// 1..15 edit the document and are undoable.
        /// </summary>
        private void apply_environment_text(int lane, string text) {
            if (document == null) {
                return;
            }
            var trimmed = text.Trim();
            if (lane == 0) {
                apply_global_environment(trimmed, persist: true);
                return;
            }
            if (lane < 1 || lane >= global_environment.slot_count) {
                return;
            }
            if (trimmed.Length == 0) {
                // An empty lane is not a map: it hands the lane back, and the objects that sampled it have
                // to return to the scene map in the same step.
                clear_environment_lane(lane);
                return;
            }
            var before = document.environment_slots[lane - 1];
            if (string.Equals(before ?? string.Empty, trimmed, StringComparison.Ordinal)) {
                return;
            }
            document.environment_slots[lane - 1] = trimmed;
            var after = trimmed;
            document.mark_dirty();
            record_undo($"Set Environment Lane {lane}",
                () => restore_environment_lane(lane, before),
                () => restore_environment_lane(lane, after));
            refresh_panels(force_details: true);
            sync_environment_lanes();
        }

        /// <summary>Puts one lane back to the map the undo command captured.</summary>
        private bool restore_environment_lane(int lane, string? text) {
            if (document == null || lane < 1 || lane >= global_environment.slot_count) {
                return false;
            }
            if (string.Equals(document.environment_slots[lane - 1], text, StringComparison.Ordinal)) {
                return true;
            }
            document.environment_slots[lane - 1] = text;
            // Unknown, not "the same as before": the next sync must hand the map over again.
            applied_environments[lane] = null;
            failed_environments[lane] = null;
            document.mark_dirty();
            refresh_panels(force_details: true);
            sync_environment_lanes();
            return true;
        }

        /// <summary>
        /// Hands every lane whose stored map differs from the renderer's over to the renderer. It runs
        /// once per frame but only calls out when a lane actually moved.
        /// </summary>
        /// <remarks>
        /// A lane is only recorded as applied once the renderer accepted it: a map that is missing when
        /// the scene loads (a slow disk, an asset that arrives a frame later) must be retried on the next
        /// frame instead of being remembered as done. The failure is logged once per distinct text so a
        /// permanently bad path does not fill the log every frame.
        /// </remarks>
        private void sync_environment_lanes() {
            if (document == null) {
                return;
            }
            for (var lane = 1; lane < global_environment.slot_count; ++lane) {
                var text = document.environment_slots[lane - 1] ?? string.Empty;
                if (string.Equals(applied_environments[lane], text, StringComparison.Ordinal)) {
                    continue;
                }
                var applied = global_environment.try_set_slot_hdr(lane, resolve_environment_path(text), out var status);
                if (!applied) {
                    applied_environments[lane] = null;
                    if (!string.Equals(failed_environments[lane], text, StringComparison.Ordinal)) {
                        failed_environments[lane] = text;
                        host_logger.write($"Editor: cannot set environment lane {lane} '{text}' ({status})");
                    }
                    environment_ui?.set_status($"Lane {lane}: {status}");
                    continue;
                }
                failed_environments[lane] = null;
                applied_environments[lane] = text;
                environment_ui?.set_status(text.Length == 0
                    ? $"Lane {lane}: procedural sky"
                    : string.Create(CultureInfo.InvariantCulture, $"Lane {lane}: {Path.GetFileName(text)}"));
            }
        }

        /// <summary>
        /// Applies the scene-wide map. Honest about the outcome: a path the renderer refuses leaves the
        /// previous map in place and the panel says why.
        /// </summary>
        private void apply_global_environment(string text, bool persist) {
            global_environment_hdr = text;
            var applied = global_environment.try_set_hdr(resolve_environment_path(text), out var status);
            if (!applied) {
                host_logger.write($"Editor: cannot set the scene environment '{text}' ({status})");
                environment_ui?.set_status($"Scene map: {status}");
                notice = $"Environment not applied: {status}";
            } else {
                environment_ui?.set_status(text.Length == 0
                    ? "Scene map: procedural sky"
                    : string.Create(CultureInfo.InvariantCulture, $"Scene map: {Path.GetFileName(text)}"));
            }
            if (persist && !try_write_project_environment_hdr(text, out var error)) {
                notice = "Environment not saved: " + error;
            }
            refresh_panels(force_details: true);
        }
        /// <summary>
        /// Handles the per-object environment dropdown. Returns true when the id belongs to it, so the
        /// caller can stop looking at other panels.
        /// </summary>
        private bool handle_node_environment_change(string? id, string value) {
            if (id != environment_panel.node_id) {
                return false;
            }
            if (document == null || selection_state == null) {
                return true;
            }
            var node = selection_state.primary == Guid.Empty ? null : document.find(selection_state.primary);
            if (node != null) {
                apply_node_environment_selection(node, value);
            }
            return true;
        }

        /// <summary>
        /// Applies one change of the per-object dropdown: the scene-wide map, a catalogue map (allocated to a
        /// free lane), or the platform picker followed by the same allocation.
        /// </summary>
        private void apply_node_environment_selection(document_node node, string selection) {
            if (selection == environment_panel.scene_value) {
                set_node_environment(node, 0);
                return;
            }
            if (selection == environment_panel.empty_lane_value) {
                // The row was showing a lane the document carries no map for; repaint it from the document.
                environment_ui?.invalidate_node();
                refresh_panels(force_details: true);
                return;
            }
            if (selection == environment_panel.choose_value) {
                if (!try_pick_environment(out var picked)) {
                    // Cancelled: the dropdown already shows the picker entry, so repaint it from the document.
                    environment_ui?.invalidate_node();
                    refresh_panels(force_details: true);
                    return;
                }
                assign_node_environment(node, picked);
                return;
            }
            assign_node_environment(node, selection);
        }

        /// <summary>
        /// Points the node's materials at the lane that carries @p text, allocating one when the map is new.
        /// A map another object already uses shares that lane instead of burning a second slot. When every
        /// lane is busy the request replaces a lane that no other object samples; if the only candidate is a
        /// lane other objects share, the notice says their sky moved too.
        /// </summary>
        private void assign_node_environment(document_node node, string text) {
            if (document == null) {
                return;
            }
            var trimmed = text.Trim();
            if (trimmed.Length == 0) {
                set_node_environment(node, 0);
                return;
            }
            var lane = lane_carrying(trimmed);
            if (lane == 0) {
                lane = free_lane();
            }
            if (lane == 0) {
                lane = lane_without_nodes();
            }
            if (lane == 0) {
                if (node.environment != 0) {
                    // Every lane is busy or shared. The selected object keeps the lane it already owns, so
                    // the request still does something useful; say plainly who else is affected.
                    lane = node.environment;
                    notice = $"Lane {lane} is shared; the other objects using it changed too";
                } else {
                    notice = "All 15 environment lanes are in use";
                    environment_ui?.invalidate_node();
                    refresh_panels(force_details: true);
                    return;
                }
            }
            if (lane == node.environment && string.Equals(document.environment_slots[lane - 1], trimmed, StringComparison.Ordinal)) {
                environment_ui?.invalidate_node();
                refresh_panels(force_details: true);
                return;
            }
            commit_node_environment(node, lane, trimmed);
        }

        /// <summary>
        /// Writes one map onto one lane and points the node at it as a single undo step: the two halves only
        /// make sense together, and recording them separately would leave a state where the lane is taken but
        /// no object samples it between two undo commands.
        /// </summary>
        private bool commit_node_environment(document_node node, int lane, string text) {
            if (document == null || lane < 1 || lane >= global_environment.slot_count) {
                return false;
            }
            var before_text = document.environment_slots[lane - 1];
            var before_node = new node_snapshot(node);
            document.environment_slots[lane - 1] = text;
            node.environment = lane;
            applied_environments[lane] = null;
            if (!push_runtime(node)) {
                document.environment_slots[lane - 1] = before_text;
                restore_node(node, before_node, mark_dirty: false);
                return false;
            }
            var after_node = new node_snapshot(node);
            document.mark_dirty();
            record_undo("Set Environment Lane", restore, apply);
            environment_ui?.invalidate(lane);
            environment_ui?.invalidate_node();
            refresh_panels(force_details: true);
            sync_environment_lanes();
            return true;

            bool restore() {
                document.environment_slots[lane - 1] = before_text;
                applied_environments[lane] = null;
                restore_node(node, before_node, mark_dirty: false);
                document.mark_dirty();
                refresh_panels(force_details: true);
                sync_environment_lanes();
                return true;
            }

            bool apply() {
                document.environment_slots[lane - 1] = text;
                applied_environments[lane] = null;
                restore_node(node, after_node, mark_dirty: false);
                document.mark_dirty();
                refresh_panels(force_details: true);
                sync_environment_lanes();
                return true;
            }
        }

        /// <summary>The lane whose map equals @p text, or 0 when none does.</summary>
        private int lane_carrying(string text) {
            if (document == null) {
                return 0;
            }
            for (var lane = 1; lane < global_environment.slot_count; ++lane) {
                if (string.Equals(document.environment_slots[lane - 1], text, StringComparison.Ordinal)) {
                    return lane;
                }
            }
            return 0;
        }

        /// <summary>
        /// The first lane that carries no map and that no node samples, or 0 when there is none. A lane with
        /// an empty text but a node pointing at it is not free: writing a map there would change that
        /// object's sky without being asked.
        /// </summary>
        private int free_lane() {
            if (document == null) {
                return 0;
            }
            for (var lane = 1; lane < global_environment.slot_count; ++lane) {
                if (string.IsNullOrEmpty(document.environment_slots[lane - 1]) && !lane_referenced(lane)) {
                    return lane;
                }
            }
            return 0;
        }

        /// <summary>The first lane that carries a map no node samples, or 0 when every lane is in use.</summary>
        private int lane_without_nodes() {
            if (document == null) {
                return 0;
            }
            for (var lane = 1; lane < global_environment.slot_count; ++lane) {
                if (!string.IsNullOrEmpty(document.environment_slots[lane - 1]) && !lane_referenced(lane)) {
                    return lane;
                }
            }
            return 0;
        }

        /// <summary>True when any node of the document samples @p lane.</summary>
        private bool lane_referenced(int lane) => nodes_on_lane(lane).Count > 0;

        /// <summary>Writes one lane onto the node, the runtime and the undo history.</summary>
        private bool set_node_environment(document_node node, int lane) {
            if (document == null) {
                return false;
            }
            if (node.environment == lane) {
                environment_ui?.invalidate_node();
                refresh_panels(force_details: true);
                return true;
            }
            var before = new node_snapshot(node);
            node.environment = lane;
            if (!push_runtime(node)) {
                restore_node(node, before, mark_dirty: false);
                return false;
            }
            var after = new node_snapshot(node);
            document.mark_dirty();
            record_undo("Set Environment Lane",
                () => restore_node(node, before, mark_dirty: true),
                () => restore_node(node, after, mark_dirty: true));
            environment_ui?.invalidate_node();
            refresh_panels(force_details: true);
            return true;
        }
    }
}