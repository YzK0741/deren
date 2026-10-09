// The G-buffer debug view's implementation: the four images the pass moves to a sampled layout, the frame's two
// bookkeeping callbacks, the CLEAR instance over the HDR target, the one-set bind, the 16-byte push and the
// fullscreen draw. Moved out of `runtime::record_gbuffer_debug_pass` UNCHANGED in behaviour except for the two
// things the pass does not own - the HDR target's own transition and the missing-set fallback, both of which are
// the frame loop's (see the pass's header) - so the knob-on A/B against the parent commit is what decides the move.

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

module deren.engine.pass.geometry_buffer_debug;

import deren.promise.rhi;
import deren.engine.pipelines; // build_gbuffer_debug: the view pipeline, and that is all the pass takes from the builder
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::engine::pass {

    gbuffer_debug_pass::~gbuffer_debug_pass() {
        this->release_owned();
    }

    void gbuffer_debug_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& gbuffer_debug_pass::io() const noexcept {
        return render_resource::gbuffer_debug_io;
    }

    deren::engine::pass::behaviour const& gbuffer_debug_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view gbuffer_debug_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.gbuffer_debug` is "the knob is on, the G-buffer pipeline and this
        // pass's pipeline exist", which is also what the overlay asks when it offers the view and what the composite
        // asks when it zeroes the bloom weight for it - one answer, not three spellings of the same question.
        return "gbuffer-debug";
    }

    void gbuffer_debug_pass::create(pass_context const& context) {
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
            deren::utility::log("gbuffer debug view disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // The debug view reads the stored surface through the frame's heap, so the pipeline is all it builds.
        auto built = pipelines::build_gbuffer_debug(*context.face, rhi::image_format::r16g16b16a16_sfloat, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("gbuffer debug view disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->debug);
        deren::utility::log("SUCCESS: gbuffer debug pipeline created (the stored surface, one channel at a time)");
    }

    void gbuffer_debug_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the surface's FORMAT and on nothing whose size changes, and the
        // set this pass binds belongs to the G-buffer family, whose owner retires it.
    }

    bool gbuffer_debug_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* gbuffer_debug_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    void gbuffer_debug_pass::set_channel(int32_t const channel) noexcept {
        // The clamp came with the parameter: the count is the declaration's own `gbuffer_channel_count`, and a
        // channel outside it would index the shader's switch by a value it does not know.
        this->debug_channel = std::clamp(channel, 0, channel_count - 1);
    }

    int32_t gbuffer_debug_pass::channel() const noexcept {
        return this->debug_channel;
    }

    void gbuffer_debug_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == nullptr ||
            io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        // THE CONTRACT VIEW IS THE GUARD (plan X5 B2): the raw lane is gone, and a null handle is exactly the
        // "this frame has no target" this refusal is about.
        deren::promise::rhi::image_view* const target_view = io.targets[0].view_handle;
        if (target_view == nullptr) {
            return;
        }
        // THE FOUR IMAGES THIS PASS READS BECOME SAMPLES, in ONE dependency info and in the declaration's order -
        // the three stored targets, then the motion-vector target. A pipeline barrier may not be recorded inside a
        // rendering instance, which is why this is here and not after the scope below.
        std::array<rhi::image_barrier, render_resource::gbuffer_debug_barriers.size()> barriers = {};
        for (std::size_t b = 0; b < barriers.size(); ++b) {
            // COLOR_ATTACHMENT -> SHADER_READ, the shipped `hdr_sampling_transition` pair: the backend derives
            // the masks and the layouts from it (abi 20).
            barriers[b] = rhi::image_barrier{.resource = io.barrier_images[b].image_handle,
                                             .from = rhi::image_use::color_attachment,
                                             .to = rhi::image_use::shader_read,
                                             .range = {}};
        }
        if (io.list->barrier(rhi::barrier_group{.images = barriers}) != rhi::error::ok) {
            return; // a refused barrier would leave the inputs in a state nobody declared
        }
        // ... and the two pieces of per-image bookkeeping the frame USED to carry are now the renderer's stage
        // preamble (see gbuffer_debug_frame's replacement note): the depth's hand-back and the motion-vector flag's
        // clearing are the frame's ordering rules about images the G-buffer pass wrote.
        // THE RENDERING SCOPE RIDES THE CONTRACT NOW (abi 20): one colour attachment, CLEAR + STORE (what the raw
        // helper spelled), zero clear colour, no depth - the whole scope this pass opens.
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
            .secondary_contents = false, // nothing here executes a secondary command buffer
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return;
        }
        io.list->set_cull_mode(rhi::cull_mode::none); // the synthetic triangle has no facing to cull
        // No set to bind: the G-buffer images are per-swapchain-image heap slots the shader indexes itself with
        // the image index its push block carries (see shaders/gbuffer_debug.slang and heap_slots.glsl).
        // The push block is the pass's own now: the channel it owns, the frame's two projection terms (from
        // `resolved_io::constants`) and the motion gain, which scales itself across resolutions by using the frame's
        // own width (four pixels saturate the motion channel).
        push_constants const push = {
            .channel = static_cast<float>(this->debug_channel),
            .proj_22 = io.constants.proj[2][2],
            .proj_32 = io.constants.proj[3][2],
            .motion_gain = static_cast<float>(io.frame.extent.width) * 0.25f,
        };
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        io.list->draw(3, 1, 0, 0);
        io.list->end_rendering();
    }

} // namespace deren::engine::pass
