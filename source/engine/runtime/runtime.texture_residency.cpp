module;
#include "../visibility/progressive_texture.h"

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
    static constexpr std::uint32_t mip_feedback_slot = render_layout::heap_slot_base + 1366;

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
            for (unsigned slot = 0; slot < 2; ++slot) {
                texture_feedback_slots[slot] = rhi::shared_object<rhi::buffer>(rhi_face().resources->create_buffer(
                    {.size = sizeof(texture_residency::feedback), .usage = rhi::buffer_usage::storage_coherent, .flags = rhi::to_bits(rhi::buffer_flag::device_address)}));
                if (!visibility_feedback_pipeline || !texture_feedback_slots[slot])
                    return rhi::error::operation_failed;
            }
            auto* descriptors = rhi::query_extension<rhi::descriptor_space>(rhi_face());
            for (unsigned slot = 0; slot < 2; ++slot) {
                auto result = descriptors ? descriptors->write_buffer({.slot = feedback_slot + slot, .resource = texture_feedback_slots[slot].get(), .size = sizeof(texture_residency::feedback)}) : rhi::error::unsupported;
                if (result != rhi::error::ok)
                    return result;
                if (progressive_mips) {
                    mip_feedback_slots[slot] = rhi::shared_object<rhi::buffer>(rhi_face().resources->create_buffer(
                        {.size = sizeof(texture_residency::mip_feedback), .usage = rhi::buffer_usage::storage_coherent, .flags = rhi::to_bits(rhi::buffer_flag::device_address)}));
                    if (!mip_feedback_slots[slot])
                        return rhi::error::operation_failed;
                    result = descriptors->write_buffer({.slot = mip_feedback_slot + slot, .resource = mip_feedback_slots[slot].get(), .size = sizeof(texture_residency::mip_feedback)});
                    if (result != rhi::error::ok)
                        return result;
                }
            }
        }
        auto slot = frame_ring().position();
        visibility_feedback_buffer = texture_feedback_slots[slot];
        if (progressive_textures && texture_feedback_pending[slot]) {
            std::memcpy(&progressive_feedback, visibility_feedback_buffer->mapped().data(), sizeof(progressive_feedback));
            texture_feedback_pending[slot] = false;
            if (progressive_mips)
                std::memcpy(&progressive_mip_feedback, mip_feedback_slots[slot]->mapped().data(), sizeof(progressive_mip_feedback));
        }
        auto mapped = visibility_feedback_buffer->mapped();
        if (mapped.size() < sizeof(texture_residency::feedback))
            return rhi::error::operation_failed;
        std::memset(mapped.data(), 0, sizeof(texture_residency::feedback));
        if (progressive_mips)
            std::memset(mip_feedback_slots[slot]->mapped().data(), 0, sizeof(texture_residency::mip_feedback));
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
        std::array<std::uint32_t, 6> push{frame_ring().position(), current_image_index, software_raster_active_frame ? 1u : 0u, feedback_slot + frame_ring().position(), material_count, visibility_recorded_item_count};
        result = commands.push_constants(0, std::as_bytes(std::span(push)));
        if (result != rhi::error::ok)
            return result;
        auto extent = render_extent();
        commands.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        // Host publication is this pass's epilogue. The checkpoint establishes execution completion.
        rhi::buffer_barrier host{.resource = visibility_feedback_buffer.get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::host_read};
        result = commands.barrier({.buffers = std::span(&host, 1)});
        if (result == rhi::error::ok)
            texture_feedback_pending[frame_ring().position()] = true;
        return result;
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
                auto result = descriptors->write_image({.slot = render_layout::heap_slots::textures + frame_ring().position() * render_layout::heap_texture_bank_stride + static_cast<std::uint32_t>(i), .resource = image, .view = &view});
                if (result != rhi::error::ok)
                    return result;
                entry.published = wanted;
                texture_bank_images[frame_ring().position()][i] = wanted ? rhi::shared_object<rhi::image>(entry.image) : owned_textures[white_texture_index];
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

    rhi::error runtime::publish_texture_bank() {
        auto* descriptors = rhi::query_extension<rhi::descriptor_space>(rhi_face());
        if (!descriptors)
            return rhi::error::unsupported;
        auto slot = frame_ring().position();
        auto& bank = texture_bank_images[slot];
        bank.resize(owned_textures.size());
        auto& bank_bytes = texture_bank_bytes[slot];
        bank_bytes.resize(owned_textures.size());
        rhi::image_view_desc view{};
        view.mip_count = view.layer_count = 0;
        for (std::size_t i = 0; i < owned_textures.size(); ++i) {
            auto selected = owned_textures[i];
            if (i < streamed_textures.size()) {
                auto& entry = streamed_textures[i];
                if (entry.source)
                    selected = entry.high ? entry.high : entry.fallback;
                else if (entry.image)
                    selected = entry.published ? rhi::shared_object<rhi::image>(entry.image) : owned_textures[white_texture_index];
            }
            if (bank[i].get() == selected.get())
                continue;
            auto result = descriptors->write_image({.slot = render_layout::heap_slots::textures + slot * render_layout::heap_texture_bank_stride + std::uint32_t(i), .resource = selected.get(), .view = &view});
            if (result != rhi::error::ok)
                return result;
            bank[i] = std::move(selected);
            bank_bytes[i] = i < streamed_textures.size() && streamed_textures[i].high.get() == bank[i].get() ? streamed_textures[i].resident_bytes : 0;
        }
        return rhi::error::ok;
    }

    rhi::error runtime::update_progressive_textures(bool all) {
        auto begin = std::chrono::steady_clock::now();
        ++texture_residency_updates;
        utility::vessel::vector<std::uint8_t> pinned(streamed_textures.size(), all ? 1 : 0);
        for (std::size_t i = 0; i < pinned.size(); ++i)
            if (streamed_textures[i].pinned)
                pinned[i] = 1;
        auto required = texture_residency::demand(progressive_feedback, material_texture_edges, pinned);
        if (!required)
            return rhi::error::invalid_argument;
        std::vector<std::uint32_t> requested(streamed_textures.size()), targets(streamed_textures.size());
        if (progressive_mips && !all) {
            for (std::size_t material = 0; material < std::min<std::size_t>(material_texture_edges.size(), texture_residency::material_capacity); ++material) {
                if (!(progressive_feedback.materials[material / 32] & (1u << (material % 32))))
                    continue;
                for (auto index : material_texture_edges[material])
                    requested[index] = std::max(requested[index], progressive_mip_feedback.requested_extent[material]);
            }
        }
        std::vector<progressive_texture::candidate> candidates(streamed_textures.size());
        std::vector<progressive_texture::mip_candidate> mip_candidates(streamed_textures.size());
        std::uint64_t pending_bytes = 0, floor_bytes = 0, uploaded_bytes = 0;
        std::size_t jobs = 0, wanted = 0, started = 0, failures = 0, screen_requests = 0;
        for (std::size_t i = 0; i < streamed_textures.size(); ++i) {
            auto& entry = streamed_textures[i];
            if (!entry.source)
                continue;
            if (entry.upload.valid() && entry.upload.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto image = entry.upload.get();
                entry.failed = !image;
                if (image) {
                    entry.high = std::move(image);
                    entry.resident_mip = entry.upload_mip;
                    entry.resident_bytes = entry.upload_bytes;
                }
            }
            if (entry.upload.valid()) {
                pending_bytes += entry.upload_bytes;
                ++jobs;
            }
            floor_bytes += entry.fallback_bytes;
            if (entry.pinned) {
                if (entry.high.get() != entry.fallback.get())
                    floor_bytes += entry.bytes;
                continue;
            }
            bool visible = (*required)[i] != 0;
            if (visible) {
                entry.last_used = texture_residency_updates;
                ++wanted;
                if (requested[i])
                    ++screen_requests;
            }
            auto const& desc = entry.description;
            auto target = progressive_mips && !all ? progressive_texture::target_mip(desc.extent.width, desc.extent.height, requested[i]) : 0;
            targets[i] = std::min(entry.fallback_mip, target);
            auto resident = entry.high ? entry.resident_mip : entry.fallback_mip;
            candidates[i] = {entry.bytes, entry.last_used, bool(entry.high) || entry.upload.valid(), visible && !entry.failed};
            mip_candidates[i] = {desc.extent.width, desc.extent.height, desc.mip_levels, entry.fallback_mip,
                                 entry.failed ? resident : targets[i], resident, entry.last_used, visible};
            if (entry.failed)
                ++failures;
        }
        auto budget = texture_streaming.restore_pending ? UINT64_MAX : progressive_budget;
        auto selected = progressive_texture::select(candidates, texture_residency_updates, budget, 30);
        std::vector<std::uint32_t> selected_mips(streamed_textures.size());
        if (progressive_mips) {
            selected_mips = progressive_texture::select_mips(mip_candidates, texture_residency_updates, budget, 30);
            for (std::size_t i = 0; i < selected.size(); ++i)
                selected[i] = selected_mips[i] < streamed_textures[i].fallback_mip;
        }
        for (std::size_t i = 0; i < streamed_textures.size(); ++i) {
            auto& entry = streamed_textures[i];
            if (entry.source && !entry.pinned && (!selected[i] || (progressive_mips && entry.high && entry.resident_mip < selected_mips[i]))) {
                entry.high = {};
                entry.resident_mip = entry.fallback_mip;
                entry.resident_bytes = 0;
                mip_candidates[i].resident = entry.fallback_mip;
            }
        }
        std::vector<std::size_t> upload_order;
        if (progressive_mips)
            upload_order = progressive_texture::promotion_order(mip_candidates, selected_mips);
        else
            for (std::size_t i = 0; i < streamed_textures.size(); ++i)
                upload_order.push_back(i);
        for (auto i : upload_order) {
            auto& entry = streamed_textures[i];
            if (!selected[i] || entry.pinned || entry.upload.valid() || entry.failed)
                continue;
            if (entry.high && (!progressive_mips || entry.resident_mip <= selected_mips[i]))
                continue;
            auto first = progressive_mips && !texture_streaming.restore_pending
                             ? std::max(selected_mips[i], entry.resident_mip ? entry.resident_mip - 1 : 0)
                             : 0;
            auto const& original = entry.description;
            auto bytes = progressive_texture::tier_bytes(original.extent.width, original.extent.height, original.mip_levels, first);
            auto upload_budget = texture_streaming.restore_pending && jobs == 0 ? std::max(progressive_upload_budget, bytes) : progressive_upload_budget;
            if (!progressive_texture::can_start(bytes, pending_bytes, jobs, upload_budget, progressive_max_jobs))
                continue;
            auto* factory = rhi_face().resources.get();
            auto description = original;
            auto source = entry.source;
            entry.upload_mip = first;
            entry.upload_bytes = bytes;
            entry.upload = std::async(std::launch::async, [factory, description, source, first]() mutable {
                auto suffix = progressive_texture::mip_range(description.extent.width, description.extent.height, description.mip_levels, first);
                std::optional<progressive_texture::fallback_image> generated;
                if (suffix) {
                    if (suffix->offset + suffix->bytes > source->size())
                        return rhi::shared_object<rhi::image>{};
                    description.extent.width = suffix->width;
                    description.extent.height = suffix->height;
                    description.mip_levels = suffix->levels;
                    description.initial_bytes = std::as_bytes(std::span(*source)).subspan(suffix->offset, suffix->bytes);
                } else {
                    generated = progressive_texture::resample(std::span(source->data(), source->size()),
                                                              description.extent.width, description.extent.height,
                                                              std::max(1u, description.extent.width >> first), std::max(1u, description.extent.height >> first),
                                                              description.format == rhi::image_format::rgba8_srgb);
                    if (!generated)
                        return rhi::shared_object<rhi::image>{};
                    description.extent.width = generated->width;
                    description.extent.height = generated->height;
                    description.mip_levels = 1;
                    description.initial_bytes = std::as_bytes(std::span(generated->bytes));
                }
                description.debug_name = "progressive mip texture";
                return rhi::shared_object<rhi::image>(factory->create_image(description));
            });
            pending_bytes += bytes;
            uploaded_bytes += bytes;
            ++jobs;
            ++started;
        }
        auto result = publish_texture_bank();
        std::uint64_t high_bytes = 0, retired_bytes = 0;
        std::size_t ready = 0, fallback = 0, target_ready = 0, intermediate = 0, full = 0;
        for (std::size_t i = 0; i < streamed_textures.size(); ++i) {
            auto const& entry = streamed_textures[i];
            if (!entry.source || entry.pinned)
                continue;
            if (entry.high)
                high_bytes += entry.resident_bytes;
            if ((*required)[i]) {
                if (entry.high) {
                    ++ready;
                    if (entry.resident_mip)
                        ++intermediate;
                    else
                        ++full;
                } else
                    ++fallback;
                if ((entry.high ? entry.resident_mip : entry.fallback_mip) <= targets[i])
                    ++target_ready;
            }
            rhi::image const* previous = nullptr;
            for (unsigned slot = 0; slot < 2; ++slot) {
                auto const& bank = texture_bank_images[slot];
                auto* image = i < bank.size() ? bank[i].get() : nullptr;
                if (image && image != entry.fallback.get() && image != entry.high.get() && image != previous)
                    retired_bytes += texture_bank_bytes[slot][i];
                previous = image;
            }
        }
        double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        if (progressive_trace.is_open())
            progressive_trace << texture_residency_updates << ',' << frame_ring().position() << ',' << high_bytes << ',' << floor_bytes << ',' << pending_bytes << ',' << jobs << ',' << wanted << ',' << ready << ',' << fallback << ',' << started << ',' << failures << ',' << high_bytes + retired_bytes << ',' << retired_bytes << ",0," << elapsed << ',' << target_ready << ',' << intermediate << ',' << full << ',' << uploaded_bytes << ',' << screen_requests << '\n';
        if (texture_residency_updates <= 2 || texture_residency_updates % 60 == 0)
            utility::log("progressive textures: high {} / {} bytes, floor {}, pending {} bytes / {} jobs, wanted {}, ready {}, fallback {}, failed {}, target ready {}, intermediate {}", high_bytes, progressive_budget, floor_bytes, pending_bytes, jobs, wanted, ready, fallback, failures, target_ready, intermediate);
        return result;
    }

} // namespace deren::engine
