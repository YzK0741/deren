module;

// Textual glm include: the definitions below name glm types, and including glm in the global
// module fragment (the same pattern source/engine/animation/controller.cpp uses) keeps clang's
// merged libc++ allocator declarations from clashing with the vendored std module under
// -fno-exceptions. Do not remove this include to "clean up".

#include <glm/glm.hpp>

module deren.app.ui;

import deren.engine.animation;
import deren.engine.runtime;
import deren.promise.gui;
import deren.application_configuration;
import deren.utility;
import deren.vstd;

namespace deren::app {
    void setup_gui(deren::engine::runtime& runtime,
                   bool const use_gui,
                   deren::app_config::app_settings const& settings,
                   gui_bindings& bindings,
                   deren::engine::animation::controller& animation,
                   deren::utility::vessel::vector<deren::utility::vessel::string> const& camera_names,
                   std::function<void(std::int32_t)> const& on_camera_selected) {
        if (!use_gui) {
            return;
        }
        // GUI ABI3 owns vessel strings and containers, matching the application.
        runtime.enable_debug_gui();
        deren::gui::panel& panel = runtime.debug_gui().add_panel("deren debug");
        panel.set_default_size(settings.gui.panel_width, settings.gui.panel_height);
        panel.add_label([&bindings] { return deren::utility::vessel::format("fps: {:>6.1f}", bindings.fps); }); // fixed-width field: a growing number must not re-wrap the panel
        // per-pass GPU milliseconds (runtime::gpu_timing_summary): the timing that steers the
        // renderer's performance work, so it sits with the fps line at the top of the panel
        panel.add_label([&runtime] { return deren::utility::vessel::string(runtime.gpu_timing_summary()); });
        // ... and the CPU phases next to it: above a few hundred fps the frame is CPU/pacing-bound,
        // so the GPU line alone no longer explains the frame time (see runtime::cpu_phase).
        panel.add_label([&runtime] { return deren::utility::vessel::string(runtime.cpu_timing_summary()); });
        panel.add_checkbox(
            "frustum culling",
            &bindings.cull_enabled,
            [&runtime](bool const enabled) { runtime.set_frustum_culling(enabled); });
        panel.add_checkbox(
            "shadow",
            &bindings.shadow_enabled,
            [&runtime](bool const enabled) { runtime.set_shadow_enabled(enabled); });
        // clustered light culling (M5): off = every active light is evaluated per pixel (the
        // brute-force reference), on = only the pixel's cluster list. Mirroring it every frame in
        // main() keeps the config and the checkbox in agreement. Offered only when the cluster
        // compute pipeline exists - without it the switch cannot do anything.
        {
            // offered whenever the compute pass exists (it works in either path)
            panel.add_checkbox("clustered lights", &bindings.clustered_lights).set_visible_when([&runtime] { return runtime.feature_available("clustered"); });
        }
        // screen-space ambient occlusion (M6): the deferred lighting stage traces the G-buffer, so the
        // whole group (switch + its three knobs) is offered only in a session whose G-buffer pipelines
        // exist - an AVAILABILITY question, which is what visible_when asks (a widget never gates on
        // feature_active: that is the runner's per-frame "this stage ran", so gating on it made the group
        // vanish the moment the user switched to unlit). The knobs then follow the switch's OWN state,
        // which is what they are attached to. The sliders edit the radius (world units), the applied
        // intensity and the sample count.
        {
            // deferred-only, via the feature registry
            panel.add_checkbox("ssao", &bindings.ssao_enabled).set_visible_when([&runtime] { return runtime.feature_available("deferred"); });
            auto make_ssao_slider = [&](deren::utility::vessel::string label, float* value, float lo, float hi) {
                panel.add_slider(std::move(label), value, lo, hi).set_visible_when([&bindings] { return bindings.ssao_enabled; });
            };
            make_ssao_slider("ssao radius", &bindings.ssao_radius, 0.05f, 3.0f);
            make_ssao_slider("ssao intensity", &bindings.ssao_intensity, 0.0f, 1.0f);
            make_ssao_slider("ssao samples", &bindings.ssao_samples, 1.0f, 16.0f);
        }
        // render mode: pbr (lit) vs unlit (flat base color, no shading). Default-semantics leaves
        // draw with the runtime's default pipeline, so this only records a combo selection here;
        // main() applies it BETWEEN frames via runtime.set_default_pipeline (the registry may
        // not be mutated while a frame records).
        panel.add_combo(
            "render mode",
            deren::utility::vessel::vector<deren::utility::vessel::string>{"pbr (lit)", "unlit (flat)"},
            &bindings.render_mode);
        // selectable BRDF theory models (pbr.frag): preset 0 is the default GGX + joint-Smith;
        // each other preset differs by exactly one piece (NDF or visibility), so the gui is a
        // live A/B comparison. CPU-side write-through (safe mid-run, see runtime::set_brdf_model).
        panel.add_combo(
            "brdf model",
            deren::utility::vessel::vector<deren::utility::vessel::string>{"GGX + joint Smith", "GGX + height-corr. Smith", "Beckmann + Smith", "Blinn-Phong + Smith"},
            &bindings.brdf_model,
            [&runtime](std::int32_t const index) { runtime.set_brdf_model(index); });
        panel.add_combo(
            "diffuse model",
            deren::utility::vessel::vector<deren::utility::vessel::string>{"Lambert", "Oren-Nayar"},
            &bindings.diffuse_model,
            [&runtime](std::int32_t const index) { runtime.set_diffuse_model(index); });
        // linear exposure applied before tonemapping (pbr.frag + skybox.frag); main pushes it
        // into the runtime every frame like the light slots
        panel.add_slider("exposure", &bindings.exposure, 0.1f, 5.0f);
        panel.add_combo("tone mapping", {"ACES (existing)", "Khronos PBR Neutral", "GameToon reference"}, &bindings.tone_mapping);
        if (!bindings.environment_names.empty())
            panel.add_combo("environment", bindings.environment_names, &bindings.environment_index);
        panel.add_label([&bindings] { return bindings.environment_status; });
        panel.add_slider("environment rotation", &bindings.environment_rotation, -180.0f, 180.0f);
        panel.add_slider("environment strength", &bindings.environment_strength, 0.0f, 8.0f);
        panel.add_combo("background", {"procedural sky", "solid colour", "environment", "blurred environment"}, &bindings.environment_background);
        panel.add_vec3("background linear RGB", &bindings.environment_color.x, 0.01f);
        panel.add_slider("background blur", &bindings.environment_blur, 0.0f, 1.0f);

        // bloom (bright-pass threshold + blend weight); 0 intensity disables it
        // the useful ranges: a threshold above ~0.75 leaves almost no pixel over it (so nothing
        // glows), and the intensity needed for a visible glow grows with the threshold - keeping
        // the threshold low is what makes the whole intensity slider effective
        panel.add_checkbox("bloom", &bindings.bloom_enabled);
        // the two knobs only matter while the chain runs (main pushes a 0 intensity when the box is clear)
        {
            auto bloom_knob = [&](deren::utility::vessel::string label, float* value, float lo, float hi) {
                panel.add_slider(std::move(label), value, lo, hi).set_visible_when([&bindings] { return bindings.bloom_enabled; });
            };
            bloom_knob("bloom intensity", &bindings.bloom_intensity, 0.0f, 3.0f);
            bloom_knob("bloom threshold", &bindings.bloom_threshold, 0.0f, 0.75f);
        }
        // FXAA: a checkbox plus its two shader knobs (main mirrors all three into the runtime every
        // frame). The knobs are genuine effects, not strength padding - "subpixel" trades edge
        // smoothing for the single-pixel sparkle FXAA leaves on near-axis-aligned edges, and the
        // threshold decides how much contrast counts as an edge (lower = softer whole image).
        panel.add_checkbox("fxaa", &bindings.fxaa_enabled);
        {
            // the knobs only matter while FXAA is on (and while the fxaa pipeline exists at all)
            auto make_fxaa_slider = [&](deren::utility::vessel::string label, float* value, float lo, float hi) {
                panel.add_slider(std::move(label), value, lo, hi).set_visible_when([&bindings] { return bindings.fxaa_enabled; });
            };
            make_fxaa_slider("fxaa subpixel", &bindings.fxaa_subpixel, 0.0f, 1.0f);
            make_fxaa_slider("fxaa edge threshold", &bindings.fxaa_edge_threshold, 0.05f, 0.5f);
        }
        // G-buffer debug view: what the deferred path stores - the one part of the renderer whose
        // contents cannot be judged from a shaded screenshot, so it gets a channel selector rather
        // than a strength knob. main() mirrors both fields into the runtime every frame.
        {
            panel.add_checkbox("gbuffer debug", &bindings.gbuffer_debug).set_visible_when([&runtime] { return runtime.feature_available("gbuffer-debug"); });

            panel.add_combo(
                     "gbuffer channel",
                     deren::utility::vessel::vector<deren::utility::vessel::string>{"albedo", "normal", "roughness", "metallic", "ao", "material id", "depth", "flags", "motion"},
                     &bindings.gbuffer_channel)
                .set_visible_when([&bindings] { return bindings.gbuffer_debug; });
        }
        // TAA: the engine's anti-aliasing (there is no MSAA on a G-buffer), with the two
        // history-weight knobs. The static weight decides how smooth a still image gets (higher =
        // smoother, slower to react to lighting changes); the minimum is what a fast-moving pixel
        // falls back to (lower = trusts the current frame more, which trades smoothing for less
        // ghosting).
        {
            // deferred-only, via the feature registry
            panel.add_checkbox("taa", &bindings.taa_enabled).set_visible_when([&runtime] { return runtime.feature_available("taa"); });
            auto make_taa_slider = [&](deren::utility::vessel::string label, float* value, float lo, float hi) {
                panel.add_slider(std::move(label), value, lo, hi).set_visible_when([&bindings] { return bindings.taa_enabled; });
            };
            make_taa_slider("taa history (static)", &bindings.taa_blend_static, 0.0f, 0.98f);
            make_taa_slider("taa history (min)", &bindings.taa_blend_min, 0.0f, 0.98f);
        }
        // THE TOON CHARACTER STAGE: re-shades the scene's opaque leaves OVER the lit frame, at depth-EQUAL,
        // so a character can carry its own shading instead of the deferred one - and without being lit twice.
        // Offered only when the renderer registered the pipeline the stage binds (it needs the mesh stage), so
        // a switch that would draw nothing is not shown at all - the failure mode feature_available exists for.
        {
            panel.add_checkbox("character forward (toon)", &bindings.character_forward).set_visible_when([&runtime] { return runtime.feature_available("character_forward"); });
            // ... AND WHICH CHAIN IT DRAWS WITH. Offered only while the character stage itself is on, because a
            // switch that selects a shading model for a pass that is not recording is a control with no effect -
            // and offered only when the rewritten pipeline was actually built, on the checkbox above's own terms.

            // THE GAME-SHADER MODEL. Offered only while the stage is on and only when the game pipeline was
            // actually built.
            panel.add_checkbox("game toon (game shader)", &bindings.game_toon).set_visible_when([&runtime, &bindings] { return bindings.character_forward && runtime.game_toon_ready(); });
        }
        // cel/toon shading: quantize the diffuse falloff (and harden shadows/highlights);
        // 0 steps leaves plain PBR, softness shrinks toward hard comic edges
        // cel/toon shading is discrete: every listed band count gives a visibly different look
        // (more bands converge back to smooth PBR, so a continuous slider had dead zones).
        // Softness stays small - a wide band edge erases the steps entirely.
        panel.add_combo(
            "toon shading",
            deren::utility::vessel::vector<deren::utility::vessel::string>{"off (plain pbr)", "2 bands (hardest)", "3 bands", "4 bands", "5 bands", "6 bands", "8 bands (softest)"},
            &bindings.toon_bands_index);
        panel.add_slider("toon softness", &bindings.toon_softness, 0.01f, 0.25f);
        panel.add_slider("sun intensity", &bindings.sun_intensity, 0.0f, 3.0f);
        // ---- punctual lights (demo lights; see apply_point_lights): the widgets edit
        //      bindings.point_lights live and main() pushes the enabled set once per frame.
        //      Each slot is a point light or - with `spot` checked - a cone light -------
        // WHICH SLOT THE GROUP BELOW EDITS: the combo doubles as the group's header, so "which light am I
        // looking at" and "how tall is this group" are both answered here instead of by scrolling past
        // thirty-six rows to find the one light that is on.
        {
            deren::utility::vessel::vector<deren::utility::vessel::string> light_items;
            light_items.reserve(std::size(bindings.point_lights));
            for (std::size_t i = 0; i < std::size(bindings.point_lights); ++i) {
                light_items.push_back(deren::utility::vessel::format("punctual light {}", i + 1));
            }
            panel.add_combo("punctual light", std::move(light_items), &bindings.active_light);
        }
        for (std::size_t i = 0; i < std::size(bindings.point_lights); ++i) {
            gui_bindings::light_slot& slot = bindings.point_lights[i];
            // ONE SLOT AT A TIME, chosen by the combo above: every widget of every slot is built once (the
            // panel is a fixed list), but a slot only draws while it is the selected one - four slots of nine
            // controls each was thirty-six rows of panel for a feature most frames leave off, which buried
            // everything below it. The cone knobs go one step further and appear only for a slot that is
            // actually a spot, so an omni slot is seven rows and a cone slot ten.
            auto const selected = [&bindings, i] { return bindings.active_light == static_cast<std::int32_t>(i); };
            auto const selected_spot = [&bindings, &slot, i] { return bindings.active_light == static_cast<std::int32_t>(i) && slot.spot; };
            {
                panel.add_checkbox("  enabled", &slot.enabled).set_visible_when(selected);
            }
            {
                panel.add_vec3("  position", slot.position, 0.1f).set_visible_when(selected);
            }
            {
                panel.add_vec3("  color", slot.color, 0.02f).set_visible_when(selected);
            }
            {
                panel.add_slider("  intensity", &slot.intensity, 0.0f, 50.0f).set_visible_when(selected);
            }
            {
                panel.add_slider("  range", &slot.range, 0.1f, 100.0f).set_visible_when(selected);
            }
            {
                panel.add_checkbox("  spot", &slot.spot).set_visible_when(selected);
            }
            {
                panel.add_vec3("  direction", slot.direction, 0.1f).set_visible_when(selected_spot);
            }
            {
                panel.add_slider("  inner cone deg", &slot.inner_cone_deg, 0.0f, 89.0f).set_visible_when(selected_spot);
            }
            {
                panel.add_slider("  outer cone deg", &slot.outer_cone_deg, 1.0f, 89.0f).set_visible_when(selected_spot);
            }
        }
        // ---- LIVE SCENE-OBJECT EDITING ------------------------------------------------
        // A SECOND PANEL, not more rows in the main one: an object has its own transform
        // vocabulary, and the main panel already carries every render/light knob. Only the
        // selected object's three rows draw; selection is owned by main because it is also what
        // the viewport click pick writes. The widgets edit app_settings.scene.objects in place
        // and raise the dirty flag main consumes - edits are runtime-only, never written back.
        if (bindings.scene_objects != nullptr && !bindings.scene_objects->empty()) {
            deren::gui::panel& object_panel = runtime.debug_gui().add_panel("scene object");
            object_panel.set_default_size(settings.gui.panel_width, 460.0f);
            // Keep a way back after the user closes the panel: clicking an object re-opens it.
            deren::gui::panel* const object_panel_ptr = &object_panel;
            bindings.reopen_object_panel = [object_panel_ptr] { object_panel_ptr->set_open(true); };
            object_panel.add_label([&bindings] {
                deren::utility::vessel::vector<deren::app_config::scene_object_settings> const& objects = *bindings.scene_objects;
                if (bindings.selected_object < 0 || static_cast<std::size_t>(bindings.selected_object) >= objects.size()) {
                    return deren::utility::vessel::string("no object selected (click one)");
                }
                deren::app_config::scene_object_settings const& object = objects[static_cast<std::size_t>(bindings.selected_object)];
                deren::utility::vessel::string const name = object.name.empty() ? deren::utility::vessel::string("<unnamed>") : object.name;
                switch (object.pipeline) {
                case 3:
                    return deren::utility::vessel::format("{}: {} - game_toon, head slot {}", bindings.selected_object, name, bindings.selected_object);
                case 4:
                    return deren::utility::vessel::format("{}: {} - gaussian cloud, splat stage", bindings.selected_object, name);
                case 2:
                    return deren::utility::vessel::format("{}: {} - deferred unlit", bindings.selected_object, name);
                default:
                    return deren::utility::vessel::format("{}: {} - deferred lit", bindings.selected_object, name);
                }
            });
            auto const mark_dirty = [&bindings] { bindings.scene_object_transform_dirty = true; };
            for (std::size_t i = 0; i < bindings.scene_objects->size(); ++i) {
                deren::app_config::scene_object_settings& object = (*bindings.scene_objects)[i];
                auto const selected = [&bindings, i] { return bindings.selected_object == static_cast<std::int32_t>(i); };
                object_panel.add_vec3("  position", &object.position[0], 0.05f, mark_dirty).set_visible_when(selected);
                object_panel.add_vec3("  rotation", &object.rotation[0], 1.0f, mark_dirty).set_visible_when(selected);
                object_panel.add_vec3("  scale", &object.scale[0], 0.01f, mark_dirty).set_visible_when(selected);
                object_panel.add_checkbox("  visible", &object.visible, [mark_dirty](bool) { mark_dirty(); }).set_visible_when(selected);
                object_panel.add_checkbox("  cast shadow", &object.cast_shadow, [mark_dirty](bool) { mark_dirty(); }).set_visible_when(selected);
                if (i < bindings.object_pipeline_options.size() && i < bindings.object_pipeline_choice.size()) {
                    auto const& options = bindings.object_pipeline_options[i];
                    deren::utility::vessel::vector<deren::utility::vessel::string> labels;
                    labels.reserve(options.size());
                    for (auto const& option : options) {
                        labels.push_back(option.name);
                    }
                    object_panel
                        .add_combo("  pipeline", labels, &bindings.object_pipeline_choice[i],
                                   [&object, &bindings, i, mark_dirty](std::int32_t choice) {
                                       auto const& choices = bindings.object_pipeline_options[i];
                                       if (choice >= 0 && static_cast<std::size_t>(choice) < choices.size()) {
                                           object.pipeline = choices[static_cast<std::size_t>(choice)].value;
                                       }
                                       mark_dirty();
                                   })
                        .set_visible_when(selected);
                }
                if (!bindings.object_environment_names.empty()) {
                    object_panel
                        .add_combo("  environment", bindings.object_environment_names, &object.environment_index,
                                   [&object, mark_dirty](std::int32_t) {
                                       object.environment_slot = 0; // the scene-wide IBL until the custom slot is ready
                                       mark_dirty();
                                   })
                        .set_visible_when(selected);
                }
                object_panel.add_checkbox("  material override", &object.material_override, [mark_dirty](bool) { mark_dirty(); }).set_visible_when(selected);
                object_panel.add_vec3("  tint", &object.tint[0], 0.01f, mark_dirty)
                    .set_visible_when([&bindings, &object, i] { return bindings.selected_object == static_cast<std::int32_t>(i) && object.material_override; });
                object_panel.add_slider("  metallic", &object.metallic_scale, 0.0f, 4.0f, [mark_dirty](float) { mark_dirty(); })
                    .set_visible_when([&bindings, &object, i] { return bindings.selected_object == static_cast<std::int32_t>(i) && object.material_override; });
                object_panel.add_slider("  roughness", &object.roughness_scale, 0.0f, 4.0f, [mark_dirty](float) { mark_dirty(); })
                    .set_visible_when([&bindings, &object, i] { return bindings.selected_object == static_cast<std::int32_t>(i) && object.material_override; });
                object_panel.add_slider("  emissive", &object.emissive_scale, 0.0f, 4.0f, [mark_dirty](float) { mark_dirty(); })
                    .set_visible_when([&bindings, &object, i] { return bindings.selected_object == static_cast<std::int32_t>(i) && object.material_override; });
                // Phase 2: this placement's own clock. The clip combo is drawn only when this
                // object has more than one channel-bearing clip; index 0 is always valid.
                object_panel.add_slider("  speed", &object.animation_speed, 0.0f, 8.0f, [mark_dirty](float) { mark_dirty(); })
                    .set_visible_when(selected);
                object_panel.add_checkbox("  playing", &object.animation_playing, [mark_dirty](bool) { mark_dirty(); }).set_visible_when(selected);
                if (bindings.object_animation_names.size() > i && bindings.object_animation_names[i].size() > 1) {
                    object_panel
                        .add_combo("  clip", bindings.object_animation_names[i], &object.animation, [mark_dirty](std::int32_t) { mark_dirty(); })
                        .set_visible_when(selected);
                }
            }
        }
        // camera orbit target: dragging it moves what the camera looks at / orbits around
        // (camera.target is a glm::vec3, i.e. three contiguous floats; the runtime rebuilds the
        // camera UBO from it every frame, so no on_change callback is needed)
        panel.add_vec3("camera target", &runtime.camera.target.x, 0.05f);
        // playback controls (only when the model carries animations): play/pause toggle bound
        // to the playback state, a time scrubber (pauses on drag so the clock cannot fight the
        // scrub; the play checkbox resumes), and - for multi-animation assets - a dropdown to
        // pick which animation plays. All playback state lives in the animation::controller.
        if (animation.has_active()) {
            panel.add_label([&animation] {
                return deren::utility::vessel::format("animation '{}' ({}s)", animation.active_name(), animation.loop_duration());
            });
            panel.add_checkbox(
                "play",
                &bindings.anim_playing,
                [&animation](bool const enabled) { animation.set_playing(enabled); });
            panel.add_slider(
                "time",
                &bindings.anim_time,
                0.0f,
                animation.playable_max_duration(),
                [&animation](float const value) {
                    animation.set_time(value); // scrubbing pauses so the clock does not fight the drag
                });
            if (animation.playable_count() > 1) {
                deren::utility::vessel::vector<deren::utility::vessel::string> names;
                names.reserve(animation.playable_count());
                for (std::size_t i = 0; i < animation.playable_count(); ++i) {
                    names.push_back(deren::utility::vessel::string(animation.playable_name(i)));
                }
                panel.add_combo(
                    "animation",
                    std::move(names),
                    &bindings.anim_index,
                    [&animation](std::int32_t const index) { animation.select(static_cast<std::size_t>(index)); });
            }
            deren::utility::log("gui: playback controls added ({} animation(s))", animation.playable_count());
        }
        // camera selector: "orbit" (free) or any scene camera (its pose seeds the orbit camera,
        // so the mouse keeps working after switching)
        if (!camera_names.empty()) {
            deren::utility::vessel::vector<deren::utility::vessel::string> items;
            items.reserve(camera_names.size() + 1);
            items.push_back("orbit");
            items.insert(items.end(), camera_names.begin(), camera_names.end());
            panel.add_combo(
                "camera",
                std::move(items),
                &bindings.current_camera,
                on_camera_selected);
            deren::utility::log("gui: camera selector added ({} camera(s))", camera_names.size());
        }
        // Cascaded shadow maps: how many cascades the sun's shadow pass fills (1 = the historic
        // single map) and how much of a cascade's range fades into the next one. Both are
        // write-through: the runtime refits the cascade boxes on the next frame, and the blend is a
        // shader constant in the light UBO - no pipeline or image rebuild, so they are live.
        panel.add_combo(
            "shadow cascades",
            deren::utility::vessel::vector<deren::utility::vessel::string>{"1 (single map)", "2", "3", "4"},
            &bindings.shadow_cascades,
            [&runtime](std::int32_t const index) { runtime.set_shadow_cascades(static_cast<std::uint32_t>(index) + 1u); });
        panel.add_slider(
            "shadow cascade blend",
            &bindings.shadow_cascade_blend,
            0.0f,
            0.5f,
            [&runtime](float const value) { runtime.set_shadow_cascade_blend(value); });
        // Shadow depth bias (bottom of the panel - a rarely-used tuning aid): the pass's bias
        // is dynamic state applied every frame; the slope factor removes acne on angled
        // surfaces, the constant adds a fixed push. Note it cannot fix geometry that is simply
        // too coarse (e.g. RecursiveSkeletons' sides are large flat triangles - the depth
        // gradient across them is what it is), it only tunes the bias offset.
        panel.add_slider(
            "shadow bias slope",
            &bindings.shadow_bias_slope,
            0.0f,
            10.0f,
            [&runtime, &bindings](float const value) {
                bindings.shadow_bias_slope = value;
                runtime.set_shadow_depth_bias(bindings.shadow_bias_constant, bindings.shadow_bias_slope, 0.0f);
            });
        panel.add_slider(
            "shadow bias constant",
            &bindings.shadow_bias_constant,
            0.0f,
            10.0f,
            [&runtime, &bindings](float const value) {
                bindings.shadow_bias_constant = value;
                runtime.set_shadow_depth_bias(bindings.shadow_bias_constant, bindings.shadow_bias_slope, 0.0f);
            });
        deren::utility::log("gui: Dear ImGui debug overlay enabled");
    }

    void apply_point_lights(deren::engine::runtime& runtime, gui_bindings const& bindings, std::span<deren::engine::punctual_light const> const extra) {
        // build the enabled demo lights into a fixed stack array (limit = the light UBO's
        // array size) and push it; the span form keeps set_point_lights cheap to call per frame.
        // `extra` is the [lighting] demo_lights stress set (M5), appended after the overlay's slots
        // so the overlay keeps working - both share the UBO's light array, so the overlay's slots
        // win when the two together would overflow it.
        deren::utility::vessel::array<deren::engine::punctual_light, deren::engine::max_punctual_lights> active = {};
        std::uint32_t count = 0;
        for (gui_bindings::light_slot const& slot : bindings.point_lights) {
            if (!slot.enabled || count >= deren::engine::max_punctual_lights) {
                continue;
            }
            deren::engine::punctual_light& light = active[count++];
            light.position = glm::vec3(slot.position[0], slot.position[1], slot.position[2]);
            light.color = glm::vec3(slot.color[0], slot.color[1], slot.color[2]);
            light.intensity = slot.intensity;
            light.range = slot.range;
            light.spot = slot.spot;
            if (slot.spot) {
                light.spot_direction = glm::vec3(slot.direction[0], slot.direction[1], slot.direction[2]);
                // clamp the cone (inner <= outer, both < 90 deg) and hand the shader the cosines
                float const outer_deg = std::clamp(slot.outer_cone_deg, 1.0f, 89.0f);
                float const inner_deg = std::clamp(slot.inner_cone_deg, 0.0f, outer_deg);
                light.spot_outer_cos = std::cos(glm::radians(outer_deg));
                light.spot_inner_cos = std::cos(glm::radians(inner_deg));
            }
        }
        for (deren::engine::punctual_light const& light : extra) {
            if (count >= deren::engine::max_punctual_lights) {
                break;
            }
            active[count++] = light;
        }
        runtime.set_point_lights(std::span(active.data(), count));
    }
} // namespace deren::app
