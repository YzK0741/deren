module;
#include <algorithm>
#include <expected>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <span>
#include <vector>
module deren.engine.gaussian_splatting.raster;
import deren.utility;
namespace deren::engine::gaussian_splatting {
    namespace rhi = deren::promise::rhi;
    namespace {
        auto failure(error_code code, char const* message) {
            return std::unexpected(foundation_error{code, message});
        }
    } // namespace
    std::expected<std::shared_ptr<raster_asset const>, foundation_error> make_raster_asset(rhi::api_core& core, deren::gaussian::asset input, foundation_limits const& limits) {
        // THE VIEWER'S OWN PROGRESS, at the two stages that can be slow or refuse: a 562,974-splat cloud is
        // packed and uploaded once, and "which half" is the first question when it does not arrive.
        deren::utility::log("gaussian asset: packing {} particle(s) (SH degree {})", input.particles.size(), input.sh_degree);
        auto packed = pack_asset(input, limits);
        if (!packed) {
            return std::unexpected(packed.error());
        }
        deren::utility::log("gaussian asset: packed {} geometry record(s); uploading to the device", packed->geometry.size());
        auto uploaded = upload_asset(core, *packed, limits);
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        deren::utility::log("gaussian asset: the static tables are uploaded");
        std::shared_ptr<raster_asset> result(new raster_asset{});
        result->cpu_asset = std::make_shared<deren::gaussian::asset const>(std::move(input));
        result->gpu_owner = std::move(*uploaded);
        result->geometry = std::move(packed->geometry);
        return std::shared_ptr<raster_asset const>{std::move(result)};
    }
    std::expected<std::shared_ptr<raster_frame const>, foundation_error> prepare_raster_frame(rhi::api_core& core, std::span<raster_instance const> inputs, raster_camera const& camera, foundation_limits const& limits) {
        auto sort_view = validate_raster_camera(camera);
        if (!sort_view) {
            return std::unexpected(sort_view.error());
        }
        if (inputs.size() > std::numeric_limits<std::uint32_t>::max()) {
            return failure(error_code::limit_exceeded, "too many raster instances");
        }
        // CHECKED THE HOUSE WAY (see pack_asset): the count is compared against max/stride BEFORE the multiply, so a
        // size_t that would wrap cannot quietly pass the budget test below with a small product.
        if (inputs.size() > limits.max_packed_bytes / sizeof(instance_record) ||
            inputs.size() > std::numeric_limits<std::size_t>::max() / sizeof(instance_record)) {
            return failure(error_code::limit_exceeded, "raster instances exceed frame byte budget");
        }
        std::uint64_t const instance_bytes = inputs.size() * sizeof(instance_record);
        if (instance_bytes > limits.max_packed_bytes) {
            return failure(error_code::limit_exceeded, "raster instances exceed frame byte budget");
        }
        std::vector<sort_instance> sort_inputs;
        sort_inputs.reserve(inputs.size());
        std::vector<instance_record> records;
        records.reserve(inputs.size());
        for (auto const& input : inputs) {
            if (!input.asset) {
                return failure(error_code::invalid_argument, "null raster asset");
            }
            auto inverse = validate_model(input.model);
            if (!inverse) {
                return std::unexpected(inverse.error());
            }
            sort_inputs.push_back({&input.asset->source(), input.model, input.stable_id});
            auto const& gpu = input.asset->gpu();
            instance_record record;
            record.geometry_address = gpu->geometry_address();
            record.sh_address = gpu->sh_address();
            record.model = input.model;
            record.inverse_model = *inverse;
            record.sh_degree = gpu->sh_degree();
            records.push_back(record);
        }
        auto sorted = sort_draw_references(sort_inputs, *sort_view, limits);
        if (!sorted) {
            return std::unexpected(sorted.error());
        }
        std::shared_ptr<raster_frame> result(new raster_frame{});
        result->total = static_cast<std::uint32_t>(sorted->size());
        result->push.view_projection = camera.view_projection;
        result->push.camera_position = glm::vec4{camera.position, 1};
        result->push.viewport_width = static_cast<float>(camera.extent.width);
        result->push.viewport_height = static_cast<float>(camera.extent.height);
        result->reference_data.reserve(sorted->size());
        for (auto const& reference : *sorted) {
            auto const& input = inputs[reference.instance_index];
            auto projected = project_splat(input.asset->geometry_records()[reference.local_gaussian_index], input.model, camera);
            if (!projected) {
                return std::unexpected(projected.error());
            }
            if (projected->visibility == projection_visibility::near_omitted) {
                ++result->near_omitted;
            } else if (projected->visibility == projection_visibility::outside) {
                ++result->outside;
            } else {
                auto const& source = input.asset->source();
                auto const& particle = source.particles[reference.local_gaussian_index];
                glm::vec3 const center{particle.center[0], particle.center[1], particle.center[2]};
                glm::vec3 const eye{records[reference.instance_index].inverse_model * glm::vec4{camera.position, 1}};
                sh_record sh;
                sh.coefficients = particle.sh;
                auto radiance = evaluate_sh(sh, source.sh_degree, glm::normalize(center - eye));
                if (!radiance) {
                    return std::unexpected(radiance.error());
                }
                result->reference_data.push_back(reference);
            }
        }
        result->push.visible_count = static_cast<std::uint32_t>(result->reference_data.size());
        if (result->push.visible_count == 0) {
            return std::shared_ptr<raster_frame const>{std::move(result)};
        }
        // The same checked shape as the instance count above. The subtraction that follows cannot underflow:
        // instance_bytes was already capped by the byte budget.
        if (result->reference_data.size() > limits.max_packed_bytes / sizeof(draw_reference) ||
            result->reference_data.size() > std::numeric_limits<std::size_t>::max() / sizeof(draw_reference)) {
            return failure(error_code::limit_exceeded, "raster uploads exceed frame byte budget");
        }
        std::uint64_t const reference_bytes = result->reference_data.size() * sizeof(draw_reference);
        if (reference_bytes > limits.max_packed_bytes - instance_bytes) {
            return failure(error_code::limit_exceeded, "raster uploads exceed frame byte budget");
        }
        result->bytes = instance_bytes + reference_bytes;
        auto* const address = rhi::query_extension<rhi::device_address>(core);
        if (address == nullptr) {
            return failure(error_code::unsupported, "raster frame requires device addresses");
        }
        result->instance_data = std::move(records);
        result->owners.reserve(inputs.size());
        for (auto const& input : inputs) {
            result->owners.push_back(input.asset);
        }
        rhi::buffer_desc desc;
        desc.usage = rhi::buffer_usage::storage_coherent;
        desc.flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::device_address);
        desc.initial_bytes = std::as_bytes(std::span{result->instance_data});
        desc.size = instance_bytes;
        result->instance_buffer = rhi::object_manager<rhi::buffer>{core.create_buffer(desc)};
        if (!result->instance_buffer || result->instance_buffer->size() < desc.size) {
            return failure(error_code::allocation_failure, "raster instance upload failed");
        }
        result->push.instances_address = address->buffer_address(*result->instance_buffer, 0);
        if (result->push.instances_address == 0 || result->push.instances_address % alignof(instance_record) != 0) {
            return failure(error_code::invalid_address, "invalid raster instance address");
        }
        desc.initial_bytes = std::as_bytes(std::span{result->reference_data});
        desc.size = reference_bytes;
        result->reference_buffer = rhi::object_manager<rhi::buffer>{core.create_buffer(desc)};
        if (!result->reference_buffer || result->reference_buffer->size() < desc.size) {
            return failure(error_code::allocation_failure, "raster reference upload failed");
        }
        result->push.references_address = address->buffer_address(*result->reference_buffer, 0);
        if (result->push.references_address == 0 || result->push.references_address % alignof(draw_reference) != 0) {
            return failure(error_code::invalid_address, "invalid raster reference address");
        }
        return std::shared_ptr<raster_frame const>{std::move(result)};
    }
} // namespace deren::engine::gaussian_splatting
