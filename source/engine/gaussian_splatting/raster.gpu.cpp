module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
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
import deren.utility.shared;
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
            deren::utility::vessel::vector<std::thread> pool;
            pool.reserve(workers - 1u);
            for (std::size_t index = 1; index < workers; ++index) {
                std::size_t const begin = std::min(count, index * chunk);
                std::size_t const end = std::min(count, begin + chunk);
                if (begin >= end) {
                    break;
                }
                pool.emplace_back([&work, index, begin, end] {
                    static_cast<void>(deren::utility::diagnostics::prepare_crash_thread());
                    work(index, begin, end);
                });
            }
            work(std::size_t{0}, std::size_t{0}, std::min(count, chunk));
            for (std::thread& worker : pool) {
                worker.join();
            }
        }
    } // namespace
    std::expected<std::shared_ptr<raster_asset const>, foundation_error> make_raster_asset(rhi::api_core& core, deren::gaussian::asset input, foundation_limits const& limits, std::shared_ptr<rhi::api_core> device_lease) {
        if (device_lease && device_lease.get() != &core)
            return failure(error_code::invalid_argument, "Gaussian asset device lease belongs to another core");
        // THE VIEWER'S OWN PROGRESS, at the two stages that can be slow or refuse: a 562,974-splat cloud is
        // packed and uploaded once, and "which half" is the first question when it does not arrive.
        deren::utility::log("gaussian asset: packing {} particle(s) (SH degree {})", input.particles.size(), input.sh_degree);
        auto packed = pack_asset(input, limits);
        if (!packed) {
            return std::unexpected(packed.error());
        }
        deren::utility::log("gaussian asset: packed {} geometry record(s); uploading to the device", packed->geometry.size());
        auto packed_owner = std::make_shared<packed_asset>(std::move(*packed));
        auto uploaded = upload_asset(core, std::shared_ptr<packed_asset const>{packed_owner}, limits, std::move(device_lease));
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        deren::utility::log("gaussian asset: the static tables are uploaded");
        std::shared_ptr<raster_asset> result(new raster_asset{});
        result->cpu_asset = std::make_shared<deren::gaussian::asset const>(std::move(input));
        result->gpu_owner = std::move(*uploaded);
        result->geometry = std::move(packed_owner->geometry);
        return std::shared_ptr<raster_asset const>{std::move(result)};
    }
    std::expected<std::shared_ptr<raster_frame const>, foundation_error> prepare_raster_frame(rhi::api_core& core, std::span<raster_instance const> inputs, raster_camera const& camera, foundation_limits const& limits, raster_frame* const reuse, std::shared_ptr<rhi::api_core> device_lease) {
        if (device_lease && device_lease.get() != &core)
            return failure(error_code::invalid_argument, "Raster device lease belongs to another core");
        if (reuse && reuse->owner != &core)
            return failure(error_code::invalid_argument, "Raster frame belongs to another core");
        if (reuse && device_lease && !reuse->device_lease && reuse->storage_view.size())
            return failure(error_code::invalid_argument, "Legacy raster storage cannot gain an owning device lease; prepare a new frame");
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
        deren::utility::vessel::vector<sort_instance> sort_inputs;
        sort_inputs.reserve(inputs.size());
        deren::utility::vessel::vector<instance_record> records;
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
        // Stage CPU results so rejection cannot clear the completed slot's last valid frame.
        raster_frame staged;
        std::shared_ptr<raster_frame> created;
        if (!reuse)
            created = std::shared_ptr<raster_frame>{new raster_frame{}};
        auto* const result = reuse ? &staged : created.get();
        auto* const target = reuse ? reuse : result;
        if (reuse) {
            result->reference_data.swap(reuse->staging_reference_data);
            result->owners.swap(reuse->staging_owners);
        }
        struct recycle_workspace {
            bool enabled;
            deren::utility::vessel::vector<draw_reference>& saved_references;
            deren::utility::vessel::vector<draw_reference>& working_references;
            deren::utility::vessel::vector<std::shared_ptr<raster_asset const>>& saved_owners;
            deren::utility::vessel::vector<std::shared_ptr<raster_asset const>>& working_owners;
            ~recycle_workspace() {
                if (enabled) {
                    working_references.clear();
                    working_owners.clear();
                    saved_references.swap(working_references);
                    saved_owners.swap(working_owners);
                }
            }
        } workspace{reuse != nullptr, target->staging_reference_data, result->reference_data, target->staging_owners, result->owners};
        if (device_lease)
            target->device_lease = device_lease;
        auto const publish = [&]() -> std::shared_ptr<raster_frame const> {
            target->owner = &core;
            if (!reuse)
                return created;
            reuse->push = result->push;
            reuse->reference_data.swap(result->reference_data);
            reuse->instance_data = std::move(result->instance_data);
            reuse->owners.swap(result->owners);
            reuse->bytes = result->bytes;
            reuse->total = result->total;
            reuse->near_omitted = result->near_omitted;
            reuse->outside = result->outside;
            return {reuse, [](raster_frame const*) {}};
        };
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
            deren::utility::vessel::vector<draw_reference> references;
            std::uint32_t near_omitted = 0;
            std::uint32_t outside = 0;
            std::optional<foundation_error> failure = {};
        };
        std::size_t const total = sorted->size();
        std::size_t const workers = raster_worker_count(total);
        deren::utility::vessel::vector<projected_chunk> chunks(workers);
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
        if (std::getenv("DEREN_GAUSSIAN_CPU_TRACE")) {
            deren::utility::log("gaussian frame cost: gather {:.1f} ms, sort {:.1f} ms, project+SH {:.1f} ms ({} visible of {})",
                                std::chrono::duration<double, std::milli>(sort_started - frame_started).count(), std::chrono::duration<double, std::milli>(sorted_at - sort_started).count(),
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sorted_at).count(), result->reference_data.size(), sorted->size());
        }
        if (result->push.visible_count == 0) {
            return publish();
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
        namespace managed = render_resource::instances;
        deren::utility::vessel::array<managed::request, 2> requests;
        deren::utility::vessel::array ids{render_resource::resource_id::gaussian_instances, render_resource::resource_id::gaussian_references};
        deren::utility::vessel::array sizes{instance_bytes, reference_bytes};
        bool rebuild = false;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& request = requests[i];
            request.id = ids[i];
            request.type = managed::kind::buffer;
            request.life = managed::lifetime::frame_slot;
            request.scope = render_resource::resource_scope::device_wide;
            request.buffer.size = sizes[i];
            request.buffer.usage = rhi::buffer_usage::storage_coherent;
            request.buffer.flags = rhi::to_bits(rhi::buffer_flag::storage) | rhi::to_bits(rhi::buffer_flag::device_address);
            auto old = target->storage_view.find(ids[i]);
            rebuild |= !old || old->objects[0].buffer->size() < sizes[i];
        }
        deren::utility::vessel::array<rhi::buffer*, 2> buffers{};
        deren::utility::vessel::array<std::uint64_t, 2> addresses{};
        error_code admission_code = error_code::allocation_failure;
        char const* admission_message = "Raster upload buffer creation failed";
        auto const admit = [&](managed::resource_snapshot const& snapshot) {
            for (std::size_t i = 0; i < requests.size(); ++i) {
                auto generation = snapshot.find(ids[i]);
                buffers[i] = generation && generation->objects.size() == 1 ? generation->objects[0].buffer.get() : nullptr;
                if (!buffers[i] || buffers[i]->size() < sizes[i] || buffers[i]->mapped().size() < sizes[i])
                    return rhi::error::unsupported;
                addresses[i] = address->buffer_address(*buffers[i], 0);
                auto const alignment = i == 0 ? alignof(instance_record) : alignof(draw_reference);
                if (!addresses[i] || addresses[i] % alignment) {
                    admission_code = error_code::invalid_address;
                    admission_message = "Invalid raster upload address";
                    return rhi::error::unsupported;
                }
            }
            return rhi::error::ok;
        };
        auto pending = target->storage_view;
        if (rebuild) {
            if (!target->storage_provider)
                target->storage_provider = &target->storage_chain.emplace<deren::engine::pass::buffer_storage_pass>();
            target->storage_provider->configure(requests);
            auto root = target->device_lease ? target->device_lease : std::shared_ptr<rhi::api_core>{&core, [](rhi::api_core*) {}};
            if (target->storage_chain.prepare_resources(root, managed::resource_context{.frame_slots = 1, .output_images = 1}, admit) != rhi::error::ok)
                return failure(admission_code, admission_message);
            pending = target->storage_chain.resources().snapshot();
        } else if (admit(pending) != rhi::error::ok) {
            return failure(admission_code, admission_message);
        }
        std::memcpy(buffers[0]->mapped().data(), result->instance_data.data(), static_cast<std::size_t>(instance_bytes));
        std::memcpy(buffers[1]->mapped().data(), result->reference_data.data(), static_cast<std::size_t>(reference_bytes));
        result->push.instances_address = addresses[0];
        result->push.references_address = addresses[1];
        target->instance_buffer = {buffers[0]};
        target->reference_buffer = {buffers[1]};
        target->storage_view = pending;
        return publish();
    }
} // namespace deren::engine::gaussian_splatting
