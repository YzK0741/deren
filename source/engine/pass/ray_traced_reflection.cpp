// The ray-traced reflection pass's implementation. It follows ray_traced_shadow.cpp: describe the one target,
// build the pipeline and its three shader binding table regions, then push the camera block and launch one to
// four rays per pixel. The graph owns the target's transitions; this file only records the work.

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
module deren.engine.pass.ray_traced_reflection;
import deren.vstd;
import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.render_resource;
import deren.engine.pass.output_targets;
import deren.engine.pipelines;
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::engine::pass {

    rt_reflection_pass::~rt_reflection_pass() {
        this->release_contract();
    }

    void rt_reflection_pass::describe_resources(render_resource::instances::resource_context const& context,
                                                deren::utility::vessel::vector<render_resource::instances::request>& out) const {
        for (auto const& value : output_targets_pass::descriptions(context)) {
            if (value.id == render_resource::resource_id::rt_reflection) {
                out.push_back(value);
            }
        }
    }

    void rt_reflection_pass::bind_resources(render_resource::instances::resource_snapshot const& snapshot) {
        resource_view = snapshot;
    }

    void rt_reflection_pass::release_contract() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& rt_reflection_pass::io() const noexcept {
        return render_resource::rt_reflection_io;
    }

    deren::engine::pass::behaviour const& rt_reflection_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view rt_reflection_pass::feature() const noexcept {
        return "rt_reflection";
    }

    bool rt_reflection_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* rt_reflection_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    void rt_reflection_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_contract();
        }
        this->built_against = context.face;
        if (this->pass_pipeline.has_value()) {
            return;
        }
        auto const fetch = [&context](std::string_view const name) {
            return context.shader != nullptr ? context.shader(context.parent, name) : std::span<uint8_t const>{};
        };
        std::span<uint8_t const> const raygen = fetch(raygen_name);
        std::span<uint8_t const> const closest_hit = fetch(closest_hit_name);
        std::span<uint8_t const> const any_hit = fetch(any_hit_name);
        std::span<uint8_t const> const miss = fetch(miss_name);
        if (raygen.empty() || closest_hit.empty() || any_hit.empty() || miss.empty()) {
            deren::utility::log("ray-traced reflections unavailable: the parent has not registered all of {}, {}, {} and {}", raygen_name, closest_hit_name, any_hit_name, miss_name);
            return;
        }
        auto built = pipelines::build_rt_reflection_ray_tracing(*context.face, raygen, closest_hit, any_hit, miss);
        if (!built) {
            deren::utility::log("ray-traced reflections unavailable: {}", built.error());
            this->release_contract();
            return;
        }
        this->pass_pipeline = std::move(*built->pipeline);

        uint32_t const handle_size = context.ray_tracing_properties.handle_size;
        uint32_t const handle_alignment = context.ray_tracing_properties.handle_alignment;
        uint32_t const base_alignment = context.ray_tracing_properties.base_alignment;
        if (handle_size == 0 || handle_alignment == 0 || base_alignment == 0 || context.create_upload_buffer == nullptr) {
            deren::utility::log("ray-traced reflections unavailable: this device published no shader binding table numbers to build one against");
            this->release_contract();
            return;
        }
        uint32_t const region_size = ((handle_size + base_alignment - 1u) / base_alignment) * base_alignment;
        uint32_t const group_count = built->group_count;
        deren::utility::vessel::vector<uint8_t> handles(static_cast<size_t>(group_count) * handle_size);
        if (!pass::shader_group_handles(context.face, *this->pass_pipeline->contract, 0, group_count, std::span<std::uint8_t>(handles))) {
            deren::utility::log("ray-traced reflections unavailable: the shader group handles could not be read back");
            this->release_contract();
            return;
        }
        deren::utility::vessel::vector<uint8_t> table(static_cast<size_t>(group_count) * region_size, 0);
        for (uint32_t group = 0; group < group_count; ++group) {
            std::memcpy(table.data() + static_cast<size_t>(group) * region_size, handles.data() + static_cast<size_t>(group) * handle_size, handle_size);
        }
        uint64_t address = 0;
        deren::promise::rhi::buffer* const table_buffer = context.create_upload_buffer(context.parent, table.data(), static_cast<uint64_t>(table.size()),
                                                                                       deren::promise::rhi::buffer_flags{deren::promise::rhi::to_bits(
                                                                                           deren::promise::rhi::buffer_flag::shader_binding_table)},
                                                                                       &address);
        if (table_buffer == nullptr || address == 0) {
            deren::utility::log("ray-traced reflections unavailable: the shader binding table buffer could not be created");
            this->release_contract();
            return;
        }
        auto const region = [region_size](uint64_t const at) { return rhi::shader_binding_table_region{.address = at, .size = region_size, .stride = region_size}; };
        this->raygen_region = region(address);
        this->miss_region = region(address + region_size);
        this->hit_region = region(address + static_cast<std::uint64_t>(2u) * region_size);
        deren::utility::log("SUCCESS: ray-traced reflection pipeline created (raygen + miss + closest hit + alpha any hit)");
    }

    void rt_reflection_pass::on_swapchain_recreated(pass_host const&) {
        // No per-generation state: the target is per frame slot and the pipeline does not depend on the surface.
    }

    void rt_reflection_pass::set_rt_reflection_samples(uint32_t const samples) noexcept {
        this->sample_count = std::clamp<uint32_t>(samples, 1u, 4u);
    }

    bool rt_reflection_pass::record_ready(resolved_io const& io) const noexcept {
        return this->pass_pipeline.has_value() && io.barrier_images.size() >= render_resource::rt_reflection_barriers.size() &&
               !io.pipelines.empty() && io.pipelines[0] != nullptr && io.list != nullptr &&
               io.barrier_images[barrier_reflection].image_handle != nullptr && this->hit_region.address != 0 &&
               io.extent.width != 0 && io.extent.height != 0;
    }

    rhi::error rt_reflection_pass::record_content(resolved_io const& io) noexcept {
        if (!this->record_ready(io)) {
            return rhi::error::invalid_argument;
        }
        push_constants push = {};
        push.inv_view_proj = io.constants.inv_view_proj;
        push.params.w = static_cast<float>(this->sample_count);
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        io.list->trace_rays(this->raygen_region, this->miss_region, this->hit_region, this->callable_region, io.extent.width, io.extent.height, 1);
        if (!this->logged) {
            this->logged = true;
            deren::utility::log("ray-traced reflections: tracing {}x{} pixels with {} ray(s) per pixel", io.extent.width, io.extent.height, this->sample_count);
        }
        return rhi::error::ok;
    }

} // namespace deren::engine::pass
