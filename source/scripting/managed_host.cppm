// module version: 1.0.0  (independent of the app version in CMakeLists project(VERSION))
// The managed ABI itself is tracked by managed_abi_version.

export module deren.scripting.managed_host;
export import deren.vstd;

export namespace deren::scripting {

    inline constexpr std::uint32_t managed_abi_version = 32;

    struct node_handle {
        std::uint64_t value = 0;
    };

    struct asset_handle {
        std::uint64_t value = 0;
    };

    struct vector_3 {
        float x, y, z;
    };

    /** Orbit camera pose owned by the managed game. Angles are degrees. */
    struct camera_state {
        float yaw_degrees = 0.0F;
        float pitch_degrees = 0.0F;
        float distance = 2.2F;
        vector_3 target{};
    };

    static_assert(sizeof(camera_state) == 24);

    /**
     * @brief one punctual light as managed code hands it to the host
     *
     * A plain old data record, because it crosses the ABI by pointer. Its fields are the ones the
     * renderer's light UBO actually carries (see deren::engine::punctual_light); the host validates
     * them and owns the conversion, so the managed side never sees an engine type.
     *
     * `direction` is the spot's axis, in world space, and is normalized by the host; it is ignored
     * for a point light. `outer_cos` / `inner_cos` are the cosines of the cone's half angles, with
     * `inner_cos` used only when `has_inner_cos` is set.
     *
     * `npr_mode` / `npr_data` carry the GameToon viewer's per-light non-photorealistic payload
     * (mode 0 wrap, 2 specular, 3 rim, 4 fog), which the toon stages read from the ordinary point
     * light lanes.
     */
    struct point_light_desc {
        float position[3] = {0.0F, 0.0F, 0.0F};
        float direction[3] = {0.0F, -1.0F, 0.0F};
        float colour[3] = {1.0F, 1.0F, 1.0F};
        float intensity = 1.0F;
        float range = 10.0F;
        float outer_cos = -0.2F;
        float inner_cos = 1.0F;
        float npr_mode = 0.0F;
        float npr_data[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        std::uint8_t spot = 0;
        std::uint8_t has_inner_cos = 0;
        std::uint8_t padding[2] = {0, 0};
    };

    static_assert(sizeof(point_light_desc) == 76);

    /**
     * @brief one node subtree's per-object material factors, as managed code hands them to the host
     *
     * THE FACTORS LIVE ON THE OBJECT rather than in the imported material, because the importer
     * resolves ONE material per glTF slot: an object that needs a different tint, a metallic or
     * roughness scale, an emissive scale, the unlit shading path, or one of the four custom
     * environment slots needs its own material row.
     *
     * The host clones the imported material once, rewrites the clone from the base row on every
     * call (so a factor never compounds across frames), and points every leaf under the node at the
     * clone. `enabled = 0` restores the imported materials and keeps the clone for a later
     * re-enable. The factors are ignored when `enabled` is clear, matching the [scene] editing
     * surface the native application used to own.
     */
    struct material_override_desc {
        float tint[3] = {1.0F, 1.0F, 1.0F};
        float metallic_scale = 1.0F;
        float roughness_scale = 1.0F;
        float emissive_scale = 1.0F;
        std::uint32_t environment_slot = 0;
        std::uint8_t unlit = 0;
        std::uint8_t enabled = 0;
        std::uint8_t padding[2] = {0, 0};
    };

    static_assert(sizeof(material_override_desc) == 32);

    /**
     * @brief startup values parsed by the managed game before the renderer exists
     *
     * The C++ host applies these values when it creates the window and the frame loop. They are
     * intentionally limited to startup decisions; per-frame settings stay in managed_api.
     */
    struct startup_options {
        std::int32_t capture_frames = 0;
        std::int32_t capture_windowed = 0;
        std::int32_t camera_set = 0;
        float camera_yaw_degrees = 0.0F;
        float camera_pitch_degrees = 0.0F;
        float camera_distance = 2.2F;
        std::int32_t target_set = 0;
        vector_3 target{};
        float sweep_yaw_deg_per_frame = 0.0F;
        float animation_seconds_per_frame = 0.0F;
    };

    static_assert(sizeof(startup_options) == 48);

    struct transform_value {
        vector_3 position;
        vector_3 rotation_degrees;
        vector_3 scale;
    };

    struct utf8_view {
        char const* data;
        std::uint32_t length;
    };

    enum class script_status : std::int32_t {
        success = 0,
        invalid_argument = 1,
        invalid_node = 2,
        scene_exists = 3,
        scene_not_found = 4,
        script_failure = 5,
        spawn_unavailable = 6,
        module_unavailable = 7,
        module_invalid = 8,
        asset_not_found = 9,
        asset_invalid = 10
    };

    /**
     * @brief resolved scene module table
     *
     * The native side owns this table for the host lifetime. Callbacks are static functions;
     * node_handle values are process-wide node ids. Spawn and despawn take the scene id because
     * they need that scene's spawn slots, but the ABI never passes an opaque context.
     */
    // The FACE FRAME the glTF loader MEASURED from the face mesh (ABI 29): where the painted face looks,
    // derived from the face primitive's UV side bands and averaged normal rather than from a head bone.
    // The managed side turns this measurement into the head basis the shaders want; see head_basis_desc.
    // `forward`/`right` are in the frame's own space, `world_*` at the load pose, and `head_relative`
    // says the frame must be rotated by the head joint before use (`skin_index`/`head_joint` name it).
    struct face_frame_desc {
        float forward[3] = {0.0F, 0.0F, 1.0F};
        float right[3] = {-1.0F, 0.0F, 0.0F};
        float world_forward[3] = {0.0F, 0.0F, 1.0F};
        float world_right[3] = {-1.0F, 0.0F, 0.0F};
        float world_center[3] = {0.0F, 0.0F, 0.0F};
        std::uint32_t head_joint = 0;
        std::uint32_t skin_index = 0;
        std::uint8_t head_relative = 0;
        std::uint8_t valid = 0;
        std::uint8_t padding[2] = {0, 0};
    };

    static_assert(sizeof(face_frame_desc) == 72);

    // The head frame the face SDF shades in (ABI 29). `captured` becomes `front.w`: 1.0 tells the shader
    // that `right` IS the captured `Q80` of the painted face rather than the head bone's right. `has_center`
    // becomes `center.w`; a zero centre is the WORLD ORIGIN rather than "no centre", so the flag is what
    // keeps a model with no head position from shading as if its head sat at the origin.
    struct head_basis_desc {
        float front[3] = {0.0F, 0.0F, 1.0F};
        float right[3] = {-1.0F, 0.0F, 0.0F};
        float up[3] = {0.0F, 1.0F, 0.0F};
        float center[3] = {0.0F, 0.0F, 0.0F};
        std::uint8_t captured = 0;
        std::uint8_t has_center = 0;
        std::uint8_t padding[2] = {0, 0};
    };

    static_assert(sizeof(head_basis_desc) == 52);

    struct scene_api {
        script_status(* get_transform)(node_handle node, transform_value* value) = nullptr;
        script_status(* set_transform)(node_handle node, transform_value const* value) = nullptr;
        script_status(* try_spawn)(std::uint64_t scene, utf8_view asset, transform_value const* value,
                                          node_handle* node) = nullptr;
        script_status(* try_despawn)(std::uint64_t scene, node_handle node) = nullptr;
        // Creates an empty managed scene tree. The returned id is independent of script scene ids.
        script_status(* create_scene_tree)(std::uint64_t* tree) = nullptr;
        // Hands a managed tree to the renderer for the current scene startup. The tree becomes
        // the runtime scene and must not be destroyed while active.
        script_status(* activate_scene)(std::uint64_t tree) = nullptr;
        script_status(* get_active_scene)(std::uint64_t* tree) = nullptr;
        // Detaches the active tree from the current script scene before it is destroyed. The
        // native renderer must not have begun a frame with this tree after the call returns.
        script_status(* deactivate_scene)(std::uint64_t tree) = nullptr;
        script_status(* destroy_scene_tree)(std::uint64_t tree) = nullptr;
        // Appends a node to a managed tree. A zero parent creates a root. The returned node id is
        // process-wide and stays valid until the node or one of its ancestors is destroyed.
        script_status(* create_node)(std::uint64_t tree, node_handle parent, utf8_view name,
                                            transform_value const* local, node_handle* node) = nullptr;
        script_status(* destroy_node)(std::uint64_t tree, node_handle node) = nullptr;
        // Local transform of a managed-tree node, stored in its placement component. These calls
        // do not consult a script scene's spawn slots.
        script_status(* get_node_transform)(std::uint64_t tree, node_handle node,
                                                   transform_value* value) = nullptr;
        script_status(* set_node_transform)(std::uint64_t tree, node_handle node,
                                                   transform_value const* value) = nullptr;
        script_status(* set_node_visible)(std::uint64_t tree, node_handle node,
                                                 std::uint8_t visible) = nullptr;
        // Enumerates library asset ids in sorted order. The returned view borrows the library's
        // storage and stays valid until the library is rescanned. Out-of-range indexes return
        // asset_not_found.
        script_status(* list_asset)(std::uint32_t index, utf8_view* id) = nullptr;
        // The asset manager loads one library asset and returns a process-wide handle. The
        // source data stays alive while at least one handle is held.
        script_status(* load_asset)(utf8_view id, asset_handle* asset) = nullptr;
        script_status(* release_asset)(asset_handle asset) = nullptr;
        // Instantiates the whole asset as a subtree under parent (a zero parent creates a root).
        // The offset is the asset instance root's local transform.
        script_status(* bind_asset)(std::uint64_t tree, node_handle parent, asset_handle asset,
                                           transform_value const* offset, node_handle* root) = nullptr;
        // The managed game owns the camera pose. Calls fail if no scene is active.
        script_status(* get_camera)(camera_state* value) = nullptr;
        script_status(* set_camera)(camera_state const* value) = nullptr;
        // Replaces ONE image-based-lighting slot with the environment named by @p path: a baked .ibl or
        // an equirectangular .hdr, relative paths resolved against the executable directory. An empty
        // path loads the procedural sky into that slot. The call is a request: the host validates the
        // path, prepares the images and applies them between frames, and keeps the slot's current
        // environment if decoding fails.
        //
        // SLOT 0 IS THE SCENE-WIDE ENVIRONMENT - the one every material samples unless its record names
        // another slot - and it is also the only slot with a BRDF LUT (the LUT is shared, so a custom
        // slot reuses slot 0's). Slots 1..3 are the per-material lanes: a material record carries the
        // slot number in its flags (bits 10..11, `material_override.environment_slot` on the managed
        // side), and the shaders pick the slot's cubemap pair out of the descriptor heap
        // (`heap_access.slang`, `custom_env_cube + slot - 1`). `layout::heap_environment_capacity` is the
        // upper bound; a larger slot is rejected with `invalid_argument`.
        script_status(* set_environment_hdr)(utf8_view path, std::uint32_t environment_slot) = nullptr;
        // The asset manifest declares which renderer pipelines its meshes support and which one a
        // bind starts on. These return that declaration for parameter validation on the managed
        // side; the strings borrow the registry's storage while the handle is alive.
        script_status(* asset_pipeline_count)(asset_handle asset, std::uint32_t* count) = nullptr;
        script_status(* asset_pipeline)(asset_handle asset, std::uint32_t index, utf8_view* name) = nullptr;
        script_status(* asset_default_pipeline)(asset_handle asset, utf8_view* name) = nullptr;
        // Switches the primitives under a bind_asset() root to @p pipeline. Names outside that
        // asset's supported list are rejected with invalid_argument; a node that is not a bind
        // root is rejected with invalid_node. The host rewrites the imported leaves between frames.
        script_status(* set_pipeline)(std::uint64_t tree, node_handle node, utf8_view pipeline) = nullptr;
        // Post-processing and environment calibration as RUNTIME state (ABI 24). These replace the
        // startup-only [render]/[lighting] hand-off: the values are CPU-side and take effect on the
        // next frame, so a managed game can move them between frames. Each entry validates the
        // arguments and returns invalid_argument instead of clamping silently.
        script_status(* set_exposure)(float exposure) = nullptr;
        // 0 = none/linear, 1 = ACES, 2 = the character-forward grading (see tone_mapping in the shaders)
        script_status(* set_tone_mapping)(std::int32_t mode) = nullptr;
        // Rotation is in DEGREES and wraps; strength is 0..64, background is a mode 0..3, and the
        // background colour channels and blur are 0..1.
        script_status(* set_environment)(float rotation_degrees, float strength, std::int32_t background,
                                         float color_r, float color_g, float color_b, float blur) = nullptr;
        // 0..3, 1.0 = the shading path's constant unchanged.
        script_status(* set_sun_intensity)(float scale) = nullptr;
        // intensity 0..4 (0 disables), threshold 0..0.75.
        script_status(* set_bloom)(float intensity, float threshold) = nullptr;
        // subpixel 0..1, edge_threshold 0..1 (the shader's default is 0.166).
        script_status(* set_fxaa)(std::uint8_t enabled, float subpixel, float edge_threshold) = nullptr;
        // blend_static 0..0.99, blend_min 0..blend_static.
        script_status(* set_taa)(std::uint8_t enabled, float blend_static, float blend_min) = nullptr;
        // radius >= 0, intensity 0..1, samples 0..16 (the shader's array bound).
        script_status(* set_ssao)(std::uint8_t enabled, float radius, float intensity, std::uint32_t samples) = nullptr;
        script_status(* set_gbuffer_debug)(std::uint8_t enabled) = nullptr;
        // The direction points FROM the surface TOWARD the sun; it must be finite and non-zero.
        script_status(* set_sun_direction)(float x, float y, float z) = nullptr;
        // A square emitter: half <= 0 clears it (axis and penumbra are then ignored); a live one needs
        // a finite non-zero axis and penumbra >= 0. irradiance selects the energy mode.
        script_status(* set_area_light)(float centre_x, float centre_y, float centre_z, float half,
                                        std::uint8_t irradiance, float axis_x, float axis_y, float axis_z,
                                        float penumbra) = nullptr;
        // Flips the clustered-light path; false is the "few lights, forward loop" mode.
        script_status(* set_clustered_lights)(std::uint8_t enabled) = nullptr;
        // Replaces the WHOLE punctual light set (ABI 26). `count` may be 0, which clears every light;
        // a count above deren::engine::max_punctual_lights, or a null array with a non-zero count, is
        // rejected with invalid_argument rather than truncated. Each record is validated field by
        // field, so a non-finite value fails the call instead of reaching the light UBO.
        script_status(* set_point_lights)(point_light_desc const* lights, std::uint32_t count) = nullptr;
        // One node subtree's per-object material factors. The walk is the same one apply_pipeline
        // uses, so a hidden subtree is rewritten too; see material_override_desc for the semantics.
        script_status(* set_material_override)(std::uint64_t tree, node_handle node, material_override_desc const* value) = nullptr;
        // Frame pacing (ABI 28): 0 means uncapped, a positive value caps the render loop. A negative or
        // non-finite value is rejected; the runtime itself clamps anything it does accept.
        script_status(* set_max_fps)(double fps) = nullptr;
        // Cel shading for the toon stages: the number of diffuse bands (0 disables banding) and the
        // softness of each step (0..1). Band counts above 32 are rejected as a caller error.
        script_status(* set_toon_shading)(float steps, float softness) = nullptr;
        // Directional shadow mapping on or off. Turning it off clears the maps and drops the pass; the
        // shadow fit is recomputed when it comes back on.
        script_status(* set_shadow_enabled)(std::uint8_t enabled) = nullptr;
        // The face frame the loader measured for a loaded asset (ABI 29), or asset_invalid when that
        // asset has no usable face primitive. The managed side derives the head basis from it.
        script_status(* asset_face_frame)(asset_handle asset, face_frame_desc* value) = nullptr;
        // Publishes one game-toon character's head frame: the direction the face looks, its right and up,
        // and the head's world position. `index` must be below deren::engine::max_game_toon_characters
        // (16); a character that never publishes one keeps the renderer's glTF-basis fallback.
        script_status(* set_head_basis)(std::uint32_t index, head_basis_desc const* value) = nullptr;
        // OFFSCREEN CUBE CAPTURE (ABI 30): a managed game can render its scene six times from one
        // world position and write the result as a reflection-probe cube. The host only installs the
        // camera and runs the pass; the face loop, the frame cadence, and the output path are the
        // game's. enabled == 0 restores the orbit camera and turns the capture pass off (the other
        // arguments are then ignored). face is 0..5 in the engine's cube order; near/far must
        // bracket the scene.
        script_status(* set_capture_camera)(std::uint8_t enabled, float const position[3], std::uint32_t face,
                                           float near_plane, float far_plane) = nullptr;
        // Arms the readback of the next presented frame. Success means it is armed; the pixels are
        // collected once with acquire_radiance_image. A second request before that collection is
        // invalid_argument rather than a silently dropped image.
        script_status(* request_radiance_capture)() = nullptr;
        // Copies the armed image out as width*height RGBA float pixels (4 floats per pixel). The caller
        // owns the buffer; a capacity that does not match the image is invalid_argument, and nothing
        // armed (or a failed readback) is script_failure. The pixels are consumed by the call.
        script_status(* acquire_radiance_image)(std::uint32_t* width, std::uint32_t* height, float* rgba,
                                                std::uint32_t float_count) = nullptr;
        // Drops the accumulated temporal history. The face loop needs it: the next face is a different
        // camera, and a history carried across faces would blend one into the next.
        script_status(* reset_temporal_history)() = nullptr;
        // Writes a captured cube (six faces in the engine's face order, each size*size*4 floats) as the
        // engine's IBL1 probe file at @p path. Called once, after all six faces are collected.
        script_status(* write_radiance_cube)(utf8_view path, std::uint32_t size, float const* pixels,
                                             std::uint32_t float_count) = nullptr;
    };
    // ONE FRAME'S PANEL CONTENT (ABI 31). The managed game describes its debug panel every frame and the
    // host turns each call into the overlay plugin's widget. Every pointer is borrowed until the frame is
    // presented and points at the caller's own storage, so a control edits the game's state directly and
    // no per-widget callback has to cross the boundary.
    struct ui_api {
        // Opens the frame's panel by title (creating it on first use) and empties its widget list.
        // default_width/height are the first-use size; 0 keeps the overlay's auto-size. A repeated or
        // nested begin_panel before end_panel is invalid_argument.
        script_status(* begin_panel)(utf8_view title, float default_width, float default_height) = nullptr;
        script_status(* end_panel)() = nullptr;
        // A line of text the game already formatted; the host never interprets it.
        script_status(* text)(utf8_view text) = nullptr;
        // Appends a control bound to the caller's storage. A checkbox value is a byte holding 0 or 1;
        // a button writes 1 into its byte when pressed, and the caller clears it.
        script_status(* checkbox)(utf8_view label, std::uint8_t* value) = nullptr;
        script_status(* slider)(utf8_view label, float* value, float minimum, float maximum) = nullptr;
        script_status(* vec3)(utf8_view label, float* value, float speed) = nullptr;
        script_status(* combo)(utf8_view label, utf8_view const* items, std::uint32_t count,
                               std::int32_t* selected) = nullptr;
        script_status(* button)(utf8_view label, std::uint8_t* pressed) = nullptr;
        // Whether the overlay currently captures the mouse (a panel is focused or being dragged).
        script_status(* wants_mouse)(std::uint8_t* value) = nullptr;
    };

    // RAW WINDOW INPUT (ABI 31). The managed game owns what the input means; the host only reports what
    // the window received, in the window's own coordinates and key codes.
    enum class input_event_type : std::uint32_t {
        mouse_move = 1,
        mouse_button,
        scroll,
        key,
        character,
        focus_lost
    };

    struct input_event {
        input_event_type type = input_event_type::mouse_move;
        std::int32_t button = 0;     // mouse_button: the backend's mouse button index
        std::uint8_t down = 0;       // mouse_button and key: 1 pressed, 0 released
        std::uint8_t padding[3] = {};
        std::int32_t key = 0;        // key: the backend's key code, as the window reports it
        float x = 0.0F;              // mouse: cursor position in window coordinates
        float y = 0.0F;
        float dx = 0.0F;             // mouse_move: movement since the previous event
        float dy = 0.0F;
        float wheel = 0.0F;          // scroll: vertical wheel delta
        std::uint32_t codepoint = 0; // character: one Unicode code point
    };

    static_assert(sizeof(input_event) == 40);

    enum class native_module : std::uint32_t {
        scene = 1,
        // ABI 31: the debug panel and the overlay state (ui_api).
        ui = 2
    };

    // Keep layouts and calling conventions in sync with managed_bootstrap.cs.
    struct native_api {
        void(* log)(char const* utf8, std::uint32_t length) = nullptr;
        script_status(* get_module)(std::uint32_t module, void const** table) = nullptr;
        // Requests the same one-shot screenshot path as the F12 key. The next recorded frame
        // copies the presented image, and the renderer saves it under [paths] screenshot_dir with
        // a timestamped file name. Null before initialize; projects call it through
        // deren.screenshot.
        script_status(* request_screenshot)() = nullptr;
        // ABI 31: raw window input. The host calls @p callback on the render thread for every event
        // the window reports and passes @p context back unchanged; passing a null callback stops the
        // forwarding. The game owns the meaning of the input, the host only reports it.
        void(* set_input_event_callback)(void(*callback)(void* context, input_event const* event), void* context) = nullptr;
    };

    // One frame's borrowed state. The layout is exact for this ABI version; any field
    // addition, removal, or reorder changes managed_abi_version.
    struct frame_info {
        std::uint64_t scene = 0;
        float delta_time = 0.0F;
        std::uint32_t input_count = 0;
        std::uint8_t const* input = nullptr;
    };

    static_assert(sizeof(frame_info) == 24);

    // Frame-boundary signals are facts sent from the host into managed code. The host never
    // executes a managed callable itself; managed code owns the queue behind do_after_frame.
    enum class frame_signal : std::uint32_t {
        after_frame = 1
    };

    struct managed_api {
        // The managed game owns its scene and script lifetime. The host only forwards frames
        // and asks for one program-wide shutdown.
        // The frame and its input bytes are borrowed only during the update.
        script_status(* update)(frame_info const* frame) = nullptr;
        script_status(* stop)() = nullptr;
        // Emitted after a presented frame. Managed code dispatches its collected callables here.
        script_status(* signal)(std::uint32_t signal) = nullptr;
    };

    enum class host_status : std::int32_t {
        success = 0,
        abi_mismatch = 1,
        invalid_api = 2,
        assembly_missing,
        runtime_config_missing,
        nethost_load_failed,
        runtime_discovery_failed,
        hostfxr_load_failed,
        runtime_initialization_failed,
        delegate_lookup_failed,
        bridge_load_failed,
        bridge_initialization_failed,
        scene_module_unavailable,
        scene_module_invalid,
        game_assembly_missing
    };

    struct host_paths {
        std::filesystem::path assembly;
        std::filesystem::path runtime_config;
        std::filesystem::path nethost;
        std::filesystem::path dotnet_root{};
        // Project script assembly loaded by the managed host during initialization.
        std::filesystem::path game_assembly;
        // The process command line, borrowed for the duration of initialization. The managed game
        // owns argument interpretation; the host only forwards the original argc/argv.
        std::int32_t argc = 0;
        char** argv = nullptr;
    };

    struct host_result {
        host_status status;
        std::int32_t detail = 0;
    };

    /**
     * @brief Loads .NET and the project assembly early so the game can parse argv.
     *
     * The game fills startup_options before the host creates the window or the Vulkan runtime.
     * A later initialize_managed_host() call reuses this session and runs the scene phase.
     */
    [[nodiscard]] host_result configure_managed_host(host_paths const& paths, native_api const& api,
                                                    startup_options& options,
                                                    std::uint32_t abi_version = managed_abi_version);

    [[nodiscard]] host_result initialize_managed_host(host_paths const& paths, native_api const& api, managed_api& scripts,
                                                     std::uint32_t abi_version = managed_abi_version);

} // namespace deren::scripting
