module;
#include <algorithm>
#include <array>
#include <cstring>
#include <glm/glm.hpp>
#include <numeric>
#include <utility>
module deren.engine.gaussian_splatting.gpu_frame;
import deren.utility.shared;
namespace deren::engine::gaussian_splatting {
    namespace rhi = deren::promise::rhi;
    namespace {
        auto failure(error_code code, char const* message) {
            return std::unexpected(foundation_error{code, message});
        }
    } // namespace
    std::expected<std::shared_ptr<gpu_frame>, foundation_error> prepare_gpu_frame(rhi::api_core& core, std::span<raster_instance const> inputs, raster_camera const& camera, foundation_limits const& limits, gpu_frame* reuse, rhi::shared_object<rhi::api_core> device_lease) {
        if (device_lease && device_lease.get() != &core) {
            return failure(error_code::invalid_argument, "GPU device lease belongs to another core");
        }
        auto view = validate_raster_camera(camera);
        if (!view) {
            return std::unexpected(view.error());
        }
        if (reuse && reuse->owner != &core) {
            return failure(error_code::invalid_argument, "GPU frame belongs to another core");
        }
        if (reuse && device_lease && !reuse->device_lease && reuse->storage_view.size()) {
            return failure(error_code::invalid_argument, "Legacy GPU storage cannot gain an owning device lease; prepare a new frame");
        }
        if (inputs.size() > UINT32_MAX || inputs.size() > limits.max_packed_bytes / (sizeof(instance_record) + sizeof(gpu_instance_range))) {
            return failure(error_code::limit_exceeded, "GPU instance tables exceed budget");
        }
        deren::utility::vessel::vector<std::uint32_t> order(inputs.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return inputs[a].stable_id < inputs[b].stable_id; });
        deren::utility::vessel::vector<instance_record> records(inputs.size());
        deren::utility::vessel::vector<gpu_instance_range> range_data;
        deren::utility::vessel::vector<std::shared_ptr<raster_asset const>> owners;
        std::uint64_t count = 0;
        for (std::size_t position = 0; position < order.size(); ++position) {
            auto const index = order[position];
            auto const& input = inputs[index];
            if (!input.asset || (position && inputs[order[position - 1]].stable_id == input.stable_id)) {
                return failure(error_code::invalid_argument, "GPU instances require nonnull assets and unique stable IDs");
            }
            auto inverse = validate_model(input.model);
            if (!inverse) {
                return std::unexpected(inverse.error());
            }
            auto const& asset = input.asset->gpu();
            std::uint64_t const next = count + asset->particle_count();
            if (next > limits.max_references || next > 65535ull * 128u) {
                return failure(error_code::limit_exceeded, "GPU candidate count exceeds budget");
            }
            if (next != count) {
                range_data.push_back({static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(next), index, 0u});
            }
            count = next;
            records[index] = {asset->geometry_address(), asset->sh_address(), input.model, *inverse, asset->sh_degree(), {}};
            owners.push_back(input.asset);
        }
        std::shared_ptr<gpu_frame> result = reuse ? reuse->shared_from_this() : std::shared_ptr<gpu_frame>{new gpu_frame{}};
        if (device_lease) {
            result->device_lease = device_lease;
        }
        if (count == 0) {
            result->owner = &core;
            result->push = {};
            result->owners = std::move(owners);
            return result;
        }
        auto* sorting = rhi::query_extension<rhi::gpu_sorting>(core);
        auto* address = rhi::query_extension<rhi::device_address>(core);
        if (!sorting || !address) {
            return failure(error_code::unsupported, "GPU frame needs sorting and device addresses");
        }
        if (!result->sorter) {
            result->sorter = rhi::unique_object<rhi::radix_sorter>{sorting->create_radix_sorter()};
        }
        if (!result->sorter) {
            return failure(error_code::unsupported, "GPU sorter initialization failed");
        }
        auto const req = result->sorter->requirements(static_cast<std::uint32_t>(count));
        if (!req.scratch_bytes) {
            return failure(error_code::unsupported, "GPU sort count exceeds backend limits");
        }
        deren::utility::vessel::array<std::uint64_t, gpu_frame::buffer_count> const sizes = {
            records.size() * sizeof(instance_record), range_data.size() * sizeof(gpu_instance_range), count * 4u, count * 4u,
            count * sizeof(draw_reference), req.scratch_bytes, 16u, sizeof(rhi::draw_indirect_command), sizeof(gpu_prepare_parameters), count * sizeof(gpu_projected_splat)};
        std::uint64_t bytes = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            auto const capacity = result->buffers[i] ? std::max(sizes[i], result->buffers[i]->size()) : sizes[i];
            if (capacity > limits.max_packed_bytes - bytes) {
                return failure(error_code::limit_exceeded, "GPU frame including sort scratch exceeds byte budget");
            }
            bytes += capacity;
        }
        namespace managed = render_resource::instances;
        using render_resource::resource_id;
        static constexpr deren::utility::vessel::array ids{resource_id::gaussian_instances, resource_id::gaussian_ranges,
                                                           resource_id::gaussian_keys, resource_id::gaussian_values, resource_id::gaussian_references,
                                                           resource_id::gaussian_sort_scratch, resource_id::gaussian_status, resource_id::gaussian_draw,
                                                           resource_id::gaussian_parameters, resource_id::gaussian_projected};
        deren::utility::vessel::array<managed::request, gpu_frame::buffer_count> requests;
        auto const flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::device_address);
        bool rebuild = false;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            auto& request = requests[i];
            request.id = ids[i];
            request.type = managed::kind::buffer;
            request.life = managed::lifetime::frame_slot;
            request.scope = render_resource::resource_scope::device_wide; // one completed slot's subchain
            bool const host = i == gpu_frame::instances || i == gpu_frame::ranges || i == gpu_frame::status || i == gpu_frame::parameters;
            request.buffer.size = sizes[i];
            request.buffer.usage = host ? rhi::buffer_usage::storage_coherent : rhi::buffer_usage::storage_gpu_only;
            request.buffer.flags = flags;
            if (i == gpu_frame::scratch) {
                request.buffer.flags |= req.scratch_flags;
            }
            if (i == gpu_frame::draw) {
                request.buffer.flags |= rhi::to_bits(rhi::buffer_flag::indirect);
            }
            if (i == gpu_frame::keys || i == gpu_frame::values || i == gpu_frame::references || i == gpu_frame::draw) {
                request.buffer.flags |= rhi::to_bits(rhi::buffer_flag::transfer_source);
            }
            auto previous = result->storage_view.find(ids[i]);
            rebuild |= !previous || previous->objects[0].buffer->size() < sizes[i] ||
                       previous->description.buffer.flags != request.buffer.flags || previous->description.buffer.usage != request.buffer.usage;
        }
        deren::utility::vessel::array<rhi::buffer*, gpu_frame::buffer_count> candidates{};
        deren::utility::vessel::array<std::uint64_t, gpu_frame::buffer_count> addresses{};
        error_code admission_code = error_code::allocation_failure;
        char const* admission_message = "GPU frame buffer creation failed";
        auto const admit = [&](managed::resource_snapshot const& candidate) {
            bytes = 0;
            for (std::size_t i = 0; i < sizes.size(); ++i) {
                auto generation = candidate.find(ids[i]);
                candidates[i] = generation && generation->objects.size() == 1 ? generation->objects[0].buffer.get() : nullptr;
                if (!candidates[i] || candidates[i]->size() < sizes[i]) {
                    admission_message = "GPU frame buffer is undersized";
                    return rhi::error::unsupported;
                }
                auto const capacity = candidates[i]->size();
                if (capacity > limits.max_packed_bytes - bytes) {
                    admission_code = error_code::limit_exceeded;
                    admission_message = "GPU frame including sort scratch exceeds byte budget";
                    return rhi::error::unsupported;
                }
                bytes += capacity;
                addresses[i] = address->buffer_address(*candidates[i], 0);
                if (!addresses[i] || addresses[i] % 16u) {
                    admission_code = error_code::invalid_address;
                    admission_message = "GPU frame buffer address is not aligned";
                    return rhi::error::unsupported;
                }
                if (requests[i].buffer.usage == rhi::buffer_usage::storage_coherent && candidates[i]->mapped().size() < sizes[i]) {
                    admission_message = "GPU frame requires host mapped parameter and status buffers";
                    return rhi::error::unsupported;
                }
            }
            return rhi::error::ok;
        };
        auto pending = result->storage_view;
        if (rebuild) {
            if (!result->storage_provider) {
                result->storage_provider = &result->storage_chain.emplace<deren::engine::pass::buffer_storage_pass>();
            }
            result->storage_provider->configure(requests);
            auto root = result->device_lease ? result->device_lease : rhi::shared_object<rhi::api_core>::borrow(*&core);
            if (result->storage_chain.prepare_resources(root, managed::resource_context{.frame_slots = 1, .output_images = 1}, admit) != rhi::error::ok) {
                return failure(admission_code, admission_message);
            }
            pending = result->storage_chain.resources().snapshot();
        } else if (admit(pending) != rhi::error::ok) {
            return failure(admission_code, admission_message);
        }
        auto const get = [&](std::size_t index) { return candidates[index]; };
        // Reserved instance lanes carry this instance's cache base; references keep their existing layout.
        for (auto const& range : range_data) {
            auto const cached = addresses[gpu_frame::projected] + std::uint64_t{range.begin} * sizeof(gpu_projected_splat);
            records[range.instance_index].padding[0] = static_cast<std::uint32_t>(cached);
            records[range.instance_index].padding[1] = static_cast<std::uint32_t>(cached >> 32u);
        }
        gpu_prepare_parameters data;
        data.raster = {.view_projection = camera.view_projection, .camera_position = glm::vec4{camera.position, 1}, .references_address = addresses[gpu_frame::references], .instances_address = addresses[gpu_frame::instances], .viewport_width = static_cast<float>(camera.extent.width), .viewport_height = static_cast<float>(camera.extent.height), .visible_count = static_cast<std::uint32_t>(count), .flags = 0u};
        data.forward = glm::vec4{view->forward, 0};
        data.ranges_address = addresses[gpu_frame::ranges];
        data.keys_address = addresses[gpu_frame::keys];
        data.values_address = addresses[gpu_frame::values];
        data.status_address = addresses[gpu_frame::status];
        data.draw_address = addresses[gpu_frame::draw];
        data.candidate_count = static_cast<std::uint32_t>(count);
        data.range_count = static_cast<std::uint32_t>(range_data.size());
        std::memcpy(get(gpu_frame::instances)->mapped().data(), records.data(), static_cast<std::size_t>(sizes[gpu_frame::instances]));
        std::memcpy(get(gpu_frame::ranges)->mapped().data(), range_data.data(), static_cast<std::size_t>(sizes[gpu_frame::ranges]));
        std::memcpy(get(gpu_frame::parameters)->mapped().data(), &data, sizeof(data));
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            result->buffers[i] = {candidates[i]};
        }
        result->storage_view = pending;
        result->owner = &core;
        result->owners = std::move(owners);
        result->push = data.raster;
        result->push.flags = 1u; // Raster reads the completed projection cache; compute still uses flags=0.
        result->parameter_address = addresses[gpu_frame::parameters];
        result->bytes = bytes;
        return result;
    }
    rhi::error gpu_frame::record_preparation(rhi::command_buffer& list, gpu_prepare_pipelines const& pipelines, deren::engine::pass::resolved_io::push_endpoint const& push_block, gpu_frame_timing const& timing) const {
        struct timing_scope {
            gpu_frame_timing const& points;
            std::uint32_t next = 0;
            void mark() {
                if (points.mark) {
                    points.mark(points.owner, next);
                }
                ++next;
            }
            ~timing_scope() {
                while (next < 3u) {
                    mark();
                }
            }
        } marks{timing};
        if (!candidate_count()) {
            return rhi::error::ok;
        }
        if (!pipelines.ready() || !push_block.push || !sorter) {
            return rhi::error::not_ready;
        }
        auto launch = [&](rhi::pipeline& pipeline, std::uint32_t groups) {
            auto error = list.bind_pipeline(pipeline);
            if (error != rhi::error::ok) {
                return error;
            }
            if (!push_block(list, std::as_bytes(std::span{&parameter_address, 1}))) {
                return rhi::error::unsupported;
            }
            list.dispatch(groups, 1, 1);
            return rhi::error::ok;
        };
        auto error = launch(*pipelines.reset, 1u);
        if (error != rhi::error::ok) {
            return error;
        }
        rhi::buffer_barrier const status_ready{.resource = buffers[status].get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::shader_read_write};
        error = list.barrier(rhi::barrier_group{.buffers = std::span{&status_ready, 1}});
        if (error != rhi::error::ok) {
            return error;
        }
        std::uint32_t const groups = (candidate_count() - 1u) / 128u + 1u;
        error = launch(*pipelines.prepare, groups);
        if (error != rhi::error::ok) {
            return error;
        }
        marks.mark();
        error = sorter->record(list, {.element_count = candidate_count(), .keys = buffers[keys].get(), .values = buffers[values].get(), .scratch = buffers[scratch].get()});
        if (error != rhi::error::ok) {
            return error;
        }
        // Sorting overwrites native bindings; every subsequent stage binds and pushes again.
        marks.mark();
        error = launch(*pipelines.finalize, groups);
        if (error != rhi::error::ok) {
            return error;
        }
        deren::utility::vessel::array<rhi::buffer_barrier, 4> const publish = {
            rhi::buffer_barrier{.resource = buffers[references].get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::shader_read},
            rhi::buffer_barrier{.resource = buffers[projected].get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::shader_read},
            rhi::buffer_barrier{.resource = buffers[draw].get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::indirect_read},
            rhi::buffer_barrier{.resource = buffers[status].get(), .from = rhi::buffer_use::shader_read_write, .to = rhi::buffer_use::host_read}};
        return list.barrier(rhi::barrier_group{.buffers = publish});
    }
    gpu_frame_stats gpu_frame::completed_stats(std::uint64_t frame_id) const noexcept {
        gpu_frame_stats result;
        result.frame_id = frame_id;
        if (candidate_count() && buffers[status] && buffers[status]->mapped().size() >= 16u) {
            std::memcpy(&result, buffers[status]->mapped().data(), 16u);
        }
        return result;
    }
} // namespace deren::engine::gaussian_splatting
