module;

// The slot grid's constants are Vulkan values (VkDeviceSize / VkFormat-free numbers), and a module both
// halves compile must include the header itself (the same rule deren.vulkan.constant_init follows).
#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

/// THE RENDERER'S SLOT LAYOUT, COMPILED BY BOTH HALVES (③-D/E batch, ruling D).
///
/// WHY IT IS NOT A MEMBER OF `core` ANY MORE: the grid is the AGREEMENT between what the host writes and
/// what a heap-native shader bakes as `array[index]` - the ENGINE names it in ~95 places (its heap
/// writes, its pass bookkeeping), and the engine cannot name a backend class member without importing
/// the backend's module. The values, the names and the comments move verbatim; `core` keeps aliases plus
/// a drift guard (see `core.declarations.cppm`), so both spellings work while the call sites migrate.
///
/// WHY A SHARED TARGET AND NOT A COPY PER HALF: `vulkan_constant_init` is compiled ONCE and linked by
/// both `deren_vulkan` and `vulkancorekit` (the shape `promise` proves), which is the only arrangement
/// that cannot drift - two BMIs of one module name cannot both be visible to one TU.
///
/// THE NUMBERS ARE MIRRORED IN shaders/heap_slots.glsl AND shaders/heap_slot_constants.glsl, and
/// tests/test_render_resources.cpp holds the two sides equal by parsing BOTH files; that test's path
/// moved here with the grid.
export module deren.vulkan.render_layout;

export namespace deren::vulkan::render_layout {

    // ================================================================================================
    // THE RENDER CHAIN'S FORMATS (③-D/E item A1, A1.0). They sat in `core` as `core::hdr_format` and
    // friends; the ENGINE creates these targets now, so the formats it creates them WITH have to be
    // nameable without importing the backend. `core` keeps aliases plus a drift guard, and the numbers
    // below are the ones the renderer shipped with - a change here without a change there is a compile
    // error, which is the whole point of moving them rather than copying them.
    // ================================================================================================

    /**
     * @brief format of the HDR scene target the deferred lighting stage renders into and the post-process
     *        pass samples: the scene color target uses it, and each swapchain image owns one
     *        single-sample resolve target in it.
     */
    inline constexpr VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    /**
     * @brief how many color targets the G-buffer pass writes (see @ref gbuffer_formats)
     */
    inline constexpr uint32_t gbuffer_target_count = 3;

    /**
     * @brief formats of the G-buffer targets, in attachment order (= the fragment output locations
     *        of shaders/gbuffer.slang), and the reason the deferred path is cheap to store:
     *        - 0 RGBA8_UNORM: albedo.rgb (base color, linear) + metallic in a
     *        - 1 RGBA16F: world normal.xyz (no encoding - the conservative layout trades 4 bytes per
     *          pixel for not having to reason about octahedral precision) + roughness in a
     *        - 2 RGBA8_UNORM: material_id low/high byte + ambient occlusion + material flags
     *        16 bytes per pixel in total; the depth is the pass's own single-sampled depth image.
     * @note every target is single-sampled (1x) on purpose: a G-buffer cannot be multisampled
     *       without per-sample shading, which is the trade that makes TAA the anti-aliasing
     *       (the anti-aliasing story is TAA/FXAA on the lit image instead).
     */
    inline constexpr std::array<VkFormat, gbuffer_target_count> gbuffer_formats = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM,
    };

    /**
     * @brief motion-vector format: the fourth G-buffer target, written by the G-buffer pass and read
     *        by TAA (RG16F because a motion vector is a signed sub-pixel quantity in UV space and
     *        8-bit would quantize it to ~1/255 of the screen - coarser than the jitter TAA exists to
     *        resolve)
     */
    inline constexpr VkFormat gbuffer_velocity_format = VK_FORMAT_R16G16_SFLOAT;

    /**
     * @brief THE SLOT GRID: where every descriptor this renderer uses lives, in SLOTS of @ref heap_slot_stride
     *        bytes, and why an index is a slot number rather than a byte offset
     *
     * @note A HEAP-NATIVE SHADER CAN ONLY NAME `array[index]`, which GL_EXT_descriptor_heap resolves to
     *       `heapBase + index * stride`. Every such array aliases the heap FROM OFFSET 0, so an index IS a
     *       byte offset divided by that array's stride - and the only way a shader can find its data at a
     *       compile-time-known index is for the data to sit on a grid whose base is FIXED. Both heaps start
     *       their grid at 1 MiB (past any reserved window a driver reports here - 94 KiB of resource heap,
     *       64 KiB of sampler heap - and the same number on every device), the stride is 64 B, and every
     *       array below is a CONTIGUOUS RUN OF SLOTS.
     *
     * @note @ref heap_slot_stride IS AN OVERRIDE, not the device's stride: the resources are mixed kinds
     *       (16 B buffers, 32 B images at this device), and one power-of-two stride covering all of them is
     *       what makes a single grid possible - the shader declares `descriptor_stride = 64` and the host
     *       writes 64 B apart. A device whose largest descriptor exceeded it would make the grid ambiguous,
     *       so that is checked once at startup and the grid is REFUSED rather than wrong.
     *
     * @note THESE NUMBERS ARE MIRRORED IN shaders/heap_slots.glsl and they have to be, because the shader
     *       bakes its base indices. A drift is invisible to validation and shows up as a wrong picture, so
     *       the constructor logs this table (see the `descriptor heap: slot grid` line).
     */
    inline constexpr VkDeviceSize heap_slot_stride = 64;
    /// THE GRID'S BYTE OFFSET FOR A SLOT (see core::heap_slots and docs/descriptor_heap_migration.md): every
    /// descriptor is 64 B from the next, so a write HERE and a heap-native shader's `array[slot]` with
    /// `descriptor_stride = 64` are the same address by construction. There is no second stride to disagree
    /// with - which is exactly what the older per-slot block could not promise, because it mixed the device's
    /// 16 B buffer stride with its 32 B image stride and put each binding at its own offset.
    /// @note A SLOT NUMBER IS ALREADY ABSOLUTE: `core::heap_slots::x` includes `heap_slot_base`, so this is a
    ///       multiply and nothing else. The first version added `heap_grid_offset` as well and doubled the
    ///       1 MiB base - every write landed past the heap and was refused, which the heap's own bounds check
    ///       reported (`... did not fit at offset 2130432`).
    /// @note THIS IS THE ONE COPY: it used to be duplicated in runtime:constructor, runtime:frames and
    ///       runtime.cpp, which is what a helper defined in terms of the stride below invites. It belongs
    ///       beside the constant it multiplies, and it is exported so those units can drop their own.
    [[nodiscard]] inline constexpr VkDeviceSize heap_slot_offset(uint32_t const slot) noexcept {
        return static_cast<VkDeviceSize>(slot) * heap_slot_stride;
    }

    // NOT renamed, unlike the other members that round: `heap_slot_base` is a NAME CONTRACT with
    // shaders/heap_slot_constants.glsl, which declares the same constant under the same name, and
    // tests/test_render_resources.cpp parses both files and requires the names AND the values to match
    // (tests/test_goo_toon_math.cpp greps this line by that name too). The C4458 that a lambda parameter of
    // this name caused in core.constructor.cppm is fixed THERE instead, by renaming the parameter - a local,
    // so the contract stays intact.
    inline constexpr uint32_t heap_slot_base = 16384;  // 1 MiB / 64 B: the grid's slot 0
    inline constexpr uint32_t heap_slot_count = 1024;  // 703 slots are in use, the rest is room to grow
    inline constexpr uint32_t heap_image_capacity = 8; // per-swapchain-image arrays (3-4 images in practice)
    /// The SAMPLER heap is a second grid with its own base, and it cannot share the resource one: the API caps
    /// the sampler heap at 128 KiB, so 64 KiB - the reserved window the embedded-sampler path requires - is the
    /// largest base it can have. Its stride is the device's own sampler descriptor size (32 B here).
    inline constexpr uint32_t heap_sampler_base = 2048; // 64 KiB / 32 B
    inline constexpr VkDeviceSize heap_sampler_stride = 32;
    struct heap_slots {
        static constexpr uint32_t textures = heap_slot_base + 0u;    // binding 1, the bindless array
        static constexpr uint32_t materials = heap_slot_base + 512u; // binding 5
        /**
         * @brief the top level structure, binding 16, as a TWO-SLOT array - and the reason it is not beside the
         *        per-frame buffers above
         *
         * @note THE TLAS IS REBUILT EVERY FRAME (see runtime::write_rt_structure_binding, which is called per
         *       frame slot), so a single slot would hold one frame's structure while the other frame is still
         *       in flight - the same hazard the camera and light UBOs have, and they are two slots each for it.
         *       This was first laid out as ONE slot beside the per-frame buffers, which is the kind of mistake
         *       a picture cannot show until the shaders read the heap: the fix is cheap now and would have been
         *       a silent wrong image later. It lives at the END of the used region because growing it in place
         *       would renumber every array after it.
         */
        static constexpr uint32_t tlas = heap_slot_base + 703u;
        /**
         * @brief the MESHLET TABLE (docs/mesh_shaders.md step 3): one 48-byte record per meshlet
         *
         * @note ONE SLOT, not a per-frame pair, and that is a property of the data rather than a shortcut: the
         *       table is written ONCE, while the scene is imported and before any frame is recorded, and never
         *       touched again - so there is no frame in flight whose contents could disagree with it. (The TLAS
         *       above is the counter-example: it is rebuilt every frame, which is why it owns two slots.)
         * @note it lives at the END of the used region for the reason stated there: the arrays above are
         *       numbered by their position, so growing one in place would renumber everything after it.
         */
        static constexpr uint32_t meshlets = heap_slot_base + 745u;
        /**
         * @brief the MESH CULLING COUNTERS (docs/mesh_shaders.md step 3, "what the culling buys"): one per-frame
         *        lane of 8 uints, added to by the mesh entries and read back by the host at shutdown
         *
         * @note ON THE HEAP rather than in a host-only buffer, because a mesh stage's only way to reach memory
         *       is a heap descriptor - there is no binding model left to hang a counter off. The read-back is a
         *       plain mapped read after `wait_idle`, which is why the buffer is host-visible and coherent.
         */
        static constexpr uint32_t meshlet_stats = heap_slot_base + 746u;
        /**
         * @brief THE HOST-CULLED MESHLET TABLE (docs/mesh_shaders.md step 3, the culling's cheapest stage): a
         *        per-frame lane of `meshlet_capacity` records, written by the host while it records a CULLED
         *        session's draws and read by that session's mesh entry
         *
         * @note per frame rather than one slot, unlike the table itself: this one is rewritten every frame from
         *       the camera, so the frame in flight that is being recorded must not overwrite the one the GPU is
         *       still reading - the rule every per-frame buffer in this renderer follows.
         * @note the compaction is the whole point: the host writes SURVIVORS contiguously, so the dispatch's
         *       group count is the survivor count and no workgroup is launched for a culled meshlet.
         */
        static constexpr uint32_t meshlet_culled = heap_slot_base + 747u;
        // THE TOON LANES THAT DO NOT FIT THE MATERIAL RECORD: one `uvec4` per material, written once at
        // import. x is the face SDF map's texture-array index (`_SDFLightmap`), y the metallic/gloss map's
        // (`_MetallicGlossMap`), and z and w are reserved for the lanes the rest of the character work
        // adds. See heap_slot_constants.glsl for why they are a buffer of their own rather than components
        // of the material record, and why it is one descriptor rather than a per-frame pair.
        static constexpr uint32_t toon_lanes = heap_slot_base + 748u;
        // THE HEAD FRAME the face SDF shades against, one block per frame slot because on a model whose
        // head turns it changes every frame. It is its own block rather than a field of the camera's: the
        // head frame belongs to the CHARACTER, not to the eye looking at it.
        static constexpr uint32_t scene_head = heap_slot_base + 749u;
        // THE TOON LIGHT RIG (`deren::vulkan::toon_rig`): the character stage's global numbers - the sun/head-light
        // split, their shadow-side colours and the chain's scalars - written once from the application's
        // config. ONE descriptor and not a per-frame pair, because the values are fixed for a run exactly as
        // the material table's and the toon lane table's are: nothing writes it while a frame is in flight.
        //
        // ONE SLOT PAST THE HEAD BLOCK AND NOT BESIDE IT, which is a MEASURED correction rather than
        // spacing: `scene_head` above is a PER-FRAME-SLOT array, so it occupies 749 AND 750 (two frames in
        // flight), and a rig placed at 750 was therefore overwritten every frame by the second slot's head
        // frame. What that looked like was not a crash and not a validation error: the rig simply read as the
        // head frame's own numbers - `_DayStrength` came out of the head basis' first lane, i.e. zero - so
        // the two-state lighting silently sat in its NIGHT state for every frame of both captures. The
        // captures differed by 0 pixels before this line moved.
        static constexpr uint32_t toon_rig = heap_slot_base + 751u;
        // THE MATERIAL COLOURS (see `deren::vulkan::toon_colour_lane`): one `vec4` per lane per material, written once
        // at import from the same sidecar the texture lanes come from. A buffer of its own because the value is
        // FOUR FLOATS and the material record has nowhere to put it here: the record is INLINE in the per-draw
        // push block (see `sdf_lanes` above for the measurement), so a colour lane could not be a record field
        // even if the record had room. The REFERENCE PORT solved the same problem by growing ITS record from
        // 128 B to 192 B - a shape this renderer cannot copy, and does not need to, because the lane table
        // pattern already exists here.
        //
        // ... AND BECAUSE THEY LIVE HERE RATHER THAN IN THE RECORD, THEY ARE THEIR OWN TERM OF THE MATERIAL
        // DEDUP KEY: `material_slot_cache` (see `runtime.declarations.cppm`) keys the record, the two texture
        // lane blocks AND these six lanes' bytes, because `register_material` writes this table only AFTER its
        // early return - six lanes left out of the key are six lanes the second of two record-identical
        // materials reads from the first one. See the "材质去重键补上 colour lanes" section of `remaining_port_spec.md` for the probe.
        static constexpr uint32_t toon_colours = heap_slot_base + 752u;
        /// THE ARTICLE'S POST LUT: see `runtime::set_post_lut` and `heap_slots_post_lut` in the shader's slot
        /// file, which `test_render_resources` holds against this spelling.
        static constexpr uint32_t post_lut = heap_slot_base + 753u;
        /**
         * THE GOO REFERENCE'S PRE-INTEGRATED FGD LOOKUP TABLE (see `runtime::set_goo_fgd_lut` and
         * `heap_slots_goo_fgd_lut` in the shader's slot file, which `test_render_resources` holds against this
         * spelling).
         *
         * 754 AND NOT 750: `scene_head` is a PER-FRAME-SLOT array and occupies 749 AND 750 - the note on
         * `toon_rig` above records what happened to the last constant that forgot this - so the next free slot
         * above the post LUT is 754.
         *
         * IT IS A GLOBAL IMAGE RATHER THAN A MATERIAL LANE, which is the architecture ruling the step-5 spec
         * makes from the reference's own graph (§3.4): the Goo `GetPreIntegratedFGDGGXAndDisneyDiffuse` group
         * has ONE `ShaderNodeTexImage`, `users == 3` containers share it, and its coordinate is computed from
         * `sqrt(NoV)` / `perceptualRoughness` / `fresnel0` - never from a material's uv. Routing it through
         * `toon_slot` would need eleven sidecar rows pointing at one file, and zero new information.
         */
        static constexpr uint32_t goo_fgd_lut = heap_slot_base + 754u;
        static constexpr uint32_t scene_camera = heap_slot_base + 514u;        // binding 0, per frame slot
        static constexpr uint32_t scene_light = heap_slot_base + 516u;         // binding 7, per frame slot
        static constexpr uint32_t cluster_counts = heap_slot_base + 518u;      // binding 11, per frame slot
        static constexpr uint32_t cluster_indices = heap_slot_base + 520u;     // binding 12, per frame slot
        static constexpr uint32_t instance_transforms = heap_slot_base + 522u; // binding 6, per frame slot
        /** @brief the transforms of the previous frame, kept for motion vectors and TAA reprojection */
        static constexpr uint32_t previous_transforms = heap_slot_base + 524u; // binding 13, per frame slot
        /** @brief the skinned joint matrices for this frame, written by the skinning pass */
        static constexpr uint32_t skin_matrices = heap_slot_base + 526u;   // binding 9, per frame slot
        static constexpr uint32_t morph_data = heap_slot_base + 528u;      // binding 10, per frame slot
        static constexpr uint32_t mask_instances = heap_slot_base + 530u;  // binding 17, per frame slot
        static constexpr uint32_t env_cube = heap_slot_base + 532u;        // binding 2
        static constexpr uint32_t irradiance_cube = heap_slot_base + 533u; // binding 3
        static constexpr uint32_t brdf_lut = heap_slot_base + 534u;        // binding 4
        static constexpr uint32_t shadow_map = heap_slot_base + 535u;      // binding 8, per image
        /** @brief the ray-traced visibility image, sampled by the lighting stage */
        static constexpr uint32_t rt_visibility = heap_slot_base + 543u; // binding 15, per image
        /**
         * @brief the SAME image as @ref rt_visibility, as a STORAGE descriptor instead of a sampled one
         *
         * @note TWO DESCRIPTORS FOR ONE IMAGE, and not redundancy: SAMPLED_IMAGE and STORAGE_IMAGE are different
         *       descriptor kinds and no single heap descriptor is both, while this image is WRITTEN by the
         *       ray-traced visibility pass and SAMPLED by the lighting stage. It lives at the end of the used
         *       region for the same reason the TLAS does - growing an array in place would renumber every array
         *       after it.
         */
        static constexpr uint32_t rt_visibility_storage = heap_slot_base + 711u;
        static constexpr uint32_t gbuffer_albedo = heap_slot_base + 551u; // per image, then five in a row
        static constexpr uint32_t gbuffer_normal = heap_slot_base + 559u;
        static constexpr uint32_t gbuffer_material = heap_slot_base + 567u;
        static constexpr uint32_t gbuffer_depth = heap_slot_base + 575u;
        static constexpr uint32_t gbuffer_velocity = heap_slot_base + 583u;
        static constexpr uint32_t ml_trace = heap_slot_base + 591u; // per image: megalights' chain
        /// @brief the same two images written as STORAGE descriptors (see @ref rt_visibility_storage)
        /// @note their compute passes WRITE them and the lighting stage SAMPLES them, and no single heap
        ///       descriptor is both kinds - so the trace and the resolve each need a second slot, at the end of
        ///       the used region for the same reason the others are there.
        static constexpr uint32_t ml_trace_storage = heap_slot_base + 719u;
        static constexpr uint32_t ml_resolved_storage = heap_slot_base + 735u;
        /**
         * @brief the joint blocks as they were ONE FRAME AGO, per frame slot: the deformation half of a
         *        motion vector
         *
         * @note A SECOND per-frame family rather than more slots inside @ref skin_matrices, because a
         *       vertex's motion vector needs the matrices the PREVIOUS frame drew with and the current
         *       buffer has already been overwritten with this frame's by the time the frame records.
         *       The layout, the indices and the frame-slot rule are identical to the current family's -
         *       that is what lets the shader read the same `skin_base` from this slot and lets the
         *       runtime publish into the CURRENT frame slot's buffer (see
         *       runtime::advance_motion_deformations), exactly as @ref previous_transforms does for the
         *       world matrices. It lives at the END of the used region for the same reason the TLAS and
         *       the two storage twins do: growing an array in place would renumber every array after it.
         */
        static constexpr uint32_t skin_matrices_previous = heap_slot_base + 743u;
        static constexpr uint32_t ml_history = heap_slot_base + 599u;
        static constexpr uint32_t ml_resolved = heap_slot_base + 607u;
        static constexpr uint32_t taa_current = heap_slot_base + 623u; // per image: TAA's pair
        static constexpr uint32_t taa_history = heap_slot_base + 631u;
        static constexpr uint32_t post_color = heap_slot_base + 639u; // per image: the post chain
        static constexpr uint32_t bloom_l0 = heap_slot_base + 647u;
        static constexpr uint32_t bloom_l1 = heap_slot_base + 655u;
        static constexpr uint32_t bloom_l2 = heap_slot_base + 663u;
        static constexpr uint32_t bloom_l3 = heap_slot_base + 671u;
        static constexpr uint32_t display_color = heap_slot_base + 695u;
    };
} // namespace deren::vulkan::render_layout
