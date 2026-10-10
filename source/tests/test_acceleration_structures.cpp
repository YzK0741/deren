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
// The native-handle exit (its S1 step) moves the acceleration-structure half of the engine behind the
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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h> // the migrated module's recording takes the driver's command buffer; a TEST may name it (it is not the engine half)

import deren.promise.rhi;
import deren.engine.backend_loader;         // load_api_core(): the acquisition lives outside the runtime
import deren.engine.acceleration_structure; // the module S1 migrates (bottom_level_structures / top_level_structure)

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"
#include "rt_traversal_probe.hpp"

namespace {
    namespace rhi = deren::promise::rhi;
    /// the migrated module's own namespace (geometry_source, ottom_level_structures, ...)
    namespace as = deren::engine::acceleration_structure;

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
    /// environment at all (the note in source/tests/test_runtime_dyn.cpp records the hang), and the picture is not
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
    CHECK(rhi::abi_version == 31u);

    if (!wants_device(argc, argv)) {
        deren::vk_test::write_line("as_probe: device path skipped (pass --with-device to run the real instrument)");
        return deren::vk_test::finish("test_acceleration_structures");
    }

    rhi::create_info const creation = probe_create_info();
    std::shared_ptr<rhi::api_core> core = deren::engine::load_api_core(creation);
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
    bool const multi_geometry = std::any_of(argv + 1, argv + argc, [](char const* arg) { return std::string_view(arg) == "--multi-geometry"; });
    std::vector<vertex> vertices{{.position = {0.0f, 0.0f, 0.0f}},
                                 {.position = {1.0f, 0.0f, 0.0f}},
                                 {.position = {0.0f, 1.0f, 0.0f}},
                                 {.position = {10.0f, 0.0f, 0.0f}},
                                 {.position = {11.0f, 0.0f, 0.0f}},
                                 {.position = {10.0f, 1.0f, 0.0f}}};
    std::uint32_t const indices[6] = {0u, 1u, 2u, 0u, 1u, 2u};
    if (!multi_geometry) {
        vertices.resize(triangle_vertices);
    }
    auto const index_data = std::span<std::uint32_t const>(indices).first(multi_geometry ? 6u : triangle_vertices);
    rhi::buffer* const vertex_buffer = core->create_buffer(rhi::buffer_desc{
        .size = vertices.size() * sizeof(vertex), .usage = rhi::buffer_usage::storage_coherent, .flags = build_input_flags});
    rhi::buffer* const index_buffer = core->create_buffer(rhi::buffer_desc{
        .size = index_data.size_bytes(), .usage = rhi::buffer_usage::storage_coherent, .flags = build_input_flags});
    CHECK(vertex_buffer != nullptr);
    CHECK(index_buffer != nullptr);
    if (vertex_buffer == nullptr || index_buffer == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    std::memcpy(vertex_buffer->mapped().data(), vertices.data(), vertices.size() * sizeof(vertex));
    std::memcpy(index_buffer->mapped().data(), index_data.data(), index_data.size_bytes());

    as::geometry_source const source{
        .vertex_address = addresses->buffer_address(*vertex_buffer, 0),
        .vertex_stride = sizeof(vertex),
        .vertex_count = triangle_vertices,
        .index_address = addresses->buffer_address(*index_buffer, 0),
        .index_type = deren::promise::rhi::index_type::uint32,
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

    // ---- 4. THE STRUCTURES, THROUGH THE TIER-1 INTERFACE (abi 26): the two levels, the refit the flag
    //         declares, and the refusals the interface promises - each as its own check.
    //
    //         THE BACKEND OWNS EVERYTHING THE CALLER DOES NOT SEE (the storage, the scratch, the alignment,
    //         the sizes), which is why this section has no size query, no buffer and no offset in it: that is
    //         the whole difference from the engine module's own path, which the probe drove before this batch.
    rhi::acceleration_structure_geometry const geometry{
        .vertex_address = addresses->buffer_address(*vertex_buffer, 0),
        .vertex_stride = sizeof(vertex),
        .vertex_count = triangle_vertices,
        .index_address = addresses->buffer_address(*index_buffer, 0),
        .index_format = rhi::index_type::uint32,
        .index_count = triangle_vertices,
    };
    // --multi-geometry uses different counts, with the probe's hit triangle in the SECOND geometry.
    // Missing per-geometry ranges either read past the first triangle or lose the ray hit.
    auto away_geometry = geometry;
    away_geometry.vertex_address += triangle_vertices * sizeof(vertex);
    away_geometry.index_address = 0;
    away_geometry.index_count = 0;
    auto hit_geometry = geometry;
    hit_geometry.index_count = 6u;
    std::array<rhi::acceleration_structure_geometry, 2> const geometries{away_geometry, hit_geometry};
    rhi::acceleration_structure* const blas = core->create_acceleration_structure(rhi::acceleration_structure_desc{
        .type = rhi::acceleration_structure_type::bottom_level,
        .flags = rhi::to_bits(rhi::acceleration_structure_flag::allow_update),
        .geometries = multi_geometry ? geometries.data() : &geometry,
        .geometry_count = multi_geometry ? static_cast<std::uint32_t>(geometries.size()) : 1u,
    });
    CHECK(blas != nullptr);
    rhi::acceleration_structure* const static_blas = core->create_acceleration_structure(rhi::acceleration_structure_desc{
        .type = rhi::acceleration_structure_type::bottom_level,
        .geometries = &geometry,
        .geometry_count = 1u,
    });
    CHECK(static_blas != nullptr);
    rhi::acceleration_structure* const tlas = core->create_acceleration_structure(rhi::acceleration_structure_desc{
        .type = rhi::acceleration_structure_type::top_level,
        .instance_capacity = 1u,
    });
    CHECK(tlas != nullptr);
    if (blas == nullptr || static_blas == nullptr || tlas == nullptr) {
        return deren::vk_test::finish("test_acceleration_structures");
    }
    deren::vk_test::write_line("as_probe: created blas address={:#x} size={} bytes, tlas address={:#x} size={} bytes",
                               blas->device_address(), blas->size_bytes(), tlas->device_address(), tlas->size_bytes());
    CHECK(blas->device_address() != 0u); // what a shader and an instance record reach it by
    CHECK(blas->size_bytes() != 0u);     // what an address-range descriptor would carry
    CHECK(tlas->device_address() != 0u);

    // THE INSTANCE the traversal reads: an identity 3x4 transform, the BLAS's own address as its reference.
    rhi::acceleration_structure_instance instance{};
    instance.transform[0] = 1.0f;
    instance.transform[5] = 1.0f;
    instance.transform[10] = 1.0f;
    instance.structure_reference = blas->device_address();
    CHECK(tlas->write_instances(std::span<rhi::acceleration_structure_instance const>(&instance, 1u)) == rhi::error::ok);
    // (a) THE CAPACITY REFUSAL: one past what it was created with - the array behind it is sized for one.
    rhi::acceleration_structure_instance const too_many[] = {instance, instance};
    CHECK(tlas->write_instances(too_many) == rhi::error::invalid_argument);
    // (b) A BOTTOM LEVEL HAS NO INSTANCE LIST, refused by name rather than silently dropped.
    CHECK(blas->write_instances(std::span<rhi::acceleration_structure_instance const>(&instance, 1u)) == rhi::error::unsupported);

    // ---- 5. THE RECORDING: both builds, the refit the flag allows, and the one it does not.
    CHECK(commands->build_acceleration_structure(*blas) == rhi::error::ok);
    CHECK(commands->build_acceleration_structure(*static_blas) == rhi::error::ok);
    CHECK(commands->build_acceleration_structure(*tlas) == rhi::error::ok);
    CHECK(commands->refit_acceleration_structure(*blas) == rhi::error::ok);                 // created with allow_update
    CHECK(commands->refit_acceleration_structure(*static_blas) == rhi::error::unsupported); // ... and this one was not

    // ---- 5b. THE ENGINE MODULE ON TOP OF IT (plan S1's P1b-2): `bottom_level_structures` is a thin front end
    //          over the same interface now, and this is what says so on a real device - it adds the same
    //          geometry, records its build and a refit, and hands back the object the runtime binds.
    // Recorded commands borrow their structures and inputs until submission has completed.
    as::bottom_level_structures levels{*core};
    rhi::object_manager<rhi::micromap> micromap_owner;
    rhi::object_manager<rhi::acceleration_structure> shaded_owner;
    rhi::object_manager<rhi::acceleration_structure> triangle_soup;
    {
        rhi::acceleration_structure_geometry const unindexed{
            .vertex_address = source.vertex_address,
            .vertex_stride = source.vertex_stride,
            .vertex_count = triangle_vertices,
        };
        triangle_soup = rhi::object_manager<rhi::acceleration_structure>{core->create_acceleration_structure(rhi::acceleration_structure_desc{
            .type = rhi::acceleration_structure_type::bottom_level,
            .geometries = &unindexed,
            .geometry_count = 1u,
        })};
        CHECK(triangle_soup);
        if (triangle_soup) {
            CHECK(commands->build_acceleration_structure(*triangle_soup) == rhi::error::ok);
        }
        // (a) THE DOCUMENTED SKIP: a source with no triangles keeps the caller's index alignment.
        std::expected<std::uint32_t, std::string> const skipped = levels.add(as::geometry_source{.vertex_count = 0, .index_count = 0});
        CHECK(skipped.has_value() && *skipped == 0u);
        CHECK(levels.structure(*skipped) == nullptr);
        std::expected<std::uint32_t, std::string> const via_module = levels.add(source, /*refittable=*/true);
        CHECK_MSG(via_module.has_value(), via_module.has_value() ? nullptr : via_module.error().c_str());
        if (via_module.has_value()) {
            CHECK(levels.size() == 2u);
            CHECK(levels.structure(*via_module) != nullptr);
            CHECK(levels.structure(*via_module)->device_address() != 0u);
            std::expected<void, std::string> const built = levels.record_build(*commands);
            CHECK_MSG(built.has_value(), built.has_value() ? nullptr : built.error().c_str());
            std::uint32_t const refit[] = {*via_module};
            std::expected<void, std::string> const refitted = levels.record_update(*commands, refit);
            CHECK_MSG(refitted.has_value(), refitted.has_value() ? nullptr : refitted.error().c_str());
            deren::vk_test::write_line("as_probe: the module built and refitted through the tier-1 interface: {} geometries",
                                       levels.last_stats().geometry_count);
        }
        // (b) THE OPACITY MICROMAP, THROUGH THE SAME INTERFACE (plan S1's P4): one micro-triangle, UNKNOWN, and a
        //     geometry that consults it. THE ORDER IS THE CALLER'S AND IT MATTERS: the micromap is built before
        //     the geometry that reads it, which is what the two record calls below do.
        if (capabilities->mesh_shader() || true) {                        // the micromap path is independent of the mesh one; kept flat on purpose
            std::vector<std::byte> const attributes(4u, std::byte{0x03}); // the 4-state "unknown" pair
            rhi::micromap_triangle const record{
                .data_offset = 0u,
                .subdivision_level = 0u,
                .format = static_cast<std::uint16_t>(rhi::micromap_format::four_state),
            };
            std::uint32_t const micro_index = 0u;
            rhi::micromap* const micromap = core->create_micromap(rhi::micromap_desc{
                .triangle_count = 1u,
                .data = attributes,
                .data_stride = 4u,
                .triangles = std::span<rhi::micromap_triangle const>(&record, 1u),
                .indices = std::span<std::uint32_t const>(&micro_index, 1u),
                .format = rhi::micromap_format::four_state,
            });
            CHECK(micromap != nullptr); // a device without VK_EXT_opacity_micromap answers null with a named log
            if (micromap != nullptr) {
                micromap_owner = rhi::object_manager<rhi::micromap>{micromap};
                rhi::acceleration_structure_geometry const with_micromap{
                    .vertex_address = source.vertex_address,
                    .vertex_stride = source.vertex_stride,
                    .vertex_count = triangle_vertices,
                    .index_address = source.index_address,
                    .index_format = rhi::index_type::uint32,
                    .index_count = triangle_vertices,
                    .opacity_micromap = micromap,
                };
                rhi::acceleration_structure* const shaded = core->create_acceleration_structure(rhi::acceleration_structure_desc{
                    .type = rhi::acceleration_structure_type::bottom_level,
                    .geometries = &with_micromap,
                    .geometry_count = 1u,
                });
                CHECK(shaded != nullptr);
                if (shaded != nullptr) {
                    shaded_owner = rhi::object_manager<rhi::acceleration_structure>{shaded};
                    CHECK(commands->build_micromap(*micromap) == rhi::error::ok); // ... the micromap FIRST
                    CHECK(commands->barrier(rhi::barrier_group{
                              .has_memory = true,
                              .memory = rhi::memory_barrier{.from = rhi::buffer_use::micromap_write, .to = rhi::buffer_use::micromap_read},
                          }) == rhi::error::ok);
                    CHECK(commands->build_acceleration_structure(*shaded) == rhi::error::ok);
                }
            }
            deren::vk_test::write_line("as_probe: an opacity micromap was created, built and consulted by a geometry");
        }
    }

    CHECK(commands->end_recording() == rhi::error::ok);

    // ---- 6. THE SUBMISSION, THROUGH THE ESCAPE (plan X4's second item: the contract has no submit verb for a
    //         buffer the CALLER owns, which is why this is still a raw pair here).
    {
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
        CHECK(vkWaitForFences(device, 1u, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS); // a hung build fails the probe
        vkDestroyFence(device, fence, nullptr);
        deren::vk_test::write_line("as_probe: built, refit, submitted and waited: 2 bottom level + 1 top level");
    }

    bool native_heap = false;
    for (int index = 1; index < argc; ++index) {
        native_heap = native_heap || std::string_view(argv[index]) == "--native-heap-traversal";
    }
    check_rt_traversal(*core, *escape, *addresses, *capabilities, *blas, *tlas, argv[0], native_heap);

    blas->release();
    static_blas->release();
    tlas->release();
    commands->release();
    vertex_buffer->release();
    index_buffer->release();
    deren::vk_test::write_line("as_probe: done (validation was ON: any VUID appears above)");
    return deren::vk_test::finish("test_acceleration_structures");
}
