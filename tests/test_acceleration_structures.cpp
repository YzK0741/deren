// ============================================================================
// THE ACCELERATION-STRUCTURE DEVICE PROBE - the SMALL instrument the S1 migration runs against.
//
// WHY IT EXISTS, AND WHY IT IS SEPARATE FROM THE rt_shadows SMOKE: the only end-to-end ray-tracing
// instrument this repository had is `render-check/rt_smoke.toml` - the full Sponza scene, a 1080x960 frame
// and the whole pass chain. That is the right acceptance for the RENDERER and the wrong instrument for a
// MIGRATION: it needs hundreds of MB, so on a host that is short of memory it fails for reasons that have
// nothing to do with the code under test (measured at HEAD, see the recording-face note), and it reports one
// bit - pass or fail - for a whole frame.
//
// S1 (docs/rhi/NATIVE_HANDLE_EXIT_PLAN.md) moves the acceleration-structure half of the engine behind the
// `ray_tracing` ability. This probe is what says each step of that migration still works, and it is written
// against THE SAME MODULE SURFACE the migration touches - `bottom_level_structures::add()` /
// `record_build()` / `record_update()` and `top_level_structure::begin()` / `add()` / `record_build()` - so
// it keeps its meaning on both sides of the change:
//
//   1. a REAL device, a hidden 64x64 window, VALIDATION ON (the point is the VUIDs the migration could
//      introduce, not a picture);
//   2. a THREE-VERTEX triangle and ONE instance: kilobytes, not scene data;
//   3. the three things the migration is most likely to break, each checked separately:
//        * the CREATE + SIZES path (`add()` twice, one refittable),
//        * the BUILD path (a recorded `vkCmdBuildAccelerationStructuresKHR` that actually executes),
//        * the REFIT path (`record_update()`, which reuses the build's scratch and re-reads the same bytes),
//      plus the module's documented SKIP (a source with no triangles keeps the caller's indices aligned
//      and answers a null handle).
//
// IT SUBMITS THE WORK ITSELF, and that is deliberate: the engine's frame owns the frame's command buffer,
// so an instrument that went through the frame loop would be measuring the frame loop too. The probe creates
// a CALLER-OWNED contract command buffer, records the structures into it, and submits it with a fence through
// the escape - which also makes this file the measured evidence for plan X4's second item (the contract has
// no submit verb for a buffer the CALLER owns; the probes in runtime.probes.cppm do the same raw submit).
//
// THE SAFE DEFAULT: without `--with-device` it prints what it would do and exits 0, so it runs anywhere.
// Run it with the flag in every slice of the migration - that is what it is for.
// ============================================================================
#include "vk_test.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h> // the migrated module's recording takes the driver's command buffer; a TEST may name it (it is not the engine half)

import deren.promise.rhi;
import deren.vulkan.backend_loader;         // load_api_core(): the acquisition lives outside the runtime
import deren.vulkan.acceleration_structure; // the module S1 migrates (bottom_level_structures / top_level_structure)

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"

namespace {
    namespace rhi = deren::promise::rhi;
    /// the migrated module's own namespace (geometry_source, ottom_level_structures, ...)
    namespace as = deren::vulkan::acceleration_structure;

    /// Is this run allowed to build a device? The flag is required rather than defaulted: the device path is
    /// slow (a real instance, device and swapchain) and a machine without a Vulkan device must still run the
    /// no-flag half.
    [[nodiscard]] bool wants_device(int const argc, char** const argv) {
        for (int i = 1; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--with-device") {
                return true;
            }
        }
        return false;
    }

    /// A three-vertex triangle at unit scale, and the frame's only geometry.
    constexpr std::uint32_t triangle_vertices = 3u;
    struct vertex {
        float position[3];
    };

    /// The device the probe asks for: 64x64 and HIDDEN. A visible window does not come up in this
    /// environment at all (the note in tests/test_runtime_dyn.cpp records the hang), and the picture is not
    /// what this instrument measures.
    [[nodiscard]] rhi::create_info probe_create_info() {
        rhi::create_info creation{};
        creation.window_title = "deren acceleration-structure probe";
        creation.window_width = 64;
        creation.window_height = 64;
        creation.window_visible = false;
        creation.validation_layers = true; // the point of the instrument: VUIDs the migration could introduce
        return creation;
    }
} // namespace

int main(int const argc, char** const argv) {
    deren::vk_test::write_line("as_probe: this executable compiled abi {}", rhi::abi_version);
    CHECK(rhi::abi_version == 25u);

    if (!wants_device(argc, argv)) {
        deren::vk_test::write_line("as_probe: device path skipped (pass --with-device to run the real instrument)");
        return deren::vk_test::finish("test_acceleration_structures");
    }

    rhi::create_info const creation = probe_create_info();
    std::shared_ptr<rhi::api_core> core = deren::vulkan::load_api_core(creation);
    CHECK(core != nullptr);
    if (core == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }

    // ---- 1. THE DEVICE'S HALF: the escape (native handles, the probe's own submit) and the capability that
    //         decides whether acceleration structures exist at all on this device.
    rhi::vulkan_escape* const escape = rhi::query_extension<rhi::vulkan_escape>(*core);
    CHECK(escape != nullptr);
    rhi::device_capabilities* const capabilities = rhi::query_extension<rhi::device_capabilities>(*core);
    CHECK(capabilities != nullptr); // announced unconditionally by this backend (see the ability's note)
    if (escape == nullptr || capabilities == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    deren::vk_test::write_line("as_probe: mesh_shader={} ray_query={} max_push_constants={} graphics_queue_family={}",
                               capabilities->mesh_shader(), capabilities->ray_query(), capabilities->max_push_constants_size(),
                               capabilities->graphics_queue_family());

    // A DEVICE WITHOUT THE EXTENSION IS A SKIP, NOT A FAILURE: the question this probe answers is "does the
    // engine's acceleration-structure path work", and on a device that cannot have one there is nothing to
    // answer - the same "capability first" order every path in the engine follows.
    bool acceleration_structure_available = false;
    for (char const* const enabled : escape->enabled_device_extensions()) {
        if (enabled != nullptr && std::strcmp(enabled, "VK_KHR_acceleration_structure") == 0) {
            acceleration_structure_available = true;
        }
    }
    if (!acceleration_structure_available) {
        deren::vk_test::write_line("as_probe: VK_KHR_acceleration_structure is not enabled on this device - skipping");
        return deren::vk_test::finish("test_acceleration_structures");
    }

    rhi::device_address* const addresses = rhi::query_extension<rhi::device_address>(*core);
    CHECK(addresses != nullptr);
    if (addresses == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }

    // ---- 2. THE GEOMETRY: three vertices and three indices, in buffers the build can READ - which is the
    //         `device_address` + `acceleration_structure_input` pair the renderer's own build inputs carry.
    constexpr rhi::buffer_flags build_input_flags =
        rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::acceleration_structure_input);
    std::vector<vertex> const vertices{{.position = {0.0f, 0.0f, 0.0f}},
                                       {.position = {1.0f, 0.0f, 0.0f}},
                                       {.position = {0.0f, 1.0f, 0.0f}}};
    std::uint32_t const indices[triangle_vertices] = {0u, 1u, 2u};
    rhi::buffer* const vertex_buffer = core->create_buffer(rhi::buffer_desc{
        .size = sizeof(vertices), .usage = rhi::buffer_usage::storage_coherent, .flags = build_input_flags});
    rhi::buffer* const index_buffer = core->create_buffer(rhi::buffer_desc{
        .size = sizeof(indices), .usage = rhi::buffer_usage::storage_coherent, .flags = build_input_flags});
    CHECK(vertex_buffer != nullptr);
    CHECK(index_buffer != nullptr);
    if (vertex_buffer == nullptr || index_buffer == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    std::memcpy(vertex_buffer->mapped().data(), vertices.data(), sizeof(vertices));
    std::memcpy(index_buffer->mapped().data(), indices, sizeof(indices));

    as::geometry_source const source{
        .vertex_address = addresses->buffer_address(*vertex_buffer, 0),
        .vertex_stride = sizeof(vertex),
        .vertex_count = triangle_vertices,
        .index_address = addresses->buffer_address(*index_buffer, 0),
        .index_type = VK_INDEX_TYPE_UINT32,
        .index_count = triangle_vertices,
    };
    deren::vk_test::write_line("as_probe: geometry vertex_address={:#x} index_address={:#x} triangles={}",
                               source.vertex_address, source.index_address, source.index_count / 3u);
    CHECK(source.vertex_address != 0u);
    CHECK(source.index_address != 0u);

    // ---- 3. THE COMMAND BUFFER THE PROBE OWNS (see the header): the contract made it, the escape turns it
    //         into the native handle the module's recording takes, and the probe submits it itself.
    rhi::command_buffer* const commands = core->create_command_buffer({});
    CHECK(commands != nullptr);
    if (commands == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    auto* const native_commands = static_cast<VkCommandBuffer>(escape->native_command_buffer(*commands));
    CHECK(native_commands != VK_NULL_HANDLE);

    // ---- 4. THE STRUCTURES. Two bottom-level entries (one refittable, which is the path a compute-skinning
    //         frame takes) and one top-level slot with one instance.
    {
        as::bottom_level_structures levels{*core};

        // (a) THE SKIP the module documents: a source with no triangles keeps the caller's index alignment and
        //     answers a null handle rather than an error.
        std::uint32_t const skipped = *levels.add(as::geometry_source{.vertex_count = 0, .index_count = 0});
        CHECK(skipped == 0u);
        CHECK(levels.handle(skipped) == VK_NULL_HANDLE);

        std::expected<std::uint32_t, std::string> const first = levels.add(source, /*refittable=*/true);
        CHECK_MSG(first.has_value(), first.has_value() ? nullptr : first.error().c_str());
        std::expected<std::uint32_t, std::string> const second = levels.add(source, /*refittable=*/false);
        CHECK_MSG(second.has_value(), second.has_value() ? nullptr : second.error().c_str());
        if (!first.has_value() || !second.has_value()) {
            return deren::vk_test::finish("test_acceleration_structures");
        }
        CHECK(*first == 1u); // the skipped source took index 0
        CHECK(*second == 2u);
        CHECK(levels.size() == 3u);
        CHECK(levels.handle(*first) != VK_NULL_HANDLE);
        CHECK(levels.handle(*second) != VK_NULL_HANDLE);

        // (b) THE BUILD: one recorded call that builds every entry (see the class note on batching).
        std::expected<void, std::string> const built = levels.record_build(native_commands);
        CHECK_MSG(built.has_value(), built.has_value() ? nullptr : built.error().c_str());
        if (!built.has_value()) {
            return deren::vk_test::finish("test_acceleration_structures");
        }
        deren::vk_test::write_line("as_probe: bottom level built: {} geometries, {} triangles, {} bytes of scratch",
                                   levels.last_stats().geometry_count, levels.last_stats().triangle_count,
                                   levels.last_stats().scratch_bytes);

        // (c) THE TOP LEVEL: one instance referring to the refittable entry.
        as::top_level_structure top{*core, 1u};
        CHECK(top.begin(0u).has_value());
        as::instance_source instance{};
        instance.blas_index = *first;
        std::expected<void, std::string> const instanced = top.add(levels, instance);
        CHECK_MSG(instanced.has_value(), instanced.has_value() ? nullptr : instanced.error().c_str());
        std::expected<void, std::string> const top_built = top.record_build(native_commands);
        CHECK_MSG(top_built.has_value(), top_built.has_value() ? nullptr : top_built.error().c_str());
        CHECK(top.handle(0u) != VK_NULL_HANDLE);
        if (!top_built.has_value()) {
            return deren::vk_test::finish("test_acceleration_structures");
        }

        // (d) THE REFIT: the same addresses and counts, so the structure is updated in place and reuses the
        //     build's scratch. This is the path most likely to break when the scratch stops being the engine's.
        std::uint32_t const refit[] = {*first};
        std::expected<void, std::string> const refitted = levels.record_update(native_commands, refit);
        CHECK_MSG(refitted.has_value(), refitted.has_value() ? nullptr : refitted.error().c_str());

        CHECK(commands->end_recording() == rhi::error::ok);

        // ---- 5. THE SUBMISSION, THROUGH THE ESCAPE (plan X4's second item: the contract has no submit verb
        //         for a buffer the CALLER owns, which is why this is still a raw pair here).
        VkDevice const device = static_cast<VkDevice>(escape->native_device());
        VkQueue const queue = static_cast<VkQueue>(escape->native_queue());
        VkFenceCreateInfo const fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        VkFence fence = VK_NULL_HANDLE;
        CHECK(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);
        VkSubmitInfo const submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .pNext = nullptr,
                                  .waitSemaphoreCount = 0u,
                                  .pWaitSemaphores = nullptr,
                                  .pWaitDstStageMask = nullptr,
                                  .commandBufferCount = 1u,
                                  .pCommandBuffers = &native_commands,
                                  .signalSemaphoreCount = 0u,
                                  .pSignalSemaphores = nullptr};
        CHECK(vkQueueSubmit(queue, 1u, &submit, fence) == VK_SUCCESS);
        CHECK(vkWaitForFences(device, 1u, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS); // 10 s: a hung build must fail the probe rather than hang it
        vkDestroyFence(device, fence, nullptr);

        // THE BUILD ACTUALLY EXECUTED, which is the one thing a recorded-but-never-submitted buffer cannot say.
        deren::vk_test::write_line("as_probe: built, submitted and waited: {} structures, {} instances",
                                   levels.size(), 1u);
    }

    commands->release();
    deren::vk_test::write_line("as_probe: done (validation was ON: any VUID appears above)");
    return deren::vk_test::finish("test_acceleration_structures");
}
