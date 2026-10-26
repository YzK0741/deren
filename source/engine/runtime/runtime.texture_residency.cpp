module;
#include <array>
#include <chrono>
#include <cstring>
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

    rhi::error runtime::update_texture_residency(bool all) {
        auto started = std::chrono::steady_clock::now();
        utility::vessel::vector<std::uint8_t> pinned(streamed_textures.size(), all ? 1 : 0);
        for (std::size_t i = 0; i < pinned.size(); ++i)
            if (streamed_textures[i].pinned)
                pinned[i] = 1;
        texture_residency::feedback bits{};
        if (!all)
            std::memcpy(&bits, visibility_feedback_buffer->mapped().data(), sizeof(bits));
        auto required = texture_residency::demand(bits, material_texture_edges, pinned);
        if (!required)
            return rhi::error::invalid_argument;
        auto* descriptors = rhi::query_extension<rhi::descriptor_space>(rhi_face());
        if (!descriptors)
            return rhi::error::unsupported;
        std::uint64_t resident_bytes = 0, uploaded_bytes = 0, deferred = 0, total_bytes = 0, pinned_bytes = 0;
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
            if (entry.published != wanted) {
                auto* image = wanted ? entry.image.get() : owned_textures[white_texture_index].get();
                auto result = descriptors->write_image({.slot = render_layout::heap_slots::textures + static_cast<std::uint32_t>(i), .resource = image, .view = &view});
                if (result != rhi::error::ok)
                    return result;
                entry.published = wanted;
            }
            if (!wanted) {
                auto result = entry.image->unload();
                if (result == rhi::error::not_ready)
                    ++deferred;
                else if (result != rhi::error::ok)
                    return result;
            } else if (entry.image->state() == rhi::lazy_state::unloaded)
                uploaded_bytes += entry.bytes;
        }
        auto result = descriptors->bind({.commands = frame_command_buffer().get()});
        if (result != rhi::error::ok)
            return result;
        for (auto const& entry : streamed_textures)
            if (entry.image && entry.image->state() == rhi::lazy_state::ready)
                resident_bytes += entry.bytes;
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (++texture_residency_updates <= 2 || uploaded_bytes || texture_residency_updates % 60 == 0)
            utility::log("texture residency: resident payload {} / {} bytes, pinned {} bytes, new demand {} bytes, deferred unload {}, update {} ms", resident_bytes, total_bytes, pinned_bytes, uploaded_bytes, deferred, elapsed);
        return rhi::error::ok;
    }
} // namespace deren::engine
