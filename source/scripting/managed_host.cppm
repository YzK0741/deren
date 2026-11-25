// module version: 1.2.0  (independent of the app version in CMakeLists project(VERSION))
// The managed ABI itself is tracked by managed_abi_version.

export module deren.scripting.managed_host;
export import deren.vstd;
export import deren.scripting.api_types;
export import deren.scripting.audio_api;

export namespace deren::scripting {

    inline constexpr std::uint32_t managed_abi_version = 47;

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
        float vertical_fov_degrees = 0.0F; // zero keeps the current lens
    };

    static_assert(sizeof(camera_state) == 28);

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
        /**
         * Editor frontend selection, chosen before the window exists: 0 keeps the classic RmlUi
         * shell, 1 installs the editor's own ui_overlay instead.
         */
        std::int32_t editor_ui = 0;
        /**
         * Scale of the editor UI, 1.0 being the authored size. Zero follows the window's display
         * scale, which is what a project normally wants; a positive value pins it (150% -> 1.5F)
         * and also converts pointer input from window coordinates into the UI's own space.
         */
        float ui_scale = 0.0F;
    };

    static_assert(sizeof(startup_options) == 56);

    struct transform_value {
        vector_3 position;
        vector_3 rotation_degrees;
        vector_3 scale;
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

    // THE TOON LIGHT RIG (ABI 39): a lane-for-lane mirror of `deren::engine::toon_rig`, which is the
    // ten `float4`s the character stage reads every frame (scene block slot 750; see that struct for
    // what each lane means - `source/shaders/toon/toon_params.slang`'s `ToonRigUBO` is the other half
    // of the mirror). It crosses the ABI as a value so managed code can carry the whole art direction
    // in one number set instead of rebuilding it from twenty scalars. The defaults are the engine
    // struct's, so a lane a caller leaves alone behaves exactly as if the rig had never been set.
    struct toon_rig_desc {
        /// x = `_DayStrength` (0 = sun off, head light carries the character; 1 = full sun),
        /// y = `_OtherLightOffset`, z = `_OtherLightStrength`, w = `_OtherLightStrength_Offset`.
        float day[4] = {1.0F, 0.0F, 1.0F, 0.0F};
        /// x/y = `_OtherLightResultStrength_day0/day1`, z = `_SelfAoShadowStrength`, w = `_AoStrength`.
        float other_light[4] = {0.7F, 0.3F, 1.0F, 1.0F};
        /// rgb = `_OtherLightColor`; w = the face expression index (0..3, clamped by the host).
        float other_colour[4] = {0.60F, 0.65F, 0.80F, 0.0F};
        /// rgb = `_MainLightColor_dark` (the sun on a surface's shadow side); a = `_BaseColorPow`.
        float main_dark[4] = {0.60F, 0.65F, 0.80F, 1.0F};
        /// x = `_RimLightArea`, y = `_RimLightStrength`, z = `_RimLightNoLxzStrength`,
        /// w = `_RimLightDiffuseColorEffect`.
        float rim[4] = {1.0F, 1.0F, 1.0F, 0.1F};
        /// rgb = `_SssColor`; a = `_SssPowStrength`.
        float sss[4] = {1.0F, 0.80F, 0.78F, 1.0F};
        /// x = `_EnvLightStrength`, y = `_EnvRotation` in degrees, z = `_SpecularStrength`,
        /// w = `_DiffuseBlendEffect`.
        float env[4] = {1.0F, 0.0F, 1.0F, 1.0F};
        /// x = backlight weight, y = `_NoFStrength`, z = `_NoFPowStrength`, w = `_RampColorNoLStrength`.
        float misc[4] = {1.0F, 1.0F, 1.0F, 1.0F};
        /// x = the toon shadow softness ladder (`[render] toon_shadow_softness`, 0..4, rounded by the
        /// host); y/z/w = nose shadow reduction, hair highlight width and shift.
        float shadow_softness[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        /// x = authored hair P-mask mode, y = angular face SDF, both 0/1 (the host normalises any
        /// other value); z/w reserved and must stay 0.
        float reference_modes[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    };

    static_assert(sizeof(toon_rig_desc) == 160);

    // One frame of the host's CHARACTER ANIMATION PLAYER (ABI 36). The host keeps one player for the
    // scene and builds it from the first bound asset whose manifest names a motion file, so managed
    // code never owns the sampling, the skin matrices or the morph upload - it only decides WHICH
    // clip plays, from when, and how fast. `available` is 0 until such an asset is bound; the rest of
    // the struct is a snapshot taken once per frame, which keeps the query side lock-free.
    struct animation_state {
        std::uint32_t playable_count = 0; // clips the player holds
        std::uint32_t current = 0;        // index of the selected clip
        std::uint8_t has_active = 0;      // a clip is selected
        std::uint8_t playing = 0;         // the clock advances on each frame
        std::uint8_t available = 0;       // the host has a player at all
        std::uint8_t padding = 0;
        float time = 0.0F;     // seconds into the clip
        float duration = 0.0F; // the clip's looping length in seconds
        float speed = 0.0F;    // playback-rate multiplier (0..16)
    };

    static_assert(sizeof(animation_state) == 24);

    // One frame's renderer counters (ABI 38): the numbers the debug overlay shows, exposed so a
    // managed module can draw its own panel instead of parsing the native one. Sizes are in pixels;
    // `gpu_timings_available` is 1 when the device can timestamp passes, and `gpu_interval_count`
    // is how many intervals the timing table holds (0 when timing is off).
    struct diagnostic_counters {
        std::uint32_t render_width = 0;
        std::uint32_t render_height = 0;
        std::uint32_t output_width = 0;
        std::uint32_t output_height = 0;
        std::uint32_t gpu_interval_count = 0;
        std::uint8_t gpu_timings_available = 0;
        std::uint8_t padding[3] = {};
    };

    static_assert(sizeof(diagnostic_counters) == 24);

    struct scene_api {
        script_status (*get_transform)(node_handle node, transform_value* value) = nullptr;
        script_status (*set_transform)(node_handle node, transform_value const* value) = nullptr;
        script_status (*try_spawn)(std::uint64_t scene, utf8_view asset, transform_value const* value,
                                   node_handle* node) = nullptr;
        script_status (*try_despawn)(std::uint64_t scene, node_handle node) = nullptr;
        // Creates an empty managed scene tree. The returned id is independent of script scene ids.
        script_status (*create_scene_tree)(std::uint64_t* tree) = nullptr;
        // Hands a managed tree to the renderer for the current scene startup. The tree becomes
        // the runtime scene and must not be destroyed while active.
        script_status (*activate_scene)(std::uint64_t tree) = nullptr;
        script_status (*get_active_scene)(std::uint64_t* tree) = nullptr;
        // Detaches the active tree from the current script scene before it is destroyed. The
        // native renderer must not have begun a frame with this tree after the call returns.
        script_status (*deactivate_scene)(std::uint64_t tree) = nullptr;
        script_status (*destroy_scene_tree)(std::uint64_t tree) = nullptr;
        // Appends a node to a managed tree. A zero parent creates a root. The returned node id is
        // process-wide and stays valid until the node or one of its ancestors is destroyed.
        script_status (*create_node)(std::uint64_t tree, node_handle parent, utf8_view name,
                                     transform_value const* local, node_handle* node) = nullptr;
        script_status (*destroy_node)(std::uint64_t tree, node_handle node) = nullptr;
        // Local transform of a managed-tree node, stored in its placement component. These calls
        // do not consult a script scene's spawn slots.
        script_status (*get_node_transform)(std::uint64_t tree, node_handle node,
                                            transform_value* value) = nullptr;
        script_status (*set_node_transform)(std::uint64_t tree, node_handle node,
                                            transform_value const* value) = nullptr;
        script_status (*set_node_visible)(std::uint64_t tree, node_handle node,
                                          std::uint8_t visible) = nullptr;
        // Enumerates library asset ids in sorted order. The returned view borrows the library's
        // storage and stays valid until the library is rescanned. Out-of-range indexes return
        // asset_not_found.
        script_status (*list_asset)(std::uint32_t index, utf8_view* id) = nullptr;
        // The asset manager loads one library asset and returns a process-wide handle. The
        // source data stays alive while at least one handle is held.
        script_status (*load_asset)(utf8_view id, asset_handle* asset) = nullptr;
        script_status (*release_asset)(asset_handle asset) = nullptr;
        // Instantiates the whole asset as a subtree under parent (a zero parent creates a root).
        // The offset is the asset instance root's local transform.
        script_status (*bind_asset)(std::uint64_t tree, node_handle parent, asset_handle asset,
                                    transform_value const* offset, node_handle* root) = nullptr;
        // The managed game owns the camera pose. Calls fail if no scene is active.
        script_status (*get_camera)(camera_state* value) = nullptr;
        script_status (*set_camera)(camera_state const* value) = nullptr;
        // Replaces ONE image-based-lighting slot with the environment named by @p path: a baked .ibl or
        // an equirectangular .hdr, relative paths resolved against the executable directory. An empty
        // path loads the procedural sky into that slot. The call is a request: the host validates the
        // path, prepares the images and applies them between frames, and keeps the slot's current
        // environment if decoding fails.
        //
        // SLOT 0 IS THE SCENE-WIDE ENVIRONMENT - the one every material samples unless its record names
        // another slot - and it is also the only slot with a BRDF LUT (the LUT is shared, so a custom
        // slot reuses slot 0's). Slots 1..15 are the per-material lanes: a material record carries the
        // slot number in its flags (bits 10..13, `material_override.environment_slot` on the managed
        // side), and the shaders pick the slot's cubemap pair out of the descriptor heap
        // (`heap_access.slang`, `custom_env_cube + slot - 1`). `layout::heap_environment_capacity` is the
        // upper bound; a larger slot is rejected with `invalid_argument`.
        script_status (*set_environment_hdr)(utf8_view path, std::uint32_t environment_slot) = nullptr;
        // The asset manifest declares which renderer pipelines its meshes support and which one a
        // bind starts on. These return that declaration for parameter validation on the managed
        // side; the strings borrow the registry's storage while the handle is alive.
        script_status (*asset_pipeline_count)(asset_handle asset, std::uint32_t* count) = nullptr;
        script_status (*asset_pipeline)(asset_handle asset, std::uint32_t index, utf8_view* name) = nullptr;
        script_status (*asset_default_pipeline)(asset_handle asset, utf8_view* name) = nullptr;
        // Switches the primitives under a bind_asset() root to @p pipeline. Names outside that
        // asset's supported list are rejected with invalid_argument; a node that is not a bind
        // root is rejected with invalid_node. The host rewrites the imported leaves between frames.
        script_status (*set_pipeline)(std::uint64_t tree, node_handle node, utf8_view pipeline) = nullptr;
        // Post-processing and environment calibration as RUNTIME state (ABI 24). These replace the
        // startup-only [render]/[lighting] hand-off: the values are CPU-side and take effect on the
        // next frame, so a managed game can move them between frames. Each entry validates the
        // arguments and returns invalid_argument instead of clamping silently.
        script_status (*set_exposure)(float exposure) = nullptr;
        // 0 = none/linear, 1 = ACES, 2 = the character-forward grading (see tone_mapping in the shaders)
        script_status (*set_tone_mapping)(std::int32_t mode) = nullptr;
        // Rotation is in DEGREES and wraps; strength is 0..64, background is a mode 0..3, and the
        // background colour channels and blur are 0..1.
        script_status (*set_environment)(float rotation_degrees, float strength, std::int32_t background,
                                         float color_r, float color_g, float color_b, float blur) = nullptr;
        // 0..3, 1.0 = the shading path's constant unchanged.
        script_status (*set_sun_intensity)(float scale) = nullptr;
        // intensity 0..4 (0 disables), threshold 0..0.75.
        script_status (*set_bloom)(float intensity, float threshold) = nullptr;
        // subpixel 0..1, edge_threshold 0..1 (the shader's default is 0.166).
        script_status (*set_fxaa)(std::uint8_t enabled, float subpixel, float edge_threshold) = nullptr;
        // blend_static 0..0.99, blend_min 0..blend_static.
        script_status (*set_taa)(std::uint8_t enabled, float blend_static, float blend_min) = nullptr;
        // radius >= 0, intensity 0..1, samples 0..16 (the shader's array bound).
        script_status (*set_ssao)(std::uint8_t enabled, float radius, float intensity, std::uint32_t samples) = nullptr;
        script_status (*set_gbuffer_debug)(std::uint8_t enabled) = nullptr;
        // The direction points FROM the surface TOWARD the sun; it must be finite and non-zero.
        script_status (*set_sun_direction)(float x, float y, float z) = nullptr;
        // A square emitter: half <= 0 clears it (axis and penumbra are then ignored); a live one needs
        // a finite non-zero axis and penumbra >= 0. irradiance selects the energy mode.
        script_status (*set_area_light)(float centre_x, float centre_y, float centre_z, float half,
                                        std::uint8_t irradiance, float axis_x, float axis_y, float axis_z,
                                        float penumbra) = nullptr;
        // Flips the clustered-light path; false is the "few lights, forward loop" mode.
        script_status (*set_clustered_lights)(std::uint8_t enabled) = nullptr;
        // Replaces the WHOLE punctual light set (ABI 26). `count` may be 0, which clears every light;
        // a count above deren::engine::max_punctual_lights, or a null array with a non-zero count, is
        // rejected with invalid_argument rather than truncated. Each record is validated field by
        // field, so a non-finite value fails the call instead of reaching the light UBO.
        script_status (*set_point_lights)(point_light_desc const* lights, std::uint32_t count) = nullptr;
        // One node subtree's per-object material factors. The walk is the same one apply_pipeline
        // uses, so a hidden subtree is rewritten too; see material_override_desc for the semantics.
        script_status (*set_material_override)(std::uint64_t tree, node_handle node, material_override_desc const* value) = nullptr;
        // Frame pacing (ABI 28): 0 means uncapped, a positive value caps the render loop. A negative or
        // non-finite value is rejected; the runtime itself clamps anything it does accept.
        script_status (*set_max_fps)(double fps) = nullptr;
        // Cel shading for the toon stages: the number of diffuse bands (0 disables banding) and the
        // softness of each step (0..1). Band counts above 32 are rejected as a caller error.
        script_status (*set_toon_shading)(float steps, float softness) = nullptr;
        // Directional shadow mapping on or off. Turning it off clears the maps and drops the pass; the
        // shadow fit is recomputed when it comes back on.
        script_status (*set_shadow_enabled)(std::uint8_t enabled) = nullptr;
        // The face frame the loader measured for a loaded asset (ABI 29), or asset_invalid when that
        // asset has no usable face primitive. The managed side derives the head basis from it.
        script_status (*asset_face_frame)(asset_handle asset, face_frame_desc* value) = nullptr;
        // Publishes one game-toon character's head frame: the direction the face looks, its right and up,
        // and the head's world position. `index` must be below deren::engine::max_game_toon_characters
        // (16); a character that never publishes one keeps the renderer's glTF-basis fallback.
        script_status (*set_head_basis)(std::uint32_t index, head_basis_desc const* value) = nullptr;
        // OFFSCREEN CUBE CAPTURE (ABI 30): a managed game can render its scene six times from one
        // world position and write the result as a reflection-probe cube. The host only installs the
        // camera and runs the pass; the face loop, the frame cadence, and the output path are the
        // game's. enabled == 0 restores the orbit camera and turns the capture pass off (the other
        // arguments are then ignored). face is 0..5 in the engine's cube order; near/far must
        // bracket the scene.
        script_status (*set_capture_camera)(std::uint8_t enabled, float const position[3], std::uint32_t face,
                                            float near_plane, float far_plane) = nullptr;
        // Arms the readback of the next presented frame. Success means it is armed; the pixels are
        // collected once with acquire_radiance_image. A second request before that collection is
        // invalid_argument rather than a silently dropped image.
        script_status (*request_radiance_capture)() = nullptr;
        // Copies the armed image out as width*height RGBA float pixels (4 floats per pixel). The caller
        // owns the buffer; a capacity that does not match the image is invalid_argument, and nothing
        // armed (or a failed readback) is script_failure. The pixels are consumed by the call.
        script_status (*acquire_radiance_image)(std::uint32_t* width, std::uint32_t* height, float* rgba,
                                                std::uint32_t float_count) = nullptr;
        // Drops the accumulated temporal history. The face loop needs it: the next face is a different
        // camera, and a history carried across faces would blend one into the next.
        script_status (*reset_temporal_history)() = nullptr;
        // Writes a captured cube (six faces in the engine's face order, each size*size*4 floats) as the
        // engine's IBL1 probe file at @p path. Called once, after all six faces are collected.
        script_status (*write_radiance_cube)(utf8_view path, std::uint32_t size, float const* pixels,
                                             std::uint32_t float_count) = nullptr;
        // ABI 36/46: THE ANIMATION PLAYERS. A placement of an animated asset owns its own player: the
        // host samples the clip, writes the skin matrices and uploads the morph deltas between frames.
        // ABI 46 addresses that player by the scene tree and the PLACEMENT node (@p node), so two
        // placements of the same character answer independently; ABI 45 and older had a single
        // tree-less player. The entries only choose the clip and the clock.
        // Every call reports scene_not_found for a node that owns no animated placement, and
        // select/set_* report invalid_argument for a clip index or a value the player cannot take.
        script_status (*get_animation_state)(std::uint64_t tree, node_handle node, animation_state* out) = nullptr;
        script_status (*animation_playable_count)(std::uint64_t tree, node_handle node, std::uint32_t* count) = nullptr;
        // The clip name borrows the player's storage and stays valid until the player is rebuilt.
        script_status (*animation_playable_name)(std::uint64_t tree, node_handle node, std::uint32_t index,
                                                 utf8_view* name) = nullptr;
        script_status (*select_animation)(std::uint64_t tree, node_handle node, std::uint32_t index) = nullptr;
        script_status (*set_animation_playing)(std::uint64_t tree, node_handle node, std::uint8_t playing) = nullptr;
        script_status (*set_animation_speed)(std::uint64_t tree, node_handle node, float speed) = nullptr;
        // Scrubbing: sets the clip time in seconds and pauses, so a GUI timeline drag does not fight
        // the advancing clock. A time outside [0, duration] is clamped.
        script_status (*set_animation_time)(std::uint64_t tree, node_handle node, float seconds) = nullptr;
        // ABI 38: TREE QUERIES. create_node() already takes a name and a parent, but until now a
        // managed module could not read either back, so every subtree it built had to be remembered
        // in its own side table. These entries make the scene tree itself the source of truth.
        // Children are addressed by index in the parent's own order; the count is 0 for a leaf.
        script_status (*node_child_count)(std::uint64_t tree, node_handle node, std::uint32_t* count) = nullptr;
        script_status (*node_child)(std::uint64_t tree, node_handle node, std::uint32_t index,
                                    node_handle* child) = nullptr;
        // The node name borrows the node's own storage and stays valid until the node is renamed or
        // destroyed; an unnamed node reports an empty view.
        script_status (*node_name)(std::uint64_t tree, node_handle node, utf8_view* name) = nullptr;
        // ABI 38: the opaque 32-bit gameplay word the renderer never interprets (see scene_node).
        // set_node_flags is a masked write: only the bits set in @p mask change.
        script_status (*get_node_flags)(std::uint64_t tree, node_handle node, std::uint32_t* flags) = nullptr;
        script_status (*set_node_flags)(std::uint64_t tree, node_handle node, std::uint32_t flags,
                                        std::uint32_t mask) = nullptr;
        // ABI 38: moves @p node (and its subtree) under @p parent, or among the roots when
        // @p parent is a null handle. keep_transform != 0 rewrites the local transform so the node
        // keeps the WORLD pose it had before the move; 0 keeps the local transform, which changes
        // the world pose. The node keeps its handle and every component; moving a node into its own
        // subtree is invalid_argument (that would carry the new parent away with it).
        script_status (*reparent_node)(std::uint64_t tree, node_handle node, node_handle parent,
                                       std::uint8_t keep_transform) = nullptr;
        // ABI 38: VIEW/SCREEN MAPPING. viewport_size is the RENDER extent in pixels - the size the
        // shading passes ran at, i.e. with the render scale already applied. It is NOT the window's
        // or the presented image's size (the host answers the render extent, see run.cpp).
        // project_point maps a world position to {x, y, view_depth}: x and y are pixels of that same
        // render grid with
        // the origin at the top-left, and view_depth is the view-space w, positive in front of the
        // camera (x/y are still written when it is not, but they are meaningless there). screen_ray
        // is the inverse: a pixel position becomes the camera position and a normalised world-space
        // direction. Both use the same unjittered matrices the frame's motion vectors use, so they
        // ignore the TAA jitter and the Vulkan y flip is already accounted for.
        script_status (*viewport_size)(std::uint32_t* width, std::uint32_t* height) = nullptr;
        script_status (*project_point)(float const* world, float* screen) = nullptr;
        script_status (*screen_ray)(float x, float y, float* origin, float* direction) = nullptr;
        // ABI 38: the renderer's own counters (see diagnostic_counters) and the debug overlay's
        // per-pass GPU timing text. The text borrows the runtime's last completed window and stays
        // valid until the next frame, so copy it if it must outlive the callback.
        script_status (*query_counters)(diagnostic_counters* counters) = nullptr;
        script_status (*gpu_timing_summary)(utf8_view* text) = nullptr;
        // ABI 39: THE TOON LIGHT RIG at runtime. `set_toon_rig` replaces the whole block; the host
        // refuses a non-finite lane or a reserved lane that is not zero with invalid_argument,
        // normalises the two flags and the two clamped lanes (see toon_rig_desc), and applies the
        // value between frames - the block may not be written while a frame is in flight, so a
        // request is queued and the newest one wins. `get_toon_rig` answers what the next frame will
        // use: the queued value while one is pending, otherwise the published rig (the `[toon]`
        // profile at startup). Neither call needs an active scene; they answer scene_not_found before
        // the host owns one.
        script_status (*set_toon_rig)(toon_rig_desc const* rig) = nullptr;
        script_status (*get_toon_rig)(toon_rig_desc* rig) = nullptr;
        // ABI 40: RENAMING A NODE. `set_node_name` replaces the name the node was created with. An
        // empty name, a name longer than 256 UTF-8 bytes, a name whose bytes carry a NUL, or a null
        // data pointer with a non-zero length is invalid_argument; handle 0 or a node that does not
        // live in the tree is invalid_node, and an unknown tree is scene_not_found. Names are not
        // unique: a lookup returns the first node in depth-first order, so a rename may leave two
        // nodes with the same name and never renames the duplicate. A rename only assigns the node's
        // own string - the engine keeps no name index, lookups just walk the children - so nothing
        // else needs refreshing. The view node_name() hands out borrows the node's own string, so a
        // successful rename invalidates every view obtained before it.
        script_status (*set_node_name)(std::uint64_t tree, node_handle node, utf8_view name) = nullptr;
        // ABI 41: THE PROJECTION ASPECT. `set_scene_aspect` tells the renderer the shape of the
        // rectangle the game shows the scene in. The render still covers the whole window; only the
        // projection changes - the world is built for that aspect, so when the UI stretches the
        // image into the rectangle the geometry looks right. A finite aspect greater than zero sets
        // it; 0 restores the presentation extent's aspect; anything else is invalid_argument. The
        // value applies from the next frame, and project_point/screen_ray answer in that same
        // projection. No active scene or no host yet is scene_not_found.
        script_status (*set_scene_aspect)(float aspect) = nullptr;
        // ABI 43: THE APPLICATION WINDOW. A borderless window has no system caption, so the project's
        // own UI draws the minimise/maximise/close buttons and the drag area. `window_minimize` and
        // `window_toggle_maximize` change the window state; `window_is_maximized` writes 0 or 1;
        // `window_close` requests the same shutdown the system close button does; `window_begin_drag`
        // hands a caption press to the system move loop and BLOCKS until the button is released.
        // A null out-pointer is invalid_argument. No window (an offscreen capture), a host built
        // before these entries, and a platform without the move loop all answer module_unavailable,
        // so a managed UI can tell "this build cannot move the window" apart from "the call failed".
        // A still-pending frame is fine - these act on the window, not on the scene, so they need no
        // active scene and never touch frame data.
        script_status (*window_minimize)() = nullptr;
        script_status (*window_toggle_maximize)() = nullptr;
        script_status (*window_is_maximized)(std::uint8_t* maximized) = nullptr;
        script_status (*window_close)() = nullptr;
        script_status (*window_begin_drag)() = nullptr;
    };
    // ONE BOX OF THE COLLISION WORLD (ABI 45). The box is stored in WORLD space: centre, one unit axis
    // per local direction and the world half extents along them. The kernel works in world space on
    // purpose - a rotated or non-uniformly scaled document node would turn a capsule into an ellipsoid
    // in the node's own frame, so the host never sees box-space shapes. `id` is the caller's number and
    // is reported back unchanged; `enabled` 0 keeps the box in the set but out of every query, so a
    // caller can toggle a door without rebuilding the world.
    struct collision_box_desc {
        float center[3] = {0.0F, 0.0F, 0.0F};
        float half[3] = {0.5F, 0.5F, 0.5F};
        float axis[9] = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
        std::uint32_t id = 0;
        std::uint8_t enabled = 1;
        std::uint8_t padding[3] = {0, 0, 0};
    };

    static_assert(sizeof(collision_box_desc) == 68);

    // THE SWEPT SHAPE (ABI 45): a capsule is the segment lower..upper dilated by `radius`; a sphere is
    // lower == upper. `padding` keeps the size a multiple of 8 so the mirror stays trivially checkable.
    struct collision_capsule_desc {
        float lower[3] = {0.0F, 0.0F, 0.0F};
        float upper[3] = {0.0F, 1.0F, 0.0F};
        float radius = 0.3F;
        float padding = 0.0F;
    };

    static_assert(sizeof(collision_capsule_desc) == 32);

    // ONE SWEEP'S RESULT (ABI 45). time is the fraction of the requested displacement travelled before
    // the first contact (1 when nothing was hit, 0 when the start already overlaps); point is the closest
    // point on the box at that time, normal the contact direction to separate along, depth how far the
    // start was already inside (positive only when time == 0), and id the collider that answered.
    struct collision_hit_desc {
        std::uint8_t hit = 0;
        std::uint8_t padding[3] = {0, 0, 0};
        float time = 1.0F;
        float point[3] = {0.0F, 0.0F, 0.0F};
        float normal[3] = {0.0F, 1.0F, 0.0F};
        float depth = 0.0F;
        std::uint32_t id = 0;
    };

    static_assert(sizeof(collision_hit_desc) == 40);

    // ONE SLIDE'S RESULT (ABI 45): the displacement actually travelled (shorter than requested when a
    // contact blocked it), the last contact normal, its id, how many contacts the loop used, whether
    // anything blocked it, and whether a contact surface was walkable (grounded).
    struct collision_move_desc {
        float displacement[3] = {0.0F, 0.0F, 0.0F};
        float normal[3] = {0.0F, 1.0F, 0.0F};
        std::uint32_t id = 0;
        std::int32_t contacts = 0;
        std::uint8_t blocked = 0;
        std::uint8_t grounded = 0;
        std::uint8_t padding[2] = {0, 0};
    };

    static_assert(sizeof(collision_move_desc) == 36);

    // ONE FLOOR PROBE (ABI 45): hit is 1 when a box was met within the requested distance; distance is
    // measured from the capsule's LOWEST point along the probe (so it already excludes the radius),
    // point and normal describe the contact, walkable applies the host's floor-angle threshold, and id
    // names the box.
    struct collision_floor_desc {
        std::uint8_t hit = 0;
        std::uint8_t walkable = 0;
        std::uint8_t padding[2] = {0, 0};
        float distance = 0.0F;
        float point[3] = {0.0F, 0.0F, 0.0F};
        float normal[3] = {0.0F, 1.0F, 0.0F};
        std::uint32_t id = 0;
    };

    static_assert(sizeof(collision_floor_desc) == 36);

    // THE COLLISION WORLD (ABI 45). Managed code owns the policy - which boxes exist, how the capsule is
    // tuned, substepping, step-up and floor snapping - and the host owns only this set plus the kernel
    // calls. A world belongs to the active scene: after the scene changes, the managed side rebuilds it.
    // Every function answers scene_not_found when no scene is active and invalid_argument for a null
    // pointer, a non-finite value, a non-positive half extent, axes that are not unit length, or more
    // boxes than the world accepts.
    struct collision_api {
        // Removes every box. Called before a rebuild, not per frame.
        script_status (*world_reset)() = nullptr;
        // Adds a box, or replaces the one with the same id (a moving solid updates its box instead of
        // rebuilding the world). `descriptor` is borrowed for the call.
        script_status (*add_box)(collision_box_desc const* descriptor) = nullptr;
        // Removes the box with @p id; removing an id that is not there is success (a rebuild may race
        // the removal of a box it already dropped).
        script_status (*remove_box)(std::uint32_t id) = nullptr;
        // Sweeps the capsule by @p displacement (a 3-float array, borrowed). The answer is success with
        // `hit == 0` when nothing was in the way, so a caller does not have to treat a clear path as an
        // error.
        script_status (*sweep_capsule)(collision_capsule_desc const* capsule, float const* displacement,
                                       collision_hit_desc* out_hit) = nullptr;
        // Sweeps, slides along what it hits and reports the total displacement. @p walkable_normal_y is
        // the minimum upward normal component that counts as ground (0.71 is 45 degrees-ish, matching
        // the engine default).
        script_status (*move_capsule)(collision_capsule_desc const* capsule, float const* displacement,
                                      float walkable_normal_y, collision_move_desc* out_move) = nullptr;
        // Probes downwards for the nearest box within @p max_distance and answers whether its normal is
        // walkable. `hit == 0` means nothing was within reach (an edge or the air).
        script_status (*find_floor)(collision_capsule_desc const* capsule, float max_distance,
                                    float walkable_normal_y, collision_floor_desc* out_floor) = nullptr;
    };

    // ONE FRAME'S PANEL CONTENT (ABI 31). The managed game describes its debug panel every frame and the
    // host turns each call into the overlay plugin's widget. Every pointer is borrowed until the frame is
    // presented and points at the caller's own storage, so a control edits the game's state directly and
    // no per-widget callback has to cross the boundary.
    struct ui_api {
        // Opens the frame's panel by title (creating it on first use) and empties its widget list.
        // default_width/height are the first-use size; 0 keeps the overlay's auto-size. A repeated or
        // nested begin_panel before end_panel is invalid_argument.
        script_status (*begin_panel)(utf8_view title, float default_width, float default_height) = nullptr;
        script_status (*end_panel)() = nullptr;
        // A line of text the game already formatted; the host never interprets it.
        script_status (*text)(utf8_view text) = nullptr;
        // Appends a control bound to the caller's storage. A checkbox value is a byte holding 0 or 1;
        // a button writes 1 into its byte when pressed, and the caller clears it.
        script_status (*checkbox)(utf8_view label, std::uint8_t* value) = nullptr;
        script_status (*slider)(utf8_view label, float* value, float minimum, float maximum) = nullptr;
        script_status (*vec3)(utf8_view label, float* value, float speed) = nullptr;
        script_status (*combo)(utf8_view label, utf8_view const* items, std::uint32_t count,
                               std::int32_t* selected) = nullptr;
        script_status (*button)(utf8_view label, std::uint8_t* pressed) = nullptr;
        // Whether the overlay currently captures the mouse (a panel is focused or being dragged).
        script_status (*wants_mouse)(std::uint8_t* value) = nullptr;
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
        std::int32_t button = 0; // mouse_button: the backend's mouse button index
        std::uint8_t down = 0;   // mouse_button and key: 1 pressed, 0 released
        std::uint8_t padding[3] = {};
        std::int32_t key = 0; // key: the backend's key code, as the window reports it
        float x = 0.0F;       // mouse: cursor position in window coordinates
        float y = 0.0F;
        float dx = 0.0F; // mouse_move: movement since the previous event
        float dy = 0.0F;
        float wheel = 0.0F;          // scroll: vertical wheel delta
        std::uint32_t codepoint = 0; // character: one Unicode code point
        // key and character: GLFW modifier bits (shift 1, control 2, alt 4, super 8). A text field
        // needs them to tell "c" from Ctrl+C, and the window already reports them.
        std::uint8_t modifiers = 0;
        std::uint8_t repeated = 0; // key: 1 when the OS repeated the key instead of pressing it
        std::uint8_t padding2[2] = {};
    };

    static_assert(sizeof(input_event) == 44);

    // ABI 33: the structs both sides pass by value and pointer, whose byte size the managed side
    // re-checks before it binds. The version number alone only catches an ABI both sides know they
    // disagree about; these entries also catch a field added, removed, or reordered without a
    // version bump, which used to surface as a call through a wrong signature. The module tables
    // (scene_api, ui_api) are not listed here: get_module() reports their size instead, so each
    // module checks its own table where it consumes it. rml_ui_event is listed because the rml_ui
    // table passes it by pointer, so its size is the one such payload the table itself cannot
    // describe. Never renumber a value once shipped.
    enum class abi_type : std::uint32_t {
        native_api = 1,
        managed_api = 2,
        frame_info = 3,
        transform_value = 4,
        input_event = 5,
        point_light_desc = 6,
        material_override_desc = 7,
        startup_options = 8,
        head_basis_desc = 9,
        face_frame_desc = 10,
        animation_state = 11,
        diagnostic_counters = 12,
        toon_rig_desc = 13,
        // ABI 42: the pointer-event payload of the rml_ui table (28 bytes).
        rml_ui_event = 14,
        // ABI 45: the collision module's payloads (see collision_api).
        collision_box_desc = 15,
        collision_capsule_desc = 16,
        collision_hit_desc = 17,
        collision_move_desc = 18,
        collision_floor_desc = 19
    };

    struct abi_layout_entry {
        abi_type type = abi_type::native_api;
        std::uint32_t size = 0;
    };

    static_assert(sizeof(abi_layout_entry) == 8);

    enum class native_module : std::uint32_t {
        scene = 1,
        // ABI 31: the debug panel and the overlay state (ui_api).
        ui = 2,
        audio = 3,
        rml_ui = 4,
        node_bounds = 5,
        rml_ui_extended = 6,
        asset_authoring = 7,
        // ABI 43: module 7 plus the environment-map picker. Appended under its own id rather than grown
        // in place so a Game.dll built against module 7 keeps working: it never asks for this table.
        asset_authoring_extended = 8,
        // ABI 45: the capsule collision world. The kernel (deren.engine.collision) is stateless, so the
        // host owns the box set and the managed side rebuilds it whenever its collision inputs change.
        collision = 9,
        // ABI 47: the editor's own immediate-mode frontend (editor_ui_api). Distinct from
        // rml_ui because the classic shell never installs it.
        editor_ui = 10
    };

    // Keep layouts and calling conventions in sync with managed_bootstrap.cs.
    struct native_api {
        void (*log)(char const* utf8, std::uint32_t length) = nullptr;
        // ABI 33: @p byte_size receives sizeof() of the module table @p table points at, so the
        // managed side compares the host's layout against its own mirror instead of trusting it.
        script_status (*get_module)(std::uint32_t module, void const** table, std::uint32_t* byte_size) = nullptr;
        // Requests the same one-shot screenshot path as the F12 key. The next recorded frame
        // copies the presented image, and the renderer saves it under [paths] screenshot_dir with
        // a timestamped file name. Null before initialize; projects call it through
        // deren.screenshot.
        script_status (*request_screenshot)() = nullptr;
        // ABI 31: raw window input. The host calls @p callback on the render thread for every event
        // the window reports and passes @p context back unchanged; passing a null callback stops the
        // forwarding. The game owns the meaning of the input, the host only reports it.
        void (*set_input_event_callback)(void (*callback)(void* context, input_event const* event), void* context) = nullptr;
        // ABI 33: writes one {type, sizeof} entry per mirrored type and returns success; a null
        // @p out or a @p capacity below the number of entries is invalid_argument rather than a
        // truncated answer. The managed side refuses to bind when any entry disagrees with its own
        // struct layout, which turns a forgotten field into "module_invalid" at startup.
        script_status (*abi_layout)(abi_layout_entry* out, std::uint32_t capacity) = nullptr;
        // ABI 34: the managed side reports that the frame-boundary work the host handed it has
        // finished, which releases the wait the host entered after before_frame/after_frame.
        // Exactly one call answers each signal; a call with nothing outstanding is
        // invalid_argument so an out-of-step managed side is visible in the log instead of
        // silently borrowing the next frame's wait.
        script_status (*notify_rendering)() = nullptr;
        // ABI 35: how long the host waits for one frame boundary before it treats the managed host
        // as dead. 0 waits indefinitely, which is what a debugger breakpoint or a deliberately long
        // blocking task needs; anything else is milliseconds, capped so an absurd value cannot turn
        // a stuck boundary into an apparently frozen process. Null (an older host) keeps 5000 ms.
        script_status (*set_frame_boundary_timeout)(std::uint32_t milliseconds) = nullptr;
        // ABI 37: the managed side reports a fault it cannot continue from, and the host answers it
        // by writing one crash report and terminating the process - the same sink, the same report
        // shape and the same exit as deren::utility::panic, so a reader does not have to know which
        // language faulted. The strings are borrowed for the duration of the call. A host that
        // returns success took the report (only a test host does); a host that is gone, or that
        // answers any other status, leaves the managed side to write stderr and fail fast on its
        // own.
        script_status (*panic)(utf8_view message, utf8_view source_file, std::uint32_t line,
                               utf8_view managed_stack) = nullptr;
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

    // ABI 34: the two frame boundaries are plain void callbacks rather than a signal number,
    // because the host now waits for managed code to report completion (native_api
    // notify_rendering) and one signal per boundary needs no payload. Both are optional; a null
    // callback means the boundary costs nothing. The host never executes a managed callable
    // itself: managed code owns the queue behind these callbacks.
    struct managed_api {
        // The managed game owns its scene and script lifetime. The host only forwards frames
        // and asks for one program-wide shutdown.
        // The frame and its input bytes are borrowed only during the update.
        script_status (*update)(frame_info const* frame) = nullptr;
        script_status (*stop)() = nullptr;
        // Emitted before the frame is recorded (after the host has decided to draw it) and again
        // after it is presented. Managed code dispatches its collected callables here and calls
        // notify_rendering once the batch is done, which is what releases the host.
        void (*before_frame)() = nullptr;
        void (*after_frame)() = nullptr;
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
