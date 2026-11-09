// ============================================================================
// module: deren.engine.render_resource:passes  (what each pass DOES with the
//         schema: bindings, render targets, barriers and push blocks, one
//         declaration per pass)
//
// This partition is one of the two domains of deren.engine.render_resource;
// the top-level facade re-exports it, so consumers keep importing
// deren.engine.render_resource.
// ============================================================================

module;

#include <cstdint>

export module deren.engine.render_resource:passes;
export import deren.vstd;

import deren.utility.shared;

import :schema;

export namespace deren::engine::render_resource {
    // =============================================================================================
    // 4. THE FIRST REAL DECLARATION - the TAA resolve (see section 5 below)
    // =============================================================================================

    // =============================================================================================
    // 5. THE SECOND DECLARATION - the TAA resolve, read off source/shaders/post/taa.slang
    // =============================================================================================

    /**
     * @brief the temporal resolve's I/O, as its shader declares it
     *
     * This is the first declaration whose bindings are all the pass's and all per swapchain image, with
     * nothing shared beside them: the four inputs (this frame's colour, the reprojected history, the motion
     * vectors and the depth the disocclusion guard reads) are all this pass's. `stages` is FRAGMENT and has
     * to be said: the resolved stage flags come from the declaration, and a resolve whose bindings
     * were declared COMPUTE would be a binding the fragment stage cannot see - which is the drift this
     * declaration exists to make impossible, now in the other direction (the field is easy to forget when every
     * other declaration so far was a compute pass).
     */
    inline constexpr deren::utility::vessel::array<pass_binding, 4> taa_bindings = {{
        {.binding = 0, .parent = binding_scope::local, .kind = binding_kind::sampled_image, .resource = resource_id::scene_color, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
        {.binding = 1, .parent = binding_scope::local, .kind = binding_kind::sampled_image, .resource = resource_id::taa_history, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
        {.binding = 2, .parent = binding_scope::local, .kind = binding_kind::sampled_image, .resource = resource_id::velocity, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
        {.binding = 3, .parent = binding_scope::local, .kind = binding_kind::sampled_image, .resource = resource_id::gbuffer_depth, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
    }};

    /// @brief the resolve RENDERS INTO the frame's HDR target, which is why it needs a target and not a binding
    inline constexpr render_target taa_target = {.resource = resource_id::hdr, .element = 0};
    inline constexpr deren::utility::vessel::array<render_target, 1> taa_targets = {taa_target};

    /// @brief the temporal resolve's declaration
    /// @ingroup engine_render_resource
    inline constexpr pass_io taa_io = {
        .name = "taa",
        .bindings = taa_bindings,
        .targets = taa_targets,
        // eight floats: the history flag, the two blend weights, the texel size, and the projection's two
        // depth terms (see vulkan.pass.taa::taa_pass::push_constants, which static_asserts this number)
        .push = push_block{.offset = 0, .size = 32, .stages = stage_flag::fragment},
    };

    // =============================================================================================
    // 6. THE THIRD DECLARATION - the scene pass, which holds no binding at all
    // =============================================================================================

    /**
     * @brief the scene pass's I/O: five colour attachments, one depth attachment, and no separate binding
     *
     * THIS IS THE FIRST DECLARATION WITH NO dedicated BINDINGS AT ALL, and that is what the pass IS: it draws the
     * scene's primitives into the frame's surface targets. The material table, the texture array, the camera
     * and light UBOs, the shadow map, the instance table and the top level structure all reach its shaders
     * through the frame's heap, whose contents its holders publish - so this declaration names no binding for
     * any of them. Those are `binding_scope::shared` resources the pass only USES, not holds.
     *
     * THE TARGETS ARE THE FRAME'S SURFACE, in the order the rendering instance needs them: the three stored
     * G-buffer targets, the motion-vector target, the scene colour target the lighting stage adds on top of,
     * and the G-buffer's depth. The per-leaf push constants are the LEAVES' (a model matrix, a material
     * index), pushed through the draw path the primitive holds, so this declaration has no push block.
     *
     * `scene_color` IS AN ALIAS, and deliberately: the resolver maps it to the image the frame's scene pass
     * stores the nearest surface's emissive in, which selects the HDR target normally, and the
     * TAA resolve's input while that resolve runs (it takes the HDR target for its output). That is a
     * FRAME decision, not a resource fact, so the declaration names what it means and the renderer decides
     * which image that is. It also means the scene pass and the TAA resolve agree by construction about which
     * image the scene writes and the resolve reads.
     */
    inline constexpr deren::utility::vessel::array<render_target, 6> scene_targets = {{
        {.resource = resource_id::gbuffer_targets, .element = 0},
        {.resource = resource_id::gbuffer_targets, .element = 1},
        {.resource = resource_id::gbuffer_targets, .element = 2},
        {.resource = resource_id::velocity, .element = 0},
        {.resource = resource_id::scene_color, .element = 0},
        {.resource = resource_id::gbuffer_depth, .element = 0, .kind = target_kind::depth},
    }};

    // cl 19.44 (MSVC 14.44) ICEs on this shape - and ONLY this shape: a module-interface-scope constexpr
    // variable whose value holds a DISENGAGED std::optional (fatal error C1001 in msc1.cpp:1589, exit
    // 0xC0000005). The four pass declarations whose push block is ABSENT (scene, transparent,
    // character_forward, cluster) are exactly that, each one independently - fixing any three still crashes.
    // constinit keeps the constant initialisation (the object is still statically initialised and immutable)
    // and gives up only constant-expression usability, which nothing asks of these four. Measured on this file:
    // clean 3/3 with constinit, ICE 4/4 without
    // (build-release-clang64/msvc/lead_lab/ice_rr/ICE_RR_FINDINGS.md).
    /// @brief the scene pass's declaration
    /// @ingroup engine_render_resource
    inline constinit pass_io const scene_io = {
        .name = "scene",
        .bindings = {},
        .targets = scene_targets,
        .push = std::nullopt,
    };

    /**
     * @brief the transparent pass's declaration: the same surface, entered with LOAD
     *
     * TWO TARGETS, not six: the alpha-blended leaves composite over the shaded frame, so the pass draws into
     * the scene colour target and depth-tests against the surface depth - and it LOADs both, because what it
     * composites over must survive. That decision is the pass's (it opens the instance), which is why the
     * declaration names the images and not the load ops (see render_target).
     *
     * It reaches the same shared resources the scene pass does - a blended surface reads the same materials, the
     * same camera and the separate same shadow map - so it declares no binding either.
     */
    inline constexpr deren::utility::vessel::array<render_target, 2> transparent_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
        {.resource = resource_id::gbuffer_depth, .element = 0, .kind = target_kind::depth},
    }};

    /// @brief the transparent pass's declaration
    /// @ingroup engine_render_resource
    // constinit for the same cl 19.44 ICE as scene_io above (a disengaged std::optional in a constexpr global)
    inline constinit pass_io const transparent_io = {
        .name = "transparent",
        .bindings = {},
        .targets = transparent_targets,
        .push = std::nullopt,
    };

    /**
     * @brief the character-forward pass's declaration: the same TWO targets as the transparent pass, and
     *        for the same structural reason
     *
     * It re-shades surfaces the deferred stage has already lit, so it needs the scene colour to overwrite
     * and the surface depth to test against - and it LOADs both, because everything it does not draw must
     * survive untouched. That is the separate whole reason it is a pass rather than a step inside the
     * lighting instance: an image cannot be a sampled texture and a depth attachment at once.
     *
     * The difference from `transparent_io` is entirely in the pipeline and the raster state, none of which
     * a declaration names (see render_target): depth compare EQUAL rather than LESS_OR_EQUAL, depth write
     * OFF and held off, and blending OFF because this pass overwrites rather than composites.
     *
     * It declares no separate binding: it reads the same materials, camera and light the scene pass does,
     * and all of those reach its shaders through the frame's heap.
     */
    inline constexpr deren::utility::vessel::array<render_target, 2> character_forward_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
        {.resource = resource_id::gbuffer_depth, .element = 0, .kind = target_kind::depth},
    }};

    /// @brief the character-forward pass's declaration
    /// @ingroup engine_render_resource
    // constinit for the same cl 19.44 ICE as scene_io above (a disengaged std::optional in a constexpr global)
    inline constinit pass_io const character_forward_io = {
        .name = "character_forward",
        .bindings = {},
        .targets = character_forward_targets,
        .push = std::nullopt,
    };

    /**
     * @brief the screen-space depth rim's declaration: ONE target and NO bindings, because it samples the
     *        frame's G-buffer through the heap
     *
     * THE TARGET IS THE SCENE COLOUR AND THE PIPELINE BLENDS ADDITIVELY: the rim is a contribution to the
     * frame the character-forward stage produced, so the pass never has to read the image it writes - which is
     * what keeps it from needing a copy of it.
     *
     * NO DEPTH ATTACHMENT IS DECLARED, and that is the whole reason this pass exists separately: it samples the
     * depth, and a pass cannot both attach an image and sample it. See the declaration's neighbours for the
     * opposite arrangement (the character-forward and transparent passes both attach the depth).
     *
     * NO BARRIER IMAGES EITHER: the G-buffer targets and the depth are published to a sampled layout by the
     * RENDERER's stage preamble, because that publication is per-image bookkeeping about images the G-buffer
     * pass wrote - the same rule the ray-traced shadow and the stochastic lighting stages follow. The push
     * block is 48 bytes: the two projection terms from the frame, the rim's width/gain/strength and its colour
     * from the pass, and the framework appends the two heap lanes on top.
     */
    inline constexpr deren::utility::vessel::array<render_target, 1> toon_screen_rim_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
    }};

    /// @brief the screen-space depth rim's declaration
    /// @ingroup engine_render_resource
    inline constexpr pass_io toon_screen_rim_io = {
        .name = "toon_screen_rim",
        .bindings = {},
        .targets = toon_screen_rim_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 48, .stages = stage_flag::fragment},
    };

    /**
     * @brief the REWRITTEN chain's rim: the same shape as the article's contour above, one stage over
     *
     * ONE TARGET AND NO BINDINGS, for the reasons that declaration gives - the rim is ADDED to the frame the
     * character stage produced (additive blend, LOAD, no depth attachment, no separate barrier; the G-buffer it
     * samples is published by the renderer's stage preamble). IT IS A SECOND DECLARATION RATHER THAN A SECOND
     * TARGET ON THAT ONE because the two rims are mutually exclusive by construction: `toon_screen_rim` is the
     * ARTICLE's contour and the holder's feature table switches it OFF whenever `runtime::goo_toon_active()` -
     * this pass's gate - is true.
     *
     * THE PUSH BLOCK IS 72 BYTES - the frame's inverse view-projection (64) and the two depth-linearization terms
     * (8) - and it is the smallest of any stage with a matrix in it because every other number this rim uses is
     * either the reference's constant (in the shader) or the material's (a colour lane the shader reads through
     * the G-buffer's material id). The framework appends the two heap lanes on top, so the shader's block is 80.
     */
    inline constexpr deren::utility::vessel::array<render_target, 1> goo_rim_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
    }};

    /// @brief the rewritten chain's rim declaration
    /// @ingroup engine_render_resource
    inline constexpr pass_io goo_rim_io = {
        .name = "goo_rim",
        .bindings = {},
        .targets = goo_rim_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 72, .stages = stage_flag::fragment},
    };

    // =============================================================================================
    // 7. THE SHARED RESOURCES - what a full-screen compute pass reaches without holding a binding
    // =============================================================================================

    /**
     * @brief the image the ray-traced shadow pass rewrites, and the only resource it has to name
     *
     * `rt_shadow` is per FRAME SLOT rather than per swapchain image (the rays are traced once per frame, not once
     * per presented image), so the host resolves this one from the frame's slot - the declaration says WHICH
     * resource, and the element is 0 because there is one family entry to choose from there. It is a storage
     * image the pass writes (UNDEFINED -> GENERAL) and the lighting stage samples (GENERAL -> SHADER_READ), both
     * transitions recorded by the pass itself.
     */
    inline constexpr deren::utility::vessel::array<barrier_image, 1> rt_shadow_barriers = {{
        {.resource = resource_id::rt_shadow_visibility, .element = 0},
    }};

    /**
     * @brief the ray-traced sun shadow pass's declaration: a full-resolution compute dispatch, sharing everything
     *
     * The traced-compute shape, with the frame's resolution instead of half: its shaders reach the frame's
     * shared resources (the camera, the light UBO, the top level structure at binding 16, the G-buffer surface
     * each ray starts from) through the heap, it holds no descriptor at all, and it names the one image it
     * rewrites through `barrier_images`. Its push block is the camera's inverse view-projection and four
     * ray-offset terms.
     */
    inline constexpr pass_io rt_shadow_io = {
        .name = "rt_shadow",
        .bindings = {},
        .targets = {},
        .barrier_images = rt_shadow_barriers,
        .push = push_block{.offset = 0, .size = 80, .stages = stage_flag::compute},
    };

    /// @brief the one image the ray-traced reflection pass writes, sampled by deferred lighting
    inline constexpr deren::utility::vessel::array<barrier_image, 1> rt_reflection_barriers = {{
        {.resource = resource_id::rt_reflection, .element = 0},
    }};

    /// @brief the ray-traced reflection pass's declaration: full-resolution trace, one storage image output
    inline constexpr pass_io rt_reflection_io = {
        .name = "rt_reflection",
        .bindings = {},
        .targets = {},
        .barrier_images = rt_reflection_barriers,
        .push = push_block{.offset = 0, .size = 80, .stages = stage_flag::compute},
    };

    /**
     * @brief the two buffers the clustered-light sort writes, and the ONLY resources it has to name
     *
     * They are bindings 11 and 12 of the frame's shared scene resources - so this pass reaches them through the
     * heap and declares no separate binding - but a pass's writes are not visible to the fragment stages
     * reading them later in the same submission without a BUFFER memory barrier, and only the writer can place
     * it. That is what `pass_io::barrier_buffers` exists for (see `barrier_buffer`), and this declaration is why
     * the field was added: before it, a pass could name an image it moves and had no way to name a buffer it
     * moves.
     *
     * Both are per FRAME SLOT (the slot's count and index arrays), which the host resolves from the frame.
     */
    inline constexpr deren::utility::vessel::array<barrier_buffer, 2> cluster_barriers = {{
        {.resource = resource_id::cluster_counts, .element = 0},
        {.resource = resource_id::cluster_indices, .element = 0},
    }};

    /**
     * @brief the clustered-light sort's declaration: a one-dimensional compute dispatch over the cluster grid
     *
     * its push block carries the heap frame/image indices, with no pass-specific payload. It reads the
     * light UBO and writes the cluster buffers through shared resources, with no dedicated binding, target or image
     * transition - the resource declaration is
     * "the two buffers I write". Its dispatch size is neither the frame's nor half of
     * it: it is `tiles_x * tiles_y * slices`, which is why its behaviour declares `extent_rule::none` and the
     * host hands the count over in the pass's frame instead.
     */
    // constinit for the same cl 19.44 ICE as scene_io above (a disengaged std::optional in a constexpr global)
    inline constinit pass_io const cluster_io = {
        .name = "cluster",
        .bindings = {},
        .targets = {},
        .barrier_images = {},
        .barrier_buffers = cluster_barriers,
        .push = push_block{.offset = 0, .size = 8, .stages = stage_flag::compute},
    };

    /// @brief the resource the deferred lighting stage RENDERS INTO, by declaration
    inline constexpr deren::utility::vessel::array<render_target, 1> deferred_targets = {{
        {.resource = resource_id::scene_color, .element = 0, .kind = target_kind::color},
    }};

    /**
     * @brief the deferred lighting stage's declaration: a fullscreen triangle that shades every pixel
     *
     * Every binding it uses is one of the frame's SHARED resources (the scene's camera, IBL, light
     * UBO and shadow map; the G-buffer's three surface targets, depth and velocity), so it declares
     * no separate binding - the shape a traced compute pass has, with one render target
     * instead of a compute dispatch.
     *
     * THE TARGET IS A RECORDED DEVIATION: it names `scene_color`, and the host hands over whichever image the frame
     * actually lights - `scene_color` while the TAA resolve runs and `hdr` when it does not (the accessor the
     * renderer always used). A `render_target` names one resource today, and a "frame-dependent target" is a
     * framework decision this pass does not get to make alone; the deviation is written down here rather than hidden.
     *
     * The push block is 92 bytes - a mat4, a vec4 (the SSAO knobs) and three floats (the render mode, whether the
     * stochastic punctual lighting pass answered this frame's punctual lights) -
     * and the attachment is LOADed, because the lighting ADDS to the emissive the G-buffer pass already wrote (which
     * is also why the pass builds its pipeline with the additive blend).
     */
    inline constexpr pass_io deferred_io = {
        .name = "deferred",
        .bindings = {},
        .targets = deferred_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 92, .stages = stage_flag::fragment},
    };

    // =============================================================================================
    // THE POST CHAIN - the composite and the bloom chain's four levels
    // =============================================================================================

    /// @brief the push block the whole post chain shares, in bytes; its shape is `deren.engine.pass.post`'s
    /// @note 36 bytes: the existing LUT row plus the explicit tone-mapping selector. The fourth
    ///       float of the FXAA lanes' row. The shader's `PostPush` declares the same field in the same place - the note
    ///       there is explicit that the layout up to the three heap indices has to match the CPU struct, because a
    ///       block that stopped short reads its lanes out of the wrong members.
    inline constexpr uint32_t post_push_bytes = 36;

    /**
     * @brief the bloom chain's four levels, as ONE declaration per level
     *
     * WHY FOUR DECLARATIONS AND NOT ONE PARAMETERIZED PASS: each of the four stages renders into a DIFFERENT
     * element of the bloom family and reads the level before it, so the LEVEL is the pass boundary. Each entry
     * here is one pass's whole I/O:
     *
     *  * `targets` is the level it writes (element `level` of the `bloom` family, which the schema declares with
     *    four elements - see its `count`);
     *  * `barrier_images` is the level it READS, which is also the image the pass has to move to a sampled layout
     *    before it renders. Level 0 reads the HDR target instead, and that transition is deliberately NOT here:
     *    the host holds it, because the composite reads HDR too and because it is needed on the frames the whole
     *    bloom chain is skipped (see `post_composite_io`);
     *  * the LAST level's pass also hands its output back to a sampled layout, from its target - the
     *    composite samples all four levels, and level 3 has no successor to do it (the writer's hand-back the GI
     *    chain's passes also use).
     *
     * The push block is the post chain's ONE block (`post_push_bytes`): the shader declares it whole, every stage
     * writes a different `mode` into it, and the bytes the stages do not read are pushed with the same defaults
     * they always were - the prefilter passes `mode = 0`, the three downsamples `mode = 1`.
     */
    /// the levels' named target and source lists: a `pass_io` holds SPANS, so every list it names has to outlive
    /// it (a brace-initialized array inside the initializer would be a temporary the span dangles on)
    inline constexpr deren::utility::vessel::array<render_target, 1> post_bloom_0_target = {{render_target{.resource = resource_id::bloom, .element = 0, .kind = target_kind::color}}};
    inline constexpr deren::utility::vessel::array<render_target, 1> post_bloom_1_target = {{render_target{.resource = resource_id::bloom, .element = 1, .kind = target_kind::color}}};
    inline constexpr deren::utility::vessel::array<render_target, 1> post_bloom_2_target = {{render_target{.resource = resource_id::bloom, .element = 2, .kind = target_kind::color}}};
    inline constexpr deren::utility::vessel::array<render_target, 1> post_bloom_3_target = {{render_target{.resource = resource_id::bloom, .element = 3, .kind = target_kind::color}}};
    inline constexpr deren::utility::vessel::array<barrier_image, 1> post_bloom_1_source = {{barrier_image{.resource = resource_id::bloom, .element = 0}}};
    inline constexpr deren::utility::vessel::array<barrier_image, 1> post_bloom_2_source = {{barrier_image{.resource = resource_id::bloom, .element = 1}}};
    inline constexpr deren::utility::vessel::array<barrier_image, 1> post_bloom_3_source = {{barrier_image{.resource = resource_id::bloom, .element = 2}}};

    inline constexpr deren::utility::vessel::array<pass_io, 4> post_bloom_io = {{
        {.name = "post_bloom_0",
         .bindings = {},
         .targets = post_bloom_0_target,
         .barrier_images = {},
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
        {.name = "post_bloom_1",
         .bindings = {},
         .targets = post_bloom_1_target,
         .barrier_images = post_bloom_1_source,
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
        {.name = "post_bloom_2",
         .bindings = {},
         .targets = post_bloom_2_target,
         .barrier_images = post_bloom_2_source,
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
        {.name = "post_bloom_3",
         .bindings = {},
         .targets = post_bloom_3_target,
         .barrier_images = post_bloom_3_source,
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
    }};

    /// @brief what the composite RENDERS INTO by declaration: the swapchain (the FXAA-off case; see its comment)
    inline constexpr deren::utility::vessel::array<render_target, 1> post_composite_targets = {{render_target{.resource = resource_id::swapchain_image, .element = 0, .kind = target_kind::color}}};

    /**
     * @brief the composite's declaration: HDR plus the weighted bloom levels, tonemapped to the display
     *
     * It renders into the SWAPCHAIN by declaration and into the LDR image on the frames FXAA runs - a RECORDED
     * DEVIATION, the same one `deferred_io` records for `scene_color` and for the same reason: `render_target`
     * names one resource, and the composite's target decides its PIPELINE too (the swapchain's format against the
     * LDR image's R16F), so the host hands over both together.
     *
     * NO BARRIER IMAGE IS DECLARED, and that is the host's half rather than an omission: the HDR target's
     * transition to a sampled layout happens before the chain on EVERY frame (the composite reads it whether or
     * not bloom runs, and the bloom chain's prefilter reads it as well), so it has exactly one holder and that
     * holder has to be the frame loop. The four bloom levels arrive already sampled - the levels' passes
     * moved them, or the host's off path did on a frame the chain was skipped.
     */
    inline constexpr pass_io post_composite_io = {
        .name = "post_composite",
        .bindings = {},
        .targets = post_composite_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment},
    };

    /// @brief what the FXAA pass RENDERS INTO by declaration: the swapchain, which it is the last writer of
    inline constexpr deren::utility::vessel::array<render_target, 1> fxaa_targets = {{render_target{.resource = resource_id::swapchain_image, .element = 0, .kind = target_kind::color}}};

    /// @brief the image the FXAA pass reads and therefore has to move to a sampled layout: the composite's LDR output
    inline constexpr deren::utility::vessel::array<barrier_image, 1> fxaa_barriers = {{barrier_image{.resource = resource_id::ldr, .element = 0}}};

    /**
     * @brief the FXAA pass's declaration: the gamma-encoded LDR image -> the anti-aliased swapchain
     *
     * It reads the SAME shared post resources the composite writes (the composite writes the LDR image through
     * binding 5 and FXAA reads it back through it, which is why FXAA cannot be folded into
     * `post.frag`: a descriptor may not name the image the pipeline is rendering into, and that would be a
     * different statically-used binding set).
     *
     * ITS INPUT IS DECLARED, unlike the composite's, and the difference is the frame: the LDR image is written by
     * the composite and read here, so this pass is the one that moves it - there is no frame where it has to be
     * moved and no pass runs, because a frame without FXAA never touches it at all.
     *
     * The push block is the chain's (`post_push_bytes`): FXAA is mode 3 of the same shader's block.
     */
    inline constexpr pass_io fxaa_io = {
        .name = "fxaa",
        .bindings = {},
        .targets = fxaa_targets,
        .barrier_images = fxaa_barriers,
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment},
    };

    /// @brief the upscale pass's push block, in bytes: EASU's four `float4` constants and the two scalar
    ///        lanes; its shape is `deren.engine.pass.upscale`'s `push_constants`
    /// @note 72, and NOT the 80 bytes the shader's block adds up to: the framework appends the heap index lanes
    ///       AFTER these bytes (`runtime::push_stage_block`), so the pass declares only what it composes. Get
    ///       this wrong in the other direction - declare the lanes as part of the pass's size - and they
    ///       land past the fields the shader reads them from, which is a silent wrong-image rather than a
    ///       validation error.
    inline constexpr uint32_t upscale_push_bytes = 72;

    /// @brief what the UPSCALE pass RENDERS INTO by declaration: the swapchain, i.e. the OUTPUT image whose
    ///        extent is the one thing this pass asks for that is not the frame's
    inline constexpr deren::utility::vessel::array<render_target, 1> upscale_targets = {{render_target{.resource = resource_id::swapchain_image, .element = 0, .kind = target_kind::color}}};

    /// @brief the image the upscale pass reads and therefore has to move to a sampled layout: the composite's LDR
    ///        output - the same image FXAA reads, which is why the two passes are mutually exclusive
    inline constexpr deren::utility::vessel::array<barrier_image, 1> upscale_barriers = {{barrier_image{.resource = resource_id::ldr, .element = 0}}};

    /**
     * @brief the UPSCALE pass's declaration: the display-referred LDR image -> the presented swapchain, by a
     *        linear (bilinear) resample
     *
     * WHY IT EXISTS: `core::render_extent()` is the RENDER chain's extent, so at `[render] render_scale < 1.0`
     * every image the chain renders into is created smaller while the swapchain keeps the output size. Before
     * this declaration the scaled frame was simply presented in the TOP-LEFT QUADRANT of that output - the
     * scene was shaded, at a lower resolution, and nothing resolved it. This pass is that resolve.
     *
     * THE TARGET IS THE SWAPCHAIN AND ITS EXTENT IS THE OUTPUT, and that pairing is the whole mechanism: this
     * is the one resource `runtime::resolve_resource_extent` answers with the swapchain's extent rather than
     * the frame's, so a pass declaring `extent_rule::resource` over it gets a viewport covering the presented
     * image while it samples an image at the render extent (`behaviour::resync_viewport` sets both).
     *
     * IT IS FXAA'S SIBLING AND ITS EXCLUSIVE ALTERNATIVE, and the exclusion is a correctness rule rather than
     * tidiness: both read the same LDR image, both want to be the frame's last writer, and FXAA's edge filter
     * is defined at the resolution it filters - so on a frame this pass resolves, FXAA must not run at all
     * (its result would be resampled away). `runtime::post_fxaa_active` carries that exclusion, and the
     * composite's target choice, the frame's `write_ldr` and the overlay's holder all read the two predicates
     * from `frame_facts` so nothing can disagree.
     *
     * The push block is this pass's and is 72 bytes - EASU's four constants (4 x float4) and the two scalar
     * lanes (which filter, and the display transfer, whose value follows from the surface's format exactly as
     * FXAA's does). It is deliberately NOT `post_push_bytes`: the resolve samples one image and filters it, so
     * every lane of the post chain's block would be a dead field. The shader's block is this size PLUS the two
     * heap index lanes the framework appends (the LDR image is a per-swapchain-image heap slot the shader names
     * itself - see source/shaders/post/upscale.slang).
     */
    inline constexpr pass_io upscale_io = {
        .name = "upscale",
        .bindings = {},
        .targets = upscale_targets,
        .barrier_images = upscale_barriers,
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = upscale_push_bytes, .stages = stage_flag::fragment},
    };

    /// @brief what the debug view RENDERS INTO: the HDR target, which is the image it actually writes - so unlike
    ///        the deferred stage's and the composite's, this declaration carries no deviation
    inline constexpr deren::utility::vessel::array<render_target, 1> gbuffer_debug_targets = {{render_target{.resource = resource_id::hdr, .element = 0, .kind = target_kind::color}}};

    /// @brief the four images it moves to a sampled layout: the three stored surface targets and the motion vectors
    inline constexpr deren::utility::vessel::array<barrier_image, 4> gbuffer_debug_barriers = {{barrier_image{.resource = resource_id::gbuffer_targets, .element = 0},
                                                                                                barrier_image{.resource = resource_id::gbuffer_targets, .element = 1},
                                                                                                barrier_image{.resource = resource_id::gbuffer_targets, .element = 2},
                                                                                                barrier_image{.resource = resource_id::velocity, .element = 0}}};

    /**
     * @brief the G-buffer debug view's declaration: the stored surface, one channel at a time, into the HDR target
     *
     * Everything it READS is one of the frame's shared G-buffer resources (the three targets, the depth, the
     * velocity), so it declares no separate binding - and the DEPTH is deliberately not among its barrier
     * images: its old layout depends on whether the G-buffer instance rendered this frame, which is shared
     * per-image bookkeeping the host holds (the same hand-back the deferred stage's frame carries as a callback).
     * The velocity target IS declared, because the debug view runs INSTEAD of the lighting stage and is therefore
     * the only stage that hands it to a sampler on those frames.
     *
     * The push block is 16 bytes - the channel, the two projection terms the depth channel linearizes with, and the
     * motion gain - and all four values are the frame's (the renderer's knob and camera).
     */
    inline constexpr pass_io gbuffer_debug_io = {
        .name = "gbuffer-debug",
        .bindings = {},
        .targets = gbuffer_debug_targets,
        .barrier_images = gbuffer_debug_barriers,
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 16, .stages = stage_flag::fragment},
    };

    /// @brief the layers the shadow pass renders into: a RUN of elements, one rendering instance per cascade
    ///
    /// THIS USED TO BE A RECORDED DEVIATION, and the vocabulary that closed it is `render_target::count`: the
    /// declaration named the image's FIRST layer and the host handed over the layers this frame has
    /// (`runtime::resolve_shadow_pass`). The deviation was forced by the framework - the validator refuses a
    /// second DEPTH target, because a rendering instance has one depth attachment - but it was never a fact about
    /// the shadow pass: that pass renders 1..`max_shadow_cascades` LAYERS of ONE array image, one instance per
    /// cascade, which is exactly a run of elements. The count is the schema's four (the family's count), and
    /// the FRAME caps it: `create_shadow_resources` allocates the layers the cascade knob asks for, and only
    /// those layers are published - so a one-cascade frame resolves one target and a three-cascade frame three.
    inline constexpr deren::utility::vessel::array<render_target, 1> shadow_targets = {{render_target{.resource = resource_id::shadow_map, .element = 0, .kind = target_kind::depth, .count = 4}}};
    /**
     * @brief the shadow pass's declaration: the scene's depth from the light, one cascade at a time
     *
     * The PUSH BLOCK is four bytes at offset 96 - the cascade index, pushed at `deren::engine::scene_cascade_push_offset`
     * (`scene_push_constant_size`), which is where the scene's push block ends: this pass draws the scene's
     * leaves through the SCENE pipeline layout, so its push shares that layout's range rather than holding one. The
     * literal is checked against the constant where both are visible (`deren.engine.pass.shadow`'s static_assert), the
     * same "two copies, one size" rule the other pass blocks follow.
     *
     * NO BARRIER IMAGES: each layer is transitioned to a depth attachment immediately before the instance that
     * renders it, so the pass reaches every image it moves through `targets` - the shape the composite and the
     * debug view have. The targets are a RUN (`shadow_targets`), which is what makes that reach enough: the run
     * is however many layers this frame's map has, in cascade order.
     */
    inline constexpr pass_io shadow_io = {
        .name = "shadow",
        .bindings = {},
        .targets = shadow_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 108, .size = 4, .stages = stage_flag::vertex | stage_flag::fragment},
    };
} // namespace deren::engine::render_resource
