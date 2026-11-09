// ============================================================================
// module: deren.engine.render_resource:schema  (what a resource IS: the
//         resource ids, kinds, scopes, lifetimes and the binding vocabulary)
//
// This partition is one of the two domains of deren.engine.render_resource;
// the top-level facade re-exports it, so consumers keep importing
// deren.engine.render_resource.
// ============================================================================

module;

#include <cstddef>
#include <cstdint>

export module deren.engine.render_resource:schema;
export import deren.vstd;

import deren.utility.shared;
import deren.engine.visibility.software;

export namespace deren::engine::render_resource {
    enum class resource_id : uint32_t {
        none = 0,
        // ---- the swapchain image itself, owned outside ----
        swapchain_image,
        // ---- the HDR chain and the G-buffer (one per swapchain image) ----
        hdr,
        bloom,
        ldr,
        gbuffer_targets, // 3: albedo (RGBA8), normal + roughness (RGBA16F), material + baked AO (RGBA8)
        gbuffer_depth,
        velocity,
        scene_color,
        taa_history,
        // the visibility buffer: one R32_UINT packed id per pixel of the main view's opaque geometry,
        // written by the visibility raster pass and read by the resolve pass (one per swapchain image)
        visibility_buffer,
        // the capture-only step's own depth image: the raster lane must not share the G-buffer's depth writer
        visibility_depth,
        // ---- one per frame slot ----
        shadow_map,
        rt_shadow_visibility,
        camera_ubo,
        light_ubo,
        material_table,
        instance_table,
        motion_vectors,
        skin_matrices,
        morph_targets,
        cluster_counts,
        cluster_indices,
        // ---- device wide ----
        furnace_cube,
        scene_textures,
        white_texture,
        ibl_env,
        ibl_irradiance,
        brdf_lut,
        // ---- what every traced pass needs ----
        top_level_structure,
        head_ubo, // shared character basis, appended to preserve existing id values
        gbuffer_albedo,
        gbuffer_normal,
        gbuffer_material,
        scene_input, // physical TAA input; scene_color remains a per-frame routing alias
        bloom_level0,
        bloom_level1,
        bloom_level2,
        bloom_level3,
        skin_matrices_previous, // appended; existing resource identities remain unchanged
        toon_colour_table,
        toon_lane_table,
        toon_rig_table,
        meshlet_table,
        mesh_indirect_commands,
        meshlet_statistics,
        meshlet_culled_table,
        // the per-frame visibility item table (v0.2b): one draw_item per visible item, read by the resolve
        visibility_items,
        gaussian_instances,
        gaussian_ranges,
        gaussian_keys,
        gaussian_values,
        gaussian_references,
        gaussian_sort_scratch,
        gaussian_status,
        gaussian_draw,
        gaussian_parameters,
        gaussian_projected,
        gaussian_geometry,
        gaussian_sh,
        rt_instance_table,
        bottom_level_structure,
        rt_mask_vertices,
        rt_skin_vertices,
        rt_opacity_micromap,
        scene_geometry_vertices,
        scene_geometry_indices,
        texture_sampler,
        gbuffer_sampler,
        taa_sampler,
        post_sampler,
        post_nearest_sampler,
        shadow_sampler,
        environment_sampler,
        post_lut,
        goo_fgd_lut,
        pass_upload_buffer,
        // ---- THE SOFTWARE RASTER'S SURFACE (M4 Task 2, docs/superpowers/specs/2026-10-22-software-raster-
        //      design.md §3.2/§4.2): two per-swapchain-image R32_UINT results and the host-written CSR tile-list
        //      buffer per frame lane. APPENDED after pass_upload_buffer so no existing identity moves. The output
        //      family publishes them through its separate `software_*` surface; Task 4 promotes that surface into
        //      the pass's selected `ids` list, which is also when these entries start being CREATED. ----
        /// the software raster's depth image: the nearest software candidate's float depth bit pattern
        software_depth,
        /// the software raster's packed-id image (the same 32-bit packing as the hardware visibility buffer)
        software_id,
        /// the CSR tile-list buffer: header + counts + offsets + entries, host-written per frame lane
        software_tiles,
        /// the ray-traced reflection target: RGB radiance and hit distance, one per frame slot
        rt_reflection,
        /// the denoised reflection radiance: the deferred stage's specular input
        rt_reflection_denoised,
        /// the previous frame's denoised radiance; read by reprojection and the temporal resolve, and replaced by
        /// the history-copy pass after the resolve has consumed it
        rt_reflection_denoised_history,
        /// reprojection output consumed by the temporal resolve
        rt_reflection_reprojected,
        /// the prefilter's output, read by the temporal resolve (kept separate so the history radiance survives)
        rt_reflection_prefiltered,
        /// the reprojection's 1/8-resolution average radiance
        rt_reflection_average,
        /// the previous frame's 1/8-resolution average radiance, read by reprojection's luminance weighting
        rt_reflection_average_history,
        /// current variance (.x) and sample count (.y) from reprojection
        rt_reflection_moments,
        /// prefiltered variance (.x) and the copied sample count (.y)
        rt_reflection_moments_prefiltered,
        /// the resolved variance (.x) and sample count (.y), read by the next frame's reprojection
        rt_reflection_moments_history,
        /// the resolved world normal (.xyz) and roughness (.w), read by the next frame's reprojection
        rt_reflection_surface_history,
        /// the resolve's current-frame surface, copied into @ref rt_reflection_surface_history after it is consumed
        rt_reflection_surface_current,
        /// the resolved linear view depth, read by the next frame's reprojection
        rt_reflection_depth_history,
        /// the resolve's current-frame depth, copied into @ref rt_reflection_depth_history after it is consumed
        rt_reflection_depth_current,
        /// the number of enumerators, so the schema's completeness can be checked by ITERATING rather than
        /// against a hand-kept list: `validate_schema()` requires exactly one entry per id in [1, count)
        local_probe,
        /// the immutable GPU cut payload owned by one subtilis geometry generation
        scene_cut_groups,
        scene_cut_clusters,
        scene_cut_references,
        /// the per-frame GPU cut item table (frame header + one record per planned item)
        subtilis_cut_items,
        /// the immutable streaming page image owned by one subtilis geometry generation
        scene_page_arena,
        scene_page_table,
        scene_page_requests,
        count,
    };

    /** @brief what a resource IS, which decides the descriptor types it can be bound as */
    enum class resource_kind : uint8_t {
        image2d,
        image3d,
        image_cube,
        buffer,
        accel_struct,
        micromap,
        sampler,
    };

    /**
     * @brief which frame counter a resource is indexed by
     *
     * THIS IS THE FIELD THIS MODULE EXISTS FOR, and it is not a classification exercise: this project's
     * documented per-image-lifetime trap is the difference between the two, and getting it wrong has already
     * produced a real bug here (one `bool` guarding a resource that is one-per-swapchain-image, so every slot
     * after the first never received its first-use transition). A pass that uses a `per_frame_slot` resource
     * is handed the frame slot; one that uses a `per_swapchain_image` resource is handed the image index.
     */
    enum class resource_scope : uint8_t {
        per_frame_slot,      // shadow maps, the light/camera/material/instance buffers, the cluster buffers
        per_swapchain_image, // every GI image, the TAA history, the G-buffer, the HDR chain
        device_wide,         // the probe grid and its geometry, the furnace cube, the IBL cubes
    };

    /** @brief how long a resource lives: what decides whether a first-use transition or a clear exists */
    enum class resource_lifetime : uint8_t {
        per_frame,  // rewritten every frame by its parent
        persistent, // holds state ACROSS frames (histories, the accumulated grid) - see the reset sites
        imported,   // owned outside this renderer (the swapchain image)
    };

    /** @brief one family of resources: what it is, how it is indexed, and how many images it holds
     *  @ingroup engine_render_resource */
    struct resource_info {
        resource_id id = resource_id::none;
        std::string_view name = {};
        resource_kind kind = resource_kind::image2d;
        resource_scope scope = resource_scope::per_swapchain_image;
        resource_lifetime lifetime = resource_lifetime::per_frame;
        /// how many images the family holds: 3 G-buffer targets, 8 probe coefficients (side*4+coefficient,
        /// exactly how `core` indexes them), 1 for an ordinary image. A binding names one ELEMENT of this.
        uint16_t count = 1;
    };

    /**
     * @brief the schema itself
     *
     * EACH ENTRY'S PROVENANCE, so the table can be re-derived rather than trusted: the HDR chain, the
     * G-buffer, the velocity and scene-colour targets, the TAA history, the stochastic-chain trio, the
     * bloom levels, the ray-traced shadow visibility (one per FRAME SLOT rather than per image) and the
     * furnace cube are all created by the ENGINE now - `deren.engine.runtime`'s
     * `create_render_chain_targets()` (③-D/E A1.1-A1.7), through the contract - releasing and rebuilding
     * them with the swapchain generation; the shadow map is the one family `deren.engine.runtime`
     * creates itself (`runtime.cpp:495-560`, layered, one image per slot); the scene buffers and the IBL
     * textures are `deren.engine.runtime`'s (`runtime.cpp:148-231`, `163-166`); the top level structure belongs to
     * `deren.engine.acceleration_structure` and reaches shaders through the frame's heap.
     *
     * WHAT AN ENTRY DOES NOT SAY YET, on purpose: its format and its extent. Those are needed by the CREATION
     * step, not by the invariants checkable today, and a format copied here before it is verified against
     * `core.cpp` would be a second copy of a fact - the thing this file's header warns about.
     * @ingroup engine_render_resource
     */
    inline constexpr deren::utility::vessel::array<resource_info, static_cast<size_t>(resource_id::count) - 1> resource_schema = {{
        {.id = resource_id::swapchain_image, .name = "swapchain_image", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::imported},
        {.id = resource_id::hdr, .name = "hdr", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        // FOUR LEVELS, not one: `core::bloom_images` is `deren::utility::vessel::array<deren::utility::vessel::vector<VkImage>, bloom_level_count>`
        // with `bloom_level_count == 4`, and the post chain's passes each own ONE of them. The count was 1 (the
        // default) until the bloom chain was extracted, which made `element = level` illegal for every pass but
        // the first - and the validator's `element >= info->count` check is what says so, so this field IS the
        // contract a declaration is written against.
        {.id = resource_id::bloom, .name = "bloom", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame, .count = 4},
        {.id = resource_id::ldr, .name = "ldr", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gbuffer_targets, .name = "gbuffer_targets", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame, .count = 3},
        {.id = resource_id::gbuffer_depth, .name = "gbuffer_depth", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::velocity, .name = "velocity", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::scene_color, .name = "scene_color", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::taa_history, .name = "taa_history", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::persistent},
        // the ONE channel is the whole payload: an unsigned id, not a colour (see deren.engine.visibility)
        {.id = resource_id::visibility_buffer, .name = "visibility_buffer", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::visibility_depth, .name = "visibility_depth", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        // FOUR LAYERS, not one: the runtime creates the shadow map as ONE layered image (`max_shadow_cascades` layers, one per cascade, plus the spare layers a shrank count leaves behind - see create_shadow_resources), and the shadow pass renders into them one at a time. The count was 1 (the default) until that pass was declared, which made `element = cascade` illegal for every cascade but the first - the same correction the bloom family needed, and the validator`s element check is what says so.
        {.id = resource_id::shadow_map, .name = "shadow_map", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame, .count = 4},
        {.id = resource_id::rt_shadow_visibility, .name = "rt_shadow_visibility", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection, .name = "rt_reflection", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        // The reflection denoiser's working set (AMD Reproject -> Prefilter -> ResolveTemporal + the history copy).
        // Every one is per-frame-slot and per-frame: the history images hold the previous recorded frame's
        // resolved radiance/surface, and the current images hold this frame's before the copy overwrites history.
        {.id = resource_id::rt_reflection_denoised, .name = "rt_reflection_denoised", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_denoised_history, .name = "rt_reflection_denoised_history", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_reprojected, .name = "rt_reflection_reprojected", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_prefiltered, .name = "rt_reflection_prefiltered", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_average, .name = "rt_reflection_average", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_average_history, .name = "rt_reflection_average_history", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_moments, .name = "rt_reflection_moments", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_moments_prefiltered, .name = "rt_reflection_moments_prefiltered", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_moments_history, .name = "rt_reflection_moments_history", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_surface_history, .name = "rt_reflection_surface_history", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_surface_current, .name = "rt_reflection_surface_current", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_depth_history, .name = "rt_reflection_depth_history", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::rt_reflection_depth_current, .name = "rt_reflection_depth_current", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::camera_ubo, .name = "camera_ubo", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::light_ubo, .name = "light_ubo", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::head_ubo, .name = "head_ubo", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gbuffer_albedo, .name = "gbuffer_albedo", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gbuffer_normal, .name = "gbuffer_normal", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gbuffer_material, .name = "gbuffer_material", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::scene_input, .name = "scene_input", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::bloom_level0, .name = "bloom_level0", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::bloom_level1, .name = "bloom_level1", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::bloom_level2, .name = "bloom_level2", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::bloom_level3, .name = "bloom_level3", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::material_table, .name = "material_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::instance_table, .name = "instance_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::motion_vectors, .name = "motion_vectors", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::skin_matrices, .name = "skin_matrices", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::skin_matrices_previous, .name = "skin_matrices_previous", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::morph_targets, .name = "morph_targets", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::cluster_counts, .name = "cluster_counts", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::cluster_indices, .name = "cluster_indices", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::furnace_cube, .name = "furnace_cube", .kind = resource_kind::image_cube, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_textures, .name = "scene_textures", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::white_texture, .name = "white_texture", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::local_probe, .name = "local_probe", .kind = resource_kind::image_cube, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::ibl_env, .name = "ibl_env", .kind = resource_kind::image_cube, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::ibl_irradiance, .name = "ibl_irradiance", .kind = resource_kind::image_cube, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::brdf_lut, .name = "brdf_lut", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::top_level_structure, .name = "top_level_structure", .kind = resource_kind::accel_struct, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::toon_colour_table, .name = "toon_colour_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::toon_lane_table, .name = "toon_lane_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::toon_rig_table, .name = "toon_rig_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::meshlet_table, .name = "meshlet_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::mesh_indirect_commands, .name = "mesh_indirect_commands", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::meshlet_statistics, .name = "meshlet_statistics", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::meshlet_culled_table, .name = "meshlet_culled_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::visibility_items, .name = "visibility_items", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_instances, .name = "gaussian_instances", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_ranges, .name = "gaussian_ranges", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_keys, .name = "gaussian_keys", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_values, .name = "gaussian_values", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_references, .name = "gaussian_references", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_sort_scratch, .name = "gaussian_sort_scratch", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_status, .name = "gaussian_status", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_draw, .name = "gaussian_draw", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_parameters, .name = "gaussian_parameters", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_projected, .name = "gaussian_projected", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gaussian_geometry, .name = "gaussian_geometry", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::gaussian_sh, .name = "gaussian_sh", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::rt_instance_table, .name = "rt_instance_table", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::bottom_level_structure, .name = "bottom_level_structure", .kind = resource_kind::accel_struct, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::rt_mask_vertices, .name = "rt_mask_vertices", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::rt_skin_vertices, .name = "rt_skin_vertices", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::rt_opacity_micromap, .name = "rt_opacity_micromap", .kind = resource_kind::micromap, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_geometry_vertices, .name = "scene_geometry_vertices", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_geometry_indices, .name = "scene_geometry_indices", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::texture_sampler, .name = "texture_sampler", .kind = resource_kind::sampler, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::gbuffer_sampler, .name = "gbuffer_sampler", .kind = resource_kind::sampler, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::taa_sampler, .name = "taa_sampler", .kind = resource_kind::sampler, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::post_sampler, .name = "post_sampler", .kind = resource_kind::sampler, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::post_nearest_sampler, .name = "post_nearest_sampler", .kind = resource_kind::sampler, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::shadow_sampler, .name = "shadow_sampler", .kind = resource_kind::sampler, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::environment_sampler, .name = "environment_sampler", .kind = resource_kind::sampler, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::post_lut, .name = "post_lut", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::goo_fgd_lut, .name = "goo_fgd_lut", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::pass_upload_buffer, .name = "pass_upload_buffer", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        // M4 Task 2's software raster surface (see the ids above): the two results are per-swapchain-image images
        // rewritten every frame; the tile list is one host-written CSR buffer per frame slot.
        {.id = resource_id::software_depth, .name = "software_depth", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::software_id, .name = "software_id", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::software_tiles, .name = "software_tiles", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        // Immutable per-geometry cut input, created by subtilis's upload pass chain (not the frame graph):
        // persistent for the generation's lifetime and addressable so a later compute cut needs no new binding.
        {.id = resource_id::scene_cut_groups, .name = "scene_cut_groups", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_cut_clusters, .name = "scene_cut_clusters", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_cut_references, .name = "scene_cut_references", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::subtilis_cut_items, .name = "subtilis_cut_items", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
        // Immutable page image, page table and request lane for one subtilis generation. M5c-1 keeps
        // every page resident; M5c-2 writes the request lane and rewrites arena/table updates.
        {.id = resource_id::scene_page_arena, .name = "scene_page_arena", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_page_table, .name = "scene_page_table", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_page_requests, .name = "scene_page_requests", .kind = resource_kind::buffer, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
    }};

    // =============================================================================================
    // THE SOFTWARE TILE-LIST BUFFER'S SIZE (M4 Task 2)
    //
    // The CSR list's byte layout lives in `deren.engine.visibility.software` (§3.4/§9 Q7), so this function is a
    // FORWARDER, not a second formula: whichever code creates the buffer - Task 4's runtime, a probe, a test - asks
    // this and gets the size the CPU builder was written against. The buffer is per FRAME LANE, so callers multiply
    // by their frame count; `software_entry_budget` is the one adjustable constant and is included in the answer.
    // =============================================================================================
    [[nodiscard]] constexpr std::uint64_t software_tiles_buffer_bytes(std::uint32_t const width, std::uint32_t const height) noexcept {
        return visibility::software::software_tiles_bytes(width, height, visibility::software::software_entry_budget);
    }

    /// @brief the schema entry for @p id, or nullptr when nothing declares it
    [[nodiscard]] constexpr resource_info const* find(resource_id const id) noexcept {
        for (resource_info const& info : resource_schema) {
            if (info.id == id) {
                return &info;
            }
        }
        return nullptr;
    }

    // =============================================================================================
    // 2. WHAT A PASS DOES WITH ONE - the usage half. The only shared token is `resource_id`.
    // =============================================================================================

    /// @brief how a binding is declared, which is what fixes its `VkDescriptorType`
    enum class binding_kind : uint8_t {
        sampled_image,
        storage_image,
        sampler,
        uniform_buffer,
        storage_buffer,
        input_attachment,
        acceleration_structure,
    };

    /**
     * @brief what the pass DOES with it, which is NOT derivable from the kind
     *
     * A filter only READS its input storage image while writing its output; another pass writes
     * `gi_trace` and reads it back in the same dispatch. A barrier stage keys on this, not on the kind.
     */
    enum class binding_access : uint8_t {
        read,
        write,
        read_write,
    };

    /// @brief which of this renderer's samplers a binding wants (five exist; the choice is a field, not a ternary)
    enum class sampler_hint : uint8_t {
        none,
        gbuffer,
        taa,
        post,
        nearest,
        shadow,
    };

    // AN `image_layout` ENUM AND A `pass_binding::layout` FIELD STOOD HERE, and they are gone with
    // VK_KHR_unified_image_layouts (see docs/unified_image_layouts.md): this renderer keeps every image in
    // VK_IMAGE_LAYOUT_GENERAL, so a declaration has no layout left to state. They had been added after the
    // first conversion because the layout was NOT derivable from the kind - the probe cache kept all nine of
    // its own bindings in GENERAL while a sampled image elsewhere was in SHADER_READ_ONLY_OPTIMAL, so
    // "storage means GENERAL, sampled means SHADER_READ" was wrong for a real pass. One layout makes the
    // question disappear, and with it the validator rule that used to enforce the answer.

    /**
     * @brief where a binding's resource comes from: the pass's own per-image binding, or the frame's heap
     *
     * `local` marks one of the pass's private per-image bindings: the resolver indexes those by `binding`, and
     * the validator requires them to be numbered contiguously from zero. `shared` marks a binding the pass
     * reaches through the frame's descriptor heap - the scene's camera and material table, the G-buffer
     * surface, the post chain's images - whose descriptors some other stage publishes; a pass declares only
     * that it USES such a binding, and cannot change it.
     */
    enum class binding_scope : uint8_t {
        local,
        shared,
    };

    /// @brief shader stages as bits, so a binding can list more than one (the shared resources serve two)
    enum class stage_flag : uint8_t {
        none = 0u,
        vertex = 1u << 0u,
        fragment = 1u << 1u,
        compute = 1u << 2u,
    };

    [[nodiscard]] constexpr stage_flag operator|(stage_flag const a, stage_flag const b) noexcept {
        return static_cast<stage_flag>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }

    [[nodiscard]] constexpr bool has_stage(stage_flag const set, stage_flag const one) noexcept {
        return (static_cast<uint8_t>(set) & static_cast<uint8_t>(one)) != 0u;
    }

    /// @brief a name for each enum, so a validator message says "storage_image is not a buffer" not "1 is not 3"
    [[nodiscard]] constexpr std::string_view name_of(binding_kind const kind) noexcept {
        switch (kind) {
        case binding_kind::sampled_image:
            return "sampled_image";
        case binding_kind::storage_image:
            return "storage_image";
        case binding_kind::sampler:
            return "sampler";
        case binding_kind::uniform_buffer:
            return "uniform_buffer";
        case binding_kind::storage_buffer:
            return "storage_buffer";
        case binding_kind::input_attachment:
            return "input_attachment";
        case binding_kind::acceleration_structure:
            return "acceleration_structure";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view name_of(resource_kind const kind) noexcept {
        switch (kind) {
        case resource_kind::image2d:
            return "image2d";
        case resource_kind::image3d:
            return "image3d";
        case resource_kind::image_cube:
            return "image_cube";
        case resource_kind::buffer:
            return "buffer";
        case resource_kind::accel_struct:
            return "accel_struct";
        case resource_kind::micromap:
            return "micromap";
        case resource_kind::sampler:
            return "sampler";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view name_of(binding_access const access) noexcept {
        switch (access) {
        case binding_access::read:
            return "read";
        case binding_access::write:
            return "write";
        case binding_access::read_write:
            return "read_write";
        }
        return "?";
    }

    /// @brief one binding: one use. Everything the descriptor and the heap write need is here.
    /// @ingroup engine_render_resource
    struct pass_binding {
        uint32_t binding = 0;
        binding_scope parent = binding_scope::local;
        binding_kind kind = binding_kind::sampled_image;
        resource_id resource = resource_id::none;
        /// which image of the resource FAMILY this binding names (0 for a single-image resource)
        uint16_t element = 0;
        /// how many descriptors this binding declares: 1, or the capacity of a bindless array. The validator
        /// refuses zero, so a binding that claims no descriptor cannot pass silently.
        uint16_t descriptor_count = 1;
        binding_access access = binding_access::read;
        sampler_hint sampler = sampler_hint::none;
        stage_flag stages = stage_flag::compute;
    };

    /// @brief a push-constant range: declared and pinned by `static_assert`, never computed from a declaration
    struct push_block {
        uint32_t offset = 0;
        uint32_t size = 0;
        stage_flag stages = stage_flag::compute;
    };

    /// @brief which slot of a rendering instance a declared target fills
    enum class target_kind : uint8_t {
        color, // a colour attachment (one of the G-buffer's targets, the HDR image a resolve writes)
        depth, // THE depth attachment, of which a rendering instance has exactly one
    };

    /**
     * @brief an image a pass RENDERS INTO, which is a use that cannot be a descriptor
     *
     * A fullscreen pass's colour attachment is not in any set: it is bound by `vkCmdBeginRendering`, so the
     * declaration has to name it somewhere else - and it has to name it SOMEWHERE, because otherwise a pass
     * could render into an image it never declared, which is the one property this layer enforces. Keeping it
     * in `bindings` would be worse than a second list: a binding is a descriptor, the layout generator walks
     * that list, and a colour attachment has no `VkDescriptorType` at all.
     *
     * The LOAD OP and the clear value are deliberately NOT declared: the pass that renders into the image is
     * the one that opens the rendering instance (see `behaviour_kind::fullscreen` and `graphics`), so it is the
     * one that says whether the old contents matter. What is declared is only what the layer has to know: which
     * resource, which image of its family, and whether it fills the colour or the DEPTH slot.
     */
    struct render_target {
        resource_id resource = resource_id::none;
        uint16_t element = 0; // which image of the resource's family (per-swapchain-image resources: the index)
        target_kind kind = target_kind::color;
        /**
         * HOW MANY CONSECUTIVE ELEMENTS of the family this ONE entry claims, rendered ONE INSTANCE PER ELEMENT.
         *
         * WHY IT IS NOT "several attachments of one instance", which is the obvious reading: a rendering instance
         * has exactly ONE depth attachment, so the N layers of one depth array cannot be one instance's targets.
         * The shadow pass renders its cascades layer by layer, each with its own transition, instance and
         * secondary - and what its declaration could not say before was that all N of those layers are elements
         * of ONE family, so the host had to hand them over itself (`runtime::resolve_shadow_pass`, now gone).
         *
         * A CEILING RATHER THAN A PROMISE: how many elements a family HAS this frame is the FRAME's fact (the
         * shadow map allocates the layers the cascade knob asks for - one to four - and `create_shadow_resources`
         * may keep spare layers a shrank count left behind), so resolution hands over the first @c count elements
         * that EXIST and the pass renders what it is given. `element + count` past the family's own count is a
         * declaration error the validator refuses, so this field is a claim about the SCHEMA, and the frame's
         * answer is only ever a shorter run.
         */
        uint16_t count = 1;
    };

    /**
     * @brief an image a pass must TRANSITION but does not bind as a descriptor
     *
     * because a layout transition names an IMAGE and no descriptor at all - the same argument that made
     * `render_target` a separate list from `bindings`.
     *
     * The LAYOUTS are deliberately not declared: which pair of layouts a transition uses is the pass's
     * knowledge (its own barriers, its own order), and a declaration that named them would have to be a
     * barrier generator rather than a description of what the pass touches. What is declared is only what the
     * layer must know to hand the pass a handle: which resource, and which image of its family.
     */
    struct barrier_image {
        resource_id resource = resource_id::none;
        /// which image of the resource's family: the FRAME's image index for a per-swapchain-image resource
        /// (resolved by the host from the frame, exactly as `render_target` does), or the element for a family
        /// a pass reaches by position (the probe grid's eight)
        uint16_t element = 0;
    };

    /**
     * @brief a BUFFER a pass orders around but never binds itself - the buffer twin of `barrier_image`
     *
     * WHY THIS EXISTS, and it is a measured need rather than symmetry for its own sake: the clustered-light
     * sort (`deren.engine.pass.cluster`) writes two buffers that are part of the frame's shared scene resources
     * (bindings 11 and 12). The pass reaches them through the heap, so its declaration names no binding for
     * either buffer - and yet
     * its writes are not visible to the fragment stages that read them later in the same submission without a
     * buffer memory barrier, which only the writer can place. Before this field the renderer kept the whole
     * recording for exactly that reason: a pass could declare an IMAGE it moves and had no way to name a BUFFER
     * it moves. The pair of channel fields is now the same shape as `barrier_images`, one per resource class.
     *
     * The ACCESS masks are deliberately not declared, for the reason `barrier_image` gives about layouts: which
     * stages and accesses a barrier names is the pass's knowledge of its own ordering, and a declaration that
     * named them would be a barrier generator rather than a description of what the pass touches.
     */
    struct barrier_buffer {
        resource_id resource = resource_id::none;
        /// which buffer of the resource's family - the frame's slot for a per-frame-slot resource (resolved by
        /// the host from the frame, exactly as `barrier_image` does for images)
        uint16_t element = 0;
    };

    /**
     * @brief one pass's declared I/O
     *
     * `bindings` carries both kinds: the `binding_scope::local` entries are the pass's private per-image
     * bindings, which the validator requires to be numbered contiguously from zero, and the
     * `binding_scope::shared` entries are declared USAGE of resources the frame's heap provides.
     * @ingroup engine_render_resource
     */
    struct pass_io {
        std::string_view name = {};
        std::span<pass_binding const> bindings = {};
        /// the images this pass renders into, in the order it uses them (a fullscreen pass has one)
        std::span<render_target const> targets = {};
        /**
         * The images this pass TRANSITIONS but never binds (see `barrier_image`), in the order the pass's own
         * record() indexes them. Empty for every pass whose resources are all its own or all descriptors.
         */
        std::span<barrier_image const> barrier_images = {};
        /**
         * The BUFFERS this pass orders around but never binds (see `barrier_buffer`), in the order the pass's
         * own record() indexes them. Empty for every pass but the clustered-light sort today.
         */
        std::span<barrier_buffer const> barrier_buffers = {};
        std::optional<push_block> push = std::nullopt;
    };

    // =============================================================================================
    // 3. THE INVARIANTS - the reason this is a module and not a comment
    // =============================================================================================

    /// @brief kind compatibility: which resource kinds a binding kind may name
    [[nodiscard]] constexpr bool compatible(binding_kind const kind, resource_kind const resource) noexcept {
        switch (kind) {
        case binding_kind::sampled_image:
        case binding_kind::storage_image:
        case binding_kind::input_attachment:
            return resource == resource_kind::image2d || resource == resource_kind::image3d || resource == resource_kind::image_cube;
        case binding_kind::sampler:
            return true; // a standalone sampler names no resource of its own
        case binding_kind::uniform_buffer:
        case binding_kind::storage_buffer:
            return resource == resource_kind::buffer;
        case binding_kind::acceleration_structure:
            return resource == resource_kind::accel_struct;
        }
        return false;
    }

    /// @brief access compatibility: what each binding kind can do
    [[nodiscard]] constexpr bool compatible(binding_kind const kind, binding_access const access) noexcept {
        switch (kind) {
        case binding_kind::sampled_image:
        case binding_kind::sampler:
        case binding_kind::uniform_buffer:
        case binding_kind::input_attachment:
        case binding_kind::acceleration_structure:
            return access == binding_access::read;
        case binding_kind::storage_image:
        case binding_kind::storage_buffer:
            return true; // writable, or readable: the spatial filter's input is a read-only storage image
        }
        return false;
    }

    /** @brief the schema's own invariants: no empty name, no empty family, and ONE entry per declared id */
    [[nodiscard]] inline std::expected<void, deren::utility::vessel::string> validate_schema() {
        for (resource_info const& info : resource_schema) {
            if (info.id == resource_id::none) {
                return std::unexpected("a schema entry has no id");
            }
            if (static_cast<uint32_t>(info.id) >= static_cast<uint32_t>(resource_id::count)) {
                return std::unexpected("a schema entry uses an id outside the enumeration");
            }
            if (info.name.empty()) {
                return std::unexpected("a schema entry has no name");
            }
            if (info.count == 0) {
                return std::unexpected(deren::utility::vessel::string(info.name) + " declares zero images");
            }
            if (info.lifetime == resource_lifetime::imported && info.scope != resource_scope::per_swapchain_image) {
                return std::unexpected(deren::utility::vessel::string(info.name) + " is imported but not per-swapchain-image");
            }
        }
        // Completeness: every enumerator the renderer can name has exactly one entry. A forgotten entry is
        // the failure this catches, and it catches it without a second hand-kept list of ids.
        for (uint32_t raw = 1; raw < static_cast<uint32_t>(resource_id::count); ++raw) {
            std::size_t seen = 0;
            for (resource_info const& info : resource_schema) {
                if (static_cast<uint32_t>(info.id) == raw) {
                    ++seen;
                }
            }
            if (seen != 1) {
                return std::unexpected("the schema has " + deren::utility::vessel::to_string(seen) + " entries for resource id " + deren::utility::vessel::to_string(raw));
            }
        }
        return {};
    }

    /**
     * @brief a pass's declaration against the schema
     *
     * THE INVARIANTS, each one either a bug this project has met or a class of bug it is built to avoid: a
     * named pass; every binding names a resource the schema DECLARES (usage is a subset of the schema); the
     * binding kind fits the resource kind (a `sampler3D` cannot be declared as a 2D image; an acceleration
     * structure is not a buffer); the access fits the kind (a sampled image cannot be written); `element` is
     * inside the family; `descriptor_count` is at least one; a sampled image names a sampler and nothing else
     * does; no two bindings share a binding number, which would silently lose one of them; and the pass's OWN
     * bindings are numbered contiguously from zero, because those are the ones the resolver indexes by binding
     * number. A render TARGET gets the same two checks a binding gets - the schema declares the resource, and
     * the element is inside its family - plus uniqueness, because two targets naming one image would be a pass
     * rendering into itself twice. A target that claims a RUN of elements (`render_target::count`) is checked
     * as the run it is: the last element has to be inside the family, and two runs of one resource may not
     * overlap.
     * @ingroup engine_render_resource
     */
    [[nodiscard]] inline std::expected<void, deren::utility::vessel::string> validate(pass_io const& io) {
        if (io.name.empty()) {
            return std::unexpected("a pass declaration has no name");
        }
        deren::utility::vessel::string const who{io.name};
        std::size_t local_count = 0;
        std::size_t depth_targets = 0;
        for (render_target const& t : io.targets) {
            deren::utility::vessel::string const where = who + ": target " + deren::utility::vessel::to_string(t.element);
            resource_info const* const info = find(t.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (info->kind != resource_kind::image2d && info->kind != resource_kind::image3d && info->kind != resource_kind::image_cube) {
                return std::unexpected(where + " names " + deren::utility::vessel::string(info->name) + ", which is not an image a pass can render into");
            }
            if (t.element >= info->count) {
                return std::unexpected(where + " names element " + deren::utility::vessel::to_string(t.element) + " of " + deren::utility::vessel::string(info->name) + ", which holds " +
                                       deren::utility::vessel::to_string(info->count));
            }
            if (t.count == 0u) {
                return std::unexpected(where + " claims no element of " + deren::utility::vessel::string(info->name) + ", so it renders nothing");
            }
            if (static_cast<uint32_t>(t.element) + t.count > info->count) {
                // A RUN that reaches past the family: `count` is a claim about the SCHEMA, and the frame can only
                // ever answer with a SHORTER run (see render_target::count).
                return std::unexpected(where + " claims elements " + deren::utility::vessel::to_string(t.element) + ".." + deren::utility::vessel::to_string(static_cast<uint32_t>(t.element) + t.count - 1u) + " of " +
                                       deren::utility::vessel::string(info->name) + ", which holds " + deren::utility::vessel::to_string(info->count));
            }
            if (t.kind == target_kind::depth && ++depth_targets > 1u) {
                // a rendering instance has exactly one depth attachment, so a second one cannot be recorded - and a
                // RUN of depth elements is still ONE entry here, because each element gets its own instance
                return std::unexpected(who + ": more than one DEPTH target is declared, and an instance has one");
            }
            for (render_target const& other : io.targets) {
                // Two entries whose element RUNS INTERSECT would have the pass render into one image twice - the
                // same defect the exact-match check below reports, one resource class over.
                if (&other != &t && other.resource == t.resource && t.element < static_cast<uint32_t>(other.element) + other.count && other.element < static_cast<uint32_t>(t.element) + t.count) {
                    return std::unexpected(where + " overlaps the entries " + deren::utility::vessel::to_string(other.element) + ".." + deren::utility::vessel::to_string(static_cast<uint32_t>(other.element) + other.count - 1u) + " of the same resource");
                }
            }
        }
        // THE BARRIER IMAGES, checked like a target: an image the pass will move between layouts has to exist
        // in the schema and has to BE an image (a transition names no descriptor, but it does name a resource).
        for (barrier_image const& t : io.barrier_images) {
            deren::utility::vessel::string const where = who + ": barrier image " + deren::utility::vessel::to_string(t.element);
            resource_info const* const info = find(t.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (info->kind != resource_kind::image2d && info->kind != resource_kind::image3d && info->kind != resource_kind::image_cube) {
                return std::unexpected(where + " names " + deren::utility::vessel::string(info->name) + ", which is not an image");
            }
            if (t.element >= info->count) {
                return std::unexpected(where + " names element " + deren::utility::vessel::to_string(t.element) + " of " + deren::utility::vessel::string(info->name) + ", which holds " +
                                       deren::utility::vessel::to_string(info->count));
            }
            for (barrier_image const& other : io.barrier_images) {
                if (&other != &t && other.resource == t.resource && other.element == t.element) {
                    return std::unexpected(where + " is declared twice, and a pass indexes these by position");
                }
            }
        }
        // ... and THE BARRIER BUFFERS, the same three checks one resource class over: it has to exist in the
        // schema, it has to BE a buffer, and a pass indexes these by position so a duplicate is a defect.
        for (barrier_buffer const& t : io.barrier_buffers) {
            deren::utility::vessel::string const where = who + ": barrier buffer " + deren::utility::vessel::to_string(t.element);
            resource_info const* const info = find(t.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (info->kind != resource_kind::buffer) {
                return std::unexpected(where + " names " + deren::utility::vessel::string(info->name) + ", which is not a buffer");
            }
            if (t.element >= info->count) {
                return std::unexpected(where + " names element " + deren::utility::vessel::to_string(t.element) + " of " + deren::utility::vessel::string(info->name) + ", which holds " +
                                       deren::utility::vessel::to_string(info->count));
            }
            for (barrier_buffer const& other : io.barrier_buffers) {
                if (&other != &t && other.resource == t.resource && other.element == t.element) {
                    return std::unexpected(where + " is declared twice, and a pass indexes these by position");
                }
            }
        }
        for (pass_binding const& b : io.bindings) {
            deren::utility::vessel::string const where = who + ": binding " + deren::utility::vessel::to_string(b.binding);
            resource_info const* const info = find(b.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (!compatible(b.kind, info->kind)) {
                return std::unexpected(where + " is declared " + deren::utility::vessel::string(name_of(b.kind)) + " but " + deren::utility::vessel::string(info->name) + " is " +
                                       deren::utility::vessel::string(name_of(info->kind)));
            }
            if (!compatible(b.kind, b.access)) {
                return std::unexpected(where + " is a " + deren::utility::vessel::string(name_of(b.kind)) + " used for " + deren::utility::vessel::string(name_of(b.access)));
            }
            if (b.element >= info->count) {
                return std::unexpected(where + " names element " + deren::utility::vessel::to_string(b.element) + " of " + deren::utility::vessel::string(info->name) +
                                       ", which holds " + deren::utility::vessel::to_string(info->count));
            }
            if (b.descriptor_count == 0) {
                return std::unexpected(where + " declares no descriptors");
            }
            if ((b.kind == binding_kind::sampled_image) != (b.sampler != sampler_hint::none)) {
                return std::unexpected(where + " must name a sampler exactly when it is a sampled image");
            }
            for (pass_binding const& other : io.bindings) {
                if (&other != &b && other.binding == b.binding) {
                    return std::unexpected(where + " is declared twice");
                }
            }
            if (b.parent == binding_scope::local) {
                if (b.binding != local_count) {
                    return std::unexpected(who + ": the pass's local bindings are not numbered contiguously from zero (expected " +
                                           deren::utility::vessel::to_string(local_count) + ", found " + deren::utility::vessel::to_string(b.binding) + ")");
                }
                ++local_count;
            }
        }
        if (io.push.has_value()) {
            if (io.push->size == 0 || (io.push->size % 4u) != 0u || io.push->offset + io.push->size > 128u) {
                return std::unexpected(who + ": the push block does not fit the 128-byte guaranteed minimum in 4-byte units");
            }
        }
        return {};
    }

} // namespace deren::engine::render_resource
