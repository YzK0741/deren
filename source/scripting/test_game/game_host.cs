using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using deren;
using deren.scripting;

namespace game {
    /// <summary>
    /// The project owns the whole managed side: it chooses assets from the library, builds and
    /// activates its scene tree, binds renderable assets, attaches scripts, and registers the
    /// frame/shutdown callbacks. The managed host only loads this type and forwards calls.
    /// </summary>
    internal sealed unsafe class game_host : game_entry {
        private static readonly Dictionary<string, int> action_indices = new() {
            ["move_left"] = 0,
            ["move_right"] = 1,
            ["toggle_spin"] = 2,
            ["spawn_helmet"] = 3
        };

        private static game_host? instance;
        private readonly List<script_base> scripts = [];
        private scene_tree? tree;
        private bool camera_checked;
        private bool frame_signal_scheduled;
        private bool frame_signal_ran;
        private bool screenshot_requested;
        private bool animation_reported;
        private reflect_probe_session? probe;
        private bool probe_active => probe != null && probe.active;
        // --asset <library-id> replaces the helmet demo with ONE bound asset, and --pipeline <name>
        // picks the pipeline it is drawn with (the asset's declared default otherwise). A developer
        // check for a real library asset, e.g. --asset laevatain --pipeline pbr.
        private static string? requested_asset;
        private static string? requested_pipeline;
        // --probe <file> captures an offscreen reflection probe of the scene and writes an IBL1 cube.
        // --probe-size sets the square face, which must match the window because the capture copies the
        // frame the window already renders; --probe-position sets the eye (x,y,z). The whole six-face
        // loop is managed: the host only installs the camera and runs the readback.
        private static string? requested_probe;
        private static uint requested_probe_size = 256;
        private static vector_3 requested_probe_position;
        private demo_debug_windows? debug_windows;
        private demo_audio_panel? audio_panel;
        private static string? requested_audio;
        private node_id initially_hidden;
        private bool input_registered;
        private bool left_down;
        private float orbit_dx;
        private static bool input_reported;
        // Test-only frame event self check. `--frame-events-selftest` turns the demo into a fixed
        // six-boundary script: it registers one task of every kind, then fails the update as soon as
        // a queue stops matching the contract. The C++ case `managed_host_frame_events` drives the
        // boundaries and asserts the notify count; the expectations below are the other half.
        private bool frame_events_selftest;
        private bool frame_events_registered;
        private int frame_events_boundaries;
        private int frame_events_before;
        private int frame_events_after;
        private int frame_events_repeat;
        private int frame_events_counted;
        private int frame_events_nonblocking;
        private int frame_events_refusals;
        private int frame_events_cancelled;
        private int frame_events_serial;
        private int frame_events_before_serial;
        private int frame_events_after_serial;
        private ulong frame_events_repeat_handle;

        public script_status preinit(startup_options* options, int argc, byte** argv) {
            if (options == null || argc < 0 || (argc > 0 && argv == null)) {
                return script_status.invalid_argument;
            }
            var args = new string[argc];
            for (var i = 0; i < argc; ++i) {
                args[i] = argv[i] == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)argv[i]) ?? string.Empty;
            }
            for (var i = 1; i < args.Length; ++i) {
                var argument = args[i];
                if (argument == "--capture-windowed") {
                    options->capture_windowed = 1;
                    continue;
                }
                if (argument == "--audio-panel") {
                    requested_audio = i + 1 < args.Length && !args[i + 1].StartsWith("--", StringComparison.Ordinal)
                        ? args[++i] : string.Empty;
                    continue;
                }
                if (argument == "--rml-selftest" && i + 1 < args.Length) {
                    rml_selftest.path = args[++i];
                    continue;
                }
                if (argument == "--audio-selftest" && i + 1 < args.Length) {
                    audio_selftest.path = args[++i];
                    continue;
                }
                if (argument == "--frame-events-selftest") {
                    frame_events_selftest = true;
                    continue;
                }
                if (argument == "--toon-selftest") {
                    toon_selftest.requested = true;
                    continue;
                }
                if (argument == "--node-name-selftest") {
                    node_name_selftest.requested = true;
                    continue;
                }
                if (argument == "--scene-aspect-selftest") {
                    aspect_selftest.requested = true;
                    continue;
                }
                if (argument == "--rml-layout-selftest" && i + 1 < args.Length) {
                    rml_layout_selftest.path = args[++i];
                    continue;
                }
                if (argument == "--rml-optional-absent") {
                    rml_layout_selftest.optional_absent = true;
                    continue;
                }
                var separator = argument.IndexOf('=');
                var name = separator < 0 ? argument : argument[..separator];
                if (name == "--asset" || name == "--pipeline") {
                    var chosen = separator < 0
                        ? (i + 1 < args.Length ? args[++i] : string.Empty)
                        : argument[(separator + 1)..];
                    if (name == "--asset") {
                        requested_asset = chosen;
                    } else {
                        requested_pipeline = chosen;
                    }
                    continue;
                }
                if (name == "--probe" || name == "--probe-size" || name == "--probe-position") {
                    var chosen = separator < 0
                        ? (i + 1 < args.Length ? args[++i] : string.Empty)
                        : argument[(separator + 1)..];
                    if (name == "--probe") {
                        requested_probe = chosen;
                    } else if (name == "--probe-size") {
                        if (uint.TryParse(chosen, NumberStyles.Integer, CultureInfo.InvariantCulture, out var face)) {
                            requested_probe_size = face;
                        }
                    } else {
                        requested_probe_position = parse_vector(chosen, requested_probe_position);
                    }
                    continue;
                }
                if (name != "--capture-frames" && name != "--capture-sweep" &&
                    name != "--capture-animation-sweep" && name != "--capture-camera") {
                    continue;
                }
                var value = separator < 0 ? (i + 1 < args.Length ? args[++i] : string.Empty) : argument[(separator + 1)..];
                if (name == "--capture-frames") {
                    if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var frames)) {
                        options->capture_frames = Math.Max(0, frames);
                    }
                } else if (name == "--capture-sweep") {
                    if (float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var sweep)) {
                        options->sweep_yaw_deg_per_frame = sweep;
                    }
                } else if (name == "--capture-animation-sweep") {
                    if (float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var animation)) {
                        options->animation_seconds_per_frame = animation;
                    }
                } else {
                    parse_camera(options, value);
                }
            }
            return script_status.success;
        }

        private static vector_3 parse_vector(string text, vector_3 fallback) {
            var parts = text.Split(',');
            if (parts.Length != 3) {
                return fallback;
            }
            var numbers = new float[3];
            for (var index = 0; index < 3; ++index) {
                if (!float.TryParse(parts[index], NumberStyles.Float, CultureInfo.InvariantCulture,
                                    out numbers[index])) {
                    return fallback;
                }
            }
            return new vector_3 { x = numbers[0], y = numbers[1], z = numbers[2] };
        }

        private static void parse_camera(startup_options* options, string text) {
            var parts = text.Split(',');
            if (parts.Length != 3 && parts.Length != 6) {
                return;
            }
            var numbers = new float[parts.Length];
            for (var i = 0; i < parts.Length; ++i) {
                if (!float.TryParse(parts[i], NumberStyles.Float, CultureInfo.InvariantCulture, out numbers[i])) {
                    return;
                }
            }
            options->camera_set = 1;
            options->camera_yaw_degrees = numbers[0];
            options->camera_pitch_degrees = numbers[1];
            options->camera_distance = numbers[2];
            if (parts.Length == 6) {
                options->target_set = 1;
                options->target.x = numbers[3];
                options->target.y = numbers[4];
                options->target.z = numbers[5];
            }
        }

        public script_status initialize(native_api* api, managed_api* scripts) {
            if (api == null || scripts == null || instance != null) {
                return script_status.invalid_argument;
            }
            var module_status = scene_api.initialize(api);
            if (module_status != script_status.success) {
                return module_status;
            }
            if (rml_selftest.path != null) {
                return rml_selftest.initialize(scripts);
            }
            if (rml_layout_selftest.path != null) {
                return rml_layout_selftest.initialize(scripts);
            }
            if (audio_selftest.path != null) {
                return audio_selftest.initialize(scripts);
            }
            if (toon_selftest.requested) {
                return toon_selftest.initialize(scripts);
            }
            if (node_name_selftest.requested) {
                return node_name_selftest.initialize(scripts);
            }
            if (aspect_selftest.requested) {
                return aspect_selftest.initialize(scripts);
            }
            // ABI 35: every run exercises the settable boundary wait, so the host test can assert
            // the value arrived and the demo keeps a wait that is still generous for its batches.
            if (!frame_scheduler.set_boundary_timeout_ms(4000)) {
                return script_status.script_failure;
            }
            if (!orbit_camera.try_get(out var pose)) {
                return script_status.script_failure;
            }
            pose.yaw_degrees = 0.0f;
            pose.pitch_degrees = 0.0f;
            pose.distance = 4.4f;
            pose.target = new vector_3 { x = 0.0f, y = -0.8f, z = 0.0f };
            if (!pose.try_apply()) {
                return script_status.script_failure;
            }
            if (!scene_tree.try_create(out var created) || created == null) {
                return script_status.script_failure;
            }
            tree = created;
            if (requested_asset != null) {
                try {
                    build_asset_scene(api, requested_asset);
                } catch (Exception error) {
                    api->write_log(System.Text.Encoding.UTF8.GetBytes(
                        $"Game: asset scene initialization failed: {error.GetBaseException().Message}"));
                    cleanup();
                    return script_status.script_failure;
                }
                audio_panel = demo_audio_panel.create(requested_audio);
                instance = this;
                scripts->update = &update;
                scripts->stop = &stop;
                api->write_log("Game: asset scene ready"u8);
                panic_probe();
                return script_status.success;
            }
            try {
                if (!asset_manager.try_load("damaged-helmet", out var helmet) || !helmet.is_valid) {
                    throw new InvalidOperationException("cannot load 'damaged-helmet'");
                }
                try {
                    var left = create_root("Left Helmet", -1.2f);
                    var right = create_root("Right Helmet", 1.2f);
                    var spawned = create_root("Spawned Helmet", 0.0f);
                    if (!tree.try_activate()) {
                        throw new InvalidOperationException("cannot activate the managed scene");
                    }
                    var left_bound = bind_helmet(left, helmet);
                    bind_helmet(right, helmet);
                    bind_helmet(spawned, helmet);
                    exercise_pipelines(api, helmet, left_bound);
                    exercise_post_processing(api);
                    exercise_lighting(api);
                    exercise_environment_slots(left_bound);
                    if (!tree.try_set_visible(spawned, false)) {
                        throw new InvalidOperationException("cannot hide the spawn root");
                    }
                    initially_hidden = spawned;
                    attach(new move_helmet(), left);
                    attach(new spin_helmet(), right);
                } finally {
                    if (!asset_manager.release(helmet)) {
                        throw new InvalidOperationException("cannot release 'damaged-helmet'");
                    }
                }
            } catch (Exception error) {
                api->write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: scene initialization failed: {error.GetBaseException().Message}"));
                cleanup();
                return script_status.script_failure;
            }

            audio_panel = demo_audio_panel.create(requested_audio);
            instance = this;
            scripts->update = &update;
            scripts->stop = &stop;
            api->write_log("Game: helmet scene ready"u8);
            panic_probe();
            return script_status.success;
        }

        /// <summary>
        /// ABI 37: on request, reports a managed fault and RETURNS.
        ///
        /// Only the host test gets to see the line after the call: the real host writes the report and
        /// terminates the process inside host_logger.panic. The probe exists so both halves of that contract
        /// have evidence - the report crossing the boundary, and the host that ends the process.
        /// </summary>
        private static void panic_probe() {
            // "throw" exercises the other half of the contract: an exception nobody catches leaves the
            // module through the same report, because managed_bootstrap hooks
            // AppDomain.UnhandledException. The window the demo's try/catch blocks cover is above this
            // call, so nothing swallows it.
            if (System.Environment.GetEnvironmentVariable("DEREN_PANIC_PROBE") == "throw") {
                throw new InvalidOperationException("panic probe: an unhandled managed exception");
            }
            if (System.Environment.GetEnvironmentVariable("DEREN_PANIC_PROBE") != "1") {
                return;
            }
            host_logger.panic("panic probe: the managed module reports a fault on request");
        }

        /// <summary>
        /// ONE library asset, bound at the origin and drawn with the requested pipeline (the asset's
        /// declared default when no --pipeline was given). This is the developer path for checking a
        /// real asset end to end without the helmet demo's three roots.
        /// </summary>
        private void build_asset_scene(native_api* api, string asset_id) {
            if (!asset_manager.try_load(asset_id, out var asset) || !asset.is_valid) {
                throw new InvalidOperationException($"cannot load '{asset_id}'");
            }
            try {
                var root = create_root(asset_id, 0.0f);
                if (!tree!.try_activate()) {
                    throw new InvalidOperationException("cannot activate the managed scene");
                }
                var bound = bind_helmet(root, asset);
                exercise_animation(api, asset_id);
                // The probe is started before the gaussian early return below: a cloud is exactly the
                // scene a reflection probe is usually captured from.
                if (requested_probe != null) {
                    probe = new reflect_probe_session(requested_probe_position, requested_probe_size,
                                                      requested_probe);
                    if (probe.begin()) {
                        api->write_log(System.Text.Encoding.UTF8.GetBytes(
                            $"Game: reflect probe: capturing {requested_probe_size}px cube of the scene at ({requested_probe_position.x}, {requested_probe_position.y}, {requested_probe_position.z}) into {requested_probe}"));
                    } else {
                        api->write_log(System.Text.Encoding.UTF8.GetBytes(
                            $"Game: reflect probe: cannot start ({probe.failure})"));
                        probe = null;
                    }
                }
                // A GAUSSIAN ASSET DECLARES NO RENDERER PIPELINE: its splat stage is not one of the
                // named pipelines, so the bind already staged the cloud and there are no leaves to
                // relabel. A cloud also has no sun rig to push - the rig is a character's lighting.
                var declared_pipelines = asset_manager.supported_pipelines(asset);
                if (declared_pipelines.Length == 0) {
                    api->write_log(System.Text.Encoding.UTF8.GetBytes(
                        $"Game: gaussian cloud '{asset_id}' bound (no named pipeline; the splat stage draws it)"));
                    return;
                }
                var default_pipeline = asset_manager.try_get_default_pipeline(asset, out var declared) && declared != null
                    ? declared
                    : "pbr";
                var pipeline = requested_pipeline ?? default_pipeline;
                if (!tree.try_set_pipeline(bound, pipeline)) {
                    throw new InvalidOperationException($"cannot apply pipeline '{pipeline}' to '{asset_id}'");
                }
                api->write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: asset '{asset_id}' bound with pipeline '{pipeline}' (declared [{string.Join(", ", asset_manager.supported_pipelines(asset))}])"));
                // The character rig is project data (toon-light-rig.toml next to deren.toml). It is
                // the lighting the reference render used, so pushing it is part of reproducing that
                // render; a project without the file keeps the sun-and-environment lighting.
                light_rig.try_load_project_rig();
                // A toon character's face is shaded against its head frame, which is a property of the
                // asset the loader measured. Publishing it here keeps the frame and the geometry it
                // belongs to in one place: a project that binds several characters gets one per slot,
                // and an asset with no face measurement (a prop, a cloud) simply keeps the fallback.
                if (pipeline == "game_toon" && !head_basis.try_publish(asset)) {
                    api->write_log(System.Text.Encoding.UTF8.GetBytes(
                        $"Game: head basis for '{asset_id}' kept at the renderer fallback"));
                }
            } finally {
                if (!asset_manager.release(asset)) {
                    throw new InvalidOperationException($"cannot release '{asset_id}'");
                }
            }
        }

        private node_id create_root(string name, float x) {
            var local = new transform_value { scale = new vector_3 { x = 1.0f, y = 1.0f, z = 1.0f } };
            local.position.x = x;
            if (!tree!.try_create_node(default, name, local, out var root) || !root.is_valid) {
                throw new InvalidOperationException($"cannot create root '{name}'");
            }
            return root;
        }

        private node_id bind_helmet(node_id root, asset_handle helmet) {
            var offset = new transform_value { scale = new vector_3 { x = 1.0f, y = 1.0f, z = 1.0f } };
            if (!tree!.try_bind_asset(root, helmet, offset, out var bound) || !bound.is_valid) {
                throw new InvalidOperationException("cannot bind 'damaged-helmet'");
            }
            return bound;
        }

        /// <summary>
        /// Proves the asset-declared pipeline information reaches managed code and that a bind
        /// root accepts its own default pipeline while a name the asset does not declare is
        /// rejected. The default is "pbr", so the rendered image is unchanged.
        /// </summary>
        private void exercise_pipelines(native_api* api, asset_handle asset, node_id bound) {
            var supported = asset_manager.supported_pipelines(asset);
            if (supported.Length == 0) {
                throw new InvalidOperationException("asset declares no pipeline");
            }
            if (!asset_manager.try_get_default_pipeline(asset, out var default_pipeline) || default_pipeline == null) {
                throw new InvalidOperationException("asset declares no default pipeline");
            }
            if (Array.IndexOf(supported, default_pipeline) < 0) {
                throw new InvalidOperationException($"default pipeline '{default_pipeline}' is not in the declared list");
            }
            if (!tree!.try_set_pipeline(bound, default_pipeline)) {
                throw new InvalidOperationException($"cannot apply pipeline '{default_pipeline}'");
            }
            if (tree.try_set_pipeline(bound, "not-a-pipeline")) {
                throw new InvalidOperationException("an undeclared pipeline name was accepted");
            }
            api->write_log(System.Text.Encoding.UTF8.GetBytes(
                $"Game: asset pipelines [{string.Join(", ", supported)}], default '{default_pipeline}'"));
        }

        /// <summary>
        /// Proves the startup-only post-processing/environment knobs now belong to the game: every
        /// setter is exercised, an out-of-range value is rejected instead of clamped, and the state
        /// is restored to the configuration defaults afterwards so the captured image is unchanged.
        /// </summary>
        private void exercise_post_processing(native_api* api) {
            if (!post_processing.try_set_exposure(1.5f) || !post_processing.try_set_tone_mapping(1) ||
                !post_processing.try_set_environment(45.0f, 2.0f, 1, 1.0f, 1.0f, 1.0f, 0.25f) ||
                !post_processing.try_set_sun_intensity(1.5f) || !post_processing.try_set_bloom(0.5f, 0.4f) ||
                !post_processing.try_set_fxaa(true) || !post_processing.try_set_taa(true) ||
                !post_processing.try_set_ssao(true, 0.7f, 0.8f, 12) || !post_processing.try_set_gbuffer_debug(true)) {
                throw new InvalidOperationException("a post-processing setter was rejected");
            }
            if (post_processing.try_set_exposure(50.0f) || post_processing.try_set_taa(true, 0.9f, 0.95f)) {
                throw new InvalidOperationException("out-of-range post-processing state was accepted");
            }
            if (!post_processing.try_set_exposure(1.0f) || !post_processing.try_set_tone_mapping(0) ||
                !post_processing.try_set_environment(0.0f, 1.0f, 0, 0.18f, 0.18f, 0.18f, 0.5f) ||
                !post_processing.try_set_sun_intensity(1.0f) || !post_processing.try_set_bloom(0.0f, 0.6f) ||
                !post_processing.try_set_fxaa(false) || !post_processing.try_set_taa(false) ||
                !post_processing.try_set_ssao(true, 0.5f, 1.0f, 8) || !post_processing.try_set_gbuffer_debug(false)) {
                throw new InvalidOperationException("cannot restore the startup post-processing state");
            }
            api->write_log("Game: post-processing round trip"u8);
        }

        /// <summary>
        /// Proves the sun direction, the area emitter and the clustered-light switch are runtime
        /// state: every setter is exercised, an invalid direction is rejected, and the startup state
        /// is restored afterwards so the captured image is unchanged.
        /// </summary>
        private void exercise_lighting(native_api* api) {
            var centre = new vector_3 { x = 0.0f, y = 1.0f, z = 0.0f };
            var axis = new vector_3 { x = 0.0f, y = 1.0f, z = 0.0f };
            if (!scene_lighting.try_set_sun_direction(new vector_3 { x = 1.0f, y = 0.8f, z = 0.2f }) ||
                !scene_lighting.try_set_area_light(centre, 0.5f, false, axis, 0.3f) ||
                !scene_lighting.try_set_clustered_lights(false)) {
                throw new InvalidOperationException("a lighting setter was rejected");
            }
            if (scene_lighting.try_set_sun_direction(new vector_3())) {
                throw new InvalidOperationException("an invalid sun direction was accepted");
            }
            if (!scene_lighting.try_set_sun_direction(new vector_3 { x = 0.3f, y = 1.0f, z = 0.5f }) ||
                !scene_lighting.try_clear_area_light() || !scene_lighting.try_set_clustered_lights(true)) {
                throw new InvalidOperationException("cannot restore the startup lighting state");
            }
            api->write_log("Game: lighting round trip"u8);
        }

        /// <summary>
        /// The per-material environment lanes: the project config fills the scene-wide slot, but a
        /// material record may name slots 1..3. This puts a real image into slot 1 and binds the left
        /// helmet to it, so the lane has an end-to-end artifact instead of only a documented entry
        /// point - the host test asserts both requests, and the render log names slot 1.
        /// </summary>
        private void exercise_environment_slots(node_id bound) {
            if (!global_environment.try_set_slot_hdr(1, "environments/neutral.ibl")) {
                throw new InvalidOperationException(
                    "cannot load 'environments/neutral.ibl' into environment slot 1");
            }
            var factors = material_override.imported;
            factors.environment_slot = 1;
            if (!tree!.try_set_material_override(bound, factors)) {
                throw new InvalidOperationException("cannot bind the left helmet to environment slot 1");
            }
            host_logger.write("Game: environment slot 1 bound to the left helmet");
        }

        /// <summary>
        /// The host's animation player, driven from the game: this asset's manifest names a motion
        /// file, so the host built a player at bind. The game only names the clip and starts it; the
        /// first update reports the clock once it has advanced, which is the end-to-end artifact the
        /// host test and the render log both look for.
        /// </summary>
        private void exercise_animation(native_api* api, string asset_id) {
            if (!animation_player.try_get_info(out var state)) {
                api->write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: animation: '{asset_id}' has no player (the asset names no playable motion)"));
                return;
            }
            var names = animation_player.get_clip_names();
            if (names.Length != state.clip_count) {
                throw new InvalidOperationException("animation: the clip count and the clip names disagree");
            }
            api->write_log(System.Text.Encoding.UTF8.GetBytes(
                $"Game: animation: '{asset_id}' offers {state.clip_count} clip(s): {string.Join(", ", names)}"));
            if (state.clip_count == 0) {
                return;
            }
            if (!animation_player.try_select(0) || !animation_player.try_set_speed(1.0F) || !animation_player.try_play()) {
                throw new InvalidOperationException("animation: cannot start the first clip");
            }
        }

        private void update_debug_windows(float delta_time) {
            if (!input_registered) {
                input_registered = true;
                // ONE HANDLER FOR THE WHOLE WINDOW STREAM. The host reports what the window produced and
                // keeps no input state; what a wheel notch or an escape key means is the game's call.
                if (!input_events.try_set_callback(on_input)) {
                    managed_bootstrap.core.write_log(
                        System.Text.Encoding.UTF8.GetBytes("Game: input events unsupported by this host"));
                }
            }
            // The camera drag is applied here, on the frame after the events: a game reads the stream and
            // decides when the pose changes.
            if (orbit_dx != 0.0f && orbit_camera.try_get(out var pose)) {
                pose.yaw_degrees += orbit_dx;
                pose.try_apply();
                orbit_dx = 0.0f;
            }
            if (debug_ui.available && tree != null) {
                debug_windows ??= new demo_debug_windows(initially_hidden);
                debug_windows.update(tree, delta_time);
            }
        }

        /// Every window event, on the render thread. This demo orbits the camera while the left button is
        /// held over the scene - the cheapest proof that the stream arrives with usable deltas.
        private static void on_input(input_event value) {
            var current = instance;
            if (current == null) {
                return;
            }
            if (!input_reported) {
                input_reported = true;
                managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: input stream live (type {(uint)value.type})"));
            }
            if (value.type == input_type.mouse_button && value.button == 0) {
                current.left_down = value.down;
                return;
            }
            if (value.type != input_type.mouse_move || !current.left_down || debug_ui.wants_mouse()) {
                return;
            }
            current.orbit_dx += -value.dx * 0.25f;
        }

        private script_status frame_events_check() {
            return frame_events_registered ? frame_events_verify() : frame_events_register();
        }

        private script_status frame_events_register() {
            frame_events_registered = true;
            if (!frame_event_processor.do_before_frame_once(() => {
                System.Threading.Interlocked.Increment(ref frame_events_before);
                frame_events_before_serial = System.Threading.Interlocked.Increment(ref frame_events_serial);
            }) ||
                !frame_event_processor.do_after_frame_once(() => {
                    System.Threading.Interlocked.Increment(ref frame_events_after);
                    frame_events_after_serial = System.Threading.Interlocked.Increment(ref frame_events_serial);
                })) {
                return frame_events_fail("a blocking once task was refused");
            }
            frame_events_repeat_handle =
                frame_event_processor.do_after_frame_repeat(() => System.Threading.Interlocked.Increment(ref frame_events_repeat));
            if (frame_events_repeat_handle == 0 ||
                frame_event_processor.do_after_counted_frame_repeat(
                    2, () => System.Threading.Interlocked.Increment(ref frame_events_counted)) == 0 ||
                frame_event_processor.do_after_frame_repeat(
                    () => {
                        System.Threading.Interlocked.Increment(ref frame_events_nonblocking);
                        // ABI 35: a non-blocking batch runs beside the record pass, so the wrappers
                        // refuse scene calls instead of racing it. At least one refusal proves the
                        // guard is in effect; without it this call would succeed.
                        if (!orbit_camera.try_get(out _)) {
                            System.Threading.Interlocked.Increment(ref frame_events_refusals);
                        }
                    }, is_blocking: false) == 0) {
                return frame_events_fail("a repeat task was refused");
            }
            var doomed = frame_event_processor.do_after_frame_repeat(
                () => System.Threading.Interlocked.Increment(ref frame_events_cancelled));
            if (doomed == 0 || !frame_event_processor.cancel(doomed) || frame_event_processor.cancel(doomed)) {
                return frame_events_fail("cancel did not behave like a one-shot handle");
            }
            if (frame_event_processor.do_after_frame_repeat(null!) != 0 ||
                frame_event_processor.do_after_counted_frame_repeat(0, () => { }) != 0 ||
                frame_event_processor.cancel(ulong.MaxValue)) {
                return frame_events_fail("an invalid registration or cancel was accepted");
            }
            // Registration happens during the first update, so the first boundary pair follows it:
            // counting it here keeps the verify step's "boundaries already answered" honest.
            ++frame_events_boundaries;
            return script_status.success;
        }

        private script_status frame_events_verify() {
            var boundaries = frame_events_boundaries;
            var repeat = Math.Min(boundaries, 3);
            var counted = boundaries / 2;
            var before = System.Threading.Volatile.Read(ref frame_events_before);
            var after = System.Threading.Volatile.Read(ref frame_events_after);
            var repeats = System.Threading.Volatile.Read(ref frame_events_repeat);
            var counted_runs = System.Threading.Volatile.Read(ref frame_events_counted);
            var nonblocking = System.Threading.Volatile.Read(ref frame_events_nonblocking);
            var cancelled = System.Threading.Volatile.Read(ref frame_events_cancelled);
            if (before != 1 || after != 1) {
                return frame_events_fail($"the once tasks ran {before}/{after} times, expected 1/1");
            }
            if (repeats != repeat) {
                return frame_events_fail($"the repeat ran {repeats} times after {boundaries} boundaries, expected {repeat}");
            }
            if (counted_runs != counted) {
                return frame_events_fail($"the counted task ran {counted_runs} times after {boundaries} boundaries, expected {counted}");
            }
            // The dispatcher waits for the previous non-blocking batch before it starts ours, so by
            // the time boundary n is answered, batch n-1 is always done. That is the only guarantee
            // worth asserting: the batch that just started is allowed to still be running.
            if (boundaries > 0 && nonblocking < boundaries - 1) {
                return frame_events_fail($"the non-blocking batch ran {nonblocking} times after {boundaries} boundaries");
            }
            if (cancelled != 0) {
                return frame_events_fail($"a cancelled task ran {cancelled} times");
            }
            var refusals = System.Threading.Volatile.Read(ref frame_events_refusals);
            // Boundary 2 is the first one whose dispatch waited for batch 1, so only from there is
            // the refusal count guaranteed to be settled.
            if (boundaries >= 2 && refusals < 1) {
                return frame_events_fail("the non-blocking task's scene call was not refused");
            }
            if (boundaries > 0 &&
                !(System.Threading.Volatile.Read(ref frame_events_before_serial) > 0 &&
                  System.Threading.Volatile.Read(ref frame_events_before_serial) <
                      System.Threading.Volatile.Read(ref frame_events_after_serial))) {
                return frame_events_fail("the before-frame task did not run before the after-frame one");
            }
            if (boundaries == 3 && !frame_event_processor.cancel(frame_events_repeat_handle)) {
                return frame_events_fail("the repeat handle was no longer cancellable");
            }
            ++frame_events_boundaries;
            return script_status.success;
        }

        private static script_status frame_events_fail(string message) {
            managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes($"Game: frame events: {message}"));
            return script_status.script_failure;
        }

        private void attach(script_base script, node_id node) {
            script.attach(0, node.handle);
            scripts.Add(script);
        }

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (instance == null || frame == null) {
                return script_status.invalid_argument;
            }
            instance.audio_panel?.update(frame->delta_time);
            if (instance.frame_events_selftest) {
                return instance.frame_events_check();
            }
            if (!instance.frame_signal_scheduled) {
                instance.frame_signal_scheduled = true;
                var current = instance;
                frame_scheduler.do_after_frame(() => {
                    current.frame_signal_ran = true;
                    // A probe capture outlives one frame: the scripted capture loop stops as soon as a
                    // screenshot request is consumed, which would end the six-face loop on frame one,
                    // so a running probe defers the screenshot instead.
                    if (!current.probe_active) {
                        current.screenshot_requested = screen_snapshot.try_request();
                    }
                    // The environment is not requested here: the host publishes the project's
                    // `[lighting] environment_hdr` before the first frame (`project_config.apply`), and
                    // bakes the analytic sky itself when a project names none, so a request from this
                    // hook would only re-upload an environment that is already bound.
                    // A non-blocking task runs on its own thread: the host does not wait for it, so
                    // a slow task costs the frame nothing. This one only reports that it ran, which
                    // is what makes the path visible in the log.
                    frame_event_processor.do_after_frame_once(() => {
                        managed_bootstrap.core.write_log("Game: non-blocking frame task ran"u8);
                    }, is_blocking: false);
                });
            } else if (!instance.frame_signal_ran ||
                       (!instance.screenshot_requested && !instance.probe_active)) {
                return script_status.script_failure;
            }
            // ONE LOG LINE, the first frame the clock has advanced: the query side reads the snapshot the
            // host publishes from the same loop that samples the clip, so a non-zero time here is the
            // game's proof that the player it asked for is actually running.
            if (!instance.animation_reported && animation_player.try_get_info(out var anim) && anim.playing && anim.time > 0.0F) {
                instance.animation_reported = true;
                animation_player.try_get_clip_name(anim.current_clip, out var clip_name);
                managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: animation: '{clip_name}' at {anim.time:F2} s of {anim.duration:F2} s"));
            }
            // One probe step per frame: the session owns the six-face cadence (settle, request,
            // collect, next face) so this is all the game side of a capture looks like.
            if (instance.probe != null && !instance.probe.step()) {
                if (instance.probe.failure != null) {
                    managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes(
                        $"Game: reflect probe failed: {instance.probe.failure}"));
                }
                instance.probe = null;
            }
            // The screenshot a capture loop stops on is deferred while the probe runs (the loop ends
            // on the frame that consumes it). Once the cube is written - or the probe gave up - the
            // deferred screenshot is asked for here, so a scripted run still ends the way it always
            // does instead of being reported as a failed frame.
            if (!instance.screenshot_requested && instance.frame_signal_ran && !instance.probe_active) {
                instance.screenshot_requested = screen_snapshot.try_request();
            }
            if (!instance.camera_checked && orbit_camera.try_get(out var pose)) {
                instance.camera_checked = pose.try_apply();
            }
            instance.update_debug_windows(frame->delta_time);
            if (frame->input == null) {
                return script_status.success;
            }
            // The exact length, not the map's size: the native side sends one byte per action slot it
            // has, so a producer/consumer mismatch (a stale map after a hot reload) throws in has_flag
            // instead of reading past the buffer or silently reading a neighbour's byte.
            input_state.begin_frame(action_indices, frame->input, checked((int)frame->input_count));
            try {
                for (var index = 0; index < instance.scripts.Count; ++index) {
                    var script = instance.scripts[index];
                    script.on_update(frame->delta_time);
                }
                return script_status.success;
            } catch (Exception error) {
                managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: update failed: {error.GetBaseException().Message}"));
                return script_status.script_failure;
            } finally {
                input_state.end_frame();
            }
        }

        [UnmanagedCallersOnly]
        private static script_status stop() {
            if (instance == null) {
                return script_status.success;
            }
            var current = instance;
            instance = null;
            try {
                for (var index = current.scripts.Count - 1; index >= 0; --index) {
                    current.scripts[index].on_destroy();
                }
            } catch (Exception error) {
                managed_bootstrap.core.write_log(System.Text.Encoding.UTF8.GetBytes(
                    $"Game: shutdown failed: {error.GetBaseException().Message}"));
                current.cleanup();
                return script_status.script_failure;
            }
            current.cleanup();
            return script_status.success;
        }

        private void cleanup() {
            audio_panel?.Dispose();
            audio_panel = null;
            debug_windows?.Dispose();
            debug_windows = null;
            scripts.Clear();
            if (tree != null) {
                tree.detach();
                tree.destroy();
                tree = null;
            }
        }
    }
}
