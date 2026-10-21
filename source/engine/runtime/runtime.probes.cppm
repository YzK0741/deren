// ============================================================================
// module: deren.engine.runtime:probes  - the heap probes, which are diagnostics rather than production
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
#include <array>     // the HZB cull probe's fixed table lanes
#include <bit>       // std::bit_cast for the caster world-matrix hash
#include <chrono>
#include <cstddef>  // std::ptrdiff_t in the cull verify's fill
#include <cstdint>  // the HZB cull probe's table word types
#include <cstdio>   // std::snprintf: the HZB probe's %.9g float metadata (round-trips float32 exactly)
#include <cstdlib>  // std::getenv: the probe capture's paths
#include <cstring>  // std::memcmp: the cull verify's survivor-order comparison
#include <expected> // std::expected: `image::get_content()` answers the content or the named reason
#include <fstream>  // the HZB probe's dumps
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <numeric>     // std::iota: the compaction reference's candidate ids
#include <optional>    // hzb_compact's answer: the counts, or the refusal
#include <span>        // std::as_bytes for the init_utils calls (the bytes behind a UBO or a zeroed table)
#include <string>      // the probe capture's derived file paths
#include <string_view> // the metadata's file-entry names
#include <thread>      // std::this_thread::yield in the frame limiter
#include <vector>      // the destination of a host image copy, when that read-back path is taken

module deren.engine.runtime:probes;
import deren.utility.shared;

import :declarations;
import deren.engine.profiling;
import deren.engine.pipelines;
import deren.engine.render_resource;
import deren.engine.render_resource.shared;
import deren.engine.pass.hzb_build; // the HZB header's host-side lane layout/magic (the probe dumps it)
import deren.engine.pass.hzb_cull;  // the phase-B table layout + the item record (the probe dumps/verifies them)
import deren.engine.visibility.hzb; // the pyramid layout + the CPU reference the cull verify runs (§6.7)
import deren.engine.meshlet;        // the candidate record the CPU reference consumes
import deren.engine.visibility;     // the draw-item record (the reference's model matrix) and its capacities

import deren.utility;
import deren.engine.frame_constants; // one frame's shared constants (see update_frame_constants)

namespace deren::engine {
    rhi::shared_object<rhi::command_buffer> runtime::make_probe_commands(std::string_view const what) {
        // ONE PROBE'S OWN RECORDING SESSION, THROUGH THE CONTRACT (abi 22): a primary command buffer the backend
        // owns, begun with the one-time-submit usage the raw pool's TRANSIENT flag used to spell. The pool, the
        // queue family and the submission are the backend's now - the probe keeps what it always had, which is a
        // buffer of its own that nothing else records into (the mask bake looked isolated too and turned out to
        // record into the frame's buffer; this one really is).
        rhi::shared_object<rhi::command_buffer> commands = this->rhi_face().make_command_buffer({.kind = rhi::command_buffer_kind::primary});
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
        // ---- THE HEAP-NATIVE PROBE (see source/shaders/debug/heap_probe_comp.slang and docs/descriptor_heap_migration.md) ----
        //
        // The whole migration assumes four things about the native path, and this is where they stop being
        // assumptions: a pipeline created with VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT and NO layout, a
        // shader that declares its resources with `descriptor_heap` and indexes the grid by slot, a combined image
        // sampler CONSTRUCTED at the use site from a sampler in the sampler heap, and its parameters delivered by
        // vkCmdPushDataEXT. It runs in its own command buffer at scene setup, which is what makes it isolated -
        // the mask bake looked isolated too and turned out to record into the frame's own buffer.
        rhi::api_core& vk = this->vulkan_core;
        // Slot 0 of the sampler heap is the texture sampler: the first of the six core::create_samplers makes, in
        // the order source/shaders/heap_slots.glsl names (the contract test compares that order, and the host has no
        // per-sampler constant because it keeps them as a list).
        uint32_t const sampler_slot = static_cast<uint32_t>(deren::engine::render_layout::heap_sampler_base);
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
        // THE CONTRACT OWNS IT (unique_object releases the reference on every path out of this function,
        // including the two early returns below), and `mapped()` is the byte view `create_buffer` threw away
        // for everything that is not host-visible.
        rhi::unique_object<rhi::buffer> answer{this->vulkan_core.create_buffer(rhi::buffer_desc{
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
        rhi::shared_object<rhi::command_buffer> const commands = this->make_probe_commands("the heap-native probe");
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
        deren::utility::vessel::array<uint32_t, 4> const push = {
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
        // ---- THE GRAPHICS HALF OF THE HEAP-NATIVE PROBE (see source/shaders/debug/heap_probe.slang) ----
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
        auto const built = pipelines::build_heap_probe_graphics(vk, probe_format, vertex_code, fragment_code, mesh_shader ? rhi::shader_stage::mesh : rhi::shader_stage::vertex);
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
        target_desc.format = probe_format;
        target_desc.flags = rhi::to_bits(rhi::image_flag::color_attachment) | rhi::to_bits(rhi::image_flag::host_transfer);
        target_desc.debug_name = "heap probe target";
        rhi::unique_object<rhi::image> target{this->rhi_face().create_image(target_desc)};
        if (!static_cast<bool>(target)) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not allocate its target");
            return;
        }
        rhi::image_view_desc target_view_range{};
        target_view_range.layer_count = 0;
        target_view_range.mip_count = 0;
        rhi::unique_object<rhi::image_view> target_view{target->make_view(target_view_range)};
        if (!static_cast<bool>(target_view)) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not prepare its target view");
            return;
        }

        // ---- THE RECORDING, THROUGH THE CONTRACT (abi 22) ----
        // The same shape the compute probe took: a contract command buffer of its own, the heap bind + the push
        // through the `descriptor_heap` verbs, the rendering scope / dynamic state / pipeline bind / draw through
        // the record series (the MESH arm is `draw_mesh_tasks`, NOT a resolved entry point - the contract has that
        // verb, so no `vkGetDeviceProcAddr` is involved), and a contract submit + idle.
        rhi::shared_object<rhi::command_buffer> const commands = this->make_probe_commands(mesh_shader ? "the heap-native MESH probe" : "the heap-native GRAPHICS probe");
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
        deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {rhi::color_attachment{.view = target_view.get(), .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {0.0f, 0.0f, 0.0f, 1.0f}}};
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
        deren::utility::vessel::array<uint32_t, 4> const push = {material_slot, 0u, 0u, 0u};
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
        std::span<std::byte const> const host_pixels = content->bytes;
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

    // =============================================================================================
    // THE HZB PROBE CAPTURE'S WRITE HALF (M3 phase A2, spec §10.2)
    //
    // WHY THIS IS A DIAGNOSTIC AND NOT A FRAME PATH: it runs at most once per session, only on the screenshot
    // frame, only when DEREN_HZB_PROBE_CAPTURE is set, and ONLY after the caller has made the device idle - the
    // frame's copy into the two readback buffers is recorded in runtime.post_graph.cpp, and the mapped bytes are
    // not valid until the frame has completed. runtime.readback.cppm calls this right after the device idle it
    // already performs for the depth dump, which is the same point its own dumps are written from.
    //
    // THE THREE FILES, and what pins each one (scripts/measure/hzb_verify.py):
    //   <prefix>.pyramid.bin - u32 {file magic, width, height, level_count} then `words` float32 words: the
    //                          pyramid lane the build pass wrote, in level 0..Lmax order. The script rebuilds
    //                          it independently from the SAME frame's depth dump and compares word for word.
    //   <prefix>.header.bin  - the 256-byte header lane VERBATIM: word 0 magic, extent, level count, then the
    //                          view[16]/proj[16]/camera_pos[3] BITS the build pass snapshotted from the camera
    //                          UBO (see common/hzb.slang).
    //   <prefix>.json        - the metadata that makes the header checkable without a second probe: this frame's
    //                          extent/level count/frame slot and the HOST's own camera snapshot for the same
    //                          frame. "Header view/proj == this frame's camera" is the C21 invariant (§4.1);
    //                          the floats are printed with %.9g, which is exact for a float32 round trip.
    // =============================================================================================
    void runtime::write_hzb_probe_capture() {
        auto const* const prefix = std::getenv("DEREN_HZB_PROBE_CAPTURE");
        if (prefix == nullptr || !this->hzb_probe_pyramid_readback || !this->hzb_probe_header_readback) {
            return; // unset, or the frame that recorded the copy was not a build frame (the copy block logged why)
        }
        std::span<std::byte> const pyramid_bytes = this->hzb_probe_pyramid_readback->mapped();
        std::span<std::byte> const header_bytes = this->hzb_probe_header_readback->mapped();
        std::uint64_t const required_words = static_cast<std::uint64_t>(this->hzb_probe_words) * deren::engine::visibility::hzb::hzb_word_bytes;
        if (required_words == 0u || pyramid_bytes.size() < required_words || header_bytes.size() < pass::hzb_header_byte_capacity) {
            deren::utility::log("hzb probe capture: FAIL unavailable readback");
            return;
        }
        // THE PYRAMID DUMP: a four-word header the script can sanity-check before comparing arithmetic ($magic
        // distinguishes "this is a pyramid dump" from "this is the depth dump" if the paths get crossed), then the
        // lane's words exactly as the build pass left them. Another magic than the HZB header's on purpose: one
        // names a FILE's shape, the other a GPU table's.
        constexpr std::uint32_t hzb_pyramid_file_magic = 0x485A4250u; // "HZBP": HZB Pyramid
        std::string const pyramid_path = std::string(prefix) + ".pyramid.bin";
        {
            std::ofstream output(pyramid_path, std::ios::binary | std::ios::trunc);
            deren::utility::vessel::array<std::uint32_t, 4> const file_header = {hzb_pyramid_file_magic, this->hzb_probe_extent.width, this->hzb_probe_extent.height,
                                                                                 this->hzb_probe_levels};
            output.write(reinterpret_cast<char const*>(file_header.data()), sizeof(file_header));
            output.write(reinterpret_cast<char const*>(pyramid_bytes.data()), static_cast<std::streamsize>(required_words));
            if (!output) {
                deren::utility::log("hzb probe capture: FAIL pyramid file write");
                return;
            }
        }
        std::string const header_path = std::string(prefix) + ".header.bin";
        {
            std::ofstream output(header_path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<char const*>(header_bytes.data()), static_cast<std::streamsize>(pass::hzb_header_byte_capacity));
            if (!output) {
                deren::utility::log("hzb probe capture: FAIL header file write");
                return;
            }
        }
        std::string const metadata_path = std::string(prefix) + ".json";
        {
            std::ofstream output(metadata_path, std::ios::trunc);
            auto const write_float = [&output](float const value) {
                char text[32] = {};
                std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
                output << text;
            };
            auto const write_array = [&write_float, &output](auto const& values) {
                output << '[';
                for (std::size_t index = 0; index < values.size(); ++index) {
                    if (index != 0u) {
                        output << ", ";
                    }
                    write_float(values[index]);
                }
                output << ']';
            };
            output << "{\n";
            output << "  \"extent\": {\"width\": " << this->hzb_probe_extent.width << ", \"height\": " << this->hzb_probe_extent.height << "},\n";
            output << "  \"level_count\": " << this->hzb_probe_levels << ",\n";
            output << "  \"frame_slot\": " << this->hzb_probe_frame_slot << ",\n";
            output << "  \"words_per_lane\": " << this->hzb_probe_words << ",\n";
            output << "  \"levels_bytes_per_lane\": " << required_words << ",\n";
            output << "  \"header_bytes_per_lane\": " << pass::hzb_header_byte_capacity << ",\n";
            output << "  \"header_magic\": " << pass::hzb_header_magic << ",\n";
            output << "  \"pyramid_file_magic\": " << hzb_pyramid_file_magic << ",\n";
            output << "  \"camera\": {\n";
            output << "    \"view\": ";
            write_array(this->hzb_probe_view);
            output << ",\n    \"proj\": ";
            write_array(this->hzb_probe_proj);
            output << ",\n    \"position\": ";
            write_array(this->hzb_probe_camera_pos);
            output << "\n  }\n}\n";
            if (!output) {
                deren::utility::log("hzb probe capture: FAIL metadata file write");
                return;
            }
        }
        deren::utility::log("hzb probe capture: wrote {} ({} word(s) + 4-word file header), {} ({} B) and the {}x{} frame camera metadata ({})",
                            pyramid_path,
                            this->hzb_probe_words,
                            header_path,
                            pass::hzb_header_byte_capacity,
                            this->hzb_probe_extent.width,
                            this->hzb_probe_extent.height,
                            metadata_path);
    }

    // =============================================================================================
    // THE PHASE-B CULL PROBE + THE IN-ENGINE REFERENCE VERIFICATION (M3 phase B1, spec §6.7/§10.2)
    //
    // WHY THIS EXISTS AND WHY IT IS THE PHASE'S ACCEPTANCE: the cull lane makes a PREDICTION ("these candidates
    // are hidden by the history depth"), and a GPU's prediction is only trustworthy when it can be checked against
    // a reference that has no GPU in it. The reference is the committed `deren::engine::visibility::hzb` module
    // (hzb_test_sphere + hzb_compact), and this function re-runs it over exactly the data the cull read:
    //   - the HISTORY pyramid + header lanes, copied by the cull lane's recorder BEFORE this frame's build
    //     overwrote the same lane (record_hzb_cull_probe_copy - that is what makes the dump describe the data the
    //     dispatch actually consumed, not this frame's re-built pyramid);
    //   - the item table, the candidate records and the draw-item model matrices, from the same host-visible
    //     tables the GPU read.
    // Every decision, the survivor ORDER and every per-item count must agree; a single disagreement is a FAIL line
    // and, per the design, a BUG rather than noise (§6.7: the shader and the reference are one contract).
    //
    // THE FILES, and what pins each one (scripts/measure/hzb_cull_verify.py):
    //   <prefix>.history.pyramid.bin  - u32 {file magic, width, height, levels} then the HISTORY lane's words
    //   <prefix>.history.header.bin   - the 256-byte HISTORY header lane VERBATIM (the camera the cull used)
    //   <prefix>.items.bin            - the frame slot's 4096 x 16 B item descriptions
    //   <prefix>.flags.bin            - the frame slot's 65536 candidate flags (u32: 0 survivor, 1 culled)
    //   <prefix>.counts.bin           - the frame slot's 4096 pass1 survivor counts
    //   <prefix>.stats.bin            - the frame slot's 16-word stats block
    //   <prefix>.survivors.bin        - the compacted survivor records (meshlet_capacity x 48 B)
    //   <prefix>.candidates.bin       - the host-culled candidate records (meshlet_capacity x 48 B)
    //   <prefix>.draw_items.bin       - the visibility draw-item lane (4096 x 112 B) = the model matrices
    //   <prefix>.json                 - the metadata that names WHICH history lane/camera was used and the shapes
    // The script checks internal consistency only; the §6 decision comparison is the in-engine verification below.
    // =============================================================================================
    void runtime::write_hzb_cull_probe_capture() {
        auto const* const prefix = std::getenv("DEREN_HZB_CULL_PROBE_CAPTURE");
        if (prefix == nullptr || this->hzb_cull_probe_dumped) {
            return;
        }
        if (!this->hzb_cull_probe_active || !this->hzb_cull_probe_pyramid_readback || !this->hzb_cull_probe_header_readback ||
            !this->hzb_cull_probe_survivors_readback) {
            // The lane never recorded its copies: either the screenshot frame had no cull lane (gate closed, or the
            // lane is unavailable). Say so once - a silently missing dump would look like a path typo.
            deren::utility::log("hzb cull probe capture: skipped - no cull lane recorded its history/survivor copies on the screenshot frame");
            return;
        }
        if (!this->hzb_cull_recorded_this_frame || this->hzb_results_mapped == nullptr || this->hzb_items_mapped == nullptr || this->meshlet_culled_mapped == nullptr ||
            this->visibility_items_mapped == nullptr) {
            deren::utility::log("hzb cull probe capture: skipped - a source table is not mapped this run");
            return;
        }
        if (this->hzb_recovery_probe_enabled && !this->hzb_recovery_probe_copied) {
            deren::utility::log("hzb recovery probe: FAIL no current-frame copy");
            return;
        }
        std::span<std::byte> const pyramid_bytes = this->hzb_cull_probe_pyramid_readback->mapped();
        std::span<std::byte> const header_mapped = this->hzb_cull_probe_header_readback->mapped();
        std::span<std::byte> const survivors_mapped = this->hzb_cull_probe_survivors_readback->mapped();
        std::uint64_t const required_pyramid = static_cast<std::uint64_t>(this->hzb_cull_probe_words) * deren::engine::visibility::hzb::hzb_word_bytes;
        std::uint64_t const required_survivors = static_cast<std::uint64_t>(pass::hzb_cull_candidate_capacity) * sizeof(deren::engine::meshlet);
        if (required_pyramid == 0u || pyramid_bytes.size() < required_pyramid || header_mapped.size() < pass::hzb_header_byte_capacity ||
            survivors_mapped.size() < required_survivors) {
            deren::utility::log("hzb cull probe capture: FAIL unavailable readback");
            return;
        }
        std::uint32_t const slot = this->hzb_cull_probe_frame_slot;
        rhi::image_extent const extent = this->hzb_cull_probe_extent;
        pass::hzb_item const* const items = static_cast<pass::hzb_item const*>(this->hzb_items_mapped);
        std::uint32_t const* const results = static_cast<std::uint32_t const*>(this->hzb_results_mapped);
        deren::engine::meshlet const* const candidates = static_cast<deren::engine::meshlet const*>(this->meshlet_culled_mapped);
        deren::engine::visibility::draw_item const* const draw_items = static_cast<deren::engine::visibility::draw_item const*>(this->visibility_items_mapped);
        deren::engine::meshlet const* const survivors = reinterpret_cast<deren::engine::meshlet const*>(survivors_mapped.data());
        std::size_t const survivor_lane_base = 0u; // the readback contains one slot lane
        std::size_t const candidate_lane_base = static_cast<std::size_t>(slot) * deren::engine::meshlet_capacity;
        std::size_t const item_lane_base = static_cast<std::size_t>(slot) * pass::hzb_cull_item_capacity;
        std::size_t const draw_lane_base = static_cast<std::size_t>(slot) * deren::engine::visibility::id_item_capacity;
        std::size_t const results_lane_base = static_cast<std::size_t>(slot) * pass::hzb_results_words_per_lane;
        auto const header_word = [&](std::size_t const index) { return reinterpret_cast<std::uint32_t const*>(header_mapped.data())[index]; };
        auto const header_float = [&](std::size_t const index) { return std::bit_cast<float>(header_word(index)); };
        auto const write_float = [](std::ostream& output, float const value) {
            char text[32] = {};
            std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
            output << text;
        };
        auto const write_file = [](std::string const& path, void const* const bytes, std::size_t const size) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(static_cast<char const*>(bytes), static_cast<std::streamsize>(size));
            return static_cast<bool>(output);
        };
        // ---- THE DUMPS ----
        // The history pyramid keeps phase A's file shape (a 4-word header the script can sanity-check first), with a
        // DIFFERENT magic: one names a FILE's shape ("this is the cull's history pyramid"), the other the HZB
        // header's GPU-table magic.
        constexpr std::uint32_t hzb_cull_pyramid_file_magic = 0x485A4342u; // "HZCB": HZB Cull, history pyramid
        std::string const pyramid_path = std::string(prefix) + ".history.pyramid.bin";
        {
            std::array<std::uint32_t, 4> const file_header{hzb_cull_pyramid_file_magic, extent.width, extent.height, this->hzb_cull_probe_levels};
            std::ofstream output(pyramid_path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<char const*>(file_header.data()), sizeof(file_header));
            output.write(reinterpret_cast<char const*>(pyramid_bytes.data()), static_cast<std::streamsize>(required_pyramid));
            if (!output) {
                deren::utility::log("hzb cull probe capture: FAIL pyramid file write");
                return;
            }
        }
        std::string const history_header_path = std::string(prefix) + ".history.header.bin";
        if (!write_file(history_header_path, header_mapped.data(), pass::hzb_header_byte_capacity)) {
            deren::utility::log("hzb cull probe capture: FAIL history header file write");
            return;
        }
        std::string const items_path = std::string(prefix) + ".items.bin";
        if (!write_file(items_path, items + item_lane_base, static_cast<std::size_t>(pass::hzb_cull_item_capacity) * sizeof(pass::hzb_item))) {
            deren::utility::log("hzb cull probe capture: FAIL items file write");
            return;
        }
        std::string const flags_path = std::string(prefix) + ".flags.bin";
        std::size_t const flags_bytes = static_cast<std::size_t>(pass::hzb_cull_candidate_capacity) * sizeof(std::uint32_t);
        if (!write_file(flags_path, results + results_lane_base + pass::hzb_results_flags_offset, flags_bytes)) {
            deren::utility::log("hzb cull probe capture: FAIL flags file write");
            return;
        }
        std::string const counts_path = std::string(prefix) + ".counts.bin";
        std::size_t const counts_bytes = static_cast<std::size_t>(pass::hzb_cull_item_capacity) * sizeof(std::uint32_t);
        if (!write_file(counts_path, results + results_lane_base + pass::hzb_results_pass1_count_offset, counts_bytes)) {
            deren::utility::log("hzb cull probe capture: FAIL counts file write");
            return;
        }
        std::string const stats_path = std::string(prefix) + ".stats.bin";
        std::size_t const stats_bytes = static_cast<std::size_t>(pass::hzb_results_stats_capacity) * sizeof(std::uint32_t);
        if (!write_file(stats_path, results + results_lane_base + pass::hzb_results_stats_offset, stats_bytes)) {
            deren::utility::log("hzb cull probe capture: FAIL stats file write");
            return;
        }
        std::string const survivors_path = std::string(prefix) + ".survivors.bin";
        if (!write_file(survivors_path, survivors, required_survivors)) {
            deren::utility::log("hzb cull probe capture: FAIL survivors file write");
            return;
        }
        std::string const candidates_path = std::string(prefix) + ".candidates.bin";
        std::size_t const candidates_bytes = static_cast<std::size_t>(deren::engine::meshlet_capacity) * sizeof(deren::engine::meshlet);
        if (!write_file(candidates_path, candidates + candidate_lane_base, candidates_bytes)) {
            deren::utility::log("hzb cull probe capture: FAIL candidates file write");
            return;
        }
        std::string const draw_items_path = std::string(prefix) + ".draw_items.bin";
        std::size_t const draw_items_bytes = static_cast<std::size_t>(deren::engine::visibility::id_item_capacity) * sizeof(deren::engine::visibility::draw_item);
        if (!write_file(draw_items_path, draw_items + draw_lane_base, draw_items_bytes)) {
            deren::utility::log("hzb cull probe capture: FAIL draw-items file write");
            return;
        }
        // The capture waits for device idle; this coherent lane is the exact GPU-written indirect input.
        if (this->mesh_indirect_mapped == nullptr) {
            deren::utility::log("hzb cull probe capture: FAIL commands not mapped");
            return;
        }
        auto const* command_data = static_cast<rhi::mesh_task_command const*>(this->mesh_indirect_mapped);
        std::size_t const command_base = static_cast<std::size_t>(slot) * runtime::mesh_command_capacity + runtime::visibility_command_run;
        std::size_t const command_bytes = static_cast<std::size_t>(this->hzb_cull_probe_item_count) * rhi::mesh_task_command_size;
        std::string const commands_path = std::string(prefix) + ".commands.bin";
        if (!write_file(commands_path, command_data + command_base, command_bytes)) {
            deren::utility::log("hzb cull probe capture: FAIL commands file write");
            return;
        }

        bool const recovery_probe = this->hzb_recovery_probe_enabled;
        std::span<std::byte> current_pyramid{};
        std::span<std::byte> current_header{};
        std::string const current_pyramid_path = std::string(prefix) + ".current.pyramid.bin";
        std::string const current_header_path = std::string(prefix) + ".current.header.bin";
        std::string const counts2_path = std::string(prefix) + ".counts2.bin";
        std::string const commands2_path = std::string(prefix) + ".commands2.bin";
        if (recovery_probe) {
            current_pyramid = this->hzb_recovery_probe_pyramid_readback->mapped();
            current_header = this->hzb_recovery_probe_header_readback->mapped();
            std::size_t const recovery_command_base = static_cast<std::size_t>(slot) * runtime::mesh_command_capacity + runtime::hzb_recovery_command_run;
            if (current_pyramid.size() < required_pyramid || current_header.size() < pass::hzb_header_byte_capacity ||
                !write_file(current_pyramid_path, current_pyramid.data(), required_pyramid) || !write_file(current_header_path, current_header.data(), pass::hzb_header_byte_capacity) ||
                !write_file(counts2_path, results + results_lane_base + pass::hzb_results_pass2_count_offset, counts_bytes) ||
                !write_file(commands2_path, command_data + recovery_command_base, command_bytes)) {
                deren::utility::log("hzb recovery probe: FAIL current files");
                return;
            }
        }
        // ---- THE IN-ENGINE VERIFICATION: THE SAME DATA, THE COMMITTED CPU REFERENCE ----
        namespace hzb = deren::engine::visibility::hzb;
        bool const header_usable = header_word(0u) == pass::hzb_header_magic && header_word(1u) == extent.width && header_word(2u) == extent.height &&
                                   header_word(3u) == this->hzb_cull_probe_levels;
        bool const header_matches_snapshot = [&] {
            auto const bits_equal = [&](std::size_t const word_base, hzb::hzb_matrix_bits const& saved) {
                for (std::size_t index = 0u; index < saved.size(); ++index) {
                    if (header_word(word_base + index) != saved[index]) {
                        return false;
                    }
                }
                return true;
            };
            return slot < this->hzb_source_valid.size() && this->hzb_source_valid[slot] &&
                   bits_equal(pass::hzb_header_word_view, this->hzb_cull_probe_expected_view) &&
                   bits_equal(pass::hzb_header_word_proj, this->hzb_cull_probe_expected_proj);
        }();
        std::uint64_t decisions = 0u, agree = 0u, order_bad = 0u, count_bad = 0u;
        std::uint64_t recovery_decisions = 0;
        std::uint64_t recovery_agree = 0;
        std::uint64_t recovery_order_bad = 0;
        std::uint64_t recovery_count_bad = 0;
        hzb::hzb_camera_snapshot current_camera{};
        bool current_usable = false;
        if (recovery_probe) {
            auto const* words = reinterpret_cast<std::uint32_t const*>(current_header.data());
            current_usable = words[0] == pass::hzb_header_magic && words[1] == extent.width && words[2] == extent.height && words[3] == this->hzb_cull_probe_levels;
            for (std::size_t i = 0; i < 16; ++i) {
                current_camera.view[i] = std::bit_cast<float>(words[pass::hzb_header_word_view + i]);
                current_camera.proj[i] = std::bit_cast<float>(words[pass::hzb_header_word_proj + i]);
            }
            for (std::size_t i = 0; i < 3; ++i) {
                current_camera.position[i] = std::bit_cast<float>(words[pass::hzb_header_word_camera_pos + i]);
            }
        }

        if (header_usable) {
            hzb::hzb_camera_snapshot camera{};
            for (std::size_t index = 0u; index < 16u; ++index) {
                camera.view[index] = static_cast<double>(header_float(pass::hzb_header_word_view + index));
                camera.proj[index] = static_cast<double>(header_float(pass::hzb_header_word_proj + index));
            }
            camera.position = {static_cast<double>(header_float(pass::hzb_header_word_camera_pos)), static_cast<double>(header_float(pass::hzb_header_word_camera_pos + 1u)), static_cast<double>(header_float(pass::hzb_header_word_camera_pos + 2u))};
            hzb::max_pyramid_view const pyramid{std::span<float const>{reinterpret_cast<float const*>(pyramid_bytes.data()), static_cast<std::size_t>(this->hzb_cull_probe_words)},
                                                extent.width, extent.height, this->hzb_cull_probe_levels};
            std::uint32_t const item_count = std::min(this->hzb_cull_probe_item_count, pass::hzb_cull_item_capacity);
            std::vector<std::uint32_t> candidate_ids(pass::hzb_cull_item_capacity);
            std::iota(candidate_ids.begin(), candidate_ids.end(), 0u);
            std::array<bool, pass::hzb_cull_item_capacity> keep = {};
            std::vector<std::uint32_t> compacted(pass::hzb_cull_item_capacity);
            for (std::uint32_t item = 0u; item < item_count; ++item) {
                pass::hzb_item const& description = items[item_lane_base + item];
                if (description.source != pass::hzb_item_source_culled || description.candidate_count == 0u) {
                    continue;
                }
                std::uint32_t const count = description.candidate_count;
                if (count > pass::hzb_cull_item_capacity || description.meshlet_base > pass::hzb_cull_candidate_capacity ||
                    count > pass::hzb_cull_candidate_capacity - description.meshlet_base) {
                    count_bad += 1u; // the GPU froze this item's count to 0 (the §9.4 overflow answer)
                    continue;
                }
                hzb::hzb_matrix model{};
                for (std::size_t index = 0u; index < 16u; ++index) {
                    model[index] = static_cast<double>(draw_items[draw_lane_base + item].model[index]);
                }
                std::fill(keep.begin(), keep.begin() + static_cast<std::ptrdiff_t>(count), false);
                for (std::uint32_t index = 0u; index < count; ++index) {
                    deren::engine::meshlet const& record = candidates[candidate_lane_base + description.meshlet_base + index];
                    hzb::hzb_sphere const sphere{{static_cast<double>(record.center_x), static_cast<double>(record.center_y), static_cast<double>(record.center_z)},
                                                 static_cast<double>(record.radius)};
                    hzb::hzb_decision const decision = hzb::hzb_test_sphere(camera, model, sphere, pyramid);
                    bool const reference_culled = decision == hzb::hzb_decision::occluded;
                    keep[index] = !reference_culled;
                    std::uint32_t const gpu_flag = results[results_lane_base + pass::hzb_results_flags_offset + description.meshlet_base + index];
                    bool const gpu_culled = gpu_flag != 0u;
                    ++decisions;
                    if (gpu_culled == reference_culled) {
                        ++agree;
                    } else if (decisions - agree <= 8u) {
                        // THE FAIL LINES ARE CAPPED: a systematic bug would otherwise flood the log with one line per
                        // candidate, and the FIRST few name the item/candidate the diff starts at.
                        deren::utility::log("hzb cull verify: FAIL - item {} candidate {} (meshlet {}): the GPU {} it, the CPU reference {} it",
                                            item,
                                            description.meshlet_base + index,
                                            description.meshlet_base + index,
                                            gpu_culled ? "culled" : "kept",
                                            reference_culled ? "culls" : "keeps");
                    }
                }
                std::optional<hzb::hzb_compaction> const reference =
                    hzb::hzb_compact(std::span<std::uint32_t const>{candidate_ids.data(), count}, std::span<bool const>{keep.data(), count},
                                     std::span<std::uint32_t>{compacted.data(), count});
                if (!reference.has_value()) {
                    count_bad += 1u;
                    continue;
                }
                if (results[results_lane_base + pass::hzb_results_pass1_count_offset + item] != reference->survivors) {
                    count_bad += 1u;
                }
                for (std::uint32_t order = 0u; order < reference->survivors; ++order) {
                    deren::engine::meshlet const& gpu_record = survivors[survivor_lane_base + description.meshlet_base + order];
                    deren::engine::meshlet const& reference_record = candidates[candidate_lane_base + description.meshlet_base + compacted[order]];
                    if (std::memcmp(&gpu_record, &reference_record, sizeof(deren::engine::meshlet)) != 0) {
                        ++order_bad;
                    }
                }
                if (recovery_probe) {
                    std::array<std::uint32_t, pass::hzb_cull_item_capacity> history_flags{};
                    std::array<bool, pass::hzb_cull_item_capacity> keep_current{};
                    hzb::max_pyramid_view const current_view{std::span<float const>{reinterpret_cast<float const*>(current_pyramid.data()), this->hzb_cull_probe_words}, extent.width, extent.height, this->hzb_cull_probe_levels};
                    for (std::uint32_t index = 0; index < count; ++index) {
                        std::uint32_t const gpu_flag = results[results_lane_base + pass::hzb_results_flags_offset + description.meshlet_base + index];
                        history_flags[index] = gpu_flag & 1u;
                        if (history_flags[index] == 0u) {
                            continue;
                        }
                        auto const& record = candidates[candidate_lane_base + description.meshlet_base + index];
                        hzb::hzb_sphere const sphere{{record.center_x, record.center_y, record.center_z}, record.radius};
                        bool const keep_now = !current_usable || hzb::hzb_test_sphere(current_camera, model, sphere, current_view) != hzb::hzb_decision::occluded;
                        keep_current[index] = keep_now;
                        ++recovery_decisions;
                        if (keep_now == ((gpu_flag & 2u) != 0u)) {
                            ++recovery_agree;
                        }
                    }
                    auto recovered = hzb::hzb_compact_recovery(std::span<std::uint32_t const>{candidate_ids.data(), count}, std::span<std::uint32_t const>{history_flags.data(), count},
                                                               std::span<bool const>{keep_current.data(), count}, reference->survivors, std::span<std::uint32_t>{compacted.data(), count});
                    if (!recovered || results[results_lane_base + pass::hzb_results_pass2_count_offset + item] != recovered->recovered) {
                        ++recovery_count_bad;
                    }
                    if (recovered) {
                        for (std::uint32_t order = reference->survivors; order < reference->survivors + recovered->recovered; ++order) {
                            auto const& gpu_record = survivors[description.meshlet_base + order];
                            auto const& expected_record = candidates[candidate_lane_base + description.meshlet_base + compacted[order]];
                            if (std::memcmp(&gpu_record, &expected_record, sizeof(deren::engine::meshlet)) != 0) {
                                ++recovery_order_bad;
                            }
                        }
                    }
                }
            }
        }
        // ---- THE ONE LINE ----
        double const agreement = decisions != 0u ? 100.0 * static_cast<double>(agree) / static_cast<double>(decisions) : 100.0;
        deren::utility::log("hzb cull verify: {} candidate(s), {} decision(s) agree ({}%), survivor order {}; pass1 counts {}",
                            decisions,
                            agree,
                            agreement,
                            order_bad == 0u ? "matched" : "MISMATCHED",
                            count_bad == 0u ? "matched" : "MISMATCHED");
        if (!header_usable) {
            deren::utility::log("hzb cull verify: FAIL - the dumped history header is not usable (magic/extent/level count), so the reference could not run");
        } else if (decisions == 0u) {
            // A LEGITIMATE EMPTY STATE rather than a failure: phase B1 only tests host-CUT runs, so a frame with
            // `subtilis = false` (every item's candidates are the shared table) has nothing for this lane to test.
            // The dump is still complete (the tables are all zero) and the script's internal checks still run.
            deren::utility::log("hzb cull verify: no candidates to compare - the frame had no host-cut (subtilis) runs, which phase B1's cull "
                                "deliberately does not walk; the lane recorded and the tables are consistent");
        } else if (agree != decisions || order_bad != 0u || count_bad != 0u) {
            deren::utility::log("hzb cull verify: FAIL - {} decision(s), {} survivor-order record(s) and {} per-item count(s) disagreed with the committed "
                                "CPU reference (this is a bug, not noise: the shader and hzb.cppm are one contract)",
                                decisions - agree,
                                order_bad,
                                count_bad);
        } else {
            deren::utility::log("hzb cull verify: {}", header_matches_snapshot
                                                           ? "the history header's view/proj are byte-equal to the snapshot the static gate compared; "
                                                             "the dumped history lane is the one the cull READ (this frame slot's previous use)"
                                                           : "the history header's view/proj are NOT byte-equal to the snapshot the static gate compared - the "
                                                             "dump or the gate is wrong");
        }
        if (recovery_probe) {
            deren::utility::log("hzb recovery verify: {} rejected candidate(s), {} decisions agree; suffix order {}; pass2 counts {}", recovery_decisions, recovery_agree, recovery_order_bad == 0 ? "matched" : "MISMATCHED", recovery_count_bad == 0 ? "matched" : "MISMATCHED");
            if (recovery_decisions != recovery_agree || recovery_order_bad != 0 || recovery_count_bad != 0) {
                deren::utility::log("hzb recovery verify: FAIL decision/order/count mismatch");
            }
        }
        // ---- THE METADATA ----
        std::string const metadata_path = std::string(prefix) + ".json";
        {
            std::ofstream output(metadata_path, std::ios::trunc);
            output << "{\n";
            output << "  \"kind\": \"hzb-cull-probe\",\n";
            output << R"(  "version": )" << (recovery_probe ? 3 : 2) << ",\n";
            output << "  \"note\": \"the cull reads the HISTORY HZB: the previous use of this frame slot (2 frames old). The copies were recorded "
                      "INSIDE the cull lane, before this frame's build overwrote the lane, so these files describe the data the dispatch consumed.\",\n";
            output << "  \"extent\": {\"width\": " << extent.width << ", \"height\": " << extent.height << "},\n";
            output << "  \"frame_slot\": " << slot << ",\n";
            output << "  \"level_count\": " << this->hzb_cull_probe_levels << ",\n";
            output << "  \"words_per_lane\": " << this->hzb_cull_probe_words << ",\n";
            output << "  \"item_count\": " << this->hzb_cull_probe_item_count << ",\n";
            output << "  \"candidate_total\": " << this->hzb_cull_candidate_total << ",\n";
            output << R"(  "gate": ")" << (recovery_probe ? "open (history prediction with current-frame recovery)" : "open (byte-identical static frame)") << "\",\n";
            output << "  \"history\": {\n";
            output << "    \"frame_slot\": " << slot << ",\n";
            output << "    \"header_magic\": " << header_word(0u) << ",\n";
            output << "    \"header_width\": " << header_word(1u) << ",\n";
            output << "    \"header_height\": " << header_word(2u) << ",\n";
            output << "    \"header_levels\": " << header_word(3u) << ",\n";
            output << "    \"header_matches_saved_snapshot\": " << (header_matches_snapshot ? "true" : "false") << ",\n";
            output << "    \"camera\": {\"view\": [";
            for (std::size_t index = 0u; index < 16u; ++index) {
                if (index != 0u) {
                    output << ", ";
                }
                write_float(output, header_float(pass::hzb_header_word_view + index));
            }
            output << "], \"proj\": [";
            for (std::size_t index = 0u; index < 16u; ++index) {
                if (index != 0u) {
                    output << ", ";
                }
                write_float(output, header_float(pass::hzb_header_word_proj + index));
            }
            output << "], \"position\": [";
            for (std::size_t index = 0u; index < 3u; ++index) {
                if (index != 0u) {
                    output << ", ";
                }
                write_float(output, header_float(pass::hzb_header_word_camera_pos + index));
            }
            output << "]}\n";
            output << "  },\n";
            output << "  \"verify\": {\"decisions\": " << decisions << ", \"agree\": " << agree << ", \"survivor_order_mismatches\": " << order_bad
                   << ", \"count_mismatches\": " << count_bad << "},\n";
            output << R"(  "recovery": {"enabled": )" << (recovery_probe ? "true" : "false") << R"(, "decisions": )" << recovery_decisions << R"(, "agree": )" << recovery_agree << R"(, "count_mismatches": )" << recovery_count_bad << R"(, "survivor_order_mismatches": )" << recovery_order_bad << "},\n";
            output << "  \"files\": {\n";
            // THE PATHS ARE JSON-ESCAPED: a Windows path is full of backslashes, and a raw one makes the whole
            // metadata file unparseable (`\U` is not a JSON escape) - which the verification script reports as a
            // dump problem rather than the probe bug it is (measured on the first Bistro run).
            auto const escaped = [](std::string const& text) {
                std::string out;
                out.reserve(text.size() + 8u);
                for (char const character : text) {
                    if (character == '\\' || character == '"') {
                        out.push_back('\\');
                    }
                    out.push_back(character);
                }
                return out;
            };
            auto const file_entry = [&output, &escaped](std::string_view const name, std::string const& path, std::size_t const bytes, bool const last) {
                output << "    \"" << name << "\": {\"path\": \"" << escaped(path) << "\", \"bytes\": " << bytes << "}" << (last ? "\n" : ",\n");
            };
            file_entry("history_pyramid", pyramid_path, static_cast<std::size_t>(required_pyramid) + sizeof(std::array<std::uint32_t, 4>), false);
            file_entry("history_header", history_header_path, pass::hzb_header_byte_capacity, false);
            file_entry("items", items_path, static_cast<std::size_t>(pass::hzb_cull_item_capacity) * sizeof(pass::hzb_item), false);
            file_entry("flags", flags_path, flags_bytes, false);
            file_entry("counts", counts_path, counts_bytes, false);
            file_entry("stats", stats_path, stats_bytes, false);
            file_entry("survivors", survivors_path, static_cast<std::size_t>(required_survivors), false);
            file_entry("candidates", candidates_path, candidates_bytes, false);
            file_entry("draw_items", draw_items_path, draw_items_bytes, false);
            file_entry("commands", commands_path, command_bytes, !recovery_probe);
            if (recovery_probe) {
                file_entry("current_pyramid", current_pyramid_path, required_pyramid, false);
                file_entry("current_header", current_header_path, pass::hzb_header_byte_capacity, false);
                file_entry("counts2", counts2_path, counts_bytes, false);
                file_entry("commands2", commands2_path, command_bytes, true);
            }
            output << "  }\n";
            output << "}\n";
            if (!output) {
                deren::utility::log("hzb cull probe capture: FAIL metadata file write");
                return;
            }
        }
        this->hzb_cull_probe_dumped = true;
        deren::utility::log("hzb cull probe capture: wrote the history pyramid/header (lane {}), the item/flags/counts/stats lanes, the survivor lane and "
                            "the candidate records under '{}.*' ({} item(s), {} candidate(s) in the item plans)",
                            slot,
                            prefix,
                            this->hzb_cull_probe_item_count,
                            this->hzb_cull_candidate_total);
    }

} // namespace deren::engine
