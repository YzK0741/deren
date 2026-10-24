module;
#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <span>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.promise.rhi;
import deren.promise.render_operations;
import deren.utility.shared;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;
    namespace rec = rhi::recording;
    using kind = rec::operation_kind;

    void core::frame_commands::reset_operations() noexcept {
        operations.clear();
        saved_state.clear();
        current_resources = {};
        current_textures = {};
        bound_pipeline = emitted_pipeline = nullptr;
        native_state_mask = 0;
        restore_state = final_application = false;
        recording_error = rhi::error::ok;
    }

    rhi::error core::frame_commands::apply_operations() noexcept {
        if (!recording || !owner || native() == VK_NULL_HANDLE)
            return rhi::error::not_ready;
        if (recording_error != rhi::error::ok)
            return recording_error;
        if (operations.empty() && !restore_state)
            return rhi::error::ok;
        bool const optimize = std::getenv("DEREN_RENDER_OPERATIONS_UNOPTIMIZED") == nullptr;
        rec::operation_stream combined;
        if (restore_state) {
            for (auto const* op : saved_state.simplify(false))
                combined.append_copy(*op);
            native_state_mask = 0;
        }
        auto batch = operations.simplify(final_application, optimize);
        owner->deferred_retained += batch.size();
        for (auto const* op : batch)
            combined.append_copy(*op);
        auto const selected = combined.simplify(false, optimize);
        auto fail = [&](rhi::error error) noexcept { recording_error = error; executable = false; return error; };

        auto valid_pipeline = [&](owned_pipeline const* pipeline, std::uint64_t identity) {
            std::lock_guard lock(owner->contract_pipelines_mutex);
            return pipeline && owner->contract_pipelines.contains(pipeline) && pipeline->identity == identity;
        };
        auto valid_buffer = [&](rec::words const& data) {
            auto* buffer = reinterpret_cast<owned_buffer const*>(data[0]);
            std::lock_guard lock(owner->contract_buffers_mutex);
            return buffer && owner->contract_buffers.contains(buffer) && buffer->identity == data[1];
        };
        auto compatible_resources = [](owned_pipeline const* pipeline, rhi::resource_group_ref const& resources) {
            if (!resources || !pipeline)
                return !resources;
            auto const& group = *static_cast<owned_resource_group const*>(resources.get());
            if (pipeline->resource_layout.size() != group.entries.size())
                return false;
            for (std::size_t i = 0; i < group.entries.size(); ++i)
                if (pipeline->resource_layout[i].binding != group.entries[i].binding ||
                    pipeline->resource_layout[i].kind != group.entries[i].kind)
                    return false;
            return true;
        };

        // Validate the whole retained batch before any native command from this batch.
        auto* pipeline = emitted_pipeline;
        std::uint64_t pipeline_identity = native_state[static_cast<unsigned>(kind::pipeline)][1];
        auto resources = current_resources;
        auto textures = current_textures;
        deren::utility::vessel::vector<std::pair<std::uint64_t, std::uint64_t>> push_ranges;
        for (auto* op : saved_state.simplify(false))
            if (op->kind() == kind::push)
                push_ranges.emplace_back(op->data[0], op->bytes.size());
        auto vertex = native_state[static_cast<unsigned>(kind::vertex)];
        auto index = native_state[static_cast<unsigned>(kind::index)];
        bool has_vertex = (native_state_mask & rec::bit(kind::vertex)) != 0;
        bool has_index = (native_state_mask & rec::bit(kind::index)) != 0;
        for (auto* op : selected) {
            switch (op->kind()) {
            case kind::pipeline:
                pipeline = reinterpret_cast<owned_pipeline const*>(op->data[0]);
                pipeline_identity = op->data[1];
                if (!valid_pipeline(pipeline, pipeline_identity))
                    return fail(rhi::error::invalid_argument);
                break;
            case kind::vertex:
            case kind::index:
            case kind::draw_indirect:
            case kind::dispatch_indirect:
            case kind::mesh_indirect:
                if (!valid_buffer(op->data))
                    return fail(rhi::error::invalid_argument);
                if (op->kind() == kind::vertex) {
                    vertex = op->data;
                    has_vertex = true;
                }
                if (op->kind() == kind::index) {
                    index = op->data;
                    has_index = true;
                }
                break;
            case kind::resource_group:
                if (!valid_pipeline(pipeline, pipeline_identity) || !compatible_resources(pipeline, op->resources))
                    return fail(rhi::error::invalid_argument);
                resources = op->resources;
                break;
            case kind::texture_group:
                if (!valid_pipeline(pipeline, pipeline_identity) || !pipeline->group_binding.enabled)
                    return fail(rhi::error::invalid_argument);
                textures = op->textures;
                break;
            case kind::push:
                push_ranges.emplace_back(op->data[0], op->bytes.size());
                break;
            default:
                break;
            }
            if (!op->is_unique())
                continue;
            if (!valid_pipeline(pipeline, pipeline_identity))
                return fail(rhi::error::invalid_argument);
            auto const compute = op->kind() == kind::dispatch || op->kind() == kind::dispatch_indirect;
            auto const trace = op->kind() == kind::trace;
            if (pipeline->bind_point != (compute ? VK_PIPELINE_BIND_POINT_COMPUTE : trace ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
                                                                                          : VK_PIPELINE_BIND_POINT_GRAPHICS))
                return fail(rhi::error::invalid_argument);
            if (!compute && !trace && has_vertex && !valid_buffer(vertex))
                return fail(rhi::error::invalid_argument);
            if (op->kind() == kind::draw_indexed && has_index && !valid_buffer(index))
                return fail(rhi::error::invalid_argument);
            // A group is logical state. Pipelines that have no group layout do not consume it.
            if (!pipeline->resource_layout.empty() && !compatible_resources(pipeline, resources))
                return fail(rhi::error::invalid_argument);
            for (auto [offset, size] : push_ranges) {
                auto overlaps = [&](std::uint32_t reserved) { return offset < reserved + 4ull && offset + size > reserved; };
                for (auto const& binding : pipeline->resource_layout)
                    if (overlaps(binding.push_byte_offset))
                        return fail(rhi::error::invalid_argument);
                if (pipeline->group_binding.enabled && overlaps(pipeline->group_binding.push_byte_offset))
                    return fail(rhi::error::invalid_argument);
            }
        }

        auto const command = native();
        auto push = [&](std::uint32_t offset, std::span<std::byte const> bytes) {
            ++owner->emitted_setters;
            return owner->descriptor_heaps.push_data(command, offset, bytes);
        };
        auto publish_groups = [&]() {
            if (!emitted_pipeline)
                return true;
            if (!emitted_pipeline->resource_layout.empty() && current_resources && compatible_resources(emitted_pipeline, current_resources)) {
                auto const& group = *static_cast<owned_resource_group const*>(current_resources.get());
                owner->descriptor_heaps.record_bind(command);
                ++owner->emitted_setters;
                for (std::size_t i = 0; i < emitted_pipeline->resource_layout.size(); ++i)
                    if (!push(emitted_pipeline->resource_layout[i].push_byte_offset, std::as_bytes(std::span(&group.values[i], 1))))
                        return false;
                if (std::find(resource_refs.begin(), resource_refs.end(), current_resources) == resource_refs.end())
                    resource_refs.push_back(current_resources);
            }
            if (emitted_pipeline->group_binding.enabled) {
                std::uint32_t token = 0;
                if (current_textures) {
                    std::lock_guard lock(owner->texture_groups.mutex);
                    auto found = owner->texture_groups.tokens_by_group.find(current_textures.get());
                    if (found == owner->texture_groups.tokens_by_group.end())
                        return false;
                    token = found->second;
                }
                owner->descriptor_heaps.record_bind(command);
                ++owner->emitted_setters;
                if (!push(emitted_pipeline->group_binding.push_byte_offset, std::as_bytes(std::span(&token, 1))))
                    return false;
                if (current_textures && std::find(group_refs.begin(), group_refs.end(), current_textures) == group_refs.end())
                    group_refs.push_back(current_textures);
            }
            return true;
        };
        for (auto* op : selected) {
            auto const& d = op->data;
            auto type = op->kind();
            if (!op->is_unique() && type != kind::push && type != kind::resource_group && type != kind::texture_group) {
                auto const index = static_cast<unsigned>(type);
                auto const mask = rec::bit(type);
                if (optimize && (native_state_mask & mask) && native_state[index] == d)
                    continue;
                native_state[index] = d;
                native_state_mask |= mask;
            }
            switch (type) {
            case kind::pipeline: {
                emitted_pipeline = reinterpret_cast<owned_pipeline const*>(d[0]);
                vkCmdBindPipeline(command, emitted_pipeline->bind_point, emitted_pipeline->native_handle);
                ++owner->emitted_setters;
                // Some graphics pipelines use static depth bias, which invalidates the native
                // dynamic value. Re-establish the logical value after every graphics switch.
                if (emitted_pipeline->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS &&
                    (native_state_mask & rec::bit(kind::depth_bias))) {
                    auto v = rec::unpack<deren::utility::vessel::array<float, 3>>(native_state[static_cast<unsigned>(kind::depth_bias)]);
                    vkCmdSetDepthBias(command, v[0], v[2], v[1]);
                    ++owner->emitted_setters;
                }
                if (!emitted_pipeline->resource_layout.empty()) {
                    owner->descriptor_heaps.record_bind(command);
                    ++owner->emitted_setters;
                    std::uint32_t zero = 0;
                    for (auto const& binding : emitted_pipeline->resource_layout)
                        if (!push(binding.push_byte_offset, std::as_bytes(std::span(&zero, 1))))
                            return fail(rhi::error::operation_failed);
                }
                if (!publish_groups())
                    return fail(rhi::error::operation_failed);
                break;
            }
            case kind::vertex: {
                auto buffer = reinterpret_cast<owned_buffer const*>(d[0])->native;
                VkDeviceSize offset = d[2];
                vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
                ++owner->emitted_setters;
                break;
            }
            case kind::index:
                vkCmdBindIndexBuffer(command, reinterpret_cast<owned_buffer const*>(d[0])->native, d[2],
                                     d[3] == static_cast<std::uint64_t>(rhi::index_type::uint32) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                ++owner->emitted_setters;
                break;
            case kind::resource_group:
                current_resources = op->resources;
                if (!publish_groups())
                    return fail(rhi::error::operation_failed);
                break;
            case kind::texture_group:
                current_textures = op->textures;
                if (!publish_groups())
                    return fail(rhi::error::operation_failed);
                break;
            case kind::push:
                if (!push(static_cast<std::uint32_t>(d[0]), op->bytes))
                    return fail(rhi::error::operation_failed);
                break;
            case kind::viewport: {
                auto v = rec::unpack<deren::utility::vessel::array<float, 6>>(d);
                VkViewport viewport{v[0], v[1], v[2], v[3], v[4], v[5]};
                vkCmdSetViewport(command, 0, 1, &viewport);
                ++owner->emitted_setters;
                break;
            }
            case kind::scissor: {
                auto v = rec::unpack<deren::utility::vessel::array<std::uint32_t, 4>>(d);
                VkRect2D rect{{static_cast<std::int32_t>(v[0]), static_cast<std::int32_t>(v[1])}, {v[2], v[3]}};
                vkCmdSetScissor(command, 0, 1, &rect);
                ++owner->emitted_setters;
                break;
            }
            case kind::cull: {
                auto mode = static_cast<rhi::cull_mode>(d[0]);
                auto native_mode = mode == rhi::cull_mode::front ? VK_CULL_MODE_FRONT_BIT : mode == rhi::cull_mode::back ? VK_CULL_MODE_BACK_BIT
                                                                                        : mode == rhi::cull_mode::none   ? VK_CULL_MODE_NONE
                                                                                                                         : VK_CULL_MODE_FRONT_AND_BACK;
                vkCmdSetCullMode(command, native_mode);
                ++owner->emitted_setters;
                break;
            }
            case kind::depth_write:
                vkCmdSetDepthWriteEnable(command, d[0] ? VK_TRUE : VK_FALSE);
                ++owner->emitted_setters;
                break;
            case kind::depth_bias: {
                auto v = rec::unpack<deren::utility::vessel::array<float, 3>>(d);
                vkCmdSetDepthBias(command, v[0], v[2], v[1]);
                ++owner->emitted_setters;
                break;
            }
            case kind::draw:
                vkCmdDraw(command, d[0], d[1], d[2], d[3]);
                ++owner->emitted_work;
                break;
            case kind::draw_indexed:
                vkCmdDrawIndexed(command, d[0], d[1], d[2], static_cast<std::int32_t>(d[3]), d[4]);
                ++owner->emitted_work;
                break;
            case kind::dispatch:
                vkCmdDispatch(command, d[0], d[1], d[2]);
                ++owner->emitted_work;
                break;
            case kind::mesh:
                owner->mesh_dispatch(command, d[0], d[1], d[2]);
                ++owner->emitted_work;
                break;
            case kind::trace: {
                VkStridedDeviceAddressRegionKHR raygen{d[0], d[1], d[2]}, miss{d[3], d[4], d[5]}, hit{d[6], d[7], d[8]}, callable{d[9], d[10], d[11]};
                owner->ray_trace_launch(command, &raygen, &miss, &hit, &callable, d[12], d[13], d[14]);
                ++owner->emitted_work;
                break;
            }
            case kind::draw_indirect:
                for (std::uint32_t i = 0; i < d[3]; ++i) {
                    vkCmdDrawIndirect(command, reinterpret_cast<owned_buffer const*>(d[0])->native, d[2] + i * d[4], 1, d[4]);
                    ++owner->emitted_work;
                }
                break;
            case kind::dispatch_indirect:
                vkCmdDispatchIndirect(command, reinterpret_cast<owned_buffer const*>(d[0])->native, d[2]);
                ++owner->emitted_work;
                break;
            case kind::mesh_indirect:
                owner->mesh_dispatch_indirect(command, reinterpret_cast<owned_buffer const*>(d[0])->native, d[2], d[3], d[4]);
                ++owner->emitted_work;
                break;
            case kind::count:
                break;
            }
        }
        // Compact the canonical logical state; keep copied payloads and group owners for interop restore.
        for (auto* op : batch)
            if (!op->is_unique())
                saved_state.append_copy(*op);
        rec::operation_stream compact;
        for (auto* op : saved_state.simplify(false))
            compact.append_copy(*op);
        saved_state = std::move(compact);
        operations.clear();
        restore_state = false;
        return rhi::error::ok;
    }
} // namespace deren::vulkan
