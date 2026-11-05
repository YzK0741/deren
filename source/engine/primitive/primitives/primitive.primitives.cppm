// ============================================================================
// module: deren.engine.primitive:primitives  (the drawable GPU primitives of
//         deren.engine.primitive: primitive, its material/push records and the
//         normal / instanced / static draw strategies)
//
// This partition is one of the three domains of deren.engine.primitive; the
// top-level facade re-exports it, so consumers keep importing
// deren.engine.primitive.
// ============================================================================

module;
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

export module deren.engine.primitive:primitives;

import deren.promise.rhi;
import deren.engine.render_layout;
import deren.engine.render_environment;
import deren.engine.scene_tree;
import deren.engine.meshlet;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.upload;
import deren.engine.subtilis.dag_import;
import deren.utility.shared;
import deren.vstd;

import :records;
import :toon;

namespace deren::engine {
    /**
     * @ingroup engine_primitive
     * @brief everything runtime::make_primitive() needs: geometry + material textures + factors
     */
    export struct primitive_create_info {
        /// If present, this immutable source supplies ALL geometry fields below;
        /// material/model still come from this descriptor. No per-frame LOD is enabled.
        std::shared_ptr<subtilis::built_geometry const> subtilis_geometry;
        std::shared_ptr<subtilis::imported_asset const> subtilis_import;
        std::span<uint8_t const> vertex_data = {};
        uint32_t vertex_stride = 0;
        uint32_t vertex_count = 0;
        std::span<uint8_t const> index_data = {};
        deren::promise::rhi::index_type index_type = deren::promise::rhi::index_type::uint32;
        uint32_t index_count = 0;

        texture_input albedo = {};
        texture_input metallic_roughness = {};
        texture_input normal = {};
        texture_input occlusion = {};
        texture_input emissive = {};

        // PBR factors, stored in the primitive's material_record
        material_factors factors = {};

        /**
         * THE TOON MATERIAL FAMILY (`deren::gltf::toon_family`), carried into the primitive's `material_record`.
         *
         * Passed as a NUMBER rather than as a name, because the classification happens in the loader where
         * the name still exists (see gltf_loader's `toon_family_of`) and nothing downstream should be
         * matching strings. 0 == `toon_family::none`.
         */
        uint32_t toon_family = 0;

        /**
         * THE OVERLAY CHANNEL (`deren::gltf::overlay_kind`), carried onto the primitive and NOT into the material
         * record - which is the opposite of what the family does, and the difference is what the fact is FOR.
         *
         * The family is a fact about SHADING, so the shader that shades the surface has to read it and it
         * belongs in the record. This one is a fact about WHICH PASS DRAWS THE SURFACE, so it is read by the
         * HOST while it is building the frame's leaf lists - long before any shader exists - and a lane in the
         * record would be a second copy of a decision the host has already made. It is the same shape as
         * `transparent`, which sits on the primitive for exactly this reason (see its note).
         *
         * 0 == `overlay_kind::none`: an ordinary surface, shaded and drawn by the passes that always drew it.
         */
        uint32_t overlay_kind = 0;

        /**
         * THE ENVIRONMENT FLAG (the `[render] background_glb` import): whether this leaf is the frame's
         * STATIC SURROUND rather than its subject.
         *
         * SAME KIND OF FACT AS `overlay_kind` ABOVE, MIRRORED FOR THE SAME REASON: it decides WHICH PASS
         * DRAWS THE LEAF, so the host reads it while it is building the frame's leaf lists and no shader
         * ever sees it - and unlike `toon_family` it therefore has no business in `material_record`. The
         * ground and the backdrop have an ordinary glTF material (a matte PBR ground, an emissive dome),
         * and the only thing that makes them "background" is that the reference's toon chain was never
         * handed them.
         *
         * IT CHANGES EXACTLY TWO LISTS (`source/engine/runtime/runtime.frames.cppm`):
         *   * the TOON CHARACTER stage skips it (`make_character_forward_frame`), so the scene's static
         *     surround is not re-shaded by a character pipeline that has no material data for it;
         *   * it does NOT CAST SHADOWS (`shadow_casters`), so a 34 m backdrop dome cannot shade the
         *     character the frame is about.
         * It STAYS in `frame_visible`, which is the point: the scene pass draws it through the ordinary
         * PBR/unlit path, where a matte ground and a glTF emissive backdrop belong.
         *
         * Default false, i.e. every model that is not a configured background behaves exactly as before.
         */
        bool environment = false;

        /**
         * WHICH GAME-TOON CHARACTER THIS PRIMITIVE BELONGS TO, for the shader's per-character head frame.
         *
         * The character stage draws many leaves with one pipeline, so a frame-wide `head_ubo` cannot describe two
         * characters with different faces. Each import names its character, `create_primitive` carries the index
         * into the high bits of `push.flags`, and the shader selects `head_frame_at(...)[character_index]`. Zero is
         * the single-character path and produces exactly the old flag word.
         */
        uint32_t character_index = 0u;

        /**
         * THE TOON TEXTURE INPUTS, filled by whoever installs a `toon_lookup` - the application, which is the
         * layer that reads the sidecar (see `runtime::set_toon_lookup`). Empty and flagless for a model with no
         * sidecar, which is every model that is not a character; then every lane points at the white fallback
         * and every bit is clear, so a shader that reads the block finds nothing switched ON rather than finding
         * white as a value.
         */
        toon_inputs toon = {};

        // glTF doubleSided: render back faces and flip their normals (cull mode + record flag)
        bool double_sided = false;

        // world transform applied to the geometry (e.g. fit-scale + centering from the bounding box);
        // pushed per primitive (the shared camera UBO carries no model matrix)
        glm::mat4 model_matrix = glm::mat4(1.0f);
    };

    /**
     * @ingroup engine_primitive
     * @brief one entry of the scene's GPU-side material table (set 0 binding 5, a storage buffer):
     *        the 5 texture array indices + all material parameters. Primitives only push a
     *        material_index and the shader reads the record — material data lives in one
     *        GPU-visible place and is shareable between primitives
     * @note layout matches the Material struct in pbr.frag (std430, 80 bytes)
     */
    export struct material_record {
        glm::uvec4 tex_indices = {};     // albedo, metallic-roughness, normal, occlusion (indices into the texture array)
        uint32_t emissive_index = 0;     // emissive texture index
        float alpha_cutoff = 0.5f;       // alphaMode MASK threshold (fragment discard below it)
        float occlusion_strength = 1.0f; // occlusion map influence: mix(1, sampled AO, strength)
        /**
         * THE TOON MATERIAL FAMILY (`deren::gltf::toon_family`, resolved at import from the material's name).
         *
         * THIS LANE WAS `_pad`, and reusing it rather than adding a field is not a space optimisation: the
         * record sits in a 16-byte std430 group of four uints (`emissive_index`, `alpha_cutoff`,
         * `occlusion_strength`, this one), so the padding is ALREADY THERE and a new field would have had to
         * come from somewhere - either by growing the record past its `static_assert(sizeof(...) == 80)` and
         * every shader's copy of the struct with it, or by taking bits from `flags`, which is a used
         * contract. Naming the lane is a zero-cost extension because the bytes were always being uploaded.
         *
         * 0 means `toon_family::none`: the toon stage's family-independent path, which is every material
         * whose name claimed no family - so a frame with no character in it is unchanged by the family test.
         */
        uint32_t toon_family = 0;
        glm::vec4 base_color_factor = glm::vec4(1.0f);
        glm::vec4 emissive_factor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        float normal_scale = 1.0f;
        uint32_t flags = 0; // bit0: normal map, bit1: occlusion map, bit2: emissive map, bit3: double-sided, bit4: alphaMode MASK, bit5: alphaMode BLEND, bit6: the EYE-DARK overlay, bit7: the HAIR-SHADOW overlay, bit8: the author's toon TRANSPARENT variant (see toon_inputs::alpha_blend), bit9: unlit shading for this material, bits10..11: the IBL environment slot (0 = scene-wide, 1..3 = per-object custom; the deferred stage reads them back through the G-buffer's material id)
                            // (bits 6 and 7 are the article's two overlay channels - see `overlay_kind` in
                            // gltf_loader and the note in register_material: the overlay fragment stage branches
                            // on them because the two masks compute different multipliers from the same inputs)
        /**
         * THE TOON TEXTURE SLOTS, which are the ones glTF's five cannot reach.
         *
         * A toon character reads a SECOND set of maps that the glTF spec has no slot for - a diffuse ramp, a
         * shadow LUT, a specular ramp, a matcap - and the toon material sidecar beside the model names them
         * (see toon_material_sidecar.cppm). They arrive here as texture-array indices, exactly like the five
         * above, and the ORDER IS THE CONTRACT with the shader's `toon_slot_*` constants:
         *
         *   x = diffuse ramp (`_DiffRampMap`), y = shadow LUT (`_ShadowLutTex`),
         *   z = specular ramp (`_SpecRampMap`), w = matcap (`_MatcapTex`)
         *
         * THE WHITE FALLBACK (element 0) IS THE "DO NOT READ" VALUE, and that is the whole contract rather than a
         * convention: a lane holds a real index when the material has that map AND the artist's `_Use` flag is
         * on, and element 0 otherwise. So the shader needs no separate enable word - `toon_indices.x != 0u` IS
         * "read the ramp" - and a ramp lookup against white (which would be a CONSTANT, not a no-op) cannot
         * happen by accident. THE SIDECAR KEEPS THE TWO REASONS APART for diagnosis; here they collapse because
         * to a shader they are the same instruction.
         */
        glm::uvec4 toon_indices = {};
    };
    // 96: the toon block is a uvec4 (16 bytes), which takes the record from 80 to 96. It cannot be 80 and it
    // does not have to grow further - an enable WORD beside the indices would push std430's 16-byte struct
    // alignment to 112, and the "element 0 means do not read" rule above is what makes the word unnecessary.
    static_assert(sizeof(material_record) == 96);

    /**
     * @ingroup engine_primitive
     * @brief max entries of the GPU material table
     * @note sized for the heaviest glTF stress sample (NodePerformanceTest: 10000 rocks, each
     *       with its own material record - factors differ per rock, so content dedup cannot
     *       collapse them). 16384 x 80 B = 1.3 MiB storage buffer, negligible. The runtime
     *       dedups byte-identical materials (register_material) and reserves index 0 as the
     *       default material; registrations past the capacity degrade to it with a logged
     *       warning instead of failing the whole scene.
     */
    export constexpr uint32_t material_capacity = 16384;

    /**
     * @ingroup engine_primitive
     * @brief max per-instance transforms of an instanced draw (set 0 binding 6 storage buffer)
     */
    export constexpr uint32_t instance_capacity = 8192;

    /**
     * @ingroup engine_primitive
     * @brief max motion slots of the scene's previous-transform buffer (set 0 binding 13), in mat4s
     * @note Every draw owns at least one slot - an instanced draw owns one per instance - and the
     *       slots are why the same capacity as the instance buffer is plenty: a scene of N leaves
     *       uses N, and only an instanced stress draw uses thousands.
     */
    export constexpr uint32_t scene_motion_capacity = instance_capacity;

    /**
     * @ingroup engine_primitive
     * @brief max skin matrices of the scene skin buffer (set 0 binding 9 storage buffer), in
     *        mat4s. Indices 0-3 are the identity block (the fallback for unskinned draws:
     *        skin_base = 0), the per-skin joint blocks follow at 4.
     * @note sized for the heavy recursive-skeleton sample (RecursiveSkeletons: 84 skins x 10
     *       joints = 840 joint matrices + identity); the buffer is 2048 x 64 B = 128 KiB per
     *       frame slot, negligible against the 8 MiB morph buffer
     */
    export constexpr uint32_t scene_skin_capacity = 2048;

    /**
     * @ingroup engine_primitive
     * @brief capacity of the scene morph buffer IN FLOATS (set 0 binding 10 storage buffer); the buffer
     *        itself is this many floats, i.e. four times as many bytes:
     *        per-morphable-primitive blocks of vertex deltas + morph weights, laid out by the
     *        caller (see the material_push_constants morph fields); 0 = no morph buffer
     */
    // EIGHT MIB WORTH OF FLOATS, which is 32 MiB of buffer. The controller's capacity check compares FLOAT
    // counts against this, and the allocation multiplies it by sizeof(float), so the two agree; calling it a
    // "byte capacity" (as this comment did) is what made a reader suspect an overflow that is not there.
    export constexpr std::size_t scene_morph_capacity = std::size_t{12u} * 1024u * 1024u;

    /**
     * @ingroup engine_primitive
     * @brief per-draw push constants, layout matches the shaders' PushConstants (96 bytes)
     * @note material data lives in the material table (set 0 binding 5), so the push block only
     *       carries the material reference, the skin-matrix block start, the morph block start
     *       and the per-primitive world transform. morph_targets == 0 means "not morphable" and
     *       the vertex shader skips the blend; morph_base is a FLOAT index into binding 10.
     */
    /**
     * @ingroup engine_primitive
     * @brief index of one material in the runtime's material table (scene block slot 5).
     *        Strongly typed on the CPU side so it cannot be confused with the other GPU-table
     *        indices (instance/skin/morph bases); it is a single uint32_t, so push-constant /
     *        material-record byte layout is unchanged (memcpy/push use the raw bytes).
     */
    export struct material_id {
        uint32_t value = 0;
    };

    export struct material_push_constants {
        material_id material_index = {}; // index into the scene's material table
        uint32_t flags = 0;              // bit0: instanced draw -> model matrix comes from the
                                         //       instance transform buffer (set 0 binding 6)
        // index into the scene skin-matrix buffer (binding 9) where this primitive's joint
        // matrices start; 0 = the identity block (unskinned). The vertex shader reads
        // matrices[skin_base + in_joints.x] etc. — set once per primitive after import
        uint32_t skin_base = 0;
        // morph blend (binding 10): float index of this primitive's morph block (deltas first:
        // per vertex per target pos-delta/nrm-delta, then the per-target weights, then the per-target
        // PREVIOUS weights); morph_targets / morph_vertices describe the block stride. All three stay 0
        // for non-morphable draws. The vertex stage reads the FIRST weight region as this frame's weights
        // and the SECOND as the weights one frame ago, which is the morph half of a deforming mesh's
        // motion vector - see runtime::morph_scratch()'s note for the writer's side of that contract.
        uint32_t morph_base = 0;
        uint32_t morph_targets = 0;  // number of morph targets (0 = no morph)
        uint32_t morph_vertices = 0; // vertex count of this primitive (block stride)
        // mat4 start of THIS instanced primitive's transforms in the shared instance buffer
        // (set 0 binding 6): the vertex shader reads instances.transforms[instance_base +
        // gl_InstanceIndex]. Only meaningful when flag bit0 is set; other draw strategies keep 0.
        uint32_t instance_base = 0;
        // Start of THIS draw's previous-frame world matrices in the scene block's motion buffer
        // (binding 13), where the vertex shader reads the matrix that turns into TAA's motion
        // vector. Every draw owns at least one slot (an instanced draw owns one per instance), and
        // the runtime advances them once per frame from the scene tree's world matrices - see
        // runtime::advance_motion_transforms(). The field sits in the 4 bytes std430 leaves between
        // instance_base and model, so the block stays 96 bytes and the pipeline layout is unchanged.
        uint32_t motion_base = 0;
        // glm::mat4 is only 4-byte aligned by default, but GLSL std430 aligns mat4 to 16 bytes
        // (offset 32 in the block): align explicitly so the CPU layout matches the shader
        alignas(16) glm::mat4 model = glm::mat4(1.0f);
    };
    static_assert(sizeof(material_push_constants) == deren::engine::render_layout::scene_push_constant_size);

    /**
     * @ingroup engine_primitive
     * @brief WHERE A MESH STAGE'S GEOMETRY LANES SIT IN ITS STAGE BLOCK, in bytes (docs/mesh_shaders.md step 2)
     *
     * A mesh stage replaces the input assembler, so `vkCmdDrawMeshTasksEXT` carries no vertex binding, no index
     * buffer and no first index - the DRAW has to hand all of that over as data, and there are TWO host-side
     * numbers for where it lands because they answer different questions:
     *
     *  - `mesh_geometry_lanes_offset` is where the SHADER reads them, and it is the same 112 in both blocks: the
     *    scene's block declares the alignment word that ends there, and the shadow pass's block has its cascade
     *    lane there. A struct member is 16-byte aligned under std140, which is why the scene block declares that
     *    word explicitly rather than letting the layout be a consequence of the layout rules.
     *  - `mesh_geometry_push_offset_*` is where the HOST starts filling, i.e. the end of the block's previous
     *    member: 108 for the scene block (the material block plus the endpoint's three heap index lanes), 112 for
     *    the shadow pass's (plus its cascade lane).
     *
     * They are separate because of the VUID below: with a descriptor-heap pipeline EVERY byte of a stage's declared
     * block must have been written by `vkCmdPushDataEXT` before the draw
     * (VUID-vkCmdDrawMeshTasksEXT-None-11376, measured - the first attempt was rejected by name for a 4-byte hole
     * and then for the block's trailing std140 padding), so the fill has to start at the earlier member's end and
     * run to the BLOCK's end rather than to the lanes' end.
     */
    export constexpr uint32_t mesh_geometry_lanes_offset = 112;
    export constexpr uint32_t mesh_geometry_push_offset_scene = deren::engine::render_layout::scene_push_constant_size + 3u * sizeof(uint32_t);
    /// ... and the shadow pass's own end: the same offset plus the cascade lane its block declares
    export constexpr uint32_t mesh_geometry_push_offset_shadow = mesh_geometry_push_offset_scene + sizeof(uint32_t);
    /**
     * @ingroup engine_primitive
     * @brief what a mesh stage needs to know about the geometry a draw covers, pushed at the session's
     *        `mesh_geometry_offset` (the shader's copy is `MeshGeometryLanes` in source/shaders/geometry/mesh_geometry.slang)
     * @note the layout is the shader's: two device addresses as four 32-bit halves (the SHADER declares eight
     *       uints rather than two `uint2`s so that nothing pads - see that struct's note), then four uints. The
     *       static_asserts below are what keep a field added here from silently shifting a lane.
     * @note THE WHOLE WINDOW IS HERE, not only the addresses: a mesh dispatch has no `firstIndex` and no
     *       `baseVertex` arguments either, and a static draw's chunk is exactly a window into a merged buffer -
     *       so a lane left out is a draw that cannot be ported rather than a draw that is slightly wrong.
     */
    export struct mesh_geometry_lanes {
        uint64_t vertex_address = 0; // the vertex buffer's device address (base, vertex 0)
        uint64_t index_address = 0;  // the index buffer's device address (base, index 0)
        uint32_t first_index = 0;    // the draw's first index inside the index buffer
        uint32_t index_count = 0;    // indices this draw covers (triangles)
        int32_t base_vertex = 0;     // added to every fetched index (static-draw chunks; 0 otherwise)
        uint32_t index_width = 4;    // bytes per index: 2 (uint16) or 4 (uint32)
    };
    static_assert(offsetof(mesh_geometry_lanes, vertex_address) == 0);
    static_assert(offsetof(mesh_geometry_lanes, index_address) == 8);
    static_assert(offsetof(mesh_geometry_lanes, first_index) == 16);
    static_assert(offsetof(mesh_geometry_lanes, index_count) == 20);
    static_assert(offsetof(mesh_geometry_lanes, base_vertex) == 24);
    static_assert(offsetof(mesh_geometry_lanes, index_width) == 28);
    static_assert(sizeof(mesh_geometry_lanes) == 32);
    /// the stage block a mesh pass needs, from its start through those lanes - and ONE number covers both blocks
    /// for a reason worth stating: the shadow block's lanes end exactly there (112 + 32), and the scene block's
    /// members end at 140 with std140 rounding the block's SIZE up to the same 144. It is what the device's push
    /// limits have to cover for the path to be available at all (see the runtime's capability gate).
    export constexpr uint32_t mesh_stage_block_size = mesh_geometry_lanes_offset + sizeof(mesh_geometry_lanes);

    /**
     * @ingroup engine_primitive
     * @brief base class of every GPU primitive: owns geometry buffers + material push constants
     *        and declares the draw strategy interface. Derived classes implement how the
     *        geometry is drawn (single draw, instanced grid, ...), so the runtime's frame loop
     *        stays a generic "for each primitive: primitive->draw()" — new strategies only add
     *        a subclass. Implements the scene tree's leaf concept (scene_tree::primitive).
     * @note
     *      - owns only its geometry (contract buffers); textures live in the runtime's shared texture
     *        array, and the descriptors that reach them are the runtime's (the frame's one heap,
     *        bound once, holding this frame slot's scene block)
     *      - the runtime binds the pipeline and the scene block before calling draw()
     *      - destroy() frees whatever the instance owns (contract buffers); call it before teardown
     *      - a scene tree node holds one of these as its primitive_leaf and update_world() feeds
     *        the accumulated world matrix straight into push.model (the push block layout is
     *        shared, so draw() keeps working unchanged)
     */
    export class primitive : public deren::engine::scene_tree::primitive {
        inline static std::atomic<uint64_t> next_lifetime{1};

    public:
        // Address reuse must not reuse optional per-instance LOD decisions.
        uint64_t const lifetime_id = next_lifetime.fetch_add(1, std::memory_order_relaxed);
        ~primitive() override = default;

        // Geometry views carry allocation leases. Production generations own buffers and device;
        // legacy assignments own their transferred RHI reference. BLAS retains leases separately.
        std::shared_ptr<void const> geometry_resources; // subchain drains after the views below release
        std::shared_ptr<subtilis::uploaded_geometry const> subtilis_geometry;
        std::shared_ptr<subtilis::imported_asset const> subtilis_import;
        geometry_buffer vertex_buffer;
        geometry_buffer index_buffer;
        deren::promise::rhi::index_type index_type = deren::promise::rhi::index_type::uint32;
        // called draw_index_count, not index_count: push_geometry_lanes_impl, push_meshlet_lanes and
        // mesh_dispatch keep a parameter named index_count, which would hide the member of that name and
        // MSVC /W4 reports C4458 (an error under /WX).
        uint32_t draw_index_count = 0;
        uint32_t vertex_count = 0;
        // Bytes per vertex of the interleaved layout (position first; 80 since the TANGENT was appended). The
        // mesh stages fetch through the shader's fixed 80-byte layout; this field is what the
        // acceleration-structure build reads and what the stride gates (visibility/subtilis admission,
        // compute_skin's expected source) compare against.
        uint32_t vertex_stride = 0;
        /**
         * THE PRIMITIVE'S MESHLETS, in the order `build_meshlets` produced them (docs/mesh_shaders.md step 3):
         * runs of at most `meshlet_max_triangles` triangles of this primitive's index window, each with an
         * object-space bounding sphere. Filled at upload, where the geometry bytes and the layout are in hand.
         *
         * THEY ARE HOST-SIDE FOR NOW, and deliberately: the meshlet table the shaders will read is a heap
         * resource of its own (a per-frame slot, written once at import and registered like the material table),
         * and the split is the part of that step worth having early - it is arithmetic whose bugs are invisible
         * on screen, so it is built and tested (source/tests/test_meshlet.cpp) before anything consumes it.
         */
        deren::utility::vessel::vector<deren::engine::meshlet> meshlets = {};
        /// where this primitive's run of meshlets starts in the GPU TABLE (docs/mesh_shaders.md step 3): the value
        /// the geometry lanes carry, so a task stage can turn "my meshlet workgroup" into a record index
        uint32_t meshlet_base = 0;
        /// how many records the complete run holds; zero when the table cannot hold the entire run
        uint32_t meshlet_count = 0;
        // Pipeline the primitive draws with. Empty = DEFAULT semantics: the primitive does not
        // care which pipeline records it, it asks the draw-time render_environment to bind that
        // session's default (normal / instanced / static draws all work this way - they draw
        // with whatever default the recording pass set). Non-empty = an explicit pipeline name
        // the primitive requests through render_environment::bind_pipeline (custom draw
        // strategies). Never a vk_pipeline pointer: pipelines live in the runtime's cache and
        // are reached by name through the environment, so the scene tree stays independent of
        // pipeline objects.
        std::string_view pipeline_name = {};
        // the material push constants (material_index + model)
        material_push_constants push = {};
        // This leaf's slot in the runtime's previous-transform buffer (scene block slot 13), which
        // is where the matrix it had one frame ago lives - the vertex shader reads it through
        // push.motion_base and the fragment stage turns the difference into TAA's motion vector.
        // no_motion_slot = not tracked frame to frame (an instanced draw, whose slots are filled
        // once at setup with its own instance transforms, so its object motion reads as zero).
        uint32_t motion_slot_index = deren::engine::scene_tree::no_motion_slot;

        /** @brief where this leaf's previous world matrix lives (see the member) */
        [[nodiscard]] uint32_t motion_slot() const noexcept override {
            return this->motion_slot_index;
        }
        bool double_sided = false; // glTF doubleSided: disable back-face culling (per draw)
        // alphaMode BLEND: drawn alpha-blended in the transparent pass (depth write off,
        // back-to-front order). The GPU material record also carries the flag; this mirror on
        // the primitive lets draw() pick the depth-write state without a GPU readback.
        bool transparent = false;
        /**
         * THE OVERLAY CHANNEL THIS PRIMITIVE BELONGS TO (`deren::gltf::overlay_kind`; 0 == none), i.e. whether the
         * frame's leaf lists put it in the overlay pass rather than in the shading passes.
         *
         * IT IS A LIST-BUILDING FACT AND NOT A DRAW-TIME ONE, which is why it is read while the frame's leaves
         * are collected rather than by draw(): an overlay surface must be ABSENT from the opaque, transparent
         * and shadow lists altogether - not drawn-and-then-ignored - because those passes would shade it (the
         * measured 8.22% of the standard frame and 68.92% of the face close-up that made this port necessary)
         * and the shadow pass would let its quad cast a shadow of its own onto the face it darkens.
         *
         * The same shape as `transparent` above: a fact the host needs, mirrored on the primitive so no GPU
         * readback is involved in a decision about which list a leaf goes into. See `overlay_kind_of` in
         * gltf_loader for what the number means and `runtime::frame_overlay` for the list it selects.
         */
        uint32_t overlay_kind = 0;

        /// this leaf's material family (`deren::gltf::toon_family`; 0 == none), mirrored from
        /// `primitive_create_info::toon_family` so the character-forward eye prepass can select family 5
        /// without a GPU readback.
        uint32_t toon_family = 0;

        /**
         * THE MATERIAL'S OUTLINE WIDTH (`toon_colour_lane::outline_edge`'s `.w`, i.e. the game's
         * `_OutlineWidth`), mirrored onto the primitive so the FRAME can decide which leaves the ① outline
         * group draws without a GPU readback.
         *
         * IT IS THE SAME KIND OF FACT AS `overlay_kind` ABOVE AND IS MIRRORED FOR THE SAME REASON: the colour
         * lanes are uploaded into `core::heap_slots::toon_colours`, one `vec4` per lane per material, and the
         * per-material `material_record` is fixed at 96 bytes and explicitly refused growth (see the record's
         * own note). The frame's leaf lists are therefore built while the only thing available is the
         * primitive, exactly as they are for the overlay channel - and the shader reads the lane itself for the
         * width, so this mirror is a LIST-BUILDING gate and never a second source of truth for the number.
         *
         * 0.0f MEANS "THIS MATERIAL HAS NO OUTLINE", WHICH IS THE `toon_inputs` DEFAULT for that lane's `.w`
         * and the article's own gate (`outline.slang` tests `w > 0`). It is NOT the white the other three lanes
         * default to: white in `.w` would be a width of 1.0 around every material that states no
         * `_OutlineWidth` row, which is the whole character.
         */
        float outline_width = 0.0f;

        /**
         * THE STATIC SURROUND FLAG, mirrored from `primitive_create_info::environment` - see that field's
         * note for what it is FOR and which two leaf lists it changes. Here rather than in the material
         * record for the same reason as `overlay_kind` and `outline_width` above: the frame's leaf lists
         * are built from primitives, before any shader could read a lane.
         */
        bool environment = false;

        /**
         * WHICH SHADING STAGE OWNS THIS LEAF (see `deren::engine::shading_path`).
         *
         * Default `deferred`, i.e. the ordinary G-buffer + deferred-lighting path. The host sets `character` on the
         * leaves of a GameToon object and on the historic single-character path; the character-forward stage then
         * re-shades exactly those leaves and nothing else. `splat` is reserved for the Gaussian stage.
         */
        shading_path path = shading_path::deferred;

        /**
         * PER-OBJECT SHADOW FILTER: false keeps this leaf out of the shadow-caster list while it still draws
         * in the visible pass. Default true preserves the historic "everything visible casts" behaviour.
         * Set it per placement by walking that object's leaves; the frame builder reads it once when it
         * fills the caster list, so a runtime edit needs runtime::scene_changed() like any other leaf change.
         */
        bool casts_shadow = true;

        /// which GameToon character owns this leaf; copied into the high bits of `push.flags` at creation. See
        /// `primitive_create_info::character_index`.
        uint32_t character_index = 0u;

        // local-space AABB of this primitive's geometry (model space, i.e. before push.model);
        // filled by the runtime when the geometry is uploaded. has_bounds == false means "no
        // single world AABB" (e.g. an instanced primitive spreads over many transforms) and the
        // primitive is never frustum-culled.
        glm::vec3 local_aabb_min = glm::vec3(0.0f);
        glm::vec3 local_aabb_max = glm::vec3(0.0f);
        bool has_bounds = false;

        /// number of joint matrices in this primitive's skin block; 0 = unskinned. Set by the
        /// animation controller together with `push.skin_base`, so the cull can union the bind
        /// box through every joint and get a conservative bound for the posed geometry.
        uint32_t skin_joint_count = 0u;

        /**
         * @brief whether this primitive's vertices can move without its node matrix changing
         *
         * A skinned or morphed draw is deformed in the vertex shader, so `local_aabb_*` describes
         * the bind pose, not what the draw will cover. The cull keeps such a primitive out of the
         * static BVH; a skinned primitive gets a conservative posed bound from
         * `skinned_world_aabb`, while a morph-only primitive (no joint matrices) stays always
         * visible.
         */
        [[nodiscard]] bool deforming() const noexcept {
            return this->push.skin_base != 0u || this->push.morph_targets != 0u;
        }

        /**
         * @brief transform the local AABB by the primitive's current world matrix (push.model,
         *        written by update_world -> set_world) into a world-space AABB
         * @return world AABB; {0,0,0}..{0,0,0} when has_bounds == false
         */
        [[nodiscard]] std::pair<glm::vec3, glm::vec3> world_aabb() const noexcept {
            if (!this->has_bounds) {
                return {};
            }
            glm::vec3 wmin = glm::vec3(std::numeric_limits<float>::infinity());
            glm::vec3 wmax = glm::vec3(-std::numeric_limits<float>::infinity());
            for (int32_t i = 0; i < 8; ++i) {
                glm::vec3 const corner{
                    (i & 1) ? this->local_aabb_max.x : this->local_aabb_min.x,
                    (i & 2) ? this->local_aabb_max.y : this->local_aabb_min.y,
                    (i & 4) ? this->local_aabb_max.z : this->local_aabb_min.z,
                };
                glm::vec4 const world = this->push.model * glm::vec4(corner, 1.0f);
                wmin = glm::min(wmin, glm::vec3(world));
                wmax = glm::max(wmax, glm::vec3(world));
            }
            return {wmin, wmax};
        }

        /**
         * @brief conservative world AABB of a skinned primitive for the current joint block
         *
         * Every vertex is a convex blend of the same local position through up to four joints, so
         * the union of the bind box transformed by ALL joints in the block contains every possible
         * blended position. The joint matrices are object-local (the animation controller already
         * folds the mesh's inverse world into them), hence `push.model * joint` before taking the
         * bounds. `matrices` is the runtime's current CPU copy of the uploaded skin block and must
         * cover `push.skin_base + skin_joint_count`.
         */
        [[nodiscard]] std::pair<glm::vec3, glm::vec3> skinned_world_aabb(std::span<glm::mat4 const> const matrices) const noexcept {
            glm::vec3 wmin = glm::vec3(std::numeric_limits<float>::infinity());
            glm::vec3 wmax = glm::vec3(-std::numeric_limits<float>::infinity());
            for (uint32_t joint = 0; joint < this->skin_joint_count; ++joint) {
                glm::mat4 const skin_world = this->push.model * matrices[static_cast<std::size_t>(this->push.skin_base) + joint];
                for (int32_t i = 0; i < 8; ++i) {
                    glm::vec3 const corner{
                        (i & 1) ? this->local_aabb_max.x : this->local_aabb_min.x,
                        (i & 2) ? this->local_aabb_max.y : this->local_aabb_min.y,
                        (i & 4) ? this->local_aabb_max.z : this->local_aabb_min.z,
                    };
                    glm::vec4 const world = skin_world * glm::vec4(corner, 1.0f);
                    wmin = glm::min(wmin, glm::vec3(world));
                    wmax = glm::max(wmax, glm::vec3(world));
                }
            }
            return {wmin, wmax};
        }

        /**
         * @brief store the world transform accumulated by the owning scene tree node
         * @param world the node's world matrix (parent_world * local)
         * @note the primitive's world transform lives in push.model, which draw() pushes as-is
         */
        void set_world(glm::mat4 const& world) override;

        /**
         * @brief record the primitive's draw commands (the frame's heap is bound by the caller, so
         *        this frame slot's scene block is already reachable; the pipeline the primitive draws with and
         *        the command buffer to record into both come from @p env)
         * @param env the recording session's render environment: the session command buffer,
         *        default / named pipeline binding (deduplicated) + the shared push-constant
         *        layout. One instance per recording thread, never shared across workers.
         */
        virtual void draw(render_environment& env) const = 0;
        /// THE ALLOCATOR IS GONE FROM THIS HOOK (the boundary batch's import cleanup): it took a
        /// `vma_allocator&`, which was the ONLY reason this module imported `deren.vulkan.core` - and every
        /// implementation already ignored it, because the geometry is owned by CONTRACT handles
        /// (`rhi::unique_object<rhi::buffer>`) whose `reset()` is the whole of what this does. No caller
        /// anywhere passed one either. The hook itself now depends on no backend type.
        virtual void destroy() noexcept = 0;
        [[nodiscard]] virtual bool is_valid() const noexcept = 0;

    protected:
        /// the lane push itself: @p host_culled and @p backface_legal are the flags the lanes carry (both false for
        /// a session that does not cull, which is why this is the body `push_meshlet_lanes` calls rather than a
        /// wrapper the vertex path used to share)
        void push_geometry_lanes_impl(render_environment const& env, primitive const& geometry, uint32_t first_index, uint32_t index_count, int32_t base_vertex, bool host_culled, bool backface_legal) const;
        /**
         * @brief CULL a meshlet session's run against the camera and push the geometry lanes for it
         *
         * @param env the session (see `render_environment::meshlet_culled`)
         * @param geometry the primitive whose meshlets this draw covers
         * @param first_index the draw's own first index, used when the session is not a meshlet one
         * @param index_count the draw's own index count, same condition
         * @param base_vertex the draw's own base vertex, same condition (a meshlet session's lanes carry
         *        the primitive's run instead, see the notes below)
         * @param material_two_sided the draw's material is two-sided, which keeps both sides: no
         *        back-face test may reject a meshlet for facing away from the camera
         * @return the number of meshlets LEFT after culling, or `not_culled` when this draw is not culled at all -
         *         which `mesh_dispatch` reads as "dispatch the primitive's whole run" (the table-driven behaviour)
         * @note the flag travels in the lanes' `base_vertex`, which a meshlet session never uses for anything else:
         *       it is what tells the entry point to read this frame's CULLED table instead of the shared one.
         */
        uint32_t push_meshlet_lanes(render_environment const& env, primitive const& geometry, uint32_t first_index, uint32_t index_count, int32_t base_vertex, bool material_two_sided) const;
        /// the `push_meshlet_lanes` answer that means "not culled" (a real run cannot be this long: the table's
        /// capacity is far below it, so no dispatch can collide with the sentinel)
        static constexpr uint32_t not_culled = 0xFFFFFFFFu;
        /**
         * @brief record ONE MESH DISPATCH over a geometry window already pushed into the stage block
         * @param env the session; it must have `mesh_stage` set and its mesh endpoints filled
         * @param index_count how many indices the draw covers (the dispatch's group count follows from it)
         * @param instance_count how many instances to dispatch: a mesh command has no instanceCount, so the
         *        INSTANCE becomes the workgroup grid's Y and the stage reads it as its instance index
         * @param survivors what the host's cull left, or `not_culled`: a culled run dispatches exactly the survivors,
         *        which is the whole point of culling before the dispatch (see push_meshlet_lanes)
         * @note nothing is BOUND here: a mesh pipeline ignores the vertex input state, so binding the buffers
         *       would be a command with no effect - the stage reaches them through the pushed lanes instead,
         *       which is the whole difference between this and bind_geometry_and_push.
         */
        void mesh_dispatch(render_environment const& env, primitive const& geometry, uint32_t index_count, uint32_t instance_count, uint32_t survivors = not_culled) const;
    };

    /**
     * @ingroup engine_primitive
     * @brief the standard primitive: one indexed draw of its own geometry (push.model places it)
     */
    export class normal_draw_primitive final : public primitive {
    public:
        /// Creation-time binding only: caller must obey the existing idle-runtime
        /// rule. Both buffer views lease the entire immutable CPU/GPU generation.
        bool bind_subtilis_geometry(std::shared_ptr<subtilis::uploaded_geometry const> generation) noexcept {
            if (!generation || this->vertex_buffer || this->index_buffer || this->geometry_resources || this->subtilis_geometry || this->draw_index_count || this->meshlet_count || !this->meshlets.empty()) {
                return false;
            }
            auto const& source = generation->source();
            this->vertex_buffer.bind(generation->vertices(), generation);
            this->index_buffer.bind(generation->indices(), generation);
            this->index_type = deren::promise::rhi::index_type::uint32;
            this->draw_index_count = source.lod0_index_count();
            this->vertex_count = source.vertex_count();
            this->vertex_stride = source.vertex_stride();
            this->subtilis_geometry = generation;
            this->geometry_resources = std::move(generation);
            return true;
        }

        void draw(render_environment& env) const override;
        void destroy() noexcept override;
        [[nodiscard]] bool is_valid() const noexcept override;
    };

    /**
     * @ingroup engine_primitive
     * @brief instanced primitive: draws the geometry of another primitive (source)
     *        instance_count times in ONE draw call; per-instance world transforms come from the
     *        runtime's instance transform buffer (scene block slot 6, push flag bit0). Owns
     *        nothing: geometry belongs to source, destroy() is a no-op, source must outlive it.
     */
    export class instanced_draw_primitive final : public primitive {
    public:
        primitive const* source = nullptr;
        uint32_t instance_count = 0;

        void draw(render_environment& env) const override;
        void destroy() noexcept override;
        [[nodiscard]] bool is_valid() const noexcept override;
    };

    /**
     * @ingroup engine_primitive
     * @brief one chunk of a static_draw: an index sub-range of the merged buffer plus the
     *        material this chunk draws with (each chunk may bind a different material, so one
     *        merged buffer can hold many sub-meshes with distinct materials)
     */
    export struct static_draw_chunk {
        uint32_t first_index = 0;   // first index of this chunk in the merged index buffer
        uint32_t index_count = 0;   // number of indices this chunk draws
        uint32_t vertex_offset = 0; // base vertex into the merged vertex buffer (chunks past the
                                    // first when the packer did not remap indices; 0 otherwise)
        // per-chunk material (same shape as primitive_create_info's material fields)
        texture_input albedo = {};
        texture_input metallic_roughness = {};
        texture_input normal = {};
        texture_input occlusion = {};
        texture_input emissive = {};
        material_factors factors = {};
        bool double_sided = false;
    };

    /**
     * @ingroup engine_primitive
     * @brief build description for a static draw: ONE merged vertex/index buffer
     *        (the packer's output) plus the chunk table over it. Each chunk is drawn as a
     *        single offset draw call after ONE buffer bind, so N static sub-meshes cost 1 bind
     *        + N draws instead of N binds + N draws.
     * @note the chunk table is REQUIRED (a non-empty, validated list): every chunk's index
     *       window and vertex references are checked against the merged buffers at
     *       static-draw build time - out-of-range chunks are logged and skipped.
     */
    export struct static_draw_create_info {
        std::span<uint8_t const> vertex_data = {};
        uint32_t vertex_stride = 0;
        uint32_t vertex_count = 0;
        std::span<uint8_t const> index_data = {};
        deren::promise::rhi::index_type index_type = deren::promise::rhi::index_type::uint32;
        uint32_t index_count = 0; // whole merged index count (the chunk table covers a subset)
        deren::utility::vessel::vector<static_draw_chunk> chunks = {};
        glm::mat4 model_matrix = glm::mat4(1.0f);
    };

    /**
     * @ingroup engine_primitive
     * @brief static batch primitive: OWNS one merged vertex/index buffer and draws a chunk
     *        table over it — every chunk shares the single buffer bind, each chunk is one
     *        offset draw with its own material (push.material_index). Self-contained: no
     *        source primitive to outlive, destroy() releases the owned buffers like a normal
     *        draw. This is the primitive-level form of a static scene: one buffer, one bind,
     *        N offset draws. Placement works like every other leaf: the node's local
     *        transform (set from static_draw_create_info::model_matrix by the static-draw builder)
     *        becomes push.model via update_world, so the whole batch shares one world
     *        transform; per-chunk placement needs separate batches or per-chunk model baking
     *        later.
     * @note AABB: one local box over the whole merged geometry (batch-level frustum culling);
     *       per-chunk AABBs would need chunk-level culling, deferred.
     */
    export class static_draw_primitive final : public primitive {
    public:
        // merged geometry: owned the same way normal_draw owns its two buffers (see the note there)
        deren::promise::rhi::unique_object<deren::promise::rhi::buffer> vertex_buffer = {};
        deren::promise::rhi::unique_object<deren::promise::rhi::buffer> index_buffer = {};
        deren::promise::rhi::index_type index_type = deren::promise::rhi::index_type::uint32;
        uint32_t index_count = 0; // whole merged index count (upper bound for chunk validation)
        uint32_t vertex_count = 0;
        // chunk table over the merged buffer; each entry draws once after the single bind.
        // Material identity lives in material_index; double_sided is per chunk (cull mode).
        // Always non-empty after a static draw is built (chunks are validated there).
        struct chunk_record {
            uint32_t first_index = 0;
            uint32_t index_count = 0;
            uint32_t vertex_offset = 0;
            material_id material_index = {};
            bool double_sided = false;
        };
        deren::utility::vessel::vector<chunk_record> chunks = {};

        void draw(render_environment& env) const override;
        void destroy() noexcept override;
        [[nodiscard]] bool is_valid() const noexcept override;
    };

    /**
     * @ingroup engine_primitive
     * @brief build the camera UBO from orbit camera state (the camera orbits the target point)
     * @param yaw yaw angle in radians (see deren::engine::runtime::camera)
     * @param pitch pitch angle in radians
     * @param distance camera distance from the target
     * @param target the point the camera looks at and orbits around (e.g. the centered scene origin,
     *        or the scene sink so the camera follows the model)
     * @param scene_radius conservative radius of the scene around @p target (bounds radius); the
     *        projection far plane always covers target + scene_radius so zooming in never clips
     *        the far side of the scene
     * @param aspect swapchain width / height
     * @return camera UBO with view/proj/camera_pos filled in
     * @note proj uses perspectiveRH_ZO with a Y flip to match Vulkan's y-down framebuffer
     */
    export camera_ubo make_orbit_camera_ubo(
        float yaw,
        float pitch,
        float distance,
        glm::vec3 const& target,
        float scene_radius,
        float aspect);

    /// Six RH 90-degree views (+X,-X,+Y,-Y,+Z,-Z), retaining Vulkan Y flip and normal winding.
    /// Readback X must be mirrored for canonical cubemap pixels. Requires face<6, 0<near<far.
    export camera_ubo make_cube_capture_camera(glm::vec3 const& position, uint32_t face,
                                               float near_plane, float far_plane);

    /**
     * @ingroup engine_primitive
     * @brief the ARROW-KEY camera pan for one frame: the world-space translation to add to the orbit
     *        camera's target (the point the eye orbits, so translating it slides the whole rig without
     *        rotating the view).
     * @param yaw the rig's yaw in radians - the same angle make_orbit_camera_ubo places the eye with,
     *        so the camera's horizontal view direction is (-sin yaw, 0, -cos yaw) and its right vector
     *        is (cos yaw, 0, -sin yaw)
     * @param distance the rig's distance from the target; the pan speed scales with it, so a zoomed-out
     *        view crosses the scene at the same on-screen rate as a close one
     * @param strafe -1 (left) .. +1 (right): moves along the camera's right vector (horizontal)
     * @param rise -1 (down) .. +1 (up): moves along WORLD up, (0, 1, 0). The first cut moved along the
     *        horizontal view direction instead and was rejected on sight: pushing the eye toward the
     *        subject reads as a zoom (the subject grows), not as the up/down slide the arrow keys ask
     *        for. World up (not the camera's screen-up) keeps the gesture the same at every pitch and
     *        degenerate at none.
     * @param dt seconds since the previous frame; clamped inside (see the note) so a stalled frame
     *        cannot teleport the camera
     * @param fast true for the SHIFT speed multiplier (4x)
     * @return the target's translation for this frame (world units; y is non-zero only while UP/DOWN
     *         is held)
     * @note pure function: the runtime owns the time base (glfwGetTime) and the key state, so the
     *       arithmetic is unit-testable headlessly. Both axes are normalized together, so a diagonal
     *       press is not faster than a straight one, and no key held returns exactly zero (an idle
     *       frame never touches the camera).
     */
    export glm::vec3 orbit_camera_pan_delta(float yaw, float distance, float strafe, float rise, float dt, bool fast);

    /**
     * @ingroup engine_primitive
     * @brief build the directional light UBO (light-space view-proj + direction) for shadow
     *        mapping. The light direction matches the analytic sky sun (see sky.glsl), so
     *        shadows, the PBR direct light and the visible sun disc all agree.
     * @param sun_direction the sun's direction in world space, pointing FROM the surface TOWARD the sun; it
     *        is normalized here and it is the SINGLE source for three consumers that must not disagree -
     *        this UBO's own matrices, the shader's `light_dir`, and the disc the sky draws (which reads this
     *        block's `light_dir`, so the sky cannot put its sun where the shadows do not fall). It comes from
     *        `[lighting] sun_direction`, which is what makes the light movable without rebuilding.
     * @param scene_center world-space center of the shadow frustum (e.g. the imported scene
     *        bounds center after the scene offset is applied)
     * @param scene_radius conservative radius covering the shadow casters
     * @return light UBO with an orthographic view-proj framing the scene bounds
     * @note ortho box sized to cover a sphere of the given radius around scene_center, along @p sun_direction
     */
    export light_ubo make_directional_light_ubo(glm::vec3 const& sun_direction, glm::vec3 const& scene_center, float scene_radius, float shadow_map_size);
} // namespace deren::engine
