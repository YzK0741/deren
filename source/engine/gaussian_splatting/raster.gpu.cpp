module;
#include <algorithm>
#include <chrono>
#include <cstring>
#include <expected>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>
module deren.engine.gaussian_splatting.raster;
import deren.utility;
namespace deren::engine::gaussian_splatting {
    namespace rhi = deren::promise::rhi;
    namespace {
        auto failure(error_code code, char const* message) {
            return std::unexpected(foundation_error{code, message});
        }

        /// the worker count a frame's per-splat work wants: one per hardware thread, capped at 16 so a huge machine
        /// does not spawn a hundred threads for one cloud - and 1 when the cloud is small enough that spawning them
        /// would cost more than the work they share.
        /// @note MEASURED, not assumed: this cloud is 562,974 splats and the per-splat pass below spent 240 ms of a
        ///       334 ms frame on ONE thread (the sort took the other 94 ms), which is the 3 fps the panel showed.
        [[nodiscard]] std::size_t raster_worker_count(std::size_t const count) noexcept {
            unsigned const hardware = std::thread::hardware_concurrency();
            std::size_t const workers = std::min<std::size_t>(hardware == 0u ? 1u : hardware, 16u);
            return count < 8192u ? 1u : workers;
        }

        /// run @p work(index, begin, end) over @p count items, chunk 0 on the CALLING thread
        ///
        /// Every worker owns one index and one half-open range, so nothing is shared but the range boundaries: each
        /// writes into its own storage and the caller decides what the order between them means. That is what keeps a
        /// parallel pass byte-for-byte the same as the serial one where the order is observable (the draw order).
        template <typename F>
        void run_in_parallel(std::size_t const workers, std::size_t const count, F const& work) {
            if (workers <= 1u) {
                work(std::size_t{0}, std::size_t{0}, count);
                return;
            }
            std::size_t const chunk = (count + workers - 1u) / workers;
            std::vector<std::thread> pool;
            pool.reserve(workers - 1u);
            for (std::size_t index = 1; index < workers; ++index) {
                std::size_t const begin = std::min(count, index * chunk);
                std::size_t const end = std::min(count, begin + chunk);
                if (begin >= end) {
                    break;
                }
                pool.emplace_back([&work, index, begin, end] { work(index, begin, end); });
            }
            work(std::size_t{0}, std::size_t{0}, std::min(count, chunk));
            for (std::thread& worker : pool) {
                worker.join();
            }
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
    std::expected<std::shared_ptr<raster_frame const>, foundation_error> prepare_raster_frame(rhi::api_core& core, std::span<raster_instance const> inputs, raster_camera const& camera, foundation_limits const& limits, raster_frame* const reuse) {
        auto const frame_started = std::chrono::steady_clock::now();
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
        auto const sort_started = std::chrono::steady_clock::now();
        auto sorted = sort_draw_references(sort_inputs, *sort_view, limits);
        if (!sorted) {
            return std::unexpected(sorted.error());
        }
        auto const sorted_at = std::chrono::steady_clock::now();
        // THE CALLER'S FRAME, WHEN IT GAVE ONE: its buffers are the frame's own, so a steady-state frame writes into
        // them in place and allocates nothing (Task 8). The shared_ptr is a NON-OWNING view in that case - the caller
        // owns the frame - and the counters and containers start clean either way, because a reused frame still holds
        // the previous frame's answer.
        std::shared_ptr<raster_frame> result{reuse, [](raster_frame*) {}};
        if (reuse == nullptr) {
            result = std::shared_ptr<raster_frame>{new raster_frame{}};
        } else {
            result->reference_data.clear();
            result->instance_data.clear();
            result->owners.clear();
            result->bytes = 0;
            result->total = 0;
            result->near_omitted = 0;
            result->outside = 0;
        }
        result->total = static_cast<std::uint32_t>(sorted->size());
        result->push.view_projection = camera.view_projection;
        result->push.camera_position = glm::vec4{camera.position, 1};
        result->push.viewport_width = static_cast<float>(camera.extent.width);
        result->push.viewport_height = static_cast<float>(camera.extent.height);
        // ---- THE PER-SPLAT PASS, ON EVERY CORE IT CAN HAVE ----
        // The work is INDEPENDENT PER SPLAT and it is the frame's whole cost (see raster_worker_count's note). Each
        // worker fills its own vector and counts its own rejections; the caller then concatenates those vectors IN
        // CHUNK ORDER and sums the counts, so the draw order and the reported totals are exactly the serial ones -
        // the sort's answer, not the schedule's.
        struct projected_chunk {
            std::vector<draw_reference> references;
            std::uint32_t near_omitted = 0;
            std::uint32_t outside = 0;
            std::optional<foundation_error> failure = {};
        };
        std::size_t const total = sorted->size();
        std::size_t const workers = raster_worker_count(total);
        std::vector<projected_chunk> chunks(workers);
        run_in_parallel(workers, total, [&](std::size_t const index, std::size_t const begin, std::size_t const end) {
            projected_chunk& chunk = chunks[index];
            chunk.references.reserve(end - begin);
            for (std::size_t i = begin; i < end; ++i) {
                auto const& reference = (*sorted)[i];
                auto const& input = inputs[reference.instance_index];
                auto projected = project_splat(input.asset->geometry_records()[reference.local_gaussian_index], input.model, camera);
                if (!projected) {
                    chunk.failure = projected.error();
                    return;
                }
                if (projected->visibility == projection_visibility::near_omitted) {
                    ++chunk.near_omitted;
                } else if (projected->visibility == projection_visibility::outside) {
                    ++chunk.outside;
                } else {
                    auto const& source = input.asset->source();
                    auto const& particle = source.particles[reference.local_gaussian_index];
                    glm::vec3 const center{particle.center[0], particle.center[1], particle.center[2]};
                    glm::vec3 const eye{records[reference.instance_index].inverse_model * glm::vec4{camera.position, 1}};
                    sh_record sh;
                    sh.coefficients = particle.sh;
                    auto radiance = evaluate_sh(sh, source.sh_degree, glm::normalize(center - eye));
                    if (!radiance) {
                        chunk.failure = radiance.error();
                        return;
                    }
                    chunk.references.push_back(reference);
                }
            }
        });
        result->reference_data.reserve(total);
        for (projected_chunk& chunk : chunks) {
            if (chunk.failure) {
                return std::unexpected(*chunk.failure);
            }
            result->near_omitted += chunk.near_omitted;
            result->outside += chunk.outside;
            result->reference_data.insert(result->reference_data.end(), chunk.references.begin(), chunk.references.end());
        }
        result->push.visible_count = static_cast<std::uint32_t>(result->reference_data.size());
        deren::utility::log("gaussian frame cost: gather {:.1f} ms, sort {:.1f} ms, project+SH {:.1f} ms ({} visible of {})",
                            std::chrono::duration<double, std::milli>(sort_started - frame_started).count(), std::chrono::duration<double, std::milli>(sorted_at - sort_started).count(),
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sorted_at).count(), result->reference_data.size(), sorted->size());
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
        if (result->instance_buffer && result->instance_capacity >= instance_bytes) {
            // IN PLACE: host-visible, so `mapped()` is the write the contract names for exactly this, and the bytes
            // may be touched once the frame that wrote them has completed - which the caller's per-slot ring
            // guarantees (the node the contract's own comment attaches to `mapped()`).
            std::span<std::byte> const mapped = result->instance_buffer->mapped();
            if (mapped.size() < instance_bytes) {
                return failure(error_code::allocation_failure, "raster instance buffer is not mapped");
            }
            std::memcpy(mapped.data(), result->instance_data.data(), static_cast<std::size_t>(instance_bytes));
        } else {
            desc.initial_bytes = std::as_bytes(std::span{result->instance_data});
            desc.size = instance_bytes;
            result->instance_buffer = rhi::object_manager<rhi::buffer>{core.create_buffer(desc)};
            result->instance_capacity = instance_bytes;
        }
        if (!result->instance_buffer || result->instance_buffer->size() < instance_bytes) {
            return failure(error_code::allocation_failure, "raster instance upload failed");
        }
        result->push.instances_address = address->buffer_address(*result->instance_buffer, 0);
        if (result->push.instances_address == 0 || result->push.instances_address % alignof(instance_record) != 0) {
            return failure(error_code::invalid_address, "invalid raster instance address");
        }
        if (result->reference_buffer && result->reference_capacity >= reference_bytes) {
            std::span<std::byte> const mapped = result->reference_buffer->mapped();
            if (mapped.size() < reference_bytes) {
                return failure(error_code::allocation_failure, "raster reference buffer is not mapped");
            }
            std::memcpy(mapped.data(), result->reference_data.data(), static_cast<std::size_t>(reference_bytes));
        } else {
            desc.initial_bytes = std::as_bytes(std::span{result->reference_data});
            desc.size = reference_bytes;
            result->reference_buffer = rhi::object_manager<rhi::buffer>{core.create_buffer(desc)};
            result->reference_capacity = reference_bytes;
        }
        if (!result->reference_buffer || result->reference_buffer->size() < reference_bytes) {
            return failure(error_code::allocation_failure, "raster reference upload failed");
        }
        result->push.references_address = address->buffer_address(*result->reference_buffer, 0);
        if (result->push.references_address == 0 || result->push.references_address % alignof(draw_reference) != 0) {
            return failure(error_code::invalid_address, "invalid raster reference address");
        }
        return std::shared_ptr<raster_frame const>{std::move(result)};
    }
} // namespace deren::engine::gaussian_splatting
