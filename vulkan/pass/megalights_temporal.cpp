// The temporal resolve's implementation: the barriers around its accumulation, the copy that becomes the next
// frame's history, the hand-off to the lighting stage, and the two things it owns outside a frame - the set
// layout generated from its declaration plus the family that holds one set per swapchain image, and its
// compute pipeline. Its barrier reasoning is the shape any running mean has: the accumulation is read as history
// at one end of the frame and written at the other, so it needs a transition on each side, and the copy that
// becomes the next frame's history is a second one. The depth and velocity it also reads are its OWN bindings,
// so those two transitions belong to the pass as well.

module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>

module deren.vulkan.pass.megalights_temporal;

import deren.promise.rhi; // the record series (abi 20): the barriers, the copy and the dispatch
import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_megalights_temporal: the compute pipeline this pass owns
import deren.utility;

// The contract's spelling, local to this TU (post.cpp, upscale.cpp, taa.cpp carry the same alias).
namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    megalights_temporal_pass::~megalights_temporal_pass() {
        this->release_owned();
    }

    void megalights_temporal_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& megalights_temporal_pass::io() const noexcept {
        return render_resource::megalights_temporal_io;
    }

    deren::vulkan::pass::behaviour const& megalights_temporal_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view megalights_temporal_pass::feature() const noexcept {
        // The CHAIN's feature: this pass is required, so "the chain is on" is the right gate - and the
        // renderer's predicate for it includes this pass having built its pipeline.
        return "megalights";
    }

    bool megalights_temporal_pass::ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* megalights_temporal_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    bool megalights_temporal_pass::resolved() const noexcept {
        return this->accumulation_resolved;
    }

    void megalights_temporal_pass::set_accumulation(float const depth_tolerance, float const max_frames) noexcept {
        // The clamps live with the values: a tolerance of 0 would reject every history (the accumulation could
        // never grow past one frame), and a cap below 1 would divide the running mean by zero.
        this->temporal_depth_tolerance = std::max(depth_tolerance, 0.0f);
        this->accumulation_frames = std::clamp(max_frames, 1.0f, max_frames_limit);
    }

    void megalights_temporal_pass::set_spatial(float const sigma) noexcept {
        this->denoise_sigma = std::clamp(sigma, 0.0f, 4.0f);
    }

    void megalights_temporal_pass::set_frame(megalights_temporal_frame const& frame) noexcept {
        this->pass_frame = frame;
        // A NEW FRAME BEGINS, the same per-frame answer the GI resolve gives: `resolved()` is exactly "this
        // frame's dispatch happened", which the renderer reads to decide whether the lighting stage may add the
        // accumulation at all.
        this->accumulation_resolved = false;
    }

    void megalights_temporal_pass::prepare_frame(frame_facts const& facts) noexcept {
        megalights_temporal_frame frame = this->pass_frame; // the callback the owner installed survives this call
        frame.history_valid = facts.megalights_history_valid;
        this->set_frame(frame);
    }

    void megalights_temporal_pass::on_swapchain_recreated(pass_host const&) {
        this->accumulation_resolved = false;
    }

    void megalights_temporal_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_owned();
        }
        this->built_against = context.face;
        if (this->ready()) {
            return; // already built for this device
        }
        std::span<uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("stochastic punctual lighting's temporal resolve disabled (the chain will stay off): the owner has no {}", shader_name);
            return;
        }
        auto built = pipelines::build_resolve_pipeline(*context.face, spirv);
        if (!built) {
            deren::utility::log("stochastic punctual lighting's temporal resolve disabled (the chain will stay off): {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->resolve);
        deren::utility::log("SUCCESS: stochastic punctual lighting's temporal resolve created (running mean with a per-pixel frame count)");
    }

    void megalights_temporal_pass::record(resolved_io const& io) {
        this->accumulation_resolved = false;
        if (!this->ready() || io.barrier_images.size() < render_resource::megalights_temporal_barriers.size() || io.frame.image_count == 0 || io.pipelines.empty() ||
            io.pipelines[0] == nullptr || io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (the declaration's own gates are the table's)
        }
        // THE CONTRACT'S OWN HANDLES (abi 20): the record series names the handles the resolved binding
        // publishes BESIDE the raw lanes, so the guard and every call below speak one vocabulary.
        rhi::image* const resolve_image = io.barrier_images[barrier_resolve].image_handle;
        rhi::image* const history_image = io.barrier_images[barrier_history].image_handle;
        if (io.list == nullptr || resolve_image == nullptr || history_image == nullptr) {
            return; // the handles this frame's publication left empty: the resolve cannot record
        }

        // Layouts, all before the dispatch. The accumulation is READ across frames (the lighting stage samples
        // it after this pass) and the history only by the resolve, which is a copy's destination first.
        // THE PAIRS RIDE THE CONTRACT NOW (abi 20): the backend derives the masks and the layouts from the
        // roles, and the batch stays ONE call - `barrier_group` is what the raw `vkCmdPipelineBarrier2` was.
        std::array<rhi::image_barrier, 2> barriers = {};
        uint32_t count = 0;
        barriers[count] = rhi::image_barrier{.resource = resolve_image,
                                             .from = rhi::image_use::undefined,
                                             .to = rhi::image_use::shader_write,
                                             .range = {}}; // the accumulation is fully overwritten
        ++count;
        if (!this->pass_frame.history_valid) {
            // FIRST USE for this image: the history's contents are whatever the allocation held, so the
            // descriptor has to be legal without their being readable - UNDEFINED -> SHADER_READ.
            barriers[count] = rhi::image_barrier{.resource = history_image,
                                                 .from = rhi::image_use::undefined,
                                                 .to = rhi::image_use::shader_read,
                                                 .range = {}};
            ++count;
        }
        if (io.list->barrier(rhi::barrier_group{.images = std::span(barriers.data(), count)}) != rhi::error::ok) {
            return; // a refused barrier would leave an input in a state nobody declared
        }

        // No set is bound: the history and the accumulation images are heap slots (one per swapchain image), and
        // the frame bound the heaps for this command buffer.

        push_constants push = {};
        push.params = glm::vec4(io.constants.proj[2][2], io.constants.proj[3][2], this->accumulation_frames, this->temporal_depth_tolerance);
        push.extents = glm::vec4(static_cast<float>(io.extent.width), static_cast<float>(io.extent.height), this->denoise_sigma, 0.0f);
        static_assert(sizeof(push) <= pass::max_push_bytes, "the resolve's push block must fit the guaranteed minimum");
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        io.list->dispatch((io.extent.width + group_size - 1u) / group_size, (io.extent.height + group_size - 1u) / group_size, 1);

        // ---- the accumulation becomes the next frame's history ----
        // A copy rather than a ping-pong, exactly like the GI resolve: the accumulation is what the lighting
        // stage samples, so the history has to be a second image and copying into it keeps every heap slot
        // in the frame stable.
        std::array<rhi::image_barrier, 2> const copy_barriers = {
            rhi::image_barrier{.resource = resolve_image,
                               .from = rhi::image_use::shader_write,
                               .to = rhi::image_use::transfer_source,
                               .range = {}}, // resolve: GENERAL -> TRANSFER_SRC
            rhi::image_barrier{.resource = history_image,
                               .from = rhi::image_use::shader_read,
                               .to = rhi::image_use::transfer_destination,
                               .range = {}},
        };
        if (io.list->barrier(rhi::barrier_group{.images = copy_barriers}) != rhi::error::ok) {
            return; // a refused barrier would leave an operand of the copy in a state nobody declared
        }

        rhi::image_copy_region const region = {.extent = {io.extent.width, io.extent.height, 1}};
        if (io.list->copy_image(rhi::image_copy{.source = resolve_image,
                                                .destination = history_image,
                                                .source_region = region,
                                                .destination_region = region}) != rhi::error::ok) {
            return; // the history was not written, so this frame's resolve must not claim one
        }

        // Hand both on: the accumulation to the lighting stage that adds it (SHADER_READ, which its binding 17
        // declares) and the history copy to the next frame's resolve.
        std::array<rhi::image_barrier, 2> const hand_back = {
            rhi::image_barrier{.resource = resolve_image,
                               .from = rhi::image_use::transfer_source,
                               .to = rhi::image_use::shader_read,
                               .range = {}}, // resolve -> SHADER_READ
            rhi::image_barrier{.resource = history_image,
                               .from = rhi::image_use::transfer_destination,
                               .to = rhi::image_use::shader_read,
                               .range = {}}, // history -> SHADER_READ
        };
        if (io.list->barrier(rhi::barrier_group{.images = hand_back}) != rhi::error::ok) {
            return; // a refused hand-back would leave both images in a state nobody declared
        }

        this->accumulation_resolved = true;
    }

} // namespace deren::vulkan::pass
