// ============================================================================
// module: deren.engine.runtime:readback  - the screenshot read-back, READ half
//
// THE READ HALF OF THE SCREENSHOT, AND NOTHING ELSE. Since S2 batch 2 the COPY is recorded through the
// promise contract's recording surface, inside the frame it belongs to (see end_recording in
// runtime.frames.cppm), and it writes into the BACKEND's read-back slot. What is left here is what
// happens after the frame lands:
//
//   acquire_current_frame_image waits for the device, reads the bytes the backend mapped for us and
//   unpacks them (the swapchain's BGRA byte order becomes the PNG writer's RGBA);
//   consume_screenshot_request is the one-shot flag the caller reads.
//
// THE ENGINE TOUCHES NO VULKAN HANDLE HERE ANY MORE - that is the point of the migration: the image,
// its extent and its format come from the contract's frame image, the bytes come from the contract's
// buffer view, and the wait is the contract's context call. What stays the engine's is the DECISION:
// which bytes to unpack, in which order, and what to say when the format cannot be described.
//
// Imports are NOT transitive: this partition imports what its own code calls.
// ============================================================================
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>  // std::memcpy of an RGBA frame
#include <expected> // std::unexpected for the failure strings
#include <fstream>
#include <span> // the bytes the contract's buffer view hands back
#include <string>
#include <vector>

module deren.engine.runtime:readback;
import deren.utility.shared;

import :declarations;

import deren.promise.rhi;
import deren.engine.visibility.software;
import deren.engine.pass.hzb_cull;

namespace deren::engine {
    std::expected<runtime::frame_image, deren::utility::vessel::string> runtime::acquire_current_frame_image() {
        // NO `screenshot_pending` CHECK HERE, AND THAT IS A MEASURED DECISION rather than an omission:
        // the one caller is `if (consume_screenshot_request()) { acquire_current_frame_image(); }`
        // (main.cpp), and consume_screenshot_request() CLEARS the flag while answering it - so a check
        // here fired on every SUCCESSFUL capture and reported "no captured frame" for a frame that had
        // just been captured (measured: 14/14 scenarios came back "no screenshot produced"). What is
        // checked instead is what actually has to hold: a frame image with an extent, and a backend slot
        // that is mapped and large enough.
        rhi::api_core& device = this->vulkan_core;
        // THE SAME IMAGE AND THE SAME SLOT THE COPY USED. `frame_image()` answers for the image the last
        // acquire returned - the copy was recorded into THAT frame's command buffer, and by the time the
        // caller asks the frame has been submitted - and the read-back slot is the backend's own,
        // host-visible buffer.
        deren::promise::rhi::image* const image = device.frame_image();
        deren::promise::rhi::buffer* const slot = device.frame_readback_buffer();
        if (image == nullptr || slot == nullptr) {
            return std::unexpected(deren::utility::vessel::string("screenshot: the backend has no frame image or read-back slot"));
        }
        deren::promise::rhi::image_extent const extent = image->extent();
        if (extent.width == 0 || extent.height == 0) {
            return std::unexpected(deren::utility::vessel::string("screenshot: the frame image has no extent"));
        }
        // The format the backend names for the frame image decides how the bytes are unpacked. A format
        // the contract cannot describe is the "unsupported swapchain format" error this path always had.
        deren::promise::rhi::image_format const format = image->format();
        bool const bgra = format == deren::promise::rhi::image_format::bgra8_srgb || format == deren::promise::rhi::image_format::bgra8_unorm;
        bool const rgba = format == deren::promise::rhi::image_format::rgba8_srgb || format == deren::promise::rhi::image_format::rgba8_unorm;
        if (!bgra && !rgba) {
            return std::unexpected(deren::utility::vessel::string("screenshot: unsupported swapchain format (need 8-bit RGBA/BGRA)"));
        }

        // ONE WAIT IS ALL THAT IS LEFT. The copy was recorded INSIDE the submitted frame, so the bytes
        // are on the device until that frame completes; the contract's buffer view carries no fence, so
        // the caller's pacing - this wait, which is the context's own - is the ordering. The same rule
        // the contract states for `buffer::mapped()`.
        device.wait_idle();
        if (auto const* prefix = std::getenv("DEREN_SOFTWARE_IMAGES_CAPTURE")) {
            auto const captured = this->software_probe_extent;
            std::uint64_t const required = static_cast<std::uint64_t>(captured.width) * captured.height * 4u;
            if (!required)
                return std::unexpected(deren::utility::vessel::string("software image capture: no completed capture"));
            if (this->software_route_probe_ready) {
                namespace software = deren::engine::visibility::software;
                auto fail = [] { return std::unexpected(deren::utility::vessel::string("software GPU capture: invalid partition or reference")); };
                auto const work_bytes = this->software_route_probe_readbacks[0]->mapped();
                auto const result_bytes = this->software_route_probe_readbacks[1]->mapped();
                auto const survivor_bytes = this->software_route_probe_readbacks[2]->mapped();
                auto const* work = reinterpret_cast<std::uint32_t const*>(work_bytes.data());
                auto const* results = reinterpret_cast<std::uint32_t const*>(result_bytes.data());
                auto const* survivors = reinterpret_cast<meshlet const*>(survivor_bytes.data());
                auto const grid = software::make_tile_grid(captured.width, captured.height);
                std::size_t const entry_offset = 6u + grid.tile_count() * 3u + 1u;
                if (work_bytes.size() < (entry_offset + software::software_entry_budget * 2u) * 4u ||
                    result_bytes.size() < pass::hzb_results_words_per_lane * 4ull || survivor_bytes.size() < meshlet_capacity * sizeof(meshlet))
                    return fail();
                if (work[2] > this->software_route_probe_budget || work[2] > software::software_entry_budget)
                    return fail();
                deren::utility::vessel::vector<software::tile_entry> entries;
                for (std::uint32_t i = 0; i < work[2]; ++i)
                    entries.push_back({work[entry_offset + i * 2u], work[entry_offset + i * 2u + 1u]});
                deren::utility::vessel::vector<software::gpu_partition_item> partition;
                deren::utility::vessel::vector<software::reference_item> references;
                for (auto const& item : this->software_route_probe_items) {
                    partition.push_back({item.item, item.records, {results + pass::hzb_results_flags_offset + item.base, item.records.size()}, {survivors + item.base, item.records.size()}, results[pass::hzb_results_pass1_count_offset + item.item], results[pass::hzb_results_pass2_count_offset + item.item]});
                    references.push_back({.item_index = item.item, .model = item.model, .records = item.records, .positions = item.positions, .indices = item.indices, .double_sided = item.double_sided});
                }
                if (!software::validate_gpu_partition(entries, partition))
                    return fail();
                software::camera_parameters camera{.view = this->software_route_probe_view, .proj = this->software_route_probe_proj, .width = captured.width, .height = captured.height};
                // Independent diagnostic CSR uses the GPU's actual IDs, without reclassifying candidates.
                deren::utility::vessel::vector<deren::utility::vessel::vector<software::tile_entry>> bins(grid.tile_count());
                for (auto const& entry : entries) {
                    auto const& item = *std::find_if(references.begin(), references.end(), [&](auto const& value) { return value.item_index == entry.item_index; });
                    auto const projected = software::project(item.records[entry.meshlet_id], item.model, camera, grid);
                    if (!projected.on_screen)
                        continue;
                    for (auto y = projected.tile_min_y; y <= projected.tile_max_y; ++y)
                        for (auto x = projected.tile_min_x; x <= projected.tile_max_x; ++x)
                            bins[std::size_t(y) * grid.tiles_x + x].push_back(entry);
                }
                software::software_tile_lists lists;
                lists.grid = grid;
                lists.cluster_count = static_cast<std::uint32_t>(entries.size());
                lists.offsets.push_back(0u);
                for (auto const& bin : bins) {
                    lists.counts.push_back(static_cast<std::uint32_t>(bin.size()));
                    lists.entries.insert(lists.entries.end(), bin.begin(), bin.end());
                    lists.offsets.push_back(static_cast<std::uint32_t>(lists.entries.size()));
                }
                lists.entry_count = static_cast<std::uint32_t>(lists.entries.size());
                auto reference = software::rasterize_tiles(lists, {.camera = camera, .items = references});
                if (!reference)
                    return fail();
                std::array<char const*, 2> names{"-cpu-depth.u32", "-cpu-id.u32"};
                std::array<std::uint32_t const*, 2> data{reference->depth_bits.data(), reference->id.data()};
                for (std::size_t i = 0; i < names.size(); ++i) {
                    std::ofstream output(std::string(prefix) + names[i], std::ios::binary | std::ios::out | std::ios::noreplace);
                    output.write(reinterpret_cast<char const*>(data[i]), static_cast<std::streamsize>(required));
                    if (!output)
                        return fail();
                }
                std::ofstream report(std::string(prefix) + "-route.json", std::ios::out | std::ios::noreplace);
                report << "{\"partition_valid\":true,\"slot\":" << this->software_route_probe_slot
                       << ",\"software_candidates\":" << entries.size() << ",\"budget\":" << this->software_route_probe_budget << "}\n";
                if (!report)
                    return fail();
                deren::utility::log("software GPU capture: partition valid, {} software candidates, slot {}", entries.size(), this->software_route_probe_slot);
            }
            std::array<char const*, 2> const suffixes{"-depth.u32", "-id.u32"};
            for (std::size_t i = 0; i < suffixes.size(); ++i) {
                auto* buffer = this->software_probe_readbacks[i].get();
                if (!buffer)
                    return std::unexpected(deren::utility::vessel::string("software image capture: missing buffer"));
                auto const bytes = buffer->mapped();
                if (bytes.size() < required)
                    return std::unexpected(deren::utility::vessel::string("software image capture: short buffer"));
                std::ofstream output(std::string(prefix) + suffixes[i], std::ios::binary | std::ios::out | std::ios::noreplace);
                output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(required));
                if (!output)
                    return std::unexpected(deren::utility::vessel::string("software image capture: file write failed"));
            }
        }
        if (auto const* path = std::getenv("DEREN_GAUSSIAN_HDR_CAPTURE"); path && this->gaussian_hdr_readback) {
            auto const bytes = this->gaussian_hdr_readback->mapped();
            std::uint64_t const required = static_cast<std::uint64_t>(this->gaussian_hdr_extent.width) * this->gaussian_hdr_extent.height * 8u;
            if (!required || bytes.size() < required) {
                return std::unexpected(deren::utility::vessel::string("Gaussian HDR capture: FAIL unavailable readback"));
            }
            std::ofstream output(path, std::ios::binary);
            deren::utility::vessel::array<std::uint32_t, 2> const dimensions = {this->gaussian_hdr_extent.width, this->gaussian_hdr_extent.height};
            output.write(reinterpret_cast<char const*>(dimensions.data()), sizeof(dimensions));
            output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(required));
            if (!output) {
                return std::unexpected(deren::utility::vessel::string("Gaussian HDR capture: FAIL file write"));
            }
        }

        // THE DEPTH-PROBE CAPTURE's file (visibility-buffer root-cause measurement): same wait, same frame, a
        // raw dump the comparison script reads (scripts/measure/depth_probe.py): u32 width, u32 height, then
        // one u32 per texel - the depth format's own bits (D32_SFLOAT float bits on this device, logged by the
        // backend copy).
        if (auto const* path = std::getenv("DEREN_DEPTH_PROBE_CAPTURE"); path && this->depth_probe_readback) {
            auto const bytes = this->depth_probe_readback->mapped();
            std::uint64_t const required = static_cast<std::uint64_t>(this->depth_probe_extent.width) * this->depth_probe_extent.height * 4u;
            if (!required || bytes.size() < required) {
                return std::unexpected(deren::utility::vessel::string("depth probe capture: FAIL unavailable readback"));
            }
            std::ofstream output(path, std::ios::binary);
            deren::utility::vessel::array<std::uint32_t, 2> const dimensions = {this->depth_probe_extent.width, this->depth_probe_extent.height};
            output.write(reinterpret_cast<char const*>(dimensions.data()), sizeof(dimensions));
            output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(required));
            if (!output) {
                return std::unexpected(deren::utility::vessel::string("depth probe capture: FAIL file write"));
            }
        }
        // THE HZB PROBE CAPTURE's files (M3 phase A2, spec §10.2): the SAME wait, the SAME frame - the copy was
        // recorded after the graph by runtime.post_graph.cpp, so by here the pyramid and header lanes are complete
        // and their readback buffers are mapped. The implementation is runtime.probes.cppm's (the diagnostics
        // partition); it is a no-op unless DEREN_HZB_PROBE_CAPTURE is set AND the frame's build lane recorded.
        this->write_hzb_probe_capture();
        // ... AND THE PHASE-B CULL PROBE'S FILES + THE IN-ENGINE REFERENCE VERIFICATION (M3 phase B1, spec §6.7):
        // its history-lane and survivor copies were recorded INSIDE the cull lane (before the build lane rewrote the
        // history lane), and the other dumps are the mapped tables - all final by this same idle point. A no-op
        // unless DEREN_HZB_CULL_PROBE_CAPTURE is set and the cull lane recorded on the screenshot frame.
        this->write_hzb_cull_probe_capture();
        std::size_t const buffer_size = static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height) * 4u;
        std::span<std::byte> const mapped = slot->mapped();
        if (mapped.empty() || mapped.size() < buffer_size) {
            return std::unexpected(deren::utility::vessel::string("screenshot: read-back slot unavailable or too small"));
        }

        frame_image result = {};
        result.width = extent.width;
        result.height = extent.height;
        result.rgba.resize(buffer_size);
        auto const* source = reinterpret_cast<std::uint8_t const*>(mapped.data());
        if (bgra) {
            // the swapchain is BGRA (sRGB); the PNG writer wants RGBA
            for (std::size_t i = 0; i < result.rgba.size(); i += 4) {
                result.rgba[i + 0] = source[i + 2]; // R
                result.rgba[i + 1] = source[i + 1]; // G
                result.rgba[i + 2] = source[i + 0]; // B
                result.rgba[i + 3] = source[i + 3]; // A
            }
        } else {
            std::memcpy(result.rgba.data(), source, buffer_size);
        }
        return result;
    }

    bool runtime::consume_screenshot_request() noexcept {
        // Could the requested frame be captured? Single-shot by design: the flag is cleared HERE, on
        // success and on failure alike. A failing read-back (unsupported swapchain format, missing
        // read-back buffer) used to leave the flag set, so main's loop called
        // acquire_current_frame_image() - which begins with a device-wide wait - and logged an error
        // every single frame until exit. A dropped capture is the correct outcome; one F12 is one
        // attempt.
        bool const captured = this->screenshot_pending;
        this->screenshot_pending = false;
        return captured;
    }
} // namespace deren::engine
