// ============================================================================
// module: deren.engine.primitive:records  (the GPU scene records of
//         deren.engine.primitive: camera / light / material inputs and the
//         geometry handles the drawable primitives separate)
//
// This partition is one of the three domains of deren.engine.primitive; the
// top-level facade re-exports it, so consumers keep importing
// deren.engine.primitive.
// ============================================================================

module;

#include <cstddef>
#include <glm/glm.hpp>

export module deren.engine.primitive:records;

import deren.promise.rhi;
import deren.engine.subtilis.dag_import;
import deren.utility.shared;
import deren.vstd;

namespace deren::engine {
    export class geometry_buffer {
        deren::promise::rhi::buffer* view = nullptr;
        std::shared_ptr<void const> parent;

    public:
        geometry_buffer() = default;
        geometry_buffer(geometry_buffer const&) = delete ("geometry_buffer is non-copyable: pass it by reference or construct a new instance");
        geometry_buffer& operator=(geometry_buffer const&) = delete ("geometry_buffer is non-copyable: modify the existing instance instead");
        geometry_buffer(geometry_buffer&& other) noexcept
            : view(std::exchange(other.view, nullptr))
            , parent(std::move(other.parent)) {
        }
        geometry_buffer& operator=(geometry_buffer&& other) noexcept {
            if (this != &other) {
                view = std::exchange(other.view, nullptr);
                parent = std::move(other.parent);
            }
            return *this;
        }
        geometry_buffer& operator=(deren::promise::rhi::unique_object<deren::promise::rhi::buffer>&& value) {
            auto shared = std::make_shared<deren::promise::rhi::unique_object<deren::promise::rhi::buffer>>(std::move(value));
            bind(shared->get(), shared);
            return *this;
        }
        void bind(deren::promise::rhi::buffer* value, std::shared_ptr<void const> access) {
            view = access ? value : nullptr;
            parent = std::move(access);
        }
        void reset() noexcept {
            view = nullptr;
            parent.reset();
        }
        deren::promise::rhi::buffer* get() const noexcept {
            return view;
        }
        deren::promise::rhi::buffer* operator->() const noexcept {
            return view;
        }
        deren::promise::rhi::buffer& operator*() const noexcept {
            return *view;
        }
        explicit operator bool() const noexcept {
            return view != nullptr;
        }
        std::shared_ptr<void const> access() const noexcept {
            return parent;
        }
    };
    /**
     * @ingroup engine_primitive
     * @brief camera UBO content, layout matches the CameraUBO block in source/shaders/shading.glsl (no model
     *        matrix: the per-primitive world transform lives in the push constants instead,
     *        so the camera UBO can be shared by every primitive)
     * @note `proj` is the CURRENT frame's projection INCLUDING the TAA jitter when temporal
     *       anti-aliasing is on (the geometry must be sampled at the jittered offsets), while
     *       `view_proj_unjittered` and `prev_view_proj` are the unjittered pair the G-buffer's motion
     *       vectors are computed from: a jitter that leaked into the velocity would be read as camera
     *       motion and the history would be reprojected to the wrong place every frame.
     */
    export struct camera_ubo {
        glm::mat4 view;
        glm::mat4 proj; // jittered when TAA is on (see the note above)
        glm::vec3 camera_pos;
        float padding = 0.0f;
        glm::mat4 view_proj_unjittered; // current view * projection, jitter removed
        glm::mat4 prev_view_proj;       // the previous RENDERED frame's view * projection
    };
    // std140 layout check (same style as light_ubo below): two mat4, a vec4-aligned position, then
    // the two unjittered matrices the motion vectors read
    static_assert(sizeof(camera_ubo) == 4 * sizeof(glm::mat4) + sizeof(glm::vec4));
    static_assert(offsetof(camera_ubo, proj) == sizeof(glm::mat4));
    static_assert(offsetof(camera_ubo, camera_pos) == 2 * sizeof(glm::mat4));
    static_assert(offsetof(camera_ubo, view_proj_unjittered) == 2 * sizeof(glm::mat4) + sizeof(glm::vec4));
    static_assert(offsetof(camera_ubo, prev_view_proj) == 3 * sizeof(glm::mat4) + sizeof(glm::vec4));

    /**
     * @ingroup engine_primitive
     * @brief one user-configurable punctual light (API surface of runtime::set_point_lights).
     *        Point lights are omni-directional; a spot light additionally restricts its cone to
     *        @p spot_direction with a soft edge whose OUTER half-angle cosine is
     *        @p spot_outer_cos and whose INNER half-angle cosine is @p spot_inner_cos
     *        (optional: when unset, the CPU derives the legacy soft inner cone as
     *        mix(outer, 1, 0.6)).
     * @note intensity/range are ARTISTIC units, not physical: the shader uses inverse-square
     *       falloff 1/(1+d^2) (well-behaved at zero distance) with a smooth range fade
     *       (1-(d/r)^2)^2 - both differ from the physical/Khronos forms (1/d^2,
     *       (1-(d/r)^4)^2), which are unbounded/too harsh for the demo's scales.
     * @note glTF KHR_lights_punctual spot innerConeAngle maps to @p spot_inner_cos (imported by
     *       the demo main); the shader smoothsteps outer->inner over the cone cosine.
     */
    export struct punctual_light {
        glm::vec3 position = glm::vec3(0.0f);                    // world position
        float range = 10.0f;                                     // 0 = infinite falloff, otherwise smooth cutoff
        glm::vec3 color = glm::vec3(1.0f);                       // linear light color
        float intensity = 1.0f;                                  // radiance scale (color * intensity)
        bool spot = false;                                       // false = point light (omni)
        glm::vec3 spot_direction = glm::vec3(0.0f, -1.0f, 0.0f); // spot axis (normalized when spot)
        float spot_outer_cos = -0.2f;                            // cos of the outer cone half-angle (spot only)
        std::optional<float> spot_inner_cos = std::nullopt;      // cos of the inner cone half-angle (spot only); nullopt = legacy mix(outer, 1, 0.6)
        // Captured GameToon viewer rig payload, carried through the existing point_light lanes:
        // colour.w = mode (0 wrap, 2 specular, 3 rim), spot_dir.w = nprData.x, position.w = nprData.y,
        // and for a POINT light params.z/params.w = nprData.z/ nprData.w (a spot keeps its cone cosines there).
        float npr_mode = 0.0f;
        glm::vec4 npr_data = glm::vec4(0.0f);
    };
    /** @brief max simultaneous punctual lights (LightUBO.punctual_lights / GLSL PunctualLight array)
     * @note 128 lights keep the light UBO at 8576 bytes, inside the 16384-byte
     *       VkPhysicalDeviceLimits::maxUniformBufferRange every implementation guarantees; the
     *       clustered path (see the cluster_* constants below) is what makes that many affordable
     *       per pixel - a brute-force loop over 128 lights would be the whole frame budget. */
    export constexpr std::uint32_t max_punctual_lights = 128;
    /** @brief clustered light culling (M5): the screen is cut into tiles of this many pixels */
    export constexpr std::uint32_t cluster_tile_size = 64;
    /** @brief number of exponential depth slices per tile (the cluster grid's z dimension) */
    export constexpr std::uint32_t cluster_slice_count = 16;
    /** @brief cluster grid capacity: tiles_x * tiles_y * slices clusters are allocated; a larger
     *         screen clamps its tile count to this capacity (2048x1536 at a 64 px tile) */
    export constexpr std::uint32_t max_cluster_tiles_x = 32;
    export constexpr std::uint32_t max_cluster_tiles_y = 24;
    /** @brief lights one cluster can contain; a cluster that overflows keeps the first
     *         @ref cluster_light_capacity lights the compute pass found (see its shader) */
    export constexpr std::uint32_t cluster_light_capacity = 32;
    /** @brief allocated cluster count (the per-slot count/index buffers are sized for this) */
    export constexpr std::uint32_t max_cluster_count = max_cluster_tiles_x * max_cluster_tiles_y * cluster_slice_count;
    /** @brief cascaded shadow maps: the light UBO carries one view-projection per cascade, and the
     *         shadow map is a 2D ARRAY depth texture with this many layers (see light_ubo below) */
    export constexpr std::uint32_t max_shadow_cascades = 4;
    /** @brief one punctual light in the GPU light UBO (std140, 64 bytes; mirror PunctualLight in pbr.frag) */
    export struct point_light {
        glm::vec4 position = {}; // xyz: world position (w unused)
        glm::vec4 color = {};    // xyz: linear color * intensity (w unused)
        glm::vec4 spot_dir = {}; // xyz: spot axis, normalized when the light is a spot (w unused)
        glm::vec4 params = {};   // x = range (0 = infinite), y = 0 point / 1 spot, z = cos(outer cone), w = cos(inner cone, spot only)
    };

    /**
     * @ingroup engine_primitive
     * @brief light UBO content, layout matches the LightUBO block in source/shaders/shading.glsl and
     *        shadow.vert (scene block slot 7): the per-cascade light-space view-projections, the
     *        light direction, the cascade ranges/texel sizes, then the punctual light array
     * @note the directional sun is built from the scene bounds (enable_shadows) and the shadow
     *       map samples agree on its direction; punctual lights never cast shadows and ride the
     *       same block after the directional header
     * @note CASCADES: `light_view_proj[0]` covers the near range, the following entries the ranges
     *       given by `cascade_splits` (view-space far distance). A frame with cascade_count == 1 is
     *       exactly the single-map behavior this used to have - one fit over the whole visible
     *       range - which is what makes the cascaded version an A/B rather than a rewrite.
     */
    /**
     * @ingroup engine_primitive
     * @brief the FACE SDF's HEAD FRAME (scene block slot 749), one block per frame slot
     *
     * WHY IT IS its BLOCK AND NOT A FIELD OF THE CAMERA'S, which would have been the smaller change: the
     * head frame is a property of the CHARACTER rather than of the eye looking at it. A renderer that put it in
     * the camera UBO would have one more thing to unpick the day a scene holds two characters facing different
     * ways - and the SDF's whole premise is that a face is shaded in its frame rather than in anyone else's.
     *
     * THREE `vec4`s RATHER THAN THREE `vec3`s, so the layout is unambiguous: a std430 `vec3` has a 16-byte
     * stride and a std140 one does too, but a struct of three of them is the kind of thing that agrees by
     * accident until someone reorders it. The fourth component is unused and the shader reads `.xyz`.
     *
     * ---- AND A FOURTH `vec4` FOR `headCenter`, WHICH STEP 7 ADDED AND WHICH IS A POSITION RATHER THAN AN AXIS ----
     *
     * `Recalculate normal` needs `normalize(posWS - headCenter)`, and `headCenter` is one of the four attributes
     * the reference's geometry-node tree `脸方向向量` publishes (`存储已命名属性.003 <- 物体信息(HC).Location`, i.e.
     * the HC OBJECT'S WORLD TRANSLATION - spec §5.1). THE THREE AXES ABOVE DO NOT CARRY IT: `head_basis_from_axes`
     * takes two of the bone matrix's ROWS and never reads its translation column, which was the right answer while
     * the only consumer was a direction and is the wrong one now. So the block grew by one `vec4` filled from the
     * SAME bone world matrix's `[3]`, and the three members above keep their names, their order and their offsets -
     * a stale STRIDE here is the failure class this project keeps recording, so `sizeof` is asserted and the
     * shader's copy is the same four members in the same order.
     */
    export struct head_ubo {
        /// the direction the face looks. **`.w` IS A FLAG AND NOT PADDING**: `1.0` means the two axes are the
        /// CAPTURED VIEWER'S FACE FRAME (`Q324`/`Q80`, measured from the face mesh's UV side bands and averaged
        /// normal - see `deren::gltf::face_frame`), `0.0` means they are the old bone/glTF frame. It tells
        /// `game_toon_skin_face_shade` which `Q80` convention to use without adding a member (the block is 64 bytes
        /// and its shader copy has no spare lane either): with the captured frame up, `right` IS `Q80`; without it,
        /// `right` is the head bone's right and the fallback keeps the historical `-right` derivation.
        glm::vec4 front = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
        glm::vec4 right = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f); // its right (the captured `Q80` when `front.w == 1`)
        glm::vec4 up = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);     // its up
        /// the reference's `headCenter`: the head object's WORLD POSITION, `posWS - this` being the sphere normal
        /// `Recalculate normal` builds. `.w` IS A FLAG AND NOT PADDING: `1.0` means "a centre is known" and `0.0`
        /// (the default) means "this model has no head bone, so there is no `HC` object to read one from" - and
        /// the SHADER then declines the sphere term and keeps the socket's `interface[]` default
        /// (`sphereNormal_Strength = 0.0`), which is what a material that states nothing gets in Goo. A `(0,0,0)`
        /// centre is NOT a neutral value for this socket: it is the WORLD ORIGIN, and `normalize(posWS)` is a
        /// radial vector from it rather than a head normal. MEASURED on the asset this step ports: every character
        /// glb in this repository is baked (8 nodes, 0 skins, `Skeleton` gone), so every one of them takes the
        /// `0.0` branch - see `deren-ab/goo_step7_result.md` §7.
        glm::vec4 center = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    };
    // 64: four `vec4`s and no padding, which is what lets the shader's copy be the same four members with no
    // `alignas` or explicit padding anywhere - and the values above are glTF's basis, matching
    // `deren::gltf::head_basis_fallback`, so a block nobody writes is still a usable frame rather than three zeros that
    // would produce NaNs on the way to the sigmoid. THE DEFAULTS MATTER MORE THAN THEY LOOK: a model with NO
    // SKELETON never calls `set_head_basis`, so this block IS that model's head frame - and getting its `front`
    // backwards costs the face its terminator outright (see the note on `deren::gltf::head_basis`).
    static_assert(sizeof(head_ubo) == 64);

    /**
     * @ingroup engine_primitive
     * @brief how many GameToon characters can carry their head frame in one frame
     *
     * The character stage already draws a LIST of leaves; what was single was the head frame, because the scene
     * constant block had room for one `head_ubo` per frame slot. A scene can now import that many characters, each
     * with its index written into the high bits of `material_push_constants::flags`, and the shader indexes the
     * per-frame array. Sixteen matches `max_scene_objects`, and the shader's character index is 8 bits wide, so the
     * CPU-side array and descriptor range are the only storage that grow: 16 * 64 = 1024 bytes per frame slot.
     */
    export constexpr std::uint32_t max_game_toon_characters = 16;

    /**
     * @ingroup engine_primitive
     * @brief which shading stage shades this leaf's pixels
     *
     * `deferred` leaves write the G-buffer and are shaded by the deferred lighting pass. `character` leaves also
     * write the G-buffer (and its depth) but are re-shaded by the character-forward stage, which is where the
     * GameToon chain lives. `splat` is reserved for Gaussian-splat objects; the stage does not consume it yet.
     *
     * THIS IS A LIST-BUILDING FACT, not a material-record lane: the frame builder decides which leaves each pass
     * receives before any shader runs, exactly as it does for `environment` and `overlay_kind`.
     */
    export enum class shading_path : std::uint8_t {
        deferred = 0,
        character = 1,
        splat = 2,
    };

    /**
     * @ingroup engine_primitive
     * @brief the TOON LIGHT RIG's global numbers (scene block slot 750), written once from the application's config
     *
     * WHAT BELONGS HERE, AND THE BOUNDARY IS THE POINT: a FAMILY's numbers live in `source/shaders/toon/toon_params.slang`,
     * because two stages must agree about them, and a MATERIAL's live in the material record or in the sidecar
     * beside the model. What is left is the RIG - the numbers the article tunes once for a whole character
     * whatever it is wearing: the split between the SUN and the head light the game adds over every character, the
     * colours those two take on a surface's shadow side, and the global scalars the shading chain multiplies by.
     * They change when the art direction changes rather than per material, so they are one block and not a field
     * of every record.
     *
     * THE LANES ARE THE ARTICLE'S VOCABULARY (`_DayStrength`, `_OtherLight*`, `_MainLightColor_dark`, ...),
     * packed four to a `vec4` so no layout question can arise - `head_ubo` above makes the same choice for the
     * same reason. The shader's `ToonRigUBO` names every lane in its comments; the two must be read together,
     * and the defaults below are the article's values wherever it states one.
     */
    export struct toon_rig {
        /// x = `_DayStrength` (0 = the sun is gone and the head light carries the character, 1 = full sun),
        /// y = `_OtherLightOffset`, z = `_OtherLightStrength`, w = `_OtherLightStrength_Offset`
        glm::vec4 day = glm::vec4(1.0f, 0.0f, 1.0f, 0.0f);
        /// x = `_OtherLightResultStrength_day0`, y = `_OtherLightResultStrength_day1`,
        /// z = `_SelfAoShadowStrength`, w = the metallic/gloss map's occlusion exponent (`_AoStrength`)
        ///
        /// x/y ARE THE AUTHOR'S DEFAULTS (0.7 / 0.3), which all SIX of its shaders declare identically
        /// (`MyZmdToonShader.shader:79-80`, `MyZmdHairToonShader.shader:63-64`, `MyZmdSkinToonShader.shader:58-59`,
        /// `MyZmdFaceShader.shader:68-69`, `MyZmdEyeShader.shader:71-72`, `MyZmdOutlineShader.shader:12-13`).
        /// The port carried 1.0 / 0.25 - its numbers, and ones that made the head light's DAY-1 share 3.3x
        /// the author's. `head_day1` is live at the default `day_strength = 1` (`mainLightColor_final =
        /// lerp(head_day0, sun + head_day1, day)`), so both were visible; `head_day0` only at day < 1.
        glm::vec4 other_light = glm::vec4(0.7f, 0.3f, 1.0f, 1.0f);
        /// rgb = `_OtherLightColor`; w = face expression index.
        glm::vec4 other_colour = glm::vec4(0.60f, 0.65f, 0.80f, 0.0f);
        /// rgb = `_MainLightColor_dark` - the sun's colour on a surface's shadow side; a = `_BaseColorPow`
        glm::vec4 main_dark = glm::vec4(0.60f, 0.65f, 0.80f, 1.0f);
        /// x = `_RimLightArea`, y = `_RimLightStrength`, z = `_RimLightNoLxzStrength`,
        /// w = `_RimLightDiffuseColorEffect`
        ///
        /// x AND w ARE THE AUTHOR'S DEFAULTS (1.0 and 0.1), which every a separate one rim-carrying shaders declares
        /// identically (`MyZmdToonShader.shader:101/104`, `MyZmdHairToonShader.shader:85/88`,
        /// `MyZmdSkinToonShader.shader:78/81`, `MyZmdFaceShader.shader:83/86` - the EYE has no rim at all).
        /// The port carried x = 0.5 and w = 1.0, and the second is the bigger of the two by far: `w` is
        /// `rimLight_brdf = (mainDiffuseColor_Light - 0.25) * w + 0.25`, so at 1.0 the rim's brightness tracks the
        /// material's lit layer TEN TIMES as strongly as the author's 0.1 does. `x` moves the window the rim
        /// occupies: `rimStart = x * -0.6 + 0.8`, `rimEnd = x * -0.4 + 0.9`, so 0.5 puts it at 0.5..0.7 and the
        /// author's 1.0 at 0.2..0.5 - a wider band starting further from the silhouette.
        glm::vec4 rim = glm::vec4(1.0f, 1.0f, 1.0f, 0.1f);
        /// rgb = `_SssColor`, a = `_SssPowStrength`
        glm::vec4 sss = glm::vec4(1.0f, 0.80f, 0.78f, 1.0f);
        /// x = `_EnvLightStrength`, y = `_EnvRotation` in degrees, z = `_SpecularStrength`,
        /// w = `_DiffuseBlendEffect`
        glm::vec4 env = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
        /// x = the backlight compensation's weight, y = `_NoFStrength`, z = `_NoFPowStrength`,
        /// w = `_RampColorNoLStrength`
        glm::vec4 misc = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        /// x = THE TOON SHADOW SOFTNESS LEVEL (`[render] toon_shadow_softness`, 0..4); y/z/w are optional nose reduction, hair width and hair shift.
        ///
        /// 0 IS THE SHIPPED PATH AND NOT ONE TAP MORE: the character chain's shadow then takes the same single
        /// 3x3 hardware-PCF lookup it always has, so a config that omits the key renders byte-identically. The
        /// ladder is the whole reason the field exists rather than a wider kernel being the default. Its members
        /// are `(half_extent, spacing)` in texels - 0 = (1,1), 1 = (2,2), 2 = (3,3), 3 = (5,4), 4 = (8,3) - so
        /// the taps are `(2*half_extent + 1)^2` = 9 / 25 / 49 / 121 / 289, and the WIDTH quoted below is the
        /// texel SPAN the taps reach, `2*half_extent*spacing + 2` indices first-to-last (the counting the
        /// shipped `calc_shadow` note calls a "4x4 texel footprint" at level 0). THE PER-LEVEL COST IS NOT
        /// REPEATED HERE ON PURPOSE - a copy of that table went stale twice. The authority is the
        /// `toon_shadow_softness` block in `config.example.toml`, with the exact ROI and mask it used, measured
        /// on the welded asset `chars/laevatain_goo.glb` = 63,835,728 B /
        /// CE313E4D9515BC1887B4E0C783ABEA66A70FBFFF69ABD18992BDF33C8F5606F9.
        /// Stronger = fewer hard comb teeth at the shadow's termination, more light
        /// leaking onto the whole skin - a taste trade the user picks per run, which is why the default is off.
        ///
        /// IT RIDES THE RIG, NOT THE PER-FRAME LIGHT BLOCK, for the rig's reason: it is fixed for a run and
        /// only the toon chain reads it (`source/shaders/toon/character_forward.slang`'s one shadow call site). The rig's
        /// other lanes let this be a per-run A/B knob with no rebuild - the same lever `_DayStrength` uses.
        glm::vec4 shadow_softness = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        /// x = authored hair P-mask mode, y = angular face SDF; z/w reserved, all opt-in.
        glm::vec4 reference_modes = glm::vec4(0.0f);
    };
    // Ten vec4s; both shader mirrors append the same member without moving previous offsets.
    static_assert(sizeof(toon_rig) == 10 * sizeof(glm::vec4));

    /**
     * @ingroup engine_primitive
     * @brief light UBO content, layout matches the LightUBO block in source/shaders/shading.glsl and
     *        shadow.vert (scene block slot 7): the per-cascade light-space view-projections, the
     *        light direction, the cascade ranges/texel sizes, then the punctual light array
     * @note the directional sun is built from the scene bounds (enable_shadows) and the shadow
     *       map samples agree on its direction; punctual lights never cast shadows and ride the
     *       same block after the directional header
     * @note CASCADES: `light_view_proj[0]` covers the near range, the following entries the ranges
     *       given by `cascade_splits` (view-space far distance). A frame with cascade_count == 1 is
     *       exactly the single-map behavior this used to have - one fit over the whole visible
     *       range - which is what makes the cascaded version an A/B rather than a rewrite.
     */
    export struct light_ubo {
        deren::utility::vessel::array<glm::mat4, max_shadow_cascades> light_view_proj = {}; // world -> light clip, per cascade
        glm::vec4 light_dir = {};                                                           // xyz: normalized light direction (sun), w: 1 / shadow map size
        glm::vec4 cascade_splits = {};                                                      // view-space FAR distance of each cascade
        glm::vec4 cascade_texel_world = {};                                                 // world size of one shadow-map texel, per cascade
        float shadow_enabled = 0.0f;                                                        // 1.0 samples the shadow map, 0.0 skips shadows
        // Selectable BRDF models (set via runtime::set_brdf_model / set_diffuse_model, gui
        // combos). Rides the std140 padding of this block - the shader reads them as floats:
        //   brdf_model:   0 = GGX + joint Smith (default), 1 = GGX + height-correlated Smith,
        //                 2 = Beckmann + Smith, 3 = Blinn-Phong + Smith
        //   diffuse_model: 0 = Lambert (default), 1 = Oren-Nayar
        float brdf_model = 0.0f;
        float diffuse_model = 0.0f;
        // Fraction of a cascade's range over which the shader blends into the next one (0.1 = the
        // last 10%): a hard switch would show the resolution/offset step as a visible line.
        float cascade_blend = 0.1f;
        float cascade_count = 1.0f; // active cascades (1 = the single-map path)
        float rt_shadows = 0.0f;    // 1.0 = the sun's shadow comes from the ray-traced visibility image
                                    // (runtime::set_rt_shadows + the device having ray queries), 0.0 = sample
                                    // the cascaded shadow maps. Rides the std140 padding that keeps
                                    // light_count on its 16-byte boundary.
        float sun_intensity = 1.0f; // 1.0 normally; 0.0 in the furnace mode, which turns the sun off
        float furnace_level = 0.0f; // 0.0 normally; the constant environment level in the furnace mode
        glm::vec4 light_count = {}; // x = active punctual light count (shader: numeric float), y = exposure, z = toon shading steps (0 = PBR), w = toon band softness
        deren::utility::vessel::array<point_light, max_punctual_lights> punctual_lights = {};
        // Clustered light culling (M5), APPENDED after the light array so the array's offset (352)
        // stays what every shader and the earlier static_asserts already encode:
        //   cluster_grid  x = active tile columns, y = active tile rows, z = depth slices,
        //                 w = 1.0 when the shader reads the per-cluster light lists (0.0 = the
        //                 brute-force loop over every active light, the A/B path)
        //   cluster_depth x = near view depth, y = far view depth the slices span (z/w unused)
        glm::vec4 cluster_grid = {};
        glm::vec4 cluster_depth = {};
        // The rectangular area light ([lighting] area_light_*), APPENDED after cluster_depth so every offset
        // above - and the SHORTER LightUBO copies that light_cluster.slang / rt_shadow.slang declare as
        // prefixes of this block - stays exactly what it is. A std140 struct's tail is the one place a new
        // member cannot move an existing one, which is why these two ride here rather than next to the sun.
        //
        // area_light      xyz = emitter centre in WORLD space (Y-up metres), w = HALF the emitter's side.
        //                 THE SIGN OF w IS THE ENERGY SWITCH: w > 0 = the emitter is on and it TAKES OVER the
        //                 main light (its direction and radiance drive the sun - v1.1), w < 0 = the emitter is
        //                 on but contributes only its penumbra (the shader reads abs(w) as the half-side),
        //                 w == 0 = NO AREA LIGHT AT ALL, which is the default and the contract that keeps a
        //                 frame taken before these keys existed byte-identical. There is no spare lane in
        //                 light_count for a boolean, and the sign is free.
        //                 As of v1.1 (the A' ruling, 2026-10-02) NO SHADER READS THIS LANE ANY MORE: the
        //                 polygon-Lambert size correction it used to feed was deleted after measurement showed
        //                 it drove the main light to zero. The lane is kept because it is part of a pinned wire
        //                 layout (and because a v2 area integral will want the centre and the half-side).
        // area_light_axis xyz = the emitter's normal (centre -> target, unit), w = the penumbra's WORLD radius
        //                 in metres. w <= 0 = the area light does not touch the shadow term, so the shading
        //                 takes the same calc_shadow path it always took. THIS IS THE ONE AREA LANE v1.1 STILL
        //                 READS (`calc_shadow_area`): a world-space radius, not an angle.
        glm::vec4 area_light = {};
        glm::vec4 area_light_axis = {};
        glm::vec4 environment = {1, 0, 1, 0};                           // cos(yaw), sin(yaw), relative strength, background mode
        glm::vec4 environment_background = {0.18f, 0.18f, 0.18f, 0.5f}; // linear RGB, blur
        glm::vec4 probe_position_gain = {};
        glm::vec4 probe_min_fade = {};
        glm::vec4 probe_max_enabled = {};
    };
    // std140 layout guard against the GLSL LightUBO: four cascade matrices (256 B), the direction,
    // the two per-cascade vec4s (304 B), four floats, light_count (a glm::vec4 whose x carries the
    // numeric count the shader reads as float) and the punctual light array - which must start at a 16-byte
    // boundary. A vec3 pad anywhere on the GLSL side would push the array and shift every light.
    static_assert(offsetof(light_ubo, light_dir) == max_shadow_cascades * sizeof(glm::mat4));
    static_assert(offsetof(light_ubo, cascade_splits) == max_shadow_cascades * sizeof(glm::mat4) + sizeof(glm::vec4));
    static_assert(offsetof(light_ubo, cascade_texel_world) == max_shadow_cascades * sizeof(glm::mat4) + 2 * sizeof(glm::vec4));
    static_assert(offsetof(light_ubo, light_count) == max_shadow_cascades * sizeof(glm::mat4) + 5 * sizeof(glm::vec4));
    static_assert(offsetof(light_ubo, punctual_lights) == max_shadow_cascades * sizeof(glm::mat4) + 6 * sizeof(glm::vec4));
    static_assert(offsetof(light_ubo, cluster_grid) == max_shadow_cascades * sizeof(glm::mat4) + 6 * sizeof(glm::vec4) + max_punctual_lights * sizeof(point_light));
    static_assert(offsetof(light_ubo, cluster_depth) == max_shadow_cascades * sizeof(glm::mat4) + 7 * sizeof(glm::vec4) + max_punctual_lights * sizeof(point_light));
    // The area light is APPENDED, so its offsets are the OLD end of the block: area_light starts where
    // sizeof(light_ubo) used to end (8 * sizeof(glm::vec4)), and each new vec4 adds one more.
    static_assert(offsetof(light_ubo, area_light) == max_shadow_cascades * sizeof(glm::mat4) + 8 * sizeof(glm::vec4) + max_punctual_lights * sizeof(point_light));
    static_assert(offsetof(light_ubo, area_light_axis) == max_shadow_cascades * sizeof(glm::mat4) + 9 * sizeof(glm::vec4) + max_punctual_lights * sizeof(point_light));
    static_assert(offsetof(light_ubo, environment) == max_shadow_cascades * sizeof(glm::mat4) + 10 * sizeof(glm::vec4) + max_punctual_lights * sizeof(point_light));
    static_assert(sizeof(light_ubo) == max_shadow_cascades * sizeof(glm::mat4) + 15 * sizeof(glm::vec4) + max_punctual_lights * sizeof(point_light));
    static_assert(sizeof(light_ubo) <= 16384, "the light UBO must stay inside the guaranteed maxUniformBufferRange (16 KB)");
    static_assert(sizeof(point_light) == 64);
    // Byte-addressed GameToon light reads share this std140 wire layout.
    static_assert(offsetof(light_ubo, punctual_lights) == 352);
    static_assert(offsetof(point_light, position) == 0);
    static_assert(offsetof(point_light, color) == 16);
    static_assert(offsetof(point_light, spot_dir) == 32);
    static_assert(offsetof(point_light, params) == 48);

    /**
     * @ingroup engine_primitive
     * @brief RGBA texture pixels ready for GPU upload (already converted to the target format)
     * @note valid == false means "missing texture", the primitive falls back to a 1x1 white image
     */
    export struct texture_input {
        // RGBA8 bytes of mip level 0 (or the whole mip chain, mip-major: mip0, mip1, ..., when
        // mip_levels > 1); the vma upload path copies each level at a computed buffer offset
        std::span<std::uint8_t const> data = {};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mip_levels = 1;
        ::deren::promise::rhi::image_format format = ::deren::promise::rhi::image_format::rgba8_unorm;
        bool valid = false;
        /// Optional immutable source. All aliases must remain immutable for as long as the runtime uses it.
        std::shared_ptr<deren::utility::vessel::vector<std::uint8_t> const> parent = {};
        bool override_format = false; // Opt-in explicit colour/data encoding, otherwise lane default.
    };

    /// Reuse only an exact whole-vector source; a partial span needs its own snapshot.
    export [[nodiscard]] inline std::shared_ptr<deren::utility::vessel::vector<std::uint8_t> const>
    share_texture_source(texture_input const& input) {
        if (input.parent && input.data.data() == input.parent->data() && input.data.size() == input.parent->size()) {
            return input.parent;
        }
        return std::make_shared<deren::utility::vessel::vector<std::uint8_t> const>(input.data.begin(), input.data.end());
    }

    /**
     * @ingroup engine_primitive
     * @brief precomputed split-sum IBL resources as half-float bytes, ready for upload
     * @note env_size == 0 disables the IBL bindings
     */
    export struct ibl_input {
        std::span<std::uint8_t const> prefiltered_env = {};
        std::span<std::uint8_t const> irradiance = {};
        std::span<std::uint8_t const> brdf_lut = {};
        std::uint32_t env_size = 0;
        std::uint32_t env_mip_count = 0;
        std::uint32_t irr_size = 0;
        std::uint32_t lut_size = 0;
    };

    /**
     * @ingroup engine_primitive
     * @brief PBR material factors, mirrored into the GPU material table (material_record)
     * @note the same shape as deren::gltf::material_factors, converted by the scene builder
     */
    export struct material_factors {
        glm::vec4 base_color_factor = glm::vec4(1.0f);
        glm::vec4 emissive_factor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        float normal_scale = 1.0f;
        float occlusion_strength = 1.0f; // occlusion map influence: mix(1, sampled AO, strength)
        float alpha_cutoff = 0.5f;       // alphaMode MASK threshold (fragment discard below it)
        bool alpha_mask = false;         // alphaMode == MASK
        bool alpha_blend = false;        // alphaMode == BLEND (alpha-blended / transparent)
    };

    /**
     * @ingroup engine_primitive
     * @brief result of one runtime::import_scene() batch import
     */
    export struct scene_import_result {
        std::uint32_t primitive_count = 0;
        std::uint32_t material_count = 0;
        std::uint64_t import_instance = 0;
        std::uint32_t subtilis_imported = 0, subtilis_fallback_lod0 = 0, subtilis_cache_hits = 0;
        std::array<std::uint32_t, static_cast<std::size_t>(subtilis::import_failure::count)> subtilis_failures{};
    };

    // The scene_drawable_iterator concept is STRUCTURAL over the getters' result shapes, so a
    // scene iterator can satisfy it with its pure-CPU types (e.g. the glTF loader's) — no
    // shared type identity is required. The runtime template converts the read values (spans /
    // widths / factors) into its internal types.

    /** @brief a vertex source: interleaved byte span + stride + vertex count */
    export template <class T>
    concept vertex_source = requires(T const& v) {
        { v.data } -> std::convertible_to<std::span<std::uint8_t const>>;
        { v.stride } -> std::convertible_to<std::uint32_t>;
        { v.count } -> std::convertible_to<std::uint32_t>;
    };

    /** @brief an index source: byte span + bytes-per-index (2 or 4) + index count */
    export template <class T>
    concept index_source = requires(T const& v) {
        { v.data } -> std::convertible_to<std::span<std::uint8_t const>>;
        { v.width } -> std::convertible_to<std::uint8_t>;
        { v.count } -> std::convertible_to<std::uint32_t>;
    };

    /** @brief a texture source: mip-major RGBA8 byte span + dimensions + validity */
    export template <class T>
    concept image_source = requires(T const& v) {
        { v.data } -> std::convertible_to<std::span<std::uint8_t const>>;
        { v.width } -> std::convertible_to<std::uint32_t>;
        { v.height } -> std::convertible_to<std::uint32_t>;
        { v.mip_levels } -> std::convertible_to<std::uint32_t>;
        { v.valid } -> std::convertible_to<bool>;
    };

    export template <image_source Image>
    [[nodiscard]] texture_input texture_input_from(Image const& image, ::deren::promise::rhi::image_format format) {
        texture_input out{};
        if (!image.valid) {
            return out;
        }
        out.data = image.data;
        out.width = image.width;
        out.height = image.height;
        out.mip_levels = image.mip_levels;
        out.format = format;
        out.valid = true;
        // Converting a mutable shared_ptr to const does not prevent writes through other aliases.
        // Only an explicitly immutable source opts into sharing; other inputs get their own snapshot.
        if constexpr (requires { { image.parent } -> std::same_as<decltype(out.parent) const&>; }) {
            out.parent = image.parent;
        }
        return out;
    }

    /** @brief a PBR factors source: the same field names/shapes as material_factors */
    export template <class T>
    concept factors_source = requires(T const& v) {
        { v.base_color_factor } -> std::convertible_to<glm::vec4>;
        { v.emissive_factor } -> std::convertible_to<glm::vec4>;
        { v.metallic_factor } -> std::convertible_to<float>;
        { v.roughness_factor } -> std::convertible_to<float>;
        { v.normal_scale } -> std::convertible_to<float>;
        { v.occlusion_strength } -> std::convertible_to<float>;
        { v.alpha_cutoff } -> std::convertible_to<float>;
        { v.alpha_mask } -> std::convertible_to<bool>;
        { v.alpha_blend } -> std::convertible_to<bool>;
    };

    /**
     * @ingroup engine_primitive
     * @brief concept for a scene-traversal iterator the runtime can consume directly:
     *        ++ moves to the next drawable, then geometry/material are read through the
     *        getters (vertex/index/transform + one getter per material slot). The getters
     *        return pure CPU values (byte spans etc., see the *_source concepts above);
     *        the runtime template converts them to its internal types (formats, index type).
     *        A missing material slot is reported through image_source::valid == false and the
     *        runtime falls back to its white texture.
     */
    export template <class I>
    concept scene_drawable_iterator = requires(I& it, I const& end) {
        { ++it } -> std::same_as<I&>;
        { it != end } -> std::convertible_to<bool>;
        { it.get_vertex() } -> vertex_source;
        { it.get_index() } -> index_source;
        { it.get_transform() } -> std::convertible_to<glm::mat4>;
        { it.get_albedo() } -> image_source;
        { it.get_metallic_roughness() } -> image_source;
        { it.get_normal() } -> image_source;
        { it.get_occlusion() } -> image_source;
        { it.get_emissive() } -> image_source;
        { it.get_factors() } -> factors_source;
        { it.get_double_sided() } -> std::convertible_to<bool>;
    };

    /**
     * @ingroup engine_primitive
     * @brief concept for a scene-tree structural iterator: DFS pre-order over the
     *        node hierarchy (transform-only nodes included), so a consumer can rebuild the
     *        parent/child edges with an explicit stack. ++ moves to the next node, then the
     *        node's identity is read through get_name() / get_local_transform() / get_depth()
     *        and its drawable load through get_drawable_count() (the number of drawables of
     *        this node; 0 = transform-only node). The paired drawable stream (a
     *        scene_drawable_iterator over the same pool) stays aligned node-for-node.
     */
    export template <class I>
    concept scene_node_iterator = requires(I& it, I const& end) {
        { ++it } -> std::same_as<I&>;
        { it != end } -> std::convertible_to<bool>;
        { it.get_name() } -> std::convertible_to<std::string_view>;
        { it.get_local_transform() } -> std::convertible_to<glm::mat4>;
        { it.get_depth() } -> std::convertible_to<std::size_t>;
        { it.get_drawable_count() } -> std::convertible_to<std::size_t>;
        { it.get_source_index() } -> std::convertible_to<std::size_t>;
    };

    /**
     * @brief the LANES of `material_record::toon_indices`, in order
     *
     * THE ORDER IS THE CONTRACT, and it is spelled out on both sides of the boundary: here for the host that
     * fills the record, and in the shader's `toon_slot_*` constants. The names are what the slot IS rather than
     * the sidecar's spelling (`_DiffRampMap` and friends), because that spelling is one ASSET PIPELINE's and the
     * mapping between the two belongs where the sidecar is read, not in a record layout.
     */

} // namespace deren::engine
