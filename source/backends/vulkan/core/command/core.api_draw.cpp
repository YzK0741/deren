// -*- C++ -*-
// ============================================================================
// file: source/backends/vulkan/core/command/core.api_draw.cpp
//
// The frame-command recording verbs that translate directly into commands on
// the frame's VkCommandBuffer: pipeline/buffer binding, draws, dispatches,
// mesh tasks, ray launches, and the dynamic state setters. Split out of
// core.api_core.cpp; no state or helper changes.
// ============================================================================
module;
#include <cstdint>
#include <vulkan/vulkan.h>

module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import deren.promise.rhi;
import deren.promise.render_operations;
import :declarations;

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;
    namespace rec = rhi::recording;

    rhi::error core::frame_commands::bind_pipeline(rhi::pipeline const& handle) {
        if (parent) {
            ++parent->operation_requests[7]; // bind_pipeline
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        if (!recording) {
            return rhi::error::not_ready;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(handle, concrete::pipeline)) {
            return rhi::error::invalid_argument;
        }
        {
            std::lock_guard lock(parent->resource.contract_pipelines_mutex);
            if (!parent->resource.contract_pipelines.contains(&handle)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const* const concrete = rhi::do_concrete<core::pipeline>(handle, concrete::pipeline);
        operations.append<rec::operation_kind::pipeline>({reinterpret_cast<std::uintptr_t>(concrete), concrete->identity});
        ++parent->deferred_requested;
        bound_pipeline = concrete;
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::bind_vertex_buffer(rhi::buffer const& handle, std::uint64_t const offset) {
        if (parent) {
            ++parent->operation_requests[8]; // bind_vertex_buffer
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(handle, concrete::buffer)) {
            return rhi::error::invalid_argument;
        }
        {
            std::lock_guard lock(parent->resource.contract_buffers_mutex);
            if (!parent->resource.contract_buffers.contains(&handle)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const* const concrete = static_cast<core::buffer const*>(&handle);
        if (concrete->parent != parent || offset >= concrete->size_bytes) {
            return rhi::error::invalid_argument;
        }
        reference_lazy(*concrete);
        operations.append<rec::operation_kind::vertex>({reinterpret_cast<std::uintptr_t>(concrete), concrete->identity, offset});
        ++parent->deferred_requested;
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::bind_index_buffer(rhi::buffer const& handle, std::uint64_t const offset, rhi::index_type const type) {
        if (parent) {
            ++parent->operation_requests[9]; // bind_index_buffer
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(handle, concrete::buffer)) {
            return rhi::error::invalid_argument;
        }
        {
            std::lock_guard lock(parent->resource.contract_buffers_mutex);
            if (!parent->resource.contract_buffers.contains(&handle)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const* const concrete = static_cast<core::buffer const*>(&handle);
        if (concrete->parent != parent || offset >= concrete->size_bytes) {
            return rhi::error::invalid_argument;
        }
        if ((type != rhi::index_type::uint16 && type != rhi::index_type::uint32) ||
            offset % (type == rhi::index_type::uint32 ? 4 : 2)) {
            return rhi::error::invalid_argument;
        }
        reference_lazy(*concrete);
        operations.append<rec::operation_kind::index>({reinterpret_cast<std::uintptr_t>(concrete), concrete->identity, offset, static_cast<std::uint64_t>(type)});
        ++parent->deferred_requested;
        return rhi::error::ok;
    }

    void core::frame_commands::draw(std::uint32_t const vertex_count, std::uint32_t const instance_count, std::uint32_t const first_vertex, std::uint32_t const first_instance) noexcept {
        if (parent) {
            ++parent->operation_requests[10]; // draw
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        if (!rendering_active) {
            recording_error = rhi::error::invalid_argument;
            return;
        }
        operations.append<rec::operation_kind::draw>({vertex_count, instance_count, first_vertex, first_instance});
        ++parent->deferred_requested;
    }

    void core::frame_commands::draw_indexed(std::uint32_t const index_count, std::uint32_t const instance_count, std::uint32_t const first_index, std::int32_t const vertex_offset, std::uint32_t const first_instance) noexcept {
        if (parent) {
            ++parent->operation_requests[11]; // draw_indexed
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        if (!rendering_active) {
            recording_error = rhi::error::invalid_argument;
            return;
        }
        operations.append<rec::operation_kind::draw_indexed>({index_count, instance_count, first_index, static_cast<std::uint32_t>(vertex_offset), first_instance});
        ++parent->deferred_requested;
    }

    rhi::error core::frame_commands::dispatch_indirect(rhi::buffer const& arguments, std::uint64_t offset) {
        if (parent) {
            ++parent->operation_requests[12]; // dispatch_indirect
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        auto* self = this->parent;
        auto const command = this->native();
        if (!self || command == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(arguments, concrete::buffer)) {
            return rhi::error::invalid_argument;
        }
        {
            std::lock_guard lock(parent->resource.contract_buffers_mutex);
            if (!parent->resource.contract_buffers.contains(&arguments)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const* concrete = static_cast<core::buffer const*>(&arguments);
        if (concrete->parent != self || !rhi::has_flag(concrete->declared_flags, rhi::buffer_flag::indirect) ||
            rhi::validate_dispatch_indirect_range(concrete->size_bytes, offset) != rhi::error::ok) {
            return rhi::error::invalid_argument;
        }
        if (rendering_active) {
            return rhi::error::invalid_argument;
        }
        operations.append<rec::operation_kind::dispatch_indirect>({reinterpret_cast<std::uintptr_t>(concrete), concrete->identity, offset, 1, 0});
        ++parent->deferred_requested;
        return rhi::error::ok;
    }

    void core::frame_commands::dispatch(std::uint32_t const groups_x, std::uint32_t const groups_y, std::uint32_t const groups_z) noexcept {
        if (parent) {
            ++parent->operation_requests[13]; // dispatch
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        if (rendering_active) {
            recording_error = rhi::error::invalid_argument;
            return;
        }
        operations.append<rec::operation_kind::dispatch>({groups_x, groups_y, groups_z});
        ++parent->deferred_requested;
    }

    void core::frame_commands::draw_mesh_tasks(std::uint32_t const groups_x, std::uint32_t const groups_y, std::uint32_t const groups_z) noexcept {
        if (parent) {
            ++parent->operation_requests[14]; // draw_mesh_tasks
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        if (!rendering_active) {
            recording_error = rhi::error::invalid_argument;
            return;
        }
        if (!parent->device.mesh_dispatch) {
            recording_error = rhi::error::unsupported;
            return;
        }
        operations.append<rec::operation_kind::mesh>({groups_x, groups_y, groups_z});
        ++parent->deferred_requested;
    }

    void core::frame_commands::trace_rays(rhi::shader_binding_table_region const& raygen, rhi::shader_binding_table_region const& miss,
                                          rhi::shader_binding_table_region const& hit, rhi::shader_binding_table_region const& callable,
                                          std::uint32_t const width, std::uint32_t const height, std::uint32_t const depth) noexcept {
        if (parent) {
            ++parent->operation_requests[15]; // trace_rays
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        if (rendering_active) {
            recording_error = rhi::error::invalid_argument;
            return;
        }
        if (!parent->device.ray_trace_launch) {
            recording_error = rhi::error::unsupported;
            return;
        }
        operations.append<rec::operation_kind::trace>({raygen.address, raygen.stride, raygen.size,
                                                       miss.address, miss.stride, miss.size, hit.address, hit.stride, hit.size,
                                                       callable.address, callable.stride, callable.size, width, height, depth});
        ++parent->deferred_requested;
    }

    void core::frame_commands::set_viewport(rhi::viewport const& vp) noexcept {
        if (parent) {
            ++parent->operation_requests[21]; // set_viewport
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        operations.append<rec::operation_kind::viewport>(rec::pack(deren::utility::vessel::array<float, 6>{vp.x, vp.y, vp.width, vp.height, vp.min_depth, vp.max_depth}));
        ++parent->deferred_requested;
    }

    void core::frame_commands::set_scissor(rhi::rect const& scissor) noexcept {
        if (parent) {
            ++parent->operation_requests[22]; // set_scissor
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        operations.append<rec::operation_kind::scissor>(rec::pack(deren::utility::vessel::array<std::uint32_t, 4>{static_cast<std::uint32_t>(scissor.offset_x), static_cast<std::uint32_t>(scissor.offset_y), scissor.width, scissor.height}));
        ++parent->deferred_requested;
    }

    void core::frame_commands::set_cull_mode(rhi::cull_mode const mode) noexcept {
        if (parent) {
            ++parent->operation_requests[23]; // set_cull_mode
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        operations.append<rec::operation_kind::cull>(rec::words{static_cast<std::uint64_t>(mode)});
        ++parent->deferred_requested;
    }

    void core::frame_commands::set_depth_write(bool const enable) noexcept {
        if (parent) {
            ++parent->operation_requests[24]; // set_depth_write
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        operations.append<rec::operation_kind::depth_write>(rec::words{enable});
        ++parent->deferred_requested;
    }

    void core::frame_commands::set_depth_bias(float const constant_factor, float const slope_factor, float const clamp) noexcept {
        if (parent) {
            ++parent->operation_requests[25]; // set_depth_bias
        }
        if (!recording || native() == VK_NULL_HANDLE || recording_error != rhi::error::ok) {
            return;
        }
        operations.append<rec::operation_kind::depth_bias>(rec::pack(deren::utility::vessel::array<float, 3>{constant_factor, slope_factor, clamp}));
        ++parent->deferred_requested;
    }

} // namespace deren::vulkan
