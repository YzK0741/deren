using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using deren.scripting;

namespace deren.editor;

/// <summary>
/// The scene editor screen of the editor's own UI library: the menus, the outliner, the details
/// inspector, the content browser and the status footer, laid out around the live viewport.
/// </summary>
/// <remarks>
/// The screen is a pure function of the scene document, the selection and the asset list, rebuilt on
/// every frame the way the Dope Sheet is: nothing is kept in the tree between frames, so an undo, a
/// reload or a selection change shows up on the next frame without an invalidation call. The viewport
/// slot paints nothing, which is what lets the renderer's scene show through where the 3D view belongs.
/// The outliner shows the flattened tree without collapsing yet, and the details rows are read-only
/// until the edit transactions land.
/// </remarks>
internal sealed unsafe partial class editor_host {
    private const float scene_header_height = 30.0F;
    private const float scene_footer_height = 22.0F;
    private const float scene_min_height = 320.0F;
    private const float scene_indent = 14.0F;
    private const float scene_fold_width = 14.0F;
    private const float scene_caption_height = ui_theme.row_height + 4.0F;
    private const float scene_outliner_min = 160.0F;
    private const float scene_outliner_max = 600.0F;
    private const float scene_details_min = 200.0F;
    private const float scene_details_max = 600.0F;
    private const float scene_browser_min = 90.0F;
    private const float scene_viewport_min = 120.0F;

    /// <summary>Which divider is being dragged; only one can move at a time.</summary>
    private enum scene_split {
        none,
        outliner,
        details,
        browser,
    }

    /// <summary>One row of the flattened outliner: the node, how deep it sits, and whether it can fold.</summary>
    private readonly struct scene_outline_row {
        internal scene_outline_row(document_node node, int depth, bool has_children, bool closed) {
            this.node = node;
            this.depth = depth;
            this.has_children = has_children;
            this.closed = closed;
        }

        internal document_node node { get; }
        internal int depth { get; }
        internal bool has_children { get; }
        internal bool closed { get; }
    }

    private readonly ui_list_view scene_outliner = new();
    private readonly ui_list_view scene_browser_list = new();
    private readonly List<scene_outline_row> scene_rows = [];
    private readonly HashSet<Guid> scene_collapsed = [];
    private string scene_picked_asset = string.Empty;
    private asset_authoring scene_authoring;
    private bool scene_started;
    // Panel sizes and the divider drag; the sizes persist next to the classic shell's layout.
    private workspace_layout scene_layout = new();
    private string scene_layout_path = string.Empty;
    private scene_split scene_split_gesture;
    // A press inside the 3D view that the pointer never dragged is a pick, the same gesture the classic
    // shell used to select the node under the cursor without stealing the camera's orbit drag.
    private float scene_press_x;
    private float scene_press_y;
    private bool scene_viewport_press;
    private const float scene_click_slop = 5.0F;
    // The outliner filter is its own field so typing a search cannot disturb a details edit.
    private readonly ui_text_edit scene_filter_edit = new();
    private string scene_filter_applied = string.Empty;
    // The browser searches the project too, with its own field and its own set of surviving indices, so a
    // filtered list keeps the absolute asset index in every row key the way the outliner keeps its rows.
    private readonly ui_text_edit scene_asset_edit = new();
    private string scene_asset_filter = string.Empty;
    private readonly List<int> scene_asset_matches = [];
    private string scene_last_asset = string.Empty;
    private long scene_last_asset_click;
    // A row press that travels is a reparent, not a click: the drag only starts once it leaves the slop.
    private Guid scene_drag_node = Guid.Empty;
    private Guid scene_drag_target = Guid.Empty;
    private bool scene_drag_armed;
    private bool scene_drag_active;
    // The one text field the details panel reuses: pressing a value row moves it onto that lane.
    private ui_text_edit? scene_value_edit;
    private string scene_value_lane = string.Empty;
    private Guid scene_value_node = Guid.Empty;
    // The inspector scrolls as one column: the offset, the height the last frame needed, and the height
    // of the window itself, which is what clamps the wheel.
    private float scene_details_scroll;
    private float scene_details_content;
    private float scene_details_window;
    private Guid scene_details_node = Guid.Empty;

    /// <summary>Starts the screen once: the document and the asset list the classic shell also builds.</summary>
    private void start_scene_ui() {
        if (scene_started || next_ui == null) { return; }
        scene_started = true;
        if (document == null) {
            report_next_ui_failure("has no scene document to show");
            return;
        }
        asset_authoring.try_open(out scene_authoring);
        if (assets.Length == 0) {
            // The classic shell lists the library while it starts; this frontend returns before that
            // happens, so the first frame that needs tiles asks for them instead.
            assets = asset_manager.list(out var status);
            if (status != script_status.success) {
                notice = string.Create(CultureInfo.InvariantCulture, $"Asset library unavailable ({status})");
            }
        }
        host_logger.write(
            $"Editor: scene screen ready ({document.roots.Count} root node(s), {assets.Length} asset(s))");
        // The panel sizes live in the same file the classic shell writes; a selftest run keeps them in
        // memory so a machine with a stale layout cannot change what the assertions see.
        if (!requested_ui_selftest) {
            string project_root = managed_bootstrap.project_directory;
            string workspace = string.Equals(project_root, managed_bootstrap.assembly_directory,
                                             StringComparison.OrdinalIgnoreCase)
                ? project_root
                : Path.Combine(project_root, ".deren");
            try {
                Directory.CreateDirectory(workspace);
                scene_layout_path = Path.Combine(workspace, "editor-layout.json");
                scene_layout = workspace_layout.load(scene_layout_path);
            } catch (Exception error) when (error is IOException or UnauthorizedAccessException) {
                host_logger.write($"Editor: could not prepare the workspace directory ({error.Message})");
            }
        }
        // A capture run cannot click, so the screenshot path can preselect a placement. Unset changes
        // nothing; an interactive run starts with nothing selected, as the classic shell did.
        if (Environment.GetEnvironmentVariable("DEREN_UI_SCENE_DEMO") is { Length: > 0 } demo) {
            var picked = demo switch {
                "light" => find_scene_node(document.roots, node => node.light != null),
                "camera" => find_scene_node(document.roots, node => node.camera),
                "actor" => find_scene_node(document.roots, node => node.subscene != null || node.asset != null),
                _ => find_scene_node(document.roots, node => node.asset != null || node.subscene != null),
            };
            if (picked != null) { selection_state?.select(picked.uid); }
            // The same run can open one details lane, so a screenshot shows the inline editor too.
            if (Environment.GetEnvironmentVariable("DEREN_UI_SCENE_EDIT") is { Length: > 0 } lane
                && selection_state != null && document.find(selection_state.primary) is { } edited) {
                begin_scene_edit_from_row(lane);
                if (scene_value_edit != null) { scene_value_edit.select_all(); }
            }
        }
    }

    private script_status run_scene_ui_frame() {
        start_scene_ui();
        if (next_ui == null) { return script_status.success; }
        // The play session is a separate process; the frame that owns its report has to be this one,
        // because the shared update hook returns straight into the scene screen.
        if (playing?.poll(output_log.add, out var play_result, out var play_failed) == true) {
            notice = play_result;
            if (play_failed) { output_log.add(Guid.Empty, play_result, true); }
        }
        install_next_ui_input();
        process_scene_input();
        if (!editor_ui_api.try_query_viewport(out var width, out var height, out var scale)) {
            return script_status.success;
        }
        next_ui.begin(width, height, scale);
        if (!next_ui.submit(build_scene_screen())) {
            report_next_ui_failure("overlay rejected the scene frame");
        }
        publish_scene_pointer_claim();
        return requested_ui_selftest ? scene_selftest_tick() : script_status.success;
    }

    private ui_element build_scene_screen() {
        rebuild_scene_rows(document?.roots ?? []);
        rebuild_scene_assets();
        build_scene_menus();
        float height = next_ui!.height;
        // The browser keeps its height and the body takes the rest, so a short window eats the 3D view
        // instead of pushing the panels off the screen.
        float body = MathF.Max(scene_min_height,
                               height - scene_header_height - scene_footer_height -
                               ui_controls.splitter_thickness - scene_layout.scene_browser_height);
        float outliner_height = MathF.Max(scene_outliner.row_height, body - scene_caption_height);
        float details_height = MathF.Max(ui_theme.row_height, body - scene_caption_height);
        float browser_height = MathF.Max(scene_browser_list.row_height, scene_layout.scene_browser_height - scene_caption_height);
        var root = ui_node.column("root").spacing(0.0F);
        root.add(build_scene_header());
        var middle = ui_node.row("scene_middle").size(float.NaN, body).spacing(0.0F);
        middle.add(ui_node.column("outliner").size(scene_layout.scene_outliner_width, body).bg(ui_theme.panel)
                       .add(build_scene_outliner_caption(),
                            build_scene_outliner(outliner_height)));
        middle.add(ui_controls.vertical_splitter("split_outliner", next_ui, scene_split_gesture == scene_split.outliner));
        middle.add(ui_node.box("viewport").weight(1.0F));
        middle.add(ui_controls.vertical_splitter("split_details", next_ui, scene_split_gesture == scene_split.details));
        middle.add(ui_node.column("details").size(scene_layout.scene_details_width, body).bg(ui_theme.panel)
                       .clipped()
                       .add(build_scene_caption("details_caption", "Details"),
                            build_scene_details(details_height)));
        root.add(middle);
        root.add(ui_controls.splitter("split_browser", next_ui, scene_split_gesture == scene_split.browser));
        root.add(ui_node.column("browser").size(float.NaN, scene_layout.scene_browser_height).bg(ui_theme.panel)
                     .clipped()
                     .add(build_scene_browser_caption(),
                          build_scene_browser(browser_height)));
        root.add(ui_node.row("footer").size(float.NaN, scene_footer_height).bg(ui_theme.header)
                     .add(centered(ui_node.label("footer_text", scene_footer_text(), ui_theme.small_size,
                                                 ui_theme.text_dim), float.NaN, scene_footer_height)));
        add_popup_overlays(scene_header_height);
        return root;
    }

    private ui_element build_scene_header() {
        var header = ui_node.row("header").size(float.NaN, scene_header_height).bg(ui_theme.header)
                         .pad(0.0F).spacing(0.0F);
        header.add(ui_node.box("header_inset").size(12.0F, 0.0F));
        header.add(centered(ui_node.label("scene_title", "Deren Editor", ui_theme.title_size, ui_theme.text_bright),
                            float.NaN, scene_header_height));
        header.add(ui_node.box("header_gap_title").size(10.0F, 0.0F));
        foreach (var menu in next_ui_menus) {
            header.add(ui_popup.menu_title(menu, next_ui_open_menu == menu.key, next_ui,
                                           scene_header_height - 8.0F));
        }
        header.add(ui_node.spacer());
        // Play is the same session the classic shell starts; the header button is what mirrors it here.
        header.add(ui_controls.button("scene_play", playing?.running == true ? "Stop" : "Play",
                                      playing?.running == true ? ui_button_style.filled : ui_button_style.outlined,
                                      next_ui, 64.0F, scene_header_height - 8.0F));
        header.add(ui_node.box("header_gap_play").size(10.0F, 0.0F));
        string document_name = document?.name ?? "no document";
        string dirty = document?.dirty == true ? " *" : string.Empty;
        header.add(centered(ui_node.label("scene_document", document_name + dirty, ui_theme.body_size,
                                          ui_theme.text_dim), float.NaN, scene_header_height));
        header.add(ui_node.box("header_trailing").size(12.0F, 0.0F));
        return header;
    }

    /// <summary>A panel caption: the title on a strip that separates the panel from the viewport.</summary>
    private static ui_element build_scene_caption(string key, string title) {
        var caption = ui_node.row(key).size(float.NaN, scene_caption_height).bg(ui_theme.panel).spacing(0.0F);
        caption.add(ui_node.box("caption_inset").size(8.0F, 0.0F));
        caption.add(centered(ui_node.label(key + "_text", title, ui_theme.small_size, ui_theme.text_dim),
                             float.NaN, scene_caption_height));
        return caption;
    }

    private ui_element build_scene_outliner(float list_height) {
        var column = ui_node.column("outliner_body").size(float.NaN, list_height);
        column.add(scene_outliner.build("outline", list_height, scene_rows.Count, build_scene_outline_row));
        return column;
    }

    private ui_element build_scene_outline_row(int index) {
        var row = scene_rows[index];
        bool selected = selection_state?.contains(row.node.uid) ?? false;
        string key = string.Create(CultureInfo.InvariantCulture, $"scene_row_{index}");
        var element = ui_node.row(key)
                          .size(float.NaN, scene_outliner.row_height)
                          .bg(scene_row_color(key, selected, index))
                          .spacing(0.0F);
        element.add(ui_node.box(key + "_indent").size(6.0F + row.depth * scene_indent, 0.0F));
        // A fold mark sits in a fixed slot so names line up whether or not a row has children; a leaf
        // still reserves the slot, or the tree would stagger.
        var fold = ui_node.box(key + "_fold").size(scene_fold_width, scene_outliner.row_height);
        if (row.has_children) {
            fold.text = row.closed ? "+" : "-";
            fold.font_size = ui_theme.body_size;
            fold.text_color = ui_theme.text_dim;
            fold.text_center = true;
        }
        element.add(fold);
        element.add(centered(ui_node.label(key + "_name",
                                           row.node.name.Length == 0 ? "(unnamed)" : row.node.name,
                                           ui_theme.body_size,
                                           selected ? ui_theme.text_bright : ui_theme.text_dim),
                             float.NaN, scene_outliner.row_height));
        element.add(ui_node.spacer(key + "_filler"));
        if (row.node.asset != null) {
            element.add(centered(ui_node.label(key + "_asset", row.node.asset, ui_theme.small_size,
                                               ui_theme.text_key), float.NaN, scene_outliner.row_height));
        } else if (row.node.subscene != null) {
            element.add(centered(ui_node.label(key + "_asset", "scene", ui_theme.small_size,
                                               ui_theme.text_key), float.NaN, scene_outliner.row_height));
        }
        element.add(ui_node.box(key + "_tail").size(4.0F, 0.0F));
        // The visibility mark is its own hit target, so hiding a node never moves the selection.
        var vis = ui_node.box(key + "_vis").size(20.0F, scene_outliner.row_height);
        vis.text = row.node.visible ? "o" : "-";
        vis.font_size = ui_theme.small_size;
        vis.text_color = row.node.visible ? ui_theme.text_dim : ui_theme.outline;
        vis.text_center = true;
        element.add(vis);
        element.add(ui_node.box(key + "_pad").size(4.0F, 0.0F));
        return element;
    }

    /// <summary>
    /// The inspector's scrolling window: a row that lies above the window is not built at all, and the
    /// content height is accumulated as if it had been, so the wheel clamp and the scrollbar agree with
    /// what the panel would show at rest. The top row of a scrolled panel sits flush against the clip
    /// instead of being cut, which is one row of slop and no misalignment of hit targets.
    /// </summary>
    private sealed class scene_details_body {
        private readonly ui_element column;
        private readonly float window;
        private readonly float scroll;
        private float cursor;

        internal scene_details_body(ui_element column, float window, float scroll) {
            this.column = column;
            this.window = MathF.Max(0.0F, window);
            this.scroll = MathF.Max(0.0F, scroll);
        }

        /// <summary>The height the whole inspector would need at rest.</summary>
        internal float content => cursor;

        internal void add(ui_element row, float height) {
            float top = cursor;
            cursor += height + 2.0F;
            // A row outside the window is not in the frame at all, so nothing below the clip can be
            // hovered or pressed, and the frame stays as small as the window is.
            if (top + height <= scroll || top >= scroll + window) { return; }
            column.add(row);
        }
    }

    private ui_element build_scene_details(float height) {
        scene_details_window = height;
        var node = selection_state == null || document == null ? null : document.find(selection_state.primary);
        // A different node starts at the top of the inspector; the same node keeps where the reader was.
        if ((node?.uid ?? Guid.Empty) != scene_details_node) {
            scene_details_node = node?.uid ?? Guid.Empty;
            scene_details_scroll = 0.0F;
        }
        var body_column = ui_node.column("details_body").size(float.NaN, height).spacing(2.0F).clipped();
        var column = new scene_details_body(body_column, height, scene_details_scroll);
        if (node == null) {
            scene_details_content = 0.0F;
            body_column.add(ui_node.label("details_empty", "Nothing selected", ui_theme.body_size, ui_theme.text_dim));
            return body_column;
        }
        // A row whose value is editable becomes a text field in place when it is pressed; the lanes are
        // the same ones the classic panel used, so the document paths behind them are already proven.
        add_scene_value_row(column, "name", "Name", node, node.name.Length == 0 ? string.Empty : node.name,
                            node.name.Length == 0 ? "(unnamed)" : node.name);
        add_scene_property(column, "asset", node.subscene != null ? "Source Scene" : "Asset",
                           node.subscene ?? node.asset ?? "(empty transform)");
        add_scene_property(column, "visible", "Visible", node.visible ? "yes (click to toggle)" : "no (click to toggle)");
        add_scene_value_row(column, "position", "Position", node, format_scene_vector(node.local.position),
                            format_scene_vector(node.local.position));
        add_scene_value_row(column, "rotation", "Rotation", node, format_scene_vector(node.local.rotation_degrees),
                            format_scene_vector(node.local.rotation_degrees));
        add_scene_value_row(column, "scale", "Scale", node, format_scene_vector(node.local.scale),
                            format_scene_vector(node.local.scale));
        // Actor flags: every toggle reuses the classic shell's own command, so the semantics stay one place.
        add_scene_section(column, "actor", "Actor");
        add_scene_toggle_row(column, "visible", "Visible", node.visible);
        add_scene_toggle_row(column, "collision_box", "Solid Box", node.collision_box);
        add_scene_toggle_row(column, "moving_solid", "Moving Solid", node.moving_solid);
        add_scene_toggle_row(column, "box_trigger", "Trigger Volume", node.box_trigger);
        add_scene_toggle_row(column, "trigger_target", "Trigger Target", node.trigger_target);
        add_scene_value_row(column, "flags", "Flags", node, seed_scene_component(node, "flags") ?? "0",
                            node.flags.ToString(CultureInfo.InvariantCulture));
        if (node.pipeline != null) {
            add_scene_property(column, "pipeline", "Pipeline", node.pipeline);
        }
        if (node.script != null) {
            add_scene_property(column, "script", "Behaviour", node.script);
        }
        if (node.camera) {
            add_scene_section(column, "camera", "Camera");
            add_scene_value_row(column, "camera_fov", "Vertical FOV", node,
                                seed_scene_component(node, "camera_fov") ?? string.Empty,
                                format_scene_number(node.camera_fov));
            add_scene_toggle_row(column, "main_camera", "Main Camera", node.main_camera);
        }
        if (node.light != null) {
            add_scene_section(column, "light", "Point Light");
            add_scene_value_row(column, "light_colour", "Color RGB", node,
                                seed_scene_component(node, "light_colour") ?? string.Empty,
                                format_scene_vector(node.light.Value.colour));
            add_scene_value_row(column, "light_intensity", "Intensity", node,
                                seed_scene_component(node, "light_intensity") ?? string.Empty,
                                format_scene_number(node.light.Value.intensity));
            add_scene_value_row(column, "light_range", "Range", node,
                                seed_scene_component(node, "light_range") ?? string.Empty,
                                format_scene_number(node.light.Value.range));
        }
        if (node.asset != null && node.subscene == null && supports_material_override(node.asset)) {
            var material = node.material ?? instance_material.imported;
            add_scene_section(column, "material", node.material.HasValue ? "Material Override" : "Imported Material");
            add_scene_value_row(column, "material_tint", "Tint RGB", node,
                                seed_scene_component(node, "material_tint") ?? string.Empty,
                                format_scene_vector(material.tint));
            add_scene_value_row(column, "material_metallic", "Metallic x", node,
                                seed_scene_component(node, "material_metallic") ?? string.Empty,
                                format_scene_number(material.metallic));
            add_scene_value_row(column, "material_roughness", "Roughness x", node,
                                seed_scene_component(node, "material_roughness") ?? string.Empty,
                                format_scene_number(material.roughness));
            add_scene_value_row(column, "material_emissive", "Emissive x", node,
                                seed_scene_component(node, "material_emissive") ?? string.Empty,
                                format_scene_number(material.emissive));
        }
        if (node.audio_source is { } audio) {
            add_scene_section(column, "audio", "Audio Source");
            add_scene_value_row(column, "audio_file", "Clip", node,
                                seed_scene_component(node, "audio_file") ?? string.Empty,
                                audio.file.Length == 0 ? "(no clip)" : audio.file);
            add_scene_value_row(column, "audio_volume", "Volume", node,
                                seed_scene_component(node, "audio_volume") ?? string.Empty,
                                format_scene_number(audio.volume));
            add_scene_toggle_row(column, "audio_loop", "Loop", audio.loop);
        }
        if (node.asset != null && node.subscene == null) {
            add_scene_section(column, "animation", "Animation");
            add_scene_value_row(column, "animation_clip", "Initial Clip", node,
                                seed_scene_component(node, "animation_clip") ?? string.Empty,
                                node.animation_clip < 0 ? "(asset default)" : node.animation_clip.ToString(CultureInfo.InvariantCulture));
            add_scene_toggle_row(column, "animation_autoplay", "Auto Play", node.animation_autoplay);
        }
        scene_details_content = column.content;
        return body_column;
    }

    /// <summary>A caption row that groups the inspector sections the way the classic panel did.</summary>
    private static void add_scene_section(scene_details_body column, string key, string title) {
        column.add(ui_node.box("scene_section_" + key).size(float.NaN, scene_caption_height)
                       .bg(ui_theme.header)
                       .pad(8.0F)
                       .label_text(title, ui_theme.small_size, ui_theme.text_dim),
                   scene_caption_height);
    }

    /// <summary>A boolean row: the value half shows the state and the whole value is the hit target.</summary>
    private void add_scene_toggle_row(scene_details_body column, string lane, string name, bool on) {
        var row = ui_node.row("scene_property_" + lane).size(float.NaN, ui_theme.row_height).spacing(0.0F);
        row.add(ui_node.box("scene_property_inset_" + lane).size(8.0F, 0.0F));
        row.add(centered(ui_node.label("scene_property_name_" + lane, name, ui_theme.small_size, ui_theme.text_dim),
                         88.0F, ui_theme.row_height));
        row.add(centered(ui_node.label("scene_property_value_" + lane, on ? "yes" : "no", ui_theme.body_size,
                                       on ? ui_theme.text_bright : ui_theme.text_dim),
                         float.NaN, ui_theme.row_height));
        column.add(row, ui_theme.row_height);
    }

    private static void add_scene_property(scene_details_body column, string key, string name, string value) {
        var row = ui_node.row("scene_property_" + key).size(float.NaN, ui_theme.row_height).spacing(0.0F);
        row.add(ui_node.box("scene_property_inset_" + key).size(8.0F, 0.0F));
        row.add(centered(ui_node.label("scene_property_name_" + key, name, ui_theme.small_size, ui_theme.text_dim),
                         88.0F, ui_theme.row_height));
        row.add(centered(ui_node.label("scene_property_value_" + key, value, ui_theme.body_size,
                                       ui_theme.text_bright), float.NaN, ui_theme.row_height));
        column.add(row, ui_theme.row_height);
    }

    /// <summary>
    /// A details row whose value can be edited: pressing it turns the value half of the row into a text
    /// field seeded with the current value, so the reader edits the value in the place it is shown.
    /// </summary>
    private void add_scene_value_row(scene_details_body column, string lane, string name, document_node node,
                                     string seed, string display) {
        bool editing = scene_value_edit != null && scene_value_edit.focused && scene_value_lane == lane
                       && scene_value_node == node.uid;
        var row = ui_node.row("scene_property_" + lane).size(float.NaN, ui_theme.row_height).spacing(0.0F)
                      .bg(editing ? ui_theme.panel_border : 0U);
        row.add(ui_node.box("scene_property_inset_" + lane).size(8.0F, 0.0F));
        row.add(centered(ui_node.label("scene_property_name_" + lane, name, ui_theme.small_size, ui_theme.text_dim),
                         88.0F, ui_theme.row_height));
        if (editing) {
            row.add(ui_node.box("scene_property_value_" + lane).weight(1.0F).pad(2.0F)
                        .bg(ui_theme.panel).stroke(ui_theme.accent, 1.0F)
                        .painted((paint, area) => paint_scene_value_field(paint, area)));
        } else {
            row.add(centered(ui_node.label("scene_property_value_" + lane, display, ui_theme.body_size,
                                           ui_theme.text_bright), float.NaN, ui_theme.row_height));
        }
        column.add(row, ui_theme.row_height);
    }

    private void paint_scene_value_field(ui_paint paint, ui_rect area) {
        var edit = scene_value_edit;
        if (edit == null) { return; }
        const float size = ui_theme.body_size;
        float text_y = area.y + (area.height - size) * 0.5F;
        paint.push_clip(area);
        if (edit.has_selection) {
            var (start, end) = edit.selection;
            float x0 = area.x + edit.caret_x(size, start);
            float x1 = area.x + edit.caret_x(size, end);
            paint.add_rect(new ui_rect(x0, area.y + 2.0F, MathF.Max(1.0F, x1 - x0), area.height - 4.0F), 0.0F,
                           ui_theme.selection, 0U, 0.0F);
        }
        if (edit.text.Length > 0) {
            paint.add_text(area.x, text_y, size, ui_theme.text_bright, edit.text);
        }
        if (edit.focused) {
            float caret_x = area.x + edit.caret_x(size);
            paint.add_rect(new ui_rect(caret_x, area.y + 3.0F, 1.0F, area.height - 6.0F), 0.0F,
                           ui_theme.accent, 0U, 0.0F);
        }
        paint.pop_clip();
    }

    /// <summary>The outliner caption carries the search field, so filtering sits next to what it filters.</summary>
    private ui_element build_scene_outliner_caption() {
        var row = ui_node.row("outliner_caption").size(float.NaN, scene_caption_height).spacing(0.0F);
        row.add(ui_node.box("outliner_caption_inset").size(8.0F, 0.0F));
        row.add(centered(ui_node.label("outliner_caption_text", "Hierarchy", ui_theme.small_size,
                                       ui_theme.text_dim), float.NaN, scene_caption_height));
        row.add(ui_node.spacer());
        row.add(ui_node.box("scene_filter").size(96.0F, scene_caption_height - 4.0F)
                    .bg(ui_theme.panel)
                    .stroke(scene_filter_edit.focused ? ui_theme.accent : ui_theme.panel_border, 1.0F)
                    .pad(2.0F)
                    .painted((paint, area) => paint_scene_field(paint, area, scene_filter_edit, "search")));
        row.add(ui_node.box("outliner_caption_trailing").size(4.0F, 0.0F));
        return row;
    }

    /// <summary>One caption search field: the placeholder when idle, otherwise the text and its caret.</summary>
    private static void paint_scene_field(ui_paint paint, ui_rect area, ui_text_edit edit, string placeholder) {
        const float size = ui_theme.small_size;
        float text_y = area.y + (area.height - size) * 0.5F;
        paint.push_clip(area);
        if (edit.text.Length == 0 && !edit.focused) {
            paint.add_text(area.x, text_y, size, ui_theme.outline, placeholder);
        } else {
            if (edit.text.Length > 0) {
                paint.add_text(area.x, text_y, size, ui_theme.text_bright, edit.text);
            }
            if (edit.focused) {
                float caret_x = area.x + edit.caret_x(size);
                paint.add_rect(new ui_rect(caret_x, area.y + 2.0F, 1.0F, area.height - 4.0F), 0.0F,
                               ui_theme.accent, 0U, 0.0F);
            }
        }
        paint.pop_clip();
    }

    /// <summary>Opens the inline editor on one details lane, seeded with the value it shows.</summary>
    private void begin_scene_edit(document_node node, string lane, string seed) {
        scene_value_edit ??= new ui_text_edit();
        scene_value_edit.reset(seed);
        scene_value_edit.focused = true;
        scene_value_lane = lane;
        scene_value_node = node.uid;
    }

    private void cancel_scene_edit() {
        if (scene_value_edit != null) { scene_value_edit.focused = false; }
        scene_value_lane = string.Empty;
        scene_value_node = Guid.Empty;
    }

    /// <summary>Applies the inline value through the document paths the classic panel already uses.</summary>
    private void apply_scene_edit() {
        var edit = scene_value_edit;
        string lane = scene_value_lane;
        var node = document == null || scene_value_node == Guid.Empty ? null : document.find(scene_value_node);
        cancel_scene_edit();
        if (edit == null || node == null || lane.Length == 0) { return; }
        string text = edit.text;
        switch (lane) {
            case "name":
                if (apply_detail_change(detail_field.name, text)) {
                    notice = string.Create(CultureInfo.InvariantCulture, $"Renamed to '{node.name}'");
                }
                break;
            case "position":
                apply_scene_vector(node, detail_field.position_x, text, "Position");
                break;
            case "rotation":
                apply_scene_vector(node, detail_field.rotation_x, text, "Rotation");
                break;
            case "scale":
                apply_scene_vector(node, detail_field.scale_x, text, "Scale");
                break;
            default:
                apply_scene_component(node, lane, text);
                break;
        }
    }

    /// <summary>
    /// Writes a whole transform vector as ONE undo step: the three lanes share a snapshot, so dragging a
    /// value back is a single Ctrl+Z rather than three.
    /// </summary>
    private void apply_scene_vector(document_node node, detail_field first_lane, string text, string label) {
        if (!try_parse_scene_vector(text, out var x, out var y, out var z)) {
            notice = "A transform lane takes three numbers, such as 0, 1, 2.5";
            return;
        }
        var before = new node_snapshot(node);
        var local = node.local;
        bool changed = false;
        changed |= set_lane(ref local, first_lane, x);
        changed |= set_lane(ref local, first_lane + 1, y);
        changed |= set_lane(ref local, first_lane + 2, z);
        if (!changed) {
            notice = string.Create(CultureInfo.InvariantCulture, $"{label} already had those values");
            return;
        }
        node.local = local;
        if (!push_runtime(node)) {
            restore_node(node, before, mark_dirty: false);
            notice = "The runtime refused the transform";
            return;
        }
        var after = new node_snapshot(node);
        document?.mark_dirty();
        record_undo("Edit " + label,
                    () => restore_node(node, before, mark_dirty: true),
                    () => restore_node(node, after, mark_dirty: true));
        notice = string.Create(CultureInfo.InvariantCulture, $"{label} set to {format_scene_vector(node.local.position)}");
    }

    private static bool try_parse_scene_vector(string text, out float x, out float y, out float z) {
        x = 0.0F;
        y = 0.0F;
        z = 0.0F;
        var parts = text.Split([',', ' ', '\t'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length != 3) { return false; }
        if (!float.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out x) ||
            !float.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out y) ||
            !float.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out z)) {
            return false;
        }
        return float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z);
    }

    private static string format_scene_vector(deren.vector_3 value) {
        return string.Create(CultureInfo.InvariantCulture, $"{value.x:0.###}, {value.y:0.###}, {value.z:0.###}");
    }

    private ui_element build_scene_browser(float list_height) {
        var column = ui_node.column("browser_body").size(float.NaN, list_height);
        column.add(scene_browser_list.build("browser_tiles", list_height, scene_asset_matches.Count, build_scene_asset_row));
        return column;
    }

    /// <summary>The browser caption carries its own search field and how much of the project survived it.</summary>
    private ui_element build_scene_browser_caption() {
        var row = ui_node.row("browser_caption").size(float.NaN, scene_caption_height).spacing(0.0F);
        row.add(ui_node.box("browser_caption_inset").size(8.0F, 0.0F));
        row.add(centered(ui_node.label("browser_caption_text", "Project", ui_theme.small_size, ui_theme.text_dim),
                         float.NaN, scene_caption_height));
        row.add(ui_node.box("browser_caption_gap").size(8.0F, 0.0F));
        row.add(centered(ui_node.label("browser_caption_count", scene_asset_count_text(), ui_theme.small_size,
                                       ui_theme.text_key), float.NaN, scene_caption_height));
        row.add(ui_node.spacer());
        row.add(ui_node.box("scene_asset_filter").size(96.0F, scene_caption_height - 4.0F)
                    .bg(ui_theme.panel)
                    .stroke(scene_asset_edit.focused ? ui_theme.accent : ui_theme.panel_border, 1.0F)
                    .pad(2.0F)
                    .painted((paint, area) => paint_scene_field(paint, area, scene_asset_edit, "find asset")));
        row.add(ui_node.box("browser_caption_trailing").size(4.0F, 0.0F));
        return row;
    }

    private string scene_asset_count_text() {
        return string.Create(CultureInfo.InvariantCulture, $"{scene_asset_matches.Count} / {assets.Length}");
    }

    /// <summary>Recomputes which assets survive the filter; the rows keep their absolute index.</summary>
    private void rebuild_scene_assets() {
        scene_asset_matches.Clear();
        string filter = scene_asset_filter.Trim();
        for (var index = 0; index < assets.Length; ++index) {
            string id = assets[index];
            if (filter.Length == 0) {
                scene_asset_matches.Add(index);
                continue;
            }
            string? name = scene_authoring.display_name(id);
            if (id.Contains(filter, StringComparison.OrdinalIgnoreCase) ||
                (name != null && name.Contains(filter, StringComparison.OrdinalIgnoreCase))) {
                scene_asset_matches.Add(index);
            }
        }
    }

    private void apply_scene_asset_filter() {
        scene_asset_edit.clear_preedit();
        scene_asset_filter = scene_asset_edit.text;
        rebuild_scene_assets();
        scene_browser_list.first = 0;
    }

    private void cancel_scene_asset_filter() {
        scene_asset_edit.clear_preedit();
        scene_asset_edit.focused = false;
        scene_asset_edit.reset(string.Empty);
        scene_asset_filter = string.Empty;
        rebuild_scene_assets();
    }

    private ui_element build_scene_asset_row(int index) {
        int asset_index = scene_asset_matches[index];
        string id = assets[asset_index];
        bool picked = string.Equals(id, scene_picked_asset, StringComparison.Ordinal);
        string key = string.Create(CultureInfo.InvariantCulture, $"scene_asset_{asset_index}");
        var row = ui_node.row(key)
                      .size(float.NaN, scene_browser_list.row_height)
                      .bg(scene_row_color(key, picked, index))
                      .spacing(0.0F);
        row.add(ui_node.box(key + "_inset").size(8.0F, 0.0F));
        string label = scene_authoring.display_name(id) ?? id;
        row.add(centered(ui_node.label(key + "_name", label, ui_theme.body_size,
                                       picked ? ui_theme.text_bright : ui_theme.text_dim),
                         float.NaN, scene_browser_list.row_height));
        row.add(ui_node.spacer(key + "_filler"));
        row.add(centered(ui_node.label(key + "_id", id, ui_theme.small_size, ui_theme.text_key),
                         float.NaN, scene_browser_list.row_height));
        row.add(ui_node.box(key + "_tail").size(8.0F, 0.0F));
        return row;
    }

    private string scene_footer_text() {
        if (notice.Length > 0) { return notice; }
        int nodes = document == null ? 0 : count_nodes(document.roots);
        string path = document?.path ?? "(unsaved document)";
        return string.Create(CultureInfo.InvariantCulture,
                             $"Scene screen on the editor UI library | {nodes} node(s) | {path}");
    }

    /// <summary>The row fill: selection wins, then the pointer, then a zebra stripe.</summary>
    private uint scene_row_color(string key, bool selected, int index) {
        // A row the pointer carries a node over is the drop target; it reads before selection so the
        // drag is never in doubt.
        if (scene_drag_active && scene_drag_target != Guid.Empty &&
            try_scene_index(key, "scene_row_", out var row) && row >= 0 && row < scene_rows.Count &&
            scene_rows[row].node.uid == scene_drag_target) {
            return ui_theme.selection;
        }
        if (selected) { return ui_theme.row_selected; }
        // The pointer's key is the deepest child of the row, so the row checks its own prefix.
        if (next_ui != null && next_ui.hovered_key.StartsWith(key, StringComparison.Ordinal)) {
            return ui_theme.row_hover;
        }
        return (index & 1) == 0 ? ui_theme.row_even : ui_theme.row_odd;
    }

    /// <summary>
    /// Reads the index out of a row key whose children append their own suffix, so a press on the name
    /// label of row 3 arrives as "scene_row_3_name" and still means row 3.
    /// </summary>
    private static bool try_scene_index(string key, string prefix, out int index) {
        index = -1;
        if (!key.StartsWith(prefix, StringComparison.Ordinal)) { return false; }
        int start = prefix.Length;
        int end = start;
        while (end < key.Length && char.IsAsciiDigit(key[end])) { ++end; }
        return end > start &&
               int.TryParse(key.AsSpan(start, end - start), NumberStyles.Integer, CultureInfo.InvariantCulture,
                            out index);
    }

    /// <summary>
    /// Flattens a set of roots in outliner order; the rows are rebuilt from scratch each frame, and the
    /// selftest passes a synthetic tree so it can cover folding without editing the live document.
    /// </summary>
    private void rebuild_scene_rows(IReadOnlyList<document_node> roots) {
        scene_rows.Clear();
        string filter = scene_filter_applied.Trim();
        foreach (var root in roots) {
            append_scene_rows(root, 0, filter);
        }
    }

    /// <summary>Appends one node and its subtree, honouring the filter and the collapsed set.</summary>
    private bool append_scene_rows(document_node node, int depth, string filter) {
        if (filter.Length == 0) {
            bool has_children = node.children.Count != 0;
            bool closed = has_children && scene_collapsed.Contains(node.uid);
            scene_rows.Add(new scene_outline_row(node, depth, has_children, closed));
            if (closed) { return true; }
            foreach (var child in node.children) {
                append_scene_rows(child, depth + 1, filter);
            }
            return true;
        }
        // A filter hides nothing that matches and keeps the ancestors of a match so the indent stays
        // meaningful; folding is ignored while a filter runs, or half the results would be invisible.
        int mark = scene_rows.Count;
        bool self = node.name.Contains(filter, StringComparison.OrdinalIgnoreCase);
        if (self) {
            scene_rows.Add(new scene_outline_row(node, depth, false, false));
        }
        foreach (var child in node.children) {
            append_scene_rows(child, depth + 1, filter);
        }
        bool matched = scene_rows.Count > mark;
        if (!self && matched) {
            scene_rows.Insert(mark, new scene_outline_row(node, depth, false, false));
        }
        return matched;
    }

    /// <summary>Commits the filter text; the rows rebuild from it on the next frame.</summary>
    private void apply_scene_filter() {
        scene_filter_edit.clear_preedit();
        scene_filter_applied = scene_filter_edit.text;
    }

    private void cancel_scene_filter() {
        scene_filter_edit.reset(scene_filter_applied);
        scene_filter_edit.focused = false;
    }

    private void build_scene_menus() {
        next_ui_menus.Clear();
        var file = new ui_menu { key = "scene_menu_file", title = "File" };
        file.items.Add(new ui_command {
            key = "scene_menu_save",
            label = "Save Scene",
            enabled = document != null,
            run = () => {
                if (document != null && save_document()) { notice = "Scene saved"; }
            },
        });
        var edit = new ui_menu { key = "scene_menu_edit", title = "Edit" };
        edit.items.Add(new ui_command {
            key = "scene_menu_undo",
            label = "Undo",
            enabled = undo_history?.can_undo == true,
            run = () => {
                if (undo_history?.try_undo() == true) { notice = "Undone"; }
            },
        });
        edit.items.Add(new ui_command {
            key = "scene_menu_redo",
            label = "Redo",
            enabled = undo_history?.can_redo == true,
            run = () => {
                if (undo_history?.try_redo() == true) { notice = "Redone"; }
            },
        });
        next_ui_menus.Add(file);
        next_ui_menus.Add(edit);
        var help = new ui_menu { key = "scene_menu_help", title = "Help" };
        help.items.Add(new ui_command {
            key = "scene_menu_shortcuts",
            label = "Shortcuts",
            run = show_scene_shortcuts,
        });
        next_ui_menus.Add(help);
    }

    private void show_scene_shortcuts() {
        var dialog = new ui_dialog {
            key = "dialog_scene_shortcuts",
            title = "Scene Editor Shortcuts",
            message = "Click a hierarchy row to select its node.\n"
                    + "The wheel scrolls whichever list is under the pointer.\n"
                    + "Ctrl+Z and Ctrl+Y step through the undo history.",
        };
        dialog.buttons.Add(new ui_command {
            key = "dialog_scene_shortcuts_close",
            label = "Close",
            style = ui_button_style.filled,
        });
        next_ui_dialog = dialog;
    }

    /// <summary>Applies this frame's pointer, wheel and key events to the selection and the lists.</summary>
    private void process_scene_input() {
        if (next_ui_events.Count == 0) { return; }
        foreach (var value in next_ui_events) {
            switch (value.type) {
                case input_type.mouse_move:
                    next_ui?.set_pointer(value.x, value.y);
                    next_ui_pointer_x = value.x;
                    next_ui_pointer_y = value.y;
                    next_ui_pointer_known = true;
                    if (scene_split_gesture != scene_split.none) {
                        update_scene_split(value.x, value.y);
                    } else {
                        update_scene_drag(value.x, value.y);
                    }
                    break;
                case input_type.mouse_button when value.button == 0 && value.down:
                    next_ui?.set_pointer(value.x, value.y);
                    next_ui_pointer_x = value.x;
                    next_ui_pointer_y = value.y;
                    next_ui_pointer_known = true;
                    // Remember a press that landed on the preview: releasing it in place picks a node,
                    // while a drag belongs to the camera and must not change the selection.
                    scene_viewport_press = scene_is_viewport_hit(value.x, value.y);
                    scene_press_x = value.x;
                    scene_press_y = value.y;
                    press_scene_ui(value.x, value.y, value.modifiers);
                    break;
                case input_type.mouse_button when value.button == 0:
                    // Releasing a divider keeps the new size and writes it next to the classic layout.
                    if (scene_split_gesture != scene_split.none) {
                        scene_split_gesture = scene_split.none;
                        save_scene_layout();
                    }
                    // Releasing a row drag reparents; releasing it in place is just the click that selected.
                    if (scene_drag_armed) { finish_scene_drag(value.x, value.y); }
                    if (scene_viewport_press) {
                        scene_viewport_press = false;
                        float dx = value.x - scene_press_x;
                        float dy = value.y - scene_press_y;
                        if (dx * dx + dy * dy <= scene_click_slop * scene_click_slop) {
                            pick_scene_viewport(value.x, value.y);
                        }
                    }
                    break;
                case input_type.scroll:
                    scroll_scene_ui(value.wheel, value.x, value.y);
                    break;
                case input_type.character when value.down:
                    // Typing goes wherever the caret is: the inline editor, then the two search fields.
                    if (scene_value_edit is { focused: true }) {
                        scene_value_edit.insert(value.codepoint);
                    } else if (scene_filter_edit.focused) {
                        scene_filter_edit.insert(value.codepoint);
                        apply_scene_filter();
                    } else if (scene_asset_edit.focused) {
                        scene_asset_edit.insert(value.codepoint);
                        apply_scene_asset_filter();
                    }
                    break;
                case input_type.key when value.down:
                    handle_scene_key(value);
                    break;
                default:
                    break;
            }
        }
        next_ui_events.Clear();
    }

    /// <summary>
    /// One drag step of one divider. Every clamp keeps the 3D view at least <see cref="scene_viewport_min"/>
    /// wide or tall, so a divider can never swallow the preview.
    /// </summary>
    private void update_scene_split(float x, float y) {        if (next_ui == null) { return; }
        switch (scene_split_gesture) {
            case scene_split.outliner: {
                float max = MathF.Min(scene_outliner_max, next_ui.width - scene_layout.scene_details_width -
                                                         ui_controls.splitter_thickness * 2.0F - scene_viewport_min);
                scene_layout = scene_layout with {
                    scene_outliner_width = Math.Clamp(x, scene_outliner_min, MathF.Max(scene_outliner_min, max)),
                };
                break;
            }
            case scene_split.details: {
                float width = next_ui.width - x;
                float max = MathF.Min(scene_details_max, next_ui.width - scene_layout.scene_outliner_width -
                                                         ui_controls.splitter_thickness * 2.0F - scene_viewport_min);
                scene_layout = scene_layout with {
                    scene_details_width = Math.Clamp(width, scene_details_min, MathF.Max(scene_details_min, max)),
                };
                break;
            }
            case scene_split.browser: {
                float available = next_ui.height - scene_header_height - scene_footer_height -
                                  ui_controls.splitter_thickness;
                float height = next_ui.height - scene_footer_height - y;
                float max = MathF.Min(available - scene_viewport_min, MathF.Min(500.0F, available));
                scene_layout = scene_layout with {
                    scene_browser_height = Math.Clamp(height, scene_browser_min, MathF.Max(scene_browser_min, max)),
                };
                break;
            }
            default:
                break;
        }
    }

    private void save_scene_layout() {
        if (scene_layout_path.Length == 0) { return; }
        scene_layout.save(scene_layout_path);
    }

    /// <summary>One step of a row drag: it becomes a reparent once it leaves the press slop.</summary>
    private void update_scene_drag(float x, float y) {
        if (!scene_drag_armed) { return; }
        if (!scene_drag_active) {
            float dx = x - scene_press_x;
            float dy = y - scene_press_y;
            if (dx * dx + dy * dy <= scene_click_slop * scene_click_slop) { return; }
            scene_drag_active = true;
        }
        scene_drag_target = scene_drop_target(x, y)?.uid ?? Guid.Empty;
    }

    /// <summary>
    /// The node a drop at this point lands under, or null for the scene root. Empty outliner space below the
    /// rows means the root, which is how the classic shell let a drag promote a node out of its parent.
    /// </summary>
    private document_node? scene_drop_target(float x, float y) {
        string key = next_ui?.hit_test(x, y)?.key ?? string.Empty;
        if (try_scene_index(key, "scene_row_", out var index) && index >= 0 && index < scene_rows.Count) {
            return scene_rows[index].node;
        }
        return null;
    }

    /// <summary>
    /// Ends a row drag. The move goes through the classic <see cref="reparent_selection"/>, so the world
    /// transform is preserved, one undo step records it and the runtime follows.
    /// </summary>
    private void finish_scene_drag(float x, float y) {
        bool moving = scene_drag_active;
        Guid dragged = scene_drag_node;
        scene_drag_armed = false;
        scene_drag_active = false;
        scene_drag_node = Guid.Empty;
        scene_drag_target = Guid.Empty;
        if (!moving || dragged == Guid.Empty || document == null || selection_state == null) { return; }
        var target = scene_drop_target(x, y);
        if (target != null && target.uid == dragged) { return; }
        if (!selection_state.contains(dragged) || selection_state.primary != dragged) {
            selection_state.select(dragged);
        }
        if (reparent_selection(target)) {
            notice = target == null
                         ? "Moved to the scene root"
                         : string.Create(CultureInfo.InvariantCulture, $"Moved under '{target.name}'");
        }
    }

    /// <summary>True when the point lands on the 3D preview rather than on a panel or a divider.</summary>
    private bool scene_is_viewport_hit(float x, float y) {
        return next_ui?.last_tree?.find("viewport") is { } viewport && viewport.rect.contains(x, y) &&
               !scene_pointer_claims(x, y);
    }

    /// <summary>
    /// Selects the node under a click in the preview. The ray comes from the projection the renderer uses,
    /// and the pick runs against the live tree, so a click selects the instance the camera actually shows.
    /// </summary>
    private void pick_scene_viewport(float x, float y) {
        if (document == null || selection_state == null || tree == null || next_ui?.last_tree?.find("viewport") is not { } viewport ||
            viewport.rect.width <= 0.0F || viewport.rect.height <= 0.0F ||
            !camera_projection.try_get_viewport_size(out var width, out var height)) {
            return;
        }
        float u = (x - viewport.rect.x) / viewport.rect.width;
        float v = (y - viewport.rect.y) / viewport.rect.height;
        if (u < 0.0F || u >= 1.0F || v < 0.0F || v >= 1.0F ||
            !camera_projection.try_screen_ray(u * width, v * height, out var origin, out var direction)) {
            return;
        }
        if (!tree.try_pick_node(origin, direction, out var hit)) {
            selection_state.clear();
            notice = "Nothing under the pointer";
            return;
        }
        var picked = document.find_runtime(hit);
        var node = picked?.instance_root ?? picked;
        if (node == null) {
            selection_state.clear();
            notice = string.Empty;
            return;
        }
        selection_state.select(node.uid);
        notice = string.Empty;
        reveal_scene_row(node.uid);
    }

    /// <summary>Scrolls the outliner so the picked node's row is inside the list, the way the classic shell did.</summary>
    private void reveal_scene_row(Guid uid) {
        if (next_ui == null) { return; }
        for (var index = 0; index < scene_rows.Count; ++index) {
            if (scene_rows[index].node.uid != uid) { continue; }
            float height = scene_outliner_window_height();
            int visible = scene_outliner.visible_rows(height);
            if (index < scene_outliner.first) {
                scene_outliner.first = index;
            } else if (index >= scene_outliner.first + visible) {
                scene_outliner.first = index - visible + 1;
            }
            scene_outliner.clamp(scene_rows.Count, height);
            return;
        }
    }

    /// <summary>The height the outliner list gets from the current layout, shared by the wheel and the reveal.</summary>
    private float scene_outliner_window_height() {
        float total = next_ui?.height ?? 0.0F;
        float body = MathF.Max(scene_min_height, total - scene_header_height - scene_footer_height -
                                                 ui_controls.splitter_thickness - scene_layout.scene_browser_height);
        return MathF.Max(scene_outliner.row_height, body - scene_caption_height);
    }

    private void press_scene_ui(float x, float y, int modifiers = 0) {
        var hit = next_ui?.hit_test(x, y);
        string key = hit?.key ?? string.Empty;
        if (handle_popup_press(key)) { return; }
        if (find_menu(key) != null) {
            next_ui_open_menu = next_ui_open_menu == key ? string.Empty : key;
            return;
        }
        if (key == "scene_play") {
            toggle_scene_play();
            return;
        }
        // A press on an editable value row keeps (and commits) whatever is being typed, unless the press
        // lands on the same field again, where the caret is placed instead.
        if (scene_value_edit is { focused: true }) {
            bool same = key == "scene_property_value_" + scene_value_lane;
            if (!same) { apply_scene_edit(); }
            if (!same || key.Length == 0) { return; }
        }
        // A click anywhere else dismisses the notice, the way the classic shell does.
        notice = string.Empty;
        switch (key) {
            case "split_outliner":
                scene_split_gesture = scene_split.outliner;
                update_scene_split(x, y);
                return;
            case "split_details":
                scene_split_gesture = scene_split.details;
                update_scene_split(x, y);
                return;
            case "split_browser":
                scene_split_gesture = scene_split.browser;
                update_scene_split(x, y);
                return;
            default:
                break;
        }
        if (key.StartsWith("scene_property_value_", StringComparison.Ordinal)) {
            begin_scene_edit_from_row(key["scene_property_value_".Length..]);
            return;
        }
        if (key.StartsWith("scene_filter", StringComparison.Ordinal)) {
            // The filter is a text field like any other: pressing it focuses it and places the caret.
            apply_scene_edit();
            scene_filter_edit.focused = true;
            float field_x = hit == null ? x : hit.rect.x;
            scene_filter_edit.move_to(scene_filter_edit.caret_at(x - field_x - 2.0F, ui_theme.small_size), false);
            return;
        }
        if (key.StartsWith("scene_asset_filter", StringComparison.Ordinal)) {
            // The browser search behaves like the hierarchy one: focus, then place the caret where the press was.
            apply_scene_edit();
            scene_asset_edit.focused = true;
            float field_x = hit == null ? x : hit.rect.x;
            scene_asset_edit.move_to(scene_asset_edit.caret_at(x - field_x - 2.0F, ui_theme.small_size), false);
            return;
        }
        if (try_scene_index(key, "scene_row_", out var row) && row >= 0 && row < scene_rows.Count) {
            // The fold mark is a hit target of its own: folding must not also move the selection.
            if (key.EndsWith("_fold", StringComparison.Ordinal)) {
                toggle_scene_fold(scene_rows[row].node);
            } else if (key.EndsWith("_vis", StringComparison.Ordinal)) {
                set_node_visibility(scene_rows[row].node, !scene_rows[row].node.visible);
            } else {
                select_scene_row(scene_rows[row].node.uid, modifiers);
                // The same press can become a reparent: the drag only starts once it leaves the slop.
                scene_drag_node = scene_rows[row].node.uid;
                scene_drag_armed = true;
                scene_drag_active = false;
                scene_drag_target = Guid.Empty;
            }
            return;
        }
        if (try_scene_index(key, "scene_asset_", out var tile) && tile >= 0 && tile < assets.Length) {
            scene_picked_asset = assets[tile];
            notice = string.Create(CultureInfo.InvariantCulture, $"Picked '{scene_authoring.display_name(assets[tile]) ?? assets[tile]}'");
            // A second press on the same tile inside the double-click window adds a node, as the
            // classic browser did; the first press only picks.
            long now = Stopwatch.GetTimestamp();
            bool activate = scene_picked_asset == scene_last_asset &&
                            Stopwatch.GetElapsedTime(scene_last_asset_click, now).TotalMilliseconds <= 500.0;
            scene_last_asset = activate ? string.Empty : scene_picked_asset;
            scene_last_asset_click = now;
            if (activate && add_node(scene_picked_asset)) {
                notice = string.Create(CultureInfo.InvariantCulture, $"Added '{scene_picked_asset}'");
            }
        }
    }

    /// <summary>Starts an inline edit for one lane; the visibility row toggles instead of accepting text.</summary>
    private void begin_scene_edit_from_row(string lane) {
        var node = selection_state == null || document == null ? null : document.find(selection_state.primary);
        if (node == null || lane.Length == 0) { return; }
        switch (lane) {
            case "name":
                begin_scene_edit(node, "name", node.name);
                return;
            case "position":
                begin_scene_edit(node, "position", format_scene_vector(node.local.position));
                return;
            case "rotation":
                begin_scene_edit(node, "rotation", format_scene_vector(node.local.rotation_degrees));
                return;
            case "scale":
                begin_scene_edit(node, "scale", format_scene_vector(node.local.scale));
                return;
            case "visible":
                if (set_node_visibility(node, !node.visible)) {
                    notice = string.Create(CultureInfo.InvariantCulture,
                                           $"Node is now {(node.visible ? "visible" : "hidden")}");
                }
                return;
            default: {
                // A component lane seeds from the node, so the field opens on the value it shows; a
                // boolean lane has no text and toggles instead.
                if (seed_scene_component(node, lane) is { } seed) {
                    begin_scene_edit(node, lane, seed);
                } else {
                    toggle_scene_component(node, lane);
                }
                return;
            }
        }
    }

    private void toggle_scene_fold(document_node node) {
        if (!scene_collapsed.Remove(node.uid)) {
            scene_collapsed.Add(node.uid);
        }
    }

    /// <summary>
    /// Selects the row under the pointer. Ctrl toggles one row into the selection; Shift takes the range
    /// between the primary node and this one in the flattened order the outliner is showing.
    /// </summary>
    private void select_scene_row(Guid uid, int modifiers = 0) {
        if (selection_state == null || document == null) { return; }
        var node = document.find(uid);
        if (node == null) { return; }
        bool control = (modifiers & modifier.control) != 0;
        bool shift = (modifiers & modifier.shift) != 0;
        if (shift) {
            select_scene_range(uid);
        } else if (control) {
            selection_state.toggle(uid);
        } else {
            selection_state.select(uid);
        }
        notice = string.Create(CultureInfo.InvariantCulture,
                               $"Selected '{(node.name.Length == 0 ? "(unnamed)" : node.name)}'");
    }

    private void select_scene_range(Guid uid) {
        if (selection_state == null) { return; }
        int anchor = -1;
        int target = -1;
        for (var index = 0; index < scene_rows.Count; ++index) {
            if (scene_rows[index].node.uid == selection_state.primary) { anchor = index; }
            if (scene_rows[index].node.uid == uid) { target = index; }
        }
        if (anchor < 0) {
            // Without a primary there is nothing to range from, so the press behaves like a plain select.
            selection_state.select(uid);
            return;
        }
        int first = Math.Min(anchor, target);
        int last = Math.Max(anchor, target);
        var picked = new List<Guid>(last - first + 1);
        for (var index = first; index <= last; ++index) {
            picked.Add(scene_rows[index].node.uid);
        }
        selection_state.replace(picked, uid);
    }

    /// <summary>
    /// Start or stop the play session. The scene screen owns the frame, so it also owns the report the
    /// session hands back; everything else is the same call the classic Play button makes.
    /// </summary>
    private void toggle_scene_play() {
        if (playing == null || document == null) {
            notice = "There is no document to play";
            return;
        }
        if (playing.running) {
            playing.stop();
            notice = "Stopping the play session";
            return;
        }
        if (document.dirty && !save_document()) {
            notice = "Save the scene before Play";
            return;
        }
        if (!orbit_camera.try_get(out var camera)) {
            notice = "The editor camera is not available";
            return;
        }
        if (playing.start(document, camera, out var message)) {
            output_log.add(Guid.Empty, "Play started", false);
        }
        notice = message;
    }
    private void scroll_scene_ui(float wheel, float x, float y) {
        var hit = next_ui?.hit_test(x, y);
        if (hit == null) { return; }
        string key = hit.key;
        if (key.StartsWith("outline", StringComparison.Ordinal)) {
            scene_outliner.scroll(scene_rows.Count,
                                  MathF.Max(scene_outliner.row_height, next_ui!.height - scene_header_height -
                                            scene_footer_height - scene_layout.scene_browser_height - scene_caption_height),
                                  wheel);
        } else if (key.StartsWith("browser", StringComparison.Ordinal)) {
            scene_browser_list.scroll(scene_asset_matches.Count,
                                      MathF.Max(scene_browser_list.row_height,
                                                scene_layout.scene_browser_height - scene_caption_height), wheel);
        } else if (key.StartsWith("details", StringComparison.Ordinal) ||
                   key.StartsWith("scene_property", StringComparison.Ordinal) ||
                   key.StartsWith("scene_section", StringComparison.Ordinal)) {
            // The inspector can be taller than its panel; the wheel walks it by whole rows and the clamp
            // comes from the height the last frame measured with every row built.
            scene_details_scroll = Math.Clamp(scene_details_scroll - wheel * ui_theme.row_step, 0.0F,
                                              MathF.Max(0.0F, scene_details_content - scene_details_window));
        }
    }

    private void handle_scene_key(input_event value) {
        // A running inline edit owns the keyboard: Enter applies, Escape cancels, the rest edits text.
        if (scene_value_edit is { focused: true }) {
            handle_text_field_key(scene_value_edit, value, apply_scene_edit, cancel_scene_edit);
            return;
        }
        if (scene_filter_edit.focused) {
            handle_text_field_key(scene_filter_edit, value, () => {
                apply_scene_filter();
                scene_filter_edit.focused = false;
            }, cancel_scene_filter);
            return;
        }
        if (scene_asset_edit.focused) {
            handle_text_field_key(scene_asset_edit, value, () => {
                apply_scene_asset_filter();
                scene_asset_edit.focused = false;
            }, cancel_scene_asset_filter);
            return;
        }
        if (value.key == key_code.escape) {
            if (next_ui_dialog != null) {
                next_ui_dialog = null;
            } else if (next_ui_open_menu.Length != 0) {
                next_ui_open_menu = string.Empty;
            } else if (scene_filter_applied.Length != 0 || scene_asset_filter.Length != 0) {
                // Escape clears whichever search is narrowing a list before it does anything else.
                scene_filter_edit.reset(string.Empty);
                scene_filter_applied = string.Empty;
                scene_asset_edit.reset(string.Empty);
                scene_asset_filter = string.Empty;
                rebuild_scene_assets();
            }
            return;
        }
        bool control = (value.modifiers & modifier.control) != 0;
        if (control) {
            if (value.key == key_code.z && undo_history?.try_undo() == true) {
                notice = "Undone";
            } else if (value.key == key_code.y && undo_history?.try_redo() == true) {
                notice = "Redone";
            } else if (value.key == key_code.d) {
                if (duplicate_selection()) { notice = "Duplicated"; }
            }
            return;
        }
        // The classic shell's other outliner keys: Delete removes the selection, F2 renames it in place.
        if (value.key == key_code.delete && delete_selected()) {
            notice = "Deleted";
        } else if (value.key == key_code.f2 && selection_state != null && document != null &&
                   document.find(selection_state.primary) is { } node) {
            begin_scene_edit(node, "name", node.name);
            scene_value_edit?.select_all();
        }
    }

    private bool scene_pointer_claims(float x, float y) {
        var hit = next_ui?.hit_test(x, y);
        if (hit == null) { return false; }
        string key = hit.key;
        // The viewport is the 3D scene itself: a press there belongs to the camera, not to the UI.
        return key != "viewport" && key != "root" && key.Length != 0;
    }

    private void publish_scene_pointer_claim() {
        bool over_ui = next_ui_pointer_known && scene_pointer_claims(next_ui_pointer_x, next_ui_pointer_y);
        if (over_ui == next_ui_pointer_claim) { return; }
        next_ui_pointer_claim = over_ui;
        editor_ui_api.set_pointer_claim(over_ui);
    }
}
