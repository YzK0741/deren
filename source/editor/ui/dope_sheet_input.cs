using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// Pointer and wheel handling for the Dope Sheet. With the classic shell skipped there is no RmlUi
    /// document to poll, so the editor takes the raw input events and routes them through the element
    /// tree it laid out on the previous frame.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private static readonly List<input_event> next_ui_events = [];
        private static bool next_ui_input_installed;
        private static readonly bool input_probe_enabled =
            Environment.GetEnvironmentVariable("DEREN_UI_INPUT_PROBE") == "1";
        private static int input_probe_count;

        private enum sheet_gesture {
            none,
            scrub,
            key,
            /// <summary>Dragging the divider between the preview and the sheet.</summary>
            split,
        }

        private sheet_gesture next_ui_gesture;
        private int next_ui_gesture_channel = -1;
        private int next_ui_gesture_key = -1;
        private ui_rect next_ui_gesture_area;
        private readonly List<motion_key_channel> next_ui_gesture_group = [];
        private List<float[]>? next_ui_before_times;
        private List<float[]>? next_ui_before_values;
        private int next_ui_selected_key = -1;
        /// <summary>Height of the channel list, remembered for the wheel because it runs before a build.</summary>
        private float next_ui_list_height = ui_theme.row_height;
        /// <summary>The sheet's share of the space below the header, dragged by the splitter.</summary>
        private float next_ui_sheet_fraction = 0.62F;
        private float next_ui_duration;
        private ui_text_edit? name_edit;
        private bool next_ui_ime_installed;
        private bool next_ui_caret_published;
        // The pointer's last logical position, and what this screen last told the window seam about
        // it: a move is what makes the claim change, and only a change is published.
        private float next_ui_pointer_x;
        private float next_ui_pointer_y;
        private bool next_ui_pointer_known;
        private bool next_ui_pointer_claim;
        private string next_ui_edit_name = string.Empty;
        private string next_ui_edit_applied = string.Empty;

        private static void on_next_ui_input(input_event value) {
            if (next_ui_events.Count >= 256) {
                return;
            }
            next_ui_events.Add(value);
        }

        /// <summary>Takes the raw events once; the editor UI is the only listener in this mode.</summary>
        private void install_next_ui_input() {
            if (next_ui_input_installed) {
                return;
            }
            if (input_events.try_set_callback(on_next_ui_input, out var status)) {
                next_ui_input_installed = true;
            } else {
                host_logger.write($"Editor: the Dope Sheet could not install its input callback ({status})");
            }
        }

        /// <summary>Applies this frame's pointer, wheel and key events to the selection and the clip.</summary>
        private void process_next_ui_input() {
            if (next_ui_events.Count == 0) {
                return;
            }
            if (input_probe_enabled && input_probe_count < 200) {
                foreach (var probe in next_ui_events) {
                    if (++input_probe_count > 200) { break; }
                    string hit_key = next_ui?.hit_test(probe.x, probe.y)?.key ?? "none";
                    string field = name_edit is { focused: true } edit
                        ? string.Create(CultureInfo.InvariantCulture, $" field caret {edit.caret} of '{edit.text}'")
                        : string.Empty;
                    host_logger.write(string.Create(
                        CultureInfo.InvariantCulture,
                        $"Editor probe: {probe.type} at {probe.x:0.0},{probe.y:0.0} down {probe.down} hit '{hit_key}'{field}"));
                }
            }
            foreach (var value in next_ui_events) {
                switch (value.type) {
                    case input_type.mouse_move:
                        next_ui_pointer_x = value.x;
                        next_ui_pointer_y = value.y;
                        next_ui_pointer_known = true;
                        next_ui?.set_pointer(value.x, value.y);
                        if (next_ui_gesture == sheet_gesture.split) {
                            update_sheet_split(value.y);
                        } else if (next_ui_gesture != sheet_gesture.none) {
                            update_next_ui_gesture(value.x);
                        }
                        break;
                    case input_type.mouse_button when value.button == 0:
                        next_ui_pointer_x = value.x;
                        next_ui_pointer_y = value.y;
                        next_ui_pointer_known = true;
                        next_ui?.set_pointer(value.x, value.y);
                        if (value.down) {
                            press_next_ui(value.x, value.y);
                        } else {
                            release_next_ui();
                        }
                        break;
                    case input_type.scroll:
                        scroll_next_ui(value.wheel, value.x, value.y);
                        break;
                    case input_type.character when value.down:
                        if (name_edit != null && name_edit.focused) {
                            name_edit.insert(value.codepoint);
                        }
                        break;
                    case input_type.key when value.down && name_edit != null && name_edit.focused:
                        handle_name_field_key(value);
                        break;
                    case input_type.key when value.down && value.key == key_code.escape: {
                        if (next_ui_dialog != null) {
                            next_ui_dialog = null;
                        } else if (next_ui_open_menu.Length > 0) {
                            next_ui_open_menu = string.Empty;
                        } else {
                            cancel_next_ui_gesture();
                        }
                        break;
                    }
                    case input_type.key when value.down && value.key == key_code.space &&
                                              next_ui_dialog == null && next_ui_open_menu.Length == 0:
                        toggle_next_ui_play();
                        break;
                }
            }
            next_ui_events.Clear();
        }

        /// <summary>
        /// The caret's box in logical pixels, or false when no field has the focus. The app converts to
        /// client and then screen pixels, which is the seam's job.
        /// </summary>
        private bool try_ime_caret_rect(ui_element? root, out float x, out float y, out float width, out float height) {
            x = 0.0F;
            y = 0.0F;
            width = 0.0F;
            height = 0.0F;
            var edit = name_edit;
            var field = root?.find("name_field");
            if (edit == null || !edit.focused || field == null) {
                return false;
            }
            float text_size = ui_theme.body_size;
            var metrics = ui_paint.measure("M", text_size);
            var preedit_prefix = edit.preedit.Length == 0
                ? string.Empty
                : edit.preedit[..Math.Clamp(edit.preedit_caret, 0, edit.preedit.Length)];
            var preedit_metrics = ui_paint.measure(preedit_prefix, text_size);
            x = field.rect.x + 4.0F + edit.caret_x(text_size) + preedit_metrics.width;
            y = field.rect.y + MathF.Max(0.0F, (field.rect.height - metrics.height) * 0.5F);
            width = 1.5F;
            height = metrics.height;
            return true;
        }

        /// <summary>
        /// Publishes the caret's box so the candidate window can point at it, and clears it when the
        /// field loses the focus instead of leaving the list where it was.
        /// </summary>
        private void publish_ime_caret() {
            if (try_ime_caret_rect(next_ui?.last_tree, out var x, out var y, out var width, out var height)) {
                next_ui_caret_published = editor_ui_api.set_ime_caret_rect(x, y, width, height);
            } else if (next_ui_caret_published) {
                next_ui_caret_published = false;
                editor_ui_api.set_ime_caret_rect(0.0F, 0.0F, 0.0F, 0.0F);
            }
        }

        /// <summary>
        /// Tells the window seam whether the pointer is over one of this screen's own controls. The
        /// seam gates only the camera copy of a pointer event, so the preview above the Dope Sheet
        /// keeps orbiting while a drag that started on the name field, a channel row or a transport
        /// button never spins the model. `viewport` is the preview itself and `root` is the whole
        /// window, so neither claims.
        /// </summary>
        private void publish_pointer_claim() {
            bool over_ui = next_ui_pointer_known && pointer_claims(next_ui_pointer_x, next_ui_pointer_y);
            if (over_ui == next_ui_pointer_claim) {
                return;
            }
            next_ui_pointer_claim = over_ui;
            editor_ui_api.set_pointer_claim(over_ui);
        }

        /// <summary>
        /// Whether a pointer at this logical position belongs to this screen's controls. An in-flight
        /// gesture claims wherever the pointer went, so dragging a key past the sheet edge keeps the
        /// camera still; `viewport` is the preview itself and `root` is the whole window, so neither
        /// claims and the preview above the sheet keeps orbiting.
        /// </summary>
        private bool pointer_claims(float x, float y) {
            if (next_ui_gesture != sheet_gesture.none) {
                return true;
            }
            var hit = next_ui?.hit_test(x, y);
            return hit != null && hit.key is not ("root" or "viewport");
        }

        /// <summary>
        /// Registry point for the composition handler. The window seam calls it on the render thread
        /// between frames; with no field focused the event is dropped so typing in a shortcut context
        /// never leaks into a field.
        /// </summary>
        private void install_next_ui_ime() {
            if (next_ui_ime_installed) { return; }
            next_ui_ime_installed = true;
            if (!editor_ui_api.try_set_ime_handler(&on_next_ui_ime, null)) {
                host_logger.write("Editor: the name field could not register its composition handler");
            }
        }

        [UnmanagedCallersOnly]
        private static void on_next_ui_ime(void* opaque, editor_ui_ime_event_layout* value) {
            _ = opaque;
            var host = instance;
            if (host?.name_edit is not { } edit || !edit.focused || value == null) { return; }
            var bytes = value->text.length == 0 || value->text.data == null
                ? string.Empty
                : Encoding.UTF8.GetString(value->text.data, (int)value->text.length);
            switch (value->kind) {
                case 0U:
                    edit.set_preedit(string.Empty, 0);
                    return;
                case 1U:
                    edit.set_preedit(bytes, value->caret);
                    return;
                case 2U:
                    if (bytes.Length != 0) {
                        edit.insert_run(bytes);
                    }
                    edit.clear_preedit();
                    return;
                case 3U:
                    edit.clear_preedit();
                    return;
            }
        }

        /// <summary>Resets the field for a fresh clip and makes sure the composition handler exists.</summary>
        private void reset_name_field(string name) {
            name_edit ??= new ui_text_edit();
            name_edit.reset(name);
            next_ui_edit_name = name;
            next_ui_edit_applied = name;
            install_next_ui_ime();
        }

        /// <summary>
        /// Gives the name field the keyboard focus and puts the caret under the pointer. The hit rect is
        /// in logical pixels and the caret index comes from the shaped run, so a click lands between the
        /// same two characters at any scale.
        /// </summary>
        private void focus_name_field(ui_rect content, float x) {
            if (name_edit == null) { return; }
            name_edit.focused = true;
            if (next_ui != null) { next_ui.focused_key = "name_field"; }
            name_edit.move_to(name_edit.caret_at(x - content.x, ui_theme.body_size), false);
        }

        /// <summary>Commits the field: the typed name becomes the clip's name for the next save.</summary>
        private void apply_name_field() {
            if (name_edit == null) { return; }
            name_edit.focused = false;
            name_edit.clear_preedit();
            if (next_ui != null && next_ui.focused_key == "name_field") { next_ui.focused_key = string.Empty; }
            next_ui_edit_name = name_edit.text;
            if (next_ui_edit_name == next_ui_edit_applied) {
                notice = string.Empty;
                return;
            }
            if (timeline_clip != null) {
                timeline_clip.clip_name = next_ui_edit_name;
                timeline_clip.dirty = true;
            }
            notice = $"Clip name set to '{next_ui_edit_name}'; Save Motion writes it into the motion file";
        }

        /// <summary>Throws the edit away and restores the staged name.</summary>
        private void cancel_name_field() {
            if (name_edit == null) { return; }
            name_edit.focused = false;
            name_edit.clear_preedit();
            name_edit.reset(next_ui_edit_name);
            if (next_ui != null && next_ui.focused_key == "name_field") { next_ui.focused_key = string.Empty; }
        }

        /// <summary>
        /// Keys of the focused field. Control shortcuts ask the system clipboard and the field's own
        /// history, so the shortcuts work on the editor's data and nothing else.
        /// </summary>
        private void handle_name_field_key(input_event value) {
            var edit = name_edit;
            if (edit == null) {
                return;
            }
            handle_text_field_key(edit, value, apply_name_field, cancel_name_field);
        }

        /// <summary>
        /// The key mapping every text field in the editor shares: caret and word motion, Home/End,
        /// Backspace and Delete, Enter to apply, Escape to cancel, and the clipboard and undo chords.
        /// </summary>
        private static void handle_text_field_key(ui_text_edit edit, input_event value, Action apply, Action cancel) {
            bool control = (value.modifiers & modifier.control) != 0;
            bool shift = (value.modifiers & modifier.shift) != 0;
            switch (value.key) {
                case key_code.left:
                    edit.move(-1, shift, control);
                    return;
                case key_code.right:
                    edit.move(1, shift, control);
                    return;
                case key_code.home:
                    edit.move_home(shift);
                    return;
                case key_code.end:
                    edit.move_end(shift);
                    return;
                case key_code.backspace:
                    edit.backspace();
                    return;
                case key_code.delete:
                    edit.delete_forward();
                    return;
                case key_code.enter:
                    apply();
                    return;
                case key_code.escape:
                    cancel();
                    return;
            }
            if (!control) {
                return;
            }
            switch (value.key) {
                case key_code.a:
                    edit.select_all();
                    return;
                case key_code.c:
                    ui_clipboard.write(edit.copy(cut: false));
                    return;
                case key_code.x:
                    ui_clipboard.write(edit.copy(cut: true));
                    return;
                case key_code.v:
                    edit.insert_run(ui_clipboard.read());
                    return;
                case key_code.z:
                    if (shift) {
                        edit.try_redo();
                    } else {
                        edit.try_undo();
                    }
                    return;
                case key_code.y:
                    edit.try_redo();
                    return;
            }
        }

        /// <summary>
        /// Wheel over the channel rows scrolls them, one row per step; a wheel anywhere else belongs to
        /// whoever else wants it, so the preview above the sheet keeps zooming the camera.
        /// </summary>
        private void scroll_next_ui(float wheel, float x, float y) {
            var clip = timeline_clip;
            if (clip == null || clip.channels.Count <= 1) {
                return;
            }
            if (!over_channel_rows(x, y)) {
                return;
            }
            channel_list.scroll(clip.channels.Count, next_ui_list_height, wheel);
        }

        /// <summary>Whether a logical point is over the rows: the list, a row, or a lane inside one.</summary>
        private bool over_channel_rows(float x, float y) {
            var hit = next_ui?.hit_test(x, y);
            return hit != null && hit.key.StartsWith("channel", StringComparison.Ordinal);
        }

        private void press_next_ui(float x, float y) {
            next_ui_gesture = sheet_gesture.none;
            var hit = next_ui?.hit_test(x, y);
            if (hit == null) {
                return;
            }
            string key = hit.key;
            // A dialog and an open menu take the pointer before the screen behind them does: that is what
            // makes them modal without any flag on the widgets.
            if (handle_popup_press(key)) {
                return;
            }
            if (find_menu(key) != null) {
                if (name_edit != null && name_edit.focused) {
                    apply_name_field();
                }
                next_ui_open_menu = next_ui_open_menu == key ? string.Empty : key;
                return;
            }
            if (key == "name_field") {
                focus_name_field(hit.content, x);
                return;
            }
            // Clicking anywhere else takes the focus away and keeps whatever was typed.
            if (name_edit != null && name_edit.focused) {
                apply_name_field();
            }
            if (key == "splitter") {
                next_ui_gesture = sheet_gesture.split;
                update_sheet_split(y);
                return;
            }
            if (key == "play") {
                toggle_next_ui_play();
                return;
            }
            if (key == "restart") {
                seek_next_ui(0.0F);
                return;
            }
            if (key == "save") {
                save_next_ui_motion();
                return;
            }
            if (key == "undo") {
                next_ui_history_step(undo: true);
                return;
            }
            if (key == "redo") {
                next_ui_history_step(undo: false);
                return;
            }
            if (key == "scrub") {
                next_ui_gesture = sheet_gesture.scrub;
                next_ui_gesture_area = hit.content;
                update_next_ui_gesture(x);
                return;
            }
            if (!key.StartsWith("channel_", StringComparison.Ordinal)) {
                return;
            }
            int separator = key.LastIndexOf('_');
            if (separator < 0 || !int.TryParse(key[(separator + 1)..], NumberStyles.Integer,
                                                CultureInfo.InvariantCulture, out var index)) {
                return;
            }
            var clip = timeline_clip;
            if (clip == null || index < 0 || index >= clip.channels.Count) {
                return;
            }
            timeline_selected_channel = index;
            next_ui_selected_key = -1;
            var channel = clip.channels[index];
            if (!key.StartsWith("channel_keys_", StringComparison.Ordinal)) {
                seek_next_ui(channel.start_time);
                return;
            }
            // A press on the key lane either grabs the nearest key or scrubs the timeline there.
            next_ui_gesture_area = hit.content;
            float hit_time = lane_time(hit.content, x);
            int nearest = nearest_key(channel, hit.content, x, this.next_ui_duration);
            if (nearest < 0) {
                next_ui_gesture = sheet_gesture.scrub;
                seek_next_ui(hit_time);
                return;
            }
            next_ui_selected_key = nearest;
            next_ui_gesture = sheet_gesture.key;
            next_ui_gesture_channel = index;
            next_ui_gesture_key = nearest;
            next_ui_gesture_group.Clear();
            next_ui_gesture_group.AddRange(time_group(clip, channel));
            next_ui_before_times = [];
            next_ui_before_values = [];
            foreach (var member in next_ui_gesture_group) {
                next_ui_before_times.Add((float[])member.times.Clone());
                next_ui_before_values.Add((float[])member.values.Clone());
            }
            seek_next_ui(channel.times[nearest]);
        }

        private void update_next_ui_gesture(float x) {
            var clip = timeline_clip;
            if (clip == null) {
                return;
            }
            if (next_ui_gesture == sheet_gesture.scrub) {
                seek_next_ui(lane_time(next_ui_gesture_area, x));
                return;
            }
            if (next_ui_gesture_channel < 0 || next_ui_gesture_channel >= clip.channels.Count) {
                return;
            }
            var channel = clip.channels[next_ui_gesture_channel];
            if (next_ui_gesture_key < 0 || next_ui_gesture_key >= channel.times.Length) {
                return;
            }            float duration = this.next_ui_duration > 0.0F ? this.next_ui_duration : clip.duration;
            if (duration <= 0.0F) {
                return;
            }
            float time = lane_time(next_ui_gesture_area, x);
            set_group_key_time(next_ui_gesture_group, next_ui_gesture_key, time);
            clip.dirty = true;
            seek_next_ui(time);
        }

        /// <summary>
        /// Drags the divider: the sheet takes everything below the pointer down to the footer, clamped so
        /// the preview and the sheet both keep a usable height at any window size.
        /// </summary>
        private void update_sheet_split(float y) {
            if (next_ui == null) {
                return;
            }
            float available = MathF.Max(sheet_min_height + viewport_min_height,
                                        next_ui.height - sheet_header_height - sheet_footer_height -
                                        ui_controls.splitter_thickness);
            float low = sheet_min_height / available;
            float high = 1.0F - viewport_min_height / available;
            if (low > high) { high = low; }
            float sheet_height = next_ui.height - sheet_footer_height - y;
            next_ui_sheet_fraction = Math.Clamp(sheet_height / available, low, high);
        }

        private void release_next_ui() {
            if (next_ui_gesture != sheet_gesture.key) {
                next_ui_gesture = sheet_gesture.none;
                return;
            }
            var clip = timeline_clip;
            int key = next_ui_gesture_key;
            next_ui_gesture = sheet_gesture.none;
            var group = new List<motion_key_channel>(next_ui_gesture_group);
            next_ui_gesture_group.Clear();
            var before_times = next_ui_before_times;
            var before_values = next_ui_before_values;
            next_ui_before_times = null;
            next_ui_before_values = null;
            if (clip == null || group.Count == 0 || before_times == null || before_values == null ||
                key >= before_times[0].Length) {
                return;
            }
            var channel = group[0];
            if (MathF.Abs(before_times[0][key] - channel.times[key]) < 0.0005F) {
                return;
            }
            float moved_time = channel.times[key];
            // A glTF sampler's times must be strictly increasing, so landing exactly on a key of the
            // same channel cannot be saved. The move is refused before it enters the history.
            for (var index = 0; index < group.Count; ++index) {
                var member = group[index];
                for (var other = 0; other < member.times.Length; ++other) {
                    if (other == key || MathF.Abs(member.times[other] - moved_time) > 0.0005F) {
                        continue;
                    }
                    apply_key_arrays(clip, group, before_times, before_values, "Move key back");
                    notice = string.Create(
                        CultureInfo.InvariantCulture,
                        $"A key already sits at {member.times[other]:0.00} s, and a glTF sampler needs strictly increasing times; the move was cancelled");
                    return;
                }
            }
            for (var index = 0; index < group.Count; ++index) {
                if (!clip.sort_channel(group[index], out var error)) {
                    notice = $"Could not order the keys after the move: {error}";
                    return;
                }
            }
            var after_times = new List<float[]>();
            var after_values = new List<float[]>();
            foreach (var member in group) {
                after_times.Add((float[])member.times.Clone());
                after_values.Add((float[])member.values.Clone());
            }
            undo_history?.push(new delegate_undo_command("Move key",
                () => apply_key_arrays(clip, group, before_times, before_values, "Move key back"),
                () => apply_key_arrays(clip, group, after_times, after_values, "Move key")));
            notice = string.Create(CultureInfo.InvariantCulture,
                                   $"Moved {channel.node_name} key to {moved_time:0.00} s");
        }

        private bool apply_key_arrays(motion_document clip, List<motion_key_channel> group, List<float[]> times,
                                      List<float[]> values, string label) {
            if (times.Count != group.Count || values.Count != group.Count) {
                return false;
            }
            for (var index = 0; index < group.Count; ++index) {
                var channel = group[index];
                if (times[index].Length != channel.times.Length || values[index].Length != channel.values.Length) {
                    return false;
                }
                channel.times = (float[])times[index].Clone();
                channel.values = (float[])values[index].Clone();
                if (!clip.write_channel(channel, out var error)) {
                    notice = $"Could not {label.ToLowerInvariant()}: {error}";
                    return false;
                }
            }
            clip.dirty = true;
            return true;
        }

        private void cancel_next_ui_gesture() {
            if (next_ui_gesture != sheet_gesture.key) {
                next_ui_gesture = sheet_gesture.none;
                return;
            }
            var clip = timeline_clip;
            next_ui_gesture = sheet_gesture.none;
            var group = new List<motion_key_channel>(next_ui_gesture_group);
            next_ui_gesture_group.Clear();
            var before_times = next_ui_before_times;
            var before_values = next_ui_before_values;
            next_ui_before_times = null;
            next_ui_before_values = null;
            if (clip == null || before_times == null || before_values == null || group.Count == 0) {
                return;
            }
            apply_key_arrays(clip, group, before_times, before_values, "Cancel the key move");
            notice = "Cancelled the key move";
        }

        private void save_next_ui_motion() {
            var clip = timeline_clip;
            if (clip == null) {
                notice = "There is no motion clip to save";
                return;
            }
            if (clip.try_save(out var error)) {
                next_ui_edit_applied = clip.clip_name;
                notice = $"Saved {clip.clip_name} to {clip.path}";
            } else {
                notice = $"Could not save the motion: {error}";
            }
        }

        private void next_ui_history_step(bool undo) {
            var history = undo_history;
            if (history == null) {
                return;
            }
            if (undo) {
                notice = history.try_undo() ? "Undid the last edit" : "Nothing to undo";
            } else {
                notice = history.try_redo() ? "Redid the last edit" : "Nothing to redo";
            }
        }

        private void toggle_next_ui_play() {
            if (animation_preview_player() is not { } node || !animation_player.try_get_info(node, out var info) ||
                !info.available) {
                return;
            }
            if (info.playing) {
                animation_player.try_pause(node);
            } else {
                animation_player.try_play(node);
            }
        }

        private void seek_next_ui(float seconds) {
            if (animation_preview_player() is { } node) {
                float duration = this.next_ui_duration > 0.0F ? this.next_ui_duration : timeline_clip?.duration ?? 0.0F;
                animation_player.try_seek(node, Math.Clamp(seconds, 0.0F, MathF.Max(0.0F, duration)));
            }
        }

        /// <summary>
        /// The channels whose key times are one array in the file. Moving a key of any of them moves the
        /// same key of the others, otherwise a save would write two different times into one accessor.
        /// </summary>
        private static List<motion_key_channel> time_group(motion_document clip, motion_key_channel channel) {
            var group = new List<motion_key_channel>();
            foreach (var candidate in clip.channels) {
                if (candidate.time_accessor_index == channel.time_accessor_index) {
                    group.Add(candidate);
                }
            }
            if (group.Count == 0) {
                group.Add(channel);
            }
            return group;
        }

        private static void set_group_key_time(List<motion_key_channel> group, int key, float time) {
            foreach (var member in group) {
                if (key < member.times.Length) {
                    member.times[key] = time;
                }
            }
        }

        private float lane_time(ui_rect area, float x) {
            float duration = this.next_ui_duration > 0.0F ? this.next_ui_duration : timeline_clip?.duration ?? 0.0F;
            if (area.width <= 5.0F || duration <= 0.0F) {
                return 0.0F;
            }
            float fraction = Math.Clamp((x - area.x) / (area.width - 5.0F), 0.0F, 1.0F);
            return fraction * duration;
        }

        /// <summary>The key whose painted square is closest to the pointer, or -1 past the grab radius.</summary>
        private static int nearest_key(motion_key_channel channel, ui_rect area, float x, float duration) {
            if (channel.key_count == 0 || area.width <= 5.0F || duration <= 0.0F) {
                return -1;
            }
            int best = -1;
            float best_distance = 6.0F;
            for (var key = 0; key < channel.times.Length; ++key) {
                float fraction = Math.Clamp(channel.times[key] / duration, 0.0F, 1.0F);
                float key_x = area.x + (area.width - 5.0F) * fraction;
                float distance = MathF.Abs(key_x + 2.5F - x);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = key;
                }
            }
            return best;
        }
    }
}
