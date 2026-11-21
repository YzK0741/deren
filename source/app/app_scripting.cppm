// module version: 0.5.0  (independent of the app version in CMakeLists project(VERSION))

export module deren.app.scripting;

import deren.vstd;
import deren.app.toon;
import deren.application_configuration;
import deren.engine.scene_tree;
import deren.gaussian_loader;
import deren.gltf_loader;
import deren.scripting.managed_host;
import deren.scripting.rml_ui_api;
import deren.utility;

export namespace deren::app {
    /**
     * @brief Decoded source data for one managed asset binding.
     *
     * The managed asset registry owns the scene and the resolved material slice for the whole
     * handle lifetime. The binder imports this data without consulting native project object
     * metadata or a preloaded asset slot.
     */
    struct managed_asset_source {
        gltf::scenes const* scenes = nullptr;
        /**
         * @brief The CPU splat cloud for a `type = "gaussian"` asset, or null for a model.
         *
         * A cloud has no glTF scene, no resolved materials and no toon sidecar: @ref scenes is null
         * exactly when this is set. The binder uploads the cloud once (`runtime::create_gaussian_asset`)
         * and stages it in the shared splat pass, whose placement is the wrapper's own transform - there
         * is nothing to import, so @ref import_shift and the material fields do not apply.
         */
        deren::gaussian::asset const* cloud = nullptr;
        std::span<gltf::resolved_material const> materials;
        std::array<float, 3> import_shift{};
        /** World-space centre of the imported bounds once @ref import_shift has been applied - the
         *  keep-out sphere's centre the shadow fit is built around (see runtime::enable_shadows). */
        std::array<float, 3> scene_center{};
        /** Radius of those imported bounds, the shadow fit's radius. */
        float scene_radius = 0.0f;
        /** The asset's baked toon lookup state, or null for a model that carries no `.toon.tsv` beside it.
         *  The binder installs it through `runtime::set_toon_lookup` immediately before the import, which is
         *  how a character's sidecar maps (ramps, LUT, matcap, colours) reach the toon lanes at all. */
        deren::app::detail::toon_lookup_state const* toon = nullptr;
        /** The manifest's pipeline the binder should apply after import; empty means "leave it on pbr". */
        std::string_view default_pipeline;
        /** The manifest's supported pipelines, used to validate later try_set_pipeline calls. */
        std::span<std::string const> pipelines;
    };

    /**
     * @brief Host callback that turns a managed asset binding into rendered primitives.
     *
     * The callback runs while a managed tree is bound under an existing node. It should attach
     * the asset's imported subtree to @p parent; returning false leaves the binding failed and
     * the wrapper node is removed.
     */
    struct managed_asset_binder {
        void* context = nullptr;
        bool (*bind)(void* context, engine::scene_tree::scene_node& parent, std::string_view asset_id,
                     managed_asset_source const& source) = nullptr;
        // Finish in-flight draws before a live edit releases a node's render resources.
        void (*before_remove)(void* context) = nullptr;
    };

    /**
     * @brief Host side of one frame's debug panel (ABI 31).
     *
     * Managed code calls these while it fills the panel; the storage it passes addresses its own game
     * state, so a control edits that state directly and no per-widget callback crosses the boundary.
     * The host owns the overlay and only translates each call into the overlay plugin's widget tree.
     * An empty host (no [gui] show, or a host that cannot draw) leaves every entry point null, and the
     * ui module then reports module_unavailable rather than silently dropping the panel.
     */
    struct managed_ui_host {
        void* context = nullptr;
        bool (*begin_panel)(void* context, std::string_view title, float default_width, float default_height) =
            nullptr;
        bool (*end_panel)(void* context) = nullptr;
        bool (*text)(void* context, std::string_view text) = nullptr;
        bool (*checkbox)(void* context, std::string_view label, std::uint8_t* value) = nullptr;
        bool (*slider)(void* context, std::string_view label, float* value, float minimum, float maximum) = nullptr;
        bool (*vec3)(void* context, std::string_view label, float* value, float speed) = nullptr;
        bool (*combo)(void* context, std::string_view label, std::string_view const* items, std::uint32_t count,
                      std::int32_t* selected) = nullptr;
        bool (*button)(void* context, std::string_view label, std::uint8_t* pressed) = nullptr;
        bool (*wants_mouse)(void* context, std::uint8_t* value) = nullptr;
    };

    /**
     * @brief what managed code can ask a running animation player to do (ABI 35)
     *
     * The command carries one index and one float, which is all the player needs: a clip selection
     * names its index, the play flag and the speed are floats, and a scrub is a time. Keeping one
     * entry instead of seven keeps the host binding and the managed mirror in one place.
     */
    enum class animation_command_kind : std::uint32_t {
        select = 0,
        playing = 1,
        speed = 2,
        time = 3
    };

    /**
     * @brief Host callback that makes a managed tree the rendered scene.
     *
     * The managed scene builder creates the tree, fills it, and calls activate_scene() before
     * binding assets. The callback installs it into the renderer and returns whether it was
     * accepted.
     */
    struct managed_scene_host {
        void* context = nullptr;
        bool (*set_scene)(void* context, engine::scene_tree::scene& scene) = nullptr;
        bool (*get_camera)(void* context, scripting::camera_state* value) = nullptr;
        bool (*set_camera)(void* context, scripting::camera_state const* value) = nullptr;
        bool (*request_screenshot)(void* context) = nullptr;
        /**
         * @brief Queues a new IBL for ONE environment slot from the named environment file; empty
         *        means the procedural sky in that slot.
         *
         * Slot 0 is the scene-wide environment, slots 1..3 are the per-material lanes a material
         * record can name (`material_override.environment_slot`). The callback runs on the render
         * thread but must not touch the device: it validates the slot and the path, stores the
         * request, and the host prepares and uploads it between frames. Returning false rejects an
         * out-of-range slot or a missing/unreadable file in the slot that named the path (relative
         * paths resolve against the executable directory) and leaves that slot's environment
         * untouched.
         */
        bool (*set_environment_hdr)(void* context, std::string_view path, std::uint32_t environment_slot) = nullptr;
        /**
         * @brief Switches the primitives under a bound asset root to another supported pipeline.
         *
         * @p node is the root returned by bind_asset(). The callback runs on the render thread and
         * rewrites the imported leaves between frames; returning false leaves the asset untouched.
         * Asset scripts are expected to check supported_pipelines() through the asset API first.
         */
        bool (*apply_pipeline)(void* context, engine::scene_tree::scene_node& node, std::string_view pipeline) = nullptr;
        /**
         * @brief Runtime post-processing and environment calibration (the former startup-only knobs).
         *
         * Each callback validates the arguments and applies the value to the render state on the
         * next frame; false rejects the call. These are global (one runtime), so they carry no tree.
         */
        bool (*set_exposure)(void* context, float exposure) = nullptr;
        bool (*set_tone_mapping)(void* context, std::int32_t mode) = nullptr;
        bool (*set_environment)(void* context, float rotation_degrees, float strength, std::int32_t background,
                                float color_r, float color_g, float color_b, float blur) = nullptr;
        bool (*set_sun_intensity)(void* context, float scale) = nullptr;
        bool (*set_bloom)(void* context, float intensity, float threshold) = nullptr;
        bool (*set_fxaa)(void* context, bool enabled, float subpixel, float edge_threshold) = nullptr;
        bool (*set_taa)(void* context, bool enabled, float blend_static, float blend_min) = nullptr;
        bool (*set_ssao)(void* context, bool enabled, float radius, float intensity, std::uint32_t samples) = nullptr;
        bool (*set_gbuffer_debug)(void* context, bool enabled) = nullptr;
        /**
         * @brief Runtime lighting: the sun's direction, the optional square area emitter, and the
         * clustered-light switch. Global like the post-processing knobs; false rejects the call.
         */
        bool (*set_sun_direction)(void* context, float x, float y, float z) = nullptr;
        bool (*set_area_light)(void* context, float centre_x, float centre_y, float centre_z, float half,
                               bool irradiance, float axis_x, float axis_y, float axis_z, float penumbra) = nullptr;
        bool (*set_clustered_lights)(void* context, bool enabled) = nullptr;
        /**
         * @brief Replaces the whole punctual light set with @p count records from @p lights.
         *
         * Global like the other lighting setters. A count above the engine's
         * `max_punctual_lights`, or a null array with a non-zero count, is the caller's error and the
         * host rejects it; a count of zero clears every light. False means the runtime refused a
         * record (a non-finite field, say) and the previous set stays.
         */
        bool (*set_point_lights)(void* context, scripting::point_light_desc const* lights, std::uint32_t count) = nullptr;
        /**
         * @brief Rewrites every leaf under @p node with one object's material factors.
         *
         * The factors live on the object, not in the imported material (one glTF slot resolves to
         * one material), so the host clones the imported material once, rebuilds the clone from the
         * base row on every call and points the leaves at it. `value->enabled == 0` restores the
         * imported materials; the clone is kept for a later re-enable. False leaves the subtree
         * untouched.
         */
        bool (*apply_material_override)(void* context, engine::scene_tree::scene_node& node,
                                        scripting::material_override_desc const& value) = nullptr;
        /** ABI 28: frame pacing policy. 0 means uncapped. */
        bool (*set_max_fps)(void* context, double fps) = nullptr;
        /** ABI 28: the toon stages' diffuse band count (0 disables banding) and step softness (0..1). */
        bool (*set_toon_shading)(void* context, float steps, float softness) = nullptr;
        /** ABI 28: the directional shadow pass on or off. */
        bool (*set_shadow_enabled)(void* context, bool enabled) = nullptr;
        /**
         * @brief ABI 29: the face frame the loader measured for @p asset, or false when it has none.
         *
         * The measurement is taken once at load, from the asset's face-family mesh, so this call copies
         * it rather than re-measuring. False covers a gaussian cloud, a model with no face primitive,
         * and an unloaded handle - the managed side then keeps the renderer's fallback frame.
         */
        bool (*asset_face_frame)(void* context, scripting::asset_handle asset, scripting::face_frame_desc& value) = nullptr;
        /**
         * @brief ABI 29: publishes one game-toon character's head frame to the shaders.
         *
         * The values are already in world space: the managed side owns the placement and applies it to
         * the frame it derived, so the host only validates and forwards. `index` addresses one of the
         * renderer's `max_game_toon_characters` slots; out-of-range, a non-finite axis, or a degenerate
         * frame is the caller's error and returns false.
         */
        bool (*set_head_basis)(void* context, std::uint32_t index, scripting::head_basis_desc const& value) = nullptr;
        /**
         * @brief ABI 30: installs (or clears) the offscreen cube-capture camera and the capture pass.
         *
         * enabled == 0 restores the orbit camera and turns the radiance-capture pass off; the other
         * arguments are then ignored. Otherwise position is the world point the cube is rendered from,
         * face is 0..5 in the engine's cube order, and near/far must be positive and ordered. The
         * managed game owns the face loop and the frame cadence; this entry only moves the camera.
         */
        bool (*set_capture_camera)(void* context, std::uint8_t enabled, float const position[3], std::uint32_t face,
                                   float near_plane, float far_plane) = nullptr;
        /**
         * @brief ABI 30: arms the readback of the next presented frame.
         *
         * False means nothing was armed: either a capture was already pending (the engine keeps one
         * unread request) or the host has no capture path. The pixels are collected once, with
         * acquire_radiance_image.
         */
        bool (*request_radiance_capture)(void* context) = nullptr;
        /**
         * @brief ABI 30: copies the armed frame's radiance into the caller's buffer.
         *
         * The buffer holds float_count floats and must match width*height*4; the host reports the
         * image's size through @p width / @p height. False covers a capacity mismatch and a failed or
         * absent readback; the managed side owns the six-face assembly and the file name.
         */
        bool (*acquire_radiance_image)(void* context, std::uint32_t& width, std::uint32_t& height, float* rgba,
                                       std::uint32_t float_count) = nullptr;
        /**
         * @brief ABI 30: writes an assembled cube as the engine's IBL1 reflection-probe file.
         *
         * pixels is six faces in face order, each size*size*4 floats, and float_count must match.
         * The file is written to a temporary name and renamed, so a half-written probe never appears
         * under @p path; an existing @p path is refused rather than overwritten.
         */
        /** ABI 30: drops the accumulated temporal history; see managed_scene_host::reset_temporal_history. */
        bool (*reset_temporal_history)(void* context) = nullptr;
        bool (*write_radiance_cube)(void* context, std::string_view path, std::uint32_t size, float const* pixels,
                                    std::uint32_t float_count) = nullptr;
        /**
         * @brief ABI 35: the character animation player.
         *
         * animation_state publishes a snapshot taken once per frame, which every query reads; the
         * other two queue work for the frame that runs before the frame is recorded. All of them
         * return false when no player exists, which the managed side reports as not_ready.
         */
        bool (*animation_state)(void* context, scripting::animation_state& out) = nullptr;
        bool (*animation_playable_name)(void* context, std::uint32_t index, std::string_view& name) = nullptr;
        bool (*animation_command)(void* context, animation_command_kind kind, std::uint32_t index,
                                  float value) = nullptr;
        /**
         * @brief ABI 37: a managed fault the process cannot continue from.
         *
         * The host answers by writing one crash report through deren::utility::panic_managed and
         * terminating; returning `true` means a host recorded the report instead (tests only). The
         * views are borrowed for the call and may be empty; `line` is 0 when the managed side had
         * no location.
         */
        bool (*panic)(void* context, std::uint32_t line, std::string_view message, std::string_view source_file,
                      std::string_view managed_stack) = nullptr;
        /**
         * @brief ABI 38: the query side of the renderer state the debug overlay already shows.
         *
         * viewport_size reports the RENDER extent in pixels - the size the shading passes ran at,
         * with the render scale already applied, not the window's or the presented image's size.
         * project_point and screen_ray map between world space and that same pixel grid using the
         * matrices the current frame renders with, so a managed HUD lines up with the 3D view. counters fills the
         * diagnostic struct from the runtime's own sizes, and gpu_timing_summary hands back the
         * overlay's per-pass timing text, borrowed until the next frame. False means the host has no
         * runtime yet.
         */
        bool (*viewport_size)(void* context, std::uint32_t& width, std::uint32_t& height) = nullptr;
        bool (*project_point)(void* context, float const world[3], float screen[3]) = nullptr;
        bool (*screen_ray)(void* context, float x, float y, float origin[3], float direction[3]) = nullptr;
        bool (*counters)(void* context, scripting::diagnostic_counters& value) = nullptr;
        bool (*gpu_timing_summary)(void* context, std::string_view& text) = nullptr;
        /**
         * @brief ABI 39: the TOON LIGHT RIG, queued and applied between frames.
         *
         * The rig lives in one block the character stage reads every frame, so it may not be written
         * while a frame is in flight; this entry only stores the value and the app applies it after
         * the managed update, the same place an environment change lands. False means the host has no
         * runtime yet (nothing was stored). `get` answers what the next frame will use: the pending
         * value while one is queued, otherwise the published rig.
         */
        bool (*set_toon_rig)(void* context, scripting::toon_rig_desc const& value) = nullptr;
        bool (*get_toon_rig)(void* context, scripting::toon_rig_desc& value) = nullptr;
        /**
         * @brief ABI 41: the projection aspect the scene is rendered with.
         *
         * The game tells the host the shape of the rectangle it will show the scene in (an editor
         * viewport drawn as a document image). The render still covers the window; the projection
         * is built for this aspect so the stretched image looks right. False means the host has no
         * runtime yet. Passing 0 restores the window's aspect.
         */
        bool (*set_scene_aspect)(void* context, float aspect) = nullptr;
        /**
         * @brief ABI 43: the application window, for a project that draws its own caption.
         *
         * A borderless window has no system title bar, so the managed UI owns the minimise,
         * maximise/restore and close buttons and the drag area. `window_minimize` and
         * `window_toggle_maximize` change the window state directly; `window_is_maximized` writes
         * 0 or 1; `window_close` raises the same close request the system button does; and
         * `window_begin_drag` hands a caption press to the system move loop, BLOCKING until the
         * button is released (false when the platform has no such loop). False means the host has
         * no window (an offscreen capture, for example), so the UI treats every one as unavailable.
         */
        bool (*window_minimize)(void* context) = nullptr;
        bool (*window_toggle_maximize)(void* context) = nullptr;
        bool (*window_is_maximized)(void* context, std::uint8_t* maximized) = nullptr;
        bool (*window_close)(void* context) = nullptr;
        bool (*window_begin_drag)(void* context) = nullptr;
    };

    class script_scene {
        bool owns_audio_session = false;
        scripting::rml_ui_api const* document_ui = nullptr;

    public:
        script_scene() = default;
        ~script_scene();
        script_scene(script_scene const&) = delete;
        script_scene& operator=(script_scene const&) = delete;

        [[nodiscard]] std::expected<scripting::startup_options, std::string> preinit(
            std::filesystem::path const& assembly, std::int32_t argc, char** argv);
        [[nodiscard]] std::expected<void, std::string> start(
            std::filesystem::path const& assembly, managed_scene_host host,
            std::filesystem::path const& asset_library_root = {}, managed_asset_binder binder = {});
        // Stop once while the runtime and UI callbacks are still alive. Safe before start or after stop.
        void stop();
        [[nodiscard]] bool update(float dt, std::span<std::uint8_t const> input_frame);
        /**
         * Emits the before-frame callback and waits for managed code to report the batch done.
         *
         * Called only for a frame the host has already decided to draw, so the pair before/after is
         * never split by a skipped frame. Managed code owns the queue behind the callback; the host
         * only hands over the boundary and waits for native_api notify_rendering.
         */
        [[nodiscard]] bool signal_before_frame();
        /** Emits the after-frame callback and waits for managed code to report the batch done. */
        [[nodiscard]] bool signal_after_frame();
        /** Installs the debug panel host (ABI 31); empty when the project draws no panel. */
        void install_ui(managed_ui_host host);
        void install_document_ui(scripting::rml_ui_api const* table) noexcept;
        /// ABI 31: hands one window event to the managed game's registered handler, if it has one.
        ///
        /// THE HOST OWNS DELIVERY, THE GAME OWNS MEANING: the app forwards what the window produced
        /// and keeps no input state of its own, so a game that wants a pointer-locked camera, a text
        /// field or a hotkey reads this stream and decides. Safe to call with nobody listening.
        static void publish_input_event(scripting::input_event const& event);
        /** Drops the asset binder before its host context is destroyed. */
        void clear_asset_binder() noexcept;

    private:
        struct placement {
            scripting::transform_value transform;
            bool dirty = false;
        };

        static placement* find(scripting::node_handle handle);
        static script_scene* find_scene(std::uint64_t scene);
        static void log(char const* text, std::uint32_t length);
        static scripting::script_status get_module(std::uint32_t module, void const** table, std::uint32_t* byte_size);
        static scripting::script_status pick_node(std::uint64_t tree, scripting::vector_3 const* origin,
                                                  scripting::vector_3 const* direction, scripting::node_handle* hit);
        static scripting::script_status query_node_bounds(std::uint64_t tree, scripting::node_handle node,
                                                          scripting::vector_3* center, float* radius);
        static scripting::script_status abi_layout(scripting::abi_layout_entry* out, std::uint32_t capacity);
        /// The native_api entry that ends a frame-boundary wait; see managed_scene_host::notify_rendering.
        static scripting::script_status notify_rendering();
        /// ABI 35: the native_api entry that sets the frame-boundary wait. 0 waits indefinitely.
        static scripting::script_status set_frame_boundary_timeout(std::uint32_t milliseconds);
        static scripting::script_status get_transform(scripting::node_handle node, scripting::transform_value* value);
        static scripting::script_status set_transform(scripting::node_handle node, scripting::transform_value const* value);
        static scripting::script_status try_spawn(std::uint64_t scene, scripting::utf8_view asset,
                                                  scripting::transform_value const* value, scripting::node_handle* node);
        static scripting::script_status try_despawn(std::uint64_t scene, scripting::node_handle node);
        static scripting::script_status create_scene_tree(std::uint64_t* tree);
        static scripting::script_status activate_scene(std::uint64_t tree);
        static scripting::script_status get_active_scene(std::uint64_t* tree);
        static scripting::script_status deactivate_scene(std::uint64_t tree);
        static scripting::script_status destroy_scene_tree(std::uint64_t tree);
        static scripting::script_status create_node(std::uint64_t tree, scripting::node_handle parent,
                                                    scripting::utf8_view name,
                                                    scripting::transform_value const* local,
                                                    scripting::node_handle* node);
        static scripting::script_status destroy_node(std::uint64_t tree, scripting::node_handle node);
        static scripting::script_status get_node_transform(std::uint64_t tree, scripting::node_handle node,
                                                           scripting::transform_value* value);
        static scripting::script_status set_node_transform(std::uint64_t tree, scripting::node_handle node,
                                                           scripting::transform_value const* value);
        static scripting::script_status set_node_visible(std::uint64_t tree, scripting::node_handle node,
                                                         std::uint8_t visible);
        /// ABI 38: tree queries. `parent` in reparent_node is a null handle for "become a root".
        static scripting::script_status node_child_count(std::uint64_t tree, scripting::node_handle node,
                                                         std::uint32_t* count);
        static scripting::script_status node_child(std::uint64_t tree, scripting::node_handle node, std::uint32_t index,
                                                   scripting::node_handle* child);
        static scripting::script_status node_name(std::uint64_t tree, scripting::node_handle node, scripting::utf8_view* name);
        /// ABI 40: renames a node; an empty, NUL-carrying or over-256-byte name is refused.
        static scripting::script_status set_node_name(std::uint64_t tree, scripting::node_handle node,
                                                      scripting::utf8_view name);
        /// ABI 41: the projection aspect the scene is drawn with; 0 restores the window's.
        static scripting::script_status set_scene_aspect(float aspect);
        /// ABI 43: the request window controls; see managed_scene_host::window_minimize and friends.
        static scripting::script_status window_minimize();
        static scripting::script_status window_toggle_maximize();
        static scripting::script_status window_is_maximized(std::uint8_t* maximized);
        static scripting::script_status window_close();
        static scripting::script_status window_begin_drag();
        static scripting::script_status get_node_flags(std::uint64_t tree, scripting::node_handle node,
                                                       std::uint32_t* flags);
        static scripting::script_status set_node_flags(std::uint64_t tree, scripting::node_handle node,
                                                       std::uint32_t flags, std::uint32_t mask);
        static scripting::script_status reparent_node(std::uint64_t tree, scripting::node_handle node,
                                                      scripting::node_handle parent, std::uint8_t keep_transform);
        /// ABI 38: view/screen mapping and the runtime counters; all five forward to managed_scene_host.
        static scripting::script_status viewport_size(std::uint32_t* width, std::uint32_t* height);
        static scripting::script_status project_point(float const* world, float* screen);
        static scripting::script_status screen_ray(float x, float y, float* origin, float* direction);
        static scripting::script_status query_counters(scripting::diagnostic_counters* counters);
        static scripting::script_status gpu_timing_summary(scripting::utf8_view* text);
        /// ABI 39: the toon light rig; see managed_scene_host::set_toon_rig.
        static scripting::script_status set_toon_rig(scripting::toon_rig_desc const* value);
        static scripting::script_status get_toon_rig(scripting::toon_rig_desc* value);
        static scripting::script_status list_asset(std::uint32_t index, scripting::utf8_view* id);
        static scripting::script_status load_asset(scripting::utf8_view id, scripting::asset_handle* asset);
        static scripting::script_status release_asset(scripting::asset_handle asset);
        static scripting::script_status bind_asset(std::uint64_t tree, scripting::node_handle parent,
                                                   scripting::asset_handle asset,
                                                   scripting::transform_value const* offset,
                                                   scripting::node_handle* root);
        static scripting::script_status get_camera(scripting::camera_state* value);
        static scripting::script_status set_camera(scripting::camera_state const* value);
        /// The native_api entry: stores or clears the one handler the app forwards to. A null
        /// callback stops the forwarding; the context is handed back untouched.
        static void set_input_event_callback(void (*callback)(void* context, scripting::input_event const* event),
                                             void* context);

        static scripting::script_status request_screenshot();

        static scripting::script_status set_environment_hdr(scripting::utf8_view path, std::uint32_t environment_slot);
        static scripting::script_status asset_pipeline_count(scripting::asset_handle asset, std::uint32_t* count);
        static scripting::script_status asset_pipeline(scripting::asset_handle asset, std::uint32_t index,
                                                       scripting::utf8_view* name);
        static scripting::script_status asset_default_pipeline(scripting::asset_handle asset,
                                                               scripting::utf8_view* name);
        static scripting::script_status set_pipeline(std::uint64_t tree, scripting::node_handle node,
                                                     scripting::utf8_view pipeline);
        static scripting::script_status set_exposure(float exposure);
        static scripting::script_status set_tone_mapping(std::int32_t mode);
        static scripting::script_status set_environment(float rotation_degrees, float strength,
                                                        std::int32_t background, float color_r, float color_g,
                                                        float color_b, float blur);
        static scripting::script_status set_sun_intensity(float scale);
        static scripting::script_status set_bloom(float intensity, float threshold);
        static scripting::script_status set_fxaa(std::uint8_t enabled, float subpixel, float edge_threshold);
        static scripting::script_status set_taa(std::uint8_t enabled, float blend_static, float blend_min);
        static scripting::script_status set_ssao(std::uint8_t enabled, float radius, float intensity,
                                                 std::uint32_t samples);
        static scripting::script_status set_gbuffer_debug(std::uint8_t enabled);
        static scripting::script_status set_sun_direction(float x, float y, float z);
        static scripting::script_status set_area_light(float centre_x, float centre_y, float centre_z, float half,
                                                       std::uint8_t irradiance, float axis_x, float axis_y,
                                                       float axis_z, float penumbra);
        static scripting::script_status set_clustered_lights(std::uint8_t enabled);
        /** Replaces the whole punctual light set; see managed_scene_host::set_point_lights. */
        static scripting::script_status set_point_lights(scripting::point_light_desc const* lights, std::uint32_t count);
        /** Rewrites one subtree's material factors; see managed_scene_host::apply_material_override. */
        static scripting::script_status set_material_override(std::uint64_t tree, scripting::node_handle node,
                                                              scripting::material_override_desc const* value);
        /** ABI 28: frame pacing policy; see managed_scene_host::set_max_fps. */
        static scripting::script_status set_max_fps(double fps);
        /** ABI 28: toon band count and step softness; see managed_scene_host::set_toon_shading. */
        static scripting::script_status set_toon_shading(float steps, float softness);
        /** ABI 28: the shadow pass on or off; see managed_scene_host::set_shadow_enabled. */
        static scripting::script_status set_shadow_enabled(std::uint8_t enabled);
        /** ABI 29: the face frame measured for one loaded asset; see managed_scene_host::asset_face_frame. */
        static scripting::script_status asset_face_frame(scripting::asset_handle asset, scripting::face_frame_desc* value);
        /** ABI 29: one character's head frame; see managed_scene_host::set_head_basis. */
        static scripting::script_status set_head_basis(std::uint32_t index, scripting::head_basis_desc const* value);
        /** ABI 30: installs or clears the cube-capture camera; see managed_scene_host::set_capture_camera. */
        static scripting::script_status set_capture_camera(std::uint8_t enabled, float const position[3],
                                                           std::uint32_t face, float near_plane, float far_plane);
        /** ABI 30: arms one frame's radiance readback; see managed_scene_host::request_radiance_capture. */
        static scripting::script_status request_radiance_capture();
        /** ABI 30: copies the armed image out; see managed_scene_host::acquire_radiance_image. */
        static scripting::script_status acquire_radiance_image(std::uint32_t* width, std::uint32_t* height,
                                                               float* rgba, std::uint32_t float_count);
        /** ABI 30: writes an assembled cube; see managed_scene_host::write_radiance_cube. */
        /** ABI 30: drops the accumulated temporal history; see managed_scene_host::reset_temporal_history. */
        static scripting::script_status reset_temporal_history();
        static scripting::script_status write_radiance_cube(scripting::utf8_view path, std::uint32_t size,
                                                            float const* pixels, std::uint32_t float_count);
        /** ABI 35: the animation player's per-frame snapshot; see managed_scene_host::animation_state. */
        static scripting::script_status get_animation_state(scripting::animation_state* value);
        /** ABI 35: how many clips the player holds, from the same snapshot. */
        static scripting::script_status animation_playable_count(std::uint32_t* count);
        /** ABI 35: one clip's name, borrowed from the player; see managed_scene_host::animation_playable_name. */
        static scripting::script_status animation_playable_name(std::uint32_t index, scripting::utf8_view* name);
        /** ABI 35: the four player commands; see managed_scene_host::animation_command. */
        static scripting::script_status animation_select(std::uint32_t index);
        static scripting::script_status animation_set_playing(std::uint8_t playing);
        static scripting::script_status animation_set_speed(float speed);
        static scripting::script_status animation_set_time(float seconds);
        /**
         * ABI 37: reports a managed fault; see managed_scene_host::panic.
         *
         * Answers success when the host took the report - which, for the real host, has already
         * terminated the process by then. Any other answer is the managed side's cue to write
         * stderr and fail fast on its own.
         */
        static scripting::script_status panic_report(scripting::utf8_view message, scripting::utf8_view source_file,
                                                     std::uint32_t line, scripting::utf8_view managed_stack);
        /** The scene whose host answers the global (tree-less) setters: the starting scene, else the newest. */
        static script_scene* current_owner();
        /** The scene that answers the animation entries, or nullptr when no host provides them. */
        static script_scene* animation_owner();

        // ABI 31: the debug panel. Each call is one widget in the frame the managed game is drawing; the
        // managed side pairs begin_panel with end_panel and owns the storage the controls write into.
        static scripting::script_status ui_begin_panel(scripting::utf8_view title, float default_width,
                                                       float default_height);
        static scripting::script_status ui_end_panel();
        static scripting::script_status ui_text(scripting::utf8_view text);
        static scripting::script_status ui_checkbox(scripting::utf8_view label, std::uint8_t* value);
        static scripting::script_status ui_slider(scripting::utf8_view label, float* value, float minimum,
                                                  float maximum);
        static scripting::script_status ui_vec3(scripting::utf8_view label, float* value, float speed);
        static scripting::script_status ui_combo(scripting::utf8_view label, scripting::utf8_view const* items,
                                                 std::uint32_t count, std::int32_t* selected);
        static scripting::script_status ui_button(scripting::utf8_view label, std::uint8_t* pressed);
        static scripting::script_status ui_wants_mouse(std::uint8_t* value);

        /**
         * Hands one frame boundary to managed code and waits for the matching notify_rendering.
         *
         * The pending flag is raised before the callback runs, so a managed side that answers
         * inline (an empty batch) settles the wait immediately instead of losing the wake-up. A
         * managed side that never answers costs one timeout and one error line, never a hung app.
         */
        [[nodiscard]] bool await_frame_callback(void (*callback)());

        scripting::managed_api scripts;
        managed_ui_host ui_host;
        engine::scene_tree::scene* tree = nullptr;
        std::uint64_t tree_id = 0;
        managed_scene_host host;
        managed_asset_binder asset_binder;
        std::uint64_t scene_id = 0;
        bool running = false;
        bool closing = false;
        // ABI 34: one frame boundary at a time. The render thread waits on this pair while managed
        // code runs the batch on its worker thread and calls notify_rendering.
        std::mutex frame_mutex;
        std::condition_variable frame_cv;
        bool frame_pending = false;
    };
} // namespace deren::app
