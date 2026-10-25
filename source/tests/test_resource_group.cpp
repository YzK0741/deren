#include "vk_test.h"

#include <array>
#include <cstring>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.backend_loader;
import deren.vulkan.main_context;
import deren.vulkan.command_buffer;
namespace rhi = deren::promise::rhi;
#include "buffer_creation_contract.hpp"
#include "recording_scope_contract.hpp"

struct queue_gate {
    VkDevice device;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    bool armed = false;
    queue_gate(VkDevice dev, VkQueue queue)
        : device(dev) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        create.pNext = &type;
        CHECK(vkCreateSemaphore(device, &create, nullptr, &semaphore) == VK_SUCCESS);
        if (!semaphore)
            return;
        std::uint64_t const value = 1;
        VkTimelineSemaphoreSubmitInfo timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &value;
        VkPipelineStageFlags const stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &semaphore;
        submit.pWaitDstStageMask = &stage;
        armed = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        CHECK(armed);
    }
    void release() {
        if (!armed)
            return;
        VkSemaphoreSignalInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signal.semaphore = semaphore;
        signal.value = 1;
        CHECK(vkSignalSemaphore(device, &signal) == VK_SUCCESS);
        armed = false;
    }
    ~queue_gate() {
        release();
        if (semaphore) {
            // The test caller drains submitted work before this scope ends.
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }
};

int main(int argc, char** argv) {
    using kind = rhi::resource_kind;
    std::array layout{
        rhi::resource_group_binding{10, kind::storage_buffer, 0, 4},
        rhi::resource_group_binding{20, kind::storage_buffer, 4, 4},
        rhi::resource_group_binding{30, kind::sampled_image, 8, 4},
        rhi::resource_group_binding{40, kind::storage_image, 12, 4},
        rhi::resource_group_binding{50, kind::sampler, 16, 4},
        rhi::resource_group_binding{60, kind::texture_group, 20, 4},
        rhi::resource_group_binding{70, kind::uniform_buffer, 24, 4}};
    CHECK(rhi::validate_resource_group_layout(layout) == rhi::error::ok);
    auto invalid = layout;
    invalid[1].binding = 10;
    CHECK(rhi::validate_resource_group_layout(invalid) == rhi::error::invalid_argument);
    invalid = layout;
    invalid[1].push_byte_offset = 0;
    CHECK(rhi::validate_resource_group_layout(invalid) == rhi::error::invalid_argument);
    invalid = layout;
    invalid[0].push_byte_offset = UINT32_MAX;
    CHECK(rhi::validate_resource_group_layout(invalid) == rhi::error::invalid_argument);
    invalid = layout;
    invalid[0].shader_stage_bits = 0;
    CHECK(rhi::validate_resource_group_layout(invalid) == rhi::error::invalid_argument);
    invalid = layout;
    invalid[0].kind = static_cast<kind>(99);
    CHECK(rhi::validate_resource_group_layout(invalid) == rhi::error::invalid_argument);
    if (argc < 2 || std::string_view(argv[1]) != "--with-device")
        return deren::vk_test::finish("test_resource_group (layout)");
    rhi::create_info creation{};
    creation.window_width = 64;
    creation.window_height = 64;
    creation.validation_layers = true;
    auto core = deren::engine::load_api_core(creation);
    CHECK(core);
    if (!core)
        return deren::vk_test::finish("test_resource_group");
    deren::vk_test::check_buffer_creation_contract(*core);
    deren::vk_test::check_recording_scope_contract(*core);
    std::array<std::byte, 16> unsupported_upload{};
    for (auto usage : {rhi::buffer_usage::storage_gpu_only, rhi::buffer_usage::acceleration_structure_storage, rhi::buffer_usage::acceleration_structure_scratch})
        CHECK(!core->resources->create_buffer({.size = unsupported_upload.size(), .usage = usage, .initial_bytes = unsupported_upload}));

    auto* completion = rhi::query_extension<rhi::command_submission>(*core);
    auto* push_capabilities = rhi::query_extension<rhi::device_capabilities>(*core);
    CHECK(completion && push_capabilities);
    if (!completion || !push_capabilities)
        return deren::vk_test::finish("test_resource_group");
    auto const push_capacity = push_capabilities->max_push_constants_size();
    CHECK(push_capacity >= 4 && (push_capacity & 3u) == 0);
    if (push_capacity < 4)
        return deren::vk_test::finish("test_resource_group");
    auto check_push_range = [&](rhi::command_recorder& recorder) {
        std::array<std::byte, 4> word{};
        CHECK(recorder.push_constants(0u, {}) == rhi::error::invalid_argument);
        CHECK(recorder.push_constants(1u, word) == rhi::error::invalid_argument);
        CHECK(recorder.push_constants(0u, std::span(word).first(1)) == rhi::error::invalid_argument);
        CHECK(recorder.push_constants(UINT32_MAX, word) == rhi::error::invalid_argument);
        CHECK(recorder.push_constants(push_capacity, word) == rhi::error::invalid_argument);
        CHECK(recorder.push_constants(push_capacity - 4u, word) == rhi::error::ok);
    };
    std::ifstream file(VR_RESOURCE_GROUP_SHADER_PATH, std::ios::binary | std::ios::ate);
    CHECK(file.good());
    if (!file.good())
        return deren::vk_test::finish("test_resource_group");
    deren::utility::vessel::vector<std::byte> code(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size()));
    rhi::pipeline_desc pipeline_desc{};
    pipeline_desc.first_stage = rhi::shader_stage::compute;
    pipeline_desc.compute_code = code;
    pipeline_desc.resource_group = layout;
    auto pipeline = core->resources->create_pipeline(pipeline_desc);
    CHECK(pipeline);
    invalid = layout;
    invalid[0].push_byte_offset = 2;
    pipeline_desc.resource_group = invalid;
    CHECK(!core->resources->create_pipeline(pipeline_desc));
    pipeline_desc.resource_group = layout;
    auto bad_layout = layout;
    bad_layout[0].shader_stage_bits = 2;
    pipeline_desc.resource_group = bad_layout;
    CHECK(!core->resources->create_pipeline(pipeline_desc));
    pipeline_desc.resource_group = layout;
    if (!pipeline)
        return deren::vk_test::finish("test_resource_group");
    auto read_shader = [](char const* path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        deren::utility::vessel::vector<std::byte> bytes;
        if (input.good()) {
            bytes.resize(static_cast<std::size_t>(input.tellg()));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        return bytes;
    };
    auto vertex = read_shader(VR_RESOURCE_GROUP_VERTEX_PATH), fragment = read_shader(VR_RESOURCE_GROUP_FRAGMENT_PATH);
    std::array colors{rhi::image_format::rgba8_unorm};
    std::array fragment_layout{rhi::resource_group_binding{10, kind::storage_buffer, 0, 2}};
    rhi::pipeline_desc graphics_desc{};
    graphics_desc.color_formats = colors;
    graphics_desc.depth_test = false;
    graphics_desc.vertex_code = vertex;
    graphics_desc.fragment_code = fragment;
    graphics_desc.resource_group = fragment_layout;
    auto graphics = core->resources->create_pipeline(graphics_desc);
    CHECK(graphics); // The vertex stage has no group and no DescriptorHeap capability.
    auto depth_vertex = read_shader(VR_RESOURCE_GROUP_DEPTH_VERTEX_PATH);
    std::array vertex_layout{rhi::resource_group_binding{10, kind::storage_buffer, 0, 1}};
    graphics_desc.color_formats = {};
    graphics_desc.depth_format = rhi::image_format::depth;
    graphics_desc.vertex_code = depth_vertex;
    graphics_desc.fragment_code = {};
    graphics_desc.resource_group = vertex_layout;
    auto depth_pipeline = core->resources->create_pipeline(graphics_desc);
    CHECK(depth_pipeline); // Depth-only vertex resources do not require an absent fragment stage.
    auto make_buffer = [&](rhi::buffer_usage usage, std::uint64_t size) {
        return rhi::shared_object<rhi::buffer>(core->resources->create_buffer({.size = size, .usage = usage, .flags = rhi::to_bits(rhi::buffer_flag::device_address)}));
    };
    auto input = make_buffer(rhi::buffer_usage::storage_coherent, 256);
    auto output = make_buffer(rhi::buffer_usage::storage_coherent, 256);
    auto uniform = make_buffer(rhi::buffer_usage::uniform_coherent, 256);
    CHECK(input && output && uniform);
    if (!input || !output || !uniform)
        return deren::vk_test::finish("test_resource_group");
    std::uint32_t value = 73;
    std::memcpy(input->mapped().data(), &value, 4);
    value = 41;
    std::memcpy(uniform->mapped().data(), &value, 4);
    std::memset(output->mapped().data(), 0, 256);
    std::uint32_t pixel = 0xff0000ff;
    rhi::image_desc image_desc{};
    image_desc.extent = {1, 1, 1};
    image_desc.format = rhi::image_format::rgba8_unorm;
    image_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
    image_desc.initial_bytes = std::as_bytes(std::span(&pixel, 1));
    rhi::shared_object<rhi::image> image(core->resources->create_image(image_desc));
    image_desc.initial_bytes = {};
    bool const storage_readback_available = rhi::query_extension<rhi::host_image_copy>(*core) != nullptr;
    image_desc.flags = rhi::to_bits(rhi::image_flag::storage) | (storage_readback_available ? rhi::to_bits(rhi::image_flag::host_transfer) : 0u);
    rhi::shared_object<rhi::image> storage(core->resources->create_image(image_desc));
    auto storage_readback = storage;
    rhi::shared_object<rhi::sampler> sampler(core->resources->create_sampler({}));
    CHECK(image && storage && sampler);
    if (!image || !storage || !sampler)
        return deren::vk_test::finish("test_resource_group");
    rhi::texture_group_info textures{};
    textures.textures[0] = image.get();
    rhi::error status = rhi::error::operation_failed;
    auto texture_group = core->resources->assign_texture_group(textures, &status);
    CHECK(texture_group && status == rhi::error::ok);
    if (!texture_group)
        return deren::vk_test::finish("test_resource_group");
    std::array entries{
        rhi::resource_group_entry{.binding = 70, .kind = kind::uniform_buffer, .resource = uniform},
        rhi::resource_group_entry{.binding = 20, .kind = kind::storage_buffer, .resource = output},
        rhi::resource_group_entry{.binding = 30, .kind = kind::sampled_image, .resource = image},
        rhi::resource_group_entry{.binding = 40, .kind = kind::storage_image, .resource = storage, .view = {.role = rhi::view_role::storage}},
        rhi::resource_group_entry{.binding = 50, .kind = kind::sampler, .resource = sampler},
        rhi::resource_group_entry{.binding = 60, .kind = kind::texture_group, .resource = texture_group},
        rhi::resource_group_entry{.binding = 10, .kind = kind::storage_buffer, .resource = input}};
    auto create = [&] { return core->resources->create_resource_group({.entries = entries}); };
    CHECK(!core->resources->create_resource_group({}));
    auto bad = entries;
    bad[0].binding = 20;
    CHECK(!core->resources->create_resource_group({.entries = bad}));
    bad = entries;
    bad[6].offset = UINT64_MAX;
    CHECK(!core->resources->create_resource_group({.entries = bad}));
    bad = entries;
    bad[6].size = UINT64_MAX;
    CHECK(!core->resources->create_resource_group({.entries = bad}));
    bad = entries;
    bad[6].resource = rhi::shared_object<rhi::buffer>::borrow(*input);
    CHECK(!core->resources->create_resource_group({.entries = bad}));
    bad = entries;
    bad[2].resource = sampler;
    CHECK(!core->resources->create_resource_group({.entries = bad}));
    auto other = deren::engine::load_api_core(creation);
    CHECK(other);
    if (other) {
        auto foreign = rhi::shared_object<rhi::buffer>(other->resources->create_buffer({.size = 256, .usage = rhi::buffer_usage::storage_coherent, .flags = rhi::to_bits(rhi::buffer_flag::device_address)}));
        bad = entries;
        bad[6].resource = foreign;
        CHECK(!core->resources->create_resource_group({.entries = bad}));
        bad = {};
        foreign.reset();
        other.reset();
    }
    // Repeated success and partial failure must recycle private slots, including samplers.
    unsigned recycled = 0;
    for (; recycled < 7000; ++recycled) {
        auto temporary = create();
        if (!temporary)
            break;
        bad = entries;
        bad[4].resource = input; // failure after several resource descriptors were written
        if (core->resources->create_resource_group({.entries = bad}))
            break;
    }
    CHECK(recycled == 7000);
    bad = {};
    // Top-level AS is a member too; descriptor creation does not build/traverse the structure.
    auto* capabilities = rhi::query_extension<rhi::device_capabilities>(*core);
    if (capabilities && capabilities->ray_query()) {
        rhi::shared_object<rhi::acceleration_structure> structure(core->resources->create_acceleration_structure({.type = rhi::acceleration_structure_type::top_level, .instance_capacity = 1}));
        CHECK(structure);
        if (structure) {
            rhi::resource_group_entry acceleration{.binding = 9, .kind = kind::acceleration_structure, .resource = structure};
            auto acceleration_group = core->resources->create_resource_group({.entries = std::span(&acceleration, 1)});
            CHECK(acceleration_group);
        }
    }
    // A group loaded in a secondary survives both batch submission and primary composition.
    for (bool batch : {true, false}) {
        auto temporary = create();
        CHECK(temporary);
        if (!temporary)
            break;
        rhi::resource_group_ref::weak retained(*temporary);
        auto segment = core->commands->make_mergeable_command_buffer({});
        CHECK(segment);
        if (!segment)
            break;
        CHECK(segment->begin_recording({}) == rhi::error::ok);
        check_push_range(*segment);
        CHECK(segment->barrier(rhi::image_barrier{.resource = storage.get(), .from = batch ? rhi::image_use::undefined : rhi::image_use::shader_write, .to = batch ? rhi::image_use::shader_write : rhi::image_use::shader_read_write}) == rhi::error::ok);
        CHECK(segment->bind_pipeline(*pipeline) == rhi::error::ok);
        CHECK(segment->load_resource_group(*temporary) == rhi::error::ok);
        std::uint32_t index = batch ? 1u : 2u;
        CHECK(segment.get()->push_constants(28, std::as_bytes(std::span(&index, 1))) == rhi::error::ok);
        segment->dispatch(1, 1, 1);
        rhi::buffer_barrier host{.resource = output.get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::host_read};
        CHECK(segment->barrier({.buffers = std::span(&host, 1)}) == rhi::error::ok);
        CHECK(segment->end_recording() == rhi::error::ok);
        temporary = std::unexpected(rhi::error::unsupported);
        std::expected<rhi::submission_token, rhi::error> token = std::unexpected(rhi::error::unsupported);
        if (batch)
            token = completion->submit(std::span(&segment, 1));
        else {
            auto parent = core->commands->make_command_buffer({});
            CHECK(parent);
            CHECK(parent->begin_recording({}) == rhi::error::ok);
            CHECK(parent->execute(std::span(&segment, 1)) == rhi::error::ok);
            CHECK(parent->end_recording() == rhi::error::ok);
            token = completion->submit(*parent);
        }
        CHECK(token);
        segment.reset();
        if (token)
            CHECK(completion->wait(*token, UINT64_MAX) == rhi::error::ok);
        CHECK(retained.expired());
        auto* values = reinterpret_cast<std::uint32_t const*>(output->mapped().data()) + index * 5;
        CHECK(values[0] == 73 && values[1] == 255 && values[2] == 1 && values[3] == 255 && values[4] == 41);
    }
    // Acquired-frame commands retain a general group, then release recorded state on next begin.
    {
        auto temporary = create();
        CHECK(temporary);
        if (temporary) {
            rhi::resource_group_ref::weak retained(*temporary);
            (void)core->frames->frame_begin();
            auto* frame = core->frames->begin_commands();
            CHECK(frame);
            if (frame) {
                CHECK(frame->begin_recording({}) == rhi::error::ok);
                check_push_range(*frame);
                CHECK(frame->barrier(rhi::image_barrier{.resource = storage.get(), .from = rhi::image_use::shader_write, .to = rhi::image_use::shader_read_write}) == rhi::error::ok);
                CHECK(frame->bind_pipeline(*pipeline) == rhi::error::ok);
                CHECK(frame->load_resource_group(*temporary) == rhi::error::ok);
                std::uint32_t index = 3;
                CHECK(frame->push_constants(28, std::as_bytes(std::span(&index, 1))) == rhi::error::ok);
                frame->dispatch(1, 1, 1);
                rhi::buffer_barrier host{.resource = output.get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::host_read};
                CHECK(frame->barrier({.buffers = std::span(&host, 1)}) == rhi::error::ok);
                CHECK(frame->end_recording() == rhi::error::ok);
                temporary = std::unexpected(rhi::error::unsupported);
                CHECK(!retained.expired());
                CHECK(core->frames->submit() == rhi::error::ok);
                core->wait_idle();
                auto* values = reinterpret_cast<std::uint32_t const*>(output->mapped().data()) + 15;
                CHECK(values[0] == 73 && values[1] == 255 && values[2] == 1 && values[3] == 255 && values[4] == 41);
                (void)core->frames->frame_begin();
                frame = core->frames->begin_commands();
                CHECK(frame);
                if (frame) {
                    CHECK(frame->begin_recording({}) == rhi::error::ok);
                    CHECK(retained.expired());
                    CHECK(frame->end_recording() == rhi::error::ok);
                    CHECK(core->frames->submit() == rhi::error::ok);
                    core->wait_idle();
                }
            }
        }
    }
    auto made = create();
    CHECK(made);
    if (!made)
        return deren::vk_test::finish("test_resource_group");
    rhi::resource_group_ref group = *made;
    auto commands = core->commands->make_command_buffer({});
    CHECK(commands);
    if (!commands)
        return deren::vk_test::finish("test_resource_group");
    // Stale pending borrowed identities fail before native replay and recover through begin/reset.
    {
        auto stale = core->commands->make_command_buffer({});
        std::array<std::byte, 64> vertex_bytes{};
        auto vertex = core->resources->create_buffer({.size = vertex_bytes.size(), .usage = rhi::buffer_usage::vertex, .initial_bytes = vertex_bytes});
        CHECK(stale && vertex);
        if (stale && vertex) {
            CHECK(stale->begin_recording({}) == rhi::error::ok);
            CHECK(stale->bind_vertex_buffer(*vertex, 0) == rhi::error::ok);
            vertex.reset();
            CHECK(stale->apply_operations() == rhi::error::invalid_argument);
            CHECK(stale->end_recording() == rhi::error::invalid_argument);
            CHECK(!completion->submit(*stale));
            CHECK(stale->begin_recording({}) == rhi::error::ok);
            stale->draw(3, 1, 0, 0); // invalid scope latches an error; no native draw is emitted.
            CHECK(stale->end_recording() == rhi::error::invalid_argument);
            CHECK(stale->begin_recording({}) == rhi::error::ok);
            CHECK(stale->end_recording() == rhi::error::ok);
            CHECK(stale->begin_recording({}) == rhi::error::ok);
            stale->end_rendering();
            CHECK(stale->apply_operations() == rhi::error::invalid_argument);
            CHECK(stale->end_recording() == rhi::error::invalid_argument);
        }
    }
    {
        auto stale = core->commands->make_command_buffer({});
        auto temporary_pipeline = core->resources->create_pipeline(pipeline_desc);
        CHECK(stale && temporary_pipeline);
        if (stale && temporary_pipeline) {
            CHECK(stale->begin_recording({}) == rhi::error::ok);
            CHECK(stale->bind_pipeline(*temporary_pipeline) == rhi::error::ok);
            CHECK(stale->apply_operations() == rhi::error::ok);
            CHECK(stale->load_resource_group(group) == rhi::error::ok);
            temporary_pipeline.reset();
            CHECK(stale->apply_operations() == rhi::error::invalid_argument);
            CHECK(stale->end_recording() == rhi::error::invalid_argument);
            CHECK(stale->begin_recording({}) == rhi::error::ok);
            CHECK(stale->end_recording() == rhi::error::ok);
        }
    }
    std::uint32_t state_word = 0;
    CHECK(commands->push_constants(0u, std::as_bytes(std::span(&state_word, 1))) == rhi::error::not_ready);
    CHECK(commands->load_resource_group(group) == rhi::error::not_ready);
    CHECK(commands->apply_operations() == rhi::error::not_ready);
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    CHECK(commands->apply_operations() == rhi::error::ok);
    CHECK(commands->apply_operations() == rhi::error::ok);
    check_push_range(*commands);
    CHECK(commands->load_resource_group(group) == rhi::error::not_ready);
    CHECK(commands->barrier(rhi::image_barrier{.resource = storage.get(), .from = rhi::image_use::shader_write, .to = rhi::image_use::shader_read_write}) == rhi::error::ok);
    CHECK(commands->bind_pipeline(*pipeline) == rhi::error::ok);
    CHECK(commands->load_resource_group(rhi::resource_group_ref::borrow(*group)) == rhi::error::invalid_argument);
    CHECK(commands->load_resource_group({}) == rhi::error::invalid_argument);
    bad = entries;
    bad[6].binding = 11;
    auto mismatch = core->resources->create_resource_group({.entries = bad});
    CHECK(mismatch);
    if (mismatch)
        CHECK(commands->load_resource_group(*mismatch) == rhi::error::invalid_argument);
    mismatch = std::unexpected(rhi::error::unsupported);
    bad = {};
    CHECK(commands->load_resource_group(group) == rhi::error::ok);
    std::uint32_t index = 0;
    CHECK(commands.get()->push_constants(0, std::as_bytes(std::span(&index, 1))) == rhi::error::invalid_argument);
    CHECK(commands.get()->push_constants(28, std::as_bytes(std::span(&index, 1))) == rhi::error::ok);
    CHECK(commands->apply_operations() == rhi::error::ok);
    CHECK(commands->bind_pipeline(*pipeline) == rhi::error::ok); // same pipeline preserves the loaded group.
    // Raw native interop deliberately overwrites the index. finish_native must restore the copied
    // logical push/group state before the next RHI consumer, including across a compatible pipeline switch.
    auto& interop = static_cast<deren::vulkan::command_buffer&>(*commands);
    auto const& context = static_cast<deren::vulkan::main_context const&>(*core->main_context());
    auto native_push = reinterpret_cast<PFN_vkCmdPushDataEXT>(vkGetDeviceProcAddr(context.device, "vkCmdPushDataEXT"));
    CHECK(native_push != nullptr);
    CHECK(interop.prepare_native() == rhi::error::ok);
    std::uint32_t clobber = 4;
    if (native_push) {
        VkPushDataInfoEXT native_info{.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT, .pNext = nullptr, .offset = 28, .data = {&clobber, sizeof(clobber)}};
        native_push(interop.get(), &native_info);
    }
    interop.finish_native();
    auto compatible_pipeline = core->resources->create_pipeline(pipeline_desc);
    CHECK(compatible_pipeline);
    if (compatible_pipeline)
        CHECK(commands->bind_pipeline(*compatible_pipeline) == rhi::error::ok);
    index = 4; // push consumes the bytes before return; changing source storage cannot change output lane 0.
    commands->dispatch(1, 1, 1);
    rhi::buffer_barrier barrier{.resource = output.get(), .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::host_read};
    CHECK(commands->barrier({.buffers = std::span(&barrier, 1)}) == rhi::error::ok);
    CHECK(commands->end_recording() == rhi::error::ok);
    CHECK(commands->push_constants(0u, std::as_bytes(std::span(&state_word, 1))) == rhi::error::not_ready);
    rhi::resource_group_ref::weak observer(group);
    rhi::shared_object<rhi::buffer>::weak input_observer(input);
    made = std::unexpected(rhi::error::unsupported);
    group.reset();
    entries = {};
    bad = {};
    input.reset();
    uniform.reset();
    image.reset();
    storage.reset();
    sampler.reset();
    texture_group.reset();
    CHECK(!observer.expired() && !input_observer.expired());
    auto& native = static_cast<deren::vulkan::main_context&>(*core->main_context());
    queue_gate gate(native.device, native.graphics_queue);
    auto token = completion->submit(*commands);
    CHECK(token);
    commands.reset();
    if (token && gate.armed) {
        CHECK(completion->poll(*token) == rhi::error::not_ready);
        CHECK(!observer.expired() && !input_observer.expired());
    }
    gate.release();
    if (token)
        CHECK(completion->wait(*token, UINT64_MAX) == rhi::error::ok);
    auto* result = reinterpret_cast<std::uint32_t const*>(output->mapped().data());
    deren::vk_test::write_line("group readback: {} {} {} {} {}", result[0], result[1], result[2], result[3], result[4]);
    CHECK(result[0] == 73);
    CHECK(result[1] == 255);
    CHECK(result[2] == 1);
    CHECK(result[3] == 255);
    CHECK(result[4] == 41);
    CHECK(observer.expired() && input_observer.expired());
    if (storage_readback_available) {
        auto content = storage_readback->get_content();
        CHECK(content && content->bytes.size() == sizeof(pixel));
        if (content && content->bytes.size() == sizeof(pixel)) {
            std::uint32_t actual = 0;
            std::memcpy(&actual, content->bytes.data(), sizeof(actual));
            CHECK(actual == pixel);
        }
    }
    core->wait_idle();
    return deren::vk_test::finish("test_resource_group");
}
