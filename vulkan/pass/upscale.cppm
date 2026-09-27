// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/upscale.cppm
 * @brief The resolve: the render chain's display-referred LDR image onto the swapchain, by a linear filter.
 * @defgroup vulkan_pass_upscale Upscale Pass
 *
 * WHY IT EXISTS: `core::render_extent()` is the render chain's extent, and below `[render] render_scale = 1.0`
 * the whole chain is created smaller while the swapchain stays at the output size. Nothing resolved that
 * difference, so a scaled frame rendered into the TOP-LEFT QUADRANT of the presented image - the scene was
 * fully shaded, at a lower resolution, and the other three quarters held whatever the allocation had. This
 * pass is that resolve: it reads the LDR image the composite wrote and writes the swapchain, which makes it
 * the frame's LAST writer whenever it runs - and therefore the owner of the debug overlay, exactly as the
 * FXAA pass is when IT is the last writer (the overlay composites a UI with no load op of its own, so it has
 * to be drawn INSIDE whichever instance writes the final image).
 *
 * WHY THE EXTENT IS THE ONE THING THAT MAKES IT WORK, and it is the reason this pass exists as a pass rather
 * than as a line in the composite: its `behaviour` declares `extent_rule::resource` over
 * `resource_id::swapchain_image`, and `runtime::resolve_resource_extent` answers that resource with the
 * OUTPUT extent - the one entry in the table that is not the frame's. So the runner sets this pass's viewport
 * and scissor from the presented image while every other pass in the frame works at the render extent, and
 * the ratio between the two IS the render scale (nothing in this pass has to be told what it is).
 *
 * WHY IT IS NOT FXAA, AND WHY THE TWO CANNOT BOTH RUN: FXAA is an edge filter whose relative luma thresholds
 * are defined on display-referred data AT THE RESOLUTION IT FILTERS, and its input is the same LDR image this
 * pass reads - but it writes the swapchain directly, at the render extent, so on a scaled frame its output
 * would be resolved away by this pass (or, if it ran first, this pass would resample an already-filtered
 * image at the wrong size). Both want the same two things, so `runtime::post_fxaa_active` excludes the frames
 * this pass runs on, and the feature registry, the composite's target choice and the overlay's owner all read
 * that one answer (see `render_features::upscale`).
 *
 * WHAT IT OWNS: its pipeline, the LDR image's transition to a sampled layout, the clear instance over the
 * swapchain, the push block's `encode_gamma` lane and the draw. WHAT IT DOES NOT OWN: the LDR image it reads
 * (a per-swapchain-image heap slot the shader names itself, from the lane the framework appends) and the
 * composite that wrote it - the composite decides to write the LDR image instead of the swapchain from the
 * same `frame_facts` predicate that gates this pass (see post_composite_pass::prepare_frame).
 *
 * THE SHAPE IS FXAA'S (the last-writer template) WITH `toon_screen_rim`'s ONE DIFFERENCE: this pass declares
 * a barrier image, because it is the pass that hands the LDR image to a sampler - the same argument FXAA's
 * declaration makes, and for the same reason (the composite cannot read the image it renders into).
 */

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module vulkan.pass.upscale;

import vulkan.pass;
import vulkan.render_resource;
import vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace vulkan::pass {

    /// @brief what this pass's frame carries: the overlay's draw, and nothing else
    struct upscale_frame {
        /**
         * The debug overlay's draw, recorded INSIDE this pass's rendering instance between its draw and
         * `vkCmdEndRendering`.
         *
         * WHY A CALLBACK AND NOT A PASS (the same decision fxaa_frame records, from the other side): the
         * overlay has no load op, so a pass of its own would CLEAR the image it is supposed to draw over. It
         * belongs to whichever instance is the frame's LAST writer, and this pass is that instance whenever
         * it runs - so the pass installs the host's hook (see `set_overlay`) and always draws it, while the
         * composite's own frame decides per frame that the overlay is NEITHER this pass's nor the FXAA pass's
         * business (see post_composite_pass::prepare_frame).
         */
        draw_callback after_draw = {};
    };

    /**
     * @brief the resolve: the display-referred LDR image -> the swapchain, by a linear (bilinear) filter
     *
     * The frame's last writer and its overlay's owner whenever the render chain runs below the output size.
     */
    class upscale_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also `upscale.slang`'s - the display transfer and nothing else
        /// @note THE TWO HEAP LANES ARE APPENDED BY THE FRAMEWORK and are therefore NOT fields here (see the
        ///       shader's note and `runtime::push_stage_block`): the shader's block is this struct plus
        ///       `uint frame_slot; uint image_index;`, and a field added here for either would make the host
        ///       append its own values PAST them.
        struct push_constants {
            float encode_gamma = 0.0f; // 0 = the swapchain is an sRGB format and encodes on write, 1 = it does not
        };

        upscale_pass() = default;
        ~upscale_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built the pipeline it records with (the renderer gates the feature on this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question, so an owner holding only a chain can ask it
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        /// @brief install the host's overlay hook (this pass is the frame's last writer whenever it runs)
        void set_overlay(draw_callback overlay) noexcept;
        /// @brief build this pass's frame from the published facts (see frame_pass::prepare_frame)
        void prepare_frame(frame_facts const& facts) noexcept override;
        /// @brief the frame for this stage; the pass composes it itself now (see frame_pass::prepare_frame),
        ///        and the setter stays for a test that wants to hand one over directly
        void set_frame(upscale_frame const& frame) noexcept;

    private:
        static constexpr std::string_view vertex_shader_name = "post.vert.spv"; // the synthetic fullscreen triangle
        static constexpr std::string_view fragment_shader_name = "upscale.frag.spv";

        static constexpr std::array<std::string_view, 1> pipeline_names = {"upscale"};
        inline static constexpr vulkan::pass::behaviour behaviour_ = {
            .kind = behaviour_kind::fullscreen,
            // THE ONE PASS IN THE FRAME WHOSE EXTENT IS NOT THE FRAME'S: the resource rule over the swapchain
            // image resolves to the OUTPUT extent (see runtime::resolve_resource_extent), which is what makes
            // the viewport cover the whole presented image while the chain being sampled stays smaller.
            .extent = extent_rule::resource,
            .extent_of = resource_id::swapchain_image,
            .pipelines = pipeline_names,
            .resync_viewport = true, // the runner sets the viewport and scissor from io.extent
        };
        void release_owned() noexcept;

        VkDevice device_ = VK_NULL_HANDLE;
        std::optional<vk_pipeline> pipeline_ = std::nullopt;
        /// the surface's format, cached at create: the push block's `encode_gamma` lane follows from it, and a
        /// session-stable device fact is exactly what a create step may keep (see the FXAA pass, which does the
        /// same for the same lane)
        VkFormat swap_chain_format_ = VK_FORMAT_UNDEFINED;
        /// the host's overlay hook, installed once (see set_overlay): this pass draws it whenever it runs
        draw_callback overlay_ = {};
        upscale_frame frame_ = {};
    };

    /// THE DECLARATION'S NUMBER AND THE PASS'S STRUCT CANNOT DRIFT: the declaration's `push` size is this
    /// struct, and the framework appends the two heap index lanes on top of it - which is the size the shader
    /// declares as its own block.
    static_assert(sizeof(upscale_pass::push_constants) == render_resource::upscale_io.push->size,
                  "the upscale pass's declared push block must be the size of the struct the pass composes");

} // namespace vulkan::pass
