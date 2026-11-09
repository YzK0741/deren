/**
 * @file source/app/app_ui.cppm
 * @defgroup app_ui Application Debug GUI
 * @brief the demo's Dear ImGui debug overlay: the live widget state the frame loop holds
 *        (gui_bindings), the panel assembly (setup_gui) and the once-per-frame push of the
 *        overlay's punctual-light slots into the runtime (apply_point_lights). Demo/application
 *        layer only - these helpers know about deren.engine.runtime, deren.engine.animation and
 *        the gui's panel types, never about glTF (the loader's diagnostics live in
 *        gltf_loader).
 * @note interface only - the implementations live in app_ui.cpp (the module follows the
 *       split interface/implementation pattern of the other app modules)
 */

module;

// gui_bindings carries glm::vec3 (the environment color, a plain value the panel edits), so the
// interface needs glm's declarations textually. Nothing else from a header is load-bearing here.
#include <glm/glm.hpp>

export module deren.app.ui;

export import deren.utility.shared;

export import deren.vstd;
import deren.application_configuration;
import deren.engine.animation; // setup_gui builds the animation playback controls
import deren.engine.runtime;

export namespace deren::app {
    /**
     * @ingroup app_ui
     * @brief live state the debug-gui widgets bind to. Held by main (the frame loop keeps the
     *        fps text and the animation mirrors in sync each frame); setup_gui() wires the
     *        widgets to these fields.
     * @note defaults mirror the historic demo values; main overrides the ones that come from
     *       config ([render] shadow toggles) or runtime state (animation playing)
     */
    struct gui_bindings {
        double fps = 0.0;                  // fps text (updated once per second when use_gui)
        bool cull_enabled = true;          // frustum-culling checkbox (write-through to the runtime)
        bool shadow_enabled = true;        // shadow checkbox (initial: settings.render.shadow)
        float shadow_bias_constant = 0.0f; // shadow depth-bias sliders (constant factor)
        float shadow_bias_slope = 1.5f;    // shadow depth-bias sliders (slope factor)
        std::int32_t shadow_cascades = 3;  // cascade-count combo (1..4; index 0 = single map)
        float shadow_cascade_blend = 0.1f; // cascade blend-band slider (fraction of the range)
        float anim_time = 0.0f;            // animation time slider (mirror of the controller clock)
        bool anim_playing = true;          // play/pause checkbox (mirror of controller state)
        std::int32_t anim_index = 0;       // animation combo selection (0 = the auto-played one)
        std::int32_t current_camera = 0;   // camera combo selection (0 = orbit, 1..N = authored)
        std::int32_t render_mode = 0;      // render-mode combo (0 = pbr, 1 = unlit); main applies it
                                           // between frames via set_default_pipeline
        std::int32_t brdf_model = 0;       // brdf-model combo (0 = GGX+joint, 1 = GGX+height-corr,
                                           // 2 = Beckmann, 3 = Blinn-Phong); write-through to runtime
        std::int32_t diffuse_model = 0;    // diffuse combo (0 = Lambert, 1 = Oren-Nayar)
        float environment_rotation = 0.0f;
        float environment_strength = 1.0f;
        std::int32_t environment_background = 0;
        glm::vec3 environment_color = {0.18f, 0.18f, 0.18f};
        float environment_blur = 0.5f;
        std::int32_t environment_index = 0;
        std::int32_t tone_mapping = 0;
        deren::utility::vessel::vector<deren::utility::vessel::string> environment_names;
        deren::utility::vessel::string environment_status;
        float exposure = 1.0f; // linear exposure slider (runtime::set_exposure)
        // bloom on/off (M9): the checkbox gates the whole chain; the intensity slider keeps its value
        // while it is off, so toggling back restores the previous look. main() mirrors it by pushing an
        // intensity of 0 when the box is clear, which is what runtime::active_features() gates the
        // bloom passes on.
        bool bloom_enabled = true;
        float bloom_intensity = 0.8f;      // bloom blend weight slider (runtime::set_bloom; 0 = off)
        std::int32_t toon_bands_index = 0; // cel-shading combo: index into toon_band_counts (0 = plain PBR)
        float sun_intensity = 1.0f;        // a scale on the sun's radiance (0..3; 1.0 = unchanged)
        float toon_softness = 0.15f;       // cel-shading band edge softness slider (smaller = harder edges)
        float bloom_threshold = 0.35f;     // bloom bright-pass threshold (visible range 0..0.75)
        // FXAA (runtime::set_fxaa): checkbox + the two shader knobs. The checkbox is mirrored by
        // main into the runtime every frame like the other post-process values.
        bool fxaa_enabled = false;          // FXAA on/off (initial: settings.render.fxaa)
        float fxaa_subpixel = 0.75f;        // sub-pixel term strength (0 = pure directional blend)
        float fxaa_edge_threshold = 0.166f; // relative luma contrast below which a pixel is "flat"
        // G-buffer debug view (runtime::set_gbuffer_debug / set_gbuffer_channel): the deferred
        // path's stored surface, one channel at a time. Mirrored into the runtime every frame like
        // the FXAA state, so these fields carry the config's initial values.
        bool gbuffer_debug = false;       // draw the G-buffer + its debug view instead of the shaded scene
        std::int32_t gbuffer_channel = 1; // 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id, 6 depth, 7 flags, 8 motion
        // TAA (runtime::set_taa): the engine's anti-aliasing. Mirrored into the runtime every
        // frame like the other render toggles; the blend weights are the two shader knobs.
        bool taa_enabled = false;
        float taa_blend_static = 0.9f; // history weight for a static pixel (0.9 = 10% of the new frame)
        float taa_blend_min = 0.5f;    // history weight floor under motion (lower = less ghosting)
        // The TOON CHARACTER STAGE (runtime::set_character_forward): re-shades the scene's opaque leaves OVER
        // the lit frame so a character can carry its shading. Mirrored into the runtime every frame like
        // the other render toggles, and the overlay only offers it when the renderer registered the pipeline
        // (see deren::app::app_renderer::feature_available).
        bool character_forward = false;
        // WHICH SHADING MODEL THAT STAGE DRAWS WITH (runtime::set_goo_toon): false = the old chain
        // (`character_forward.slang`), true = the rewritten one (`goo_toon.slang`, see
        // `runtime::goo_toon_pipeline_name`). A SECOND SWITCH RATHER THAN ONE, because the two answer different
        // questions - whether the stage runs, and which model it runs - and the rewrite is verified by holding the
        // first fixed and moving the second. Mirrored into the runtime every frame like the other render toggles.
        // WHICH SHADING MODEL THAT STAGE DRAWS WITH, SECOND OPT-IN (runtime::set_game_toon): true = the
        // game-shader model (`game_toon.slang`, see `runtime::game_toon_pipeline_name`). A THIRD MODEL beside
        // the old chain and Goo, not a replacement; the two model switches are separate so the A/B can move one
        // at a time. Mirrored into the runtime every frame like the other render toggles.
        bool game_toon = false;
        // Four editable punctual lights (the runtime supports a larger set): the gui rows below
        // edit these fields live (no per-widget callbacks), and main() pushes the enabled set to
        // the runtime once per frame via deren::app::apply_point_lights(). Each slot is a point light
        // or - with `spot` set - a cone light (direction + inner/outer half-angles in degrees;
        // apply_point_lights clamps inner <= outer < 90 and converts to the shader's cosines).
        // Plain C arrays keep the light slots glm-free.
        struct light_slot {
            bool enabled = false;
            bool spot = false;                        // false = point light (omni), true = spot cone
            float position[3] = {0.0f, 0.0f, 0.0f};   // world position
            float direction[3] = {0.0f, -1.0f, 0.0f}; // spot axis (spot only; normalized when pushed)
            float color[3] = {1.0f, 1.0f, 1.0f};      // linear color
            float intensity = 1.0f;
            float range = 10.0f;          // 0 = infinite falloff
            float inner_cone_deg = 20.0f; // spot: soft inner half-angle (degrees)
            float outer_cone_deg = 30.0f; // spot: hard cutoff half-angle (degrees)
        };
        light_slot point_lights[4] = {};
        // Which slot the panel's punctual-light group is editing (0-based, clamped by the panel). The group
        // draws ONE slot at a time - four slots of nine controls each was thirty-six rows of panel for a
        // feature most frames leave off - so this index is what the "punctual light" combo writes and every
        // widget of the group tests.
        std::int32_t active_light = 0;
        // clustered light culling (M5): checkbox mirrored into the runtime every frame
        // (runtime::set_clustered_lights); false = the brute-force loop over every light
        bool clustered_lights = true;
        // screen-space ambient occlusion (M6, deferred path): mirrored into the runtime every frame
        bool ssao_enabled = true;
        float ssao_radius = 0.5f;
        float ssao_intensity = 1.0f;
        float ssao_samples = 8.0f; // slider value; main rounds it into the runtime call
        // LIVE SCENE-OBJECT EDITING ([scene.objects]). The vector is HELD BY main's app_settings
        // (stable for the whole run because the config loader never resizes it after startup), and
        // this pointer is null when the historic single-object path is running. main applies the
        // edited values to the matching wrapper nodes once per frame when the dirty flag is set -
        // the widgets deliberately write the same structs the config produced, so the GUI and the
        // TOML describe one scene rather than two. Edits are runtime-only; nothing is written back.
        deren::utility::vessel::vector<deren::app_config::scene_object_settings>* scene_objects = nullptr;
        // The historic single-model path also feeds one implicit entry through `scene_objects`, so
        // clicking the model can select and move it. That entry has no asset list, pipeline choice,
        // per-object environment or animation controller; the panel shows only the rows this path
        // can actually apply.
        bool object_transform_only = false;
        // One controller per object (phase 2), aligned with scene_objects; null when the historic
        // single-object path runs. The names are captured once at setup for the per-object clip combo.
        deren::utility::vessel::vector<deren::engine::animation::controller*>* object_animations = nullptr;
        deren::utility::vessel::vector<deren::utility::vessel::vector<deren::utility::vessel::string>> object_animation_names = {};
        // Per-object pipeline choices. The combo binds an INDEX, so the visible list is filtered to the
        // pipelines that the object's asset supports and the callback maps the index back to the value.
        // Aligned with scene_objects; filled by main before setup_gui.
        struct object_pipeline_option {
            std::int32_t value = 0;
            deren::utility::vessel::string name = {};
        };
        deren::utility::vessel::vector<deren::utility::vessel::vector<object_pipeline_option>> object_pipeline_options = {};
        deren::utility::vessel::vector<std::int32_t> object_pipeline_choice = {};
        // Aligned with scene_objects. Gaussian clouds are staged by the splat path and have no
        // environment/material override or skeletal animation; showing those widgets would let the
        // operator edit state that the renderer ignores.
        deren::utility::vessel::vector<bool> object_is_gaussian = {};
        // Per-object environment combo items: "Scene default" followed by environment_names, so index 0 keeps
        // the object on the scene-wide IBL and 1..N pick an HDR from the same list the global combo uses.
        deren::utility::vessel::vector<deren::utility::vessel::string> object_environment_names = {};
        std::int32_t selected_object = -1;         // -1 = nothing selected
        bool scene_object_transform_dirty = false; // set by any object widget, consumed by main
        // Re-opens the "scene object" panel after the user closed it, so clicking an object in the
        // viewport always brings its editor back. Empty when the panel was not built.
        std::function<void()> reopen_object_panel = {};
    };

    /**
     * @ingroup app_ui
     * @brief build the demo's Dear ImGui debug overlay when @p use_gui: enable it on the
     *        runtime and assemble the "deren debug" panel — fps label, frustum-culling
     *        / skybox / shadow toggles, the camera-target drag, animation playback controls
     *        (label + play + time scrubber + animation dropdown when the controller has an
     *        active animation), the camera selector (when @p camera_names is non-empty), the
     *        shadow depth-bias sliders and the punctual light rows (per-slot enable / position /
     *        color / intensity / range plus spot cone editing: spot toggle, direction, inner and
     *        outer half-angles).
     * @param runtime the initialized runtime (enable_debug_gui() is called here)
     * @param use_gui whether the overlay is wanted ([gui] show); no-op when false
     * @param settings startup settings: [gui] panel size + [render] initial skybox/shadow states
     * @param bindings live widget state (see gui_bindings); the frame loop updates fps and the
     *        animation mirrors each frame
     * @param animation the animation controller the playback widgets drive (may be idle)
     * @param camera_names display names of the scene's authored cameras (no "orbit" entry is
     *        added here — setup_gui prepends it); empty disables the camera selector
     * @param on_camera_selected called with the combo index (0 = orbit, i = camera_names[i-1])
     *        when the user picks a camera; empty when the selector is not shown
     * @note when gui_bindings::scene_objects is set, setup_gui also adds a "scene object" panel
     *       whose rows only appear for the selected object; main holds selection (see its click
     *       pick) and applies the edited transforms
     * @note the overlay's glTF-side data (authored camera list, orbit seeding) stays in main:
     *       app_ui only ever sees display names and a callback, never glTF types
     */
    void setup_gui(deren::engine::runtime& runtime,
                   bool use_gui,
                   deren::app_config::app_settings const& settings,
                   gui_bindings& bindings,
                   deren::engine::animation::controller& animation,
                   deren::utility::vessel::vector<deren::utility::vessel::string> const& camera_names,
                   std::function<void(std::int32_t)> const& on_camera_selected);

    /**
     * @ingroup app_ui
     * @brief push the enabled punctual-light slots of @p bindings into the runtime's light UBO.
     *        Called once per frame from main (while the gui is active): the gui widgets edit
     *        bindings.point_lights live, so a drag/toggle becomes visible next frame without
     *        per-widget callbacks. Each slot is pushed as a point light, or as a spot light when
     *        its `spot` flag is set (direction + clamped inner/outer cone angles). Cheap no-op
     *        when nothing is enabled.
     * @param runtime the runtime whose light UBO receives the slots
     * @param bindings the overlay's live light widgets (see gui_bindings)
     * @param extra additional lights appended after the overlay's slots - the [lighting] demo_lights
     *        stress set (M5). Both share the UBO's light array, so the overlay's slots win when the
     *        two together would overflow deren::engine::max_punctual_lights.
     */
    void apply_point_lights(deren::engine::runtime& runtime,
                            gui_bindings const& bindings,
                            std::span<deren::engine::punctual_light const> extra = {});
} // namespace deren::app
