module;

// GLFW is confined to the deren.app.glfw module; this translation unit only names its wrapper types.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
// The reflect-probe cube writer (IBL1) lives outside the module: it is a file format helper, not a
// module interface. Included in the GLOBAL fragment so its std headers do not join the module.
#include "radiance_cube_capture.hpp"
module deren.app.application;

import :capture;
import :environment_loader;
import :startup_splash;

import deren.ui.rml_document;
import deren.ui.rml_overlay;
import deren.ui.rml_scripting;
import deren.vstd;
import deren.utility.shared;
import deren.utility; // re-exports frame_clock / frame_stats / bvh / vessel / thread_pool / data_block
import deren.asset_library;
import deren.application_configuration;

import deren.app.config;
import deren.app.glfw;
import deren.app.pipelines;
import deren.app.renderer;
import deren.app.scripting;
import deren.app.toon;

import deren.engine.backend_loader;
import deren.engine.lighting.radiance_capture;
import deren.engine.lighting.reflection_probe;
import deren.engine.math;
import deren.engine.primitive;
import deren.engine.render_layout;

import deren.engine.gaussian_splatting;
import deren.engine.gaussian_splatting.raster;
import deren.engine.animation;
import deren.engine.runtime;
import deren.engine.scene_tree;
import deren.gltf_loader;
import deren.promise.gui;
import deren.promise.rhi;
import deren.scripting.managed_host;

namespace deren::app {
    namespace {
        using pipeline_material_bases = std::unordered_map<std::uint64_t, deren::engine::material_id>;

        /// Rewrites every imported leaf under @p node to @p pipeline. The walk never consults
        /// `visible`, because a hidden asset subtree must still switch so it renders correctly when
        /// shown again. `unlit` replaces the leaf's material with a cloned unlit variant (recording
        /// the clone source in @p bases so leaving unlit restores the original); `game_toon` and
        /// `pbr` only choose the shading path. Unknown names return false and touch nothing.
        bool apply_pipeline_to_subtree(deren::engine::runtime& runtime, pipeline_material_bases& bases,
                                       deren::engine::scene_tree::scene_node& node, std::string_view const pipeline) {
            if (node.primitive_leaf != nullptr) {
                // Imported leaves always come from runtime::create_primitive(), i.e. they are
                // deren::engine::primitive; RTTI is off, so this is the documented downcast.
                auto& leaf = *static_cast<deren::engine::primitive*>(node.primitive_leaf.get());
                if (pipeline == "pbr") {
                    if (auto const found = bases.find(leaf.lifetime_id); found != bases.end()) {
                        leaf.push.material_index = found->second;
                    }
                    leaf.pipeline_name = {};
                    leaf.path = deren::engine::shading_path::deferred;
                } else if (pipeline == "unlit") {
                    auto const inserted = bases.try_emplace(leaf.lifetime_id, leaf.push.material_index);
                    leaf.pipeline_name = {};
                    leaf.path = deren::engine::shading_path::deferred;
                    leaf.push.material_index =
                        runtime.clone_material(inserted.first->second, glm::vec3(1.0f), 1.0f, 1.0f, 1.0f, true);
                } else if (pipeline == "game_toon") {
                    // Asset selection must enable the pass as well as mark its leaves. An editor
                    // may start empty with both startup switches off and import a character later.
                    runtime.set_character_forward(true);
                    runtime.set_game_toon(true);
                    if (auto const found = bases.find(leaf.lifetime_id); found != bases.end()) {
                        leaf.push.material_index = found->second;
                    }
                    leaf.pipeline_name = {};
                    leaf.path = deren::engine::shading_path::character;
                } else {
                    return false;
                }
            }
            for (auto& child : node.children) {
                if (!apply_pipeline_to_subtree(runtime, bases, child, pipeline)) {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    int run(int argc, char** argv) {
        static_cast<void>(deren::utility::diagnostics::install_crash_handler());
        // CPU-only authoring/verification commands run without a window or Vulkan.
        if (argc == 4 && std::string_view(argv[1]) == "--bake-environment") {
            std::string error;
            if (!bake_environment_ibl(argv[2], argv[3], environment_cubemap_size, environment_mip_count,
                                      environment_irradiance_size, environment_brdf_lut_size, 1.0f, error)) {
                deren::utility::println(stderr, "environment bake failed: {}", error);
                return 2;
            }
            deren::utility::println("baked environment written to '{}'", argv[3]);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--skeleton-hash") {
            auto model = deren::gltf::load_model(argv[2]);
            if (!model) {
                deren::utility::println("cannot load skeleton model");
                return 2;
            }
            auto hash = deren::gltf::skeleton_fingerprint(*model);
            if (!hash) {
                deren::utility::println("{}", hash.error());
                return 2;
            }
            deren::utility::println("skeleton_hash={}", *hash);
            return 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--verify-motion") {
            auto model = deren::gltf::load_model(argv[2]);
            if (!model) {
                deren::utility::println("cannot load skeleton model");
                return 2;
            }
            auto result = deren::gltf::load_motion(*model, argv[3]);
            if (!result) {
                deren::utility::println("motion verification failed: {}", result.error());
                return 2;
            }
            deren::utility::println("motion verified: {} clip(s)", *result);
            return 0;
        }
        // --version: print the version (single source: project(VERSION) in CMakeLists.txt, injected
        // as DEREN_VERSION_*) and exit before any config / Vulkan init.
        for (std::int32_t i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--version") {
                deren::utility::println("deren {}.{}.{}", DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);
                return 0;
            }
        }

        // A project launch owns every startup option: `deren run <project>` is the ONLY entry. The
        // engine locates the subcommand and passes the untouched argv to the project's preinit; the
        // native preview mode and its capture CLI are gone, so a launch without a managed project is
        // a usage error rather than a second startup path.
        auto const fail_project = [](std::string_view message) {
            deren::utility::println(stderr, "{}", message);
            deren::utility::error(message);
            deren::utility::wait_log_all();
            return 2;
        };
        std::int32_t run_index = -1;
        for (std::int32_t i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "run") {
                run_index = i;
                break;
            }
        }
        if (run_index < 0) {
            return fail_project("usage: deren run <project-directory>");
        }
        if (run_index + 1 >= argc) {
            return fail_project("usage: deren run <project-directory>");
        }
        auto project = deren::app_config::load_project_settings(argv[run_index + 1]);
        if (!project) {
            return fail_project(project.error());
        }
        deren::utility::log("project: launching '{}' ({})", project->name, project->settings.config_file);
        std::filesystem::path const script_assembly = std::move(project->script_assembly);
        if (script_assembly.empty()) {
            return fail_project("the project names no managed assembly; every launch needs one ([scripts] assembly)");
        }
        deren::utility::vessel::vector<deren::input::action_binding> project_actions = std::move(project->input_actions);
        deren::app_config::app_settings launch_settings = std::move(project->settings);
        if (launch_settings.gui.rml_document.empty() != launch_settings.gui.rml_font.empty()) {
            return fail_project("gui.rml_document and gui.rml_font must be configured together");
        }
        deren::app::script_scene script_scene;
        capture::options capture{};
        auto startup = script_scene.preinit(script_assembly, argc, argv);
        if (!startup) {
            deren::utility::error("{}", startup.error());
            deren::utility::wait_log_all();
            return 2;
        }
        capture.frames = startup->capture_frames;
        capture.windowed = startup->capture_windowed != 0;
        if (startup->camera_set != 0) {
            capture.camera = deren::utility::vessel::array<float, 3>{
                startup->camera_yaw_degrees, startup->camera_pitch_degrees, startup->camera_distance};
            if (startup->target_set != 0) {
                capture.target = glm::vec3{startup->target.x, startup->target.y, startup->target.z};
            }
        }
        capture.sweep_yaw_deg_per_frame = startup->sweep_yaw_deg_per_frame;
        capture.animation_seconds_per_frame = startup->animation_seconds_per_frame;
        deren::app::startup_config config = deren::app::analyse_config(launch_settings);
        auto& settings = config.settings;
        std::filesystem::path const& shaders_dir = config.shaders_dir;
        // Keep the splash younger than its GLFW window, including early-exit teardown.
        bool const offscreen = settings.render.offscreen || (capture.frames > 0 && !capture.windowed);
        glfw_window window{settings.render.window_width, settings.render.window_height,
                           settings.render.window_title.c_str(), capture.frames == 0, offscreen,
                           settings.render.dark_window_frame, settings.render.borderless_window};
        std::span<deren::input::action_binding const> const actions{project_actions.data(), project_actions.size()};
        deren::input::action_state action_state{actions};
        deren::app::startup_splash splash{capture.frames == 0 && !settings.render.offscreen};
        splash.attach_window(window.native_handle());

        // startup banner: version (single source: project(VERSION) in CMakeLists.txt)
        deren::utility::log("deren {}.{}.{}", DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);

        // 4. The IBL resolutions are compiled in (environment_loader.cppm): nothing re-shapes them.
        //    THE ANALYTIC SKY IS NOT BAKED HERE ANY MORE. It used to be generated before the window
        //    existed, because the renderer wanted an environment before its first frame - but the
        //    managed project decides what slot 0 holds, and the demo replaced that atmosphere with its
        //    own HDR on the very first update, so the startup bake was pure launch latency (1.1 s
        //    Release, 16 s Debug, both convolutions on the CPU). The sky is now a FALLBACK baked in the
        //    frame loop only when the project has not named an environment by the frame about to be
        //    recorded (see `bake_fallback_sky`), and every bake - the sky's and every decoded HDR's -
        //    goes through the on-disk cache, so a second launch reads one file instead.
        constexpr std::int32_t env_size = environment_cubemap_size;
        constexpr std::int32_t env_mip_count = environment_mip_count;
        constexpr std::int32_t irr_size = environment_irradiance_size;
        constexpr std::int32_t lut_size = environment_brdf_lut_size;
        auto const startup_start = std::chrono::steady_clock::now();

        // THE SUN, AS ONE VECTOR FOR EVERYTHING THAT HAS TO AGREE ABOUT IT: the light UBO built from it
        // (the shading's `light_dir` and the shadow cascades) and the disc the sky draws - which reads
        // that UBO's `light_dir`, so the sky cannot put its sun where the shadows do not fall.
        auto const& sun_direction = settings.lighting.sun_direction;

        // 5. THE APPLICATION ACQUIRES THE DEVICE ROOT, THEN BUILDS THE RENDERER FROM IT. The loader is
        //    the app's decision (which backend, from where, with which search rules, and what a
        //    failure means for startup) and the runtime is the renderer, which loads nothing and
        //    refuses an empty root loudly. THE TYPE IS THE RHI CONTRACT'S, and it is the ONLY creation
        //    structure there is: the program fills `deren::promise::rhi::create_info`, hands it to
        //    `deren::engine::load_api_core()` and then hands BOTH the root and the same structure to
        //    the runtime.
        deren::promise::rhi::create_info core_options = {};
        core_options.window_width = settings.render.window_width;
        core_options.window_height = settings.render.window_height;
        // the title is a REFERENCED `char const*`: it has to outlive the core's construction, and
        // `settings` lives to the end of main, so this `.c_str()` is valid for the whole run.
        core_options.window_title = settings.render.window_title.c_str();
        core_options.vsync = settings.render.vsync;
        core_options.validation_layers = settings.render.validation_layers;
        // the render scale is a CREATION option and not a runtime setter, because it decides the extent
        // every render target is created with (see create_info::render_scale).
        core_options.render_scale = settings.render.render_scale;
        // A scripted capture gets no visible window: the screenshot is a read-back of the swapchain
        // image, not a capture of the window.
        core_options.window_visible = capture.frames == 0;
        // the pointer the application created above (and keeps valid for the whole run) selects the
        // core's caller-window branch: it binds to that window and destroys none.
        core_options.native_window = window.native_handle();
        deren::promise::rhi::shared_object<deren::promise::rhi::api_core> core = deren::engine::load_api_core(core_options);
        if (core == nullptr) {
            deren::utility::error("startup: no device root was acquired, so there is no renderer to run "
                                  "(see the backend_loader diagnosis above)");
            return 1;
        }
        deren::engine::runtime runtime{std::move(core), core_options};
        runtime.background_color = glm::vec3(settings.render.clear_color[0], settings.render.clear_color[1], settings.render.clear_color[2]);
        runtime.set_gpu_timings(settings.render.gpu_timings);
        runtime.set_async_present(settings.render.async_present);
        // [render] max_fps: 0 keeps the loop uncapped (what a throughput measurement needs). The key has
        // been parsed all along; without this call the frame pacing ignored it.
        runtime.set_max_fps(settings.render.max_fps);
        // Shadow cascades: applied BEFORE the first scene set binds the layered image.
        runtime.set_shadow_cascades(static_cast<std::uint32_t>(settings.render.shadow_cascades));
        runtime.set_shadow_cascade_blend(settings.render.shadow_cascade_blend);
        runtime.set_shadow_depth_bias(settings.render.shadow_bias_constant, settings.render.shadow_bias_slope, 0.0f);
        runtime.set_shadow_map_size(static_cast<std::uint32_t>(settings.render.shadow_map_size));
        // [render] shadow: the raster shadow term itself. Applied before the first scene set, so the
        // first frame already renders (or skips) the cascaded shadow maps.
        runtime.set_shadow_enabled(settings.render.shadow);
        // THE ACTION SEAM IS INSTALLED AFTER THE RUNTIME, ON PURPOSE: the runtime's constructor
        // registers the orbit camera's cursor/button/scroll callbacks, and installing this seam
        // first let the runtime overwrite it - every mouse event then reached the camera and none
        // reached the UI (a hover never happened, so no click ever fired). Constructing it here
        // makes it capture the camera's handlers as the previous ones and forward every event back
        // (see glfw_action_input's constructor), so both readers see the mouse.
        glfw_action_input action_input{&window, action_state};
        auto const runtime_ready = std::chrono::steady_clock::now();
        deren::utility::log("vulkan runtime initialized: {:.1f} ms (no environment baked yet)", std::chrono::duration<double, std::milli>(runtime_ready - startup_start).count());

        // 6. Pipelines up front (deren::app::setup_pipeline): the standard PBR pipeline and the
        //    directional shadow pass.
        deren::app::setup_pipeline(runtime, shaders_dir);
        // THIS APPLICATION'S WIRING: its passes are fed by `deren.app.renderer`, which finds them in
        // the chain the runtime keeps and answers the frame's per-stage questions.
        deren::app::app_renderer app_renderer;
        static_cast<void>(app_renderer.attach(runtime)); // builds this app's chain and hands it over
        // ... and the CREATE step runs over that chain (the shaders above are registered by now).
        runtime.create_passes();
        runtime.set_furnace(settings.render.furnace);
        runtime.set_rt_shadows(settings.render.rt_shadows);
        runtime.set_rt_reflections(settings.render.rt_reflections);
        runtime.set_rt_reflection_whitelist(settings.render.rt_reflection_cloth,
                                            settings.render.rt_reflection_strength,
                                            settings.render.rt_reflection_materials);
        runtime.set_rt_reflection_samples(settings.render.rt_reflection_samples);
        runtime.set_rt_mask_bake(settings.render.rt_mask_bake);
        runtime.set_rt_skin_bake(settings.render.rt_skin_bake);
        runtime.set_visibility_buffer(settings.render.visibility_buffer);
        runtime.set_visibility_texture_streaming(settings.render.visibility_texture_streaming);
        runtime.set_subtilis(settings.render.subtilis);
        runtime.set_subtilis_error_px(settings.render.subtilis_error_px);
        runtime.set_subtilis_hysteresis(settings.render.subtilis_hysteresis);
        runtime.set_subtilis_gpu_cut(settings.render.subtilis_gpu_cut);
        runtime.set_subtilis_page_upload_bytes(settings.render.subtilis_page_upload_bytes);
        runtime.set_subtilis_resident_page_bytes(settings.render.subtilis_resident_page_bytes);
        runtime.set_software_raster(settings.render.software_raster);
        runtime.set_software_raster_threshold_px(settings.render.software_raster_threshold_px);

        // 7. THE BASE IBL IS NOT UPLOADED HERE. Slot 0 is whatever the project names on its first
        //    frame; when it names nothing, the analytic sky is baked as a fallback just before that
        //    frame is recorded (`bake_fallback_sky` below). What the renderer holds until then is the
        //    furnace/1x1 cube it already creates, which is what a frame without an environment is.
        // THE LOOK OF THE FRAME IS THE MANAGED PROJECT'S: the environment, sun, exposure, tone
        // mapping, bloom, FXAA, TAA, SSAO, the G-buffer debug view and clustered light culling are
        // parsed by the managed framework from the same project files and applied before the
        // project's initialize (see deren::project_config), so the C# game owns them and can move
        // them any frame. What stays native is what must exist before the project's scripts run:
        // the swapchain and its creation parameters, the shadow cascade/map sizing, the GPU-driven
        // paths, the render scale, and the pipeline / toon stage selection below.
        runtime.set_default_pipeline(settings.render.unlit ? "unlit" : "pbr");
        app_renderer.set_unlit(settings.render.unlit);
        app_renderer.set_gbuffer_channel(settings.render.gbuffer_channel);
        runtime.set_character_forward(settings.render.character_forward);
        runtime.set_game_toon(settings.render.game_toon);

        // THE TOON LIGHT RIG ([toon] profile -> deren::engine::toon_rig), published ONCE before the
        // first frame: nothing rewrites that block while a frame is in flight (see
        // core::heap_slots::toon_rig), which is the contract that makes a write from here safe - and
        // the reason it cannot move into the frame loop. Without this block every lane keeps the
        // struct's construction value, and the character stage's day/head/rim/environment shading
        // (plus the shadow softness ladder and the face/hair shaping switches) is flat.
        // What the managed get_toon_rig answers before its first set: the rig the config published.
        deren::scripting::toon_rig_desc startup_toon_rig{};
        static_assert(sizeof(deren::engine::toon_rig) == sizeof(deren::scripting::toon_rig_desc));
        {
            deren::engine::toon_rig rig = {};
            rig.day.x = settings.toon.day_strength;
            rig.other_light.x = settings.toon.head_light_day0;
            rig.other_light.y = settings.toon.head_light_day1;
            rig.other_colour = glm::vec4(settings.toon.head_light_colour[0], settings.toon.head_light_colour[1],
                                         settings.toon.head_light_colour[2], 0.0f);
            rig.reference_modes.x = settings.toon.hair_masked_highlights ? 1.0f : 0.0f;
            rig.reference_modes.y = settings.toon.face_angular_sdf ? 1.0f : 0.0f;
            rig.main_dark.x = settings.toon.sun_dark_colour[0];
            rig.main_dark.y = settings.toon.sun_dark_colour[1];
            rig.main_dark.z = settings.toon.sun_dark_colour[2];
            rig.env.x = settings.toon.env_strength;
            rig.env.y = settings.toon.env_rotation;
            rig.env.z = settings.toon.specular_strength;
            rig.env.w = settings.toon.diffuse_blend_effect;
            rig.rim.x = settings.toon.rim_area;
            rig.rim.y = settings.toon.rim_strength;
            rig.rim.z = settings.toon.rim_nolxz_strength;
            rig.misc.x = settings.toon.backlight_strength;
            // the face's expression index rides the head light's colour block's fourth lane
            rig.other_colour.w = settings.toon.emotion_type;
            // the shadow softness ladder ([render] toon_shadow_softness) plus the three shape lanes
            rig.shadow_softness.x = settings.render.toon_shadow_softness;
            rig.shadow_softness.y = settings.toon.nose_shadow_reduction;
            rig.shadow_softness.z = settings.toon.hair_highlight_width;
            rig.shadow_softness.w = settings.toon.hair_highlight_shift;
            runtime.set_toon_rig(rig);
            std::memcpy(&startup_toon_rig, &rig, sizeof(rig));
            deren::utility::log("toon: light rig published - day strength {:.3f}, head light {:.2f} at day 1 of {:.2f} at day 0, env strength {:.2f}, shadow softness {}",
                                rig.day.x, rig.other_light.y, rig.other_light.x, rig.env.x,
                                static_cast<int>(rig.shadow_softness.x));
        }

        // 8. The local reflection probe, when the config names one (a scene-authored calibration; the
        //    probe is uploaded once, before the first frame, and the global IBL is used when it fails).
        if (settings.reflection_probe.enabled) {
            auto const& r = settings.reflection_probe;
            auto vec = [](auto const& a) { return glm::vec3(a[0], a[1], a[2]); };
            auto path = std::filesystem::path(r.asset.c_str());
            if (path.is_relative()) {
                path = deren::utility::executable_directory() / path;
            }
            std::error_code ec;
            auto const length = std::filesystem::file_size(path, ec);
            std::optional<deren::engine::probe_detail::asset> asset;
            if (!ec && length <= 32 + std::uint64_t(6) * 512 * 512 * 8 * 2) {
                std::vector<std::byte> bytes(length);
                std::ifstream file(path, std::ios::binary);
                if (file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
                    asset = deren::engine::probe_detail::decode(bytes);
                }
            }
            if (!asset || !runtime.set_local_probe(asset->half, asset->size, asset->mips, vec(r.position), vec(r.minimum), vec(r.maximum), r.fade, r.gain)) {
                deren::utility::log("local probe: invalid asset/config/upload; using global IBL ({})", path.string());
            } else {
                deren::utility::log("local probe loaded: {} ({}px, {} mips), position ({},{},{})", path.string(), asset->size, asset->mips, r.position[0], r.position[1], r.position[2]);
            }
        }
        auto const startup_done = std::chrono::steady_clock::now();
        deren::utility::log("startup window (runtime + pipelines + probe, no IBL): {:.1f} ms", std::chrono::duration<double, std::milli>(startup_done - startup_start).count());

        // 9. THE MANAGED PROJECT: the C# game owns the scene. The engine hands it three native services
        //    (import a glTF asset into a scene node, read/write the camera, request a screenshot) and
        //    receives nothing but the scoped handlers below.
        struct managed_cloud_binding {
            std::shared_ptr<deren::engine::gaussian_splatting::raster_asset const> asset;
            std::uint64_t stable_id;
        };
        struct managed_bind_context {
            bool starting = true;
            deren::engine::runtime* render_runtime = nullptr;
            // TAA/SSAO live on this application's renderer, not on the runtime.
            deren::app::app_renderer* renderer = nullptr;
            // A managed set_environment_hdr() request is validated here and applied between frames
            // (the upload waits for the GPU, which must never happen inside the callback). The slot
            // travels with the path: 0 is the scene-wide environment, 1..3 the per-material lanes.
            std::filesystem::path pending_environment;
            std::uint32_t pending_environment_slot = 0;
            bool environment_pending = false;
            // A managed set_toon_rig() request is validated in app_scripting (finite lanes, clamped
            // expression index and shadow ladder) and queued here: the rig's block may not be written
            // while a frame is in flight, so the frame thread publishes it between frames. `toon_rig`
            // is therefore "what the next frame uses" - the pending value while one is queued,
            // otherwise the rig the last frame rendered with, which is what get_toon_rig answers.
            deren::scripting::toon_rig_desc toon_rig{};
            bool toon_rig_pending = false;
            // A managed set_scene_aspect() request. The value is a plain float the frame-start camera
            // setup reads, so there is nothing to upload - it is queued anyway so it lands at the same
            // point as every other write a managed frame makes, and the newest request wins. 0 is the
            // explicit "back to the window's aspect" value.
            float scene_aspect = 0.0F;
            bool scene_aspect_pending = false;
            // Clone sources for leaves currently shaded by the unlit pipeline, keyed by the
            // leaf's lifetime id (pointer identity alone would reuse a freed leaf's entry).
            pipeline_material_bases pipeline_bases;
            // The radius of the largest scene bound so far: `enable_shadows` remembers the extent
            // the camera's far plane needs, so it is re-run only when a later bind is bigger.
            float shadow_scene_radius = 0.0f;
            // THE UPLOADED SPLAT CLOUDS, keyed by library id. A cloud is a device asset (its static
            // tables are built once), so a second placement of the same asset reuses it instead of
            // re-reading and re-uploading the `.ply`. `next_cloud_id` is the per-instance stable id the
            // splat sort needs as its deterministic tie-break.
            deren::utility::vessel::vector<std::pair<std::string, std::shared_ptr<deren::engine::gaussian_splatting::raster_asset const>>> clouds;
            std::uint64_t next_cloud_id = 1;
            // THE MANAGED OBJECT OVERRIDES, keyed by the leaf's lifetime id (pointer identity alone
            // would reuse a freed leaf's entry). `base` is the row the leaf pointed at when the
            // override first appeared, `clone` is the engine row that carries the factors, and the
            // factors themselves are rebuilt from `base` on every call so they never compound.
            struct material_override_row {
                deren::engine::material_id base{};
                deren::engine::material_id clone{};
                bool base_recorded = false;
            };
            std::unordered_map<std::uint64_t, material_override_row> material_overrides;
            struct debug_window {
                deren::gui::panel* panel = nullptr;
                bool submitted = false;
            };
            deren::utility::vessel::unordered_map<std::string, debug_window> ui_windows;
            // Widget calls apply to the current begin/end pair.
            deren::gui::panel* ui_panel = nullptr;
            bool ui_panel_open = false;
            bool ui_available = false;
            // The size a panel falls back to when the game names none (`[gui] panel_width/panel_height`).
            float ui_default_width = 380.0F;
            float ui_default_height = 140.0F;
            // ---- ABI 35: THE CHARACTER ANIMATION PLAYER ----
            //
            // ONE player for the scene, built when the first asset whose manifest names a motion file
            // is bound. The host owns it because it needs the runtime's per-frame skin/morph slots and
            // the scene pointer; managed code only names the clip and the clock. `animation_commands`
            // is written from the managed frame thread and drained by the frame thread right before it
            // records, which is why it is the one field here behind a lock.
            struct animation_command {
                deren::app::animation_command_kind kind = deren::app::animation_command_kind::select;
                std::uint32_t index = 0;
                float value = 0.0F;
            };
            std::unique_ptr<deren::engine::animation::controller> player;
            deren::scripting::animation_state animation_state{};
            std::vector<std::string> animation_names;
            std::mutex animation_mutex;
            std::vector<animation_command> animation_pending;
            // ABI 43: the window the managed request controls act on; null for an offscreen capture,
            // which must answer module_unavailable instead of pretending to move a window.
            deren::app::glfw_window* window = nullptr;
        };
        managed_bind_context managed_binding{.starting = true,
                                             .render_runtime = &runtime,
                                             .renderer = &app_renderer,
                                             .pending_environment = {},
                                             .pending_environment_slot = 0,
                                             .environment_pending = false,
                                             .toon_rig = startup_toon_rig,
                                             .toon_rig_pending = false,
                                             .scene_aspect = 0.0F,
                                             .scene_aspect_pending = false,
                                             .pipeline_bases = {},
                                             .shadow_scene_radius = 0.0f,
                                             .clouds = {},
                                             .next_cloud_id = 1,
                                             .material_overrides = {},
                                             .ui_windows = {},
                                             .ui_panel = nullptr,
                                             .ui_panel_open = false,
                                             .ui_available = false,
                                             .ui_default_width = 380.0F,
                                             .ui_default_height = 140.0F,
                                             .player = nullptr,
                                             .animation_state = {},
                                             .animation_names = {},
                                             .animation_mutex = {},
                                             .animation_pending = {},
                                             .window = &window};
        // The snapshot every ABI 35 query reads. The frame thread writes it once per frame; a managed
        // thread reads it under the same lock, so a query never sees a half-built state.
        constexpr auto publish_animation_state = +[](managed_bind_context& context) {
            deren::scripting::animation_state state{};
            if (context.player != nullptr) {
                auto const& player = *context.player;
                state.playable_count = static_cast<std::uint32_t>(player.playable_count());
                state.current = static_cast<std::uint32_t>(player.current());
                state.has_active = player.has_active() ? 1 : 0;
                state.playing = player.is_playing() ? 1 : 0;
                state.available = 1;
                state.time = player.current_time();
                state.duration = player.loop_duration();
                state.speed = player.speed();
            }
            std::lock_guard const lock(context.animation_mutex);
            context.animation_state = state;
        };
        auto const bind_managed_asset = +[](void* opaque, deren::engine::scene_tree::scene_node& parent,
                                            std::string_view asset_id, deren::app::managed_asset_source const& source) -> bool {
            auto& context = *static_cast<managed_bind_context*>(opaque);
            auto& runtime = *context.render_runtime;
            // Live imports may grow GPU tables used by older frames. This wait is per import, not per frame.
            if (!context.starting) {
                runtime.rhi_face().wait_idle();
                // Controllers cache scene-node addresses, which a structural edit may invalidate.
                context.player.reset();
                context.animation_names.clear();
                context.animation_pending.clear();
                publish_animation_state(context);
            }
            // A CLOUD IS STAGED, NOT IMPORTED: it has no scene graph, no materials and no toon lookup, so
            // the whole import path below does not apply. The upload happens once per library id (the
            // static tables are expensive), and every placement of that asset becomes one instance in the
            // shared splat pass, placed by the wrapper node's own transform.
            if (source.cloud != nullptr) {
                std::shared_ptr<deren::engine::gaussian_splatting::raster_asset const> uploaded;
                for (auto const& [id, cloud] : context.clouds) {
                    if (id == asset_id) {
                        uploaded = cloud;
                        break;
                    }
                }
                if (uploaded == nullptr) {
                    auto created = runtime.create_gaussian_asset(*source.cloud);
                    if (!created) {
                        deren::utility::error("gaussian: the host refused to upload '{}' (error {})", asset_id,
                                              static_cast<std::int32_t>(created.error().code));
                        return false;
                    }
                    uploaded = *created;
                    context.clouds.emplace_back(std::string{asset_id}, uploaded);
                    deren::utility::log("gaussian: '{}' uploaded - {} splats, SH degree {}", asset_id,
                                        uploaded->source().particles.size(), uploaded->source().sh_degree);
                }
                // Tie the placement to the ECS node so undo/delete cannot leave a ghost cloud.
                if (!deren::engine::scene_tree::components().try_emplace<managed_cloud_binding>(
                        parent.node_id, managed_cloud_binding{uploaded, context.next_cloud_id++})) {
                    return false;
                }
                runtime.scene_changed();
                return true;
            }
            if (source.scenes == nullptr || source.materials.empty()) {
                return false;
            }
            deren::gltf::scene_node_iterator const node_first = source.scenes->nodes_begin();
            deren::gltf::scene_node_iterator const node_last;
            deren::gltf::drawable_iterator const drawable_first(*source.scenes, source.materials);
            deren::gltf::drawable_iterator const drawable_last;
            glm::vec3 const import_shift{source.import_shift[0], source.import_shift[1], source.import_shift[2]};
            // THE TOON MATERIAL LOOKUP, installed for the duration of this import. The runtime asks it,
            // by material name, for the maps and colours a `.toon.tsv` declares - the application layer
            // is the only layer that has read that file. A model without a sidecar gets the empty lookup,
            // which the import reads as "no lane is switched on" rather than as a block of white.
            if (source.toon != nullptr) {
                runtime.set_toon_lookup(deren::engine::runtime::toon_lookup{
                    .parent = const_cast<deren::app::detail::toon_lookup_state*>(source.toon),
                    .texture = deren::app::detail::toon_texture_callback,
                    .colour = deren::app::detail::toon_colour_callback,
                    .scalar = deren::app::detail::toon_scalar_callback});
            } else {
                runtime.set_toon_lookup(deren::engine::runtime::toon_lookup{});
            }
            deren::engine::scene_import_result const result =
                runtime.import_scene(node_first, node_last, drawable_first, drawable_last, import_shift,
                                     false, 0u, &parent);
            if (result.primitive_count == 0) {
                return false;
            }
            // ---- ABI 35: THE ANIMATION PLAYER IS BUILT FROM THE FIRST ASSET THAT BRINGS CLIPS ----
            //
            // One player drives the whole scene: the runtime has ONE skin-matrix region for the
            // animation controller (blocks 4.. after the shared identity block), so a second animated
            // asset is reported and stays static instead of silently stealing the skeleton writes.
            // init() is legal here because a bind runs inside the managed update, before the frame is
            // paced: the per-frame skin slots it writes are not being recorded yet.
            if (context.player == nullptr && !source.scenes->animations.empty()) {
                auto player = std::make_unique<deren::engine::animation::controller>();
                deren::engine::animation::backend const backend{
                    .scene = &runtime.get_scene(),
                    .morph_scratch_active = [&runtime]() { return static_cast<float*>(runtime.morph_scratch()); },
                    .set_skin_matrices_active = [&runtime](std::span<glm::mat4 const> matrices, std::uint32_t base) { runtime.set_skin_matrices_at(matrices, base); },
                    .morph_scratch_slot = [&runtime](std::uint32_t slot) { return static_cast<float*>(runtime.morph_scratch(slot)); },
                    .set_skin_matrices_slot = [&runtime](std::span<glm::mat4 const> matrices, std::uint32_t slot) { runtime.set_skin_matrices(matrices, slot); },
                    .scene_changed = [&runtime]() { runtime.scene_changed(); },
                    .run_tasks = [&runtime](std::span<std::function<void()>> tasks) { runtime.run_tasks(tasks); },
                    .task_worker_count = [&runtime]() { return runtime.task_pool_threads(); },
                };
                player->init(*source.scenes, backend, import_shift, result.import_instance);
                context.animation_names.clear();
                for (std::size_t index = 0; index < player->playable_count(); ++index) {
                    context.animation_names.emplace_back(player->playable_name(index));
                }
                std::string names;
                for (auto const& name : context.animation_names) {
                    names += names.empty() ? "'" : ", '";
                    names += name;
                    names += '\'';
                }
                deren::utility::log("animation: '{}' drives the scene - {} clip(s) [{}]", asset_id,
                                    player->playable_count(), names);
                context.player = std::move(player);
                publish_animation_state(context);
            } else if (context.player != nullptr && !source.scenes->animations.empty()) {
                deren::utility::log("animation: '{}' has {} clip(s), but one player already drives this scene - "
                                    "the asset stays static",
                                    asset_id, source.scenes->animations.size());
            }
            // THE SHADOW FIT: the frame's directional shadow maps frame the sphere the imported
            // primitives occupy, which is what makes the sun cast a shadow at all. `enable_shadows`
            // also remembers the extent the camera's far plane needs, so it is re-run only when a
            // later bind grows the bound (`[render] shadow_cascades`/bias/size are already applied
            // above, before the first scene set binds the layered image).
            if (source.scene_radius > context.shadow_scene_radius) {
                context.shadow_scene_radius = source.scene_radius;
                glm::vec3 const shadow_center{source.scene_center[0], source.scene_center[1], source.scene_center[2]};
                runtime.enable_shadows(shadow_center, source.scene_radius);
            }
            // The manifest's default pipeline takes effect at bind, so the asset looks as authored
            // from the first frame instead of after the script's first update.
            if (!source.default_pipeline.empty() && source.default_pipeline != "pbr" &&
                !apply_pipeline_to_subtree(runtime, context.pipeline_bases, parent, source.default_pipeline)) {
                return false;
            }
            runtime.scene_changed();
            return true;
        };
        auto const apply_managed_pipeline = +[](void* opaque, deren::engine::scene_tree::scene_node& node,
                                                std::string_view pipeline) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr ||
                !apply_pipeline_to_subtree(*context->render_runtime, context->pipeline_bases, node, pipeline)) {
                return false;
            }
            context->render_runtime->scene_changed();
            return true;
        };
        auto const install_managed_scene = +[](void* opaque, deren::engine::scene_tree::scene& managed) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            // A NEW SCENE STARTS WITH NO CLOUDS: the staged set belongs to the tree that staged it, so a
            // replacement tree must not inherit the previous one's splats. The cached uploads stay valid
            // (their device tables outlive the staging) and are reused when the new tree places them again.
            context->render_runtime->clear_gaussian_assets();
            // A new tree also ends the animation player: it holds a raw pointer to the tree it animated
            // and the node indices the clips address, and set_scene() only stores that pointer, so
            // keeping it would write skin matrices into a tree that no longer exists.
            if (context->player != nullptr) {
                deren::utility::log("animation: the scene tree was replaced - dropping the player");
                context->player.reset();
                context->animation_names.clear();
                context->animation_pending.clear();
                publish_animation_state(*context);
            }
            context->render_runtime->set_scene(managed);
            context->render_runtime->scene_changed();
            return true;
        };
        // ---- world <-> screen queries and diagnostics (ABI 38) ---------------------------------------
        // All five read `frame_camera()` (the matrix the frame renders with, capture override included) and
        // the RENDER extent, so the answer is in the same pixels the passes shade - which differ from the
        // window's whenever the render scale is below 1.
        auto const query_viewport_size = +[](void* opaque, std::uint32_t& width, std::uint32_t& height) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            auto const extent = context->render_runtime->query_frame_diagnostics().render;
            width = extent.width;
            height = extent.height;
            return width != 0 && height != 0;
        };
        auto const query_project_point = +[](void* opaque, float const world[3], float screen[3]) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr || world == nullptr || screen == nullptr) {
                return false;
            }
            auto const& runtime = *context->render_runtime;
            auto const extent = runtime.query_frame_diagnostics().render;
            auto const camera = runtime.frame_camera();
            glm::vec4 const point{world[0], world[1], world[2], 1.0f};
            glm::vec4 const clip = camera.view_proj_unjittered * point;
            glm::vec4 const eye = camera.view * point;
            if (std::abs(clip.w) < 1.0e-6F) {
                return false;
            }
            // The projection already carries the y flip (see make_orbit_camera_ubo), so NDC -> pixel is the
            // plain scale with the origin at the TOP-left, the same convention input events arrive in.
            float const inverse_w = 1.0f / clip.w;
            screen[0] = (clip.x * inverse_w * 0.5f + 0.5f) * static_cast<float>(extent.width);
            screen[1] = (clip.y * inverse_w * 0.5f + 0.5f) * static_cast<float>(extent.height);
            screen[2] = -eye.z; // view-space depth; negative means the point is BEHIND the camera
            return true;
        };
        auto const query_screen_ray = +[](void* opaque, float x, float y, float origin[3], float direction[3]) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr || origin == nullptr || direction == nullptr) {
                return false;
            }
            auto const& runtime = *context->render_runtime;
            auto const extent = runtime.query_frame_diagnostics().render;
            if (extent.width == 0 || extent.height == 0) {
                return false;
            }
            auto const camera = runtime.frame_camera();
            float const ndc_x = (x / static_cast<float>(extent.width)) * 2.0f - 1.0f;
            float const ndc_y = (y / static_cast<float>(extent.height)) * 2.0f - 1.0f;
            // Vulkan's 0..1 depth: z = 0 is the near plane, so the unprojected near point minus the eye IS the
            // direction. Solving it through the inverse matrix keeps the y flip and the jitter-free matrix in
            // one place instead of re-deriving the frustum here.
            glm::vec4 const near_point = glm::inverse(camera.view_proj_unjittered) * glm::vec4{ndc_x, ndc_y, 0.0f, 1.0f};
            if (std::abs(near_point.w) < 1.0e-6F) {
                return false;
            }
            glm::vec3 const eye = glm::vec3{camera.camera_pos};
            glm::vec3 const ray = glm::vec3{near_point} / near_point.w - eye;
            if (glm::dot(ray, ray) < 1.0e-12F) {
                return false;
            }
            glm::vec3 const unit = glm::normalize(ray);
            origin[0] = eye.x;
            origin[1] = eye.y;
            origin[2] = eye.z;
            direction[0] = unit.x;
            direction[1] = unit.y;
            direction[2] = unit.z;
            return true;
        };
        auto const query_counters = +[](void* opaque, deren::scripting::diagnostic_counters& counters) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            auto const& runtime = *context->render_runtime;
            auto const diagnostics = runtime.query_frame_diagnostics();
            counters = {};
            counters.render_width = diagnostics.render.width;
            counters.render_height = diagnostics.render.height;
            counters.output_width = diagnostics.output.width;
            counters.output_height = diagnostics.output.height;
            counters.gpu_interval_count = diagnostics.gpu_intervals;
            counters.gpu_timings_available = diagnostics.gpu_timings ? std::uint8_t{1} : std::uint8_t{0};
            return true;
        };
        auto const query_gpu_timing_summary = +[](void* opaque, std::string_view& text) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            // Borrowed into a thread-local buffer: the ABI hands the module a view, so the bytes it points
            // at must outlive the call. Valid until the next query on this thread.
            thread_local deren::utility::vessel::string summary;
            summary = context->render_runtime->gpu_timing_summary();
            text = summary;
            return true;
        };
        auto const get_managed_camera = +[](void* opaque, deren::scripting::camera_state* value) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr || value == nullptr) {
                return false;
            }
            auto const& camera = context->render_runtime->camera;
            value->yaw_degrees = glm::degrees(camera.yaw);
            value->pitch_degrees = glm::degrees(camera.pitch);
            value->distance = camera.distance;
            value->target = {.x = camera.target.x, .y = camera.target.y, .z = camera.target.z};
            return true;
        };
        auto const set_managed_camera = +[](void* opaque, deren::scripting::camera_state const* value) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr || value == nullptr ||
                !std::isfinite(value->yaw_degrees) || !std::isfinite(value->pitch_degrees) ||
                !std::isfinite(value->distance) || value->distance <= 0.0F || !std::isfinite(value->target.x) ||
                !std::isfinite(value->target.y) || !std::isfinite(value->target.z)) {
                return false;
            }
            auto& camera = context->render_runtime->camera;
            camera.yaw = glm::radians(value->yaw_degrees);
            camera.pitch = glm::radians(value->pitch_degrees);
            camera.distance = value->distance;
            camera.target = glm::vec3(value->target.x, value->target.y, value->target.z);
            return true;
        };
        auto const request_managed_screenshot = +[](void* opaque) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->request_screenshot();
            return true;
        };
        // The managed side names an environment slot; an empty path loads the procedural sky into it.
        // This only validates and records the newest request - the device work happens in the loop
        // (one request in flight: a newer one replaces the queued one, whatever slot it names).
        auto const set_managed_environment_hdr = +[](void* opaque, std::string_view path,
                                                     std::uint32_t environment_slot) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            if (environment_slot >= deren::engine::render_layout::heap_environment_capacity) {
                return false;
            }
            std::filesystem::path request{std::string(path)};
            if (!request.empty()) {
                // relative paths resolve against the executable, like the reflective probe asset
                if (request.is_relative()) {
                    request = deren::utility::executable_directory() / request;
                }
                std::error_code ec;
                if (!std::filesystem::is_regular_file(request, ec) || ec) {
                    return false;
                }
            }
            context->pending_environment = std::move(request);
            context->pending_environment_slot = environment_slot;
            context->environment_pending = true;
            return true;
        };
        // The toon light rig: the managed side hands over all ten lanes, the frame thread publishes
        // them between frames. This writes the queued value only; the block the character stage reads
        // is the runtime's, and it may not move while a frame is in flight.
        auto const set_managed_toon_rig = +[](void* opaque, deren::scripting::toon_rig_desc const& rig) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->toon_rig = rig;
            context->toon_rig_pending = true;
            return true;
        };
        auto const get_managed_toon_rig = +[](void* opaque, deren::scripting::toon_rig_desc& out) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr) {
                return false;
            }
            out = context->toon_rig;
            return true;
        };
        // The projection aspect: a bare float the frame-start camera setup reads, queued so it lands
        // with the other managed writes. 0 clears the override and restores the window's aspect.
        auto const set_managed_scene_aspect = +[](void* opaque, float aspect) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->scene_aspect = aspect;
            context->scene_aspect_pending = true;
            return true;
        };
        // The request window controls: a borderless project draws its own caption, so the managed UI
        // asks the app's window to minimise, maximise/restore, report its state, close, or start a
        // caption drag. They act on the window only - no scene or frame data is touched - so they are
        // answered immediately instead of being queued with the scene writes. Every one starts from the
        // same capability check: no context, no window, or an offscreen capture's null native handle
        // answers false, which the managed side reports as module_unavailable.
        auto const set_managed_window_minimize = +[](void* opaque) -> bool {
            auto* const context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->window == nullptr || context->window->native_handle() == nullptr) {
                return false;
            }
            context->window->minimize_window();
            return true;
        };
        auto const set_managed_window_toggle_maximize = +[](void* opaque) -> bool {
            auto* const context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->window == nullptr || context->window->native_handle() == nullptr) {
                return false;
            }
            context->window->toggle_maximize_window();
            return true;
        };
        auto const get_managed_window_is_maximized = +[](void* opaque, std::uint8_t* maximized) -> bool {
            auto* const context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->window == nullptr || context->window->native_handle() == nullptr ||
                maximized == nullptr) {
                return false;
            }
            *maximized = context->window->is_maximized_window() ? 1 : 0;
            return true;
        };
        auto const set_managed_window_close = +[](void* opaque) -> bool {
            auto* const context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->window == nullptr || context->window->native_handle() == nullptr) {
                return false;
            }
            context->window->request_close();
            return true;
        };
        auto const set_managed_window_begin_drag = +[](void* opaque) -> bool {
            auto* const context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->window == nullptr || context->window->native_handle() == nullptr) {
                return false;
            }
            return context->window->begin_window_drag();
        };
        // ---- ABI 35: THE ANIMATION ENTRIES ----
        // The queries read the snapshot the frame thread publishes; the commands only queue work for
        // that thread, because the player writes GPU-visible per-slot buffers that may not be touched
        // between pace_and_acquire() and the end of the frame.
        auto const get_managed_animation_state = +[](void* opaque, deren::scripting::animation_state& out) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr) {
                return false;
            }
            std::lock_guard const lock(context->animation_mutex);
            out = context->animation_state;
            return true;
        };
        auto const get_managed_animation_name = +[](void* opaque, std::uint32_t index, std::string_view& name) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->player == nullptr) {
                return false;
            }
            std::lock_guard const lock(context->animation_mutex);
            if (index >= context->animation_names.size()) {
                return false;
            }
            // The view borrows the player's storage and lives until the player is rebuilt.
            name = context->animation_names[index];
            return true;
        };
        auto const queue_managed_animation = +[](void* opaque, deren::app::animation_command_kind kind,
                                                 std::uint32_t index, float value) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->player == nullptr) {
                return false;
            }
            // Reject against the PUBLISHED state, not against the player: a managed request is answered
            // with success only when the frame thread will certainly be able to apply it.
            std::lock_guard const lock(context->animation_mutex);
            switch (kind) {
            case deren::app::animation_command_kind::select:
                if (index >= context->animation_state.playable_count) {
                    return false;
                }
                break;
            case deren::app::animation_command_kind::playing:
                break;
            case deren::app::animation_command_kind::speed:
                if (!std::isfinite(value) || value < 0.0F) {
                    return false;
                }
                break;
            case deren::app::animation_command_kind::time:
                if (!std::isfinite(value)) {
                    return false;
                }
                break;
            }
            context->animation_pending.push_back({.kind = kind, .index = index, .value = value});
            return true;
        };
        // ABI 37: the managed side's fault. This lambda has no way back - the call writes the same
        // report deren::utility::panic writes and terminates the process - so the bool it returns
        // only ever answers a host that records the report instead of dying, which is the test host.
        auto const report_managed_panic = +[](void*, std::uint32_t line, std::string_view message,
                                              std::string_view source_file, std::string_view managed_stack) -> bool {
            deren::utility::panic_managed(message, source_file, line, managed_stack);
        };
        // The former startup-only [render]/[lighting] knobs, now runtime state owned by the managed
        // game. The validation lives in der::app::scripting; these lambdas only forward.
        auto const set_managed_exposure = +[](void* opaque, float exposure) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_exposure(exposure);
            return true;
        };
        auto const set_managed_tone_mapping = +[](void* opaque, std::int32_t mode) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_tone_mapping(mode);
            return true;
        };
        // A calibration change invalidates TAA's accumulated history for the same reason an
        // environment switch does: the history was lit by the old rotation/strength.
        auto const set_managed_environment = +[](void* opaque, float rotation_degrees, float strength,
                                                 std::int32_t background, float color_r, float color_g, float color_b,
                                                 float blur) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_environment(rotation_degrees, strength, background,
                                                     glm::vec3(color_r, color_g, color_b), blur);
            if (context->renderer != nullptr) {
                context->renderer->reset_temporal_history();
            }
            return true;
        };
        auto const set_managed_sun_intensity = +[](void* opaque, float scale) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_sun_intensity(scale);
            return true;
        };
        auto const set_managed_bloom = +[](void* opaque, float intensity, float threshold) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_bloom(intensity, threshold);
            return true;
        };
        auto const set_managed_fxaa = +[](void* opaque, bool enabled, float subpixel, float edge_threshold) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_fxaa(enabled, subpixel, edge_threshold);
            return true;
        };
        auto const set_managed_taa = +[](void* opaque, bool enabled, float blend_static, float blend_min) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->renderer == nullptr) {
                return false;
            }
            context->renderer->set_taa(enabled, blend_static, blend_min);
            return true;
        };
        auto const set_managed_ssao = +[](void* opaque, bool enabled, float radius, float intensity,
                                          std::uint32_t samples) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->renderer == nullptr) {
                return false;
            }
            context->renderer->set_ssao(enabled, radius, intensity, samples);
            return true;
        };
        auto const set_managed_gbuffer_debug = +[](void* opaque, bool enabled) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_gbuffer_debug(enabled);
            return true;
        };
        // Lighting changes relight the previous frame's samples, so TAA's history goes the way of the
        // environment switch: forward to the runtime, then drop the accumulated history.
        auto const set_managed_sun_direction = +[](void* opaque, float x, float y, float z) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_sun_direction(glm::vec3(x, y, z));
            if (context->renderer != nullptr) {
                context->renderer->reset_temporal_history();
            }
            return true;
        };
        auto const set_managed_area_light = +[](void* opaque, float centre_x, float centre_y, float centre_z,
                                                float half, bool irradiance, float axis_x, float axis_y,
                                                float axis_z, float penumbra) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_area_light(glm::vec3(centre_x, centre_y, centre_z), half, irradiance,
                                                    glm::vec3(axis_x, axis_y, axis_z), penumbra);
            if (context->renderer != nullptr) {
                context->renderer->reset_temporal_history();
            }
            return true;
        };
        auto const set_managed_point_lights = +[](void* opaque, deren::scripting::point_light_desc const* lights,
                                                  std::uint32_t count) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            // The ABI record is deliberately flat; this is the only place that knows both shapes, so
            // the conversion and the finiteness check live together. A bad record fails the whole
            // call instead of leaving half a rig applied.
            deren::utility::vessel::array<deren::engine::punctual_light, deren::engine::max_punctual_lights> converted{};
            for (std::uint32_t index = 0; index < count; ++index) {
                auto const& record = lights[index];
                if (!std::isfinite(record.position[0]) || !std::isfinite(record.position[1]) ||
                    !std::isfinite(record.position[2]) || !std::isfinite(record.direction[0]) ||
                    !std::isfinite(record.direction[1]) || !std::isfinite(record.direction[2]) ||
                    !std::isfinite(record.colour[0]) || !std::isfinite(record.colour[1]) ||
                    !std::isfinite(record.colour[2]) || !std::isfinite(record.intensity) ||
                    !std::isfinite(record.range) || record.range <= 0.0F || !std::isfinite(record.outer_cos) ||
                    !std::isfinite(record.inner_cos) || !std::isfinite(record.npr_mode) ||
                    !std::isfinite(record.npr_data[0]) || !std::isfinite(record.npr_data[1]) ||
                    !std::isfinite(record.npr_data[2]) || !std::isfinite(record.npr_data[3])) {
                    return false;
                }
                glm::vec3 const direction{record.direction[0], record.direction[1], record.direction[2]};
                if (record.spot != 0 && glm::dot(direction, direction) <= 1e-12F) {
                    return false;
                }
                auto& light = converted[index];
                light.position = glm::vec3{record.position[0], record.position[1], record.position[2]};
                light.range = record.range;
                light.color = glm::vec3{record.colour[0], record.colour[1], record.colour[2]};
                light.intensity = record.intensity;
                light.spot = record.spot != 0;
                light.spot_direction = light.spot ? glm::normalize(direction) : direction;
                light.spot_outer_cos = record.outer_cos;
                if (record.has_inner_cos != 0) {
                    light.spot_inner_cos = record.inner_cos;
                }
                light.npr_mode = record.npr_mode;
                light.npr_data = glm::vec4{record.npr_data[0], record.npr_data[1], record.npr_data[2],
                                           record.npr_data[3]};
            }
            context->render_runtime->set_point_lights(
                std::span<deren::engine::punctual_light const>{converted.data(), count});
            return true;
        };
        auto const set_managed_clustered_lights = +[](void* opaque, bool enabled) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_clustered_lights(enabled);
            return true;
        };
        // One node subtree's material factors, applied between frames. The engine owns ONE material
        // row per imported slot, so an object that needs its own tint or shading path needs a clone;
        // this walk creates it once per leaf and afterwards rebuilds it from the recorded base row.
        auto const set_managed_max_fps = +[](void* opaque, double fps) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            // The runtime clamps a negative value to "uncapped", but a negative frame rate is a caller
            // error rather than policy, so the app-level validator has already refused it.
            context->render_runtime->set_max_fps(fps);
            return true;
        };
        auto const set_managed_toon_shading = +[](void* opaque, float steps, float softness) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_toon_shading(steps, softness);
            return true;
        };
        auto const set_managed_shadow_enabled = +[](void* opaque, bool enabled) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            context->render_runtime->set_shadow_enabled(enabled);
            return true;
        };
        // One character slot's head frame. The engine keeps one block per slot and uploads the whole
        // array every frame, so this is a plain write between frames: the managed side has already
        // normalised the axes and decided what the `.w` flags mean (captured frame, known centre).
        auto const set_managed_head_basis = +[](void* opaque, std::uint32_t index,
                                                deren::scripting::head_basis_desc const& value) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            auto const axis = [](float const(&values)[3]) {
                return glm::vec3(values[0], values[1], values[2]);
            };
            context->render_runtime->set_head_basis(
                index, deren::engine::head_ubo{
                           .front = glm::vec4(axis(value.front), value.captured != 0 ? 1.0F : 0.0F),
                           .right = glm::vec4(axis(value.right), 0.0F),
                           .up = glm::vec4(axis(value.up), 0.0F),
                           .center = glm::vec4(axis(value.center), value.has_center != 0 ? 1.0F : 0.0F)});
            return true;
        };
        // OFFSCREEN CUBE CAPTURE (ABI 30). The camera and the pass are the host's to install; the face
        // loop, the cadence, and the output path belong to the managed game, which is why these are
        // four separate entries rather than one blocking capture call.
        auto const set_managed_capture_camera = +[](void* opaque, std::uint8_t enabled, float const position[3],
                                                    std::uint32_t face, float near_plane,
                                                    float far_plane) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr || context->renderer == nullptr) {
                return false;
            }
            if (enabled == 0) {
                context->render_runtime->set_camera_override(std::nullopt);
                context->renderer->set_radiance_capture(false);
                return true;
            }
            if (position == nullptr || face > 5) {
                return false;
            }
            context->render_runtime->set_camera_override(deren::engine::make_cube_capture_camera(
                glm::vec3(position[0], position[1], position[2]), face, near_plane, far_plane));
            context->renderer->set_radiance_capture(true);
            return true;
        };
        auto const request_managed_radiance = +[](void* opaque) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            return context != nullptr && context->render_runtime != nullptr &&
                   context->render_runtime->request_radiance_capture();
        };
        auto const acquire_managed_radiance = +[](void* opaque, std::uint32_t& width, std::uint32_t& height,
                                                  float* rgba, std::uint32_t float_count) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr || rgba == nullptr) {
                return false;
            }
            auto image = context->render_runtime->acquire_radiance_image();
            if (!image) {
                deren::utility::error("radiance readback failed: {}", image.error());
                return false;
            }
            std::uint64_t const expected = std::uint64_t(image->width) * image->height * 4;
            if (expected != float_count || image->rgba.size() != expected) {
                deren::utility::error("radiance readback: image is {}x{} ({} floats), caller has {}", image->width,
                                      image->height, image->rgba.size(), float_count);
                return false;
            }
            // The readback is the rendered face as the pass saw it: mirrored in X and with the alpha
            // lane carrying storage noise rather than coverage. Cubemap faces are canonical, so the
            // engine's own canonicalizer converts it before the caller sees anything: the managed side
            // then stores raw faces and never has to know the capture convention.
            if (image->width != image->height) {
                deren::utility::error("radiance readback: {}x{} is not a square face", image->width, image->height);
                return false;
            }
            if (!deren::engine::radiance_detail::canonicalize_face(
                    std::span<float const>{image->rgba.data(), image->rgba.size()},
                    static_cast<std::int32_t>(image->width), std::span<float>{rgba, static_cast<std::size_t>(expected)})) {
                deren::utility::error("radiance readback: the {}x{} face is not canonicalizable", image->width,
                                      image->height);
                return false;
            }
            width = image->width;
            height = image->height;
            return true;
        };
        auto const reset_managed_temporal_history = +[](void* opaque) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->renderer == nullptr) {
                return false;
            }
            context->renderer->reset_temporal_history();
            return true;
        };
        auto const write_managed_radiance_cube = +[](void*, std::string_view path, std::uint32_t size,
                                                     float const* pixels, std::uint32_t float_count) -> bool {
            if (pixels == nullptr || path.empty()) {
                return false;
            }
            auto const written = deren::app::radiance_capture::write_cube(
                std::filesystem::path(path), static_cast<int>(size),
                std::span<float const>{pixels, float_count});
            if (!written) {
                deren::utility::error("radiance cube: {}", deren::app::radiance_capture::describe(written.error()));
                // "invalid pixels" alone is not actionable: report the FIRST offending float, so a
                // capture that produced a negative radiance, a NaN, or a non-unit alpha names itself.
                auto const count = std::size_t{float_count};
                auto const expected = std::size_t{6} * size * size * 4;
                if (count != expected) {
                    deren::utility::error("radiance cube: {} float(s), expected {} for {}px", count, expected, size);
                } else {
                    for (std::size_t index = 0; index < count; ++index) {
                        auto const value = pixels[index];
                        auto const channel = index % 4;
                        if (!std::isfinite(value) || value < 0.0F || value > 65504.0F ||
                            (channel == 3 && value != 1.0F)) {
                            deren::utility::error(
                                "radiance cube: first invalid value {} at pixel {} channel {} ({}x{} face {})",
                                value, index / 4, channel, size, size, (index / 4) / (std::size_t{size} * size));
                            break;
                        }
                    }
                }
                return false;
            }
            deren::utility::log("radiance cube: wrote '{}' ({}px, 6 faces)", std::string(path), size);
            return true;
        };
        auto const apply_managed_material_override =
            +[](void* opaque, deren::engine::scene_tree::scene_node& node,
                deren::scripting::material_override_desc const& value) -> bool {
            auto* context = static_cast<managed_bind_context*>(opaque);
            if (context == nullptr || context->render_runtime == nullptr) {
                return false;
            }
            auto const finite = [](float const number) { return std::isfinite(number); };
            if (!finite(value.tint[0]) || !finite(value.tint[1]) || !finite(value.tint[2]) ||
                !finite(value.metallic_scale) || !finite(value.roughness_scale) ||
                !finite(value.emissive_scale) || value.environment_slot > 3u || value.unlit > 1 ||
                value.enabled > 1) {
                return false;
            }
            auto& runtime = *context->render_runtime;
            glm::vec3 const tint{value.tint[0], value.tint[1], value.tint[2]};
            auto const rewrite = [&](deren::engine::scene_tree::scene_node& entry) {
                if (entry.primitive_leaf == nullptr) {
                    return;
                }
                // Imported leaves always come from runtime::create_primitive(), i.e. they are
                // deren::engine::primitive; RTTI is off, so this is the documented downcast.
                auto& leaf = *static_cast<deren::engine::primitive*>(entry.primitive_leaf.get());
                auto& row = context->material_overrides[leaf.lifetime_id];
                if (value.enabled == 0) {
                    // Turning the override off restores the imported material; the clone stays
                    // allocated for a later re-enable rather than churning a table row per toggle.
                    if (row.base_recorded) {
                        leaf.push.material_index = row.base;
                    }
                    return;
                }
                if (!row.base_recorded) {
                    row.base = leaf.push.material_index;
                    row.base_recorded = true;
                }
                if (row.clone.value == 0) {
                    row.clone = runtime.clone_material(row.base, tint, value.metallic_scale,
                                                       value.roughness_scale, value.emissive_scale,
                                                       value.unlit != 0, value.environment_slot);
                } else {
                    runtime.update_material_override(row.clone, row.base, tint, value.metallic_scale,
                                                     value.roughness_scale, value.emissive_scale,
                                                     value.unlit != 0, value.environment_slot);
                }
                leaf.push.material_index = row.clone;
            };
            auto walk = [&](auto&& self, deren::engine::scene_tree::scene_node& entry) -> void {
                rewrite(entry);
                // `visible` is deliberately not consulted: a hidden subtree must still be rewritten
                // so it renders with the object's factors when it is shown again.
                for (auto& child : entry.children) {
                    self(self, child);
                }
            };
            walk(walk, node);
            runtime.scene_changed();
            return true;
        };
        // ---- ABI 31: THE MANAGED DEBUG PANEL -----------------------------------------------
        // THE PLUGIN IS LOADED ONLY WHEN THE PROJECT ASKS FOR IT: `enable_debug_gui()` resolves
        // `deren_gui_<api>.dll` and returns false when the device cannot present a window or no plugin
        // answers. A project with `[gui] show = false` keeps the ui module published as well, so the
        // game's panel calls report `module_unavailable` and the game can say so in its log - a null
        // table would look like a bug in the game instead of a setting in the project.
        std::optional<deren::ui::rml_overlay> game_ui;
        struct ui_input_context {
            deren::ui::rml_overlay* overlay;
            glfw_window* window;
            deren::input::action_state* actions;
            deren::engine::runtime* renderer;
            bool debug_keyboard_owner = false;
        };
        ui_input_context ui_input{};
        game_ui.emplace(runtime.rhi_face());
        if (!game_ui->initialize(shaders_dir)) {
            return fail_project("RmlUi could not load its shaders");
        }
        deren::ui::rml_scripting document_ui(game_ui->content());
        script_scene.install_document_ui(&deren::ui::rml_scripting::table());
        // Destruct before document_ui/game_ui, on early returns as well as normal shutdown.
        struct script_session_guard {
            deren::app::script_scene& scene;
            ~script_session_guard() {
                this->scene.stop();
            }
        } script_session{script_scene};
        {
            if (!settings.gui.rml_document.empty() &&
                !game_ui->content().open(settings.gui.rml_document, settings.gui.rml_font, settings.gui.rml_fallback_font)) {
                return fail_project("RmlUi could not load its document or font");
            }
            runtime.set_output_overlay(&*game_ui, &deren::ui::rml_overlay::record);
            ui_input = {&*game_ui, &window, &action_state, &runtime};
            action_input.set_event_filter(+[](void* opaque, window_event const& event) -> bool {
                auto& host = *static_cast<ui_input_context*>(opaque);
                auto& document = host.overlay->content();
                using type = window_event_type;
                bool const pointer = event.type == type::mouse_move || event.type == type::mouse_button || event.type == type::scroll;
                bool const keyboard = event.type == type::key || event.type == type::character;
                bool const debug_mouse = host.renderer->debug_gui_wants_mouse() && !document.has_mouse_capture();
                if (event.type == type::focus_lost) {
                    host.debug_keyboard_owner = false;
                }
                if (event.type == type::mouse_button && event.down) {
                    host.debug_keyboard_owner = debug_mouse;
                    if (debug_mouse) {
                        document.input({.type = deren::ui::input_type::focus_lost});
                        host.actions->release_keys();
                    }
                }
                if (pointer && debug_mouse) {
                    document.input({.type = deren::ui::input_type::mouse_leave});
                    return true;
                }
                if (keyboard && host.debug_keyboard_owner) {
                    return true;
                }
                deren::ui::input_event input{};
                switch (event.type) {
                case type::mouse_move: {
                    input.type = deren::ui::input_type::move;
                    int w = 0, h = 0, fw = 0, fh = 0;
                    host.window->window_size(w, h);
                    host.window->framebuffer_size(fw, fh);
                    input.x = w > 0 ? static_cast<int>(event.x * fw / w) : 0;
                    input.y = h > 0 ? static_cast<int>(event.y * fh / h) : 0;
                    break;
                }
                case type::mouse_button:
                    input.type = deren::ui::input_type::button;
                    input.button = event.button;
                    break;
                case type::scroll:
                    input.type = deren::ui::input_type::wheel;
                    input.wheel = event.wheel;
                    break;
                case type::character:
                    input.type = deren::ui::input_type::character;
                    input.codepoint = event.codepoint;
                    break;
                case type::focus_lost:
                    input.type = deren::ui::input_type::focus_lost;
                    break;
                case type::key:
                    input.type = deren::ui::input_type::key;
                    input.key = ui_key(event.key);
                    break;
                }
                input.down = event.down;
                input.modifiers = ((event.modifiers & 1) ? 2 : 0) | ((event.modifiers & 2) ? 1 : 0) | (event.modifiers & 12);
                bool const consumed = host.overlay->content().input(input);
                if (host.overlay->content().wants_keyboard()) {
                    host.actions->release_keys();
                }
                return consumed; }, &ui_input);
        }
        // ABI 31: THE WINDOW'S EVENTS BECOME THE GAME'S EVENTS. The action layer forwards every
        // raw event here (it keeps its own named-action map untouched); this seam only renames the
        // fields into the scripting ABI's flat record, so the app owns no input state either.
        auto const forward_window_event = +[](void*, deren::app::window_event const& source) {
            deren::scripting::input_event event{};
            switch (source.type) {
            case deren::app::window_event_type::mouse_move:
                event.type = deren::scripting::input_event_type::mouse_move;
                break;
            case deren::app::window_event_type::mouse_button:
                event.type = deren::scripting::input_event_type::mouse_button;
                break;
            case deren::app::window_event_type::scroll:
                event.type = deren::scripting::input_event_type::scroll;
                break;
            case deren::app::window_event_type::key:
                event.type = deren::scripting::input_event_type::key;
                break;
            case deren::app::window_event_type::character:
                event.type = deren::scripting::input_event_type::character;
                break;
            case deren::app::window_event_type::focus_lost:
                event.type = deren::scripting::input_event_type::focus_lost;
                break;
            }
            event.button = source.button;
            event.down = source.down ? 1U : 0U;
            event.key = source.key;
            event.x = source.x;
            event.y = source.y;
            event.dx = source.dx;
            event.dy = source.dy;
            event.wheel = source.wheel;
            event.codepoint = source.codepoint;
            script_scene::publish_input_event(event);
        };
        action_input.set_event_sink(forward_window_event, nullptr);
        managed_binding.ui_available = settings.gui.show && runtime.enable_debug_gui();
        if (managed_binding.ui_available) {
            deren::utility::log("gui: managed debug panel enabled (default {}x{})", settings.gui.panel_width,
                                settings.gui.panel_height);
            // `[gui] panel_width/panel_height` is the size a panel gets when the game asks for none: the
            // ABI's zero means "the project's default", not "no size".
            managed_binding.ui_default_width = settings.gui.panel_width;
            managed_binding.ui_default_height = settings.gui.panel_height;
            // One begin/end pair at a time. Each title keeps its own position and widget list.
            auto ui_begin_panel = +[](void* opaque, std::string_view title, float width, float height) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || title.empty() || context.ui_panel_open) {
                    return false;
                }
                auto& overlay = context.render_runtime->debug_gui();
                auto& window = context.ui_windows[std::string{title}];
                if (window.panel == nullptr) {
                    window.panel = &overlay.add_panel(deren::utility::vessel::string{title});
                }
                window.submitted = true;
                if (!window.panel->open()) {
                    return false;
                }
                context.ui_panel = window.panel;
                {
                    // A zero side means "the project's `[gui]` size", so a game can size one axis only.
                    auto const panel_width = width > 0.0F ? width : context.ui_default_width;
                    auto const panel_height = height > 0.0F ? height : context.ui_default_height;
                    context.ui_panel->set_default_size(panel_width, panel_height);
                }
                context.ui_panel->clear();
                context.ui_panel_open = true;
                return true;
            };
            auto ui_end_panel = +[](void* opaque) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || !context.ui_panel_open) {
                    return false;
                }
                context.ui_panel_open = false;
                context.ui_panel = nullptr;
                return true;
            };
            auto ui_text = +[](void* opaque, std::string_view text) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || !context.ui_panel_open) {
                    return false;
                }
                context.ui_panel->add_label(deren::utility::vessel::string{text});
                return true;
            };
            auto ui_checkbox = +[](void* opaque, std::string_view label, std::uint8_t* value) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || !context.ui_panel_open || value == nullptr) {
                    return false;
                }
                // THE MANAGED SIDE'S BYTE IS THE WIDGET'S BOOL: the plugin binds ImGui's `bool*`, and one
                // byte holding 0 or 1 is what both sides mean by a switch. The static_assert is the whole
                // platform assumption this reinterpretation rests on.
                static_assert(sizeof(bool) == 1, "the managed checkbox hands over one byte");
                context.ui_panel->add_checkbox(deren::utility::vessel::string{label}, reinterpret_cast<bool*>(value));
                return true;
            };
            auto ui_slider = +[](void* opaque, std::string_view label, float* value, float minimum, float maximum) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || !context.ui_panel_open || value == nullptr) {
                    return false;
                }
                context.ui_panel->add_slider(deren::utility::vessel::string{label}, value, minimum, maximum);
                return true;
            };
            auto ui_vec3 = +[](void* opaque, std::string_view label, float* value, float speed) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || !context.ui_panel_open || value == nullptr) {
                    return false;
                }
                context.ui_panel->add_vec3(deren::utility::vessel::string{label}, value, speed);
                return true;
            };
            auto ui_combo = +[](void* opaque, std::string_view label, std::string_view const* items, std::uint32_t count,
                                std::int32_t* selected) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || !context.ui_panel_open || items == nullptr || selected == nullptr || count == 0) {
                    return false;
                }
                deren::utility::vessel::vector<deren::utility::vessel::string> names;
                names.reserve(count);
                for (std::uint32_t index = 0; index < count; ++index) {
                    names.emplace_back(items[index]);
                }
                context.ui_panel->add_combo(deren::utility::vessel::string{label}, std::move(names), selected);
                return true;
            };
            auto ui_button = +[](void* opaque, std::string_view label, std::uint8_t* pressed) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || !context.ui_panel_open || pressed == nullptr) {
                    return false;
                }
                // THE PRESS IS A ONE-SHOT BYTE the game reads on its next frame and clears itself; the
                // callback runs while the overlay draws, which is after the managed update of this frame.
                *pressed = 0;
                context.ui_panel->add_button(deren::utility::vessel::string{label}, [pressed] { *pressed = 1; });
                return true;
            };
            auto ui_wants_mouse = +[](void* opaque, std::uint8_t* value) -> bool {
                auto& context = *static_cast<managed_bind_context*>(opaque);
                if (!context.ui_available || value == nullptr) {
                    return false;
                }
                *value = context.render_runtime->debug_gui_wants_mouse() ? 1U : 0U;
                return true;
            };
            // INSTALLED BEFORE start(): the game may build its first panel inside initialize(), and a ui
            // table published afterwards would report module_unavailable for that first frame.
            script_scene.install_ui(deren::app::managed_ui_host{
                .context = &managed_binding,
                .begin_panel = ui_begin_panel,
                .end_panel = ui_end_panel,
                .text = ui_text,
                .checkbox = ui_checkbox,
                .slider = ui_slider,
                .vec3 = ui_vec3,
                .combo = ui_combo,
                .button = ui_button,
                .wants_mouse = ui_wants_mouse});
        }
        auto const started = script_scene.start(
            script_assembly,
            deren::app::managed_scene_host{.context = &managed_binding,
                                           .set_scene = install_managed_scene,
                                           .get_camera = get_managed_camera,
                                           .set_camera = set_managed_camera,
                                           .request_screenshot = request_managed_screenshot,
                                           .set_environment_hdr = set_managed_environment_hdr,
                                           .apply_pipeline = apply_managed_pipeline,
                                           .set_exposure = set_managed_exposure,
                                           .set_tone_mapping = set_managed_tone_mapping,
                                           .set_environment = set_managed_environment,
                                           .set_sun_intensity = set_managed_sun_intensity,
                                           .set_bloom = set_managed_bloom,
                                           .set_fxaa = set_managed_fxaa,
                                           .set_taa = set_managed_taa,
                                           .set_ssao = set_managed_ssao,
                                           .set_gbuffer_debug = set_managed_gbuffer_debug,
                                           .set_sun_direction = set_managed_sun_direction,
                                           .set_area_light = set_managed_area_light,
                                           .set_clustered_lights = set_managed_clustered_lights,
                                           .set_point_lights = set_managed_point_lights,
                                           .apply_material_override = apply_managed_material_override,
                                           .set_max_fps = set_managed_max_fps,
                                           .set_toon_shading = set_managed_toon_shading,
                                           .set_shadow_enabled = set_managed_shadow_enabled,
                                           .set_head_basis = set_managed_head_basis,
                                           .set_capture_camera = set_managed_capture_camera,
                                           .request_radiance_capture = request_managed_radiance,
                                           .acquire_radiance_image = acquire_managed_radiance,
                                           .reset_temporal_history = reset_managed_temporal_history,
                                           .write_radiance_cube = write_managed_radiance_cube,
                                           .animation_state = get_managed_animation_state,
                                           .animation_playable_name = get_managed_animation_name,
                                           .animation_command = queue_managed_animation,
                                           .panic = report_managed_panic,
                                           .viewport_size = query_viewport_size,
                                           .project_point = query_project_point,
                                           .screen_ray = query_screen_ray,
                                           .counters = query_counters,
                                           .gpu_timing_summary = query_gpu_timing_summary,
                                           .set_toon_rig = set_managed_toon_rig,
                                           .get_toon_rig = get_managed_toon_rig,
                                           .set_scene_aspect = set_managed_scene_aspect,
                                           .window_minimize = set_managed_window_minimize,
                                           .window_toggle_maximize = set_managed_window_toggle_maximize,
                                           .window_is_maximized = get_managed_window_is_maximized,
                                           .window_close = set_managed_window_close,
                                           .window_begin_drag = set_managed_window_begin_drag},
            settings.paths.asset_library,
            deren::app::managed_asset_binder{
                .context = &managed_binding, .bind = bind_managed_asset, .before_remove = +[](void* opaque) {
                    auto& context = *static_cast<managed_bind_context*>(opaque);
                    if (!context.starting) {
                        context.render_runtime->rhi_face().wait_idle();
                        context.player.reset();
                        context.animation_names.clear();
                        context.animation_pending.clear();
                        publish_animation_state(context);
                    }
                    // A removed leaf must not survive in the next frame's cached BVH.
                    context.render_runtime->scene_changed();
                }});
        if (!started) {
            deren::utility::println(stderr, "{}", started.error());
            deren::utility::error("{}", started.error());
            deren::utility::wait_log_all();
            return 2;
        }
        // The editor binds assets during update as well as initialize. The context outlives the session.
        managed_binding.starting = false;
        // The scene the game built is only visible through the log once; dumping it here is the last
        // point where "what the game imported" and "what the renderer holds" are the same thing.
        runtime.log_scene_tree();
        // One line naming every optional feature the device could not create. Restored from the
        // pre-managed application, which printed it before its first frame for the same reason.
        runtime.log_feature_status();

        // 10. Scripted capture: apply the camera override LAST, so nothing in the game's setup can win
        //     over the requested view.
        if (capture.camera) {
            runtime.camera.yaw = glm::radians((*capture.camera)[0]);
            runtime.camera.pitch = glm::radians((*capture.camera)[1]);
            runtime.camera.distance = (*capture.camera)[2];
            if (capture.target) {
                runtime.camera.target = *capture.target;
            }
            deren::utility::log("capture camera: yaw {:.1f} deg, pitch {:.1f} deg, distance {:.2f}, target ({:.2f}, {:.2f}, {:.2f})",
                                (*capture.camera)[0], (*capture.camera)[1], (*capture.camera)[2],
                                runtime.camera.target.x, runtime.camera.target.y, runtime.camera.target.z);
        }
        float const sweep_base_yaw = runtime.camera.yaw;

        // Per-frame cheap clock: stamp() once per presented frame on this (the presentation) thread.
        deren::utility::frame_clock frame_clock;
        // FPS statistics (deren.utility:frame_stats): a rolling one-second window of frame gaps.
        deren::utility::frame_stats frame_stats;
        std::int32_t captured_frames = 0; // presented frames so far (scripted capture)
        // capture-only transition probe: exercise progressive restoration while uploads are in flight.
        auto* disable_streaming_text = std::getenv("DEREN_BENCH_DISABLE_STREAMING_FRAME");
        int const disable_streaming_frame = disable_streaming_text ? std::atoi(disable_streaming_text) : -1;
        // Opt-in benchmark trace: keep timestamps in memory and write only after rendering ends.
        char const* const frame_trace_path = std::getenv("DEREN_BENCH_FRAME_TRACE");
        deren::utility::vessel::vector<std::pair<double, double>> frame_trace;
        auto trace_previous = std::chrono::steady_clock::now();
        auto const trace_origin = trace_previous;
        double const trace_unix_ms = std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count(); // NOLINT
        if (frame_trace_path) {
            frame_trace.reserve(4096);
        }
        // Resource queries walk allocator metadata: enable only in separate memory captures.
        char const* const resource_trace_path = std::getenv("DEREN_BENCH_RESOURCE_TRACE");
        struct memory_sample {
            std::uint64_t frame;
            deren::promise::rhi::resource_memory_statistics gpu;
            deren::utility::memory::allocation_statistics cpu;
        };
        deren::utility::vessel::vector<memory_sample> resource_trace;
        std::uint64_t resource_frame = 0;
        auto sample_resources = [&] {
            auto gpu = runtime.rhi_face().resources->memory_statistics();
            auto cpu = deren::utility::vessel::memory_statistics();
            resource_trace.push_back({resource_frame, gpu, cpu});
        };
        if (resource_trace_path) {
            resource_trace.reserve(128);
            sample_resources();
        }
        // ONE place that turns the live pose into the six numbers `--capture-camera` takes: the screenshot
        // prints it (so a bug report carries its camera) and so does the exit below.
        auto const log_camera_pose = [&runtime](char const* const why) {
            deren::utility::log("camera pose ({}): {:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}   -> --capture-camera {:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}",
                                why,
                                glm::degrees(runtime.camera.yaw),
                                glm::degrees(runtime.camera.pitch),
                                runtime.camera.distance,
                                runtime.camera.target.x,
                                runtime.camera.target.y,
                                runtime.camera.target.z,
                                glm::degrees(runtime.camera.yaw),
                                glm::degrees(runtime.camera.pitch),
                                runtime.camera.distance,
                                runtime.camera.target.x,
                                runtime.camera.target.y,
                                runtime.camera.target.z);
        };

        // A + B: ONE PREPARATION PATH FOR EVERY ENVIRONMENT, WITH A DISK CACHE IN FRONT OF THE BAKE.
        // `environment-cache/` sits beside the executable, which is where relative asset paths already
        // resolve, so the cache lives and dies with one build tree. Its key pins the source file's
        // identity and the baked resolutions (see environment_loader.cppm), so editing an HDR or
        // changing the light re-keys an entry instead of silently reusing the old pixels.
        std::filesystem::path const environment_cache_dir = deren::utility::executable_directory() / "environment-cache";
        // A: the analytic sky, baked only when the project has not named slot 0's environment by the
        // time the first recorded frame is about to be drawn. It is the same bake startup used to run
        // unconditionally; the difference is that a project shipping its own HDR never pays for it.
        bool slot0_environment_set = false;

        // A FAILED FRAME ENDS THE SESSION WITH A NON-ZERO CODE: a renderer that could not record,
        // submit or present must not look like a clean exit to a script or CI job. `closed` is the
        // ordinary way out (window closed, ESC) and keeps the exit code it had.
        int exit_code = 0;
        auto const ends_session = [&exit_code](deren::engine::frame_status status) {
            if (status == deren::engine::frame_status::closed) {
                return true;
            }
            if (deren::engine::is_failure(status)) {
                exit_code = 4;
                return true;
            }
            return false;
        };

        while (true) {
            if (capture.frames > 0 && captured_frames == disable_streaming_frame) {
                runtime.set_visibility_texture_streaming(false);
            }
            // Phase 1: poll window events (ESC / native close -> closed, minimized -> skipped)
            deren::engine::frame_status const polled = runtime.poll_events();
            if (ends_session(polled)) {
                break;
            }
            if (polled == deren::engine::frame_status::skipped) {
                frame_stats.on_skipped();
                std::this_thread::yield();
                continue;
            }
            runtime.recreate_if_minimized();
            // dt: the wall clock, clamped - UNLESS the capture asked for a frame-indexed clock.
            float const dt = capture.animation_seconds_per_frame != 0.0f
                                 ? capture.animation_seconds_per_frame
                                 : static_cast<float>(std::min(frame_clock.delta_seconds(), 0.25));
            // the managed game drives the scene here: node transforms, spawns, script ticks
            if (auto const input_frame = action_state.begin_frame(); input_frame.has_value()) {
                if (!script_scene.update(dt, *input_frame)) {
                    return 2;
                }
                for (auto it = managed_binding.ui_windows.begin(); it != managed_binding.ui_windows.end();) {
                    if (!it->second.submitted) {
                        runtime.debug_gui().remove_panel(*it->second.panel);
                        it = managed_binding.ui_windows.erase(it);
                    } else {
                        it->second.submitted = false;
                        ++it;
                    }
                }
                managed_binding.ui_panel = nullptr;
                managed_binding.ui_panel_open = false;
            }
            if (!managed_binding.clouds.empty()) {
                runtime.clear_gaussian_assets();
                auto stage_clouds = [&](auto&& self, deren::engine::scene_tree::scene_node const& node,
                                        glm::mat4 const& parent_world) -> void {
                    if (!node.visible) {
                        return;
                    }
                    auto const world = parent_world * node.local;
                    if (auto const* cloud = deren::engine::scene_tree::components().try_get<managed_cloud_binding>(node.node_id)) {
                        runtime.add_gaussian_asset(cloud->asset, world, cloud->stable_id);
                    }
                    for (auto const& child : node.children) {
                        self(self, child, world);
                    }
                };
                for (auto const& root : runtime.get_scene().roots) {
                    stage_clouds(stage_clouds, root, glm::mat4(1.0f));
                }
            }
            // A managed environment change is applied between frames: preparing the IBL is CPU work
            // and set_ibl waits for the GPU, so neither may run inside the managed callback. The
            // newest request wins (whatever slot it names), and a request that fails to decode keeps
            // that slot's current environment. Every decoded bake goes through the on-disk cache, so
            // a project that ships the same HDR pays for its convolutions once per source, not once
            // per launch.
            if (managed_binding.environment_pending) {
                std::uint32_t const environment_slot = managed_binding.pending_environment_slot;
                std::filesystem::path request = std::move(managed_binding.pending_environment);
                managed_binding.pending_environment.clear();
                managed_binding.pending_environment_slot = 0;
                managed_binding.environment_pending = false;
                auto const prepare_start = std::chrono::steady_clock::now();
                environment_cache_report cache;
                auto prepared = prepare_environment_cached(request, environment_cache_dir, sun_direction,
                                                           env_size, env_mip_count, irr_size, 1.0f, &cache);
                if (!prepared.error.empty()) {
                    deren::utility::log("environment slot {}: '{}' rejected: {} (keeping the slot's current IBL)",
                                        environment_slot, request.empty() ? "procedural sky" : request.string(), prepared.error.c_str());
                } else {
                    // Slot 0 owns the BRDF LUT; the custom slots reuse it, so they upload none
                    // (`set_ibl(slot != 0, ...)` only needs the environment pair). The LUT bytes stay
                    // alive for the whole call: `ibl_input` holds spans, not containers.
                    bool applied = false;
                    if (environment_slot == 0u) {
                        auto const lut = deren::engine::generate_brdf_lut(lut_size);
                        auto const lut_bytes = deren::engine::to_half_rg(lut);
                        applied = runtime.set_ibl(0u, deren::engine::ibl_input{
                                                          .prefiltered_env = prepared.environment,
                                                          .irradiance = prepared.irradiance,
                                                          .brdf_lut = lut_bytes,
                                                          .env_size = static_cast<std::uint32_t>(env_size),
                                                          .env_mip_count = static_cast<std::uint32_t>(env_mip_count),
                                                          .irr_size = static_cast<std::uint32_t>(irr_size),
                                                          .lut_size = static_cast<std::uint32_t>(lut_size),
                                                      });
                    } else {
                        applied = runtime.set_ibl(environment_slot, deren::engine::ibl_input{
                                                                        .prefiltered_env = prepared.environment,
                                                                        .irradiance = prepared.irradiance,
                                                                        .env_size = static_cast<std::uint32_t>(env_size),
                                                                        .env_mip_count = static_cast<std::uint32_t>(env_mip_count),
                                                                        .irr_size = static_cast<std::uint32_t>(irr_size),
                                                                    });
                    }
                    // the history holds the old radiance; a new environment invalidates every pixel
                    app_renderer.reset_temporal_history();
                    // a slot 0 request is the project's answer to "what is the environment", so the
                    // fallback sky must never overwrite it later.
                    if (environment_slot == 0u) {
                        slot0_environment_set = true;
                    }
                    deren::utility::log("environment slot {}: switched to '{}' in {:.1f} ms ({}){}",
                                        environment_slot, request.empty() ? "procedural sky" : request.string(),
                                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare_start).count(),
                                        applied ? "uploaded" : "upload failed",
                                        cache.hit ? ", cache hit" : cache.write_failed ? ", cache write failed"
                                                                                       : ", cached");
                }
            }
            // The managed toon rig is published here for the same reason: the block the character
            // stage reads may not be written while a frame is in flight, and the managed callback
            // runs during script_scene.update(). Newest request wins; lanes a request leaves at the
            // struct defaults keep the construction value.
            if (managed_binding.toon_rig_pending) {
                managed_binding.toon_rig_pending = false;
                deren::engine::toon_rig rig = {};
                std::memcpy(&rig, &managed_binding.toon_rig, sizeof(rig));
                runtime.set_toon_rig(rig);
                deren::utility::log("toon: light rig applied from managed code - day strength {:.3f}, head light {:.2f} at day 1 of {:.2f} at day 0, env strength {:.2f}, shadow softness {}",
                                    rig.day.x, rig.other_light.y, rig.other_light.x, rig.env.x,
                                    static_cast<int>(rig.shadow_softness.x));
            }
            // The projection aspect, for a game that shows the scene inside a sub-rectangle of the
            // window: the render covers the window, but the world is built for this aspect so the
            // stretched image looks right. Zero restores the window's own aspect.
            if (managed_binding.scene_aspect_pending) {
                managed_binding.scene_aspect_pending = false;
                float const aspect = managed_binding.scene_aspect;
                runtime.set_scene_aspect(aspect != 0.0F ? std::optional<float>{aspect} : std::nullopt);
                deren::utility::log("scene: projection aspect {} from managed code",
                                    aspect != 0.0F ? std::format("{:.4f}", aspect) : std::string{"follows the window"});
            }
            // A: THE FALLBACK SKY IS BAKED HERE, NOT AT STARTUP. A project that names slot 0's
            // environment before its first recorded frame (in initialize, or in update) never reaches
            // this point, which is the whole saving: the demo's own HDR used to replace a sky baked
            // seconds earlier. One attempt only - a rejected bake keeps the renderer's furnace cube
            // rather than retrying every frame.
            if (!slot0_environment_set) {
                slot0_environment_set = true;
                auto const bake_start = std::chrono::steady_clock::now();
                environment_cache_report bake_cache;
                auto baked = prepare_environment_cached(std::filesystem::path{}, environment_cache_dir, sun_direction,
                                                        env_size, env_mip_count, irr_size, 1.0f, &bake_cache);
                if (!baked.error.empty()) {
                    deren::utility::log("fallback sky: rejected: {} (the frame keeps the furnace environment)", baked.error.c_str());
                } else {
                    auto const bake_lut = deren::engine::generate_brdf_lut(lut_size);
                    auto const bake_lut_bytes = deren::engine::to_half_rg(bake_lut);
                    bool const bake_applied = runtime.set_ibl(0u, deren::engine::ibl_input{
                                                                      .prefiltered_env = baked.environment,
                                                                      .irradiance = baked.irradiance,
                                                                      .brdf_lut = bake_lut_bytes,
                                                                      .env_size = static_cast<std::uint32_t>(env_size),
                                                                      .env_mip_count = static_cast<std::uint32_t>(env_mip_count),
                                                                      .irr_size = static_cast<std::uint32_t>(irr_size),
                                                                      .lut_size = static_cast<std::uint32_t>(lut_size),
                                                                  });
                    app_renderer.reset_temporal_history();
                    deren::utility::log("fallback sky: {} in {:.1f} ms, cache {} ({}{})",
                                        bake_applied ? "uploaded" : "upload failed",
                                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bake_start).count(),
                                        bake_cache.hit ? "hit" : "miss",
                                        bake_cache.file.empty() ? "disabled" : bake_cache.file.string(),
                                        bake_cache.write_failed ? ", write failed" : "");
                }
            }
            // Scripted capture: the camera sweep, advanced by the number of PRESENTED frames - a frame
            // index, not a clock reading, so two runs of one sweep are byte-identical. It has to happen
            // before pace_and_acquire(), which is the phase that writes the camera UBO.
            if (capture.sweep_yaw_deg_per_frame != 0.0f) {
                runtime.camera.yaw = sweep_base_yaw + glm::radians(capture.sweep_yaw_deg_per_frame) * static_cast<float>(captured_frames);
            }
            // Phase 2: pace + acquire the next frame slot.
            deren::engine::frame_status const paced = runtime.pace_and_acquire();
            if (ends_session(paced)) {
                break;
            }
            if (paced == deren::engine::frame_status::skipped) {
                frame_stats.on_skipped();
                // THE MANAGED TICK ALREADY RAN THIS ITERATION: begin_frame() refuses to hand out a
                // second input frame until finish_frame() closes this one, so a skip that forgot it
                // would stop the game's update() for the rest of the session.
                action_state.finish_frame();
                std::this_thread::yield();
                continue;
            }
            // ABI 35: advance the animation player between the acquired slot and the recording, which is
            // the window its update() contract requires: the pose it writes is the one this frame
            // records, and no managed command can touch the player while the frame is being built.
            if (managed_binding.player != nullptr) {
                std::vector<managed_bind_context::animation_command> commands;
                {
                    std::lock_guard const lock(managed_binding.animation_mutex);
                    commands.swap(managed_binding.animation_pending);
                }
                deren::engine::animation::controller& player = *managed_binding.player;
                for (managed_bind_context::animation_command const& command : commands) {
                    switch (command.kind) {
                    case deren::app::animation_command_kind::select:
                        player.select(command.index);
                        break;
                    case deren::app::animation_command_kind::playing:
                        player.set_playing(command.value != 0.0F);
                        break;
                    case deren::app::animation_command_kind::speed:
                        player.set_speed(command.value);
                        break;
                    case deren::app::animation_command_kind::time:
                        player.set_time(command.value);
                        break;
                    }
                }
                player.update(dt);
                publish_animation_state(managed_binding);
            }
            // Phase 3: record + submit + present the paced frame
            // The before-frame boundary lives here, past every skip path, so before and after are
            // always handed out as a pair. The cost: the camera UBO was already written by
            // pace_and_acquire(), so a before-frame task that moves the camera shows the move one
            // frame later.
            if (!script_scene.signal_before_frame()) {
                return 3;
            }
            deren::engine::frame_status const rec = runtime.begin_recording();
            if (ends_session(rec)) {
                break;
            }
            if (game_ui) {
                auto* image = runtime.rhi_face().frames->frame_image();
                auto const extent = image->extent();
                // The document is authored in density-independent pixels: on a 150%/200% window the
                // framebuffer is larger than the window, and the ratio is what keeps the HUD the
                // authored size instead of shrinking it. Headless windows report 0 and use 1.
                int window_width = 0;
                int window_height = 0;
                window.window_size(window_width, window_height);
                float const pixel_ratio = window_width > 0 && window_height > 0
                                              ? static_cast<float>(extent.width) / static_cast<float>(window_width)
                                              : 1.0F;
                // A document may show the 3D scene itself (`deren://scene`): hand the frame's resolved color
                // image over so the overlay samples it instead of an atlas. The image exists only on frames
                // a resolve finishes (FXAA/upscale); on the others the viewport element draws nothing.
                auto const scene_color = runtime.current_scene_color();
                deren::ui::scene_texture const scene_texture{.image = scene_color.image};
                if (!game_ui->prepare(extent.width, extent.height, image->format(),
                                      runtime.rhi_face().frames->walk_frames()->position(), pixel_ratio, scene_texture)) {
                    return fail_project("RmlUi could not prepare its overlay");
                }
                if (!document_ui.has_managed_document()) {
                    for (auto const& event : game_ui->content().take_events()) {
                        deren::utility::log("rmlui: {} {} {}", event.type, event.id, event.value);
                        if (event.type == "click" && event.id == "apply") {
                            game_ui->content().set_text("status", game_ui->content().value("name"));
                        }
                    }
                }
            }
            runtime.prepare_scene_recording();
            deren::engine::frame_status const recorded = runtime.record_graph();
            if (ends_session(recorded)) {
                break;
            }
            deren::engine::frame_status const ended = runtime.end_recording();
            if (ends_session(ended)) {
                break;
            }
            deren::engine::frame_status const result = runtime.submit_and_present();
            if (ends_session(result)) {
                break;
            }
            if (result == deren::engine::frame_status::skipped) {
                frame_stats.on_skipped();
                // THE TICK MUST STILL CLOSE: the frame was recorded but never presented, and the
                // managed update that ran earlier in this iteration is waiting for its finish.
                action_state.finish_frame();
                std::this_thread::yield();
                continue;
            }
            action_state.finish_frame();

            // A frame was presented: publish its stamp for cheap readers (frame_clock)
            ++resource_frame;
            if (resource_trace_path && (resource_frame % 10 == 0 || resource_frame == static_cast<std::uint64_t>(capture.frames))) {
                sample_resources();
            }
            if (frame_trace_path) {
                auto const boundary = std::chrono::steady_clock::now();
                frame_trace.emplace_back(std::chrono::duration<double, std::milli>(boundary - trace_origin).count(),
                                         std::chrono::duration<double, std::milli>(boundary - trace_previous).count());
                trace_previous = boundary;
            }
            splash.close();
            frame_clock.stamp();
            // The host only reports the frame boundary. Managed code drains its own queues here and
            // reports back through native_api notify_rendering; no managed callable is stored on
            // the native side.
            if (!script_scene.signal_after_frame()) {
                return 3;
            }

            // scripted capture: once the requested number of frames has been PRESENTED, request the
            // screenshot from the runtime - the F12 block below consumes the request in this same
            // iteration, so the capture happens on a fully warmed-up frame
            if (capture.frames > 0 && ++captured_frames >= capture.frames) {
                runtime.request_screenshot();
            }

            // fps statistics: accumulate the frame gap into the rolling window
            frame_stats.tick();
            if (frame_stats.window_rolled()) {
                deren::utility::log("fps: {:.1f} ({:.2f} ms/frame)", frame_stats.window_fps(), frame_stats.window_frame_ms());
            }

            // F12 / managed screenshot request: the runtime reports the request (edge-triggered in
            // poll_events), main captures the presented swapchain image and writes it as a PNG.
            if (runtime.consume_screenshot_request()) {
                auto const image = runtime.acquire_current_frame_image();
                if (!image) {
                    deren::utility::log("screenshot failed: {}", image.error());
                } else {
                    // base directory from [paths] screenshot_dir (empty = the current working
                    // directory); created on demand so a fresh checkout can capture immediately
                    std::filesystem::path directory(settings.paths.screenshot_dir);
                    if (!directory.empty()) {
                        std::error_code ec;
                        std::filesystem::create_directories(directory, ec);
                        if (ec) {
                            deren::utility::log("screenshot: cannot create '{}' - saving to the working directory", directory.string());
                            directory.clear();
                        }
                    }
                    std::filesystem::path const path = directory / deren::utility::vessel::format("screenshot_{:%Y%m%d_%H%M%S}.png", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
                    auto const written = deren::utility::write_png(path, image->width, image->height, image->rgba);
                    if (written) {
                        deren::utility::log("screenshot saved: {} ({}x{})", path.string(), image->width, image->height);
                    } else {
                        deren::utility::log("screenshot save failed: {}", written.error());
                    }
                    // ... AND THE POSE THAT FRAME WAS RENDERED WITH, so a screenshot is reproducible.
                    log_camera_pose("screenshot");
                }
                // scripted capture: the frame is captured (saved or not), so leave the render loop
                if (capture.frames > 0) {
                    break;
                }
            }
        }

        // The pose the session ENDED on, so a view reached by orbiting can be written down.
        log_camera_pose("exit");
        // 15. Wait for the GPU to finish; primitives and pipelines are released by the runtime destructor
        runtime.rhi_face().wait_idle();
        if (resource_trace_path) {
            std::ofstream trace(resource_trace_path, std::ios::binary | std::ios::out);
            trace << "frame,supported,gpu_buffer_local,gpu_image_local,gpu_total_local,gpu_peak_local,gpu_total,gpu_reserved,gpu_reserved_local,cpu_current,cpu_peak\n";
            for (auto const& sample : resource_trace) {
                trace << deren::utility::vessel::format("{},{},{},{},{},{},{},{},{},{},{}\n", sample.frame, sample.gpu.supported ? 1 : 0,
                                                        sample.gpu.buffers.current_device_local_bytes, sample.gpu.images.current_device_local_bytes,
                                                        sample.gpu.total.current_device_local_bytes, sample.gpu.total.peak_device_local_bytes, sample.gpu.total.current_bytes,
                                                        sample.gpu.allocator_reserved_bytes, sample.gpu.allocator_device_local_reserved_bytes,
                                                        sample.cpu.current_bytes, sample.cpu.peak_bytes);
            }
            if (!trace) {
                deren::utility::error("benchmark resource trace could not be written");
            }
        }
        if (frame_trace_path) {
            std::ofstream trace(frame_trace_path, std::ios::binary | std::ios::out);
            trace << "frame,unix_ms,interval_ms\n";
            for (std::size_t i = 0; i < frame_trace.size(); ++i) {
                trace << deren::utility::vessel::format("{},{:.6f},{:.6f}\n", i + 1, trace_unix_ms + frame_trace[i].first, frame_trace[i].second);
            }
            if (!trace) {
                deren::utility::error("benchmark frame trace could not be written");
            }
        }
        deren::utility::log("render loop finished");
        return exit_code;
    }
} // namespace deren::app
