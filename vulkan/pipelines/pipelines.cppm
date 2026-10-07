// module version: 0.20.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pipelines/pipelines.cppm
 * @defgroup vulkan_pipelines Per-Pass Pipeline Builders
 * @brief The engine's per-pass pipelines: one builder per pass, next to the generic builder in
 *        deren.vulkan.core.pipeline (that one knows HOW to build a pipeline, this one knows what each pass's
 *        pipeline looks like - formats, sample counts, blend state, push-constant ranges).
 *
 * Extracted from deren.vulkan.runtime, whose implementation had grown past 4900 lines. The builders are
 * stateless, and EVERY ONE OF THEM NOW TAKES A `VkDevice` rather than the whole core: a device is what a
 * caller that owns one has (a pass's create step gets exactly that, see vulkan.pass::pass_context), and the
 * two builders that also need the surface's format - the composite and FXAA, which write the swapchain image -
 * are handed it as a parameter. That is the whole of what they used to reach into the core for. Ownership stays
 * with whoever asked for the build (the runtime today, a pass once its three-piece has moved).
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
#include <vulkan/vulkan.h>

export module deren.vulkan.pipelines;

import deren.promise.rhi;
import deren.vulkan.render_resource;

namespace deren::vulkan::pipelines {

    namespace rhi = deren::promise::rhi;

    /// AN ENGINE-OWNED SHADER MODULE (abi 8): the raw `VkShaderModule` the compute and ray-tracing
    /// pipeline assemblies consume, created with `vkCreateShaderModule` on the escape's device and
    /// destroyed HERE - the engine side owns it whole, which is why no backend symbol is involved.
    /// (The CONTRACT's shader objects - `create_shader` - are the ownership face; these are the raw
    /// recording face, until the pipeline descs grow the compute/ray-tracing shapes.)
    /// THE RELEASES RUN OUT-OF-LINE, here in the module that owns the handles: a virtual release()
    /// call textually inside an importer's destructor trips a clang 22 codegen crash (EmitBuiltin
    /// NewDeleteCall under EmitDeferred, measured this session), and keeping the call here also
    /// keeps the importer's generated code free of it.
    export void release_contract_shader(rhi::shader* resource) noexcept;
    export void release_contract_pipeline(rhi::pipeline* resource) noexcept;

    export struct shader_module_handle {
        /// SET WHEN THE MODULE IS A CONTRACT OBJECT: the release runs inside the backend and the raw
        /// destroy below must NOT also run (that would be a double destroy). Null for a module the
        /// engine created raw itself.
        rhi::shader* contract = nullptr;
        VkShaderModule module = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;

        shader_module_handle() = default;
        shader_module_handle(rhi::shader* owned, VkShaderModule shader, VkDevice dev) noexcept
            : contract(owned)
            , module(shader)
            , device(dev) {
        }
        shader_module_handle(VkShaderModule shader, VkDevice dev) noexcept
            : module(shader)
            , device(dev) {
        }
        shader_module_handle(shader_module_handle&& other) noexcept
            : contract(other.contract)
            , module(other.module)
            , device(other.device) {
            other.contract = nullptr;
            other.module = VK_NULL_HANDLE;
            other.device = VK_NULL_HANDLE;
        }
        shader_module_handle& operator=(shader_module_handle&& other) noexcept {
            if (this != &other) {
                this->destroy();
                this->contract = other.contract;
                this->module = other.module;
                this->device = other.device;
                other.contract = nullptr;
                other.module = VK_NULL_HANDLE;
                other.device = VK_NULL_HANDLE;
            }
            return *this;
        }
        shader_module_handle(shader_module_handle const&) = delete;
        shader_module_handle& operator=(shader_module_handle const&) = delete;
        ~shader_module_handle() noexcept {
            this->destroy();
        }
        [[nodiscard]] VkShaderModule get() const noexcept {
            return this->module;
        }

    private:
        void destroy() noexcept {
            if (this->contract != nullptr) {
                release_contract_shader(this->contract); // the backend owns the VkShaderModule here
            } else if (this->module != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
                vkDestroyShaderModule(this->device, this->module, nullptr);
            }
            this->contract = nullptr;
            this->module = VK_NULL_HANDLE;
        }
    };

    /// AN ENGINE-HELD PIPELINE (abi 8): either a CONTRACT pipeline (the graphics recipes, created
    /// through `create_pipeline`, released inside the backend when this dies) or a RAW one (the
    /// compute and ray-tracing assemblies, created and destroyed here through the escape's device).
    /// The pass-visible interface is the one `vk_pipeline` exposed to them: `get_pipeline()`.
    export struct pipeline_handle {
        /// SET WHEN THE PIPELINE IS A CONTRACT OBJECT (the graphics recipes): its release() runs
        /// inside the backend. Null for a pipeline the engine created raw (compute/ray-tracing).
        rhi::pipeline* contract = nullptr;
        VkPipeline native = VK_NULL_HANDLE;
        VkDevice raw_device = VK_NULL_HANDLE; // set only for the raw-created ones
        /// the per-pipeline dynamic state `vk_pipeline` carried: the runner sets these and
        /// `begin_pipeline` re-emits them per draw
        VkViewport viewport = {};
        VkRect2D scissor = {};

        // EVERY SPECIAL MEMBER IS OUT-OF-LINE, in this module: an importer TU that generated the
        // destructor's body itself crashed clang 22's codegen (EmitBuiltinNewDeleteCall under
        // EmitDeferred, measured this session) - with the bodies defined HERE the importer only
        // calls them, which is both the workaround and the better shape for a module type.
        pipeline_handle() noexcept;
        pipeline_handle(rhi::pipeline* owned, VkPipeline raw) noexcept;
        pipeline_handle(VkPipeline raw, VkDevice device) noexcept;
        pipeline_handle(pipeline_handle&& other) noexcept;
        pipeline_handle& operator=(pipeline_handle&& other) noexcept;
        pipeline_handle(pipeline_handle const&) = delete;
        pipeline_handle& operator=(pipeline_handle const&) = delete;
        ~pipeline_handle() noexcept;
        [[nodiscard]] VkPipeline get_pipeline() const noexcept {
            return this->native;
        }
        /// bind + re-emit the stored dynamic state - `vk_pipeline::begin_pipeline`'s exact behavior
        void begin_pipeline(VkCommandBuffer command_buffer) const noexcept {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->native);
            vkCmdSetViewport(command_buffer, 0u, 1u, &this->viewport);
            vkCmdSetScissor(command_buffer, 0u, 1u, &this->scissor);
        }

    private:
        void destroy_raw() noexcept;
    };

    /// THE CONTRACT'S SHADER FACTORY, in the shape the raw assembly sites need: the module handle is
    /// engine-owned from here on.
    export [[nodiscard]] std::expected<shader_module_handle, std::string> make_shader_module_raw(rhi::api_core& face, std::span<uint8_t const> const code, rhi::shader_stage const stage, char const* const what) {
        rhi::shader_desc desc{};
        desc.stage = stage;
        desc.code = std::as_bytes(std::span(code));
        desc.debug_name = what;
        rhi::shader* const built = face.create_shader(desc);
        if (built == nullptr) {
            return std::unexpected(std::string(what) + ": the contract's shader factory refused the module");
        }
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        return shader_module_handle(built, static_cast<VkShaderModule>(natives.native_shader_module(*built)), static_cast<VkDevice>(natives.native_device()));
    }

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
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        return pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
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

    export std::expected<post_owned, std::string> build_post(rhi::api_core& face, VkDevice device, rhi::image_format swap_chain_format,
                                                             std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<gbuffer_owned, std::string> build_gbuffer_debug(rhi::api_core& face, VkDevice device, rhi::image_format color_format, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<taa_owned, std::string> build_taa(rhi::api_core& face, VkDevice device, rhi::image_format color_format, std::span<uint8_t const> vertex_shader_code,
                                                           std::span<uint8_t const> fragment_shader_code);

    /// the ray-traced sun shadow: a compute pipeline over the descriptors the frame's heap carries
    export std::expected<compute_pipeline_owned, std::string> build_two_set_compute(rhi::api_core& face, VkDevice device, std::span<uint8_t const> compute_shader_code);
    /// the stochastic punctual lighting trace (shaders/megalights_trace.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_trace(rhi::api_core& face, VkDevice device,
                                                                                     std::span<uint8_t const> compute_shader_code);
    /// the stochastic chain's temporal resolve (shaders/megalights_temporal.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(rhi::api_core& face, VkDevice device,
                                                                                        std::span<uint8_t const> compute_shader_code);
    /// the mask bake: a compute pass over the material table and the texture array
    /// array), which collapses the triangles a material's alphaMode MASK cuts out and writes the expanded
    /// vertices a bottom level structure is then built from - see shaders/mask_bake.slang
    export std::expected<compute_pipeline_owned, std::string> build_mask_bake(rhi::api_core& face, VkDevice device, std::span<uint8_t const> compute_shader_code);
    /// the compute skinning pass: the scene block's per-joint matrices - see
    /// shaders/compute_skin.slang
    export std::expected<compute_pipeline_owned, std::string> build_compute_skin(rhi::api_core& face, VkDevice device, std::span<uint8_t const> compute_shader_code);
    /// the clustered-light sort (shaders/light_cluster.slang): heap-native, and NO push constants
    /// at all - the shader reads the light UBO and writes the two cluster buffers through heap slots, which is
    /// why this builder takes no push size. It is the first compute pipeline in this module
    /// that came out of `deren.vulkan.core`.
    export std::expected<compute_pipeline_owned, std::string> build_cluster(rhi::api_core& face, VkDevice device, std::span<uint8_t const> compute_shader_code);

    /**
     * @brief the HEAP-NATIVE probe's pipeline: the first one in this renderer created the heap way
     * @param device the logical device
     * @param compute_shader_code the probe's SPIR-V (see shaders/heap_probe_comp.slang)
     * @return the pipeline, or the reason it could not be created
     * @note NO SET LAYOUT AND NO PIPELINE LAYOUT, which is not a simplification but the flag's requirement:
     *       "the pipeline layout must be NULL and shader resources will be sourced from a descriptor heap". The
     *       probe's parameters therefore reach the shader through vkCmdPushDataEXT (see descriptor_heap::push_data)
     *       and not through vkCmdPushConstants, which needs a layout to push to.
     */
    export std::expected<compute_pipeline_owned, std::string> build_heap_probe(rhi::api_core& face, VkDevice device, std::span<uint8_t const> compute_shader_code);

    /// the probe's target: one size for the image, the viewport, the scissor and the readback, so a mismatch
    /// between them is impossible rather than merely unlikely
    export inline constexpr uint32_t heap_probe_extent = 4u;

    /**
     * @brief the GRAPHICS half of the heap-native probe: a heap-flagged, layout-less pipeline over two stages
     * @param device the logical device
     * @param colour_format the format the probe renders into (dynamic rendering, like every pass here)
     * @param vertex_code / @param fragment_code the probe's SPIR-V (see shaders/heap_probe.slang)
     * @return the pipeline, or the reason it could not be created
     * @note no vertex input, no blend and a static viewport: the probe's subject is the FRAGMENT stage reading the
     *       heap through a graphics pipeline at all, and every one of those would be a second thing that could be
     *       wrong. The flag and the null layout are the rule the compute probe established.
     */
    /// @param first_stage the stage that emits the geometry: VERTEX for the original probe, MESH for the
    ///        mesh-shader mechanism proof (docs/mesh_shaders.md step 0). Everything else - the empty vertex
    ///        input, the heap flag, the NULL layout, the fragment stage - is identical between the two.
    export std::expected<pipeline_handle, std::string> build_heap_probe_graphics(rhi::api_core& face, VkDevice device, rhi::image_format colour_format, std::span<uint8_t const> vertex_code, std::span<uint8_t const> fragment_code, rhi::shader_stage first_stage = rhi::shader_stage::vertex);

    /// what build_resolve_pipeline() creates: the resolve pipeline
    export struct resolve_pipeline_owned {
        std::optional<pipeline_handle> resolve;
    };

    export std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(rhi::api_core& face, VkDevice device,
                                                                                     std::span<uint8_t const> compute_shader_code);

    export std::expected<pipeline_handle, std::string> build_fxaa(rhi::api_core& face, VkDevice device, rhi::image_format swap_chain_format, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
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
    export std::expected<pipeline_handle, std::string> build_shadow(rhi::api_core& face, VkDevice device, rhi::image_format depth_format, float depth_bias_constant_factor, float depth_bias_slope_factor,
                                                                    float depth_bias_clamp, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code,
                                                                    rhi::shader_stage first_stage = rhi::shader_stage::vertex);
    /// @brief what the FXAA pass's own create step needs: the anti-aliasing pipeline
    export struct fxaa_owned {
        std::optional<pipeline_handle> antialias;
    };
    export std::expected<fxaa_owned, std::string> build_fxaa_owned(rhi::api_core& face, VkDevice device, rhi::image_format swap_chain_format,
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
    export std::expected<upscale_owned, std::string> build_upscale_owned(rhi::api_core& face, VkDevice device, rhi::image_format swap_chain_format,
                                                                         std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export struct deferred_owned {
        std::optional<pipeline_handle> lighting;
    };

    /// the additive blend state comes in as a parameter: the helper that builds it is a local of the
    /// runtime, next to the passes whose blend modes it describes
    export std::expected<deferred_owned, std::string> build_deferred(rhi::api_core& face, [[maybe_unused]] VkDevice device, std::span<rhi::blend_mode const> color_blend, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    // post: the composite chain's owner. The two fullscreen pipelines (one per color format the chain renders
    // into) are created here.
    std::expected<post_owned, std::string> build_post(rhi::api_core& face, [[maybe_unused]] VkDevice const device, rhi::image_format const swap_chain_format,
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

    std::expected<gbuffer_owned, std::string> build_gbuffer_debug(rhi::api_core& face, [[maybe_unused]] VkDevice const device, rhi::image_format const color_format,
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
    std::expected<taa_owned, std::string> build_taa(rhi::api_core& face, [[maybe_unused]] VkDevice const device, rhi::image_format const color_format, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
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

    // The mask bake (see shaders/mask_bake.slang): a compute pipeline over the material heap slots ALONE, because
    // everything it needs is there - the material table for the alpha texture's index and the cutoff, and the
    // bindless texture array to sample it. It owns no set layout, like every traced compute pass, and it is the only compute
    // pass here whose output is not an image: it writes vertices into a buffer the acceleration structure is
    // then built from.
    std::expected<compute_pipeline_owned, std::string> build_mask_bake(rhi::api_core& face, [[maybe_unused]] VkDevice const /*device*/, std::span<uint8_t const> const compute_shader_code) {
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
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        out.trace = pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
        return out;
    }

    // The compute skinning pass (see shaders/compute_skin.slang): the same shape as the mask bake above and
    // for the same reason - it reads only the per-joint matrices heap slot.
    std::expected<compute_pipeline_owned, std::string> build_compute_skin(rhi::api_core& face, [[maybe_unused]] VkDevice const /*device*/, std::span<uint8_t const> const compute_shader_code) {
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
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        out.trace = pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
        return out;
    }

    // The clustered-light sort: NO set, NO layout and NO push range. Every stage of the frame is heap-native
    // (see docs/descriptor_heap_handover.md), so this pipeline is created with VK_NULL_HANDLE and the heap flag;
    // the shader reads the light UBO and the cluster buffers out of the scene block by slot, and the slot itself
    // travels in the stage push block (shaders/heap_slots.glsl). Owning the pipeline is all that is left to own.
    std::expected<compute_pipeline_owned, std::string> build_cluster(rhi::api_core& face, [[maybe_unused]] VkDevice const /*device*/, std::span<uint8_t const> const compute_shader_code) {
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
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        out.trace = pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
        return out;
    }

    std::expected<compute_pipeline_owned, std::string> build_heap_probe(rhi::api_core& face, [[maybe_unused]] VkDevice const /*device*/, std::span<uint8_t const> const compute_shader_code) {
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
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        out.trace = pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
        return out;
    }

    // The SHARED builder every traced pass uses (its name still says `two_set`, from the two sets it used to
    // declare): the shape several traced passes have in common - the camera block and the light UBO in the scene
    // block (plus the top level structure at binding 16 when the device has ray tracing), the stored surface in
    // the G-buffer images - so the caller's only variable is the push block size.
    // (Its old name, build_rt_shadow, is gone with the ray-query shadow pass: the shadow traces through a real
    // ray-tracing PIPELINE now, which is a different builder below.)
    std::expected<pipeline_handle, std::string> build_heap_probe_graphics(rhi::api_core& face, VkDevice const device, rhi::image_format const colour_format, std::span<uint8_t const> const vertex_code, std::span<uint8_t const> const fragment_code, rhi::shader_stage const first_stage) {
        using fail = std::unexpected<std::string>;
        auto const vertex_module = make_shader_module_raw(face, vertex_code, first_stage, "heap probe (graphics) first stage");
        if (!vertex_module.has_value()) {
            return fail("heap probe (graphics): the first shader module's creation failed");
        }
        auto const fragment_module = make_shader_module_raw(face, fragment_code, rhi::shader_stage::fragment, "heap probe (graphics) fragment");
        if (!fragment_module.has_value()) {
            return fail("heap probe (graphics): fragment shader module creation failed");
        }
        std::array<VkPipelineShaderStageCreateInfo, 2> stages = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        // THE FIRST STAGE IS A PARAMETER, and that is the whole difference between the vertex probe and the
        // MESH probe (docs/mesh_shaders.md step 0): a mesh pipeline substitutes VK_SHADER_STAGE_MESH_BIT_EXT
        // here, keeps the same fragment stage, and ignores the (empty) vertex input state - so the same 4x4
        // target and the same readback compare the two paths directly.
        stages[0].stage = first_stage == rhi::shader_stage::mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT;
        // the probe's rendering create info needs the CONCRETE format; its colour is one of the named
        // 8-bit shapes the contract spells, so the back-mapping is this one switch
        VkFormat const native_colour = colour_format == rhi::image_format::bgra8_unorm  ? VK_FORMAT_B8G8R8A8_UNORM
                                       : colour_format == rhi::image_format::bgra8_srgb ? VK_FORMAT_B8G8R8A8_SRGB
                                       : colour_format == rhi::image_format::rgba8_srgb ? VK_FORMAT_R8G8B8A8_SRGB
                                                                                        : VK_FORMAT_R8G8B8A8_UNORM;
        stages[0].module = vertex_module->get();
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module->get();
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo const vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                                   .pNext = nullptr,
                                                                   .flags = 0,
                                                                   .vertexBindingDescriptionCount = 0,
                                                                   .pVertexBindingDescriptions = nullptr,
                                                                   .vertexAttributeDescriptionCount = 0,
                                                                   .pVertexAttributeDescriptions = nullptr};
        VkPipelineInputAssemblyStateCreateInfo const input_assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                                       .pNext = nullptr,
                                                                       .flags = 0,
                                                                       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                                       .primitiveRestartEnable = VK_FALSE};
        VkViewport const viewport = {.x = 0.0f, .y = 0.0f, .width = static_cast<float>(heap_probe_extent), .height = static_cast<float>(heap_probe_extent), .minDepth = 0.0f, .maxDepth = 1.0f};
        VkRect2D const scissor = {.offset = {0, 0}, .extent = {heap_probe_extent, heap_probe_extent}};
        VkPipelineViewportStateCreateInfo const viewport_state = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                                  .pNext = nullptr,
                                                                  .flags = 0,
                                                                  .viewportCount = 1,
                                                                  .pViewports = &viewport,
                                                                  .scissorCount = 1,
                                                                  .pScissors = &scissor};
        VkPipelineRasterizationStateCreateInfo const rasterization = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                                      .pNext = nullptr,
                                                                      .flags = 0,
                                                                      .depthClampEnable = VK_FALSE,
                                                                      .rasterizerDiscardEnable = VK_FALSE,
                                                                      .polygonMode = VK_POLYGON_MODE_FILL,
                                                                      .cullMode = VK_CULL_MODE_NONE,
                                                                      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                                      .depthBiasEnable = VK_FALSE,
                                                                      .depthBiasConstantFactor = 0.0f,
                                                                      .depthBiasClamp = 0.0f,
                                                                      .depthBiasSlopeFactor = 0.0f,
                                                                      .lineWidth = 1.0f};
        VkPipelineMultisampleStateCreateInfo const multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                                  .pNext = nullptr,
                                                                  .flags = 0,
                                                                  .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                                                                  .sampleShadingEnable = VK_FALSE,
                                                                  .minSampleShading = 1.0f,
                                                                  .pSampleMask = nullptr,
                                                                  .alphaToCoverageEnable = VK_FALSE,
                                                                  .alphaToOneEnable = VK_FALSE};
        VkPipelineColorBlendAttachmentState const blend_attachment = {.blendEnable = VK_FALSE,
                                                                      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                      .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                      .colorBlendOp = VK_BLEND_OP_ADD,
                                                                      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                      .alphaBlendOp = VK_BLEND_OP_ADD,
                                                                      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
        VkPipelineColorBlendStateCreateInfo const colour_blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                                  .pNext = nullptr,
                                                                  .flags = 0,
                                                                  .logicOpEnable = VK_FALSE,
                                                                  .logicOp = VK_LOGIC_OP_COPY,
                                                                  .attachmentCount = 1,
                                                                  .pAttachments = &blend_attachment,
                                                                  .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f}};
        VkPipelineRenderingCreateInfo rendering = {};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &native_colour;
        // The heap flag is a flags2 bit and the rendering struct hangs off it, so both travel in one pNext chain.
        VkPipelineCreateFlags2CreateInfo flags = {};
        flags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        flags.pNext = &rendering;
        flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkGraphicsPipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &flags;
        pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &colour_blend;
        pipeline_info.layout = VK_NULL_HANDLE; // required by the flag, exactly as for the compute probe

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("heap probe (graphics): vkCreateGraphicsPipelines failed");
        }
        return pipeline_handle(pipeline, device);
    }

    std::expected<compute_pipeline_owned, std::string> build_two_set_compute(rhi::api_core& face, [[maybe_unused]] VkDevice const /*device*/, std::span<uint8_t const> const compute_shader_code) {
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
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        out.trace = pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
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
    export std::expected<ray_tracing_pipeline_owned, std::string> build_rt_shadow_ray_tracing(rhi::api_core& face, [[maybe_unused]] VkDevice const /*device*/,
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
        rhi::pipeline* const built = face.create_pipeline(desc);
        if (built == nullptr) {
            return fail("rt shadow: the contract's ray-tracing pipeline factory refused the descriptor");
        }
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        out.pipeline = pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
        out.group_count = static_cast<uint32_t>(groups.size());
        return out;
    }

    // The stochastic punctual lighting trace (shaders/megalights_trace.slang): the same compute-pipeline shape as
    // the passes above, with a push block of its own. It FORWARDS to the builder above rather than repeating
    // twenty lines of Vulkan, and it exists as its own name because a caller reading `build_two_set_compute`
    // inside this pass's create() would have to check that the two are still the same shape - which is exactly
    // the kind of coupling a name is for.
    std::expected<compute_pipeline_owned, std::string> build_megalights_trace(rhi::api_core& face, VkDevice device,
                                                                              std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(face, device, compute_shader_code);
    }
    // ... and the chain's temporal resolve: the same shape, with the accumulation's own push block.
    std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(rhi::api_core& face, VkDevice device,
                                                                                 std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(face, device, compute_shader_code);
    }

    // The temporal resolve: its own pipeline, over the images the frame's heap carries. It reads no scene
    // buffer: the push block carries the two projection terms its depth guard needs.
    std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(rhi::api_core& face, [[maybe_unused]] VkDevice const /*device*/, std::span<uint8_t const> const compute_shader_code) {
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
        auto& natives = *static_cast<rhi::vulkan_escape*>(face.query_extension(rhi::extension_kind::vulkan_escape));
        out.resolve = pipeline_handle(built, static_cast<VkPipeline>(natives.native_pipeline(*built)));
        return out;
    }

    std::expected<deferred_owned, std::string> build_deferred(rhi::api_core& face, [[maybe_unused]] VkDevice device, std::span<rhi::blend_mode const> const color_blend, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
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

    std::expected<pipeline_handle, std::string> build_fxaa(rhi::api_core& face, [[maybe_unused]] VkDevice device, rhi::image_format const swap_chain_format, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_code) {
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

    std::expected<pipeline_handle, std::string> build_shadow(rhi::api_core& face, [[maybe_unused]] VkDevice const device, rhi::image_format const depth_format, float const depth_bias_constant_factor,
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
    std::expected<fxaa_owned, std::string> build_fxaa_owned(rhi::api_core& face, [[maybe_unused]] VkDevice const device, rhi::image_format const swap_chain_format,
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

    std::expected<upscale_owned, std::string> build_upscale_owned(rhi::api_core& face, [[maybe_unused]] VkDevice const device, rhi::image_format const swap_chain_format,
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
    void release_contract_shader(rhi::shader* resource) noexcept {
        resource->release();
    }

    void release_contract_pipeline(rhi::pipeline* resource) noexcept {
        resource->release();
    }

    pipeline_handle::pipeline_handle() noexcept
        : contract(nullptr)
        , native(VK_NULL_HANDLE)
        , raw_device(VK_NULL_HANDLE) {
    }

    pipeline_handle::pipeline_handle(rhi::pipeline* owned, VkPipeline raw) noexcept
        : contract(owned)
        , native(raw)
        , raw_device(VK_NULL_HANDLE) {
    }

    pipeline_handle::pipeline_handle(VkPipeline raw, VkDevice device) noexcept
        : contract(nullptr)
        , native(raw)
        , raw_device(device) {
    }

    pipeline_handle::pipeline_handle(pipeline_handle&& other) noexcept
        : contract(other.contract)
        , native(other.native)
        , raw_device(other.raw_device)
        , viewport(other.viewport)
        , scissor(other.scissor) {
        other.contract = nullptr;
        other.native = VK_NULL_HANDLE;
        other.raw_device = VK_NULL_HANDLE;
    }

    pipeline_handle& pipeline_handle::operator=(pipeline_handle&& other) noexcept {
        if (this != &other) {
            this->destroy_raw();
            this->contract = other.contract;
            this->native = other.native;
            this->raw_device = other.raw_device;
            this->viewport = other.viewport;
            this->scissor = other.scissor;
            other.contract = nullptr;
            other.native = VK_NULL_HANDLE;
            other.raw_device = VK_NULL_HANDLE;
        }
        return *this;
    }

    pipeline_handle::~pipeline_handle() noexcept {
        this->destroy_raw();
    }

    void pipeline_handle::destroy_raw() noexcept {
        // ONLY the raw-created pipelines die here: a contract pipeline's destruction is its
        // release() inside the backend, never a direct vkDestroyPipeline from the engine.
        if (this->contract != nullptr) {
            release_contract_pipeline(this->contract);
        } else if (this->native != VK_NULL_HANDLE && this->raw_device != VK_NULL_HANDLE) {
            vkDestroyPipeline(this->raw_device, this->native, nullptr);
        }
        this->contract = nullptr;
        this->native = VK_NULL_HANDLE;
    }

} // namespace deren::vulkan::pipelines
