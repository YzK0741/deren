using System;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using deren.scripting;

namespace deren.editor {
    /// <summary>
    /// Drives the Dope Sheet through real pointer handlers on a copy of the motion files, so the
    /// selection, the drag, the undo history and the save round trip are checked without a mouse.
    /// The project's own motion files are never written: the test edits a temporary copy.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private static bool requested_ui_selftest;
        private int ui_selftest_phase;
        private int ui_selftest_frame;
        private string ui_selftest_directory = string.Empty;
        private motion_document? ui_selftest_original_clip;
        private int ui_selftest_channel;
        private int ui_selftest_key;
        private float ui_selftest_key_time;
        private float ui_selftest_moved_time;
        private float ui_selftest_preview_time;
        private uint ui_selftest_atlas_size;
        private uint ui_selftest_atlas_glyphs;

        private script_status ui_selftest_tick() {
            if (!requested_ui_selftest) {
                return script_status.success;
            }
            if (++this.ui_selftest_frame < 4) {
                return script_status.success;
            }
            // The handlers work on the tree of the previous frame, so every phase runs one frame after
            // the state it reacts to was painted.
            var root = next_ui?.last_tree;
            var clip = timeline_clip;
            switch (this.ui_selftest_phase) {
                case 0:
                    this.ui_selftest_phase = 1;
                    return script_status.success;
                case 1: {
                    if (root == null || clip == null) {
                        return selftest_failure("the Dope Sheet painted no tree or loaded no clip");
                    }
                    if (clip.buffer_path == null) {
                        return selftest_failure("the motion clip's buffer is embedded, so the copy test cannot run");
                    }
                    // Edit a copy: the project's asset stays untouched even if the test moves keys.
                    this.ui_selftest_directory = Path.Combine(Path.GetTempPath(), "deren-ui-selftest");
                    Directory.CreateDirectory(this.ui_selftest_directory);
                    string motion_copy = Path.Combine(this.ui_selftest_directory, Path.GetFileName(clip.path));
                    string buffer_name = Path.GetFileName(clip.buffer_path);
                    File.Copy(clip.path, motion_copy, true);
                    File.Copy(clip.buffer_path, Path.Combine(this.ui_selftest_directory, buffer_name), true);
                    if (!motion_document.try_load(motion_copy, out var copy, out var copy_error)) {
                        return selftest_failure($"the motion copy did not load: {copy_error}");
                    }
                    this.ui_selftest_original_clip = clip;
                    timeline_clip = copy;
                    this.ui_selftest_phase = 2;
                    return script_status.success;
                }
                case 2: {
                    var refresh = next_ui!.last_tree?.find("channel_3");
                    if (refresh == null) {
                        return selftest_failure("the third channel row is missing");
                    }
                    press_next_ui(refresh.content.x + 10.0F, refresh.content.y + refresh.content.height * 0.5F);
                    if (timeline_selected_channel != 3) {
                        return selftest_failure(
                            $"clicking the fourth channel row selected channel {timeline_selected_channel}");
                    }
                    this.ui_selftest_phase = 3;
                    return script_status.success;
                }
                case 3: {
                    var lane = next_ui!.last_tree?.find("channel_keys_3");
                    var temp = timeline_clip;
                    if (lane == null || temp == null || temp.channels.Count <= 3) {
                        return selftest_failure("the fourth channel has no key lane");
                    }
                    var channel = temp.channels[3];
                    this.ui_selftest_channel = 3;
                    this.ui_selftest_key = channel.times.Length / 2;
                    this.ui_selftest_key_time = channel.times[this.ui_selftest_key];
                    float fraction = Math.Clamp(this.ui_selftest_key_time / MathF.Max(temp.duration, 0.0001F),
                                                0.0F, 1.0F);
                    float x = lane.content.x + (lane.content.width - 5.0F) * fraction + 2.5F;
                    float y = lane.content.y + lane.content.height * 0.5F;
                    press_next_ui(x, y);
                    if (next_ui_gesture != sheet_gesture.key || next_ui_selected_key < 0) {
                        return selftest_failure("clicking a key did not start a key drag");
                    }
                    this.ui_selftest_phase = 4;
                    return script_status.success;
                }
                case 4: {
                    var temp = timeline_clip;
                    var lane = next_ui!.last_tree?.find("channel_keys_3");
                    if (temp == null || lane == null) {
                        return selftest_failure("the key drag lost its lane");
                    }
                    var channel = temp.channels[this.ui_selftest_channel];
                    float x = lane.content.x + lane.content.width * 0.5F;
                    update_next_ui_gesture(x);
                    if (MathF.Abs(channel.times[this.ui_selftest_key] - this.ui_selftest_key_time) < 0.0005F) {
                        return selftest_failure("dragging the key did not move it");
                    }
                    if (!temp.dirty) {
                        return selftest_failure("moving a key did not mark the motion document dirty");
                    }
                    // The preview follows the gesture; the player applies a seek on the next frame, so
                    // the assertion happens in the following phase.
                    this.ui_selftest_preview_time = lane_time(lane.content, x);
                    release_next_ui();
                    if (undo_history == null || !undo_history.can_undo) {
                        return selftest_failure("a finished key drag pushed no undo step");
                    }
                    this.ui_selftest_moved_time = channel.times[this.ui_selftest_key];
                    this.ui_selftest_phase = 5;
                    return script_status.success;
                }
                case 5: {
                    // The seek requested by the drag reached the player this frame.
                    if (animation_preview_player() is { } preview &&
                        animation_player.try_get_info(preview, out var moved) && moved.available &&
                        MathF.Abs(moved.time - this.ui_selftest_preview_time) > 0.05F) {
                        return selftest_failure(string.Create(
                            CultureInfo.InvariantCulture,
                            $"the preview stayed at {moved.time:0.000} s while the key was dragged to {this.ui_selftest_preview_time:0.000} s"));
                    }
                    next_ui_history_step(undo: true);
                    var channel = timeline_clip!.channels[this.ui_selftest_channel];
                    if (Array.IndexOf(channel.times, this.ui_selftest_key_time) < 0) {
                        return selftest_failure("undoing the key drag did not restore the key time");
                    }
                    this.ui_selftest_phase = 6;
                    return script_status.success;
                }
                case 6: {
                    next_ui_history_step(undo: false);
                    var channel = timeline_clip!.channels[this.ui_selftest_channel];
                    if (Array.IndexOf(channel.times, this.ui_selftest_moved_time) < 0) {
                        return selftest_failure("redoing the key drag did not restore the moved key time");
                    }
                    this.ui_selftest_phase = 7;
                    return script_status.success;
                }
                case 7: {
                    var temp = timeline_clip!;
                    if (!temp.try_save(out var save_error)) {
                        return selftest_failure($"saving the motion copy failed: {save_error}");
                    }
                    if (temp.dirty) {
                        return selftest_failure("saving the motion copy did not clear its dirty flag");
                    }
                    if (!motion_document.try_load(temp.path, out var reloaded, out var reload_error)) {
                        return selftest_failure($"the saved motion copy did not reload: {reload_error}");
                    }
                    if (reloaded.channels.Count != temp.channels.Count) {
                        return selftest_failure(
                            $"the reloaded copy has {reloaded.channels.Count} channels instead of {temp.channels.Count}");
                    }
                    for (var index = 0; index < temp.channels.Count; ++index) {
                        var before = temp.channels[index];
                        var after = reloaded.channels[index];
                        if (before.times.Length != after.times.Length) {
                            return selftest_failure($"channel {index} reloaded with a different key count");
                        }
                        for (var key = 0; key < before.times.Length; ++key) {
                            if (MathF.Abs(before.times[key] - after.times[key]) > 0.0001F) {
                                return selftest_failure(string.Create(
                                    CultureInfo.InvariantCulture,
                                    $"channel {index} key {key} reloaded as {after.times[key]:0.0000} s instead of {before.times[key]:0.0000} s"));
                            }
                        }
                    }
                    host_logger.write(string.Create(
                        CultureInfo.InvariantCulture,
                        $"Editor selftest: the Dope Sheet selected, dragged, undid, redid and saved key {this.ui_selftest_key} of channel {this.ui_selftest_channel} ({this.ui_selftest_original_clip?.channels.Count ?? 0} channels reloaded)."));
                    timeline_clip = this.ui_selftest_original_clip;
                    notice = "Dope Sheet selftest passed";
                    this.ui_selftest_phase = 8;
                    return script_status.success;
                }
                case 8: {
                    // A second move, cancelled: Escape has to restore the key without leaving a step
                    // in the history.
                    var temp = timeline_clip;
                    var lane = next_ui!.last_tree?.find("channel_keys_3");
                    if (temp == null || lane == null || temp.channels.Count <= this.ui_selftest_channel) {
                        return selftest_failure("the cancelled-move phase lost its clip or lane");
                    }
                    float fraction = Math.Clamp(this.ui_selftest_moved_time / MathF.Max(temp.duration, 0.0001F),
                                                0.0F, 1.0F);
                    float press_x = lane.content.x + (lane.content.width - 5.0F) * fraction + 2.5F;
                    press_next_ui(press_x, lane.content.y + lane.content.height * 0.5F);
                    if (next_ui_gesture != sheet_gesture.key) {
                        return selftest_failure("the second key press did not start a drag");
                    }
                    update_next_ui_gesture(lane.content.x + lane.content.width * 0.25F);
                    cancel_next_ui_gesture();
                    var channel = temp.channels[this.ui_selftest_channel];
                    if (Array.IndexOf(channel.times, this.ui_selftest_moved_time) < 0) {
                        return selftest_failure("cancelling the second move did not restore the key time");
                    }
                    this.ui_selftest_phase = 9;
                    return script_status.success;
                }
                case 9: {
                    // The undo step still belongs to the first move: one undo must reach the original
                    // key time, so the cancelled gesture pushed nothing.
                    next_ui_history_step(undo: true);
                    var channel = timeline_clip!.channels[this.ui_selftest_channel];
                    if (Array.IndexOf(channel.times, this.ui_selftest_key_time) < 0) {
                        return selftest_failure(
                            "one undo after the cancelled move did not reach the original key time");
                    }
                    host_logger.write(
                        "Editor selftest: the Dope Sheet cancelled a second move without touching the history.");
                    this.ui_selftest_phase = 10;
                    return script_status.success;
                }
                case 10: {
                    // Resize and scale sweep: the layout is rebuilt at several logical sizes and
                    // densities, and nothing may leave the window or starve the transport bar.
                    (float width, float height, float scale)[] cases = [
                        (1280.0F, 720.0F, 1.0F),
                        (1024.0F, 600.0F, 1.0F),
                        (800.0F, 480.0F, 1.0F),
                        (1920.0F, 1080.0F, 1.0F),
                        (1280.0F, 720.0F, 1.5F),
                        (1024.0F, 600.0F, 1.5F),
                        (1066.0F, 600.0F, 1.5F),
                    ];
                    foreach (var (width, height, scale) in cases) {
                        if (verify_dope_sheet_layout(width, height, scale) is { } problem) {
                            return selftest_failure(problem);
                        }
                    }
                    host_logger.write(string.Create(
                        CultureInfo.InvariantCulture,
                        $"Editor selftest: the Dope Sheet layout stayed inside the window at {cases.Length} size and scale combinations."));
                    this.ui_selftest_phase = 11;
                    return script_status.success;
                }
                case 11: {
                    // Shaping: clusters stay byte offsets into the run, and the width the shaper reports
                    // is the width the painter measures, otherwise a caret would drift from its glyph.
                    if (verify_shaping() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: the shaper kept clusters and measured widths in agreement for Latin and CJK runs.");
                    this.ui_selftest_phase = 12;
                    return script_status.success;
                }
                case 12: {
                    // The text field: grapheme-snapped caret, selection, clipboard, one undo step per
                    // edit, the composition preview, and a caret that survives CJK shaping.
                    if (verify_text_edit() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: the name field edited, selected, copied, pasted and undid at grapheme boundaries.");
                    this.ui_selftest_phase = 13;
                    return script_status.success;
                }
                case 13: {
                    // The composition handler: the window seam's events reach the focused field as a
                    // preview that never edits, then as one committed run.
                    if (verify_ime_handler() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: the composition handler fed the focused name field a preview and one commit.");
                    this.ui_selftest_phase = 14;
                    return script_status.success;
                }
                case 14: {
                    // The save path: a CUBICSPLINE channel stores three elements per key, so the writer
                    // has to advance by element and not by key. A duplicate key time must be refused
                    // because a glTF sampler input has to be strictly increasing.
                    if (verify_cubic_save() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: a CUBICSPLINE channel kept its tangents through edit, save and reload, and duplicate key times were refused.");
                    this.ui_selftest_phase = 15;
                    return script_status.success;
                }
                case 15: {
                    // The gesture that would create a duplicate time: the release path has to refuse it,
                    // put the key back and push nothing onto the history.
                    if (verify_collision_gesture() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: a drag that ended on another key was cancelled, restored and left out of the history.");
                    this.ui_selftest_phase = 16;
                    return script_status.success;
                }
                case 16: {
                    // The candidate window anchor: the box has to sit inside the field, advance with
                    // the caret, and disappear when the field loses the focus.
                    if (verify_ime_caret() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: the candidate anchor followed the caret inside the name field and cleared with the focus.");
                    this.ui_selftest_phase = 17;
                    return script_status.success;
                }
                case 17: {
                    // Renaming the clip patches the JSON in place: the rest of the file stays byte for
                    // byte, and an animation without a name gets one inserted.
                    if (verify_clip_rename() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: renaming the clip rewrote only the animation name, and a missing name was inserted.");
                    this.ui_selftest_phase = 18;
                    return script_status.success;
                }
                case 18: {
                    // The pointer claim: a press over a control must keep the camera out of the drag,
                    // while the preview area above the sheet stays the camera's.
                    if (verify_pointer_claim() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: the pointer claim covered the sheet's controls and left the preview to the camera.");
                    this.ui_selftest_phase = 19;
                    return script_status.success;
                }
                case 19: {
                    // The click path end to end, minus the window: the same events the host publishes
                    // are pushed through the registered callback and must focus the name field.
                    if (verify_name_field_press() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: a window event press focused the name field at the click position.");
                    this.ui_selftest_phase = 20;
                    return script_status.success;
                }
                case 20: {
                    // A keystroke reaches the field as a character event. The window seam marks every
                    // typed character as a press, and the field inserts only on a press - so this also
                    // guards the bug where typing inserted nothing at all.
                    if (verify_character_typing() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: a published character event typed into the focused name field.");
                    this.ui_selftest_phase = 21;
                    return script_status.success;
                }
                case 21: {
                    // The virtualized channel list: only the rows inside the window exist as elements,
                    // the wheel moves the window by whole rows, and it never scrolls past the last one.
                    if (verify_virtual_list() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: the channel list built only its visible rows and stopped at its last row.");
                    this.ui_selftest_phase = 22;
                    return script_status.success;
                }
                case 22: {
                    // The splitter: a drag on the divider resizes the sheet, and both areas keep a
                    // usable height at the clamp.
                    if (verify_splitter_drag() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write("Editor selftest: dragging the splitter resized the sheet inside its clamp.");
                    this.ui_selftest_phase = 23;
                    return script_status.success;
                }
                case 23: {
                    // Menus and the modal layer: a title opens a dropped panel, an entry that needs a
                    // confirmation opens a dialog, the scrim keeps the press away from the screen behind
                    // it, and the confirm button actually does the work.
                    if (verify_menus() is { } problem) {
                        return selftest_failure(problem);
                    }
                    host_logger.write(
                        "Editor selftest: the menu opened, the dialog blocked the screen behind it and its button ran.");
                    this.ui_selftest_phase = 24;
                    return script_status.success;
                }
                case 24: {
                    // Distinct glyphs are what make the atlas grow: 700 of them at 44 logical pixels
                    // need more than the original 1024x1024 sheet, so the overlay has to double it
                    // instead of dropping the text nobody would see.
                    if (next_ui == null) { return selftest_failure("the atlas probe has no UI context"); }
                    if (!editor_ui_api.try_query_stats(out this.ui_selftest_atlas_size,
                                                       out this.ui_selftest_atlas_glyphs, out uint skipped)) {
                        return selftest_failure("the atlas report is unavailable");
                    }
                    if (skipped != 0U) {
                        return selftest_failure(string.Create(CultureInfo.InvariantCulture,
                                                              $"the atlas had already dropped {skipped} glyphs"));
                    }
                    var filling = new StringBuilder();
                    for (int index = 0; index < 700; ++index) {
                        filling.Append(char.ConvertFromUtf32(0x4E00 + index * 3));
                    }
                    var probe = ui_node.column("atlas_probe").size(400.0F, 300.0F);
                    probe.add(ui_node.label("atlas_probe_text", filling.ToString(), 44.0F, ui_theme.text_bright));
                    if (!next_ui.submit(probe)) { return selftest_failure("the atlas probe frame was refused"); }
                    this.ui_selftest_phase = 25;
                    return script_status.success;
                }
                case 25: {
                    // The native side rasterises during prepare, which happens after this update, so the
                    // report is read one frame later than the probe was submitted.
                    if (!editor_ui_api.try_query_stats(out uint size, out uint glyphs, out uint skipped)) {
                        return selftest_failure("the atlas report vanished");
                    }
                    if (size <= this.ui_selftest_atlas_size) {
                        return selftest_failure(string.Create(
                            CultureInfo.InvariantCulture,
                            $"the atlas stayed {size} pixels wide under 700 new glyphs ({this.ui_selftest_atlas_size})"));
                    }
                    if (skipped != 0U) {
                        return selftest_failure(string.Create(CultureInfo.InvariantCulture,
                                                              $"growing the atlas dropped {skipped} glyphs"));
                    }
                    if (glyphs < this.ui_selftest_atlas_glyphs + 600U) {
                        return selftest_failure(string.Create(
                            CultureInfo.InvariantCulture,
                            $"the atlas kept only {glyphs} glyphs after the probe ({this.ui_selftest_atlas_glyphs})"));
                    }
                    host_logger.write(string.Create(
                        CultureInfo.InvariantCulture,
                        $"Editor selftest: the glyph atlas doubled to {size} pixels and kept {glyphs} glyphs."));
                    this.ui_selftest_phase = 26;
                    return script_status.success;
                }
                default:
                    return script_status.success;
            }
        }

        /// <summary>
        /// Lays the Dope Sheet out at a synthetic size and density without submitting it, then checks
        /// that the transport still sits inside the window and that no element leaves the frame.
        /// </summary>
        private string? verify_dope_sheet_layout(float width, float height, float scale) {
            var context = next_ui;
            if (context == null) { return "the layout sweep has no UI context"; }
            context.begin(width, height, scale);
            var root = build_dope_sheet();
            ui_layout_engine.measure(root, width);
            ui_layout_engine.arrange(root, new ui_rect(0.0F, 0.0F, width, height));
            var transport = root.find("transport");
            var footer = root.find("footer");
            if (transport == null || footer == null) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the layout at {width:0}x{height:0}@{scale:0.0} lost its transport or footer");
            }
            if (transport.rect.bottom > height + 0.01F || transport.rect.y < 0.0F) {
                return string.Create(
                    CultureInfo.InvariantCulture,
                    $"the transport left the window at {width:0}x{height:0}@{scale:0.0} (y {transport.rect.y:0.0}, bottom {transport.rect.bottom:0.0})");
            }
            if (footer.rect.bottom > height + 0.01F) {
                return string.Create(
                    CultureInfo.InvariantCulture,
                    $"the footer left the window at {width:0}x{height:0}@{scale:0.0} (bottom {footer.rect.bottom:0.0})");
            }
            if (transport.rect.bottom > footer.rect.y + 0.01F) {
                return string.Create(
                    CultureInfo.InvariantCulture,
                    $"the transport overlapped the footer at {width:0}x{height:0}@{scale:0.0} (bottom {transport.rect.bottom:0.0} vs footer {footer.rect.y:0.0})");
            }
            var play = root.find("play");
            if (play != null && play.rect.right > width + 0.01F) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"a transport button overflowed the window at {width:0}x{height:0}@{scale:0.0}");
            }
            return null;
        }

        /// <summary>
        /// Shapes a Latin run, a CJK run and a mixed run, and checks the invariants a caret relies on:
        /// clusters are UTF-8 byte offsets into the whole run, they never decrease, the shaper reports
        /// the same advance the painter measures, and CJK code points really resolve through a fallback
        /// face instead of landing on glyph 0.
        /// </summary>
        private string? verify_shaping() {
            const float size = 13.0F;
            var latin = ui_text_shape.shape("Animation", size, out var latin_width);
            if (latin.Length == 0) { return "the shaper returned no glyphs for a Latin run"; }
            if (latin[0].cluster != 0) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the Latin run started at cluster {latin[0].cluster} instead of 0");
            }
            for (var i = 1; i < latin.Length; i++) {
                if (latin[i].cluster < latin[i - 1].cluster) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"Latin clusters went backwards at glyph {i} ({latin[i - 1].cluster} to {latin[i].cluster})");
                }
            }
            var latin_measured = ui_paint.measure("Animation", size);
            if (MathF.Abs(latin_measured.width - latin_width) > 0.25F) {
                return string.Create(
                    CultureInfo.InvariantCulture,
                    $"the shaper reported {latin_width:0.00} for a Latin run but the painter measured {latin_measured.width:0.00}");
            }
            // Seven Chinese characters: three bytes each, so the clusters are 0, 3, 6, ...
            const string cjk = "\u52A8\u753B\u7F16\u8F91\u5668\u5173\u952E";
            var cjk_run = ui_text_shape.shape(cjk, size, out var cjk_width);
            if (cjk_run.Length != 7) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"seven CJK code points shaped into {cjk_run.Length} glyphs");
            }
            for (var i = 0; i < cjk_run.Length; i++) {
                if (cjk_run[i].glyph == 0U) {
                    return string.Create(CultureInfo.InvariantCulture, $"CJK glyph {i} fell back to glyph 0");
                }
                if (cjk_run[i].cluster != (uint)(i * 3)) {
                    return string.Create(
                        CultureInfo.InvariantCulture,
                        $"CJK glyph {i} reported cluster {cjk_run[i].cluster} instead of {i * 3}");
                }
            }
            if (cjk_width < size * 6.0F) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"seven CJK glyphs measured only {cjk_width:0.00} at {size:0}");
            }
            // A mixed run keeps its byte offsets across the font switch: "Animation" is nine bytes.
            var mixed = ui_text_shape.shape("Animation\u52A8\u753B", size, out _);
            if (mixed.Length < 2) { return "the mixed run shaped into too few glyphs"; }
            var first_cjk = Array.FindIndex(mixed, value => value.cluster >= 9U);
            if (first_cjk < 0) { return "the mixed run lost its CJK cluster offset"; }
            if (mixed[first_cjk].cluster != 9U) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the mixed run put its first CJK glyph at cluster {mixed[first_cjk].cluster} instead of 9");
            }
            return null;
        }

        /// <summary>
        /// Exercises the text field the way a user drives it: type, move, select, copy, paste, undo,
        /// plus a CJK run and a composition preview. Every assertion is about observable state (text,
        /// caret, selection), never about pixels, so the test does not depend on the font.
        /// </summary>
        private string? verify_text_edit() {
            var edit = new ui_text_edit();
            edit.reset("Bibideba");
            edit.focused = true;
            if (edit.caret != "Bibideba".Length) { return "the field did not start with the caret at the end"; }
            if (!edit.insert('X')) { return "typing into the field was refused"; }
            if (edit.text != "BibidebaX") {
                return string.Create(CultureInfo.InvariantCulture, $"typing produced '{edit.text}'");
            }
            // One undo step removes the whole typed run and restores the caret.
            if (!edit.try_undo()) { return "the field refused to undo a typed character"; }
            if (edit.text != "Bibideba") {
                return string.Create(CultureInfo.InvariantCulture, $"undo produced '{edit.text}'");
            }
            if (edit.caret != 8) {
                return string.Create(CultureInfo.InvariantCulture, $"undo left the caret at {edit.caret} instead of 8");
            }
            // Selection and the clipboard: copy, cut, paste back as one step.
            edit.move_home(false);
            edit.move_end(true);
            var copied = edit.copy(cut: false);
            if (copied != "Bibideba" || !edit.has_selection) { return "copy lost the selection or the text"; }
            if (!ui_clipboard.write(copied)) { return "the system clipboard refused a write"; }
            // Another process can hold the clipboard for a few milliseconds, so the round trip retries
            // instead of turning a transient owner into a test failure.
            var read_back = ui_clipboard.read();
            for (var attempt = 0; attempt < 4 && read_back != copied; attempt++) {
                Thread.Sleep(5);
                ui_clipboard.write(copied);
                read_back = ui_clipboard.read();
            }
            if (read_back != copied) { return "the system clipboard did not return what was written"; }
            edit.move_end(false);
            edit.insert_run("-tail");
            if (!edit.insert_run(copied)) { return "pasting the clipboard text was refused"; }
            if (edit.text != "Bibideba-tailBibideba") {
                return string.Create(CultureInfo.InvariantCulture, $"paste produced '{edit.text}'");
            }
            if (!edit.try_undo()) { return "the field refused to undo a paste"; }
            if (edit.text != "Bibideba-tail") {
                return string.Create(CultureInfo.InvariantCulture, $"one undo removed too much or too little ('{edit.text}')");
            }
            // Combining mark: the caret must treat "e" plus an acute as one boundary.
            edit.reset("e\u0301a");
            if (ui_text_edit.boundaries(edit.text).Length != 3) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"a combining mark produced {ui_text_edit.boundaries(edit.text).Length} boundaries");
            }
            edit.move_home(false);
            edit.move(1, false, false);
            if (edit.caret != 2) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the caret stopped at {edit.caret} instead of past the combining mark");
            }
            // CJK: one code point, three bytes, one caret step.
            edit.reset("Animation");
            edit.move_end(false);
            if (!edit.insert(0x52A8U)) { return "typing a CJK code point was refused"; }
            if (edit.text != "Animation\u52A8") {
                return "the field did not append the CJK code point";
            }
            edit.move(-1, false, false);
            float before = edit.caret_x(13.0F);
            edit.move(1, false, false);
            float after = edit.caret_x(13.0F);
            if (!(after > before)) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the caret did not advance over a CJK glyph ({before:0.00} to {after:0.00})");
            }
            // The composition preview is shown but is not part of the text until it commits.
            edit.set_preedit("\u4F60\u597D", 2);
            if (edit.text != "Animation\u52A8") { return "the composition preview changed the text"; }
            edit.clear_preedit();
            if (edit.preedit.Length != 0) { return "the composition preview did not clear"; }
            edit.focused = false;
            return null;
        }

        /// <summary>
        /// Drives the composition handler the window seam calls: a preview message must not edit the
        /// text, and the commit must append the run in one undo step. The handler is invoked directly,
        /// so this covers the routing and the state machine; the Win32 side is documented separately
        /// because it needs a real IME.
        /// </summary>
        private string? verify_ime_handler() {
            var edit = new ui_text_edit();
            edit.reset("Clip");
            edit.focused = true;
            var previous = name_edit;
            name_edit = edit;
            var handler = (delegate* unmanaged<void*, editor_ui_ime_event_layout*, void>)&on_next_ui_ime;
            try {
                var preview = Encoding.UTF8.GetBytes("\u4F60\u597D");
                fixed (byte* preview_data = preview) {
                    var update = new editor_ui_ime_event_layout {
                        kind = 1U,
                        text = new utf8_view { data = preview_data, length = (uint)preview.Length },
                        caret = 2,
                    };
                    handler(null, &update);
                }
                if (edit.preedit != "\u4F60\u597D" || edit.preedit_caret != 2) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"the preview reached the field as '{edit.preedit}' with caret {edit.preedit_caret}");
                }
                if (edit.text != "Clip") { return "the preview edited the text"; }
                var commit = Encoding.UTF8.GetBytes("\u4F60\u597D");
                fixed (byte* commit_data = commit) {
                    var committed = new editor_ui_ime_event_layout {
                        kind = 2U,
                        text = new utf8_view { data = commit_data, length = (uint)commit.Length },
                    };
                    handler(null, &committed);
                }
                if (edit.text != "Clip\u4F60\u597D") {
                    return string.Create(CultureInfo.InvariantCulture, $"the commit produced '{edit.text}'");
                }
                if (edit.preedit.Length != 0) { return "the commit left the preview behind"; }
                if (!edit.try_undo()) { return "the commit did not form one undo step"; }
                if (edit.text != "Clip") {
                    return string.Create(CultureInfo.InvariantCulture, $"undoing the commit produced '{edit.text}'");
                }
            } finally {
                name_edit = previous;
            }
            return null;
        }

        // A two-key CUBICSPLINE clip: one VEC3 accessor holding in-tangent, value and out-tangent per
        // key, with a byte stride of 16 so the writer's element arithmetic is exercised.
        private const string cubic_fixture_json = """
        {
          "asset": { "version": "2.0" },
          "buffers": [ { "uri": "motion.bin", "byteLength": 104 } ],
          "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 8 },
            { "buffer": 0, "byteOffset": 8, "byteLength": 96, "byteStride": 16 }
          ],
          "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR" },
            { "bufferView": 1, "componentType": 5126, "count": 6, "type": "VEC3", "byteOffset": 0 }
          ],
          "nodes": [ { "name": "Bone" } ],
          "animations": [
            {
              "name": "Cubic Clip",
              "samplers": [ { "input": 0, "output": 1, "interpolation": "CUBICSPLINE" } ],
              "channels": [ { "sampler": 0, "target": { "node": 0, "path": "translation" } } ]
            }
          ]
        }
        """;

        /// <summary>
        /// Writes the fixture, edits it, saves it and reloads it. The expected floats are the authored
        /// ones in glTF order, so a writer that packs a key as one contiguous run fails on the second
        /// key's tangent, and one that ignores the stride fails on the padding.
        /// </summary>
        private string? verify_cubic_save() {
            var directory = Path.Combine(Environment.CurrentDirectory, "dynamic", "motion-cubic-selftest");
            Directory.CreateDirectory(directory);
            var json_path = Path.Combine(directory, "motion.gltf");
            var bin_path = Path.Combine(directory, "motion.bin");
            File.WriteAllText(json_path, cubic_fixture_json);
            File.WriteAllBytes(bin_path, cubic_fixture_bytes());
            if (!motion_document.try_load(json_path, out var clip, out var error) || clip == null) {
                return $"the CUBICSPLINE fixture did not load: {error}";
            }
            if (clip.channels.Count != 1) { return "the CUBICSPLINE fixture loaded no channel"; }
            var channel = clip.channels[0];
            if (channel.interpolation != "CUBICSPLINE") {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the fixture loaded as '{channel.interpolation}'");
            }
            float[] expected = [10, 11, 12, 1, 2, 3, 20, 21, 22, 30, 31, 32, 4, 5, 6, 40, 41, 42];
            if (!float_arrays_equal(channel.values, expected)) {
                return "the fixture's tangent blocks did not load in glTF order";
            }
            // Swap the two keys in memory, then let the writer put them back in time order.
            channel.times = [1.0F, 0.0F];
            var swapped = new float[expected.Length];
            Array.Copy(expected, 9, swapped, 0, 9);
            Array.Copy(expected, 0, swapped, 9, 9);
            channel.values = swapped;
            if (!clip.sort_channel(channel, out error)) {
                return $"the CUBICSPLINE channel could not be reordered: {error}";
            }
            if (!clip.try_save(out error)) { return $"saving the CUBICSPLINE channel failed: {error}"; }
            if (!motion_document.try_load(json_path, out var reloaded, out error) || reloaded == null) {
                return $"the saved CUBICSPLINE fixture did not reload: {error}";
            }
            var after = reloaded.channels[0];
            if (after.times.Length != 2 || after.times[0] != 0.0F || after.times[1] != 1.0F) {
                return "the saved key times are not the ordered pair";
            }
            if (!float_arrays_equal(after.values, expected)) {
                return "saving the CUBICSPLINE channel damaged its tangents";
            }
            // The bytes on disk must match the same layout, not just the in-memory arrays.
            var bytes = File.ReadAllBytes(bin_path);
            for (var element = 0; element < 6; ++element) {
                for (var part = 0; part < 3; ++part) {
                    var offset = 8 + element * 16 + part * sizeof(float);
                    var value = System.Buffers.Binary.BinaryPrimitives.ReadSingleLittleEndian(
                        bytes.AsSpan(offset, sizeof(float)));
                    if (value != expected[element * 3 + part]) {
                        return string.Create(
                            CultureInfo.InvariantCulture,
                            $"element {element} part {part} holds {value} instead of {expected[element * 3 + part]}");
                    }
                }
                var padding = bytes[8 + element * 16 + 12];
                if (padding != 0) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"element {element} overwrote the stride padding");
                }
            }
            // Two keys at the same time are not a legal sampler input, so the save must refuse.
            after.times = [0.0F, 0.0F];
            if (reloaded.try_save(out var collision_error)) {
                return "saving two keys at the same time was allowed";
            }
            if (!collision_error.Contains("strictly increasing", StringComparison.Ordinal)) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the duplicate-time refusal said '{collision_error}'");
            }
            return null;
        }

        /// <summary>The fixture buffer: two key times, then six strided VEC3 elements.</summary>
        private static byte[] cubic_fixture_bytes() {
            var bytes = new byte[104];
            System.Buffers.Binary.BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(0, sizeof(float)), 0.0F);
            System.Buffers.Binary.BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(4, sizeof(float)), 1.0F);
            float[] values = [10, 11, 12, 1, 2, 3, 20, 21, 22, 30, 31, 32, 4, 5, 6, 40, 41, 42];
            for (var element = 0; element < 6; ++element) {
                for (var part = 0; part < 3; ++part) {
                    System.Buffers.Binary.BinaryPrimitives.WriteSingleLittleEndian(
                        bytes.AsSpan(8 + element * 16 + part * sizeof(float), sizeof(float)),
                        values[element * 3 + part]);
                }
            }
            return bytes;
        }

        /// <summary>
        /// Lays the header out at a fixed size and checks the caret box the app forwards to the
        /// candidate window: inside the field, advancing with the caret, and gone once the focus is
        /// retired. The box is logical pixels, like everything the frontend authors.
        /// </summary>
        private string? verify_ime_caret() {
            var context = next_ui;
            if (context == null) { return "the caret check has no UI context"; }
            var previous = name_edit;
            var edit = new ui_text_edit();
            edit.reset("Bibi");
            edit.focused = true;
            name_edit = edit;
            try {
                context.begin(1280.0F, 720.0F, 1.0F);
                var root = build_dope_sheet();
                ui_layout_engine.measure(root, 1280.0F);
                ui_layout_engine.arrange(root, new ui_rect(0.0F, 0.0F, 1280.0F, 720.0F));
                var field = root.find("name_field");
                if (field == null) { return "the header built no name field"; }
                if (!try_ime_caret_rect(root, out var start_x, out var start_y, out var width, out var height)) {
                    return "the focused field published no caret box";
                }
                if (width <= 0.0F || height <= 0.0F) { return "the caret box is empty"; }
                if (start_x < field.rect.x - 0.01F || start_x > field.rect.right + 0.01F) {
                    return string.Create(
                        CultureInfo.InvariantCulture,
                        $"the caret box left the field horizontally ({start_x:0.0} outside {field.rect.x:0.0}..{field.rect.right:0.0})");
                }
                if (start_y < field.rect.y - 0.01F || start_y + height > field.rect.bottom + 0.01F) {
                    return string.Create(
                        CultureInfo.InvariantCulture,
                        $"the caret box left the field vertically ({start_y:0.0}..{start_y + height:0.0} outside {field.rect.y:0.0}..{field.rect.bottom:0.0})");
                }
                edit.move_home(false);
                if (!try_ime_caret_rect(root, out var home_x, out _, out _, out _)) {
                    return "the caret box disappeared after moving to the start";
                }
                if (!(start_x > home_x)) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"the caret box did not advance with the caret ({home_x:0.00} to {start_x:0.00})");
                }
                edit.set_preedit("\u4F60\u597D", 2);
                if (!try_ime_caret_rect(root, out var preedit_x, out _, out _, out _)) {
                    return "the caret box disappeared during a composition";
                }
                if (!(preedit_x > home_x)) { return "the composition did not move the caret box"; }
                edit.clear_preedit();
                edit.focused = false;
                if (try_ime_caret_rect(root, out _, out _, out _, out _)) {
                    return "the caret box survived a lost focus";
                }
            } finally {
                name_edit = previous;
            }
            return null;
        }

        /// <summary>
        /// Checks the pointer claim the window seam reads before it chains a pointer event to the
        /// camera: the sheet's controls claim, the preview above it does not, and a drag in flight
        /// keeps claiming even where it wanders.
        /// </summary>
        private string? verify_pointer_claim() {
            var context = next_ui;
            if (context == null) { return "the pointer claim check has no UI context"; }
            context.begin(1280.0F, 720.0F, 1.0F);
            var root = build_dope_sheet();
            if (!context.submit(root)) { return "the pointer claim check could not submit its tree"; }
            var field = root.find("name_field");
            var viewport = root.find("viewport");
            var play = root.find("play");
            if (field == null || viewport == null || play == null) {
                return "the Dope Sheet is missing an element the pointer claim needs";
            }
            var gesture = next_ui_gesture;
            try {
                next_ui_gesture = sheet_gesture.none;
                float field_x = field.rect.x + field.rect.width * 0.5F;
                float field_y = field.rect.y + field.rect.height * 0.5F;
                if (!pointer_claims(field_x, field_y)) {
                    return "the name field did not claim the pointer";
                }
                if (!pointer_claims(play.rect.x + play.rect.width * 0.5F, play.rect.y + play.rect.height * 0.5F)) {
                    return "a transport button did not claim the pointer";
                }
                float preview_x = viewport.rect.x + viewport.rect.width * 0.5F;
                float preview_y = viewport.rect.y + viewport.rect.height * 0.5F;
                if (pointer_claims(preview_x, preview_y)) {
                    return "the preview area claimed the pointer, so the camera could no longer orbit it";
                }
                next_ui_gesture = sheet_gesture.key;
                if (!pointer_claims(preview_x, preview_y)) {
                    return "a drag in flight stopped claiming the pointer once it left the sheet";
                }
            } finally {
                next_ui_gesture = gesture;
            }
            return null;
        }

        /// <summary>
        /// Pushes a press through the callback the host publishes window events to, instead of calling
        /// the press handler directly: this is the path a real click takes, so it catches a broken
        /// registration, a swallowed event and a hit test that disagrees with the painted tree.
        /// </summary>
        private string? verify_name_field_press() {
            var context = next_ui;
            if (context == null) { return "the click check has no UI context"; }
            if (name_edit == null) { return "the click check has no name field editor"; }
            context.begin(1280.0F, 720.0F, 1.0F);
            var root = build_dope_sheet();
            if (!context.submit(root)) { return "the click check could not submit its tree"; }
            var field = root.find("name_field");
            if (field == null) { return "the click check could not find the name field"; }
            float x = field.rect.x + 6.0F;
            float y = field.rect.y + field.rect.height * 0.5F;
            string before = name_edit.text;
            try {
                name_edit.focused = false;
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.mouse_move, 0, false, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
                on_next_ui_input(new input_event(input_type.mouse_button, 0, true, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
                if (next_ui_events.Count != 2) {
                    return "the input callback did not keep the two published events";
                }
                process_next_ui_input();
                if (!name_edit.focused) {
                    return "a press published as a window event did not focus the name field";
                }
                if (name_edit.text != before) {
                    return "focusing the name field changed its text";
                }
            } finally {
                name_edit.focused = false;
                name_edit.clear_preedit();
                if (next_ui != null) {
                    next_ui.focused_key = string.Empty;
                }
                next_ui_events.Clear();
            }
            return null;
        }

        /// <summary>
        /// Types into the focused field the way the window reports typing: one character event per
        /// codepoint, flagged as a press. A character without the flag must not insert, which is what
        /// the field's filter says and what the seam guarantees for a real keystroke.
        /// </summary>
        private string? verify_character_typing() {
            var context = next_ui;
            if (context == null) { return "the typing check has no UI context"; }
            if (name_edit == null) { return "the typing check has no name field editor"; }
            context.begin(1280.0F, 720.0F, 1.0F);
            var root = build_dope_sheet();
            if (!context.submit(root)) { return "the typing check could not submit its tree"; }
            var field = root.find("name_field");
            if (field == null) { return "the typing check could not find the name field"; }
            float x = field.rect.x + 6.0F;
            float y = field.rect.y + field.rect.height * 0.5F;
            string before = name_edit.text;
            try {
                name_edit.focused = false;
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.mouse_button, 0, true, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
                process_next_ui_input();
                if (!name_edit.focused) {
                    return "the typing check could not focus the name field";
                }
                name_edit.move_end(false);
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.character, 0, true, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0x45U));
                process_next_ui_input();
                if (name_edit.text != before + "E") {
                    return "a character event flagged as a press did not type into the field";
                }
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.character, 0, false, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0x46U));
                process_next_ui_input();
                if (name_edit.text != before + "E") {
                    return "a character event without the press flag changed the text";
                }
            } finally {
                name_edit.focused = false;
                name_edit.clear_preedit();
                name_edit.reset(before);
                if (next_ui != null) {
                    next_ui.focused_key = string.Empty;
                }
                next_ui_events.Clear();
            }
            return null;
        }

        /// <summary>
        /// The virtualized list on its own, then in the sheet: 100 items in a five-row window must build
        /// five rows, the wheel must move the window by whole rows, the last window must start at the
        /// last full page, and a list shorter than its window must not scroll at all. The sheet with the
        /// real clip must likewise build fewer rows than the clip has channels.
        /// </summary>
        private string? verify_virtual_list() {
            var view = new ui_list_view();
            var height = 5.0F * view.pitch - view.row_gap;
            var frame = view.build("probe_list", height, 100,
                                   index => ui_node.box($"probe_row_{index}").size(float.NaN, view.row_height));
            var list = frame.find("probe_list");
            if (list == null) { return "the list view did not build a list"; }
            if (list.children.Count != 5) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the list built {list.children.Count} rows for 100 items in a five-row window");
            }
            if (list.children[0].key != "probe_row_0" || list.children[4].key != "probe_row_4") {
                return "the list built rows other than the first window";
            }
            try {
                view.scroll(100, height, -3.0F);
                if (view.first != 3) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"three wheel steps left the list at row {view.first}");
                }
                view.scroll(100, height, -1000.0F);
                if (view.first != 95) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"scrolling past the end stopped at row {view.first} of 95");
                }
                var tail = view.build("probe_list", height, 100,
                                      index => ui_node.box($"probe_row_{index}").size(float.NaN, view.row_height));
                if (tail.find("probe_row_95") == null || tail.find("probe_row_94") != null) {
                    return "the last window of the list did not start at its last page";
                }
                view.first = 0;
                view.scroll(3, height, -5.0F);
                if (view.first != 0) { return "a list shorter than its window scrolled"; }

                var context = next_ui;
                var clip = timeline_clip;
                if (context == null || clip == null) { return "the list check has no UI context or clip"; }
                context.begin(1280.0F, 720.0F, 1.0F);
                var root = build_dope_sheet();
                if (!context.submit(root)) { return "the list check could not submit its tree"; }
                var channels = root.find("channels");
                if (channels == null) { return "the sheet did not build its channel list"; }
                int rows = channel_list.visible_rows(next_ui_list_height);
                if (channels.children.Count != Math.Min(rows, clip.channels.Count)) {
                    return string.Create(
                        CultureInfo.InvariantCulture,
                        $"the sheet built {channels.children.Count} rows for {clip.channels.Count} channels that fit {rows}");
                }
                if (clip.channels.Count > rows && channels.children.Count == clip.channels.Count) {
                    return "the sheet built every channel even though only a window fits";
                }
                // The wheel belongs to whoever is under the pointer: rows scroll, the preview does not.
                var viewport = root.find("viewport");
                var row = root.find("channel_0");
                if (viewport == null || row == null) {
                    return "the list check could not find the preview or a channel row";
                }
                channel_list.first = 0;
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.scroll, 0, false, 0, viewport.rect.x + 4.0F,
                                                 viewport.rect.y + 4.0F, 0.0F, 0.0F, -1.0F, 0U));
                process_next_ui_input();
                if (channel_list.first != 0) {
                    return "a wheel over the preview scrolled the channel rows";
                }
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.scroll, 0, false, 0, row.rect.x + 40.0F,
                                                 row.rect.y + 4.0F, 0.0F, 0.0F, -2.0F, 0U));
                process_next_ui_input();
                if (channel_list.first != 2) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"a wheel over the rows left the list at row {channel_list.first}");
                }
            } finally {
                view.first = 0;
                channel_list.first = 0;
            }
            return null;
        }

        /// <summary>
        /// The divider end to end through the real events: a press on it starts a split drag, dragging
        /// down shrinks the sheet, and the clamps keep both the sheet and the preview usable.
        /// </summary>
        private string? verify_splitter_drag() {
            var context = next_ui;
            if (context == null) { return "the splitter check has no UI context"; }
            if (timeline_clip == null) { return "the splitter check has no motion clip"; }
            float before = next_ui_sheet_fraction;
            context.begin(1280.0F, 720.0F, 1.0F);
            var root = build_dope_sheet();
            if (!context.submit(root)) { return "the splitter check could not submit its tree"; }
            var splitter = root.find("splitter");
            var sheet = root.find("sheet");
            if (splitter == null || sheet == null) {
                return "the splitter check could not find the divider or the sheet";
            }
            float start_height = sheet.rect.height;
            float x = splitter.rect.x + splitter.rect.width * 0.5F;
            float y = splitter.rect.y + splitter.rect.height * 0.5F;
            try {
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.mouse_move, 0, false, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
                on_next_ui_input(new input_event(input_type.mouse_button, 0, true, 0, x, y, 0.0F, 0.0F, 0.0F, 0U));
                process_next_ui_input();
                if (next_ui_gesture != sheet_gesture.split) {
                    return "a press on the divider did not start a split drag";
                }
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.mouse_move, 0, false, 0, x, y + 60.0F, 0.0F, 0.0F, 0.0F, 0U));
                process_next_ui_input();
                next_ui_events.Clear();
                on_next_ui_input(new input_event(input_type.mouse_button, 0, false, 0, x, y + 60.0F, 0.0F, 0.0F, 0.0F, 0U));
                process_next_ui_input();
                if (next_ui_gesture != sheet_gesture.none) {
                    return "releasing the divider left the drag running";
                }
                context.begin(1280.0F, 720.0F, 1.0F);
                var after = build_dope_sheet();
                if (!context.submit(after)) { return "the splitter check could not submit the resized tree"; }
                var resized = after.find("sheet");
                var viewport = after.find("viewport");
                if (resized == null || viewport == null) { return "the resized sheet lost its parts"; }
                if (!(resized.rect.height < start_height - 1.0F)) {
                    return string.Create(
                        CultureInfo.InvariantCulture,
                        $"dragging the divider down did not shrink the sheet ({start_height:0.0} -> {resized.rect.height:0.0})");
                }
                if (resized.rect.height < sheet_min_height - 0.01F) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"the sheet shrank below its minimum ({resized.rect.height:0.0})");
                }
                if (viewport.rect.height < viewport_min_height - 0.01F) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"the preview shrank below its minimum ({viewport.rect.height:0.0})");
                }
                // Dragging past the top must clamp instead of pushing the preview out of the window.
                update_sheet_split(0.0F);
                context.begin(1280.0F, 720.0F, 1.0F);
                var clamped = build_dope_sheet();
                if (!context.submit(clamped)) { return "the splitter check could not submit the clamped tree"; }
                var tall = clamped.find("sheet");
                var small = clamped.find("viewport");
                if (tall == null || small == null) { return "the clamped sheet lost its parts"; }
                if (small.rect.height < viewport_min_height - 0.01F) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"dragging past the top left the preview {small.rect.height:0.0} high");
                }
                if (tall.rect.bottom > 720.0F - sheet_footer_height + 0.01F || tall.rect.y < 0.0F) {
                    return string.Create(CultureInfo.InvariantCulture,
                                         $"the clamped sheet left the window (y {tall.rect.y:0.0}, bottom {tall.rect.bottom:0.0})");
                }
            } finally {
                next_ui_sheet_fraction = before;
                next_ui_gesture = sheet_gesture.none;
                next_ui_events.Clear();
            }
            return null;
        }

        /// <summary>
        /// Drives the File menu with real presses: the title opens the panel, the reload entry asks first
        /// because the clip is dirty, the scrim turns a press on the transport into a dismissal instead of
        /// a click, and the confirm button finally reloads from disk and clears the history.
        /// </summary>
        private string? verify_menus() {
            var context = next_ui;
            var before = timeline_clip;
            if (context == null || before == null) { return "the menu check has no context or clip"; }
            string open_before = next_ui_open_menu;
            try {
                next_ui_dialog = null;
                next_ui_open_menu = string.Empty;
                context.begin(1280.0F, 720.0F, 1.0F);
                if (!context.submit(build_dope_sheet())) { return "the menu check could not submit its tree"; }
                var file = context.last_tree?.find("menu_file");
                if (file == null) { return "the header has no File menu"; }
                press_next_ui(file.rect.x + file.rect.width * 0.5F, file.rect.y + file.rect.height * 0.5F);
                if (next_ui_open_menu != "menu_file") {
                    return $"pressing the File title left the open menu as '{next_ui_open_menu}'";
                }
                // The dropped panel is an overlay, so it exists only after the frame that queued it.
                context.begin(1280.0F, 720.0F, 1.0F);
                if (!context.submit(build_dope_sheet())) { return "the menu check could not submit its panel"; }
                var panel = context.overlay_root;
                if (panel == null) { return "the open File menu dropped no panel"; }
                var reload = panel.find("menu_file_reload");
                if (reload == null) { return "the File menu has no reload entry"; }
                before.dirty = true;
                press_next_ui(reload.rect.x + reload.rect.width * 0.5F, reload.rect.y + reload.rect.height * 0.5F);
                if (next_ui_dialog?.key != "dialog_reload") {
                    return "reloading a clip with unsaved edits did not ask first";
                }
                // A press on the transport lands on the scrim: the dialog closes and the button does not fire.
                context.begin(1280.0F, 720.0F, 1.0F);
                if (!context.submit(build_dope_sheet())) { return "the menu check could not submit the dialog"; }
                var play = context.last_tree?.find("play");
                if (play == null) { return "the transport has no play button"; }
                press_next_ui(play.rect.x + play.rect.width * 0.5F, play.rect.y + play.rect.height * 0.5F);
                if (next_ui_dialog != null) { return "a press on the scrim left the dialog open"; }
                if (!ReferenceEquals(timeline_clip, before)) { return "a press behind the dialog ran the reload"; }
                // Now let the confirmation through.
                context.begin(1280.0F, 720.0F, 1.0F);
                if (!context.submit(build_dope_sheet())) { return "the menu check could not reopen the menu"; }
                var title = context.last_tree?.find("menu_file");
                if (title == null) { return "the File menu vanished after the dialog closed"; }
                press_next_ui(title.rect.x + title.rect.width * 0.5F, title.rect.y + title.rect.height * 0.5F);
                context.begin(1280.0F, 720.0F, 1.0F);
                if (!context.submit(build_dope_sheet())) { return "the menu check could not submit the second panel"; }
                var again = context.overlay_root?.find("menu_file_reload");
                if (again == null) { return "the File menu did not reopen its reload entry"; }
                press_next_ui(again.rect.x + again.rect.width * 0.5F, again.rect.y + again.rect.height * 0.5F);
                context.begin(1280.0F, 720.0F, 1.0F);
                if (!context.submit(build_dope_sheet())) { return "the menu check could not submit the second dialog"; }
                var discard = context.overlay_root?.find("dialog_reload_discard");
                if (discard == null) { return "the confirmation dialog has no discard button"; }
                press_next_ui(discard.rect.x + discard.rect.width * 0.5F, discard.rect.y + discard.rect.height * 0.5F);
                if (next_ui_dialog != null) { return "the discard button left the dialog open"; }
                if (timeline_clip == null || ReferenceEquals(timeline_clip, before)) {
                    return "the discard button did not reload the clip from disk";
                }
                if (timeline_clip.dirty) { return "the reloaded clip still says it has unsaved edits"; }
                if (undo_history != null && undo_history.can_undo) {
                    return "the reload kept an undo step that points at the old clip";
                }
            } finally {
                next_ui_open_menu = open_before;
                next_ui_dialog = null;
                next_ui_gesture = sheet_gesture.none;
                next_ui_events.Clear();
            }
            return null;
        }

        /// <summary>
        /// Renames a clip through the save path and compares the whole file text with a
        /// single-substitution expectation, which catches a writer that reformats or reorders JSON.
        /// Then it removes the name from a second copy and checks that the writer inserts one.
        /// </summary>
        private string? verify_clip_rename() {
            var directory = Path.Combine(Environment.CurrentDirectory, "dynamic", "motion-rename-selftest");
            Directory.CreateDirectory(directory);
            var json_path = Path.Combine(directory, "motion.gltf");
            var bin_path = Path.Combine(directory, "motion.bin");
            File.WriteAllText(json_path, cubic_fixture_json);
            File.WriteAllBytes(bin_path, cubic_fixture_bytes());
            var original = cubic_fixture_json;
            if (!motion_document.try_load(json_path, out var clip, out var error) || clip == null) {
                return $"the rename fixture did not load: {error}";
            }
            const string renamed = "Cubed \u52A8\u753B";
            clip.clip_name = renamed;
            if (!clip.try_save(out error)) { return $"renaming the clip failed: {error}"; }
            var expected = original.Replace("\"Cubic Clip\"", "\"" + JsonEncodedText.Encode(renamed).Value + "\"");
            var saved = File.ReadAllText(json_path);
            if (!string.Equals(saved, expected, StringComparison.Ordinal)) {
                return "renaming the clip changed more than the animation name";
            }
            if (!motion_document.try_load(json_path, out var reloaded, out error) || reloaded == null) {
                return $"the renamed clip did not reload: {error}";
            }
            if (reloaded.clip_name != renamed) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the reloaded clip is named '{reloaded.clip_name}'");
            }
            // The insert path: an animation with no name gets one as its first member.
            var nameless = cubic_fixture_json.Replace("\"name\": \"Cubic Clip\",", string.Empty);
            if (nameless == cubic_fixture_json) { return "the fixture did not drop its animation name"; }
            File.WriteAllText(json_path, nameless);
            if (!motion_document.try_load(json_path, out var fresh, out error) || fresh == null) {
                return $"the nameless fixture did not load: {error}";
            }
            fresh.clip_name = "Named Later";
            if (!fresh.try_save(out error)) { return $"inserting a clip name failed: {error}"; }
            var inserted = File.ReadAllText(json_path);
            if (inserted.Replace("\"name\": \"Named Later\", ", string.Empty, StringComparison.Ordinal) != nameless) {
                return "inserting a clip name changed bytes other than the inserted member";
            }
            if (!motion_document.try_load(json_path, out var inserted_clip, out error) || inserted_clip == null) {
                return $"the clip with an inserted name did not reload: {error}";
            }
            if (inserted_clip.clip_name != "Named Later") {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the inserted name reloaded as '{inserted_clip.clip_name}'");
            }
            return null;
        }

        private static bool float_arrays_equal(float[] left, float[] right) {            if (left.Length != right.Length) { return false; }
            for (var index = 0; index < left.Length; ++index) {
                if (MathF.Abs(left[index] - right[index]) > 0.0001F) { return false; }
            }
            return true;
        }

        /// <summary>
        /// Builds the state a drag leaves behind when it ends exactly on another key of the same
        /// channel, then runs the release path: the key returns to its old time, the notice explains
        /// why, and the history gains nothing.
        /// </summary>
        private string? verify_collision_gesture() {
            var clip = timeline_clip;
            if (clip == null || clip.channels.Count == 0) { return "the collision check has no clip"; }
            var channel = clip.channels[0];
            if (channel.times.Length < 3) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the collision check needs three keys, the channel has {channel.times.Length}");
            }
            int key = 1;
            var before_times = (float[])channel.times.Clone();
            var before_values = (float[])channel.values.Clone();
            // The drag lands on key 0's time, which is what a user can do by dropping a key on another.
            channel.times[key] = channel.times[0];
            var undo_before = undo_history?.can_undo ?? false;
            var label_before = undo_history?.next_undo_label;
            next_ui_gesture = sheet_gesture.key;
            next_ui_gesture_channel = 0;
            next_ui_gesture_key = key;
            next_ui_gesture_group.Clear();
            next_ui_gesture_group.Add(channel);
            next_ui_before_times = [before_times];
            next_ui_before_values = [before_values];
            notice = string.Empty;
            release_next_ui();
            if (next_ui_gesture != sheet_gesture.none) { return "the release did not end the gesture"; }
            if (!float_arrays_equal(channel.times, before_times)) {
                return "the refused move did not put the key back";
            }
            if (!notice.Contains("cancelled", StringComparison.Ordinal)) {
                return string.Create(CultureInfo.InvariantCulture,
                                     $"the refused move said '{notice}'");
            }
            if ((undo_history?.can_undo ?? false) != undo_before ||
                undo_history?.next_undo_label != label_before) {
                return "the refused move still pushed an undo entry";
            }
            return null;
        }
    }
}


