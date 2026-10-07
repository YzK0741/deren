// The TAA resolve's implementation: the barriers, the fullscreen draw into the HDR target, and the copy that
// becomes the next frame's history. Moved out of `deren.vulkan.runtime` unchanged in behaviour - the same barrier
// batches in the same order, the same attachment, the same copy and the same two flags - so the capture gate
// can decide the move on `deferred_taa_fxaa`, the scenario that runs with TAA on.

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

module deren.vulkan.pass.taa;

import deren.promise.rhi;
import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.vulkan.pipelines;
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    taa_pass::~taa_pass() {
        this->release_owned();
    }

    void taa_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& taa_pass::io() const noexcept {
        return render_resource::taa_io;
    }

    deren::vulkan::pass::behaviour const& taa_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view taa_pass::feature() const noexcept {
        return "taa";
    }

    bool taa_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* taa_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    bool taa_pass::wrote_history() const noexcept {
        return this->history_written;
    }

    void taa_pass::set_blend(float const static_weight, float const min_weight) noexcept {
        // The clamps came with the parameters, because they are the same fact: a static weight of 1 would never
        // accept the current frame, and a floor above the static weight would make the moving case trust the
        // history MORE than the still one, which is the opposite of what the two are for.
        this->blend_static = std::clamp(static_weight, 0.0f, 0.99f);
        this->blend_min = std::clamp(min_weight, 0.0f, this->blend_static);
    }

    void taa_pass::reset_history() noexcept {
        // The off -> on edge: the renderer calls this when TAA is switched on, because blending against
        // frames that were never resolved shows the alias instead of hiding it.
        this->valid_history.assign(this->valid_history.size(), false);
    }

    void taa_pass::on_swapchain_recreated(pass_host const&) {
        // THE PASS'S OWN GENERATION RESET, and the framework's `recreate_stage` is what guarantees it happens:
        // the histories belong to images that no longer exist.
        this->valid_history.assign(this->valid_history.size(), false);
        this->history_written = false;
    }

    void taa_pass::create(pass_context const& context) {
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
            deren::utility::log("taa disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        auto built = pipelines::build_taa(*context.face, rhi::image_format::r16g16b16a16_sfloat, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("taa disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->resolve);
        deren::utility::log("SUCCESS: TAA resolve pipeline created (history reprojection over the deferred path)");
    }

    void taa_pass::record(resolved_io const& io) {
        this->history_written = false;
        if (io.own.size() < own_binding_count || io.targets.empty() || io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        // THE CONTRACT'S OWN HANDLES (abi 20): the record series takes the handles the resolved binding
        // publishes BESIDE the raw lanes, so the guard speaks the vocabulary every call below does.
        if (io.list == nullptr || io.own[0].image_handle == nullptr || io.own[1].image_handle == nullptr ||
            io.targets[0].image_handle == nullptr || io.targets[0].view_handle == nullptr) {
            return; // the handles this frame's publication left empty: the resolve cannot record
        }
        uint32_t const index = io.frame.image_index;
        if (this->valid_history.size() != io.frame.image_count) {
            this->valid_history.assign(io.frame.image_count, false);
        }
        if (index >= this->valid_history.size()) {
            return;
        }
        bool const history_valid = this->valid_history[index];

        // Layouts, all before vkCmdBeginRendering: this frame's colour, the motion vectors and (on its first
        // use for this image) the history become inputs, and the HDR target - still untouched this frame -
        // becomes the resolve's attachment. The history is left in SHADER_READ_ONLY by the previous frame's
        // copy and is only ever read as a texture, so it needs no barrier once it is valid.
        std::array<rhi::image_barrier, 2> barriers = {};
        barriers[0] = rhi::image_barrier{.resource = io.own[0].image_handle,
                                         .from = rhi::image_use::color_attachment,
                                         .to = rhi::image_use::shader_read,
                                         .range = {}}; // scene_color: COLOR_ATTACHMENT -> SHADER_READ
        // THE MOTION VECTORS ARE NOT THIS PASS'S BARRIER ANY MORE, and they were the bug: this batch used to
        // transition io.own[2] unconditionally, on the assumption that the TAA resolve is the frame's first
        // sampler of the velocity (the assumption `require_velocity_publish`'s call site documents). Once an
        // EARLIER stage publishes it - the stochastic punctual lighting chain's resolve samples it too, and its
        // stage runs before this one - the unconditional transition claims a COLOR_ATTACHMENT old layout the
        // image is no longer in, and the validation layer invalidates the command buffer
        // (VUID-VkImageMemoryBarrier2-oldLayout-01197). The host now publishes it through
        // `ensure_velocity_sampled`, which transitions only while the G-buffer's flag is still armed and
        // consumes it, so the first sampler in the frame publishes and the rest are no-ops.
        uint32_t barrier_count = 1;
        if (!history_valid) {
            barriers[barrier_count] = rhi::image_barrier{.resource = io.own[1].image_handle,
                                                         .from = rhi::image_use::undefined,
                                                         .to = rhi::image_use::shader_read,
                                                         .range = {}}; // the history's first use: UNDEFINED -> SHADER_READ
            ++barrier_count;
        }
        // THE PAIR RIDES THE CONTRACT NOW (abi 20): the backend derives the masks and the layouts from the
        // roles, and the batch stays ONE call - `barrier_group` is what the raw `vkCmdPipelineBarrier2` was.
        if (io.list->barrier(rhi::barrier_group{.images = std::span(barriers.data(), barrier_count)}) != rhi::error::ok) {
            return; // a refused barrier would leave an input in a state nobody declared
        }

        // NOTE: the G-buffer depth the disocclusion guard samples is transitioned by the HOST, just before
        // this stage (see runtime::record_scene_tail): it is a shared per-image transition whose flag belongs
        // to the G-buffer pass, and a pass can only declare its own bindings. The order the host has to
        // preserve is "after this batch, before the draw" only in the sense that the barrier must precede the
        // draw - the barrier commands are independent of this batch, so the host places them first.

        // THE TARGET'S PAIR, also the contract's (abi 20): UNDEFINED -> COLOR_ATTACHMENT, the shipped
        // `color_attachment_transition` recipe the raw spelling used.
        if (io.list->barrier(rhi::image_barrier{.resource = io.targets[0].image_handle,
                                                .from = rhi::image_use::undefined,
                                                .to = rhi::image_use::color_attachment,
                                                .range = {}}) != rhi::error::ok) {
            return; // a refused barrier would leave the target in a state nobody declared
        }

        // The runner has bound the pipeline and set the viewport and scissor from io.extent (this pass
        // declared resync_viewport); what a fullscreen pass still owns is its instance - the load op is its
        // knowledge - and the state the viewport fields do not cover.
        // THE RENDERING SCOPE RIDES THE CONTRACT NOW (abi 20): one colour attachment, CLEAR + STORE (what
        // `make_color_attachment_info` spelled), zero clear colour, no depth and no secondary contents.
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
            .secondary_contents = false, // the resolve records straight into the primary; no secondary contents
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return;
        }
        io.list->set_cull_mode(rhi::cull_mode::none);
        // No set to bind: the history and the surface are heap slots, one pair per swapchain image, and the frame
        // bound the heaps for this command buffer (see begin_recording).
        // THE PUSH BLOCK IS THE PASS'S OWN (S3): every lane of it is a fact this pass has - its two blend
        // weights, the texel size of the extent it was resolved at, the projection's two depth terms (which
        // arrive as frame CONSTANTS, the channel that exists for exactly this) and the history flag it maintains
        // per image. The renderer used to compose it and hand it over as raw bytes.
        push_constants push = {};
        push.blend_static = this->blend_static;
        push.blend_min = this->blend_min;
        push.texel_size_x = 1.0f / static_cast<float>(io.extent.width);
        push.texel_size_y = 1.0f / static_cast<float>(io.extent.height);
        push.depth_scale = io.constants.proj[2][2];
        push.depth_offset = io.constants.proj[3][2];
        push.history_valid = history_valid ? 1.0f : 0.0f;
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        io.list->draw(3, 1, 0, 0);
        io.list->end_rendering();

        // ---- the resolved frame becomes the next frame's history ----
        // A copy rather than a ping-pong: the resolve necessarily writes the image the post chain reads, so
        // the history has to be a separate image, and copying into it keeps every heap slot in the frame
        // stable (no per-frame descriptor rewrites). The barriers move the HDR target out to TRANSFER_SRC and back - the
        // post chain still finds it in GENERAL, exactly where it expects it.
        std::array<rhi::image_barrier, 2> const copy_barriers = {
            rhi::image_barrier{.resource = io.targets[0].image_handle,
                               .from = rhi::image_use::color_attachment,
                               .to = rhi::image_use::transfer_source,
                               .range = {}}, // HDR -> TRANSFER_SRC
            rhi::image_barrier{.resource = io.own[1].image_handle,
                               .from = rhi::image_use::shader_read,
                               .to = rhi::image_use::transfer_destination,
                               .range = {}}, // history: SHADER_READ -> TRANSFER_DST
        };
        if (io.list->barrier(rhi::barrier_group{.images = copy_barriers}) != rhi::error::ok) {
            return; // a refused barrier would leave an operand of the copy in a state nobody declared
        }

        rhi::image_copy_region const region = {.extent = {io.extent.width, io.extent.height, 1}};
        if (io.list->copy_image(rhi::image_copy{.source = io.targets[0].image_handle,
                                                .destination = io.own[1].image_handle,
                                                .source_region = region,
                                                .destination_region = region}) != rhi::error::ok) {
            return; // the history was not written, so this frame's resolve must not claim one
        }

        // hand both images on: the HDR target back to the post chain, the history copy to the next frame's
        // resolve (which will find it in TRANSFER_DST and transition it from there)
        std::array<rhi::image_barrier, 2> const hand_back = {
            rhi::image_barrier{.resource = io.targets[0].image_handle,
                               .from = rhi::image_use::transfer_source,
                               .to = rhi::image_use::color_attachment,
                               .range = {}}, // HDR -> COLOR_ATTACHMENT
            rhi::image_barrier{.resource = io.own[1].image_handle,
                               .from = rhi::image_use::transfer_destination,
                               .to = rhi::image_use::shader_read,
                               .range = {}}, // history -> SHADER_READ
        };
        if (io.list->barrier(rhi::barrier_group{.images = hand_back}) != rhi::error::ok) {
            return; // a refused hand-back would leave both images in a state nobody declared
        }

        this->valid_history[index] = true;
        this->history_written = true;
    }

} // namespace deren::vulkan::pass
