/**
 * @file source/engine/pass/software_visibility.cpp
 * @brief the software raster pass's pipeline and tile dispatch (see the header's WHY THIS PASS EXISTS).
 */
module;

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <utility>

module deren.engine.pass.software_visibility;

import deren.utility.shared;
import deren.utility; // deren::utility::log
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_layout;

namespace deren::engine::pass {
    void software_visibility_pass::create(pass_context const& context) {
        auto const* requested = std::getenv("DEREN_SOFTWARE_RASTER_TRIANGLES");
        this->triangle_requested = requested && std::string_view(requested) == "1";

        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->visibility_pipeline.reset(); // a new device generation: the old handle belonged to the old device
            this->triangle_pipeline.reset();
            for (auto& scratch : this->triangle_scratch)
                scratch.reset();
            for (auto& buffer : this->route_metadata)
                buffer.reset();
            for (auto& buffer : this->route_arguments)
                buffer.reset();
            this->gpu_route_ready = false;
        }
        this->built_against = context.face;
        this->device = context.face;
        if (this->visibility_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<std::uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<std::uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("software visibility disabled: the owner has no {}", shader_name);
            return;
        }
        // A HEAP-NATIVE COMPUTE PIPELINE: no layout, no set - every descriptor it samples or writes is a heap slot.
        auto built = pipelines::build_software_visibility(*context.face, spirv);
        if (!built) {
            deren::utility::log("software visibility disabled: {}", built.error());
            this->visibility_pipeline.reset();
            return;
        }
        this->visibility_pipeline = std::move(built->trace);
        auto const* caps = rhi::query_extension<rhi::device_capabilities>(*context.face);
        if (this->triangle_requested && caps && caps->buffer_int64_atomics()) {
            auto const atomic_spirv = context.shader ? context.shader(context.owner, "software_visibility_atomic.comp.spv") : std::span<std::uint8_t const>{};
            if (!atomic_spirv.empty()) {
                auto atomic_built = pipelines::build_software_visibility(*context.face, atomic_spirv);
                if (atomic_built)
                    this->triangle_pipeline = std::move(atomic_built->trace);
            }
            deren::utility::log("software triangle prototype: pipeline {}", this->triangle_pipeline ? "ready" : "unavailable; hardware fallback");
        }
        deren::utility::log("SUCCESS: software visibility pipeline created (compute, sparse active-tile dispatch, two R32_UINT software images)");
    }

    bool software_visibility_pass::prepare_triangles(std::uint32_t slot, std::uint32_t width, std::uint32_t height) {
        this->triangle_frame = false;
        if (!this->triangle_requested)
            return true;
        if (!this->device || !this->triangle_pipeline || slot >= this->triangle_scratch.size() || width == 0u || height == 0u)
            return false;
        // Shader pixel indices are uint32. Reject rather than truncate a scratch allocation/index.
        std::uint64_t const pixels = static_cast<std::uint64_t>(width) * height;
        if (pixels > UINT32_MAX)
            return false;
        auto* heap = rhi::query_extension<rhi::descriptor_space>(*this->device);
        if (!heap || !heap->ready() || !heap->fixed_slots().count)
            return false;
        auto& scratch = this->triangle_scratch[slot];
        if (!scratch || scratch->size() != pixels * sizeof(std::uint64_t)) {
            auto replacement = this->device->resources->create_buffer({.size = pixels * sizeof(std::uint64_t),
                                                                       .usage = rhi::buffer_usage::storage_gpu_only,
                                                                       .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage)});
            if (!replacement)
                return false;
            scratch = std::move(replacement);
        }
        if (heap->write_buffer({.slot = render_layout::heap_slots::software_atomic + slot,
                                .resource = scratch.get(),
                                .size = scratch->size(),
                                .type = rhi::descriptor_type::storage_buffer}) != rhi::error::ok)
            return false;
        this->triangle_slot = slot;
        this->triangle_frame = true;
        return true;
    }

    std::span<std::uint32_t> software_visibility_pass::route_metadata_words() const noexcept {
        if (!this->gpu_route_ready || !this->route_metadata[this->triangle_slot])
            return {};
        auto bytes = this->route_metadata[this->triangle_slot]->mapped();
        return {reinterpret_cast<std::uint32_t*>(bytes.data()), bytes.size() / 4u};
    }

    bool software_visibility_pass::prepare_gpu_route(std::uint32_t slot, std::uint32_t width, std::uint32_t height, gpu_route_parameters parameters) {
        this->gpu_route_ready = false;
        if (!this->device || !std::isfinite(parameters.threshold_px) || parameters.threshold_px <= 0.0f ||
            !std::isfinite(parameters.near_plane) || parameters.near_plane <= 0.0f ||
            !std::isfinite(parameters.projection_scale) || parameters.projection_scale <= 0.0f ||
            parameters.budget > visibility::software::software_entry_budget)
            return false;
        auto const* caps = rhi::query_extension<rhi::device_capabilities>(*this->device);
        if (!caps || !caps->compute_indirect() || !this->prepare_triangles(slot, width, height) || !this->triangle_frame)
            return false;
        auto* heap = rhi::query_extension<rhi::descriptor_space>(*this->device);
        if (!heap || !heap->ready() || !heap->fixed_slots().count)
            return false;
        constexpr std::uint64_t metadata_bytes = (8u + visibility::software::software_entry_budget) * 4ull;
        auto prepare = [&](rhi::shared_object<rhi::buffer>& buffer, std::uint64_t bytes, std::uint32_t heap_slot, bool indirect) {
            if (!buffer || buffer->size() != bytes) {
                auto flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage);
                if (indirect)
                    flags |= rhi::to_bits(rhi::buffer_flag::indirect);
                auto created = this->device->resources->create_buffer({.size = bytes, .usage = rhi::buffer_usage::storage_coherent, .flags = flags});
                if (!created)
                    return false;
                buffer = rhi::shared_object<rhi::buffer>{std::move(created)};
            }
            auto mapped = buffer->mapped();
            return mapped.size() >= bytes && heap->write_buffer({.slot = heap_slot + slot, .resource = buffer.get(), .size = bytes, .type = rhi::descriptor_type::storage_buffer}) == rhi::error::ok;
        };
        if (!prepare(this->route_metadata[slot], metadata_bytes, render_layout::heap_slots::software_route_metadata, false) ||
            !prepare(this->route_arguments[slot], 16u, render_layout::heap_slots::software_route_arguments, true))
            return false;
        auto meta = reinterpret_cast<std::uint32_t*>(this->route_metadata[slot]->mapped().data());
        meta[0] = std::bit_cast<std::uint32_t>(parameters.threshold_px);
        meta[1] = std::bit_cast<std::uint32_t>(parameters.near_plane);
        meta[2] = std::bit_cast<std::uint32_t>(parameters.projection_scale);
        meta[3] = parameters.perspective ? 1u : 2u;
        meta[4] = parameters.budget;
        meta[5] = meta[6] = meta[7] = 0u;
        auto args = reinterpret_cast<std::uint32_t*>(this->route_arguments[slot]->mapped().data());
        args[0] = 0u;
        args[1] = 1u;
        args[2] = 1u;
        args[3] = 0u;
        this->gpu_route_ready = true;
        return true;
    }

    rhi::error software_visibility_pass::record_content(resolved_io const& io) noexcept {
        if (!this->has_pipeline() || io.list == nullptr || this->software_depth_target == nullptr || this->software_id_target == nullptr ||
            io.extent.width == 0u || io.extent.height == 0u) {
            return rhi::error::invalid_argument;
        }
        if (this->triangle_frame) {
            auto order = [&](rhi::buffer_use from, rhi::buffer_use to) {
                rhi::buffer_barrier const barrier{.resource = this->triangle_scratch[this->triangle_slot].get(), .from = from, .to = to};
                return io.list->barrier(rhi::barrier_group{.buffers = std::span<rhi::buffer_barrier const>(&barrier, 1u)});
            };
            std::uint32_t const gx = (io.extent.width + tile_extent - 1u) / tile_extent;
            std::uint32_t const gy = (io.extent.height + tile_extent - 1u) / tile_extent;
            // The slot was fenced before preparation; previous unpack reads must finish before a new clear.
            if (order(rhi::buffer_use::shader_read_write, rhi::buffer_use::shader_write) != rhi::error::ok ||
                !io.push_block(*io.list, {}, 2u))
                return rhi::error::not_ready;
            io.list->dispatch(gx, gy, 1u);
            if (order(rhi::buffer_use::shader_write, rhi::buffer_use::shader_read_write) != rhi::error::ok ||
                !io.push_block(*io.list, {}, this->hzb_results_active ? 1u : 0u))
                return rhi::error::not_ready;
            if (this->gpu_route_ready) {
                auto const status = io.list->dispatch_indirect(*this->route_arguments[this->triangle_slot], 0u);
                if (status != rhi::error::ok)
                    return status;
            } else if (this->active_tile_count != 0u)
                io.list->dispatch(std::min(this->active_tile_count, 65535u), (this->active_tile_count + 65534u) / 65535u, 1u);
            if (order(rhi::buffer_use::shader_read_write, rhi::buffer_use::shader_read) != rhi::error::ok ||
                !io.push_block(*io.list, {}, 4u))
                return rhi::error::not_ready;
            io.list->dispatch(gx, gy, 1u);
            static bool logged = false;
            if (!logged) {
                deren::utility::log("software triangle prototype: recorded clear/raster/unpack, {} unique cluster(s)", this->active_tile_count);
                logged = true;
            }
            return rhi::error::ok;
        }
        // Match the heap-native shader's three uint lanes (frame slot, image index, endpoint spare lane). Mode bit 1
        // asks the same shader to clear both private storage images before the active-tile raster dispatch.
        if (!io.push_block(*io.list, {}, 2u)) {
            return rhi::error::invalid_argument;
        }
        std::uint32_t const groups_x = (io.extent.width + tile_extent - 1u) / tile_extent;
        std::uint32_t const groups_y = (io.extent.height + tile_extent - 1u) / tile_extent;
        io.list->dispatch(groups_x, groups_y, 1u); // initialize every pixel, including tiles omitted from the CSR
        if (io.list->barrier(rhi::image_barrier{.resource = this->software_depth_target,
                                                .from = rhi::image_use::shader_write,
                                                .to = rhi::image_use::shader_read_write}) != rhi::error::ok ||
            io.list->barrier(rhi::image_barrier{.resource = this->software_id_target,
                                                .from = rhi::image_use::shader_write,
                                                .to = rhi::image_use::shader_read_write}) != rhi::error::ok) {
            return rhi::error::not_ready;
        }
        // The host writes the compact active-tile list alongside the CSR offsets. Each dispatched raster group maps
        // through that list to one non-empty 16x16 tile; the separate clear dispatch handles empty tiles.
        if (!io.push_block(*io.list, {}, this->hzb_results_active ? 1u : 0u)) {
            return rhi::error::invalid_argument;
        }
        if (this->active_tile_count == 0u) {
            return rhi::error::ok;
        }
        io.list->dispatch(this->active_tile_count, 1u, 1u);
        return rhi::error::ok;
    }
} // namespace deren::engine::pass
