// The scene pass's implementation: the rendering instance, the parallel segments, and the draw loop that used
// to live in `runtime::record_opaque_scene` / `record_main_segment` / `sub_render_task`. Moved unchanged in
// behaviour - same attachment order, same clear values, same segment count rule, same per-segment bind and
// draw order, same secondary inheritance - so the capture gate decides the move on the twelve scenarios, which
// between them cover the deferred surface write, the forward shading path and the lighting stage that reads the
// depth this pass writes.

module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory> // std::shared_ptr: the session's target is the contract buffer the frame borrowed
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.scene;

import deren.promise.rhi; // the record series (abi 20): the barriers, the rendering scope, the secondary's lifecycle
import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    render_resource::pass_io const& scene_pass::io() const noexcept {
        return render_resource::scene_io;
    }

    deren::vulkan::pass::behaviour const& scene_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view scene_pass::feature() const noexcept {
        // THE RENDERER'S GATE, as a feature name: this pass runs when the surface pipelines exist. It used to be
        // the first line of `runtime::resolve_scene_pass` (which returned false), and the OTHER half of that gate -
        // whether this frame's target generation exists - needs no answer here: the pass's declared targets are
        // resolved from the frame's resource table, so a frame without them does not resolve the pass at all.
        return "scene";
    }

    void scene_pass::create(pass_context const&) {
        // NOTHING TO BUILD, and that is the pass: it owns no set layout (its bindings all live in the shared
        // scene set) and no pipeline (a leaf names the pipeline it wants, and the renderer's registry owns it).
        // A pass whose create is empty is not a pass that is missing something - it is a pass that draws other
        // people's content, which is exactly what a scene pass is.
    }

    void scene_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing of this pass is per-image: the targets are the frame's, the secondaries are the frame loop's,
        // and the leaves hold their own buffers.
    }

    void scene_pass::set_frame(scene_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    bool scene_pass::begin_segment(rhi::command_buffer& command_buffer) const {
        // The inheritance is built FRESH here, so the chain points at this invocation's stack: a task is moved
        // into the pool and may run later, on another thread.
        //
        // THE INHERITANCE RIDES THE CONTRACT NOW (abi 20): the native colour formats, the depth format, the
        // sample count and the view mask the old `VkCommandBufferInheritanceRenderingInfo` carried are named in
        // the contract's own tagged structure, and the BACKEND derives the descriptor-heap half itself from the
        // heaps it has bound (so the heap info is deliberately NOT chained here - the chain takes one entry,
        // and a second truth is what it refuses).
        std::array<rhi::image_format, max_render_targets> color_formats = {};
        uint32_t color_count = 0;
        for (::deren::promise::rhi::image_format const format : this->pass_frame.color_formats) {
            if (color_count >= color_formats.size()) {
                break;
            }
            color_formats[color_count++] = format;
        }
        rhi::command_buffer_inheritance_info const inheritance = {
            .color_format_count = color_count,
            .color_formats = color_formats.data(),
            .depth_format = this->pass_frame.depth_format,
            .samples = static_cast<std::uint32_t>(this->pass_frame.samples),
            .view_mask = 0, // the raw inheritance built `viewMask = 0` (make_inheritance_rendering_info)
        };
        rhi::command_buffer_begin_info const begin = {
            .struct_size = sizeof(rhi::command_buffer_begin_info),
            .usage = rhi::to_bits(rhi::command_buffer_usage::render_pass_continue),
            .next = &inheritance.header,
        };
        return command_buffer.begin_recording(begin) == rhi::error::ok;
    }

    void scene_pass::record_segment(rhi::command_buffer& command_buffer, std::span<primitive const* const> const leaves) const {
        // THE SESSION'S TARGET: the frame hands a BORROWED contract buffer (the runtime owns this segment's
        // buffer for the whole slot), and the session builder takes the owning handle, so the borrow is spelled
        // with a NON-OWNING shared_ptr - the control block drops nothing (the shape the probe backend's static
        // buffer uses for the same reason).
        std::shared_ptr<rhi::command_buffer> const session{&command_buffer, [](rhi::command_buffer*) noexcept {}};
        render_environment env = this->pass_frame.make_environment(this->pass_frame.owner, session, this->pass_frame.gbuffer);
        // NO DESCRIPTOR SET IS BOUND, and there is nothing left to bind: the frame bound the resource and sampler
        // heaps once for this command buffer, and every slot a scene shader reads - the camera, the lights, the
        // clusters, the material table, the textures, the instance transforms - is a heap slot it names itself,
        // with the two indices its push block carries picking the frame's generation. The per-segment bind this
        // replaced existed because a secondary inherits no state from its primary; the heap bind has the same
        // property, and the runtime makes it when it records the segment's command buffer.
        for (primitive const* const leaf : leaves) {
            leaf->draw(env); // polymorphic: normal / instanced / static / custom, each through its own pipeline
        }
    }

    void scene_pass::record(resolved_io const& io) {
        if (this->pass_frame.make_environment == nullptr || this->pass_frame.segments.empty() || this->pass_frame.leaves.empty() || io.targets.empty() || io.list == nullptr) {
            return; // the runner resolves all of this or skips the pass (see runtime::resolve_scene_pass)
        }

        // THE INSTANCE, over the targets the declaration names: every colour target in declaration order, and
        // the one DEPTH target in the depth slot. The declarations and the resolved handles are index-aligned,
        // which is what lets the pass tell the two apart without the framework carrying a kind in resolved_io.
        // THE SCOPE RIDES THE CONTRACT NOW (abi 20): the attachments are contract VIEWS, the load op and the
        // store op are the roles the raw helpers spelled (CLEAR + STORE for both kinds), and the flags word is
        // the one bit the raw call passed (`secondary_contents`).
        std::array<rhi::color_attachment, max_render_targets> color_attachments = {};
        uint32_t color_count = 0;
        rhi::depth_attachment depth_attachment = {};
        bool has_depth = false;
        render_resource::pass_io const& declared = this->io();
        for (std::size_t t = 0; t < io.targets.size() && t < declared.targets.size() && t < max_render_targets; ++t) {
            if (declared.targets[t].kind == render_resource::target_kind::depth) {
                // the surface depth must SURVIVE the instance: the lighting stage, the transparent pass, the
                // resolve and the debug view all read it later in the same submission
                depth_attachment = rhi::depth_attachment{.view = io.targets[t].view_handle,
                                                         .load = rhi::load_op::clear, // make_depth_attachment_info's loadOp
                                                         .store = rhi::store_op::store,
                                                         .read_only = false,
                                                         .has_stencil = false,
                                                         .clear_depth = 1.0f,
                                                         .clear_stencil = 0};
                has_depth = true;
            } else {
                // every target clears to zero: no geometry, no motion, no emissive
                color_attachments[color_count++] = rhi::color_attachment{.view = io.targets[t].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {}};
            }
        }
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height},
            .layer_count = 1, // the raw make_rendering_info's layerCount, which is 1 at every site in this engine
            .colors = std::span<rhi::color_attachment const>(color_attachments.data(), color_count),
            .depth = depth_attachment,
            .has_depth = has_depth,
            .secondary_contents = true, // the raw call passed VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return; // a refused scope would leave the targets in a state nobody declared
        }

        // THE SEGMENTS: the renderer's own rule, kept exactly - one segment when there is only one, or when
        // there are too few leaves for the fan-out to pay for itself.
        std::size_t const leaf_count = this->pass_frame.leaves.size();
        std::size_t const segment_count = std::min<std::size_t>(this->pass_frame.segments.size(), std::max<std::size_t>(1, leaf_count));
        if (segment_count == 1 || leaf_count < min_leaves_for_parallel) {
            rhi::command_buffer* const single = this->pass_frame.segments[0].buffer;
            if (single != nullptr && this->begin_segment(*single)) {
                this->record_segment(*single, this->pass_frame.leaves);
                // NEVER EXECUTE A SECONDARY THAT DID NOT END: an ended buffer is what `execute()` requires, and
                // the raw spelling's failed vkEndCommandBuffer was the same hazard the begin flag guards.
                if (single->end_recording() == rhi::error::ok) {
                    if (io.list->execute(*single) != rhi::error::ok) {
                        deren::utility::log("scene pass: executing the main secondary was refused - the frame records no scene content");
                    }
                } else {
                    deren::utility::log("scene pass: main secondary end failed - scene skipped this frame");
                }
            } else {
                deren::utility::log("scene pass: main secondary begin failed - scene skipped this frame");
            }
        } else {
            // One task per contiguous span of leaves, each recording its own secondary on its own pool (a pool
            // is not thread safe, and a worker never shares one). The tasks read shared state and write only
            // their own command buffer; the batch is waited on before the primary executes the segments IN
            // ORDER, so the recorded command stream is the sequential one.
            std::vector<std::function<void()>> tasks;
            tasks.reserve(segment_count);
            std::vector<std::atomic<bool>> segment_recorded(segment_count);
            for (std::size_t s = 0; s < segment_count; ++s) {
                std::size_t const seg_first = leaf_count * s / segment_count;
                std::size_t const seg_last = leaf_count * (s + 1) / segment_count;
                rhi::command_buffer* const segment = this->pass_frame.segments[s].buffer;
                std::span<primitive const* const> const segment_leaves(this->pass_frame.leaves.data() + seg_first, seg_last - seg_first);
                std::atomic<bool>* const recorded = &segment_recorded[s];
                tasks.emplace_back([this, segment, segment_leaves, recorded] {
                    if (segment == nullptr || !this->begin_segment(*segment)) {
                        deren::utility::log("scene pass: main segment secondary begin failed - segment skipped this frame");
                        recorded->store(false, std::memory_order_relaxed);
                        return;
                    }
                    this->record_segment(*segment, segment_leaves);
                    if (segment->end_recording() != rhi::error::ok) {
                        deren::utility::log("scene pass: main segment secondary end failed - segment skipped this frame");
                        recorded->store(false, std::memory_order_relaxed);
                        return;
                    }
                    recorded->store(true, std::memory_order_relaxed);
                });
            }
            this->pass_frame.run_tasks(this->pass_frame.owner, tasks);
            for (std::size_t s = 0; s < segment_count; ++s) {
                if (!segment_recorded[s].load(std::memory_order_relaxed)) {
                    continue; // never execute a secondary whose begin failed
                }
                rhi::command_buffer* const segment = this->pass_frame.segments[s].buffer;
                if (io.list->execute(*segment) != rhi::error::ok) {
                    deren::utility::log("scene pass: executing segment {} was refused - its content is missing this frame", s);
                }
            }
        }

        // THE INSTANCE ENDS HERE, which is the whole point of the pass owning it: the lighting stage, the
        // transparent pass, the resolve and the lighting stage all run after this, each opening its own.
        io.list->end_rendering();
    }

} // namespace deren::vulkan::pass
