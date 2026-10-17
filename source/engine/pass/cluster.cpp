// The clustered-light sort's implementation: the shared scene block, the 1D dispatch over the cluster grid and
// the two buffer barriers that publish its writes. Moved out of `runtime::record_cluster_pass` and
// `core::make_cluster_pipeline` UNCHANGED in behaviour - the same single set bound at index 0, the same 64-wide
// workgroup, the same `(count + 63) / 64` group count and the same two COMPUTE_SHADER -> FRAGMENT_SHADER buffer
// barriers over the same two buffers - so the A/B against the parent commit decides it.
//
// It is the first compute pipeline this branch has taken out of `deren.vulkan.core`: `core::make_cluster_pipeline`
// built it against the core's own scene pipeline layout, which a pass cannot own. The replacement builds the
// same shape in `deren.engine.pipelines` (no native set layout - the heap frame/image lanes are pushed at record time),
// so what changes is WHO owns the layout, not what the driver is asked for.

module;

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

module deren.engine.pass.cluster;
import deren.utility.shared;

import deren.promise.rhi;      // the record series (abi 20): the dispatch and the two buffer barriers
import deren.engine.pipelines; // build_cluster: the compute pipeline this pass owns
import deren.utility;
import deren.engine.primitive;

// The contract's spelling, local to this TU (post.cpp, upscale.cpp, taa.cpp carry the same alias).
namespace rhi = deren::promise::rhi;

namespace deren::engine::pass {

    cluster_pass::~cluster_pass() {
        this->release_owned();
    }

    void cluster_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& cluster_pass::io() const noexcept {
        return render_resource::cluster_io;
    }

    deren::engine::pass::behaviour const& cluster_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view cluster_pass::feature() const noexcept {
        // The renderer's registry answers whether there is anything to sort (the knob, a punctual light, a
        // shading stage that reads a light list) and whether this pass built its pipeline.
        return "clustered";
    }

    bool cluster_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* cluster_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    void cluster_pass::set_frame(cluster_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void cluster_pass::prepare_frame(frame_facts const& facts) noexcept {
        // ONE number, and the host is the only one that can compute it: the grid the light culling produced,
        // which is the same `cluster_grid` the light UBO carries (see `runtime::make_frame_facts`).
        this->set_frame(cluster_frame{.cluster_count = facts.cluster_count});
    }

    void cluster_pass::create(pass_context const& context) {
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
        std::span<uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("clustered lights disabled: the owner has no {}", shader_name);
            return;
        }
        // NO SET LAYOUT IS ASKED FOR: the pipeline is heap-native (a null layout plus the heap flag), so the
        // pass's descriptors come from the frame's bound heap and not from a set handed over by the owner.
        auto built = pipelines::build_cluster(*context.face, spirv);
        if (!built) {
            deren::utility::log("clustered lights disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: clustered light pipeline created (the frame's punctual lights are binned per cluster)");
    }

    void cluster_pass::describe_resources(render_resource::instances::resource_context const&,
                                          deren::utility::vessel::vector<render_resource::instances::request>& requests) const {
        namespace managed = render_resource::instances;
        managed::request counts;
        counts.id = render_resource::resource_id::cluster_counts;
        counts.type = managed::kind::buffer;
        counts.life = managed::lifetime::frame_scratch;
        counts.scope = render_resource::resource_scope::per_frame_slot;
        counts.buffer.size = uint64_t(deren::engine::max_cluster_count) * sizeof(uint32_t);
        counts.buffer.usage = rhi::buffer_usage::storage_coherent;
        counts.buffer.flags = rhi::to_bits(rhi::buffer_flag::device_address);
        counts.zero_on_create = true;
        counts.zero_on_frame = true;
        requests.push_back(counts);
        auto indices = counts;
        indices.id = render_resource::resource_id::cluster_indices;
        indices.buffer.size *= deren::engine::cluster_light_capacity;
        indices.zero_on_frame = false;
        requests.push_back(indices);
    }

    void cluster_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the grid's DIMENSIONS are the renderer's frame data (it recomputes them from the new
        // extent and hands them over every frame), and this pass owns no per-generation handle.
    }

    rhi::error cluster_pass::record_content(resolved_io const& io) noexcept {
        if (!this->pass_pipeline.has_value() || io.pipelines.empty() || io.pipelines[0] == nullptr || this->pass_frame.cluster_count == 0)
            return rhi::error::invalid_argument; // the record step resolves this or skips the pass (see runtime::resolve_cluster_pass)
        if (io.list == nullptr)
            return rhi::error::invalid_argument;
        // The dispatch reads the frame's OWN camera and light slots and writes the same generation's cluster
        // buffers - and it needs no bind to do it: the frame bound the heaps once, and the two indices its push
        // block carries pick the slot. A compute stage is not part of a rendering instance, so this still records
        // before vkCmdBeginRendering.
        // NO BIND IS RECORDED HERE (abi 21): the pass NAMES its pipeline in `behaviour::pipelines`, so the
        // record step binds it before this entry is called - a frame that cannot resolve it does not record the
        // pass at all.
        // The shader's only push fields are the heap frame/image indices. The host appends them to
        // the empty pass-specific payload; no earlier pass in this command buffer may be assumed to set them.
        if (!io.push_block(*io.list, {}))
            return rhi::error::invalid_argument;
        io.list->dispatch((this->pass_frame.cluster_count + group_size - 1u) / group_size, 1, 1);
        return rhi::error::ok;
    }

} // namespace deren::engine::pass
