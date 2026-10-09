// ============================================================================
// module: deren.vulkan.runtime:probes  - the heap probes, which are diagnostics rather than production
//
// run_heap_probe reads back one slot of the bindless texture array to prove the host wrote it, and
// run_heap_graphics_probe renders the bindings of one material through the heap-native shaders and
// reads the result back. Both exist to answer "did the heap get what the shader expects" without a
// validation layer, which is how this migration was debugged - and neither belongs in the production
// runtime, so they live in their own partition.
//
// Imports are NOT transitive: this partition imports what the probe code calls for itself.
// ============================================================================
module;

#include <GLFW/glfw3.h>
#include <algorithm> // std::min in the resource publication
#include <bit>       // std::bit_cast for the caster world-matrix hash
#include <chrono>
#include <cstring>  // std::memcpy, for composing a pass's push block
#include <expected> // std::expected: `image::get_content()` answers the content or the named reason
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>   // std::as_bytes for the init_utils calls (the bytes behind a UBO or a zeroed table)
#include <thread> // std::this_thread::yield in the frame limiter
#include <vector> // the destination of a host image copy, when that read-back path is taken

module deren.vulkan.runtime:probes;

import :declarations;
import deren.vulkan.profiling;
import deren.vulkan.pipelines;
import deren.vulkan.render_resource;
import deren.vulkan.render_resource.shared;

import deren.utility;
import deren.vulkan.constant_init;
import deren.vulkan.frame_constants; // one frame's shared constants (see update_frame_constants)

// Route std::pmr allocations through mimalloc for this TU (deren.utility:better_pmr). Idempotent:
// init_pmr() returns the same process-wide singleton no matter which TU calls it first, so
// main.cpp's keep-alive and this one coexist safely. The reference itself is never read; it
// only forces the (dynamic) initialization before any pmr container in this TU is constructed.
[[maybe_unused]] static auto& pmr = deren::utility::init_pmr(); // NOLINT(keep-alive)

namespace deren::vulkan {
    std::shared_ptr<rhi::command_buffer> runtime::make_probe_commands(std::string_view const what) {
        // ONE PROBE'S OWN RECORDING SESSION, THROUGH THE CONTRACT (abi 22): a primary command buffer the backend
        // owns, begun with the one-time-submit usage the raw pool's TRANSIENT flag used to spell. The pool, the
        // queue family and the submission are the backend's now - the probe keeps what it always had, which is a
        // buffer of its own that nothing else records into (the mask bake looked isolated too and turned out to
        // record into the frame's buffer; this one really is).
        std::shared_ptr<rhi::command_buffer> commands = this->rhi_face().make_command_buffer({.kind = rhi::command_buffer_kind::primary});
        if (commands == nullptr) {
            deren::utility::log("descriptor heap: {} could not allocate a contract command buffer", what);
            return nullptr;
        }
        rhi::command_buffer_begin_info const begin = {.struct_size = sizeof(rhi::command_buffer_begin_info),
                                                      .usage = rhi::to_bits(rhi::command_buffer_usage::one_time_submit),
                                                      .next = nullptr};
        if (commands->begin_recording(begin) != rhi::error::ok) {
            deren::utility::log("descriptor heap: {} could not begin recording", what);
            return nullptr;
        }
        return commands;
    }

    bool runtime::submit_probe_commands(rhi::command_buffer& commands) {
        // THE CONTRACT SUBMITS IT (plan X4): `api_core::submit()` takes a list the CALLER owns as well as the
        // frame's, so an isolated probe buffer no longer goes to the queue through the escape - the raw
        // `vkQueueSubmit` that stood here was this file's second and last Vulkan call. NOTHING IS PRESENTED for an
        // owned list (no swapchain image, no acquire state), and the wait afterwards is still the contract's
        // `wait_idle()`, which the callers already perform.
        return this->rhi_face().submit(commands) == rhi::error::ok;
    }

    void runtime::run_heap_probe(uint32_t const texture_slot) {
        // ---- THE HEAP-NATIVE PROBE (see shaders/heap_probe_comp.slang and docs/descriptor_heap_migration.md) ----
        //
        // The whole migration assumes four things about the native path, and this is where they stop being
        // assumptions: a pipeline created with VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT and NO layout, a
        // shader that declares its resources with `descriptor_heap` and indexes the grid by slot, a combined image
        // sampler CONSTRUCTED at the use site from a sampler in the sampler heap, and its parameters delivered by
        // vkCmdPushDataEXT. It runs in its own command buffer at scene setup, which is what makes it isolated -
        // the mask bake looked isolated too and turned out to record into the frame's own buffer.
        rhi::api_core& vk = this->vulkan_core;
        // Slot 0 of the sampler heap is the texture sampler: the first of the six core::create_samplers makes, in
        // the order shaders/heap_slots.glsl names (the contract test compares that order, and the host has no
        // per-sampler constant because it keeps them as a list).
        uint32_t const sampler_slot = static_cast<uint32_t>(deren::vulkan::render_layout::heap_sampler_base);
        std::span<uint8_t const> const spirv = this->registered_shader("heap_probe.comp.spv");
        if (!contract_heap_ready(vk) || spirv.empty()) {
            return; // no heap, no grid or no shader: nothing to probe with, and no heap path to protect
        }
        auto const built = pipelines::build_heap_probe(vk, spirv);
        if (!built.has_value()) {
            deren::utility::log("descriptor heap: the heap-native probe's pipeline was refused: {}", built.error());
            return;
        }

        // The answer's buffer: 16 bytes, host-visible, and ADDRESSABLE because the push carries its address.
        // THE CONTRACT OWNS IT (object_manager releases the reference on every path out of this function,
        // including the two early returns below), and `mapped()` is the byte view `create_buffer` threw away
        // for everything that is not host-visible.
        rhi::object_manager<rhi::buffer> answer{this->vulkan_core.create_buffer(rhi::buffer_desc{
            .size = 16u,
            .usage = rhi::buffer_usage::storage_coherent,
            .flags = rhi::to_bits(rhi::buffer_flag::device_address),
        })};
        std::span<std::byte> const answer_bytes = answer ? answer->mapped() : std::span<std::byte>{};
        if (answer_bytes.empty()) {
            deren::utility::log("descriptor heap: the heap-native probe could not allocate its answer buffer");
            return;
        }
        uint64_t const answer_address = this->buffer_address(*answer);

        // ---- THE RECORDING, THROUGH THE CONTRACT (abi 22) ----
        // A contract command buffer of its own, the heap bind and the push through the `descriptor_heap` ability's
        // OWN verbs, a contract `dispatch`, a contract `submit` and a device idle. The hand-made
        // `VkCommandPool`/`VkCommandBuffer`/`VkFence`/`vkQueueSubmit`/`vkWaitForFences` that stood here named a
        // device and a queue the contract already owns, and the pipeline arrives as the CONTRACT object the
        // builder returned (no `get_pipeline()`).
        std::shared_ptr<rhi::command_buffer> const commands = this->make_probe_commands("the heap-native probe");
        if (commands == nullptr) {
            return;
        }
        rhi::descriptor_heap* const heap = rhi::query_extension<rhi::descriptor_heap>(vk);
        if (heap == nullptr) {
            deren::utility::log("descriptor heap: the heap-native probe has no descriptor_heap ability to bind");
            return;
        }
        rhi::heap_bind_info const heap_bind{.commands = commands.get()};
        if (heap->bind(heap_bind) != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native probe could not bind the heaps on its own command buffer");
            return;
        }
        if (commands->bind_pipeline(*built->trace->contract) != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native probe's pipeline was refused by the command buffer");
            return;
        }
        // The parameters, THROUGH PUSH DATA: there is no pipeline layout to push constants to, which is the flag's
        // requirement and the reason this function exists - and the push is the ability's verb now, not the raw
        // entry point's.
        std::array<uint32_t, 4> const push = {
            static_cast<uint32_t>(answer_address & 0xFFFFFFFFu),
            static_cast<uint32_t>(answer_address >> 32u),
            texture_slot,
            sampler_slot, // the host's choice of sampler, not the shader's
        };
        rhi::heap_push_info const push_info{.commands = commands.get(), .offset = 0u, .data = std::as_bytes(std::span(push))};
        if (heap->push_data(push_info) != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native probe's push data was refused");
            return;
        }
        commands->dispatch(1u, 1u, 1u);
        if (commands->end_recording() != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native probe could not close its recording");
            return;
        }
        if (!this->submit_probe_commands(*commands)) {
            deren::utility::log("descriptor heap: the heap-native probe's submission was refused");
            return;
        }
        // THE IDLE IS THE PROBE'S OWN REQUIREMENT, not a synchronisation shortcut: the answer is read out of HOST
        // memory below, so the work has to be DONE - and the contract's `wait_idle()` says exactly that. It runs
        // once, at scene setup, with nothing else in flight (the fence this replaced waited for the same thing).
        vk.wait_idle();

        uint32_t const readback = *reinterpret_cast<uint32_t const*>(answer_bytes.data());
        uint32_t const material_readback = reinterpret_cast<uint32_t const*>(answer_bytes.data())[1];
        deren::utility::log("descriptor heap: the heap-native probe sampled grid slot {} through sampler slot {} and read back 0x{:08x} (texture red 0x{:04x}, alpha 0x{:04x}); the material table's DEFAULT record read 0x{:04x} (its white base colour is 0xffff)",
                            texture_slot,
                            sampler_slot,
                            readback,
                            readback & 0xFFFFu,
                            readback >> 16u,
                            material_readback & 0xFFFFu);
    }

    void runtime::run_heap_graphics_probe(uint32_t const material_slot, bool const mesh_shader) {
        // ---- THE GRAPHICS HALF OF THE HEAP-NATIVE PROBE (see shaders/heap_probe.slang) ----
        //
        // The compute probe proved the mechanism for a compute pipeline; this is the same question for the kind
        // the frame is mostly made of. It renders into a target CLEARED TO BLACK first, so a white pixel can only
        // have come from the fragment stage's read of the heap rather than from the clear or from a default.
        //
        // AND IT RUNS TWICE (docs/mesh_shaders.md step 0): once with the triangle emitted by a VERTEX stage and
        // once by a MESH stage. Same fragment stage, same target, same readback, one different stage type - so
        // the two log lines compare the two pipeline kinds directly, and the mesh one is what proves a mesh
        // pipeline can be created heap-natively (the heap flag, a NULL layout) and dispatched with
        // vkCmdDrawMeshTasksEXT at all.
        rhi::api_core& vk = this->vulkan_core;
        std::span<uint8_t const> const vertex_code = this->registered_shader(mesh_shader ? "heap_probe.mesh.spv" : "heap_probe.vert.spv");
        std::span<uint8_t const> const fragment_code = this->registered_shader("heap_probe.frag.spv");
        if (!contract_heap_ready(vk) || vertex_code.empty() || fragment_code.empty()) {
            return;
        }
        constexpr rhi::image_format probe_format = rhi::image_format::rgba8_unorm;
        auto const built = pipelines::build_heap_probe_graphics(vk, contract_image_format(probe_format), vertex_code, fragment_code, mesh_shader ? rhi::shader_stage::mesh : rhi::shader_stage::vertex);
        if (!built.has_value()) {
            deren::utility::log("descriptor heap: the heap-native graphics probe's pipeline was refused: {}", built.error());
            return;
        }

        // ---- THE READ-BACK PATH, NOW SINGLE (docs/host_image_copy.md) ----
        // With VK_EXT_host_image_copy the copy out of the image is performed by the IMPLEMENTATION into a pointer
        // of the app's own memory: the target needs the HOST_TRANSFER usage instead of TRANSFER_SRC, no staging
        // buffer is created and no copy command is recorded. The capability is REQUIRED of the device now, so
        // there is no second path to choose between and no predicate for later branches to disagree about.
        //
        // THE SIZE IS NO LONGER THIS SIDE'S BUSINESS (abi 25): `image::get_content()` answers the bytes AND the
        // shape they cover (`rhi::image_content`), so the probe no longer computes
        // `extent * extent * 4` from a format assumption - the backend knows what it created.
        rhi::image_desc target_desc{};
        target_desc.extent = rhi::image_extent{.width = pipelines::heap_probe_extent, .height = pipelines::heap_probe_extent, .depth = 1u};
        target_desc.mip_levels = 1;
        target_desc.array_layers = 1;
        target_desc.format = contract_image_format(probe_format);
        target_desc.flags = rhi::to_bits(rhi::image_flag::color_attachment) | rhi::to_bits(rhi::image_flag::host_transfer);
        target_desc.debug_name = "heap probe target";
        rhi::object_manager<rhi::image> target{this->rhi_face().create_image(target_desc)};
        if (!static_cast<bool>(target)) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not allocate its target");
            return;
        }
        rhi::image_view_desc target_view_range{};
        target_view_range.layer_count = 0;
        target_view_range.mip_count = 0;
        rhi::object_manager<rhi::image_view> target_view{target->make_view(target_view_range)};
        if (!static_cast<bool>(target_view)) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not prepare its target view");
            return;
        }

        // ---- THE RECORDING, THROUGH THE CONTRACT (abi 22) ----
        // The same shape the compute probe took: a contract command buffer of its own, the heap bind + the push
        // through the `descriptor_heap` verbs, the rendering scope / dynamic state / pipeline bind / draw through
        // the record series (the MESH arm is `draw_mesh_tasks`, NOT a resolved entry point - the contract has that
        // verb, so no `vkGetDeviceProcAddr` is involved), and a contract submit + idle.
        std::shared_ptr<rhi::command_buffer> const commands = this->make_probe_commands(mesh_shader ? "the heap-native MESH probe" : "the heap-native GRAPHICS probe");
        if (commands == nullptr) {
            return;
        }
        rhi::descriptor_heap* const heap = rhi::query_extension<rhi::descriptor_heap>(vk);
        if (heap == nullptr) {
            deren::utility::log("descriptor heap: the heap-native graphics probe has no descriptor_heap ability to bind");
            return;
        }
        rhi::heap_bind_info const heap_bind{.commands = commands.get()};
        if (heap->bind(heap_bind) != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not bind the heaps on its own command buffer");
            return;
        }
        // THE TARGET'S FIRST TRANSITION IS A CONTRACT ROLE PAIR: `undefined -> color_attachment` is what the raw
        // barrier spelled (TOP_OF_PIPE -> COLOR_ATTACHMENT_OUTPUT, no source access, UNDEFINED -> GENERAL).
        // THE TARGET'S FIRST TRANSITION IS A CONTRACT BARRIER (the general `barrier(image_barrier)` form, not
        // `use()`): `use()` declares the FRAME image's role pair and REFUSES a buffer that records into a buffer
        // the caller created (its own doc), while `barrier` resolves any image the backend handed out and records
        // into whatever list is recording. `undefined -> color_attachment` is what the raw barrier spelled
        // (TOP_OF_PIPE -> COLOR_ATTACHMENT_OUTPUT, no source access, UNDEFINED -> GENERAL).
        rhi::image_barrier const to_colour{.resource = target.get(),
                                           .from = rhi::image_use::undefined,
                                           .to = rhi::image_use::color_attachment,
                                           .range = rhi::subresource_range{.base_mip = 0u, .mip_count = 1u, .base_layer = 0u, .layer_count = 1u}};
        if (commands->barrier(to_colour) != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native graphics probe's target transition was refused");
            return;
        }
        std::array<rhi::color_attachment, 1> const colors = {rhi::color_attachment{.view = target_view.get(), .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {0.0f, 0.0f, 0.0f, 1.0f}}};
        rhi::rendering_info const rendering = {
            .struct_size = sizeof(rhi::rendering_info),
            .area = rhi::rect{.offset_x = 0, .offset_y = 0, .width = pipelines::heap_probe_extent, .height = pipelines::heap_probe_extent},
            .layer_count = 1,
            .colors = colors,
            .depth = {},
            .has_depth = false,
            .secondary_contents = false, // recorded into its own primary, not a secondary
        };
        if (commands->begin_rendering(rendering) != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native graphics probe's rendering scope was refused");
            return;
        }
        // THE DYNAMIC STATE THE CONTRACT'S GRAPHICS PIPELINES DECLARE (abi 21): `create_pipeline`'s graphics recipe
        // declares viewport, scissor and cull mode dynamic for every recipe, so the probe states them - and it
        // states them through the record series now instead of through raw commands.
        constexpr float probe_edge = static_cast<float>(pipelines::heap_probe_extent);
        commands->set_viewport(rhi::viewport{.x = 0.0f, .y = 0.0f, .width = probe_edge, .height = probe_edge, .min_depth = 0.0f, .max_depth = 1.0f});
        commands->set_scissor(rhi::rect{.offset_x = 0, .offset_y = 0, .width = pipelines::heap_probe_extent, .height = pipelines::heap_probe_extent});
        commands->set_cull_mode(rhi::cull_mode::none); // the probe's subject is the fragment stage reading the heap
        // The slot, THROUGH PUSH DATA: the pipeline has no layout (the flag requires that), so this is the only
        // way a parameter reaches the fragment stage - and running the probe with a wrong value here is the
        // negative proof (see the caller).
        std::array<uint32_t, 4> const push = {material_slot, 0u, 0u, 0u};
        rhi::heap_push_info const push_info{.commands = commands.get(), .offset = 0u, .data = std::as_bytes(std::span(push))};
        if (heap->push_data(push_info) != rhi::error::ok || commands->bind_pipeline(*built->contract) != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native graphics probe's push or pipeline bind was refused");
            return;
        }
        if (mesh_shader) {
            // ONE workgroup, ONE triangle: the mesh entry emits three vertices and one index triple, and the
            // launch size is the dispatch's business (the shader's own [numthreads(1, 1, 1)] sizes the group).
            // This call is the whole host-side difference between the two probes - there is no vertex buffer, no
            // vertex input and no layout either way, and the verb is the same one the passes use.
            commands->draw_mesh_tasks(1u, 1u, 1u);
        } else {
            commands->draw(3u, 1u, 0u, 0u); // the fullscreen triangle the vertex entry builds from SV_VertexID
        }
        commands->end_rendering();

        if (commands->end_recording() != rhi::error::ok) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not close its recording");
            return;
        }
        if (!this->submit_probe_commands(*commands)) {
            deren::utility::log("descriptor heap: the heap-native graphics probe's submission was refused");
            return;
        }
        // the probe reads HOST memory below (and the host copy is a host-side call): the work has to be DONE
        vk.wait_idle();

        // ---- THE READ-BACK IS `image::get_content()` NOW (abi 25): the SAME host image copy, in ONE call.
        //
        // WHY THE SHAPE CHANGED (and what it replaced): this used to query the `host_image_copy` ABILITY, size a
        // vector from the format, build the region by hand and call
        // `copy_image_to_memory(image, span, region)` - three steps a caller had to get right (the size, the
        // region's meaning, the ability's presence). `image::get_content()` answers the CONTENT
        // (`rhi::image_content`: the bytes PLUS the extent and bytes-per-pixel they cover), so the caller sizes
        // nothing and strides nothing, and the backend keeps ONE implementation of the copy
        // (`vkCopyImageToMemoryEXT`, no staging buffer and no copy command) behind it.
        //
        // IT IS LEGAL HERE for the reason it always was: the barrier above has been submitted and waited on, so
        // the render's writes are visible to the host stage and the image is in GENERAL (the layout this
        // renderer keeps every image in). The region is the whole image, tightly packed.
        rhi::image_copy_region const host_region = {
            .extent = rhi::image_extent{.width = pipelines::heap_probe_extent, .height = pipelines::heap_probe_extent, .depth = 1u},
            .mip_level = 0,
            .base_array_layer = 0,
            .array_layer_count = 1,
            .offset_x = 0,
            .offset_y = 0,
            .offset_z = 0,
        };
        std::expected<rhi::image_content, rhi::error> const content = target->get_content(host_region);
        if (!content) {
            deren::utility::log("descriptor heap: the heap-native {} probe's read-back failed (rhi::error {})", mesh_shader ? "MESH" : "GRAPHICS",
                                static_cast<std::uint32_t>(content.error()));
            return;
        }
        std::vector<std::byte> const& host_pixels = content->bytes;
        if (host_pixels.size() < 4u) {
            deren::utility::log("descriptor heap: the heap-native {} probe's read-back came back short ({} bytes)", mesh_shader ? "MESH" : "GRAPHICS",
                                host_pixels.size());
            return;
        }
        auto const* const pixel = reinterpret_cast<std::uint8_t const*>(host_pixels.data());
        deren::utility::log("descriptor heap: the heap-native {} probe rendered grid slot {} into a {}x{} target and read back rgba {},{},{},{} (the default material's white base colour is 255,255,255,255, so the WRONG slot proves the index selects the descriptor)",
                            mesh_shader ? "MESH" : "GRAPHICS",
                            material_slot,
                            pipelines::heap_probe_extent,
                            pipelines::heap_probe_extent,
                            pixel[0],
                            pixel[1],
                            pixel[2],
                            pixel[3]);
        deren::utility::log("descriptor heap: the heap-native {} probe read that pixel back through the HOST IMAGE COPY (no staging buffer, no copy command)",
                            mesh_shader ? "MESH" : "GRAPHICS");
    }

} // namespace deren::vulkan
