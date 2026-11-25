using System;
using deren.scripting;

namespace deren.editor;

/// <summary>
/// The isolated animation editor. Switching modes puts the open project scene aside untouched and
/// opens a preview scene that holds exactly one placement of the chosen asset, so nothing the
/// animation editor does can reach a saved scene. The preview is in-memory only: it has no path and
/// is never written to disk. This first slice previews an existing clip (play, pause, seek); clip
/// authoring and the motion.gltf write come next.
/// </summary>
internal sealed unsafe partial class editor_host {
    private static bool requested_animation_mode;
    private static int requested_animation_toggle;
    private int animation_toggle_remaining;
    private int animation_toggle_wait;
    private int animation_toggle_total;

    private bool animation_mode;
    private scene_document? animation_scene;
    private Guid animation_saved_selection;
    private Guid animation_preview_uid;
    private bool animation_playing;
    private bool animation_preview_started;
    private bool animation_scrubbing;
    private bool animation_scrub_was_playing;
    private string animation_clip_text = string.Empty;

    private document_node? animation_preview() => document?.find(animation_preview_uid);

    /// <summary>
    /// The preview placement's runtime node, or null while the asset is still importing. Every
    /// animation entry is addressed by this node, because the host keeps one player per placement.
    /// </summary>
    private deren.node_id? animation_preview_player() {
        var node = animation_preview();
        return node != null && node.runtime.is_valid ? node.runtime : null;
    }

    /// <summary>
    /// Selects the first placement whose asset already drives an animation player. Used by the
    /// command-line entry points, which have no mouse to pick a character with.
    /// </summary>
    private bool select_first_animatable() {
        if (document == null || selection_state == null) { return false; }
        foreach (var root in document.roots) {
            if (root.asset == null) { continue; }
            if (animation_player.try_get_info(root.runtime, out var info) && info.available) {
                selection_state.select(root.uid);
                refresh_panels(force_details: true);
                return true;
            }
        }
        return false;
    }

    /// <summary>
    /// Enters the animation editor with an <b>empty</b> preview scene. The project scene is put aside
    /// untouched and the preview starts with no placement at all; the author brings content in with
    /// <see cref="open_animation_asset"/>, so entering never guesses which character to edit.
    /// Returns null on success or the reason the mode cannot start.
    /// </summary>
    private string? enter_animation_mode() {
        if (animation_mode) { return null; }
        if (document == null || tree == null) { return "Open a scene before entering the animation editor"; }

        var preview = new scene_document { name = "Animation Preview" };
        animation_scene = document;
        animation_saved_selection = selection_state?.primary ?? Guid.Empty;
        animation_preview_uid = Guid.Empty;
        animation_playing = false;
        animation_preview_started = false;
        animation_scrubbing = false;
        animation_clip_text = string.Empty;
        if (!replace_scene(preview)) {
            animation_scene = null;
            return "Could not open the animation preview scene";
        }
        animation_mode = true;
        frame_animation_preview();
        if (ui != null) {
            ui.try_set_element_style("animation_strip", "display", "flex");
            ui.set_text("btn_animation", "Scene");
            ui.set_text("toolbar_note", "ANIMATION EDITOR");
        }
        notice = "Animation editor: open an asset for the preview";
        refresh_panels(force_details: true);
        enter_timeline();
        return null;
    }

    /// <summary>
    /// Loads one asset into the empty preview scene, replacing whatever it held before. The preview
    /// keeps exactly one placement so the timeline always names one clip. Returns null on success or
    /// the reason the placement could not be created.
    /// </summary>
    private string? open_animation_asset(string asset, int clip) {
        if (!animation_mode || document == null || tree == null) { return "Open the animation editor first"; }
        if (asset.Length == 0) { return "Select an asset to animate"; }

        var previous = new System.Collections.Generic.List<document_node>(document.roots);
        foreach (var existing in previous) {
            if (!scene_sync.destroy_one(document, tree, existing, out var removed)) {
                return $"Could not unload the preview placement '{existing.name}' ({removed})";
            }
            document.remove_root(existing);
        }
        var character = new document_node {
            name = asset,
            asset = asset,
            animation_clip = Math.Max(0, clip),
            animation_autoplay = false,
        };
        document.add_root(character);
        if (!scene_sync.create_one(document, tree, null, character, out var created) ||
            !scene_sync.push(document, tree, character, out created)) {
            document.remove_root(character);
            animation_preview_uid = Guid.Empty;
            return $"Could not open '{asset}' in the preview ({created})";
        }
        animation_preview_uid = character.uid;
        animation_preview_started = false;
        animation_clip_text = string.Empty;
        selection_state?.select(character.uid);
        // Frame the real mesh bounds once the placement exists: the empty preview's fixed pose cannot
        // know how large the asset is, and a character imported in centimetres would be off-screen.
        if (!focus_selection()) {
            frame_animation_preview();
        }
        refresh_panels(force_details: true);
        enter_timeline();
        notice = $"Animating: {asset}";
        return null;
    }

    /// <summary>
    /// Opens the asset the content browser highlights in the preview scene. This is the timeline
    /// header's Open button, so an empty preview can be filled without leaving the animation editor.
    /// </summary>
    private void open_animation_selected() {
        var asset = browser?.selected_asset;
        if (string.IsNullOrEmpty(asset)) {
            notice = "Select an asset in the Content Browser first";
            return;
        }
        if (open_animation_asset(asset, 0) is { } error) {
            notice = error;
        }
    }

    /// <summary>
    /// Leaves the animation editor and reopens the project scene that was put aside. The preview
    /// scene was never saved, so leaving it simply drops it.
    /// </summary>
    private void exit_animation_mode() {
        if (!animation_mode) { return; }
        animation_mode = false;
        animation_playing = false;
        animation_preview_started = false;
        animation_scrubbing = false;
        animation_preview_uid = Guid.Empty;
        if (ui != null) {
            ui.try_set_element_style("animation_strip", "display", "none");
            ui.set_text("btn_animation", "Animation");
            ui.set_text("toolbar_note", "SCENE EDITOR");
        }
        leave_timeline();
        var back = animation_scene;
        animation_scene = null;
        if (back == null) { return; }
        if (!replace_scene(back)) {
            notice = "Could not return to the project scene";
            return;
        }
        if (animation_saved_selection != Guid.Empty) { selection_state?.select(animation_saved_selection); }
        notice = "Back in the scene editor";
        refresh_panels(force_details: true);
    }

    /// <summary>
    /// Diagnostic entry point: alternates the scene editor and the animation editor N times, leaving
    /// the animation editor open. Used to reproduce the skinning drift that repeated mode switches
    /// caused, with a screenshot at the end.
    /// </summary>
    private void animation_mode_toggle_tick() {
        if (requested_animation_toggle > 0) {
            animation_toggle_remaining = requested_animation_toggle;
            animation_toggle_total = requested_animation_toggle;
            requested_animation_toggle = 0;
            select_first_animatable();
        }
        if (animation_toggle_remaining <= 0) { return; }
        if (animation_toggle_wait > 0) {
            --animation_toggle_wait;
            return;
        }
        toggle_animation_mode();
        --animation_toggle_remaining;
        animation_toggle_wait = 6;
        if (animation_toggle_remaining == 0) {
            host_logger.write($"Editor: animation mode toggled {animation_toggle_total} times; the preview scene stays open");
        }
    }

    private void toggle_animation_mode() {
        if (animation_mode) {
            exit_animation_mode();
            return;
        }
        if (enter_animation_mode() is { } error) {
            notice = error;
            refresh_panels();
        }
    }

    /// <summary>
    /// Points the viewport camera at the preview character. The character asset hangs below its
    /// origin along -Y, so the framed target sits below the placement and the lens looks at the
    /// body rather than the empty origin.
    /// </summary>
    private void frame_animation_preview() {
        var pose = new orbit_camera {
            yaw_degrees = 0,
            pitch_degrees = 4,
            distance = 1.7f,
            target = new vector_3 { x = 0, y = -0.9f, z = 0 },
            vertical_fov_degrees = 50,
        };
        if (!pose.try_apply(out var status)) {
            host_logger.write($"Editor: could not frame the animation preview ({status})");
        }
    }

    /// <summary>
    /// Refreshes the timeline strip from the preview player. Runs every frame; the texts are only
    /// written when they change because RmlUi re-layouts on every text write.
    /// </summary>
    private void update_animation_strip() {
        if (!animation_mode || ui == null || animation_player_node() is not { } node) { return; }
        string clip = "No clip";
        string time = "0.00 / 0.00 s";
        float fraction = 0;
        if (animation_player.try_get_info(node, out var info) && info.available) {
            if (!animation_preview_started) {
                // A placement starts playing as soon as its player exists; the animation editor opens
                // on the first frame paused at 0.00 s so the author sees the rest pose before scrubbing.
                animation_preview_started = true;
                animation_player.try_pause(node);
                animation_player.try_seek(node, 0);
                // The placement's mesh binds on an import that trails creation, so the bounds only
                // exist by the time its player reports a clip. Frame the asset now, otherwise the
                // fixed pose from the empty preview stays and a character imported at another scale
                // ends up too small or off-screen.
                if (tree != null) {
                    viewport_navigation.focus(tree, node, applied_aspect);
                }
            }
            if (animation_player.try_get_clip_name(node, info.current_clip, out var name) && !string.IsNullOrEmpty(name)) {
                clip = name;
            }
            time = $"{info.time:0.00} / {info.duration:0.00} s";
            fraction = info.duration > 0 ? Math.Clamp(info.time / info.duration, 0, 1) : 0;
            animation_playing = info.playing;
        } else {
            clip = "This asset has no animation clip";
        }
        if (clip != animation_clip_text) {
            animation_clip_text = clip;
            ui.set_text("animation_clip", clip);
        }
        ui.set_text("animation_time", time);
        ui.try_set_element_style("animation_fill", "width", $"{fraction * 100:0.###}%");
        ui.try_set_element_style("animation_playhead", "left", $"{fraction * 100:0.###}%");
        ui.set_text("animation_play", animation_playing ? "Pause" : "Play");
    }

    private deren.node_id? animation_player_node() => animation_preview_player();

    private void toggle_animation_playback() {
        if (animation_preview_player() is not { } node) { return; }
        if (!animation_player.try_get_info(node, out var info) || !info.available) { return; }
        if (!animation_player.try_set_playing(node, !info.playing)) {
            notice = "Could not change the preview playback";
            return;
        }
        animation_playing = !info.playing;
    }

    private void seek_animation(float seconds) {
        if (animation_preview_player() is not { } node) { return; }
        if (!animation_player.try_seek(node, Math.Max(0, seconds))) {
            notice = "Could not seek the preview clip";
        }
    }

    /// <summary>
    /// Routes the timeline strip's own events. Returns true when the event belonged to the strip, so
    /// the caller does not fall through to viewport selection or the outliner.
    /// </summary>
    private bool handle_animation_strip(rml_event input) {
        if (!animation_mode || ui == null) { return false; }
        if (input.type == rml_event_type.click) {
            switch (input.id) {
            case "animation_play":
                toggle_animation_playback();
                return true;
            case "animation_restart":
                seek_animation(0);
                if (animation_preview_player() is { } restart_node) {
                    animation_player.try_set_playing(restart_node, true);
                }
                return true;
            }
        }
        if (input.id is not ("animation_scrub" or "animation_fill" or "animation_playhead")) {
            return false;
        }
        if (input.type == rml_event_type.pointer_down && input.button == 0) {
            animation_scrubbing = true;
            animation_scrub_was_playing = animation_playing;
            scrub_animation_to(input.x);
            return true;
        }
        if (input.type == rml_event_type.pointer_move && animation_scrubbing) {
            scrub_animation_to(input.x);
            return true;
        }
        if (input.type is rml_event_type.pointer_up or rml_event_type.pointer_cancel && animation_scrubbing) {
            animation_scrubbing = false;
            // Seeking pauses the player, so a scrub that started during playback resumes when it ends.
            if (animation_scrub_was_playing && animation_preview_player() is { } node) {
                animation_player.try_set_playing(node, true);
            }
            return true;
        }
        return animation_scrubbing;
    }

    private void scrub_animation_to(float x) {
        if (animation_preview_player() is not { } node || !animation_player.try_get_info(node, out var info) ||
            !info.available || info.duration <= 0 || ui == null ||
            !ui.try_get_element_rect("animation_scrub", out var track) || track.width <= 0) {
            return;
        }
        float fraction = Math.Clamp((x - track.x) / track.width, 0, 1);
        seek_animation(fraction * info.duration);
    }
}
