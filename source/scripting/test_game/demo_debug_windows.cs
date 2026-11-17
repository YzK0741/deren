using System;
using System.Collections.Generic;
using deren;
using deren.scripting;

namespace game {
    internal sealed class demo_debug_windows : IDisposable {
        private readonly ui_value[] window_buttons = [new(false), new(false), new(false), new(false)];
        private readonly bool[] open = new bool[4];
        private static readonly string[] titles = ["Performance", "Rendering", "Scene", "Animation"];
        private static readonly string[] tone_names = ["ACES", "Khronos PBR Neutral", "Game Toon"];
        private readonly ui_value screenshot = new(false);
        private readonly ui_value exposure = new();
        private readonly ui_value tone = new();
        private readonly ui_value sun = new();
        private readonly ui_value bloom = new();
        private readonly ui_value fxaa = new();
        private readonly ui_value taa = new();
        private readonly ui_value ssao = new();
        private readonly ui_value shadow = new();
        private readonly ui_value gbuffer = new();
        private readonly ui_value selection = new(0);
        private readonly ui_value expand = new(false);
        private readonly ui_value visibility = new(false);
        private readonly ui_value clip = new();
        private readonly ui_value playing = new();
        private readonly ui_value speed = new();
        private readonly ui_value time = new();
        private readonly HashSet<node_id> hidden = [];
        private readonly HashSet<node_id> expanded = [];
        private readonly List<(node_id node, string name, string parent)> rows = [];
        private readonly List<string> labels = [];
        private scene_tree? inspected_tree;
        private node_id selected;
        private int drawn_selection;
        private bool drawn_visibility;
        private bool scene_widgets_drawn;
        private string[] clip_names = [];
        private animation_player.animation_info drawn_animation;
        private bool animation_widgets_drawn;
        private float elapsed;
        private float frame_ms;
        private int frames;

        public demo_debug_windows(node_id initially_hidden) {
            if (initially_hidden.is_valid) {
                hidden.Add(initially_hidden);
            }
            var render = project_config.render;
            exposure.number = render.exposure;
            tone.selected = render.effective_tone_mapping();
            bloom.flag = render.bloom;
            fxaa.flag = render.fxaa;
            taa.flag = render.taa;
            ssao.flag = render.ssao;
            shadow.flag = render.shadow;
            gbuffer.flag = render.gbuffer_debug;
            var lighting = project_config.lighting;
            var area = lighting.derive_area_light(default);
            sun.vector = area.enabled && lighting.area_light_irradiance ? area.to_light_dir : lighting.sun_direction;
        }

        public void update(scene_tree tree, float delta_time) {
            if (delta_time > 0.0f && float.IsFinite(delta_time)) {
                elapsed += delta_time;
                ++frames;
                if (elapsed >= 0.25f) {
                    frame_ms = 1000.0f * elapsed / frames;
                    elapsed = 0.0f;
                    frames = 0;
                }
            }
            if (debug_ui.begin_panel("deren debug", 200.0f, 0.0f)) {
                for (var i = 0; i < titles.Length; ++i) {
                    if (take_click(window_buttons[i])) {
                        open[i] = !open[i];
                    }
                    debug_ui.button(open[i] ? $"Hide {titles[i]}" : $"Show {titles[i]}", window_buttons[i]);
                }
                if (take_click(screenshot) && !screen_snapshot.try_request()) {
                    host_logger.write("Game: screenshot request rejected");
                }
                debug_ui.button("Screenshot", screenshot);
                debug_ui.end_panel();
            }

            draw_window(0, draw_performance);
            draw_window(1, draw_rendering);
            if (begin_window(2)) {
                draw_scene(tree);
                debug_ui.end_panel();
            } else {
                scene_widgets_drawn = false;
            }
            if (begin_window(3)) {
                draw_animation();
                debug_ui.end_panel();
            } else {
                animation_widgets_drawn = false;
            }
        }

        private bool begin_window(int index) {
            if (!open[index]) {
                return false;
            }
            open[index] = debug_ui.begin_panel(titles[index], 360.0f, 0.0f);
            return open[index];
        }

        private void draw_window(int index, Action draw) {
            if (begin_window(index)) {
                draw();
                debug_ui.end_panel();
            }
        }

        private void draw_performance() {
            debug_ui.text(frame_ms > 0.0f ? $"{1000.0f / frame_ms:F1} FPS  |  {frame_ms:F2} ms/frame" : "Collecting frame times");
            if (frame_diagnostics.try_get_counters(out var counters)) {
                debug_ui.text($"Render {counters.render_width} × {counters.render_height}   Output {counters.output_width} × {counters.output_height}");
            }
            var timings = frame_diagnostics.gpu_timing_text();
            debug_ui.text(string.IsNullOrWhiteSpace(timings) ? "GPU timings unavailable" : timings);
        }

        private void draw_rendering() {
            var render = project_config.render;
            if (exposure.number != render.exposure && post_processing.try_set_exposure(exposure.number)) {
                render.exposure = exposure.number;
            }
            exposure.number = render.exposure;
            debug_ui.slider("Exposure", exposure, 0.05f, 4.0f);
            var curve = render.effective_tone_mapping();
            if (tone.selected != curve && post_processing.try_set_tone_mapping(tone.selected)) {
                render.tone_mapping = tone.selected;
                curve = tone.selected;
            }
            tone.selected = curve;
            debug_ui.combo("Tone mapping", tone_names, tone);
            var lighting = project_config.lighting;
            var area = lighting.derive_area_light(default);
            var direction = area.enabled && lighting.area_light_irradiance ? area.to_light_dir : lighting.sun_direction;
            if (!area.enabled || !lighting.area_light_irradiance) {
                var value = sun.vector;
                if ((value.x != direction.x || value.y != direction.y || value.z != direction.z) &&
                    scene_lighting.try_set_sun_direction(value)) {
                    lighting.sun_direction = value;
                    direction = value;
                }
                sun.vector = direction;
                debug_ui.vec3("Sun direction", sun, 0.02f);
            } else {
                debug_ui.text("Sun direction follows the area light");
            }
            checkbox("Bloom", bloom, ref render.bloom, enabled => post_processing.try_set_bloom(enabled ? 1.0f : 0.0f, 0.75f));
            checkbox("FXAA", fxaa, ref render.fxaa, enabled => post_processing.try_set_fxaa(enabled));
            checkbox("TAA", taa, ref render.taa, enabled => post_processing.try_set_taa(enabled, render.taa_blend_static, render.taa_blend_min));
            checkbox("SSAO", ssao, ref render.ssao, enabled => post_processing.try_set_ssao(enabled, render.ssao_radius, render.ssao_intensity, (uint)render.ssao_samples));
            checkbox("Raster shadows", shadow, ref render.shadow, enabled => scene_lighting.try_set_shadow_enabled(enabled));
            checkbox("G-buffer debug", gbuffer, ref render.gbuffer_debug, enabled => post_processing.try_set_gbuffer_debug(enabled));
        }

        private static void checkbox(string label, ui_value cell, ref bool current, Func<bool, bool> apply) {
            if (cell.flag != current && apply(cell.flag)) {
                current = cell.flag;
            }
            cell.flag = current;
            debug_ui.checkbox(label, cell);
        }

        private void draw_scene(scene_tree tree) {
            if (!ReferenceEquals(inspected_tree, tree)) {
                inspected_tree = tree;
                expanded.Clear();
                selected = default;
                scene_widgets_drawn = false;
            }
            if (scene_widgets_drawn) {
                if (selection.selected != drawn_selection) {
                    var row = selection.selected - 1;
                    selected = row >= 0 && row < rows.Count ? rows[row].node : default;
                } else if (selected.is_valid) {
                    if (take_click(expand) && !expanded.Add(selected)) {
                        expanded.Remove(selected);
                    }
                    if (visibility.flag != drawn_visibility && tree.try_set_visible(selected, visibility.flag)) {
                        if (visibility.flag) {
                            hidden.Remove(selected);
                        } else {
                            hidden.Add(selected);
                        }
                    }
                }
            }
            expand.clear();
            rows.Clear();
            labels.Clear();
            labels.Add("Select a node");
            if (tree.try_get_root_count(out var roots)) {
                for (uint i = 0; i < roots && rows.Count < 128; ++i) {
                    if (tree.try_get_root(i, out var root)) {
                        append_node(tree, root, "(root)", 0);
                    }
                }
            }
            drawn_selection = rows.FindIndex(row => row.node == selected) + 1;
            selection.selected = drawn_selection;
            debug_ui.combo("Nodes", labels.ToArray(), selection);
            if (rows.Count == 128) {
                debug_ui.text("128 nodes shown. Collapse a branch to see more.");
            }
            if (drawn_selection == 0) {
                selected = default;
                scene_widgets_drawn = true;
                debug_ui.text("Select a node to inspect its transform");
                return;
            }
            var current = rows[drawn_selection - 1];
            debug_ui.text($"Name: {current.name}\nParent: {current.parent}");
            if (tree.try_get_transform(selected, out var pose)) {
                debug_ui.text($"Local position: {format(pose.position)}\nLocal rotation (deg): {format(pose.rotation_degrees)}\nLocal scale: {format(pose.scale)}");
            } else {
                debug_ui.text("Local transform unavailable");
            }
            drawn_visibility = !hidden.Contains(selected);
            visibility.flag = drawn_visibility;
            debug_ui.checkbox("Local visibility", visibility);
            if (tree.try_get_child_count(selected, out var children) && children != 0) {
                debug_ui.button(expanded.Contains(selected) ? "Collapse children" : "Expand children", expand);
            }
            scene_widgets_drawn = true;
        }

        private void append_node(scene_tree tree, node_id node, string parent, int depth) {
            if (!tree.try_get_name(node, out var name) || string.IsNullOrEmpty(name)) {
                name = "(unnamed)";
            }
            tree.try_get_child_count(node, out var children);
            var branch = expanded.Contains(node);
            rows.Add((node, name, parent));
            labels.Add($"{new string(' ', depth * 2)}{(children == 0 ? "" : branch ? "[-] " : "[+] ")}{name}{(hidden.Contains(node) ? " (hidden)" : "")}");
            for (uint i = 0; branch && depth < 31 && i < children && rows.Count < 128; ++i) {
                if (tree.try_get_child(node, i, out var child)) {
                    append_node(tree, child, name, depth + 1);
                }
            }
        }

        private void draw_animation() {
            if (!animation_player.try_get_info(out var animation) || !animation.available || animation.clip_count <= 0) {
                clip_names = [];
                animation_widgets_drawn = false;
                debug_ui.text("No animation clips in this scene");
                return;
            }
            if (animation_widgets_drawn && animation.current_clip == drawn_animation.current_clip) {
                if (clip.selected != drawn_animation.current_clip + 1) {
                    if (clip.selected > 0) {
                        animation_player.try_select(clip.selected - 1);
                    }
                } else if (animation.has_active) {
                    if (playing.flag != drawn_animation.playing) {
                        animation_player.try_set_playing(playing.flag);
                    }
                    if (speed.number != drawn_animation.speed) {
                        animation_player.try_set_speed(speed.number);
                    }
                    if (time.number != drawn_animation.time) {
                        animation_player.try_pause();
                        animation_player.try_seek(time.number);
                    }
                }
            }
            if (clip_names.Length != animation.clip_count + 1 || !animation_widgets_drawn) {
                clip_names = ["Select a clip", ..animation_player.get_clip_names()];
            }
            drawn_animation = animation;
            animation_widgets_drawn = true;
            clip.selected = animation.current_clip + 1;
            debug_ui.combo("Clip", clip_names, clip);
            if (animation.has_active) {
                playing.flag = animation.playing;
                speed.number = animation.speed;
                time.number = animation.time;
                debug_ui.checkbox("Playing", playing);
                debug_ui.slider("Speed", speed, 0.0f, animation_player.max_speed);
                if (animation.duration > 0.0f) {
                    debug_ui.slider("Position (s)", time, 0.0f, animation.duration);
                }
            }
        }

        private static string format(vector_3 value) => $"X {value.x:F2}   Y {value.y:F2}   Z {value.z:F2}";

        private static bool take_click(ui_value cell) {
            var clicked = cell.flag;
            cell.clear();
            return clicked;
        }

        public void Dispose() {
            foreach (var cell in window_buttons) {
                cell.Dispose();
            }
            foreach (var cell in new[] { screenshot, exposure, tone, sun, bloom, fxaa, taa, ssao, shadow, gbuffer,
                                        selection, expand, visibility, clip, playing, speed, time }) {
                cell.Dispose();
            }
        }
    }
}
