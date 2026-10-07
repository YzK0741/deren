// The FXAA pass's implementation: the LDR image's transition to a sampled layout, the clear instance over the
// swapchain, the one-set bind, the 52-byte push block with mode 3 in it, the fullscreen draw and the overlay
// inside the same instance. Moved out of `runtime::record_fxaa` (which was the second half of
// `record_composite`) UNCHANGED in behaviour - the same two barriers in the same order, the same attachment, the
// same push lanes and the same draw - so the capture gate decides the move on `deferred_taa_fxaa`, the one
// scenario that runs with FXAA on.

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.fxaa;

// The record series (abi 20): the two barriers, the rendering scope, the cull mode and the draw now
// go through the contract, so this file names no `vkCmd*` at all (plan §1 step 4, one pass per commit).
import deren.promise.rhi;
import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_fxaa_owned: the pass's own pipeline, built from its two shaders and the surface's format
import deren.utility;

// The contract's spelling, local to this TU (post.cpp carries the same alias): the record series names
// deren::promise::rhi types at every call site below.
namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    fxaa_pass::~fxaa_pass() {
        this->release_owned();
    }

    void fxaa_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& fxaa_pass::io() const noexcept {
        return render_resource::fxaa_io;
    }

    deren::vulkan::pass::behaviour const& fxaa_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view fxaa_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.fxaa` = "the knob is on AND the pipeline exists", which is one
        // definition (`runtime::post_fxaa_active`) shared with the composite's target choice and the overlay's
        // owner - so the runner's gate, the frame's target and the overlay cannot disagree about whether this pass
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
        auto built = pipelines::build_fxaa_owned(*context.face, context.device, context.swap_chain_format, vertex_spirv, fragment_spirv);
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

    VkPipeline fxaa_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
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

    void fxaa_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == VK_NULL_HANDLE ||
            io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        // THE CONTRACT'S OWN HANDLES (abi 20): the raw `VkImage`/`VkImageView` this pass used to read out of
        // the resolved io are the SAME objects, published beside the raw ones by the pilot's framework change
        // (`resolved_binding::image_handle` / `view_handle`).
        if (io.targets[0].image_handle == nullptr || io.targets[0].view_handle == nullptr) {
            return;
        }
        // THE INPUT FIRST, and it is THIS pass's transition rather than the frame loop's: the LDR image was written
        // by the composite earlier in this same command buffer, so its old layout is known to be a colour attachment
        // and the src masks have to publish that write. It is the declaration's one barrier image, so the handle is
        // the one the pass named - and on a frame this pass does not run, nothing moves the image at all (the
        // composite writes it as an attachment and the next frame writes it again).
        // THE PAIR RIDES THE CONTRACT (abi 20): the backend derives the masks and the layouts from the
        // (color_attachment, shader_read) pair - the shipped recipe, which the shadow gate `static_assert`s
        // field for field against the raw transition this line used to spell.
        if (!io.barrier_images.empty() && io.barrier_images[0].image_handle != nullptr) {
            if (io.list->barrier(rhi::image_barrier{.resource = io.barrier_images[0].image_handle,
                                                    .from = rhi::image_use::color_attachment,
                                                    .to = rhi::image_use::shader_read,
                                                    .range = {}}) != rhi::error::ok) {
                return; // a refused barrier would leave the input in a state nobody declared
            }
        }
        // ... then the swapchain, which the instance CLEARs: UNDEFINED as the old layout asserts nothing about
        // contents the filter is about to replace entirely.
        // ... and the swapchain target, whose old state is UNDEFINED because the instance CLEARs it. The
        // backend derives the (undefined, color_attachment) recipe (abi 20).
        if (io.list->barrier(rhi::image_barrier{.resource = io.targets[0].image_handle,
                                                .from = rhi::image_use::undefined,
                                                .to = rhi::image_use::color_attachment,
                                                .range = {}}) != rhi::error::ok) {
            return;
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
        std::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
            .layer_count = 1,
            .colors = colors,
            .depth = {},
            .has_depth = false,
            .secondary_contents = false, // the overlay records into the primary; nothing here executes a secondary
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return;
        }
        io.list->set_cull_mode(rhi::cull_mode::none); // the synthetic triangle has no facing to cull
        // The post chain's source is a heap slot now (see shaders/post.slang): the third push lane names it, and
        // the frame bound the heaps for this command buffer, so there is no set to bind here.
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        io.list->draw(3, 1, 0, 0);
        // INSIDE the instance, between the draw and its end: this pass is the frame's LAST writer whenever it runs,
        // so the overlay belongs here - drawing it in the composite's instance instead would let the edge filter
        // blur the UI text into mush (see fxaa_frame::after_draw, and the composite's frame for the other case).
        if (this->pass_frame.after_draw.valid()) {
            this->pass_frame.after_draw.record(this->pass_frame.after_draw.owner, *io.cmd);
        }
        io.list->end_rendering();
    }

} // namespace deren::vulkan::pass
