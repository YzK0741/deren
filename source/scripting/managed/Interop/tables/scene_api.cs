using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// One punctual light across the ABI: the flat mirror of deren::scripting::point_light_desc.
    /// The angles are already cosines here, because that is what the light UBO stores; the managed
    /// wrapper in Api/point_light.cs takes degrees and does the conversion.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct point_light_desc {
        public fixed float position[3];
        public fixed float direction[3];
        public fixed float colour[3];
        public float intensity;
        public float range;
        public float outer_cos;
        public float inner_cos;
        public float npr_mode;
        public fixed float npr_data[4];
        public byte spot;
        public byte has_inner_cos;
        public fixed byte padding[2];
    }
    /// <summary>
    /// One node subtree's per-object material factors: the flat mirror of
    /// deren::scripting::material_override_desc. Enabling it makes the host clone the imported
    /// material once and rebuild that clone from the factors; disabling restores the import.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct material_override_desc {
        public fixed float tint[3];
        public float metallic_scale;
        public float roughness_scale;
        public float emissive_scale;
        public uint environment_slot;
        public byte unlit;
        public byte enabled;
        public fixed byte padding[2];
    }

    internal static unsafe class scene_api {
        /// <summary>ABI 29: the face frame the loader measured for a loaded asset (72 bytes).</summary>
        [StructLayout(LayoutKind.Sequential)]
        internal struct face_frame_desc {
            public fixed float forward[3];
            public fixed float right[3];
            public fixed float world_forward[3];
            public fixed float world_right[3];
            public fixed float world_center[3];
            public uint head_joint;
            public uint skin_index;
            public byte head_relative;
            public byte valid;
            public fixed byte padding[2];
        }

        /// <summary>ABI 29: one character slot head frame (52 bytes; the two flags become the vec4 `.w`s).</summary>
        [StructLayout(LayoutKind.Sequential)]
        internal struct head_basis_desc {
            public fixed float front[3];
            public fixed float right[3];
            public fixed float up[3];
            public fixed float center[3];
            public byte captured;
            public byte has_center;
            public fixed byte padding[2];
        }

        /// <summary>
        /// ABI 39: the toon light rig (160 bytes): the lane-for-lane mirror of deren::scripting::toon_rig_desc,
        /// which is itself the mirror of the engine's deren::engine::toon_rig. Every lane is a float4 the
        /// character stage reads; the managed-friendly names live in Api/scene/toon_rig.cs.
        /// </summary>
        [StructLayout(LayoutKind.Sequential)]
        internal struct toon_rig_desc {
            public fixed float day[4];
            public fixed float other_light[4];
            public fixed float other_colour[4];
            public fixed float main_dark[4];
            public fixed float rim[4];
            public fixed float sss[4];
            public fixed float env[4];
            public fixed float misc[4];
            public fixed float shadow_softness[4];
            public fixed float reference_modes[4];
        }

        /// <summary>
        /// ABI 38: the frame's shape - the extent the passes shade, the extent presentation shows, and the
        /// size of the GPU timing table. The mirror of deren::scripting::diagnostic_counters (24 bytes).
        /// </summary>
        [StructLayout(LayoutKind.Sequential)]
        internal struct diagnostic_counters {
            public uint render_width;
            public uint render_height;
            public uint output_width;
            public uint output_height;
            public uint gpu_interval_count;
            public byte gpu_timings_available;
            public fixed byte padding[3];
        }


        private static delegate* unmanaged<node_handle, transform_value*, script_status> get_transform;
        private static delegate* unmanaged<node_handle, transform_value*, script_status> set_transform;
        private static delegate* unmanaged<ulong, utf8_view, transform_value*, node_handle*, script_status> try_spawn;
        private static delegate* unmanaged<ulong, node_handle, script_status> try_despawn;
        private static delegate* unmanaged<ulong*, script_status> create_scene_tree;
        private static delegate* unmanaged<ulong, script_status> activate_scene;
        private static delegate* unmanaged<ulong*, script_status> get_active_scene;
        private static delegate* unmanaged<ulong, script_status> deactivate_scene;
        private static delegate* unmanaged<ulong, script_status> destroy_scene_tree;
        private static delegate* unmanaged<ulong, node_handle, utf8_view, transform_value*, node_handle*, script_status> create_node;
        private static delegate* unmanaged<ulong, node_handle, script_status> destroy_node;
        private static delegate* unmanaged<ulong, node_handle, transform_value*, script_status> get_node_transform;
        private static delegate* unmanaged<ulong, node_handle, transform_value*, script_status> set_node_transform;
        private static delegate* unmanaged<ulong, node_handle, byte, script_status> set_node_visible;
        private static delegate* unmanaged<uint, utf8_view*, script_status> list_asset;
        private static delegate* unmanaged<utf8_view, ulong*, script_status> load_asset;
        private static delegate* unmanaged<ulong, script_status> release_asset;
        private static delegate* unmanaged<ulong, node_handle, ulong, transform_value*, node_handle*, script_status> bind_asset;
        private static delegate* unmanaged<orbit_camera*, script_status> get_camera;
        private static delegate* unmanaged<orbit_camera*, script_status> set_camera;
        // Optional like the camera entries: a host that predates this pointer still works, and the
        // wrapper reports module_unavailable instead of dereferencing null. ABI 32 made it slot-aware:
        // 0 is the scene-wide environment, 1..3 are the per-material lanes.
        private static delegate* unmanaged<utf8_view, uint, script_status> set_environment_hdr;
        // Optional too: the per-asset pipeline queries arrived with ABI 23.
        private static delegate* unmanaged<ulong, uint*, script_status> asset_pipeline_count;
        private static delegate* unmanaged<ulong, uint, utf8_view*, script_status> asset_pipeline;
        private static delegate* unmanaged<ulong, utf8_view*, script_status> asset_default_pipeline;
        private static delegate* unmanaged<ulong, node_handle, utf8_view, script_status> set_pipeline;
        // Optional too: the runtime post-processing knobs arrived with ABI 24.
        private static delegate* unmanaged<float, script_status> set_exposure;
        private static delegate* unmanaged<int, script_status> set_tone_mapping;
        private static delegate* unmanaged<float, float, int, float, float, float, float, script_status> set_environment;
        private static delegate* unmanaged<float, script_status> set_sun_intensity;
        private static delegate* unmanaged<float, float, script_status> set_bloom;
        private static delegate* unmanaged<byte, float, float, script_status> set_fxaa;
        private static delegate* unmanaged<byte, float, float, script_status> set_taa;
        private static delegate* unmanaged<byte, float, float, uint, script_status> set_ssao;
        private static delegate* unmanaged<byte, script_status> set_gbuffer_debug;
        // Optional too: the runtime lighting knobs arrived with ABI 25.
        private static delegate* unmanaged<float, float, float, script_status> set_sun_direction;
        private static delegate* unmanaged<float, float, float, float, byte, float, float, float, float, script_status> set_area_light;
        private static delegate* unmanaged<byte, script_status> set_clustered_lights;
        // Optional too: the punctual light set arrived with ABI 26.
        private static delegate* unmanaged<point_light_desc*, uint, script_status> set_point_lights;
        private static delegate* unmanaged<ulong, node_handle, material_override_desc*, script_status> set_material_override;
        private static delegate* unmanaged<double, script_status> set_max_fps;
        private static delegate* unmanaged<float, float, script_status> set_toon_shading;
        private static delegate* unmanaged<byte, script_status> set_shadow_enabled;
        // Optional too: the head frame contract arrived with ABI 29.
        private static delegate* unmanaged<ulong, face_frame_desc*, script_status> asset_face_frame;
        private static delegate* unmanaged<uint, head_basis_desc*, script_status> set_head_basis;
        private static delegate* unmanaged<byte, float*, uint, float, float, script_status> set_capture_camera;
        private static delegate* unmanaged<script_status> request_radiance_capture;
        private static delegate* unmanaged<uint*, uint*, float*, uint, script_status> acquire_radiance_image;
        private static delegate* unmanaged<script_status> reset_temporal_history;
        private static delegate* unmanaged<utf8_view, uint, float*, uint, script_status> write_radiance_cube;
        // Optional too: the animation player arrived with ABI 36.
        private static delegate* unmanaged<animation_state*, script_status> get_animation_state;
        private static delegate* unmanaged<uint*, script_status> animation_playable_count;
        private static delegate* unmanaged<uint, utf8_view*, script_status> animation_playable_name;
        private static delegate* unmanaged<uint, script_status> select_animation;
        private static delegate* unmanaged<byte, script_status> set_animation_playing;
        private static delegate* unmanaged<float, script_status> set_animation_speed;
        private static delegate* unmanaged<float, script_status> set_animation_time;
        // Optional too: the scene-graph/screen/diagnostics batch arrived with ABI 38.
        private static delegate* unmanaged<ulong, node_handle, uint*, script_status> node_child_count;
        private static delegate* unmanaged<ulong, node_handle, uint, node_handle*, script_status> node_child;
        private static delegate* unmanaged<ulong, node_handle, utf8_view*, script_status> node_name;
        private static delegate* unmanaged<ulong, node_handle, uint*, script_status> get_node_flags;
        private static delegate* unmanaged<ulong, node_handle, uint, uint, script_status> set_node_flags;
        private static delegate* unmanaged<ulong, node_handle, node_handle, byte, script_status> reparent_node;
        private static delegate* unmanaged<uint*, uint*, script_status> viewport_size;
        private static delegate* unmanaged<float*, float*, script_status> project_point;
        private static delegate* unmanaged<float, float, float*, float*, script_status> screen_ray;
        private static delegate* unmanaged<diagnostic_counters*, script_status> query_counters;
        private static delegate* unmanaged<utf8_view*, script_status> gpu_timing_summary;
        // ABI 39: the toon light rig. Both are optional for the same reason as the batch above.
        private static delegate* unmanaged<scene_api.toon_rig_desc*, script_status> set_toon_rig;
        private static delegate* unmanaged<scene_api.toon_rig_desc*, script_status> get_toon_rig;
        // ABI 40: renaming a node; optional like the entries above.
        private static delegate* unmanaged<ulong, node_handle, utf8_view, script_status> set_node_name;

        internal static bool is_ready() {
            if (!host_call_guard.host_calls_allowed) {
                // A non-blocking batch must not touch host state the record pass is using; the
                // executor logs the count at the end of the batch.
                host_call_guard.note_refusal();
                return false;
            }
            return get_transform != null && set_transform != null && try_spawn != null && try_despawn != null &&
                   create_scene_tree != null && activate_scene != null && get_active_scene != null &&
                   deactivate_scene != null && destroy_scene_tree != null && create_node != null && destroy_node != null &&
                   get_node_transform != null && set_node_transform != null && set_node_visible != null &&
                   list_asset != null && load_asset != null && release_asset != null && bind_asset != null;
        }

        internal static script_status initialize(native_api* api) {
            get_transform = null;
            set_transform = null;
            try_spawn = null;
            try_despawn = null;
            create_scene_tree = null;
            activate_scene = null;
            get_active_scene = null;
            deactivate_scene = null;
            destroy_scene_tree = null;
            create_node = null;
            destroy_node = null;
            get_node_transform = null;
            set_node_transform = null;
            set_node_visible = null;
            list_asset = null;
            load_asset = null;
            release_asset = null;
            bind_asset = null;
            get_camera = null;
            set_camera = null;
            set_environment_hdr = null;
            asset_pipeline_count = null;
            asset_pipeline = null;
            asset_default_pipeline = null;
            set_pipeline = null;
            set_exposure = null;
            set_tone_mapping = null;
            set_environment = null;
            set_sun_intensity = null;
            set_bloom = null;
            set_fxaa = null;
            set_taa = null;
            set_ssao = null;
            set_gbuffer_debug = null;
            set_sun_direction = null;
            set_area_light = null;
            set_clustered_lights = null;
            set_point_lights = null;
            set_material_override = null;
            set_max_fps = null;
            set_toon_shading = null;
            set_shadow_enabled = null;
            asset_face_frame = null;
            set_head_basis = null;
            set_capture_camera = null;
            request_radiance_capture = null;
            acquire_radiance_image = null;
            reset_temporal_history = null;
            write_radiance_cube = null;
            get_animation_state = null;
            animation_playable_count = null;
            animation_playable_name = null;
            select_animation = null;
            set_animation_playing = null;
            set_animation_speed = null;
            set_animation_time = null;
            node_child_count = null;
            node_child = null;
            node_name = null;
            get_node_flags = null;
            set_node_flags = null;
            reparent_node = null;
            viewport_size = null;
            project_point = null;
            screen_ray = null;
            query_counters = null;
            gpu_timing_summary = null;
            set_toon_rig = null;
            get_toon_rig = null;
            set_node_name = null;

            void* table = null;
            uint byte_size = 0;
            var status = api->get_module((uint)native_module.scene, &table, &byte_size);
            if (status != script_status.success) {
                return status;
            }
            if (table == null) {
                return script_status.module_invalid;
            }
            // ABI 33: the host reports the table size, so a field count that drifted between the two
            // sides is a refused binding instead of a call through a pointer read past the table.
            if (byte_size != sizeof(scene_api_layout)) {
                host_logger.write(api, 
                    $"Managed host: the scene module table is {byte_size} bytes, this build expects " +
                    $"{sizeof(scene_api_layout)}.");
                return script_status.module_invalid;
            }
            var resolved = *(scene_api_layout*)table;
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            get_transform = resolved.get_transform;
            set_transform = resolved.set_transform;
            try_spawn = resolved.try_spawn;
            try_despawn = resolved.try_despawn;
            create_scene_tree = resolved.create_scene_tree;
            activate_scene = resolved.activate_scene;
            get_active_scene = resolved.get_active_scene;
            deactivate_scene = resolved.deactivate_scene;
            destroy_scene_tree = resolved.destroy_scene_tree;
            create_node = resolved.create_node;
            destroy_node = resolved.destroy_node;
            get_node_transform = resolved.get_node_transform;
            set_node_transform = resolved.set_node_transform;
            set_node_visible = resolved.set_node_visible;
            list_asset = resolved.list_asset;
            load_asset = resolved.load_asset;
            release_asset = resolved.release_asset;
            bind_asset = resolved.bind_asset;
            get_camera = resolved.get_camera;
            set_camera = resolved.set_camera;
            set_environment_hdr = resolved.set_environment_hdr;
            asset_pipeline_count = resolved.asset_pipeline_count;
            asset_pipeline = resolved.asset_pipeline;
            asset_default_pipeline = resolved.asset_default_pipeline;
            set_pipeline = resolved.set_pipeline;
            set_exposure = resolved.set_exposure;
            set_tone_mapping = resolved.set_tone_mapping;
            set_environment = resolved.set_environment;
            set_sun_intensity = resolved.set_sun_intensity;
            set_bloom = resolved.set_bloom;
            set_fxaa = resolved.set_fxaa;
            set_taa = resolved.set_taa;
            set_ssao = resolved.set_ssao;
            set_gbuffer_debug = resolved.set_gbuffer_debug;
            set_sun_direction = resolved.set_sun_direction;
            set_area_light = resolved.set_area_light;
            set_clustered_lights = resolved.set_clustered_lights;
            set_point_lights = resolved.set_point_lights;
            set_material_override = resolved.set_material_override;
            set_max_fps = resolved.set_max_fps;
            set_toon_shading = resolved.set_toon_shading;
            set_shadow_enabled = resolved.set_shadow_enabled;
            asset_face_frame = resolved.asset_face_frame;
            set_head_basis = resolved.set_head_basis;
            set_capture_camera = resolved.set_capture_camera;
            request_radiance_capture = resolved.request_radiance_capture;
            acquire_radiance_image = resolved.acquire_radiance_image;
            reset_temporal_history = resolved.reset_temporal_history;
            write_radiance_cube = resolved.write_radiance_cube;
            get_animation_state = resolved.get_animation_state;
            animation_playable_count = resolved.animation_playable_count;
            animation_playable_name = resolved.animation_playable_name;
            select_animation = resolved.select_animation;
            set_animation_playing = resolved.set_animation_playing;
            set_animation_speed = resolved.set_animation_speed;
            set_animation_time = resolved.set_animation_time;
            node_child_count = resolved.node_child_count;
            node_child = resolved.node_child;
            node_name = resolved.node_name;
            get_node_flags = resolved.get_node_flags;
            set_node_flags = resolved.set_node_flags;
            reparent_node = resolved.reparent_node;
            viewport_size = resolved.viewport_size;
            project_point = resolved.project_point;
            screen_ray = resolved.screen_ray;
            query_counters = resolved.query_counters;
            gpu_timing_summary = resolved.gpu_timing_summary;
            set_toon_rig = resolved.set_toon_rig;
            get_toon_rig = resolved.get_toon_rig;
            set_node_name = resolved.set_node_name;
            return script_status.success;
        }

        internal static script_status query_transform(node_handle node, transform_value* value) {
            return is_ready() ? get_transform(node, value) : script_status.module_unavailable;
        }

        internal static script_status assign_transform(node_handle node, transform_value* value) {
            return is_ready() ? set_transform(node, value) : script_status.module_unavailable;
        }

        internal static script_status spawn(ulong scene, utf8_view asset, transform_value* value, node_handle* node) {
            return is_ready() ? try_spawn(scene, asset, value, node) : script_status.module_unavailable;
        }

        internal static script_status despawn(ulong scene, node_handle node) {
            return is_ready() ? try_despawn(scene, node) : script_status.module_unavailable;
        }

        internal static script_status create_tree(ulong* tree) {
            return is_ready() ? create_scene_tree(tree) : script_status.module_unavailable;
        }

        internal static script_status activate(ulong tree) {
            return is_ready() ? activate_scene(tree) : script_status.module_unavailable;
        }

        internal static script_status active_tree(ulong* tree) {
            return is_ready() ? get_active_scene(tree) : script_status.module_unavailable;
        }

        internal static script_status deactivate(ulong tree) {
            return is_ready() ? deactivate_scene(tree) : script_status.module_unavailable;
        }

        internal static script_status destroy_tree(ulong tree) {
            return is_ready() ? destroy_scene_tree(tree) : script_status.module_unavailable;
        }

        internal static script_status create_tree_node(ulong tree, node_handle parent, utf8_view name,
                                                       transform_value* local, node_handle* node) {
            return is_ready() ? create_node(tree, parent, name, local, node) : script_status.module_unavailable;
        }

        internal static script_status destroy_tree_node(ulong tree, node_handle node) {
            return is_ready() ? destroy_node(tree, node) : script_status.module_unavailable;
        }

        internal static script_status query_tree_transform(ulong tree, node_handle node, transform_value* value) {
            return is_ready() ? get_node_transform(tree, node, value) : script_status.module_unavailable;
        }

        internal static script_status assign_tree_transform(ulong tree, node_handle node, transform_value* value) {
            return is_ready() ? set_node_transform(tree, node, value) : script_status.module_unavailable;
        }

        internal static script_status assign_tree_visible(ulong tree, node_handle node, bool visible) {
            return is_ready() ? set_node_visible(tree, node, visible ? (byte)1 : (byte)0) : script_status.module_unavailable;
        }

        internal static script_status list_scene_asset(uint index, utf8_view* id) {
            return is_ready() ? list_asset(index, id) : script_status.module_unavailable;
        }

        internal static script_status load_scene_asset(utf8_view id, ulong* asset) {
            return is_ready() ? load_asset(id, asset) : script_status.module_unavailable;
        }

        internal static script_status release_scene_asset(ulong asset) {
            return is_ready() ? release_asset(asset) : script_status.module_unavailable;
        }

        internal static script_status bind_tree_asset(ulong tree, node_handle parent, ulong asset,
                                                       transform_value* offset, node_handle* root) {
            return is_ready() ? bind_asset(tree, parent, asset, offset, root) : script_status.module_unavailable;
        }

        internal static script_status query_camera(orbit_camera* value) {
            return is_ready() && get_camera != null ? get_camera(value) : script_status.module_unavailable;
        }

        internal static script_status assign_camera(orbit_camera* value) {
            return is_ready() && set_camera != null ? set_camera(value) : script_status.module_unavailable;
        }

        internal static script_status assign_environment_hdr(utf8_view path, uint environment_slot) {
            return is_ready() && set_environment_hdr != null
                ? set_environment_hdr(path, environment_slot)
                : script_status.module_unavailable;
        }

        internal static script_status query_asset_pipeline_count(ulong asset, uint* count) {
            return is_ready() && asset_pipeline_count != null
                ? asset_pipeline_count(asset, count)
                : script_status.module_unavailable;
        }

        internal static script_status query_asset_pipeline(ulong asset, uint index, utf8_view* name) {
            return is_ready() && asset_pipeline != null
                ? asset_pipeline(asset, index, name)
                : script_status.module_unavailable;
        }

        internal static script_status query_asset_default_pipeline(ulong asset, utf8_view* name) {
            return is_ready() && asset_default_pipeline != null
                ? asset_default_pipeline(asset, name)
                : script_status.module_unavailable;
        }

        internal static script_status assign_pipeline(ulong tree, node_handle node, utf8_view pipeline) {
            return is_ready() && set_pipeline != null
                ? set_pipeline(tree, node, pipeline)
                : script_status.module_unavailable;
        }

        internal static script_status assign_exposure(float exposure) {
            return is_ready() && set_exposure != null ? set_exposure(exposure) : script_status.module_unavailable;
        }

        internal static script_status assign_tone_mapping(int mode) {
            return is_ready() && set_tone_mapping != null ? set_tone_mapping(mode) : script_status.module_unavailable;
        }

        internal static script_status assign_environment(float rotation_degrees, float strength, int background,
                                                         float color_r, float color_g, float color_b, float blur) {
            return is_ready() && set_environment != null
                ? set_environment(rotation_degrees, strength, background, color_r, color_g, color_b, blur)
                : script_status.module_unavailable;
        }

        internal static script_status assign_sun_intensity(float scale) {
            return is_ready() && set_sun_intensity != null
                ? set_sun_intensity(scale)
                : script_status.module_unavailable;
        }

        internal static script_status assign_bloom(float intensity, float threshold) {
            return is_ready() && set_bloom != null ? set_bloom(intensity, threshold) : script_status.module_unavailable;
        }

        internal static script_status assign_fxaa(bool enabled, float subpixel, float edge_threshold) {
            return is_ready() && set_fxaa != null
                ? set_fxaa(enabled ? (byte)1 : (byte)0, subpixel, edge_threshold)
                : script_status.module_unavailable;
        }

        internal static script_status assign_taa(bool enabled, float blend_static, float blend_min) {
            return is_ready() && set_taa != null
                ? set_taa(enabled ? (byte)1 : (byte)0, blend_static, blend_min)
                : script_status.module_unavailable;
        }

        internal static script_status assign_ssao(bool enabled, float radius, float intensity, uint samples) {
            return is_ready() && set_ssao != null
                ? set_ssao(enabled ? (byte)1 : (byte)0, radius, intensity, samples)
                : script_status.module_unavailable;
        }

        internal static script_status assign_gbuffer_debug(bool enabled) {
            return is_ready() && set_gbuffer_debug != null
                ? set_gbuffer_debug(enabled ? (byte)1 : (byte)0)
                : script_status.module_unavailable;
        }

        internal static script_status assign_sun_direction(float x, float y, float z) {
            return is_ready() && set_sun_direction != null
                ? set_sun_direction(x, y, z)
                : script_status.module_unavailable;
        }

        internal static script_status assign_area_light(float centre_x, float centre_y, float centre_z, float half,
                                                        bool irradiance, float axis_x, float axis_y, float axis_z,
                                                        float penumbra) {
            return is_ready() && set_area_light != null
                ? set_area_light(centre_x, centre_y, centre_z, half, irradiance ? (byte)1 : (byte)0,
                                 axis_x, axis_y, axis_z, penumbra)
                : script_status.module_unavailable;
        }

        internal static script_status assign_clustered_lights(bool enabled) {
            return is_ready() && set_clustered_lights != null
                ? set_clustered_lights(enabled ? (byte)1 : (byte)0)
                : script_status.module_unavailable;
        }

        internal static script_status assign_point_lights(point_light_desc* lights, uint count) {
            return is_ready() && set_point_lights != null
                ? set_point_lights(lights, count)
                : script_status.module_unavailable;
        }

        internal static script_status assign_material_override(ulong tree, node_handle node,
                                                               material_override_desc* value) {
            return is_ready() && set_material_override != null
                ? set_material_override(tree, node, value)
                : script_status.module_unavailable;
        }
        internal static script_status assign_max_fps(double fps) {
            return is_ready() && set_max_fps != null
                ? set_max_fps(fps)
                : script_status.module_unavailable;
        }

        internal static script_status assign_toon_shading(float steps, float softness) {
            return is_ready() && set_toon_shading != null
                ? set_toon_shading(steps, softness)
                : script_status.module_unavailable;
        }

        internal static script_status assign_shadow_enabled(bool enabled) {
            return is_ready() && set_shadow_enabled != null
                ? set_shadow_enabled(enabled ? (byte)1 : (byte)0)
                : script_status.module_unavailable;
        }

        internal static script_status query_face_frame(ulong asset, face_frame_desc* value) {
            return is_ready() && asset_face_frame != null
                ? asset_face_frame(asset, value)
                : script_status.module_unavailable;
        }

        internal static script_status assign_head_basis(uint index, head_basis_desc* value) {
            return is_ready() && set_head_basis != null
                ? set_head_basis(index, value)
                : script_status.module_unavailable;
        }

        internal static script_status assign_capture_camera(byte enabled, float* position, uint face, float near_plane,
                                                          float far_plane) {
            return is_ready() && set_capture_camera != null
                ? set_capture_camera(enabled, position, face, near_plane, far_plane)
                : script_status.module_unavailable;
        }

        internal static script_status assign_radiance_request() {
            return is_ready() && request_radiance_capture != null
                ? request_radiance_capture()
                : script_status.module_unavailable;
        }

        internal static script_status read_radiance_image(uint* width, uint* height, float* rgba, uint float_count) {
            return is_ready() && acquire_radiance_image != null
                ? acquire_radiance_image(width, height, rgba, float_count)
                : script_status.module_unavailable;
        }

        internal static script_status assign_temporal_reset() {
            return is_ready() && reset_temporal_history != null
                ? reset_temporal_history()
                : script_status.module_unavailable;
        }

        internal static script_status publish_radiance_cube(utf8_view path, uint size, float* pixels, uint float_count) {
            return is_ready() && write_radiance_cube != null
                ? write_radiance_cube(path, size, pixels, float_count)
                : script_status.module_unavailable;
        }

        internal static script_status query_animation_state(animation_state* value) {
            return is_ready() && get_animation_state != null
                ? get_animation_state(value)
                : script_status.module_unavailable;
        }

        internal static script_status query_animation_playable_count(uint* count) {
            return is_ready() && animation_playable_count != null
                ? animation_playable_count(count)
                : script_status.module_unavailable;
        }

        internal static script_status query_animation_playable_name(uint index, utf8_view* name) {
            return is_ready() && animation_playable_name != null
                ? animation_playable_name(index, name)
                : script_status.module_unavailable;
        }

        internal static script_status assign_animation_select(uint index) {
            return is_ready() && select_animation != null
                ? select_animation(index)
                : script_status.module_unavailable;
        }

        internal static script_status assign_animation_playing(byte playing) {
            return is_ready() && set_animation_playing != null
                ? set_animation_playing(playing)
                : script_status.module_unavailable;
        }

        internal static script_status assign_animation_speed(float speed) {
            return is_ready() && set_animation_speed != null
                ? set_animation_speed(speed)
                : script_status.module_unavailable;
        }

        internal static script_status assign_animation_time(float seconds) {
            return is_ready() && set_animation_time != null
                ? set_animation_time(seconds)
                : script_status.module_unavailable;
        }

        // ---- ABI 38: the scene graph read back, world <-> screen, and diagnostics ---------------------

        internal static script_status query_child_count(ulong tree, node_handle node, uint* count) {
            return is_ready() && node_child_count != null
                ? node_child_count(tree, node, count)
                : script_status.module_unavailable;
        }

        internal static script_status query_child(ulong tree, node_handle node, uint index, node_handle* child) {
            return is_ready() && node_child != null
                ? node_child(tree, node, index, child)
                : script_status.module_unavailable;
        }

        internal static script_status query_node_name(ulong tree, node_handle node, utf8_view* name) {
            return is_ready() && node_name != null
                ? node_name(tree, node, name)
                : script_status.module_unavailable;
        }

        internal static script_status query_node_flags(ulong tree, node_handle node, uint* flags) {
            return is_ready() && get_node_flags != null
                ? get_node_flags(tree, node, flags)
                : script_status.module_unavailable;
        }

        internal static script_status assign_node_flags(ulong tree, node_handle node, uint flags, uint mask) {
            return is_ready() && set_node_flags != null
                ? set_node_flags(tree, node, flags, mask)
                : script_status.module_unavailable;
        }

        internal static script_status assign_parent(ulong tree, node_handle node, node_handle parent, byte keep_transform) {
            return is_ready() && reparent_node != null
                ? reparent_node(tree, node, parent, keep_transform)
                : script_status.module_unavailable;
        }

        internal static script_status query_viewport_size(uint* width, uint* height) {
            return is_ready() && viewport_size != null
                ? viewport_size(width, height)
                : script_status.module_unavailable;
        }

        internal static script_status project_world_point(float* world, float* screen) {
            return is_ready() && project_point != null
                ? project_point(world, screen)
                : script_status.module_unavailable;
        }

        internal static script_status query_screen_ray(float x, float y, float* origin, float* direction) {
            return is_ready() && screen_ray != null
                ? screen_ray(x, y, origin, direction)
                : script_status.module_unavailable;
        }

        internal static script_status query_diagnostic_counters(diagnostic_counters* counters) {
            return is_ready() && query_counters != null
                ? query_counters(counters)
                : script_status.module_unavailable;
        }

        internal static script_status query_gpu_timing_text(utf8_view* text) {
            return is_ready() && gpu_timing_summary != null
                ? gpu_timing_summary(text)
                : script_status.module_unavailable;
        }

        internal static script_status assign_toon_rig(scene_api.toon_rig_desc* value) {
            return is_ready() && set_toon_rig != null
                ? set_toon_rig(value)
                : script_status.module_unavailable;
        }

        internal static script_status read_toon_rig(scene_api.toon_rig_desc* value) {
            return is_ready() && get_toon_rig != null
                ? get_toon_rig(value)
                : script_status.module_unavailable;
        }

        // ---- ABI 40: renaming a node ----------------------------------------------------------------

        internal static script_status assign_node_name(ulong tree, node_handle node, utf8_view name) {
            return is_ready() && set_node_name != null
                ? set_node_name(tree, node, name)
                : script_status.module_unavailable;
        }
    }

    /// <summary>
    /// The scene module table as the host publishes it: the flat mirror of
    /// deren::scripting::scene_api_layout. The host blits it through the module table pointer, so
    /// the fields are positional; managed code only reads them and never assigns them.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct scene_api_layout {
        public delegate* unmanaged<node_handle, transform_value*, script_status> get_transform;
        public delegate* unmanaged<node_handle, transform_value*, script_status> set_transform;
        public delegate* unmanaged<ulong, utf8_view, transform_value*, node_handle*, script_status> try_spawn;
        public delegate* unmanaged<ulong, node_handle, script_status> try_despawn;
        public delegate* unmanaged<ulong*, script_status> create_scene_tree;
        public delegate* unmanaged<ulong, script_status> activate_scene;
        public delegate* unmanaged<ulong*, script_status> get_active_scene;
        public delegate* unmanaged<ulong, script_status> deactivate_scene;
        public delegate* unmanaged<ulong, script_status> destroy_scene_tree;
        public delegate* unmanaged<ulong, node_handle, utf8_view, transform_value*, node_handle*, script_status> create_node;
        public delegate* unmanaged<ulong, node_handle, script_status> destroy_node;
        public delegate* unmanaged<ulong, node_handle, transform_value*, script_status> get_node_transform;
        public delegate* unmanaged<ulong, node_handle, transform_value*, script_status> set_node_transform;
        public delegate* unmanaged<ulong, node_handle, byte, script_status> set_node_visible;
        public delegate* unmanaged<uint, utf8_view*, script_status> list_asset;
        public delegate* unmanaged<utf8_view, ulong*, script_status> load_asset;
        public delegate* unmanaged<ulong, script_status> release_asset;
        public delegate* unmanaged<ulong, node_handle, ulong, transform_value*, node_handle*, script_status> bind_asset;
        public delegate* unmanaged<orbit_camera*, script_status> get_camera;
        public delegate* unmanaged<orbit_camera*, script_status> set_camera;
        public delegate* unmanaged<utf8_view, uint, script_status> set_environment_hdr;
        public delegate* unmanaged<ulong, uint*, script_status> asset_pipeline_count;
        public delegate* unmanaged<ulong, uint, utf8_view*, script_status> asset_pipeline;
        public delegate* unmanaged<ulong, utf8_view*, script_status> asset_default_pipeline;
        public delegate* unmanaged<ulong, node_handle, utf8_view, script_status> set_pipeline;
        public delegate* unmanaged<float, script_status> set_exposure;
        public delegate* unmanaged<int, script_status> set_tone_mapping;
        public delegate* unmanaged<float, float, int, float, float, float, float, script_status> set_environment;
        public delegate* unmanaged<float, script_status> set_sun_intensity;
        public delegate* unmanaged<float, float, script_status> set_bloom;
        public delegate* unmanaged<byte, float, float, script_status> set_fxaa;
        public delegate* unmanaged<byte, float, float, script_status> set_taa;
        public delegate* unmanaged<byte, float, float, uint, script_status> set_ssao;
        public delegate* unmanaged<byte, script_status> set_gbuffer_debug;
        public delegate* unmanaged<float, float, float, script_status> set_sun_direction;
        public delegate* unmanaged<float, float, float, float, byte, float, float, float, float, script_status> set_area_light;
        public delegate* unmanaged<byte, script_status> set_clustered_lights;
        public delegate* unmanaged<point_light_desc*, uint, script_status> set_point_lights;
        public delegate* unmanaged<ulong, node_handle, material_override_desc*, script_status> set_material_override;
        public delegate* unmanaged<double, script_status> set_max_fps;
        public delegate* unmanaged<float, float, script_status> set_toon_shading;
        public delegate* unmanaged<byte, script_status> set_shadow_enabled;
        // ABI 29: the face frame the loader measured, and the head frame derived from it.
        public delegate* unmanaged<ulong, scene_api.face_frame_desc*, script_status> asset_face_frame;
        public delegate* unmanaged<uint, scene_api.head_basis_desc*, script_status> set_head_basis;
        // ABI 30: the offscreen cube camera and the radiance readback behind the reflect probe.
        // The host installs the camera and runs the pass; the face loop, the cadence, and the
        // output file belong to the game.
        public delegate* unmanaged<byte, float*, uint, float, float, script_status> set_capture_camera;
        public delegate* unmanaged<script_status> request_radiance_capture;
        public delegate* unmanaged<uint*, uint*, float*, uint, script_status> acquire_radiance_image;
        public delegate* unmanaged<script_status> reset_temporal_history;
        public delegate* unmanaged<utf8_view, uint, float*, uint, script_status> write_radiance_cube;
        // ABI 36: the host's character animation player. The three queries read the per-frame
        // snapshot; the commands only queue work for the frame thread.
        public delegate* unmanaged<animation_state*, script_status> get_animation_state;
        public delegate* unmanaged<uint*, script_status> animation_playable_count;
        public delegate* unmanaged<uint, utf8_view*, script_status> animation_playable_name;
        public delegate* unmanaged<uint, script_status> select_animation;
        public delegate* unmanaged<byte, script_status> set_animation_playing;
        public delegate* unmanaged<float, script_status> set_animation_speed;
        public delegate* unmanaged<float, script_status> set_animation_time;
        // ABI 38: scene-graph queries, world/screen conversion, and diagnostics. Optional like the
        // entries above: a host that predates them leaves the pointers null and every wrapper reports
        // module_unavailable instead of calling through one.
        public delegate* unmanaged<ulong, node_handle, uint*, script_status> node_child_count;
        public delegate* unmanaged<ulong, node_handle, uint, node_handle*, script_status> node_child;
        public delegate* unmanaged<ulong, node_handle, utf8_view*, script_status> node_name;
        public delegate* unmanaged<ulong, node_handle, uint*, script_status> get_node_flags;
        public delegate* unmanaged<ulong, node_handle, uint, uint, script_status> set_node_flags;
        public delegate* unmanaged<ulong, node_handle, node_handle, byte, script_status> reparent_node;
        public delegate* unmanaged<uint*, uint*, script_status> viewport_size;
        public delegate* unmanaged<float*, float*, script_status> project_point;
        public delegate* unmanaged<float, float, float*, float*, script_status> screen_ray;
        public delegate* unmanaged<scene_api.diagnostic_counters*, script_status> query_counters;
        public delegate* unmanaged<utf8_view*, script_status> gpu_timing_summary;
        // ABI 39: the toon light rig, queued by the host and applied between frames; get answers what the
        // next frame will use (the pending value while one is queued, otherwise the published rig).
        public delegate* unmanaged<scene_api.toon_rig_desc*, script_status> set_toon_rig;
        public delegate* unmanaged<scene_api.toon_rig_desc*, script_status> get_toon_rig;
        // ABI 40: renaming a node. Optional like the batch above: a host that predates it leaves the
        // pointer null and assign_node_name() reports module_unavailable instead of calling through it.
        public delegate* unmanaged<ulong, node_handle, utf8_view, script_status> set_node_name;

        public readonly bool is_valid() {
            return get_transform != null && set_transform != null && try_spawn != null && try_despawn != null &&
                   create_scene_tree != null && activate_scene != null && get_active_scene != null &&
                   deactivate_scene != null && destroy_scene_tree != null && create_node != null && destroy_node != null &&
                   get_node_transform != null && set_node_transform != null && set_node_visible != null &&
                   list_asset != null && load_asset != null && release_asset != null && bind_asset != null;
        }
    }
}
