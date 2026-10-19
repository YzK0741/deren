// The FXAA pass's implementation: the LDR image's transition to a sampled layout, the clear instance over the
// swapchain, the one-set bind, the 52-byte push block with mode 3 in it, the fullscreen draw and the overlay
// inside the same instance. Moved out of `runtime::record_fxaa` (which was the second half of
// `record_composite`) UNCHANGED in behaviour - the same two barriers in the same order, the same attachment, the
// same push lanes and the same draw - so the capture gate decides the move on `deferred_taa_fxaa`, the one
// scenario that runs with FXAA on.

module;

#include <cstdint>
#include <cstring>
#include <span>

module deren.engine.pass.fxaa;
import deren.utility.shared;

// The record series (abi 20): the two barriers, the rendering scope, the cull mode and the draw now
// go through the contract, so this file names no `vkCmd*` at all (plan §1 step 4, one pass per commit).
import deren.promise.rhi;
import deren.engine.pipelines; // build_fxaa_owned: the pass's own pipeline, built from its two shaders and the surface's format
import deren.utility;

// The contract's spelling, local to this TU (post.cpp carries the same alias): the record series names
// deren::promise::rhi types at every call site below.
namespace rhi = deren::promise::rhi;

namespace deren::engine::pass {

    fxaa_pass::~fxaa_pass() {
        this->release_owned();
    }

    void fxaa_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& fxaa_pass::io() const noexcept {
        return render_resource::fxaa_io;
    }

    deren::engine::pass::behaviour const& fxaa_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view fxaa_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.fxaa` = "the knob is on AND the pipeline exists", which is one
        // definition (`runtime::post_fxaa_active`) shared with the composite's target choice and the overlay's
        // owner - so the record step's gate, the frame's target and the overlay cannot disagree about whether this pass
        // runs.
        return "fxaa";
    }

    void fxaa_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_owned();
        }
        this->built_against = context.face;
        if (this->pass_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<uint8_t const> const vertex_spirv = context.shader != nullptr ? context.shader(context.owner, vertex_shader_name) : std::span<uint8_t const>{};
        std::span<uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<uint8_t const>{};
        if (vertex_spirv.empty() || fragment_spirv.empty()) {
            deren::utility::log("fxaa disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // The surface's format is the pipeline's declared colour format (the filter writes the swapchain).
        auto built = pipelines::build_fxaa_owned(*context.face, context.swap_chain_format, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("fxaa disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->antialias);
        this->swap_chain_format = context.swap_chain_image_format;
        deren::utility::log("SUCCESS: fxaa pipeline created (LDR -> anti-aliased swapchain)");
    }

    void fxaa_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the surface's FORMAT (a session-stable device fact) and not on
        // its size, and the set this pass binds belongs to the post family, whose owner retires it.
    }

    bool fxaa_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* fxaa_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    void fxaa_pass::set_frame(fxaa_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void fxaa_pass::set_overlay(draw_callback const overlay) noexcept {
        // The host's hook, installed once. This pass needs no fact to decide whether to use it: whenever FXAA
        // resolves, THIS is the frame's last writer (the composite's frame is what needs the answer).
        this->overlay_callback = overlay;
    }

    void fxaa_pass::prepare_frame([[maybe_unused]] frame_facts const& facts) noexcept {
        this->set_frame(fxaa_frame{.after_draw = this->overlay_callback});
    }

    rhi::error fxaa_pass::record_content(resolved_io const& io) noexcept {
        if (!this->pipeline_ready() || io.targets.empty() || io.pipelines.empty() || !io.pipelines[0] ||
            !io.cmd || !io.list || io.cmd.get() != io.list || !io.targets[0].image_handle || !io.targets[0].view_handle ||
            io.extent.width == 0 || io.extent.height == 0) {
            return rhi::error::invalid_argument;
        }
        // The push block is composed HERE (S3): the frame's settings (exposure, the bloom weight/threshold and
        // this pass's two thresholds - all of them lanes the composite pushes too, which is why they are frame
        // settings rather than one pass's parameters), the struct's defaults for the lanes this mode does not
        // read, the `encode_gamma` lane (this pass always writes the swapchain, so the answer is the surface's
        // own format), and the pass's own stage lane - FXAA is mode 3.
        render_settings const& settings = io.constants.settings;
        post_push_constants push = {
            .exposure = settings.exposure,
            .bloom_intensity = settings.bloom_intensity,
            .bloom_threshold = settings.bloom_threshold,
            .mode = 3.0f, // the pass's own stage lane: FXAA
            // Same meaning as in the composite: 0 = the swapchain attachment encodes to display values in
            // hardware, so FXAA must hand it LINEAR values; 1 = the target is a UNORM format and FXAA's own
            // display-encoded result is what should be stored.
            .encode_gamma = is_srgb_swapchain_format(this->swap_chain_format) ? 0.0f : 1.0f,
            .fxaa_subpixel = settings.fxaa_subpixel,
            .fxaa_edge_threshold = settings.fxaa_edge_threshold,
        };
        // THE RENDERING SCOPE RIDES THE CONTRACT (abi 20): one colour attachment, CLEAR + STORE (what the
        // raw helper spelled), zero clear colour, no depth - the whole scope this pass opens.
        deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
            .layer_count = 1,
            .colors = colors,
            .depth = {},
            .has_depth = false,
            .secondary_contents = false, // this instance is inline; the overlay segment runs in its own after it ends
        };
        auto const begun = io.list->begin_rendering(rendering_info);
        if (begun != rhi::error::ok) {
            return begun;
        }
        io.list->set_cull_mode(rhi::cull_mode::none); // the synthetic triangle has no facing to cull
        // The post chain's source is a heap slot now (see source/shaders/post/post.slang): the third push lane names it, and
        // the frame bound the heaps for this command buffer, so there is no set to bind here.
        if (!io.push_block(*io.cmd, pass::push_bytes(push))) {
            io.list->end_rendering();
            return rhi::error::operation_failed;
        }
        io.list->draw(3, 1, 0, 0);
        // THE OVERLAY RECORDS ITS OWN SEGMENT HERE (gui abi 5): the third-party recorder is not a contract
        // consumer, so the plugin starts one of its mergeable targets under this instance's inheritance and
        // returns it; the segment is EXECUTED by the host in its own secondary-contents instance after THIS
        // instance ends (see draw_callback::finish - the two cannot share one instance because a
        // CONTENTS_SECONDARY_COMMAND_BUFFERS instance may contain no primary draw).
        if (this->pass_frame.after_draw.valid()) {
            auto const overlay_begin = pass::make_overlay_begin(
                rendering_info, io.targets[0].image_handle != nullptr ? io.targets[0].image_handle->format()
                                                                      : deren::promise::rhi::image_format::unknown);
            this->pass_frame.after_draw.record(this->pass_frame.after_draw.owner, *io.cmd, overlay_begin.begin);
        }
        io.list->end_rendering();
        if (this->pass_frame.after_draw.valid() && this->pass_frame.after_draw.finish != nullptr) {
            return this->pass_frame.after_draw.finish_with_dependency(*io.cmd, *io.targets[0].image_handle, rendering_info);
        }
        return rhi::error::ok;
    }

} // namespace deren::engine::pass
