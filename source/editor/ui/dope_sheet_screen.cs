using System;
using System.Collections.Generic;
using System.Globalization;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// The animation editor's Dope Sheet drawn by the editor's own UI library instead of RmlUi. One
    /// frame is a fresh tree: a header, one row per animated channel with its keys on a shared
    /// time-to-screen transform, a transport bar and a status line. Pointer, wheel and key events reach
    /// the previous frame's tree through <see cref="dope_sheet_input"/>.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private static bool requested_ui_demo;
        private ui_context? next_ui;
        private string next_ui_asset = string.Empty;
        private bool next_ui_reported;
        /// <summary>The channel rows, virtualized: only the rows the sheet can show are built.</summary>
        private readonly ui_list_view channel_list = new();
        /// <summary>The menus of this frame. Rebuilt every frame because their enabled state follows it.</summary>
        private readonly List<ui_menu> next_ui_menus = [];
        /// <summary>Key of the menu whose entries are showing, or an empty string.</summary>
        private string next_ui_open_menu = string.Empty;
        private bool next_ui_popup_demo_applied;
        /// <summary>The modal dialog in front of everything, or null when none is open.</summary>
        private ui_dialog? next_ui_dialog;

        // Layout constants, logical pixels.
        private const float sheet_header_height = 30.0F;
        private const float sheet_ruler_height = 18.0F;
        private const float sheet_transport_height = 38.0F;
        private const float sheet_footer_height = 22.0F;
        private const float sheet_name_width = 190.0F;
        private const float sheet_max_painted_keys = 160.0F;
        /// <summary>The sheet never shrinks below its ruler, three rows and the transport.</summary>
        private const float sheet_min_height = sheet_ruler_height + ui_theme.row_height * 3.0F + sheet_transport_height + 20.0F;
        /// <summary>And the preview above it keeps at least this much room to show the scene.</summary>
        private const float viewport_min_height = 80.0F;
        /// <summary>Scrollbar plus its gap: the ruler is inset by this so times line up with the lanes.</summary>
        private const float sheet_scroll_width = 10.0F;

        /// <summary>
        /// Starts the demo the first time a frame runs: the animation editor's own transition is
        /// reused, so the Dope Sheet reads the same motion clip the classic Timeline reads.
        /// </summary>
        private void start_next_ui_demo() {
            if (next_ui == null) { return; }
            if (!animation_mode) {
                if (!select_first_animatable()) {
                    if (next_ui!.frame_index < 3) {
                        host_logger.write($"Editor: the next UI demo is waiting for an animatable placement (frame {next_ui!.frame_index})");
                    }
                    return;
                }
                var selected = selection_state?.primary ?? Guid.Empty;
                next_ui_asset = document?.find(selected)?.asset ?? string.Empty;
                host_logger.write($"Editor: next UI demo selected '{next_ui_asset}' at frame {next_ui!.frame_index}");
                if (enter_animation_mode() is { } enter_error) {
                    report_next_ui_failure($"could not enter the animation editor: {enter_error}");
                    return;
                }
            }
            if (animation_preview() == null) {
                if (next_ui_asset.Length == 0) { return; }
                if (open_animation_asset(next_ui_asset, 0) is { } open_error) {
                    report_next_ui_failure($"could not open '{next_ui_asset}': {open_error}");
                    return;
                }
            }
            if (timeline_clip == null) {
                load_timeline_clip();
            }
            if (name_edit == null) {
                reset_name_field(timeline_clip?.clip_name ?? string.Empty);
            }
            // A visual aid for capture runs: DEREN_UI_POPUP_DEMO=menu|dialog opens a popup on the first
            // frame, because a capture run cannot press a title. It never changes a normal run.
            if (!next_ui_popup_demo_applied) {
                next_ui_popup_demo_applied = true;
                switch (Environment.GetEnvironmentVariable("DEREN_UI_POPUP_DEMO") ?? string.Empty) {
                case "menu":
                    next_ui_open_menu = "menu_file";
                    break;
                case "edit":
                    next_ui_open_menu = "menu_edit";
                    break;
                case "shortcuts":
                    show_shortcuts_dialog();
                    break;
                case "reload":
                    if (timeline_clip != null) { timeline_clip.dirty = true; }
                    request_reload_clip();
                    break;
                default:
                    break;
                }
            }
        }

        private void report_next_ui_failure(string message) {
            if (next_ui_reported) { return; }
            next_ui_reported = true;
            host_logger.write($"Editor: the next UI demo {message}");
        }

        private script_status run_next_ui_frame() {
            start_next_ui_demo();
            if (next_ui == null) { return script_status.success; }
            install_next_ui_input();
            process_next_ui_input();
            if (!editor_ui_api.try_query_viewport(out var width, out var height, out var scale)) {
                // The overlay has not prepared a frame yet, so there is no logical size to lay out in.
                return script_status.success;
            }
            next_ui.begin(width, height, scale);
            if (!next_ui.submit(build_dope_sheet())) {
                report_next_ui_failure("overlay rejected the Dope Sheet frame");
            }
            publish_ime_caret();
            publish_pointer_claim();
            if (requested_ui_selftest) {
                return ui_selftest_tick();
            }
            return script_status.success;
        }

        private ui_element build_dope_sheet() {
            var clip = timeline_clip;
            float time = 0.0F;
            float duration = 0.0F;
            bool playing = false;
            if (animation_preview_player() is { } node && animation_player.try_get_info(node, out var info) && info.available) {
                time = info.time;
                duration = info.duration;
                playing = info.playing;
            }
            if (duration <= 0.0F) { duration = clip?.duration ?? 0.0F; }
            string clip_name = next_ui_edit_name.Length > 0
                ? next_ui_edit_name
                : clip?.clip_name ?? (next_ui_asset.Length > 0 ? next_ui_asset : "No motion clip");
            string summary = clip == null
                ? "no motion clip"
                : string.Create(CultureInfo.InvariantCulture, $"{clip.channels.Count} channels, {clip.total_keys} keys");
            float available = MathF.Max(sheet_min_height + viewport_min_height,
                                        next_ui!.height - sheet_header_height - sheet_footer_height -
                                        ui_controls.splitter_thickness);
            // The user can drag the divider; the fraction is clamped so neither area ever collapses.
            float sheet_height = Math.Clamp(available * next_ui_sheet_fraction, sheet_min_height,
                                            MathF.Max(sheet_min_height, available - viewport_min_height));
            // The rows get a fixed slot so their content can never push the transport out of the sheet.
            float rows_height = MathF.Max(sheet_ruler_height + ui_theme.row_height + 4.0F,
                                          sheet_height - sheet_transport_height);
            next_ui_list_height = MathF.Max(channel_list.row_height,
                                            rows_height - 16.0F - sheet_ruler_height - 2.0F);
            // The pointer handlers run before this build, so they read the previous frame's transform.
            next_ui_duration = duration;
            build_menus(clip);

            var root = ui_node.column("root");
            root.add(build_sheet_header(clip_name, summary));
            // Nothing is painted here, so the scene behind the interface shows through above the sheet.
            root.add(ui_node.box("viewport").weight(1.0F));
            root.add(ui_controls.splitter("splitter", next_ui, next_ui_gesture == sheet_gesture.split));
            var sheet = ui_node.column("sheet").size(float.NaN, sheet_height).bg(ui_theme.window).clipped();
            sheet.add(build_sheet_rows(clip, rows_height, time, duration));
            sheet.add(build_sheet_transport(clip, clip_name, time, duration, playing));
            root.add(sheet);
            root.add(ui_node.row("footer").size(float.NaN, sheet_footer_height).bg(ui_theme.header)
                         .add(ui_node.box("footer_gap").size(12.0F, 0.0F),
                              centered(ui_node.label("footer_text",
                                                     notice.Length == 0 ? "Dope Sheet drawn by the editor UI library" : notice,
                                                     ui_theme.small_size, ui_theme.text_dim),
                                       float.NaN, sheet_footer_height)));
            add_popup_overlays(sheet_header_height);
            return root;
        }

        /// <summary>Vertically centers a label inside a fixed-height slot; the text stays left aligned.</summary>
        private static ui_element centered(ui_element label, float width, float height) {
            label.size(width, height);
            label.text_center = true;
            return label;
        }

        /// <summary>
        /// The editable name field of the header. It is the first real text field of the library: a
        /// caret snapped to grapheme boundaries, a selection anchor, one undo step per edit, and a
        /// composition preview that the input method drives through <see cref="ui_text_edit.set_preedit"/>.
        /// </summary>
        private ui_element build_name_field() {
            var field = ui_node.box("name_field").size(220.0F, sheet_header_height - 8.0F)
                                .bg(ui_theme.panel)
                                .stroke(name_edit != null && name_edit.focused ? ui_theme.accent : ui_theme.panel_border, 1.0F)
                                .pad(4.0F)
                                .painted(paint_name_field);
            return field;
        }

        private void paint_name_field(ui_paint paint, ui_rect area) {
            var edit = name_edit;
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
            if (edit.preedit.Length > 0) {
                float x = area.x + edit.caret_x(size);
                paint.add_text(x, text_y, size, ui_theme.key_selected, edit.preedit);
                var (preedit_width, _) = ui_paint.measure(edit.preedit, size);
                paint.add_rect(new ui_rect(x, area.y + area.height - 4.0F, preedit_width, 1.0F), 0.0F,
                               ui_theme.accent, 0U, 0.0F);
            }
            if (edit.focused) {
                float caret_x = area.x + edit.caret_x(size);
                paint.add_rect(new ui_rect(caret_x, area.y + 3.0F, 1.0F, area.height - 6.0F), 0.0F,
                               ui_theme.accent, 0U, 0.0F);
            }
            paint.pop_clip();
        }

        private ui_element build_sheet_header(string clip_name, string summary) {
            var header = ui_node.row("header").size(float.NaN, sheet_header_height).bg(ui_theme.header);
            float slot = sheet_header_height - 4.0F;
            string hint = name_edit != null && name_edit.focused
                ? "Enter applies, Esc cancels"
                : "click the name field to edit";
            header.add(ui_node.box("header_gap").size(12.0F, slot),
                       centered(ui_node.label("title", "Animation Editor", ui_theme.title_size, ui_theme.text_bright), float.NaN, slot),
                       ui_node.box("header_gap_title").size(10.0F, slot));
            foreach (var menu in next_ui_menus) {
                header.add(ui_popup.menu_title(menu, next_ui_open_menu == menu.key, next_ui, slot),
                           ui_node.box("header_gap_" + menu.key).size(2.0F, slot));
            }
            header.add(ui_node.box("header_gap_menus").size(8.0F, slot),
                       build_name_field(),
                       ui_node.box("header_gap_field").size(10.0F, slot),
                       centered(ui_node.label("hint", hint, ui_theme.small_size, ui_theme.text_dim), float.NaN, slot),
                       ui_node.spacer(),
                       centered(ui_node.label("clip", clip_name, ui_theme.body_size, ui_theme.text_key), float.NaN, slot),
                       ui_node.box("header_gap2").size(12.0F, slot),
                       centered(ui_node.label("summary", summary, ui_theme.small_size, ui_theme.text_dim), float.NaN, slot),
                       ui_node.box("header_gap3").size(12.0F, slot));
            return header;
        }

        private ui_element build_sheet_rows(motion_document? clip, float rows_height, float time, float duration) {
            var body = ui_node.column("rows").size(float.NaN, rows_height).pad(8.0F).spacing(2.0F).clipped();
            // The ruler shares the channel rows' two-column split so times line up with the keys below.
            var ruler = ui_node.row("ruler").size(float.NaN, sheet_ruler_height);
            ruler.add(centered(ui_node.label("ruler_name", "Channel", ui_theme.small_size, ui_theme.text_dim),
                               sheet_name_width, sheet_ruler_height));
            ruler.add(ui_node.box("ruler_track").weight(1.0F).size(float.NaN, sheet_ruler_height)
                          .painted((paint, area) => {
                              for (var division = 1; division < 10; ++division) {
                                  float x = area.x + area.width * division / 10.0F;
                                  paint.add_rect(new ui_rect(x, area.y + area.height - 6.0F, 1.0F, 6.0F), 0.0F,
                                                 ui_theme.outline, 0, 0);
                              }
                              paint.add_text(area.x, area.y, ui_theme.small_size, ui_theme.text_dim, "0.00 s");
                              paint.add_text(area.x + area.width * 0.82F, area.y, ui_theme.small_size, ui_theme.text_dim,
                                             duration.ToString("0.00", CultureInfo.InvariantCulture) + " s");
                          }));
            // The lanes stop short of the scrollbar, so the ruler is inset by the same amount.
            ruler.add(ui_node.box("ruler_scroll_gap").size(sheet_scroll_width - 4.0F, sheet_ruler_height));
            body.add(ruler);

            if (clip == null || clip.channels.Count == 0) {
                body.add(centered(ui_node.label("empty", "Open an asset with a motion clip to see its channels",
                                                ui_theme.body_size, ui_theme.text_dim),
                                  float.NaN, ui_theme.row_height));
                return body;
            }

            // Only the rows inside the visible window exist; the rest of the clip is a scroll offset.
            body.add(channel_list.build("channels", next_ui_list_height, clip.channels.Count,
                                        index => build_channel_row(clip, index, duration, time)));
            return body;
        }

        /// <summary>
        /// One channel as a row: its name in the left column and its keys on the shared lane. The row
        /// key carries the absolute channel index, which is what the pointer handling parses back out.
        /// </summary>
        private ui_element build_channel_row(motion_document clip, int index, float duration, float time) {
            var channel = clip.channels[index];
            bool selected = index == timeline_selected_channel;
            int chosen_key = selected ? next_ui_selected_key : -1;
            var row = ui_node.row($"channel_{index}").size(float.NaN, channel_list.row_height)
                           .bg(selected ? ui_theme.row_selected : index % 2 == 0 ? ui_theme.row_even : ui_theme.row_odd,
                               3.0F);
            row.add(centered(ui_node.label($"channel_name_{index}", channel.node_name, ui_theme.body_size,
                                           selected ? ui_theme.text_bright : ui_theme.text_key),
                             sheet_name_width, channel_list.row_height));
            row.add(ui_node.box($"channel_keys_{index}").weight(1.0F).size(float.NaN, channel_list.row_height)
                        .painted((paint, area) =>
                                     paint_channel_lane(paint, area, channel, duration, time, selected, chosen_key)));
            return row;
        }

        /// <summary>
        /// Keys of one channel, placed by one time-to-screen transform shared by every lane, so a key at
        /// 1.00 s sits on the same column in every row. Dense channels paint every n-th key.
        /// </summary>
        private static void paint_channel_lane(ui_paint paint, ui_rect area, motion_key_channel channel, float duration,
                                               float time, bool selected, int chosen_key) {
            if (area.is_empty) { return; }
            for (var division = 1; division < 10; ++division) {
                float x = area.x + area.width * division / 10.0F;
                paint.add_rect(new ui_rect(x, area.y, 1.0F, area.height), 0.0F, ui_theme.grid, 0, 0);
            }
            if (duration <= 0.0F || channel.key_count == 0) { return; }
            float middle = area.y + area.height * 0.5F;
            int stride = Math.Max(1, (int)MathF.Ceiling(channel.key_count / sheet_max_painted_keys));
            var times = channel.times;
            for (var key = 0; key < times.Length && key < channel.key_count; key += stride) {
                float fraction = Math.Clamp(times[key] / duration, 0.0F, 1.0F);
                float x = area.x + (area.width - 5.0F) * fraction;
                paint.add_rect(new ui_rect(x, middle - 2.5F, 5.0F, 5.0F), 2.0F,
                               selected ? ui_theme.key_selected : ui_theme.key, 0, 0);
            }
            // The grabbed key is redrawn at full size on top of the strided dots, so the selection stays
            // visible even when its channel paints only every n-th key.
            if (chosen_key >= 0 && chosen_key < times.Length) {
                float fraction = Math.Clamp(times[chosen_key] / duration, 0.0F, 1.0F);
                float x = area.x + (area.width - 5.0F) * fraction;
                paint.add_rect(new ui_rect(x - 1.5F, middle - 4.0F, 8.0F, 8.0F), 3.0F, ui_theme.playhead, 0, 0);
            }
            float playhead_x = area.x + (area.width - 1.5F) * Math.Clamp(time / duration, 0.0F, 1.0F);
            paint.add_rect(new ui_rect(playhead_x, area.y, 1.5F, area.height), 0.0F, ui_theme.playhead, 0, 0);
        }

        private ui_element build_sheet_transport(motion_document? clip, string clip_name, float time, float duration,
                                                 bool playing) {
            var transport = ui_node.row("transport").size(float.NaN, sheet_transport_height).bg(ui_theme.panel)
                                    .spacing(6.0F).pad(6.0F);
            float button_height = sheet_transport_height - 12.0F;
            transport.add(ui_controls.button("play", playing ? "Pause" : "Play", ui_button_style.filled, next_ui, 74.0F,
                                             button_height));
            transport.add(ui_controls.button("restart", "Restart", ui_button_style.outlined, next_ui, 74.0F, button_height));
            transport.add(ui_controls.button("undo", "Undo", ui_button_style.subtle, next_ui, 58.0F, button_height));
            transport.add(ui_controls.button("redo", "Redo", ui_button_style.subtle, next_ui, 58.0F, button_height));
            transport.add(ui_controls.button("save", "Save Motion", ui_button_style.outlined, next_ui, 96.0F,
                                             button_height));
            transport.add(centered(ui_node.label("time",
                                                 string.Create(CultureInfo.InvariantCulture, $"{time:0.00} / {duration:0.00} s"),
                                                 ui_theme.small_size, ui_theme.text_key),
                                   96.0F, button_height));
            float fraction = duration > 0.0F ? Math.Clamp(time / duration, 0.0F, 1.0F) : 0.0F;
            transport.add(ui_node.box("scrub").weight(1.0F).size(float.NaN, button_height)
                              .painted((paint, area) => {
                                  float track_y = area.y + area.height * 0.5F - 3.0F;
                                  paint.add_rect(new ui_rect(area.x, track_y, area.width, 6.0F), 3.0F, ui_theme.row_odd,
                                                 ui_theme.outline, 1.0F);
                                  if (fraction > 0.0F) {
                                      paint.add_rect(new ui_rect(area.x, track_y, area.width * fraction, 6.0F), 3.0F,
                                                     ui_theme.accent, 0, 0);
                                  }
                                  float head = area.x + (area.width - 2.0F) * fraction;
                                  paint.add_rect(new ui_rect(head, area.y + 4.0F, 2.0F, area.height - 8.0F), 1.0F,
                                                 ui_theme.playhead, 0, 0);
                              }));
            transport.add(centered(ui_node.label("clip_name", clip == null ? clip_name : clip.clip_name,
                                                 ui_theme.small_size, ui_theme.text_dim),
                                   float.NaN, button_height));
            return transport;
        }
    }
}
