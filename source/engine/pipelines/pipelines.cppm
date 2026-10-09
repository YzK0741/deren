// module version: 0.20.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file source/engine/pipelines/pipelines.cppm
 * @defgroup engine_pipelines Per-Pass Pipeline Builders
 * @brief The engine's per-pass pipelines: one builder per pass, next to the generic builder in
 *        deren.vulkan.core.pipeline (that one knows HOW to build a pipeline, this one knows what each pass's
 *        pipeline looks like - formats, sample counts, blend state, push-constant ranges).
 *
 * Extracted from deren.engine.runtime, whose implementation had grown past 4900 lines. The builders are
 * stateless, and EVERY ONE OF THEM NOW TAKES THE CONTRACT (`rhi::api_core`) ALONE (abi 21): they used to take
 * a `VkDevice` as well - a leftover from the years when a builder created its pipeline raw through the escape -
 * and after the last raw builder (the heap probe's graphics pipeline) moved onto `create_pipeline`, not one of
 * them read it. A builder that needs the surface's format is handed that as a parameter instead (the composite
 * and FXAA, which write the swapchain image). Ownership stays with whoever asked for the build (the runtime
 * today, a pass once its three-piece has moved).
 *
 * NO BUILDER TAKES OR MAKES A PIPELINE LAYOUT: every stage is heap-native, so a pipeline is created with
 * `layout = VK_NULL_HANDLE` and the descriptor-heap flag (see deren.vulkan.core.pipeline), and the descriptors a stage
 * reads come from the frame's bound heap rather than from a set. The set-layout plumbing the builders used to
 * thread through is gone with it.
 *
 * The pipeline handles are std::optional because vk_pipeline is an RAII owner with no default
 * constructor, which is also how the runtime holds them.
 */

module;

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

export module deren.engine.pipelines;

import deren.promise.rhi;
import deren.engine.render_resource;
import deren.engine.render_layout;

namespace deren::engine::pipelines {

    namespace rhi = deren::promise::rhi;

    /// THE TWO RELEASES THE ENGINE'S OWN RAII TYPES CALL, out of line here for the reason below.
    /// THE RELEASES RUN OUT-OF-LINE, here in the module that owns the handles: a virtual release()
    /// call textually inside an importer's destructor trips a clang 22 codegen crash (EmitBuiltin
    /// NewDeleteCall under EmitDeferred, measured this session), and keeping the call here also
    /// keeps the importer's generated code free of it.
    ///
    /// `release_contract_shader` IS GONE WITH THE RAW SHADER LANE (abi 21): the engine-owned shader module
    /// handle it existed for (`shader_module_handle` / `make_shader_module_raw`) had exactly ONE user - the
    /// heap probe's graphics builder - and that builder now goes through `create_pipeline`, which owns its
    /// modules inside the backend. `release_contract_pipeline` is the one the pipeline RAII needs.
    export void release_contract_pipeline(rhi::pipeline* resource) noexcept;

    /// AN ENGINE-HELD PIPELINE (abi 8): a CONTRACT pipeline (every pipeline this renderer builds since abi 21),
    /// created through `create_pipeline` and released inside the backend when this dies.
    ///
    /// THE RAW LANE IS GONE (this batch): the type first gave up OWNING a raw `VkPipeline` (abi 21), and now it
    /// gives up CARRYING one: its last reader was `begin_pipeline` - the runner's raster bind of the geometry
    /// pipelines - and that verb takes the CONTRACT command buffer now, so `native`, its `get_pipeline()`
    /// accessor and the eight `vulkan_escape::native_pipeline()` reads that filled it are deleted. The stored
    /// dynamic state speaks the CONTRACT's vocabulary too (`rhi::viewport` / `rhi::rect`), which is what lets the
    /// bind and the state be stated as the record series' own verbs.
    export struct pipeline_handle {
        /// SET FOR EVERY PIPELINE THIS RENDERER BUILDS: its release() runs inside the backend.
        rhi::pipeline* contract = nullptr;
        /// the per-pipeline dynamic state the runner caches per frame extent and `begin_pipeline` re-emits
        rhi::viewport viewport = {};
        rhi::rect scissor = {};

        // EVERY SPECIAL MEMBER IS OUT-OF-LINE, in this module: an importer TU that generated the
        // destructor's body itself crashed clang 22's codegen (EmitBuiltinNewDeleteCall under
        // EmitDeferred, measured this session) - with the bodies defined HERE the importer only
        // calls them, which is both the workaround and the better shape for a module type.
        pipeline_handle() noexcept;
        pipeline_handle(rhi::pipeline* owned) noexcept;
        pipeline_handle(pipeline_handle&& other) noexcept;
        pipeline_handle& operator=(pipeline_handle&& other) noexcept;
        pipeline_handle(pipeline_handle const&) = delete;
        pipeline_handle& operator=(pipeline_handle const&) = delete;
        ~pipeline_handle() noexcept;
        /// bind + re-emit the stored dynamic state, through the record series: the three verbs the raw shape
        /// spelled, and the reason a caller no longer needs the pipeline's native handle.
        void begin_pipeline(deren::promise::rhi::command_buffer& commands) const noexcept {
            if (this->contract == nullptr) {
                return; // nothing was built: a bind of nothing is worse than a skipped one
            }
            (void)commands.bind_pipeline(*this->contract);
            commands.set_viewport(this->viewport);
            commands.set_scissor(this->scissor);
        }

    private:
        void release_owned() noexcept;
    };

    /// THE CONTRACT'S GRAPHICS PIPELINE FACTORY, in the shape `make_pipeline` used to spell: the
    /// vertex-input derivation and the dynamic-state shape stay backend-internal, the caller speaks
    /// contract formats and blend modes.
    export [[nodiscard]] std::expected<pipeline_handle, std::string> make_graphics_pipeline(rhi::api_core& face,
                                                                                            std::span<rhi::image_format const> const color_formats,
                                                                                            rhi::image_format const depth_format,
                                                                                            std::span<uint8_t const> const vertex_code,
                                                                                            std::span<uint8_t const> const fragment_code,
                                                                                            std::uint32_t const sample_count,
                                                                                            bool const depth_test,
                                                                                            float const depth_bias_constant_factor,
                                                                                            float const depth_bias_slope_factor,
                                                                                            float const depth_bias_clamp,
                                                                                            std::span<rhi::blend_mode const> const blend_modes = {},
                                                                                            rhi::shader_stage const first_stage = rhi::shader_stage::vertex,
                                                                                            rhi::depth_compare const compare = rhi::depth_compare::less_or_equal,
                                                                                            char const* const what = "pipeline") {
        rhi::pipeline_desc desc{};
        desc.color_formats = color_formats;
        desc.depth_format = depth_format;
        desc.vertex_code = std::as_bytes(std::span(vertex_code));
        desc.fragment_code = std::as_bytes(std::span(fragment_code));
        desc.first_stage = first_stage;
        desc.sample_count = sample_count;
        desc.depth_test = depth_test;
        desc.depth_bias_constant_factor = depth_bias_constant_factor;
        desc.depth_bias_slope_factor = depth_bias_slope_factor;
        desc.depth_bias_clamp = depth_bias_clamp;
        desc.blend_modes = blend_modes;
        desc.compare = compare;
        desc.debug_name = what;
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return std::unexpected(std::string(what) + ": the contract's pipeline factory refused the descriptor");
        }
        return pipeline_handle(built);
    }

    /// what build_post() creates: the chain's two composites
    export struct post_owned {
        std::optional<pipeline_handle> composite; // tonemap + bloom sum, writes the swapchain
        std::optional<pipeline_handle> hdr;       // the same pass writing an HDR target instead (FXAA on)
    };

    /// @brief what build_gbuffer_debug() creates: the debug view's pipeline
    export struct gbuffer_owned {
        std::optional<pipeline_handle> debug;
    };

    /// what build_taa() creates: the resolve pipeline
    export struct taa_owned {
        std::optional<pipeline_handle> resolve;
    };

    /// what a compute builder returns: the COMPUTE pipeline
    export struct compute_pipeline_owned {
        std::optional<pipeline_handle> trace;
    };

    export std::expected<post_owned, std::string> build_post(rhi::api_core& face, rhi::image_format swap_chain_format,
                                                             std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<gbuffer_owned, std::string> build_gbuffer_debug(rhi::api_core& face, rhi::image_format color_format, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<taa_owned, std::string> build_taa(rhi::api_core& face, rhi::image_format color_format, std::span<uint8_t const> vertex_shader_code,
                                                           std::span<uint8_t const> fragment_shader_code);

    /// the ray-traced sun shadow: a compute pipeline over the descriptors the frame's heap carries
    export std::expected<compute_pipeline_owned, std::string> build_two_set_compute(rhi::api_core& face, std::span<uint8_t const> compute_shader_code);
    /// the stochastic punctual lighting trace (source/shaders/megalights_trace.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_trace(rhi::api_core& face,
                                                                                     std::span<uint8_t const> compute_shader_code);
    /// the stochastic chain's temporal resolve (source/shaders/megalights_temporal.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(rhi::api_core& face,
                                                                                        std::span<uint8_t const> compute_shader_code);
    /// the mask bake: a compute pass over the material table and the texture array
    /// array), which collapses the triangles a material's alphaMode MASK cuts out and writes the expanded
    /// vertices a bottom level structure is then built from - see source/shaders/mask_bake.slang
    export std::expected<compute_pipeline_owned, std::string> build_mask_bake(rhi::api_core& face, std::span<uint8_t const> compute_shader_code);
    /// the compute skinning pass: the scene block's per-joint matrices - see
    /// source/shaders/compute_skin.slang
    export std::expected<compute_pipeline_owned, std::string> build_compute_skin(rhi::api_core& face, std::span<uint8_t const> compute_shader_code);
    /// the clustered-light sort (source/shaders/light_cluster.slang): heap-native, and NO push constants
    /// at all - the shader reads the light UBO and writes the two cluster buffers through heap slots, which is
    /// why this builder takes no push size. It is the first compute pipeline in this module
    /// that came out of `deren.vulkan.core`.
    export std::expected<compute_pipeline_owned, std::string> build_cluster(rhi::api_core& face, std::span<uint8_t const> compute_shader_code);

    /**
     * @brief the HEAP-NATIVE probe's pipeline: the first one in this renderer created the heap way
     * @param device the logical device
     * @param compute_shader_code the probe's SPIR-V (see source/shaders/heap_probe_comp.slang)
     * @return the pipeline, or the reason it could not be created
     * @note NO SET LAYOUT AND NO PIPELINE LAYOUT, which is not a simplification but the flag's requirement:
     *       "the pipeline layout must be NULL and shader resources will be sourced from a descriptor heap". The
     *       probe's parameters therefore reach the shader through vkCmdPushDataEXT (see descriptor_heap::push_data)
     *       and not through vkCmdPushConstants, which needs a layout to push to.
     */
    export std::expected<compute_pipeline_owned, std::string> build_heap_probe(rhi::api_core& face, std::span<uint8_t const> compute_shader_code);

    /// the probe's target: one size for the image, the viewport, the scissor and the readback, so a mismatch
    /// between them is impossible rather than merely unlikely
    export inline constexpr uint32_t heap_probe_extent = 4u;

    /**
     * @brief the GRAPHICS half of the heap-native probe: a heap-flagged, layout-less pipeline over two stages
     * @param device the logical device
     * @param colour_format the format the probe renders into (dynamic rendering, like every pass here)
     * @param vertex_code / @param fragment_code the probe's SPIR-V (see source/shaders/heap_probe.slang)
     * @return the pipeline, or the reason it could not be created
     * @note no vertex input, no blend and a static viewport: the probe's subject is the FRAGMENT stage reading the
     *       heap through a graphics pipeline at all, and every one of those would be a second thing that could be
     *       wrong. The flag and the null layout are the rule the compute probe established.
     */
    /// @param first_stage the stage that emits the geometry: VERTEX for the original probe, MESH for the
    ///        mesh-shader mechanism proof (docs/mesh_shaders.md step 0). Everything else - the empty vertex
    ///        input, the heap flag, the NULL layout, the fragment stage - is identical between the two.
    export std::expected<pipeline_handle, std::string> build_heap_probe_graphics(rhi::api_core& face, rhi::image_format colour_format, std::span<uint8_t const> vertex_code, std::span<uint8_t const> fragment_code, rhi::shader_stage first_stage = rhi::shader_stage::vertex);

    /// what build_resolve_pipeline() creates: the resolve pipeline
    export struct resolve_pipeline_owned {
        std::optional<pipeline_handle> resolve;
    };

    export std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(rhi::api_core& face,
                                                                                     std::span<uint8_t const> compute_shader_code);

    export std::expected<pipeline_handle, std::string> build_fxaa(rhi::api_core& face, rhi::image_format swap_chain_format, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    /**
     * @brief the SHADOW pass's depth-only pipeline
     *
     * WHY IT TAKES A DEVICE rather than being `core::make_depth_pipeline`: that method is a member of
     * the `core` object (it reads `this->device`, and the caller's depth format), and
     * a PASS reaches neither - the device and the format arrive through `pass_context`. The three
     * BIAS factors are parameters for the same reason the depth format is: they are the pipeline's, not the
     * device's, and the depth pass is the one pipeline in this renderer created with slope-scaled bias.
     * @param device the logical device
     * @param depth_format the shadow map's depth attachment format
     * @param depth_bias_constant_factor constant depth bias
     * @param depth_bias_slope_factor slope-scaled depth bias
     * @param depth_bias_clamp depth bias clamp, 0 disables clamping
     * @param vertex_shader_code the geometry stage's SPIR-V (the mesh path's own module when it fetches its
     *        own vertices)
     * @param fragment_shader_code the fragment stage's SPIR-V, the same shader either way
     * @param first_stage the stage that emits the geometry: VERTEX for the input-assembler path, MESH for the
     *        one that fetches its own vertices - the FRAGMENT stage is the same shader either way, which is
     *        what makes the two paths comparable (see docs/mesh_shaders.md step 1)
     */
    export std::expected<pipeline_handle, std::string> build_shadow(rhi::api_core& face, rhi::image_format depth_format, float depth_bias_constant_factor, float depth_bias_slope_factor,
                                                                    float depth_bias_clamp, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code,
                                                                    rhi::shader_stage first_stage = rhi::shader_stage::vertex);
    /// @brief what the FXAA pass's own create step needs: the anti-aliasing pipeline
    export struct fxaa_owned {
        std::optional<pipeline_handle> antialias;
    };
    export std::expected<fxaa_owned, std::string> build_fxaa_owned(rhi::api_core& face, rhi::image_format swap_chain_format,
                                                                   std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    /**
     * @brief what the UPSCALE pass's own create step needs: the resolve pipeline
     *
     * FXAA's sibling, and its body is the same pipeline recipe on purpose: both passes read the composite's
     * display-referred LDR image and write the swapchain with `post.vert.spv`'s synthetic triangle and no depth
     * attachment, so the only things that differ are the fragment shader and the viewport the RUNNER sets from
     * each pass's own declaration (FXAA's is the frame's extent, this one's is the swapchain's).
     */
    export struct upscale_owned {
        std::optional<pipeline_handle> resolve;
    };
    export std::expected<upscale_owned, std::string> build_upscale_owned(rhi::api_core& face, rhi::image_format swap_chain_format,
                                                                         std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export struct deferred_owned {
        std::optional<pipeline_handle> lighting;
    };

    /// the additive blend state comes in as a parameter: the helper that builds it is a local of the
    /// runtime, next to the passes whose blend modes it describes
    export std::expected<deferred_owned, std::string> build_deferred(rhi::api_core& face, std::span<rhi::blend_mode const> color_blend, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    // post: the composite chain's owner. The two fullscreen pipelines (one per color format the chain renders
    // into) are created here.
    std::expected<post_owned, std::string> build_post(rhi::api_core& face, rhi::image_format const swap_chain_format,
                                                      std::span<uint8_t const> const vertex_shader_code,
                                                      std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        post_owned out;

        // TWO variants, one per color format the chain renders into: the composite writes the
        // swapchain, the bright-pass prefilter and the downsample passes write the R16F bloom levels. A
        // pipeline's rendering color format must match its attachment, so one swapchain-format pipeline
        // was a validation error for the HDR passes.
        auto const make_post_variant = [&](rhi::image_format const color_format) -> std::expected<pipeline_handle, std::string> {
            return make_graphics_pipeline(face,
                                          std::span<rhi::image_format const>(&color_format, 1), rhi::image_format::unknown,
                                          vertex_shader_code, fragment_shader_code, 1u, false, 0.0f, 0.0f, 0.0f,
                                          {}, rhi::shader_stage::vertex, rhi::depth_compare::less_or_equal,
                                          "post variant");
        };

        auto composite_pipeline = make_post_variant(swap_chain_format);
        if (!composite_pipeline) {
            return fail(std::move(composite_pipeline.error()));
        }
        out.composite = std::move(composite_pipeline).value();

        auto hdr_pipeline = make_post_variant(rhi::image_format::r16g16b16a16_sfloat);
        if (!hdr_pipeline) {
            return fail(std::move(hdr_pipeline.error()));
        }
        out.hdr = std::move(hdr_pipeline).value();
        return out;
    }

    std::expected<gbuffer_owned, std::string> build_gbuffer_debug(rhi::api_core& face, rhi::image_format const color_format,
                                                                  std::span<uint8_t const> const vertex_shader_code,
                                                                  std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        gbuffer_owned out;

        auto pipeline_result = make_graphics_pipeline(face,
                                                      std::span<rhi::image_format const>(&color_format, 1), rhi::image_format::unknown,
                                                      vertex_shader_code, fragment_shader_code, 1u, false, 0.0f, 0.0f, 0.0f,
                                                      {}, rhi::shader_stage::vertex, rhi::depth_compare::less_or_equal,
                                                      "g-buffer debug view pipeline");
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.debug = std::move(pipeline_result).value();
        return out;
    }
    // taa: the resolve pass' owner. It writes the HDR target, so its rendering color format is hdr_format
    // (a span of one), and its sampler is the odd one out - linear magnification, nearest minification,
    // because the resolve upsamples the scene color but must not average neighbouring history texels.
    std::expected<taa_owned, std::string> build_taa(rhi::api_core& face, rhi::image_format const color_format, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        taa_owned out;

        auto pipeline_result = make_graphics_pipeline(face,
                                                      std::span<rhi::image_format const>(&color_format, 1), rhi::image_format::unknown,
                                                      vertex_shader_code, fragment_shader_code, 1u, false, 0.0f, 0.0f, 0.0f,
                                                      {}, rhi::shader_stage::vertex, rhi::depth_compare::less_or_equal,
                                                      "taa resolve pipeline");
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.resolve = std::move(pipeline_result).value();
        return out;
    }

    // The mask bake (see source/shaders/mask_bake.slang): a compute pipeline over the material heap slots ALONE, because
    // everything it needs is there - the material table for the alpha texture's index and the cutoff, and the
    // bindless texture array to sample it. It owns no set layout, like every traced compute pass, and it is the only compute
    // pass here whose output is not an image: it writes vertices into a buffer the acceleration structure is
    // then built from.
    std::expected<compute_pipeline_owned, std::string> build_mask_bake(rhi::api_core& face, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;
        // THE PIPELINE IS A CONTRACT OBJECT NOW (abi 21): the heap-native compute rules live in the backend
        // (`core::create_compute_pipeline`), which is the side that owns the bind point and the heap flag.
        rhi::pipeline_desc desc{};
        desc.first_stage = rhi::shader_stage::compute;
        desc.compute_code = std::as_bytes(compute_shader_code);
        desc.debug_name = "mask bake";
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("mask bake: the contract's compute pipeline factory refused the descriptor");
        }
        out.trace = pipeline_handle(built);
        return out;
    }

    // The compute skinning pass (see source/shaders/compute_skin.slang): the same shape as the mask bake above and
    // for the same reason - it reads only the per-joint matrices heap slot.
    std::expected<compute_pipeline_owned, std::string> build_compute_skin(rhi::api_core& face, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;
        // THE PIPELINE IS A CONTRACT OBJECT NOW (abi 21): the heap-native compute rules live in the backend
        // (`core::create_compute_pipeline`), which is the side that owns the bind point and the heap flag.
        rhi::pipeline_desc desc{};
        desc.first_stage = rhi::shader_stage::compute;
        desc.compute_code = std::as_bytes(compute_shader_code);
        desc.debug_name = "compute skin";
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("compute skin: the contract's compute pipeline factory refused the descriptor");
        }
        out.trace = pipeline_handle(built);
        return out;
    }

    // The clustered-light sort: NO set, NO layout and NO push range. Every stage of the frame is heap-native
    // (see docs/descriptor_heap_handover.md), so this pipeline is created with VK_NULL_HANDLE and the heap flag;
    // the shader reads the light UBO and the cluster buffers out of the scene block by slot, and the slot itself
    // travels in the stage push block (source/shaders/heap_slots.glsl). Owning the pipeline is all that is left to own.
    std::expected<compute_pipeline_owned, std::string> build_cluster(rhi::api_core& face, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;
        // THE PIPELINE IS A CONTRACT OBJECT NOW (abi 21): the heap-native compute rules live in the backend
        // (`core::create_compute_pipeline`), which is the side that owns the bind point and the heap flag.
        rhi::pipeline_desc desc{};
        desc.first_stage = rhi::shader_stage::compute;
        desc.compute_code = std::as_bytes(compute_shader_code);
        desc.debug_name = "cluster";
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("cluster: the contract's compute pipeline factory refused the descriptor");
        }
        out.trace = pipeline_handle(built);
        return out;
    }

    std::expected<compute_pipeline_owned, std::string> build_heap_probe(rhi::api_core& face, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;
        // THE PIPELINE IS A CONTRACT OBJECT NOW (abi 21): the heap-native compute rules live in the backend
        // (`core::create_compute_pipeline`), which is the side that owns the bind point and the heap flag.
        rhi::pipeline_desc desc{};
        desc.first_stage = rhi::shader_stage::compute;
        desc.compute_code = std::as_bytes(compute_shader_code);
        desc.debug_name = "heap probe";
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("heap probe: the contract's compute pipeline factory refused the descriptor");
        }
        out.trace = pipeline_handle(built);
        return out;
    }

    // The SHARED builder every traced pass uses (its name still says `two_set`, from the two sets it used to
    // declare): the shape several traced passes have in common - the camera block and the light UBO in the scene
    // block (plus the top level structure at binding 16 when the device has ray tracing), the stored surface in
    // the G-buffer images - so the caller's only variable is the push block size.
    // (Its old name, build_rt_shadow, is gone with the ray-query shadow pass: the shadow traces through a real
    // ray-tracing PIPELINE now, which is a different builder below.)
    std::expected<pipeline_handle, std::string> build_heap_probe_graphics(rhi::api_core& face, rhi::image_format const colour_format, std::span<uint8_t const> const vertex_code, std::span<uint8_t const> const fragment_code, rhi::shader_stage const first_stage) {
        // THE PROBE IS A GRAPHICS RECIPE LIKE EVERY OTHER ONE IN THIS FILE (see make_graphics_pipeline): one
        // colour target in the format the caller names, NO depth, no blend, single-sampled - and the FIRST STAGE
        // is the parameter that makes the vertex probe and the MESH probe (docs/mesh_shaders.md step 0) the same
        // builder: a mesh pipeline substitutes the mesh stage and ignores the (empty) vertex input.
        //
        // THE RAW PATH THAT STOOD HERE IS GONE WITH THE CONTRACT'S OWN FACTORY, and this builder was its LAST
        // user: `make_shader_module_raw` (two vkCreateShaderModule calls), a hand-built
        // `VkGraphicsPipelineCreateInfo`, `vkCreateGraphicsPipelines` and `pipeline_handle(raw, device)` - whose
        // destructor was the engine's only vkDestroyPipeline. The heap flag and the NULL layout the comment above
        // insists on are `create_pipeline`'s business now, exactly as they are for every other recipe in this
        // file; the probe's own subject (a fragment stage reading the heap through a graphics pipeline at all) is
        // unchanged, which is why the two probes still compare directly.
        std::array<rhi::image_format, 1> const color_formats = {colour_format};
        return make_graphics_pipeline(face,
                                      std::span<rhi::image_format const>(color_formats),
                                      rhi::image_format::unknown, // no depth attachment
                                      vertex_code,
                                      fragment_code,
                                      1u,
                                      false, // no depth test
                                      0.0f,
                                      0.0f,
                                      0.0f,
                                      {},
                                      first_stage,
                                      rhi::depth_compare::less_or_equal,
                                      "heap probe (graphics) pipeline");
    }

    std::expected<compute_pipeline_owned, std::string> build_two_set_compute(rhi::api_core& face, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;
        // THE PIPELINE IS A CONTRACT OBJECT NOW (abi 21): the heap-native compute rules live in the backend
        // (`core::create_compute_pipeline`), which is the side that owns the bind point and the heap flag.
        rhi::pipeline_desc desc{};
        desc.first_stage = rhi::shader_stage::compute;
        desc.compute_code = std::as_bytes(compute_shader_code);
        desc.debug_name = "rt shadow";
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("rt shadow: the contract's compute pipeline factory refused the descriptor");
        }
        out.trace = pipeline_handle(built);
        return out;
    }
    /**
     * @brief what the ray-tracing shadow pipeline builder returns
     *
     * `group_count` is the number of shader groups the pipeline created, in the order the SBT must follow
     * (raygen, miss, hit): the shader binding table itself is the CALLER's, because the group handles it is
     * filled with are per-pipeline data and its regions have to outlive this call.
     */
    export struct ray_tracing_pipeline_owned {
        std::optional<pipeline_handle> pipeline;
        uint32_t group_count = 0;
    };

    /**
     * @brief the ray-traced sun shadow's PIPELINE: one raygen, one miss and one triangles hit group
     *
     * WHY A PIPELINE AND NOT AN INLINE RAY QUERY, which is what this pass used to run: a ray query has no
     * any-hit stage, so an alphaMode MASK surface is SOLID to the ray. A traced ray can run an any-hit shader
     * for exactly that test, and it can consult an opacity micromap, which is the hardware form of the same
     * question (per-microtriangle opacity, with the any-hit shader as the fallback for its 'unknown' states).
     * Both of those live in the hit group this creates.
     *
     * Recursion depth is 1: the shadow ray answers a yes/no question and the traversal terminates on the first
     * hit (`gl_RayFlagsTerminateOnFirstHitEXT` in the raygen), so there is nothing for a second level to do.
     */
    export std::expected<ray_tracing_pipeline_owned, std::string> build_rt_shadow_ray_tracing(rhi::api_core& face,
                                                                                              std::span<uint8_t const> raygen_code,
                                                                                              std::span<uint8_t const> closest_hit_code, std::span<uint8_t const> miss_code,
                                                                                              std::span<uint8_t const> any_hit_code) {
        using fail = std::unexpected<std::string>;
        ray_tracing_pipeline_owned out;

        // THE PIPELINE IS A CONTRACT OBJECT NOW (abi 21): the ray-tracing rules - the entry-point resolution
        // (the loader exports no extension command), the heap flag, the group TYPE derived from which slots a
        // group fills, and the ray-tracing bind point - live in the backend (`core::create_ray_tracing_pipeline`),
        // which is the side that owns them. What stays here is the pipeline's SHAPE, which is this pass's.
        std::array<rhi::ray_tracing_stage, 4> const stages = {
            rhi::ray_tracing_stage{.stage = rhi::shader_stage::ray_generation, .code = std::as_bytes(raygen_code), .debug_name = "rt shadow raygen"},
            rhi::ray_tracing_stage{.stage = rhi::shader_stage::miss, .code = std::as_bytes(miss_code), .debug_name = "rt shadow miss"},
            rhi::ray_tracing_stage{.stage = rhi::shader_stage::closest_hit, .code = std::as_bytes(closest_hit_code), .debug_name = "rt shadow closest hit"},
            rhi::ray_tracing_stage{.stage = rhi::shader_stage::any_hit, .code = std::as_bytes(any_hit_code), .debug_name = "rt shadow any hit"},
        };
        // THE GROUP ORDER IS THE SHADER BINDING TABLE'S ORDER, and the migration did not move it: group 0 is
        // the raygen, group 1 the miss shader, and group 2 is the hit group that names BOTH the closest-hit
        // stage (index 2) and the any-hit stage (index 3) - the any-hit shader is what lets an alphaMode MASK
        // surface refuse the intersection, which is why this pipeline exists at all. The count travels back
        // with the pipeline because the caller's regions follow exactly this order.
        std::array<rhi::ray_tracing_group, 3> const groups = {
            rhi::ray_tracing_group{.general = 0},
            rhi::ray_tracing_group{.general = 1},
            rhi::ray_tracing_group{.closest_hit = 2, .any_hit = 3, .triangles = true},
        };
        rhi::pipeline_desc desc{};
        desc.ray_tracing_stages = stages;
        desc.ray_tracing_groups = groups;
        // RECURSION DEPTH 1 (the header's own note): the shadow ray answers a yes/no question and terminates on
        // the first hit, so there is nothing for a second level to do.
        desc.max_ray_recursion = 1u;
        desc.debug_name = "rt shadow";
        std::array<rhi::acceleration_structure_heap_binding, 1> const bindings = {
            rhi::acceleration_structure_heap_binding{
                .stage = rhi::shader_stage::ray_generation,
                .descriptor_set = 0,
                .binding = 16,
                .byte_offset = static_cast<uint32_t>(render_layout::heap_slots::tlas * render_layout::heap_slot_stride),
                .array_stride = static_cast<uint32_t>(render_layout::heap_slot_stride),
                .array_count = rhi::max_frames_in_flight,
            },
        };
        desc.acceleration_structure_bindings = bindings;
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("rt shadow: the contract's ray-tracing pipeline factory refused the descriptor");
        }
        out.pipeline = pipeline_handle(built);
        out.group_count = static_cast<uint32_t>(groups.size());
        return out;
    }

    // The stochastic punctual lighting trace (source/shaders/megalights_trace.slang): the same compute-pipeline shape as
    // the passes above, with a push block of its own. It FORWARDS to the builder above rather than repeating
    // twenty lines of Vulkan, and it exists as its own name because a caller reading `build_two_set_compute`
    // inside this pass's create() would have to check that the two are still the same shape - which is exactly
    // the kind of coupling a name is for.
    std::expected<compute_pipeline_owned, std::string> build_megalights_trace(rhi::api_core& face,
                                                                              std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(face, compute_shader_code);
    }
    // ... and the chain's temporal resolve: the same shape, with the accumulation's own push block.
    std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(rhi::api_core& face,
                                                                                 std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(face, compute_shader_code);
    }

    // The temporal resolve: its own pipeline, over the images the frame's heap carries. It reads no scene
    // buffer: the push block carries the two projection terms its depth guard needs.
    std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(rhi::api_core& face, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        resolve_pipeline_owned out;
        // THE PIPELINE IS A CONTRACT OBJECT NOW (abi 21): the heap-native compute rules live in the backend
        // (`core::create_compute_pipeline`), which is the side that owns the bind point and the heap flag.
        rhi::pipeline_desc desc{};
        desc.first_stage = rhi::shader_stage::compute;
        desc.compute_code = std::as_bytes(compute_shader_code);
        desc.debug_name = "temporal resolve";
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("temporal resolve: the contract's compute pipeline factory refused the descriptor");
        }
        out.resolve = pipeline_handle(built);
        return out;
    }

    std::expected<deferred_owned, std::string> build_deferred(rhi::api_core& face, std::span<rhi::blend_mode const> const color_blend, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        deferred_owned out;
        rhi::image_format const color_formats = rhi::image_format::r16g16b16a16_sfloat;
        auto pipeline_result = make_graphics_pipeline(face,
                                                      std::span<rhi::image_format const>(&color_formats, 1), rhi::image_format::unknown,
                                                      vertex_shader_code, fragment_shader_code, 1u, false, 0.0f, 0.0f, 0.0f,
                                                      color_blend, rhi::shader_stage::vertex, rhi::depth_compare::less_or_equal,
                                                      "deferred lighting pipeline");
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.lighting = std::move(pipeline_result).value();
        return out;
    }

    std::expected<pipeline_handle, std::string> build_fxaa(rhi::api_core& face, rhi::image_format const swap_chain_format, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_code) {
        using fail = std::unexpected<std::string>;
        auto pipeline_result = make_graphics_pipeline(face,
                                                      std::span<rhi::image_format const>(&swap_chain_format, 1), rhi::image_format::unknown,
                                                      vertex_shader_code, fragment_code, 1u, false, 0.0f, 0.0f, 0.0f,
                                                      {}, rhi::shader_stage::vertex, rhi::depth_compare::less_or_equal,
                                                      "fxaa pipeline");
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        return std::move(pipeline_result).value();
    }

    std::expected<pipeline_handle, std::string> build_shadow(rhi::api_core& face, rhi::image_format const depth_format, float const depth_bias_constant_factor,
                                                             float const depth_bias_slope_factor, float const depth_bias_clamp, std::span<uint8_t const> const vertex_shader_code,
                                                             std::span<uint8_t const> const fragment_shader_code,
                                                             // THE STAGE THAT EMITS THE GEOMETRY: VERTEX for the input-assembler path, MESH
                                                             // for the one that fetches its own vertices (docs/mesh_shaders.md step 1 - the
                                                             // fragment stage is the SAME shader either way, which is what makes the two
                                                             // paths comparable at all).
                                                             rhi::shader_stage first_stage) {
        using fail = std::unexpected<std::string>;
        // No color attachment, depth test + write, single-sampled, and the slope-scaled bias the shadow pass needs
        // (it removes acne on surfaces angled away from the light, in units of depth per depth-unit of slope - the
        // numbers are the pass's and the caller's, not this builder's).
        auto result = make_graphics_pipeline(face,
                                             {}, // no color attachment: depth-only
                                             depth_format,
                                             vertex_shader_code,
                                             fragment_shader_code,
                                             1u,
                                             true, // depth test + write (the write is dynamic state)
                                             depth_bias_constant_factor,
                                             depth_bias_slope_factor,
                                             depth_bias_clamp,
                                             {},
                                             first_stage,
                                             rhi::depth_compare::less_or_equal,
                                             "shadow depth pipeline");
        if (!result) {
            return fail(std::string(result.error()));
        }
        return std::move(result).value();
    }
    std::expected<fxaa_owned, std::string> build_fxaa_owned(rhi::api_core& face, rhi::image_format const swap_chain_format,
                                                            std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        fxaa_owned out;

        // The anti-aliasing pipeline renders into the SWAPCHAIN, so its declared colour format is the surface's.
        auto pipeline_result = make_graphics_pipeline(face,
                                                      std::span<rhi::image_format const>(&swap_chain_format, 1), rhi::image_format::unknown,
                                                      vertex_shader_code, fragment_shader_code, 1u, false, 0.0f, 0.0f, 0.0f,
                                                      {}, rhi::shader_stage::vertex, rhi::depth_compare::less_or_equal,
                                                      "fxaa pipeline (owned)");
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.antialias = std::move(pipeline_result).value();
        return out;
    }

    std::expected<upscale_owned, std::string> build_upscale_owned(rhi::api_core& face, rhi::image_format const swap_chain_format,
                                                                  std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        upscale_owned out;

        // The resolve pipeline renders into the SWAPCHAIN, so its declared colour format is the surface's - and
        // that format is also what decides the shader's `encode_gamma` lane (see upscale_pass::record). The
        // single-target convenience form's blend state is the engine's standard src-alpha one, which the
        // fragment shader's alpha of 1.0 reduces to a copy: the same arrangement the composite's and FXAA's
        // swapchain writes already have, and the reason no blend state is spelled out here.
        auto pipeline_result = make_graphics_pipeline(face,
                                                      std::span<rhi::image_format const>(&swap_chain_format, 1), rhi::image_format::unknown,
                                                      vertex_shader_code, fragment_shader_code, 1u, false, 0.0f, 0.0f, 0.0f,
                                                      {}, rhi::shader_stage::vertex, rhi::depth_compare::less_or_equal,
                                                      "upscale pipeline");
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.resolve = std::move(pipeline_result).value();
        return out;
    }
    void release_contract_pipeline(rhi::pipeline* resource) noexcept {
        resource->release();
    }

    pipeline_handle::pipeline_handle() noexcept
        : contract(nullptr) {
    }

    pipeline_handle::pipeline_handle(rhi::pipeline* owned) noexcept
        : contract(owned) {
    }

    pipeline_handle::pipeline_handle(pipeline_handle&& other) noexcept
        : contract(other.contract)
        , viewport(other.viewport)
        , scissor(other.scissor) {
        other.contract = nullptr;
    }

    pipeline_handle& pipeline_handle::operator=(pipeline_handle&& other) noexcept {
        if (this != &other) {
            this->release_owned();
            this->contract = other.contract;
            this->viewport = other.viewport;
            this->scissor = other.scissor;
            other.contract = nullptr;
        }
        return *this;
    }

    pipeline_handle::~pipeline_handle() noexcept {
        this->release_owned();
    }

    void pipeline_handle::release_owned() noexcept {
        // A CONTRACT PIPELINE'S DESTRUCTION IS ITS release() INSIDE THE BACKEND (abi 21), never a direct
        // vkDestroyPipeline from the engine: the raw branch that used to stand here had exactly one user - the
        // heap probe's graphics builder - and that builder goes through `create_pipeline` now.
        if (this->contract != nullptr) {
            release_contract_pipeline(this->contract);
        }
        this->contract = nullptr;
    }

} // namespace deren::engine::pipelines
