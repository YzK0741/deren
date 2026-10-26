module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <glm/glm.hpp>
#include <span>
module deren.engine.runtime;
import deren.engine.texture_residency;
import deren.promise.rhi;
import deren.utility.shared;

namespace deren::engine {
    namespace rhi = deren::promise::rhi;
    static constexpr std::uint32_t feedback_slot = render_layout::heap_slot_base + 1364;

    rhi::error runtime::prepare_texture_feedback() {
        if (!rhi::query_extension<rhi::frame_checkpoint>(rhi_face()))
            return rhi::error::unsupported;
        if (!visibility_feedback_pipeline) {
            auto code = registered_shader("visibility_feedback.comp.spv");
            if (code.empty())
                return rhi::error::not_ready;
            rhi::pipeline_desc desc{};
            desc.first_stage = rhi::shader_stage::compute;
            desc.compute_code = std::as_bytes(code);
            desc.debug_name = "visibility material feedback";
            visibility_feedback_pipeline = rhi_face().resources->create_pipeline(desc);
            visibility_feedback_buffer = rhi::shared_object<rhi::buffer>(rhi_face().resources->create_buffer(
                {.size = sizeof(texture_residency::feedback), .usage = rhi::buffer_usage::storage_coherent, .flags = rhi::to_bits(rhi::buffer_flag::device_address)}));
            if (!visibility_feedback_pipeline || !visibility_feedback_buffer)
                return rhi::error::operation_failed;
            auto* descriptors = rhi::query_extension<rhi::descriptor_space>(rhi_face());
            auto result = descriptors ? descriptors->write_buffer({.slot = feedback_slot, .resource = visibility_feedback_buffer.get(), .size = sizeof(texture_residency::feedback)}) : rhi::error::unsupported;
            if (result != rhi::error::ok)
                return result;
        }
        auto mapped = visibility_feedback_buffer->mapped();
        if (mapped.size() < sizeof(texture_residency::feedback))
            return rhi::error::operation_failed;
        std::memset(mapped.data(), 0, sizeof(texture_residency::feedback));
        return rhi::error::ok;
    }

    rhi::error runtime::record_texture_feedback(rhi::command_buffer& commands) {
        auto result = commands.bind_pipeline(*visibility_feedback_pipeline);
        if (result != rhi::error::ok)
            return result;
        auto* descriptors = rhi::query_extension<rhi::descriptor_space>(rhi_face());
        result = descriptors ? descriptors->bind({.commands = &commands}) : rhi::error::unsupported;
        if (result != rhi::error::ok)
            return result;
        std::array<std::uint32_t, 6> push{frame_ring().position(), current_image_index, software_raster_active_frame ? 1u : 0u, feedback_slot, material_count, visibility_recorded_item_count};
        result = commands.push_constants(0, std::as_bytes(std::span(push)));
        if (result != rhi::error::ok)
            return result;
        auto extent = render_extent();
        commands.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        // Host publication is this pass's epilogue. The checkpoint establishes execution completion.
        rhi::buffer_barrier host{.resource = visibility_feedback_buffer.get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::host_read};
        return commands.barrier({.buffers = std::span(&host, 1)});
    }

    texture_residency::feedback runtime::gather_cpu_texture_feedback(std::span<primitive const* const> visible) const noexcept {
        texture_residency::feedback result{};
        utility::vessel::vector<std::uint32_t> materials;
        for (primitive const* const leaf : visible) {
            if (leaf == nullptr)
                continue;
            materials.clear();
            leaf->append_material_indices(materials);
            for (std::uint32_t const material : materials) {
                if (material >= texture_residency::material_capacity) {
                    result.invalid = 1;
                    return result;
                }
                result.materials[material / 32u] |= 1u << (material % 32u);
            }
        }
        return result;
    }

    void runtime::collect_cpu_texture_feedback_from(std::span<primitive const* const> visible) noexcept {
        cpu_texture_feedback = gather_cpu_texture_feedback(visible);
    }

    void runtime::collect_cpu_texture_feedback() noexcept {
        utility::vessel::vector<primitive const*> visible;
        visible.reserve(frame_visible.size() + frame_transparent.size() + frame_overlay.size() + frame_character.size());
        visible.insert(visible.end(), frame_visible.begin(), frame_visible.end());
        visible.insert(visible.end(), frame_transparent.begin(), frame_transparent.end());
        visible.insert(visible.end(), frame_overlay.begin(), frame_overlay.end());
        visible.insert(visible.end(), frame_character.begin(), frame_character.end());
        collect_cpu_texture_feedback_from(visible);
    }

    void runtime::prepare_texture_demand() {
        cpu_texture_feedback_ready = false;
        texture_prefetch_ready = false;
        prefetch_cpu_feedback = {};
        prepared_selection_ready = false;
        prepared_visible.clear();
        if (!texture_streaming.enabled || !frustum_culling || bound_scene == nullptr)
            return;
        auto const extent = presentation_extent();
        if (extent.width == 0 || extent.height == 0)
            return;
        float const aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        camera_ubo preview = make_orbit_camera_ubo(camera.yaw, camera.pitch, camera.distance, camera.target,
                                                   scene_extent_radius, aspect);
        preview.view_proj_unjittered = preview.proj * preview.view;
        deren::utility::frustum const view_frustum = deren::utility::make_frustum(preview.view_proj_unjittered);

        utility::vessel::vector<primitive const*> visible;
        std::optional<deren::utility::bvh<primitive>> local_bvh;
        if (!bvh_dirty && cull_bvh.has_value() && !frame_leaves_buffer.empty()) {
            // Static/common path: the cached BVH and leaf snapshot are still valid.
            visible.reserve(frame_leaves_buffer.size());
            for (primitive const* const leaf : frame_leaves_buffer)
                if (!leaf->has_bounds)
                    visible.push_back(leaf);
            for (auto const* node : cull_bvh->frustum_cull(view_frustum))
                visible.push_back(node->extra_data);
        } else {
            // Animation or scene change: world matrices changed after the last recorded frame. Build
            // a private leaf list/BVH for this preview rather than mutating the render caches, which
            // begin_recording() rebuilds after acquire.
            for (scene_tree::scene_node& root : bound_scene->roots)
                scene_tree::update_world(root, scene_transform);
            utility::vessel::vector<primitive const*> leaves;
            for (scene_tree::scene_node const& root : bound_scene->roots)
                collect_leaf_primitives(root, leaves);
            std::erase_if(leaves, [](primitive const* const leaf) { return leaf->overlay_kind != 0u; });
            utility::vessel::vector<deren::utility::aabb_box<primitive>> boxes;
            boxes.reserve(leaves.size());
            for (primitive const* const leaf : leaves) {
                if (!leaf->has_bounds)
                    visible.push_back(leaf);
                else {
                    auto const [wmin, wmax] = leaf->world_aabb();
                    boxes.push_back(deren::utility::aabb_box<primitive>{
                        .min = wmin, .max = wmax, .extra_data = const_cast<primitive*>(leaf)});
                }
            }
            if (!boxes.empty()) {
                if (auto made = deren::utility::bvh<primitive>::make(boxes)) {
                    local_bvh = std::move(*made);
                    for (auto const* node : local_bvh->frustum_cull(view_frustum))
                        visible.push_back(node->extra_data);
                }
            }
        }
        if (visible.empty())
            return; // scene not ready yet (or nothing visible): let begin_recording() collect the live lists
        collect_cpu_texture_feedback_from(visible);
        cpu_texture_feedback_ready = true;
        prefetch_cpu_feedback = cpu_texture_feedback;
        // Speculative next-frame candidates: widen the same preview's projection by 25% per axis and
        // queue the extra materials for upload. The worker runs after this frame's update, i.e. while
        // the GPU renders this frame, so a texture that enters the actual view next frame is likely
        // already resident. Demand itself stays the unwidened list.
        if (auto const* tree = cull_bvh.has_value() ? &*cull_bvh : (local_bvh.has_value() ? &*local_bvh : nullptr)) {
            auto widened = preview.proj;
            widened[0][0] *= 0.75f;
            widened[1][1] *= 0.75f;
            deren::utility::frustum const wide_frustum = deren::utility::make_frustum(widened * preview.view);
            utility::vessel::vector<primitive const*> extra;
            for (auto const* node : tree->frustum_cull(wide_frustum)) {
                bool const already = std::find(visible.begin(), visible.end(), node->extra_data) != visible.end();
                if (!already)
                    extra.push_back(node->extra_data);
            }
            auto const extra_feedback = gather_cpu_texture_feedback(extra);
            if (extra_feedback.invalid) {
                prefetch_cpu_feedback.invalid = 1;
            } else {
                for (std::size_t word = 0; word < texture_residency::feedback_words; ++word)
                    prefetch_cpu_feedback.materials[word] |= extra_feedback.materials[word];
            }
        }
        texture_prefetch_ready = true;
        static bool logged_prep_cull = false;
        if (!logged_prep_cull && texture_streaming.enabled) {
            logged_prep_cull = true;
            utility::log("prep cull: visible leaves {} / {}", visible.size(), frame_leaves_buffer.size());
        }
        prepared_visible = std::move(visible);
        prepared_selection_aspect = aspect;
        prepared_selection_ready = cpu_frame_prep_enabled;
        schedule_texture_prefetch();
    }

    void runtime::start_texture_upload_worker() {
        if (!this->upload_workers.empty())
            return;
        std::size_t const hardware = std::max(1u, std::thread::hardware_concurrency());
        std::size_t const count = std::min<std::size_t>(4u, hardware);
        this->upload_workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            this->upload_workers.emplace_back([this]() {
                for (;;) {
                    rhi::shared_object<rhi::lazy_image> image;
                    {
                        std::unique_lock lock(this->upload_queue_mutex);
                        this->upload_queue_cv.wait(lock, [this]() {
                            return this->upload_worker_stopping || !this->upload_queue.empty();
                        });
                        if (this->upload_worker_stopping) {
                            // Shutdown discards pending work instead of draining it: the shared objects
                            // release with the queue, and the caller is tearing the device down anyway.
                            this->upload_queue.clear();
                            this->upload_inflight.clear();
                            return;
                        }
                        image = std::move(this->upload_queue.back());
                        this->upload_queue.pop_back();
                    }
                    if (image) {
                        if (image->state() == rhi::lazy_state::unloaded &&
                            image->prepare() == rhi::error::ok)
                            this->texture_upload_prefetched.fetch_add(1, std::memory_order_relaxed);
                        std::lock_guard lock(this->upload_queue_mutex);
                        this->upload_inflight.erase(image.get());
                    }
                }
            });
        }
    }

    void runtime::stop_texture_upload_worker() {
        {
            std::lock_guard lock(this->upload_queue_mutex);
            this->upload_worker_stopping = true;
        }
        this->upload_queue_cv.notify_all();
        for (auto& worker : this->upload_workers)
            if (worker.joinable())
                worker.join();
        this->upload_workers.clear();
    }

    void runtime::schedule_texture_prefetch() {
        if (!this->texture_streaming.enabled || !this->texture_prefetch_ready || this->streamed_textures.empty())
            return;
        utility::vessel::vector<std::uint8_t> pinned(this->streamed_textures.size(), 0);
        for (std::size_t i = 0; i < pinned.size(); ++i)
            if (this->streamed_textures[i].pinned)
                pinned[i] = 1;
        auto required = texture_residency::demand(this->prefetch_cpu_feedback, this->material_texture_edges, pinned);
        if (!required)
            return;
        bool queued = false;
        {
            std::lock_guard lock(this->upload_queue_mutex);
            for (std::size_t i = 0; i < this->streamed_textures.size(); ++i) {
                auto& entry = this->streamed_textures[i];
                if (!entry.image || (*required)[i] == 0 || entry.image->state() != rhi::lazy_state::unloaded)
                    continue;
                if (!this->upload_inflight.insert(entry.image.get()).second)
                    continue;
                // Speculative candidates are not demand: give the upload a one-frame retention grace
                // so a candidate that becomes visible on the next frame is not unloaded between the
                // worker finishing and the frame that asks for it.
                entry.last_demand_frame = std::max(entry.last_demand_frame, this->texture_residency_updates);
                this->upload_queue.push_back(entry.image);
                queued = true;
            }
        }
        if (queued)
            this->upload_queue_cv.notify_all();
    }

    rhi::error runtime::update_texture_residency(bool all) {
        auto started = std::chrono::steady_clock::now();
        utility::vessel::vector<std::uint8_t> pinned(streamed_textures.size(), all ? 1 : 0);
        for (std::size_t i = 0; i < pinned.size(); ++i)
            if (streamed_textures[i].pinned)
                pinned[i] = 1;
        texture_residency::feedback bits{};
        if (!all)
            bits = cpu_texture_feedback;
        auto required = texture_residency::demand(bits, material_texture_edges, pinned);
        if (!required)
            return rhi::error::invalid_argument;
        auto* descriptors = rhi::query_extension<rhi::descriptor_space>(rhi_face());
        if (!descriptors)
            return rhi::error::unsupported;
        std::uint64_t resident_bytes = 0, uploaded_bytes = 0, deferred = 0, total_bytes = 0, pinned_bytes = 0;
        std::uint64_t const epoch = ++texture_residency_updates;
        rhi::image_view_desc view{};
        view.mip_count = 0;
        view.layer_count = 0;
        for (std::size_t i = 0; i < streamed_textures.size(); ++i) {
            auto& entry = streamed_textures[i];
            if (!entry.image)
                continue;
            total_bytes += entry.bytes;
            if (entry.pinned)
                pinned_bytes += entry.bytes;
            bool const wanted = (*required)[i] != 0;
            if (wanted) {
                entry.last_demand_frame = epoch;
            } else if (epoch > entry.last_demand_frame + 1u) {
                // Not demanded now and not demanded in the immediately preceding frame: this frame is
                // N and the image last mattered at N-1 or earlier. Retire it only now, so an image
                // loaded for N-1 remains available for N+1's demand (one-frame reuse horizon).
                if (entry.published) {
                    auto result = descriptors->write_image({.slot = render_layout::heap_slots::textures + static_cast<std::uint32_t>(i), .resource = owned_textures[white_texture_index].get(), .view = &view});
                    if (result != rhi::error::ok)
                        return result;
                    entry.published = false;
                }
                auto result = entry.image->unload();
                if (result == rhi::error::not_ready)
                    ++deferred;
                else if (result != rhi::error::ok)
                    return result;
                continue;
            }
            if (wanted) {
                // The prefetch worker overlaps the upload with the previous GPU frame. If it has not
                // finished by the time this frame must publish the texture, wait for it here rather
                // than ever sampling an unloaded image.
                auto state = entry.image->state();
                if (state == rhi::lazy_state::unloaded || state == rhi::lazy_state::loading) {
                    this->texture_upload_waited.fetch_add(1, std::memory_order_relaxed);
                    if (auto prepared = entry.image->prepare(); prepared != rhi::error::ok)
                        return prepared;
                } else if (state == rhi::lazy_state::failed) {
                    return entry.image->prepare();
                }
            }
            if (entry.published != wanted) {
                auto* image = wanted ? entry.image.get() : owned_textures[white_texture_index].get();
                auto result = descriptors->write_image({.slot = render_layout::heap_slots::textures + static_cast<std::uint32_t>(i), .resource = image, .view = &view});
                if (result != rhi::error::ok)
                    return result;
                entry.published = wanted;
            }
            if (wanted && entry.image->state() == rhi::lazy_state::unloaded)
                uploaded_bytes += entry.bytes;
        }
        auto result = descriptors->bind({.commands = frame_command_buffer().get()});
        if (result != rhi::error::ok)
            return result;
        schedule_texture_prefetch();
        std::uint64_t required_count = 0;
        for (std::size_t i = 0; i < streamed_textures.size(); ++i)
            if ((*required)[i] != 0)
                ++required_count;
        for (auto const& entry : streamed_textures)
            if (entry.image && entry.image->state() == rhi::lazy_state::ready)
                resident_bytes += entry.bytes;
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (epoch <= 2 || uploaded_bytes || epoch % 60 == 0)
            utility::log("texture residency: resident payload {} / {} bytes, pinned {} bytes, new demand {} bytes, deferred unload {}, prefetched {}, waited {}, required {}/{}, update {} ms",
                         resident_bytes, total_bytes, pinned_bytes, uploaded_bytes, deferred,
                         this->texture_upload_prefetched.load(std::memory_order_relaxed),
                         this->texture_upload_waited.load(std::memory_order_relaxed), required_count,
                         streamed_textures.size(), elapsed);
        return rhi::error::ok;
    }
} // namespace deren::engine
